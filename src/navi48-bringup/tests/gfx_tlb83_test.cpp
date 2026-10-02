// gfx_tlb83_test.cpp — build 0.0.528 (; apple/gfx_tlb83.h): switch 83, the TLB-only spin poll for
// gmc_flush_gpu_tlb's engine-17 ack, its leaf lock, its counters, the `acked` line cap, and the marker holder's gVramMmLock wait.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_tlb83_test.cpp -o /tmp/tlb83 && /tmp/tlb83 \
//         src/navi48-bringup/src/amd/gmc_v12_0.cpp src/navi48-bringup/src/amd/amdgpu_regs.h \
//         src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/apple/AppleHardwareHook.cpp \
//         src/navi48-bringup/src/apple/gfx_commit.h src/navi48-bringup/src/amd
// Covers:
//   U1 the selector: M1 ON (339), M2 OFF (595, 0 = OFF), bare 83 reads, every other M refused;
//   U2 the poll against a fake ack register and a fake clock: ON acks inside the spin for small N (no sleep); a late ack falls back
//      to the sleep loop after 2000 us of spinning; a clock that never advances still falls back (the 2000-delay cap); a clock that
//      goes back ends the spin; the ack mask and expected value are poll_reg's (other bits ignored, a wrong value never acks);
//      a timeout returns 0 with the elapsed count at the 100 ms timeout, exactly as the poll_reg model; a differential run over
//      2000 random register sequences: wherever the poll_reg model acks, ON acks on the SAME read with the SAME value;
//   U3 the counters: the reads-to-ack buckets at every edge; ON/OFF notes; the `acked` line cap (32, then counted);
//      the holder query (no side effect) and the lock-wait histogram;
//   U4 every new line <= 491 bytes at every field's widest (both bare-83 lines, the kswin2 line);
//   U5 source pins: poll_reg is byte-identical to 0.0.527's; only gmc_flush_gpu_tlb uses the new helper and only the helper
//      calls n48_tlb83_poll; OFF calls poll_reg exactly as before; the switch is read once, OFF at boot, written in one place,
//      mid-arm guarded; the leaf lock wraps the request write and the ack wait and nothing else is acquired or logged inside it;
//      the timeout line is uncapped and the acked line capped; item 4's timing brackets IOLockLock in both MM paths.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_tlb83.h"
#include "gfx_ks81.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}

// ------------------------------------------------------------------------------------------------------------------------ U1
static void u1_selector()
{
    expect_u("U1 M byte: 0 read, 1 ON, 2 OFF", n48_tlb83_mode_of_m(0) == N48_TLB83_M_READ && n48_tlb83_mode_of_m(1) == N48_TLB83_ON &&
             n48_tlb83_mode_of_m(2) == N48_TLB83_OFF, 1u);
    uint32_t bad = 0;
    for (uint32_t m = 3; m < 256; m++) if (n48_tlb83_mode_of_m(m) != N48_TLB83_M_BAD) bad++;
    expect_u("U1 every other M (3..255) is refused", bad, 0u);
    expect_u("U1 the verb numbers: 339 ON / 595 OFF", (83u | 1u << 8) == 339u && (83u | 2u << 8) == 595u, 1u);
    expect_u("U1 OFF is 0 (a zero-initialised mode is OFF)", N48_TLB83_OFF, 0u);
    expect_u("U1 the constants: spin 2000 us / 2000 delays, timeout 100000 us (gmc_flush_gpu_tlb's), step 1000, 32 lines",
             N48_TLB83_SPIN_US == 2000u && N48_TLB83_SPIN_MAX_ITERS == 2000u && N48_TLB83_TIMEOUT_US == 100000u &&
             N48_TLB83_SLEEP_STEP_US == 1000u && N48_TLB83_ACK_LINES == 32u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ U2
// The fake: the register's k-th read (1-based) answers seq(k); the clock advances rdUs per read, dUs per delay, sUs per sleep.
// A runaway guard: past 5,000,000 reads the fake answers the ack and flags it (so a planted unbounded spin FAILS, never hangs).
struct Fake {
    uint32_t mask, expected;
    uint32_t ackAt;          // the read (1-based) from which the register acks; 0 = never
    uint32_t noise;          // bits set in every value (outside the mask)
    const uint32_t *seq;     // or an explicit sequence (len); past its end: the last value
    uint32_t len;
    uint64_t t, rdUs, dUs, sUs;
    int clockStuck, clockBack;
    uint32_t reads, delays, sleeps, runaway;
};
static uint32_t f_rd(void *c)
{
    Fake *f = static_cast<Fake *>(c);
    f->reads++;
    f->t += f->rdUs;
    if (f->reads > 5000000u) { f->runaway = 1u; return f->expected; }
    if (f->seq) return f->seq[f->reads - 1u < f->len ? f->reads - 1u : f->len - 1u];
    if (f->ackAt && f->reads >= f->ackAt) return f->expected | f->noise;
    return f->noise;
}
static uint64_t f_now(void *c)
{
    Fake *f = static_cast<Fake *>(c);
    if (f->clockStuck) return 1000ull;
    if (f->clockBack) return f->reads > 3u ? 5ull : 1000ull + f->t;
    return 1000ull + f->t;
}
static void f_delay(void *c) { Fake *f = static_cast<Fake *>(c); f->delays++; f->t += f->dUs; }
static void f_sleep(void *c) { Fake *f = static_cast<Fake *>(c); f->sleeps++; f->t += f->sUs; }
static Fake fake(uint32_t vmid, uint32_t ackAt)
{
    Fake f {};
    f.mask = 1u << vmid; f.expected = 1u << vmid; f.ackAt = ackAt; f.rdUs = 1; f.dUs = 1; f.sUs = 1100;
    return f;
}
static uint32_t run_on(Fake &f, n48_tlb83_res &r, uint64_t timeout = N48_TLB83_TIMEOUT_US)
{
    const n48_tlb83_io io { &f, f_rd, f_now, f_delay, f_sleep };
    return n48_tlb83_poll(&io, f.mask, f.expected, timeout, &r);
}
// THE MODEL OF poll_reg (amdgpu_regs.h; its text is pinned byte-identical in U5): read; acked -> true; elapsed >= timeout -> false;
// IOSleep(1); elapsed += 1000.
static bool ref_poll_reg(Fake &f, uint64_t timeout_us, uint32_t *outValue, uint64_t *elapsedOut)
{
    uint64_t elapsed_us = 0; uint32_t v = 0;
    while (true) {
        v = f_rd(&f);
        if ((v & f.mask) == f.expected) { *outValue = v; *elapsedOut = elapsed_us; return true; }
        if (elapsed_us >= timeout_us) { *outValue = v; *elapsedOut = elapsed_us; return false; }
        f_sleep(&f); elapsed_us += 1000;
    }
}
static void u2_poll()
{
    // ON acks inside the spin for small N
    uint32_t badSmall = 0;
    for (uint32_t n : { 1u, 2u, 3u, 5u, 10u, 50u, 200u, 900u }) {
        Fake f = fake(2, n); n48_tlb83_res r {};
        const uint32_t ok = run_on(f, r);
        if (!(ok == 1u && r.ok == 1u && r.reads == n && r.sleeps == 0u && f.sleeps == 0u && r.fell == 0u && r.delays == n - 1u &&
              r.value == (1u << 2) && !f.runaway)) badSmall |= 1u;
    }
    expect_u("U2 ON acks within the spin for N = 1..900 reads: N reads, N-1 delays, no sleep, no fall-back", badSmall, 0u);
    { Fake f = fake(2, 1); n48_tlb83_res r {}; run_on(f, r);
      expect_u("U2 ON: the first read acks -> spin_us 0, elapsed 0, one read, no delay", r.spin_us == 0u && r.elapsed == 0u && r.reads == 1u &&
               r.delays == 0u, 1u); }
    // a late ack: the spin ends at 2000 us and the sleep loop takes over
    { Fake f = fake(2, 1010); n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      expect_u("U2 ON: an ack at read 1010 -> falls back after >= 2000 us of spinning, then acked in the sleep loop",
               ok == 1u && r.fell == 1u && r.spin_us >= 2000u && r.spin_us < 2010u && r.sleeps >= 1u && f.sleeps == r.sleeps &&
               !f.runaway, 1u);
      expect_u("U2 ON: the spin made ~1000 delays at 2 us per round (the clock bound fired, not the count bound)",
               r.delays >= 990u && r.delays <= 1000u, 1u);
      expect_u("U2 ON: the sleep loop's accounting starts at the spin's elapsed time (+1000 per sleep)",
               r.elapsed == r.spin_us + 1000ull * r.sleeps, 1u); }
    // never acks: a timeout, as poll_reg's
    { Fake f = fake(2, 0); n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      Fake g = fake(2, 0); uint32_t gv = 0; uint64_t ge = 0;
      const bool rok = ref_poll_reg(g, N48_TLB83_TIMEOUT_US, &gv, &ge);
      expect_u("U2 never acks: ON returns 0 (false), exactly as the poll_reg model", ok == 0u && r.ok == 0u && rok == false, 1u);
      expect_u("U2 never acks: the poll_reg model ends at elapsed 100000 after 101 reads and 100 sleeps",
               ge == 100000u && g.reads == 101u && g.sleeps == 100u, 1u);
      expect_u("U2 never acks: ON ends with its accounting in [100000, 101000) - the SAME 100 ms timeout",
               r.elapsed >= 100000u && r.elapsed < 101000u && r.fell == 1u && !f.runaway, 1u);
      expect_u("U2 never acks: ON slept 98 times (2000 us spun + 98 x 1000), the model 100", r.sleeps == 98u && g.sleeps == 100u, 1u);
      expect_u("U2 never acks: the value returned is the last value read (poll_reg's *outValue)", r.value == 0u && gv == 0u, 1u); }
    // a clock that never advances: the delay cap ends the spin; the timeout is then the sleep loop's (100 sleeps)
    { Fake f = fake(2, 0); f.clockStuck = 1; n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      expect_u("U2 a stuck clock: the spin still ends after 2000 delays and the poll times out at 100 ms of sleeps",
               ok == 0u && r.delays == 2000u && r.fell == 1u && r.spin_us == 0u && r.sleeps == 100u && r.elapsed == 100000u &&
               !f.runaway, 1u); }
    { Fake f = fake(2, 0); f.clockBack = 1; n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      expect_u("U2 a clock that goes back ends the spin at once (counted as 2000 us) and still times out",
               ok == 0u && r.fell == 1u && r.delays < 5u && r.elapsed >= 100000u && r.elapsed < 101000u, 1u); }
    // a timeout shorter than the spin bound
    { Fake f = fake(2, 0); n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r, 500u);
      expect_u("U2 a 500 us timeout: the spin stops at the timeout and returns 0 with no sleep", ok == 0u && r.sleeps == 0u &&
               r.elapsed >= 500u && r.elapsed < 510u, 1u); }
    // the mask and the expected value are poll_reg's
    { Fake f = fake(5, 3); f.noise = 0xFFFF0000u | (1u << 4) | (1u << 6); n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      expect_u("U2 mask: bits outside 1 << vmid are ignored (acked at read 3, value carries the noise)",
               ok == 1u && r.reads == 3u && r.value == (0xFFFF0000u | (1u << 4) | (1u << 6) | (1u << 5)), 1u); }
    { Fake f = fake(5, 0); f.noise = ~(1u << 5); n48_tlb83_res r {};
      const uint32_t ok = run_on(f, r);
      expect_u("U2 mask: every OTHER bit set, the vmid bit clear -> never acked (timeout)", ok == 0u && r.value == ~(1u << 5), 1u); }
    // differential: random sequences; wherever the poll_reg model acks, ON acks on the same read with the same value
    {
        uint32_t lcg = 0x528u, mism = 0, both = 0, refTo = 0;
        static uint32_t seq[400];
        for (uint32_t t = 0; t < 2000u; t++) {
            const uint32_t vmid = t % 16u;
            for (uint32_t i = 0; i < 400u; i++) {
                lcg = lcg * 1664525u + 1013904223u;
                uint32_t v = lcg;
                if ((lcg >> 8) % 23u != 0u) v &= ~(1u << vmid);   // mostly not acked
                seq[i] = v;
            }
            seq[399] &= ~(1u << vmid);
            Fake a = fake(vmid, 0); a.seq = seq; a.len = 400u;
            Fake b = fake(vmid, 0); b.seq = seq; b.len = 400u;
            n48_tlb83_res r {}; uint32_t gv = 0; uint64_t ge = 0;
            const uint32_t ok = run_on(a, r);
            const bool rok = ref_poll_reg(b, N48_TLB83_TIMEOUT_US, &gv, &ge);
            if (rok) { both++; if (!(ok == 1u && r.value == gv && r.reads == b.reads)) mism++; }
            else { refTo++; if (ok && !((r.value & (1u << vmid)) == (1u << vmid))) mism++; }
        }
        std::printf("      differential: %u acked by the model, %u model timeouts\n", both, refTo);
        expect_u("U2 differential (2000 sequences): ON acks on the model's read with the model's value; never a false ack",
                 mism == 0u && both > 1500u, 1u);
    }
}

// ------------------------------------------------------------------------------------------------------------------------ U3
static void u3_counters()
{
    const uint32_t in[]  = { 0, 1, 2, 3, 4, 10, 11, 100, 101, 1000, 1001, 100000 };
    const uint32_t out[] = { 0, 0, 1, 1, 2, 2,  3,  3,   4,   4,    5,    5 };
    uint32_t bad = 0;
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) if (n48_tlb83_rbucket(in[i]) != out[i]) bad |= 1u << i;
    expect_u("U3 reads-to-ack buckets: 1, 2-3, 4-10, 11-100, 101-1000, > 1000 at every edge", bad, 0u);
    n48_tlb83_stats s {};
    n48_tlb83_res r {}; r.ok = 1u; r.reads = 7u; r.spin_us = 12u;
    n48_tlb83_note_on(&s, &r, 15u, 0u);
    r = n48_tlb83_res {}; r.ok = 1u; r.reads = 2100u; r.fell = 1u; r.spin_us = 2003u;
    n48_tlb83_note_on(&s, &r, 4100u, 1u);
    r = n48_tlb83_res {}; r.ok = 0u; r.reads = 2099u; r.fell = 1u; r.spin_us = 2001u;
    n48_tlb83_note_on(&s, &r, 100500u, 0u);
    n48_tlb83_note_off(&s, 1u, 1100u); n48_tlb83_note_off(&s, 0u, 101000u);
    expect_u("U3 ON notes: calls 3, acked 2, timeouts 1; buckets 4-10 and > 1000 (acked only); spin sum/max; fell 2 (acked 1); contended 1",
             s.calls[1] == 3u && s.acked[1] == 2u && s.timeouts[1] == 1u && s.rd[2] == 1u && s.rd[5] == 1u &&
             s.rd[0] + s.rd[1] + s.rd[3] + s.rd[4] == 0u && s.spinUs == 4016u && s.spinMax == 2003u && s.fell == 2u &&
             s.fellAcked == 1u && s.contended == 1u && s.waitMax[1] == 100500u && s.waitSum[1] == 104615u, 1u);
    expect_u("U3 OFF notes: calls 2, acked 1, timeouts 1, wait max 101000, sum 102100; ON counters untouched by OFF",
             s.calls[0] == 2u && s.acked[0] == 1u && s.timeouts[0] == 1u && s.waitMax[0] == 101000u && s.waitSum[0] == 102100u &&
             s.calls[1] == 3u, 1u);
    uint32_t logged = 0;
    for (uint32_t i = 0; i < 100u; i++) logged += n48_tlb83_ack_line(&s);
    expect_u("U3 item 3: the first 32 `acked` lines log, the other 68 are counted", logged == 32u && s.ackUnlogged == 68u &&
             s.ackLines == 100u, 1u);
    // item 4: the holder query has no side effect; the lock-wait histogram
    n48_mkh_table t {};
    const uintptr_t me = 0x1234u, other = 0x5678u;
    expect_u("U3 item 4: no holder -> 0", n48_mkh_holder(&t, me), 0u);
    const uint32_t tok = n48_mkh_enter(&t, me, 10u);
    expect_u("U3 item 4: after enter the holder is named (slot 1), another thread is not", n48_mkh_holder(&t, me) == 1u &&
             n48_mkh_holder(&t, other) == 0u && n48_mkh_holder(&t, 0u) == 0u, 1u);
    expect_u("U3 item 4: the query counts nothing (skips 0, yields 0), unlike n48_mkh_gate at M4",
             t.s[0].skips == 0u && t.s[0].yn == 0u, 1u);
    n48_mkh_unhold(&t, tok);
    expect_u("U3 item 4: after unhold the holder is gone", n48_mkh_holder(&t, me), 0u);
    n48_mkh_snap sn {}; n48_mkh_end(&t, tok, &sn);
    n48_kwl_stats l {};
    n48_kwl_note(&l, 3u); n48_kwl_note(&l, 1500u); n48_kwl_note(&l, 9000u);
    expect_u("U3 item 4: the lock-wait histogram: <=100, <=2000, more; n 3, sum 10503, max 9000",
             l.b[0] == 1u && l.b[4] == 1u && l.b[7] == 1u && l.n == 3u && l.sum == 10503u && l.mx == 9000u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ U4
static void u4_widths()
{
    char buf[4096];
    n48_tlb83_stats s {};
    for (uint32_t i = 0; i < 2u; i++) { s.calls[i] = s.acked[i] = s.timeouts[i] = s.waitMax[i] = s.waitSum[i] = ~0ull; }
    for (uint32_t i = 0; i < N48_TLB83_RB; i++) s.rd[i] = ~0ull;
    s.spinUs = s.spinMax = s.fell = s.fellAcked = s.contended = s.ackLines = s.ackUnlogged = ~0ull;
    const char *how = " - REFUSED: a continuous arm stands, unchanged";
    const char *longest = n48_tlb83_mode_name(N48_TLB83_OFF);
    if (std::strlen(n48_tlb83_mode_name(N48_TLB83_ON)) > std::strlen(longest)) longest = n48_tlb83_mode_name(N48_TLB83_ON);
    int w = std::snprintf(buf, sizeof buf, N48_TLB83_REPORT1_FMT, N48_TLB83_REPORT1_ARGS(&s, longest, how));
    std::printf("      %d: %s\n", w, buf);
    expect_u("U4 bare-83 line 1 <= 491 bytes at 20-digit fields, the longest mode name and `how`", w > 0 && w <= 491, 1u);
    w = std::snprintf(buf, sizeof buf, N48_TLB83_REPORT2_FMT, N48_TLB83_REPORT2_ARGS(&s));
    std::printf("      %d: %s\n", w, buf);
    expect_u("U4 bare-83 line 2 <= 491 bytes at 20-digit fields", w > 0 && w <= 491, 1u);
    w = std::snprintf(buf, sizeof buf, N48_TLB83_REPORT3_FMT, N48_TLB83_REPORT3_ARGS(&s, "MISSING"));
    std::printf("      %d: %s\n", w, buf);
    expect_u("U4 bare-83 line 3 <= 491 bytes at 20-digit fields", w > 0 && w <= 491, 1u);
    const uint32_t C5 = 99999u, C6 = 999999u, C7 = 9999999u;
    w = std::snprintf(buf, sizeof buf, N48_KW2_FMT, C5, C7, C5, C5, C5, C5, C5, C5, C5, C5, C6, "OFF", C5, C5, C5, C5, C5, C5, C5, C7,
                      C5, C5, C7, C5, C5, C7);
    std::printf("      %d: %s\n", w, buf);
    expect_u("U4 kswin2 line <= 491 bytes at every capped field's widest", w > 0 && w <= 491, 1u);
    expect_u("U4 the caps: 99999 / 9999999", n48_tlb83_c5(~0ull) == C5 && n48_tlb83_c7(~0ull) == C7 && n48_tlb83_c5(7u) == 7u &&
             n48_kw_c6(~0ull) == C6, 1u);
    n48_tlb83_stats z {}; z.ackLines = 5u;
    w = std::snprintf(buf, sizeof buf, N48_TLB83_REPORT3_FMT, N48_TLB83_REPORT3_ARGS(&z, "present"));
    expect_u("U4 line 3 prints `logged 5 of the first 32` below the cap", std::string(buf).find("logged 5 of the first 32") != std::string::npos, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ U5
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
static std::string body_of(const std::string &s, const char *head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return s.substr(a, (b == std::string::npos ? s.size() : b) - a);
}
static uint32_t in_order(const std::string &s, std::initializer_list<const char *> needles)
{
    size_t at = 0;
    for (const char *n : needles) {
        const size_t p = s.find(n, at);
        if (p == std::string::npos) return 0u;
        at = p + std::strlen(n);
    }
    return 1u;
}
static const char *kPollReg0527 =
    "static inline bool poll_reg(const DeviceContext &ctx, uint32_t reg, uint32_t mask, uint32_t expected,\n"
    "                            uint64_t timeout_us, uint32_t *outValue = nullptr) {\n"
    "    uint64_t elapsed_us = 0; uint32_t v = 0;\n"
    "    while (true) {\n"
    "        v = RREG32(ctx, reg);\n"
    "        if ((v & mask) == expected) { if (outValue) *outValue = v; return true; }\n"
    "        if (elapsed_us >= timeout_us) { if (outValue) *outValue = v; return false; }\n"
    "        IOSleep(1); elapsed_us += 1000;\n"
    "    }\n"
    "}\n";
static void u5_pins(const char *gmcP, const char *regsP, const char *brP, const char *ahhP, const char *cmP, const char *amdDir)
{
    const std::string gmc = slurp(gmcP), regs = slurp(regsP), br = slurp(brP), ahh = slurp(ahhP), cm = slurp(cmP);
    expect_u("U5 read the five source files", !gmc.empty() && !regs.empty() && !br.empty() && !ahh.empty() && !cm.empty(), 1u);
    // poll_reg unchanged (PSP/SMU keep sleeping)
    expect_u("U5 poll_reg is byte-identical to 0.0.527's (read, IOSleep(1), elapsed += 1000)", pin_count(regs, kPollReg0527), 1u);
    // only gmc_flush_gpu_tlb uses the helper; only the helper calls the pure poll
    const std::string fl = body_of(gmc, "kern_return_t\ngmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,");
    const std::string hp = body_of(gmc, "static __attribute__((noinline)) bool tlb83_request_and_wait(");
    expect_u("U5 found gmc_flush_gpu_tlb and tlb83_request_and_wait", !fl.empty() && !hp.empty(), 1u);
    expect_u("U5 tlb83_request_and_wait: defined once, called once, and that call is in gmc_flush_gpu_tlb",
             pin_count(gmc, "tlb83_request_and_wait(") == 2u && pin_count(fl, "tlb83_request_and_wait(dev, req_reg, req, ack_reg, expected, &value);") == 1u, 1u);
    expect_u("U5 n48_tlb83_poll is called once in the kext sources, inside tlb83_request_and_wait",
             pin_count(gmc, "n48_tlb83_poll(") == 1u && pin_count(hp, "n48_tlb83_poll(&io, expected, expected, N48_TLB83_TIMEOUT_US, &res);") == 1u &&
             pin_count(br, "n48_tlb83_poll(") == 0u && pin_count(ahh, "n48_tlb83_poll(") == 0u && pin_count(regs, "n48_tlb83_poll(") == 0u, 1u);
    {
        uint32_t other = 0;
        for (const char *f : { "psp_v14_0.cpp", "smu_v14_0.cpp", "sdma_v7_0.cpp", "amdgpu_init.cpp", "amdgpu_gart.cpp", "mes_v12_1.cpp",
                               "cp_v12_0.cpp", "rlc_v12_0.cpp", "compute_test.cpp" }) {
            const std::string t = slurp((std::string(amdDir) + "/" + f).c_str());
            if (t.empty()) other |= 0x100u;
            other += pin_count(t, "n48_tlb83_poll(") + pin_count(t, "tlb83_request_and_wait(") + pin_count(t, "gTlb83");
        }
        expect_u("U5 no other amd/ file (psp, smu, sdma, init, gart, mes, cp, rlc, compute) uses the spin, the helper or the switch", other, 0u);
    }
    // the gate: read once; ON -> helper; OFF -> the 0.0.527 write + poll_reg
    expect_u("U5 the switch is read ONCE in gmc_flush_gpu_tlb (acquire) and ON takes the helper",
             pin_count(fl, "gTlb83On") == 1u &&
             in_order(fl, { "if (__atomic_load_n(&gTlb83On, __ATOMIC_ACQUIRE) == N48_TLB83_ON) {",
                            "acked = tlb83_request_and_wait(dev, req_reg, req, ack_reg, expected, &value);", "} else {",
                            "WREG32(dev, req_reg, req);", "acked = poll_reg(dev, ack_reg, expected, expected,\n                         /*timeout_us*/ 100000, &value);",
                            "tlb83_note_off(t0, acked);", "}" }), 1u);
    expect_u("U5 gmc_flush_gpu_tlb writes the request once per path (ON in the helper, OFF here) and calls poll_reg once",
             pin_count(fl, "WREG32(dev, req_reg, req);") == 1u && pin_count(hp, "WREG32(dev, req_reg, req);") == 1u &&
             pin_count(fl, "poll_reg(") == 1u && pin_count(hp, "poll_reg(") == 0u, 1u);
    expect_u("U5 the mask/expected are unchanged: expected = 1u << vmid for both paths",
             pin_count(fl, "uint32_t expected = (1u << vmid);") == 1u, 1u);
    expect_u("U5 a failed wait returns kIOReturnTimeout after the UNCAPPED timeout line; the acked line is capped (item 3)",
             in_order(fl, { "if (!acked) {", "GMC_LOG(\"flush_gpu_tlb: ack timeout", "return kIOReturnTimeout;", "}",
                            "if (n48_tlb83_ack_line(&gTlb83S))\n        GMC_LOG(\"flush_gpu_tlb(ip=%u vmid=%u type=%u): req=%#x acked\"",
                            "return kIOReturnSuccess;" }) &&
             pin_count(fl, "n48_tlb83_ack_line(") == 1u, 1u);
    expect_u("U5 the helper returns the poll's own answer and value", pin_count(hp, "*value = res.value;\n    return res.ok != 0u;") == 1u, 1u);
    // the switch: OFF at boot; one writer; mid-arm guarded
    expect_u("U5 gTlb83On is OFF at boot (defined once, N48_TLB83_OFF)", pin_count(gmc, "volatile uint32_t gTlb83On { N48_TLB83_OFF };") == 1u, 1u);
    const uint32_t writers = pin_count(br, "__atomic_store_n(&amdgpu::gTlb83On,") + pin_count(gmc, "__atomic_store_n(&gTlb83On") +
                             pin_count(ahh, "gTlb83On =") + pin_count(br, "gTlb83On =") + pin_count(gmc, "gTlb83On =") +
                             pin_count(ahh, "__atomic_store_n(&amdgpu::gTlb83On");
    expect_u("U5 gTlb83On is written in ONE place (navi48_tlb83_switch)", writers == 1u &&
             pin_count(body_of(br, "uint32_t navi48_tlb83_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {"),
                       "__atomic_store_n(&amdgpu::gTlb83On, want, __ATOMIC_RELEASE);") == 1u, 1u);
    expect_u("U5 the switch refuses a change under a continuous arm and ON without the lock, before the write",
             in_order(body_of(br, "uint32_t navi48_tlb83_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {"),
                      { "if (contRefused) { *st = 5u;", "else if (want == N48_TLB83_M_BAD) { *st = 11u;",
                        "if (want == N48_TLB83_ON && !amdgpu::gTlb83Lock) { *st = 12u;", "__atomic_store_n(&amdgpu::gTlb83On, want" }), 1u);
    expect_u("U5 the selector computes the mid-arm guard with 83 and passes it (once), and 83 is in the guarded list",
             in_order(ahh, { "} else if ((arg & 0xffull) == 83ull) {",
                             "n48_cm_cont_switch_refused(83u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)",
                             "(void)navi48_tlb83_switch(m, contRefused83 ? 1u : 0u, &st);" }) &&
             pin_count(ahh, "} else if ((arg & 0xffull) == 83ull) {") == 1u && pin_count(cm, "case 83u:") == 1u, 1u);
    // the leaf lock (item 5)
    {
        const size_t a = hp.find("if (lk && !IOLockTryLock(lk)) { contended = 1u; IOLockLock(lk); }");
        const size_t b = hp.find("if (lk) IOLockUnlock(lk);");
        const std::string inside = (a != std::string::npos && b != std::string::npos && b > a) ? hp.substr(a, b - a) : std::string();
        expect_u("U5 LEAF LOCK: taken (try, then block) before the request write, released after the poll, before the notes",
                 !inside.empty() && in_order(hp, { "IOLock *const lk = gTlb83Lock;", "if (lk && !IOLockTryLock(lk))", "WREG32(dev, req_reg, req);",
                                                   "n48_tlb83_poll(", "if (lk) IOLockUnlock(lk);", "n48_tlb83_note_on(" }), 1u);
        expect_u("U5 LEAF LOCK: nothing else is acquired, logged or allocated while it is held",
                 !inside.empty() && pin_count(inside, "LOG(") == 0u && pin_count(inside, "IOLockLock(") == 1u &&
                 pin_count(inside, "Alloc") == 0u && pin_count(inside, "navi48_") == 0u && pin_count(inside, "Unlock") == 0u &&
                 pin_count(inside, "IOSimpleLock") == 0u, 1u);
        expect_u("U5 LEAF LOCK: gTlb83Lock is locked only through the helper's `lk` (never by name) anywhere",
                 pin_count(gmc, "IOLockLock(gTlb83Lock") + pin_count(br, "IOLockLock(amdgpu::gTlb83Lock") + pin_count(ahh, "gTlb83Lock") +
                 pin_count(gmc, "IOLockLock(") + pin_count(gmc, "IOLockUnlock(") + pin_count(gmc, "IOLockTryLock("), 3u);
        expect_u("U5 the lock is allocated once, in start(), and the helper is the only other reader in gmc_v12_0.cpp",
                 pin_count(br, "if (!amdgpu::gTlb83Lock) amdgpu::gTlb83Lock = IOLockAlloc();") == 1u && pin_count(br, "gTlb83Lock = ") == 1u &&
                 pin_count(gmc, "IOLock *gTlb83Lock { nullptr };") == 1u && pin_count(gmc, "= gTlb83Lock;") == 1u &&
                 pin_count(gmc, "gTlb83Lock = ") == 0u, 1u);
    }
    // item 4: the holder's gVramMmLock wait, both MM paths
    {
        const std::string rd = body_of(br, "bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {");
        const std::string wr = body_of(br, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
        uint32_t ok = (!rd.empty() && !wr.empty()) ? 1u : 0u;
        for (const std::string *b : { &rd, &wr })
            ok &= in_order(*b, { "const uint32_t mkw = n48::hw_mkh_holder(caller);", "if (mkw) clock_get_uptime(&l0);", "if (isOwner) {",
                                 "IOLockLock(gVramMmLock);", "} else {", "IOLockLock(gVramMmLock);", "}", "if (mkw) mm_lockwait_note(l0);",
                                 "uint64_t h0 = 0ull; clock_get_uptime(&h0);" }) &
                  (pin_count(*b, "hw_mkh_holder(") == 1u ? 1u : 0u) & (pin_count(*b, "mm_lockwait_note(") == 1u ? 1u : 0u);
        expect_u("U5 item 4: both MM paths bracket IOLockLock(gVramMmLock) for a marker holder, before the hold starts", ok, 1u);
        expect_u("U5 item 4: the holder query is side-effect free (n48_mkh_holder, not n48_mkh_gate) and feeds only the histogram",
                 pin_count(ahh, "uint32_t hw_mkh_holder(uintptr_t caller) { return n48_mkh_holder(&gMkh, caller); }") == 1u &&
                 pin_count(ahh, "void hw_mkh_lockwait(uint64_t us) { n48_kwl_note(&gKwL, us); }") == 1u &&
                 pin_count(ahh, "n48_kwl_note(") == 1u && pin_count(br, "n48_kwl_note(") == 0u, 1u);
        expect_u("U5 the kswin2 line follows the first kswin line in kswin_report_line",
                 in_order(body_of(ahh, "static __attribute__((noinline)) void kswin_report_line()"),
                          { "HWLOG(N48_KW_REPORT_FMT,", "HWLOG(N48_KW2_FMT," }), 1u);
    }
}

int main(int argc, char **argv)
{
    u1_selector();
    u2_poll();
    u3_counters();
    u4_widths();
    if (argc >= 7) u5_pins(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
    else { gFail++; std::printf("FAIL  U5 source pins need 6 paths (gmc_v12_0.cpp amdgpu_regs.h Navi48Bringup.cpp AHH gfx_commit.h amd/)\n"); }
    std::printf("gfx_tlb83_test: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
