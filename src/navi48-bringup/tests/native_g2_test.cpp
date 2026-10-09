// native_g2_test.cpp - kext 0.0.627 (GPU-apps stage G2: several native client sessions; an internal design note section 1 and section 4 G2): the pure half
// (amd/native_g2_pure.h) driven directly and through a small model of the kernel's session table and in-order ring, the close-disarm rule (amd/native_disp_pure.h), plus
// source pins that tie the REAL kernel code (amd/native_s1c.cpp, Navi48NativeClient.cpp, Navi48Bringup.cpp, Navi48UserClient.cpp, tools/pc/navi48test.c) to it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g2_test.cpp -o /tmp/native_g2 && /tmp/native_g2 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g2_plant.sh plants breaks in the real code and demands that
//    this suite FAILS on each)
// Covers:
//   T1  the switch and the stat verb's admission (103 only with the latch ON; pages 0..4);
//   T2  OFF identity: one slot (slot 0 = VMID 8), a second open refused, a DEAD slot refused, LAST = the one session's last seqno (= the global count);
//   T3  slot allocation and VMID hand-out with the switch ON (lowest free, 8..11, a fifth open refused, WindowServer only when nothing is open, a G1 test refuses all);
//   T4  VMID release: FREE only after a park AND an idle own wait; a DEAD slot is never handed out again (model: open / close / leak sequences);
//   T5  per-session waits over interleaved seqnos: a session waits only for its OWN last seqno (never for another session's later work); WaitSeq's LAST;
//   T6  guilty-session attribution with interleaved seqnos: the oldest unretired seqno names its owner; unknown when nothing is outstanding, when the log
//       wrapped, or when the entry is stale; the kernel probe is its own owner;
//   T7  the stall decision: recover ONLY a non-WindowServer user session with the switch ON, no MES timeout and no G1 test; everything else latches (as today);
//   T8  poisoning: after a recovery the guilty session's submits and waits are Aborted, QUERY_STATE2 says RESET|GUILTY, the other sessions continue
//       (model run, reachability of BOTH branches);
//   T9  the VRAM budget (25 % for non-WindowServer sessions together; OFF and WindowServer unlimited) and the stat pages' layout;
//   T10 the close-disarm rule: keyed on the closing session being WindowServer's (an admin and a future non-admin app session never disarm);
//   T11 source pins (reachability and order) on the kernel code, the lock map, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "native_g2_pure.h"
#include "native_disp_pure.h"

using namespace n48native::g2;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static size_t count_of(const std::string &s, const std::string &needle) {
    size_t n = 0, p = 0;
    while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
    return n;
}
static bool in_order(const std::string &s, std::initializer_list<const char *> needles) {
    size_t p = 0;
    for (const char *n : needles) { const size_t q = s.find(n, p); if (q == std::string::npos) return false; p = q + std::strlen(n); }
    return true;
}
static std::string fn_body(const std::string &s, const char *head) {
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// ---- a model of the kernel's slot table, the in-order ring and the stall path (the decisions are the pure functions the kernel calls) -----------------
struct SessM { bool open = false, hello = false, ws = false, poisoned = false; uint32_t id = 0; uint64_t last = 0; };
struct Model {
    bool multi;
    uint8_t state[kMaxSessions] = { kSlotFree, kSlotFree, kSlotFree, kSlotFree };
    SessM s[kMaxSessions];
    uint32_t open = 0, idCounter = 0;
    bool g1Holds = false, mesTimedOut = false, hung = false;
    uint64_t attempts = 0;   // 0.0.629 (R3): automatic recoveries tried this boot (gG2.recoveries + gG2.failed)
    uint64_t emitted = 0, retired = 0;
    OwnerLog log {};
    std::vector<std::pair<uint64_t, uint8_t>> ring;   // (seq, owner), in order
    explicit Model(bool m) : multi(m) {}
    // n1c_open: OFF = the compare-and-swap (one open), then open_pick; ON = open_pick under the lock.
    OpenPick open_(bool ws) {
        if (!multi && open != 0u) return OpenPick{ kExclusive, kNoSlot };
        const OpenPick p = open_pick(multi, state, multi ? open : 0u, ws, g1Holds);
        if (p.rc != kOk) return p;
        SessM &x = s[p.slot];
        x = SessM{}; x.open = true; x.hello = true; x.ws = ws; x.id = ++idCounter;
        state[p.slot] = kSlotOpen; open++;
        if (open == 1u) { emitted = 0; retired = 0; ring.clear(); }   // the first session restarts the count (the ring is idle)
        return p;
    }
    // n1c_close: the own wait, then the park, then the slot's next state.
    void close_(uint32_t slot, bool parkOk = true) {
        SessM &x = s[slot];
        const bool ownIdle = !hung && own_idle(retired, x.last);
        const bool parked = ownIdle && parkOk;
        state[slot] = slot_after_close(parked, ownIdle);
        x.open = false; x.hello = false; open--;
    }
    uint64_t submit(uint32_t slot) {   // ring_submit: seqno, owner, the session's last
        if (s[slot].poisoned || hung) return 0;
        const uint64_t seq = ++emitted;
        owner_note(log, seq, (uint8_t)slot, s[slot].id, seq * 15u);
        s[slot].last = seq; ring.push_back({ seq, (uint8_t)slot });
        return seq;
    }
    void retire_to(uint64_t seq) { if (seq > retired && seq <= emitted) retired = seq; }
    // stall_handle: attribution, decision, recovery (MES RESET acked + probe retired = ok), poison or latch.
    uint32_t stall(bool resetAcked = true, bool probeRetires = true) {
        const Guilty g = guilty_of(log, retired, emitted);
        const bool user = g.known && g.slot < kMaxSessions && s[g.slot].open && s[g.slot].id == g.sessId && s[g.slot].hello;
        const bool ws = user && s[g.slot].ws;
        const uint32_t act = stall_action_budgeted(multi, g.known, g.slot, user, ws, mesTimedOut, g1Holds, attempts);
        if (act == kStallRecover) {
            s[g.slot].poisoned = true;                         // before the RESET (g2_recover)
            const uint64_t probe = emitted + 1;
            attempts++;                                        // g2_recover counts every attempt (recoveries or failed)
            if (recovery_ok(resetAcked, true, probeRetires)) {
                emitted = probe; owner_note(log, probe, kOwnerKernel, 0u, 0u); retired = probe;
                return kStallRecover;
            }
        }
        hung = true;
        return kStallLatch;
    }
    uint32_t wait_rc(uint32_t slot, uint64_t target) {   // n1c_wait's verdict once the wait loop ends (0 done, 1 still busy)
        if (s[slot].poisoned) return kAborted;
        uint64_t res = 0;
        if (own_wait_resolve(target, emitted, s[slot].last, &res) != kOk) return kBadArg;
        if (hung) return kAborted;
        return (res == 0 || retired >= res) ? 0u : 1u;
    }
};

// ---- T1 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t1_switch() {
    expect_u("latch absent -> OFF", latch_value(false, 0), kLatchOff);
    expect_u("latch =1 -> ON", latch_value(true, 1), kLatchOn);
    expect_u("latch =2 -> OFF (only 1 turns it on)", latch_value(true, 2), kLatchOff);
    expect(!latch_is_on(kLatchUnset) && !latch_is_on(kLatchOff) && latch_is_on(kLatchOn), "only the ON latch is on");
    expect(!action_admitted(false, kActSessStat) && action_admitted(true, kActSessStat) && !action_admitted(true, 102u) && !action_admitted(true, 104u), "103 exists only with the latch ON, and only 103");
    for (uint64_t a = 0; a <= 4; a++) expect(native_exempt(true, kActSessStat, a), "pages 0..4 are legal");
    expect(!native_exempt(true, kActSessStat, 5) && !native_exempt(false, kActSessStat, 0), "page 5 and the latch OFF are refused");
    expect_u("13 scalars (the accel extra budget)", kOutN, 13);
}

// ---- T2 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t2_off_identity() {
    expect_u("OFF uses one slot", slots_in_use(false), 1);
    Model m(false);
    const OpenPick a = m.open_(true);
    expect(a.rc == kOk && a.slot == 0 && vmid_of_slot(a.slot) == 8u, "OFF: the first open gets slot 0 = VMID 8");
    expect(m.open_(false).rc == kExclusive, "OFF: a second (root) open is refused with ExclusiveAccess, as today");
    expect(m.open_(true).rc == kExclusive, "OFF: a second WindowServer open is refused");
    { uint8_t st[kMaxSessions] = { kSlotFree, kSlotFree, kSlotFree, kSlotFree };
      for (uint32_t open = 1; open < 5; open++) expect(open_pick(false, st, open, false, false).rc == kExclusive, "OFF: any open count refuses (the CAS already guarantees 0)");
      st[0] = kSlotDead; expect(open_pick(false, st, 0, false, false).rc == kExclusive, "OFF: a DEAD slot 0 is never reused");
      st[0] = kSlotFree; expect(open_pick(false, st, 0, false, true).rc == kOk, "OFF: the G1 test's own open is admitted exactly as before (g1Holds is not an OFF input)"); }
    const uint64_t q1 = m.submit(0), q2 = m.submit(0);
    expect(q1 == 1 && q2 == 2 && m.s[0].last == m.emitted, "OFF: the one session's last seqno IS the global count");
    uint64_t r = 0;
    expect(own_wait_resolve(N48N_SEQ_LAST, m.emitted, m.s[0].last, &r) == kOk && r == m.emitted, "OFF: WaitSeq LAST resolves to the global count, as 0.0.626");
    m.close_(0);
    expect(m.state[0] == kSlotFree || m.state[0] == kSlotDead, "the close settles the slot");
    expect(m.state[0] == kSlotDead, "a close with unretired own work leaves the slot DEAD");
    Model n(false); (void)n.open_(false); (void)n.submit(0); n.retire_to(1); n.close_(0);
    expect(n.state[0] == kSlotFree && n.open_(false).rc == kOk, "OFF: after a clean close the next client gets VMID 8 again");
    expect(stall_action(false, true, 1u, true, false, false, false) == kStallLatch, "OFF: a stall always latches HUNG (today's behaviour)");
    expect(vram_budget_ok(false, false, ~0ull - 10, 1ull << 40, 1ull << 30), "OFF: no VRAM budget (0.0.626 had none)");
}

// ---- T3 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t3_slots() {
    expect_u("ON uses four slots", slots_in_use(true), 4);
    for (uint32_t i = 0; i < kMaxSessions; i++) { expect_u("VMID of slot", vmid_of_slot(i), 8u + i); expect(n48native::vmid_ok(vmid_of_slot(i)), "outside the MES mask"); }
    Model m(true);
    const OpenPick w = m.open_(true);
    expect(w.rc == kOk && w.slot == 0, "ON: WindowServer first gets slot 0 / VMID 8");
    const OpenPick a = m.open_(false), b = m.open_(false), c = m.open_(false);
    expect(a.rc == kOk && b.rc == kOk && c.rc == kOk && a.slot == 1 && b.slot == 2 && c.slot == 3, "ON: the extra root sessions get VMIDs 9, 10, 11 in order");
    expect(m.open_(false).rc == kExclusive, "ON: a fifth open is refused (ExclusiveAccess)");
    m.close_(2);
    expect(m.state[2] == kSlotFree && m.open_(false).slot == 2, "ON: a cleanly closed slot is handed out again (lowest free)");
    Model n(true); (void)n.open_(false);
    expect(n.open_(true).rc == kExclusive, "ON: WindowServer opens only when NO session is open (exactly the policy's WsAlreadyOpen, re-checked under the lock)");
    Model g(true); g.g1Holds = true;
    expect(g.open_(false).rc == kExclusive && g.open_(true).rc == kExclusive, "ON: while a G1 test holds the GPU every open is refused");
    Model r(true); const OpenPick r0 = r.open_(false);
    expect(r0.slot == 0 && vmid_of_slot(r0.slot) == 8u, "ON: with no WindowServer the first root session gets VMID 8 ('8 for the first')");
}

// ---- T4 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t4_release() {
    expect(slot_after_close(true, true) == kSlotFree, "parked + idle -> FREE");
    expect(slot_after_close(false, true) == kSlotDead && slot_after_close(true, false) == kSlotDead && slot_after_close(false, false) == kSlotDead, "anything less -> DEAD");
    Model m(true);
    (void)m.open_(true); (void)m.open_(false); (void)m.open_(false);   // slots 0, 1, 2
    (void)m.submit(1); (void)m.submit(2);
    m.retire_to(1);                                                      // slot 1's work retired, slot 2's not
    m.close_(1); m.close_(2);
    expect(m.state[1] == kSlotFree && m.state[2] == kSlotDead, "the slot whose own wait failed is DEAD, the other FREE");
    const OpenPick x = m.open_(false), y = m.open_(false), z = m.open_(false);
    expect(x.rc == kOk && x.slot == 1 && y.rc == kOk && y.slot == 3 && z.rc == kExclusive, "VMID 10 (slot 2, DEAD) is NEVER handed out again; 9 and 11 are");
    for (uint32_t i = 0; i < kMaxSessions; i++) for (uint32_t j = i + 1; j < kMaxSessions; j++)
        if (m.s[i].open && m.s[j].open) expect(vmid_of_slot(i) != vmid_of_slot(j), "no two open sessions share a VMID");
    Model p(true); (void)p.open_(false); (void)p.open_(false); p.close_(1, false);
    expect(p.state[1] == kSlotDead, "a park that did not flush leaves the slot DEAD");
}

// ---- T5 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t5_waits() {
    Model m(true);
    (void)m.open_(true); (void)m.open_(false);                           // WindowServer slot 0, app slot 1
    const uint64_t w1 = m.submit(0), a1 = m.submit(1), w2 = m.submit(0), a2 = m.submit(1), a3 = m.submit(1);   // 1 2 3 4 5 interleaved
    expect(w1 == 1 && a1 == 2 && w2 == 3 && a2 == 4 && a3 == 5, "interleaved seqnos on the one ring");
    m.retire_to(3);
    expect(own_idle(m.retired, m.s[0].last), "WindowServer's own work (up to 3) is retired: its BoFree / unmap / close proceed");
    expect(!own_idle(m.retired, m.emitted), "... although the GPU is NOT idle (a whole-GPU wait would block WindowServer for the app's work)");
    expect(!own_idle(m.retired, m.s[1].last), "the app (last 5) still waits");
    expect(m.wait_rc(0, N48N_SEQ_LAST) == 0u && m.wait_rc(1, N48N_SEQ_LAST) == 1u, "WaitSeq LAST: each session waits for its OWN last seqno");
    uint64_t r = 0;
    expect(own_wait_resolve(N48N_SEQ_LAST, 5, 3, &r) == kOk && r == 3, "LAST = own last (3), not the global 5");
    expect(own_wait_resolve(6, 5, 3, &r) == kBadArg, "a seqno never issued is BadArg");
    expect(own_wait_resolve(4, 5, 3, &r) == kOk && r == 4, "another session's issued seqno is a harmless target");
    expect(own_wait_resolve(0, 5, 3, &r) == kOk && r == 0, "0 = done at once");
    m.retire_to(5);
    expect(own_idle(m.retired, m.s[1].last) && m.wait_rc(1, a2) == 0u, "everything retires in order");
}

// ---- T6 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t6_guilty() {
    OwnerLog l {};
    const uint8_t owners[] = { 0, 1, 0, 2, 1, 0, 3 };
    for (uint64_t q = 1; q <= 7; q++) owner_note(l, q, owners[q - 1], 100u + owners[q - 1], q * 20u);
    for (uint64_t re = 0; re < 7; re++) {
        const Guilty g = guilty_of(l, re, 7);
        expect(g.known && g.seq == re + 1 && g.slot == owners[re] && g.sessId == 100u + owners[re] && g.startWc == (re + 1) * 20u, "the OLDEST unretired seqno (retired + 1) names its owner, its session id and its ring start");
    }
    expect(!guilty_of(l, 7, 7).known, "nothing outstanding: no owner");
    expect(!guilty_of(l, 9, 7).known, "a slot above emitted (clamped elsewhere): no owner");
    OwnerLog w {};
    for (uint64_t q = 1; q <= kOwnerN + 5; q++) owner_note(w, q, (uint8_t)(q % 4), 1u, 0u);
    expect(!guilty_of(w, 2, kOwnerN + 5).known, "more than kOwnerN outstanding (the entry was overwritten): unknown -> latch");
    expect(guilty_of(w, 5, kOwnerN + 5).known && guilty_of(w, 5, kOwnerN + 5).slot == (uint8_t)(6 % 4), "exactly kOwnerN outstanding: still known");
    OwnerLog st {}; owner_note(st, 3, 1, 1, 0); owner_note(st, 3 + kOwnerN, 2, 2, 0);
    expect(!guilty_of(st, 2, 3 + 0).known || guilty_of(st, 2, 3).seq == 3, "seq 3's entry was overwritten by seq 3 + kOwnerN");
    expect(!guilty_of(st, 2, 3).known, "a stale entry (another seqno) never names an owner");
    OwnerLog k {}; owner_note(k, 1, kOwnerKernel, 0, 0);
    const Guilty gk = guilty_of(k, 0, 1);
    expect(gk.known && gk.slot == kOwnerKernel && stall_action(true, gk.known, gk.slot, false, false, false, false) == kStallLatch, "the kernel probe is its own owner and is never 'recovered'");
}

// ---- T7 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t7_decision() {
    expect(stall_action(true, true, 1u, true, false, false, false) == kStallRecover, "ON, a non-WindowServer user session guilty: RECOVER (reachable)");
    expect(stall_action(true, true, 0u, true, false, false, false) == kStallRecover, "... on any slot, VMID 8 included, when it is not WindowServer's");
    expect(stall_action(true, true, 0u, true, true, false, false) == kStallLatch, "WindowServer's session guilty: latch HUNG (today's behaviour)");
    expect(stall_action(false, true, 1u, true, false, false, false) == kStallLatch, "switch OFF: latch");
    expect(stall_action(true, false, 1u, true, false, false, false) == kStallLatch, "owner unknown: latch");
    expect(stall_action(true, true, 1u, false, false, false, false) == kStallLatch, "not a user session (closed, re-opened, no Hello, the G1 test context): latch");
    expect(stall_action(true, true, 1u, true, false, true, false) == kStallLatch, "a MES frame timed out earlier this boot: no further MES frame: latch");
    expect(stall_action(true, true, 1u, true, false, false, true) == kStallLatch, "a G1 test holds the GPU: latch (G1 owns recovery)");
    expect(stall_action(true, true, kMaxSessions, true, false, false, false) == kStallLatch && stall_action(true, true, kOwnerKernel, true, false, false, false) == kStallLatch, "an out-of-range owner: latch");
    expect(recovery_ok(true, true, true) && !recovery_ok(false, true, true) && !recovery_ok(true, false, true) && !recovery_ok(true, true, false), "the recovery counts only with the RESET acked, the probe emitted AND retired");
    expect_u("collateral: later submissions of others behind the guilty one", collateral(5, 9), 4);
    expect_u("collateral: none", collateral(9, 9), 0);
    expect(kVerifyNs == 1000000000ull, "the probe's verify bound is 1 s (G1's)");
}

// ---- T8 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t8_poison() {
    expect(poison_rc(false) == kOk && poison_rc(true) == kAborted, "poisoned -> Aborted");
    expect(ctx_state2(false, false) == 0 && ctx_state2(true, false) == (N48N_CTX_QUERY2_RESET | N48N_CTX_QUERY2_GUILTY) && ctx_state2(false, true) == (N48N_CTX_QUERY2_RESET | N48N_CTX_QUERY2_GUILTY), "QUERY_STATE2: RESET|GUILTY for HUNG or poisoned");
    // a non-WindowServer app hangs between WindowServer submissions
    Model m(true);
    (void)m.open_(true); (void)m.open_(false);
    (void)m.submit(0); const uint64_t bad = m.submit(1); (void)m.submit(0); (void)m.submit(0);
    m.retire_to(bad - 1);                                                // WindowServer's first frame retired, the app's IB never does
    expect(m.stall() == kStallRecover, "the app's stall is RECOVERED (branch reached)");
    expect(m.s[1].poisoned && !m.s[0].poisoned && !m.hung, "only the guilty session is poisoned; no HUNG latch");
    expect(m.submit(1) == 0 && m.wait_rc(1, N48N_SEQ_LAST) == kAborted, "the poisoned session: submits refused, waits Aborted (its fences are failed)");
    expect(m.submit(0) != 0 && m.wait_rc(0, N48N_SEQ_LAST) == 1u, "WindowServer continues: its next submit is accepted");
    m.retire_to(m.emitted);
    expect(m.wait_rc(0, N48N_SEQ_LAST) == 0u, "... and retires");
    m.close_(1);
    expect(m.state[1] == kSlotFree, "the poisoned session's work is behind the probe: its close frees normally (VMID 9 reusable)");
    // WindowServer guilty: today's HUNG
    Model w(true);
    (void)w.open_(true); (void)w.open_(false);
    (void)w.submit(1); const uint64_t wbad = w.submit(0); (void)w.submit(1);
    w.retire_to(wbad - 1);
    expect(w.stall() == kStallLatch && w.hung && !w.s[0].poisoned && !w.s[1].poisoned, "WindowServer guilty: HUNG latched, nobody poisoned (branch reached)");
    expect(w.wait_rc(1, N48N_SEQ_LAST) == kAborted, "under HUNG every wait is Aborted, as today");
    // the recovery failing latches HUNG
    Model f(true); (void)f.open_(false); (void)f.open_(false); (void)f.submit(1); f.retire_to(0);
    expect(f.stall(false, true) == kStallLatch && f.hung, "RESET not acked: HUNG");
    Model g(true); (void)g.open_(false); (void)g.open_(false); (void)g.submit(1);
    expect(g.stall(true, false) == kStallLatch && g.hung, "probe did not retire: HUNG");
}

// ---- T9 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t9_budget_stat() {
    const uint64_t total = 16ull << 30;
    expect_u("the cap is 25 %", others_vram_cap(total), 4ull << 30);
    expect(vram_budget_ok(true, false, 0, 4ull << 30, total), "exactly the cap is allowed");
    expect(!vram_budget_ok(true, false, 0, (4ull << 30) + 4096, total), "one page over the cap is refused");
    expect(!vram_budget_ok(true, false, 3ull << 30, 2ull << 30, total), "the cap is shared by the non-WindowServer sessions together");
    expect(vram_budget_ok(true, true, 4ull << 30, 8ull << 30, total), "WindowServer's own allocations are not limited by it");
    expect(!vram_budget_ok(true, false, ~0ull, 1, total), "no overflow");
    SessView v {}; v.state = kSlotOpen; v.ws = true; v.hello = true; v.poisoned = true; v.vmid = 9; v.id = 7; v.vram = 1; v.gtt = 2; v.imported = 3; v.ptBytes = 4; v.lastSeq = 5; v.nBos = 6; v.faultAtOpen = 0x1234500081ull;
    uint64_t o[kOutN];
    sess_out(1, v, o);
    expect(o[0] == kStatOk && o[1] == 1 && o[2] == (1u | 0x100u | 0x200u | 0x400u) && o[3] == 9 && o[4] == 7 && o[5] == 1 && o[6] == 2 && o[7] == 3 && o[8] == 4 && o[9] == 5 && o[10] == 6 && o[11] == 0x1234500081ull && o[12] == 0, "the session page (0.0.629: o[11] = the fault word at its open)");
    GlobalView g {}; g.open = 2; g.dead = 1; g.hung = true; g.emitted = 10; g.retired = 9; g.othersVram = 11; g.othersCap = 12; g.recoveries = 13; g.lastGuiltySlot = 2; g.lastGuiltySeq = 14; g.collateralTotal = 15; g.faultNow = 16; g.faultPrev = 17;
    global_out(g, o);
    expect(o[1] == kStatGlobal && o[2] == (2u | (1u << 8) | 0x10000u) && o[3] == 10 && o[4] == 9 && o[5] == 11 && o[6] == 12 && o[7] == 13 && o[8] == 2 && o[9] == 14 && o[10] == 15 && o[11] == 16 && o[12] == 17, "the global page (0.0.629: o[11] the fault word now, o[12] at the previous sessstat 4)");
}

// ---- T10 ---------------------------------------------------------------------------------------------------------------------------------------------------
static void t10_close_disarm() {
    using n48disp::ws_close_disarms;
    expect(ws_close_disarms(true, true), "WindowServer's session closing with the latch ON disarms");
    expect(!ws_close_disarms(true, false), "any other session closing (an admin tool, or a future non-admin app) never disarms");
    expect(!ws_close_disarms(false, true) && !ws_close_disarms(false, false), "latch OFF: nothing");
}

// ---- T11: source pins -------------------------------------------------------------------------------------------------------------------------------------------
static void t11_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), hdr = slurp(K + "src/amd/native_s1c.h"), ncl = slurp(K + "src/Navi48NativeClient.cpp"), nclh = slurp(K + "src/Navi48NativeClient.hpp"),
                      brg = slurp(K + "src/Navi48Bringup.cpp"), ucl = slurp(K + "src/Navi48UserClient.cpp"), pure = slurp(K + "src/amd/native_g2_pure.h"), dpure = slurp(K + "src/amd/native_disp_pure.h"),
                      plist = slurp(K + "Info.plist"), cli = slurp(root + "/tools/pc/navi48test.c");
    expect(!eng.empty() && !hdr.empty() && !ncl.empty() && !nclh.empty() && !brg.empty() && !ucl.empty() && !pure.empty() && !dpure.empty() && !plist.empty() && !cli.empty(), "the sources were read");
    // the switch: one latch, read once
    expect(eng.find("PE_parse_boot_argn(\"navi48-multisession\", &v, sizeof(v))") != std::string::npos && count_of(eng, "PE_parse_boot_argn(\"navi48-multisession\"") == 1, "the switch is boot-arg navi48-multisession, parsed in one place");
    expect(eng.find("if (OSCompareAndSwap(n48native::g2::kLatchUnset, n48native::g2::latch_value(present, v), &gMultiLatch))") != std::string::npos, "latched once (first writer wins)");
    // open: OFF = the CAS, ON = open_pick; the VMID of the slot everywhere
    const std::string op = fn_body(eng, "IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {");
    expect(!op.empty(), "n1c_open found");
    expect(in_order(op, { "if (gHang.hung) {", "const bool multi = multi_on();", "if (!multi && !OSCompareAndSwap(0, 1, &gOpenFlag)) {", "IOLockLock(gCliLock);",
                          "const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), appsNow, wsOpen);", "if (pk.rc != kOk) {", "const uint32_t slot = pk.slot, vmid = vmid_of_slot(slot);",
                          "const uint32_t cntl = ctx_prepare(slot, vmid, &ctxOk);", "if (!ctxOk) { rc = kIOReturnNotReady; break; }", "if (program_root(vmid, s->rootPa) != kOk)", "gSlotState[slot] = kSlotOpen;", "IOLockUnlock(gCliLock);" }),
           "n1c_open: HUNG gate, the OFF compare-and-swap, the lock, open_pick, the slot's VMID for the context and the root, the slot marked open");
    expect(op.find("const uint32_t openNow = multi ? __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) : 0u;") != std::string::npos, "OFF feeds open_pick an open count of 0 (the CAS already made it exclusive)");
    { const std::string cp = fn_body(eng, "static __attribute__((noinline)) uint32_t ctx_prepare(uint32_t slot, uint32_t vmid, bool *ok) {");
      expect(in_order(cp, { "const CtxRegs r = ctx_regs(vmid);", "if (slot != 0u && !ctx_geometry_ok(cntl)) {", "WREG32(dev, r.cntl, cntl_native(before));", "cntl = RREG32(dev, r.cntl);", "*ok = ctx_geometry_ok(cntl);" }),
             "CONTEXT9..11 get cntl_native once (never CONTEXT8), then the geometry is checked by read-back before use"); }
    expect_u("the only CNTL write is that one", count_of(eng, "WREG32(dev, r.cntl,"), 1);
    expect(in_order(op, { "if (openNow == 0u) {", "(*cp.rptr_cpu & cp.ring_ptr_mask) == cp.wptr" }) && in_order(op, { "if (openNow == 0u) {", "*seq_slot() = 0ull;", "__atomic_store_n(&gEmitted, 0ull, __ATOMIC_RELEASE);" }),
           "only the FIRST session waits for an idle ring and restarts the seqno");
    expect(count_of(eng, "IOMalloc(sizeof(Session))") == 1 && eng.find("IOFree(ns") == std::string::npos && eng.find("IOFree(s, sizeof(Session))") == std::string::npos, "slots 1..3 are allocated once and never freed");
    expect(eng.find("static Session gSess;") != std::string::npos && eng.find("static Session *gSlot[kMaxSessions] = { &gSess, nullptr, nullptr, nullptr };") != std::string::npos, "slot 0 is the static gSess (VMID 8)");
    // close: the own wait, the park of the session's own VMID, the slot's next state
    const std::string cl = fn_body(eng, "void n1c_close(const N1cRef &ref, const char *how) {");
    expect(in_order(cl, { "Session *s = sess_lock(ref, false);", "bool leak = hung_now();", "if (!leak && !own_wait(s)) leak = true;", "const bool ownIdle = !leak;", "IOLockLock(gCliLock);",
                          "if (program_root(s->vmid, gPark.pa) != kOk) leak = true; else parked = true;", "bo_release(s, h, kRelClosing)", "gSlotState[s->slot] = slot_after_close(parked, ownIdle);",
                          "IOLockUnlock(gCliLock);", "IOLockUnlock(sl);" }), "close: own wait (session lock only), park THIS VMID, free, then the slot state from slot_after_close");
    expect(cl.find("own_wait(s)") < cl.find("IOLockLock(gCliLock);", cl.find("bool leak = hung_now();")), "close: the own wait runs WITHOUT gCliLock");
    expect(in_order(cl, { "if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr);" }), "close: with the switch ON only the scanout owner restores the console (OFF: always, as before)");
    // the own wait: THIS session's last seqno, never the global count
    const std::string ow = fn_body(eng, "static bool own_wait(Session *s, bool *noBlame = nullptr) {");
    expect(!ow.empty() && ow.find("const uint64_t own = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE);") != std::string::npos && ow.find("n48native::g2::own_idle(retired_now(emitted_now()), own)") != std::string::npos,
           "own_wait waits for retired >= the session's OWN last seqno");
    expect(ow.find("IOLockLock") == std::string::npos && ow.find("if (!multi_on()) { hang_from_wait(); return false; }") != std::string::npos, "own_wait takes no lock; OFF is exactly the 0.0.626 idle wait (latch, not idle)");
    expect(eng.find("idle_wait") == std::string::npos, "no whole-GPU idle wait is left");
    { const std::string b = fn_body(eng, "IOReturn n1c_bo_free(");
      expect(b.find("own_wait(s, &noBlame)") != std::string::npos && b.find("own_wait(s, &noBlame)") < b.find("IOLockLock(gCliLock);"), "BoFree waits for its own session only, before gCliLock");
      const std::string g = fn_body(eng, "IOReturn n1c_gem_va("), un = g.substr(g.find("const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);\n            if (hit < 0)"));
      expect(un.find("if (!own_wait(s)) {") != std::string::npos && un.find("IOLockLock(gCliLock)") == std::string::npos && un.find("pt_unmap(") > un.find("own_wait(s)"), "unmap waits for its own session only (no gCliLock in that branch), THEN clears the PTEs"); }
    // the ring: one seqno assignment under the ring lock, the owner recorded before the seqno is published, the session's VMID in the packet
    const std::string rs = fn_body(eng, "static uint32_t ring_submit(");
    expect(in_order(rs, { "IOLockLock(gRingLock);", "const uint64_t seq = emitted_now() + 1ull;", "va, by, nIbs, s->vmid, hasFence,", "ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);", "IOLockUnlock(gRingLock);" }),
           "ring_submit: the seqno, the packet with THIS session's VMID and the owner, under one hold of the ring lock");
    const std::string rp = fn_body(eng, "static uint32_t ring_put_locked(");
    expect(in_order(rp, { "ring[ring_idx(wc, mask)] = dw[i]", "n48native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc);", "if (owner != nullptr) __atomic_store_n(&owner->emitted, seq, __ATOMIC_RELEASE);", "__atomic_store_n(&gEmitted, seq, __ATOMIC_RELEASE);", "WDOORBELL64(" }),
           "ring_put_locked: the owner and the session's last seqno are recorded BEFORE the global seqno is published and the doorbell rung");
    expect(rp.find("IOLockLock(gRingLock)") == std::string::npos && rp.find("hang_from_wait") == std::string::npos, "ring_put_locked neither takes the ring lock (the caller holds it) nor latches (the caller decides, after dropping the lock)");
    expect(eng.find("kNativeVmid, hasFence") == std::string::npos && eng.find("build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), &va, &by, 1u, kNativeVmid") == std::string::npos, "no submit is built with a fixed VMID 8");
    // the stall path
    const std::string sh = fn_body(eng, "static bool stall_handle(uint64_t em, uint64_t re) {");
    expect(in_order(sh, { "const Guilty g = guilty_of(gOwners, re, em);", "__atomic_load_n(&gs->id, __ATOMIC_ACQUIRE) == g.sessId", "__atomic_load_n(&gs->hello, __ATOMIC_ACQUIRE)", "const bool ws = user && __atomic_load_n(&gs->ws, __ATOMIC_ACQUIRE);",
                          "const uint64_t tried = gG2.recoveries + gG2.failed;", "const uint32_t act = stall_action_budgeted(multi_on(), g.known, g.slot, user, ws, mes_timeout_seen(), g1_holds(), tried);", "if (act == kStallRecover && g2_recover(g, gs)) {", "return false;",
                          "const bool det = hang_latch_wait(gHang);", "if (det) hang_announce(em, re);" }), "stall_handle: attribute, decide (the pure stall_action with the real inputs), recover or latch");
    expect_u("stall_handle is reached from hang_poll and stall_from_wait only", count_of(eng, "stall_handle(em, re)"), 2);
    const std::string hp = fn_body(eng, "static bool hang_poll() {");
    expect(in_order(hp, { "if (multi_on()) {", "hang_note(gHang, re, em, now);", "expired = hang_expired(gHang, re, em, now);", "return expired ? stall_handle(em, re) : false;", "const bool det = hang_detect(gHang, re, em, now);" }),
           "hang_poll: ON goes to stall_handle (one owner: gRecovering), OFF is the 0.0.626 hang_detect");
    const std::string rc = fn_body(eng, "static bool g2_recover(");
    expect(in_order(rc, { "IOLockLock(gRingLock);", "g2_scrub_locked(g.startWc);", "__atomic_store_n(&gs->poisoned, 1u, __ATOMIC_SEQ_CST);", "mes_reset_legacy_gfx_queue(dev, gCtx->mes, 0u, 0u, gs->vmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);",
                          "if (r == kIOReturnSuccess) {", "put = ring_put_locked(dw, 8u, probe, kOwnerKernel, 0u, nullptr) == kOk;", "IOLockUnlock(gRingLock);", "const bool ok = recovery_ok(r == kIOReturnSuccess, put, ret);" }),
           "g2_recover: emission frozen, scrub, POISON before the RESET, one MES RESET of the guilty VMID, the probe only after an ack, verdict by recovery_ok");
    expect(rc.find("mes_remove_hw_queue") == std::string::npos && rc.find("cp_map_gfx_kgq_mes") == std::string::npos && rc.find("WREG32") == std::string::npos && rc.find("gHang.hung") == std::string::npos,
           "the automatic recovery sends no REMOVE_QUEUE / ADD_QUEUE (G1 review condition 1: never `auto`), writes no register, never touches the HUNG flag");
    expect_u("the HUNG flag is still cleared in exactly one place (G1's clear_latch)", count_of(eng, "gHang.hung = false"), 1);
    // poison: the gate, the wait, QUERY_STATE2
    const std::string gg = fn_body(eng, "static uint32_t gpu_gate(const Session *s) {");
    expect(in_order(gg, { "if (hung_now()) return hang_rc(true, false);", "if (s != nullptr && sess_poisoned(s)) return n48native::g2::poison_rc(true);", "hang_poll()", "sess_poisoned(s)" }), "gpu_gate: HUNG, then the poison, then the stall poll (and the poison again)");
    expect(fn_body(eng, "IOReturn n1c_submit(").find("const uint32_t gate = gpu_gate(s);") != std::string::npos, "Submit is gated per session");
    const std::string wt = fn_body(eng, "IOReturn n1c_wait(");
    expect(in_order(wt, { "if (sess_poisoned(s)) return kIOReturnAborted;", "n48native::g2::own_wait_resolve(target, em, __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE), &res);", "if (sess_poisoned(s)) return kIOReturnAborted;", "if (res == 0ull || re >= res)" }),
           "WaitSeq: poisoned -> Aborted, LAST = own last, and the poison is checked BEFORE a retired seqno is reported done");
    expect(fn_body(eng, "IOReturn n1c_ctx(").find("n48native::g2::ctx_state2(hung_now(), sess_poisoned(s))") != std::string::npos, "QUERY_STATE2 reports the poison");
    // the budget
    const std::string bc = fn_body(eng, "IOReturn n1c_bo_create(");
    expect(in_order(bc, { "IOReturn rc = (IOReturn)gpu_gate(s);", "IOLockLock(gCliLock);", "if (!n48native::g2::vram_budget_ok(multi, s->ws, gVramOthers, p.size, vram_total())) {", "gCtx->gmc.vram_alloc.alloc(p.size, p.align, &a)", "if (multi && !s->ws) { b.budgeted = 1u; gVramOthers += p.size; }" }),
           "BoCreate: the gate before gCliLock, the budget before the pools, the charge after a successful placement");
    expect(count_of(eng, "budget_release(b)") == 4, "every VRAM free (Closing and Normal, vis and hi) takes the BO off the budget");
    // gCliLock is never held across a stall check
    for (const char *fn : { "IOReturn n1c_bo_create(", "IOReturn n1c_gem_va(", "IOReturn n1c_submit(", "IOReturn n1c_bo_import_host(" }) {
        const std::string b = fn_body(eng, fn);
        const size_t g = b.find("gpu_gate(s)"), l = b.find("IOLockLock(gCliLock);");
        expect(g != std::string::npos && (l == std::string::npos || g < l), "the stall check (which may run a recovery) precedes any gCliLock in this selector");
    }
    // the lock map is written down
    expect(eng.find("// THE LOCK MAP (0.0.627, G2). Order: a session's own lock -> gCliLock -> gRingLock -> gHangLock; gTlbLock is a leaf") != std::string::npos, "the lock map comment");
    expect(in_order(fn_body(eng, "static uint32_t flush_vmid(uint32_t vmid) {"), { "IOLockLock(gTlbLock);", "gmc_flush_gpu_tlb(", "IOLockUnlock(gTlbLock);", "hang_from_wait();" }), "TLB flushes are serialised (gTlbLock is a leaf: the latch runs after it is dropped)");
    // the client: the session reference and the WindowServer flag
    expect(ncl.find("wsSession = d.reason == n48native::policy::kReasonWindowServer;") != std::string::npos && ncl.find("const amdgpu::N1cRef r { sessSlot, sessId };") != std::string::npos, "the client keeps its session reference and the WindowServer flag");
    expect(count_of(ncl, "(r, ") >= 20 && ncl.find("amdgpu::n1c_hello(si[0]") == std::string::npos, "every selector carries the client's own session");
    expect(count_of(ncl, "n48disp_on_ws_client_closed(wsSession)") == 2 && count_of(ncl, "n48disp_on_ws_client_closed(adminClient)") == 0 && count_of(ncl, "n48disp_on_ws_client_closed(!") == 0,
           "the close-disarm hook is keyed on THIS session being WindowServer's, never on the admin flag");
    expect(dpure.find("constexpr bool ws_close_disarms(bool latchOn, bool closingIsWsSession) { return latchOn && closingIsWsSession; }") != std::string::npos, "the rule itself");
    expect(nclh.find("bool           wsSession { false };") != std::string::npos, "wsSession defaults to false (an unknown client never disarms)");
    // the verb (six sites)
    expect(in_order(brg, { "if (n48native::g2::is_g2_action(action)) {", "if (!g5page && !n48native::g2::native_exempt(amdgpu::n1c_multi_latched_on(), action, argScalar)) return kIOReturnBadArgument;", "amdgpu::n1c_g2_stat(argScalar, v);", "if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;" }),
           "accelExperiment: 103 is gated by its latch and sits before the native refusal");
    expect(ucl.find("!n48native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {") != std::string::npos, "the user client: 103 skips the old bound only with the latch");
    expect(hdr.find("IOReturn n1c_g2_stat(uint64_t page, uint64_t *o);") != std::string::npos && hdr.find("bool n1c_multi_latched_on();") != std::string::npos, "declared");
    expect(cli.find("else if (what && !strcmp(what, \"sessstat\")) in = 103;") != std::string::npos && cli.find("in == 103 ? \"sessstat\"") != std::string::npos && cli.find("hangstat sessstat scdcread appallow fbpublish m6stat m6xstat vramstat\\n") != std::string::npos && cli.find("|sessstat [0..5]|scdcread <line 2|3> <off 0..0x5f> [len 1..16]|appallow add") != std::string::npos,
           "navi48test: the verb, its name, the known list and the usage");
    // no single-session leftovers, the stack rule
    expect(eng.find("static Session *gS;") == std::string::npos && eng.find("gS->") == std::string::npos && eng.find("&gS,") == std::string::npos, "no global single-session pointer is left");
    expect(eng.find("Session tmp") == std::string::npos && eng.find("Session copy") == std::string::npos && eng.find("OwnerLog l") == std::string::npos, "no Session / OwnerLog on the stack (static or heap only)");
    // banned strings, the version
    const std::string b1 = std::string("pipe+0x2") + "80", b2 = std::string("+0x2") + "82", b3 = std::string("+0x2") + "99";
    for (const std::string *f : { &eng, &pure, &ncl, &hdr }) expect(f->find(b1) == std::string::npos && f->find(b2) == std::string::npos && f->find(b3) == std::string::npos, "no banned pipe offsets");
    expect(count_of(plist, "<string>0.0.664</string>") == 2 && plist.find("0.0.626") == std::string::npos, "Info.plist is 0.0.664, carried twice");
    expect(hdr.find("constexpr uint32_t kN1cKextBuild = 664;") != std::string::npos, "the native client reports build 660");
}

// ---- T12: the G2 review fixes (0.0.629) -------------------------------------------------------------------------------------------------------------------------
// R1: the open / close race, modelled as the interleavings of the two threads around gCliLock. The record type has its lock in the MIDDLE (like Session), so a
// wipe that writes it is visible. The model's close uses EITHER the 0.0.629 local copy (fixed) or a re-read of the record (0.0.627); the open runs the REAL
// sess_wipe_keep_lock.
struct RaceRec { uint64_t a[3]; void *lock; uint32_t id; bool open; uint64_t z[5]; };
static void t12_r1_race() {
    int realLock = 0;
    // close: C1 = release gCliLock (the slot is FREE from here), C2 = release the session lock. open: O1 = take gCliLock (only after C1), O2 = wipe the record.
    const char *orders[] = { "C1 O1 O2 C2", "C1 O1 C2 O2", "C1 C2 O1 O2" };
    for (const char *ord : orders) {
        for (int useLocal = 0; useLocal < 2; useLocal++) {
            RaceRec r {}; r.lock = &realLock; r.id = 7; r.open = true; r.a[1] = 9; r.z[4] = 3;
            void *const local = r.lock;   // n1c_close: IOLock *const sl = s->lock (right after sess_lock)
            void *unlocked = &realLock; bool sawNull = false;
            for (const char *p = ord; *p; p++) {
                if (p[0] == 'O' && p[1] == '2') { n48native::g2::sess_wipe_keep_lock(&r); if (r.lock == nullptr) sawNull = true; }
                if (p[0] == 'C' && p[1] == '2') unlocked = useLocal ? local : r.lock;
            }
            expect(!sawNull && r.lock == &realLock, "R1: the open's wipe never writes the record's lock field (never NULL, never another lock)");
            expect(unlocked == &realLock, useLocal ? "R1: the close releases exactly the lock it holds, in every interleaving with an open (local copy)"
                                                   : "R1: even a re-read of the record finds the same lock (the wipe keeps it)");
            expect(r.id == 0u && !r.open && r.a[1] == 0u && r.z[4] == 0u, "R1: every other field is wiped");
        }
    }
    // the 0.0.627 wipe (bzero of the whole record, then the lock put back) DOES expose NULL between the two stores: the model shows the window the fix removes
    RaceRec old {}; old.lock = &realLock;
    void *keep = old.lock; std::memset(&old, 0, sizeof(old)); const bool window = old.lock == nullptr; old.lock = keep;
    expect(window, "R1 (model sanity): the old whole-record wipe had a NULL-lock window (the reachable bad state)");
}
// R2: the wrong-session blame. The REAL hang clock (s1c::hang_kick / hang_note / hang_expired) and bound_verdict, as stall_from_wait uses them.
static uint32_t bound_check(n48native::s1c::Hang &h, bool busy, uint64_t re, uint64_t em, uint64_t now) {   // the kernel's stall_from_wait, switch ON
    bool mine = false;
    if (!busy) { n48native::s1c::hang_note(h, re, em, now); mine = n48native::s1c::hang_expired(h, re, em, now); }
    return bound_verdict(busy, mine);
}
static void t12_r2_blame() {
    using namespace n48native::s1c;
    const uint64_t ms = 1000000ull;
    // VMID 8 (slot 0) runs a 1.9 s job; VMID 9 (slot 1) submits 50 ms later, behind it, and waits for it with own_wait.
    Model m(true); (void)m.open_(false); (void)m.open_(false);
    Hang h {};
    hang_kick(h, 0, 0, 0);               const uint64_t s8 = m.submit(0);   // the ring was idle: the clock starts at this kick
    hang_kick(h, 0, s8, 50 * ms);        const uint64_t s9 = m.submit(1);   // not idle: no restart
    m.retire_to(s8);                                                        // at 1.9 s; nobody looked
    const uint64_t at = 50 * ms + kWaitCapNs;                               // VMID 9's own wait reaches its 2 s at 2.05 s
    const Guilty g = guilty_of(m.log, m.retired, m.emitted);
    expect(g.known && g.slot == 1u && g.seq == s9 && stall_action_budgeted(true, g.known, g.slot, true, false, false, false, 0) == kStallRecover,
           "R2 (model sanity): the oldest unretired seqno is VMID 9's, so a stall at this moment WOULD reset and poison VMID 9 (the 0.0.627 behaviour)");
    expect_u("R2: a 1.9 s job on VMID 8 then VMID 9: VMID 9's 2 s bound blames NOBODY (Timeout to the caller)", bound_check(h, false, m.retired, m.emitted, at), kBoundNoBlame);
    expect(!m.s[1].poisoned && !m.hung, "R2: VMID 9 is not poisoned, nothing latched");
    Hang h2 {}; hang_kick(h2, 0, 0, 0); hang_note(h2, 1, 2, 1950 * ms);   // somebody polled just after VMID 8 retired
    expect_u("R2: ... also when a poll saw VMID 8 retire at 1.95 s", bound_check(h2, false, 1, 2, at), kBoundNoBlame);
    // VMID 9's job then really hangs: no progress for kHangNs after the last change -> the next bound blames it (both branches reachable)
    expect_u("R2: no progress for the hang threshold: the stall path runs (VMID 9 blamed)", bound_check(h, false, m.retired, m.emitted, at + kHangNs), kBoundStalled);
    { Hang hx {}; hang_kick(hx, 0, 0, 0); hang_note(hx, 1, 2, at);
      expect_u("R2: ... and not a nanosecond earlier", bound_check(hx, false, 1, 2, at + kHangNs - 1), kBoundNoBlame); }
    Hang h3 {}; hang_kick(h3, 0, 0, 0);
    expect_u("R2: a lone hang on an idle ring is still caught at its 2 s (the G2 hang leg is unchanged)", bound_check(h3, false, 0, 1, kWaitCapNs), kBoundStalled);
    expect_u("R2: HUNG latched / a recovery running: busy (the waiter re-checks)", bound_check(h3, true, 0, 1, 10 * kWaitCapNs), kBoundBusy);
    expect(kHangNs == kWaitCapNs, "R2: the hang threshold is the same 2 s (only its starting point changed)");
}
// R3: at most three automatic recoveries per boot.
static void t12_r3_budget() {
    expect(recovery_budget_left(0) && recovery_budget_left(2) && !recovery_budget_left(3) && !recovery_budget_left(100), "R3: three attempts, no fourth");
    expect(stall_action_budgeted(true, true, 1u, true, false, false, false, 2) == kStallRecover && stall_action_budgeted(true, true, 1u, true, false, false, false, 3) == kStallLatch,
           "R3: the budgeted decision recovers the third and latches the fourth");
    expect(stall_action_budgeted(true, true, 1u, true, true, false, false, 0) == kStallLatch && stall_action_budgeted(false, true, 1u, true, false, false, false, 0) == kStallLatch,
           "R3: the budget never turns a latch into a recovery");
    Model m(true); (void)m.open_(true);
    uint32_t recovered = 0;
    for (int k = 0; k < 4; k++) {
        const OpenPick a = m.open_(false);
        expect(a.rc == kOk, "R3 model: an app session opens");
        const uint64_t bad = m.submit(a.slot);
        m.retire_to(bad - 1);
        if (m.stall() == kStallRecover) recovered++;
        if (!m.hung) m.close_(a.slot);
        if (m.hung) break;
    }
    expect(recovered == 3 && m.hung && m.attempts == 3, "R3 model: four app hangs in a row: three are recovered, the fourth latches HUNG");
}
// R4: the global system-memory cap.
static void t12_r4_syscap() {
    const uint64_t G = 1ull << 30;
    expect(kSysGlobalCap == 6 * G, "R4: the global cap is 6 GiB");
    expect(sys_budget_ok(true, false, 0, 6 * G) && !sys_budget_ok(true, false, 0, 6 * G + 4096), "R4: exactly the cap, not a page more");
    expect(sys_budget_ok(true, true, 6 * G, 4 * G), "R4: WindowServer's session is never refused by it (its per-session caps apply)");
    expect(sys_budget_ok(false, false, 100 * G, 100 * G), "R4: switch OFF: no global cap");
    expect(!sys_budget_ok(true, false, ~0ull, 1), "R4: no overflow");
    // WindowServer 4.5 GiB (its own caps: 512 MiB GTT + 4 GiB imports), app A 1 GiB, app B asks 0.6 GiB -> refused; after A frees, B fits
    uint64_t held = 0; held += (G / 2) + 4 * G; 
    expect(sys_budget_ok(true, false, held, G), "R4 model: app A's 1 GiB fits next to WindowServer's 4.5 GiB"); held += G;
    expect(!sys_budget_ok(true, false, held, 6 * G / 10), "R4 model: app B's 0.6 GiB would pass 6 GiB in total: refused");
    held -= G;
    expect(sys_budget_ok(true, false, held, 6 * G / 10), "R4 model: after A's memory is really freed, B fits");
}
// R5: the fault word (read only) and its change test.
static void t12_r5_fault() {
    const uint64_t boot = fault_word(0x00881071u, 0x00012345u);   // VMID 8's self-test fault, still latched
    expect(fault_word_vmid(boot) == 8u && fault_word_vmid(fault_word(0x00981071u, 0)) == 9u, "R5: the word carries STATUS_LO32.VMID (bits 23:20)");
    expect(fault_word_vmid(boot) == n48native::fault_status_vmid(0x00881071u), "R5: the same VMID field as native_s1b_pure.h's fault_status_vmid");
    expect((boot & 0xFFFFFFFFull) == 0x00881071u && (boot >> 32) == 0x00012345u, "R5: STATUS_LO32 low, ADDR_LO32 high");
    expect(!fault_changed(boot, boot) && fault_changed(boot, fault_word(0x00981071u, 0x00020000u)), "R5: only a CHANGE tells a new fault from the boot-time one");
}
// R6: BoFree of a CPU-mapped visible-VRAM BO.
static void t12_r6_mapped() {
    expect(kBusy == 0xe00002d5u && kBusy != n48native::s1c::kTimeout && kBusy != kAborted && kBusy != n48native::s1c::kNotFound && kBusy != n48native::s1c::kNotReady, "R6: a distinct status (kIOReturnBusy)");
    expect(bofree_mapping_rc(true, true, true, 2) == kBusy && bofree_mapping_rc(true, true, true, 5) == kBusy, "R6: switch ON, visible VRAM, a mapping holds the descriptor: refused");
    expect(bofree_mapping_rc(true, true, true, 1) == kOk && bofree_mapping_rc(true, true, false, 0) == kOk, "R6: only the record's own reference (or never mapped): freed");
    expect(bofree_mapping_rc(false, true, true, 2) == kOk, "R6: switch OFF: as 0.0.626");
    expect(bofree_mapping_rc(true, false, true, 2) == kOk, "R6: GTT / host BOs are not refused (their pages belong to the descriptor the mapping keeps)");
}
static void t12_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), cli = slurp(root + "/tools/pc/navi48test.c");
    // R1
    const std::string cl = fn_body(eng, "void n1c_close(const N1cRef &ref, const char *how) {");
    const size_t lastCli = cl.rfind("IOLockUnlock(gCliLock);");
    expect(!cl.empty() && lastCli != std::string::npos && in_order(cl, { "Session *s = sess_lock(ref, false);", "if (s == nullptr) return;", "IOLock *const sl = s->lock;" }), "R1: close keeps the lock it holds in a local right after sess_lock");
    const std::string tail = lastCli != std::string::npos ? cl.substr(lastCli + std::strlen("IOLockUnlock(gCliLock);")) : std::string("s->");
    expect(tail.find("IOLockUnlock(sl);") != std::string::npos && tail.find("s->") == std::string::npos && tail.find("sess_unlock(") == std::string::npos,
           "R1: after close's LAST gCliLock release nothing reads the Session; the lock is released through the local");
    const std::string op = fn_body(eng, "IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {");
    expect(op.find("bzero(s, sizeof(Session))") == std::string::npos && eng.find("bzero(s, sizeof(Session))") == std::string::npos, "R1: no whole-record wipe of a live slot's Session");
    expect(in_order(op, { "if (s->lock == nullptr) {", "IOLock *nl = IOLockAlloc();", "__atomic_store_n(&s->lock, nl, __ATOMIC_RELEASE);", "n48native::g2::sess_wipe_keep_lock(s);", "s->slot = slot;" }),
           "R1: open creates the lock once per slot, then wipes the record WITHOUT it");
    expect_u("R1: the lock field is stored in exactly one place (its creation)", count_of(eng, "__atomic_store_n(&s->lock, "), 1);
    expect(eng.find("s->lock = ") == std::string::npos, "R1: ... and never by a plain assignment");
    // R2
    const std::string sf = fn_body(eng, "static uint32_t stall_from_wait() {");
    expect(in_order(sf, { "if (!multi_on()) { hang_from_wait(); return n48native::g2::kBoundStalled; }", "const bool busy = gHang.hung || __atomic_load_n(&gRecovering, __ATOMIC_SEQ_CST) != 0u;", "if (!busy) {",
                          "hang_note(gHang, re, em, now);", "mine = hang_expired(gHang, re, em, now);", "if (mine) __atomic_store_n(&gRecovering, 1u, __ATOMIC_SEQ_CST);", "IOLockUnlock(gHangLock);",
                          "if (mine) (void)stall_handle(em, re);", "return n48native::g2::bound_verdict(busy, mine);" }),
           "R2: a ring wait's bound blames only when the hang clock (last progress) expired; otherwise no blame");
    expect_u("R2: gRecovering is claimed by stall_from_wait only under `mine`", count_of(sf, "__atomic_store_n(&gRecovering, 1u"), 1);
    const std::string ow = fn_body(eng, "static bool own_wait(Session *s, bool *noBlame = nullptr) {");
    expect(in_order(ow, { "if (!multi_on()) { hang_from_wait(); return false; }", "if (stall_from_wait() == n48native::g2::kBoundNoBlame) { if (noBlame != nullptr) *noBlame = true; return false; }", "stalled = true; continue;" }),
           "R2: own_wait returns Timeout (not idle, no blame) when the bound blamed nobody");
    expect(fn_body(eng, "static uint32_t ring_submit(").find("if (er == kTimeout) { (void)stall_from_wait(); return kTimeout; }") != std::string::npos, "R2: the ring-space wait goes through the same check");
    const std::string bf = fn_body(eng, "IOReturn n1c_bo_free(const N1cRef &ref, uint64_t handle) {");
    expect(in_order(bf, { "else if (!own_wait(s, &noBlame)) { m = kRelLeak; rc = kIOReturnTimeout; }", "if (noBlame) {", "} else {", "IOLockLock(gCliLock);", "bo_release(s, h, m);" }),
           "R2: BoFree after a no-blame Timeout keeps the BO (no release, no leak)");
    // R3
    const std::string sh = fn_body(eng, "static bool stall_handle(uint64_t em, uint64_t re) {");
    expect(in_order(sh, { "const uint64_t tried = gG2.recoveries + gG2.failed;", "stall_action_budgeted(multi_on(), g.known, g.slot, user, ws, mes_timeout_seen(), g1_holds(), tried);", "if (act == kStallRecover && g2_recover(g, gs)) {" }),
           "R3: stall_handle feeds the attempts of this boot to the budgeted decision");
    expect(sh.find("stall_action(multi_on()") == std::string::npos, "R3: no unbudgeted decision is left");
    expect(in_order(fn_body(eng, "static bool g2_recover("), { "const bool ok = recovery_ok(r == kIOReturnSuccess, put, ret);", "gG2.recoveries++;", "else gG2.failed++;" }), "R3: every attempt is counted");
    // R4
    const std::string bc = fn_body(eng, "IOReturn n1c_bo_create(");
    expect(in_order(bc, { "would_exceed(s->gttUsed, p.size, kGttCap)", "if (!n48native::g2::sys_budget_ok(multi, s->ws, gSysHeld, p.size)) {", "rc = kIOReturnNoMemory; break;", "sysmem_alloc(sm, p.size, p.align)", "if (multi) { b.sysCounted = 1u; gSysHeld += p.size; }" }),
           "R4: BoCreate (GTT): the global cap before the allocation, the charge after it");
    const std::string im = fn_body(eng, "IOReturn n1c_bo_import_host(");
    expect(in_order(im, { "if (s != nullptr) IOLockLock(gCliLock);", "const ImportChk c = import_check(", "if (!n48native::g2::sys_budget_ok(multi_on(), s->ws, gSysHeld, size)) {", "rc = kIOReturnNoMemory; break;", "pt_map_pages(", "if (multi_on()) { b.sysCounted = 1u; gSysHeld += size; }" }),
           "R4: import: the global cap under gCliLock before anything is mapped, the charge with the record");
    const std::string rl = fn_body(eng, "static void bo_release(Session *s, uint32_t h, RelMode mode) {");
    const size_t lk = rl.find("} else {\n        // Leak:");
    expect_u("R4: the total drops in exactly the four places memory is really freed", count_of(rl, "sys_release(b)"), 4);
    expect(lk != std::string::npos && rl.substr(lk).find("sys_release") == std::string::npos, "R4: a leak keeps its bytes counted");
    // R5
    const std::string fs = fn_body(eng, "static __attribute__((noinline)) uint64_t fault_snap(const char *when, uint32_t slot) {");
    expect(in_order(fs, { "hub_rd(0x15d0)", "hub_rd(0x15d1)", "hub_rd(0x15d2)", "hub_rd(0x15d3)", "return n48native::g2::fault_word(sLo, aLo);" }), "R5: the four GCVM L2 protection-fault registers the S1b fault leg reads");
    expect(fs.find("WREG") == std::string::npos && fs.find("wr(") == std::string::npos && eng.find("vm_l2_protection_fault_cntl") == std::string::npos && eng.find("faultclear") == std::string::npos,
           "R5: read only: nothing here writes or clears a fault register");
    expect(in_order(op, { "n48native::g2::sess_wipe_keep_lock(s);", "s->faultAtOpen = fault_snap(\"open\", slot);", "gSlotState[slot] = kSlotOpen;" }), "R5: the word is recorded at each session's open");
    const std::string st = fn_body(eng, "IOReturn n1c_g2_stat(uint64_t page, uint64_t *o) {");
    expect(in_order(st, { "g.faultNow = fault_snap(\"sessstat\", kStatGlobal);", "g.faultPrev = gFaultPrev; gFaultPrev = g.faultNow;", "global_out(g, o);", "v.faultAtOpen = s->faultAtOpen;", "sess_out(slot, v, o);" }),
           "R5: sessstat 4 reports the word now and at the previous call; a session page its word at open");
    expect(cli.find("gcvm fault now / prev") != std::string::npos && cli.find("gcvm fault at open") != std::string::npos, "R5: navi48test prints them");
    // R6
    expect(in_order(bf, { "const uint32_t mrc = hung_now() ? kOk : n48native::g2::bofree_mapping_rc(multi_on(), b0.kind == kBoVis, b0.vmd != nullptr, b0.vmd != nullptr ? (uint32_t)b0.vmd->getRetainCount() : 0u);",
                          "if (mrc != kOk) {", "rc = (IOReturn)mrc;", "} else {", "own_wait(s, &noBlame)", "bo_release(s, h, m);" }),
           "R6: BoFree refuses a CPU-mapped visible-VRAM BO (switch ON) BEFORE any wait or release");
    { const size_t a = bf.find("if (mrc != kOk) {"), b = bf.find("} else {", a);
      expect(a != std::string::npos && b != std::string::npos && bf.substr(a, b - a).find("bo_release") == std::string::npos && bf.substr(a, b - a).find("own_wait") == std::string::npos, "R6: the refusal touches nothing"); }
    expect(eng.find("static_assert(n48native::g2::kBusy == (uint32_t)kIOReturnBusy") != std::string::npos, "R6: kBusy is kIOReturnBusy");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    t1_switch(); t2_off_identity(); t3_slots(); t4_release(); t5_waits(); t6_guilty(); t7_decision(); t8_poison(); t9_budget_stat(); t10_close_disarm(); t11_pins(root);
    t12_r1_race(); t12_r2_blame(); t12_r3_budget(); t12_r4_syscap(); t12_r5_fault(); t12_r6_mapped(); t12_pins(root);
    std::printf("native_g2: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
