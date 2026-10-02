// gfx_ks81_test.cpp — build 0.0.526 (; apple/gfx_ks81.h): the marker window's timing (item 1), switch 81's flip
// mode through a keystone withdrawal (item 2), its M3 longer switch-64 bound (item 3) and its M4 marker-holder yield skip (item 5).
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_ks81_test.cpp -o /tmp/ks81 && /tmp/ks81 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/apple/gfx_commit.h
// Covers:
//   K1 the histogram bucketing at every edge (<=100, <=250, <=500, <=1000, <=2000, <=4000, <=8000, more), the max, the event cap;
//   K2 both new lines fit the 491-byte log body at every counter's widest value (and the 81 line);
//   K3 the M byte, the modes: item 2 at M1/M3/M4 only, item 5 at M4 only, the 5000 us bound at M3 only (2000 / 400 otherwise);
//   K4 item 2's decision over every input: OFF (and every unknown mode) notes EVERY withdrawal; a token mismatch and a guard
//      refusal are always noted; a keystone refusal is kept only when 81 keeps flip AND 73 is ON; a commit is never noted;
//   K5 the holder table: the flag is set by enter and cleared by unhold; only the holder is gated; the skip only at M4 and only for
//      the holder; a full table leaves the thread ungated (it spins as before); nesting; the parts and the spin accounting;
//   K6 the switch-64 wait at both bounds decides nothing: the marker the unchanged checks re-read is still held exactly when the
//      wait timed out, at 2000 us and at 5000 us alike;
//   K7 source pins (AppleHardwareHook.cpp, Navi48Bringup.cpp, gfx_commit.h): the window starts after the fetch_add and ends at the
//      release, on BOTH releases, with the flag cleared right before each; hook_unmapVA has ONE return after the fetch_add; the
//      parts wrap their statements; p73_final precedes the (now switched) note; the wait uses 81's bound; the MM gate sits inside
//      switch 37's yield branch, before the spin; the switch is OFF at boot, written in one place, mid-arm guarded.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <cctype>
#include <initializer_list>
#include "gfx_ks81.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

// ------------------------------------------------------------------------------------------------------------------------ K1
static void k1_hist()
{
    const uint64_t in[]  = { 0, 1, 100, 101, 250, 251, 500, 501, 1000, 1001, 2000, 2001, 4000, 4001, 8000, 8001, 1000000ull,
                             ~0ull };
    const uint32_t out[] = { 0, 0, 0,   1,   1,   2,   2,   3,   3,    4,    4,    5,    5,    6,    6,    7,    7, 7 };
    uint32_t bad = 0;
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) if (n48_kw_bucket(in[i]) != out[i]) bad |= 1u << i;
    expect_u("K1 bucket edges (upper-inclusive) for 18 inputs", bad, 0u);
    n48_kw_stats st {};
    n48_kw_note(&st, N48_KW_H_T, 100); n48_kw_note(&st, N48_KW_H_T, 101); n48_kw_note(&st, N48_KW_H_T, 9000);
    expect_u("K1 note: counts per bucket and the max", st.b[0][0] == 1u && st.b[0][1] == 1u && st.b[0][7] == 1u && st.mx[0] == 9000u, 1u);
    // record: the parts that ran only, the Y histogram only when it spun, the counters, the event cap
    n48_mkh_snap sn {};
    sn.valid = 1u; sn.mask = (1u << N48_KW_P_ORIG) | (1u << N48_KW_P_ENTRY); sn.dur[N48_KW_P_ORIG] = 300; sn.dur[N48_KW_P_ENTRY] = 40;
    sn.dur[N48_KW_P_WD] = 7777;   // did not run: never recorded
    uint32_t ev = 0;
    n48_kw_stats s2 {};
    expect_u("K1 record: a 1500 us window is no event", n48_kw_record(&s2, &sn, 1500, &ev), 0u);
    expect_u("K1 record: T, E and O recorded; W not (did not run); Y not (no spin)",
             s2.b[N48_KW_H_T][4] == 1u && s2.b[N48_KW_H_E][0] == 1u && s2.b[N48_KW_H_O][2] == 1u && s2.mx[N48_KW_H_W] == 0u &&
             s2.mx[N48_KW_H_Y] == 0u && s2.n == 1u, 1u);
    sn.yn = 3; sn.yus = 4100; sn.skips = 2;
    uint32_t evs = 0, last = 0;
    for (uint32_t i = 0; i < 20u; i++) if (n48_kw_record(&s2, &sn, 2001, &ev)) { evs++; last = ev; }
    expect_u("K1 record: > 2000 us is an event, only the first 16 per boot (event numbers 1..16)",
             evs == 16u && last == 16u && s2.ev == 20u, 1u);
    expect_u("K1 record: the spin goes to Y (4100 -> <=8000) and the totals; the skips counted",
             s2.b[N48_KW_H_Y][6] == 20u && s2.yc == 60u && s2.yus == 82000u && s2.skips == 40u, 1u);
    n48_mkh_snap inv {};
    expect_u("K1 record: an untimed window (table full) counts untimed only", n48_kw_record(&s2, &inv, 99999, &ev) == 0u &&
             s2.untimed == 1u && s2.n == 21u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ K2
static void k2_widths()
{
    n48_kw_stats st {};
    for (uint32_t h = 0; h < N48_KW_HISTS; h++) { for (uint32_t b = 0; b < N48_KW_BUCKETS; b++) st.b[h][b] = 0xFFFFFFFFu; st.mx[h] = ~0ull; }
    st.n = st.untimed = st.ev = st.yc = st.yus = st.skips = ~0ull;
    char buf[2048];
    int w = std::snprintf(buf, sizeof buf, N48_KW_REPORT_FMT, N48_KW_REPORT_ARGS(&st, "OFF", n48_ks81_wait_bound_us(N48_KS81_KEEP_WAIT)));
    std::printf("      kswin report line at its widest: %d bytes\n      %s\n", w, buf);
    expect_u("K2 the bare-64 kswin line <= 491 bytes at every counter's widest", w > 0 && w <= 491, 1u);
    w = std::snprintf(buf, sizeof buf, N48_KW_EVENT_FMT, ~0ull, 16u, 4294967295u, 4294967295u, 4294967295u, ~0ull, ~0ull, ~0ull,
                      ~0ull, ~0ull, 0xFFFFFFFFu, 4294967295u, ~0ull, 4294967295u, "M?");
    std::printf("      kswin event line at its widest: %d bytes\n", w);
    expect_u("K2 the event line <= 491 bytes at 20-digit fields", w > 0 && w <= 491, 1u);
    w = std::snprintf(buf, sizeof buf, N48_KS81_REPORT_FMT, "M?", " - `gfxneuter 81` REFUSED: a continuous arm stands, unchanged",
                      ~0ull, ~0ull, ~0ull, 4294967295u, 4294967295u, "SKIPPED (M4)", ~0ull, ~0ull);
    std::printf("      ks81 line at its widest: %d bytes\n", w);
    expect_u("K2 the ks81 line <= 491 bytes at 20-digit fields and the longest `how`", w > 0 && w <= 491, 1u);
    expect_u("K2 display caps: 99999 / 999999 / 9999999", n48_kw_c5(100000u) == 99999u && n48_kw_c5(5u) == 5u && n48_kw_c6(~0ull) == 999999u &&
             n48_kw_c7(~0ull) == 9999999u, 1u);
    uint32_t wmax = 0;
    for (uint32_t m = 0; m < 0x100u; m++) if (n48_ks81_wait_bound_us(m) > wmax) wmax = n48_ks81_wait_bound_us(m);
    uint32_t nmax = 0;
    for (uint32_t m = 0; m < 0x100u; m++) if (std::strlen(n48_ks81_mode_name(m)) > nmax) nmax = (uint32_t)std::strlen(n48_ks81_mode_name(m));
    expect_u("K2 the widest bound printed is 5000 and the widest mode name 3 characters (the widths measured above)", wmax * 0x10u + nmax, 5000u * 0x10u + 3u);
}

// ------------------------------------------------------------------------------------------------------------------------ K3
static void k3_modes()
{
    expect_u("K3 M byte: 0 read, 1 KEEP, 2 OFF, 3 KEEP_WAIT, 4 KEEP_NOYIELD, 5/0x80/0xFF refused",
             n48_ks81_mode_of_m(0) == N48_KS81_M_READ && n48_ks81_mode_of_m(1) == N48_KS81_KEEP && n48_ks81_mode_of_m(2) == N48_KS81_OFF &&
             n48_ks81_mode_of_m(3) == N48_KS81_KEEP_WAIT && n48_ks81_mode_of_m(4) == N48_KS81_KEEP_NOYIELD &&
             n48_ks81_mode_of_m(5) == N48_KS81_M_BAD && n48_ks81_mode_of_m(0x80) == N48_KS81_M_BAD && n48_ks81_mode_of_m(0xFF) == N48_KS81_M_BAD, 1u);
    expect_u("K3 the verb numbers: 337 / 593 / 849 / 1105", (81u | 1u << 8) == 337u && (81u | 2u << 8) == 593u && (81u | 3u << 8) == 849u &&
             (81u | 4u << 8) == 1105u, 1u);
    uint32_t keep = 0, skip = 0, lng = 0;
    for (uint32_t m = 0; m < 0x100u; m++) {
        if (n48_ks81_keeps_flip(m)) keep |= 1u << (m < 31u ? m : 31u);
        if (n48_ks81_skip_yield(m)) skip |= 1u << (m < 31u ? m : 31u);
        if (n48_ks81_wait_bound_us(m) != N48_KS_WAIT_BOUND_US) lng |= 1u << (m < 31u ? m : 31u);
    }
    expect_u("K3 item 2 applies at M1, M3, M4 only (modes 1, 3, 4)", keep, (1u << 1) | (1u << 3) | (1u << 4));
    expect_u("K3 item 5 applies at M4 only", skip, 1u << 4);
    expect_u("K3 the longer bound applies at M3 only", lng, 1u << 3);
    expect_u("K3 bounds: OFF 2000/400 (= 0.0.497's constants), M1 2000, M3 5000/1000, M4 2000",
             n48_ks81_wait_bound_us(N48_KS81_OFF) == 2000u && N48_KS_WAIT_BOUND_US == 2000u &&
             n48_ks81_wait_max_polls(n48_ks81_wait_bound_us(N48_KS81_OFF)) == N48_KS_WAIT_MAX_POLLS &&
             n48_ks81_wait_bound_us(N48_KS81_KEEP) == 2000u && n48_ks81_wait_bound_us(N48_KS81_KEEP_WAIT) == 5000u &&
             n48_ks81_wait_max_polls(5000u) == 1000u && n48_ks81_wait_bound_us(N48_KS81_KEEP_NOYIELD) == 2000u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ K4
static void k4_note()
{
    // OFF identity: mode 0 and every value that is not M1/M3/M4 notes EVERY withdrawal (whatever tok/guard/73), never a commit.
    const uint32_t offModes[] = { N48_KS81_OFF, 2u, 5u, 0x80u, N48_KS81_M_BAD, N48_KS81_M_READ };
    uint32_t bad = 0;
    for (uint32_t mode : offModes)
        for (uint32_t t = 0; t < 2u; t++) for (uint32_t g = 0; g < 2u; g++) for (uint32_t k = 0; k < 2u; k++) for (uint32_t p = 0; p < 2u; p++) {
            const uint32_t withdrawal = !(t && k);
            if (n48_ks81_note_withdrawal(mode, t, g, k, p) != withdrawal) bad++;
        }
    expect_u("K4 OFF identity: every non-keeping mode notes exactly the withdrawals (96 combinations)", bad, 0u);
    const uint32_t onModes[] = { N48_KS81_KEEP, N48_KS81_KEEP_WAIT, N48_KS81_KEEP_NOYIELD };
    uint32_t tokMis = 0, guard = 0, ks73 = 0, ksNo73 = 0, commit = 0;
    for (uint32_t mode : onModes)
        for (uint32_t p = 0; p < 2u; p++) {
            for (uint32_t g = 0; g < 2u; g++) tokMis += n48_ks81_note_withdrawal(mode, 0u, g, 0u, p);   // token mismatch
            guard += n48_ks81_note_withdrawal(mode, 1u, 0u, 0u, p);                                       // guard refused
            if (p) ks73 += n48_ks81_note_withdrawal(mode, 1u, 1u, 0u, 1u);                               // keystone, 73 ON
            else ksNo73 += n48_ks81_note_withdrawal(mode, 1u, 1u, 0u, 0u);                               // keystone, 73 OFF
            commit += n48_ks81_note_withdrawal(mode, 1u, 1u, 1u, p);
        }
    expect_u("K4 ON: a token mismatch is ALWAYS noted (12 of 12)", tokMis, 12u);
    expect_u("K4 ON: a flight-ring guard refusal is ALWAYS noted (6 of 6)", guard, 6u);
    expect_u("K4 ON: a keystone refusal with 73 ON is KEPT (0 of 3 noted)", ks73, 0u);
    expect_u("K4 ON: a keystone refusal with 73 OFF is noted (3 of 3: the present is not held)", ksNo73, 3u);
    expect_u("K4 a commit (tok && ks) is never noted", commit, 0u);
}

// ------------------------------------------------------------------------------------------------------------------------ K5
static void k5_holders()
{
    n48_mkh_table t {};
    const uintptr_t A = 0x1000, B = 0x2000, C = 0x3000;
    expect_u("K5 no holder: the gate is 0 for anyone, skip or not", n48_mkh_gate(&t, A, 1u) | n48_mkh_gate(&t, A, 0u), 0u);
    const uint32_t ta = n48_mkh_enter(&t, A, 1000);
    expect_u("K5 enter claims slot 1 and SETS the flag", ta == 1u && t.s[0].holder == A && t.s[0].busy == A, 1u);
    expect_u("K5 the holder is gated (yield, accounted) with the switch not at M4", n48_mkh_gate(&t, A, 0u), 1u);
    expect_u("K5 ... and skips at M4 (the skip counted in its slot)",
             n48_mkh_gate_skips(n48_mkh_gate(&t, A, 1u)) == 1u && t.s[0].skips == 1u, 1u);
    expect_u("K5 a NON-holder is never gated, M4 or not", n48_mkh_gate(&t, B, 1u) | n48_mkh_gate(&t, B, 0u) | n48_mkh_gate(&t, 0u, 1u), 0u);
    n48_mkh_spin(&t, n48_mkh_gate(&t, A, 0u), 750);
    n48_mkh_spin(&t, n48_mkh_gate(&t, A, 1u), 999);   // a skip token accounts no spin
    n48_mkh_spin(&t, 0u, 999);                         // a non-holder accounts nothing
    expect_u("K5 spin accounting: one spin of 750 us on the holder's slot", t.s[0].yn == 1u && t.s[0].yus == 750u, 1u);
    // parts
    n48_mkh_mark(&t, ta, 1100); n48_mkh_part(&t, ta, N48_KW_P_ENTRY, 1u, 1180);
    n48_mkh_mark(&t, ta, 1200); n48_mkh_part(&t, ta, N48_KW_P_WD, 0u, 1500);    // did not run
    n48_mkh_mark(&t, ta, 1500); n48_mkh_part(&t, ta, N48_KW_P_ORIG, 1u, 1900);
    // nesting on the same thread: the flag stays with the outer window
    const uint32_t tn = n48_mkh_enter(&t, A, 1950);
    expect_u("K5 a nested enter on the same thread gets a NESTED token on the same slot", tn == (1u | N48_MKH_TOK_NESTED) && t.s[0].depth == 1u, 1u);
    n48_mkh_unhold(&t, tn);
    expect_u("K5 the nested unhold keeps the flag", t.s[0].holder == A && t.s[0].depth == 0u, 1u);
    n48_mkh_snap nsn {}; n48_mkh_end(&t, tn, &nsn);
    expect_u("K5 the nested end reads nothing and frees nothing", nsn.valid == 0u && t.s[0].busy == A, 1u);
    n48_mkh_unhold(&t, ta);
    expect_u("K5 unhold CLEARS the flag (the slot stays claimed until the end)", t.s[0].holder == 0u && t.s[0].busy == A, 1u);
    expect_u("K5 after the unhold the thread is no longer gated (even at M4)", n48_mkh_gate(&t, A, 1u), 0u);
    n48_mkh_snap sn {}; n48_mkh_end(&t, ta, &sn);
    expect_u("K5 end: the window's parts (E 80, O 400; W not run), t0, the spin, the skip; the slot freed",
             sn.valid == 1u && sn.mask == ((1u << N48_KW_P_ENTRY) | (1u << N48_KW_P_ORIG)) && sn.dur[N48_KW_P_ENTRY] == 80u &&
             sn.dur[N48_KW_P_ORIG] == 400u && sn.t0 == 1000u && sn.yn == 1u && sn.yus == 750u && sn.skips == 2u &&
             t.s[0].busy == 0u && t.s[0].holder == 0u, 1u);
    // two holders at once (the marker is a COUNTER: overlapping unmaps), each gated on its own slot only
    const uint32_t t1 = n48_mkh_enter(&t, A, 10), t2 = n48_mkh_enter(&t, B, 20);
    expect_u("K5 two concurrent holders: two slots, each gated to its own", t1 != t2 && n48_mkh_gate(&t, A, 0u) == t1 &&
             n48_mkh_gate(&t, B, 0u) == t2 && n48_mkh_gate(&t, C, 1u) == 0u, 1u);
    n48_mkh_unhold(&t, t1);
    expect_u("K5 one releases: the other is still a holder", n48_mkh_gate(&t, A, 1u) == 0u && n48_mkh_gate_skips(n48_mkh_gate(&t, B, 1u)) == 1u, 1u);
    n48_mkh_end(&t, t1, &sn); n48_mkh_unhold(&t, t2); n48_mkh_end(&t, t2, &sn);
    // a full table: the ninth thread is not a holder (it spins as before) and its window is untimed
    uint32_t toks[N48_MKH_SLOTS];
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++) toks[i] = n48_mkh_enter(&t, 0x10000u + i * 0x100u, i);
    const uint32_t over = n48_mkh_enter(&t, 0x90000u, 99);
    expect_u("K5 a full table: token 0, counted, the thread NOT gated (fail toward the old behaviour)",
             over == 0u && t.overflow == 1u && n48_mkh_gate(&t, 0x90000u, 1u) == 0u, 1u);
    n48_mkh_mark(&t, 0u, 5); n48_mkh_part(&t, 0u, N48_KW_P_DESC, 1u, 9); n48_mkh_unhold(&t, 0u); n48_mkh_end(&t, 0u, &sn);
    expect_u("K5 token 0: marks, parts, unhold and end touch nothing", sn.valid == 0u && t.s[0].busy == 0x10000u, 1u);
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++) { n48_mkh_unhold(&t, toks[i]); n48_mkh_end(&t, toks[i], &sn); }
    uint32_t anyBusy = 0; for (uint32_t i = 0; i < N48_MKH_SLOTS; i++) anyBusy |= t.s[i].busy != 0u || t.s[i].holder != 0u;
    expect_u("K5 every slot free again", anyBusy, 0u);
}

// ------------------------------------------------------------------------------------------------------------------------ K6
struct WaitModel { uint64_t now; uint64_t releaseAt; uint32_t reads; };
static uint32_t wm_marker(void *c) { WaitModel *m = static_cast<WaitModel *>(c); m->reads++; return m->now < m->releaseAt ? 1u : 0u; }
static uint64_t wm_now(void *c) { return static_cast<WaitModel *>(c)->now; }
static void wm_pause(void *c) { static_cast<WaitModel *>(c)->now += N48_KS_WAIT_POLL_US; }
static void k6_wait()
{
    const uint64_t holds[] = { 1, 1000, 1995, 2000, 2500, 4000, 4995, 5000, 6000, 1ull << 40 };
    const uint32_t modes[] = { N48_KS81_OFF, N48_KS81_KEEP, N48_KS81_KEEP_WAIT, N48_KS81_KEEP_NOYIELD };
    uint32_t mism = 0, released2 = 0, released5 = 0, bound2 = 0, bound5 = 0;
    for (uint32_t mode : modes)
        for (uint64_t h : holds) {
            WaitModel m { 1000000ull, 1000000ull + h, 0u };
            const n48_ks_wait_io io { &wm_marker, &wm_now, &wm_pause, &m };
            const uint32_t b = n48_ks81_wait_bound_us(mode);
            uint32_t polls = 0; uint64_t us = 0;
            const uint32_t o = n48_ks_wait_run_b(1u, 1u, &io, b, n48_ks81_wait_max_polls(b), &polls, &us);
            // THE UNCHANGED CHECKS re-read the marker now (clause 9b's fresh read): held <=> the wait TIMED OUT.
            const uint32_t heldAfter = n48_ks_withdrawing(wm_marker(&m));
            if (heldAfter != (o == N48_KSW_TIMEOUT ? 1u : 0u)) mism++;
            if (o == N48_KSW_TIMEOUT && us > b) mism++;
            if (b == 2000u) { if (o == N48_KSW_RELEASED) released2++; if (o == N48_KSW_TIMEOUT && us == 2000u) bound2++; }
            else { if (o == N48_KSW_RELEASED) released5++; if (o == N48_KSW_TIMEOUT && us == 5000u) bound5++; }
        }
    expect_u("K6 at every bound the checks see the marker held exactly when the wait timed out (fail-closed; 40 cases)", mism, 0u);
    expect_u("K6 2000 us (OFF, M1, M4): a hold of <= 2000 us is RELEASED (4 holds x 3 modes)", released2, 12u);
    expect_u("K6 5000 us (M3): a hold of <= 5000 us is RELEASED (8 holds)", released5, 8u);
    expect_u("K6 the timeouts end exactly at the bound in use (2000: 6 holds x 3 modes; 5000: 2 holds)", bound2 * 0x100u + bound5, 18u * 0x100u + 2u);
    // the fixed-bound wait (0.0.497) is the _b form at 2000/400
    WaitModel a { 5, 5 + 3000, 0 }, c { 5, 5 + 3000, 0 };
    const n48_ks_wait_io ia { &wm_marker, &wm_now, &wm_pause, &a }, ic { &wm_marker, &wm_now, &wm_pause, &c };
    uint32_t pa = 0, pc = 0; uint64_t ua = 0, uc = 0;
    const uint32_t oa = n48_ks_wait_run(1u, 1u, &ia, &pa, &ua);
    const uint32_t oc = n48_ks_wait_run_b(1u, 1u, &ic, N48_KS_WAIT_BOUND_US, N48_KS_WAIT_MAX_POLLS, &pc, &uc);
    expect_u("K6 n48_ks_wait_run == n48_ks_wait_run_b(2000, 400)", oa == oc && pa == pc && ua == uc && oa == N48_KSW_TIMEOUT, 1u);
    // an unusable clock: the poll cap alone bounds it (400 / 1000)
    struct Z { static uint64_t now(void *) { return 0ull; } };
    WaitModel z { 0, ~0ull, 0 };
    const n48_ks_wait_io iz { &wm_marker, &Z::now, &wm_pause, &z };
    uint32_t pz = 0; uint64_t uz = 0;
    const uint32_t oz = n48_ks_wait_run_b(1u, 1u, &iz, 5000u, n48_ks81_wait_max_polls(5000u), &pz, &uz);
    expect_u("K6 clock unusable at M3: TIMEOUT after exactly 1000 polls", oz == N48_KSW_TIMEOUT && pz == 1000u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ K7
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
static std::string fn_body(const std::string &s, const char *sig)
{
    const size_t a = s.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t e = s.find("\n}\n", a);
    return s.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
// Code only: comments and string/char literals blanked (so a `return` in a comment or a log string is not a return).
static std::string code_only(const std::string &s)
{
    std::string o = s;
    for (size_t i = 0; i < o.size();) {
        if (o[i] == '/' && i + 1 < o.size() && o[i + 1] == '/') { while (i < o.size() && o[i] != '\n') o[i++] = ' '; }
        else if (o[i] == '/' && i + 1 < o.size() && o[i + 1] == '*') {
            while (i + 1 < o.size() && !(o[i] == '*' && o[i + 1] == '/')) { if (o[i] != '\n') o[i] = ' '; i++; }
            if (i + 1 < o.size()) { o[i] = ' '; o[i + 1] = ' '; i += 2; }
        } else if (o[i] == '"' || o[i] == '\'') {
            const char q = o[i++];
            while (i < o.size() && o[i] != q) { if (o[i] == '\\') o[i++] = ' '; if (i < o.size()) o[i++] = ' '; }
            i++;
        } else i++;
    }
    return o;
}
static uint32_t count_word(const std::string &s, const char *w)
{
    uint32_t c = 0; const size_t n = std::strlen(w);
    for (size_t a = s.find(w); a != std::string::npos; a = s.find(w, a + 1)) {
        const bool l = a == 0 || !(std::isalnum((unsigned char)s[a - 1]) || s[a - 1] == '_');
        const bool r = a + n >= s.size() || !(std::isalnum((unsigned char)s[a + n]) || s[a + n] == '_');
        if (l && r) c++;
    }
    return c;
}
static bool ordered(const std::string &s, std::initializer_list<const char *> xs)
{
    size_t last = 0; bool first = true;
    for (const char *x : xs) {
        const size_t p = first ? s.find(x) : s.find(x, last + 1);
        if (p == std::string::npos) return false;
        last = p; first = false;
    }
    return true;
}
static void k7_pins(const char *ahhP, const char *bupP, const char *cmP)
{
    const std::string s = slurp(ahhP), b = slurp(bupP), c = slurp(cmP);
    expect_u("K7 the three sources read", !s.empty() && !b.empty() && !c.empty(), 1u);
    const std::string u = fn_body(s, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    const char *fetch = "    __atomic_fetch_add(&e->ksWithdrawing, 1u, __ATOMIC_SEQ_CST);\n";
    const char *enter = "    uint32_t kwTok = kswin_enter();\n";
    const char *deferRel = "    if (!held) kswin_unhold(kwTok);   // build 0.0.526: the flag cleared RIGHT BEFORE the release\n"
                           "    if (!held) ks_withdraw_leave_atomic(&e->ksWithdrawing);\n"
                           "    if (!held) kwTok = kswin_end(kwTok, e->seq, e->unmapFires + 1u, kdv);";
    const char *exitRel = "    if (held) kswin_unhold(kwTok);   // build 0.0.526: the flag cleared RIGHT BEFORE the release\n"
                          "    if (held) ks_withdraw_leave_atomic(&e->ksWithdrawing);\n"
                          "    if (held) (void)kswin_end(kwTok, e->seq, e->unmapFires, kdv);   // ... and the window ends RIGHT AFTER it\n"
                          "\n    return rc;";
    expect_u("K7 hook_unmapVA: exactly one fetch_add and one kswin_enter", !u.empty() && count(u, fetch) == 1u && count(u, enter) == 1u &&
             count(u, "kswin_enter(") == 1u, 1u);
    expect_u("K7 ORDER: the scope return < the fetch_add < the flag set / clock start < the deferral verdict < the entry read",
             ordered(u, { "if (!e || e->state != 1)", fetch, enter, "kdv = n48_fr_defer_verdict(&gKsRing", "ks_eop_at_expiry(self",
                          "vmctx_read_root(*e, rootAtEntry, &why)" }), 1u);
    expect_u("K7 the DEFER release: flag cleared RIGHT BEFORE it, the window ended RIGHT AFTER it (contiguous, once)", count(u, deferRel), 1u);
    expect_u("K7 the EXIT release: flag cleared RIGHT BEFORE it, the window ended RIGHT AFTER it, then the one return", count(u, exitRel), 1u);
    expect_u("K7 exactly two unholds, two ends, two marker releases in hook_unmapVA",
             count(u, "kswin_unhold(kwTok)") * 0x100u + count(u, "kswin_end(kwTok") * 0x10u + count(u, "ks_withdraw_leave_atomic(&e->ksWithdrawing);"),
             0x222u);
    const std::string uc = code_only(u);
    const size_t fAt = uc.find("__atomic_fetch_add(&e->ksWithdrawing");
    expect_u("K7 EVERY exit after the fetch_add is the one `return rc;` (no other return below the marker set)",
             fAt != std::string::npos && count_word(uc.substr(fAt), "return") == 1u && count(uc.substr(fAt), "return rc;") == 1u, 1u);
    expect_u("K7 the DEFER release precedes the entry read; the EXIT release is after the re-arm and the freed branches",
             ordered(u, { deferRel, "vmctx_read_root(*e, rootAtEntry, &why)", "unmapva: RE-ARMED for ctx",
                          "kstone-defer: *** DEFERRED WITHDRAWAL LOST TO A FREE ***", "unmapva: *** ROOT FREED BY THIS CALL ***", exitRel }), 1u);
    expect_u("K7 the parts wrap their statements: D, E, W, O, R in order",
             ordered(u, { "kswin_mark(kwTok);   // build 0.0.526 item 1 (D)", "if (gXdDescPort) gfxsrc_desc_unmap(e->seq, va, size);",
                          "kswin_part(kwTok, N48_KW_P_DESC, gXdDescPort ? 1u : 0u);",
                          "kswin_mark(kwTok);   // build 0.0.526 item 1 (E)", "vmctx_read_root(*e, rootAtEntry, &why)",
                          "navi48_vram_read_mm(rootOff + (uint64_t)kRingRootSlot * 8u, d, 2)", "kswin_part(kwTok, N48_KW_P_ENTRY, 1u);",
                          "kswin_mark(kwTok);   // build 0.0.526 item 1 (W)", "if (e->wrote && !e->withdrawn && !deferNow) {",
                          "kswin_part(kwTok, N48_KW_P_WD, fwOpened);", "kswin_mark(kwTok);   // build 0.0.526 item 1 (O)",
                          "const uint64_t rc = reinterpret_cast<Fn>(gOrigUnmapVA)(self, va, size);",
                          "kswin_part(kwTok, N48_KW_P_ORIG, 1u);",
                          "const uint32_t kwRe = (e->wrote && e->withdrawn) ? 1u : 0u;", "kswin_mark(kwTok);\n    if (e->wrote && e->withdrawn) {",
                          "kswin_part(kwTok, N48_KW_P_REARM, kwRe);", exitRel }), 1u);
    const std::string ke = fn_body(s, "static __attribute__((noinline)) uint32_t kswin_end(uint32_t tok, uint32_t ctxSeq, uint32_t fire, uint32_t kdv)");
    expect_u("K7 kswin_end reads the clock FIRST (the window ends at the release its caller just made), then frees the slot",
             ordered(ke, { "const uint64_t t1 = drain_now_us();", "n48_mkh_end(&gMkh, tok, &sn);", "n48_kw_record(&gKw, &sn, total, &evNo)",
                           "return 0u;" }) && count(ke, "drain_now_us()") == 1u, 1u);
    expect_u("K7 kswin_enter uses the same clock and the calling thread",
             count(s, "return n48_mkh_enter(&gMkh, reinterpret_cast<uintptr_t>(current_thread()), drain_now_us());"), 1u);
    // item 2
    const std::string cm = fn_body(s, "static uint64_t hook_gfxCommitIB(void *self, void *info) {");
    const char *p73 = "        if (gP73On && !(tokMatch && ksOk)) p73_final(here.seq, 0u);";
    const char *note = "        if (!(tokMatch && ksOk)) ks81_fm_withdrawal(tokMatch ? 1u : 0u, frHasPendingEntry ? 1u : 0u);";
    expect_u("K7 ORDER: the keystone answers < 73's final(seq, 0) < the (switched) flip-mode note, each once",
             ordered(cm, { "const bool ksOk = frHasPendingEntry && commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);", p73, note }) &&
             count(cm, p73) == 1u && count(cm, note) == 1u, 1u);
    expect_u("K7 the unconditional note is gone; navi48_fm_note_withdrawal() is called only inside ks81_fm_withdrawal",
             count(s, "if (!(tokMatch && ksOk)) navi48_fm_note_withdrawal();") == 0u && count(s, "navi48_fm_note_withdrawal();") == 1u, 1u);
    const std::string fw = fn_body(s, "static __attribute__((noinline)) void ks81_fm_withdrawal(uint32_t tokMatch, uint32_t guardOk)");
    expect_u("K7 ks81_fm_withdrawal: the pure decision (81, tok, guard, 73) decides the note",
             ordered(fw, { "if (n48_ks81_note_withdrawal(__atomic_load_n(&gKs81Mode, __ATOMIC_RELAXED), tokMatch, guardOk, 0u, gP73On ? 1u : 0u)) {",
                           "navi48_fm_note_withdrawal();", "return;", "gKs81.kept" }), 1u);
    // item 3
    const std::string wb = fn_body(s, "__attribute__((noinline)) static void ks_wait_marker_bounded(const WsFrame &wf, uint32_t on, uint32_t gateSeq)");
    expect_u("K7 the wait (still void, still first `if (!on) return;`) runs the pure loop at 81's bound, once",
             ordered(wb, { "    if (!on) return;\n", "const uint32_t bUs = n48_ks81_wait_bound_us(__atomic_load_n(&gKs81Mode, __ATOMIC_RELAXED));",
                           "const uint32_t o = n48_ks_wait_run_b(1u, rw ? 1u : 0u, &io, bUs, n48_ks81_wait_max_polls(bUs), &polls, &us);" }) &&
             count(s, "n48_ks_wait_run(") == 0u && count(s, "n48_ks_wait_run_b(") == 1u, 1u);
    // the switch
    expect_u("K7 81 is OFF at boot and written in ONE place (the verb, after the mid-arm guard)",
             count(s, "static volatile uint32_t gKs81Mode { N48_KS81_OFF };") == 1u && count(s, "__atomic_store_n(&gKs81Mode") == 1u &&
             count(s, "gKs81Mode =") == 0u &&
             ordered(s, { "} else if ((arg & 0xffull) == 81ull) {",
                          "const bool contRefused81 = n48_cm_cont_switch_refused(81u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
                          "if (contRefused81) st = 5;", "__atomic_store_n(&gKs81Mode, want81, __ATOMIC_RELEASE);" }), 1u);
    expect_u("K7 gfx_commit.h guards 81 mid-arm", count(c, "    case 81u:   /* build 0.0.526"), 1u);
    expect_u("K7 the bare-64 verb prints the kswin line after kswait64's",
             ordered(s, { "} else if ((arg & 0xffull) == 64ull) {", "kswait_report_line(contRefused64", "kswin_report_line();",
                          "} else if ((arg & 0xffull) == 65ull) {" }), 1u);
    // item 5: the gate inside switch 37's yield, in both MM paths, before the spin; the fc verify reader untouched
    const char *mm[] = { "bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {",
                         "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {",
                         "static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {" };
    uint32_t okmm = 0;
    for (const char *sig : mm) {
        const std::string f = fn_body(b, sig);
        okmm += ordered(f, { "if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {", "const uint32_t mkh = n48::hw_mkh_gate(caller);",
                             "if (!n48_mkh_gate_skips(mkh)) {", "IODelay(50);",
                             "n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));",
                             "if (mkh) n48::hw_mkh_spin(mkh, elapsedUs);", "IOLockLock(gVramMmLock);" }) &&
                count(f, "n48::hw_mkh_gate(") == 1u ? 1u : 0u;
    }
    expect_u("K7 read_mm, write_mm and the timed twin: should_yield -> the holder gate -> (not skipped) the spin -> its accounting -> IOLockLock",
             okmm, 3u);
    expect_u("K7 the gate is asked only there and in their timed twin (fc_vram_read_timed, whose lock block must equal read_mm's)",
             count(b, "n48::hw_mkh_gate(") == 3u && count(b, "n48::hw_mkh_spin(") == 3u &&
             count(fn_body(b, "static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {"),
                   "const uint32_t mkh = n48::hw_mkh_gate(caller);") == 1u, 1u);
    expect_u("K7 hw_mkh_gate skips only at M4 (the pure n48_ks81_skip_yield of the live mode)",
             count(s, "return n48_mkh_gate(&gMkh, caller, n48_ks81_skip_yield(__atomic_load_n(&gKs81Mode, __ATOMIC_RELAXED)));"), 1u);
}

int main(int argc, char **argv)
{
    k1_hist();
    k2_widths();
    k3_modes();
    k4_note();
    k5_holders();
    k6_wait();
    if (argc >= 4) k7_pins(argv[1], argv[2], argv[3]);
    else { std::printf("FAIL  K7 needs AppleHardwareHook.cpp Navi48Bringup.cpp gfx_commit.h\n"); gFail++; gRun++; }
    std::printf("gfx_ks81_test: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
