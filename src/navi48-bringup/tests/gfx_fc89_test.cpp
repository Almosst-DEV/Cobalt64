// gfx_fc89_test.cpp — build 0.0.534 ( RUN AF,): SWITCH 89 (a FULL fast-copy chunk verified by an SDMA read-back +
// a sampled MM cross-check, fastcopy89.h) and SWITCH 90 (the expiry check defers on a busy gXdLock, gfx_ks90.h).
//
// T34a  the read-back over a FAKE VRAM, driven through the real n48_fc_chunk_run (the write) and n48_fc89_chunk (the read-back):
//       (a) clean; (b) one flipped byte in the landing (outside the sample plan, and inside it); (c) a landing at a WRONG offset that
//       both DMA directions share (the read-back agrees, the MM cross-check at the correct offset catches it); (d) a read-back timeout
//       (and a failed ring write); (e) refused before the ring (not taken, the lock still held); (f) an engine that answers LANDED and
//       copies nothing over a buffer that still holds the same image (the poison catches it); (g) the cross-check's MM read refused.
//       The lock discipline (submit under the lock, the cross-check after the ONE unlock) is asserted on every run.
// T34b  resprov's compared rung (ws_resprov.h n48_rp_record) over whole copies built from these chunk results: RECORDED only when
//       every chunk read back clean; a mismatch, a timeout or a refused cross-check never records.
// T34c  random schedules against an independent oracle, and the OFF identity: switch 89 OFF (and every not-usable state) answers
//       0 before anything is touched, and frozen copies of 0.0.533's fastcopy.h verify / outcome / chunk-run answer exactly as the
//       live header over random inputs.
// T34d  switch 90: busy -> DEFERRED until 2 x the flight bound from the oldest live flight's stamp, then today's withdrawal; a
//       deferral always releases the marker and never takes the root[511] clear (hook_unmapVA's predicates, driven with the verdict);
//       RUN AF's own numbers; OFF identity over random rings (frozen copy of 0.0.533's expiry outcome).
// T34e  the lines (<= 491 bytes at their widest) and the kext's wiring (orders, the default-OFF switches, the mid-arm guard), plus the
//       OFF identity of the two touched kext functions: navi48_fc_chunk and ks_eop_at_expiry with 0.0.534's lines undone hash to
//       0.0.533's exact text.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_fc89_test.cpp -o /tmp/fc89 && \
//         /tmp/fc89 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/apple/gfx_commit.h
//
// The three source arguments are REQUIRED (the wiring section fails without them).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <initializer_list>

#include "fastcopy89.h"
#include "gfx_ks90.h"
#include "gfx_flightring.h"
#include "ws_resprov.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    if (!ok || std::getenv("FC89_VERBOSE")) std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// =============================================================================================================================
// THE FAKE MACHINE: VRAM (dwords), the staging buffer, the read-back buffer, the SDMA engine (write and read-back directions),
// the MM window's sampled read, gFastCopyLock.
// =============================================================================================================================
static const uint64_t kBaseMc = 0x8000000000ull, kStgMc = 0x7f0100000ull, kRbMc = 0x7f0200000ull;
enum { E_LANDED = 0, E_TIMEOUT, E_REFUSED, E_RING, E_NOOP, E_PARTIAL };
struct Machine {
    std::vector<uint32_t> vram;
    std::vector<uint8_t> stg;
    std::vector<uint32_t> rb;
    int64_t delta = 0;          // bytes: the engine's view of VRAM is shifted by this in BOTH directions (a shared wrong address)
    int wmode = E_LANDED, rmode = E_LANDED;
    int64_t flip_byte = -1;     // after the write lands: flip this byte of the LANDING (chunk-relative)
    int plan_refuse = 0;
    uint32_t seed = 1;
    // the lock and its witnesses
    int locked = 0, unlocks = 0, rb_submits = 0, rb_submit_unlocked = 0, plans = 0, plan_locked = 0;
    int w_submits = 0;
    explicit Machine(uint32_t vramDwords = 1u << 20) : vram(vramDwords, 0u), stg(N48_FC_STAGING_BYTES, 0u), rb(N48_FC_STAGING_BYTES / 4u, 0u) {}
};
static uint32_t pat(uint32_t seed, uint64_t off) { uint32_t x = (uint32_t)(off * 2654435761u) ^ (seed * 0x9E3779B9u); x ^= x >> 13; x *= 0x5bd1e995u; return x ^ (x >> 15); }
static int m_fill(void *ctx, uint8_t *dst, uint64_t off, uint32_t take)
{
    Machine *m = static_cast<Machine *>(ctx);
    for (uint32_t i = 0; i < take; i += 4u) { const uint32_t w = pat(m->seed, off + i); std::memcpy(dst + i, &w, take - i >= 4u ? 4u : take - i); }
    return 1;
}
static uint64_t vram_index(const Machine *, uint64_t mc, int64_t delta) { return (uint64_t)((int64_t)(mc - kBaseMc) + delta) / 4u; }
static uint32_t m_wsubmit(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t)
{
    Machine *m = static_cast<Machine *>(ctx);
    m->w_submits++;
    if (src_mc != kStgMc) return N48_FC_SUB_REFUSED;
    if (m->wmode == E_TIMEOUT) return N48_FC_SUB_TIMEOUT;
    const uint64_t at = vram_index(m, dst_mc, m->delta);
    std::memcpy(&m->vram[at], m->stg.data(), bytes);
    if (m->flip_byte >= 0) reinterpret_cast<uint8_t *>(&m->vram[at])[m->flip_byte] ^= 0x10u;
    return N48_FC_SUB_LANDED;
}
static uint32_t m_rbsubmit(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t)
{
    Machine *m = static_cast<Machine *>(ctx);
    m->rb_submits++;
    if (!m->locked) m->rb_submit_unlocked++;
    if (dst_mc != kRbMc) return N48_FC_SUB_REFUSED;
    switch (m->rmode) {
    case E_TIMEOUT: return N48_FC_SUB_TIMEOUT;
    case E_REFUSED: return N48_FC_SUB_REFUSED;
    case E_RING:    return N48_FC_SUB_RING;
    case E_NOOP:    return N48_FC_SUB_LANDED;
    case E_PARTIAL: std::memcpy(m->rb.data(), &m->vram[vram_index(m, src_mc, m->delta)], bytes / 2u); return N48_FC_SUB_LANDED;
    default:        std::memcpy(m->rb.data(), &m->vram[vram_index(m, src_mc, m->delta)], bytes); return N48_FC_SUB_LANDED;
    }
}
static void m_unlock(void *ctx) { Machine *m = static_cast<Machine *>(ctx); m->locked = 0; m->unlocks++; }
static int m_plan(void *ctx, uint64_t d_at, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns)
{
    Machine *m = static_cast<Machine *>(ctx);
    m->plans++;
    if (m->locked) m->plan_locked++;
    if (m->plan_refuse) return 0;
    for (uint32_t i = 0; i < ns; i++) got[i] = m->vram[(d_at + n48_fc_sample_at(n, rot, i)) / 4u];   // the TRUE offset: MM has no delta
    return 1;
}

struct Chunk {
    uint64_t result = 0; uint32_t wrun = 0; n48_fc89_out out {};
    std::vector<uint32_t> exp;
};
// The kext's order for ONE chunk: n48_fc_chunk_run (the write, FULL snapshot) under the lock; then, the write landed, n48_fc89_chunk.
static Chunk run_chunk(Machine &m, n48_fc_state &s, n48_fc89_state &q, uint64_t d_at, uint32_t n, uint32_t rot)
{
    Chunk c;
    c.exp.assign(n / 4u, 0u);
    const n48_fc_ops wops { &m, &m_fill, &m_wsubmit };
    m.locked = 1;
    c.wrun = n48_fc_chunk_run(&wops, &s, m.stg.data(), kStgMc, kBaseMc + d_at, n, c.exp.data(), N48_FC_M_FULL, rot, 0u, nullptr);
    if (c.wrun != N48_FC_RUN_OK) { m.locked = 0; return c; }
    std::vector<uint32_t> got(N48_FC_SAMPLE_MAX, 0u);
    const n48_fc89_ops ops { &m, &m_rbsubmit, &m_unlock, &m_plan };
    c.result = n48_fc89_chunk(&ops, &s, &q, m.rb.data(), kRbMc, kBaseMc + d_at, d_at, c.exp.data(), got.data(), n, rot, &c.out);
    return c;
}
static uint64_t cmp_of(uint64_t r) { return (r & N48_FC_R_FAIL) ? 0u : (r >> 32) & 0x3FFFFFFFull; }
static uint64_t bad_of(uint64_t r) { return (r & N48_FC_R_FAIL) ? 0u : (uint32_t)r; }
static bool is_fail(uint64_t r, uint32_t code) { return (r & N48_FC_R_TAKEN) && (r & N48_FC_R_FAIL) && (uint32_t)(r & 0xffu) == code; }
static n48_fc89_state usable() { n48_fc89_state q {}; q.rb_ok = 1u; q.pc_done = 1u; return q; }

static void test_cases()
{
    const uint64_t d_at = 0x40000u;
    const uint32_t n = 0x10000u;   // 64 KiB: 16 pages, 18 samples
    {   // (a) clean
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (a) clean: taken, compared n/4 = 16384, 0 mismatched, not failed",
               (c.result & N48_FC_R_TAKEN) && !(c.result & N48_FC_R_FAIL) && cmp_of(c.result) == n / 4u && bad_of(c.result) == 0u);
        expect("T34a (a) the read-back ran under the lock, the lock was released ONCE, then the cross-check ran unlocked",
               m.rb_submits == 1 && m.rb_submit_unlocked == 0 && m.unlocks == 1 && m.plans == 1 && m.plan_locked == 0 && !m.locked);
        expect("T34a (a) rb_bad 0, x_bad 0, ns 18, the path not latched, the buffer not retired",
               c.out.rb_bad == 0u && c.out.x_bad == 0u && c.out.ns == 18u && !s.latched_off && !q.rb_retired);
    }
    {   // (b) one flipped byte in the landing, outside the sample plan
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        uint32_t off = 0x1234u * 4u + 1u;   // a dword the plan (first, last, one per page at (rot + p*97) % 1024) does not name
        for (uint32_t i = 0; i < n48_fc_sample_count(n); i++) if (n48_fc_sample_at(n, 3u, i) / 4u == off / 4u) off += 8u;
        m.flip_byte = off;
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (b) one flipped byte NOT sampled: the read-back finds exactly 1, the cross-check 0, mismatched 1, compared n/4",
               c.out.rb_bad == 1u && c.out.x_bad == 0u && bad_of(c.result) == 1u && cmp_of(c.result) == n / 4u);
        Machine m2; n48_fc_state s2 {}; n48_fc89_state q2 = usable();
        m2.flip_byte = n48_fc_sample_at(n, 3u, 5u) + 2u;
        const Chunk c2 = run_chunk(m2, s2, q2, d_at, n, 3u);
        expect("T34a (b) one flipped byte AT a sample: both compares see it (mismatched 2)",
               c2.out.rb_bad == 1u && c2.out.x_bad == 1u && bad_of(c2.result) == 2u);
    }
    {   // (c) a wrong offset both DMA directions share
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.delta = 0x20000;
        for (uint32_t i = 0; i < n / 4u; i++) m.vram[d_at / 4u + i] = 0x5A5A0000u + i;   // what really stands at the chunk's offset
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (c) wrong landing shared by both directions: the read-back AGREES (rb_bad 0)", c.out.rb_bad == 0u);
        expect("T34a (c) ... the MM cross-check at the correct offset catches every sample (x_bad 18): mismatched, not clean",
               c.out.x_bad == 18u && bad_of(c.result) == 18u && !(c.result & N48_FC_R_FAIL));
    }
    {   // (d) a read-back timeout, and a failed ring write
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.rmode = E_TIMEOUT;
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (d) read-back TIMEOUT: CHUNK FAILED with fastcopy.h's TIMEOUT run code, nothing credited",
               is_fail(c.result, N48_FC_RUN_TIMEOUT) && cmp_of(c.result) == 0u && c.out.run == N48_FC89_TIMEOUT);
        expect("T34a (d) ... exactly as a write timeout: the path LATCHED OFF (TIMEOUT), staging retired, timeouts 1; the read-back "
               "buffer RETIRED; the lock released once; no cross-check",
               s.latched_off && s.latch_why == N48_FC_LATCH_TIMEOUT && s.staging_retired && s.timeouts == 1u && q.rb_retired &&
               m.unlocks == 1 && m.plans == 0);
        expect("T34a (d) ... and no later chunk takes the read-back (n48_fc89_use: retired / latched)",
               !n48_fc89_use(1u, N48_FC_M_FULL, &q, s.latched_off));
        Machine m2; n48_fc_state s2 {}; n48_fc89_state q2 = usable();
        m2.rmode = E_RING;
        const Chunk c2 = run_chunk(m2, s2, q2, d_at, n, 3u);
        expect("T34a (d) read-back RING failure: FAILED (RING), latched (RING), buffer retired, no credit",
               is_fail(c2.result, N48_FC_RUN_RING) && s2.latched_off && s2.latch_why == N48_FC_LATCH_RING && q2.rb_retired &&
               m2.unlocks == 1);
    }
    {   // (e) refused before the ring
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.rmode = E_REFUSED;
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (e) refused before the ring: NOT TAKEN (0), the lock STILL HELD, no unlock, no cross-check, nothing latched",
               c.result == 0u && m.locked == 1 && m.unlocks == 0 && m.plans == 0 && !s.latched_off && !q.rb_retired);
    }
    {   // (f) LANDED but nothing copied over a buffer holding the same image (a re-copy of an identical chunk): the poison catches it
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const Chunk c0 = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (f) first copy clean", bad_of(c0.result) == 0u && cmp_of(c0.result) == n / 4u);
        m.rmode = E_NOOP;
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);   // same seed: the same image; the buffer still holds it
        expect("T34a (f) the engine answered LANDED and copied NOTHING: every dword mismatches (rb_bad n/4)",
               c.out.rb_bad == n / 4u && bad_of(c.result) == n / 4u);
        Machine m2; n48_fc_state s2 {}; n48_fc89_state q2 = usable();
        m2.rmode = E_PARTIAL;
        const Chunk c2 = run_chunk(m2, s2, q2, d_at, n, 3u);
        expect("T34a (f) a SHORT read-back (half): the unwritten half mismatches (rb_bad n/8)", c2.out.rb_bad == n / 8u);
    }
    {   // (g) the cross-check's MM read refused
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.plan_refuse = 1;
        const Chunk c = run_chunk(m, s, q, d_at, n, 3u);
        expect("T34a (g) cross-check read refused: FAILED as a verify-read failure, nothing credited",
               is_fail(c.result, N48_FC_RUN_VERIFY_READ) && cmp_of(c.result) == 0u && c.out.xread_fail == 1u);
    }
    {   // the 1 MiB chunk (the staging size) and a tail-shaped chunk
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const Chunk c = run_chunk(m, s, q, 0x100000u, N48_FC_STAGING_BYTES, 7u);
        expect("T34a a full 1 MiB chunk: clean, compared 262144", bad_of(c.result) == 0u && cmp_of(c.result) == 262144u);
        const Chunk c2 = run_chunk(m, s, q, 0x300000u, 0x1f04u, 9u);
        expect("T34a a 0x1f04-byte chunk (partial last page): clean, compared 0x7c1", bad_of(c2.result) == 0u && cmp_of(c2.result) == 0x7c1u);
    }
    {   // shape / not-usable
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        std::vector<uint32_t> exp(8, 1u), got(N48_FC_SAMPLE_MAX, 0u);
        const n48_fc89_ops ops { &m, &m_rbsubmit, &m_unlock, &m_plan };
        m.locked = 1;
        expect("T34a bad shapes answer NOT TAKEN with the lock held and no fence consumed",
               n48_fc89_chunk(&ops, &s, &q, m.rb.data(), kRbMc, kBaseMc, 0u, exp.data(), got.data(), 30u, 0u, nullptr) == 0u &&
               n48_fc89_chunk(&ops, &s, &q, m.rb.data(), kRbMc, kBaseMc, 0u, exp.data(), got.data(), 0u, 0u, nullptr) == 0u &&
               n48_fc89_chunk(&ops, &s, &q, m.rb.data(), 0u, kBaseMc, 0u, exp.data(), got.data(), 32u, 0u, nullptr) == 0u &&
               m.locked == 1 && m.unlocks == 0 && s.fence_last == 0u);
        s.fence_last = 0xFFFFFFFFu;
        expect("T34a fence values exhausted: NOT TAKEN (the path latched by n48_fc_fence_next), lock held, nothing submitted",
               n48_fc89_chunk(&ops, &s, &q, m.rb.data(), kRbMc, kBaseMc, 0u, exp.data(), got.data(), 32u, 0u, nullptr) == 0u &&
               m.rb_submits == 0 && m.locked == 1 && s.latched_off);
    }
}

// =============================================================================================================================
// T34b resprov's rung over whole copies (the copy loop sums each chunk's compared/mismatched; a FAILED chunk fails the copy).
// =============================================================================================================================
static uint32_t record_copy(const std::vector<uint64_t> &results, uint64_t bytes)
{
    uint64_t compared = 0u, mismatched = 0u; uint32_t failed = 0u;
    for (uint64_t r : results) { if (!(r & N48_FC_R_TAKEN) || (r & N48_FC_R_FAIL)) failed = 1u; compared += cmp_of(r); mismatched += bad_of(r); }
    static n48_rp t; std::memset(&t, 0, sizeof t);
    n48_rp_copy c; std::memset(&c, 0, sizeof c);
    c.copied = failed ? 0u : 1u; c.bytes = bytes; c.compared = compared; c.mismatched = mismatched;
    c.ownerWs = 1u; c.ctx = 0x77u; c.vaOk = 1u; c.va = 0x401000000ull; c.contiguous = 1u; c.vram = 0x10000000ull;
    c.mode = N48_RP_G12_4KB_2D; c.retiled = 1u; c.elemBytes = 4u; c.w = 1920u; c.h = 1080u;
    return n48_rp_record(&t, &c);
}
static void test_resprov()
{
    // the wallpaper: 0x7e9000 bytes = 7 chunks of 1 MiB + one of 0xe9000
    const uint64_t bytes = 0x7e9000ull;
    for (int variant = 0; variant < 5; variant++) {
        Machine m(1u << 22); n48_fc_state s {}; n48_fc89_state q = usable();   // 16 MiB
        std::vector<uint64_t> res;
        for (uint64_t pos = 0; pos < bytes; ) {
            const uint32_t n = (uint32_t)((bytes - pos) > N48_FC_STAGING_BYTES ? N48_FC_STAGING_BYTES : bytes - pos);
            if (pos == 0x300000u) {
                if (variant == 1) m.flip_byte = 4097;
                if (variant == 2) m.rmode = E_TIMEOUT;
                if (variant == 3) m.plan_refuse = 1;
                if (variant == 4) m.rmode = E_NOOP;
            }
            m.seed = (uint32_t)(pos >> 20) + 11u;
            const Chunk c = run_chunk(m, s, q, 0x100000ull + pos, n, (uint32_t)(pos >> 20));
            res.push_back(c.result);
            m.flip_byte = -1; m.rmode = E_LANDED; m.plan_refuse = 0;
            if (s.latched_off) break;   // the path is off: the copy failed (a failed chunk ends the copy in the kext)
            pos += n;
        }
        const uint32_t why = record_copy(res, bytes);
        char l[200];
        std::snprintf(l, sizeof l, "T34b wallpaper (0x7e9000 B) variant %d: resprov answers %u (want %s)", variant, why,
                      variant == 0 ? "RECORDED" : "NOT recorded");
        expect(l, variant == 0 ? why == N48_RP_REC_OK : why != N48_RP_REC_OK);
    }
    // the rung itself on the credit: n/4 per chunk covers the bytes exactly
    expect("T34b compared * 4 == bytes for a clean multi-chunk copy (the rung holds with no slack needed)",
           (uint64_t)(0x7e9000u / 4u) * 4u == 0x7e9000u);
}

// =============================================================================================================================
// T34c random schedules and the OFF identity (frozen 0.0.533 copies)
// =============================================================================================================================
// FROZEN from fastcopy.h at 0.0.533 (ef162dc5), renamed; never edit.
static inline uint32_t f533_fence_outcome(n48_fc_state *s, uint32_t sub)
{
    if (sub == N48_FC_SUB_LANDED) return N48_FC_RUN_OK;
    if (sub == N48_FC_SUB_REFUSED) return N48_FC_RUN_REFUSED;
    if (s) {
        s->staging_retired = 1u;
        if (sub == N48_FC_SUB_RING) n48_fc_latch(s, N48_FC_LATCH_RING);
        else { s->timeouts++; n48_fc_latch(s, N48_FC_LATCH_TIMEOUT); }
    }
    return sub == N48_FC_SUB_RING ? N48_FC_RUN_RING : N48_FC_RUN_TIMEOUT;
}
static inline uint64_t f533_verify(int (*read)(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords), n48_fc_plan_read_fn plan,
                                   void *ctx, uint64_t d_at, const uint32_t *exp, uint32_t *got, uint32_t n, uint32_t mode,
                                   uint32_t rot, uint64_t *compared, uint32_t *read_fail)
{
    uint64_t bad = 0u, cmp = 0u;
    if (read_fail) *read_fail = 0u;
    if (!read || !exp || n == 0u || (n & 3u)) { if (read_fail) *read_fail = 1u; if (compared) *compared = 0u; return 0u; }
    if (mode == N48_FC_M_FULL) {
        uint32_t buf[32];
        for (uint32_t i = 0; i < n / 4u; ) {
            const uint32_t cnt = (n / 4u - i) >= 32u ? 32u : (n / 4u - i);
            if (!read(ctx, d_at + (uint64_t)i * 4u, buf, cnt)) { if (read_fail) *read_fail = 1u; break; }
            for (uint32_t j = 0; j < cnt; j++) if (buf[j] != exp[i + j]) bad++;
            cmp += cnt;
            i += cnt;
        }
    } else {
        const uint32_t ns = n48_fc_sample_count(n);
        if (!ns || ns > N48_FC_SAMPLE_MAX || !plan || !got || !plan(ctx, d_at, n, rot, got, ns)) {
            if (read_fail) *read_fail = 1u;
        } else {
            for (uint32_t i = 0; i < ns; i++) if (got[i] != exp[i]) bad++;
            cmp = ns;
        }
    }
    if (compared) *compared = cmp;
    return bad;
}
static int m_read(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords)
{
    Machine *m = static_cast<Machine *>(ctx);
    for (uint32_t i = 0; i < dwords; i++) dst[i] = m->vram[vram / 4u + i];
    return 1;
}
static uint32_t rng_state = 0x1234567u;
static uint32_t rnd() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static void test_random_and_off()
{
    // the random schedule against an independent oracle
    uint32_t bad = 0u, runs = 0u;
    for (uint32_t it = 0; it < 400u; it++) {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.seed = rnd();
        const uint32_t n = 4u * (1u + rnd() % 0x4000u);
        const uint64_t d_at = 4096ull * (rnd() % 64u);
        const uint32_t rot = rnd() % 1000u;
        const uint32_t k = rnd() % 8u;
        if (k == 1u) m.flip_byte = rnd() % n;
        if (k == 2u) { m.delta = 4 * (int64_t)(1 + rnd() % 1024u); }
        if (k == 3u) m.rmode = E_TIMEOUT;
        if (k == 4u) m.rmode = E_REFUSED;
        if (k == 5u) m.rmode = E_NOOP;
        if (k == 6u) m.plan_refuse = 1;
        const Chunk c = run_chunk(m, s, q, d_at, n, rot);
        runs++;
        // the oracle, computed from the machine alone
        const uint64_t land = (uint64_t)((int64_t)d_at + m.delta) / 4u;
        uint64_t rbWant = 0u, xWant = 0u;
        for (uint32_t i = 0; i < n / 4u; i++) if (m.vram[land + i] != c.exp[i]) rbWant++;
        if (m.rmode == E_NOOP) { rbWant = 0u; for (uint32_t i = 0; i < n / 4u; i++) if (~c.exp[i] != c.exp[i]) rbWant++; }
        for (uint32_t i = 0; i < n48_fc_sample_count(n); i++) {
            const uint32_t o = n48_fc_sample_at(n, rot, i);
            if (m.vram[(d_at + o) / 4u] != c.exp[o / 4u]) xWant++;
        }
        uint64_t want;
        if (m.rmode == E_REFUSED) want = 0u;
        else if (m.rmode == E_TIMEOUT) want = n48_fc_result_fail(N48_FC_RUN_TIMEOUT);
        else if (m.plan_refuse) want = n48_fc_result_fail(N48_FC_RUN_VERIFY_READ);
        else want = n48_fc_result_ok(n / 4u, rbWant + xWant);
        // SOUNDNESS: a clean answer means the TRUE VRAM holds the stream at every sample AND the SDMA view holds it in full
        uint32_t trueSampled = 1u;
        for (uint32_t i = 0; i < n48_fc_sample_count(n); i++) {
            const uint32_t o = n48_fc_sample_at(n, rot, i);
            if (m.vram[(d_at + o) / 4u] != c.exp[o / 4u]) trueSampled = 0u;
        }
        const bool clean = (c.result & N48_FC_R_TAKEN) && !(c.result & N48_FC_R_FAIL) && bad_of(c.result) == 0u;
        if (c.result != want || (clean && (!trueSampled || rbWant)) || (clean && cmp_of(c.result) != n / 4u) ||
            (c.result && m.unlocks != 1) || (!c.result && m.unlocks != 0) || m.plan_locked || m.rb_submit_unlocked) bad++;
    }
    char l[200];
    std::snprintf(l, sizeof l, "T34c %u random chunks: result == the oracle, clean only when true, one unlock when taken: %u bad", runs, bad);
    expect(l, bad == 0u && runs == 400u);

    // OFF identity: switch 89 OFF (and every not-usable state) is never taken
    uint32_t offBad = 0u;
    for (uint32_t on = 0; on < 2u; on++)
        for (uint32_t mode = 0; mode < 4u; mode++)
            for (uint32_t ok = 0; ok < 2u; ok++)
                for (uint32_t ret = 0; ret < 2u; ret++)
                    for (uint32_t lat = 0; lat < 2u; lat++) {
                        n48_fc89_state q {}; q.rb_ok = ok; q.rb_retired = ret;
                        const uint32_t want = (on && mode == N48_FC_M_FULL && ok && !ret && !lat) ? 1u : 0u;
                        if (n48_fc89_use(on, mode, &q, lat) != want) offBad++;
                    }
    expect("T34c n48_fc89_use: taken ONLY with 89 ON, the chunk FULL, the buffer bound + controlled + not retired, the path not latched",
           offBad == 0u && !n48_fc89_use(1u, N48_FC_M_FULL, nullptr, 0u));

    // the pipeline over random schedules: 0.0.533 (frozen: write, then the full MM verify) vs 0.0.534 with 89 OFF
    uint32_t pipeBad = 0u;
    for (uint32_t it = 0; it < 200u; it++) {
        const uint32_t seed = rnd(), n = 4u * (1u + rnd() % 0x2000u), rot = rnd() % 997u, wm = rnd() % 4u == 0u ? E_TIMEOUT : E_LANDED;
        const int64_t flip = (rnd() % 3u == 0u) ? (int64_t)(rnd() % n) : -1;
        const uint64_t d_at = 4096ull * (rnd() % 32u);
        uint64_t outA = 0u, outB = 0u; n48_fc_state sA {}, sB {};
        std::vector<uint32_t> vA, vB, rbB;
        for (int pass = 0; pass < 2; pass++) {
            Machine m; m.seed = seed; m.wmode = wm; m.flip_byte = flip;
            n48_fc_state &s = pass ? sB : sA;
            std::vector<uint32_t> exp(n / 4u, 0u);
            const n48_fc_ops wops { &m, &m_fill, &m_wsubmit };
            const uint32_t r = n48_fc_chunk_run(&wops, &s, m.stg.data(), kStgMc, kBaseMc + d_at, n, exp.data(), N48_FC_M_FULL, rot, 0u, nullptr);
            uint64_t out = 0u;
            const uint32_t fenceBefore = s.fence_last;
            if (r == N48_FC_RUN_OK && pass) {   // 0.0.534's call site with switch 89 OFF
                n48_fc89_state q = usable();
                if (n48_fc89_use(0u, N48_FC_M_FULL, &q, s.latched_off)) out = 0xdeadull;   // never
                if (s.fence_last != fenceBefore) out = 0xbeefull;
            }
            if (!out) {
                if (r != N48_FC_RUN_OK) out = n48_fc_result_fail(r);
                else {
                    uint64_t cmp = 0u; uint32_t rf = 0u;
                    const uint64_t b = (pass ? n48_fc_verify : f533_verify)(&m_read, nullptr, &m, d_at, exp.data(), nullptr, n, N48_FC_M_FULL, rot, &cmp, &rf);
                    out = rf ? n48_fc_result_fail(N48_FC_RUN_VERIFY_READ) : n48_fc_result_ok(cmp, b);
                }
            }
            (pass ? outB : outA) = out;
            (pass ? vB : vA) = m.vram;
            if (pass) rbB = m.rb;
        }
        bool rbUntouched = true;
        for (uint32_t w : rbB) if (w) { rbUntouched = false; break; }
        if (outA != outB || vA != vB || std::memcmp(&sA, &sB, sizeof sA) != 0 || !rbUntouched) pipeBad++;
    }
    expect("T34c OFF identity: 200 random chunks through 0.0.533's frozen pipeline and 0.0.534's with 89 OFF: same result, VRAM, state; "
           "the read-back buffer never touched, no fence consumed", pipeBad == 0u);

    // frozen vs live fastcopy.h (0.0.534 does not edit it)
    uint32_t hdrBad = 0u;
    for (uint32_t it = 0; it < 2000u; it++) {
        n48_fc_state a {}, b {};
        a.timeouts = b.timeouts = rnd() % 3u; a.latched_off = b.latched_off = rnd() & 1u; a.latch_why = b.latch_why = rnd() % 6u;
        const uint32_t sub = rnd() % 6u;
        if (n48_fc_fence_outcome(&a, sub) != f533_fence_outcome(&b, sub) || std::memcmp(&a, &b, sizeof a) != 0) hdrBad++;
    }
    for (uint32_t it = 0; it < 100u; it++) {
        Machine m; m.seed = rnd();
        for (uint32_t i = 0; i < 4096u; i++) m.vram[i] = rnd() % 4u ? pat(m.seed, i * 4u) : rnd();
        std::vector<uint32_t> exp(4096u);
        for (uint32_t i = 0; i < 4096u; i++) exp[i] = pat(m.seed, i * 4u);
        const uint32_t n = 4u * (1u + rnd() % 4095u);
        uint64_t c1 = 0, c2 = 0; uint32_t f1 = 0, f2 = 0;
        if (n48_fc_verify(&m_read, nullptr, &m, 0u, exp.data(), nullptr, n, N48_FC_M_FULL, 0u, &c1, &f1) !=
                f533_verify(&m_read, nullptr, &m, 0u, exp.data(), nullptr, n, N48_FC_M_FULL, 0u, &c2, &f2) || c1 != c2 || f1 != f2)
            hdrBad++;
    }
    expect("T34c frozen 0.0.533 n48_fc_fence_outcome / n48_fc_verify (full) == the live header over random inputs", hdrBad == 0u);
}

// =============================================================================================================================
// T34d switch 90
// =============================================================================================================================
static const uint64_t kBound = 2000000ull;   // kKsFlightUs (the kext static_asserts 2 x it == 4 s)
// hook_unmapVA's predicates after ks_eop_at_expiry (AppleHardwareHook.cpp, pinned in T34e): deferNow = n48_ksd_defer(kdv);
// held = n48_ks_withdraw_hold(kdv) (0 = the marker released at once); the root[511] clear runs only when !deferNow.
struct Unmap { uint32_t kdv, deferNow, held, writes511, what; };
static Unmap unmap_after(uint32_t on90, uint32_t locked, uint32_t kdv, uint32_t nowOk, uint64_t now, uint64_t blkAt)
{
    Unmap u {};
    u.kdv = locked ? kdv : n48_ks90_kdv(on90, 0u, kdv, nowOk, now, blkAt, kBound, &u.what);
    u.deferNow = n48_ksd_defer(u.kdv);
    u.held = n48_ks_withdraw_hold(u.kdv);
    u.writes511 = u.deferNow ? 0u : 1u;   // armed: `if (e->wrote && !e->withdrawn && !deferNow)`
    return u;
}
// FROZEN from gfx_flightring.h at 0.0.533 (ef162dc5), renamed; never edit.
static inline uint32_t f533_expiry_outcome(uint32_t locked, uint32_t kdv_after)
{
    if (!locked) return N48_FR_X_BUSY;
    if (kdv_after == N48_KSD_DEFER) return N48_FR_X_DEFERRED;
    if (kdv_after == N48_KSD_NOW_EOP || kdv_after == N48_KSD_NOW_NOT_IN_FLIGHT || kdv_after == N48_KSD_NOW_NOTHING)
        return N48_FR_X_CLEARED;
    return N48_FR_X_WITHDRAWN;
}
static void test_ks90()
{
    // one committed flight at t0; unmaps at a sweep of ages with the lock BUSY
    const uint64_t t0 = 253405000ull;
    uint32_t sweepBad = 0u, deferred = 0u, capped = 0u;
    for (uint64_t age = 0; age <= 5000000ull; age += 50000ull) {
        n48_fr_ring r; n48_fr_reset(&r);
        (void)n48_fr_push(&r, 4u, t0, 1u, 0x100u, 0xabcu, nullptr);
        (void)n48_fr_mark_committed(&r, 4u);
        uint32_t bs = 0u; uint64_t ba = 0u;
        const uint32_t kdv = n48_fr_defer_verdict(&r, 1u, 1u, t0 + age, kBound, &bs, &ba);
        const Unmap u = unmap_after(1u, 0u, kdv, 1u, t0 + age, ba);
        if (age < kBound) { if (kdv != N48_KSD_DEFER || u.kdv != N48_KSD_DEFER) sweepBad++; continue; }   // in bound: the ordinary deferral
        if (kdv != N48_KSD_NOW_TIMEOUT || ba != t0) sweepBad++;
        if (age < 2u * kBound) { deferred++; if (u.what != N48_KS90_DEFER || u.kdv != N48_KSD_DEFER || u.writes511 || u.held) sweepBad++; }
        else { capped++; if (u.what != N48_KS90_CAPPED || u.kdv != N48_KSD_NOW_TIMEOUT || !u.writes511 || !u.held) sweepBad++; }
    }
    char l[220];
    std::snprintf(l, sizeof l, "T34d busy lock, one flight: DEFERRED from 2 s up to 4 s (%u ages: marker released, no root[511] clear), "
                  "WITHDRAWN (today's) from 4 s (%u ages)", deferred, capped);
    expect(l, sweepBad == 0u && deferred == 40u && capped == 21u);
    // RUN AF: `oldest live seq 4 at 4149853 of 2000000 us ... gXdLock BUSY` - PAST the 2 x cap: 90 alone would still withdraw
    {
        const Unmap u = unmap_after(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 + 4149853ull, t0);
        expect("T34d RUN AF's own expiry (4,149,853 us, lock busy): CAPPED - today's withdrawal (90 alone would NOT have saved AF)",
               u.what == N48_KS90_CAPPED && u.writes511 == 1u);
        const Unmap u2 = unmap_after(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 + 3999999ull, t0);
        expect("T34d ... the same check 1 us under the cap: DEFERRED", u2.what == N48_KS90_DEFER && u2.writes511 == 0u && u2.held == 0u);
    }
    // the OLDEST live flight drives the cap
    {
        n48_fr_ring r; n48_fr_reset(&r);
        (void)n48_fr_push(&r, 4u, t0, 1u, 0x100u, 0xabcu, nullptr); (void)n48_fr_mark_committed(&r, 4u);
        (void)n48_fr_push(&r, 5u, t0 + 1500000ull, 2u, 0x140u, 0xabdu, nullptr); (void)n48_fr_mark_committed(&r, 5u);
        uint32_t bs = 0u; uint64_t ba = 0u;
        const uint64_t now = t0 + 4100000ull;   // seq 4 past the cap, seq 5 at 2.6 s (past its bound, under its own cap)
        const uint32_t kdv = n48_fr_defer_verdict(&r, 1u, 1u, now, kBound, &bs, &ba);
        const Unmap u = unmap_after(1u, 0u, kdv, 1u, now, ba);
        expect("T34d two live flights: the OLDEST (seq 4, 4.1 s) is past the cap -> withdrawal, although seq 5 is under its own",
               kdv == N48_KSD_NOW_TIMEOUT && bs == 4u && u.what == N48_KS90_CAPPED && u.writes511 == 1u);
    }
    // lock taken, other verdicts, no live flight, torn clock: unchanged
    {
        uint32_t ub = 0u;
        for (uint32_t v = 0; v < N48_KSD_REASONS; v++) {
            uint32_t w = 9u;
            if (n48_ks90_kdv(1u, 1u, v, 1u, t0 + 2500000ull, t0, kBound, &w) != v || w != N48_KS90_NONE) ub++;   // locked
            if (v != N48_KSD_NOW_TIMEOUT && (n48_ks90_kdv(1u, 0u, v, 1u, t0 + 2500000ull, t0, kBound, &w) != v || w != N48_KS90_NONE)) ub++;
        }
        uint32_t w = 0u;
        if (n48_ks90_kdv(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 + 2500000ull, 0u, kBound, &w) != N48_KSD_NOW_TIMEOUT || w != N48_KS90_TORN) ub++;
        if (n48_ks90_kdv(1u, 0u, N48_KSD_NOW_TIMEOUT, 0u, t0 + 2500000ull, t0, kBound, &w) != N48_KSD_NOW_TIMEOUT || w != N48_KS90_TORN) ub++;
        if (n48_ks90_kdv(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 - 1u, t0, kBound, &w) != N48_KSD_NOW_TIMEOUT || w != N48_KS90_TORN) ub++;
        if (n48_ks90_kdv(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 + 2500000ull, t0, 0u, &w) != N48_KSD_NOW_TIMEOUT || w != N48_KS90_TORN) ub++;
        expect("T34d unchanged: the lock taken, any verdict but TIMEOUT, no live flight (stamp 0), no clock, a future stamp, a zero bound",
               ub == 0u);
        // a ring whose only flight EXPIRED (judged-frame maintenance): the verdict is TIMEOUT with no live entry (stamp 0) -> withdrawal
        n48_fr_ring r; n48_fr_reset(&r);
        (void)n48_fr_push(&r, 4u, t0, 1u, 0x100u, 0xabcu, nullptr); (void)n48_fr_mark_committed(&r, 4u);
        (void)n48_fr_expire(&r, 1u, t0 + 2100000ull, kBound);
        uint32_t bs = 0u; uint64_t ba = 0u;
        const uint32_t kdv = n48_fr_defer_verdict(&r, 1u, 1u, t0 + 2200000ull, kBound, &bs, &ba);
        const Unmap u = unmap_after(1u, 0u, kdv, 1u, t0 + 2200000ull, ba);
        expect("T34d an EXPIRED (not live) flight: TIMEOUT with no blocker -> never deferred (no unbounded deferral past expiry)",
               kdv == N48_KSD_NOW_TIMEOUT && ba == 0u && u.what == N48_KS90_TORN && u.writes511 == 1u);
    }
    // a timeline: the lock busy for B us after the flight's bound expired, then free (65's poll retires it: EOP)
    {
        uint32_t tlBad = 0u;
        for (uint64_t busyUntil = 2000000ull; busyUntil <= 6000000ull; busyUntil += 250000ull) {
            uint32_t withdrewLive = 0u, deferrals = 0u, cleared = 0u;
            for (uint64_t t = 2000000ull; t <= 7000000ull; t += 100000ull) {
                const bool locked = t >= busyUntil;
                if (locked) { cleared = 1u; break; }                         // 65's poll reads OURS: RETIRED, NOW_EOP (no stop)
                const Unmap u = unmap_after(1u, 0u, N48_KSD_NOW_TIMEOUT, 1u, t0 + t, t0);
                if (u.writes511) { withdrewLive = 1u; break; }               // today's withdrawal while live: stop_why 3
                deferrals++;
            }
            const bool want = busyUntil <= 4000000ull;                       // freed before the cap: saved
            if ((cleared == 1u) != want || (withdrewLive == 1u) == want) tlBad++;
            (void)deferrals;
        }
        expect("T34d timelines: a lock freed before 4 s is saved (65's poll then retires the flight); one busy past 4 s withdraws",
               tlBad == 0u);
    }
    // OFF identity over random rings: 90 OFF == 0.0.533 (verdict, outcome, predicates)
    {
        uint32_t offBad = 0u, defNever = 0u;
        for (uint32_t it = 0; it < 5000u; it++) {
            n48_fr_ring r; n48_fr_reset(&r);
            const uint32_t k = 1u + rnd() % 4u;
            for (uint32_t i = 0; i < k; i++) {
                const uint64_t at = (rnd() % 7u == 0u) ? 0ull : t0 + (uint64_t)(rnd() % 6000000u);
                (void)n48_fr_push(&r, 10u + i, at, 1u + i, 0x100u + 0x40u * i, 0xa00u + i, nullptr);
                if (rnd() % 3u) (void)n48_fr_mark_committed(&r, 10u + i);
                if (rnd() % 5u == 0u) (void)n48_fr_mark_not_run(&r, 10u + i);
            }
            const uint64_t now = t0 + (uint64_t)(rnd() % 9000000u);
            const uint32_t nowOk = rnd() % 11u ? 1u : 0u;
            uint32_t bs = 0u; uint64_t ba = 0u;
            const uint32_t kdv = n48_fr_defer_verdict(&r, 1u, nowOk, now, kBound, &bs, &ba);
            const uint32_t locked = rnd() & 1u;
            const Unmap off = unmap_after(0u, locked, kdv, nowOk, now, ba);
            // 0.0.533: the verdict stands; the outcome is the frozen one
            if (off.kdv != kdv || n48_fr_expiry_outcome(locked, off.kdv) != f533_expiry_outcome(locked, kdv)) offBad++;
            const Unmap on = unmap_after(1u, locked, kdv, nowOk, now, ba);
            // ON changes ONLY busy + TIMEOUT + a live oldest flight under the cap, and only toward DEFER
            const bool mayChange = !locked && kdv == N48_KSD_NOW_TIMEOUT && nowOk && ba && now >= ba && now - ba < 2u * kBound;
            if (on.kdv != kdv && (!mayChange || on.kdv != N48_KSD_DEFER)) offBad++;
            if (mayChange && on.kdv != N48_KSD_DEFER) offBad++;
            if (on.what == N48_KS90_DEFER && (on.writes511 || on.held)) defNever++;
        }
        expect("T34d OFF identity over 5000 random rings: 90 OFF leaves every verdict and outcome as 0.0.533; ON changes only a busy, "
               "live, under-cap TIMEOUT, and only to DEFER", offBad == 0u);
        expect("T34d every switch-90 DEFER releases the marker at once (hold 0) and never takes the root[511] clear", defNever == 0u);
    }
    // counters
    {
        n48_ks90_stats st {};
        n48_ks90_count(&st, N48_KS90_DEFER); n48_ks90_count(&st, N48_KS90_DEFER); n48_ks90_count(&st, N48_KS90_CAPPED);
        n48_ks90_count(&st, N48_KS90_TORN); n48_ks90_count(&st, N48_KS90_NONE);
        expect("T34d counters: busy 5, deferred-busy 2, capped-withdrawals 1, torn 1",
               st.busy == 5u && st.deferred == 2u && st.capped == 1u && st.torn == 1u);
    }
}

// =============================================================================================================================
// T34f the control's freshness (0.0.534 review M1) and SHADOW (review item 3)
// =============================================================================================================================
static uint32_t pc63(uint32_t i) { return 0xA5000000u | (i & 0x00FFFFFFu); }   // Navi48Bringup.cpp fc_pc_word (63's control)
static void test_control_fresh()
{
    const uint32_t nd = N48_FC_STAGING_BYTES / 4u;
    std::vector<uint32_t> rb(nd);
    uint32_t last = 0u, badPat = 0u, badNonce = 0u;
    for (uint32_t k = 0; k < 3000u; k++) {
        const uint32_t fence = N48_FC_FENCE_BASE + rnd() % 5000u, fence2 = fence + 1u + rnd() % 7u;
        const uint32_t a = n48_fc89_pc_nonce(fence, last), b = n48_fc89_pc_nonce(fence2, a);
        if ((a & 0xFFFFFFu) == (last & 0xFFFFFFu) || (b & 0xFFFFFFu) == (a & 0xFFFFFFu)) badNonce++;
        for (uint32_t t = 0; t < 8u; t++) {
            const uint32_t i = rnd() % nd;
            if (n48_fc89_pc_word(i, a) == pc63(i) || n48_fc89_pc_word(i, a) == n48_fc89_pc_word(i, b) ||
                ~n48_fc89_pc_word(i, a) == n48_fc89_pc_word(i, a)) badPat++;
        }
        last = b;
    }
    expect("T34f control pattern: never 63's word, never the previous control's word (nonces differ in the low 24 bits, 3000 pairs)",
           badPat == 0u && badNonce == 0u);
    const uint32_t prev = n48_fc89_pc_nonce(N48_FC_FENCE_BASE + 3u, 0u), now = n48_fc89_pc_nonce(N48_FC_FENCE_BASE + 9u, prev);
    for (uint32_t i = 0; i < nd; i++) rb[i] = n48_fc89_pc_word(i, now);
    uint32_t fi[2] = { 0, 0 }, fv[2] = { 0, 0 };
    expect("T34f control: a FRESH read-back of this control's pattern passes (0 wrong)", n48_fc89_pc_check(rb.data(), nd, now, fi, fv) == 0u);
    for (uint32_t i = 0; i < nd; i++) rb[i] = pc63(i);
    expect("T34f control: a read-back returning 63's OLD pattern (stale scratch) FAILS in every dword",
           n48_fc89_pc_check(rb.data(), nd, now, fi, fv) == nd && fi[0] == 0u && fv[0] == pc63(0));
    for (uint32_t i = 0; i < nd; i++) rb[i] = n48_fc89_pc_word(i, prev);
    expect("T34f control: a read-back returning the PREVIOUS 89 control's pattern FAILS in every dword",
           n48_fc89_pc_check(rb.data(), nd, now, fi, fv) == nd);
    for (uint32_t i = 0; i < nd; i++) rb[i] = ~n48_fc89_pc_word(i, now);
    expect("T34f control: an engine that wrote nothing (the poison stands) FAILS in every dword", n48_fc89_pc_check(rb.data(), nd, now, fi, fv) == nd);
}

// SHADOW over the fake machine: phase A (the read-back) then TODAY's n48_fc_verify through the recording read, decided by MM.
struct MmView { Machine *m; int64_t flip_dw; };   // the MM window's view (it may differ from the SDMA view at one dword)
static int mm_view_read(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords)
{
    MmView *v = static_cast<MmView *>(ctx);
    for (uint32_t i = 0; i < dwords; i++) {
        dst[i] = v->m->vram[vram / 4u + i];
        if ((int64_t)(vram / 4u + i) == v->flip_dw) dst[i] ^= 0x100u;
    }
    return 1;
}
struct ShOut { uint64_t result, today; n48_fc89_shadow_rec rec; n48_fc89_out o; uint32_t taken, sdOk; };
static ShOut run_shadow(Machine &m, n48_fc_state &s, n48_fc89_state &q, uint64_t d_at, uint32_t n, uint32_t rot, int64_t mmFlipDw,
                        int64_t sdFlipDw)
{
    ShOut r {};
    std::vector<uint32_t> exp(n / 4u), sd(n / 4u);
    const n48_fc_ops wops { &m, &m_fill, &m_wsubmit };
    m.locked = 1;
    if (n48_fc_chunk_run(&wops, &s, m.stg.data(), kStgMc, kBaseMc + d_at, n, exp.data(), N48_FC_M_FULL, rot, 0u, nullptr) != N48_FC_RUN_OK) return r;
    const n48_fc89_ops ops { &m, &m_rbsubmit, &m_unlock, &m_plan };
    r.taken = n48_fc89_shadow_a(&ops, &s, &q, m.rb.data(), kRbMc, kBaseMc + d_at, exp.data(), sd.data(), n, &r.o, &r.sdOk);
    if (!r.taken) return r;
    if (sdFlipDw >= 0) sd[sdFlipDw] ^= 0x200u;          // an SDMA view that differs from VRAM (GL2 vs DRAM) at one dword
    MmView v { &m, mmFlipDw >= 0 ? (int64_t)(d_at / 4u) + mmFlipDw : -1 };
    r.rec.read = &mm_view_read; r.rec.ctx = &v; r.rec.sd = sd.data(); r.rec.sd_ok = r.sdOk; r.rec.nd = n / 4u; r.rec.d_at = d_at;
    uint64_t cmp = 0; uint32_t rf = 0;
    const uint64_t bad = n48_fc_verify(&n48_fc89_shadow_read, nullptr, &r.rec, d_at, exp.data(), nullptr, n, N48_FC_M_FULL, rot, &cmp, &rf);
    r.result = n48_fc89_shadow_decide(bad, cmp, rf, r.o.run, r.o.rb_bad);
    // today's (0.0.533) verify over the same MM view, unwrapped
    uint64_t cmp2 = 0; uint32_t rf2 = 0;
    const uint64_t bad2 = f533_verify(&mm_view_read, nullptr, &v, d_at, exp.data(), nullptr, n, N48_FC_M_FULL, rot, &cmp2, &rf2);
    r.today = rf2 ? n48_fc_result_fail(N48_FC_RUN_VERIFY_READ) : n48_fc_result_ok(cmp2, bad2);
    return r;
}
static void test_shadow()
{
    const uint64_t d_at = 0x40000u; const uint32_t n = 0x10000u;
    {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const ShOut r = run_shadow(m, s, q, d_at, n, 5u, -1, -1);
        expect("T34f SHADOW agree: taken, both views compared in full (16384), 0 disagreements, clean, == today's verify",
               r.taken && r.sdOk && r.rec.compared == n / 4u && r.rec.dis == 0u && r.result == r.today && bad_of(r.result) == 0u &&
               cmp_of(r.result) == n / 4u && m.unlocks == 1 && m.plan_locked == 0);
    }
    {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const ShOut r = run_shadow(m, s, q, d_at, n, 5u, 777, -1);
        expect("T34f SHADOW: SDMA equals the stream but the MM view differs at one dword: recorded as a DISAGREEMENT (offset, both values) "
               "and DECIDED BY MM (mismatched 1), == today's verify",
               r.rec.dis == 1u && r.rec.nfirst == 1u && r.rec.off[0] == 777u * 4u && r.rec.mm[0] == (r.rec.sdv[0] ^ 0x100u) &&
               r.o.rb_bad == 0u && bad_of(r.result) == 1u && r.result == r.today);
    }
    {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        const ShOut r = run_shadow(m, s, q, d_at, n, 5u, -1, 1234);
        expect("T34f SHADOW: the SDMA view differs but the MM view is right: a disagreement recorded, the chunk CLEAN (MM decides)",
               r.rec.dis == 1u && r.rec.off[0] == 1234u * 4u && bad_of(r.result) == 0u && cmp_of(r.result) == n / 4u && r.result == r.today);
    }
    {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.rmode = E_TIMEOUT;
        const ShOut r = run_shadow(m, s, q, d_at, n, 5u, -1, -1);
        expect("T34f SHADOW read-back timeout: taken, no SDMA view (nothing compared), the chunk decided by MM (clean), the path latched "
               "and the buffer retired", r.taken && !r.sdOk && r.rec.compared == 0u && r.result == r.today && bad_of(r.result) == 0u &&
               s.latched_off && q.rb_retired && m.unlocks == 1);
        Machine m2; n48_fc_state s2 {}; n48_fc89_state q2 = usable();
        m2.rmode = E_REFUSED;
        const ShOut r2 = run_shadow(m2, s2, q2, d_at, n, 5u, -1, -1);
        expect("T34f SHADOW refused: NOT TAKEN, the lock still held (today's path runs)", !r2.taken && m2.locked == 1 && m2.unlocks == 0);
    }
    uint32_t bad = 0u;
    for (uint32_t it = 0; it < 150u; it++) {
        Machine m; n48_fc_state s {}; n48_fc89_state q = usable();
        m.seed = rnd();
        const uint32_t nn = 4u * (1u + rnd() % 0x2000u);
        const int64_t mf = rnd() % 3u ? -1 : (int64_t)(rnd() % (nn / 4u)), sf = rnd() % 3u ? -1 : (int64_t)(rnd() % (nn / 4u));
        if (rnd() % 4u == 0u) m.flip_byte = rnd() % nn;
        const ShOut r = run_shadow(m, s, q, 4096ull * (rnd() % 32u), nn, rnd() % 999u, mf, sf);
        if (!r.taken || r.result != r.today) bad++;
    }
    expect("T34f SHADOW over 150 random chunks: the decision is today's full MM verify's, word for word", bad == 0u);
    char line[1400];
    const int n1 = std::snprintf(line, sizeof line, N48_FC89_SH_FMT, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
    const int n2 = std::snprintf(line, sizeof line, N48_FC89_SH_DIS_FMT, ~0ull, ~0ull, ~0ull, ~0ull, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u,
                                 ~0u, ~0u, ~0u, ~0u);
    std::snprintf(line, sizeof line, "T34f SHADOW lines at their widest fit 491 bytes: report %d, disagreement %d", n1, n2);
    expect(line, n1 > 0 && n1 <= 491 && n2 > 0 && n2 <= 491);
    expect("T34f modes: 89 | 3 << 8 = 857 SHADOW; n48_fc89_mode_of keeps 1 and 3 only; extra: ON 1032 B, SHADOW n, OFF 0",
           (89u | (N48_FC89_M_SHADOW << 8)) == 857u && n48_fc89_mode_of(1u) == 1u && n48_fc89_mode_of(3u) == 3u &&
           n48_fc89_mode_of(2u) == 0u && n48_fc89_mode_of(4u) == 0u && n48_fc89_extra(1u, 4096u) == 1032u &&
           n48_fc89_extra(3u, 4096u) == 4096u && n48_fc89_extra(0u, 4096u) == 0u);
}

// =============================================================================================================================
// T34e the lines and the kext's wiring
// =============================================================================================================================
static bool has(const std::string &t, const char *s) { return t.find(s) != std::string::npos; }
static size_t count_of(const std::string &t, const char *s)
{
    size_t n = 0, at = 0; const size_t k = std::strlen(s);
    while ((at = t.find(s, at)) != std::string::npos) { n++; at += k; }
    return n;
}
static std::string fn_text(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    size_t i = src.find('{', a + std::strlen(head) - 1u);
    if (i == std::string::npos) return std::string();
    int depth = 0;
    for (size_t j = i; j < src.size(); j++) {
        if (src[j] == '{') depth++;
        else if (src[j] == '}') { if (--depth == 0) return src.substr(a, j - a + 1u); }
    }
    return std::string();
}
static bool order(const std::string &t, std::initializer_list<const char *> steps)
{
    size_t prev = 0; bool first = true;
    for (const char *st : steps) {
        const size_t at = t.find(st, first ? 0 : prev);
        if (at == std::string::npos || count_of(t, st) != 1u) return false;
        prev = at + 1; first = false;
    }
    return true;
}
static std::string slurp(const char *p)
{
    std::ifstream in(p ? p : "");
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
static uint64_t fnv(const std::string &t)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : t) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}
static std::string undo(std::string t, const char *now, const char *was)
{
    const size_t at = t.find(now);
    if (at == std::string::npos || count_of(t, now) != 1u) return std::string("<not found>");
    return t.replace(at, std::strlen(now), was);
}

// 0.0.534's lines in the two touched kext functions (undone below to 0.0.533's text, whose FNV-1a 64 the build recorded).
static const char kChunkIns[] =
    "\t// build 0.0.534 (switch 89, fastcopy89.h): a FULL chunk whose write landed is verified by an SDMA read-back + a sampled MM\n"
    "\t// cross-check. fc89_chunk answers 0 with gFastCopyLock STILL HELD (89 OFF, not usable, refused): everything below is 0.0.533's.\n"
    "\tif (r == N48_FC_RUN_OK && full) { const uint64_t r89 = fc89_chunk(s, l, pos, dAt, n, exp, expBytes, rot, stageUs, &sc, m89); if (r89) return r89; }\n";
static const char kAllocNow[] =
    "\tconst uint32_t m89 = full ? fc89_mode_now() : 0u;      // build 0.0.534 (switch 89): read once, before the allocation\n"
    "\tconst size_t expBytes = full ? (size_t)(n + n48_fc89_extra(m89, n)) : (size_t)N48_FC_SAMPLE_MAX * 8u;\n";
static const char kAllocWas[] = "\tconst size_t expBytes = full ? (size_t)n : (size_t)N48_FC_SAMPLE_MAX * 8u;\n";
static const char kExpNow[] =
    "    // build 0.0.534 (switch 90): BUSY (nothing read) and still TIMEOUT -> DEFER while the oldest live flight is under the cap.\n"
    "    const uint32_t d90 = locked ? 0u : ks90_busy(self, ctxSeq, fire, kdv, *nowOk, *nowUs, oldSeq, *blkAtUs);\n"
    "    const uint32_t xo = n48_fr_expiry_outcome(locked, *kdv);\n";
static const char kExpWas[] = "    const uint32_t xo = n48_fr_expiry_outcome(locked, *kdv);\n";
static const char kWdNow[] =
    "    else if (!d90) gKsX.withdrawn++;   // BUSY or WITHDRAWN: the withdrawal below is today's (a switch-90 deferral is counted there)\n";
static const char kWdWas[] = "    else gKsX.withdrawn++;   // BUSY or WITHDRAWN: the withdrawal below is today's\n";
static const char kNameNow[] =
    "              o.polled, n, o.unchanged, o.other, o.unreadable, o.in_bound, *kdv,\n"
    "              d90 ? \"gXdLock BUSY, nothing read: DEFERRED (switch 90, under 2 x the bound)\" : n48_fr_expiry_outcome_name(xo));\n";
static const char kNameWas[] =
    "              o.polled, n, o.unchanged, o.other, o.unreadable, o.in_bound, *kdv, n48_fr_expiry_outcome_name(xo));\n";
static const uint64_t kHashChunk533 = 0x11be9a1556bf62f1ull, kHashExp533 = 0x60c16a25ecf0cc41ull;

static void test_lines_and_wiring(const char *ahhPath, const char *nbPath, const char *cmPath)
{
    char line[1400];
    n48_fc89_state q; std::memset(&q, 0xff, sizeof q); q.rb_retired = 0u;
    n48_fc89_stats st; std::memset(&st, 0xff, sizeof st);
    const int n1 = std::snprintf(line, sizeof line, N48_FC89_FMT,
                                 N48_FC89_ARGS(0u, " - `gfxneuter 89` SET, but the read-back cannot run (see above)", &q, &st));
    const int n2 = std::snprintf(line, sizeof line, N48_KS90_LINE_FMT, (void *)~0ull, 0xffffffffu, 0xffffffffu, 0xffffffffu, ~0ull, ~0ull, ~0ull);
    const int n3 = std::snprintf(line, sizeof line, N48_KS90_REPORT_FMT, "OFF (602, default)",
                                 " - `gfxneuter 90` REFUSED: a continuous arm stands, unchanged", ~0ull, ~0ull, ~0ull, ~0ull);
    const int n4 = std::snprintf(line, sizeof line, N48_FR_X_LINE_FMT, (void *)~0ull, 0xffffffffu, 0xffffffffu, 0xffffffffu, ~0ull, ~0ull,
                                 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                 "gXdLock BUSY, nothing read: DEFERRED (switch 90, under 2 x the bound)");
    std::snprintf(line, sizeof line, "T34e lines at their widest fit n48log's 491-byte body: fastcopy89 %d, ksexp90 %d, ksexp90 report %d, "
                  "ksexp65 with the switch-90 outcome %d", n1, n2, n3, n4);
    expect(line, n1 > 0 && n1 <= 491 && n2 > 0 && n2 <= 491 && n3 > 0 && n3 <= 491 && n4 > 0 && n4 <= 491);
    if (std::getenv("FC89_VERBOSE")) std::printf("%s\n", line);

    const std::string ahh = slurp(ahhPath), nb = slurp(nbPath), cm = slurp(cmPath);
    expect("T34e the kext sources were given and read (AppleHardwareHook.cpp, Navi48Bringup.cpp, gfx_commit.h)",
           !ahh.empty() && !nb.empty() && !cm.empty());
    if (ahh.empty() || nb.empty() || cm.empty()) return;
    const std::string chunk = fn_text(nb, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,");
    const std::string c89 = fn_text(nb, "static __attribute__((noinline)) uint64_t fc89_chunk(FcSlot *s, IOLock *l, uint64_t pos, uint64_t dAt, uint64_t n, uint32_t *exp,");
    const std::string kick = fn_text(nb, "static uint32_t fc89_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {");
    const std::string set = fn_text(nb, "uint32_t navi48_fc89_set(uint32_t m) {");
    const std::string bind = fn_text(nb, "static uint32_t fc89_rb_bind() {");
    const std::string ctl = fn_text(nb, "static uint32_t fc89_control() {");
    const std::string exp = fn_text(ahh, "__attribute__((noinline)) static void ks_eop_at_expiry(const void *self, uint32_t ctxSeq, uint32_t fire, uint32_t *kdv,");
    const std::string busy = fn_text(ahh, "static __attribute__((noinline)) uint32_t ks90_busy(const void *self, uint32_t ctxSeq, uint32_t fire, uint32_t *kdv, uint32_t nowOk,");
    const std::string um = fn_text(ahh, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    expect("T34e the functions are found", !chunk.empty() && !c89.empty() && !kick.empty() && !set.empty() && !bind.empty() &&
           !ctl.empty() && !exp.empty() && !busy.empty() && !um.empty());

    // OFF identity of the two touched functions: 0.0.534's lines undone == 0.0.533's exact text
    std::string c533 = undo(chunk, kAllocNow, kAllocWas);
    { const size_t at = c533.find(kChunkIns); if (at != std::string::npos && count_of(c533, kChunkIns) == 1u) c533.erase(at, std::strlen(kChunkIns)); else c533 = "<not found>"; }
    std::string e533 = undo(undo(undo(exp, kExpNow, kExpWas), kWdNow, kWdWas), kNameNow, kNameWas);
    std::snprintf(line, sizeof line, "T34e OFF identity in the kext: navi48_fc_chunk minus switch 89's call site and mode read hashes to 0.0.533's "
                  "(%#llx), ks_eop_at_expiry with switch 90's three edits undone hashes to 0.0.533's (%#llx)",
                  (unsigned long long)fnv(c533), (unsigned long long)fnv(e533));
    expect(line, fnv(c533) == kHashChunk533 && fnv(e533) == kHashExp533);

    // switch 89's call site: under gFastCopyLock, after the write ran and its latched snapshot, before today's unlock
    expect("T34e navi48_fc_chunk: the write run, the snapshot of the latch, THEN fc89_chunk (only a landed FULL chunk), THEN today's unlock",
           order(chunk, { "const uint32_t r = n48_fc_chunk_run(&ops, &gFc,", "const uint32_t latched = gFc.latched_off",
                          "if (r == N48_FC_RUN_OK && full) { const uint64_t r89 = fc89_chunk(", "if (r89) return r89; }",
                          "\tIOLockUnlock(l);\n\ts->stageUs" }));
    // 0.0.534 review item 2: the mode read and the WHOLE allocation (exp + the mode's extra) come BEFORE gFastCopyLock is taken;
    // neither fc89_chunk nor fc89_shadow allocates.
    const std::string sh = fn_text(nb, "static __attribute__((noinline)) uint64_t fc89_shadow(FcSlot *s, IOLock *l, uint64_t dAt, uint64_t n, uint32_t *exp, size_t expBytes,\n"
                                       "                                                      uint32_t rot, uint64_t stageUs, const FcSub *wsc) {");
    expect("T34e ALLOCATION ORDER: navi48_fc_chunk reads 89's mode, sizes exp with its extra, allocates, THEN takes gFastCopyLock; "
           "fc89_chunk / fc89_shadow never allocate (got / sd live inside exp)",
           !sh.empty() && order(chunk, { "const uint32_t m89 = full ? fc89_mode_now() : 0u;",
                                         "const size_t expBytes = full ? (size_t)(n + n48_fc89_extra(m89, n))",
                                         "IOMalloc(expBytes)", "\t\tIOLockLock(l);\n\t\twhy = n48_fc_decide(",
                                         "fc89_chunk(s, l, pos, dAt, n, exp, expBytes, rot, stageUs, &sc, m89)" }) &&
           !has(c89, "IOMalloc") && !has(sh, "IOMalloc") && has(c89, "uint32_t *got = exp + n / 4u;") && has(sh, "uint32_t *sd = exp + n / 4u;") &&
           has(c89, "expBytes < (size_t)(n + n48_fc89_extra(m89, n))"));
    expect("T34e SHADOW (fc89_shadow): phase A under the lock, then TODAY's n48_fc_verify (FULL) through the recording read, the decision "
           "n48_fc89_shadow_decide from the MM answer, today's tail (latch on MM bad only)",
           order(sh, { "if (!n48_fc89_shadow_a(&ops, &gFc, &gFc89,", "rec.read = &fc_mm_read;",
                       "const uint64_t bad = n48_fc_verify(&n48_fc89_shadow_read, nullptr, &rec, dAt, exp, nullptr, (uint32_t)n, N48_FC_M_FULL, rot,",
                       "IOFree(exp, expBytes);", "const uint64_t r = n48_fc89_shadow_decide(bad, cmp, rf, o.run, o.rb_bad);",
                       "if (bad) n48_fc_latch(&gFc, N48_FC_LATCH_MISMATCH);", "return r;" }) && count_of(sh, "n48_fc_latch(") == 1u &&
           has(c89, "const uint64_t rs = fc89_shadow(s, l, dAt, n, exp, expBytes, rot, stageUs, wsc);"));   // 0.0.535 item 3: + the NOT TAKEN line
    expect("T34e fc89_chunk: 89 OFF returns 0 FIRST (nothing read, allocated or locked); then usable + DCC; the pure order; not taken "
           "returns 0 with the lock held; the write's times, exp freed; a failed read-back poisons the range as a dead range",
           order(c89, { "if (!m89) return 0ull;",
                        "if (!n48_fc89_use(1u, s->mode, &gFc89, gFc.latched_off) || (fc_dcc_raw() & N48_DCC_NOPTE_COMP_EN_MASK) ||",
                        "uint32_t *got = exp + n / 4u;",
                        "const uint64_t r = n48_fc89_chunk(&ops, &gFc, &gFc89, static_cast<uint32_t *>(gFc89Rb.cpu), gFc89RbMc,",
                        "if (!r) { gFc89S.not_taken++; fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs); return 0ull; }",
                        "s->stageUs += stageUs; s->dmaUs += wsc->dmaUs; s->lockUs += wsc->lockUs;",
                        "IOFree(exp, expBytes);", "if (o.run != N48_FC89_OK) {", "n48_cg_poison_mark_sticky(&gCgPoison, dAt, dAt + n);",
                        "navi48_ic_chunk_dead(pos, dAt, n);", "if (bad || o.rb_bad || o.x_bad) n48_fc_latch(&gFc, N48_FC_LATCH_MISMATCH);" }) &&
           c89.find("IOLockLock") > c89.find("if (!r) { gFc89S.not_taken++; fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs); return 0ull; }") &&
           has(nb, "static void fc89_unlock(void *vc) { IOLockUnlock(static_cast<Fc89Ctx *>(vc)->l); }"));
    expect("T34e fc89_kick: the source is the chunk's own VRAM, the destination EXACTLY the read-back buffer (<= its size), the queue "
           "idle, THEN the packet, ring, doorbell; no VRAM destination check (the engine writes only our buffer), no lock",
           order(kick, { "srcMc == gBringup.gmc.vram_start + c->dAt", "dstMc == gFc89RbMc && bytes != 0u && bytes <= N48_FC_STAGING_BYTES",
                         "if (n48_fc_submit_ok(destWhy, idle) != 1u) return N48_FC_SUB_REFUSED;",
                         "n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + N48_FC_WB_FENCE_OFF, fence);",
                         "amdgpu::sdma_ring_write(dev, inst, pkt, k)", "amdgpu::sdma_kick_doorbell(dev, inst)", "return 0u;" }) &&
           !has(kick, "IOLock") && !has(kick, "hdp_flush") &&
           has(nb, "static const n48_fc_wait_ops kFc89WaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc89_kick, &fc_fence_read, &fc_delay };"));
    expect("T34e switch 89: boots OFF; the buffer is bound and controlled on the VERB thread under gFastCopyLock (HIGH bump only); "
           "a failed control retires it",
           has(nb, "static volatile uint32_t gFc89Mode { 0u };") &&
           order(set, { "if (m == N48_FC89_M_OFF) { __atomic_store_n(&gFc89Mode, 0u, __ATOMIC_RELEASE); return 0u; }",
                        "if (!n48_fc89_mode_of(m)) return 11u;", "IOLockLock(l);",
                        "if (fc89_rb_bind() != 0u) st = 12u;", "else if (fc89_control() != 0u) st = 12u;",
                        "__atomic_store_n(&gFc89Mode, m, __ATOMIC_RELEASE);", "IOLockUnlock(l);" }) &&
           order(bind, { "if (!(gBringup.gmc.gart_high_bump)) {", "amdgpu::sysmem_alloc(gFc89Rb, N48_FC_STAGING_BYTES, 4096)",
                         "amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFc89Rb.bus, N48_FC_STAGING_BYTES, &mc);" }) &&
           order(ctl, { "const uint32_t nonce = n48_fc89_pc_nonce(fence, gFc89.pc_nonce);", "gFc89.pc_nonce = nonce; gFc89PcOff = off;",
                        "amdgpu::WBAR0_32(dev, off + i, n48_fc89_pc_word(i / 4u, nonce));",
                        "for (uint32_t i = 0; i < N48_FC_STAGING_BYTES / 4u; i++) rb[i] = ~n48_fc89_pc_word(i, nonce);",
                        "const uint32_t sub = fc89_wait(&sc, a.gpu_va, gFc89RbMc, N48_FC_STAGING_BYTES, fence);",
                        "const uint32_t run = n48_fc_fence_outcome(&gFc, sub);",
                        "if (run == N48_FC_RUN_OK) gFc89.pc_wrong = n48_fc89_pc_check(rb, N48_FC_STAGING_BYTES / 4u, nonce, firstIdx, first);",
                        "if (pass) gFc89.rb_ok = 1u;", "else if (run != N48_FC_RUN_REFUSED) gFc89.rb_retired = 1u;",
                        "run == N48_FC_RUN_REFUSED ? \"NOT RUN - refused before the ring (retried at the next ON)\"" }) &&
           !has(ctl, "fc_pc_word(") && has(nb, "\tgFcPcOff = off;") &&
           count_of(nb, "IOMalloc(") >= 1u && !has(c89, "sysmem_alloc") && !has(c89, "gmc_bind_existing"));
    // switch 90
    expect("T34e switch 90: boots OFF; ks90_busy reads it FIRST, then the pure verdict is WRITTEN BACK into hook_unmapVA's kdv",
           has(ahh, "static volatile uint32_t gKs90On { 0u };") &&
           order(busy, { "if (!gKs90On) return 0u;", "*kdv = n48_ks90_kdv(1u, 0u, *kdv, nowOk, nowUs, blkAtUs, kKsFlightUs, &what);",
                         "n48_ks90_count(&gKs90, what);", "if (what != N48_KS90_DEFER) return 0u;" }));
    expect("T34e ks_eop_at_expiry: the try-lock (busy = nothing read), the retirements' re-ask, THEN switch 90 (only when not locked), "
           "THEN the outcome",
           order(exp, { "if (gXdLock && IOLockTryLock(gXdLock)) {", "gKsX.busy++;",
                        "const uint32_t d90 = locked ? 0u : ks90_busy(self, ctxSeq, fire, kdv, *nowOk, *nowUs, oldSeq, *blkAtUs);",
                        "const uint32_t xo = n48_fr_expiry_outcome(locked, *kdv);" }) && count_of(ahh, "ks90_busy(") == 2u);
    expect("T34e hook_unmapVA (unchanged): the expiry check, THEN deferNow/held from ITS verdict, the early marker release, and the "
           "root[511] clear gated on !deferNow",
           order(um, { "ks_eop_at_expiry(self, e->seq, e->unmapFires + 1u, &kdv, &kdAnyLiveAtScan, &kdNowUs, &kdNowOk,",
                       "const uint32_t deferNow = n48_ksd_defer(kdv);", "const uint32_t held = n48_ks_withdraw_hold(kdv);",
                       "if (!held) ks_withdraw_leave_atomic(&e->ksWithdrawing);", "if (e->wrote && !e->withdrawn && !deferNow) {" }));
    // the two selectors and the mid-arm guard
    expect("T34e the selectors: 89 via navi48_fc89_set (M1 345 ON, M2 601 OFF), 90 via n48_ra_set (M1 346, M2 602); both mid-arm guarded "
           "in the kext and in gfx_commit.h's list; the report lines",
           has(ahh, "} else if ((arg & 0xffull) == 89ull) {") && has(ahh, "} else if ((arg & 0xffull) == 90ull) {") &&
           has(ahh, "const bool contRefused89 = n48_cm_cont_switch_refused(89u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") &&
           has(ahh, "const bool contRefused90 = n48_cm_cont_switch_refused(90u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") &&
           has(ahh, "else if (m == 1u || m == 2u || m == 3u) { set89 = navi48_fc89_set(m); if (set89) st = set89; }") &&
           (89u | 3u << 8) == 857u && N48_FC89_M_SHADOW == 3u &&
           has(ahh, "else { changed90 = n48_ra_set(m, &f90); if (changed90) gKs90On = f90; }") &&
           has(cm, "    case 89u:   /* build 0.0.534") && has(cm, "    case 90u:   /* build 0.0.534") &&
           (89u | 1u << 8) == 345u && (89u | 2u << 8) == 601u && (90u | 1u << 8) == 346u && (90u | 2u << 8) == 602u &&
           N48_FC89_M_ON == 1u && N48_FC89_M_OFF == 2u &&
           has(nb, "N48LOG(N48_FC89_FMT, N48_FC89_ARGS(fc89_mode_now(), why, &gFc89, &gFc89S));") && has(nb, "N48LOG(N48_FC89_SH_FMT,") &&
           has(ahh, "HWLOG(N48_KS90_REPORT_FMT, gKs90On ? \"ON (346)\" : \"OFF (602, default)\", why,"));
}

// =============================================================================================================================
// build 0.0.535 item 3 — T35: THE BOUNDED QUEUE-IDLE WAIT before a read-back kick, and the per-chunk NOT TAKEN line.
//   (a) busy for 3 polls, then idle within the bound: TAKEN (1), waited < bound; (b) busy past the bound with a moving clock: NOT TAKEN
//   (0) at >= 2000 us and no later than one step past it; (c) a clock that never moves and a queue that never idles: still bounded
//   (at most bound / step + 1 polls + 1 check - a mock that is polled more answers idle and records the overrun, which fails here);
//   (d) idle at once: no delay at all; the counters; the NOT TAKEN line's cap (64) and its reasons; the lines fit 491 bytes; and the
//   kext's wiring: the wait runs ONLY for a busy queue check inside fc89_kick, the reason reaches every NOT TAKEN return of fc89_chunk,
//   and 89 OFF still returns 0 first (OFF identity: nothing of this runs with 89 OFF).
// =============================================================================================================================
struct QMock { uint32_t busyFor, calls, delays, overrun; uint64_t t; uint32_t step; };
static uint32_t qm_idle(void *c) {
    QMock *m = static_cast<QMock *>(c);
    if (++m->calls > 100000u) { m->overrun = 1u; return 1u; }   // an unbounded wait: stop it and record it
    return m->calls > m->busyFor ? 1u : 0u;
}
static uint64_t qm_now(void *c) { return static_cast<QMock *>(c)->t; }
static void qm_delay(void *c, uint32_t us) { QMock *m = static_cast<QMock *>(c); m->delays++; m->t += m->step ? us : 0u; }
static void test_qwait535(const char *nbPath)
{
    uint64_t w = 0;
    QMock a { 3u, 0u, 0u, 0u, 1000000u, 1u };
    n48_fc89_qwait_ops o { &a, &qm_idle, &qm_now, &qm_delay };
    expect("T35a busy 3 polls then idle: TAKEN within the bound (4 checks, 3 delays, waited 30 us)",
           n48_fc89_qwait(&o, N48_FC89_QWAIT_US, &w) == 1u && a.calls == 4u && a.delays == 3u && w == 30u && !a.overrun);
    QMock b { 0xFFFFFFFFu, 0u, 0u, 0u, 5u, 1u };
    o.ctx = &b;
    expect("T35b busy past the bound: NOT TAKEN at >= 2000 us, never more than one step past it",
           n48_fc89_qwait(&o, N48_FC89_QWAIT_US, &w) == 0u && w >= N48_FC89_QWAIT_US && w <= N48_FC89_QWAIT_US + N48_FC89_QWAIT_STEP_US &&
           !b.overrun);
    QMock c { 0xFFFFFFFFu, 0u, 0u, 0u, 7u, 0u };   // the clock never moves
    o.ctx = &c;
    expect("T35c a stuck clock and a queue that never idles: still bounded (<= bound / step + 2 checks), NOT TAKEN",
           n48_fc89_qwait(&o, N48_FC89_QWAIT_US, &w) == 0u && !c.overrun && c.calls <= N48_FC89_QWAIT_US / N48_FC89_QWAIT_STEP_US + 2u);
    QMock d { 0u, 0u, 0u, 0u, 9u, 1u };
    o.ctx = &d;
    expect("T35d idle at once: 1, no delay", n48_fc89_qwait(&o, N48_FC89_QWAIT_US, &w) == 1u && d.delays == 0u && w == 0u);
    expect("T35 the bound is at most 2 ms", N48_FC89_QWAIT_US <= 2000u && N48_FC89_QWAIT_STEP_US > 0u);
    n48_fc89_stats st {};
    n48_fc89_qwait_note(&st, 1u, 30u); n48_fc89_qwait_note(&st, 0u, 2004u); n48_fc89_qwait_note(&st, 1u, 12u);
    expect("T35 the counters: waits 3, idle 2, busy 1, max 2004 us", st.qwaits == 3u && st.qwait_idle == 2u && st.qwait_busy == 1u &&
           st.qwait_max_us == 2004u);
    uint32_t lines = 0u, printed = 0u;
    for (uint32_t k = 0; k < 100u; k++) printed += n48_fc89_nt_line(&lines);
    expect("T35 the NOT TAKEN lines are capped at 64 per boot", printed == 64u && lines == N48_FC89_NT_LINES && N48_FC89_NT_LINES == 64u);
    expect("T35 the reasons are named apart", std::strcmp(n48_fc89_nt_name(N48_FC89_NT_QBUSY), n48_fc89_nt_name(N48_FC89_NT_OTHER)) != 0 &&
           std::strstr(n48_fc89_nt_name(N48_FC89_NT_QBUSY), "busy after the bounded wait") != nullptr);
    char buf[1024];
    const unsigned long long M = 18446744073709551615ull;
    int nb1 = std::snprintf(buf, sizeof buf, N48_FC89_NT_FMT, M, M, M, M, "SHADOW", n48_fc89_nt_name(N48_FC89_NT_OTHER), M, M);
    int nb2 = std::snprintf(buf, sizeof buf, N48_FC89_QW_FMT, 4294967295u, M, M, M, M, M, M, 4294967295u, 4294967295u);
    expect("T35 the NOT TAKEN line and the wait line fit 491 bytes at their widest", nb1 > 0 && nb1 <= 491 && nb2 > 0 && nb2 <= 491);
    const std::string nb = slurp(nbPath);
    const std::string kick = fn_text(nb, "static uint32_t fc89_kick("), c89 = fn_text(nb, "uint64_t fc89_chunk(FcSlot *s, IOLock *l,");
    const std::string ntf = fn_text(nb, "void fc89_nt(const FcSlot *s,");
    expect("T35 fc89_kick: the queue check, the bounded wait ONLY when it answered busy, the reason, THEN today's gate",
           order(kick, { "uint32_t qs = scanout_queue_check(dev, inst, &rptr);", "if (qs == kScanStQueueBusy) {",
                         "const uint32_t ok = n48_fc89_qwait(&kFc89QWait, N48_FC89_QWAIT_US, &gFc89WaitUs);",
                         "n48_fc89_qwait_note(&gFc89S, ok, gFc89WaitUs);", "if (ok) qs = kScanStOk;",
                         "gFc89NtWhy = qs == kScanStQueueBusy ? N48_FC89_NT_QBUSY : N48_FC89_NT_OTHER;",
                         "const uint32_t idle = (qs == kScanStOk && inst.wb_bus &&",
                         "if (n48_fc_submit_ok(destWhy, idle) != 1u) return N48_FC_SUB_REFUSED;" }) &&
           count_of(kick, "n48_fc89_qwait(") == 1u &&
           // build 0.0.542: the one other caller is `scanout full`'s sf_kick (the same bounded wait, without 89's counters)
           count_of(nb, "n48_fc89_qwait(") == 2u &&
           count_of(fn_text(nb, "static uint32_t sf_kick("), "if (n48_fc89_qwait(&kFc89QWait, N48_FC89_QWAIT_US, &w)) qs = kScanStOk;") == 1u &&
           has(nb, "static const n48_fc89_qwait_ops kFc89QWait = { nullptr, &fc89_q_idle, &fc_now_us, &fc_delay };"));
    expect("T35 fc89_chunk: 89 OFF still returns 0 FIRST; every NOT TAKEN return names the chunk (3 sites), with the kick's reason",
           order(c89, { "if (!m89) return 0ull;", "fc89_nt(s, pos, dAt, n, m89, N48_FC89_NT_OTHER, 0u);",
                        "gFc89NtWhy = N48_FC89_NT_OTHER; gFc89WaitUs = 0u;",
                        "if (!rs) fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs);",
                        "if (!r) { gFc89S.not_taken++; fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs); return 0ull; }" }) &&
           count_of(c89, "fc89_nt(") == 3u);
    expect("T35 fc89_nt: counted by reason, printed only under the 64-line cap, copy # and chunk index from the slot and the offset",
           order(ntf, { "if (why == N48_FC89_NT_QBUSY) gFc89S.nt_qbusy++; else gFc89S.nt_other++;", "if (!n48_fc89_nt_line(&gFc89NtLines)) return;",
                        "N48LOG(N48_FC89_NT_FMT, (unsigned long long)s->copyNo, (unsigned long long)(pos / N48_FC_STAGING_BYTES)," }) &&
           has(nb, "s->copyNo = navi48_peer_copy_seq() + 1u;") && has(nb, "N48LOG(N48_FC89_QW_FMT, N48_FC89_QWAIT_US,"));
}

int main(int argc, char **argv)
{
    test_qwait535(argc > 2 ? argv[2] : nullptr);
    test_cases();
    test_resprov();
    test_random_and_off();
    test_ks90();
    test_control_fresh();
    test_shadow();
    test_lines_and_wiring(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("gfx_fc89: %d check(s), %d failed\n", gChecks, gFails);
    std::printf("gfx_fc89: %s\n", gFails ? "N48-FC89-TEST-FAIL" : "N48-FC89-TEST-PASS");
    return gFails ? 1 : 0;
}
