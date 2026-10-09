//
//  navi48_dispread_flow.h - build 0.0.622 (multi-monitor track, stage M1): the SEQUENCES of the three read-only display verbs, written against an environment E so the host test can
//  run the very code the kext runs over a fake DC_I2C engine / fake DMUB ring. C++17, no kernel header. The decisions are in navi48_dispread.h.
//
//  Environment contract (the kext's glue is at the end of dcn/navi48_dcn.cpp; the host test's is in tests/native_dispread_test.cpp):
//      uint32_t E::rd(uint32_t baseIdx, uint32_t off)   a plain register read (BASE_IDX 1 or 2); 0xFFFFFFFF when unreadable
//      void     E::wr2(uint32_t off, uint32_t value)    a register write at BASE_IDX 2 (the kext's goes through the DCN write allowlist; a refusal makes E::refused() true, and nothing reaches the card)
//      bool     E::refused()                            sticky: some write of this run was refused by the allowlist
//      void     E::delay_us(uint32_t us)
//
//  ddc_read is a port of RDNA4FB's AMDRDNA4FB::readEDIDI2C (src/RDNA4FB/src/framebuffer.cpp, BSD-3-Clause; hardware-verified there on this card's connectors) with the brief's containment added:
//    * NOTHING is written until the arbitration verdict (one READ) says the engine is free: owned by hardware / DMUB, or already by software, is a refusal with no register written;
//    * lines 2 and 3 only, blocks 0..3, the bus bytes come from the plan (n48dr_plan_ddc) and nowhere else;
//    * the SW_STATUS poll is bounded to 20 ms per transaction (n48dr_poll_bound); NACK / timeout / abort / overflow end it with a distinct status;
//    * every path that took the engine releases it (soft reset, setup 0, DONE_USING); a request that was not granted is withdrawn with DONE_USING only (we never reset an engine we do not own);
//    * a refused write (allowlist) aborts BEFORE the GO, so a half-programmed engine never starts a transaction.
//
#ifndef N48_DISPREAD_FLOW_H
#define N48_DISPREAD_FLOW_H

#include <stdint.h>
#include <stddef.h>
#include "navi48_dispread.h"

namespace n48dr {

struct DdcResult {
    uint32_t status = N48DR_BAD_ARG;
    uint32_t arb = 0;            // the arbitration word last read while the transaction ran
    uint32_t arbAfter = 0;       // ... and after the release (owner field 0 = given back)
    uint32_t swStatus = 0;
    uint32_t polls = 0;          // SW_STATUS polls made
    bool released = false;       // the release sequence ran
    bool granted = false;        // software owned the engine at some point
    uint8_t data[N48DR_EDID_BLOCK] = {};
};

// 0.0.633: the engine sequence below is ONE function shared by ddc_read (EDID block, nread = 128) and scdc_read (SCDC status bytes, nread <= 16): the same arbitration verdict, the same releases, the same
// bounded poll, the same 17 register write sites. The plan (n48dr_plan_ddc / n48dr_plan_scdc) is the only source of what goes on the bus; nread is the length of the plan's last (read) transaction.
template <class E>
inline DdcResult i2c_xfer(E &e, uint32_t line, const n48dr_plan &p, unsigned nread)
{
    DdcResult r;
    const uint32_t rSetup = N48DR_I2C_DDC1_SETUP + 2u * line;
    const uint32_t rSpeed = N48DR_I2C_DDC1_SPEED + 2u * line;

    // ---- 0. the entry verdict: ONE read. Hardware / DMUB owner, software owner, or no response = REFUSED with no register written.
    uint32_t arb = e.rd(2u, N48DR_I2C_ARBITRATION);
    r.arb = arb;
    uint32_t v = n48dr_arb_entry(arb);
    if (v != 0u) { r.status = v; r.arbAfter = arb; return r; }

    // ---- 1. wake the engine BEFORE acquiring (RDNA4FB / dce_i2c_hw setup_engine order): a never-used engine sits in soft reset with its registers write-blocked.
    e.wr2(N48DR_I2C_CONTROL, 0u);                                  // deassert DC_I2C_SOFT_RESET
    const uint32_t memPwr = e.rd(2u, N48DR_DIO_MEM_PWR_CTRL);
    if (memPwr != 0xFFFFFFFFu && (memPwr & 1u)) {
        e.wr2(N48DR_DIO_MEM_PWR_CTRL, memPwr & ~1u);               // unforce I2C RAM light sleep
        for (int i = 0; i < 10 && (e.rd(2u, N48DR_DIO_MEM_PWR_STATUS) & 1u); i++) e.delay_us(1u);
    }
    e.wr2(rSetup, e.rd(2u, rSetup) | N48DR_SETUP_CLK_EN);
    if (e.refused()) { r.status = N48DR_WRITE_REFUSED; r.arbAfter = e.rd(2u, N48DR_I2C_ARBITRATION); return r; }

    // ---- 2. acquire (dce_i2c_hw acquire_engine): re-check the owner (it may have changed while we woke the engine), request, require the grant.
    arb = e.rd(2u, N48DR_I2C_ARBITRATION);
    r.arb = arb;
    v = n48dr_arb_entry(arb);
    if (v != 0u) { r.status = v; r.arbAfter = arb; return r; }   // someone took it meanwhile: we own nothing, nothing more is written
    e.wr2(N48DR_I2C_ARBITRATION, arb | N48DR_ARB_SW_USE_REQ);
    arb = e.rd(2u, N48DR_I2C_ARBITRATION);
    r.arb = arb;
    if (!n48dr_arb_granted(arb)) {
        // withdraw our request only (DONE_USING); the engine is not ours, so no soft reset
        e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);
        r.status = e.refused() ? N48DR_WRITE_REFUSED : N48DR_NOT_GRANTED;
        r.arbAfter = e.rd(2u, N48DR_I2C_ARBITRATION);
        return r;
    }
    r.granted = true;

    // ---- 3. everything from here exits through the release.
    uint32_t status = N48DR_OK;
    do {
        // engine setup (setup_engine)
        e.wr2(rSetup, N48DR_SETUP_VALUE);
        e.wr2(N48DR_I2C_CONTROL, N48DR_CTL_SW_STATUS_RESET | (line << N48DR_CTL_DDC_SELECT_SHIFT));
        e.wr2(N48DR_I2C_ARBITRATION, (e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_USE_REQ) & ~N48DR_ARB_NO_QUEUED_SW_GO);
        // SCL speed: prescale from the microsecond time base (set_speed). If the time base is unprogrammed keep whatever the firmware left.
        const uint32_t mtb = e.rd(1u, N48DR_MICROSECOND_TIME_BASE_DIV);
        const uint32_t refBase = mtb & 0x7Fu;
        uint32_t xtalDiv = (mtb >> 8) & 0x7Fu;
        if (mtb != 0xFFFFFFFFu && refBase != 0u) {
            if (xtalDiv == 0u) xtalDiv = 2u;
            const uint32_t prescale = (refBase * 1000u / xtalDiv) / 100u;      // 100 kHz
            e.wr2(rSpeed, (prescale << 16) | (2u << 8) | 2u);
        }
        // the transactions, then the FIFO (address bytes carry the R/W bit; the first push sets index 0)
        for (unsigned i = 0; i < p.ntxn; i++) e.wr2(N48DR_I2C_TRANSACTION0 + i, n48dr_txn_reg(&p.t[i]));
        for (unsigned i = 0; i < p.fifo_len; i++)
            e.wr2(N48DR_I2C_DATA, (i == 0u ? N48DR_DATA_INDEX_WRITE : 0u) | ((uint32_t)p.fifo[i] << N48DR_DATA_SHIFT));
        if (e.refused()) { status = N48DR_WRITE_REFUSED; break; }          // BEFORE the GO: a half-programmed engine never starts
        const uint32_t ctl = (line << N48DR_CTL_DDC_SELECT_SHIFT) | ((uint32_t)(p.ntxn - 1u) << N48DR_CTL_TXN_COUNT_SHIFT);
        e.wr2(N48DR_I2C_CONTROL, ctl);
        e.wr2(N48DR_I2C_CONTROL, ctl | N48DR_CTL_GO);
        if (e.refused()) { status = N48DR_WRITE_REFUSED; break; }
        // bounded poll: 20 ms per transaction
        const uint32_t bound = n48dr_poll_bound(&p);
        bool finished = false;
        for (uint32_t i = 0; i < bound; i++) {
            r.polls++;
            const uint32_t sts = e.rd(2u, N48DR_I2C_SW_STATUS);
            r.swStatus = sts;
            const int vd = n48dr_sw_status_verdict(sts);
            if (vd > 0) { status = (uint32_t)vd; finished = true; break; }
            if (vd == 0) { finished = true; break; }
            e.delay_us(N48DR_DDC_POLL_STEP_US);
        }
        if (!finished) { status = N48DR_TIMEOUT; break; }
        if (status != N48DR_OK) break;
        // the reply FIFO: the read data starts after every byte we pushed (process_channel_reply)
        e.wr2(N48DR_I2C_DATA, N48DR_DATA_INDEX_WRITE | N48DR_DATA_READ | ((uint32_t)p.read_index << N48DR_DATA_INDEX_SHIFT));
        for (unsigned i = 0; i < nread && i < N48DR_EDID_BLOCK; i++) r.data[i] = (uint8_t)((e.rd(2u, N48DR_I2C_DATA) >> N48DR_DATA_SHIFT) & 0xFFu);
        r.arb = e.rd(2u, N48DR_I2C_ARBITRATION);
    } while (0);

    // ---- 4. ALWAYS release: soft reset is safe while software owns the engine; DONE_USING hands it back.
    e.wr2(N48DR_I2C_CONTROL, N48DR_CTL_SOFT_RESET | N48DR_CTL_SW_STATUS_RESET);
    e.wr2(rSetup, 0u);
    e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);
    r.released = true;
    r.arbAfter = e.rd(2u, N48DR_I2C_ARBITRATION);
    r.status = (status == N48DR_OK && e.refused()) ? N48DR_WRITE_REFUSED : status;
    return r;
}

template <class E>
inline DdcResult ddc_read(E &e, uint32_t line, uint32_t block)
{
    DdcResult r;
    n48dr_plan p;
    if (n48dr_plan_ddc(line, block, &p) != 0u) return r;     // BAD_ARG: not one register touched
    return i2c_xfer(e, line, p, N48DR_EDID_BLOCK);
}

// 0.0.633: scdcread. One 1-byte register-offset write to the SCDC slave (0x54) then a repeated-start read of len (1..16) bytes, lines 2 / 3 only, offsets 0x00..0x5F only: READ-ONLY toward the sink.
template <class E>
inline DdcResult scdc_read(E &e, uint32_t line, uint32_t off, uint32_t len)
{
    DdcResult r;
    n48dr_plan p;
    if (n48dr_plan_scdc(line, off, len, &p) != 0u) return r;     // BAD_ARG: not one register touched
    return i2c_xfer(e, line, p, len);
}

// ---- dmubring ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Which status a dmubring page gets, from the ring's facts alone (no I/O). page 1..25 = command page-1.
inline uint32_t ring_page_verdict(uint32_t page, bool ringKnown, uint32_t size, uint32_t wptr, uint64_t ringVramOff, uint64_t bar0Size)
{
    if (!ringKnown) return N48DR_NO_RING;
    uint32_t sane = 0u;
    const uint32_t n = n48dr_ring_count(size, wptr, &sane);
    if (!sane) return N48DR_NO_RING;
    if (page < 1u || page > n) return N48DR_PAGE_RANGE;
    if (!n48dr_ring_in_bar0(ringVramOff, (uint64_t)n * N48DR_DMUB_CMD_SIZE, bar0Size)) return N48DR_RING_UNREACHABLE;
    return N48DR_OK;
}

// Read command `idx` (0-based) through M::rd32(vramOffset) - the kext's M is a BAR0 read (amdgpu::RBAR0_32); NEVER MM_INDEX. Header dword + the first 32 payload bytes.
template <class M>
inline void ring_read_cmd(M &m, uint64_t ringVramOff, uint32_t idx, uint32_t *header, uint32_t payload8[8])
{
    const uint64_t at = ringVramOff + (uint64_t)idx * N48DR_DMUB_CMD_SIZE;
    *header = m.rd32(at);
    for (unsigned i = 0; i < 8u; i++) payload8[i] = m.rd32(at + 4u + 4u * i);
}

// ---- dispcensus ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// One page of the census (0.0.628: pages 0..9, 0.0.630; every page is READS through E::rd only): 24 registers packed two per scalar into v[1..12]; v[0] = status | page << 8 | count << 16.
template <class E>
inline uint32_t census_page(E &e, uint32_t page, uint64_t *v)
{
    for (unsigned i = 0; i < 13u; i++) v[i] = 0u;
    if (!n48dr_census_arg_ok(page)) { v[0] = N48DR_BAD_ARG; return N48DR_BAD_ARG; }
    uint32_t vals[N48DR_CENSUS_PER_PAGE] = {};
    const uint32_t n = n48dr_census_in_page(page);
    for (uint32_t i = 0; i < n; i++) { const struct n48dr_reg *r = n48dr_census_reg(page, i); vals[i] = e.rd(r->base_idx, r->off); }   // 0.0.628: pages 2.. come from the M4a table
    v[0] = (uint64_t)N48DR_OK | ((uint64_t)page << 8) | ((uint64_t)n << 16);
    n48dr_pack_regs(v, 1u, vals, n);
    return N48DR_OK;
}

// ---- region4read (0.0.624, stage M1.5) ----------------------------------------------------------------------------------------------------------------------------------------------------------
// The sequence: compute the REGION4 window base from the DMCUB register values the caller probed (n48dr_region4_base refuses a disabled / misaligned / outside-VRAM window), validate the (offset, dwords)
// range against the 64 KiB window, then ONE read of `dwords` dwords at (base + offset) through M::rd(mmOff, dst, n) -> bool. The kext's M is navi48_vram_read_mm (MM_INDEX under gVramMmLock and the MM-priority
// scope: the existing serialised, bounds-checked READ path) and nothing else: M has no write member, so this sequence cannot write. *base_out is the base (all ones when refused).
template <class M>
inline uint32_t region4_read(M &m, uint32_t off_lo, uint32_t off_hi, uint32_t enabled, uint64_t fb_base_mc, uint64_t fb_size, uint32_t off, uint32_t dwords, uint64_t *base_out, uint32_t *dst)
{
    uint64_t base = ~0ull;
    const uint32_t st = n48dr_region4_base(off_lo, off_hi, enabled, fb_base_mc, fb_size, &base);
    if (base_out) *base_out = base;
    if (st != N48DR_OK) return st;
    if (!dst || !n48dr_r4_range_ok(off, dwords)) return N48DR_BAD_ARG;
    return m.rd(base + off, dst, dwords) ? (uint32_t)N48DR_OK : (uint32_t)N48DR_VRAM_READ_FAILED;
}

}  // namespace n48dr

#endif /* N48_DISPREAD_FLOW_H */
