// native_g4_test.cpp - kext 0.0.640, review fixes 0.0.641 (GPU-apps stage G4: ordinary user applications as a third client role; an internal design note section 4 and the G1 / G2 reviews): the pure half
// (amd/native_g4_pure.h, amd/native_open_policy_pure.h, Navi48AppKey.h) driven directly and through small models, plus source pins that tie the REAL kernel code
// (amd/native_s1c.cpp, Navi48NativeClient.cpp / .hpp, Navi48Bringup.cpp, Navi48UserClient.cpp, Navi48NativeABI.h, tools/pc/navi48test.c) to it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g4_test.cpp -o /tmp/native_g4 && /tmp/native_g4 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g4_plant.sh plants breaks in the real code and demands that this suite FAILS on each)
// Covers:
//   T1  the switches: navi48-apps latch; the role exists only with BOTH latches;
//   T2  the open policy over the WHOLE input space (admin x uid x ws-arg x already-open x apps x allow-list): with the switches OFF the verdict is open_decision's, bit for bit; an
//       administrator and uid 88 are never touched; a system uid (< 501) is never an APP; an ordinary user is an APP only on the list; every reason has its own text;
//   T3  the selector sets: an APP reaches exactly 0, 1, 3..8 and 21 (never ReadRegs 2, never scanout 9..14, never 15..20, never an unknown number), a subset of WindowServer's set;
//   T4  the allow-list: add / remove / list, 32 entries, duplicates, a full list, the empty list at boot, the verb's argument space, the name key (16 characters, never 0), a threaded add;
//   T5  the budget report in QueryInfo: nothing OFF or for WindowServer, 25 % cap and the available bytes for the others, the APP flag, the lo / hi words;
//   T6  the close path of R6: a model of the VRAM pool shows a mapped BO is never handed to the next client (switch ON) and that OFF behaves as 0.0.629;
//   T7  defence in depth: an APP session's engine refuses ReadRegs and the scanout calls by itself;
//   T8  source pins (reachability and order) on the kernel code, the verb's six sites, the version and the ABI;
//   T9..T14 (kext 0.0.641, the G4 review fixes): T9 the close path (gated on apps_on(), the close-time unmap sweep, never free while mapped), T10 the "open refused" log rate limit,
//       T11 the visible-VRAM pool (hi-first, the non-WindowServer cap, sessstat), T12 WindowServer always gets a slot (open_pick / ws_already_open, exhaustive), T13 the strike table,
//       T14 wording and the one ungated change.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "native_g4_pure.h"

using namespace n48native;

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

// ---- T1 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t1_switch() {
    expect(g4::latch_value(true, 1) == g4::kLatchOn && g4::latch_value(true, 0) == g4::kLatchOff && g4::latch_value(true, 2) == g4::kLatchOff && g4::latch_value(false, 1) == g4::kLatchOff,
           "T1: only navi48-apps=1 latches ON");
    expect(g4::latch_is_on(g4::kLatchOn) && !g4::latch_is_on(g4::kLatchOff) && !g4::latch_is_on(g4::kLatchUnset), "T1: Unset reads as OFF");
    expect(g4::apps_enabled(true, true) && !g4::apps_enabled(true, false) && !g4::apps_enabled(false, true) && !g4::apps_enabled(false, false), "T1: the role needs navi48-apps AND navi48-multisession");
    expect(g4::action_admitted(true, 104) && !g4::action_admitted(false, 104) && !g4::action_admitted(true, 103) && !g4::action_admitted(true, 105) && !g4::action_admitted(true, 0), "T1: action 104 exists only with the role enabled");
    expect(g4::kActAppAllow == 104u && g4::is_g4_action(104) && !g4::is_g4_action(103) && !g4::is_g4_action(105), "T1: the verb is 104");
}

// ---- T2 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t2_open_policy() {
    using namespace policy;
    static const uint32_t uids[] = { 0, 1, 88, 89, 200, 499, 500, 501, 502, 1000, 65534, 0xFFFFFFFFu };
    uint64_t nOff = 0, nApp = 0, nNotAllowed = 0, nSame = 0;
    for (int admin = 0; admin <= 1; admin++) for (uint32_t uid : uids) for (int ws = 0; ws <= 1; ws++) for (int open = 0; open <= 1; open++)
    for (int apps = 0; apps <= 1; apps++) for (int allow = 0; allow <= 1; allow++) {
        const OpenDecision base = open_decision(admin != 0, uid, ws != 0, open != 0);
        const OpenDecision d = app_refine(base, uid, apps != 0, allow != 0);
        if (!apps) { nOff++; expect(d.admit == base.admit && d.reason == base.reason, "T2: with the switches OFF the verdict is open_decision's, bit for bit (every input, either allow-list state)"); }
        if (admin) expect(d.admit && d.reason == kReasonAdmin, "T2: an administrator is admitted as an administrator, whatever the switches");
        if (!admin && uid == 88) expect(d.admit == base.admit && d.reason == base.reason, "T2: uid 88 is decided by the WindowServer rule alone");
        if (!admin && uid < kAppMinUid) expect(d.admit == base.admit && d.reason == base.reason && d.reason != kReasonApp, "T2: a system uid (< 501) is never an APP");
        if (!admin && uid >= kAppMinUid && apps) {
            if (allow) { nApp++; expect(d.admit && d.reason == kReasonApp, "T2: uid >= 501, switches ON, on the allow-list: admitted as an APP"); }
            else { nNotAllowed++; expect(!d.admit && d.reason == kReasonAppNotAllowed, "T2: uid >= 501, switches ON, NOT on the allow-list: refused with its own reason"); }
        }
        if (!admin && uid >= kAppMinUid && !apps) expect(!d.admit && d.reason == kReasonNotPrivileged, "T2: uid >= 501 with the switches OFF: today's NotPrivileged");
        if (d.reason == base.reason && d.admit == base.admit) nSame++;
        expect(d.reason < kReasonCount && open_reason_text(d.reason) != nullptr && std::strcmp(open_reason_text(d.reason), "?") != 0, "T2: every reason has a text");
        expect(d.admit == (d.reason == kReasonAdmin || d.reason == kReasonWindowServer || d.reason == kReasonApp), "T2: admit is true exactly for the three admitting reasons");
    }
    expect(nOff > 0 && nApp > 0 && nNotAllowed > 0 && nSame > 0, "T2: every branch of the matrix was reached");
    for (uint32_t a = 0; a < kReasonCount; a++) for (uint32_t b = a + 1; b < kReasonCount; b++)
        expect(std::strcmp(open_reason_text((OpenReason)a), open_reason_text((OpenReason)b)) != 0, "T2: reason texts are distinct");
    expect_u("T2: seven reasons", kReasonCount, 7);
    expect_u("T2: the first ordinary macOS user is uid 501", kAppMinUid, 501);
    // a refused WindowServer-path verdict is never turned into an APP (uid 88 < 501 anyway, but also the reason test)
    expect(!app_refine(OpenDecision{ false, kReasonWsArgOff }, 600, true, true).admit && !app_refine(OpenDecision{ false, kReasonWsAlreadyOpen }, 600, true, true).admit,
           "T2: only a NotPrivileged refusal is ever refined");
}

// ---- T3 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t3_selectors() {
    using namespace policy;
    uint32_t nAllowed = 0;
    for (uint32_t sel = 0; sel < 64; sel++) {
        const bool want = sel == 0 || sel == 1 || (sel >= 3 && sel <= 8) || sel == 21;
        expect(selector_allowed_for_app(sel) == want, "T3: an APP reaches exactly 0, 1, 3..8 and 21");
        if (selector_allowed_for_app(sel)) { nAllowed++; expect(selector_allowed_for_windowserver(sel) && selector_allowed(true, sel), "T3: the APP set is inside WindowServer's set"); }
    }
    expect_u("T3: nine selectors", nAllowed, 9);
    expect(!selector_allowed_for_app(N48N_SEL_READREGS), "T3: never ReadRegs");
    for (uint32_t sel = N48N_SEL_SCAN_QUERY; sel <= N48N_SEL_SCAN_RELEASE; sel++) expect(!selector_allowed_for_app(sel), "T3: never the scanout set 9..14");
    for (uint32_t sel = 15; sel <= 20; sel++) expect(!selector_allowed_for_app(sel), "T3: never DAL / mode trial / hold / release / nub publish / withdraw");
    expect(!selector_allowed_for_app(0xFFFFFFFFu) && !selector_allowed_for_app(22) && !selector_allowed_for_app(23), "T3: no unknown number");
}

// ---- T4 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t4_allowlist() {
    using namespace g4;
    static uint64_t list[kMaxEntries];   // static: zero-initialised, like the kernel's
    expect_u("T4: the list is empty at boot (zero-initialised)", al_count(list), 0);
    uint64_t o[kOutN];
    // the name key
    expect(n48a_comm_key("mpv") == n48a_comm_key("mpv") && n48a_comm_key("mpv") != n48a_comm_key("IINA") && n48a_comm_key("mpv") != n48a_comm_key("mpV"), "T4: the key is stable and name-sensitive");
    expect(n48a_comm_key("") != 0ull && n48a_comm_key(nullptr) != 0ull, "T4: the key is never 0 (0 is the empty slot)");
    expect((n48a_comm_key("Blender") & ~N48A_KEY_MASK) == 0ull && (n48a_comm_key("com.apple.WebKit.GPU") & ~N48A_KEY_MASK) == 0ull, "T4: the key fits 60 bits");
    expect(n48a_comm_key("0123456789abcdefXYZ") == n48a_comm_key("0123456789abcdef") && n48a_comm_key("0123456789abcde") != n48a_comm_key("0123456789abcdef"), "T4: only the first 16 characters count (MAXCOMLEN)");
    {   // the vector is computed here independently, byte by byte
        uint64_t h = 0xcbf29ce484222325ull; for (const char *c = "mpv"; *c; c++) { h ^= (uint8_t)*c; h *= 0x100000001b3ull; }
        h &= ((1ull << 60) - 1ull);
        expect_u("T4: the key of \"mpv\" is FNV-1a(\"mpv\") masked to 60 bits", n48a_comm_key("mpv"), h);
    }
    // the verb's argument space
    expect(args_ok(104, n48a_arg(N48A_OP_ADD, 5)) && args_ok(104, n48a_arg(N48A_OP_REMOVE, 5)) && args_ok(104, n48a_arg(N48A_OP_LIST, 0)) && args_ok(104, n48a_arg(N48A_OP_LIST, 3)), "T4: legal arguments");
    expect(!args_ok(104, n48a_arg(N48A_OP_LIST, 4)) && !args_ok(104, n48a_arg(N48A_OP_ADD, 0)) && !args_ok(104, n48a_arg(N48A_OP_REMOVE, 0)) && !args_ok(104, 0ull) && !args_ok(103, n48a_arg(N48A_OP_ADD, 5)), "T4: list page 4, a zero key, op 0 and a wrong action are refused");
    for (uint32_t op = 0; op < 16; op++) {
        const bool legal = op >= 1 && op <= 3;
        expect(args_ok(104, ((uint64_t)op << 60) | 1ull) == legal, "T4: only operations 1..3 exist");
    }
    expect(native_exempt(true, 104, n48a_arg(N48A_OP_ADD, 9)) && !native_exempt(false, 104, n48a_arg(N48A_OP_ADD, 9)) && !native_exempt(true, 104, 0ull), "T4: the verb's exemption needs the role AND a legal argument");
    // OFF: nothing happens
    expect_u("T4: role disabled: the verb reports OFF", verb_run(false, list, n48a_arg(N48A_OP_ADD, 7), o), kAllowOff);
    expect_u("T4: ... and changed nothing", al_count(list), 0);
    // add / list / remove
    expect_u("T4: add ok", verb_run(true, list, n48a_arg(N48A_OP_ADD, 7), o), kAllowOk);
    expect(o[0] == kAllowOk && o[1] == 1ull && o[2] == N48A_OP_ADD && o[3] == kMaxEntries && o[4] == 7ull, "T4: add reports code, count, op, max, key");
    expect_u("T4: add again -> already there", verb_run(true, list, n48a_arg(N48A_OP_ADD, 7), o), kAllowExists);
    expect_u("T4: ... and the count stays 1", al_count(list), 1);
    expect(al_contains(list, 7) && !al_contains(list, 8) && !al_contains(list, 0), "T4: contains");
    expect_u("T4: remove an absent key", verb_run(true, list, n48a_arg(N48A_OP_REMOVE, 9), o), kAllowNotFound);
    expect_u("T4: list page 0", verb_run(true, list, n48a_arg(N48A_OP_LIST, 0), o), kAllowOk);
    expect(o[1] == 1ull && o[2] == N48A_OP_LIST && o[4] == 7ull && o[5] == 0ull && o[12] == 0ull, "T4: list shows the entry in slot 0 and empty slots after it");
    expect_u("T4: remove it", verb_run(true, list, n48a_arg(N48A_OP_REMOVE, 7), o), kAllowOk);
    expect_u("T4: gone", al_count(list), 0);
    expect(al_add(list, 0) == kAllowBadArg && al_remove(list, 0) == kAllowBadArg && al_add(list, 1ull << 60) == kAllowBadArg && al_remove(list, 1ull << 61) == kAllowBadArg, "T4: key 0 and keys wider than 60 bits are refused");
    // fill to 32, the 33rd is refused, a slot freed is reused
    for (uint64_t k = 1; k <= kMaxEntries; k++) expect_u("T4: add 1..32", al_add(list, 100 + k), kAllowOk);
    expect_u("T4: 32 entries", al_count(list), kMaxEntries);
    expect_u("T4: the 33rd is refused: FULL", al_add(list, 999), kAllowFull);
    expect_u("T4: the verb says so too", verb_run(true, list, n48a_arg(N48A_OP_ADD, 999), o), kAllowFull);
    expect(!al_contains(list, 999), "T4: and it was not added");
    expect_u("T4: remove one", al_remove(list, 105), kAllowOk);
    expect_u("T4: now the 33rd fits", al_add(list, 999), kAllowOk);
    expect(al_contains(list, 999) && !al_contains(list, 105), "T4: the freed slot was reused");
    {   // the four list pages cover every slot exactly once
        std::vector<uint64_t> seen;
        for (uint64_t pg = 0; pg < kListPages; pg++) { verb_run(true, list, n48a_arg(N48A_OP_LIST, pg), o); for (uint32_t i = 0; i < kListPerPage; i++) if (o[4 + i]) seen.push_back(o[4 + i]); }
        expect_u("T4: the pages list all 32 entries", seen.size(), 32);
        std::sort(seen.begin(), seen.end());
        bool distinct = true; for (size_t i = 1; i < seen.size(); i++) if (seen[i] == seen[i - 1]) distinct = false;
        expect(distinct, "T4: ... each once");
    }
    // a duplicate planted by a racing add is removed in one go
    list[0] = 4242; list[1] = 4242;
    expect_u("T4: remove clears every slot holding the key", al_remove(list, 4242), kAllowOk);
    expect(!al_contains(list, 4242), "T4: ... both");
    std::memset(list, 0, sizeof list);
    {   // 0.0.642, deterministic interleavings through al_add_h's seams (the racing adder acts exactly where a second thread could)
        std::memset(list, 0, sizeof list);
        uint32_t rc = al_add_h(list, 777, [&](uint32_t p) { if (p == 0u) list[7] = 777; });   // a rival claimed slot 7 after our contains check: we take the LOWEST free slot (0) and clear the higher duplicate
        expect(rc == kAllowOk && list[0] == 777 && list[7] == 0 && al_count(list) == 1u, "T4: a higher duplicate left by a racing adder is cleared (lowest slot wins)");
        std::memset(list, 0, sizeof list);
        for (uint64_t k = 0; k < 5; k++) list[k] = 900 + k;
        rc = al_add_h(list, 778, [&](uint32_t p) { if (p == 1u) list[1] = 778; });   // we claimed slot 5; the rival then took the LOWER slot 1: we retreat, the key stays once
        expect(rc == kAllowExists && list[1] == 778 && list[5] == 0 && al_count(list) == 5u, "T4: a lower duplicate by a racing adder: we retreat from our higher slot");
        std::memset(list, 0, sizeof list);
        rc = al_add_h(list, 779, [&](uint32_t p) { if (p == 0u) list[0] = 779; });   // the rival wrote the very slot we scan first: the key is present, the slot is not written twice
        expect(rc == kAllowExists && list[0] == 779 && al_count(list) == 1u, "T4: a slot filled with the same key by a racing adder counts as present");
        std::memset(list, 0, sizeof list);
        list[3] = 555;   // a key already present BEHIND a hole: add answers EXISTS and moves nothing
        rc = al_add(list, 555);
        expect(rc == kAllowExists && list[3] == 555 && list[0] == 0 && al_count(list) == 1u, "T4: adding a key that is present (behind a free slot) is EXISTS and writes nothing");
        std::memset(list, 0, sizeof list);
        for (uint64_t k = 0; k < kMaxEntries; k++) list[k] = 900 + k;
        rc = al_add_h(list, 780, [](uint32_t) {});
        expect(rc == kAllowFull && al_count(list) == kMaxEntries, "T4: a full list with the key absent is FULL after the retries");
    }
    for (int round = 0; round < 1500; round++) {   // two threads add 20 keys each (10 shared), released together: every distinct key stored exactly once
        std::memset(list, 0, sizeof list);
        std::atomic<int> go{0};
        std::thread a([&] { while (!go.load()) {} for (uint64_t k = 1; k <= 20; k++) al_add(list, 500 + k); });
        std::thread b([&] { while (!go.load()) {} for (uint64_t k = 11; k <= 30; k++) al_add(list, 500 + k); });
        go.store(1); a.join(); b.join();
        bool all = true; for (uint64_t k = 1; k <= 30; k++) if (!al_contains(list, 500 + k)) all = false;
        if (!all || al_count(list) != 30u) { expect(all, "T4: no key was lost"); expect_u("T4: a threaded add stores each distinct key exactly once", al_count(list), 30); break; }
    }
    expect(true, "T4: 1500 threaded rounds ran");
}

// ---- T5 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t5_budget() {
    using namespace g4;
    const uint64_t total = 16ull << 30, free_ = 15ull << 30;
    BudgetReport r = budget_report(false, false, false, 0, total, free_);
    expect(r.flags == 0u && r.cap == 0ull && r.avail == 0ull, "T5: the switch OFF reports no budget");
    r = budget_report(false, false, true, 0, total, free_);
    expect(r.flags == 0u && r.cap == 0ull && r.avail == 0ull, "T5: ... even for an APP flag");
    r = budget_report(true, true, false, 0, total, free_);
    expect(r.flags == 0u && r.cap == 0ull && r.avail == 0ull, "T5: WindowServer's session has no budget");
    r = budget_report(true, false, false, 0, total, free_);
    expect(r.flags == (uint32_t)N48N_BUDGET_VALID && r.cap == total / 4 && r.avail == total / 4, "T5: another session: the 25 % cap, all of it available");
    r = budget_report(true, false, true, 1ull << 30, total, free_);
    expect(r.flags == ((uint32_t)N48N_BUDGET_VALID | (uint32_t)N48N_BUDGET_APP) && r.cap == total / 4 && r.avail == total / 4 - (1ull << 30), "T5: an APP: the APP flag, what the others hold is subtracted");
    r = budget_report(true, false, true, total / 4 + 5, total, free_);
    expect(r.cap == total / 4 && r.avail == 0ull, "T5: over the cap: nothing available (no underflow)");
    r = budget_report(true, false, true, 0, total, 123ull << 20);
    expect(r.avail == (123ull << 20), "T5: the pool's free bytes bound it too");
    r = budget_report(true, false, false, 0, 0, 0);
    expect(r.cap == 0ull && r.avail == 0ull && r.flags == (uint32_t)N48N_BUDGET_VALID, "T5: an empty pool: valid, zero");
    {   // the packing: lo / high words, exact indices
        BudgetReport b { (uint32_t)N48N_BUDGET_VALID | (uint32_t)N48N_BUDGET_APP, 0x0000000123456789ull, 0x00000002AABBCCDDull };
        uint32_t res[8] = { 11, 22, 33, 0, 0, 0, 0, 0 };
        budget_pack(b, res);
        expect(res[0] == 11 && res[1] == 22 && res[2] == 33, "T5: reserved[0..2] (the write counter and the minor) are not touched");
        expect(res[3] == 3u && res[4] == 0x23456789u && res[5] == 1u && res[6] == 0xAABBCCDDu && res[7] == 2u, "T5: flags, cap lo / hi, avail lo / hi land in reserved[3..7]");
        expect((((uint64_t)res[N48N_INFO_R_BUDGET_CAP_LO]) | ((uint64_t)res[N48N_INFO_R_BUDGET_CAP_HI] << 32)) == b.cap, "T5: cap round-trips");
    }
    expect(N48N_INFO_R_BUDGET_FLAGS == 3u && N48N_INFO_R_BUDGET_CAP_LO == 4u && N48N_INFO_R_BUDGET_CAP_HI == 5u && N48N_INFO_R_BUDGET_AVAIL_LO == 6u && N48N_INFO_R_BUDGET_AVAIL_HI == 7u, "T5: the ABI indices");
    expect(sizeof(((n48n_info *)nullptr)->reserved) / sizeof(uint32_t) == 8, "T5: reserved[] has eight words (the struct size is unchanged)");
    expect_u("T5: ABI minor 12", N48N_ABI_MINOR, 12);
}

// ---- T6 -------------------------------------------------------------------------------------------------------------------------------------------------
namespace {
// A model of the visible-VRAM pool, the budget counter and one BO with a CPU mapping, driven by the very rule bo_release's close branch asks.
struct PoolModel {
    bool multi;
    uint64_t poolFree, held, leakedBytes = 0; uint32_t leakedBos = 0;
    PoolModel(bool m, uint64_t total) : multi(m), poolFree(total), held(0) {}
    struct Bo { uint64_t size; uint32_t descRefs; };   // descRefs: the descriptor's retain count (the record's own + one per live user mapping)
    Bo create(uint64_t size, bool mapped) { poolFree -= size; held += size; return Bo{ size, mapped ? 2u : 1u }; }
    void close(const Bo &b) {   // bo_release(kRelClosing) for a visible-VRAM BO
        const bool ret = g4::close_vram_return_ok(multi, true, true, b.descRefs);
        if (ret) { poolFree += b.size; held -= b.size; }
        else { leakedBytes += b.size; leakedBos++; }   // not freed, still counted
    }
};
}
static void t6_close_path() {
    using namespace g4;
    expect(close_vram_return_ok(true, true, true, 1) && close_vram_return_ok(true, true, true, 0), "T6: ON: a BO nobody maps is returned");
    expect(!close_vram_return_ok(true, true, true, 2) && !close_vram_return_ok(true, true, true, 7), "T6: ON: a mapped visible-VRAM BO is NOT returned");
    expect(close_vram_return_ok(false, true, true, 2) && close_vram_return_ok(false, true, true, 99), "T6: OFF: as 0.0.629 - always returned (switch-off identity)");
    expect(close_vram_return_ok(true, false, true, 5) && close_vram_return_ok(true, false, false, 5), "T6: ON: GTT / hi-pool / host BOs (not visible VRAM) are unaffected");
    expect(close_vram_return_ok(true, true, false, 5), "T6: ON: a BO that never had a descriptor (never CPU-mapped) is returned");
    {   // model: client A maps a BO and dies; with the rule the next client cannot be handed A's range
        const uint64_t total = 256ull << 20;
        PoolModel on(true, total);
        auto a = on.create(64ull << 20, true), b = on.create(32ull << 20, false);
        on.close(a); on.close(b);
        expect_u("T6: ON: only the unmapped BO went back to the pool", on.poolFree, total - (64ull << 20));
        expect_u("T6: ON: the mapped BO's bytes are leaked and counted", on.leakedBytes, 64ull << 20);
        expect_u("T6: ON: one BO leaked", on.leakedBos, 1);
        expect_u("T6: ON: the budget keeps counting the leaked bytes", on.held, 64ull << 20);
        // a following client can never be given more than the pool really has
        expect(on.poolFree <= total - on.leakedBytes, "T6: ON: free + leaked never exceeds the pool");
        PoolModel off(false, total);
        auto c = off.create(64ull << 20, true);
        off.close(c);
        expect_u("T6: OFF: the mapped BO is returned (0.0.629 behaviour)", off.poolFree, total);
        expect_u("T6: OFF: nothing leaked", off.leakedBytes, 0);
    }
    {   // a session page marks an APP with 0x800 and changes nothing else
        g2::SessView sv {}; sv.state = g2::kSlotOpen; sv.hello = true; sv.ws = false;
        uint64_t so[g2::kOutN];
        g2::sess_out(1, sv, so);
        const uint64_t plain = so[2];
        sv.app = true;
        g2::sess_out(1, sv, so);
        expect_u("T6: sessstat: an APP session page carries flag 0x800", so[2], plain | 0x800ull);
        expect_u("T6: sessstat: without the APP role the flags are exactly the 0.0.629 value", plain, (uint64_t)g2::kSlotOpen | 0x200ull);
    }
    // the global stat page: bits 20..35 of o[2] carry the leak count; zero changes nothing
    g2::GlobalView gv {}; gv.open = 2; gv.dead = 1; gv.hung = true;
    uint64_t o[g2::kOutN];
    g2::global_out(gv, o);
    expect_u("T6: sessstat 4: with no leak o[2] is exactly the 0.0.629 value", o[2], 2ull | (1ull << 8) | 0x10000ull);
    gv.mappedLeakBos = 3;
    g2::global_out(gv, o);
    expect_u("T6: sessstat 4: the leaked-BO count rides in bits 20..35", o[2], 2ull | (1ull << 8) | 0x10000ull | (3ull << 20));
}

// ---- T7 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t7_defence() {
    using namespace g4;
    for (int app = 0; app <= 1; app++) {
        expect(app_call_allowed(app != 0, kCallOther), "T7: the application calls are open to everyone");
        expect(app_call_allowed(false, kCallReadRegs) && app_call_allowed(false, kCallScanout), "T7: a non-APP session (WindowServer, root) keeps ReadRegs and the scanout calls");
    }
    expect(!app_call_allowed(true, kCallReadRegs) && !app_call_allowed(true, kCallScanout), "T7: an APP session's engine refuses ReadRegs and the scanout set by itself");
    // G2's poisoning and recovery rules apply to an APP session unchanged: it is a non-WindowServer user session (ws false), so a stall it causes is recovered, not latched
    expect(g2::stall_action(true, true, 1, true, false, false, false) == g2::kStallRecover, "T7: a stall owned by an APP session (slot 1, Hello done, not WindowServer) is recovered");
    expect(g2::stall_action(true, true, 1, true, true, false, false) == g2::kStallLatch && g2::stall_action(false, true, 1, true, false, false, false) == g2::kStallLatch, "T7: ... WindowServer's session and the switch OFF still latch");
    expect(g2::poison_rc(true) == g2::kAborted && g2::ctx_state2(false, true) == (uint64_t)(N48N_CTX_QUERY2_RESET | N48N_CTX_QUERY2_GUILTY), "T7: the poisoned APP session gets Aborted and RESET|GUILTY");
}

// ---- T8 -------------------------------------------------------------------------------------------------------------------------------------------------
static void t8_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), engh = slurp(K + "src/amd/native_s1c.h"), cli = slurp(K + "src/Navi48NativeClient.cpp"), clih = slurp(K + "src/Navi48NativeClient.hpp");
    const std::string brg = slurp(K + "src/Navi48Bringup.cpp"), ucl = slurp(K + "src/Navi48UserClient.cpp"), abi = slurp(K + "src/Navi48NativeABI.h"), plist = slurp(K + "Info.plist");
    const std::string cmd = slurp(root + "/tools/pc/navi48test.c"), pol = slurp(K + "src/amd/native_open_policy_pure.h"), g4p = slurp(K + "src/amd/native_g4_pure.h"), g2p = slurp(K + "src/amd/native_g2_pure.h");
    expect(!eng.empty() && !engh.empty() && !cli.empty() && !clih.empty() && !brg.empty() && !ucl.empty() && !abi.empty() && !plist.empty() && !cmd.empty() && !pol.empty() && !g4p.empty() && !g2p.empty(), "T8: every source is readable");
    // version and ABI
    expect(count_of(plist, "<string>0.0.664</string>") == 2 && plist.find("0.0.632") == std::string::npos, "T8: Info.plist is 0.0.664, carried twice");
    expect(engh.find("constexpr uint32_t kN1cKextBuild = 664;") != std::string::npos, "T8: the native client reports build 660");
    expect(abi.find("#define N48N_ABI_MINOR     12u ") != std::string::npos, "T8: ABI minor 12");
    expect(abi.find("#define N48N_BUDGET_VALID (1u << 0)") != std::string::npos && abi.find("#define N48N_BUDGET_APP   (1u << 1)") != std::string::npos, "T8: the budget flags are in the ABI header");
    // the latch: one reader, latched once, needs multi
    expect_u("T8: boot-arg navi48-apps is read in exactly one place in the whole kext", count_of(eng, "PE_parse_boot_argn(\"navi48-apps\""), 1);
    for (const char *f : { "src/Navi48Bringup.cpp", "src/Navi48NativeClient.cpp", "src/Navi48UserClient.cpp", "src/Navi48MetalNub.cpp", "src/dcn/navi48_dcn.cpp" })
        expect(slurp(K + f).find("PE_parse_boot_argn(\"navi48-apps\"") == std::string::npos, "T8: no other source reads navi48-apps");
    const std::string ap = fn_body(eng, "static bool apps_on() {");
    expect(!ap.empty() && ap.find("OSCompareAndSwap(n48native::g4::kLatchUnset") != std::string::npos && ap.find("return n48native::g4::apps_enabled(n48native::g4::latch_is_on(l), multi_on());") != std::string::npos,
           "T8: the latch is a compare-and-swap from Unset and the role also needs the multi-session latch");
    // the open policy wiring
    const std::string init = fn_body(cli, "bool IOAccelNavi48NativeClient::initWithTask(");
    expect(!init.empty(), "T8: initWithTask is found");
    expect(in_order(init, { "clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess", "kauth_cred_getuid(kauth_cred_get())", "amdgpu::n1c_apps_enabled()", "if (appsOn && !admin && uid >= n48native::policy::kAppMinUid) {",
                            "proc_name(proc_selfpid(), comm, (int)sizeof comm);", "onAllow = amdgpu::n1c_app_allowed(comm, &struckOut);", "n48native::policy::app_refine(", "n48native::policy::open_decision(admin, uid", ", uid, appsOn, onAllow);",
                            "if (!d.admit) {", "privileged = true;", "appSession = d.reason == n48native::policy::kReasonApp;" }),
           "T8: the privilege query, the uid, the name lookup (only for an ordinary non-root user with the role on), the refinement of open_decision's verdict, the refusal, then the role");
    expect_u("T8: the allow-list is consulted in exactly one place", count_of(cli, "n1c_app_allowed("), 1);
    expect_u("T8: the opener's name is read in exactly one place", count_of(cli, "proc_name(proc_selfpid(), comm"), 1);
    expect(init.find("comm[N48A_COMM_MAX] = 0;") != std::string::npos, "T8: the name is NUL-terminated by hand");
    expect_u("T8: appSession is written in exactly one place", count_of(cli, "appSession = "), 1);
    expect(clih.find("bool           appSession { false };") != std::string::npos, "T8: appSession defaults to false (least privilege)");
    expect(in_order(init, { "if (d.reason != n48native::policy::kReasonAppNotAllowed) {", "amdgpu::n1c_log_gate(&suppressed)", "NCLOG(\"open refused: uid %u, reason: %s\"" }),
           "T8: a refused unlisted app is not logged by the refusal line (the rate-limited app-open line does it), and that line itself sits behind the rate limit");
    expect(init.find("gAppOpenLogged < 64u") != std::string::npos, "T8: the app-open line is rate limited");
    // create(): the mark
    const std::string cr = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::create(");
    expect(in_order(cr, { "amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession, &ref);", "uc->sessSlot = ref.slot; uc->sessId = ref.id;", "if (uc->appSession) {", "amdgpu::n1c_mark_app(ref, uc->appKey);",
                          "amdgpu::n1c_close(ref, \"app mark refused\"); uc->release(); return mrc;", "uc->opened = true;", "if (!uc->attach(attachTo))" }),
           "T8: an APP session is marked right after the open, before the client is returned; a refused mark closes it again");
    // the selector gate
    const std::string ext = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::externalMethod(");
    expect(in_order(ext, { "if (!args || !opened) return kIOReturnNotReady;", "if (!n48native::policy::selector_allowed(adminClient, selector)) {", "if (appSession && !n48native::policy::selector_allowed_for_app(selector)) {",
                           "return kIOReturnNotPrivileged;", "switch (selector) {" }),
           "T8: the APP gate sits after the opened check and the WindowServer gate and BEFORE the switch (reachable for every selector)");
    { const size_t a = ext.find("if (appSession && "), b = ext.find("switch (selector) {");
      expect(a != std::string::npos && b != std::string::npos && ext.substr(a, b - a).find("return kIOReturnNotPrivileged;") != std::string::npos, "T8: the APP refusal is NotPrivileged"); }
    // the engine
    expect_u("T8: the APP mark is read in the engine through app_refused (sixteen calls: 0.0.661 adds the five instance-2 scanout selectors)", count_of(eng, "app_refused(ref, "), 16);
    for (const char *f : { "IOReturn n1c_read_regs(", "IOReturn n1c_scan_query(", "IOReturn n1c_scan_acquire(", "IOReturn n1c_scan_register(", "IOReturn n1c_scan_present(", "IOReturn n1c_scan_status(", "IOReturn n1c_scan_release(",
                           "IOReturn n1c_dal_step(", "IOReturn n1c_mode_trial(", "IOReturn n1c_mode_hold(", "IOReturn n1c_mode_release(" }) {
        const std::string b = fn_body(eng, f);
        expect(!b.empty() && in_order(b, { "if (!sess_hello(ref)) return kIOReturnNotReady;", "if (app_refused(ref, n48native::g4::", ")) return kIOReturnNotPrivileged;" }), (std::string("T8: ") + f + " refuses an APP session right after the Hello check").c_str());
    }
    expect(in_order(fn_body(eng, "IOReturn n1c_read_regs("), { "kCallReadRegs", "regs_allowed(" }) && fn_body(eng, "IOReturn n1c_scan_acquire(").find("kCallScanout") != std::string::npos, "T8: ReadRegs uses its own call class, the scanout calls theirs");
    { const std::string aa = fn_body(eng, "bool n1c_app_allowed(const char *comm, bool *struckOut) {");
      expect(in_order(aa, { "*struckOut = false;", "if (!apps_on()) return false;", "n48native::g4::al_contains(gAppAllow, key)", "n48native::g4::strike_refused(n48native::g4::strike_count(gStrikes, key))", "return n48native::g4::app_admit(listed,", }),
             "T8: the allow-list answers false whenever the role is disabled; a listed name with out-of-limit strikes is refused too"); }
    expect(eng.find("static uint64_t gAppAllow[n48native::g4::kMaxEntries];") != std::string::npos, "T8: the list is a static array of kMaxEntries (zero at every boot)");
    expect(slurp(K + "src/Navi48AppKey.h").find("return h != 0ull ? h : 1ull;   /* 0 is the empty slot */") != std::string::npos, "T8: a name key is never 0 (0 is the empty slot)");
    expect(fn_body(eng, "IOReturn n1c_g2_stat(").find("v.ws = s->ws; v.app = s->app;") != std::string::npos && cmd.find("(out[5] & 0x800)") != std::string::npos, "T8: sessstat reports the APP role and navi48test prints it");
    const std::string mk = fn_body(eng, "IOReturn n1c_mark_app(const N1cRef &ref, uint64_t key) {");
    expect(in_order(mk, { "if (!apps_on()) return kIOReturnNotPermitted;", "sess_lock(ref, false);", "s->appKey = key;", "s->app, true", "sess_unlock(s);" }), "T8: n1c_mark_app refuses unless the role is enabled, and marks under the session's lock");
    expect(eng.find("bool      app;") != std::string::npos, "T8: the session record carries the role");
    expect(g2p.find("sess_wipe_keep_lock") != std::string::npos && fn_body(eng, "IOReturn n1c_open(").find("n48native::g2::sess_wipe_keep_lock(s);") != std::string::npos, "T8: every open wipes the record (the APP mark of a previous client cannot survive)");
    // QueryInfo
    const std::string qi = fn_body(eng, "IOReturn n1c_query_info(const N1cRef &ref, n48n_info *o) {");
    expect(in_order(qi, { "o->vram_hi_free =", "const uint64_t poolFree = o->vram_vis_free + o->vram_hi_free;", "n48native::g4::budget_pack(n48native::g4::budget_report(multi_on(), s->ws, __atomic_load_n(&s->app, __ATOMIC_ACQUIRE), __atomic_load_n(&gVramOthers, __ATOMIC_RELAXED), vram_total(), poolFree), o->reserved);",
                          "o->va_low_first" }), "T8: QueryInfo reports the session's budget from the live counters (after the pool numbers, before the VA range)");
    expect(qi.find("o->reserved[2] = N48N_ABI_MINOR;") != std::string::npos, "T8: the minor is still reserved[2]");
    // R6 on close
    { const std::string sh = fn_body(eng, "static bool stall_handle(uint64_t em, uint64_t re) {");
      expect(sh.find("const bool ws = user && __atomic_load_n(&gs->ws, __ATOMIC_ACQUIRE);") != std::string::npos && count_of(sh, "&gs->app,") == 1 &&
             in_order(sh, { "const uint32_t act = stall_action_budgeted(", "if (act == kStallRecover && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {", "n48native::g4::strike_add(gStrikes, gs->appKey)", "if (act == kStallRecover && g2_recover(g, gs)) {" }),
             "T8: the recovery decision keys on WindowServer's flag only (an APP session is recovered like any other non-WindowServer user session); the APP flag is read once, only to COUNT the strike, after the decision and before the recovery runs"); }
    const std::string rl = fn_body(eng, "static void bo_release(Session *s, uint32_t h, RelMode mode) {");
    const size_t ck = rl.find("if (mode == kRelClosing) {"), nk = rl.find("} else if (mode == kRelNormal) {");
    expect(ck != std::string::npos && nk != std::string::npos, "T8: bo_release's closing branch is found");
    if (ck != std::string::npos && nk != std::string::npos) {
        const std::string c = rl.substr(ck, nk - ck);
        expect(in_order(c, { "const bool vramReturnOk = n48native::g4::close_vram_return_ok(apps_on(), b.kind == kBoVis, b.vmd != nullptr, b.vmd != nullptr ? (uint32_t)b.vmd->getRetainCount() : 0u);",
                             "if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }", "else if (b.kind == kBoVis && !vramReturnOk) {", "gMappedLeakBytes += b.size; gMappedLeakBos++;", "else if (b.kind == kBoVis) { gCtx->gmc.vram_alloc.free(b.a); budget_release(b); }" }),
               "T8: the close path reads the descriptor's references BEFORE it drops its own, then leaks a mapped visible-VRAM range instead of freeing it");
        const size_t l0 = c.find("else if (b.kind == kBoVis && !vramReturnOk) {"), l1 = c.find("else if (b.kind == kBoVis) {");
        expect(l0 != std::string::npos && l1 != std::string::npos && c.substr(l0, l1 - l0).find("vram_alloc.free") == std::string::npos && c.substr(l0, l1 - l0).find("budget_release") == std::string::npos,
               "T8: the leak branch neither frees the range nor takes it off the budget (it stays counted)");
        expect_u("T8: the closing branch frees visible VRAM in exactly one place", count_of(c, "vram_alloc.free(b.a)"), 1);
    }
    expect(fn_body(eng, "IOReturn n1c_bo_free(").find("const uint32_t mrc = hung_now() ? kOk : n48native::g2::bofree_mapping_rc(multi_on(), b0.kind == kBoVis") != std::string::npos, "T8: BoFree of a mapped BO is still refused (0.0.629 R6)");
    expect(in_order(fn_body(eng, "IOReturn n1c_g2_stat("), { "g.mappedLeakBos = gMappedLeakBos;", "global_out(g, o);" }), "T8: sessstat 4 reports the leaked-BO count");
    // the verb (six sites)
    expect(in_order(brg, { "if (n48native::g2::is_g2_action(action)) {", "if (n48native::g4::is_g4_action(action)) {", "if (!n48native::g4::native_exempt(amdgpu::n1c_apps_enabled(), action, argScalar)) return kIOReturnBadArgument;",
                           "amdgpu::n1c_app_verb(argScalar, v);", "if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;" }),
           "T8: accel 104 is dispatched after 103, before the native-boot refusal, behind its own exemption");
    expect(in_order(ucl, { "if (n48native::g4::action_admitted(amdgpu::n1c_apps_enabled(), action)) {", "} else", "if (!n48native::g1::action_admitted(amdgpu::n1c_g1_latched_on(), action) && !n48native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {",
                           "if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;" }),
           "T8: the user client lets 104 past the old action bound only with the role on");
    expect(slurp(K + "src/Navi48UserClient.cpp").find("kIOClientPrivilegeAdministrator") != std::string::npos, "T8: the verb's user client is the root-only legacy client");
    expect(cmd.find("else if (what && !strcmp(what, \"appallow\")) in = 104;") != std::string::npos && cmd.find("in == 104 ? \"appallow\" :") != std::string::npos && cmd.find("scdcread appallow fbpublish m6stat m6xstat vramstat\\n") != std::string::npos &&
           cmd.find("appallow add <name>/remove <name>/list/strikes]") != std::string::npos, "T8: navi48test: the table line, the name, the known list and the usage");
    expect(cmd.find("n48a_arg(isAdd ? N48A_OP_ADD : N48A_OP_REMOVE, n48a_comm_key(arg2))") != std::string::npos && cmd.find("n48a_arg(N48A_OP_LIST, arg2 ? strtoull(arg2, NULL, 0) : 0)") != std::string::npos && cmd.find("Navi48AppKey.h") != std::string::npos, "T8: navi48test hashes the name with the kext's own function");
    expect(g4p.find("#include \"../Navi48AppKey.h\"") != std::string::npos && cli.find("#include \"Navi48AppKey.h\"") != std::string::npos, "T8: kext and CLI share one key header");
    // the stat page and policy header keep the old verdicts
    expect(pol.find("constexpr OpenDecision open_decision(bool admin, uint32_t uid, bool wsArg, bool alreadyOpen) {") != std::string::npos, "T8: open_decision keeps its four-argument signature (the third role is a separate refinement)");
    // nothing in the new kernel code writes a hardware register
    for (const std::string &b : { ap, mk, fn_body(eng, "IOReturn n1c_app_verb("), fn_body(eng, "static inline bool app_refused(") })
        expect(b.find("WREG") == std::string::npos && b.find("bar0_memcpy") == std::string::npos && b.find("RREG") == std::string::npos, "T8: the new role code touches no register");
    // banned strings
    // (the needles are assembled from pieces so that this file itself never contains the strings)
    const std::string ban1 = std::string("pipe+0x") + "280", ban2 = std::string("+0x") + "282", ban3 = std::string("+0x") + "299";
    for (const std::string *f : { &eng, &cli, &g4p, &pol, &brg })
        expect(f->find(ban1) == std::string::npos && f->find(ban2) == std::string::npos && f->find(ban3) == std::string::npos, "T8: no forbidden pipe-offset string");
}

// ---- T9 (0.0.641, review HIGH 1): the close path ---------------------------------------------------------------------------------------------------------------
namespace {
// What the kernel does for ONE visible-VRAM BO at session close, with the very pure functions it calls, in the very order it calls them (clientClose -> n1c_unmap_for_close -> n1c_close ->
// bo_release(kRelClosing)): the sweep removes up to unmap_attempts() mappings (only those that can be removed: the rest were handed to another task), then the close frees the range only
// when close_vram_return_ok says the descriptor has no reference beyond the record's.
struct CloseSim { bool freed; bool leaked; uint32_t refsAfterSweep; uint32_t removed; };
CloseSim close_sim(bool appsOn, uint32_t maps, uint32_t removable) {
    uint32_t refs = 1u + maps;   // the record's reference + one per IOMemoryMap made from the descriptor
    CloseSim r { false, false, 0u, 0u };
    const uint32_t n = g4::unmap_attempts(appsOn, true, true, refs);   // the sweep (only runs with apps ON: unmap_attempts is 0 otherwise)
    for (uint32_t i = 0; i < n && r.removed < removable && refs > 1u; i++) { refs--; r.removed++; }   // removeMappingForDescriptor + release of the returned map
    r.refsAfterSweep = refs;
    if (g4::close_vram_return_ok(appsOn, true, true, refs)) r.freed = true; else r.leaked = true;
    return r;
}
}
static void t9_close_path(const std::string &root) {
    using namespace g4;
    expect(kUnmapMaxPerBo == 256u, "T9: the sweep is bounded to 256 removals per BO");
    // pure
    expect(unmap_attempts(false, true, true, 9) == 0u && unmap_attempts(true, false, true, 9) == 0u && unmap_attempts(true, true, false, 9) == 0u && unmap_attempts(true, true, true, 1) == 0u && unmap_attempts(true, true, true, 0) == 0u,
           "T9: no sweep with apps OFF, for non-visible BOs, without a descriptor, or with no mapping");
    expect(unmap_attempts(true, true, true, 2) == 1u && unmap_attempts(true, true, true, 5) == 4u && unmap_attempts(true, true, true, 0xFFFFFFFFu) == kUnmapMaxPerBo, "T9: one removal per reference beyond the record's, bounded");
    // the model over every combination
    uint64_t nOff = 0, nFreedAfterSweep = 0, nLeaked = 0;
    for (int apps = 0; apps <= 1; apps++) for (uint32_t maps = 0; maps <= 6; maps++) for (uint32_t removable = 0; removable <= 6; removable++) {
        const CloseSim r = close_sim(apps != 0, maps, removable);
        expect(r.freed != r.leaked, "T9: a BO is either freed or leaked");
        expect(!(r.freed && apps && r.refsAfterSweep > 1u), "T9: apps ON: a range is NEVER freed while a mapping remains");
        if (!apps) { nOff++; expect(r.freed && r.removed == 0u, "T9: apps OFF (including multisession alone): always freed, no sweep: byte-for-byte the 0.0.632 close"); }
        else if (removable >= maps) { nFreedAfterSweep++; expect(r.freed && r.removed == maps, "T9: apps ON, every mapping removable: the sweep removes them all and the range is freed"); }
        else { nLeaked++; expect(r.leaked && r.refsAfterSweep == 1u + maps - removable, "T9: apps ON, a mapping that cannot be removed: leak-and-count fallback, with the retain count still above the record's"); }
    }
    expect(nOff > 0 && nFreedAfterSweep > 0 && nLeaked > 0, "T9: every branch of the model was reached");
    // the retain-count arithmetic of the real kernel path (pins)
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), cli = slurp(K + "src/Navi48NativeClient.cpp"), g4p = slurp(K + "src/amd/native_g4_pure.h"), engh = slurp(K + "src/amd/native_s1c.h");
    const std::string sw = fn_body(eng, "void n1c_unmap_for_close(const N1cRef &ref, IOUserClient *uc) {");
    expect(!sw.empty(), "T9: n1c_unmap_for_close is found");
    expect(in_order(sw, { "if (uc == nullptr || !apps_on()) return;", "Session *s = sess_lock(ref, false);", "n48native::g4::unmap_attempts(true,", "(uint32_t)vmd->getRetainCount()", "if (n != 0u) vmd->retain();",
                          "sess_unlock(s);", "uc->removeMappingForDescriptor(vmd)", "m->release();", "vmd->release();" }),
           "T9: the sweep runs only with apps ON, counts the references under the session lock, takes its own reference, DROPS the lock, then removes and releases each returned map, then releases its reference");
    expect(sw.find("gCliLock") == std::string::npos && sw.find("vram_alloc") == std::string::npos && sw.find("bo_release") == std::string::npos && sw.find("b.vmd = nullptr") == std::string::npos,
           "T9: the sweep never frees, never touches a pool and takes no shared lock (n1c_close still decides alone whether a range goes back)");
    expect(g4p.find("constexpr bool close_vram_return_ok(bool appsOn,") != std::string::npos && eng.find("close_vram_return_ok(multi_on()") == std::string::npos && count_of(eng, "close_vram_return_ok(apps_on(), ") == 1,
           "T9: the close rule is keyed on apps_on(); multi_on() no longer reaches it");
    // REACHABILITY: the sweep is called from both client close paths, strictly before n1c_close, with this client and its own session
    const std::string cc = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::clientClose() {"), st = fn_body(cli, "void IOAccelNavi48NativeClient::stop(IOService *provider) {");
    expect(in_order(cc, { "if (opened) {", "n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this);", "amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"clientClose\");", "opened = false;" }),
           "T9: clientClose sweeps this session's mappings first, then closes it");
    expect(in_order(st, { "if (opened) {", "n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this);", "amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"stop\");", "opened = false;" }),
           "T9: stop() (a client that never saw clientClose) sweeps too");
    expect(engh.find("void n1c_unmap_for_close(const N1cRef &ref, IOUserClient *uc);") != std::string::npos && engh.find("class IOUserClient;") != std::string::npos, "T9: the engine declares the sweep");
    // the descriptor is the one the user client's mappings were made from: created once, retained per clientMemoryForType call
    expect(in_order(fn_body(eng, "IOReturn n1c_memory_for_handle("), { "if (b.vmd == nullptr) {", "IODeviceMemory::withRange(", "md->retain(); *memory = md;" }), "T9: the BO's ONE descriptor is created once and retained for each mapping caller (refs = 1 + live maps)");
}

// ---- T10 (0.0.641): the "open refused" log rate limit --------------------------------------------------------------------------------------------------------
static void t10_log_gate(const std::string &root) {
    using namespace g4;
    expect(kLogPerSec == 5u, "T10: the limit is 5 lines per second");
    uint64_t w = 0; bool ad = false; uint32_t sup = 0; uint32_t logged = 0;
    for (int i = 0; i < 100; i++) { w = log_gate_step(w, 1000, &ad, &sup); if (ad) logged++; }
    expect_u("T10: 100 refusals inside one second log exactly kLogPerSec lines", logged, kLogPerSec);
    expect(!ad, "T10: the 101st is dropped");
    w = log_gate_step(w, 1001, &ad, &sup);
    expect(ad && sup == 100u - kLogPerSec, "T10: the next second logs again and reports the 95 dropped lines once");
    w = log_gate_step(w, 1001, &ad, &sup);
    expect(ad && sup == 0u, "T10: ... and the report is not repeated");
    logged = 0;
    for (uint64_t sec = 2000; sec < 2010; sec++) for (int i = 0; i < 7; i++) { w = log_gate_step(w, sec, &ad, &sup); if (ad) logged++; }
    expect_u("T10: over 10 seconds of 7 per second, 5 per second are logged", logged, 10u * kLogPerSec);
    w = 0; for (int i = 0; i < 200000; i++) w = log_gate_step(w, 5, &ad, &sup);
    expect(((w >> 48) & 0xFFFFu) == 0xFFFFu, "T10: the suppressed counter saturates at 16 bits, it does not wrap into the second field");
    w = log_gate_step(w, 6, &ad, &sup);
    expect(ad && sup == 0xFFFFu, "T10: ... and is reported as saturated");
    const std::string K = root + "/src/navi48-bringup/";
    const std::string cli = slurp(K + "src/Navi48NativeClient.cpp"), eng = slurp(K + "src/amd/native_s1c.cpp");
    const std::string init = fn_body(cli, "bool IOAccelNavi48NativeClient::initWithTask(");
    expect_u("T10: the 'open refused' line has exactly one call site, behind the gate", count_of(cli, "NCLOG(\"open refused: uid %u"), 1);
    expect(in_order(init, { "if (!d.admit) {", "if (d.reason != n48native::policy::kReasonAppNotAllowed) {", "if (amdgpu::n1c_log_gate(&suppressed)) {", "suppressed by the rate limit", "NCLOG(\"open refused: uid %u" }),
           "T10: REACHABLE: every refusal passes the gate before its line, and the suppressed count is reported first");
    const std::string lg = fn_body(eng, "bool n1c_log_gate(uint32_t *suppressed) {");
    expect(in_order(lg, { "const uint64_t sec = now_ns() / 1000000000ull;", "n48native::g4::log_gate_step(w, sec, &admit, suppressed)", "__atomic_compare_exchange_n(&gLogGate, &w, nw" }), "T10: the gate steps the shared word with a compare-and-swap loop");
    expect(lg.find("return false") == std::string::npos && lg.find("gCliLock") == std::string::npos, "T10: the gate touches no lock and nothing but its own word");
}

// ---- T11 (0.0.641, review MEDIUM): the visible-VRAM pool -----------------------------------------------------------------------------------------------------
namespace {
// The placement decision of n1c_bo_create for a VRAM request, in the very order the kernel asks the pure functions: hi first (apps, non-WindowServer, NO_CPU_ACCESS), the visible cap, the
// visible pool, the hi fallback. visFree / hiFree: whether the pool can satisfy the request.
enum Where { kWhereNone, kWhereVis, kWhereHi };
Where place_sim(bool apps, bool isWs, bool hiOk, bool hiFree, bool visFree, uint64_t held, uint64_t add, uint64_t cap) {
    bool hiDone = g4::hi_first(apps, isWs, hiOk) && hiFree;
    if (!hiDone) {
        if (!g4::appvis_ok(apps, isWs, held, add, cap)) return kWhereNone;
        if (visFree) return kWhereVis;
    }
    if (hiDone || (hiOk && hiFree)) return kWhereHi;
    return kWhereNone;
}
}
static void t11_visible_pool(const std::string &root) {
    using namespace g4;
    expect(appvis_cap_bytes(false, 0) == (64ull << 20) && appvis_cap_bytes(false, 100) == (64ull << 20), "T11: the cap defaults to 64 MiB without the boot-arg");
    expect(appvis_cap_bytes(true, 16) == (16ull << 20) && appvis_cap_bytes(true, 128) == (128ull << 20) && appvis_cap_bytes(true, 64) == (64ull << 20) && appvis_cap_bytes(true, 100) == (100ull << 20), "T11: navi48-appvis=16..128 is the cap in MiB");
    expect(appvis_cap_bytes(true, 15) == (64ull << 20) && appvis_cap_bytes(true, 129) == (64ull << 20) && appvis_cap_bytes(true, 0) == (64ull << 20) && appvis_cap_bytes(true, 0xFFFFFFFFu) == (64ull << 20), "T11: out of range falls back to the default");
    const uint64_t cap = 64ull << 20;
    expect(appvis_ok(true, false, 0, cap, cap) && !appvis_ok(true, false, 1, cap, cap) && appvis_ok(true, false, cap - 4096, 4096, cap) && !appvis_ok(true, false, cap - 4096, 8192, cap), "T11: non-WindowServer: held + add must stay within the cap");
    expect(appvis_ok(true, true, cap * 10, cap * 10, cap) && appvis_ok(false, false, cap * 10, cap * 10, cap), "T11: WindowServer is never capped, and apps OFF has no cap (0.0.632)");
    expect(hi_first(true, false, true) && !hi_first(true, true, true) && !hi_first(true, false, false) && !hi_first(false, false, true), "T11: hi first only for apps ON, non-WindowServer, NO_CPU_ACCESS");
    expect(appvis_counts(true, false, true) && !appvis_counts(true, true, true) && !appvis_counts(false, false, true) && !appvis_counts(true, false, false), "T11: only a non-WindowServer visible placement with apps ON counts");
    // placement decisions
    expect(place_sim(true, false, true, true, true, 0, 4096, cap) == kWhereHi, "T11: apps ON, NO_CPU_ACCESS, both pools free: the hi pool");
    expect(place_sim(true, false, true, false, true, 0, 4096, cap) == kWhereVis, "T11: ... the hi pool full: falls back to visible (under the cap)");
    expect(place_sim(true, false, true, false, true, cap, 4096, cap) == kWhereNone, "T11: ... the hi pool full and the cap reached: out of memory");
    expect(place_sim(true, false, false, true, true, 0, 4096, cap) == kWhereVis, "T11: apps ON, CPU access wanted: visible");
    expect(place_sim(true, false, false, true, true, cap, 4096, cap) == kWhereNone, "T11: ... over the cap: the existing out-of-memory status, although the hi pool is free (it is not CPU-mappable)");
    expect(place_sim(true, true, true, true, true, cap, 4096, cap) == kWhereVis, "T11: WindowServer is placed visible first and is not capped");
    expect(place_sim(false, false, true, true, true, cap * 4, 4096, cap) == kWhereVis, "T11: apps OFF: visible first, as 0.0.632 (also for a NO_CPU_ACCESS BO)");
    expect(place_sim(false, false, true, true, false, 0, 4096, cap) == kWhereHi, "T11: apps OFF: the old hi fallback");
    // sessstat 4
    g2::GlobalView gv {}; gv.open = 2; uint64_t o[g2::kOutN];
    g2::global_out(gv, o);
    expect_u("T11: sessstat 4 with apps OFF: bits 36..63 of o[2] are 0 (the 0.0.640 value)", o[2] >> 36, 0);
    gv.appVisBytes = 40ull << 20; gv.appVisCapMiB = 64;
    g2::global_out(gv, o);
    expect_u("T11: sessstat 4 carries the non-WindowServer visible use in KiB (bits 36..55)", (o[2] >> 36) & 0xFFFFFull, 40ull << 10);
    expect_u("T11: ... and the cap in MiB (bits 56..63)", o[2] >> 56, 64);
    expect_u("T11: ... and the lower bits are untouched", o[2] & 0xFFFFFFFFFull, 2ull);
    gv.appVisBytes = 128ull << 20; gv.appVisCapMiB = 128; g2::global_out(gv, o);
    expect(((o[2] >> 36) & 0xFFFFFull) == (128ull << 10) && (o[2] >> 56) == 128ull, "T11: the largest cap (128 MiB) fits its fields");
    // the real kernel
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), cmd = slurp(root + "/tools/pc/navi48test.c");
    const std::string bc = fn_body(eng, "IOReturn n1c_bo_create(const N1cRef &ref, const n48n_gem_create_in *in, uint64_t out[4]) {");
    expect(in_order(bc, { "vram_budget_ok(multi, s->ws, gVramOthers, p.size, vram_total())", "const bool appsNow = apps_on();", "n48native::g4::hi_first(appsNow, s->ws, p.hiOk)", "vram_alloc_hi.alloc(p.size, p.align, &a)", "if (!hiDone) {",
                          "n48native::g4::appvis_ok(appsNow, s->ws, gVisOthers, p.size, appsNow ? appvis_cap() : 0ull)", "rc = kIOReturnNoMemory; break;", "got = gCtx->gmc.vram_alloc.alloc(p.size, p.align, &a);",
                          "n48native::g4::appvis_counts(appsNow, s->ws, true)) { b.visCounted = 1u; gVisOthers += p.size; }", "} else if (hiDone || (p.hiOk &&", "b.kind = kBoHi;" }),
           "T11: REACHABLE, in this order: the existing 25 % budget, hi first for a non-WindowServer NO_CPU_ACCESS BO, the visible cap (the existing NoMemory), the visible pool, the count, the hi fallback");
    expect(in_order(eng, { "static inline void budget_release(Bo &b) {", "if (b.visCounted != 0u) { gVisOthers =", "b.visCounted = 0u;" }), "T11: freeing a visible BO takes it off the visible counter (a leaked one is never released, so it stays counted)");
    expect_u("T11: navi48-appvis is read in exactly one place", count_of(eng, "PE_parse_boot_argn(\"navi48-appvis\""), 1);
    expect(in_order(fn_body(eng, "static uint64_t appvis_cap() {"), { "PE_parse_boot_argn(\"navi48-appvis\"", "n48native::g4::appvis_cap_bytes(present, v)", "__atomic_store_n(&gAppVisLatch, 1u" }), "T11: the cap comes from the pure function, latched once");
    expect(in_order(fn_body(eng, "IOReturn n1c_g2_stat("), { "if (apps_on()) { g.appVisBytes = gVisOthers; g.appVisCapMiB = (uint32_t)(appvis_cap() >> 20); }", "global_out(g, o);" }), "T11: sessstat 4 reports the use and the cap (apps ON), under the lock that guards the counter");
    expect(cmd.find("(out[5] >> 36) & 0xfffff, (out[5] >> 56));") != std::string::npos && cmd.find("if ((out[5] >> 56) != 0)") != std::string::npos, "T11: navi48test prints the non-WindowServer visible use and its cap");
}

// ---- T12 (0.0.641, review MEDIUM): WindowServer always gets a slot -----------------------------------------------------------------------------------------------
namespace {
// open_pick as it was in 0.0.640 / 0.0.632 (the reference for "apps absent is byte-identical").
g2::OpenPick open_pick_old(bool multi, const uint8_t *state, uint32_t openCount, bool ws, bool g1Holds) {
    using namespace g2;
    if (!multi) return (openCount == 0u && state[0] == kSlotFree) ? OpenPick{ kOk, 0u } : OpenPick{ kExclusive, kNoSlot };
    if (g1Holds) return OpenPick{ kExclusive, kNoSlot };
    if (ws && openCount != 0u) return OpenPick{ kExclusive, kNoSlot };
    for (uint32_t i = 0; i < kMaxSessions; i++) if (state[i] == kSlotFree) return OpenPick{ kOk, i };
    return OpenPick{ kExclusive, kNoSlot };
}
}
static void t12_slots(const std::string &root) {
    using namespace g2;
    uint64_t nOld = 0, nWs = 0, nApp = 0;
    for (int multi = 0; multi <= 1; multi++) for (uint32_t code = 0; code < 81u; code++) for (int ws = 0; ws <= 1; ws++) for (int g1 = 0; g1 <= 1; g1++) for (int apps = 0; apps <= 1; apps++) for (int wsOpen = 0; wsOpen <= 1; wsOpen++) {
        uint8_t st[kMaxSessions]; uint32_t c = code, open = 0, fr = 0;
        for (uint32_t i = 0; i < kMaxSessions; i++) { st[i] = (uint8_t)(c % 3u); c /= 3u; if (st[i] == kSlotOpen) open++; if (st[i] == kSlotFree) fr++; }
        const OpenPick p = open_pick(multi != 0, st, open, ws != 0, g1 != 0, apps != 0, wsOpen != 0);
        if (!apps) {   // (iii) apps absent: identical to 0.0.640 / 0.0.632, whatever wsOpen says
            nOld++; const OpenPick q = open_pick_old(multi != 0, st, open, ws != 0, g1 != 0);
            expect(p.rc == q.rc && p.slot == q.slot, "T12 (iii): apps absent: open_pick is the old function, every input");
            const OpenPick d = open_pick(multi != 0, st, open, ws != 0, g1 != 0);
            expect(d.rc == q.rc && d.slot == q.slot, "T12 (iii): the default arguments are the old behaviour");
            continue;
        }
        if (!multi) continue;
        if (g1) { expect(p.rc != kOk, "T12: a G1 test context still refuses every open"); continue; }
        const bool wsConsistent = !wsOpen || open != 0u;   // a WindowServer session open implies some slot is open
        if (!wsConsistent) continue;
        if (ws) {
            nWs++;
            expect((p.rc == kOk) == (!wsOpen && fr >= 1u), "T12 (i): a WindowServer open is admitted exactly when no WindowServer session is open and a slot is free, whatever else is open");
            if (p.rc == kOk) expect(st[p.slot] == kSlotFree, "T12: ... into a FREE slot");
        } else {
            nApp++;
            expect((p.rc == kOk) == (fr >= 1u && (wsOpen || fr >= 2u)), "T12 (i): a non-WindowServer open is admitted unless it would take the last free slot while no WindowServer session is open");
            if (p.rc == kOk) { bool lowest = true; for (uint32_t i = 0; i < p.slot; i++) if (st[i] == kSlotFree) lowest = false; expect(lowest && st[p.slot] == kSlotFree, "T12: the lowest free slot"); }
        }
    }
    expect(nOld > 0 && nWs > 0 && nApp > 0, "T12: every branch of the matrix was reached");
    // (i) scenario: WindowServer restarts while three non-WindowServer sessions are open
    { uint8_t st[kMaxSessions] = { kSlotFree, kSlotOpen, kSlotOpen, kSlotOpen };   // WindowServer (slot 0) just closed; three apps remain
      const OpenPick p = open_pick(true, st, 3u, true, false, true, false);
      expect(p.rc == kOk && p.slot == 0u, "T12 (i): WindowServer restarts while 3 non-WindowServer sessions are open: it gets the free slot");
      const OpenPick o = open_pick(true, st, 3u, true, false, false, false);
      expect(o.rc != kOk, "T12 (iii): ... and with apps absent it is still refused, as in 0.0.640"); }
    // the reserve keeps one slot free: every sequence of non-WindowServer opens from an empty table stops at 3 sessions while no WindowServer session is open
    { uint8_t st[kMaxSessions] = { kSlotFree, kSlotFree, kSlotFree, kSlotFree }; uint32_t open = 0, ok = 0;
      for (int i = 0; i < 8; i++) { const OpenPick p = open_pick(true, st, open, false, false, true, false); if (p.rc == kOk) { st[p.slot] = kSlotOpen; open++; ok++; } }
      expect_u("T12 (i): with no WindowServer session, non-WindowServer opens stop at three: the reserve keeps one slot free", ok, 3);
      const OpenPick w = open_pick(true, st, open, true, false, true, false);
      expect(w.rc == kOk && w.slot == 3u, "T12 (i): ... and WindowServer then gets that slot"); }
    // random lifetimes: the reserve holds under any open/close order (no DEAD slots)
    { uint32_t seed = 12345u; uint8_t st[kMaxSessions] = { 0, 0, 0, 0 }; bool wsOpen = false; int wsSlot = -1; uint32_t open = 0; uint64_t wsRefusedWithFree = 0, wsTry = 0;
      for (int step = 0; step < 20000; step++) {
          seed = seed * 1103515245u + 12345u; const uint32_t r = (seed >> 16) & 7u;
          if (r < 3u) { const OpenPick p = open_pick(true, st, open, false, false, true, wsOpen); if (p.rc == kOk) { st[p.slot] = kSlotOpen; open++; } }
          else if (r < 5u) { wsTry++; const OpenPick p = open_pick(true, st, open, true, false, true, wsOpen); if (p.rc == kOk) { st[p.slot] = kSlotOpen; open++; wsOpen = true; wsSlot = (int)p.slot; } else if (!wsOpen && free_slots(st) > 0u) wsRefusedWithFree++; }
          else { uint32_t k = (seed >> 8) % kMaxSessions; if (st[k] == kSlotOpen) { st[k] = kSlotFree; open--; if (wsOpen && (int)k == wsSlot) { wsOpen = false; wsSlot = -1; } } }
          if (!wsOpen) expect(free_slots(st) >= 1u, "T12: with no WindowServer session open, a slot is always free for it");
      }
      expect(wsTry > 0 && wsRefusedWithFree == 0, "T12: WindowServer is never refused while it has no session and a slot is free"); }
    // (ii) the WindowServer identity rule is unchanged: uid 88 behind navi48-metal-ws, nobody else
    { using namespace policy;
      for (uint32_t uid : { 0u, 87u, 88u, 89u, 501u }) for (int ws = 0; ws <= 1; ws++) for (int al = 0; al <= 1; al++) {
          const OpenDecision d = open_decision(false, uid, ws != 0, al != 0);
          const bool want = uid == 88u && ws && !al;
          expect(d.admit == want && (!want || d.reason == kReasonWindowServer), "T12 (ii): the WindowServer identity rule (uid 88, boot-arg, alreadyOpen) is unchanged");
          const OpenDecision r = app_refine(d, uid, true, true);
          if (uid == 88u) expect(r.admit == d.admit && r.reason == d.reason, "T12 (ii): app_refine never touches uid 88");
      } }
    // the policy's alreadyOpen input
    expect(ws_already_open(false, true, false) && !ws_already_open(false, false, true) && ws_already_open(true, false, true) && !ws_already_open(true, true, false), "T12: alreadyOpen: any native client with apps OFF, only a WindowServer session with apps ON");
    // the real kernel
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), cli = slurp(K + "src/Navi48NativeClient.cpp"), g2p = slurp(K + "src/amd/native_g2_pure.h");
    expect(in_order(fn_body(eng, "IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {"), { "const bool appsNow = apps_on(), wsOpen = __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;",
                          "const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), appsNow, wsOpen);", "if (pk.rc != kOk) {", "if (ws) __atomic_store_n(&gSessWs, 1u, __ATOMIC_SEQ_CST);" }),
           "T12: REACHABLE: n1c_open hands open_pick the live apps flag and the live WindowServer flag (gSessWs, written with the open itself) under gCliLock");
    expect(in_order(fn_body(cli, "bool IOAccelNavi48NativeClient::initWithTask("), { "const bool appsOn = amdgpu::n1c_apps_enabled();", "n48native::g2::ws_already_open(appsOn, amdgpu::n1c_is_open(), amdgpu::n1c_ws_is_open())", "uid, appsOn, onAllow);" }),
           "T12: REACHABLE: the open policy's alreadyOpen input is ws_already_open(apps, any open, WindowServer open)");
    expect(g2p.find("inline OpenPick open_pick(bool multi, const uint8_t *state, uint32_t openCount, bool ws, bool g1Holds, bool apps = false, bool wsOpen = false) {") != std::string::npos, "T12: open_pick keeps its old five arguments first, the new ones default to OFF");
}

// ---- T13 (0.0.641, review MEDIUM): the strike count --------------------------------------------------------------------------------------------------------------
static void t13_strikes(const std::string &root) {
    using namespace g4;
    expect(kStrikeLimit == 3u && kStrikeSlots == 4u, "T13: three strikes refuse a name; the table has four slots");
    StrikeTab t {};
    const uint64_t a = n48a_comm_key("evil-app"), b = n48a_comm_key("good-app");
    expect(strike_count(t, a) == 0u && !strike_refused(0u) && app_admit(true, 0u), "T13: no strikes at boot");
    expect(strike_add(t, a) == 1u && strike_add(t, a) == 2u, "T13: strikes accumulate per name");
    expect(app_admit(true, strike_count(t, a)) && strike_count(t, b) == 0u, "T13: 2 strikes: still admitted; another name is not touched");
    expect(strike_add(t, a) == 3u && strike_refused(strike_count(t, a)) && !app_admit(true, strike_count(t, a)), "T13: the third strike refuses the name");
    expect(app_admit(true, strike_count(t, b)) && !app_admit(false, 0u), "T13: ... only that name; a name not on the list is never admitted");
    expect(strike_add(t, b) == 1u && strike_count(t, a) == 3u, "T13: a second name gets its own slot");
    StrikeTab f {}; for (uint64_t k = 1; k <= kStrikeSlots; k++) (void)strike_add(f, k);
    expect(strike_add(f, 99u) == kStrikeLimit && strike_refused(strike_add(f, 99u)), "T13: a full table fails closed (cannot happen: the per-boot recovery budget is smaller than the table)");
    expect(strike_add(f, 0u) == kStrikeLimit && strike_count(f, 0u) == 0u, "T13: key 0 is never a name");
    { StrikeTab m {}; std::vector<std::thread> th; for (int i = 0; i < 4; i++) th.emplace_back([&] { for (int j = 0; j < 100; j++) (void)strike_add(m, a); }); for (auto &x : th) x.join();
      expect_u("T13: concurrent strikes for one name are all counted, in one slot", strike_count(m, a), 400); uint32_t used = 0; for (uint32_t i = 0; i < kStrikeSlots; i++) if (m.key[i]) used++; expect_u("T13: ... in exactly one slot", used, 1); }
    // reachability through the real open decision: listed name, three recoveries blamed on it, then its open is refused with kReasonAppNotAllowed
    { using namespace policy; StrikeTab s {}; const OpenDecision base = open_decision(false, 501, false, false);
      auto decide = [&]() { return app_refine(base, 501, true, app_admit(true, strike_count(s, a))); };
      expect(decide().admit && decide().reason == kReasonApp, "T13: before any strike a listed name opens as an APP");
      (void)strike_add(s, a); (void)strike_add(s, a); expect(decide().admit, "T13: ... after 2 strikes still");
      (void)strike_add(s, a); expect(!decide().admit && decide().reason == kReasonAppNotAllowed, "T13: ... after 3: refused with kReasonAppNotAllowed");
      expect(g2::kMaxAutoRecoveries == kStrikeLimit, "T13: three strikes is the whole per-boot recovery budget"); }
    // the verb: strikes op
    expect(args_ok(104, n48a_arg(N48A_OP_STRIKES, 0)) && !args_ok(104, n48a_arg(N48A_OP_STRIKES, 1)) && native_exempt(true, 104, n48a_arg(N48A_OP_STRIKES, 0)) && !native_exempt(false, 104, n48a_arg(N48A_OP_STRIKES, 0)), "T13: appallow strikes is a verb argument (payload 0), only with the role on");
    { uint64_t l[kMaxEntries] = { 0 }, o[kOutN]; StrikeTab s {}; (void)strike_add(s, a); (void)strike_add(s, a); (void)strike_add(s, b);
      expect(verb_run(true, l, n48a_arg(N48A_OP_STRIKES, 0), o, &s) == kAllowOk, "T13: strikes: ok");
      expect(o[2] == N48A_OP_STRIKES && o[4] == a && o[5] == 2 && o[6] == b && o[7] == 1 && o[8] == 0 && o[9] == 0 && o[12] == kStrikeLimit && o[3] == kMaxEntries, "T13: strikes: o[4 + 2i] / o[5 + 2i] = key / count of table slot i, o[12] = the limit");
      expect(verb_run(false, l, n48a_arg(N48A_OP_STRIKES, 0), o, &s) == kAllowOff, "T13: strikes: OFF with the role disabled");
      expect(verb_run(true, l, n48a_arg(N48A_OP_LIST, 0), o) == kAllowOk && o[2] == N48A_OP_LIST, "T13: list still works without a strike table argument"); }
    // the real kernel
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), cli = slurp(K + "src/Navi48NativeClient.cpp"), cmd = slurp(root + "/tools/pc/navi48test.c");
    const std::string sh = fn_body(eng, "static bool stall_handle(uint64_t em, uint64_t re) {");
    expect(in_order(sh, { "if (act == kStallRecover && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {", "n48native::g4::strike_add(gStrikes, gs->appKey)", "N1C_LOG(\"stall: strike %u of %u", "if (act == kStallRecover && g2_recover(g, gs)) {" }),
           "T13: REACHABLE: an automatic recovery blamed on an APP session adds a strike for its name before the recovery runs");
    expect(in_order(fn_body(eng, "IOReturn n1c_mark_app(const N1cRef &ref, uint64_t key) {"), { "s->appKey = key;", "__atomic_store_n(&s->app, true, __ATOMIC_RELEASE);" }), "T13: the session's name key is recorded before the APP flag is published");
    expect(in_order(fn_body(cli, "bool IOAccelNavi48NativeClient::initWithTask("), { "onAllow = amdgpu::n1c_app_allowed(comm, &struckOut);", "appKey = n48a_comm_key(comm);", "struckOut ? \"REFUSED: 3 strikes", "uid, appsOn, onAllow);" }),
           "T13: REACHABLE: a struck-out name reaches app_refine as 'not allowed' (kReasonAppNotAllowed) and is logged by name");
    expect(eng.find("verb_run(apps_on(), gAppAllow, arg, o, &gStrikes)") != std::string::npos && eng.find("static n48native::g4::StrikeTab gStrikes;") != std::string::npos, "T13: the verb shows the kernel's table (zero at every boot)");
    expect(cmd.find("N48A_OP_STRIKES") != std::string::npos && cmd.find("strikes") != std::string::npos, "T13: navi48test can print the strike table");
}

// ---- T14 (0.0.641): wording, and the ungated remainder ----------------------------------------------------------------------------------------------------------
static void t14_wording(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    for (const char *f : { "src/amd/native_g4_pure.h", "src/Navi48NativeClient.cpp", "src/Navi48NativeClient.hpp", "src/Navi48AppKey.h", "src/amd/native_open_policy_pure.h" }) {
        const std::string t = slurp(K + f);
        // (Navi48NativeClient.cpp keeps ONE older line from 0.0.612, the uid-88 selector refusal's log text, pinned by native_ws_open_test: not G4's, not changed)
        expect(count_of(t, "non-admin") == (std::string(f) == "src/Navi48NativeClient.cpp" ? 1u : 0u) && t.find("not an administrator") == std::string::npos, (std::string("T14: ") + f + " says non-root, not non-admin").c_str());
    }
    expect(std::strcmp(policy::open_reason_text(policy::kReasonNotPrivileged), "not root (euid != 0)") == 0, "T14: the refusal text says not root");
    const std::string g4p = slurp(K + "src/amd/native_g4_pure.h");
    expect(g4p.find("close_vram_return_ok and the close-time unmap sweep are keyed on apps_on()") != std::string::npos && g4p.find("BoFree / close behave as 0.0.629") == std::string::npos, "T14: the header comment says exactly what is gated");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    t1_switch(); t2_open_policy(); t3_selectors(); t4_allowlist(); t5_budget(); t6_close_path(); t7_defence(); t8_pins(root); t9_close_path(root); t10_log_gate(root); t11_visible_pool(root); t12_slots(root); t13_strikes(root); t14_wording(root);
    std::printf("native_g4: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
