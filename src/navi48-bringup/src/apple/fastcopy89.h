// fastcopy89.h — build 0.0.534 ( ranked item 1): SWITCH 89, THE FULL VERIFY OF A FAST-COPY CHUNK BY AN SDMA
// READ-BACK INSTEAD OF THE MM WINDOW.
//
// WHY. 0.0.521 Part D latches FULL verify for resprov candidates (and 62-overlapping / copy-guard-poisoned copies were
// already FULL), because resprov records a copy only when `compared * 4 >= bytes` (ws_resprov.h n48_rp_record) and only a copy read
// back in full may clear the copy guard's poison or complete 62 clean (fastcopy.h n48_fc_cg_close_wrote / n48_fc_hg_clean). The full
// verify reads every dword through the MM window at ~1.8 us/dword: the 1920x1080 login wallpaper (0x7e9000 bytes) took 4.05 s
//, stalled WindowServer ~4.2 s at every start-up and, through the judge it starved, voided RUN AF.
//
// WHAT CHANGES (switch 89 ON, a chunk latched FULL, the write's fence LANDED): under gFastCopyLock, the read-back buffer (a second
// 1 MiB GART-bound system-memory buffer, bound once on the verb thread) is POISONED with the bitwise complement of the staged stream
// (every dword the engine does not overwrite then mismatches), our SDMA0 QUEUE0 copies the SAME VRAM range [d_at, d_at + n) back into
// it (COPY_LINEAR + FENCE, fastcopy.h's packet with source and destination swapped), the fence is waited for (fastcopy.h's bounded
// n48_fc_submit_wait), and the read-back is compared with the snapshot of the staged stream (`exp`, taken before the write) dword by
// dword. gFastCopyLock is released, then an INDEPENDENT sampled cross-check reads fastcopy.h's sample plan (the chunk's first and last
// dword and one dword in every 4 KiB page) through the MM window and compares it with the same snapshot, so an error that both DMA
// directions share (a wrong MC base: the write and the read-back land at the same wrong place and agree) is still caught at the
// correct VRAM offset. On success the chunk credits n / 4 dwords compared (resprov's rung holds exactly as under the MM full verify).
//   read-back refused before its ring write (the queue busy, the DCC compression bits set), the fence values exhausted, or a bad
//   shape: NOT TAKEN - the caller runs today's full MM verify for the chunk (the write landed; nothing of the read-back reached VRAM).
//   read-back fence TIMEOUT or a failed ring write: the chunk FAILS exactly as a write timeout does (fastcopy.h n48_fc_fence_outcome:
//   the path latched OFF and the staging RETIRED; here also the read-back buffer RETIRED: the engine may still write it), no credit.
//   any mismatch (read-back or cross-check): VERIFY MISMATCH (the caller latches the path OFF; the copy is poisoned; resprov refuses a
//   copy with `mismatched`), with the full n / 4 credited (every dword WAS compared).
//   the cross-check's MM read refused: the chunk FAILS as today's verify-read failure (N48_FC_RUN_VERIFY_READ), no credit.
//
// `accel gfxneuter 89 | M << 8`: M 1 ON (= 345), M 2 OFF (= 601, the default and the boot value), bare `89` reads. OFF = 0.0.533.
// Pure C that compiles as C++; no heap, no libc beyond memcpy. The kext half is Navi48Bringup.cpp fc89_chunk / navi48_fc89_set.
#ifndef N48_FASTCOPY89_H
#define N48_FASTCOPY89_H

#include <stdint.h>
#include <string.h>
#include "fastcopy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N48_FC89_SWITCH 89u
#define N48_FC89_M_ON   1u      // 89 | 1 << 8 = 345
#define N48_FC89_M_OFF  2u      // 89 | 2 << 8 = 601 (default, boot value)
#define N48_FC89_M_SHADOW 3u    // 89 | 3 << 8 = 857: SHADOW - the read-back runs and is COMPARED with today's full MM verify; every
                                // decision (credit, resprov, poison) is the MM verify's alone (0.0.534 review item 3)
// The kext's switch value: 0 OFF, N48_FC89_M_ON or N48_FC89_M_SHADOW.
static inline uint32_t n48_fc89_mode_of(uint32_t m) { return (m == N48_FC89_M_ON || m == N48_FC89_M_SHADOW) ? m : 0u; }
// The extra bytes a FULL chunk's `exp` allocation carries for the mode, ALLOCATED BEFORE gFastCopyLock (0.0.534 review item 2):
// ON - the cross-check's `got` (N48_FC_SAMPLE_MAX dwords) after the n / 4 snapshot dwords; SHADOW - a copy of the read-back (n bytes).
static inline uint64_t n48_fc89_extra(uint32_t m89, uint64_t n)
{
    return m89 == N48_FC89_M_ON ? (uint64_t)N48_FC_SAMPLE_MAX * 4u : m89 == N48_FC89_M_SHADOW ? n : 0u;
}

// The read-back side's state (one per boot; under gFastCopyLock in the kext).
typedef struct {
    uint32_t rb_ok;        // the read-back buffer is bound (GART) and its positive control passed
    uint32_t rb_retired;   // never cleared: a read-back fence that did not land (the engine may still write the buffer)
    uint32_t pc_done, pc_wrong;
    uint32_t pc_nonce;     // the last control's nonce (0.0.534 review M1): the next control's pattern differs in every dword
} n48_fc89_state;

// ---- THE CONTROL's PATTERN (0.0.534 review M1: freshness). 63's control SDMA-writes fc_pc_word(i) = 0xA5000000 | i into a scratch
// block the first-fit allocator likely hands back to this control, so a read that returned STALE VRAM would pass a control that wrote
// the same pattern. This one has top byte 0x3C (never 0xA5: every dword differs from 63's) and its low 24 bits are i ^ nonce, with a
// nonce whose low 24 bits differ from the previous control's (every dword differs from the previous control's too).
static inline uint32_t n48_fc89_pc_word(uint32_t i, uint32_t nonce) { return 0x3C000000u | ((i ^ nonce) & 0x00FFFFFFu); }
static inline uint32_t n48_fc89_pc_nonce(uint32_t fence, uint32_t last)
{
    uint32_t n = (fence * 0x9E3779B1u) & 0x00FFFFFFu;
    if (n == (last & 0x00FFFFFFu)) n ^= 1u;
    return n;
}
// The control's compare: every dword of the read-back against THIS control's pattern. Returns the wrong count; the first two wrong
// indices and values in `fi`/`fv` (optional, 2 entries).
static inline uint32_t n48_fc89_pc_check(const uint32_t *rb, uint32_t nd, uint32_t nonce, uint32_t *fi, uint32_t *fv)
{
    uint32_t wrong = 0u;
    for (uint32_t i = 0; i < nd; i++) {
        if (rb[i] == n48_fc89_pc_word(i, nonce)) continue;
        if (wrong < 2u && fi && fv) { fi[wrong] = i; fv[wrong] = rb[i]; }
        wrong++;
    }
    return wrong;
}

// The chunk takes the read-back: 89 ON, the chunk latched FULL, the buffer usable, the path not latched off.
static inline uint32_t n48_fc89_use(uint32_t on, uint32_t mode, const n48_fc89_state *q, uint32_t latched_off)
{
    return (on && mode == N48_FC_M_FULL && q && q->rb_ok && !q->rb_retired && !latched_off) ? 1u : 0u;
}

// Every dword of the read-back buffer set to the complement of what the engine must write there: a dword the read-back does not
// overwrite (a short copy, a copy that never ran, an engine that wrote elsewhere) can never compare equal.
static inline void n48_fc89_poison(uint32_t *rb, const uint32_t *exp, uint32_t nd)
{
    for (uint32_t i = 0; i < nd; i++) rb[i] = ~exp[i];
}
static inline uint64_t n48_fc89_compare(const uint32_t *rb, const uint32_t *exp, uint32_t nd)
{
    uint64_t bad = 0u;
    for (uint32_t i = 0; i < nd; i++) if (rb[i] != exp[i]) bad++;
    return bad;
}

// The independent cross-check: fastcopy.h's sample plan read through the MM window (`plan`: the kext's fc_mm_read_plan, ONE
// gVramMmLock acquire) at the chunk's CORRECT VRAM offset, compared with the FULL snapshot at the same offsets. `got` holds
// N48_FC_SAMPLE_MAX dwords. *read_fail 1 = the window refused (no compare). Returns the mismatches.
static inline uint64_t n48_fc89_xcheck(n48_fc_plan_read_fn plan, void *ctx, uint64_t d_at, const uint32_t *exp, uint32_t *got,
                                       uint32_t n, uint32_t rot, uint32_t *read_fail, uint32_t *ns_out)
{
    uint64_t bad = 0u;
    const uint32_t ns = n48_fc_sample_count(n);
    if (ns_out) *ns_out = ns;
    if (read_fail) *read_fail = 0u;
    if (!plan || !exp || !got || !ns || ns > N48_FC_SAMPLE_MAX || !plan(ctx, d_at, n, rot, got, ns)) {
        if (read_fail) *read_fail = 1u;
        return 0u;
    }
    for (uint32_t i = 0; i < ns; i++) if (got[i] != exp[n48_fc_sample_at(n, rot, i) / 4u]) bad++;
    return bad;
}

// What the read-back run answers (internal; the result word is what the caller returns).
enum { N48_FC89_OK = 0, N48_FC89_NOT_TAKEN = 1, N48_FC89_TIMEOUT = 2, N48_FC89_RING = 3 };

// THE RESULT WORD (fastcopy.h's encoding). NOT TAKEN = 0 (the caller runs today's full MM verify). A timeout or a failed ring write
// FAILS the chunk with fastcopy.h's own run code (no compare credit); a refused cross-check read fails it as a verify-read failure (no
// credit); otherwise n / 4 dwords compared and every mismatch of both compares.
static inline uint64_t n48_fc89_result(uint32_t run, uint64_t rb_bad, uint32_t xread_fail, uint64_t x_bad, uint32_t n)
{
    if (run == N48_FC89_NOT_TAKEN) return 0ull;
    if (run == N48_FC89_TIMEOUT) return n48_fc_result_fail(N48_FC_RUN_TIMEOUT);
    if (run != N48_FC89_OK) return n48_fc_result_fail(N48_FC_RUN_RING);
    if (xread_fail) return n48_fc_result_fail(N48_FC_RUN_VERIFY_READ);
    return n48_fc_result_ok((uint64_t)(n / 4u), rb_bad + x_bad);
}

// THE ORDER, driven through callbacks so the host test runs exactly this function against a mock engine and a mock VRAM.
typedef struct {
    void *ctx;
    // the read-back submission (VRAM `src_mc` -> the read-back buffer `dst_mc`): ring write, doorbell, bounded fence wait (N48_FC_SUB_*)
    uint32_t (*submit)(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t fence_value);
    // gFastCopyLock released (the read-back buffer is then another chunk's): after the compare, before the cross-check
    void (*unlock)(void *ctx);
    // the MM window's sampled read (fastcopy.h's plan read)
    n48_fc_plan_read_fn plan;
} n48_fc89_ops;
typedef struct { uint64_t rb_bad, x_bad; uint32_t run, sub, xread_fail, ns, fence; } n48_fc89_out;

// Called with gFastCopyLock HELD, after the chunk's WRITE fence landed (fastcopy.h n48_fc_chunk_run answered N48_FC_RUN_OK) and with
// `exp` = the full snapshot of the staged stream (n / 4 dwords). Returns 0 (NOT TAKEN) with the lock STILL HELD - nothing was
// submitted, or the submission was refused before its ring write; the caller then runs today's verify - or the chunk's result word
// with the lock RELEASED (o->unlock called exactly once).
//   1. the read-back buffer poisoned with ~exp;  2. a fence value never used before;  3. the read-back submitted and its fence waited
//   for;  4. ONLY THEN the compare of the read-back with exp;  5. the lock released;  6. the MM cross-check at the correct offset.
static inline uint64_t n48_fc89_chunk(const n48_fc89_ops *o, n48_fc_state *s, n48_fc89_state *q, uint32_t *rb, uint64_t rb_mc,
                                      uint64_t src_mc, uint64_t d_at, const uint32_t *exp, uint32_t *got, uint32_t n, uint32_t rot,
                                      n48_fc89_out *out)
{
    n48_fc89_out tmp;
    if (!out) out = &tmp;
    out->rb_bad = 0u; out->x_bad = 0u; out->run = N48_FC89_NOT_TAKEN; out->sub = 0u; out->xread_fail = 0u; out->ns = 0u; out->fence = 0u;
    if (!o || !o->submit || !o->unlock || !o->plan || !s || !q || !rb || !exp || !got || !rb_mc || n == 0u || (n & 3u) ||
        n > N48_FC_STAGING_BYTES)
        return 0ull;
    const uint32_t nd = n / 4u;
    n48_fc89_poison(rb, exp, nd);                                        // 1
    const uint32_t fence = n48_fc_fence_next(s);                         // 2 (0 = exhausted: the path latched; not taken)
    if (!fence) return 0ull;
    out->fence = fence;
    const uint32_t sub = o->submit(o->ctx, src_mc, rb_mc, n, fence);     // 3
    out->sub = sub;
    if (sub == N48_FC_SUB_REFUSED) return 0ull;                          // nothing written to the ring: today's verify runs
    if (sub != N48_FC_SUB_LANDED) {
        (void)n48_fc_fence_outcome(s, sub);                              // the write path's own timeout / ring handling
        q->rb_retired = 1u;                                              // ... and this buffer is never handed out again
        out->run = sub == N48_FC_SUB_RING ? N48_FC89_RING : N48_FC89_TIMEOUT;
        o->unlock(o->ctx);
        return n48_fc89_result(out->run, 0u, 0u, 0u, n);
    }
    out->run = N48_FC89_OK;
    out->rb_bad = n48_fc89_compare(rb, exp, nd);                         // 4
    o->unlock(o->ctx);                                                   // 5
    out->x_bad = n48_fc89_xcheck(o->plan, o->ctx, d_at, exp, got, n, rot, &out->xread_fail, &out->ns);   // 6
    return n48_fc89_result(out->run, out->rb_bad, out->xread_fail, out->x_bad, n);
}

// ---- SHADOW (0.0.534 review item 3): the read-back AND today's full MM verify for the same chunk; the MM verify decides everything.
// Phase A, under gFastCopyLock (called exactly where n48_fc89_chunk is): the buffer poisoned, the read-back submitted and waited for,
// copied out to `sd` (n / 4 dwords, allocated before the lock) and compared with exp; the lock released. Returns 0 (NOT TAKEN, the lock
// STILL HELD: refused before its ring write / fences exhausted / a bad shape - today's path runs) or 1 (the lock RELEASED; the caller
// runs today's MM verify through n48_fc89_shadow_read and decides the chunk by n48_fc89_shadow_decide). A read-back timeout or ring
// failure is handled exactly as in ON (the path latched OFF and the staging retired by n48_fc_fence_outcome, the buffer retired) - the
// ONE departure from 0.0.533: our own extra submission failed, so the queue is not trusted again - but THIS chunk is still decided by
// the MM verify (`out->run` says the read-back gave no view: `sd_ok` 0, nothing compared).
static inline uint32_t n48_fc89_shadow_a(const n48_fc89_ops *o, n48_fc_state *s, n48_fc89_state *q, uint32_t *rb, uint64_t rb_mc,
                                         uint64_t src_mc, const uint32_t *exp, uint32_t *sd, uint32_t n, n48_fc89_out *out,
                                         uint32_t *sd_ok)
{
    out->rb_bad = 0u; out->x_bad = 0u; out->run = N48_FC89_NOT_TAKEN; out->sub = 0u; out->xread_fail = 0u; out->ns = 0u; out->fence = 0u;
    *sd_ok = 0u;
    if (!o || !o->submit || !o->unlock || !s || !q || !rb || !exp || !sd || !rb_mc || n == 0u || (n & 3u) || n > N48_FC_STAGING_BYTES)
        return 0u;
    const uint32_t nd = n / 4u;
    n48_fc89_poison(rb, exp, nd);
    const uint32_t fence = n48_fc_fence_next(s);
    if (!fence) return 0u;
    out->fence = fence;
    const uint32_t sub = o->submit(o->ctx, src_mc, rb_mc, n, fence);
    out->sub = sub;
    if (sub == N48_FC_SUB_REFUSED) return 0u;
    if (sub != N48_FC_SUB_LANDED) {
        (void)n48_fc_fence_outcome(s, sub);
        q->rb_retired = 1u;
        out->run = sub == N48_FC_SUB_RING ? N48_FC89_RING : N48_FC89_TIMEOUT;
        o->unlock(o->ctx);
        return 1u;
    }
    out->run = N48_FC89_OK;
    memcpy(sd, rb, (size_t)n);                 // the SDMA view, out of the shared buffer before the lock is released
    out->rb_bad = n48_fc89_compare(sd, exp, nd);
    *sd_ok = 1u;
    o->unlock(o->ctx);
    return 1u;
}
// Today's full MM verify's read (the kext's fc_mm_read), wrapped: every dword it returns is compared with the SDMA view at the same
// offset and a disagreement recorded (the first 4 with both values). It returns exactly what the wrapped read returned and changes no
// dword it hands back, so n48_fc_verify's answer through it is the unwrapped answer (tested).
typedef struct {
    int (*read)(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords);
    void *ctx;
    const uint32_t *sd;       // the SDMA view (n / 4 dwords), valid when sd_ok
    uint32_t sd_ok, nd;
    uint64_t d_at;
    uint64_t dis, compared;   // dwords where the MM and SDMA views differ; dwords compared between the two views
    uint32_t nfirst, off[4], mm[4], sdv[4];
} n48_fc89_shadow_rec;
static inline int n48_fc89_shadow_read(void *c, uint64_t vram, uint32_t *dst, uint32_t dwords)
{
    n48_fc89_shadow_rec *r = (n48_fc89_shadow_rec *)c;
    const int ok = r->read(r->ctx, vram, dst, dwords);
    if (!ok || !r->sd_ok || vram < r->d_at) return ok;
    const uint64_t base = (vram - r->d_at) / 4u;
    for (uint32_t j = 0; j < dwords && base + j < r->nd; j++) {
        r->compared++;
        if (dst[j] == r->sd[base + j]) continue;
        if (r->nfirst < 4u) { r->off[r->nfirst] = (uint32_t)((base + j) * 4u); r->mm[r->nfirst] = dst[j]; r->sdv[r->nfirst] = r->sd[base + j]; r->nfirst++; }
        r->dis++;
    }
    return ok;
}
// THE DECISION under SHADOW: the MM verify's (today's navi48_fc_chunk tail: a refused read fails the chunk, else its compare count and
// mismatches). The read-back's answer is an argument ONLY so a test can prove it is ignored.
static inline uint64_t n48_fc89_shadow_decide(uint64_t mm_bad, uint64_t mm_cmp, uint32_t mm_rf, uint32_t rb_run, uint64_t rb_bad)
{
    (void)rb_run; (void)rb_bad;
    if (mm_rf) return n48_fc_result_fail(N48_FC_RUN_VERIFY_READ);
    return n48_fc_result_ok(mm_cmp, mm_bad);
}

// ---- the counters and the report line (<= 491 bytes: tested) --------------------------------------------------------------------
typedef struct {
    uint64_t chunks, bytes, mismatches, timeouts, xmismatches, xread_fail, not_taken, us, dma_us, xcheck_us, ring_fail;
    uint64_t sh_chunks, sh_agree, sh_disagree, sh_dis_dwords, sh_noview, sh_rb_bad, sh_mm_bad;   // SHADOW
    // build 0.0.535 item 3: the bounded queue-idle wait before a read-back kick (read-backs that found SDMA0
    // QUEUE0 busy and waited; of them, idle within the bound / still busy past it; the longest wait) and NOT TAKEN by reason.
    uint64_t qwaits, qwait_idle, qwait_busy, qwait_max_us, nt_qbusy, nt_other;
} n48_fc89_stats;

// ---- build 0.0.535 item 3 (: 25-31 of ~105 full chunks per run were NOT TAKEN; the only variable refusal is the
// queue check right after the forward write's fence - a lagging read-pointer write-back or another QUEUE0 user). THE BOUNDED WAIT:
// when the read-back's kick finds SDMA0 QUEUE0 not idle, it polls the queue check every N48_FC89_QWAIT_STEP_US for at most
// N48_FC89_QWAIT_US (2 ms) before refusing - at most bound / step + 1 polls, so it is bounded even by a clock that does not move -
// and only then is the chunk NOT TAKEN (today's full MM verify runs, as before). Pure; the kext's callbacks are the queue check,
// the uptime clock and IODelay. Returns 1 = idle (within the bound), 0 = still busy; *waited_us = the time spent polling.
#define N48_FC89_QWAIT_US      2000u
#define N48_FC89_QWAIT_STEP_US 10u
#define N48_FC89_NT_LINES      64u     // per-chunk NOT TAKEN lines per boot, then counted only
enum { N48_FC89_NT_OTHER = 0u, N48_FC89_NT_QBUSY = 1u };
typedef struct {
    void *ctx;
    uint32_t (*idle)(void *ctx);                 // 1 = QUEUE0's read pointer is at our write pointer
    uint64_t (*now_us)(void *ctx);
    void (*delay)(void *ctx, uint32_t us);
} n48_fc89_qwait_ops;
static inline uint32_t n48_fc89_qwait(const n48_fc89_qwait_ops *o, uint32_t bound_us, uint64_t *waited_us)
{
    const uint64_t t0 = o->now_us(o->ctx);
    const uint32_t polls = bound_us / N48_FC89_QWAIT_STEP_US + 1u;
    for (uint32_t p = 0; ; p++) {
        const uint32_t ok = o->idle(o->ctx);
        const uint64_t el = o->now_us(o->ctx) - t0;
        if (ok || el >= bound_us || p >= polls) { if (waited_us) *waited_us = el; return ok ? 1u : 0u; }
        o->delay(o->ctx, N48_FC89_QWAIT_STEP_US);
    }
}
// The accounting of one wait (under the caller's lock): the counters above.
static inline void n48_fc89_qwait_note(n48_fc89_stats *st, uint32_t ok, uint64_t waited_us)
{
    st->qwaits++;
    if (ok) st->qwait_idle++; else st->qwait_busy++;
    if (waited_us > st->qwait_max_us) st->qwait_max_us = waited_us;
}
// A per-chunk NOT TAKEN line may be printed: 1 while fewer than N48_FC89_NT_LINES were (and counts it), else 0.
static inline uint32_t n48_fc89_nt_line(uint32_t *lines) { if (!lines || *lines >= N48_FC89_NT_LINES) return 0u; (*lines)++; return 1u; }
static inline const char *n48_fc89_nt_name(uint32_t why)
{
    return why == N48_FC89_NT_QBUSY ? "SDMA0 QUEUE0 still busy after the bounded wait"
                                    : "other (read-back unusable, DCC COMP_EN set, fences exhausted, shape or the kick's other gate)";
}
// args: copy # (the COPIED # this copy will print if no other copy completes first), chunk index, VRAM, bytes, mode, reason, the
// chunk's wait (us), the boot's longest wait (us)
#define N48_FC89_NT_FMT \
    "fastcopy89: NOT TAKEN copy #%llu chunk %llu (VRAM %#llx, %#llx bytes, %s): %s; this chunk waited %llu us (boot max %llu us); " \
    "today's full MM verify runs"
// args: waits, idle within the bound, busy past it, the longest wait, not taken for a busy queue, not taken for another reason, lines
#define N48_FC89_QW_FMT \
    "fastcopy89: queue-idle wait (bound %u us): waited %llu (idle within %llu, busy past %llu), max %llu us; NOT TAKEN queue %llu, " \
    "other %llu; lines %u/%u"
// The SHADOW report line (args: chunks, agreeing, disagreeing chunks, disagreeing dwords, chunks with no SDMA view, SDMA-vs-stream
// mismatches, MM-vs-stream mismatches) and the per-chunk disagreement line (args: VRAM, bytes, disagreeing dwords, compared, then 4 x
// (offset, MM, SDMA)).
#define N48_FC89_SH_FMT \
    "fastcopy89: SHADOW (857): chunks %llu, SDMA and MM views AGREE %llu, DISAGREE %llu (%llu dwords), no SDMA view %llu; stream " \
    "mismatches SDMA %llu, MM %llu (the MM verify decided every chunk)"
#define N48_FC89_SH_DIS_FMT \
    "fastcopy89: SHADOW DISAGREE at VRAM %#llx (%#llx bytes): %llu of %llu dwords differ between the MM window and the SDMA read-back; " \
    "first +%#x MM %08x SDMA %08x, +%#x MM %08x SDMA %08x, +%#x MM %08x SDMA %08x, +%#x MM %08x SDMA %08x (decided by MM)"
#define N48_FC89_FMT \
    "fastcopy89: read-back verify %s%s; buf %s, control %s (%u wrong); chunks %llu, bytes %llu, mismatches %llu, timeouts %llu, " \
    "ring %llu, x-mismatches %llu (read fail %llu), not taken %llu; us %llu (dma %llu, x-check %llu)"
#define N48_FC89_ARGS(on, why, q, st) \
    (on) == N48_FC89_M_SHADOW ? "SHADOW (857)" : (on) ? "ON (345)" : "OFF (601, default)", (why), \
    (q)->rb_retired ? "RETIRED" : (q)->rb_ok ? "bound" : "not bound", \
    (q)->pc_done ? ((q)->rb_ok ? "PASSED" : "FAILED") : "not run", (q)->pc_wrong, \
    (unsigned long long)(st)->chunks, (unsigned long long)(st)->bytes, (unsigned long long)(st)->mismatches, \
    (unsigned long long)(st)->timeouts, (unsigned long long)(st)->ring_fail, (unsigned long long)(st)->xmismatches, \
    (unsigned long long)(st)->xread_fail, (unsigned long long)(st)->not_taken, (unsigned long long)(st)->us, \
    (unsigned long long)(st)->dma_us, (unsigned long long)(st)->xcheck_us

#ifdef __cplusplus
}
#endif

#endif /* N48_FASTCOPY89_H */
