// native_s2a_test.cpp - build 0.0.603 (native-stack step S2a, kernel half): the pure half of the native scanout selectors, driven against a
// model of the plane hardware, plus source pins on the kext.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn src/navi48-bringup/tests/native_s2a_test.cpp -o /tmp/native_s2a && /tmp/native_s2a .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_s2a_plant.sh plants breaks and shows
//    every check that catches them)
// Covers:
//   S1  the ABI 1.1 addendum: selector numbers 9..14, struct sizes/offsets, the Hello minor flag, kext build == Info.plist;
//   S2  ScanoutRegister bounds (reg_bounds): every refusal, the exact fit, overflow, the frame-buffer window, the HIGH dword, the console;
//   S3  the slot table: register / alias / full, the reuse rule (front, pending, unknown), unregister, the exact flip-target set;
//   S4  a present-per-VBLANK run against a hardware model (EARLIEST_INUSE lagging the latch by a frame): no shown or pending buffer is ever
//       drawn into, every present latches, none is replaced, no repeats; a double-present run counts replaced; an idle run counts repeats;
//   S5  the frame counter, 24 bits extended to 64 (wrap, seed, a backward step);
//   S6  the rate-based storm guard: 120 Hz for a minute never trips, 1001 in a second trips once, the cumulative-cap regression;
//   S7  the exemption table: EXACTLY (74,0), (76,0), (77,0); (76,1002), (77,1), (75,0), (74,1) ... refused;
//   S8  the restore state machine against a fake plane: program first, poll, the second attempt, give-up; restore_verified;
//   S9  BoFree-while-front: unpin_plan (front / pending / idle / leak / close / unknown EARLIEST);
//   S10 geometry, refresh, idle deadline, lock ranks;
//   S11 source pins: the shared restore function, the single console writer, restore-before-free on every BO path, lock order and no sleep
//       under the scanout lock, the exemption placed before the refusal, the golden copy at bind, the rate guard replacing the cumulative cap,
//       the version, the banned strings.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "navi48_scanout_pure.h"
#include "Navi48NativeABI.h"

using namespace n48scan;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }

// The numbers of this machine: 2560x1440 ARGB8888, pitch 2560 px, console at 0x80_0000_0000 (1 MiB..), visible pool 0x80_0270_0000...
static const uint32_t kH = 1440, kPitchB = 2560 * 4;   // the plane is 2560 wide: pitch 2560 px = 10240 B
static const uint64_t kBytes = (uint64_t)kPitchB * kH;                    // 14,745,600 = 0xE10000
static const uint64_t kConsole = 0x8000000000ull;
static const uint64_t kPoolLo = 0x8002700000ull, kPoolHi = 0x8010000000ull;
static const Fb kFb { 0x8000000000ull, 0x8400000000ull, kConsole, kBytes };

// ---------------------------------------------------------------------------------------------------------------------------
static void s1_abi() {
    expect_u("selector SCAN_QUERY", N48N_SEL_SCAN_QUERY, 9);       expect_u("SCAN_ACQUIRE", N48N_SEL_SCAN_ACQUIRE, 10);
    expect_u("SCAN_REGISTER", N48N_SEL_SCAN_REGISTER, 11);         expect_u("SCAN_PRESENT", N48N_SEL_SCAN_PRESENT, 12);
    expect_u("SCAN_STATUS", N48N_SEL_SCAN_STATUS, 13);             expect_u("SCAN_RELEASE", N48N_SEL_SCAN_RELEASE, 14);
    expect_u("the v1.0 selector count is unchanged (old clients' tables stay valid)", N48N_SEL_COUNT, 9);
    expect_u("the ABI 1.1 selector count", N48N_SEL_COUNT_1_1, 15);
    expect_u("ABI major unchanged: every v1.0 client still handshakes", N48N_ABI_VERSION, 1);
    expect_u("ABI minor (1.9 since 0.0.612: BoImportHost, selector 21; 1.8 since 0.0.610: the Metal nub selectors 19 / 20; 1.7 since 0.0.609: the row-120 HELD mode; 1.6 was 0.0.608: row 120 transition reads; 1.5 was 0.0.607: row 120 + the hold report; 1.4 was 0.0.606's 512 B result, 1.3 0.0.605's mode-trial selector, 1.2 0.0.604's DAL step)", N48N_ABI_MINOR, 9);
    expect(N48N_HELLO_F_MINOR != 1u && (N48N_HELLO_F_MINOR & 1u) == 0u, "the minor flag is not bit 0: T1's Hello(flags = 1) is still refused");
    expect_u("query size", sizeof(n48n_scan_query), 96); expect_u("reg size", sizeof(n48n_scan_reg), 32);
    expect_u("slot size", sizeof(n48n_scan_slot), 32);   expect_u("status size", sizeof(n48n_scan_status), 256);
    expect_u("slots in the ABI equal the kernel's", N48N_SCAN_MAX_SLOTS, kMaxSlots);
    expect_u("no-slot value equal", N48N_SCAN_NO_SLOT, kNoSlot);
    expect_u("format constant", N48N_SCAN_FMT_ARGB8888, kFmtArgb8888);
    expect_u("a v1.0 client's Hello out[0] (no flag) would be exactly the major", (uint64_t)N48N_ABI_VERSION, 1);
    expect_u("with the flag out[0] = major | minor << 16", (uint64_t)N48N_ABI_VERSION | ((uint64_t)N48N_ABI_MINOR << 16), 0x90001);
}

static BoRef vis(uint64_t mc, uint64_t size) { return BoRef{ true, mc, size }; }
static void s2_bounds() {
    uint64_t mc = 0, by = 0;
    const uint64_t poolMc = kPoolLo;                                            // 64 KiB aligned
    expect_u("a full-size buffer in the visible pool is accepted", reg_bounds(vis(poolMc, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kOk);
    expect_u("... at its own MC", mc, poolMc); expect_u("... of the exact byte count", by, kBytes);
    expect_u("a buffer ending exactly at the top of the visible pool is accepted", reg_bounds(vis(kPoolHi - kBytes, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kOk);
    expect_u("exact fit at a nonzero offset", reg_bounds(vis(poolMc, 3 * kBytes), 2 * kBytes, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kOk);
    expect_u("one byte short of the BO refused (offset + bytes > size)", reg_bounds(vis(poolMc, 3 * kBytes - 1), 2 * kBytes, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("offset beyond the BO refused", reg_bounds(vis(poolMc, kBytes), kBytes + 65536, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("offset == size refused (nothing fits)", reg_bounds(vis(poolMc, kBytes), kBytes, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("a hi-pool / GTT BO (not visible) refused", reg_bounds(BoRef{ false, poolMc, kBytes }, 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("pitch different from the live plane refused", reg_bounds(vis(poolMc, kBytes), 0, kPitchB + 64, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("height different from the live plane refused", reg_bounds(vis(poolMc, kBytes), 0, kPitchB, kH - 1, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("pitch 0 refused", reg_bounds(vis(poolMc, kBytes), 0, 0, kH, 0, kH, kFb, &mc, &by), kBadArg);
    expect_u("height 0 refused", reg_bounds(vis(poolMc, kBytes), 0, kPitchB, 0, kPitchB, 0, kFb, &mc, &by), kBadArg);
    expect_u("pitch not a multiple of 4 refused", reg_bounds(vis(poolMc, kBytes), 0, kPitchB + 2, kH, kPitchB + 2, kH, kFb, &mc, &by), kBadArg);
    expect_u("MC not 64 KiB aligned refused", reg_bounds(vis(poolMc + 4096, kBytes + 4096), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("an unaligned BO base made aligned by the offset is accepted", reg_bounds(vis(poolMc + 4096, kBytes + 65536), 65536 - 4096, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kOk);
    expect_u("... and lands on the aligned MC", mc, poolMc + 65536);
    // the frame-buffer window
    expect_u("below the window refused", reg_bounds(vis(0x7F00000000ull, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("the last byte one past the window refused", reg_bounds(vis(kFb.hi - kBytes + 65536, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    { const Fb top { 0x8000000000ull, 0x8100000000ull, kConsole, kBytes };     // a window that ends on the HIGH boundary
      expect_u("ending exactly at the window top accepted", reg_bounds(vis(top.hi - kBytes, kBytes), 0, kPitchB, kH, kPitchB, kH, top, &mc, &by), kOk);
      expect_u("one 64 KiB step later (one byte past the window) refused", reg_bounds(vis(top.hi - kBytes + 65536, kBytes), 0, kPitchB, kH, kPitchB, kH, top, &mc, &by), kBadArg); }
    { const Fb mid { 0x8000000000ull, kPoolHi, kConsole, kBytes };            // a window that ends INSIDE the console's HIGH dword: only the window can refuse
      expect_u("ending exactly at a window top inside the HIGH dword accepted", reg_bounds(vis(mid.hi - kBytes, kBytes), 0, kPitchB, kH, kPitchB, kH, mid, &mc, &by), kOk);
      expect_u("64 KiB past that window top (same HIGH dword) refused by the window alone", reg_bounds(vis(mid.hi - kBytes + 65536, kBytes), 0, kPitchB, kH, kPitchB, kH, mid, &mc, &by), kBadArg); }
    { Fb none = kFb; none.hi = 0; expect_u("NO window (hi 0) fails CLOSED", reg_bounds(vis(poolMc, kBytes), 0, kPitchB, kH, kPitchB, kH, none, &mc, &by), kBadArg); }
    // the HIGH dword: start and last byte must both share the console's
    { Fb wide { 0x0ull, 0xFFFFFFFFFFFFull, kConsole, kBytes };
      expect_u("start in another HIGH dword refused", reg_bounds(vis(0x8100000000ull, kBytes), 0, kPitchB, kH, kPitchB, kH, wide, &mc, &by), kBadArg);
      expect_u("a buffer STRADDLING a HIGH boundary (starts in 0x80, ends in 0x81) refused: HIGH would change mid-scan", reg_bounds(vis(0x8100000000ull - 0x700000ull, kBytes), 0, kPitchB, kH, kPitchB, kH, wide, &mc, &by), kBadArg);
      expect_u("... while the same size just below the boundary is fine", reg_bounds(vis(0x8100000000ull - kBytes, kBytes), 0, kPitchB, kH, kPitchB, kH, wide, &mc, &by), kOk); }
    // the console
    expect_u("overlapping the console buffer refused", reg_bounds(vis(kConsole + 0x100000, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("the console itself refused", reg_bounds(vis(kConsole, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("a buffer just after the console accepted", reg_bounds(vis(kConsole + kBytes, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kOk);
    // overflow
    expect_u("offset that overflows mc + offset refused", reg_bounds(vis(poolMc, 0xFFFFFFFFFFFF0000ull), 0xFFFFFFFFFFFF0000ull, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    expect_u("mc near 2^64 refused", reg_bounds(vis(0xFFFFFFFFFFFF0000ull, kBytes), 0, kPitchB, kH, kPitchB, kH, kFb, &mc, &by), kBadArg);
    // the output is untouched on refusal
    mc = 0x1234; by = 0x5678;
    (void)reg_bounds(vis(poolMc, kBytes), 0, kPitchB + 4, kH, kPitchB, kH, kFb, &mc, &by);
    expect(mc == 0x1234 && by == 0x5678, "a refusal leaves the outputs untouched");
}

static void s3_slots() {
    Table t; table_init(t, kConsole, kBytes);
    const uint64_t A = kPoolLo, B = kPoolLo + 0x1000000ull, C = kPoolLo + 0x2000000ull, D = kPoolLo + 0x3000000ull;
    uint32_t s = 99;
    expect_u("first register", slot_register(t, A, kBytes, &s), kOk); expect_u("lowest free is 0", s, 0);
    expect_u("second", slot_register(t, B, kBytes, &s), kOk); expect_u("slot 1", s, 1);
    expect_u("an alias of slot 0 refused", slot_register(t, A, kBytes, &s), kBadArg);
    expect_u("a partial overlap of slot 1 refused", slot_register(t, B + 0x80000, kBytes, &s), kBadArg);
    expect_u("overlapping the console refused", slot_register(t, kConsole + 0x10000, kBytes, &s), kBadArg);
    expect_u("mc 0 refused", slot_register(t, 0, kBytes, &s), kBadArg);
    expect_u("bytes 0 refused", slot_register(t, C, 0, &s), kBadArg);
    expect_u("third", slot_register(t, C, kBytes, &s), kOk); expect_u("slot 2", s, 2);
    expect_u("a fourth: the table is full", slot_register(t, D, kBytes, &s), kNoResources);
    // the exact flip-target set
    expect(flip_target_ok(t, kConsole) && flip_target_ok(t, A) && flip_target_ok(t, B) && flip_target_ok(t, C), "the console and the three slots are flip targets");
    expect(!flip_target_ok(t, D) && !flip_target_ok(t, 0) && !flip_target_ok(t, A + 0x1000), "nothing else is (not even inside a slot)");
    // the reuse rule
    t.pendActive = true; t.pendSlot = 1; t.pendMc = B;
    expect(!slot_reusable(t, 0, A, true), "the slot the hardware is fetching (EARLIEST_INUSE) is NOT reusable");
    expect(!slot_reusable(t, 1, A, true), "the PENDING slot is NOT reusable");
    expect(slot_reusable(t, 2, A, true), "an idle slot is reusable");
    expect(!slot_reusable(t, 2, A, false), "EARLIEST_INUSE unreadable => unknown => NOT reusable");
    expect(slot_reusable(t, 0, B, true), "the previous front is reusable once EARLIEST_INUSE has moved on (and it is not pending, not front)");
    t.front = 0;
    expect(!slot_reusable(t, 0, B, true), "the FRONT slot is NOT reusable even when EARLIEST_INUSE has not (yet) reached it: it is on screen from the next frame");
    expect(slot_reusable(t, 2, B, true), "... and the idle slot still is");
    t.front = kNoSlot;
    expect(!slot_reusable(t, 7, A, true), "an out-of-range slot is not reusable");
    Table free0; table_init(free0, kConsole, kBytes);
    expect(slot_reusable(free0, 0, 0, false), "a free (unregistered) slot is trivially reusable even with no reading");
    // unregister
    expect(!slot_unregister(t, 0, A, true) && t.s[0].used, "unregistering the shown slot is refused and changes nothing");
    expect(!slot_unregister(t, 1, A, true) && t.s[1].used, "unregistering the pending slot is refused");
    t.front = 2;
    expect(!slot_unregister(t, 2, A, true) && t.s[2].used, "unregistering the FRONT slot is refused (it is on screen; the console must go back first)");
    t.front = 0;
    expect(slot_unregister(t, 2, A, true) && !t.s[2].used && t.front == 0, "an idle slot is dropped");
    expect(slot_unregister(t, 2, A, true), "dropping a free slot is a no-op success");
    // present_begin
    Table u; table_init(u, kConsole, kBytes);
    (void)slot_register(u, A, kBytes, &s); (void)slot_register(u, B, kBytes, &s);
    expect_u("presenting an unregistered slot returns 0", present_begin(u, 2, 1, 5), 0);
    expect_u("presenting slot 0 returns its MC", present_begin(u, 0, 1, 5), A);
    expect(u.pendActive && u.pendSlot == 0 && u.pendMc == A && u.pendId == 1 && u.pendTarget == 5 && u.presents == 1 && u.replaced == 0, "pending is recorded");
    expect_u("a second present before the first latched replaces it", present_begin(u, 1, 2, 6), B);
    expect_u("... and counts replaced", u.replaced, 1);
    expect(u.pendSlot == 1 && u.s[0].presents == 1 && u.s[1].presents == 1, "the new one is pending, per-slot counts kept");
    // latch_observe: the RAW SURFACE_FLIP_PENDING bit only
    expect(!latch_observe(u, true, 10), "the flip bit still set (no VUPDATE has accepted the address): no latch");
    expect(latch_observe(u, false, 10), "the flip bit clear: LATCHED");
    expect(u.latched == 1 && u.front == 1 && !u.pendActive && u.s[1].latchedFrame == 10 && u.s[1].latches == 1 && u.firstLatchFrame == 10, "latched bookkeeping");
    expect(!latch_observe(u, false, 11), "nothing pending: no second latch");
    note_vupdate_no_latch(u); expect_u("a VUPDATE that latched nothing while a picture is up is a repeat", u.repeats, 1);
    Table w; table_init(w, kConsole, kBytes); note_vupdate_no_latch(w); expect_u("before any picture there is no repeat", w.repeats, 0);
}

// ---- a model of the plane: HUBP0 with EARLIEST_INUSE lagging the latch by one frame ---------------------------------------------------
struct Hubp {
    uint64_t primary, earliest, latchedPrimary, request;
    bool flipPending;
    uint32_t raw;
    uint32_t settleLag;                    // 0: EARLIEST_INUSE reaches the new surface within the same frame; 1: it lags a whole frame (worst case)
    explicit Hubp(uint64_t console, uint32_t lag = 0) : primary(console), earliest(console), latchedPrimary(console), request(console), flipPending(false), raw(1000), settleLag(lag) {}
    void program(uint64_t mc) { primary = mc; request = mc; flipPending = true; }
    void vupdate() {                       // the VUPDATE accepts the programmed address (the flip bit clears); EARLIEST_INUSE follows immediately or a frame later
        if (settleLag) earliest = latchedPrimary;
        if (flipPending) { latchedPrimary = primary; flipPending = false; }
        if (!settleLag) earliest = latchedPrimary;
        raw = (raw + 1) & kFcMask;
    }
};
static void run_flipper(uint32_t lag, const char *tag, uint64_t *skippedOut, uint64_t *busyOut, Table *tOut) {
    Table &t = *tOut; table_init(t, kConsole, kBytes);
    const uint64_t mcs[3] = { kPoolLo, kPoolLo + 0x1000000ull, kPoolLo + 0x2000000ull };
    uint32_t s = 0;
    for (int i = 0; i < 3; i++) (void)slot_register(t, mcs[i], kBytes, &s);
    Hubp hw(kConsole, lag);
    FcExt fc {};
    uint64_t skipped = 0, drewIntoBusy = 0;
    for (int f = 0; f < 600; f++) {
        // the app's turn (between VUPDATEs): resolve the previous present, then draw into a REUSABLE slot only, then present it
        const uint64_t fcNow = fc_extend(fc, hw.raw);
        (void)latch_observe(t, hw.flipPending, fcNow);
        uint32_t pick = kNoSlot;
        for (uint32_t k = 0; k < 3; k++) if (slot_reusable(t, k, hw.earliest, true)) { pick = k; break; }
        if (pick == kNoSlot) { skipped++; }
        else {
            // the invariant the reuse rule exists for: what we are about to draw into is not on screen, not being fetched, not about to be
            if (t.s[pick].mc == hw.earliest || t.s[pick].mc == hw.latchedPrimary || (t.pendActive && t.pendSlot == pick) || t.front == pick) drewIntoBusy++;
            const uint64_t mc = present_begin(t, pick, (uint64_t)f + 1, fcNow + 1);
            if (mc == 0 || !flip_target_ok(t, mc)) { drewIntoBusy += 1000; break; }
            hw.program(mc);
        }
        hw.vupdate();                      // the interrupt: the handler resolves the latch, else counts a repeat
        const uint64_t fcIrq = fc_extend(fc, hw.raw);
        if (!latch_observe(t, hw.flipPending, fcIrq)) note_vupdate_no_latch(t);
    }
    (void)tag;
    *skippedOut = skipped; *busyOut = drewIntoBusy;
}
static void s4_run() {
    const uint64_t mcs[3] = { kPoolLo, kPoolLo + 0x1000000ull, kPoolLo + 0x2000000ull };
    uint32_t s = 0;
    for (uint32_t lag = 0; lag < 2; lag++) {
        Table t; uint64_t skipped = 0, busy = 0;
        run_flipper(lag, lag ? "EARLIEST_INUSE a frame late" : "EARLIEST_INUSE prompt", &skipped, &busy, &t);
        expect_u(lag ? "600 frames, EARLIEST_INUSE a frame late: nothing ever drawn into a shown / pending / fetched buffer" : "600 frames: nothing ever drawn into a shown / pending / fetched buffer", busy, 0);
        expect_u("the run never starved for a safe buffer (three slots suffice)", skipped, 0);
        expect_u("presents", t.presents, 600);
        expect_u("EVERY present was seen to latch (the IRQ resolves each one the frame it is accepted)", t.latched, 600);
        expect_u("nothing was replaced before it was shown (no false replaced from a lagging EARLIEST_INUSE)", t.replaced, 0);
        expect_u("no repeat in a run that presents every VBLANK", t.repeats, 0);
        expect(t.lastLatchFrame - t.firstLatchFrame == 599, "the latch frames span exactly 599 frames: one DISTINCT frame per latch");
    }
    // presenting twice per VBLANK: the first is overwritten before any VUPDATE accepts it
    Table d; table_init(d, kConsole, kBytes);
    for (int i = 0; i < 3; i++) (void)slot_register(d, mcs[i], kBytes, &s);
    Hubp h2(kConsole); FcExt fc2 {};
    for (int f = 0; f < 100; f++) {
        (void)latch_observe(d, h2.flipPending, fc_extend(fc2, h2.raw));
        for (uint32_t k = 0; k < 2; k++) { const uint64_t mc = present_begin(d, (uint32_t)((f * 2 + k) % 3), (uint64_t)f * 2 + k + 1, 0); h2.program(mc); }
        h2.vupdate();
        if (!latch_observe(d, h2.flipPending, fc_extend(fc2, h2.raw))) note_vupdate_no_latch(d);
    }
    expect_u("two presents per VBLANK: the first of each pair is replaced (100 frames)", d.replaced, 100);
    expect(d.latched == 100 && d.presents == 200, "and exactly one per frame is delivered");

    // presenting nothing: the front repeats
    Table r; table_init(r, kConsole, kBytes);
    (void)slot_register(r, mcs[0], kBytes, &s);
    Hubp h3(kConsole); FcExt fc3 {};
    (void)present_begin(r, 0, 1, 0); h3.program(mcs[0]);
    for (int f = 0; f < 50; f++) { h3.vupdate(); if (!latch_observe(r, h3.flipPending, fc_extend(fc3, h3.raw))) note_vupdate_no_latch(r); }
    expect_u("a single present is latched exactly once", r.latched, 1);
    expect_u("and the 49 VUPDATEs after it are repeats of the front", r.repeats, 49);
    // the poll path (no interrupt at all): a Present resolves the previous latch itself before it counts a replace
    Table q; table_init(q, kConsole, kBytes);
    for (int i = 0; i < 3; i++) (void)slot_register(q, mcs[i], kBytes, &s);
    Hubp h4(kConsole); FcExt fc4 {};
    for (int f = 0; f < 60; f++) {
        (void)latch_observe(q, h4.flipPending, fc_extend(fc4, h4.raw));
        uint32_t pick = kNoSlot;
        for (uint32_t k = 0; k < 3; k++) if (slot_reusable(q, k, h4.earliest, true)) { pick = k; break; }
        if (pick == kNoSlot) continue;
        h4.program(present_begin(q, pick, (uint64_t)f + 1, 0));
        h4.vupdate();                      // NO interrupt handler in this run
    }
    expect(q.replaced == 0 && q.latched >= 59, "with no interrupts at all, the poll before each Present still counts every latch and no false replace");
}

static void s5_frame() {
    FcExt e {};
    expect_u("the first read seeds the value", fc_extend(e, 1000), 1000);
    expect_u("a forward step", fc_extend(e, 1005), 1005);
    expect_u("the same value again does not move it", fc_extend(e, 1005), 1005);
    expect_u("the top bits of the raw register are ignored", fc_extend(e, 0xFF000000u | 1006), 1006);
    FcExt hb {};
    expect_u("... including in the SEEDING read", fc_extend(hb, 0xAB000005u), 5);
    FcExt w {};
    (void)fc_extend(w, 0xFFFFFE);
    expect_u("up to the wrap", fc_extend(w, 0xFFFFFF), 0xFFFFFF);
    expect_u("across the 24-bit wrap: 0xFFFFFF -> 0x000001 is +2", fc_extend(w, 1), 0x1000001ull);
    expect_u("and on", fc_extend(w, 100), 0x1000064ull);
    // many wraps, sampled every 7 frames
    FcExt m {}; uint64_t truth = 0xFFFF00u, last = 0;
    (void)fc_extend(m, (uint32_t)truth & kFcMask);
    bool mono = true, exact = true;
    for (int i = 0; i < 20000000; i += 1) {
        truth += 7;
        const uint64_t v = fc_extend(m, (uint32_t)(truth & kFcMask));
        if (v < last) mono = false;
        last = v;
        if (v != truth) { exact = false; break; }
    }
    expect(mono && exact, "sampled every 7 frames over 140 M frames (8 wraps) the extended value equals the true count and never goes backwards");
    FcExt b {};
    (void)fc_extend(b, 5000);
    expect_u("a stale read a few frames BEHIND does not jump forward by 2^24", fc_extend(b, 4990), 5000);
    expect_u("and the next good read continues from where it was", fc_extend(b, 5010), 5010);
    FcExt hlf {};
    (void)fc_extend(hlf, 0);
    expect_u("a forward step just under half a wrap is taken", fc_extend(hlf, 0x7FFFFF), 0x7FFFFF);
    FcExt hlf2 {};
    (void)fc_extend(hlf2, 0);
    expect_u("a step of half a wrap or more is treated as backwards", fc_extend(hlf2, 0x800000), 0);
}

static void s6_storm() {
    Storm s {};
    bool tripped = false;
    // 120 Hz for a minute, timestamps 8.333 ms apart
    for (uint64_t i = 0; i < 120 * 60; i++) if (storm_note(s, 1000000000ull + i * 8333333ull)) tripped = true;
    expect(!tripped && !s.tripped, "120 interrupts a second for 60 s never trips the rate guard");
    // 1000 in one window is still fine, the 1001st trips
    Storm a {}; int trips = 0; uint64_t t0 = 5000000000ull;
    for (uint32_t i = 0; i < 1000; i++) if (storm_note(a, t0 + i * 100000ull)) trips++;
    expect_u("1000 interrupts in 100 ms: not yet", trips, 0);
    if (storm_note(a, t0 + 1000 * 100000ull - 1)) trips++;
    expect_u("the 1001st inside the same second trips", trips, 1);
    for (uint32_t i = 0; i < 5000; i++) if (storm_note(a, t0 + 200000000ull + i)) trips++;
    expect_u("and it trips exactly once", trips, 1);
    // a slow drip across windows never trips: 900 per window for 10 windows
    Storm d {}; bool dt = false;
    for (uint32_t w = 0; w < 10; w++) for (uint32_t i = 0; i < 900; i++) if (storm_note(d, (uint64_t)w * 1000000000ull + 1 + i * 1000000ull)) dt = true;
    expect(!dt, "900 per window for 10 windows: the window resets, no trip");
    // a clock that goes backwards restarts the window rather than wrapping the subtraction
    Storm c {};
    (void)storm_note(c, 9000000000ull);
    (void)storm_note(c, 1000ull);
    expect_u("a backwards clock restarts the window", c.n, 1);
    // the regression: the cumulative cap (200000 entries) trips at 120 Hz after this long, the rate guard never does
    const uint64_t capSeconds = 200000ull / 120ull;
    expect(capSeconds / 60 >= 27 && capSeconds / 60 <= 28, "the cumulative cap of 200000 trips after ~27.7 minutes at 120 Hz (the defect this replaces)");
    Storm l {}; bool lt = false;
    for (uint64_t i = 0; i < 250000; i++) if (storm_note(l, 1 + i * 8333333ull)) lt = true;
    expect(!lt, "250000 interrupts at 120 Hz (34 minutes): the rate guard does not trip");
}

static void s7_exempt() {
    expect(accel_exempt(74, 0) && accel_exempt(76, 0) && accel_exempt(77, 0), "dcnstate 0, dcnflip 0, dcnmode 0 are exempt");
    expect(!accel_exempt(75, 0), "dcnvbl 0 is NOT exempt (it is a register write of an interrupt source; refuse)");
    expect(!accel_exempt(76, 1002), "(76, 1002) - flip mode's A/B test - is refused");
    expect(!accel_exempt(76, 1) && !accel_exempt(76, 2) && !accel_exempt(76, 30) && !accel_exempt(76, 1001) && !accel_exempt(76, 1240), "dcnflip's test pattern and A/B range are refused");
    expect(!accel_exempt(77, 1) && !accel_exempt(77, 30) && !accel_exempt(77, 101) && !accel_exempt(77, 130), "dcnmode 1..30 and 101..130 (writes) are refused");
    expect(!accel_exempt(74, 1), "dcnstate 1 (the allowlist self-test) is refused");
    expect(!accel_exempt(0, 0), "action 0 is not in the table (it is never refused anyway)");
    expect(!accel_exempt(74, 0x100000000ull) && !accel_exempt(76, 0x100000000ull), "an argument whose LOW 32 bits are 0 is not 0");
    int n = 0;
    for (uint32_t a = 0; a < 400; a++) for (uint64_t g : { 0ull, 1ull, 2ull, 30ull, 100ull, 101ull, 130ull, 1001ull, 1002ull, 1240ull, 0xFFFFFFFFull, ~0ull }) if (accel_exempt(a, g)) n++;
    expect_u("over every action 0..399 and every listed argument, exactly three pairs are exempt", n, 3);
}

static void s8_restore() {
    // a fake plane that shows `shown` and latches whatever is programmed after `latchAfter` polls (or never; or only the second program)
    struct Fake { uint64_t primary, earliest, request; int polls; int latchAfter; int programs; int ignoreFirst; };
    auto run = [&](int latchAfter, int ignoreFirst, uint32_t *attemptsOut, uint32_t *programsOut, bool *verifiedOut, uint32_t *pollsOut) {
        const uint64_t shown = 0x8002700000ull;
        Fake f { shown, shown, shown, 0, latchAfter, 0, ignoreFirst };
        RestoreSm m; restore_begin(m, kConsole);
        RestoreAct act = restore_next(m, false, 0, 0, true);
        expect_u("the FIRST step is always to program the console", act, kActProgram);
        uint32_t polls = 0;
        while (act != kActDone) {
            if (act == kActProgram) {
                f.programs++; f.request = kConsole; f.polls = 0;
                if (!(ignoreFirst && f.programs == 1)) f.primary = kConsole;
                act = kActPoll;
                continue;
            }
            polls++;
            f.polls++;
            if (f.primary == kConsole && f.polls >= f.latchAfter && f.latchAfter >= 0) f.earliest = kConsole;
            const bool pend = f.earliest != f.request;
            act = restore_next(m, true, f.primary, f.earliest, pend);
            if (polls > 100000) break;
        }
        *attemptsOut = m.attempts; *programsOut = (uint32_t)f.programs; *verifiedOut = m.verified; *pollsOut = polls;
    };
    uint32_t at = 0, pr = 0, po = 0; bool ok = false;
    run(5, 0, &at, &pr, &ok, &po);
    expect(ok && at == 1 && pr == 1 && po == 5, "a plane that latches after 5 polls: verified on the first attempt, programmed once");
    run(0, 0, &at, &pr, &ok, &po);
    expect(ok && at == 1 && pr == 1 && po == 1, "an already-latched plane is verified at the first poll");
    run(-1, 0, &at, &pr, &ok, &po);
    expect(!ok && at == kRestoreAttempts && pr == kRestoreAttempts && po == kRestorePolls * kRestoreAttempts, "a plane that never latches: two attempts of 200 polls, then NOT verified");
    run(3, 1, &at, &pr, &ok, &po);
    expect(ok && at == 2 && pr == 2, "the first program lost (never took): the second attempt verifies");
    RestoreSm early; restore_begin(early, kConsole);
    expect_u("even when the plane already reads as the console, the machine programs it first", restore_next(early, true, kConsole, kConsole, false), kActProgram);
    // restore_verified
    expect(restore_verified(kConsole, kConsole, false, kConsole), "plane, EARLIEST_INUSE and not-pending all on the console: verified");
    expect(!restore_verified(kConsole, kConsole, true, kConsole), "still pending: not verified");
    expect(!restore_verified(kConsole, kPoolLo, false, kConsole), "EARLIEST_INUSE still on our buffer: not verified");
    expect(!restore_verified(kPoolLo, kConsole, false, kConsole), "the plane still programmed to our buffer: not verified");
    expect(!restore_verified(0, 0, false, 0), "a console of 0 is never verified");
}

static void s9_unpin() {
    Table t; table_init(t, kConsole, kBytes);
    const uint64_t A = kPoolLo, B = kPoolLo + 0x1000000ull, C = kPoolLo + 0x2000000ull;
    uint32_t s = 0;
    (void)slot_register(t, A, kBytes, &s); (void)slot_register(t, B, kBytes, &s); (void)slot_register(t, C, kBytes, &s);
    t.pendActive = true; t.pendSlot = 1; t.pendMc = B;
    // hardware fetching A, B pending, C idle
    UnpinPlan p = unpin_plan(t, 1u << 0, false, A, true);
    expect(p.fullRelease && p.dropMask == 0, "BoFree of the buffer the hardware is SHOWING: the full restore first, nothing dropped");
    t.front = 2;
    p = unpin_plan(t, 1u << 2, false, A, true);
    expect(p.fullRelease && p.dropMask == 0, "BoFree of the FRONT buffer (latched, EARLIEST_INUSE not there yet): the full restore first");
    t.front = kNoSlot;
    p = unpin_plan(t, 1u << 1, false, A, true);
    expect(p.fullRelease, "BoFree of the PENDING buffer: the full restore first");
    p = unpin_plan(t, 1u << 2, false, A, true);
    expect(!p.fullRelease && p.dropMask == (1u << 2), "BoFree of an idle buffer: just that slot is dropped");
    p = unpin_plan(t, (1u << 2) | (1u << 0), false, A, true);
    expect(p.fullRelease, "one BO hosting an idle slot AND the shown slot: the full restore (any busy slot decides)");
    p = unpin_plan(t, 1u << 2, false, A, false);
    expect(p.fullRelease, "EARLIEST_INUSE unreadable: unknown, so the full restore");
    p = unpin_plan(t, 1u << 2, true, A, true);
    expect(p.fullRelease && p.dropMask == 0, "leak (HUNG) / close: ALWAYS the full restore first, even for an idle slot");
    p = unpin_plan(t, 0, false, A, true);
    expect(!p.fullRelease && p.dropMask == 0, "no pins: nothing to do");
    Table none; table_init(none, kConsole, kBytes);
    p = unpin_plan(none, 1u << 0, false, 0, false);
    expect(!p.fullRelease && p.dropMask == 1u, "a pin on a slot that was already dropped (free) is harmless");
}

static void s10_misc() {
    expect(restore_exact(0, false, 0, 0), "a console plane with VMID 0, TMZ 0, FLIP_TYPE 0, viewport start 0 is restorable exactly");
    expect(!restore_exact(1, false, 0, 0) && !restore_exact(15, false, 0, 0), "a nonzero VMID is refused (the restore writes VMID 0)");
    expect(!restore_exact(0, true, 0, 0), "TMZ set is refused");
    expect(!restore_exact(0, false, 1, 0), "an immediate FLIP_TYPE is refused");
    expect(!restore_exact(0, false, 0, 0x10000) && !restore_exact(0, false, 0, 1), "a nonzero PRI_VIEWPORT_START (x or y) is refused");
    expect_u("geometry: the native boot plane is ok", geom_check(2560, 1440, 2560, 8, 0, false), kGeomOk);
    expect_u("DCC enabled refused", geom_check(2560, 1440, 2560, 8, 0, true), kGeomDcc);
    expect_u("a tiled plane refused", geom_check(2560, 1440, 2560, 8, 1, false), kGeomTiling);
    expect_u("another pixel format refused", geom_check(2560, 1440, 2560, 10, 0, false), kGeomFormat);
    expect_u("pitch below the width refused", geom_check(2560, 1440, 2559, 8, 0, false), kGeomPitch);
    expect_u("pitch above the limit refused", geom_check(2560, 1440, 16385, 8, 0, false), kGeomPitch);
    expect_u("zero width refused", geom_check(0, 1440, 2560, 8, 0, false), kGeomDims);
    expect_u("an absurd height refused", geom_check(2560, 8193, 2560, 8, 0, false), kGeomDims);
    // refresh: the census mode and the EDID 120 Hz row
    const uint64_t r60 = refresh_mhz(241500000ull, 2720, 1481), r120 = refresh_mhz(497750000ull, 2720, 1525);
    expect(r60 >= 59940 && r60 <= 59960, "241.5 MHz over 2720 x 1481 is 59.95 Hz (the census)");
    expect(r120 >= 119990 && r120 <= 120010, "497.75 MHz over 2720 x 1525 is 120.00 Hz (the EDID row)");
    expect_u("zero pixel clock -> 0", refresh_mhz(0, 2720, 1481), 0); expect_u("zero totals -> 0", refresh_mhz(241500000ull, 0, 1481), 0);
    // idle
    expect(!idle_expired(kIdleNs - 1, 0) && idle_expired(kIdleNs, 0) && idle_expired(kIdleNs + 1, 0), "the idle deadline is exactly 5 s");
    expect(!idle_expired(5, 10), "a clock behind the last activity is never expired");
    expect_u("the idle constant is 5 s", kIdleNs, 5000000000ull);
    // lock ranks: DOCUMENTATION of the ranks only (a tautology over two constants proves nothing about the kext). The ORDER itself is pinned by
    // source text in s11 (Register / Acquire / Release take gCliLock before any n48dcn call, teardown runs under it, the DCN layer never names
    // gCliLock, Query / Present / Status never take it) and by plants 51-54 in native_s2a_plant.sh, which invert it in the source.
    expect(lock_order_ok(0, kRankCliLock) && lock_order_ok(kRankCliLock, kRankScanLock) && lock_order_ok(0, kRankScanLock), "gCliLock then the scanout lock, either alone: allowed");
    expect(!lock_order_ok(kRankScanLock, kRankCliLock), "the scanout lock then gCliLock: the inversion, refused");
    expect(!lock_order_ok(kRankScanLock, kRankScanLock), "the scanout lock is not recursive");
}

// ---------------------------------------------------------------------------------------------------------------------------
// The region of `src` from the first occurrence of `head` to the closing "\n}\n" of that function.
static std::string fn_body(const std::string &src, const std::string &head) {
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? src.substr(a) : src.substr(a, b - a + 3);
}
// True when every IOSleep( in `body` is IMMEDIATELY preceded (ignoring whitespace) by `IOLockUnlock(l);` - the lock is dropped right before the
// sleep - or by a `for (;;) {` loop head (the watchdog's top-of-loop sleep, where no lock is held; the loop's back edges all pass an unlock).
static bool sleeps_only_unlocked(const std::string &body, const std::string &unlockStmt, const std::string &) {
    size_t p = 0;
    while ((p = body.find("IOSleep(", p)) != std::string::npos) {
        size_t e = p;
        while (e > 0 && (body[e - 1] == ' ' || body[e - 1] == '\t' || body[e - 1] == '\n')) e--;
        const bool unlocked = e >= unlockStmt.size() && body.compare(e - unlockStmt.size(), unlockStmt.size(), unlockStmt) == 0;
        const std::string loop = "for (;;) {";
        const bool loopHead = e >= loop.size() && body.compare(e - loop.size(), loop.size(), loop) == 0;
        const std::string nl = "/*nolock*/";
        const bool noLock = e >= nl.size() && body.compare(e - nl.size(), nl.size(), nl) == 0;   // kext-stop wait: no scanout lock is held there
        if (!unlocked && !loopHead && !noLock) return false;
        p += 8;
    }
    return true;
}

static void s11_pins(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), dcnh = slurp(src + "dcn/navi48_dcn.hpp"), eng = slurp(src + "amd/native_s1c.cpp"), engh = slurp(src + "amd/native_s1c.h"),
                      cli = slurp(src + "Navi48NativeClient.cpp"), boot = slurp(src + "Navi48Bringup.cpp"), pure = slurp(src + "dcn/navi48_scanout_pure.h"),
                      abi = slurp(src + "Navi48NativeABI.h"), plist = slurp(root + "/src/navi48-bringup/Info.plist"), tool = slurp(root + "/tools/native/n48scan.c"),
                      mk = slurp(root + "/src/navi48-bringup/Makefile");
    expect(!dcn.empty() && !dcnh.empty() && !eng.empty() && !engh.empty() && !cli.empty() && !boot.empty() && !pure.empty() && !abi.empty() && !plist.empty() && !tool.empty(), "the sources are readable");

    // -- ONE restore function, shared --
    expect_u("the console plane is programmed in exactly ONE place (the restore)", count_of(dcn, "dcn41_hubp_program_flip(&gDcn.d, 0u, consoleMc"), 1);
    expect_u("...and the native Present is the only other writer of the plane (one call, in scanPresent)", count_of(fn_body(dcn, "uint32_t scanPresent("), "dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false)"), 1);
    expect_u("scan_restore is defined once and called by Release, BoGone, the watchdog, the escape and the kext stop (5 sites) and, since 0.0.609, the row-120 hold's end (mt_scan_release)", count_of(dcn, "scan_restore("), 7);
    const std::string rel = fn_body(dcn, "uint32_t scanRelease(");
    expect(rel.find("scan_restore(why, false, 0u, out)") != std::string::npos, "ScanoutRelease runs the shared restore");
    expect(fn_body(dcn, "void scanEscape(const char *why) {").find("scan_restore(why, true, 0u, nullptr)") != std::string::npos, "`dcnflip 0` (scanEscape) runs the shared restore, forced");
    expect(fn_body(dcn, "static void scan_watchdog_loop(uint32_t myGen) {").find("scan_restore(why, false, myGen, nullptr)") != std::string::npos, "the watchdog runs the shared restore");
    expect(fn_body(dcn, "uint32_t scanBoGone(").find("scan_restore(") != std::string::npos, "BoGone runs the shared restore when the plan says full");
    expect(dcn.find("scanEscape(\"explicit dcnflip 0\");") != std::string::npos && dcn.find("scanEscape(\"explicit dcnflip 0\");") > dcn.find("uint32_t flip(uint64_t arg"), "dcnflip 0 calls scanEscape inside flip()");
    {
        const std::string rb = fn_body(dcn, "static uint32_t scan_restore(");
        expect(rb.find("restore_next(sm, false") != std::string::npos && rb.find("kActProgram") != std::string::npos && rb.find("restore_verified(cur, early, pend, consoleMc)") != std::string::npos,
               "scan_restore is the n48scan state machine plus a final verification");
        expect(rb.find("table_init(gScan.tbl") > rb.find("dcn41_hubp_program_flip") && rb.find("gScan.acquired = false") > rb.find("dcn41_hubp_program_flip"), "the state is torn down only AFTER the console was programmed");
        expect(rb.find("gDcn.srcEnabled = false") != std::string::npos, "the VUPDATE source is disabled by the restore");
        expect(sleeps_only_unlocked(rb, "IOLockUnlock(l);", "IOLockLock(l);"), "scan_restore sleeps only with the scanout lock DROPPED");
    }
    // -- N48N side: restore before free, on every path --
    const std::string ecl = fn_body(eng, "void n1c_close(");
    expect(ecl.find("scan_teardown(s, how, tr)") != std::string::npos && ecl.find("scan_teardown(s, how, tr)") < ecl.find("idle_wait()") && ecl.find("scan_teardown(s, how, tr)") < ecl.find("bool leak = hung_now();"),
           "close: the console goes back BEFORE the idle wait and the HUNG decision");
    expect(ecl.find("IOLockLock(gCliLock);") < ecl.find("scan_teardown(s, how, tr)"), "close: ... under gCliLock (lock order gCliLock -> DCN)");
    expect(ecl.find("scan_teardown(s, how, tr)") < ecl.find("bo_release(h, kRelClosing)"), "close: ... and before any BO memory is freed");
    const std::string ebo = fn_body(eng, "static void bo_release(uint32_t h, RelMode mode) {");
    expect(ebo.find("if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;") != std::string::npos, "bo_release unpins in EVERY mode");
    expect(ebo.find("if (b.pinLeak != 0u) mode = kRelLeak;") > ebo.find("scan_unpin(h, b, mode)") && ebo.find("if (b.pinLeak != 0u) mode = kRelLeak;") < ebo.find("if (mode == kRelClosing) {"),
           "a BO whose console restore did NOT verify is LEAKED (mode forced to Leak before any branch frees it)");
    expect(fn_body(eng, "static void scan_teardown(").find("pinLeak = 1u") != std::string::npos && fn_body(eng, "static void scan_teardown(").find("r[0] == 0ull") != std::string::npos, "close / Release: an unverified restore marks the pinned BOs leak");
    expect(ebo.find("scan_unpin(h, b, mode)") < ebo.find("if (mode == kRelClosing) {") && ebo.find("scan_unpin(h, b, mode)") < ebo.find("sysmem_free(") && ebo.find("scan_unpin(h, b, mode)") < ebo.find("vram_alloc.free(b.a)"),
           "bo_release: the pin is dealt with BEFORE the Closing / Normal / Leak branches free or leak anything");
    const std::string eun = fn_body(eng, "static bool scan_unpin(");
    expect(eun.find("n48dcn::scanBoGone(b.pinMask, b.pinGen, mode != kRelNormal, r)") != std::string::npos, "Leak and Closing ask for the full restore (alwaysFull = mode != Normal)");
    expect(fn_body(eng, "IOReturn n1c_bo_free(").find("if (hung_now()) bo_release(h, kRelLeak);") != std::string::npos, "BoFree under HUNG goes through bo_release(Leak), hence the restore first");
    // BoFree: the pin lives on the Bo, set under gCliLock only
    expect_u("pinMask is written in exactly four places, all under gCliLock: register (stale clear, set), teardown (clear), unpin (clear); Release goes through teardown", count_of(eng, "pinMask = "), 4);
    const std::string ereg = fn_body(eng, "IOReturn n1c_scan_register(");
    expect(ereg.find("IOLockLock(gCliLock)") != std::string::npos && ereg.find("IOLockLock(gCliLock)") < ereg.find("n48dcn::scanRegister("), "Register takes gCliLock before the DCN lock");
    for (const char *fn : { "IOReturn n1c_scan_acquire(", "IOReturn n1c_scan_release(" }) {
        const std::string b = fn_body(eng, fn);
        expect(b.find("IOLockLock(gCliLock)") != std::string::npos && b.find("closed while we waited for the lock") != std::string::npos, "this scanout selector takes gCliLock and re-checks the session under it");
    }
    expect(fn_body(eng, "IOReturn n1c_scan_acquire(").find("IOLockLock(gCliLock)") < fn_body(eng, "IOReturn n1c_scan_acquire(").find("n48dcn::scanAcquire("), "Acquire takes gCliLock BEFORE the DCN lock (a racing close cannot leave the plane taken)");
    expect(fn_body(eng, "IOReturn n1c_scan_release(").find("IOLockLock(gCliLock)") < fn_body(eng, "IOReturn n1c_scan_release(").find("scan_teardown("), "Release takes gCliLock before the teardown");
    for (const char *fn : { "IOReturn n1c_scan_query(", "IOReturn n1c_scan_present(", "IOReturn n1c_scan_status(" }) {
        const std::string b = fn_body(eng, fn);
        expect(!b.empty() && b.find("IOLockLock(gCliLock)") == std::string::npos && b.find("if (!sess_hello()) return kIOReturnNotReady;") != std::string::npos, "Query / Present / Status take only the scanout lock (never gCliLock after it)");
    }
    expect(dcn.find("gCliLock") == std::string::npos, "the DCN layer never names the client lock: nothing there can take it after the scanout lock");
    // -- the interrupt path --
    const std::string ih = fn_body(dcn, "static bool scanIrq(const struct dcn41_irq &irq, uint64_t nowNs, int *ar) {");
    expect(!ih.empty() && ih.find("IOSleep") == std::string::npos && ih.find("IODelay") == std::string::npos && ih.find("scan_restore(") == std::string::npos, "the interrupt handler's scanout half neither sleeps nor restores (it asks the watchdog)");
    expect(ih.find("gScan.wantRestore = true") != std::string::npos && ih.find("storm_note(gScan.storm, nowNs)") != std::string::npos, "the rate guard asks the watchdog for the restore");
    expect(ih.find("dcn41_irq_ack") != std::string::npos && ih.find("dcn41_irq_ack") < ih.find("storm_note"), "the interrupt is acknowledged first, inside the lock");
    const std::string ihe = fn_body(dcn, "bool ihEntry(");
    expect(ihe.find("IOSleep") == std::string::npos, "ihEntry never sleeps");
    expect(ihe.find("__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u || !scanIrq(irq, t, &ar)") != std::string::npos, "ihEntry: the scanout path only when a native client holds the plane; else the old two ack lines");
    expect(ihe.find("__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u &&\n\t    gDcn.dceEntries - gDcn.entriesAtEnable > kDceStormCap") != std::string::npos, "the cumulative cap is BYPASSED while the plane is taken (the rate guard is the guard)");
    expect(dcn.find("kDceStormCap = 200000") != std::string::npos, "the legacy cumulative cap itself is unchanged");
    // no sleeping under the scanout lock anywhere in the scan code
    {
        const size_t a = dcn.find("// ---- build 0.0.603: native scanout (S2a).");
        const std::string scanCode = dcn.substr(a);
        expect(a != std::string::npos && sleeps_only_unlocked(scanCode, "IOLockUnlock(l);", "IOLockLock(l);"), "no IOSleep in the scanout code follows a lock without an unlock");
        { const size_t mtAt = scanCode.find("// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half"); expect(mtAt != std::string::npos, "the mode-trial block follows the scan code"); expect_u("the scan code has no IODelay (0.0.606: the mode-trial block after it has exactly one, in mt_delay_us)", count_of(scanCode.substr(0, mtAt), "IODelay("), 0); }
        const std::string wd = fn_body(dcn, "static void scan_watchdog_loop(uint32_t myGen) {");
        expect(wd.find("IOSleep(50);") != std::string::npos && wd.find("scan_poll_locked(false)") != std::string::npos && wd.find("idle_expired(now_ns(), gScan.lastActivityNs)") != std::string::npos, "the watchdog polls the plane itself and applies the idle deadline (no interrupt needed)");
        expect(wd.find("gScan.gen != myGen") != std::string::npos, "the watchdog ends with its own acquisition");
        expect(wd.find("if (gScan.wantRestore) why = gScan.wantWhy") != std::string::npos && wd.find("if (gScan.wantRestore) why = gScan.wantWhy") < wd.find("idle_expired("), "the watchdog performs the restores the IRQ handler requests (before the idle test)");
    }
    // -- ABI-level fixes of the 0.0.603 review --
    {
        const std::string acq0 = fn_body(dcn, "uint32_t scanAcquire(");
        expect(acq0.find("restore_exact(ex.vmid, ex.tmz, ex.flipType, ex.vpStart)") != std::string::npos && acq0.find("scan_read_exact(&ex)") != std::string::npos, "Acquire refuses unless VMID, TMZ, FLIP_TYPE and PRI_VIEWPORT_START are all 0 (the restore is exact by construction)");
        expect(dcn.find("kOffVmidSettings = 0x0609") != std::string::npos && dcn.find("kOffViewportStart = 0x05e9") != std::string::npos, "the two registers are the review's 0x0609 and 0x05e9");
        expect(fn_body(dcn, "uint32_t scanBoGone(").find("return r[0] != 0ull ? 1u : 2u;") != std::string::npos && fn_body(dcn, "uint32_t scanBoGone(").find("gScan.restoreFailed) rc = 2u") != std::string::npos,
               "scanBoGone reports an unverified restore (now, or earlier this acquisition) as 2: leak");
        const std::string scr = fn_body(dcn, "static uint32_t scan_restore(");
        expect(scr.find("gScan.restoreFailed = !verified;") != std::string::npos, "a restore records whether it verified");
        const std::string wd2 = fn_body(dcn, "static void scan_watchdog_loop(uint32_t myGen) {");
        expect(wd2.find("n48scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc)") != std::string::npos && wd2.find("live plane geometry changed") != std::string::npos && wd2.find("IOSleep(50);") != std::string::npos,
               "the watchdog checks the live geometry every 50 ms and restores on a mismatch (covers hold mode)");
        expect(acq0.find("__atomic_fetch_add(&gScan.watchdogAlive") != std::string::npos && acq0.find("__atomic_fetch_add(&gScan.watchdogAlive") < acq0.find("kernel_thread_start(&scan_watchdog"), "the alive counter goes up BEFORE the thread starts");
        expect(fn_body(dcn, "static void scan_watchdog(void *arg, wait_result_t) {").find("__atomic_fetch_sub(&gScan.watchdogAlive, 1u") > fn_body(dcn, "static void scan_watchdog(void *arg, wait_result_t) {").find("scan_watchdog_loop("), "... and down only after the loop, the thread's last act");
        const std::string sd = fn_body(dcn, "void scanShutdown() {");
        expect(sd.find("scan_restore(\"kext stop\", true, 0u, r)") != std::string::npos && sd.find("watchdogAlive") != std::string::npos && sd.find("IOSleep(1)") != std::string::npos && sd.find("i < 2000u") != std::string::npos, "scanShutdown restores, then waits (bounded, 2 s) for the watchdog threads");
        const std::string stp = fn_body(boot, "void Navi48Bringup::stop(IOService *provider) {");
        expect(stp.find("n48dcn::scanShutdown();") != std::string::npos && stp.find("n48dcn::scanShutdown();") < stp.find("unmapRegisters();") && stp.find("n48dcn::scanShutdown();") < stp.find("amdgpu::bringup_teardown"), "Navi48Bringup::stop runs the scanout shutdown BEFORE the registers are unmapped");
        const size_t bl = dcn.find("uint32_t bind(Navi48Bringup *owner) {");
        const std::string bnd = bl == std::string::npos ? std::string() : dcn.substr(bl, 800);
        expect(bnd.find("IOLockLock(bindLock);") != std::string::npos && bnd.find("IOLockLock(bindLock);") < bnd.find("bind_locked(owner)") && bnd.find("bind_locked(owner)") < bnd.find("IOLockUnlock(bindLock);") && bnd.find("OSCompareAndSwapPtr") != std::string::npos, "bind() is serialised by a lock (lazily created with a CAS)");
        expect_u("bind_locked is called once (from bind)", count_of(dcn, "bind_locked(owner)"), 1);
        expect(tool.find("st.latched >= 1") != std::string::npos && tool.find("haveFinal") != std::string::npos && tool.find("0.95 * vrate") != std::string::npos && tool.find("fc1 >= fc0") != std::string::npos, "the tool: PASS needs latched >= 1 in EVERY mode, a valid final Status, and delivered >= 0.95 x VUPDATE in flip mode");
        expect(tool.find("EARLIEST_INUSE reached the presented buffer") == std::string::npos && tool.find("SURFACE_FLIP_PENDING clearing") != std::string::npos, "the tool's header says what the kernel actually counts");
    }
    // -- Acquire's refusals --
    const std::string acq = fn_body(dcn, "uint32_t scanAcquire(");
    expect(acq.find("native_s1b_state().gate != n48native::kGateOn") != std::string::npos && acq.find("geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc)") != std::string::npos, "Acquire: native boots only, geometry (DCC_EN refused inside geom_check)");
    expect(acq.find("kernel_thread_start(&scan_watchdog") != std::string::npos && acq.find("kernel_thread_start(&scan_watchdog") > acq.find("__atomic_store_n(&gScan.active, 1u"), "Acquire starts the watchdog thread");
    expect(acq.find("gDcn.d.scanout_hi == 0ull") != std::string::npos, "Acquire fails closed without a frame-buffer window");
    expect(acq.find("cur < gDcn.d.scanout_lo || cur >= gDcn.d.scanout_hi") != std::string::npos, "Acquire refuses a console it could not restore (outside the flip window)");
    expect(fn_body(dcn, "uint32_t scanPresent(").find("flip_target_ok(gScan.tbl, mc)") != std::string::npos, "Present checks the exact flip-target set");
    expect(fn_body(dcn, "uint32_t scanPresent(").find("scan_poll_locked(false)") < fn_body(dcn, "uint32_t scanPresent(").find("present_begin("), "Present resolves the previous latch before it counts a replace");
    expect(fn_body(dcn, "uint32_t scanRegister(").find("reg_bounds(") != std::string::npos, "Register runs every bound through reg_bounds");
    // -- the golden copy and the exemption --
    {
        const std::string bind = fn_body(dcn, "static uint32_t bind_locked(Navi48Bringup *owner) {");
        expect(bind.find("gDcn.armed = true;") < bind.find("dcn41_otg_read_timing(&gDcn.d, (uint32_t)gOtg, &gDcn.golden)") && bind.find("native_s1b_state().gate == n48native::kGateOn") != std::string::npos,
               "the dcnmode golden copy is taken at bind, on a native boot, reads only");
    }
    {
        // 0.0.613: the same table, now OR-ed with the display exemptions (amd/native_disp_pure.h native_exempt: false for everything with boot-arg navi48-metal-disp absent)
        const size_t ex = boot.find("if (!n48scan::accel_exempt(action, argScalar) && !n48disp::native_exempt(n48disp_latched_on(), action, argScalar)) {"), rf = boot.find("if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;");
        expect(ex != std::string::npos && rf != std::string::npos && ex < rf, "the exemption is placed BEFORE native_s1b_refuse(0) in accelExperiment");
        expect_u("the refusal line exists once", count_of(boot, "native_s1b_refuse(0)"), 1);
        expect_u("the exemption table is consulted once", count_of(boot, "n48scan::accel_exempt("), 1);
        expect(boot.find("action == 76 && n48_fm_test_n(argScalar) != 0u") > rf, "flip mode's A/B test and dcnflip's pattern stay behind the refusal");
    }
    // -- the client: shapes --
    for (const char *sel : { "N48N_SEL_SCAN_QUERY", "N48N_SEL_SCAN_ACQUIRE", "N48N_SEL_SCAN_REGISTER", "N48N_SEL_SCAN_PRESENT", "N48N_SEL_SCAN_STATUS", "N48N_SEL_SCAN_RELEASE" })
        expect_u(std::string(sel).c_str(), count_of(cli, std::string("case ") + sel + ":"), 1);
    expect(cli.find("if (!shape(0, 0, 0, sizeof(n48n_scan_query)))") != std::string::npos && cli.find("if (!shape(1, 2, 0, 0))") != std::string::npos &&
           cli.find("if (!shape(0, 2, sizeof(n48n_scan_reg), 0))") != std::string::npos && cli.find("if (!shape(2, 3, 0, 0))") != std::string::npos &&
           cli.find("if (!shape(0, 0, 0, sizeof(n48n_scan_status)))") != std::string::npos && cli.find("if (!shape(0, 2, 0, 0))") != std::string::npos, "the six new selectors check their shapes exactly");
    // -- the hello / info / build --
    expect(fn_body(eng, "IOReturn n1c_hello(").find("(flags & ~(uint64_t)N48N_HELLO_F_MINOR) != 0ull") != std::string::npos, "Hello accepts only the minor flag");
    expect(fn_body(eng, "IOReturn n1c_query_info(").find("o->reserved[2] = N48N_ABI_MINOR;") != std::string::npos, "QueryInfo reports the minor");
    {
        const size_t p = engh.find("kN1cKextBuild = ");
        const int build = p == std::string::npos ? -1 : std::atoi(engh.c_str() + p + 16);
        const size_t v = plist.find("<string>0.0.");
        const int ver = v == std::string::npos ? -2 : std::atoi(plist.c_str() + v + 12);
        expect(build > 0 && build == ver, "kN1cKextBuild equals the Info.plist patch version (Hello can no longer print a stale build)");
        expect_u("Info.plist is 0.0.620", (uint64_t)ver, 620);
        expect_u("the plist carries the version twice", count_of(plist, "0.0.620"), 2);
    }
    expect(mk.find("$(wildcard src/dcn/*.cpp)") != std::string::npos, "the Makefile builds src/dcn/*.cpp");
    // -- the tool --
    expect(tool.find("N48N_SEL_SCAN_ACQUIRE") != std::string::npos && tool.find("N48N_SEL_SCAN_RELEASE") != std::string::npos && tool.find("RESTORED") != std::string::npos &&
           tool.find("delivered presents/s") != std::string::npos && tool.find("--hold") != std::string::npos && tool.find("--seconds") != std::string::npos, "the tool acquires, releases, verifies the console and prints delivered presents/s");
    expect(tool.find("N48N_SCANSLOT_REUSABLE") != std::string::npos, "the tool only draws into a slot the kernel calls REUSABLE");
    // -- banned strings --
    const std::string bad1 = std::string("pipe+0x2") + "80", bad2 = std::string("+0x2") + "82", bad3 = std::string("+0x2") + "99";
    for (const std::string *f : { &pure, &abi, &tool }) expect(f->find(bad1) == std::string::npos && f->find(bad2) == std::string::npos && f->find(bad3) == std::string::npos, "no banned pipe offsets in the new files");
    {
        const size_t a = dcn.find("// ---- build 0.0.603: native scanout (S2a).");
        const std::string added = dcn.substr(a);
        expect(added.find(bad1) == std::string::npos && added.find(bad2) == std::string::npos && added.find(bad3) == std::string::npos, "no banned pipe offsets in the new scanout code");
    }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    s1_abi(); s2_bounds(); s3_slots(); s4_run(); s5_frame(); s6_storm(); s7_exempt(); s8_restore(); s9_unpin(); s10_misc(); s11_pins(root);
    std::printf("native_s2a_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
