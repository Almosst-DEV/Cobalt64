// native_g2_test.cpp - kext 0.0.627 (GPU-apps stage G2: several native client sessions; an internal design note section 1 and section 4 G2): the pure half
// (amd/native_g2_pure.h) driven directly and through a small model of the kernel's session table and in-order ring, the close-disarm rule (amd/native_disp_pure.h), plus
// source pins that tie the REAL kernel code (amd/native_s1c.cpp, Navi48NativeClient.cpp, Navi48Bringup.cpp, Navi48UserClient.cpp, tools/pc/navi48test.c) to it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g2_test.cpp -o /tmp/native_g2 && /tmp/native_g2 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g2_plant.sh plants breaks in the real code and demands that
//    this suite FAILS on each)
// Covers:
//   T1  the switch and the stat verb's admission (103 only with the latch ON; pages 0..4);
//   T2  OFF identity: one record (record 0), VMID 8, a second open refused, LAST = the one session's last seqno (= the global count);
//   T3  record allocation and VMID-pool hand-out with the switch ON (lowest free record, handles 0..7 = VMIDs 8..15, WindowServer only when nothing is open, a G1
//       test refuses all) and the 0.0.669 property that the NINTH client still opens (non-resident) - the client count is not the VMID count;
//   T4  VMID release: FREE only after a park AND an idle own wait; anything less QUARANTINES the record and holds its handle, and the RECLAIM pass returns both
//       the moment own_idle proves the work retired (the resources are not monotonically consumed);
//   T5  per-session waits over interleaved seqnos: a session waits only for its OWN last seqno (never for another session's later work); WaitSeq's LAST;
//   T6  guilty-session attribution with interleaved seqnos: the oldest unretired seqno names its owner; unknown when nothing is outstanding, when the log
//       wrapped, or when the entry is stale; the kernel probe is its own owner;
//   T7  the stall decision: recover ONLY a non-WindowServer user session with the switch ON, no MES timeout and no G1 test; everything else latches (as today);
//   T8  poisoning: after a recovery the guilty session's submits and waits are Aborted, QUERY_STATE2 says RESET|GUILTY, the other sessions continue
//       (model run, reachability of BOTH branches);
//   T9  the VRAM budget (25 % for non-WindowServer sessions together; OFF and WindowServer unlimited) and the stat pages' layout;
//   T10 the close-disarm rule: keyed on the closing session being WindowServer's (an admin and a future non-admin app session never disarm);
//   T11 source pins (reachability and order) on the kernel code, the lock map, the version;
//   T13 the VMID pool itself: vmid_pick (a free handle, else one STOLEN from an idle client, else NotReady), reclaim_pick, and the accounting that shows the pool
//       never shrinks (0.0.669).
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

// ---- a model of the kernel's CLIENT TABLE, its VMID POOL, the in-order ring and the stall path (the decisions are the pure functions the kernel calls) -----
// 0.0.669: `state` is the CLIENT TABLE (kMaxClients records) and `owner` is the VMID POOL (kVmidPoolSize handles). A record's `vmid` is DERIVED from owner[],
// exactly as the kernel derives it, so the model cannot accidentally keep the two in step by construction.
struct SessM { bool open = false, hello = false, ws = false, poisoned = false; uint32_t id = 0; uint64_t last = 0; };
struct Model {
    bool multi;
    uint8_t state[kMaxClients];
    SessM s[kMaxClients];
    uint32_t owner[kVmidPoolSize];
    const uint32_t usable = (1u << kVmidPoolSize) - 1u;   // every handle carries the geometry (the kernel's gHandleUsable)
    uint32_t open = 0, idCounter = 0;
    bool g1Holds = false, mesTimedOut = false, hung = false;
    uint64_t attempts = 0;   // 0.0.629 (R3): automatic recoveries tried this boot (gG2.recoveries + gG2.failed)
    uint64_t emitted = 0, retired = 0;
    OwnerLog log {};
    std::vector<std::pair<uint64_t, uint8_t>> ring;   // (seq, owner), in order
    explicit Model(bool m) : multi(m) {
        for (uint32_t i = 0; i < kMaxClients; i++) state[i] = kCliFree;
        for (uint32_t h = 0; h < kVmidPoolSize; h++) owner[h] = kNoClient;
    }
    // ---- the pool (native_s1c.cpp's vmid_assign_locked / vmid_release_locked / vmid_need, driven by the pure vmid_pick) ----
    uint32_t vmid_of(uint32_t cli) const { for (uint32_t h = 0; h < kVmidPoolSize; h++) if (owner[h] == cli) return vmid_of_handle(h); return 0u; }
    uint32_t held() const { uint32_t n = 0; for (uint32_t h = 0; h < kVmidPoolSize; h++) if (owner[h] != kNoClient) n++; return n; }
    uint32_t owner_free() const { return kVmidPoolSize - held(); }
    // Assign a handle: the lowest FREE one, else STEAL one whose client's own work is retired. 0 = NotReady (the client stays non-resident).
    uint32_t take(uint32_t cli) {
        bool idle[kVmidPoolSize];
        for (uint32_t h = 0; h < kVmidPoolSize; h++) idle[h] = owner[h] != kNoClient && s[owner[h]].open && own_idle(retired, s[owner[h]].last);
        const VmidPick p = vmid_pick(owner, idle, usable);
        if (p.rc != kOk) return 0u;
        owner[p.handle] = cli;
        return vmid_of_handle(p.handle);
    }
    void release(uint32_t cli) { for (uint32_t h = 0; h < kVmidPoolSize; h++) if (owner[h] == cli) owner[h] = kNoClient; }
    // THE RECLAIM PASS: a QUARANTINED record whose work has now retired gives its handle AND its table entry back.
    uint32_t reclaim() {
        bool idle[kMaxClients];
        for (uint32_t i = 0; i < kMaxClients; i++) idle[i] = state[i] == kCliQuarantined && own_idle(retired, s[i].last);
        uint32_t n = 0;
        for (;;) { const uint32_t i = reclaim_pick(state, idle, kMaxClients); if (i == kNoSlot) break; release(i); state[i] = kCliFree; idle[i] = false; n++; }
        return n;
    }
    // n1c_open: OFF = the compare-and-swap (one open), then the reclaim pass + open_pick; then the pool handle (or none at all).
    CliPick open_(bool ws) {
        if (!multi && open != 0u) return CliPick{ kExclusive, kNoSlot };
        reclaim();
        const CliPick p = open_pick(multi, state, kMaxClients, multi ? open : 0u, ws, g1Holds);
        if (p.rc != kOk) return p;
        SessM &x = s[p.cli];
        x = SessM{}; x.open = true; x.hello = true; x.ws = ws; x.id = ++idCounter;
        state[p.cli] = kCliOpen; open++;
        if (open == 1u) { emitted = 0; retired = 0; ring.clear(); }   // the first session restarts the count (the ring is idle)
        (void)take(p.cli);   // the open TRIES for a handle; finding none leaves the client non-resident (it is not a refusal)
        return p;
    }
    // n1c_close: the own wait, then the park, then the record's next state. A quarantine KEEPS the handle (the GPU may still read that tree).
    void close_(uint32_t cli, bool parkOk = true) {
        SessM &x = s[cli];
        const bool ownIdle = !hung && own_idle(retired, x.last);
        const bool parked = ownIdle && parkOk;
        if (parked) release(cli);
        state[cli] = cli_after_close(parked, ownIdle);
        x.open = false; x.hello = false; open--;
    }
    uint64_t submit(uint32_t cli) {   // n1c_submit: the residency first (vmid_need), then ring_submit
        if (s[cli].poisoned || hung) return 0;
        if (vmid_of(cli) == 0u && take(cli) == 0u) return 0;   // no handle: NotReady, nothing on the ring
        const uint64_t seq = ++emitted;
        owner_note(log, seq, (uint8_t)cli, s[cli].id, seq * 15u);
        s[cli].last = seq; ring.push_back({ seq, (uint8_t)cli });
        return seq;
    }
    void retire_to(uint64_t seq) { if (seq > retired && seq <= emitted) retired = seq; }
    // stall_handle: attribution, decision, recovery (MES RESET acked + probe retired = ok), poison or latch.
    uint32_t stall(bool resetAcked = true, bool probeRetires = true) {
        const Guilty g = guilty_of(log, retired, emitted);
        const bool user = g.known && g.slot < kMaxClients && s[g.slot].open && s[g.slot].id == g.sessId && s[g.slot].hello;
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
    uint32_t wait_rc(uint32_t cli, uint64_t target) {   // n1c_wait's verdict once the wait loop ends (0 done, 1 still busy)
        if (s[cli].poisoned) return kAborted;
        uint64_t res = 0;
        if (own_wait_resolve(target, emitted, s[cli].last, &res) != kOk) return kBadArg;
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
    for (uint64_t a = 0; a <= (uint64_t)kStatClients; a++) expect(native_exempt(true, kActSessStat, a), "every page up to kStatClients is legal");
    expect(!native_exempt(true, kActSessStat, kStatClients + 1u) && native_exempt(true, kActSessStat, kStatClients) && native_exempt(true, kActSessStat, kStatGlobal) && !native_exempt(false, kActSessStat, 0),
           "0.0.670: kStatClients (9) is the last legal page - the global page (8) and the pool pages (0..7) keep their numbers - and the latch OFF is refused");
    expect_u("13 scalars (the accel extra budget)", kOutN, 13);
}

// ---- T2 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t2_off_identity() {
    expect_u("the VMID pool is the hardware's 8", kVmidPoolSize, 8u);
    expect_u("the client table is a memory bound, not a GPU one", kMaxClients, 64u);
    expect(kMaxClients > kVmidPoolSize, "0.0.670: there are more clients than VMIDs (that is the whole point)");
    Model m(false);
    const CliPick a = m.open_(true);
    expect(a.rc == kOk && a.cli == 0 && m.vmid_of(a.cli) == 8u, "OFF: the first open gets record 0 and pool handle 0 = VMID 8");
    expect(m.open_(false).rc == kExclusive, "OFF: a second (root) open is refused with ExclusiveAccess, as today");
    expect(m.open_(true).rc == kExclusive, "OFF: a second WindowServer open is refused");
    { uint8_t st[kMaxClients]; for (uint32_t i = 0; i < kMaxClients; i++) st[i] = kCliFree;
      for (uint32_t open = 1; open < 5; open++) expect(open_pick(false, st, kMaxClients, open, false, false).rc == kExclusive, "OFF: any open count refuses (the CAS already guarantees 0)");
      st[0] = kCliQuarantined; expect(open_pick(false, st, kMaxClients, 0, false, false).rc == kExclusive, "OFF: a QUARANTINED record 0 is not reused (the reclaim pass runs first)");
      st[0] = kCliFree; expect(open_pick(false, st, kMaxClients, 0, false, true).rc == kOk, "OFF: the G1 test's own open is admitted exactly as before (g1Holds is not an OFF input)"); }
    const uint64_t q1 = m.submit(0), q2 = m.submit(0);
    expect(q1 == 1 && q2 == 2 && m.s[0].last == m.emitted, "OFF: the one session's last seqno IS the global count");
    uint64_t r = 0;
    expect(own_wait_resolve(N48N_SEQ_LAST, m.emitted, m.s[0].last, &r) == kOk && r == m.emitted, "OFF: WaitSeq LAST resolves to the global count, as 0.0.626");
    m.close_(0);
    expect(m.state[0] == kCliFree || m.state[0] == kCliQuarantined, "the close settles the record");
    expect(m.state[0] == kCliQuarantined, "a close with unretired own work QUARANTINES the record (and holds its handle)");
    Model n(false); (void)n.open_(false); (void)n.submit(0); n.retire_to(1); n.close_(0);
    expect(n.state[0] == kCliFree && n.open_(false).rc == kOk, "OFF: after a clean close the next client gets VMID 8 again");
    expect(stall_action(false, true, 1u, true, false, false, false) == kStallLatch, "OFF: a stall always latches HUNG (today's behaviour)");
    expect(vram_budget_ok(false, false, ~0ull - 10, 1ull << 40, 1ull << 30), "OFF: no VRAM budget (0.0.626 had none)");
}

// ---- T3 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t3_slots() {
    for (uint32_t h = 0; h < kVmidPoolSize; h++) { expect_u("VMID of pool handle", vmid_of_handle(h), 8u + h); expect(n48native::vmid_ok(vmid_of_handle(h)), "outside the MES mask"); }
    Model m(true);
    const CliPick w = m.open_(true);
    expect(w.rc == kOk && w.cli == 0, "ON: WindowServer first gets record 0");
    const CliPick a = m.open_(false), b = m.open_(false), c = m.open_(false);
    expect(a.rc == kOk && b.rc == kOk && c.rc == kOk && a.cli == 1 && b.cli == 2 && c.cli == 3, "ON: the extra root sessions get records 1, 2, 3");
    expect(m.vmid_of(0) == 8u && m.vmid_of(1) == 9u && m.vmid_of(2) == 10u && m.vmid_of(3) == 11u, "ON: and pool handles 0..3 = VMIDs 8..11 in order");
    m.close_(2);
    expect(m.state[2] == kCliFree && m.open_(false).cli == 2, "ON: a cleanly closed record is handed out again (lowest free)");
    // ---- 0.0.669: THE CLIENT COUNT IS NOT THE VMID COUNT ------------------------------------------------------------------------------------------
    // Eight clients SUBMIT, so all eight handles are held by clients whose work has NOT retired. The ninth client still OPENS - non-resident - and gets a
    // handle at its first Submit, the moment somebody's work retires. A ninth client used to be refused with ExclusiveAccess.
    { Model q(true); uint32_t cli[9] = { 0 };
      for (uint32_t i = 0; i < kVmidPoolSize; i++) {
          const CliPick p = q.open_(false);
          expect(p.rc == kOk, "ON: the eight clients that will hold the pool all open");
          cli[i] = p.cli;
          expect_u("ON: the i-th client holds pool handle i", q.vmid_of(p.cli), 8u + i);
          expect(q.submit(p.cli) != 0u, "ON: its Submit goes on the ring");
      }
      expect_u("ON: every handle is held now", q.owner_free(), 0u);
      const CliPick ninth = q.open_(false);
      cli[8] = ninth.cli;
      expect(ninth.rc == kOk, "ON: the ninth client (one more than the pool) still opens");
      if (ninth.rc == kOk) {   // kNoSlot must never index the table: a smaller kMaxClients has to FAIL the check above, not walk off the array
          expect(q.vmid_of(ninth.cli) == 0u, "ON: ... NON-RESIDENT: the client count is not the VMID count");
          expect_u("ON: ... and its Submit cannot go on the ring while all eight handles are held by busy clients", q.submit(ninth.cli), 0u);
          q.retire_to(q.emitted);   // everything retires
          expect(q.submit(ninth.cli) != 0u, "ON: ... and goes on the ring as soon as an owner's work has retired (the handle is stolen from it)");
      }
      expect_u("ON: the pool is still exactly 8 handles", q.held(), kVmidPoolSize);
      // and the table really holds kMaxClients records
      uint32_t n = kVmidPoolSize + 1u;
      while (q.open_(false).rc == kOk) n++;
      expect_u("ON: the table fills at kMaxClients records", n, kMaxClients);
      expect_u("ON: ... and only then is an open refused", q.owner_free() > 0u || q.held() == kVmidPoolSize, 1u); }
    Model n(true); (void)n.open_(false);
    expect(n.open_(true).rc == kExclusive, "ON: WindowServer opens only when NO session is open (exactly the policy's WsAlreadyOpen, re-checked under the lock)");
    Model g(true); g.g1Holds = true;
    expect(g.open_(false).rc == kExclusive && g.open_(true).rc == kExclusive, "ON: while a G1 test holds the GPU every open is refused");
    Model r(true); const CliPick r0 = r.open_(false);
    expect(r0.cli == 0 && r.vmid_of(r0.cli) == 8u, "ON: with no WindowServer the first root session gets VMID 8 ('8 for the first')");
}

// ---- T4 ----------------------------------------------------------------------------------------------------------------------------------------------------
static void t4_release() {
    expect(cli_after_close(true, true) == kCliFree, "parked + idle -> FREE");
    expect(cli_after_close(false, true) == kCliQuarantined && cli_after_close(true, false) == kCliQuarantined && cli_after_close(false, false) == kCliQuarantined, "anything less -> QUARANTINED (held, never lost)");
    Model m(true);
    (void)m.open_(true); (void)m.open_(false); (void)m.open_(false);   // records 0, 1, 2
    (void)m.submit(1); (void)m.submit(2);
    m.retire_to(1);                                                      // record 1's work retired, record 2's not
    const uint32_t freeBefore = m.owner_free();
    m.close_(1); m.close_(2);
    expect(m.state[1] == kCliFree && m.state[2] == kCliQuarantined, "the record whose own wait failed is QUARANTINED, the other FREE");
    expect_u("the FREE record's handle went back to the pool", m.owner_free(), freeBefore + 1u);
    expect(m.vmid_of(1) == 0u && m.vmid_of(2) != 0u, "the FREE record's handle is released, the QUARANTINED one's is still HELD (the GPU may read that tree)");
    const CliPick x = m.open_(false), y = m.open_(false);
    expect(x.rc == kOk && x.cli == 1 && y.rc == kOk && y.cli == 3, "the free record 1 and then record 3 (record 2 is QUARANTINED) are handed out");
    expect(m.open_(false).rc == kOk, "... and the table is nowhere near full: it holds kMaxClients records, not kMaxSessions");
    // ---- 0.0.669: THE POINT. A quarantined record is HELD, never lost. Once its own work has retired, the RECLAIM pass returns the record AND its handle, so
    // the available resources are NOT monotonically consumed. A record whose own work was not proven retired was marked DEAD and its VMID was never handed out again for the whole boot, which
    // cost one of the eight handles per unproven close - the leak this revision removes.
    const uint32_t freeQ = m.owner_free();
    m.retire_to(m.emitted);
    expect_u("the reclaim pass returns the quarantined record", m.reclaim(), 1u);
    expect(m.state[2] == kCliFree && m.vmid_of(2) == 0u, "record 2 is reusable and its handle is back in the pool");
    expect_u("... one more handle available than before", m.owner_free(), freeQ + 1u);
    expect_u("nothing was lost: a second reclaim has nothing left to do", m.reclaim(), 0u);
    // the accounting cannot drift: every open record holds at most one handle, and the handles are distinct
    { uint32_t residents = 0;
      for (uint32_t i = 0; i < kMaxClients; i++) if (m.s[i].open && m.vmid_of(i) != 0u) residents++;
      expect_u("the held handles are exactly the resident open records", m.held(), residents);
      for (uint32_t i = 0; i < kMaxClients; i++) if (m.s[i].open && m.vmid_of(i) != 0u)
          for (uint32_t j = i + 1; j < kMaxClients; j++) if (m.s[j].open && m.vmid_of(j) != 0u)
              expect(m.vmid_of(i) != m.vmid_of(j), "no two resident clients share a VMID"); }
    Model p(true); (void)p.open_(false); (void)p.open_(false); p.close_(1, false);
    expect(p.state[1] == kCliQuarantined && p.vmid_of(1) != 0u, "a park that did not flush leaves the record QUARANTINED with its handle held");
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
    expect(stall_action(true, true, kMaxClients, true, false, false, false) == kStallLatch && stall_action(true, true, kOwnerKernel, true, false, false, false) == kStallLatch, "an out-of-range owner (>= kMaxClients) or the kernel probe: latch");
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
    expect(m.state[1] == kCliFree, "the poisoned session's work is behind the probe: its close frees normally (its handle is reusable)");
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
    // 0.0.669: page 0..kVmidPoolSize-1 describes ONE POOL HANDLE (its VMID 8 + the page, its owner or FREE)
    SessView v {}; v.state = kCliOpen; v.ws = true; v.hello = true; v.poisoned = true; v.vmid = 9; v.id = 7; v.cli = 3; v.vram = 1; v.gtt = 2; v.imported = 3; v.ptBytes = 4; v.lastSeq = 5; v.nBos = 6; v.faultAtOpen = 0x1234500081ull;
    uint64_t o[kOutN];
    sess_out(1, v, o);
    expect(o[0] == kStatOk && o[1] == 1 && o[2] == (1u | 0x100u | 0x200u | 0x400u) && o[3] == 9 && o[4] == 7 && o[5] == 1 && o[6] == 2 && o[7] == 3 && o[8] == 4 && o[9] == 5 && o[10] == 6 && o[11] == 0x1234500081ull && o[12] == 3,
           "the pool-handle page (0.0.629: o[11] = the fault word at its open; 0.0.670: o[12] = the client index holding the handle)");
    v.free = true; v.cli = kNoClient; v.ws = false; v.hello = false; v.poisoned = false; v.state = kCliFree;
    sess_out(0, v, o);
    expect(o[2] == (0x1000u) && o[12] == (uint64_t)kNoClient, "a FREE pool handle says so (flag 0x1000) and names no client");
    GlobalView g {}; g.open = 2; g.quarantined = 1; g.hung = true; g.emitted = 10; g.retired = 9; g.othersVram = 11; g.othersCap = 12; g.recoveries = 13; g.lastGuiltySlot = 2; g.lastGuiltySeq = 14; g.collateralTotal = 15; g.faultNow = 16; g.faultPrev = 17;
    global_out(g, o);
    expect(o[1] == kStatGlobal && o[2] == (2u | (1u << 8) | 0x10000u) && o[3] == 10 && o[4] == 9 && o[5] == 11 && o[6] == 12 && o[7] == 13 && o[8] == 2 && o[9] == 14 && o[10] == 15 && o[11] == 16 && o[12] == 17,
           "the global page: the LAYOUT IS UNCHANGED (0.0.629: o[11] the fault word now, o[12] at the previous sessstat 8; 0.0.670: bits 8..15 are the quarantined count, the count that used to be named 'dead')");
    // 0.0.669: the NEW client-table page
    GlobalView c {}; c.open = 3; c.quarantined = 1; c.records = 5; c.freeList = 61; c.handlesFree = 2; c.handlesHeld = 6;
    cli_out(c, o);
    expect(o[0] == kStatOk && o[1] == kStatClients && o[2] == (3ull | (1ull << 8) | (5ull << 16) | ((uint64_t)kMaxClients << 24)) && o[3] == 61 && o[4] == kVmidPoolSize && o[5] == 2 && o[6] == 6 && o[7] == 1,
           "the client-table page: open / quarantined / records allocated / kMaxClients, the free list, the pool size, the pool's free and held handles");
    expect(o[8] == 0 && o[9] == 0 && o[10] == 0 && o[11] == 0 && o[12] == 0, "... and everything else is zero (a page with its own shape, not a reused one)");
    // T13's model-level pool checks live below; here the pure pool decision itself
    expect(args_ok(kActSessStat, kStatClients) && !args_ok(kActSessStat, kStatClients + 1u), "0.0.670 ADDS page kStatClients and nothing beyond it");
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
                          "const uint32_t reclaimed = cli_reclaim_locked();", "const CliPick pk = open_pick(multi, gCliState, kMaxClients, openNow, ws, g1_holds(), appsNow, wsOpen);", "if (pk.rc != kOk) {", "const uint32_t cli = pk.cli;",
                          "if (gHandleUsable == 0u) handle_probe_all();", "const uint32_t arc = vmid_assign_locked(s, nullptr);", "if (arc != kOk && arc != kNotReady) { rc = (IOReturn)arc; break; }",
                          "gCliState[cli] = kCliOpen;", "IOLockUnlock(gCliLock);" }),
           "n1c_open: HUNG gate, the OFF compare-and-swap, the lock, the RECLAIM pass, open_pick over the CLIENT TABLE, the per-HANDLE geometry probe, the pool handle (or none: the client stays non-resident), the record marked open");
    expect(op.find("const uint32_t openNow = multi ? __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) : 0u;") != std::string::npos, "OFF feeds open_pick an open count of 0 (the CAS already made it exclusive)");
    expect(op.find("client opened NON-RESIDENT") != std::string::npos, "0.0.670: a client whose handle could not be had at open time is NOT refused: it opens NON-RESIDENT (and says so)");
    { const std::string cp = fn_body(eng, "static __attribute__((noinline)) uint32_t ctx_prepare(uint32_t h, uint32_t vmid, bool *ok) {");
      expect(in_order(cp, { "const CtxRegs r = ctx_regs(vmid);", "if (h != 0u && !ctx_geometry_ok(cntl)) {", "WREG32(dev, r.cntl, cntl_native(before));", "cntl = RREG32(dev, r.cntl);", "*ok = ctx_geometry_ok(cntl);" }),
             "0.0.670: CONTEXT9..15 (pool handles 1..7) get cntl_native once per HANDLE (never CONTEXT8, which is read only), then the geometry is checked by read-back before use"); }
    expect(in_order(fn_body(eng, "static __attribute__((noinline)) void handle_probe_all(void) {"), { "for (uint32_t h = 0; h < kVmidPoolSize; h++) {", "ctx_prepare(h, vmid_of_handle(h), &ok);", "if (ok) usable |= (1u << h);", "gHandleUsable = usable;" }),
           "0.0.670: every pool handle is probed ONCE at the first open and a handle that fails is taken out of the pool for the rest of the boot (fail closed)");
    expect_u("the only CNTL write is that one", count_of(eng, "WREG32(dev, r.cntl,"), 1);
    // 0.0.667: the "ring idle for the FIRST session" wait left gCliLock. Assert the hoist (the loop is BEFORE the lock) and that nothing sleeps
    // between the lock and its unlock, so moving the wait back inside gCliLock fails this test.
    { const size_t loop = op.find("for (;;)"), lk = op.find("IOLockLock(gCliLock);");
      expect(loop != std::string::npos && lk != std::string::npos && loop < lk,
             "n1c_open: the ring-idle wait for the FIRST session runs BEFORE gCliLock is taken (0.0.667)"); }
    { const size_t lk = op.find("IOLockLock(gCliLock);"), ul = op.find("IOLockUnlock(gCliLock);", lk);
      const std::string under = (lk != std::string::npos && ul != std::string::npos) ? op.substr(lk, ul - lk) : std::string();
      expect(!under.empty() && under.find("IOSleep") == std::string::npos && under.find("IODelay") == std::string::npos,
             "n1c_open: nothing sleeps between IOLockLock(gCliLock) and its unlock - only the non-blocking re-check"); }
    expect(in_order(op, { "if (openNow == 0u) {", "*seq_slot() = 0ull;", "__atomic_store_n(&gEmitted, 0ull, __ATOMIC_RELEASE);" }),
           "only the FIRST session restarts the seqno");
    expect(count_of(eng, "IOMalloc(sizeof(Session))") == 1 && eng.find("IOFree(ns") == std::string::npos && eng.find("IOFree(s, sizeof(Session))") == std::string::npos, "the records are allocated once and never freed (a racing selector must never touch freed memory: they are RECYCLED through gCliState)");
    expect(eng.find("static Session gSess;") != std::string::npos && eng.find("static Session *gCli[kMaxClients] = { &gSess };") != std::string::npos, "record 0 is the static gSess");
    expect(eng.find("static_assert(kMaxClients * sizeof(Session) <= n48native::g2::kClientKernelBudget") != std::string::npos, "0.0.670: the client table is bounded by MEMORY (a static_assert against the pure header's budget), not by the GPU");
    // close: the own wait, the park of the session's own VMID, the slot's next state
    const std::string cl = fn_body(eng, "void n1c_close(const N1cRef &ref, const char *how) {");
    expect(in_order(cl, { "Session *s = sess_lock(ref, false);", "bool leak = hung_now();", "if (!leak && !own_wait(s)) leak = true;", "const bool ownIdle = !leak;", "IOLockLock(gCliLock);",
                          "if (s->vmid == 0u) parked = true;", "else if (vmid_release_locked(s) == kOk) parked = true;", "bo_release(s, h, kRelClosing)",
                          "const uint8_t nst = n48native::g2::cli_after_close(parked, ownIdle);", "gCliState[s->cli] = nst;", "IOLockUnlock(gCliLock);", "IOLockUnlock(sl);" }),
           "close: own wait (session lock only), park + RELEASE this client's pool handle (a non-resident client has nothing to park), free, then the record's state from cli_after_close");
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
    expect(in_order(rs, { "IOLockLock(gRingLock);", "const uint32_t vm = __atomic_load_n(&s->vmid, __ATOMIC_ACQUIRE);", "if (vm == 0u) { IOLockUnlock(gRingLock); return kNotReady; }",
                          "const uint64_t seq = emitted_now() + 1ull;", "va, by, nIbs, vm, hasFence,", "ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->cli, s->id, s);", "IOLockUnlock(gRingLock);" }),
           "ring_submit: the VMID HANDLE is read UNDER gRingLock (a steal revokes it there), then the seqno, the packet with it and the owner, under one hold of the ring lock");
    const std::string rp = fn_body(eng, "static uint32_t ring_put_locked(");
    expect(in_order(rp, { "ring[ring_idx(wc, mask)] = dw[i]", "n48native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc);", "if (owner != nullptr) __atomic_store_n(&owner->emitted, seq, __ATOMIC_RELEASE);", "__atomic_store_n(&gEmitted, seq, __ATOMIC_RELEASE);", "WDOORBELL64(" }),
           "ring_put_locked: the owner and the session's last seqno are recorded BEFORE the global seqno is published and the doorbell rung");
    expect(rp.find("IOLockLock(gRingLock)") == std::string::npos && rp.find("hang_from_wait") == std::string::npos, "ring_put_locked neither takes the ring lock (the caller holds it) nor latches (the caller decides, after dropping the lock)");
    expect(eng.find("kNativeVmid, hasFence") == std::string::npos && eng.find("build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), &va, &by, 1u, kNativeVmid") == std::string::npos, "no submit is built with a fixed VMID 8");
    expect(in_order(fn_body(eng, "IOReturn n1c_submit("), { "const uint32_t vrc = vmid_need(s, true);", "if (vrc != kOk) { rc = (IOReturn)vrc; break; }", "for (uint32_t attempt = 0; attempt < 2u; attempt++) {",
                                                                  "if (er != kNotReady) break;", "const uint32_t vrc2 = vmid_need(s, true);", "if (er != kOk) { rc = (IOReturn)er; break; }" }),
           "0.0.670: Submit takes its residency first (bounded wait), and retries ONCE when a steal revoked the handle between that and the ring");
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
    expect(in_order(rc, { "IOLockLock(gRingLock);", "const uint32_t gvm = __atomic_load_n(&gs->vmid, __ATOMIC_ACQUIRE);", "if (gvm == 0u) {", "g2_scrub_locked(g.startWc);", "__atomic_store_n(&gs->poisoned, 1u, __ATOMIC_SEQ_CST);", "mes_reset_legacy_gfx_queue(dev, gCtx->mes, 0u, 0u, gvm, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);",
                          "if (r == kIOReturnSuccess) {", "put = ring_put_locked(dw, 8u, probe, kOwnerKernel, 0u, nullptr) == kOk;", "IOLockUnlock(gRingLock);", "const bool ok = recovery_ok(r == kIOReturnSuccess, put, ret);" }),
           "g2_recover: the guilty client must hold a handle (guard), emission frozen, scrub, POISON before the RESET, one MES RESET of the guilty VMID, the probe only after an ack, verdict by recovery_ok");
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
    expect(cli.find("else if (what && !strcmp(what, \"sessstat\")) in = 103;") != std::string::npos && cli.find("in == 103 ? \"sessstat\"") != std::string::npos && cli.find("hangstat sessstat scdcread appallow fbpublish m6stat m6xstat vramstat\\n") != std::string::npos && cli.find("|sessstat [0..9]|scdcread <line 2|3> <off 0..0x5f> [len 1..16]|appallow add") != std::string::npos && cli.find("if (c == 0 && out[4] == 9) {") != std::string::npos,
           "navi48test: the verb, its name, the known list and the usage");
    // no single-session leftovers, the stack rule
    expect(eng.find("static Session *gS;") == std::string::npos && eng.find("gS->") == std::string::npos && eng.find("&gS,") == std::string::npos, "no global single-session pointer is left");
    expect(eng.find("gSlotState") == std::string::npos && eng.find("slot_after_close") == std::string::npos && eng.find("kSlotDead") == std::string::npos && eng.find("vmid_of_slot") == std::string::npos,
           "0.0.670: the slot model is GONE from the engine (no gSlotState, no slot_after_close, no kSlotDead, no vmid_of_slot): a client is a table record, a VMID is a pool handle");
    expect(eng.find("Session tmp") == std::string::npos && eng.find("Session copy") == std::string::npos && eng.find("OwnerLog l") == std::string::npos, "no Session / OwnerLog on the stack (static or heap only)");
    // banned strings, the version
    const std::string b1 = std::string("pipe+0x2") + "80", b2 = std::string("+0x2") + "82", b3 = std::string("+0x2") + "99";
    for (const std::string *f : { &eng, &pure, &ncl, &hdr }) expect(f->find(b1) == std::string::npos && f->find(b2) == std::string::npos && f->find(b3) == std::string::npos, "no banned pipe offsets");
    expect(count_of(plist, "<string>0.0.670</string>") == 2 && plist.find("0.0.626") == std::string::npos, "Info.plist is 0.0.670, carried twice");
    expect(hdr.find("constexpr uint32_t kN1cKextBuild = 670;") != std::string::npos, "the native client reports build 670");
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
        const CliPick a = m.open_(false);
        expect(a.rc == kOk, "R3 model: an app session opens");
        const uint64_t bad = m.submit(a.cli);
        m.retire_to(bad - 1);
        if (m.stall() == kStallRecover) recovered++;
        if (!m.hung) m.close_(a.cli);
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
    expect(in_order(op, { "if (s->lock == nullptr) {", "IOLock *nl = IOLockAlloc();", "__atomic_store_n(&s->lock, nl, __ATOMIC_RELEASE);", "n48native::g2::sess_wipe_keep_lock(s);", "s->cli = cli; s->ws = ws;" }),
           "R1: open creates the lock once per record, then wipes the record WITHOUT it");
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
    // 0.0.667: the old text (the kTimeout branch INSIDE the lock hold) is gone. Assert the surviving contract: the ring-space Timeout still
    // goes through stall_from_wait(), and it is reached with gRingLock already dropped.
    { const std::string rs = fn_body(eng, "static uint32_t ring_submit(");
      const size_t ul = rs.find("IOLockUnlock(gRingLock)"), sf = rs.find("stall_from_wait()");
      expect(ul != std::string::npos && sf != std::string::npos && ul < sf,
             "R2: the ring-space Timeout still goes through stall_from_wait(), now reached with gRingLock dropped (0.0.667)"); }
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
    expect(in_order(op, { "n48native::g2::sess_wipe_keep_lock(s);", "s->faultAtOpen = fault_snap(\"open\", cli);", "gCliState[cli] = kCliOpen;" }), "R5: the word is recorded at each session's open");
    const std::string st = fn_body(eng, "IOReturn n1c_g2_stat(uint64_t page, uint64_t *o) {");
    expect(in_order(st, { "g.faultNow = fault_snap(\"sessstat\", kStatGlobal);", "g.faultPrev = gFaultPrev; gFaultPrev = g.faultNow;", "global_out(g, o);", "v.faultAtOpen = s->faultAtOpen;", "sess_out(handle, v, o);" }),
           "R5: sessstat 8 reports the word now and at the previous call; a pool-handle page its word at open");
    expect(cli.find("gcvm fault now / prev") != std::string::npos && cli.find("gcvm fault at open") != std::string::npos, "R5: navi48test prints them");
    // R6
    expect(in_order(bf, { "const uint32_t mrc = hung_now() ? kOk : n48native::g2::bofree_mapping_rc(multi_on(), b0.kind == kBoVis, b0.vmd != nullptr, b0.vmd != nullptr ? (uint32_t)b0.vmd->getRetainCount() : 0u);",
                          "if (mrc != kOk) {", "rc = (IOReturn)mrc;", "} else {", "own_wait(s, &noBlame)", "bo_release(s, h, m);" }),
           "R6: BoFree refuses a CPU-mapped visible-VRAM BO (switch ON) BEFORE any wait or release");
    { const size_t a = bf.find("if (mrc != kOk) {"), b = bf.find("} else {", a);
      expect(a != std::string::npos && b != std::string::npos && bf.substr(a, b - a).find("bo_release") == std::string::npos && bf.substr(a, b - a).find("own_wait") == std::string::npos, "R6: the refusal touches nothing"); }
    expect(eng.find("static_assert(n48native::g2::kBusy == (uint32_t)kIOReturnBusy") != std::string::npos, "R6: kBusy is kIOReturnBusy");
}

// ---- T13 (0.0.669): the VMID POOL itself -------------------------------------------------------------------------------------------------------------------------
// vmid_pick: the lowest FREE usable handle, else one STOLEN from a client whose own work is retired, else NotReady. reclaim_pick: the quarantined record whose
// work has retired. Together these are what make the available resources non-monotonic.
static void t13_pool() {
    uint32_t owner[kVmidPoolSize]; bool idle[kVmidPoolSize];
    for (uint32_t h = 0; h < kVmidPoolSize; h++) { owner[h] = kNoClient; idle[h] = false; }
    const uint32_t all = (1u << kVmidPoolSize) - 1u;
    { const VmidPick p = vmid_pick(owner, idle, all);
      expect(p.rc == kOk && p.handle == 0u && p.victim == kNoClient, "the pool is empty: the lowest handle is handed out, nobody is a victim"); }
    for (uint32_t h = 1; h < kVmidPoolSize; h++) owner[h] = 100u + h;
    { const VmidPick p = vmid_pick(owner, idle, all);
      expect(p.rc == kOk && p.handle == 0u, "... and a FREE handle is always preferred"); }
    owner[0] = 100u;   // the pool is full now
    { const VmidPick p = vmid_pick(owner, idle, all);
      expect(p.rc == kNotReady && p.handle == kNoSlot && p.victim == kNoClient, "pool full and nobody idle: NotReady (the caller waits bounded, it is not a refusal)"); }
    idle[3] = true;
    { const VmidPick p = vmid_pick(owner, idle, all);
      expect(p.rc == kOk && p.handle == 3u && p.victim == (100u + 3u), "pool full, one client's work retired: its handle is STOLEN (the lowest idle one)"); }
    { const VmidPick p = vmid_pick(owner, idle, all & ~(1u << 3));
      expect(p.rc == kNotReady, "a handle without the native geometry (usable bit clear) is NEVER handed out, whatever the state"); }
    { uint32_t o2[kVmidPoolSize]; bool i2[kVmidPoolSize];
      for (uint32_t h = 0; h < kVmidPoolSize; h++) { o2[h] = kNoClient; i2[h] = false; }
      o2[0] = 100u;   // handle 0 is taken; handle 1 is FREE but its CONTEXT failed the geometry probe at boot
      const VmidPick p = vmid_pick(o2, i2, all & ~(1u << 1));
      expect(p.rc == kOk && p.handle != 1u, "a FREE handle whose CONTEXT failed the geometry probe is skipped as well (fail closed, never a wild pointer)"); }
    { const VmidPick p = vmid_pick(owner, idle, 1u << 3);
      // the only usable handle is held and only it is idle
      expect(p.rc == kOk && p.handle == 3u && p.victim == (100u + 3u), "a restricted `usable` mask is honoured"); }
    for (uint32_t h = 0; h < kVmidPoolSize; h++) idle[h] = true;
    { const VmidPick p = vmid_pick(owner, idle, all);
      expect(p.rc == kOk && p.handle == 0u && p.victim == 100u, "several idle clients: the LOWEST handle wins (deterministic)"); }
    // reclaim_pick
    uint8_t state[kMaxClients]; bool rid[kMaxClients];
    for (uint32_t i = 0; i < kMaxClients; i++) { state[i] = kCliFree; rid[i] = false; }
    expect_u("nothing quarantined: nothing to reclaim", reclaim_pick(state, rid, kMaxClients), kNoSlot);
    state[5] = kCliQuarantined;
    expect_u("a quarantined record whose work has NOT retired is left alone", reclaim_pick(state, rid, kMaxClients), kNoSlot);
    rid[5] = true;
    expect_u("... and is reclaimed the moment own_idle proves it retired", reclaim_pick(state, rid, kMaxClients), 5u);
    rid[5] = false; rid[2] = true; state[2] = kCliOpen;
    expect_u("an OPEN record is never 'reclaimed' (only a quarantine ends that way)", reclaim_pick(state, rid, kMaxClients), kNoSlot);
    rid[2] = false; state[2] = kCliQuarantined; rid[2] = true; rid[5] = true;
    expect_u("the lowest reclaimable record wins", reclaim_pick(state, rid, kMaxClients), 2u);
    rid[2] = false;
    // the model-level point: resources are NOT monotonically consumed. Eight clients submit, ten open, all of them eventually close, and the pool AND the table
    // come back whole - there is no state from which a resource is gone for the boot.
    { Model m(true); uint32_t cli[10]; uint32_t n = 0;
      for (uint32_t i = 0; i < 10u; i++) {
          const CliPick p = m.open_(false);
          expect(p.rc == kOk, "T13: ten clients open (kMaxClients is far above the pool size)");
          if (p.rc != kOk) break;                                        // a shrunk kMaxClients must FAIL the check above, not overrun this array
          cli[n++] = p.cli;
      }
      expect_u("T13: ten records are in use", n, 10u);
      for (uint32_t i = 0; i < 8u && i < n; i++) (void)m.submit(cli[i]);  // the pool is fully held by BUSY clients
      expect_u("T13: the pool is exhausted and the table is not", m.owner_free(), 0u);
      for (uint32_t i = 0; i < n; i++) m.close_(cli[i]);                  // every close is unproven (nothing retired)
      expect(n > 0u && m.state[cli[0]] == kCliQuarantined, "T13: an unproven close quarantines its record");
      expect(n > 9u && m.state[cli[8]] == kCliFree && m.state[cli[9]] == kCliFree, "T13: the two clients that never submitted (non-resident) close cleanly");
      expect_u("T13: ... so the handles are HELD, not lost", m.owner_free(), 0u);
      m.retire_to(m.emitted);                                            // the GPU catches up
      expect_u("T13: one reclaim pass gives every quarantined record AND its handle back", m.reclaim(), 8u);
      expect_u("T13: the pool is whole again (all 8 handles free)", m.owner_free(), kVmidPoolSize);
      uint32_t freeRecords = 0; for (uint32_t i = 0; i < kMaxClients; i++) if (m.state[i] == kCliFree) freeRecords++;
      expect_u("T13: and so is the client table", freeRecords, kMaxClients);
      expect_u("T13: nothing is left to reclaim", m.reclaim(), 0u); }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    t1_switch(); t2_off_identity(); t3_slots(); t4_release(); t5_waits(); t6_guilty(); t7_decision(); t8_poison(); t9_budget_stat(); t10_close_disarm(); t11_pins(root);
    t12_r1_race(); t12_r2_blame(); t12_r3_budget(); t12_r4_syscap(); t12_r5_fault(); t12_r6_mapped(); t12_pins(root); t13_pool();
    std::printf("native_g2: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
