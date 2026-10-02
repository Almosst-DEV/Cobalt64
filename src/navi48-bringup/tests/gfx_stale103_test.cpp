// gfx_stale103_test.cpp — build 0.0.544 ( ranked item (1); switch 103, gfx_stale103.h): the stale-input gate and
// the interaction instruments.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_stale103_test.cpp -o /tmp/st103 && \
//         /tmp/st103 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_commit.h
// Covers (fixtures from RUN AU, notes/logs/runs/run11ao/driverlog-stream.txt, quoted where used):
//   S1 F483's U reading 0x401160000 (page 0x13b6f000) is flagged: its writers F22 / F27 were refused (`FRAME TARGET ... VA
//      0x401160000 -> VRAM offset 0x13b6f000`), no committed frame wrote that page, the proof is the ledger (DCC T#);
//   S2 F482's date-U (0x4011e0000) is not: proven earlier in the same frame (P's CB0 at dw 2714 precedes U at dw 1043 in
//      tex531's print order), or last written by a committed frame;
//   S3 U's self-read (tex 0 = its CB0 0x400800000, page 0x10030000, `526 refused frame(s)`) hits the exemption, and the LATCH:
//      a refused layer writer followed by self-reading frames recovers in one frame; without the exemption it never does;
//   S4 heap reuse: same VA on a different page is not stale; a different VA on the refused page is;
//   S5 OFF identity; S6 SHADOW never changes a verdict; S7 ON only refuses (only STALE, only after a yes);
//   S8 the table's replacement policy and the arm reset; S9 the verb's decode; S10 the boxes (1080p and 1440p by value);
//   S11 the draw classification; S12 the re-opened window; S13 every line <= 491 bytes;
//   S14 the kext glue (source pins): OFF at boot, the guard, the three asks untouched and wrapped only while active, the feed,
//       the tex_state wiring.
// build 0.0.545 ( MUST-FIX 1-3, SHOULD-FIXes):
//   S15 the U -> layer -> Z -> P cascade (latch a): one refused text frame latches the layer NOT_EXEC for ever under ON without
//       the frame-level CB exemption; with it the next frame recovers; an INCOMPLETE target set exempts nothing;
//   S16 latch (b): one refused P, then P frames sampling the plane pages: refused for ever before, committed with the P policy;
//       the S1 clock read is still refused under ON with every new rule on (never committed; not a frame target; P or not);
//   S17 the simulated-ON table diverges from the real one in SHADOW (a frame SHADOW commits but ON would refuse), counted;
//   S18 isSys keying; S19 trunc counts only real overflow; S20 the recovery times; S21 the frame target set's completeness;
//   S22 the new glue (source pins, ORDER: the set is filled before the policy pass's asks).
// build 0.0.546 ( MUST-FIX and SHOULD-FIXes):
//   S23 THE EVICTION LATCH: reproduced with 0.0.545's own n48_st_slot / n48_st_classify (copied verbatim below): a plane row evicted
//       under pressure, re-marked by a refused writer, returns with exec_seq 0 and every later P reading it is STALE (0 of 40); fixed
//       by each half of the fix alone and by both (40 of 40); a ledger-proved P passes; the S1 clock (U, not a P) is still STALE;
//       role rows survive eviction pressure; only-role-rows evicts the oldest, counted evictRole; n48_st_void voids on it;
//   S24 the frame's set is incomplete with no reader or a skipped IB (n48_st_fset_blind before the close), counted;
//   S25 the glue (source pins, ORDER: the IB loop's flag is set before its continue and read before the close; the verb labels).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_stale103.h"
#include "gfx_capture_scan.h"
#include "gfx_commit.h"

#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p)
{
    std::string s; FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (!f) return s;
    char b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const std::string &n)
{
    uint32_t c = 0; size_t p = 0;
    if (n.empty()) return 0;
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}
static std::string body_of(const std::string &s, const std::string &head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t e = s.find("\n}\n", a);
    return s.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
static uint64_t gRs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }

static n48_st_tab gT;   /* static: 10 KiB */

/* RUN AU's surfaces (driverlog-stream.txt): VA -> the page the kext's own walk printed. */
static const uint64_t kClockVa = 0x401160000ull, kClockPg = 0x13b6f000ull;   /* `VA 0x401160000 -> page 0x13b6f000` */
static const uint64_t kLayerVa = 0x400800000ull, kLayerPg = 0x10030000ull;   /* `VA 0x400800000 -> page 0x10030000` */
static const uint64_t kScrVa   = 0x4010e0000ull, kScrPg   = 0x13afa000ull;   /* `VA 0x4010e0000 -> page 0x13afa000` */
static const uint64_t kDateVa  = 0x4011e0000ull, kDatePg  = 0x13c10000ull;   /* the date scratch: its page is NOT in the log (SYNTHETIC) */
static const uint64_t kArm = 3601057435ull;   /* `the first plane commit ... at 3601057435 us` (any non-zero key serves) */

/* The run up to F482: F22 and F27 refused (both named 0x401160000 first: `FRAME TARGET: 2 colour target(s); the first is VA
 * 0x401160000`), F470-F482 committed (`dpled841: f470 ... gate COMMIT` ... `f482 ... gate COMMIT`) with tex531's CB0s. */
static void run_to_f482(n48_st_tab *t)
{
    std::memset(t, 0, sizeof *t);
    n48_st_arm_scope(t, kArm);
    n48_st_mark(t, kClockVa, kClockPg, 0u, 22u, 0u, N48_ST_COV_UNKNOWN);   /* F22 VERDICT program-unknown -> neuter */
    n48_st_mark(t, kClockVa, kClockPg, 0u, 27u, 0u, N48_ST_COV_UNKNOWN);   /* F27 VERDICT segment-policy -> neuter */
    for (uint64_t f = 470u; f <= 482u; f++) {
        n48_st_mark(t, kScrVa, kScrPg, 0u, f, 1u, N48_ST_COV_UNKNOWN);      /* tex531 F481/F482: P cb0 0x4010e0000 */
        n48_st_mark(t, kLayerVa, kLayerPg, 0u, f, 1u, N48_ST_COV_UNKNOWN);  /* tex531 F482: AN / U cb0 0x400800000 */
        n48_st_mark(t, kDateVa, kDatePg, 0u, f, 1u, N48_ST_COV_UNKNOWN);    /* tex531 F483: P cb0 0x4011e0000 (the family's) */
    }
}
static n48_st_q ask(uint64_t frame, uint64_t va, uint64_t page, uint32_t proof, uint64_t cbPage, uint32_t same)
{
    n48_st_q q; std::memset(&q, 0, sizeof q);
    q.arm = kArm; q.frame = frame; q.va = va; q.page = page; q.proof = proof;
    q.cb_known = cbPage ? 1u : 0u; q.cb_page = cbPage; q.same_frame = same;
    return q;
}

/* ---- S1 ---- */
static void s1()
{
    run_to_f482(&gT);
    /* `tex531: F483 cb0 0x400800000 win 828,147-1089,260 ... U(90) draw dw 1043 tex 1 heap 46 base 0x401160000 320x128 fmt 65 sw 27
     * dcc comp 1` - a DCC T#: its only proof is the ledger (dccstrip60 `proven by ledger 166`). */
    n48_st_q q = ask(483u, kClockVa, kClockPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    const n48_st_row *r = nullptr;
    const uint32_t v = n48_st_classify(&gT, &q, &r);
    expect_u("S1 F483 U tex 1 0x401160000 (page 0x13b6f000, ledger proof, cb0 0x400800000): STALE", v, N48_ST_V_STALE);
    expect_u("S1 ... its writer is F27 (the newest refused frame naming the page)", r ? r->seq : 0u, 27u);
    expect_u("S1 ... ON turns the yes into a no", n48_st_keep(N48_ST_M_ON, v), 0u);
    expect_u("S1 ... SHADOW keeps the yes", n48_st_keep(N48_ST_M_SHADOW, v), 1u);
    expect_u("S1 ... OFF keeps the yes", n48_st_keep(N48_ST_M_OFF, v), 1u);
    /* the same ask answered by resprov (no frame number: proof 0) is stale too */
    q.proof = N48_ST_P_RESPROV;
    expect_u("S1 ... and with a resprov proof (frame unknown -> 0): STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    /* a committed frame that names the page clears it */
    n48_st_mark(&gT, kClockVa, kClockPg, 0u, 483u, 1u, N48_ST_COV_UNKNOWN);
    q = ask(484u, kClockVa, kClockPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    expect_u("S1 ... after a committed writer (F483 EXEC) the next ask is fresh", n48_st_classify(&gT, &q, nullptr), N48_ST_V_FRESH);
}

/* ---- S2 ---- */
static void s2()
{
    run_to_f482(&gT);
    /* `tex531: F482 cb0 0x400800000 win 858,92-1099,140 ... U(90) draw dw 1043 tex 1 heap 48 base 0x4011e0000 256x64 fmt 65` with
     * `F482 cb0 0x4011e0000 ... P(85) draw dw 2714` printed BEFORE it (translation order). Table as of F482's asks: F470-F481. */
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, kClockVa, kClockPg, 0u, 27u, 0u, N48_ST_COV_UNKNOWN);
    for (uint64_t f = 470u; f <= 481u; f++) n48_st_mark(&gT, kDateVa, kDatePg, 0u, f, 1u, N48_ST_COV_UNKNOWN);
    n48_st_q q = ask(482u, kDateVa, kDatePg, N48_ST_P_LEDGER, kLayerPg, 1u);
    expect_u("S2 F482 date-U (0x4011e0000; last writer F481 committed): fresh, not flagged", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_FRESH);
    /* even had its last judged writer (F481, hypothetically an S frame) been refused, P wrote it earlier in F482 itself */
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    for (uint64_t f = 470u; f <= 480u; f++) n48_st_mark(&gT, kDateVa, kDatePg, 0u, f, 1u, N48_ST_COV_UNKNOWN);
    n48_st_mark(&gT, kDateVa, kDatePg, 0u, 481u, 0u, N48_ST_COV_UNKNOWN);
    uint32_t v = n48_st_classify(&gT, &q, nullptr);
    expect_u("S2 ... with a refused F481 writer: exempt (same frame), not flagged", v, N48_ST_V_EXEMPT_SAME);
    expect_u("S2 ... ON keeps it", n48_st_keep(N48_ST_M_ON, v), 1u);
    q.same_frame = 0u; q.proof = N48_ST_P_FRAMELOCAL;
    v = n48_st_classify(&gT, &q, nullptr);
    expect_u("S2 ... a frame-local proof is exempt (same frame)", v, N48_ST_V_EXEMPT_SAME);
    expect_u("S2 ... ON keeps it", n48_st_keep(N48_ST_M_ON, v), 1u);
    q.proof = N48_ST_P_LEDGER;
    expect_u("S2 control: no same-frame write and a refused writer -> STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
}

/* ---- S3: the self-read and the latch ---- */
static void s3()
{
    run_to_f482(&gT);
    /* a refused frame names the login layer (`VA 0x400800000 -> page 0x10030000 ... 526 refused frame(s)`) */
    n48_st_mark(&gT, kLayerVa, kLayerPg, 0u, 483u, 0u, N48_ST_COV_UNKNOWN);
    /* `tex531: F483 cb0 0x400800000 ... U(90) draw dw 1043 tex 0 heap 1 base 0x400800000 1920x1080 fmt 50` - its own CB0 */
    n48_st_q q = ask(484u, kLayerVa, kLayerPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    const uint32_t v = n48_st_classify(&gT, &q, nullptr);
    expect_u("S3 U's backdrop self-read (tex 0 = its CB0 page 0x10030000): exempt-self", v, N48_ST_V_EXEMPT_SELF);
    expect_u("S3 ... ON keeps it", n48_st_keep(N48_ST_M_ON, v), 1u);
    q.cb_page = kScrPg;
    expect_u("S3 control: the same read from another target (Z's cb0 0x4010e0000): STALE", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_STALE);
    q.cb_known = 0u; q.cb_page = 0u;
    expect_u("S3 control: an unknown CB0 (inherited) is not exempt", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    /* THE LATCH: frames 484..490 each self-read the layer and write it; a frame whose read the gate refuses is refused and so is
     * a refused writer of the layer again. With the exemption the first frame commits and the layer is EXEC; without it, never. */
    uint32_t committed = 0u, firstCommit = 0u;
    for (uint64_t f = 484u; f <= 490u; f++) {
        n48_st_q qq = ask(f, kLayerVa, kLayerPg, N48_ST_P_LEDGER, kLayerPg, 0u);
        const uint32_t keep = n48_st_keep(N48_ST_M_ON, n48_st_classify(&gT, &qq, nullptr));
        n48_st_mark(&gT, kLayerVa, kLayerPg, 0u, f, keep, N48_ST_COV_UNKNOWN);
        if (keep) { committed++; if (!firstCommit) firstCommit = (uint32_t)f; }
    }
    expect_u("S3 LATCH: every self-reading frame after the refused writer commits (7 of 7)", committed, 7u);
    expect_u("S3 LATCH: ... from the very next frame (F484)", firstCommit, 484u);
}

/* ---- S4: heap reuse ---- */
static void s4()
{
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    const uint64_t va = 0x406a00000ull, pgA = 0x14000000ull, pgB = 0x15000000ull;   /* SYNTHETIC ('s rollover VAs) */
    n48_st_mark(&gT, va, pgA, 0u, 100u, 0u, N48_ST_COV_UNKNOWN);   /* a refused frame writes VA -> page A */
    n48_st_q q = ask(101u, va, pgB, N48_ST_P_LEDGER, 0u, 0u); /* Apple re-maps VA onto page B, never written by a refused frame */
    expect_u("S4 same VA, different page: not stale (no row for page B)", n48_st_classify(&gT, &q, nullptr), N48_ST_V_NOROW);
    q = ask(101u, 0x407200000ull, pgA, N48_ST_P_LEDGER, 0u, 0u);   /* a different VA now on page A */
    expect_u("S4 different VA, the refused page: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    /* a committed frame writing page B under another VA does not clear page A */
    n48_st_mark(&gT, 0x407200000ull, pgB, 0u, 102u, 1u, N48_ST_COV_UNKNOWN);
    q = ask(103u, va, pgA, N48_ST_P_LEDGER, 0u, 0u);
    expect_u("S4 ... page A stays stale after page B commits", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    expect_u("S4 the row keeps the writer's VA for the log only", n48_st_find(&gT, pgA) ? n48_st_find(&gT, pgA)->va : 0u, va);
    q = ask(103u, va, 0u, N48_ST_P_LEDGER, 0u, 0u);
    expect_u("S4 an unresolved texture page is not judged", n48_st_classify(&gT, &q, nullptr), N48_ST_V_NOPAGE);
}

/* ---- S4b: a resprov proof is judged against the writers AFTER the copy (the kext marks a recorded copy EXEC) ---- */
static void s4b()
{
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    const uint64_t va = 0x405800000ull, pg = 0x13028000ull;   /* the 1080p wallpaper's VA and a copy destination (run10g #62) */
    n48_st_mark(&gT, va, pg, 0u, 10u, 0u, N48_ST_COV_UNKNOWN);    /* a refused ColorFill of the surface (AB2's 0x405800000 twin fill) */
    n48_st_mark(&gT, va, pg, 0u, 12u, 1u, N48_ST_COV_FULL);       /* then the residency copy records (st103_copy_note: last judged 12) */
    n48_st_q q = ask(13u, va, pg, N48_ST_P_RESPROV, 0u, 0u);
    expect_u("S4b a copy AFTER the refused fill: fresh", n48_st_classify(&gT, &q, nullptr), N48_ST_V_FRESH);
    n48_st_mark(&gT, va, pg, 0u, 14u, 0u, N48_ST_COV_UNKNOWN);    /* a refused frame writes it after the copy */
    q.frame = 15u;
    expect_u("S4b a refused writer AFTER the copy: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
}

/* ---- S5/S6/S7: the three modes over random tables and asks ---- */
static void s567()
{
    uint32_t offBad = 0u, shBad = 0u, onBad = 0u, onRefused = 0u, activeBad = 0u;
    for (uint32_t it = 0; it < 200000u; it++) {
        if ((it & 1023u) == 0u) {
            std::memset(&gT, 0, sizeof gT);
            n48_st_arm_scope(&gT, 1u + (rnd() & 3u));
            for (uint32_t k = 0; k < 64u; k++)
                n48_st_mark(&gT, 0x400000000ull + (rnd() & 63u) * 0x10000ull, (1u + (rnd() & 31u)) << 12, 0u, k + 1u, rnd() & 1u, 0u);
        }
        n48_st_q q; std::memset(&q, 0, sizeof q);
        q.arm = 1u + (rnd() & 3u); q.frame = 1u + (rnd() & 127u); q.page = (rnd() & 7u) ? ((uint64_t)(rnd() & 31u) << 12) : 0u;
        q.va = 0x400000000ull + (rnd() & 63u) * 0x10000ull; q.proof = rnd() % N48_ST_PROOFS; q.same_frame = rnd() & 1u;
        q.cb_known = rnd() & 1u; q.cb_page = (uint64_t)(rnd() & 31u) << 12;
        /* build 0.0.545: the new fields too (isSys, P, a random frame target set, complete or not) */
        uint64_t fk[4]; for (uint32_t z = 0; z < 4u; z++) fk[z] = n48_st_key((uint64_t)(rnd() & 31u) << 12, rnd() & 1u);
        q.sys = (rnd() & 7u) == 0u ? 1u : 0u; q.cb_sys = (rnd() & 7u) == 0u ? 1u : 0u; q.is_p = rnd() & 1u;
        q.fcb_ok = rnd() & 1u; q.fcb = (rnd() & 1u) ? fk : nullptr; q.fcb_n = rnd() & 3u;
        const uint32_t v = n48_st_classify(&gT, &q, nullptr);
        if (n48_st_keep(N48_ST_M_OFF, v) != 1u) offBad++;
        if (n48_st_keep(N48_ST_M_SHADOW, v) != 1u) shBad++;
        const uint32_t k = n48_st_keep(N48_ST_M_ON, v);
        if (k > 1u || (k == 0u) != (v == N48_ST_V_STALE)) onBad++;
        if (!k) onRefused++;
        for (uint32_t m = 0; m < 16u; m++)
            if (m != N48_ST_M_ON && m != N48_ST_M_SHADOW && (n48_st_active(m) || n48_st_keep(m, v) != 1u)) activeBad++;
    }
    expect_u("S5 OFF identity: every yes stands (200 000 random tables/asks)", offBad, 0u);
    expect_u("S5 ... OFF and every M but 1 / 3 is inactive and keeps every yes", activeBad, 0u);
    expect_u("S6 SHADOW never changes a verdict (200 000)", shBad, 0u);
    expect_u("S7 ON answers 0 exactly for STALE, else 1 (200 000)", onBad, 0u);
    expect_u("S7 ... non-vacuous: ON refused some", onRefused > 1000u ? 1u : 0u, 1u);
    n48_st_ctr c; std::memset(&c, 0, sizeof c);
    n48_st_count(&c, N48_ST_M_SHADOW, N48_ST_V_STALE, N48_ST_P_LEDGER, 1u);
    n48_st_count(&c, N48_ST_M_ON, N48_ST_V_STALE, N48_ST_P_RESPROV, 0u);
    n48_st_count(&c, N48_ST_M_ON, N48_ST_V_EXEMPT_SELF, N48_ST_P_LEDGER, 1u);
    expect_u("S6/S7 the counters: shadowed 1 refused 1 stale 2 exempt-self 1 cb0-unknown 1",
             (c.shadowed == 1u && c.refused == 1u && c.v[N48_ST_V_STALE] == 2u && c.v[N48_ST_V_EXEMPT_SELF] == 1u &&
              c.cbUnknown == 1u && c.byProof[N48_ST_P_RESPROV] == 1u && c.asks == 3u) ? 1u : 0u, 1u);
}

/* ---- S8: replacement and the arm reset ---- */
static void s8()
{
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    for (uint32_t k = 0; k < N48_ST_ROWS; k++) n48_st_mark(&gT, 0x400000000ull + k * 0x10000ull, (uint64_t)(k + 1u) << 12, 0u, 1000u + k, 1u, 0u);
    expect_u("S8 256 rows fill the table", gT.used, N48_ST_ROWS);
    n48_st_mark(&gT, 0x500000000ull, 0x999000ull, 0u, 2000u, 0u, 0u);
    expect_u("S8 a 257th evicts the OLDEST EXEC row (page 0x1000, frame 1000)", n48_st_find(&gT, 0x1000ull) ? 1u : 0u, 0u);
    expect_u("S8 ... counted evicted-exec 1, lost 0, still 256 rows",
             (gT.evictExec == 1u && gT.lost == 0u && gT.used == N48_ST_ROWS && n48_st_find(&gT, 0x999000ull)) ? 1u : 0u, 1u);
    /* every row NOT_EXEC: the oldest refused row goes and is counted lost */
    std::memset(&gT, 0, sizeof gT);
    n48_st_arm_scope(&gT, kArm);
    for (uint32_t k = 0; k < N48_ST_ROWS; k++) n48_st_mark(&gT, 0x400000000ull + k * 0x10000ull, (uint64_t)(k + 1u) << 12, 0u, 1000u + k, 0u, 0u);
    n48_st_mark(&gT, 0x500000000ull, 0x999000ull, 0u, 2000u, 0u, 0u);
    expect_u("S8 all refused: the OLDEST refused row goes, counted LOST", (n48_st_find(&gT, 0x1000ull) == nullptr && gT.lost == 1u &&
             n48_st_find(&gT, 0x2000ull) != nullptr) ? 1u : 0u, 1u);
    /* an EXEC row is preferred over an older refused one */
    n48_st_mark(&gT, 0x400010000ull, 0x2000ull, 0u, 2001u, 1u, 0u);   /* page 0x2000 now EXEC at 2001 */
    n48_st_mark(&gT, 0x600000000ull, 0xaaa000ull, 0u, 2002u, 0u, 0u);
    expect_u("S8 ... an EXEC row is evicted before any refused row, even a newer one", (n48_st_find(&gT, 0x2000ull) == nullptr &&
             n48_st_find(&gT, 0x3000ull) != nullptr && gT.lost == 1u && gT.evictExec == 1u) ? 1u : 0u, 1u);
    /* out of order */
    n48_st_mark(&gT, 0x400020000ull, 0x3000ull, 0u, 5u, 1u, 0u);
    expect_u("S8 a mark older than the row is ignored and counted", (gT.outOfOrder == 1u && n48_st_find(&gT, 0x3000ull)->verdict == N48_ST_NOT_EXEC)
             ? 1u : 0u, 1u);
    n48_st_mark(&gT, 0x400020000ull, 0u, 0u, 3000u, 0u, 0u);
    expect_u("S8 an unresolved target is counted, not recorded", gT.unresolved, 1u);
    /* the arm reset */
    n48_st_q q = ask(3000u, 0x600000000ull, 0xaaa000ull, N48_ST_P_LEDGER, 0u, 0u);
    expect_u("S8 before the reset the refused row answers STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    q.arm = kArm + 1u;
    expect_u("S8 an ask under ANOTHER arm finds nothing", n48_st_classify(&gT, &q, nullptr), N48_ST_V_NOROW);
    const uint64_t r0 = gT.resets;
    n48_st_arm_scope(&gT, kArm);
    expect_u("S8 the same arm does not reset", gT.resets == r0 && gT.used == N48_ST_ROWS ? 1u : 0u, 1u);
    n48_st_arm_scope(&gT, kArm + 1u);
    expect_u("S8 a new arm empties the table", (gT.used == 0u && gT.arm == kArm + 1u && gT.resets == r0 + 1u) ? 1u : 0u, 1u);
    expect_u("S8 ... and the old refused row is gone", n48_st_classify(&gT, &q, nullptr), N48_ST_V_NOROW);
    expect_u("S8 the table is 256 rows (static in the kext: sizeof)", sizeof(n48_st_tab) < 16384u ? 1u : 0u, 1u);
}

/* ---- S9: the verb ---- */
static void s9()
{
    expect_u("S9 ON = 359", N48_ST_SWITCH | (N48_ST_M_ON << 8), 359u);
    expect_u("S9 OFF = 615", N48_ST_SWITCH | (N48_ST_M_OFF << 8), 615u);
    expect_u("S9 SHADOW = 871", N48_ST_SWITCH | (N48_ST_M_SHADOW << 8), 871u);
    n48_st_arg d = n48_st_decode(871u);
    expect_u("S9 871 decodes SHADOW, ok", d.m == N48_ST_M_SHADOW && d.ok ? 1u : 0u, 1u);
    d = n48_st_decode(103u);
    expect_u("S9 bare 103 is a read", d.m == 0u && d.ok ? 1u : 0u, 1u);
    d = n48_st_decode(359u | (1ull << 20));
    expect_u("S9 ON with a stray payload bit is refused", d.ok, 0u);
    const uint64_t a1440 = n48_st_box_arg(N48_ST_M_BOX_A, 1067u, 187u, 1493u, 387u);
    d = n48_st_decode(a1440);
    expect_u("S9 a 1440p box A decodes by value", (d.ok && d.m == N48_ST_M_BOX_A && d.x0 == 1067u && d.y0 == 187u && d.x1 == 1493u &&
             d.y1 == 387u) ? 1u : 0u, 1u);
    expect_u("S9 a box with x0 >= x1 is refused", n48_st_decode(n48_st_box_arg(N48_ST_M_BOX_B, 500u, 1u, 500u, 2u)).ok, 0u);
    expect_u("S9 a box payload above bit 59 is refused", n48_st_decode(a1440 | (1ull << 61)).ok, 0u);
    d = n48_st_decode(103u | (6u << 8) | (30u << 12));
    expect_u("S9 M 6 S 30 sets the window", d.ok && d.m == N48_ST_M_WIN && d.secs == 30u ? 1u : 0u, 1u);
    expect_u("S9 M 6 S 1201 is refused", n48_st_decode(103u | (6u << 8) | (1201u << 12)).ok, 0u);
    expect_u("S9 M 7 is refused", n48_st_decode(103u | (7u << 8)).ok, 0u);
    expect_u("S9 the switch is in the continuous mid-arm guard", n48_cm_cont_switch_guarded(103u), 1u);
    expect_u("S9 ... a read stays allowed mid-arm", n48_cm_cont_switch_refused(103u, 1u, 1u, N48_CM_SHOT_ARMED), 0u);
    expect_u("S9 ... a change is refused mid-arm", n48_cm_cont_switch_refused(103u, 0u, 1u, N48_CM_SHOT_ARMED), 1u);
}

/* ---- S10: the boxes ---- */
static uint32_t sc(uint32_t x, uint32_t y) { return (x & 0x7FFFu) | ((y & 0x7FFFu) << 16); }
static void s10()
{
    const n48_st_box a = N48_ST_BOX_A_DEFAULT, b = N48_ST_BOX_B_DEFAULT;
    expect_u("S10 F483 U win 828,147-1089,260 meets box A", n48_st_box_meets(&a, sc(828, 147), sc(1089, 260)), 1u);
    expect_u("S10 F483 AR win 880,967-1040,995 meets box B", n48_st_box_meets(&b, sc(880, 967), sc(1040, 995)), 1u);
    expect_u("S10 ... and not box A", n48_st_box_meets(&a, sc(880, 967), sc(1040, 995)), 0u);
    expect_u("S10 F481 P win 0,0-161,29 meets neither", n48_st_box_meets(&a, sc(0, 0), sc(161, 29)) | n48_st_box_meets(&b, sc(0, 0), sc(161, 29)), 0u);
    expect_u("S10 a full-screen draw meets both", n48_st_box_meets(&a, sc(0, 0), sc(1920, 1080)) & n48_st_box_meets(&b, sc(0, 0), sc(1920, 1080)), 1u);
    expect_u("S10 touching edges (half-open) do not meet", n48_st_box_meets(&a, sc(1120, 140), sc(1200, 290)), 0u);
    const n48_st_box a1440 = { 1067u, 187u, 1493u, 387u };   /* the clock box scaled by 4/3 */
    expect_u("S10 a 1440p box by value: the scaled clock meets it", n48_st_box_meets(&a1440, sc(1104, 196), sc(1452, 347)), 1u);
    expect_u("S10 ... the 1080p position does not", n48_st_box_meets(&a1440, sc(828, 147), sc(1060, 186)), 0u);
    /* the stash: F483's U (tex 0 and 1), the capsule AR, a P outside - and a frame with no known window */
    static n48_st_boxlog L; std::memset(&L, 0, sizeof L);
    const uint32_t rec[8] = { 0x04011600u, 0xc4100000u, 0x801fc04fu, 0x91b00facu, 0u, 0x00400000u, 0x006b0200u, 0x0004011cu };
    const uint64_t U = (192ull << 32) | 0x92c6ae13ull;
    uint32_t kept = 0u;
    kept += n48_st_box_note(&L, &a, &b, 483u, U, 1043u | (0u << 24), 1u, rec, 0xFu, 0x4008000u, 0u, sc(828, 147), sc(1089, 260), 10u, 0u);
    kept += n48_st_box_note(&L, &a, &b, 483u, U, 1043u | (1u << 24), 46u, rec, 0xFu, 0x4008000u, 0u, sc(828, 147), sc(1089, 260), 10u, 0u);
    kept += n48_st_box_note(&L, &a, &b, 483u, U, 1043u | (1u << 24), 46u, rec, 0xFu, 0x4008000u, 0u, sc(828, 147), sc(1089, 260), 10u, 0u);
    kept += n48_st_box_note(&L, &a, &b, 483u, 7u, 1176u, 1u, rec, 0xFu, 0x4008000u, 0u, sc(880, 967), sc(1040, 995), 11u, 0u);
    kept += n48_st_box_note(&L, &a, &b, 483u, 7u, 4432u, 1u, rec, 0xFu, 0x4010e00u, 0u, sc(0, 0), sc(161, 29), 12u, 0u);
    kept += n48_st_box_note(&L, &a, &b, 483u, 7u, 99u, 1u, rec, 0x3u, 0x4010e00u, 0u, sc(828, 147), sc(1089, 260), 12u, 0u);
    expect_u("S10 stash: U x2 and AR kept; the retry's re-read a dupe; P outside; an unknown window counted",
             (kept == 3u && L.n == 3u && L.dupes == 1u && L.noWin == 1u && L.r[2].boxes == 2u && L.r[0].boxes == 1u) ? 1u : 0u, 1u);
    expect_u("S10 a refused frame prints none", n48_st_box_frame_end(&L, 483u, 0u), 0u);
    for (uint32_t k = 0; k < 3u; k++) (void)n48_st_box_note(&L, &a, &b, 484u, U, 1043u | (k << 24), 1u, rec, 0xFu, 0u, 0u, sc(828, 147), sc(1089, 260), 10u, 0u);
    expect_u("S10 a committed frame prints its three", n48_st_box_frame_end(&L, 484u, 1u), 3u);
    L.lines = N48_ST_BOX_LINES - 1u;
    for (uint32_t k = 0; k < 3u; k++) (void)n48_st_box_note(&L, &a, &b, 485u, U, 1043u | (k << 24), 1u, rec, 0xFu, 0u, 0u, sc(828, 147), sc(1089, 260), 10u, 0u);
    expect_u("S10 ... the per-arm cap bounds it (1 of 3, 2 suppressed)", n48_st_box_frame_end(&L, 485u, 1u) == 1u && L.suppressed == 2u ? 1u : 0u, 1u);
}

/* ---- S11/S12 ---- */
static void s1112()
{
    expect_u("S11 a DRAW_INDEX_AUTO header is DRAWN", n48_st_draw_class(0xC0012D00u), N48_ST_D_DRAWN);
    expect_u("S11 DRAW_INDEX_2 is DRAWN", n48_st_draw_class(0xC0032700u), N48_ST_D_DRAWN);
    expect_u("S11 the one-dword NOP is NOPED", n48_st_draw_class(0xFFFF1000u), N48_ST_D_NOPED);
    expect_u("S11 a type-3 NOP is NOPED", n48_st_draw_class(0xC0021000u), N48_ST_D_NOPED);
    expect_u("S11 SET_CONTEXT_REG is unknown", n48_st_draw_class(0xC0016900u), N48_ST_D_UNKNOWN);
    expect_u("S11 a type-2 filler is unknown", n48_st_draw_class(0x80000000u), N48_ST_D_UNKNOWN);
    n48_st_win w; std::memset(&w, 0, sizeof w);
    expect_u("S12 no seconds set: never due", n48_st_win_due(&w, 1000u, 100000000u), 0u);
    w.secs = 30u;
    expect_u("S12 before START: not due", n48_st_win_due(&w, 0u, 100000000u), 0u);
    expect_u("S12 START+29.9 s: not due", n48_st_win_due(&w, 1000000u, 1000000u + 29900000u), 0u);
    expect_u("S12 START+30 s: due, once", n48_st_win_due(&w, 1000000u, 1000000u + 30000000u), 1u);
    expect_u("S12 ... not twice", n48_st_win_due(&w, 1000000u, 1000000u + 90000000u), 0u);
    expect_u("S12 ... epoch moved once", w.epoch, 1u);
    n48_st_win_arm(&w);
    expect_u("S12 a new arm may fire again", n48_st_win_due(&w, 5000000u, 5000000u + 30000000u), 1u);
}

/* ---- S13: the lines ---- */
static void s13()
{
    char buf[2048]; uint32_t ok = 1u; int n;
    const unsigned long long M = ~0ull; const uint32_t U32 = ~0u;
    n = std::snprintf(buf, sizeof buf, N48_ST_LINE_FMT, "SHADOW would-refuse", M, U32, M, "UCF", M, M, "frame-local", M, M, M, M,
                      " unknown", M, U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  LINE %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_REPORT_FMT, n48_st_mode_name(N48_ST_M_SHADOW),
                      " - `gfxneuter 103` REFUSED (unknown M or a bad payload), unchanged", M, U32, U32, M, M, M, M, M, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  REPORT %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_REPORT2_FMT, M, M, M, M, M, M, M, M, M, M, M, M, M, M, U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  REPORT2 %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_REPORT3_FMT, U32, U32, U32, U32, U32, U32, U32, U32, U32, M, M, M, M, M, M, M, M, U32, U32,
                      U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  REPORT3 %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_BOX_FMT, "AB", M, "DRAWN", U32, U32, "pgm", M, U32, U32, M, U32, U32, U32, U32, U32, M,
                      " inh", U32, U32, U32, U32, U32, U32, U32, U32, U32, U32, U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  BOX %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_SIG_FMT, M, "16224|16224|16224|16224|16224|16224|16224|16224|16224|16224|16224", U32, U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  SIG %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_WIN_FMT, U32, M, M, U32, U32, M, U32, U32, U32);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  WIN %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_SIM_FMT, U32, U32, M, M, M, M, M, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  SIM %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_WOULD_FMT, M, M, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  WOULD %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_REC_FMT, "SIMULATED-ON", M, M, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  REC %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_FS_FMT, M, M, M, M, M, M, M, M, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FS %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_VOID_FMT, M, M, M, M);   /* build 0.0.546: + the role-row evictions */
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  VOID %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_ST_FS2_FMT, M, M, M, M);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FS2 %d\n", n); }
    n = std::snprintf(buf, sizeof buf, "%s", N48_ST_PREV_FMT);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  PREV %d\n", n); }
    expect_u("S13 every stale103 line fits 491 bytes at maximal fields", ok, 1u);
}

/* ---- S14: the kext glue ---- */
static void s14(const std::string &ahh, const std::string &cm)
{
    if (ahh.empty()) { expect_u("S14 AppleHardwareHook.cpp given", 0u, 1u); return; }
    expect_u("S14 OFF at boot", count(ahh, "static volatile uint32_t gSt103Mode { N48_ST_M_OFF };"), 1u);
    expect_u("S14 the verb is the mode's only writer", count(ahh, "__atomic_store_n(&gSt103Mode,"), 1u);
    expect_u("S14 PIN SWITCH-GUARD:103: the verb calls the guard with its own selector, exactly once",
             count(ahh, "n48_cm_cont_switch_refused(103u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("S14 the guard list holds 103", count(cm, "    case 103u:"), 1u);
    expect_u("S14 the table is file-scope static (not stack)", count(ahh, "static n48_st_tab gSt103Tab {};"), 1u);
    /* THE GATE'S ONLY ENTRY: three wrappers, swapped in by st103_wire (identity-checked) only while 103 is active; the three
     * provenance asks themselves are 0.0.543's text (no 103 inside them), so OFF hands the translator the original pointers. */
    const std::string tiled = body_of(ahh, "static int gfxsrc_desc_tiled_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, const uint32_t *t10, uint32_t *clamp) {");
    const std::string dcc = body_of(ahh, "static int gfxsrc_desc_dcc_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes) {");
    expect_u("S14 the provenance asks carry no switch-103 code (0.0.543's bodies)",
             (!tiled.empty() && !dcc.empty() && count(tiled, "st103") + count(dcc, "st103") == 0u &&
              count(tiled, "return 1;") == 5u && count(dcc, "return via != N48_DL_DCC_NONE ? 1 : 0;") == 1u) ? 1u : 0u, 1u);
    expect_u("S14 the wrappers: each returns the ORIGINAL's answer, gated only after a yes",
             (count(ahh, "    return gfxsrc_desc_tiled_ok(ctx, va, mode, elemBytes) ? st103_gate(ctx, va, mode, 0u) : 0;") == 1u &&
              count(ahh, "    const int ok = gfxsrc_desc_tiled_okt(ctx, va, mode, elemBytes, t10, &cl) ? st103_gate(ctx, va, mode, 0u) : 0;") == 1u &&
              count(ahh, "    if (clamp) *clamp = ok ? cl : 0u;\n    return ok;\n}\nstatic int st103_dcc_ok(") == 1u &&
              count(ahh, "    return gfxsrc_desc_dcc_ok(ctx, va, mode, elemBytes) ? st103_gate(ctx, va, mode, 1u) : 0;") == 1u) ? 1u : 0u, 1u);
    const std::string gate = body_of(ahh, "static int st103_gate(void *ctx, uint64_t va, uint32_t mode, uint32_t dcc) {");
    expect_u("S14 st103_gate: inactive -> 1 first; a no only when st103_after_yes answers 0, with the failed-ask note",
             (gate.find("if (!n48_st_active(m) || !c) return 1;") != std::string::npos &&
              gate.find("if (!n48_st_active(m) || !c) return 1;") < gate.find("if (st103_after_yes(c, va, mode, 0ull, st103_proof_of(c, va, mode, dcc), m)) return 1;") &&
              gate.find("if (st103_after_yes(c, va, mode, 0ull, st103_proof_of(c, va, mode, dcc), m)) return 1;") < gate.find("gfxsrc_defer_ask_failed(va);") &&
              count(gate, "return 0;") == 1u) ? 1u : 0u, 1u);
    const std::string after = body_of(ahh, "static __attribute__((noinline)) uint32_t st103_after_yes(");
    expect_u("S14 st103_after_yes returns n48_st_keep (the one decision on the REAL table's answer) or 1", (count(after, "return n48_st_keep(mode, v);") == 1u &&
             count(after, "return 1u;") == 2u && count(after, "{ gSt103C.unarmed++; return 1u; }") == 1u && count(after, "return 0") == 0u &&
             count(after, "n48_st_keep(") == 1u) ? 1u : 0u, 1u);
    expect_u("S14 n48_st_keep is called only by st103_after_yes; st103_after_yes only by st103_gate",
             (count(ahh, "n48_st_keep(") == 1u && count(ahh, "st103_after_yes(") == 2u) ? 1u : 0u, 1u);
    const std::string wire = body_of(ahh, "static void st103_wire(xlat12_draw_extra *e) {");
    expect_u("S14 st103_wire swaps exactly the three originals, each identity-checked",
             (count(wire, "if (e->desc_tiled_ok == static_cast<TiledFn>(&gfxsrc_desc_tiled_ok)) e->desc_tiled_ok = &st103_tiled_ok;") == 1u &&
              count(wire, "if (e->desc_tiled_okt == static_cast<TiledTFn>(&gfxsrc_desc_tiled_okt)) e->desc_tiled_okt = &st103_tiled_okt;") == 1u &&
              count(wire, "if (e->desc_dcc_ok == static_cast<TiledFn>(&gfxsrc_desc_dcc_ok)) e->desc_dcc_ok = &st103_dcc_ok;") == 1u &&
              count(wire, " = &") == 3u) ? 1u : 0u, 1u);
    const size_t pw = ahh.find("            if (n48_st_active(st103_mode())) st103_wire(&ex);");
    expect_u("S14 the wrappers are wired only while 103 is active, AFTER every original callback is set, exactly once",
             (count(ahh, "st103_wire(&ex)") == 1u && pw != std::string::npos &&
              ahh.find("ex.desc_tiled_ok = &gfxsrc_desc_tiled_ok;") < pw && ahh.find("ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;") < pw &&
              ahh.find("if (gXdResProvLin && gXdResProv) ex.desc_tiled_okt = &gfxsrc_desc_tiled_okt;") < pw) ? 1u : 0u, 1u);
    /* the feed, its condition and its inputs */
    expect_u("S14 the feed: armed (COMMIT-level) frames only, after the gate, with commitOk (the targets: the frame's set)",
             count(ahh, "    if (st103On && arm == N48_SD_ARM_COMMIT) st103_feed(gXdC.judged + 1u, commitOk ? 1u : 0u, hHeld, tgtN);\n"), 1u);
    expect_u("S14 ... it follows the ledger feed (pf_note LEDGER) and precedes the X9 witness block",
             (ahh.find("pf_note(N48_PF_T_LEDGER, pfL);") < ahh.find("st103_feed(gXdC.judged + 1u") &&
              ahh.find("st103_feed(gXdC.judged + 1u") < ahh.find("    if (action != N48_SD_ACT_TRANSLATE) {\n        for (uint32_t q = 0; q < hHeld; q++) n48_dep_note(")) ? 1u : 0u, 1u);
    expect_u("S14 st103On is read once per frame from the mode and the descriptor path",
             count(ahh, "const uint32_t st103On = (dp && n48_st_active(st103_mode())) ? 1u : 0u;"), 1u);
    expect_u("S14 103 gathers the per-draw targets into its OWN state (58's gLedCbt lines untouched)",
             (count(ahh, "    if (ledMid) n48_gcap_cbt_reset(&gLedCbt);\n    if (st103On) n48_gcap_cbt_reset(&gSt103Cbt);") == 1u &&
              count(ahh, "            if (ledMid) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);\n            if (st103On && n48_gcap_cbt_ib(dst, got, &gSt103Cbt) != got) gSt103CbtShort = 1u;") == 1u &&
              count(ahh, "n48_gcap_cbt_reset(&gSt103Cbt)") == 1u && count(ahh, "n48_gcap_cbt_ib(dst, got, &gSt103Cbt)") == 1u &&
              body_of(ahh, "static __attribute__((noinline)) void st103_feed(").find("gLedCbt") == std::string::npos &&
              body_of(ahh, "static __attribute__((noinline)) void st103_frame_targets(").find("gLedCbt") == std::string::npos)
                 ? 1u : 0u, 1u);
    expect_u("S14 the ledger still reads the per-draw targets only under ledMid", count(ahh, "if (ledMid && commitOk) {"), 1u);
    expect_u("S14 the draw's CB0 state is wired while 87 is ON or 103 is active (one assignment)",
             (count(ahh, "if (__atomic_load_n(&gP87.on, __ATOMIC_ACQUIRE) == N48_P87_ON || n48_st_active(st103_mode())) ex.tex_state = &gP87Ts;") == 1u &&
              count(ahh, "ex.tex_state = ") == 1u) ? 1u : 0u, 1u);
    expect_u("S14 the T# observer notes for 103 before 87's early return",
             (ahh.find("if (n48_st_active(st103_mode())) st103_note(id, at_i, heap, rec);") <
              ahh.find("    if (__atomic_load_n(&gP87.on, __ATOMIC_ACQUIRE) != N48_P87_ON) return;   // OFF: one load\n    if (n48_p87_note(")) ? 1u : 0u, 1u);
    expect_u("S14 the frame top (arm scope + window) and the frame end (boxes, signature) are gated on st103On",
             (count(ahh, "if (st103On) st103_frame_top(gXdC.judged + 1u, arm == N48_SD_ARM_COMMIT ? 1u : 0u);") == 1u &&
              count(ahh, "if (st103On) st103_frame_end(gXdC.judged + 1u, commitOk ? 1u : 0u, &f);") == 1u) ? 1u : 0u, 1u);
    expect_u("S14 the arm scope runs before the frame's asks (frame top precedes the policy pass's translate call)",
             ahh.find("if (st103On) st103_frame_top(") < ahh.find("if (st103On) st103_frame_end(") ? 1u : 0u, 1u);
    expect_u("S14 STOP prints this arm's table", count(ahh, "        st103_arm_stop();"), 1u);
    expect_u("S14 both resprov record sites mark a RECORDED copy as a writer (under gXdLock), and nothing else calls it",
             (count(ahh, "        if (why == N48_RP_REC_OK) st103_copy_note(&it[i].c);") == 1u &&
              count(ahh, "    if (why == N48_RP_REC_OK) st103_copy_note(c);   // build 0.0.544 (switch 103): the copy wrote this page\n    IOLockUnlock(gXdLock);") == 1u &&
              count(ahh, "st103_copy_note(") == 3u) ? 1u : 0u, 1u);
    expect_u("S14 dpled841's cap re-opens only with the window epoch",
             count(ahh, "if (dpWinEpoch != gSt103Win.epoch) { dpWinEpoch = gSt103Win.epoch; dpLines = 0u; }"), 1u);
    /* the stash only reads: nothing in the gate reads gSt103 */
    const std::string cgate = body_of(ahh, "static uint32_t gfxsrc_commit_try(");
    expect_u("S14 the commit gate reads nothing of 103", (!cgate.empty() && count(cgate, "gSt103") + count(cgate, "st103") == 0u) ? 1u : 0u, 1u);
}

/* ==== build 0.0.545 ==== */
/* A tiny ON engine over the kext's own pure functions: one frame's reads (each with its draw's CB0 page) against the table(s), the
 * frame's colour-target set (complete, or not when `fcbOn` is 0: the "before" of MUST-FIX 2), the P flag (0 when `pOn` is 0: the
 * "before" of MUST-FIX 3), a verdict per read through n48_st_keep, and the feed: the real table marked with the frame's outcome,
 * the simulated one (when given) with n48_st_sim_exec. `force` refuses the frame for another reason (a refused text frame). */
typedef struct { uint64_t page, cb; } Rd;
typedef struct { const Rd *rd; uint32_t nrd; const uint64_t *tg; uint32_t ntg; uint32_t isP; } Fr;
static n48_st_tab gSim;   /* static: the simulated-ON table */
static n48_st_fr_ctr gFc;
static uint32_t on_frame(n48_st_tab *t, n48_st_tab *sim, uint64_t frame, const Fr &f, uint32_t mode, uint32_t fcbOn, uint32_t pOn,
                         uint32_t force)
{
    static n48_st_fset fs;
    n48_st_fset_begin(&fs, frame, f.isP);
    for (uint32_t k = 0; k < f.ntg; k++) n48_st_fset_add(&fs, 0x400000000ull + k, n48_st_key(f.tg[k], 0u));
    n48_st_fset_close(&fs, fcbOn ? 0u : 1u, f.ntg, N48_GCAP_CBT_MAX, 0u);
    uint32_t commit = force ? 0u : 1u;
    for (uint32_t k = 0; k < f.nrd; k++) {
        n48_st_q q; std::memset(&q, 0, sizeof q);
        q.arm = kArm; q.frame = frame; q.va = 0x400000000ull; q.page = f.rd[k].page; q.proof = N48_ST_P_LEDGER;
        q.cb_known = 1u; q.cb_page = f.rd[k].cb; q.is_p = pOn ? fs.is_p : 0u; q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
        const uint32_t v = n48_st_classify(t, &q, nullptr);
        if (!n48_st_keep(mode, v)) commit = 0u;
        if (mode == N48_ST_M_ON && v == N48_ST_V_STALE) fs.simStale = 1u;
        if (sim && n48_st_classify(sim, &q, nullptr) == N48_ST_V_STALE) fs.simStale = 1u;
    }
    for (uint32_t k = 0; k < fs.n; k++) {
        n48_st_mark_key(t, fs.va[k], fs.key[k], frame, commit, N48_ST_COV_UNKNOWN);
        if (sim) n48_st_mark_key(sim, fs.va[k], fs.key[k], frame, n48_st_sim_exec(commit, fs.simStale), N48_ST_COV_UNKNOWN);
    }
    n48_st_fr_count(&gFc, &fs, commit, 0u);
    return commit;
}
/* THE CASCADE's surfaces (RUN AU's pages where the log has them): the text scratch Tx (the date scratch 0x4011e0000), the login
 * layer L (0x400800000 -> 0x10030000), the blur scratch Sc (0x4010e0000 -> 0x13afa000), the plane (0x401800000 -> 0x10930000,
 * run10u). A: text into Tx. B: U composites Tx into L. C: Z blurs L into Sc and U composites Sc back into L (ONE frame: (a)).
 * P: the plane pass reads L. */
static const uint64_t kTx = kDatePg, kL = kLayerPg, kSc = kScrPg, kPl = 0x10930000ull;
static const Rd kRdB[1] = { { kTx, kL } };
static const Rd kRdC[2] = { { kL, kSc }, { kSc, kL } };
static const Rd kRdP[1] = { { kL, kPl } };
static const uint64_t kTgA[1] = { kTx }, kTgB[1] = { kL }, kTgC[2] = { kSc, kL }, kTgP[1] = { kPl };
static const Fr kA = { nullptr, 0u, kTgA, 1u, 0u }, kB = { kRdB, 1u, kTgB, 1u, 0u }, kC = { kRdC, 2u, kTgC, 2u, 0u },
                kP = { kRdP, 1u, kTgP, 1u, 1u };
typedef struct { uint32_t cCommits, pCommits; uint32_t lExecAtEnd; } Casc;
static Casc cascade(uint32_t mode, uint32_t fcbOn, uint32_t pOn, n48_st_tab *sim)
{
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    if (sim) { std::memset(sim, 0, sizeof *sim); n48_st_arm_scope(sim, kArm); }
    std::memset(&gFc, 0, sizeof gFc);
    Casc r = { 0u, 0u, 0u };
    uint64_t fr = 100u;
    for (uint32_t cyc = 0; cyc <= 20u; cyc++) {
        (void)on_frame(&gT, sim, fr++, kA, mode, fcbOn, pOn, cyc == 1u ? 1u : 0u);   /* cycle 1: ONE refused text frame */
        (void)on_frame(&gT, sim, fr++, kB, mode, fcbOn, pOn, 0u);
        const uint32_t c = on_frame(&gT, sim, fr++, kC, mode, fcbOn, pOn, 0u);
        const uint32_t p = on_frame(&gT, sim, fr++, kP, mode, fcbOn, pOn, 0u);
        if (cyc >= 2u) { r.cCommits += c; r.pCommits += p; }
    }
    const n48_st_row *l = n48_st_find(&gT, n48_st_key(kL, 0u));
    r.lExecAtEnd = (l && l->verdict == N48_ST_EXEC) ? 1u : 0u;
    return r;
}

/* ---- S15: latch (a), the U -> layer -> Z -> P cascade ---- */
static void s15()
{
    Casc b = cascade(N48_ST_M_ON, 0u, 0u, nullptr);
    expect_u("S15 BEFORE (0.0.544: no frame-CB exemption, no P policy): C (Z+U) never commits again (0 of 19 cycles)", b.cCommits, 0u);
    expect_u("S15 BEFORE: ... and P never commits again: the GLASS FREEZES (0 of 19)", b.pCommits, 0u);
    expect_u("S15 BEFORE: ... the layer ends NOT_EXEC", b.lExecAtEnd, 0u);
    Casc a = cascade(N48_ST_M_ON, 1u, 1u, nullptr);
    expect_u("S15 AFTER (frame-CB exemption + P policy): C commits in every cycle after the refused text frame (19 of 19)", a.cCommits, 19u);
    expect_u("S15 AFTER: ... P commits in every cycle (19 of 19)", a.pCommits, 19u);
    expect_u("S15 AFTER: ... the layer ends EXEC", a.lExecAtEnd, 1u);
    Casc x = cascade(N48_ST_M_ON, 1u, 0u, nullptr);
    expect_u("S15 the exemption ALONE breaks latch (a): C 19, P 19", (x.cCommits == 19u && x.pCommits == 19u) ? 1u : 0u, 1u);
    Casc y = cascade(N48_ST_M_ON, 0u, 1u, nullptr);
    expect_u("S15 the P policy alone does NOT: C stays latched (0), P shows the last committed layer (19)",
             (y.cCommits == 0u && y.pCommits == 19u) ? 1u : 0u, 1u);
    /* the exemption itself, one ask: U reads Sc inside C, Sc a target of C (complete set) -> exempt-frame-cb; incomplete -> STALE */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, 0x4010e0000ull, kSc, 0u, 10u, 1u, 0u);
    n48_st_mark(&gT, 0x4010e0000ull, kSc, 0u, 11u, 0u, 0u);
    n48_st_fset fs; n48_st_fset_begin(&fs, 12u, 0u);
    n48_st_fset_add(&fs, 0x4010e0000ull, n48_st_key(kSc, 0u)); n48_st_fset_add(&fs, 0x400800000ull, n48_st_key(kL, 0u));
    n48_st_fset_close(&fs, 0u, 2u, N48_GCAP_CBT_MAX, 0u);
    n48_st_q q = ask(12u, 0x4010e0000ull, kSc, N48_ST_P_LEDGER, kL, 0u);
    q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
    expect_u("S15 one ask: Sc is a target of the same frame (complete set) -> exempt-frame-cb", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_EXEMPT_FCB);
    expect_u("S15 ... ON keeps it", n48_st_keep(N48_ST_M_ON, n48_st_classify(&gT, &q, nullptr)), 1u);
    n48_st_fset_close(&fs, 1u, 2u, N48_GCAP_CBT_MAX, 0u);   /* the per-draw pass could not see a target */
    q.fcb_ok = fs.ok;
    expect_u("S15 ... the SAME set, incomplete (unseen): no exemption -> STALE (the stricter ON)", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_STALE);
}

/* ---- S16: latch (b), P frames sampling the plane pages; and the clock is still refused ---- */
static void s16()
{
    /*: the 1440p P-slot CB0s 0x400100000 / 0x404000000 (identity 39 samples them). SYNTHETIC pages. */
    const uint64_t pa = 0x10100000ull, pb = 0x10400000ull;
    const Rd rA[1] = { { pa, pb } }, rB[1] = { { pb, pa } };
    const uint64_t tA[1] = { pa }, tB[1] = { pb };
    const Fr toA = { rB, 1u, tA, 1u, 1u }, toB = { rA, 1u, tB, 1u, 1u };   /* a P writing A reads B, and the other way round */
    uint32_t before = 0u, after = 0u;
    for (uint32_t pOn = 0; pOn < 2u; pOn++) {
        std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
        uint32_t c = 0u; uint64_t fr = 200u;
        for (uint32_t i = 0; i < 40u; i++) {
            const uint32_t k = on_frame(&gT, nullptr, fr, (i & 1u) ? toB : toA, N48_ST_M_ON, 1u, pOn, i == 4u ? 1u : 0u);
            if (i > 4u) c += k;
            fr++;
        }
        if (pOn) after = c; else before = c;
    }
    expect_u("S16 BEFORE (no P policy): after ONE refused P, every later P is refused (0 of 35)", before, 0u);
    expect_u("S16 AFTER (P policy): every later P commits (35 of 35)", after, 35u);
    /* the S1 clock with every new rule on: never committed this arm, not a target of U's frame -> still STALE, P or not */
    run_to_f482(&gT);
    n48_st_fset fs; n48_st_fset_begin(&fs, 483u, 0u);
    n48_st_fset_add(&fs, kLayerVa, n48_st_key(kLayerPg, 0u)); n48_st_fset_add(&fs, kScrVa, n48_st_key(kScrPg, 0u));
    n48_st_fset_close(&fs, 0u, 2u, N48_GCAP_CBT_MAX, 0u);
    n48_st_q q = ask(483u, kClockVa, kClockPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
    expect_u("S16 S1's clock (0x13b6f000, writers F22/F27 refused) with the complete frame set: STALE", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_STALE);
    /* RE-BASELINED 0.0.546: a P frame's LEDGER proof now counts as once-committed, so the P variant of this ask
     * is asked with a RESPROV proof (no in-arm commit) and stays STALE; the real clock asker (U) is not a P and stays STALE above. */
    q.is_p = 1u;
    q.proof = N48_ST_P_RESPROV;
    expect_u("S16 ... asked by a P frame with a resprov proof (never committed this arm: exec_seq 0): STALE",
             n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    expect_u("S16 ... and ON refuses it", n48_st_keep(N48_ST_M_ON, n48_st_classify(&gT, &q, nullptr)), 0u);
    q.proof = N48_ST_P_LEDGER;
    expect_u("S16 0.0.546: the same page asked by a P frame with a LEDGER proof: p-once (the ledger proof is an in-arm commit)",
             n48_st_classify(&gT, &q, nullptr), N48_ST_V_P_ONCE);
    n48_st_mark(&gT, kClockVa, kClockPg, 0u, 483u, 1u, 0u);   /* one committed write ... */
    n48_st_mark(&gT, kClockVa, kClockPg, 0u, 484u, 0u, 0u);   /* ... then a refused one */
    q.frame = 485u;
    expect_u("S16 a page committed once, last writer refused, asked by P: counted p-once, not refused", n48_st_classify(&gT, &q, nullptr),
             N48_ST_V_P_ONCE);
    q.is_p = 0u;
    expect_u("S16 ... the same page asked by a non-P frame: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
}

/* ---- S17: the simulated-ON table in SHADOW ---- */
static void s17()
{
    /* SHADOW with 0.0.544's rules (no exemption, no P policy in the simulation): SHADOW commits everything but the forced refusal */
    Casc sh = cascade(N48_ST_M_SHADOW, 0u, 0u, &gSim);
    expect_u("S17 SHADOW commits every frame (C 19, P 19): SHADOW never changes a verdict", (sh.cCommits == 19u && sh.pCommits == 19u) ? 1u : 0u, 1u);
    const n48_st_row *lr = n48_st_find(&gT, n48_st_key(kL, 0u)), *ls = n48_st_find(&gSim, n48_st_key(kL, 0u));
    expect_u("S17 the REAL table: the layer EXEC (fed with commitOk)", (lr && lr->verdict == N48_ST_EXEC) ? 1u : 0u, 1u);
    expect_u("S17 the SIMULATED-ON table: the layer NOT_EXEC (what ON would have recorded)", (ls && ls->verdict == N48_ST_NOT_EXEC) ? 1u : 0u, 1u);
    expect_u("S17 ON WOULD REFUSE: P frames counted, and all were committed here", (gFc.wouldP >= 19u && gFc.wouldCommP == gFc.wouldP) ? 1u : 0u, 1u);
    expect_u("S17 ... other frames counted (C in every cycle)", gFc.wouldOther >= 19u ? 1u : 0u, 1u);
    expect_u("S17 P delivered actual 21 vs simulated-ON 1 (only cycle 0's)", (gFc.pActual == 21u && gFc.pSim == 1u) ? 1u : 0u, 1u);
    /* with 0.0.545's rules the simulation recovers: the layer EXEC in both, simulated P delivered close to actual */
    Casc sh2 = cascade(N48_ST_M_SHADOW, 1u, 1u, &gSim);
    ls = n48_st_find(&gSim, n48_st_key(kL, 0u));
    expect_u("S17 0.0.545 rules: the simulated layer ends EXEC; simulated-ON P delivered == actual (21)",
             (sh2.pCommits == 19u && ls && ls->verdict == N48_ST_EXEC && gFc.pSim == gFc.pActual && gFc.pActual == 21u) ? 1u : 0u, 1u);
    expect_u("S17 n48_st_sim_exec: committed and no STALE -> 1; STALE -> 0; refused -> 0",
             (n48_st_sim_exec(1u, 0u) == 1u && n48_st_sim_exec(1u, 1u) == 0u && n48_st_sim_exec(0u, 0u) == 0u) ? 1u : 0u, 1u);
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    std::memset(&gSim, 0, sizeof gSim); n48_st_arm_scope(&gSim, kArm);
    for (uint32_t k = 0; k <= N48_ST_ROWS; k++) {   /* 257 targets: committed in SHADOW, refused in the simulation */
        n48_st_mark(&gT, 0x400000000ull, (uint64_t)(k + 1u) << 12, 0u, 10u + k, 1u, 0u);
        n48_st_mark(&gSim, 0x400000000ull, (uint64_t)(k + 1u) << 12, 0u, 10u + k, n48_st_sim_exec(1u, 1u), 0u);
    }
    expect_u("S17 LOST is counted per table: the simulated table lost 1 refused row, the real one evicted an EXEC row and lost 0",
             (gSim.lost == 1u && gT.lost == 0u && gT.evictExec == 1u) ? 1u : 0u, 1u);
}

/* ---- S18: isSys in the key ---- */
static void s18()
{
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, 0x400000000ull, 0x5000ull, 1u, 10u, 0u, 0u);   /* system page 0x5000, refused writer */
    n48_st_mark(&gT, 0x400010000ull, 0x5000ull, 0u, 11u, 1u, 0u);   /* VRAM page 0x5000, committed writer */
    expect_u("S18 the same page number, sys vs VRAM: two rows", gT.used, 2u);
    n48_st_q q = ask(12u, 0x400000000ull, 0x5000ull, N48_ST_P_LEDGER, 0u, 0u);
    expect_u("S18 the VRAM read: fresh", n48_st_classify(&gT, &q, nullptr), N48_ST_V_FRESH);
    q.sys = 1u;
    expect_u("S18 the system read: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    expect_u("S18 key 0 for an unresolved page, either space", n48_st_key(0u, 1u) | n48_st_key(0u, 0u), 0u);
}

/* ---- S19: trunc ---- */
static void s19()
{
    n48_st_fset fs; n48_st_fset_begin(&fs, 1u, 0u);
    for (uint32_t k = 0; k < 12u; k++) n48_st_fset_add(&fs, 0x400000000ull + k * 0x10000ull, (uint64_t)(k + 1u) << 12);
    n48_st_fset_close(&fs, 0u, 12u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S19 12 targets, 8 held, the per-draw pass complete: truncated 0", n48_st_trunc(&fs, 12u, 8u, 12u, N48_GCAP_CBT_MAX), 0u);
    n48_st_fset_close(&fs, 1u, 12u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S19 ... the per-draw pass incomplete (unseen): the 4 past the held rows count", n48_st_trunc(&fs, 12u, 8u, 12u, N48_GCAP_CBT_MAX), 4u);
    n48_st_fset_close(&fs, 0u, 35u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S19 ... 35 per-draw targets past a cap of 32: 3 + the 4 (the pass incomplete)", n48_st_trunc(&fs, 12u, 8u, 35u, N48_GCAP_CBT_MAX), 7u);
    n48_st_fset_begin(&fs, 2u, 0u);
    for (uint32_t k = 0; k < N48_ST_FCB_MAX + 3u; k++) n48_st_fset_add(&fs, 0x400000000ull, (uint64_t)(k + 1u) << 12);
    n48_st_fset_close(&fs, 0u, 30u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S19 targets past the set's own cap (40): counted, and the set is incomplete",
             (n48_st_trunc(&fs, 0u, 0u, 30u, N48_GCAP_CBT_MAX) == 3u && fs.ok == 0u) ? 1u : 0u, 1u);
}

/* ---- S20: the recovery times ---- */
static void s20()
{
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    gT.role_now = N48_ST_ROLE_PLANE;
    gT.now = 1000u;    n48_st_mark(&gT, 0x401800000ull, kPl, 0u, 1u, 1u, 0u);
    gT.now = 2000u;    n48_st_mark(&gT, 0x401800000ull, kPl, 0u, 2u, 0u, 0u);   /* NOT_EXEC from 2000 */
    gT.now = 900000u;  n48_st_mark(&gT, 0x401800000ull, kPl, 0u, 3u, 0u, 0u);   /* still NOT_EXEC: the clock does not restart */
    gT.now = 2502000u; n48_st_mark(&gT, 0x401800000ull, kPl, 0u, 4u, 1u, 0u);   /* EXEC again after 2.5 s */
    expect_u("S20 a plane row back to EXEC: count 1, longest 2 500 000 us, > 1 s 1",
             (gT.recN == 1u && gT.recMaxUs == 2500000u && gT.recOver1s == 1u) ? 1u : 0u, 1u);
    gT.role_now = 0u;
    gT.now = 3000000u; n48_st_mark(&gT, 0x402000000ull, 0x20000000ull, 0u, 5u, 0u, 0u);   /* no role: not a plane/layer */
    gT.now = 9000000u; n48_st_mark(&gT, 0x402000000ull, 0x20000000ull, 0u, 6u, 1u, 0u);
    expect_u("S20 a row with no plane/layer role is not counted", gT.recN, 1u);
    gT.now = 3000000u; n48_st_mark(&gT, 0x401800000ull, kPl, 0u, 7u, 0u, 0u);
    n48_st_role(&gT, kArm, n48_st_key(0x20000000ull, 0u), N48_ST_ROLE_LAYER);
    gT.now = 3500000u; n48_st_mark(&gT, 0x402000000ull, 0x20000000ull, 0u, 8u, 0u, 0u);
    uint64_t n = 0, mx = 0, o = 0;
    n48_st_open(&gT, 5000000u, &n, &mx, &o);
    expect_u("S20 at STOP: two plane/layer rows STILL NOT_EXEC (a latch), longest 2 000 000 us, > 1 s 2",
             (n == 2u && mx == 2000000u && o == 2u) ? 1u : 0u, 1u);
}

/* ---- S21: the frame target set ---- */
static void s21()
{
    n48_st_fset fs; n48_st_fset_begin(&fs, 7u, 1u);
    expect_u("S21 a fresh set proves nothing (why NONE, ok 0)", (fs.ok == 0u && (fs.why & N48_ST_FS_NONE)) ? 1u : 0u, 1u);
    n48_st_fset_add(&fs, 0x400800000ull, n48_st_key(kLayerPg, 0u));
    n48_st_fset_add(&fs, 0x400800000ull, n48_st_key(kLayerPg, 0u));
    n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S21 one target (a duplicate stored once), nothing missing: complete", (fs.ok == 1u && fs.n == 1u) ? 1u : 0u, 1u);
    n48_st_fset_close(&fs, 1u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S21 unseen by the per-draw pass: incomplete", fs.ok, 0u);
    n48_st_fset_close(&fs, 0u, 33u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S21 more per-draw targets than 32: incomplete", fs.ok, 0u);
    n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 1u);
    expect_u("S21 a short per-draw walk: incomplete", fs.ok, 0u);
    n48_st_fset_begin(&fs, 8u, 0u);
    n48_st_fset_add(&fs, 0x400800000ull, 0u);
    n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S21 a target that did not resolve: incomplete (counted)", (fs.ok == 0u && fs.unres == 1u) ? 1u : 0u, 1u);
    /* an incomplete set never exempts, whatever it holds */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, kScrVa, kScrPg, 0u, 5u, 0u, 0u);
    n48_st_fset_begin(&fs, 6u, 0u); n48_st_fset_add(&fs, kScrVa, n48_st_key(kScrPg, 0u)); n48_st_fset_close(&fs, 1u, 1u, 32u, 0u);
    n48_st_q q = ask(6u, kScrVa, kScrPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
    expect_u("S21 an incomplete set holding the page: STALE (fail toward no exemption)", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    q.fcb_ok = 1u; q.fcb = nullptr;
    expect_u("S21 no key list: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
}

/* ---- S22: the 0.0.545 glue ---- */
static void s22(const std::string &ahh)
{
    if (ahh.empty()) { expect_u("S22 AppleHardwareHook.cpp given", 0u, 1u); return; }
    const std::string dec = body_of(ahh, "static uint32_t gfxsrc_decide_frame(");
    const size_t fill = dec.find("        if (st103On && arm == N48_SD_ARM_COMMIT) st103_frame_targets(vm, gXdC.judged + 1u, pShape, f.reader_ok, hVa, hPage, hSys, hHeld);\n");
    const size_t pset = dec.find("        pShape = n48_p73_is_p_shape(shapeOk, f.nib, f.ib[0].len);\n");
    const size_t pol = dec.find("                gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);");
    const size_t loop = dec.find("            if (st103On && n48_gcap_cbt_ib(dst, got, &gSt103Cbt) != got) gSt103CbtShort = 1u;");
    expect_u("S22 ORDER: the per-draw pass (IB loop) -> pShape -> the frame's target set -> the policy pass (its asks)",
             (fill != std::string::npos && pset != std::string::npos && pol != std::string::npos && loop != std::string::npos &&
              loop < pset && pset < fill && fill < pol && count(dec, "st103_frame_targets(") == 1u) ? 1u : 0u, 1u);
    expect_u("S22 the P recogniser is ONE predicate: no literal 1040 comparison left in the hook, four readers of pShape",
             (count(ahh, "len == 1040u") == 0u && count(ahh, "n48_p73_is_p_shape(") == 1u &&
              count(dec, "cy515_judge(gXdC.judged + 1u, commitOk ? 1u : 0u, pShape,") == 1u && count(dec, "gP73J.isP = pShape;") == 1u &&
              count(dec, "gC80J.isShape = pShape;") == 1u) ? 1u : 0u, 1u);
    const std::string top = body_of(ahh, "static __attribute__((noinline)) void st103_frame_top(");
    expect_u("S22 the frame top invalidates the set and the short-walk flag BEFORE anything asks",
             (count(top, "n48_st_fset_begin(&gSt103Fs, 0ull, 0u);") == 1u && count(top, "gSt103CbtShort = 0u;") == 1u &&
              count(top, "gSt103C.lines = 0u; gSt103C.exLines = 0u;") == 1u &&
              top.find("n48_st_fset_begin(&gSt103Fs, 0ull, 0u);") < top.find("if (gXdShot.state != N48_CM_SHOT_ARMED")) ? 1u : 0u, 1u);
    const std::string tg = body_of(ahh, "static __attribute__((noinline)) void st103_frame_targets(");
    expect_u("S22 the set: every held row (key with isSys) and every per-draw target, closed with the pass's own completeness",
             (count(tg, "n48_st_fset_add(&fs, hVa[q], n48_st_key(hPage[q], hSys[q]));") == 1u &&
              count(tg, "n48_st_fset_add(&fs, va, n48_st_key(page, sys ? 1u : 0u));") == 1u &&
              count(tg, "n48_st_fset_close(&fs, gSt103Cbt.unseen, gSt103Cbt.n, N48_GCAP_CBT_MAX, gSt103CbtShort);") == 1u) ? 1u : 0u, 1u);
    const std::string after = body_of(ahh, "static __attribute__((noinline)) uint32_t st103_after_yes(");
    expect_u("S22 the ask: the set only for its own frame, its completeness as proven (never forced)",
             count(after, "if (fs.frame == q.frame) { q.is_p = fs.is_p; q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n; }"), 1u);
    expect_u("S22 the ask: no walk when no table of this arm can answer; the original's walk reused only for the same VA",
             (after.find("{ gSt103C.unarmed++; return 1u; }") < after.find("gfxc_page(") &&
              count(after, "if (!page && gXdAskPg.ok && gXdAskPg.va == va) {") == 1u) ? 1u : 0u, 1u);
    expect_u("S22 the ask: the simulated table only in SHADOW; its answer feeds simStale and the counters, never the return",
             (count(after, "const uint32_t sim = mode == N48_ST_M_SHADOW ? 1u : 0u;") == 1u &&
              count(after, "uint32_t vs = sim ? n48_st_classify(&gSt103Sim, &q, &srow) : v;") == 1u &&
              count(after, "if (vs == N48_ST_V_STALE) fs.simStale = 1u;") == 1u && count(after, "n48_st_keep(mode, vs)") == 0u) ? 1u : 0u, 1u);
    expect_u("S22 the originals record their walk; each wrapper clears it before calling its original",
             (count(ahh, "gXdAskPg.va = va; gXdAskPg.page = askPage; gXdAskPg.sys = askSys ? 1u : 0u; gXdAskPg.ok = 1u;") == 1u &&
              count(ahh, "gXdAskPg.va = va; gXdAskPg.page = dccPage; gXdAskPg.sys = dccSys ? 1u : 0u; gXdAskPg.ok = 1u;") == 1u &&
              count(ahh, "    gXdAskPg.ok = 0u;   // build 0.0.545: only THIS ask's walk may be reused\n") == 3u) ? 1u : 0u, 1u);
    const std::string feed = body_of(ahh, "static __attribute__((noinline)) void st103_feed(");
    expect_u("S22 the feed: real table with commitOk, simulated with n48_st_sim_exec (SHADOW only), truncated from n48_st_trunc",
             (count(feed, "const uint32_t simExec = n48_st_sim_exec(exec, fs.simStale);") == 1u &&
              count(feed, "n48_st_mark_key(&gSt103Tab, fs.va[i], fs.key[i], frame, exec, N48_ST_COV_UNKNOWN);") == 1u &&
              count(feed, "if (sim) n48_st_mark_key(&gSt103Sim, fs.va[i], fs.key[i], frame, simExec, N48_ST_COV_UNKNOWN);") == 1u &&
              count(feed, "const uint32_t sim = st103_mode() == N48_ST_M_SHADOW ? 1u : 0u;") == 1u &&
              count(feed, "n48_st_trunc(&fs, tgtN, hHeld, gSt103Cbt.n, N48_GCAP_CBT_MAX)") == 1u &&
              count(feed, "if (fs.frame != frame) return;") == 1u) ? 1u : 0u, 1u);
    expect_u("S22 both tables are file-scope static and arm-scoped together; the verb re-scopes both",
             (count(ahh, "static n48_st_tab gSt103Sim {};") == 1u && count(ahh, "    n48_st_reset(&gSt103Sim, arm);") == 1u &&
              count(ahh, "            gSt103Sim.arm = 0ull;") == 1u) ? 1u : 0u, 1u);
    const std::string rep2 = body_of(ahh, "static __attribute__((noinline)) void st103_report2() {");
    expect_u("S22 STOP: the simulated table, both tables' recovery times, the sets, and ON EVIDENCE VOID when LOST > 0",
             (count(rep2, "HWLOG(N48_ST_SIM_FMT,") == 1u && count(rep2, "HWLOG(N48_ST_REC_FMT,") == 1u &&
              count(rep2, "HWLOG(N48_ST_FS_FMT,") == 1u && count(rep2, "if (n48_st_void(&gSt103Tab, &gSt103Sim))") == 1u &&
              count(ahh, "    st103_report2();") == 1u) ? 1u : 0u, 1u);
    const std::string gate = body_of(ahh, "static int st103_gate(void *ctx, uint64_t va, uint32_t mode, uint32_t dcc) {");
    expect_u("S22 no fail-closed on overflow: LOST is read by no ask, gate or feed - only the report",
             (count(after, "lost") + count(gate, "lost") + count(feed, "lost") + count(tg, "lost") == 0u &&
              count(after, "evictRole") + count(gate, "evictRole") + count(feed, "evictRole") + count(tg, "evictRole") +
              count(after, "n48_st_void") + count(gate, "n48_st_void") + count(feed, "n48_st_void") + count(tg, "n48_st_void") == 0u &&
              count(rep2, "lost") == 3u && count(ahh, "n48_st_void(") == 1u) ? 1u : 0u, 1u);   /* RE-BASELINED 0.0.546: 5 -> 3 (the condition is n48_st_void) */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, kScrVa, kScrPg, 0u, 5u, 0u, 0u);
    n48_st_q q = ask(6u, kScrVa, kScrPg, N48_ST_P_LEDGER, 0u, 0u);
    const uint32_t a0 = n48_st_classify(&gT, &q, nullptr);
    gT.lost = 1000u;
    expect_u("S22 ... n48_st_classify never reads LOST (a table that lost rows answers the same)",
             (a0 == n48_st_classify(&gT, &q, nullptr) && a0 == N48_ST_V_STALE) ? 1u : 0u, 1u);
}


/* ==== build 0.0.546 ==== */
/* 0.0.545's n48_st_slot, n48_st_mark_key and n48_st_classify's P rule, VERBATIM from 2fbad453 (the "before" of MUST-FIX 1). */
static n48_st_row *slot545(n48_st_tab *t)
{
    if (t->used < N48_ST_ROWS) return &t->r[t->used++];
    uint32_t best = N48_ST_ROWS;
    for (uint32_t k = 0; k < N48_ST_ROWS; k++)
        if (t->r[k].verdict == N48_ST_EXEC && (best == N48_ST_ROWS || t->r[k].seq < t->r[best].seq)) best = k;
    if (best != N48_ST_ROWS) { t->evictExec++; return &t->r[best]; }
    best = 0u;
    for (uint32_t k = 1; k < N48_ST_ROWS; k++) if (t->r[k].seq < t->r[best].seq) best = k;
    t->lost++;
    return &t->r[best];
}
static void mark545(n48_st_tab *t, uint64_t va, uint64_t key, uint64_t seq, uint32_t exec, uint32_t cov)
{
    if (!t) return;
    if (!key) { t->unresolved++; return; }
    n48_st_row *r = n48_st_find(t, key);
    if (r && r->seq > seq) { t->outOfOrder++; return; }
    const uint32_t wasNot = (r && r->verdict == N48_ST_NOT_EXEC) ? 1u : 0u;
    if (!r) {
        r = slot545(t);
        r->page = key; r->exec_seq = 0ull; r->role = 0u; r->ns_us = 0ull;
    }
    r->role |= t->role_now;
    r->va = va; r->seq = seq; r->verdict = exec ? N48_ST_EXEC : N48_ST_NOT_EXEC; r->cov = cov;
    if (exec) {
        if (wasNot && r->role) {
            const uint64_t d = t->now > r->ns_us ? t->now - r->ns_us : 0ull;
            t->recN++;
            if (d > t->recMaxUs) t->recMaxUs = d;
            if (d > 1000000ull) t->recOver1s++;
        }
        r->exec_seq = seq; r->ns_us = 0ull; t->markExec++;
    } else {
        if (!wasNot) r->ns_us = t->now;
        t->markRef++;
    }
}
static uint32_t classify545(const n48_st_tab *t, const n48_st_q *q, const n48_st_row **out)
{
    if (out) *out = 0;
    if (!q || !q->page) return N48_ST_V_NOPAGE;
    const uint64_t key = n48_st_key(q->page, q->sys);
    const n48_st_row *r = n48_st_find_c(t, q->arm, key);
    if (!r) return N48_ST_V_NOROW;
    if (out) *out = r;
    if (r->verdict == N48_ST_EXEC) return N48_ST_V_FRESH;
    if (q->proof != N48_ST_P_FRAMELOCAL && !(r->seq > n48_st_proof_seq(r, q->proof, q->frame))) return N48_ST_V_OLDER;
    if (q->cb_known && q->cb_page && n48_st_key(q->cb_page, q->cb_sys) == key) return N48_ST_V_EXEMPT_SELF;
    if (q->proof == N48_ST_P_FRAMELOCAL || q->same_frame) return N48_ST_V_EXEMPT_SAME;
    if (q->fcb_ok && q->fcb)
        for (uint32_t k = 0; k < q->fcb_n; k++) if (q->fcb[k] == key) return N48_ST_V_EXEMPT_FCB;
    if (q->is_p && r->exec_seq != 0ull) return N48_ST_V_P_ONCE;
    return N48_ST_V_STALE;
}
typedef void (*MarkFn)(n48_st_tab *, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
typedef uint32_t (*ClsFn)(const n48_st_tab *, const n48_st_q *, const n48_st_row **);
/*'s latch, under ON: two planes A / B written by P frames (each reads the other: 1440p's identity 39), then 300 non-P frames
 * each naming a new colour target (eviction pressure), then ONE refused writer of B, then 40 P frames. Returns the Ps committed. */
static uint32_t latch_run(MarkFn mk, ClsFn cl, uint32_t proof, uint64_t *evictRole)
{
    const uint64_t pa = 0x10100000ull, pb = 0x10400000ull;
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    uint64_t fr = 1u;
    uint32_t done = 0u;
    for (uint32_t i = 0; i < 50u; i++) {
        const uint64_t tgt = (i & 1u) ? pb : pa, rd = (i & 1u) ? pa : pb;
        n48_st_fset fs; n48_st_fset_begin(&fs, fr, 1u);
        n48_st_fset_add(&fs, 0x401800000ull, n48_st_key(tgt, 0u));
        n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
        uint32_t commit = 1u;
        if (i >= 10u) {   /* the first 10 P frames commit before the pressure; the 40 after the refused writer are counted */
            n48_st_q q; std::memset(&q, 0, sizeof q);
            q.arm = kArm; q.frame = fr; q.va = 0x404000000ull; q.page = rd; q.proof = proof; q.cb_known = 1u; q.cb_page = tgt;
            q.is_p = 1u; q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
            commit = n48_st_keep(N48_ST_M_ON, cl(&gT, &q, nullptr));
            done += commit;
        }
        gT.role_now = N48_ST_ROLE_PLANE;
        mk(&gT, 0x401800000ull, fs.key[0], fr, commit, N48_ST_COV_UNKNOWN);
        gT.role_now = 0u;
        fr++;
        if (i == 9u) {
            for (uint32_t k = 0; k < 300u; k++, fr++)   /* 300 non-P committed frames, one new target each */
                mk(&gT, 0x402000000ull, 0x20000000ull + ((uint64_t)k << 12), fr, 1u, N48_ST_COV_UNKNOWN);
            mk(&gT, 0x404000000ull, n48_st_key(pb, 0u), fr++, 0u, N48_ST_COV_UNKNOWN);   /* ONE refused writer of plane B (the next P reads B) */
        }
    }
    if (evictRole) *evictRole = gT.evictRole;
    return done;
}
static void mark_new(n48_st_tab *t, uint64_t va, uint64_t key, uint64_t seq, uint32_t exec, uint32_t cov) { n48_st_mark_key(t, va, key, seq, exec, cov); }

/* ---- S23: the eviction latch and its fix ---- */
static void s23()
{
    expect_u("S23 BEFORE (0.0.545 slot + classify, ledger proof): the evicted plane row returns with exec_seq 0: every later P STALE (0 of 40)",
             latch_run(&mark545, &classify545, N48_ST_P_LEDGER, nullptr), 0u);
    expect_u("S23 BEFORE (resprov proof): 0 of 40", latch_run(&mark545, &classify545, N48_ST_P_RESPROV, nullptr), 0u);
    uint64_t er = 99u;
    expect_u("S23 AFTER (0.0.546 slot + classify, ledger proof): 40 of 40", latch_run(&mark_new, &n48_st_classify, N48_ST_P_LEDGER, &er), 40u);
    expect_u("S23 AFTER: the pressure evicted no plane row (evictRole 0)", er, 0u);
    expect_u("S23 AFTER (resprov proof): 40 of 40 - the role rows kept their exec_seq",
             latch_run(&mark_new, &n48_st_classify, N48_ST_P_RESPROV, nullptr), 40u);
    expect_u("S23 the new slot ALONE (0.0.545 classify, resprov proof): 40 of 40", latch_run(&mark_new, &classify545, N48_ST_P_RESPROV, nullptr), 40u);
    expect_u("S23 the ledger clause ALONE (0.0.545 slot, ledger proof): 40 of 40", latch_run(&mark545, &n48_st_classify, N48_ST_P_LEDGER, nullptr), 40u);
    expect_u("S23 the ledger clause does not cover a resprov proof (0.0.545 slot): still 0 of 40",
             latch_run(&mark545, &n48_st_classify, N48_ST_P_RESPROV, nullptr), 0u);
    /* one ask: a page this arm never committed (exec_seq 0), last writer refused */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, 0x401800000ull, 0x10100000ull, 0u, 7u, 0u, 0u);
    n48_st_q q = ask(8u, 0x401800000ull, 0x10100000ull, N48_ST_P_LEDGER, 0x10400000ull, 0u);
    q.is_p = 1u;
    expect_u("S23 a ledger-proved P, exec_seq 0: p-once (passes under ON)", n48_st_classify(&gT, &q, nullptr), N48_ST_V_P_ONCE);
    expect_u("S23 ... ON keeps it", n48_st_keep(N48_ST_M_ON, n48_st_classify(&gT, &q, nullptr)), 1u);
    expect_u("S23 ... 0.0.545 refused it (STALE)", classify545(&gT, &q, nullptr), N48_ST_V_STALE);
    q.proof = N48_ST_P_RESPROV;
    expect_u("S23 ... the same with a resprov proof: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    q.proof = N48_ST_P_LEDGER; q.is_p = 0u;
    expect_u("S23 ... the same ledger ask by a non-P frame: STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    /* the S1 clock: U (not a P), ledger proof, complete set without the clock page -> still refused */
    run_to_f482(&gT);
    n48_st_fset fs; n48_st_fset_begin(&fs, 483u, 0u);
    n48_st_fset_add(&fs, kLayerVa, n48_st_key(kLayerPg, 0u));
    n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    q = ask(483u, kClockVa, kClockPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
    expect_u("S23 the S1 clock (U, not a P, ledger proof): STALE, and ON refuses it",
             (n48_st_classify(&gT, &q, nullptr) == N48_ST_V_STALE && n48_st_keep(N48_ST_M_ON, n48_st_classify(&gT, &q, nullptr)) == 0u) ? 1u : 0u, 1u);
    /* role rows under pressure */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    gT.role_now = N48_ST_ROLE_PLANE;
    for (uint32_t k = 0; k < 4u; k++) n48_st_mark(&gT, 0x401800000ull, 0x10000000ull + ((uint64_t)k << 12), 0u, 1u + k, 1u, 0u);  /* oldest */
    gT.role_now = 0u;
    n48_st_mark(&gT, 0x401900000ull, 0x11000000ull, 0u, 5u, 0u, 0u);
    n48_st_role(&gT, kArm, n48_st_key(0x11000000ull, 0u), N48_ST_ROLE_LAYER);   /* a LAYER row, NOT_EXEC, old */
    for (uint32_t k = 0; k < N48_ST_ROWS - 5u; k++) n48_st_mark(&gT, 0x402000000ull, 0x20000000ull + ((uint64_t)k << 12), 0u, 10u + k, 1u, 0u);
    for (uint32_t k = 0; k < 600u; k++) n48_st_mark(&gT, 0x403000000ull, 0x30000000ull + ((uint64_t)k << 12), 0u, 1000u + k, (k & 1u), 0u);
    uint32_t alive = 0u;
    for (uint32_t k = 0; k < 4u; k++) alive += n48_st_find(&gT, 0x10000000ull + ((uint64_t)k << 12)) ? 1u : 0u;
    alive += n48_st_find(&gT, 0x11000000ull) ? 1u : 0u;
    expect_u("S23 role rows survive 600 new rows of pressure (5 of 5; the oldest rows of the table)", alive, 5u);
    expect_u("S23 ... evictRole 0; the refused non-role rows that fell out are LOST", (gT.evictRole == 0u && gT.lost > 0u) ? 1u : 0u, 1u);
    /* only role rows: the oldest goes, counted evictRole (and lost when NOT_EXEC) */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    gT.role_now = N48_ST_ROLE_PLANE;
    for (uint32_t k = 0; k < N48_ST_ROWS; k++) n48_st_mark(&gT, 0x401800000ull, 0x10000000ull + ((uint64_t)k << 12), 0u, 100u + k, k != 3u, 0u);
    n48_st_mark(&gT, 0x401800000ull, 0x10000000ull + (3ull << 12), 0u, 50u, 0u, 0u);   /* out of order: ignored, row 3 stays seq 103 */
    gT.role_now = 0u;
    n48_st_mark(&gT, 0x402000000ull, 0x2f000000ull, 0u, 999u, 1u, 0u);
    expect_u("S23 only role rows: the OLDEST (seq 100) goes, evictRole 1, lost 0 (it was EXEC)",
             (n48_st_find(&gT, 0x10000000ull) == nullptr && gT.evictRole == 1u && gT.lost == 0u && n48_st_find(&gT, 0x2f000000ull)) ? 1u : 0u, 1u);
    n48_st_mark(&gT, 0x402000000ull, 0x2f001000ull, 0u, 1000u, 1u, 0u);
    n48_st_mark(&gT, 0x402000000ull, 0x2f002000ull, 0u, 1001u, 1u, 0u);   /* the non-role rows go first again */
    expect_u("S23 ... then the newer non-role rows are evicted before any role row (evictExec 2, evictRole still 1)",
             (gT.evictRole == 1u && gT.evictExec == 2u && n48_st_find(&gT, 0x2f002000ull) && n48_st_find(&gT, 0x10001000ull)) ? 1u : 0u, 1u);
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    gT.role_now = N48_ST_ROLE_PLANE;
    for (uint32_t k = 0; k < N48_ST_ROWS; k++) n48_st_mark(&gT, 0x401800000ull, 0x10000000ull + ((uint64_t)k << 12), 0u, 100u + k, k != 0u, 0u);
    gT.role_now = 0u;
    n48_st_mark(&gT, 0x402000000ull, 0x2f000000ull, 0u, 999u, 1u, 0u);
    expect_u("S23 only role rows, the oldest NOT_EXEC: evictRole 1 AND lost 1", (gT.evictRole == 1u && gT.lost == 1u) ? 1u : 0u, 1u);
    /* ON EVIDENCE VOID */
    static n48_st_tab sim; std::memset(&sim, 0, sizeof sim);
    std::memset(&gT, 0, sizeof gT);
    expect_u("S23 n48_st_void: clean tables -> 0", n48_st_void(&gT, &sim), 0u);
    gT.evictRole = 1u;
    expect_u("S23 n48_st_void: a real role-row eviction alone -> VOID", n48_st_void(&gT, &sim), 1u);
    gT.evictRole = 0u; sim.evictRole = 2u;
    expect_u("S23 n48_st_void: a simulated role-row eviction alone -> VOID", n48_st_void(&gT, &sim), 1u);
    sim.evictRole = 0u; sim.lost = 1u;
    expect_u("S23 n48_st_void: simulated LOST -> VOID (as 0.0.545)", n48_st_void(&gT, &sim), 1u);
    sim.lost = 0u; gT.lost = 1u;
    expect_u("S23 n48_st_void: real LOST -> VOID (as 0.0.545)", n48_st_void(&gT, &sim), 1u);
    gT.lost = 0u; gT.evictExec = 50u; sim.evictExec = 50u;
    expect_u("S23 n48_st_void: evictExec (non-role rows) does not void", n48_st_void(&gT, &sim), 0u);
    /* classify never reads evictRole */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, kScrVa, kScrPg, 0u, 5u, 0u, 0u);
    q = ask(6u, kScrVa, kScrPg, N48_ST_P_LEDGER, 0u, 0u);
    const uint32_t a0 = n48_st_classify(&gT, &q, nullptr);
    gT.evictRole = 1000u;
    expect_u("S23 n48_st_classify never reads evictRole (never fail closed)", (a0 == n48_st_classify(&gT, &q, nullptr) && a0 == N48_ST_V_STALE) ? 1u : 0u, 1u);
}

/* ---- S24: no reader / a skipped IB makes the set incomplete ---- */
static void s24()
{
    n48_st_fset fs;
    n48_st_fset_begin(&fs, 9u, 1u); n48_st_fset_add(&fs, kLayerVa, n48_st_key(kLayerPg, 0u));
    n48_st_fset_blind(&fs, 1u, 0u); n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S24 reader ok, no IB skipped: complete", fs.ok, 1u);
    n48_st_fset_begin(&fs, 9u, 1u); n48_st_fset_add(&fs, kLayerVa, n48_st_key(kLayerPg, 0u));
    n48_st_fset_blind(&fs, 0u, 0u); n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S24 no reader: incomplete (NOREAD)", (fs.ok == 0u && (fs.why & N48_ST_FS_NOREAD)) ? 1u : 0u, 1u);
    n48_st_fset_begin(&fs, 9u, 1u); n48_st_fset_add(&fs, kLayerVa, n48_st_key(kLayerPg, 0u));
    n48_st_fset_blind(&fs, 1u, 1u); n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S24 an IB skipped: incomplete (IBSKIP)", (fs.ok == 0u && (fs.why & N48_ST_FS_IBSKIP)) ? 1u : 0u, 1u);
    n48_st_fset_begin(&fs, 9u, 1u);
    n48_st_fset_blind(&fs, 0u, 0u); n48_st_fset_close(&fs, 0u, 0u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S24 no reader and no targets: an EMPTY set is incomplete, not vacuously complete", fs.ok, 0u);
    n48_st_fr_ctr c; std::memset(&c, 0, sizeof c);
    n48_st_fr_count(&c, &fs, 0u, 0u);
    n48_st_fset_begin(&fs, 10u, 0u); n48_st_fset_blind(&fs, 1u, 1u); n48_st_fset_close(&fs, 0u, 0u, N48_GCAP_CBT_MAX, 0u);
    n48_st_fr_count(&c, &fs, 0u, 0u);
    expect_u("S24 counted: no reader 1, IB skipped 1, incomplete 2", (c.fsNoRead == 1u && c.fsIbSkip == 1u && c.fsNo == 2u) ? 1u : 0u, 1u);
    n48_st_fset_begin(&fs, 11u, 0u);
    for (uint32_t k = 0; k < 8u; k++) n48_st_fset_add(&fs, 0x400000000ull, (uint64_t)(k + 1u) << 12);
    n48_st_fset_blind(&fs, 1u, 1u); n48_st_fset_close(&fs, 0u, 8u, N48_GCAP_CBT_MAX, 0u);
    expect_u("S24 trunc: a skipped IB counts the targets past the held rows (12 named, 8 held -> 4)", n48_st_trunc(&fs, 12u, 8u, 8u, N48_GCAP_CBT_MAX), 4u);
    /* the exemption: a skipped IB leaves a target-set that must exempt nothing */
    std::memset(&gT, 0, sizeof gT); n48_st_arm_scope(&gT, kArm);
    n48_st_mark(&gT, kScrVa, kScrPg, 0u, 5u, 0u, 0u);
    n48_st_fset_begin(&fs, 6u, 0u); n48_st_fset_add(&fs, kScrVa, n48_st_key(kScrPg, 0u));
    n48_st_fset_blind(&fs, 1u, 1u); n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u);
    n48_st_q q = ask(6u, kScrVa, kScrPg, N48_ST_P_LEDGER, kLayerPg, 0u);
    q.fcb_ok = fs.ok; q.fcb = fs.key; q.fcb_n = fs.n;
    expect_u("S24 a set holding the page but with an IB skipped: no exemption -> STALE", n48_st_classify(&gT, &q, nullptr), N48_ST_V_STALE);
    /* ORDER matters: a blind mark after the close proves nothing (the kext marks before it; S25 pins that) */
    n48_st_fset_begin(&fs, 6u, 0u); n48_st_fset_add(&fs, kScrVa, n48_st_key(kScrPg, 0u));
    n48_st_fset_close(&fs, 0u, 1u, N48_GCAP_CBT_MAX, 0u); n48_st_fset_blind(&fs, 1u, 1u);
    expect_u("S24 (ordering) marked after the close, ok stays 1 - which is why the kext marks before it", fs.ok, 1u);
}

/* ---- S25: the 0.0.546 glue ---- */
static void s25(const std::string &ahh)
{
    if (ahh.empty()) { expect_u("S25 AppleHardwareHook.cpp given", 0u, 1u); return; }
    const std::string dec = body_of(ahh, "static uint32_t gfxsrc_decide_frame(");
    const size_t pre = dec.find("        if (st103On && n > f.nib) gSt103IbSkip = 1u;");
    const size_t lp = dec.find("        for (uint32_t k = 0; k < f.nib && f.reader_ok; k++) {");
    const size_t st = dec.find("            if (st103On && (got != len || f.ib[k].walk != len)) gSt103IbSkip = 1u;");
    const size_t ct = dec.find("            if (got != len || f.ib[k].walk != len) continue;");
    const size_t fill = dec.find("st103_frame_targets(vm, gXdC.judged + 1u, pShape, f.reader_ok, hVa, hPage, hSys, hHeld);");
    expect_u("S25 ORDER: the IB-skip flag: past-the-cap before the loop, the skip right before its continue, both before the set is filled",
             (pre != std::string::npos && lp != std::string::npos && st != std::string::npos && ct != std::string::npos &&
              fill != std::string::npos && pre < lp && lp < st && st < ct && ct - st < 140u && ct < fill &&
              count(dec, "gSt103IbSkip = 1u;") == 2u && count(ahh, "gSt103IbSkip = ") == 3u) ? 1u : 0u, 1u);
    const std::string top = body_of(ahh, "static __attribute__((noinline)) void st103_frame_top(");
    expect_u("S25 the frame top clears the flag before anything asks", (count(top, "gSt103IbSkip = 0u;") == 1u &&
             top.find("gSt103IbSkip = 0u;") < top.find("if (gXdShot.state != N48_CM_SHOT_ARMED")) ? 1u : 0u, 1u);
    const std::string tg = body_of(ahh, "static __attribute__((noinline)) void st103_frame_targets(");
    const size_t bl = tg.find("n48_st_fset_blind(&fs, readerOk, gSt103IbSkip);"), cl = tg.find("n48_st_fset_close(&fs,");
    expect_u("S25 ORDER: the set reads the reader and the flag BEFORE its close", (bl != std::string::npos && cl != std::string::npos &&
             bl < cl && count(tg, "n48_st_fset_blind(") == 1u) ? 1u : 0u, 1u);
    const std::string rep2 = body_of(ahh, "static __attribute__((noinline)) void st103_report2() {");
    const size_t pv = rep2.find("if (!gSt103Tab.arm) HWLOG(N48_ST_PREV_FMT);"), sm = rep2.find("HWLOG(N48_ST_SIM_FMT,");
    expect_u("S25 the verb's report2 lines are LABELLED (previous arm) when no arm is scoped, before the first of them",
             (pv != std::string::npos && sm != std::string::npos && pv < sm) ? 1u : 0u, 1u);
    const size_t vb = ahh.find("} else if (m == N48_ST_M_ON || m == N48_ST_M_OFF || m == N48_ST_M_SHADOW) {");
    const std::string verb = vb == std::string::npos ? std::string() : ahh.substr(vb, ahh.find("} else if (m == N48_ST_M_BOX_A", vb) - vb);
    expect_u("S25 the label is the choice: the verb's mode change does NOT reset the simulated or frame counters (no unlocked writes)",
             (!verb.empty() && count(verb, "gSt103SimC") + count(verb, "gSt103Fc") == 0u) ? 1u : 0u, 1u);
    expect_u("S25 STOP prints the new line and voids on n48_st_void (LOST or evictRole, either table)",
             (count(rep2, "HWLOG(N48_ST_FS2_FMT,") == 1u && count(rep2, "if (n48_st_void(&gSt103Tab, &gSt103Sim))") == 1u &&
              count(rep2, "evictRole") == 4u) ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    const std::string ahh = slurp(argc > 1 ? argv[1] : nullptr);
    const std::string cm = slurp(argc > 2 ? argv[2] : nullptr);
    s1(); s2(); s3(); s4(); s4b(); s567(); s8(); s9(); s10(); s1112(); s13();
    s14(ahh, cm);
    s15(); s16(); s17(); s18(); s19(); s20(); s21(); s22(ahh);
    s23(); s24(); s25(ahh);
    std::printf("%s: %d run, %d failed\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
