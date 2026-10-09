//
//  navi48_dmubcmd_flow.h - build 0.0.625 (stages M2 / M3): the SEQUENCES of the three command verbs, written against an environment E so the host test runs the very code the kext runs over a fake DMUB (a register
//  file with a firmware model, a VRAM window, an allowlist). C++17, no kernel header. The decisions and constants are navi48_dmubcmd.h's.
//
//  Environment contract (the kext's glue is at the end of dcn/navi48_dcn.cpp; the host test's is in tests/native_dmubcmd_test.cpp):
//      bool     E::probe(Probe *p)                       READ the DMCUB registers and fill *p (false = no device); never writes
//      bool     E::rd(uint64_t vramOff, uint32_t *dst, uint32_t n)         the kext's navi48_vram_read_mm
//      bool     E::wr(uint64_t vramOff, const uint32_t *src, uint32_t n)   the kext's navi48_vram_write_mm (the existing locked MM_INDEX writer; the ONLY VRAM write of these verbs)
//      uint32_t E::rptr() / E::wptr()                    a live register READ each
//      bool     E::wrWptr(uint32_t v)                    THE one register write: DMCUB_INBOX1_WPTR, through the DCN write allowlist; false = refused (nothing reached the card)
//      uint32_t E::frames()                              OTG0_OTG_STATUS_FRAME_COUNT
//      void     E::delay_us(uint32_t) ; uint64_t E::now_us()
//
//  The order of dmubsend (every step is a refusal that leaves the ring untouched until step 8; 0.0.626: step 3 also requires the live ring size == 0x2000, and `replay` copies an existing slot, steps 3b / 4):
//    1 allowlist (header, d1, d2)          2 probe + REGION4 base (as region4read computes it)        3 DMCUB state, ring placement, ring sanity, idle, wrap
//    4 write the 64-byte slot at base + WPTR        5 read it back and COMPARE (a mismatch moves nothing)        6 re-read WPTR / RPTR live (the ring must still be idle)
//    7 OTG0 frame counter, clock        8 write WPTR (+0x40)        9 poll RPTR <= 10000 x 10 us for the new WPTR, no retry        10 VBIOS ring variables (+0x3114 / +0x3118 / +0x311c), read back.
//
#ifndef N48_DMUBCMD_FLOW_H
#define N48_DMUBCMD_FLOW_H

#include <stdint.h>
#include "navi48_dmubcmd.h"

namespace n48dm {

struct Probe {
    n48dm_pre pre {};                 // enabled, soft_reset, ring_region4, ring_at_base, size, wptr, rptr
    uint32_t r4Enabled = 0, r4Lo = 0, r4Hi = 0;
    uint64_t fbBaseMc = 0, vramSize = 0;
};

// The REGION4 window base, exactly as region4read computes it (n48dr_region4_base over the same register values).
inline uint32_t window_base(const Probe &p, uint64_t *base)
{
    return n48dr_region4_base(p.r4Lo, p.r4Hi, p.r4Enabled, p.fbBaseMc, p.vramSize, base);
}

// replaySrc == N48DM_NO_REPLAY: the ordinary send (hdr, d1, d2 are the command). Otherwise (0.0.626, F8) a REPLAY: replaySrc is the ring offset of an existing slot (64-byte aligned, below the live WPTR); the slot's
// 64 bytes are copied byte for byte into the next slot, after its header and (d1, d2) pass the same allowlist; hdr / d1 / d2 are ignored and set from the source.
template <class E>
inline n48dm_send_out send(E &e, uint32_t hdr, uint32_t d1, uint32_t d2, uint32_t replaySrc = N48DM_NO_REPLAY, uint32_t tplId = N48DM_TPL_NONE)
{
    const bool replay = replaySrc != N48DM_NO_REPLAY;
    const bool tpl = tplId != N48DM_TPL_NONE;      // 0.0.630 (M4c): a mainline VBIOS template (sub 1 / sub 2), named by ID; hdr / d1 / d2 are ignored and set from the template
    const struct n48dm_tpl *tp = n48dm_tpl_get(tplId);
    n48dm_send_out o;
    unsigned i;
    for (i = 0; i < sizeof(o); i++) ((uint8_t *)&o)[i] = 0;
    o.status = N48DR_BAD_ARG; o.hdr = hdr; o.d1 = d1; o.d2 = d2; o.base = ~0ull;

    // ---- 1. the allowlist: nothing is read, nothing is written (a replay: the source offset must be 64-byte aligned and inside the ring; its slot is judged in step 3b, once the live WPTR is known)
    uint32_t st = 0u;
    if (tpl) {
        if (replay || !tp) st = N48DR_PAYLOAD_REFUSED;
        else { hdr = tp->dw[0]; d1 = tp->dw[1]; d2 = tp->dw[2]; o.hdr = hdr; o.d1 = d1; o.d2 = d2; }
    }
    else if (!replay) st = n48dm_slot_check(hdr, d1, d2);
    else if ((replaySrc % N48DM_SLOT_BYTES) != 0u || replaySrc >= N48DM_RING_SIZE) st = N48DR_REPLAY_REFUSED;
    if (st != 0u) { o.status = st; return o; }

    // ---- 2. the live state (reads only) and the window base
    Probe p;
    if (!e.probe(&p)) { o.status = N48DR_NO_DEVICE; return o; }
    uint64_t base = ~0ull;
    st = window_base(p, &base);
    o.base = base;
    if (st != N48DR_OK) { o.status = st; return o; }

    // ---- 3. the pre-checks
    st = n48dm_precheck(&p.pre);
    if (st != 0u) { o.status = st; return o; }
    const uint32_t wptr0 = p.pre.wptr, rptr0 = p.pre.rptr, next = wptr0 + N48DM_SLOT_BYTES;
    o.old_wptr = wptr0; o.new_wptr = next; o.rptr_start = rptr0; o.rptr_end = rptr0;

    // ---- 3b. a replay: the source slot must lie wholly below the live WPTR; read it and judge its header and (d1, d2) with the SAME allowlist, before anything is written
    uint32_t slot[N48DM_SLOT_DWORDS], back[N48DM_SLOT_DWORDS];
    if (replay) {
        if (replaySrc + N48DM_SLOT_BYTES > wptr0) { o.status = N48DR_REPLAY_REFUSED; return o; }
        if (!e.rd(base + replaySrc, slot, N48DM_SLOT_DWORDS)) { o.status = N48DR_VRAM_READ_FAILED; return o; }
        hdr = slot[0]; d1 = slot[1]; d2 = slot[2]; o.hdr = hdr; o.d1 = d1; o.d2 = d2;
        st = n48dm_slot_check(slot[0], slot[1], slot[2]);
        if (st != 0u) { o.status = st; return o; }
    }

    // ---- 3c. a template: the whole 16-dword slot must be EXACTLY one of the seven fixed templates (checked on the slot that is about to be written, before anything is)
    if (tpl) {
        n48dm_tpl_slot(slot, tp);
        st = n48dm_tpl_slot_check(slot);
        if (st != 0u) { o.status = st; return o; }
    }

    // ---- 4. the slot, 5. read it back and compare BEFORE the WPTR moves
    if (!replay && !tpl) n48dm_build_slot(slot, hdr, d1, d2);
    if (!e.wr(base + wptr0, slot, N48DM_SLOT_DWORDS)) { o.status = N48DR_SLOT_WRITE_FAILED; return o; }
    if (!e.rd(base + wptr0, back, N48DM_SLOT_DWORDS)) { o.status = N48DR_VRAM_READ_FAILED; return o; }
    for (i = 0; i < N48DM_SLOT_DWORDS; i++)
        if (back[i] != slot[i]) { o.status = N48DR_READBACK_MISMATCH; return o; }

    // ---- 6. the ring must still be idle at the same place (a re-read of the two registers)
    if (e.wptr() != wptr0) { o.status = N48DR_RING_BUSY; return o; }
    if (e.rptr() != wptr0) { o.status = N48DR_RING_BUSY; return o; }

    // ---- 7. the witness, 8. the one register write
    o.frames0 = e.frames();
    const uint64_t t0 = e.now_us();
    if (!e.wrWptr(next)) { o.status = N48DR_WPTR_REFUSED; o.frames1 = e.frames(); return o; }
    o.sent = 1u;

    // ---- 9. the bounded poll: n48dm_poll_max_for(header) reads (0.0.630: 100 ms, 2 s for sub 1 / sub 2), N48DM_POLL_STEP_US apart; never retried
    bool done = false;
    uint32_t r = rptr0;
    const uint32_t pollMax = n48dm_poll_max_for(slot[0]);       // 0.0.630: 2 s for the two mainline VBIOS sub-types, 100 ms for everything else
    for (uint32_t n = 0; n < pollMax; n++) {
        o.polls++;
        r = e.rptr();
        if (r == next) { done = true; break; }
        e.delay_us(N48DM_POLL_STEP_US);
    }
    o.rptr_end = r;
    o.frames1 = e.frames();
    o.elapsed_us = (uint32_t)(e.now_us() - t0);
    if (!done) { o.status = N48DR_POLL_TIMEOUT; return o; }       // everything stays as it is: no further write

    // ---- 10. the VBIOS ring variables (the plan keeps them consistent): +0x3114 = 0x40, +0x3118 = the old RPTR, +0x311c = the new WPTR. +0x3100 is left alone.
    const uint32_t vars[3] = { N48DM_VAR_CMDSIZE_VALUE, rptr0, next };       // dwords at +0x3114, +0x3118, +0x311c: contiguous by the layout (static_asserted in the test)
    uint32_t vback[3] = { 0, 0, 0 };
    if (!e.wr(base + N48DM_VAR_LASTSIZE, vars, 3u) || !e.rd(base + N48DM_VAR_LASTSIZE, vback, 3u) || vback[0] != vars[0] || vback[1] != vars[1] || vback[2] != vars[2]) {
        o.status = N48DR_VARS_FAILED;
        return o;
    }
    o.vars = 1u;
    o.status = N48DR_OK;
    return o;
}

struct ModeResult {
    uint32_t status = N48DR_BAD_ARG, saved = 0, dirty = 0;
    uint64_t base = ~0ull;
    uint32_t block[N48DM_MODE_DWORDS] = {};     // the block in the window after the op (read back), or the saved copy for `save`
    uint32_t before[N48DM_MODE_DWORDS] = {};    // the block read before a write
};

// dmubmode: save | restore | 0x1a6 | 0x1d4. s is the kext-held saved copy (this boot).
template <class E>
inline ModeResult mode(E &e, uint64_t op, n48dm_mode_state &s)
{
    ModeResult r;
    uint32_t st = n48dm_mode_gate(op, &s);
    r.saved = s.saved; r.dirty = s.dirty;
    if (st != 0u) { r.status = st; return r; }
    Probe p;
    if (!e.probe(&p)) { r.status = N48DR_NO_DEVICE; return r; }
    uint64_t base = ~0ull;
    st = window_base(p, &base);
    r.base = base;
    if (st != N48DR_OK) { r.status = st; return r; }
    if (!p.pre.enabled || p.pre.soft_reset) { r.status = N48DR_DMUB_NOT_READY; return r; }
    if (op != N48DM_MODE_SAVE) { st = n48dm_idle_gate(&p.pre); if (st != 0u) { r.status = st; return r; } }     // 0.0.626 (F6): every WRITE op needs an idle ring (save only reads)
    if (!e.rd(base + N48DM_MODE_BLOCK, r.before, N48DM_MODE_DWORDS)) { r.status = N48DR_VRAM_READ_FAILED; return r; }
    if (op == N48DM_MODE_SAVE) {
        for (unsigned i = 0; i < N48DM_MODE_DWORDS; i++) { s.buf[i] = r.before[i]; r.block[i] = r.before[i]; }
        s.saved = 1u; s.dirty = 0u;
        r.saved = 1u; r.dirty = 0u; r.status = N48DR_OK;
        return r;
    }
    uint32_t words[N48DM_MODE_DWORDS];
    n48dm_mode_words(op, &s, words);
    if (op != N48DM_MODE_RESTORE) { s.dirty = 1u; r.dirty = 1u; }       // a VBE write: the original is no longer in the window, whatever the write does
    if (!e.wr(base + N48DM_MODE_BLOCK, words, N48DM_MODE_DWORDS)) { r.status = N48DR_SLOT_WRITE_FAILED; return r; }
    if (!e.rd(base + N48DM_MODE_BLOCK, r.block, N48DM_MODE_DWORDS)) { r.status = N48DR_VRAM_READ_FAILED; return r; }
    for (unsigned i = 0; i < N48DM_MODE_DWORDS; i++)
        if (r.block[i] != words[i]) { r.status = N48DR_READBACK_MISMATCH; return r; }
    if (op == N48DM_MODE_RESTORE) { s.dirty = 0u; r.dirty = 0u; }       // clean ONLY after a restore that read back
    r.status = N48DR_OK;
    return r;
}

struct CtxResult {
    uint32_t status = N48DR_BAD_ARG, before = 0, after = 0, ctx = 0, value = 0;
    uint64_t base = ~0ull;
};

// dmubctx: one dword at base + ctx + 0x1a0 (ctx 0x7000 / 0x7800 only), read back.
template <class E>
inline CtxResult ctx(E &e, uint64_t arg)
{
    CtxResult r;
    r.ctx = (uint32_t)(arg & 0xFFFFu); r.value = (uint32_t)(arg >> 32);
    uint32_t st = n48dm_ctx_gate(arg);
    if (st != 0u) { r.status = st; return r; }
    Probe p;
    if (!e.probe(&p)) { r.status = N48DR_NO_DEVICE; return r; }
    uint64_t base = ~0ull;
    st = window_base(p, &base);
    r.base = base;
    if (st != N48DR_OK) { r.status = st; return r; }
    if (!p.pre.enabled || p.pre.soft_reset) { r.status = N48DR_DMUB_NOT_READY; return r; }
    st = n48dm_idle_gate(&p.pre);       // 0.0.626 (F6): the marker write needs an idle ring
    if (st != 0u) { r.status = st; return r; }
    const uint64_t at = base + r.ctx + N48DM_CTX_STATUS;
    if (!e.rd(at, &r.before, 1u)) { r.status = N48DR_VRAM_READ_FAILED; return r; }
    if (!e.wr(at, &r.value, 1u)) { r.status = N48DR_SLOT_WRITE_FAILED; return r; }
    if (!e.rd(at, &r.after, 1u)) { r.status = N48DR_VRAM_READ_FAILED; return r; }
    r.status = r.after == r.value ? (uint32_t)N48DR_OK : (uint32_t)N48DR_READBACK_MISMATCH;
    return r;
}

}  // namespace n48dm

#endif /* N48_DMUBCMD_FLOW_H */
