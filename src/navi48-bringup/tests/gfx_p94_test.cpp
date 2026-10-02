// gfx_p94_test.cpp — build 0.0.538: switch 94 (gfx_p94.h, present-time promotion without gXdLock), switch 95
// (gfx_p95.h, replay a held present), the glass instruments, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_p94_test.cpp -o /tmp/p94 && /tmp/p94 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/DisplayPipeGuard.cpp \
//         src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/apple/gfx_commit.h \
//         src/navi48-bringup/src/apple/gfx_p94.h src/navi48-bringup/src/apple/gfx_p95.h
// Covers:
//   T1 94 basics: PENDING + spared + the P's own fence OURS -> promoted and copied at the present that asked; not yet -> held;
//      a read failure; the one-read bound; no lock of the model's gXdLock is ever asked by 94;
//   T2 94's guard: WITHDRAWN / REFUSED / UNKNOWN / COMMITTED / not-yet-spared slots are never asked, never read for, never marked;
//   T3 94's entry: no entry, not live (NOT_RUN / EXPIRED / NOPED / RETIRED / fence-less), torn (the entry changed around its fence
//      fields), an entry that ended between the snapshot and the mark; ANOTHER seq's entry OURS never promotes (seq equality);
//      a fence value from ANOTHER EPOCH with the same ordinal never promotes (epoch|ordinal); 94 NEVER WRITES THE RING (memcmp);
//   T4 randomized schedules (gate / keystone withdrawal / final spared or not / GPU fence writes in order / the ring's poll / expiry /
//      reclamation / non-P writers / invalidation (unjudged frame, disarm) / presents), with other threads' actions interleaved at
//      EVERY shared access of 94's step and 95's slot read (N48_P94_YIELD), against an oracle that knows each P's true
//      retirement: with 94 and/or 95 ON (and 86 ON or OFF, its lock busy or free) never a copy of a non-retired, withdrawn,
//      refused or not-spared P, never a copy of a P of another plane, never OUT-OF-ORDER (the glass's P seq strictly rises), no
//      COMMITTED slot for a non-retired P and no PENDING / COMMITTED slot for a withdrawn P at any quiet point;
//   T5 OFF IDENTITY: 94 OFF and 95 OFF, the live present (n48_p94_should_copy + the kext's 95-OFF flow) against FROZEN copies of
//      gfx_present73.h and gfx_p86.h at 6af255a0 (tests/frozen/*_6af255a0.h) over random schedules: every decision, reason and
//      slot, and the whole p73 table and 86 counters byte for byte;
//   T6 95: remember / replay / supersede / stale / older than the glass / withdrawn / refused / NOT_RUN / flip mode blocked / only
//      the NEXT present / the newest wins / bounded table / switch 80 ON incomplete; Part E's decision and delivery;
//   T7 94 x 95 interplay: a held plane whose fence landed is replayed at the next present only with 94's replay-time step;
//   T8 the instruments: the per-second window, its cap per arm, the arm restart; the stamps and the two histograms;
//   T9 the lock order: 94's io reads under the MM model lock only, never with gXdLock (model) held, nothing taken under it;
//   T10 every new log line fits the 491-byte body at maximal fields;
//   T11 the kext glue (source pins, with order): OFF at boot, verb-only writers, the mid-arm guard, the present's order (94, then
//       86's unchanged question, then the notes), the io, the retirement stamps, dpg_perform's 95 branch and the pinned 73 line,
//       flip mode's blocked answer, no lock anywhere on the new present-side code.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>

// The frozen 0.0.537 copies first (inside namespace p537, before the live headers, so their calls bind to their own functions),
// over the (unchanged) flight ring at file scope.
#include "gfx_flightring.h"
namespace p537 {
#include "frozen/gfx_present73_6af255a0.h"
#include "frozen/gfx_p86_6af255a0.h"
}
static void (*gYield)(int) = nullptr;
#define N48_P94_YIELD(k) do { if (gYield) gYield(k); } while (0)
#include "gfx_p94.h"
#include "gfx_p95.h"

#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}
static uint64_t gRs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }
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
static size_t at(const std::string &s, const std::string &n) { return s.find(n); }
static std::string between(const std::string &s, const std::string &a, const std::string &b)
{
    const size_t i = s.find(a);
    if (i == std::string::npos) return std::string();
    const size_t j = s.find(b, i + a.size());
    if (j == std::string::npos) return std::string();
    return s.substr(i, j - i);
}
/* The body of the function whose definition line is `head`: from it to the first "\n}\n" after it. */
static std::string fn_body(const std::string &s, const std::string &head)
{
    const size_t i = s.find(head);
    if (i == std::string::npos) return std::string();
    const size_t j = s.find("\n}\n", i);
    return j == std::string::npos ? std::string() : s.substr(i, j + 3 - i);
}

// ================================================================================================================== the model
static const uint64_t KEYS[3] = { 0x10930000ull, 0x11200000ull, 0x11a70000ull };
static const uint64_t FBASE = 0x7f000000ull;   // our fence page (VRAM offset)
static const uint32_t NFS = 8u;                // fence slot positions (reused by ordinal)
enum { F_NONE = 0, F_PEND, F_SPARED, F_NOTRUN, F_WITHDRAWN, F_REFUSED };
struct PInfo { uint64_t key = 0; uint32_t fate = F_NONE, spare = 0, done = 0, want = 0; uint64_t voff = 0; };
struct Model {
    n48_p73 t; n48_p86 c86; n48_p94 c94; n48_p95 c95; n48_fr_ring ring;
    std::vector<PInfo> P;              // by seq
    uint32_t fence[NFS];
    uint32_t epoch = 0x5a5au, ord = 0u;
    uint32_t cur = 0u;                 // the submission between its gate and its final (0 = none)
    std::deque<uint32_t> gpu;          // spared flights, completed in order
    uint32_t xLocked = 0u, xBusy = 0u, xLockCalls = 0u;   // switch 86's gXdLock (model)
    uint32_t mmHeld = 0u, readFailP = 0u, reads94 = 0u, lockOrderBad = 0u, readsUnderX = 0u;
    uint64_t presentNo = 1000u;
    uint32_t glass = 0u, glassOk = 0u; // the oracle's glass P
    uint32_t viol = 0u, violOoo = 0u, violQuiet = 0u, copies = 0u, replays = 0u, promoted94 = 0u;
    const char *lastViol = "";
};
static Model *gM = nullptr;
static void model_reset(Model &m)
{
    std::memset((void *)&m.t, 0, sizeof m.t); std::memset((void *)&m.c86, 0, sizeof m.c86);
    std::memset((void *)&m.c94, 0, sizeof m.c94); std::memset((void *)&m.c95, 0, sizeof m.c95);
    n48_fr_reset(&m.ring);
    m.P.assign(1, PInfo());
    for (uint32_t k = 0; k < NFS; k++) m.fence[k] = (0x1111u << 16) | k;   // a previous boot's values (another epoch)
    m.ord = 0u; m.cur = 0u; m.gpu.clear(); m.xLocked = 0u; m.xBusy = 0u; m.xLockCalls = 0u; m.mmHeld = 0u;
    m.readFailP = 0u; m.reads94 = 0u; m.lockOrderBad = 0u; m.readsUnderX = 0u; m.presentNo = 1000u; m.glass = 0u; m.glassOk = 0u;
    m.viol = m.violOoo = m.violQuiet = m.copies = m.replays = m.promoted94 = 0u; m.lastViol = "";
}
static uint32_t fence_at(Model &m, uint64_t voff, uint32_t *v)
{
    if (voff < FBASE || voff >= FBASE + NFS * 4u || (voff & 3u)) return 0u;
    *v = m.fence[(voff - FBASE) / 4u];
    return 1u;
}
// switch 94's io: the MM-window read only. The model insists it never runs with gXdLock (model) held and takes nothing under MM.
static uint32_t io94_read(void *c, uint64_t voff, uint32_t *v)
{
    Model &m = *(Model *)c;
    m.reads94++;
    if (m.xLocked) m.readsUnderX++;
    if (m.mmHeld) m.lockOrderBad++;
    m.mmHeld = 1u;
    uint32_t ok = (m.readFailP && rnd() % m.readFailP == 0u) ? 0u : fence_at(m, voff, v);
    m.mmHeld = 0u;
    return ok;
}
// switch 86's io: IOLockTryLock(gXdLock) (busy or free) and the same read under it
static uint32_t io86_try(void *c) { Model &m = *(Model *)c; m.xLockCalls++; if (m.xBusy || m.xLocked) return 0u; m.xLocked = 1u; return 1u; }
static void io86_unlock(void *c) { ((Model *)c)->xLocked = 0u; }
static uint32_t io86_read(void *c, uint64_t voff, uint32_t *v)
{
    Model &m = *(Model *)c;
    if (!m.xLocked) m.lockOrderBad++;
    return fence_at(m, voff, v);
}
static uint64_t io94_page(void *) { return FBASE; }   // build 0.0.539 (S2): the model's fence page
static n48_p94_io io94_of(Model &m) { n48_p94_io io = { &io94_read, &m.ring, &m, &io94_page }; return io; }
static n48_p86_io io86_of(Model &m) { n48_p86_io io = { &io86_try, &io86_unlock, &io86_read, &m.ring, &m }; return io; }

// ---- the other threads' actions (each atomic, as their locks make them: the gate, final and polls are serialized) ----
static void act_gate(Model &m, uint64_t key)
{
    if (m.cur) return;
    const uint32_t s = (uint32_t)m.P.size();
    PInfo p; p.key = key;
    const uint32_t commit = rnd() % 10u != 0u;
    if (commit) {
        m.ord++;
        p.want = ((m.epoch & 0xffffu) << 16) | (m.ord & 0xffffu);
        p.voff = FBASE + (uint64_t)(m.ord % NFS) * 4u;
        uint32_t idx = N48_FR_CAPACITY;
        if (!n48_fr_push(&m.ring, s, 1u, m.ord, p.voff, p.want, &idx)) { (void)n48_fr_reclaim(&m.ring); (void)n48_fr_push(&m.ring, s, 1u, m.ord, p.voff, p.want, &idx); }
        p.fate = F_PEND;
        p.spare = rnd() % 8u != 0u;
    } else p.fate = F_REFUSED;
    m.P.push_back(p);
    const uint32_t i = n48_p73_gate_p(&m.t, key, key, commit, s);
    n48_p94_note_gate(&m.c94, i, s, 1u);
    if (!commit) return;
    if (rnd() % 20u == 0u) {             // the keystone (or the token / the guard) withdraws it at the hook
        (void)n48_p73_final(&m.t, s, 0u);
        (void)n48_fr_free_by_seq(&m.ring, s);
        m.P[s].fate = F_WITHDRAWN;
        return;
    }
    (void)n48_fr_mark_committed(&m.ring, s);
    if (m.P[s].spare) m.gpu.push_back(s);   // orig wrote the IB: it may run before the final is recorded
    m.cur = s;
}
static void act_final(Model &m)
{
    if (!m.cur) return;
    const uint32_t s = m.cur;
    m.cur = 0u;
    if (m.P[s].spare) { (void)n48_p73_final(&m.t, s, 1u); m.P[s].fate = F_SPARED; }
    else { (void)n48_p73_final(&m.t, s, 0u); (void)n48_fr_mark_not_run(&m.ring, s); m.P[s].fate = F_NOTRUN; }
}
static void act_gpu(Model &m)
{
    if (m.gpu.empty()) return;
    const uint32_t s = m.gpu.front(); m.gpu.pop_front();
    m.fence[(m.P[s].voff - FBASE) / 4u] = m.P[s].want;
    m.P[s].done = 1u;
}
static void act_poll(Model &m)
{
    uint64_t cur = 0ull;
    for (uint32_t fi = n48_fr_next_poll(&m.ring, &cur); fi < N48_FR_CAPACITY; fi = n48_fr_next_poll(&m.ring, &cur)) {
        uint32_t v = 0u;
        const uint32_t got = fence_at(m, m.ring.e[fi].vram_off, &v);
        const uint32_t seq = m.ring.e[fi].seq;
        if (n48_fr_poll_entry(&m.ring, fi, got, v)) { (void)n48_p73_retired(&m.t, seq); n48_p94_note_ret(&m.c94, &m.t, seq, 2u); }
    }
}
static void act_expire(Model &m)
{
    const uint32_t i = rnd() % N48_FR_CAPACITY;
    if (m.ring.e[i].state == N48_FR_COMMITTED || m.ring.e[i].state == N48_FR_NOT_RUN) m.ring.e[i].state = N48_FR_EXPIRED;
}
static void act_nonp(Model &m) { if (m.cur) return; const uint64_t k = KEYS[rnd() % 3u]; (void)n48_p73_gate_nonp(&m.t, &k, 1u); }
static void act_inval(Model &m) { if (!m.cur) n48_p73_invalidate_all(&m.t); }
static void act_random(Model &m)
{
    switch (rnd() % 16u) {
    case 0: case 1: case 2: act_gate(m, KEYS[rnd() % 3u]); break;
    case 3: case 4: act_final(m); break;
    case 5: case 6: case 7: act_gpu(m); break;
    case 8: case 9: act_poll(m); break;
    case 10: act_expire(m); break;
    case 11: (void)n48_fr_reclaim(&m.ring); break;
    case 12: act_nonp(m); break;
    case 13: if (rnd() % 4u == 0u) act_inval(m); break;
    default: break;
    }
}
static uint32_t gYieldP = 0u;
static void yield_hook(int) { if (gM && gYieldP && rnd() % gYieldP == 0u) { act_random(*gM); if (rnd() % 3u == 0u) act_random(*gM); } }

// ---- the oracle ----
static void viol(Model &m, const char *why) { m.viol++; m.lastViol = why; }
static void check_copy(Model &m, uint32_t s, uint64_t key)
{
    if (!s || s >= m.P.size()) { viol(m, "copy of no P"); return; }
    const PInfo &p = m.P[s];
    if (p.fate != F_SPARED) viol(m, "copy of a P that was not spared (withdrawn / refused / NOT_RUN / pending)");
    if (!p.done) viol(m, "copy of a P whose fence was never written (not retired)");
    if (p.key != key) viol(m, "copy of a P of another plane");
}
static void deliver(Model &m, uint32_t s)
{
    if (m.glassOk && !n48_p73_seq_newer(s, m.glass)) { m.violOoo++; viol(m, "OUT-OF-ORDER"); }
    n48_p73_delivered(&m.t);
    if (m.t.lastDelivered != s) viol(m, "Part E did not record the delivered P");
    m.glass = s; m.glassOk = 1u;
}
static void check_quiet(Model &m)
{
    for (uint32_t i = 0; i < N48_P73_SLOTS; i++) {
        const n48_p73_slot &sl = m.t.s[i];
        if (!sl.key || !sl.seq || sl.seq >= m.P.size()) continue;
        const PInfo &p = m.P[sl.seq];
        if (sl.state == N48_P73_ST_COMMITTED && (p.fate != F_SPARED || !p.done)) { m.violQuiet++; viol(m, "COMMITTED slot for a non-retired P"); }
        if ((sl.state == N48_P73_ST_COMMITTED || sl.state == N48_P73_ST_PENDING) && (p.fate == F_WITHDRAWN || p.fate == F_REFUSED || p.fate == F_NOTRUN)) {
            m.violQuiet++; viol(m, "PENDING / COMMITTED slot for a withdrawn / refused / NOT_RUN P");
        }
    }
}
// THE PRESENT, as the kext runs it: hw_p95_present (95 ON) or dpg_perform's plain question (95 OFF) -> hw_p73_present (94, then
// 86's unchanged question, then the notes) -> the copy (sometimes refused) -> delivered.
static uint32_t gOn86 = 0u, gOn94 = 0u, gOn95 = 0u, gFmBlocked = 0u;
static struct { uint32_t why, slot; } gLast { N48_P73_HOLDS, N48_P73_SLOTS };
static uint32_t model_p73_present(Model &m, uint64_t key)
{
    uint32_t why = N48_P73_HOLDS, i = N48_P73_SLOTS, r94 = 0u;
    const n48_p86_io io86 = io86_of(m); const n48_p94_io io94 = io94_of(m);
    const uint32_t lc0 = m.xLockCalls;
    const uint32_t copy = n48_p94_should_copy(1u, gOn86, gOn94, &m.t, &m.c86, &m.c94, key, &io86, &io94, &why, &i, &r94);
    if (!gOn86 && m.xLockCalls != lc0) viol(m, "gXdLock asked with 86 OFF");
    if (r94 == N48_P94_R_PROMOTED) m.promoted94++;
    gLast.why = copy ? (uint32_t)N48_P73_HOLDS : why; gLast.slot = i;
    return copy;
}
static void model_present(Model &m, uint64_t key)
{
    m.presentNo++;
    uint32_t copy = 0u, seq = 0u; uint64_t src = key;
    if (gOn95) {
        copy = model_p73_present(m, key);
        if (copy) { n48_p95_on_copy(&m.c95); seq = m.t.decided; }
        else {
            const n48_p95_geom g = { key, 0x7e9000ull, 1920u, 1080u, 7680u, 1920u, 1080u, 3u };
            (void)n48_p95_remember(&m.c95, &m.t, gLast.why, gLast.slot, &g, m.presentNo);
            if (gOn94) {
                const uint64_t k = n48_p95_newest_pending(&m.c95, &m.t);
                const n48_p94_io io94 = io94_of(m);
                if (k && k != key && n48_p94_step(&m.t, &m.c94, k, &io94) == N48_P94_R_PROMOTED) m.promoted94++;
            }
            n48_p95_geom o {};
            if (n48_p95_pick(&m.c95, &m.t, gFmBlocked, m.presentNo, &o, &seq)) {
                if (gFmBlocked) viol(m, "a replay while flip mode is blocked");
                if (o.key == key) viol(m, "a replay of the held present itself");
                if (o.len != 0x7e9000ull || o.pw != 1920u || o.swz != 3u) viol(m, "a replay with another geometry");
                if (m.t.decided != seq || !m.t.decidedOk) viol(m, "a replay without Part E's decision");
                src = o.key; copy = 1u; m.replays++;
            }
        }
    } else {
        copy = model_p73_present(m, key);
        if (copy) seq = m.t.decided;
    }
    if (copy) {
        check_copy(m, seq, src);
        m.copies++;
        if (rnd() % 10u != 0u) deliver(m, seq);   // a refused copy (status not delivered) leaves the glass as it was
    }
}

// ================================================================================================================ T1 - T3
static void setup_spared(Model &m, uint64_t key, uint32_t *seqOut)
{
    // one P for `key`: gated, committed, spared by the final; its flight COMMITTED in the ring; the fence not yet written
    for (;;) {
        const uint32_t before = (uint32_t)m.P.size();
        act_gate(m, key);
        if (m.cur == before && m.P[before].spare) { act_final(m); *seqOut = before; return; }
        if (m.cur) act_final(m);
    }
}
static void t_basic()
{
    static Model m; gM = &m; gYield = nullptr; model_reset(m);
    uint32_t s = 0u; setup_spared(m, KEYS[0], &s);
    const uint32_t i = n48_p73_find(&m.t, KEYS[0]);
    expect_u("T1 the P's slot is PENDING and spared", m.t.s[i].state == N48_P73_ST_PENDING && m.t.s[i].spared == s, 1u);
    n48_p94_io io = io94_of(m);
    uint32_t r = n48_p94_step(&m.t, &m.c94, KEYS[0], &io);
    expect_u("T1 the fence not yet written -> NOTYET, one read, still PENDING", r == N48_P94_R_NOTYET && m.reads94 == 1u &&
             m.t.s[i].state == N48_P73_ST_PENDING && m.t.s[i].retired != s, 1u);
    while (!m.P[s].done) act_gpu(m);
    m.readFailP = 1u;
    r = n48_p94_step(&m.t, &m.c94, KEYS[0], &io);
    expect_u("T1 a failed read -> UNREAD, nothing marked", r == N48_P94_R_UNREAD && m.t.s[i].state == N48_P73_ST_PENDING &&
             m.t.s[i].retired != s, 1u);
    m.readFailP = 0u;
    const uint32_t lc0 = m.xLockCalls;
    r = n48_p94_step(&m.t, &m.c94, KEYS[0], &io);
    expect_u("T1 fence OURS -> PROMOTED: COMMITTED, retired marked, counted once in pCommitted", r == N48_P94_R_PROMOTED &&
             m.t.s[i].state == N48_P73_ST_COMMITTED && m.t.s[i].retired == s && m.t.pCommitted == 1u, 1u);
    expect_u("T1 94 never asked the lock (gXdLock model untouched) and read under MM only", m.xLockCalls == lc0 &&
             m.readsUnderX == 0u && m.lockOrderBad == 0u, 1u);
    uint32_t why = 0u, sl = 0u;
    expect_u("T1 ... and the present copies it", n48_p73_present(&m.t, KEYS[0], &why, &sl) == 1u && m.t.decided == s, 1u);
    const uint32_t reads = m.reads94;
    expect_u("T1 a COMMITTED slot is not asked again (no read)", n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_NONE &&
             m.reads94 == reads, 1u);
    // the ring's own poll later: finds the slot COMMITTED -> retireNoSlot, nothing else
    const uint64_t rns = m.t.retireNoSlot, pc = m.t.pCommitted;
    act_poll(m);
    expect_u("T1 the ring's own poll retires the entry later (94 never did) and counts retireNoSlot; no second commit",
             m.t.retireNoSlot == rns + 1u && m.t.pCommitted == pc, 1u);
    expect_u("T1 a key with no slot is not asked", n48_p94_step(&m.t, &m.c94, 0x123000ull, &io), N48_P94_R_NONE);
    expect_u("T1 a COMMITTED slot with a null io: not asked", n48_p94_step(&m.t, &m.c94, KEYS[0], nullptr), N48_P94_R_NONE);
    uint32_t s2 = 0u; setup_spared(m, KEYS[1], &s2);
    const uint64_t a0 = m.c94.asked, n0 = m.c94.r[N48_P94_R_NOENTRY];
    expect_u("T1 a candidate with a null io: asked, NOENTRY, no read", n48_p94_step(&m.t, &m.c94, KEYS[1], nullptr) == N48_P94_R_NOENTRY &&
             m.c94.asked == a0 + 1u && m.c94.r[N48_P94_R_NOENTRY] == n0 + 1u && m.reads94 == reads, 1u);
}
static void t_guard()
{
    static Model m; gM = &m; gYield = nullptr;
    n48_p94_io io;
    const uint32_t states[] = { N48_P73_ST_WITHDRAWN, N48_P73_ST_REFUSED, N48_P73_ST_UNKNOWN, N48_P73_ST_COMMITTED };
    for (uint32_t k = 0; k < 4u; k++) {
        model_reset(m); io = io94_of(m);
        uint32_t s = 0u; setup_spared(m, KEYS[1], &s);
        while (!m.P[s].done) act_gpu(m);                    // the fence reads OURS: only the guard can refuse
        const uint32_t i = n48_p73_find(&m.t, KEYS[1]);
        m.t.s[i].state = states[k];
        const uint32_t ret0 = m.t.s[i].retired;
        const uint64_t asked = m.c94.asked;
        const uint32_t r = n48_p94_step(&m.t, &m.c94, KEYS[1], &io);
        char b[160]; std::snprintf(b, sizeof b, "T2 a %s slot (fence OURS) is never asked, read for, marked or promoted",
                                   n48_p73_state_name(states[k]));
        expect_u(b, r == N48_P94_R_NONE && m.c94.asked == asked && m.reads94 == 0u && m.t.s[i].retired == ret0 &&
                 m.t.s[i].state == states[k], 1u);
    }
    // PENDING, final not seen yet (not spared), fence OURS
    model_reset(m); io = io94_of(m);
    for (;;) { const uint32_t b = (uint32_t)m.P.size(); act_gate(m, KEYS[2]); if (m.cur == b && m.P[b].spare) break; if (m.cur) act_final(m); }
    const uint32_t s = m.cur;
    while (!m.P[s].done) act_gpu(m);
    const uint32_t i = n48_p73_find(&m.t, KEYS[2]);
    expect_u("T2 a PENDING P whose final was not seen (not spared) is never asked, even with its fence OURS",
             n48_p94_step(&m.t, &m.c94, KEYS[2], &io) == N48_P94_R_NONE && m.reads94 == 0u && m.t.s[i].retired == 0u, 1u);
    // a withdrawn P (the final withdrew it): WITHDRAWN, never asked
    (void)n48_p73_final(&m.t, s, 0u);
    expect_u("T2 the hook withdrew it: WITHDRAWN, never asked", n48_p94_step(&m.t, &m.c94, KEYS[2], &io) == N48_P94_R_NONE &&
             m.t.s[i].state == N48_P73_ST_WITHDRAWN && m.t.s[i].retired == 0u && m.reads94 == 0u, 1u);
}
static void t_entry()
{
    static Model m; gM = &m; gYield = nullptr;
    n48_p94_io io;
    // no entry
    model_reset(m); io = io94_of(m);
    uint32_t s = 0u; setup_spared(m, KEYS[0], &s);
    uint32_t idx = 0u; (void)n48_fr_find(&m.ring, s, &idx);
    n48_fr_entry save = m.ring.e[idx];
    m.ring.e[idx].state = N48_FR_FREE;
    expect_u("T3 no ring entry for the P's seq -> NOENTRY, no read", n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_NOENTRY && m.reads94 == 0u, 1u);
    m.ring.e[idx] = save;
    const uint32_t notlive[] = { N48_FR_NOT_RUN, N48_FR_EXPIRED, N48_FR_NOPED, N48_FR_RETIRED, N48_FR_PENDING };
    while (!m.P[s].done) act_gpu(m);
    for (uint32_t k = 0; k < 5u; k++) {
        m.ring.e[idx].state = notlive[k];
        char b[160]; std::snprintf(b, sizeof b, "T3 the P's entry %s (fence OURS) -> NOTLIVE, no read, nothing marked", n48_fr_state_name(notlive[k]));
        const uint32_t rd = m.reads94;
        expect_u(b, n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_NOTLIVE && m.reads94 == rd &&
                 m.t.s[n48_p73_find(&m.t, KEYS[0])].state == N48_P73_ST_PENDING, 1u);
    }
    m.ring.e[idx] = save;
    m.ring.e[idx].want = 0u;
    expect_u("T3 a fence-less entry (want 0) -> NOTLIVE", n48_p94_step(&m.t, &m.c94, KEYS[0], &io), N48_P94_R_NOTLIVE);
    m.ring.e[idx] = save;
    // ANOTHER seq's flight at a LOWER index, COMMITTED and OURS; the P's own not yet (seq equality)
    model_reset(m); io = io94_of(m);
    uint32_t sA = 0u, sB = 0u;
    setup_spared(m, KEYS[1], &sA);
    setup_spared(m, KEYS[0], &sB);
    act_gpu(m);                                            // A (older) done; B not
    while (!m.gpu.empty() && m.gpu.front() != sB) act_gpu(m);
    expect_u("T3 setup: A's fence OURS, B's not", m.P[sA].done == 1u && m.P[sB].done == 0u, 1u);
    expect_u("T3 ANOTHER seq's entry OURS never promotes the P (seq equality) -> NOTYET",
             n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_NOTYET &&
             m.t.s[n48_p73_find(&m.t, KEYS[0])].state == N48_P73_ST_PENDING, 1u);
    // epoch|ordinal: the P's slot holds its ordinal from ANOTHER epoch
    const uint32_t pos = (uint32_t)((m.P[sB].voff - FBASE) / 4u);
    m.fence[pos] = (m.P[sB].want & 0xffffu) | (0x1111u << 16);
    expect_u("T3 the same ordinal from another epoch never promotes (epoch|ordinal) -> NOTYET",
             n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_NOTYET &&
             m.t.s[n48_p73_find(&m.t, KEYS[0])].state == N48_P73_ST_PENDING, 1u);
    m.fence[pos] = (m.P[sB].want & 0xffff0000u) | ((m.P[sB].want + 8u) & 0xffffu);   // a LATER ordinal at the same slot
    expect_u("T3 a later ordinal at the same fence slot (slot reuse) never promotes -> NOTYET",
             n48_p94_step(&m.t, &m.c94, KEYS[0], &io), N48_P94_R_NOTYET);
    // 94 NEVER WRITES THE RING: every outcome, memcmp
    m.fence[pos] = m.P[sB].want;
    n48_fr_ring before; std::memcpy((void *)&before, (const void *)&m.ring, sizeof before);
    const uint32_t r = n48_p94_step(&m.t, &m.c94, KEYS[0], &io);
    expect_u("T3 fence OURS -> PROMOTED", r, N48_P94_R_PROMOTED);
    expect_u("T3 94 NEVER WRITES THE FLIGHT RING (the whole ring byte-identical across a promotion)",
             std::memcmp((const void *)&before, (const void *)&m.ring, sizeof before), 0u);
    // TORN: the entry changes between the two snapshots around its fence fields (interleaved at yield 2 / 3)
    struct Tn { static void h(int k) {
        if (k != 3 || !gM) return;
        for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) if (gM->ring.e[i].state == N48_FR_COMMITTED) { gM->ring.e[i].state = N48_FR_RETIRED; gM->ring.e[i].state = N48_FR_FREE; gM->ring.e[i].seq = 0xdeadu; }
    } };
    model_reset(m); io = io94_of(m);
    uint32_t sT = 0u; setup_spared(m, KEYS[2], &sT); while (!m.P[sT].done) act_gpu(m);
    gYield = &Tn::h;
    const uint32_t rT = n48_p94_step(&m.t, &m.c94, KEYS[2], &io);
    gYield = nullptr;
    expect_u("T3 the entry re-used between its two snapshots -> TORN, no read, nothing marked", rT == N48_P94_R_TORN &&
             m.reads94 == 0u && m.t.s[n48_p73_find(&m.t, KEYS[2])].state == N48_P73_ST_PENDING, 1u);
    // the entry EXPIRED between the read and the mark
    struct Tx { static void h(int k) {
        if (k != 5 || !gM) return;
        for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) if (gM->ring.e[i].state == N48_FR_COMMITTED) gM->ring.e[i].state = N48_FR_EXPIRED;
    } };
    model_reset(m); io = io94_of(m);
    setup_spared(m, KEYS[2], &sT); while (!m.P[sT].done) act_gpu(m);
    gYield = &Tx::h;
    const uint32_t rX = n48_p94_step(&m.t, &m.c94, KEYS[2], &io);
    gYield = nullptr;
    expect_u("T3 the entry EXPIRED between the read and the mark -> NOTLIVE, nothing marked", rX == N48_P94_R_NOTLIVE &&
             m.t.s[n48_p73_find(&m.t, KEYS[2])].state == N48_P73_ST_PENDING && m.t.s[n48_p73_find(&m.t, KEYS[2])].retired != sT, 1u);
    // the ring's poll retired it between the read and the mark: accepted (RETIRED), and exactly one promotion
    struct Tp { static void h(int k) { if (k == 5 && gM) act_poll(*gM); } };
    model_reset(m); io = io94_of(m);
    setup_spared(m, KEYS[2], &sT); while (!m.P[sT].done) act_gpu(m);
    gYield = &Tp::h;
    const uint32_t rP = n48_p94_step(&m.t, &m.c94, KEYS[2], &io);
    gYield = nullptr;
    expect_u("T3 the ring's poll promoted between the read and the mark: 94 raced (not a second commit), COMMITTED once",
             rP == N48_P94_R_RACED && m.t.pCommitted == 1u && m.t.s[n48_p73_find(&m.t, KEYS[2])].state == N48_P73_ST_COMMITTED, 1u);
    // a new P for the plane gated between the CAS and the re-check: reverted, the new P stays PENDING, counted
    struct Tg { static void h(int k) { if (k == 11 && gM) { act_gate(*gM, KEYS[2]); } } };
    model_reset(m); io = io94_of(m);
    setup_spared(m, KEYS[2], &sT); while (!m.P[sT].done) act_gpu(m);
    gYield = &Tg::h;
    const uint32_t rG = n48_p94_step(&m.t, &m.c94, KEYS[2], &io);
    gYield = nullptr;
    const n48_p73_slot &sg = m.t.s[n48_p73_find(&m.t, KEYS[2])];
    expect_u("T3 a new P gated between the CAS and the re-check: LOST, never COMMITTED for the new P", rG == N48_P94_R_LOST &&
             sg.seq != sT && sg.state != N48_P73_ST_COMMITTED, 1u);
    // the mark never overwrites a newer P's own mark
    struct Tm { static void h(int k) {
        if (k != 8 || !gM) return;
        for (uint32_t n = 0; n < 64u && !gM->cur; n++) act_gate(*gM, KEYS[2]);
        act_final(*gM);
        const uint32_t i = n48_p73_find(&gM->t, KEYS[2]);
        __atomic_store_n(&gM->t.s[i].retired, gM->t.s[i].seq, __ATOMIC_SEQ_CST);   // the newer P's poll marked it
    } };
    model_reset(m); io = io94_of(m);
    setup_spared(m, KEYS[2], &sT); while (!m.P[sT].done) act_gpu(m);
    gYield = &Tm::h;
    const uint32_t rM = n48_p94_step(&m.t, &m.c94, KEYS[2], &io);
    gYield = nullptr;
    const n48_p73_slot &sm = m.t.s[n48_p73_find(&m.t, KEYS[2])];
    expect_u("T3 a newer P's mark landed after the guard: RACED, its mark kept", rM == N48_P94_R_RACED && sm.retired == sm.seq &&
             sm.seq != sT, 1u);
}

// ================================================================================================================ T4
static void t_random()
{
    static Model m; gM = &m;
    uint64_t totViol = 0, totCopies = 0, totReplays = 0, totProm = 0, totOoo = 0, totQuiet = 0, lockBad = 0, underX = 0, seeds = 0;
    const char *last = "";
    for (uint32_t cfg = 0; cfg < 16u; cfg++) {
        gOn94 = (cfg & 1u) ? 1u : 0u; gOn95 = (cfg & 2u) ? 1u : 0u; gOn86 = (cfg & 4u) ? 1u : 0u;
        const uint32_t busy = (cfg & 8u) ? 1u : 0u;
        if (!gOn94 && !gOn95) continue;
        for (uint32_t seed = 0; seed < 120u; seed++) {
            gRs = 0x1234567ull * (seed + 1u) + cfg; model_reset(m); seeds++;
            gYieldP = 2u + seed % 5u; gYield = &yield_hook;
            m.readFailP = (seed % 7u == 0u) ? 9u : 0u;
            for (uint32_t step = 0; step < 1500u; step++) {
                m.xBusy = busy && (rnd() % 4u != 0u);
                gFmBlocked = rnd() % 11u == 0u;
                const uint32_t a = rnd() % 10u;
                if (a < 3u) {
                    // present: mostly the plane of the newest gated P (WindowServer presents what it just drew)
                    uint64_t k = KEYS[rnd() % 3u];
                    if (m.P.size() > 1u && rnd() % 3u) k = m.P.back().key;
                    model_present(m, k);
                } else {
                    gYield = nullptr; act_random(m); gYield = &yield_hook;
                }
                if (m.cur == 0u || rnd() % 2u) { gYield = nullptr; check_quiet(m); gYield = &yield_hook; }
            }
            gYield = nullptr;
            totViol += m.viol; totCopies += m.copies; totReplays += m.replays; totProm += m.promoted94; totOoo += m.violOoo;
            totQuiet += m.violQuiet; lockBad += m.lockOrderBad; underX += m.readsUnderX;
            if (m.viol) last = m.lastViol;
        }
    }
    gOn94 = gOn95 = gOn86 = 0u; gFmBlocked = 0u;
    std::printf("      T4: %llu schedules, %llu copies, %llu replays, %llu 94 promotions; last violation: %s\n",
                (unsigned long long)seeds, (unsigned long long)totCopies, (unsigned long long)totReplays, (unsigned long long)totProm, last);
    expect_u("T4 ORACLE: never a copy / replay of a non-retired, withdrawn, refused, NOT_RUN or other-plane P", totViol, 0u);
    expect_u("T4 ORACLE: OUT-OF-ORDER impossible (the glass's P seq strictly rises)", totOoo, 0u);
    expect_u("T4 ORACLE: no quiet COMMITTED slot for a non-retired P, no PENDING/COMMITTED slot for a withdrawn P", totQuiet, 0u);
    expect_u("T4 the lock order held (94's read under MM only, never with gXdLock held; 86's under its lock)", lockBad + underX, 0u);
    expect_u("T4 non-vacuous: copies, replays and 94 promotions all happened", (totCopies > 2000u && totReplays > 50u && totProm > 200u) ? 1u : 0u, 1u);
}

// ================================================================================================================ T5 OFF identity
struct Twin {
    p537::n48_p73 ft; p537::n48_p86 fc;   // frozen
    n48_p73 t; n48_p86 c; n48_p94 c94; n48_p95 c95;   // live
    n48_fr_ring ring;
    uint32_t fence[NFS];
    uint32_t xBusy, xLocked;
};
static Twin *gT = nullptr;
static uint32_t tw_try(void *) { if (gT->xBusy || gT->xLocked) return 0u; gT->xLocked = 1u; return 1u; }
static void tw_unlock(void *) { gT->xLocked = 0u; }
static uint32_t tw_read(void *, uint64_t voff, uint32_t *v)
{
    if (voff < FBASE || voff >= FBASE + NFS * 4u) return 0u;
    *v = gT->fence[(voff - FBASE) / 4u]; return 1u;
}
static void t_identity()
{
    static Twin w; gT = &w; gYield = nullptr;
    uint64_t presents = 0, diffs = 0, copies = 0, held = 0;
    for (uint32_t seed = 0; seed < 80u; seed++) {
        gRs = 0xabcdef12ull + seed * 7919ull;
        std::memset((void *)&w, 0, sizeof w);
        n48_fr_reset(&w.ring);
        uint32_t seq = 0u, ord = 0u, cur = 0u, curSpare = 0u; uint64_t lastKey = 0ull;
        std::deque<uint32_t> gpu; std::vector<uint64_t> wantOf(1, 0ull), voffOf(1, 0ull);
        const uint32_t on86 = seed & 1u;
        for (uint32_t step = 0; step < 2000u; step++) {
            const uint32_t a = rnd() % 12u;
            if (a < 2u && !cur) {        // gate
                seq++; const uint64_t key = KEYS[rnd() % 3u]; const uint32_t commit = rnd() % 8u != 0u; lastKey = key;
                ord++; const uint32_t want = (0x5a5au << 16) | ord; const uint64_t voff = FBASE + (uint64_t)(ord % NFS) * 4u;
                wantOf.push_back(want); voffOf.push_back(voff);
                if (commit) { uint32_t ix; if (!n48_fr_push(&w.ring, seq, 1u, ord, voff, want, &ix)) { (void)n48_fr_reclaim(&w.ring); (void)n48_fr_push(&w.ring, seq, 1u, ord, voff, want, &ix); } (void)n48_fr_mark_committed(&w.ring, seq); }
                (void)p537::n48_p73_gate_p(&w.ft, key, key, commit, seq);
                (void)n48_p73_gate_p(&w.t, key, key, commit, seq);
                if (commit) { cur = seq; curSpare = rnd() % 6u != 0u; if (curSpare) gpu.push_back(seq); }
            } else if (a < 4u && cur) {  // final
                (void)p537::n48_p73_final(&w.ft, cur, curSpare); (void)n48_p73_final(&w.t, cur, curSpare);
                if (!curSpare) (void)n48_fr_mark_not_run(&w.ring, cur);
                cur = 0u;
            } else if (a < 6u && !gpu.empty()) {   // GPU
                const uint32_t s = gpu.front(); gpu.pop_front(); w.fence[(voffOf[s] - FBASE) / 4u] = (uint32_t)wantOf[s];
            } else if (a < 8u) {         // poll
                uint64_t cu = 0ull;
                for (uint32_t fi = n48_fr_next_poll(&w.ring, &cu); fi < N48_FR_CAPACITY; fi = n48_fr_next_poll(&w.ring, &cu)) {
                    uint32_t v = 0u; const uint32_t got = tw_read(nullptr, w.ring.e[fi].vram_off, &v); const uint32_t s = w.ring.e[fi].seq;
                    if (n48_fr_poll_entry(&w.ring, fi, got, v)) { (void)p537::n48_p73_retired(&w.ft, s); (void)n48_p73_retired(&w.t, s); }
                }
                if (rnd() % 3u == 0u) (void)n48_fr_reclaim(&w.ring);
            } else if (a == 8u) {        // non-P writer / invalidation
                const uint64_t k = KEYS[rnd() % 3u];
                if (rnd() % 2u) { (void)p537::n48_p73_gate_nonp(&w.ft, &k, 1u); (void)n48_p73_gate_nonp(&w.t, &k, 1u); }
                else if (!cur) { p537::n48_p73_invalidate_all(&w.ft); n48_p73_invalidate_all(&w.t); }
            } else {                     // present: mostly the plane of the newest gated P
                const uint64_t k = (lastKey && rnd() % 4u) ? lastKey : KEYS[rnd() % 3u];
                w.xBusy = rnd() % 3u == 0u;
                const p537::n48_p86_io fio = { &tw_try, &tw_unlock, &tw_read, &w.ring, nullptr };
                const n48_p86_io lio = { &tw_try, &tw_unlock, &tw_read, &w.ring, nullptr };
                const n48_p94_io l94 = { &tw_read, &w.ring, nullptr, &io94_page };
                uint32_t fw = 0u, fs = 0u, lw = 0u, ls = 0u, r94 = 7u;
                const uint32_t fcopy = p537::n48_p86_should_copy(1u, on86, &w.ft, &w.fc, k, &fio, &fw, &fs);
                // the live present as the kext runs it with 94 OFF and 95 OFF: n48_p94_should_copy (94 OFF) = 86's question
                const uint32_t lcopy = n48_p94_should_copy(1u, on86, 0u, &w.t, &w.c, &w.c94, k, &lio, &l94, &lw, &ls, &r94);
                presents++;
                if (fcopy != lcopy || fw != lw || fs != ls || r94 != N48_P94_R_NONE) diffs++;
                if (lcopy) { copies++; if (rnd() % 5u) { p537::n48_p73_delivered(&w.ft); n48_p73_delivered(&w.t); } } else held++;
            }
            if (std::memcmp((const void *)&w.ft, (const void *)&w.t, sizeof w.t) != 0 ||
                std::memcmp((const void *)&w.fc, (const void *)&w.c, sizeof w.c) != 0) diffs++;
        }
        n48_p94 z; std::memset((void *)&z, 0, sizeof z);
        if (std::memcmp((const void *)&z, (const void *)&w.c94, sizeof z) != 0) diffs++;   // 94 OFF: its state untouched
    }
    expect_u("T5 the frozen and live layouts are the same size", sizeof(p537::n48_p73) == sizeof(n48_p73) && sizeof(p537::n48_p86) == sizeof(n48_p86), 1u);
    std::printf("      T5: %llu presents (%llu copied, %llu held) over 80 schedules\n", (unsigned long long)presents,
                (unsigned long long)copies, (unsigned long long)held);
    expect_u("T5 OFF IDENTITY: every decision, reason, slot, the p73 table and the 86 counters equal 0.0.537's (frozen) byte for byte", diffs, 0u);
    expect_u("T5 ... non-vacuous (copies and holds, 86 ON and OFF)", (copies > 1000u && held > 1000u) ? 1u : 0u, 1u);
}

// ================================================================================================================ T6 / T7
static n48_p95_geom geom_of(uint64_t key) { const n48_p95_geom g = { key, 0x7e9000ull, 1920u, 1080u, 7680u, 1920u, 1080u, 3u }; return g; }
static void t_p95()
{
    static Model m; gM = &m; gYield = nullptr; model_reset(m);
    n48_p95_geom o {}; uint32_t s = 0u;
    // X held UNKNOWN on PENDING (spared, fence not yet), remembered at present 10
    uint32_t sX = 0u; setup_spared(m, KEYS[0], &sX);
    const uint32_t iX = n48_p73_find(&m.t, KEYS[0]);
    uint32_t why = 0u, sl = 0u;
    expect_u("T6 setup: X's present is held UNKNOWN", n48_p73_present(&m.t, KEYS[0], &why, &sl) == 0u && why == N48_P73_HOLD_UNKNOWN, 1u);
    n48_p95_geom gX = geom_of(KEYS[0]); gX.pw = 1919u;
    expect_u("T6 a held UNKNOWN present on a PENDING slot is remembered", n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 10u), 1u);
    expect_u("T6 ... other holds are not (OLDER / NO-MATCH / REFUSED / WITHDRAWN / no slot)",
             n48_p95_remember(&m.c95, &m.t, N48_P73_HOLD_OLDER, sl, &gX, 10u) + n48_p95_remember(&m.c95, &m.t, N48_P73_HOLD_NOMATCH, sl, &gX, 10u) +
             n48_p95_remember(&m.c95, &m.t, N48_P73_HOLD_WITHDRAWN, sl, &gX, 10u) + n48_p95_remember(&m.c95, &m.t, N48_P73_HOLD_UNKNOWN, N48_P73_SLOTS, &gX, 10u), 0u);
    expect_u("T6 at present 11, X still PENDING: no replay", n48_p95_pick(&m.c95, &m.t, 0u, 11u, &o, &s), 0u);
    // X commits (fence + poll); at present 11 it replays with ITS geometry and Part E's decision
    while (!m.P[sX].done) act_gpu(m);
    act_poll(m);
    expect_u("T6 X COMMITTED at its seq", m.t.s[iX].state == N48_P73_ST_COMMITTED && m.t.s[iX].seq == sX, 1u);
    expect_u("T6 flip mode blocked (a restore / A copy pending / engaged OFF): NO replay, the entry kept",
             n48_p95_pick(&m.c95, &m.t, 1u, 11u, &o, &s) == 0u && m.c95.blockedFm == 1u && m.c95.e[0].used == 1u, 1u);
    expect_u("T6 at the NEXT present X replays (its key, its geometry, its seq)", n48_p95_pick(&m.c95, &m.t, 0u, 11u, &o, &s) == 1u &&
             o.key == KEYS[0] && o.pw == 1919u && s == sX && m.t.decided == sX && m.t.decidedOk == 1u, 1u);
    n48_p73_delivered(&m.t);
    expect_u("T6 ... the copy made it the glass's P (Part E)", m.t.lastDelivered == sX && m.t.deliveredOk == 1u, 1u);
    expect_u("T6 ... and it is gone from the table (no second replay)", n48_p95_pick(&m.c95, &m.t, 0u, 12u, &o, &s), 0u);
    // only the NEXT present
    model_reset(m); setup_spared(m, KEYS[1], &sX);
    (void)n48_p73_present(&m.t, KEYS[1], &why, &sl);
    gX = geom_of(KEYS[1]);
    expect_u("T6 remembered (plane 1)", n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 20u), 1u);
    while (!m.P[sX].done) act_gpu(m); act_poll(m);
    expect_u("T6 the same present does not replay its own hold", n48_p95_pick(&m.c95, &m.t, 0u, 20u, &o, &s), 0u);
    expect_u("T6 a present two after the hold does NOT replay (aged, dropped)", n48_p95_pick(&m.c95, &m.t, 0u, 22u, &o, &s) == 0u &&
             m.c95.dropAged == 1u, 1u);
    // older than the glass: never
    model_reset(m); setup_spared(m, KEYS[1], &sX);
    (void)n48_p73_present(&m.t, KEYS[1], &why, &sl);
    expect_u("T6 remembered (plane 1, again)", n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 30u), 1u);
    while (!m.P[sX].done) act_gpu(m); act_poll(m);
    m.t.lastDelivered = sX + 5u; m.t.deliveredOk = 1u;   // the glass already shows a NEWER P
    expect_u("T6 a committed P OLDER than the glass is never replayed (dropped)", n48_p95_pick(&m.c95, &m.t, 0u, 31u, &o, &s) == 0u &&
             m.c95.dropOlder == 1u, 1u);
    m.t.lastDelivered = sX; m.c95.e[0].used = 1u;
    expect_u("T6 ... nor one EQUAL to the glass", n48_p95_pick(&m.c95, &m.t, 0u, 31u, &o, &s), 0u);
    // withdrawn / NOT_RUN after the hold (remembered PENDING, not yet spared)
    const uint32_t outcomes[] = { 0u, 1u };
    for (uint32_t k = 0; k < 2u; k++) {
        model_reset(m);
        for (;;) { const uint32_t b = (uint32_t)m.P.size(); act_gate(m, KEYS[2]); if (m.cur == b) break; }
        const uint32_t sp = m.cur;
        (void)n48_p73_present(&m.t, KEYS[2], &why, &sl);
        expect_u("T6 setup: held UNKNOWN before its final", why, N48_P73_HOLD_UNKNOWN);
        const n48_p95_geom g2 = geom_of(KEYS[2]);
        expect_u("T6 remembered (plane 2, before its final)", n48_p95_remember(&m.c95, &m.t, why, sl, &g2, 40u), 1u);
        m.cur = 0u;
        if (outcomes[k] == 0u) { (void)n48_p73_final(&m.t, sp, 0u); (void)n48_fr_free_by_seq(&m.ring, sp); }   // withdrawn
        else { (void)n48_p73_final(&m.t, sp, 0u); (void)n48_fr_mark_not_run(&m.ring, sp); }                  // walk refused: NOT_RUN
        m.fence[(m.P[sp].voff - FBASE) / 4u] = m.P[sp].want;   // even a fence reading OURS changes nothing
        act_poll(m);
        expect_u(k ? "T6 a remembered P that went NOT_RUN is never replayed (dropped stale)"
                   : "T6 a remembered P that the hook WITHDREW is never replayed (dropped stale)",
                 n48_p95_pick(&m.c95, &m.t, 0u, 41u, &o, &s) == 0u && m.c95.dropStale == 1u && m.t.s[sl].state == N48_P73_ST_WITHDRAWN, 1u);
    }
    // a new P for X after the hold (stale), a non-P writer (UNKNOWN), REFUSED
    model_reset(m); setup_spared(m, KEYS[0], &sX);
    (void)n48_p73_present(&m.t, KEYS[0], &why, &sl);
    gX = geom_of(KEYS[0]);
    expect_u("T6 remembered (plane 0)", n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 50u), 1u);
    while (!m.P[sX].done) act_gpu(m);
    act_gate(m, KEYS[0]); if (m.cur) act_final(m);
    act_poll(m);
    expect_u("T6 a newer P for the plane after the hold: the remembered one is stale, never replayed",
             n48_p95_pick(&m.c95, &m.t, 0u, 51u, &o, &s) == 0u && m.c95.dropStale == 1u, 1u);
    // a copy supersedes
    model_reset(m); setup_spared(m, KEYS[0], &sX);
    (void)n48_p73_present(&m.t, KEYS[0], &why, &sl);
    (void)n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 60u);
    n48_p95_on_copy(&m.c95);
    while (!m.P[sX].done) act_gpu(m); act_poll(m);
    expect_u("T6 a present 73 let copy empties the table (every remembered present is older)", n48_p95_pick(&m.c95, &m.t, 0u, 61u, &o, &s) == 0u &&
             m.c95.supersededByCopy == 1u, 1u);
    // bounded table, eviction of the oldest seq, one entry per plane
    model_reset(m);
    n48_p95 &p = m.c95;
    for (uint32_t k = 0; k < 3u; k++) { uint32_t sk = 0u; setup_spared(m, KEYS[k], &sk); (void)n48_p73_present(&m.t, KEYS[k], &why, &sl);
                                        n48_p95_geom g = geom_of(KEYS[k]); (void)n48_p95_remember(&p, &m.t, why, sl, &g, 70u + k); }
    uint32_t sN = 0u; setup_spared(m, KEYS[0], &sN); (void)n48_p73_present(&m.t, KEYS[0], &why, &sl);
    n48_p95_geom g0 = geom_of(KEYS[0]); (void)n48_p95_remember(&p, &m.t, why, sl, &g0, 73u);
    uint32_t used = 0u; for (uint32_t k = 0; k < N48_P95_ENTS; k++) used += p.e[k].used;
    expect_u("T6 one entry per plane (a newer hold replaces it): 3 in use, replaced 1", used == 3u && p.replaced == 1u, 1u);
    for (uint32_t k = 0; k < 5u; k++) { n48_p95_geom g = geom_of(0x20000000ull + k * 0x1000ull); (void)g; }
    // eviction: fill 4 distinct keys directly (a synthetic fifth plane)
    {
        static Model q; model_reset(q); gM = &q;
        const uint64_t K5[5] = { 0x100000ull, 0x200000ull, 0x300000ull, 0x400000ull, 0x500000ull };
        for (uint32_t k = 0; k < 5u; k++) {
            uint32_t sk = 0u; setup_spared(q, K5[k], &sk); (void)n48_p73_present(&q.t, K5[k], &why, &sl);
            n48_p95_geom g = geom_of(K5[k]); (void)n48_p95_remember(&q.c95, &q.t, why, sl, &g, 80u);
        }
        uint32_t has0 = 0u, n = 0u;
        for (uint32_t k = 0; k < N48_P95_ENTS; k++) if (q.c95.e[k].used) { n++; if (q.c95.e[k].g.key == K5[0]) has0 = 1u; }
        expect_u("T6 a full table (4) evicts the OLDEST seq; nothing allocated", n == 4u && has0 == 0u && q.c95.evicted == 1u, 1u);
        gM = &m;
    }
    // the newest wins; entries not newer than it are dropped
    model_reset(m);
    uint32_t sA = 0u, sB = 0u;
    setup_spared(m, KEYS[0], &sA); (void)n48_p73_present(&m.t, KEYS[0], &why, &sl); n48_p95_geom gA = geom_of(KEYS[0]); (void)n48_p95_remember(&m.c95, &m.t, why, sl, &gA, 90u);
    setup_spared(m, KEYS[1], &sB); (void)n48_p73_present(&m.t, KEYS[1], &why, &sl); n48_p95_geom gB = geom_of(KEYS[1]); (void)n48_p95_remember(&m.c95, &m.t, why, sl, &gB, 90u);
    while (!m.gpu.empty()) act_gpu(m); act_poll(m);
    expect_u("T6 two committed holds of the previous present: the NEWEST seq replays, the older is dropped",
             n48_p95_pick(&m.c95, &m.t, 0u, 91u, &o, &s) == 1u && s == sB && o.key == KEYS[1] && m.c95.dropOlder == 1u, 1u);
    // switch 80 ON, not COMPLETE
    model_reset(m); setup_spared(m, KEYS[0], &sX);
    (void)n48_p73_present(&m.t, KEYS[0], &why, &sl); (void)n48_p95_remember(&m.c95, &m.t, why, sl, &gX, 100u);
    while (!m.P[sX].done) act_gpu(m); act_poll(m);
    m.t.c80on = N48_P73_C80_ON;
    expect_u("T6 switch 80 ON: a P not judged COMPLETE is never replayed", n48_p95_pick(&m.c95, &m.t, 0u, 101u, &o, &s) == 0u &&
             m.c95.dropIncomplete == 1u, 1u);
    // reset (ON) clears `used` only
    m.c95.e[1].used = 1u; m.c95.e[1].seq = 77u; n48_p95_reset_table(&m.c95);
    expect_u("T6 turning 95 ON forgets every entry (only `used` cleared, never torn)", m.c95.e[1].used == 0u && m.c95.e[1].seq == 77u, 1u);
}
static void t_interplay()
{
    static Model m; gM = &m; gYield = nullptr;
    for (uint32_t with94 = 0; with94 < 2u; with94++) {
        model_reset(m);
        gOn95 = 1u; gOn94 = with94; gOn86 = 0u; gFmBlocked = 0u;
        uint32_t sX = 0u; setup_spared(m, KEYS[0], &sX);
        model_present(m, KEYS[0]);                    // X held (PENDING spared), remembered
        while (!m.P[sX].done) act_gpu(m);             // X's fence lands; no poll (the judge is busy)
        uint32_t sY = 0u; for (;;) { const uint32_t b = (uint32_t)m.P.size(); act_gate(m, KEYS[1]); if (m.cur == b) { sY = b; break; } }
        const uint32_t r0 = m.replays;
        model_present(m, KEYS[1]);                    // Y held (its P not even final): the replay question for X
        const uint32_t iX = n48_p73_find(&m.t, KEYS[0]);
        if (with94) expect_u("T7 94 ON: its replay-time step promotes X (fence OURS) and 95 replays X at the next present",
                             m.replays == r0 + 1u && m.t.s[iX].state == N48_P73_ST_COMMITTED && m.glass == sX && m.viol == 0u, 1u);
        else expect_u("T7 94 OFF: X is still PENDING (no poll ran): no replay, nothing shown", m.replays == r0 &&
                      m.t.s[iX].state == N48_P73_ST_PENDING && m.viol == 0u, 1u);
        (void)sY;
    }
    // 94 promoting the CURRENT plane: a normal copy, and the table emptied
    model_reset(m); gOn95 = 1u; gOn94 = 1u;
    uint32_t sA = 0u; setup_spared(m, KEYS[2], &sA);
    model_present(m, KEYS[2]);
    uint32_t sB = 0u; setup_spared(m, KEYS[1], &sB);
    while (!m.gpu.empty()) act_gpu(m);
    const uint32_t c0 = m.copies;
    model_present(m, KEYS[1]);
    expect_u("T7 94 promotes the present's own plane: a normal copy (not a replay) and the table is emptied", m.copies == c0 + 1u &&
             m.t.decided == sB && m.c95.supersededByCopy >= 1u && m.viol == 0u, 1u);
    gOn95 = gOn94 = 0u;
}

// ================================================================================================================ T8 instruments
static void t_instruments()
{
    n48_p94_sec w; std::memset((void *)&w, 0, sizeof w);
    n48_p94_secline L;
    uint32_t lines = 0u;
    for (uint64_t t = 0; t < 500000ull; t += 100000ull) { n48_p94_sec_count(&w, N48_P94_OC_PRESENTS); lines += n48_p94_sec_tick(&w, 0u, t, &L); }
    expect_u("T8 not armed: no line, the totals count", lines == 0u && w.tot[N48_P94_OC_PRESENTS] == 5u, 1u);
    uint64_t t = 1000000ull;
    for (uint32_t k = 0; k < 35u; k++, t += 100000ull) {
        n48_p94_sec_count(&w, N48_P94_OC_PRESENTS); n48_p94_sec_count(&w, k % 2u ? N48_P94_OC_COPIED : N48_P94_OC_UNKNOWN);
        lines += n48_p94_sec_tick(&w, 1u, t, &L);
    }
    expect_u("T8 armed 3.4 s: 3 one-second lines", lines, 3u);
    expect_u("T8 the last line: window 1000 ms, starting 2000 ms after the arm's first present, 10 presents in it",
             L.len_ms == 1000u && L.at_ms == 2000u && L.n[N48_P94_OC_PRESENTS] == 10u && L.line == 3u &&
             L.n[N48_P94_OC_COPIED] + L.n[N48_P94_OC_UNKNOWN] == 10u, 1u);
    for (uint32_t k = 0; k < 300u; k++, t += 1000000ull) { n48_p94_sec_count(&w, N48_P94_OC_PRESENTS); lines += n48_p94_sec_tick(&w, 1u, t, &L); }
    expect_u("T8 at most 240 lines per arm; every other closed window counted suppressed", w.lines == N48_P94_SEC_LINES &&
             w.suppressed == w.windows - N48_P94_SEC_LINES && w.windows > 290u && lines == N48_P94_SEC_LINES, 1u);
    (void)n48_p94_sec_tick(&w, 0u, t, &L); t += 1000000ull;
    (void)n48_p94_sec_tick(&w, 1u, t, &L);
    expect_u("T8 a new arm: a fresh line budget", w.lines, 0u);
    // stamps and histograms
    static n48_p94 c; std::memset((void *)&c, 0, sizeof c);
    static n48_p73 tt; std::memset((void *)&tt, 0, sizeof tt);
    const uint32_t i = n48_p73_gate_p(&tt, 0x1000ull, 0x1000ull, 1u, 7u);
    n48_p94_note_gate(&c, i, 7u, 1000u);
    n48_p94_hist_present(&c, &tt, i, 1040u);
    expect_u("T8 present - gate = 40 us (bucket < 50 us); not yet retired counted", c.hGate.n == 1u && c.hGate.b[1] == 1u && c.notRetired == 1u, 1u);
    n48_p94_note_ret(&c, &tt, 7u, 5000u);
    n48_p94_hist_present(&c, &tt, i, 25000u);
    expect_u("T8 present - retirement = 20 ms (bucket < 100 ms); present - gate 24 ms", c.hRet.n == 1u && c.hRet.b[7] == 1u &&
             c.hGate.b[7] == 1u, 1u);
    {   // a COMMITTED P without a retirement stamp (the expiry poll): counted apart, not in the histogram
        static n48_p94 c2; std::memset((void *)&c2, 0, sizeof c2);
        static n48_p73 t2; std::memset((void *)&t2, 0, sizeof t2);
        const uint32_t j = n48_p73_gate_p(&t2, 0x2000ull, 0x2000ull, 1u, 9u);
        t2.s[j].state = N48_P73_ST_COMMITTED;
        n48_p94_hist_present(&c2, &t2, j, 100u);
        expect_u("T8 COMMITTED without a stamp: retired-unstamped, not in the histogram", c2.retNoStamp == 1u && c2.hRet.n == 0u, 1u);
    }
    (void)n48_p73_gate_p(&tt, 0x1000ull, 0x1000ull, 1u, 8u);   // a new P: the old stamps no longer apply
    n48_p94_hist_present(&c, &tt, i, 26000u);
    expect_u("T8 a new P's present: the old P's stamps are not used (no gate stamp, not retired)", c.noGate == 1u && c.notRetired == 2u, 1u);
    expect_u("T8 the outcome class of every hold reason", n48_p94_oc_of(N48_P73_HOLDS) == N48_P94_OC_COPIED &&
             n48_p94_oc_of(N48_P73_HOLD_UNKNOWN) == N48_P94_OC_UNKNOWN && n48_p94_oc_of(N48_P73_HOLD_OLDER) == N48_P94_OC_OLDER &&
             n48_p94_oc_of(N48_P73_HOLD_REFUSED) == N48_P94_OC_REFUSED && n48_p94_oc_of(N48_P73_HOLD_WITHDRAWN) == N48_P94_OC_WITHDRAWN &&
             n48_p94_oc_of(N48_P73_HOLD_NOMATCH) == N48_P94_OC_NOMATCH && n48_p94_oc_of(N48_P73_HOLD_INCOMPLETE) == N48_P94_OC_INCOMPLETE, 1u);
}

// ================================================================================================================ T12 (0.0.539)
// T12a: 94's step never points its one-dword read outside OUR fence page, whatever the ring entry holds: an entry
// whose vram_off is below / past / straddling the page, unaligned, Apple's live fence page or 0 is refused NOTLIVE before any read,
// the slot untouched; the page's last slot is read; no page (no function, or 0: the ring map not built) refuses every entry.
static void t_s2()
{
    static Model m; gM = &m; gYield = nullptr; model_reset(m);
    n48_p94_io io = io94_of(m);
    uint32_t s = 0u; setup_spared(m, KEYS[0], &s);
    uint32_t idx = 0u; (void)n48_fr_find(&m.ring, s, &idx);
    while (!m.P[s].done) act_gpu(m);   // the P's fence reads OURS
    const n48_fr_entry save = m.ring.e[idx];
    const uint32_t i = n48_p73_find(&m.t, KEYS[0]);
    const uint64_t bad[] = { FBASE - 4u, FBASE + 0x1000u, FBASE + 0xFFEu, FBASE + 2u, 0x4000001000ull, 0ull, FBASE + 0x2000u };
    uint32_t refused = 0u;
    for (uint64_t v : bad) {
        m.ring.e[idx] = save; m.ring.e[idx].vram_off = v;
        const uint32_t rd = m.reads94;
        if (n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_OFFPAGE && m.reads94 == rd && m.t.s[i].state == N48_P73_ST_PENDING)
            refused++;
    }
    expect_u("T12a S2: vram_off below / past / straddling our fence page, unaligned, Apple's live page, 0: OFFPAGE (its own reason), no read (of 7)", refused, 7u);
    expect_u("T12a S2: counted in its own outcome (fix round), not in NOTLIVE", m.c94.r[N48_P94_R_OFFPAGE] >= 7u &&
             std::strcmp(n48_p94_r_name(N48_P94_R_OFFPAGE), "outside the fence page") == 0, 1u);
    m.ring.e[idx] = save; m.ring.e[idx].vram_off = FBASE + 0xFFCu;
    uint32_t rd = m.reads94;
    const uint32_t rLast = n48_p94_step(&m.t, &m.c94, KEYS[0], &io);
    expect_u("T12a S2: the page's LAST slot (page + 0xFFC) is inside: read (the model has no fence there: read failed)",
             rLast == N48_P94_R_UNREAD && m.reads94 == rd + 1u, 1u);
    m.ring.e[idx] = save;
    n48_p94_io noPage = io; noPage.fence_page = nullptr;
    n48_p94_io zeroPage = io; zeroPage.fence_page = [](void *) -> uint64_t { return 0ull; };
    rd = m.reads94;
    expect_u("T12a S2: no fence page (no function / 0: the ring map not built) refuses every entry, no read",
             n48_p94_step(&m.t, &m.c94, KEYS[0], &noPage) == N48_P94_R_OFFPAGE && n48_p94_step(&m.t, &m.c94, KEYS[0], &zeroPage) == N48_P94_R_OFFPAGE &&
             m.reads94 == rd, 1u);
    expect_u("T12a S2: n48_p94_in_page bounds (first slot, last slot, one past, below, unaligned, page 0)",
             n48_p94_in_page(FBASE, FBASE) == 1u && n48_p94_in_page(FBASE + 0xFFCu, FBASE) == 1u && n48_p94_in_page(FBASE + 0x1000u, FBASE) == 0u &&
             n48_p94_in_page(FBASE - 4u, FBASE) == 0u && n48_p94_in_page(FBASE + 1u, FBASE) == 0u && n48_p94_in_page(0x1000u, 0ull) == 0u &&
             n48_p94_in_page(~0ull - 3ull, FBASE) == 0u, 1u);
    expect_u("T12a S2: in the page, the same entry promotes", n48_p94_step(&m.t, &m.c94, KEYS[0], &io) == N48_P94_R_PROMOTED &&
             m.t.s[i].state == N48_P73_ST_COMMITTED, 1u);
}
// T12b: THE REPLAY'S POST-COPY RE-CHECK. X COMMITTED at seq sX with its gate stamp; the identity taken; nothing moved:
// not raced. During the copy: a NEW P gated on X's slot (seq and stamp move), the stamp alone re-written (same seq, another time), an
// invalidation (state moves), a non-P writer: each RACED. A key with no slot or seq 0 is never recorded.
static void t_m1()
{
    static Model m; gM = &m; gYield = nullptr;
    uint32_t raced = 0u, clean = 0u, sq = 0u, gs = 0u, st = 0u;
    for (uint32_t k = 0; k < 5u; k++) {
        model_reset(m);
        uint32_t sX = 0u; setup_spared(m, KEYS[0], &sX);
        while (!m.P[sX].done) act_gpu(m);
        act_poll(m);
        const uint32_t iX = n48_p73_find(&m.t, KEYS[0]);
        if (m.t.s[iX].state != N48_P73_ST_COMMITTED) { expect_u("T12b setup: X COMMITTED", 0u, 1u); return; }
        n48_p95_rid r;
        if (!n48_p95_rid_take(&r, &m.t, m.c94.tm, KEYS[0], sX, 11u) || r.slot != iX || r.seq != sX || r.gseq != sX) {
            expect_u("T12b the identity is taken (slot, seq, the gate stamp of the P)", 0u, 1u); return;
        }
        if (k == 0u) { clean += n48_p95_rid_raced(&r, &m.t, m.c94.tm, &sq, &gs, &st) == 0u ? 1u : 0u; continue; }
        if (k == 1u) { m.cur = 0u; for (;;) { const uint32_t b = (uint32_t)m.P.size(); act_gate(m, KEYS[0]); if ((uint32_t)m.P.size() > b) break; } }
        if (k == 2u) n48_p94_note_gate(&m.c94, iX, sX, 777u);
        if (k == 3u) n48_p73_invalidate_all(&m.t);
        if (k == 4u) { const uint64_t kk = KEYS[0]; (void)n48_p73_gate_nonp(&m.t, &kk, 1u); }
        raced += n48_p95_rid_raced(&r, &m.t, m.c94.tm, &sq, &gs, &st);
    }
    expect_u("T12b M1: nothing moved during the copy -> not raced", clean, 1u);
    expect_u("T12b M1: a new P gated on the slot / the gate stamp re-written / an invalidation / a non-P writer -> RACED (of 4)", raced, 4u);
    n48_p95_rid r2;
    expect_u("T12b M1: no slot for the key, or seq 0 -> not recorded",
             n48_p95_rid_take(&r2, &m.t, m.c94.tm, 0x777000ull, 5u, 1u) == 0u && r2.used == 0u &&
             n48_p95_rid_take(&r2, &m.t, m.c94.tm, KEYS[0], 0u, 1u) == 0u, 1u);
}

// ================================================================================================================ T10 widths
static void t_widths()
{
    char b[2048];
    const unsigned long long M = 0xffffffffffffffffull; const unsigned U = 0xffffffffu;
    const char *how = " - `gfxneuter 94` REFUSED - a continuous arm stands, unchanged";
    n48_wt_hist h0; std::memset((void *)&h0, 0xff, sizeof h0);
    int n = std::snprintf(b, sizeof b, N48_P94_FMT, N48_P94_ON_TXT, how, M, M, M, M);
    expect_u("T10 the switch-94 report line fits 491 bytes at maximal fields", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P94_FMT1B, M, M, M, M, M, M, M, M);
    expect_u("T10 the step-outcome line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P94_FMT2, M, M, M, M, M, M, M, M);
    expect_u("T10 the per-second totals line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P94_FMT3, M, M, M, M, U, M, M, M, M);
    expect_u("T10 the promotions / windows line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n48_p94_secline L; for (uint32_t k = 0; k < N48_P94_OCS; k++) L.n[k] = U;
    L.at_ms = M; L.len_ms = M; L.line = U;
    n = std::snprintf(b, sizeof b, N48_P94_SEC_FMT, N48_P94_SEC_ARGS(L));
    expect_u("T10 the per-second line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P94_LINE_FMT, M, M, U, U, U);
    expect_u("T10 the PROMOTED line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P95_FMT, N48_P95_ON_TXT, " - `gfxneuter 95` REFUSED - a continuous arm stands, unchanged",
                      M, M, M, M, M, M, M);
    expect_u("T10 the switch-95 report line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P95_FMT2, M, M, M, M, M, U);
    expect_u("T10 the switch-95 drop line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P95_LINE_FMT, M, M, M, U, U, U);
    expect_u("T10 the REPLAYED line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_P95_FMT3, M, M, M, M);
    expect_u("T10 (0.0.539 M1) the post-copy re-check report line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    int wr = 0;
    for (uint32_t k = 0; k < 64u; k++) {
        const int w = std::snprintf(b, sizeof b, N48_P95_RACED_FMT, M, M, U, U, U, n48_p73_state_name(k), U, U, U);
        if (w > wr) wr = w;
    }
    expect_u("T10 (0.0.539 M1) the RACED line fits at its widest state name", wr > 0 && (unsigned)wr <= N48_LOG_CAP_BODY, 1u);
    n = std::snprintf(b, sizeof b, N48_WT_FMT, N48_WT_ARGS("present with 95 (hw_p95_present, whole)", &h0));
    expect_u("T10 (0.0.539 S1) the hw_p95_present wall-time line fits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    n48_wt_hist h; std::memset((void *)&h, 0xff, sizeof h);
    n = std::snprintf(b, sizeof b, N48_WT_FMT, N48_WT_ARGS("present minus its P's retirement observed (not a call)", &h));
    expect_u("T10 the two histogram lines fit (the longer name)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
}

// ================================================================================================================ T11 glue
static void t_glue(const char *ahhP, const char *dpgP, const char *nbP, const char *cmP, const char *h94P, const char *h95P)
{
    const std::string s = slurp(ahhP), d = slurp(dpgP), nb = slurp(nbP), cm = slurp(cmP), h94 = slurp(h94P), h95 = slurp(h95P);
    expect_u("T11 the six sources were read", !s.empty() && !d.empty() && !nb.empty() && !cm.empty() && !h94.empty() && !h95.empty(), 1u);
    // OFF at boot, the verbs the only writers
    expect_u("T11 switches 94 and 95 are zero-initialised (OFF at boot), exactly once each",
             count(s, "static n48_p94 gP94 {};") == 1u && count(s, "static n48_p95 gP95 {};") == 1u && N48_P94_OFF == 0u && N48_P95_OFF == 0u, 1u);
    expect_u("T11 94's mode is written only by its verb (ON and OFF)", count(s, "__atomic_store_n(&gP94.on,") == 2u && count(s, "gP94.on =") == 0u, 1u);
    expect_u("T11 95's mode is written only by its verb (ON and OFF)", count(s, "__atomic_store_n(&gP95.on,") == 2u && count(s, "gP95.on =") == 0u, 1u);
    expect_u("T11 M values: 94 M 1 ON, M 2 OFF",
             count(s, "        } else if (m == 1u) {\n            __atomic_store_n(&gP94.on, (uint32_t)N48_P94_ON, __ATOMIC_RELEASE); changed94 = 1;\n"
                      "        } else if (m == 2u) {\n            __atomic_store_n(&gP94.on, (uint32_t)N48_P94_OFF, __ATOMIC_RELEASE); changed94 = 1;"), 1u);
    expect_u("T11 M values: 95 M 1 ON (the table forgotten on OFF -> ON), M 2 OFF",
             count(s, "        } else if (m == 1u) {\n            if (__atomic_load_n(&gP95.on, __ATOMIC_ACQUIRE) != N48_P95_ON) n48_p95_reset_table(&gP95);\n"
                      "            __atomic_store_n(&gP95.on, (uint32_t)N48_P95_ON, __ATOMIC_RELEASE); changed95 = 1;\n"
                      "        } else if (m == 2u) {\n            __atomic_store_n(&gP95.on, (uint32_t)N48_P95_OFF, __ATOMIC_RELEASE); changed95 = 1;"), 1u);
    expect_u("T11 PIN SWITCH-GUARD:94/95 - each verb calls the continuous guard with its own selector, exactly once, before any change",
             count(s, "n48_cm_cont_switch_refused(94u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             count(s, "n48_cm_cont_switch_refused(95u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             at(s, "n48_cm_cont_switch_refused(94u,") < at(s, "__atomic_store_n(&gP94.on,") &&
             at(s, "n48_cm_cont_switch_refused(95u,") < at(s, "__atomic_store_n(&gP95.on,") &&
             count(s, "} else if ((arg & 0xffull) == 94ull) {") == 1u && count(s, "} else if ((arg & 0xffull) == 95ull) {") == 1u, 1u);
    expect_u("T11 gfx_commit.h guards 94 and 95", count(cm, "    case 94u:   /* build 0.0.538") == 1u && count(cm, "    case 95u:   /* build 0.0.538") == 1u, 1u);
    // the present's order
    const std::string pres = fn_body(s, "uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo)");
    const char *r94 = "    const uint32_t r94 = (gP73On && p94_mode() == N48_P94_ON) ? p94_step(phys, presentNo) : (uint32_t)N48_P94_R_NONE;";
    const char *q86 = "    const uint32_t copy = n48_p86_should_copy(gP73On ? 1u : 0u, p86_mode() == N48_P86_ON ? 1u : 0u, &gP73, &gP86, phys, &kP86Io,";
    const char *nt = "    p94_present_note(copy, why, i, r94, gP86.r[N48_P86_R_PROMOTED] != p86Promoted0 ? 1u : 0u);";
    expect_u("T11 hw_p73_present: 94's step (ON only, gated on 73 and 94) BEFORE 86's unchanged question, then the log-only note",
             !pres.empty() && count(pres, r94) == 1u && count(pres, q86) == 1u && count(pres, nt) == 1u &&
             at(pres, r94) < at(pres, q86) && at(pres, q86) < at(pres, nt) && count(pres, "p94_step(") == 1u, 1u);
    expect_u("T11 the step runs the pure n48_p94_step over kP94Io, exactly once in the kext",
             count(s, "    const uint32_t r = n48_p94_step(&gP73, &gP94, key, &kP94Io);") == 1u && count(s, "n48_p94_step(") == 1u, 1u);
    expect_u("T11 the io is {ks_x_read, gKsRing, the fence page}: the expiry poll's and 86's one-dword MM reader over the flight ring, "
             "bounded to our fence page (0.0.539 S2: gRingMap.base + N48_F828_FENCE_PAGE_OFF, 0 unmapped)",
             count(s, "static const n48_p94_io kP94Io = { &ks_x_read, &gKsRing, nullptr, &p94_fence_page };") == 1u &&
             count(s, "static uint64_t p94_fence_page(void *) { return gRingMap.base ? gRingMap.base + (uint64_t)N48_F828_FENCE_PAGE_OFF : 0ull; }") == 1u &&
             count(h94, "        if (!n48_p94_in_page(vo, page)) { *why = N48_P94_R_OFFPAGE; return 0u; }") == 1u &&
             count(s, "(unsigned long long)gP94.r[N48_P94_R_LOST], (unsigned long long)gP94.r[N48_P94_R_OFFPAGE]);") == 1u &&
             at(h94, "        if (!n48_p94_in_page(vo, page))") < at(h94, "        *voff = vo; *want = w;") &&
             count(s, "static uint32_t ks_x_read(void *, uint64_t off, uint32_t *val) { return navi48_vram_read_mm(off, val, 1) ? 1u : 0u; }") == 1u, 1u);
    // no lock on the new present-side code
    const std::string blk = between(s, "static const n48_p94_io kP94Io", "// THE PRESENT SIDE (DisplayPipeGuard.cpp dpg_perform");
    const std::string b95 = between(s, "uint32_t hw_p95_on() {", "// 0.0.369");
    expect_u("T11 no lock and no gXdLock anywhere in 94's / 95's present-side code (the step, the notes, the replay, hw_p95_present)",
             !blk.empty() && !b95.empty() && count(blk + b95, "IOLock") == 0u && count(blk + b95, "gXdLock") == 0u &&
             count(pres, "IOLock") == 0u, 1u);
    // the MM read is a leaf
    expect_u("T11 navi48_vram_read_mm holds gVramMmLock over the register reads only (a leaf: nothing is taken under it)",
             count(nb, "\tuint64_t h0 = 0ull; clock_get_uptime(&h0);   // 0.0.514 A3: the hold starts\n\tfor (uint32_t i = 0; i < dwords; i++)\n"
                       "\t\tdst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4);\n\tif (gVramMmLock) IOLockUnlock(gVramMmLock);") == 1u, 1u);
    // 95's glue
    expect_u("T11 hw_p95_on: 73 AND 95 ON", count(s, "uint32_t hw_p95_on() { return (gP73On && __atomic_load_n(&gP95.on, __ATOMIC_ACQUIRE) == N48_P95_ON) ? 1u : 0u; }"), 1u);
    const std::string h95f = fn_body(s, "uint32_t hw_p95_present(uint64_t *phys, uint64_t *len, uint32_t *pw, uint32_t *ph, uint32_t *stride, uint32_t *surfW,");
    expect_u("T11 hw_p95_present: 73 OFF copies; 73's own answer first; a copy empties the table; a hold goes to the replay only while 95 is "
             "ON; 0.0.539 S1: timed as a whole (wt_ticks first, one exit through wt_note_since(&gWtP95, wt0))",
             !h95f.empty() && at(h95f, "    const uint64_t wt0 = wt_ticks();") < at(h95f, "    if (!gP73On) r = 1u;") &&
             at(h95f, "    if (!gP73On) r = 1u;") < at(h95f, "    else if (hw_p73_present(*phys, presentNo)) {") &&
             at(h95f, "    else if (hw_p73_present(*phys, presentNo)) {") < at(h95f, "        n48_p95_on_copy(&gP95);\n        r = 1u;") &&
             at(h95f, "        n48_p95_on_copy(&gP95);") < at(h95f, "else if (__atomic_load_n(&gP95.on, __ATOMIC_ACQUIRE) != N48_P95_ON) r = 0u;") &&
             at(h95f, "else if (__atomic_load_n(&gP95.on, __ATOMIC_ACQUIRE) != N48_P95_ON) r = 0u;") < at(h95f, "    else r = p95_replay(phys, len, pw, ph, stride, surfW, surfH, swz, presentNo);") &&
             at(h95f, "    else r = p95_replay(") < at(h95f, "    wt_note_since(&gWtP95, wt0);\n    return r;\n}") &&
             count(h95f, "return") == 1u && count(s, "wt_note_since(&gWtP95, wt0);") == 1u &&
             count(s, "HWLOG(N48_WT_FMT, N48_WT_ARGS(\"present with 95 (hw_p95_present, whole)\", &gWtP95));") == 2u, 1u);
    // build 0.0.539: the replay's identity taken right after the pick; re-checked in dpg_perform after the copy
    const std::string ac = fn_body(s, "void hw_p95_after_copy(uint64_t presentNo, uint32_t made)");
    expect_u("T11 M1: the replay's identity (slot, picked seq, gate stamp gP94.tm) is taken right after the pick, before the swap",
             count(s, "    (void)n48_p95_rid_take(&gP95Rid, &gP73, gP94.tm, o.key, s, presentNo);") == 1u &&
             at(s, "if (!n48_p95_pick(") < at(s, "(void)n48_p95_rid_take(&gP95Rid,") && at(s, "(void)n48_p95_rid_take(&gP95Rid,") < at(s, "    *phys = o.key;") &&
             count(s, "n48_p95_rid_take(") == 1u, 1u);
    expect_u("T11 M1: hw_p95_after_copy - nothing pending: one load; another present's record dropped (stale); no copy counted; else "
             "re-checked by n48_p95_rid_raced, a race counted and logged (<= 8 lines)",
             !ac.empty() && at(ac, "    if (!gP95Rid.used) return;") < at(ac, "    gP95Rid.used = 0u;") &&
             at(ac, "    if (rid.presentNo != presentNo) { gP95.recheckStale++; return; }") < at(ac, "    if (!made) { gP95.recheckNoCopy++; return; }") &&
             at(ac, "    if (!made) { gP95.recheckNoCopy++; return; }") < at(ac, "    gP95.rechecked++;") &&
             at(ac, "    gP95.rechecked++;") < at(ac, "    if (!n48_p95_rid_raced(&rid, &gP73, gP94.tm, &seqNow, &gseqNow, &stNow)) return;") &&
             at(ac, "if (!n48_p95_rid_raced(") < at(ac, "    gP95.raced++;") && at(ac, "    gP95.raced++;") < at(ac, "HWLOG(N48_P95_RACED_FMT,") &&
             count(ac, "if (gP95.racedLines < 8u) {") == 1u && count(ac, "IOLock") == 0u && count(ac, "gXdLock") == 0u, 1u);
    expect_u("T11 the replay asks flip mode's blocked answer and the NEXT-present rule (the perform number)",
             count(s, "    if (!n48_p95_pick(&gP95, &gP73, navi48_fm_replay_blocked(), presentNo, &o, &s)) return 0u;") == 1u &&
             count(s, "n48_p95_pick(") == 1u, 1u);
    expect_u("T11 the replay swaps all eight copy inputs, only after a pick",
             count(s, "    *phys = o.key; *len = o.len; *pw = o.pw; *ph = o.ph; *stride = o.stride; *surfW = o.surfW; *surfH = o.surfH; *swz = o.swz;") == 1u &&
             at(s, "if (!n48_p95_pick(") < at(s, "    *phys = o.key;"), 1u);
    expect_u("T11 the replay remembers THIS present's hold by the answer hw_p73_present recorded",
             count(s, "    (void)n48_p95_remember(&gP95, &gP73, gP95Last.why, gP95Last.slot, &g, presentNo);") == 1u &&
             count(s, "    gP95Last.why = copy ? (uint32_t)N48_P73_HOLDS : why; gP95Last.slot = i;") == 1u, 1u);
    const std::string fb = fn_body(nb, "uint32_t navi48_fm_replay_blocked(void) {");
    expect_u("T11 navi48_fm_replay_blocked: a restore requested, an A copy pending, or engaged with flip mode OFF; lock-free",
             !fb.empty() && count(fb, "if (__atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE)) return 1u;") == 1u &&
             count(fb, "if (__atomic_load_n(&gFm.aCopyPending, __ATOMIC_ACQUIRE)) return 1u;") == 1u &&
             count(fb, "if (__atomic_load_n(&gFm.engaged, __ATOMIC_ACQUIRE) && !__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) return 1u;") == 1u &&
             count(fb, "IOLock") == 0u, 1u);
    // dpg_perform: 95's branch, then the pinned 73 line, then the copy
    const std::string dp = fn_body(d, "static uint32_t dpg_perform(void *self, void *txn) {");
    const char *d95 = "    if (n48::hw_p95_on()) {\n        if (!n48::hw_p95_present(&phys, &len, &pw, &ph, &stride, &surfW, &surfH, &swz, gSh.perform)) return 0;\n    } else\n"
                      "    if (n48::hw_p73_on() && !n48::hw_p73_present(phys, gSh.perform)) return 0;\n    uint64_t cv[11] = { 0 };";
    expect_u("T11 dpg_perform: 95's branch (ON) else 0.0.537's pinned question (OFF), right before the copy's status array, once",
             !dp.empty() && count(dp, d95) == 1u && count(d, "n48::hw_p95_") == 3u &&
             at(dp, d95) > at(dp, "    if (!descOk || (swz != 0u && swz != 3u)) {") && at(dp, d95) < at(dp, "    if (navi48_fm_on()) {") &&
             at(dp, d95) < at(dp, "n48_dpg_verify_due(phys, gSh.lastVerifyToken, gSh.verifyRuns)"), 1u);
    const char *dl = "    if (n48::hw_p73_on() && n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED) n48::hw_p73_delivered();\n";
    const char *dr = "    n48::hw_p95_after_copy(gSh.perform, n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED ? 1u : 0u);\n";
    expect_u("T11 M1: dpg_perform re-checks a replay after its copy returned, right beside hw_p73_delivered, unconditionally (every "
             "present; nothing pending costs one load), once",
             count(dp, dl) == 1u && count(dp, dr) == 1u && at(dp, dl) < at(dp, dr) &&
             at(dp, dr) - at(dp, dl) < 600u && at(dp, "gSh.lastCopySt = cst;") < at(dp, dl) && count(d, "hw_p95_after_copy(") == 1u, 1u);
    expect_u("T11 dpg_perform takes no lock before the question", count(dp.substr(0, at(dp, d95) == std::string::npos ? 0 : at(dp, d95)), "IOLock"), 0u);
    // the stamps
    expect_u("T11 the retirement stamp sits after the judged-frame poll's retirement (p73_retired's count unchanged, 5; the expiry site untouched)",
             count(s, "p94_ret_note(") == 2u && count(s, "                p94_ret_note(frSeq);") == 1u &&
             at(s, "                p94_ret_note(frSeq);") > at(s, "                p73_retired(frSeq);") && count(s, "p73_retired(") == 5u &&
             count(s, "static __attribute__((noinline)) void p94_ret_note(uint32_t seq)\n{\n    if (!gP73On) return;\n") == 1u, 1u);
    expect_u("T11 the gate stamp sits right after switch 73's gate step, only for a COMMIT with a seq",
             count(s, "        const uint32_t i = n48_p73_gate_p(&gP73, key, gP73J.tgtVa, gP73J.commit, gP73J.gateSeq);\n"
                      "        if (i < N48_P73_SLOTS && gP73J.commit && gP73J.gateSeq)   // build 0.0.538 (log-only): the P's gate stamp\n"
                      "            n48_p94_note_gate(&gP94, i, gP73J.gateSeq, latch_now_us());") == 1u, 1u);
    // the pure step's shape (what T3's interleavings exercise)
    expect_u("T11 gfx_p94.h: the step = guard, snapshot, ONE read, 86's own fence test, re-validation, mark",
             count(h94, "    if (!n48_p94_candidate(t, i, &seq)) return N48_P94_R_NONE;") == 1u &&
             count(h94, "        if (!n48_p86_fence_ours(&e, seq, got, val)) r = got ? N48_P94_R_NOTYET : N48_P94_R_UNREAD;") == 1u &&
             count(h94, "        else if (!n48_p94_entry_still(io->ring, seq)) r = N48_P94_R_NOTLIVE;") == 1u &&
             count(h94, "io->read32(") == 1u && count(h94, "n48_p73_retired(") == 0u && count(h94, "n48_p73_promote_cas(s)") == 1u, 1u);
    expect_u("T11 gfx_p94.h / gfx_p95.h never write the flight ring (no ring writer is called)",
             count(h94 + h95, "n48_fr_poll_entry(") + count(h94 + h95, "n48_fr_mark_") + count(h94 + h95, "n48_fr_free_by_seq(") +
             count(h94 + h95, "n48_fr_reclaim") + count(h94 + h95, "n48_fr_push(") + count(h94 + h95, "n48_fr_expire(") +
             count(h94 + h95, "n48_fr_retire_nopped("), 0u);
}

int main(int argc, char **argv)
{
    t_basic();
    t_guard();
    t_entry();
    t_random();
    t_identity();
    t_p95();
    t_interplay();
    t_instruments();
    t_s2();
    t_m1();
    t_widths();
    if (argc >= 7) t_glue(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
    else expect_u("the source files were given (AHH, DisplayPipeGuard.cpp, Navi48Bringup.cpp, gfx_commit.h, gfx_p94.h, gfx_p95.h)", 0u, 1u);
    std::printf("%d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
