// fastcopy.h — build 0.0.496 (notes/design/FAST-PAGEIN.md): THE RESIDENCY COPY THROUGH SDMA, switch 63.
//
// WHY. Every residency copy (pageTexture, system memory -> VRAM) runs through the MM window at 1.87 MB/s (: run10g 1,327
// copies, 167.7 MB in 89.8 s; ~2.07 us per dword, SUSPECTED ~90% of it the per-batch MM read-back). The hybrid desktop needs
// 80-370x that. This header is the PURE half of the fix: the CPU still produces EXACTLY today's byte stream (the same 256-byte
// batches from the same producer - 0.0.492's rp_retile_bytes, or readBytes followed by 0.0.495's in-copy overlay), but it
// produces it into a GART-mapped system-memory STAGING buffer; our SDMA0 QUEUE0 then copies that staging buffer, one COPY_LINEAR
// packet, into the SAME pre-checked VRAM range the MM loop would have written, the copy waits for its own fence, and the MM
// window verifies a sample (or, M 3, every dword). Only the transport changes: which bytes land, and where, do not.
//
// `accel gfxneuter 63 | M << 8`: M 1 ON, sampled verify (319); M 2 OFF (575, the default and the boot value); M 3 ON, full MM
// verify (831); bare 63 prints the report. Latched PER COPY (the first chunk's reading holds for every later chunk of that copy).
//
// What lives here (host-tested in tests/gfx_copyguard_test.cpp section T19; the kext compiles THIS header):
//   n48_fc_chunk_len     the chunk plan - the MM loop's own arithmetic, which that loop now calls (one copy, not two);
//   n48_fc_decide        SDMA or the MM reason (OFF answers MM for every input; DCC COMP_EN set answers MM);
//   n48_fc_submit_ok     the last gate before the ring is written: the VRAM guard's answer for THIS chunk, the queue idle;
//   n48_fc_build_packet  COPY_LINEAR exactly as scanout_sdma_copies emits it (8 dwords, CPV 1) plus one FENCE (4 dwords);
//   n48_fc_fence_next    a per-submission fence value never equal to any earlier one (0 = exhausted: the path latches off);
//   n48_fc_poll_*        the fence wait's schedule and hard bound;
//   n48_fc_fence_outcome landed / timeout -> FAILED + the path latched OFF for the boot + the staging buffer RETIRED;
//   n48_fc_sample_plan   the sampled verify: the chunk's first and last dword and one dword in every 4 KiB page, rotating;
//   n48_fc_chunk_run     THE ORDER, driven through callbacks: every batch produced into staging (the overlay inside the
//                        producer) -> the expected values snapshotted -> the fence value -> the submission -> the outcome.
//   n48_fc_verify        the MM-window verify against the snapshot, run by the kext AFTER both locks are released (0.0.514:
//                        the sampled branch reads its whole plan through ONE `plan` call - one gVramMmLock acquire per chunk).
//   0.0.509: n48_fc_submit_wait (the submission's order; the fence bound counts from the kick), n48_fc_copy_mode and
//            n48_fc_cg_close_wrote/n48_fc_hg_clean (F-3: sampled verify never vouches for a switch-62 range), n48_fc_run_dead
//            (F-2: a timed-out chunk's range stays poisoned for the boot), n48_fc_vmax_note (the verify outlier record).
//
// Pure C that compiles as C++; no heap, no libc beyond memcpy.
#ifndef N48_FASTCOPY_H
#define N48_FASTCOPY_H

#include <stdint.h>
#include <string.h>
#include "sdma_dcc.h"   // N48_DCC_NOPTE_COMP_EN_MASK: the no-PTE compression bits that must read clear (0.0.418,)

#ifdef __cplusplus
extern "C" {
#endif

// ---- the switch ---------------------------------------------------------------------------------------------------------------
#define N48_FC_SWITCH       63u
#define N48_FC_M_SAMPLED    1u      // 63 | 1 << 8 = 319: ON, sampled MM verify
#define N48_FC_M_OFF        2u      // 63 | 2 << 8 = 575: OFF (default, boot value)
#define N48_FC_M_FULL       3u      // 63 | 3 << 8 = 831: ON, every dword verified through the MM window
// The latched per-copy mode: 0 = OFF (the MM loop, exactly 0.0.495), else N48_FC_M_SAMPLED or N48_FC_M_FULL.
static inline uint32_t n48_fc_mode_of(uint32_t m) { return (m == N48_FC_M_SAMPLED || m == N48_FC_M_FULL) ? m : 0u; }

// ---- capacities -----------------------------------------------------------------------------------------------------------------
#define N48_FC_STAGING_BYTES (1u << 20)   // == Navi48AccelPeer.cpp kCopyChunkBytes (static_assert there): one chunk, one submission
#define N48_FC_BATCH_BYTES   256u         // the MM loop's batch: the producer is called with exactly the same (offset, take) pairs
#define N48_FC_PAGE_BYTES    4096u
#define N48_FC_SAMPLE_MAX    (N48_FC_STAGING_BYTES / N48_FC_PAGE_BYTES + 2u)   // 258: first, last, one per 4 KiB page
#define N48_FC_PKT_DWORDS    12u          // COPY_LINEAR (8, CPV 1) + FENCE (4)
#define N48_FC_WB_FENCE_OFF  0x1C0u       // SDMA0's write-back page: 0x00/0x40/0x80/0xC0 QUEUE0, 0x100-0x138 external rptrs,
                                          // 0x140 external wptr, 0x180 external fence - 0x1C0 is the next free 64-byte slot
#define N48_FC_FENCE_BASE    0xFC000001u  // the first fence value; never 0 (the slot's zero-filled state)
#define N48_FC_FENCE_TIMEOUT_US 200000u   // the hard bound on one chunk's fence (a 1 MiB copy is ~1 ms at 1 GB/s)
#define N48_FC_POLL_FINE_US  500u         // IODelay(5) for the first 500 us, then IODelay(50) to 2 ms, then IOSleep(1)

// ---- the packet (the SDMA v7 encoding amdgpu_sdma.h names; the kext static_asserts these against it) ---------------------------
#define N48_FC_SDMA_OP_COPY          1u
#define N48_FC_SDMA_OP_FENCE         5u
#define N48_FC_SDMA_SUBOP_COPY_LINEAR 0u
#define N48_FC_HDR_COPY_LINEAR_CPV1  ((N48_FC_SDMA_OP_COPY & 0xffu) | ((N48_FC_SDMA_SUBOP_COPY_LINEAR & 0xffu) << 8) | (1u << 28))
#define N48_FC_HDR_FENCE             (N48_FC_SDMA_OP_FENCE & 0xffu)

// ---- decision reasons (index 0 = SDMA; every other value keeps the MM loop, nothing staged, nothing written) ---------------------
enum {
    N48_FC_SDMA = 0,
    N48_FC_MM_OFF,          // 1 switch 63 OFF (latched for this copy)
    N48_FC_MM_LATCHED,      // 2 the path is latched OFF for the boot (a fence timeout, a failed control, a verify mismatch)
    N48_FC_MM_NO_PC,        // 3 the positive control has not passed
    N48_FC_MM_NO_STAGING,   // 4 no staging buffer, or it was retired
    N48_FC_MM_NO_SDMA,      // 5 SDMA0 QUEUE0 not up, or busy
    N48_FC_MM_DCC,          // 6 SDMA0_DCC_CNTL has a no-PTE COMP_EN bit set (the engine would compress what it writes)
    N48_FC_MM_SHAPE,        // 7 the chunk: 0 bytes, over the staging buffer, not whole dwords, a misaligned or zero destination
    N48_FC_MM_NO_MEM,       // 8 the verify snapshot could not be allocated (kext only)
    N48_FC_MM_NO_SLOT,      // 9 no per-thread copy slot (kext only)
    N48_FC_MM_REASONS
};
static inline const char *n48_fc_reason_name(uint32_t r)
{
    static const char *const k[N48_FC_MM_REASONS] = { "SDMA", "off", "latched-off", "no-control", "no-staging", "no-sdma",
                                                      "dcc-comp", "shape", "no-mem", "no-slot" };
    return r < N48_FC_MM_REASONS ? k[r] : "?";
}

// The chunk plan: THE MM LOOP'S OWN ARITHMETIC (residency_copy_to_vram through 0.0.495, now a call to this): at most the
// destination segment's span, at most `chunk_max`, whole dwords until the tail. `w_bytes` is the copy's length, `pos` how much
// is already written, `d_span` what the destination segment at `pos` still holds.
static inline uint64_t n48_fc_chunk_len(uint64_t w_bytes, uint64_t pos, uint64_t d_span, uint64_t chunk_max)
{
    if (pos >= w_bytes) return 0u;
    uint64_t n = w_bytes - pos;
    if (d_span < n) n = d_span;
    if (n > chunk_max) n = chunk_max;
    if (n < w_bytes - pos) n &= ~3ULL;
    return n;
}

// SDMA, or why not. `on`: the latched mode is ON. `sdma_up`: QUEUE0 inited, enabled, idle and the MC base is the one the
// scanout path proved. `dcc_raw`: SDMA0_DCC_CNTL as read now. `bytes`: the copy's whole length; `d_at`, `n`: this chunk.
static inline uint32_t n48_fc_decide(uint32_t on, uint32_t sdma_up, uint32_t dcc_raw, uint32_t pc_passed, uint32_t latched_off,
                                     uint32_t staging_ok, uint64_t bytes, uint64_t d_at, uint64_t n)
{
    if (!on) return N48_FC_MM_OFF;
    if (latched_off) return N48_FC_MM_LATCHED;
    if (!pc_passed) return N48_FC_MM_NO_PC;
    if (!staging_ok) return N48_FC_MM_NO_STAGING;
    if (!sdma_up) return N48_FC_MM_NO_SDMA;
    if (dcc_raw & N48_DCC_NOPTE_COMP_EN_MASK) return N48_FC_MM_DCC;
    if (n == 0u || bytes == 0u || n > bytes || n > N48_FC_STAGING_BYTES || (n & 3u) || !d_at || (d_at & 3u)) return N48_FC_MM_SHAPE;
    return N48_FC_SDMA;
}

// The last gate, asked under gScanoutLock right before the ring is written: the VRAM guard's answer for exactly [d_at, d_at + n)
// (navi48_vram_apple_dest_check, asked AGAIN here although the copy loop asked it for the same chunk) and the queue still idle.
static inline uint32_t n48_fc_submit_ok(uint32_t dest_why, uint32_t queue_idle)
{
    return (dest_why == 0u && queue_idle) ? 1u : 0u;
}

// COPY_LINEAR exactly as scanout_sdma_copies (Navi48Bringup.cpp) emits it - header, count - 1, 0, src lo/hi, dst lo/hi, 0 - then
// that function's FENCE - header, address lo/hi, value. Returns the dwords written (N48_FC_PKT_DWORDS) or 0 when `cap` is short
// or `bytes` is 0.
static inline uint32_t n48_fc_build_packet(uint32_t *pkt, uint32_t cap, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes,
                                           uint64_t fence_mc, uint32_t fence_value)
{
    if (!pkt || cap < N48_FC_PKT_DWORDS || bytes == 0u) return 0u;
    uint32_t k = 0u;
    pkt[k++] = N48_FC_HDR_COPY_LINEAR_CPV1;
    pkt[k++] = bytes - 1u;
    pkt[k++] = 0u;
    pkt[k++] = (uint32_t)src_mc;
    pkt[k++] = (uint32_t)(src_mc >> 32);
    pkt[k++] = (uint32_t)dst_mc;
    pkt[k++] = (uint32_t)(dst_mc >> 32);
    pkt[k++] = 0u;
    pkt[k++] = N48_FC_HDR_FENCE;
    pkt[k++] = (uint32_t)fence_mc;
    pkt[k++] = (uint32_t)(fence_mc >> 32);
    pkt[k++] = fence_value;
    return k;
}

// ---- the path's state (one per boot; under gFastCopyLock in the kext) -----------------------------------------------------------
typedef struct {
    uint32_t pc_passed;       // the positive control passed
    uint32_t latched_off;     // never cleared: a fence timeout, a failed control, a verify mismatch, fence values exhausted
    uint32_t staging_ok;      // a staging buffer is bound
    uint32_t staging_retired; // never cleared: the engine may still write it (a fence that did not land) - never reused
    uint32_t fence_last;      // the last fence value handed out (0 = none yet)
    uint32_t timeouts;        // fences that did not land in the bound
    uint32_t latch_why;       // the first latch's cause (N48_FC_LATCH_*)
} n48_fc_state;
enum { N48_FC_LATCH_NONE = 0, N48_FC_LATCH_TIMEOUT, N48_FC_LATCH_PC, N48_FC_LATCH_MISMATCH, N48_FC_LATCH_FENCES, N48_FC_LATCH_RING };

static inline uint32_t n48_fc_staging_usable(const n48_fc_state *s) { return (s && s->staging_ok && !s->staging_retired) ? 1u : 0u; }

static inline void n48_fc_latch(n48_fc_state *s, uint32_t why)
{
    if (!s) return;
    if (!s->latched_off) s->latch_why = why;
    s->latched_off = 1u;
}

// A value never equal to any earlier one this boot: strictly increasing from N48_FC_FENCE_BASE. 0 = exhausted (the path latches).
static inline uint32_t n48_fc_fence_next(n48_fc_state *s)
{
    if (!s) return 0u;
    if (s->fence_last == 0xFFFFFFFFu) { n48_fc_latch(s, N48_FC_LATCH_FENCES); return 0u; }
    s->fence_last = s->fence_last ? s->fence_last + 1u : N48_FC_FENCE_BASE;
    return s->fence_last;
}

// The fence wait. `elapsed_us` since the doorbell; the next delay, and whether the wait is over.
static inline uint32_t n48_fc_poll_delay_us(uint64_t elapsed_us)
{
    if (elapsed_us < N48_FC_POLL_FINE_US) return 5u;
    if (elapsed_us < 2000u) return 50u;
    return 1000u;
}
enum { N48_FC_POLL_WAIT = 0, N48_FC_POLL_LANDED = 1, N48_FC_POLL_TIMEOUT = 2 };
static inline uint32_t n48_fc_poll_state(uint32_t seen, uint32_t want, uint64_t elapsed_us)
{
    if (want != 0u && seen == want) return N48_FC_POLL_LANDED;
    if (elapsed_us >= N48_FC_FENCE_TIMEOUT_US) return N48_FC_POLL_TIMEOUT;
    return N48_FC_POLL_WAIT;
}

// What the submission callback answers.
enum { N48_FC_SUB_LANDED = 1, N48_FC_SUB_TIMEOUT = 2, N48_FC_SUB_REFUSED = 3, N48_FC_SUB_RING = 4 };

// ---- build 0.0.509 item 1: THE SUBMISSION'S ORDER AND CLOCK ----------------------------------------------------------------
// fc2 latched the path OFF on a fence that DID land: 0.0.508's fc_submit took its clock BEFORE waiting for gScanoutLock,
// so the 200 ms bound also counted the time the scanout thumbnail held that lock (382,123 us "elapsed", the engine not hung).
// This is the kext's submission, driven through callbacks so the host test runs exactly this order: the entry time, the lock,
// the lock wait (reported, never bounded), the gate + packet + ring + doorbell (`kick`), and ONLY THEN the bound's clock. The
// bound's VALUE (N48_FC_FENCE_TIMEOUT_US) and the poll schedule are unchanged.
typedef struct {
    uint64_t (*now_us)(void *ctx);   // a monotonic microsecond clock
    void (*lock)(void *ctx);         // gScanoutLock
    void (*unlock)(void *ctx);
    // under the lock: the last gate, the packet, the ring write and the doorbell. 0 = kicked; else N48_FC_SUB_REFUSED (nothing
    // written to the ring) or N48_FC_SUB_RING (the ring write or the doorbell failed).
    uint32_t (*kick)(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t fence_value);
    uint32_t (*fence)(void *ctx);    // the fence dword as it reads now
    void (*delay)(void *ctx, uint32_t us);
} n48_fc_wait_ops;
typedef struct {
    uint64_t lock_us;   // lock taken minus entry: the wait for gScanoutLock (0.0.508 counted it inside the bound)
    uint64_t el_us;     // the fence wait, counted FROM THE KICK (the bound applies to this)
    uint32_t seen;      // the fence dword's last reading
    uint32_t kicked;    // 1 = the doorbell was rung (the engine may write VRAM whatever the outcome)
} n48_fc_wait_out;
static inline uint32_t n48_fc_submit_wait(const n48_fc_wait_ops *o, void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes,
                                          uint32_t fence, n48_fc_wait_out *out)
{
    out->lock_us = 0u; out->el_us = 0u; out->seen = 0u; out->kicked = 0u;
    const uint64_t t_entry = o->now_us(ctx);
    o->lock(ctx);
    const uint64_t t_locked = o->now_us(ctx);
    out->lock_us = t_locked >= t_entry ? t_locked - t_entry : 0u;
    const uint32_t k = o->kick(ctx, src_mc, dst_mc, bytes, fence);
    if (k) { o->unlock(ctx); return k; }
    out->kicked = 1u;
    const uint64_t t_kick = o->now_us(ctx);   // THE BOUND COUNTS FROM HERE: after the doorbell, never before the lock
    uint32_t ps = N48_FC_POLL_WAIT, v = 0u;
    uint64_t el = 0u;
    for (uint32_t guard = 0; guard < 1000000u; guard++) {          // bounded twice: the elapsed time and the iterations
        v = o->fence(ctx);
        const uint64_t tn = o->now_us(ctx);
        el = tn >= t_kick ? tn - t_kick : 0u;
        ps = n48_fc_poll_state(v, fence, el);
        if (ps != N48_FC_POLL_WAIT) break;
        o->delay(ctx, n48_fc_poll_delay_us(el));
    }
    o->unlock(ctx);
    out->el_us = el; out->seen = v;
    return ps == N48_FC_POLL_LANDED ? N48_FC_SUB_LANDED : N48_FC_SUB_TIMEOUT;
}

// ---- build 0.0.509 item 3 (F-3 of the 0.0.496 review,): SAMPLED VERIFY NEVER VOUCHES FOR A 62 RANGE ---------------
// The mode a copy runs under, fixed at its first chunk: sampled verify (319) on a copy that overlaps a range switch 62 registered
// (its copy bumped the heap generation: a patch, an unpatchable program, or the substituted-VRAM registry) is upgraded to FULL
// verify. Every other input keeps its mode (OFF stays OFF: 0).
static inline uint32_t n48_fc_copy_mode(uint32_t mode, uint32_t overlaps62)
{
    return (mode == N48_FC_M_SAMPLED && overlaps62) ? N48_FC_M_FULL : mode;
}
// The copy's close: a copy with ANY chunk the SDMA path verified by sampling (`sampled` 1) was not read back in full, so it may
// neither clear the copy guard's poison nor (switch 62) count as clean - clean clears 62's poison and drives F1's prune. It
// still POISONS on a failure or a mismatch. Returns the `wrote` the copy guard's close is given: 0 makes n48_cg_close_poison
// neither mark nor clear, so it is used only when there is nothing to mark.
static inline uint32_t n48_fc_cg_close_wrote(uint32_t wrote, uint32_t failed, uint32_t mismatch, uint32_t sampled)
{
    return (sampled && !failed && !mismatch) ? 0u : wrote;
}
// build 0.0.510 A1 (the 0.0.509 review's MEDIUM): under 319 a non-latching failure (a FILL, a REFUSED chunk, a
// verify-read failure, an MM-loop failure) poisons the copy's range in the copy guard, and only a copy read back IN FULL may clear
// that poison (n48_fc_cg_close_wrote) - so a sampled retry never could. The per-copy latch now also upgrades to FULL when the
// copy's range overlaps a live copy-guard poison row (`cg_poisoned`: the kext asks n48_cg_poison_overlaps over its own table for
// [dAt, dAt + wBytes) at the copy's first chunk), so the retry that completes clean clears it. Every other input keeps
// n48_fc_copy_mode's answer.
// build 0.0.521 Part D: `rp_candidate` - the copy is a resprov CANDIDATE (the copier's `retile` = switch 59's
// resprov ON and the image re-tiled / backing-sourced / a known asset, all decided BEFORE the first chunk). Resprov records a copy
// only when it was read back IN FULL (ws_resprov.h n48_rp_record: `c->compared * 4ull < c->bytes` -> N48_RP_REC_UNVERIFIED), and
// a sampled SDMA chunk credits only its sample count (n48_fc_verify: `cmp = ns`), so under 319 every candidate was refused and no
// texture was ever proven (run10x: 0 RECORDED of 8308 copies). Such a copy is latched FULL: every SDMA chunk (n bytes, n % 4 == 0,
// the only shape SDMA takes) credits n / 4 dwords, and every chunk the MM loop runs (the tail included) credits ceil(take / 4) per
// batch, so compared * 4 >= the copy's bytes, as the rung asks. Every other copy (buffers: ~96% of the bytes) keeps its mode; OFF
// stays OFF; the rung itself is unchanged.
static inline uint32_t n48_fc_latch_mode(uint32_t mode, uint32_t overlaps62, uint32_t cg_poisoned, uint32_t rp_candidate)
{
    return n48_fc_copy_mode(mode, (overlaps62 || cg_poisoned || rp_candidate) ? 1u : 0u);
}
// 62's completion: clean only when the copy was read back in full (no sampled chunk).
static inline uint32_t n48_fc_hg_clean(uint32_t clean, uint32_t sampled) { return (clean && !sampled) ? 1u : 0u; }
// The copy slot's answer at the close: the copy ran some chunk through SDMA under sampled verify.
static inline uint32_t n48_fc_copy_sampled(uint32_t mode, uint32_t chunks_sdma)
{
    return (mode == N48_FC_M_SAMPLED && chunks_sdma) ? 1u : 0u;
}
// What n48_fc_chunk_run answers.
enum { N48_FC_RUN_OK = 0, N48_FC_RUN_FILL = 1, N48_FC_RUN_REFUSED = 2, N48_FC_RUN_TIMEOUT = 3, N48_FC_RUN_RING = 4,
       N48_FC_RUN_FENCES = 5, N48_FC_RUN_SHAPE = 6 };
// build 0.0.510 A1: a chunk REFUSED before the ring was written (the VRAM guard's second answer, or F-5: the queue busy after
// staging) at the copy's FIRST chunk (`pos` 0) has written NOTHING of this copy: the MM loop runs it (as a declined chunk) instead
// of failing the copy, so no poison is left behind for the retry to clear. A refusal at pos > 0 (earlier chunks landed) still
// fails the copy, as in 0.0.509.
static inline uint32_t n48_fc_refused_to_mm(uint32_t run, uint64_t pos)
{
    return (run == N48_FC_RUN_REFUSED && pos == 0u) ? 1u : 0u;
}
// ---- build 0.0.509 item 2 (F-2 of the 0.0.496 review): a chunk whose doorbell was rung and whose fence did not land (a
// timeout), or whose ring write/doorbell failed part-way, may still be written by a LATE DMA: its VRAM range is poisoned for the
// rest of the boot (the copy guard's sticky row, 62's sticky entry), which no later clean copy clears.
static inline uint32_t n48_fc_run_dead(uint32_t run) { return (run == N48_FC_RUN_TIMEOUT || run == N48_FC_RUN_RING) ? 1u : 0u; }

// THE FENCE OUTCOME. Only a landed fence is success. A timeout FAILS the copy (the caller poisons its range and keeps the skip,
// as every FAILED path does), LATCHES the path OFF for the boot and RETIRES the staging buffer (the engine may still read it and
// write VRAM later; it is never handed out again and never freed). A refusal before the doorbell wrote nothing to the ring: the
// copy fails, the path stays usable. A ring write that did not complete (short, or the doorbell refused) latches the path off.
static inline uint32_t n48_fc_fence_outcome(n48_fc_state *s, uint32_t sub)
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

// THE SAMPLED VERIFY'S PLAN: byte offsets (dword-aligned) inside an `n`-byte chunk - the first dword, the last dword, then one
// dword in EVERY 4 KiB page (the last page may be partial), at an offset that rotates with `rot` from chunk to chunk. Returns the
// count written to `off` (<= cap); 0 when `n` is not whole dwords or `cap` cannot hold the plan.
static inline uint32_t n48_fc_sample_count(uint32_t n) { return n < 4u || (n & 3u) ? 0u : 2u + (n + N48_FC_PAGE_BYTES - 1u) / N48_FC_PAGE_BYTES; }
// Sample i of the plan (i < n48_fc_sample_count(n)): computed, never stored, so neither the kext nor this header keeps a
// 258-entry table on the stack.
static inline uint32_t n48_fc_sample_at(uint32_t n, uint32_t rot, uint32_t i)
{
    if (i == 0u) return 0u;
    if (i == 1u) return n - 4u;
    const uint32_t p = i - 2u;
    const uint32_t lo = p * N48_FC_PAGE_BYTES;
    const uint32_t len = (n - lo) < N48_FC_PAGE_BYTES ? (n - lo) : N48_FC_PAGE_BYTES;
    return lo + ((rot + p * 97u) % (len / 4u)) * 4u;
}
static inline uint32_t n48_fc_sample_plan(uint32_t n, uint32_t rot, uint32_t *off, uint32_t cap)
{
    const uint32_t want = n48_fc_sample_count(n);
    if (!want || !off || cap < want) return 0u;
    for (uint32_t i = 0; i < want; i++) off[i] = n48_fc_sample_at(n, rot, i);
    return want;
}

// ---- THE ORDER --------------------------------------------------------------------------------------------------------------------
// The kext's per-chunk sequence, driven through callbacks so the host test runs exactly this function with a mock producer,
// a mock engine and a mock VRAM. Called with gFastCopyLock held (the staging buffer is this call's alone); `submit` takes
// gScanoutLock itself, only around the ring write and the fence wait.
typedef struct {
    void *ctx;
    // the batch producer: bytes [off, off + take) of THIS chunk into `dst` (the kext: rp_retile_bytes, or ic_read = readBytes then
    // 0.0.495's overlay). 1 = produced.
    int (*fill)(void *ctx, uint8_t *dst, uint64_t off, uint32_t take);
    // the submission: ring write, doorbell, fence wait (N48_FC_SUB_*).
    uint32_t (*submit)(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t fence_value);
} n48_fc_ops;

// `stg`/`stg_mc`: the staging buffer (CPU view / GART MC). `dst_mc`: the chunk's VRAM MC address. `exp`: the verify snapshot,
// N48_FC_SAMPLE_MAX dwords (sampled: exp[i] = the dword at plan offset i) or n / 4 dwords (full). `lead`: when the chunk starts
// below resource offset 128 (`pos`), its bytes [pos, min(128, pos + n)) are copied to lead[0..) - the copy loop builds the log's
// head (the first 32 dwords) and first two source dwords from them, exactly as its batch loop would have.
#define N48_FC_LEAD_BYTES 128u
static inline uint32_t n48_fc_chunk_run(const n48_fc_ops *o, n48_fc_state *s, uint8_t *stg, uint64_t stg_mc, uint64_t dst_mc,
                                        uint32_t n, uint32_t *exp, uint32_t mode, uint32_t rot, uint64_t pos, uint8_t *lead)
{
    if (!o || !o->fill || !o->submit || !s || !stg || !exp || n == 0u || (n & 3u) || n > N48_FC_STAGING_BYTES)
        return N48_FC_RUN_SHAPE;
    // 1. THE BYTE STREAM, batch by batch, exactly the MM loop's (offset, take) pairs: every byte the engine will copy is produced
    //    - and, for a patched shader heap, overlaid - HERE, before anything is submitted.
    for (uint32_t k = 0; k < n; ) {
        const uint32_t take = (n - k) >= N48_FC_BATCH_BYTES ? N48_FC_BATCH_BYTES : (n - k);
        if (!o->fill(o->ctx, stg + k, (uint64_t)k, take)) return N48_FC_RUN_FILL;
        k += take;
    }
    // 2. The verify's expected values and the log's head, from the staged stream (the staging buffer is reused by the next
    //    chunk once gFastCopyLock is released, and the verify runs after that).
    if (mode == N48_FC_M_FULL) memcpy(exp, stg, n);
    else {
        const uint32_t ns = n48_fc_sample_count(n);
        if (!ns || ns > N48_FC_SAMPLE_MAX) return N48_FC_RUN_SHAPE;
        for (uint32_t i = 0; i < ns; i++) memcpy(&exp[i], stg + n48_fc_sample_at(n, rot, i), 4);
    }
    if (lead && pos < N48_FC_LEAD_BYTES) {
        const uint64_t want = N48_FC_LEAD_BYTES - pos;
        memcpy(lead, stg, (size_t)(want < n ? want : n));
    }
    // 3. A fence value never used before, then 4. the submission, then 5. its outcome.
    const uint32_t fence = n48_fc_fence_next(s);
    if (!fence) return N48_FC_RUN_FENCES;
    const uint32_t sub = o->submit(o->ctx, stg_mc, dst_mc, n, fence);
    return n48_fc_fence_outcome(s, sub);
}

// THE VERIFY, through the MM window (`read`: up to 32 dwords at a 0-based VRAM offset; 1 = read), against the snapshot. Returns
// the mismatches; `*compared` the dwords compared; `*read_fail` 1 if the window refused a read (the caller fails the copy).
// build 0.0.514 A1: THE SAMPLED BRANCH READS THE WHOLE PLAN IN ONE CALL. `plan` reads the `ns` dwords at
// d_at + n48_fc_sample_at(n, rot, i), i < ns, into got[0..ns) (the kext: fc_vram_read_sampled_timed - ONE gVramMmLock acquire per
// chunk, where 0.0.513 took it once per sampled dword and waited ~198 us each time in fc3). `got` holds N48_FC_SAMPLE_MAX dwords.
// The offsets and the compare are 0.0.513's; a refused plan read sets *read_fail and gives NO compare credit (0.0.513 credited
// the dwords read before the refusal - the copy failed either way). Full mode is unchanged: 32 dwords per `read` call.
typedef int (*n48_fc_plan_read_fn)(void *ctx, uint64_t d_at, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns);
static inline uint64_t n48_fc_verify(int (*read)(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords), n48_fc_plan_read_fn plan,
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

// ---- the result the kext's per-chunk call hands back to the copy loop (one word: the inlined copy keeps no new local) ----------
// bit 63: SDMA took this chunk (else the MM loop runs it, nothing was staged or written); bit 62: it FAILED (the low byte is
// N48_FC_RUN_*, or N48_FC_RUN_VERIFY_READ); else bits [61:32] the dwords compared, [31:0] the mismatches.
#define N48_FC_R_TAKEN (1ull << 63)
#define N48_FC_R_FAIL  (1ull << 62)
#define N48_FC_RUN_VERIFY_READ 7u
static inline uint64_t n48_fc_result_ok(uint64_t compared, uint64_t mismatched)
{
    return N48_FC_R_TAKEN | ((compared & 0x3FFFFFFFull) << 32) | (mismatched > 0xFFFFFFFFull ? 0xFFFFFFFFull : mismatched);
}
static inline uint64_t n48_fc_result_fail(uint32_t run) { return N48_FC_R_TAKEN | N48_FC_R_FAIL | (uint64_t)(run & 0xffu); }
static inline const char *n48_fc_fail_text(uint64_t r)
{
    switch ((uint32_t)(r & 0xffu)) {
    case N48_FC_RUN_FILL:        return "the SDMA path's batch producer failed (readBytes short, or the chunk could not be converted)";
    case N48_FC_RUN_REFUSED:     return "the SDMA submission was refused before the ring was written (VRAM guard or queue busy)";
    case N48_FC_RUN_TIMEOUT:     return "the SDMA fence did not land in the bound - range poisoned, SDMA path latched OFF";
    case N48_FC_RUN_RING:        return "the SDMA ring write or doorbell failed - range poisoned, SDMA path latched OFF";
    case N48_FC_RUN_FENCES:      return "the SDMA fence values are exhausted - SDMA path latched OFF";
    case N48_FC_RUN_VERIFY_READ: return "the MM window refused the SDMA copy's verify read";
    default:                     return "the SDMA path failed";
    }
}

// ---- the report's counters ------------------------------------------------------------------------------------------------------
typedef struct {
    uint64_t chunks_sdma, copies_sdma, bytes_sdma;
    uint64_t stage_us, dma_us, verify_us;
    uint64_t sampled, full_dwords, mismatched;
    uint64_t fallback[N48_FC_MM_REASONS];
    uint64_t fill_fail, refused, ring_fail, verify_read_fail;
    // build 0.0.509 (appended): item 1 - the gScanoutLock wait per submission (total, max, count); item 2 - chunks whose
    // range is poisoned for the boot; item 3 - copies upgraded to full verify; item 4 - the verify's wait for gVramMmLock (the
    // MM-window lock, the MM-priority yield included) apart from its reads, and the boot's slowest verify per dword.
    uint64_t lock_wait_us, lock_wait_max_us, lock_waits;
    uint64_t dead_ranges, forced_full;
    uint64_t vlock_us, vread_us;
    uint64_t vmax_ns_dw, vmax_bytes, vmax_dwords, vmax_us, vmax_lock_us, vmax_read_us;
    // build 0.0.510 A1 (appended): copies upgraded to full verify because their range overlapped a live copy-guard poison row;
    // first chunks REFUSED before the ring was written and handed to the MM loop instead of failing the copy.
    uint64_t forced_full_cg, refused_mm;
    // build 0.0.521 Part D (appended): copies upgraded to full verify as resprov candidates; every copy's latched mode.
    uint64_t forced_full_rp, latched_sampled, latched_full;
} n48_fc_stats;

// Item 4: the boot's slowest verify per dword (ns), kept with its copy's size and its lock/read split. Pure; returns 1 when
// this copy is the new maximum.
static inline uint32_t n48_fc_vmax_note(n48_fc_stats *s, uint64_t verify_us, uint64_t lock_us, uint64_t read_us, uint64_t dwords,
                                        uint64_t bytes)
{
    if (!s || !dwords) return 0u;
    const uint64_t ns = verify_us * 1000u / dwords;
    if (ns <= s->vmax_ns_dw) return 0u;
    s->vmax_ns_dw = ns; s->vmax_bytes = bytes; s->vmax_dwords = dwords; s->vmax_us = verify_us;
    s->vmax_lock_us = lock_us; s->vmax_read_us = read_us;
    return 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* N48_FASTCOPY_H */
