// gfx_mib_xib_checks.h — build 0.0.505 (notes/design/CROSS-IB.md Q4 C1-C4, tests T1-T8; ): THE CROSS-IB
// CHECKS, included by gfx_mib_test.cpp (the `gfx_mib` suite: it links xlat12_ib.c/xlat12.c and reads AppleHardwareHook.cpp
// for its pins). Everything here drives the REAL code (gfx_mib.h n48_mib_segment / n48_mib_lead_try / n48_mib_units /
// n48_mib_head_executes / n48_mib_xib_note, gfx_commit.h n48_cm_gate, xlat12_ib_translate_draw_ex) over run10p's REAL
// frames (tests/fixture_xib_run10p.h, each frame from its own capture rows): F52 (frame b), F20, F98 (family e), F30 (an N
// family). The planted breaks are made in the REAL files by the builder (plant.py), and each must turn a check below FAIL.
// T7's reference is 0.0.504's own n48_mib_segment / n48_mib_seg_disguised_head / n48_mib_units, FROZEN below verbatim
// (`git show 0299a0f9:src/navi48-bringup/src/apple/gfx_mib.h`, renamed only), so "switch 69 OFF = 0.0.504" is compared
// table for table rather than asserted.
#pragma once
#include <vector>
#include <cstring>
#include "fixture_xib_run10p.h"

namespace xib505 {

// ---- 0.0.504, FROZEN (verbatim but for the three names). Do not edit: T7 compares switch 69 OFF against it. ----
static inline int xib_frozen_disguised_head_504(const uint32_t *ib, uint32_t off, uint32_t n)
{
    if (n < 1u) return 0;                                      /* need dword 0 just to decode a header at all */
    const uint32_t h = ib[off];
    if (((h >> 30) & 3u) != 3u) return 0;                      /* not a type-3 packet: never a NOP at all */
    if (((h >> 8) & 0xFFu) != 0x10u) return 0;                 /* not opcode NOP (xlat12_ib.c's OP_NOP == 0x10) */
    const uint32_t noplen = ((h >> 16) & 0x3FFFu) + 2u;        /* xlat12_ib.c's plen_at formula, by content */
    /* THE BOUNDS GUARD: the NOP's own declared body (`noplen` dwords) AND the two-dword trailing EVENT_WRITE this
     * rule must read right after it both have to lie inside the IB - checked with ARITHMETIC ONLY, before any dword
     * past `ib[off]` itself is read. A NOP that claims more than the IB holds is refused HERE, never read past `n`;
     * this is not merely implied by the length-10 check below, because that check has not run yet when this one
     * decides (a NOP whose declared length happens to equal 10 but whose IB is shorter than 12 dwords is refused by
     * THIS rung, independent of the one after it). */
    if ((uint64_t)noplen + 2u > (uint64_t)n) return 0;
    if (ib[off + 1u] != XLAT12_SEG_HEAD1 || ib[off + 2u] != XLAT12_SEG_ACQUIRE) return 0;
    /* Only a NOP of EXACTLY 10 dwords - EVENT_WRITE(2 dw) + ACQUIRE_MEM(8 dw), the one combination seg_start_at's
     * own pattern is built from - is the disguise this rule recognises. A shorter or longer NOP could still
     * coincidentally carry XLAT12_SEG_HEAD1/XLAT12_SEG_ACQUIRE at dwords 1/2 (just checked) without genuinely being
     * that pair's body, so this is a THIRD, independent guard, not a restatement of the two above. */
    if (noplen != 10u) return 0;
    return ib[off + noplen] == XLAT12_SEG_HEAD0 && ib[off + noplen + 1u] == XLAT12_SEG_EVENT_E;
}

static inline uint32_t xib_frozen_segment_504(const uint32_t *ib, uint32_t nib,
                                       const uint32_t *ib_off, const uint32_t *ib_n,
                                       xlat12_ib_segment *segs, uint32_t maxSegs,
                                       uint32_t *ib_nseg, uint32_t *totalOut,
                                       n48_mib_seg_diag *diagOut)
{
    if (totalOut) *totalOut = 0u;
    if (diagOut) *diagOut = n48_mib_seg_diag {};
    if (!ib || !ib_off || !ib_n || !segs || !ib_nseg || nib == 0u || maxSegs == 0u) return 0u;
    uint32_t ns = 0u, tot = 0u, ok = 1u;
    for (uint32_t k = 0; k < nib; k++) {
        const uint32_t off_k = ib_off[k];
        const uint32_t n_k = ib_n[k];
        uint32_t s_k = 0u, t_k = 0u, asked = 0u;
        if (n_k && ns < maxSegs) {
            asked = 1u;
            s_k = xlat12_ib_segments(&ib[off_k], n_k, &segs[ns], maxSegs - ns, &t_k);
            // 0.0.432: THE ONE NOP-SKIP ATTEMPT — k >= 1, the plain call found nothing, and there is
            // still room to write the disguised segment.
            if (k > 0u && s_k == 0u && t_k == 0u && ns < maxSegs && xib_frozen_disguised_head_504(ib, off_k, n_k)) {
                uint32_t first_at = 0u, first_raw = 0u, walked = 0u;
                (void)xlat12_ib_seg_probe(&ib[off_k + 2u], n_k - 2u, &first_at, &first_raw, &walked);
                uint32_t localEnd = 0u, disguiseOk = 0u;
                if (first_at == XLAT12_SEG_NONE) {
                    if (walked == n_k - 2u) { localEnd = n_k; disguiseOk = 1u; }   /* the probe's walk reached the IB's own end */
                } else {
                    localEnd = 2u + first_at; disguiseOk = 1u;                    /* the next IB's own real, undisguised head */
                }
                if (disguiseOk) {
                    uint32_t s2 = 1u, t2 = 1u;
                    uint32_t dGlobal = 0u, atGlobal = 0u;
                    segs[ns].head = 0u; segs[ns].start = 2u; segs[ns].end = localEnd;
                    n48_mib_seg_draws(ib, off_k + 2u, off_k + localEnd, &dGlobal, &atGlobal);
                    segs[ns].draws = dGlobal;
                    segs[ns].draw_at = (atGlobal >= off_k) ? (atGlobal - off_k) : 0u;
                    if (localEnd < n_k) {
                        const uint32_t restOff = off_k + localEnd, restN = n_k - localEnd;
                        uint32_t moreTot = 0u;
                        const uint32_t moreS = (ns + 1u < maxSegs)
                            ? xlat12_ib_segments(&ib[restOff], restN, &segs[ns + 1u], maxSegs - ns - 1u, &moreTot) : 0u;
                        if (moreS && moreS == moreTot) {
                            for (uint32_t j = 0u; j < moreS; j++) {
                                segs[ns + 1u + j].head  += localEnd;
                                segs[ns + 1u + j].start += localEnd;
                                segs[ns + 1u + j].end   += localEnd;
                            }
                            s2 += moreS; t2 += moreTot;
                        } else {
                            disguiseOk = 0u;   /* what follows does not itself tile: refuse, exactly as before this brief */
                        }
                    }
                    if (disguiseOk) { s_k = s2; t_k = t2; }
                }
            }
        }
        // 0.0.430: THE CENSUS REASONS, one decision each. An IB the table had no room for is an
        // OVERFLOW, never a zero-segment class - the recogniser was never asked, so no start class can be blamed. An IB
        // the recogniser DID ask and which named no segment is counted by its own start class (the head/nop-head/mid
        // split asked for). A return below the recognisers' own total is the truncation a full table caused; both
        // are the same overflow so the line has one reason for "the table was too small".
        if (diagOut) {
            if (n_k && !asked) diagOut->overflow = 1u;
            if (asked && s_k != t_k) diagOut->overflow = 1u;
            if (asked && s_k == 0u && t_k == 0u)
                diagOut->zero_by_class[n48_mib_start_class(&ib[off_k], n_k)]++;
        }
        for (uint32_t j = 0; j < s_k; j++) {
            segs[ns + j].head  += off_k;
            segs[ns + j].start += off_k;
            segs[ns + j].end   += off_k;
        }
        ib_nseg[k] = s_k;
        ns += s_k; tot += t_k;
        if (s_k == 0u || s_k != t_k) ok = 0u;   /* ns 0 in ANY IB answers segment-policy (B2) */
    }
    if (diagOut && tot > diagOut->max_segs) diagOut->max_segs = tot;   /* the recognisers' Sigma, refusal or not */
    if (!ok || ns == 0u) { if (totalOut) *totalOut = 0u; return 0u; }
    if (totalOut) *totalOut = tot;
    return ns;
}

static inline uint32_t xib_frozen_units_504(const uint32_t *ib, uint32_t nib, const uint32_t *ib_off, const uint32_t *ib_n,
                                     const xlat12_ib_segment *segs, uint32_t ns, uint32_t maxCons,
                                     xlat12_ib_segment *units, uint32_t *cfirst, uint32_t *ccount, uint32_t maxUnits)
{
    if (!ib || !ib_off || !ib_n || !segs || !units || !cfirst || !ccount || nib == 0u || ns == 0u || maxCons == 0u) return 0u;
    uint32_t nu = 0u, prevIb = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < ns; k++) {
        const uint32_t ibk = n48_mib_ib_of(ib_off, ib_n, nib, segs[k].start);
        if (ibk == 0xFFFFFFFFu || segs[k].end > ib_off[ibk] + ib_n[ibk]) return 0u;   /* a segment outside its IB */
        const int join = nu > 0u && ibk == prevIb && ccount[nu - 1u] < maxCons &&
                         n48_mib_seg_continues(ib, segs[k].start, segs[k].end);
        if (join) {
            units[nu - 1u].end = segs[k].end;
            units[nu - 1u].draws += segs[k].draws;
            ccount[nu - 1u]++;
        } else {
            if (nu >= maxUnits) return 0u;
            units[nu].head = segs[k].head; units[nu].start = segs[k].start; units[nu].end = segs[k].end;
            units[nu].draws = segs[k].draws; units[nu].draw_at = segs[k].draw_at;
            cfirst[nu] = k; ccount[nu] = 1u;
            nu++;
        }
        prevIb = ibk;
    }
    return nu;
}

// ---- helpers ----
struct Run {
    xlat12_ib_segment segs[N48_XV_MAX_SEGS];
    uint32_t ibn[N48_XV_MAX_IBS];
    uint32_t tot, ns, lm;
    n48_mib_seg_diag dg;
};
static void run_new(const uint32_t *cat, const uint32_t *off, const uint32_t *len, uint32_t nib, uint32_t xib, Run &r)
{
    std::memset(&r, 0, sizeof r);
    r.ns = n48_mib_segment(cat, nib, off, len, r.segs, N48_XV_MAX_SEGS, r.ibn, &r.tot, &r.dg, xib, &r.lm);
}
static void run_504(const uint32_t *cat, const uint32_t *off, const uint32_t *len, uint32_t nib, Run &r)
{
    std::memset(&r, 0, sizeof r);
    r.ns = xib_frozen_segment_504(cat, nib, off, len, r.segs, N48_XV_MAX_SEGS, r.ibn, &r.tot, &r.dg);
}
struct Fr { const char *name; const uint32_t *cat; uint32_t n; const n48_mib_fixture_ib *ibs; uint32_t nib; };
static void offlen(const Fr &f, uint32_t *off, uint32_t *len)
{
    for (uint32_t k = 0; k < N48_XV_MAX_IBS; k++) { off[k] = k < f.nib ? f.ibs[k].off : 0u; len[k] = k < f.nib ? f.ibs[k].len : 0u; }
}
// A PM4 walk as the CP reads it (the same rules as xlat12_ib.c's plen_at, by content): the packet starts, or `ok` 0 when
// a header is not a packet or a packet runs past `n`.
static std::vector<uint32_t> cp_walk(const uint32_t *d, uint32_t n, uint32_t *ok)
{
    std::vector<uint32_t> b; uint32_t i = 0u; *ok = 1u;
    while (i < n) {
        const uint32_t h = d[i];
        uint32_t l;
        if (h == 0xFFFF1000u || ((h >> 30) & 3u) == 2u) l = 1u;
        else if (((h >> 30) & 3u) != 3u) { *ok = 0u; break; }
        else { l = ((h >> 16) & 0x3FFFu) + 2u; if ((uint64_t)i + l > n) { *ok = 0u; break; } }
        b.push_back(i); i += l;
    }
    if (i != n) *ok = 0u;
    return b;
}
static uint32_t translate(const uint32_t *in, uint32_t n, std::vector<uint32_t> &out, uint32_t *olen, xlat12_draw_stats *ds)
{
    out.assign(n + 64u, 0u); *olen = 0u; *ds = xlat12_draw_stats {};
    return xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), nullptr, in, n, out.data(), olen, ds);
}
static void opset(const uint32_t *d, uint32_t n, std::vector<uint32_t> &ops)
{
    uint32_t ok = 0u; const std::vector<uint32_t> b = cp_walk(d, n, &ok);
    for (uint32_t i : b) if (((d[i] >> 30) & 3u) == 3u && d[i] != 0xFFFF1000u) {
        const uint32_t op = (d[i] >> 8) & 0xFFu;
        bool have = false; for (uint32_t o : ops) if (o == op) have = true;
        if (!have) ops.push_back(op);
    }
}

static const Fr kF52 { "F52", kXibF52, kXibF52_dwords, kXibF52Ibs, kXibF52Nib };
static const Fr kF20 { "F20", kXibF20, kXibF20_dwords, kXibF20Ibs, kXibF20Nib };
static const Fr kF98 { "F98", kXibF98, kXibF98_dwords, kXibF98Ibs, kXibF98Nib };
static const Fr kF30 { "F30", kXibF30, kXibF30_dwords, kXibF30Ibs, kXibF30Nib };

// ---- T1: frame b (F52), IB0 HEAD, IB1 MID ----
static void t1()
{
    uint32_t off[4], len[4]; offlen(kF52, off, len);
    Run r {}, f {};
    run_new(kF52.cat, off, len, 2u, 0u, r); run_504(kF52.cat, off, len, 2u, f);
    expect_u("T1 F52 OFF: 0 segments (IB1 opens MID)", r.ns, 0u);
    expect_u("T1 F52 OFF: ... exactly 0.0.504's answer", r.ns == f.ns && r.tot == f.tot ? 1u : 0u, 1u);
    expect_u("T1 F52 OFF: ... and no lead", r.lm, 0u);
    run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_DISG0, r);
    expect_u("T1 F52 bit 0 alone: still 0 (IB1 is MID, not a disguise)", r.ns, 0u);
    run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
    expect_u("T1 F52 bit 1: 18 segments", r.ns, 18u);
    expect_u("T1 F52 bit 1: total == segments", r.tot, 18u);
    expect_u("T1 F52 bit 1: lead_mask 0b10", r.lm, 2u);
    expect_u("T1 F52 bit 1: 9 heads in IB0", r.ibn[0], 9u);
    expect_u("T1 F52 bit 1: 9 rows in IB1 (the lead + 8)", r.ibn[1], 9u);
    const xlat12_ib_segment &L = r.segs[9];
    expect_u("T1 F52 lead: head == off_1", L.head, off[1]);
    expect_u("T1 F52 lead: start == head (translated from its first dword)", L.start, off[1]);
    expect_u("T1 F52 lead: end == off_1 + 1168 (IB1's first real head)", L.end, off[1] + 1168u);
    expect_u("T1 F52 lead: the lead holds a draw", L.draws >= 1u ? 1u : 0u, 1u);
    expect_u("T1 F52 lead: the next row's head is the real head at IB1 + 1168", r.segs[10].head, off[1] + 1168u);
    uint32_t bad = 0u, at = 0u;
    for (uint32_t k = 0; k < r.ns; k++) {
        const uint32_t want = (k == 9u) ? r.segs[k].head : r.segs[k].head + 2u;
        if (r.segs[k].start != want || r.segs[k].head != at) bad++;
        at = r.segs[k].end;
    }
    expect_u("T1 F52: every row tiles; start == head + 2 except the lead's start == head", bad, 0u);
    expect_u("T1 F52: ... ending at the frame's own dword count", at, kF52.n);
    Run r3 {}; run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_MASK, r3);
    expect_u("T1 F52 M 3: the same table as bit 1", (r3.ns == r.ns && r3.lm == r.lm &&
             std::memcmp(r3.segs, r.segs, sizeof r.segs) == 0) ? 1u : 0u, 1u);
    // C2: THE UNITS. The unit that starts with the lead starts at its head; every other unit at head + 2.
    xlat12_ib_segment units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS];
    const uint32_t nu = n48_mib_units(kF52.cat, 2u, off, len, r.segs, r.ns, XLAT12_UNIT_CONS_MAX, units, cf, cc,
                                      N48_XV_MAX_SEGS, r.lm);
    expect_u("T1 F52 units: formed", nu > 0u ? 1u : 0u, 1u);
    uint32_t leadUnits = 0u, ubad = 0u;
    for (uint32_t j = 0; j < nu; j++) {
        const uint32_t isLead = units[j].head == off[1] ? 1u : 0u;
        leadUnits += isLead;
        if (units[j].start != (isLead ? units[j].head : units[j].head + 2u)) ubad++;
    }
    expect_u("T1 F52 units: exactly one unit starts at IB1's offset (the lead's)", leadUnits, 1u);
    expect_u("T1 F52 units: the lead unit's start == head, every other unit's == head + 2 (P2)", ubad, 0u);
    // P5: switch 49 never credits a lead: the dword at its head is Apple's SET_CONTEXT_REG, not the head's EVENT_WRITE.
    expect_u("T1 F52 P5: n48_mib_head_executes answers 0 at the lead's head", (uint32_t)n48_mib_head_executes(kF52.cat, L.head, kF52.n), 0u);
    expect_u("T1 F52 P5: ... and 1 at IB1's first real head (control)", (uint32_t)n48_mib_head_executes(kF52.cat, r.segs[10].head, kF52.n), 1u);
}

// ---- T2: F20, IB0 NOP-HEAD, IB1 MID ----
static void t2()
{
    uint32_t off[4], len[4]; offlen(kF20, off, len);
    Run r {};
    run_new(kF20.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
    expect_u("T2 F20 bit 1 alone: 0 (IB0 NOP-HEAD is refused)", r.ns, 0u);
    expect_u("T2 F20 bit 1 alone: ... though IB1's lead formed (the census reads it)", r.lm, 2u);
    run_new(kF20.cat, off, len, 2u, N48_MIB_XIB_DISG0, r);
    expect_u("T2 F20 bit 0 alone: 0 (IB1 MID)", r.ns, 0u);
    expect_u("T2 F20 bit 0 alone: ... IB0's disguise formed", r.ibn[0] > 0u ? 1u : 0u, 1u);
    run_new(kF20.cat, off, len, 2u, N48_MIB_XIB_MASK, r);
    expect_u("T2 F20 both: 15 segments", r.ns, 15u);
    expect_u("T2 F20 both: lead_mask 0b10", r.lm, 2u);
    expect_u("T2 F20 both: IB0 first row {0, 2, 1360}", (r.segs[0].head == 0u && r.segs[0].start == 2u && r.segs[0].end == 1360u) ? 1u : 0u, 1u);
    const uint32_t li = r.ibn[0];
    expect_u("T2 F20 both: the lead {off_1, off_1, off_1 + 1094}",
             (r.segs[li].head == off[1] && r.segs[li].start == off[1] && r.segs[li].end == off[1] + 1094u) ? 1u : 0u, 1u);
    expect_u("T2 F20: n48_mib_head_executes answers 0 at IB0's disguised head (it never runs)",
             (uint32_t)n48_mib_head_executes(kF20.cat, 0u, kF20.n), 0u);
}

// ---- T3: family e (F98), IB0 NOP-HEAD, IB1 HEAD ----
static void t3()
{
    uint32_t off[4], len[4]; offlen(kF98, off, len);
    Run r {};
    run_new(kF98.cat, off, len, 2u, 0u, r);
    expect_u("T3 F98 OFF: 0 (IB0's disguise is not offered at k = 0)", r.ns, 0u);
    run_new(kF98.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
    expect_u("T3 F98 bit 1 alone: 0 (no lead on IB 0)", r.ns, 0u);
    run_new(kF98.cat, off, len, 2u, N48_MIB_XIB_DISG0, r);
    expect_u("T3 F98 bit 0: the IB-0 disguise is accepted (the frame segments)", r.ns > 0u ? 1u : 0u, 1u);
    expect_u("T3 F98 bit 0: lead_mask 0", r.lm, 0u);
    expect_u("T3 F98 bit 0: IB0's first row is {0, 2, ...}", (r.segs[0].head == 0u && r.segs[0].start == 2u) ? 1u : 0u, 1u);
    uint32_t bad = 0u, at = 0u;
    for (uint32_t k = 0; k < r.ns; k++) { if (r.segs[k].head != at || r.segs[k].start != r.segs[k].head + 2u) bad++; at = r.segs[k].end; }
    expect_u("T3 F98 bit 0: every row tiles with an ENCODER start to the frame's end", (bad == 0u && at == kF98.n) ? 1u : 0u, 1u);
    n48_cm_frame c; mib_frame_from(c, r.segs, r.ns, len, 2u, kF98.n); c.lead_mask = r.lm;
    uint32_t d = 0u;
    expect_u("T3 F98 bit 0: the table passes the gate", n48_cm_gate(&c, &d), N48_CM_OK);
}

// ---- T4: an N family (F30): the lead forms; IB0's last segment still refuses DRAW_SHAPE; the frame is NOT accepted ----
static void t4()
{
    uint32_t off[4], len[4]; offlen(kF30, off, len);
    Run r {};
    run_new(kF30.cat, off, len, 2u, N48_MIB_XIB_MASK, r);
    expect_u("T4 F30: the lead forms (lead_mask 0b10)", r.lm, 2u);
    expect_u("T4 F30: the segment stage answers a table", r.ns > 0u ? 1u : 0u, 1u);
    n48_cm_frame c; mib_frame_from(c, r.segs, r.ns, len, 2u, kF30.n); c.lead_mask = r.lm;
    uint32_t refusedAt = 0xFFFFFFFFu, st0 = 0u;
    for (uint32_t k = 0; k < r.ns; k++) {
        std::vector<uint32_t> out; uint32_t olen = 0u; xlat12_draw_stats ds {};
        const uint32_t st = translate(&kF30.cat[r.segs[k].start], r.segs[k].end - r.segs[k].start, out, &olen, &ds);
        c.seg[k].status = st; c.seg[k].out_len = st ? 0u : olen;
        if (k == r.ibn[0] - 1u) st0 = st;
        if (st && refusedAt == 0xFFFFFFFFu) refusedAt = k;
    }
    expect_u("T4 F30: IB0's last segment (the compute tail) refuses DRAW_SHAPE", st0, (uint32_t)XLAT12_IB_ERR_DRAW_SHAPE);
    uint32_t d = 0u;
    expect_u("T4 F30: the frame is NOT accepted (SEG_REFUSED)", n48_cm_gate(&c, &d), N48_CM_SEG_REFUSED);
}

// ---- T5: the gate over F52's real table with the lead, and every mutant ----
static uint32_t gate_f52(uint32_t mutant, uint32_t *detail)
{
    uint32_t off[4], len[4]; offlen(kF52, off, len);
    Run r {}; run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
    n48_cm_frame c; mib_frame_from(c, r.segs, r.ns, len, 2u, kF52.n); c.lead_mask = r.lm;
    switch (mutant) {
    case 1: c.lead_mask = 0u; break;                                             /* the lead without its bit */
    case 2: c.lead_mask |= 1u; c.seg[0].start = c.seg[0].head; break;            /* bit 0 + IB0's first row from its head */
    case 3: c.lead_mask |= 4u; break;                                            /* a bit past nib */
    case 4: c.seg[1].start = c.seg[1].head; break;                               /* a non-lead ENCODER row from its head */
    case 5: c.seg[9].start = c.seg[9].head + 2u; c.seg[9].out_len -= 2u; break;  /* the lead given head + 2 */
    case 6: c.seg_kind = N48_CM_KIND_HEADLESS; break;                            /* not ENCODER */
    case 7: c.lead_mask |= 1u; break;                                            /* bit 0 alone */
    default: break;
    }
    return n48_cm_gate(&c, detail);
}
static void t5()
{
    uint32_t d = 0u;
    expect_u("T5 gate: F52's real table with the lead passes (COVER, SEG_KIND, COVER per IB)", gate_f52(0u, &d), N48_CM_OK);
    expect_u("T5 mutant: the lead's bit dropped -> SEG_KIND", gate_f52(1u, &d), N48_CM_SEG_KIND);
    expect_u("T5 mutant: ... named at the lead's row (0x8009xxxx)", d >> 16, 0x8009u);
    expect_u("T5 mutant: bit 0 + IB0 from its head -> SEG_KIND 0x00050001 (P4)", gate_f52(2u, &d) == N48_CM_SEG_KIND && d == 0x00050001u ? 1u : 0u, 1u);
    expect_u("T5 mutant: a bit past nib -> SEG_KIND 0x00050002", gate_f52(3u, &d) == N48_CM_SEG_KIND && d == 0x00050002u ? 1u : 0u, 1u);
    expect_u("T5 mutant: a non-lead ENCODER row starting at its head -> SEG_KIND (P3)", gate_f52(4u, &d), N48_CM_SEG_KIND);
    expect_u("T5 mutant: ... named at row 1", d >> 16, 0x8001u);
    expect_u("T5 mutant: the lead given head + 2 -> SEG_KIND", gate_f52(5u, &d), N48_CM_SEG_KIND);
    expect_u("T5 mutant: HEADLESS kind over a multi-IB frame -> SEG_KIND", gate_f52(6u, &d), N48_CM_SEG_KIND);
    expect_u("T5 mutant: bit 0 alone -> SEG_KIND 0x00050001", gate_f52(7u, &d) == N48_CM_SEG_KIND && d == 0x00050001u ? 1u : 0u, 1u);
    // A lead without mib: a single-IB frame (IB1's lead table alone) claiming a lead on IB 1.
    {
        n48_cm_frame c; uint32_t off[4], len[4]; offlen(kF52, off, len);
        Run r {}; run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
        mib_frame_from(c, r.segs, 9u, len, 1u, len[0]); c.mib = 0u; c.lead_mask = 2u;
        expect_u("T5 mutant: a lead without mib -> SEG_KIND 0x00050003", n48_cm_gate(&c, &d) == N48_CM_SEG_KIND && d == 0x00050003u ? 1u : 0u, 1u);
        c.lead_mask = 0u;
        expect_u("T5 control: ... and the same single-IB table without the lead passes", n48_cm_gate(&c, &d), N48_CM_OK);
    }
}

// ---- T6: F52's lead translates in place, and emits nothing an ordinary segment's translation does not ----
static void t6()
{
    uint32_t off[4], len[4]; offlen(kF52, off, len);
    Run r {}; run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_LEAD, r);
    const xlat12_ib_segment &L = r.segs[9];
    std::vector<uint32_t> out; uint32_t olen = 0u; xlat12_draw_stats ds {};
    const uint32_t st = translate(&kF52.cat[L.start], L.end - L.start, out, &olen, &ds);
    expect_u("T6 F52 lead: xlat12_ib_translate_draw_ex translates it", st, 0u);
    expect_u("T6 F52 lead: out_len == end - start (in place)", olen, L.end - L.start);
    expect_u("T6 F52 lead: ... carrying every draw", ds.draws, L.draws);
    uint32_t ok = 0u; const std::vector<uint32_t> b = cp_walk(out.data(), olen, &ok);
    expect_u("T6 F52 lead: the output walks as packets from dword 0 to its end (no Apple dword left mid-packet)", ok, 1u);
    std::vector<uint32_t> ordOps, leadOps; uint32_t ordN = 0u;
    for (uint32_t k = 0; k < r.ns; k++) {
        if (k == 9u) continue;
        std::vector<uint32_t> o; uint32_t ol = 0u; xlat12_draw_stats d2 {};
        if (translate(&kF52.cat[r.segs[k].start], r.segs[k].end - r.segs[k].start, o, &ol, &d2) == 0u) { opset(o.data(), ol, ordOps); ordN++; }
    }
    opset(out.data(), olen, leadOps);
    uint32_t extra = 0u;
    for (uint32_t op : leadOps) { bool in = false; for (uint32_t o : ordOps) if (o == op) in = true; if (!in) extra++; }
    expect_u("T6 F52: ordinary segments of the same frame translate (the comparison set is not empty)", ordN > 0u ? 1u : 0u, 1u);
    expect_u("T6 F52 lead: every packet opcode in its output is one the translator emits for the frame's ordinary segments", extra, 0u);
}

// ---- T7: OFF identity against 0.0.504, table for table ----
static void t7_one(const char *name, const uint32_t *cat, const n48_mib_fixture_ib *ibs, uint32_t nib)
{
    uint32_t off[4] = { 0, 0, 0, 0 }, len[4] = { 0, 0, 0, 0 };
    for (uint32_t k = 0; k < nib; k++) { off[k] = ibs[k].off; len[k] = ibs[k].len; }
    Run a {}, b {};
    run_new(cat, off, len, nib, 0u, a); run_504(cat, off, len, nib, b);
    char lbl[160];
    std::snprintf(lbl, sizeof lbl, "T7 %s OFF: segs/total/ib_nseg/diag byte-identical to 0.0.504, lead_mask 0", name);
    expect_u(lbl, (a.ns == b.ns && a.tot == b.tot && a.lm == 0u && std::memcmp(a.segs, b.segs, sizeof a.segs) == 0 &&
                   std::memcmp(a.ibn, b.ibn, sizeof a.ibn) == 0 && std::memcmp(&a.dg, &b.dg, sizeof a.dg) == 0) ? 1u : 0u, 1u);
    if (a.ns) {
        xlat12_ib_segment u1[N48_XV_MAX_SEGS], u2[N48_XV_MAX_SEGS]; uint32_t c1[N48_XV_MAX_SEGS], n1[N48_XV_MAX_SEGS], c2[N48_XV_MAX_SEGS], n2[N48_XV_MAX_SEGS];
        std::memset(u1, 0, sizeof u1); std::memset(u2, 0, sizeof u2);
        std::memset(c1, 0, sizeof c1); std::memset(n1, 0, sizeof n1); std::memset(c2, 0, sizeof c2); std::memset(n2, 0, sizeof n2);
        const uint32_t x1 = n48_mib_units(cat, nib, off, len, a.segs, a.ns, XLAT12_UNIT_CONS_MAX, u1, c1, n1, N48_XV_MAX_SEGS, 0u);
        const uint32_t x2 = xib_frozen_units_504(cat, nib, off, len, b.segs, b.ns, XLAT12_UNIT_CONS_MAX, u2, c2, n2, N48_XV_MAX_SEGS);
        std::snprintf(lbl, sizeof lbl, "T7 %s OFF: the unit table is byte-identical to 0.0.504's", name);
        expect_u(lbl, (x1 == x2 && std::memcmp(u1, u2, sizeof u1) == 0 && std::memcmp(c1, c2, sizeof c1) == 0 &&
                       std::memcmp(n1, n2, sizeof n1) == 0) ? 1u : 0u, 1u);
    }
}
static void t7()
{
    t7_one("F52 (run10p)", kXibF52, kXibF52Ibs, kXibF52Nib);
    t7_one("F20 (run10p)", kXibF20, kXibF20Ibs, kXibF20Nib);
    t7_one("F98 (run10p)", kXibF98, kXibF98Ibs, kXibF98Nib);
    t7_one("F30 (run10p)", kXibF30, kXibF30Ibs, kXibF30Nib);
    t7_one("F48 (arm32)", kF48Mib, kF48MibIbs, kF48MibNib);
    t7_one("F20 (arm32)", kF20Mib, kF20MibIbs, kF20MibNib);
    t7_one("F21 (arm32, NOP-headed IB1)", kF21Mib, kF21MibIbs, kF21MibNib);
    t7_one("F33 (decide36b, NOP-headed IB1)", kF33D36BMib, kF33D36BMibIbs, kF33D36BMibNib);
    // The disguise at k >= 1 with a predicate bit in its header: 0.0.504 accepts it (length 10), so OFF must too; switch 69
    // asks for 0xC0081000 exactly and refuses it.
    std::vector<uint32_t> m(kF21Mib, kF21Mib + kF21Mib_dwords);
    m[kF21MibIbs[1].off] |= 1u;
    uint32_t off[4] = { kF21MibIbs[0].off, kF21MibIbs[1].off, 0, 0 }, len[4] = { kF21MibIbs[0].len, kF21MibIbs[1].len, 0, 0 };
    Run a {}, b {};
    run_new(m.data(), off, len, 2u, 0u, a); run_504(m.data(), off, len, 2u, b);
    expect_u("T7 F21 with 0xC0081001 at IB1: OFF accepts it, exactly as 0.0.504", (a.ns > 0u && a.ns == b.ns &&
             std::memcmp(a.segs, b.segs, sizeof a.segs) == 0) ? 1u : 0u, 1u);
    run_new(m.data(), off, len, 2u, N48_MIB_XIB_DISG0, a);
    expect_u("T7 F21 with 0xC0081001 at IB1: switch 69 M 1 refuses the disguise (exactly 0xC0081000)", a.ns, 0u);
}

// ---- T8: item (4) for k = 0 — F20 IB0's disguised segment ----
static uint32_t t8_ok(const uint32_t *in, const uint32_t *cand, uint32_t n)
{
    for (uint32_t i = 0; i < 10u; i++) if (cand[i] != in[i]) return 0u;   /* output dwords 0-9 equal the input */
    uint32_t ok = 0u; const std::vector<uint32_t> b = cp_walk(cand, n, &ok);
    return (ok && b.size() >= 3u && b[0] == 0u && b[1] == 10u && b[2] == 12u) ? 1u : 0u;
}
static int t8_csn(void *, uint64_t) { return 1; }
static void t8_one(const char *name, const Fr &F, uint32_t csElide)
{
    uint32_t off[4], len[4]; offlen(F, off, len);
    Run r {}; run_new(F.cat, off, len, 2u, N48_MIB_XIB_MASK, r);
    const xlat12_ib_segment &S = r.segs[0];
    char lbl[200];
    std::snprintf(lbl, sizeof lbl, "T8 %s IB0: the disguised row is {0, 2, end}", name);
    expect_u(lbl, (r.ns > 0u && S.head == 0u && S.start == 2u && S.end > 12u) ? 1u : 0u, 1u);
    std::vector<uint32_t> out(S.end + 64u, 0u); uint32_t olen = 0u; xlat12_draw_stats ds {};
    xlat12_draw_extra ex {};
    if (csElide) { ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = &t8_csn; }
    const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), csElide ? &ex : nullptr, &F.cat[S.start],
                                                    S.end - S.start, out.data(), &olen, &ds);
    std::snprintf(lbl, sizeof lbl, "T8 %s IB0: the disguised segment translates, in place", name);
    expect_u(lbl, (st == 0u && olen == S.end - S.start) ? 1u : 0u, 1u);
    std::vector<uint32_t> cand(F.cat, F.cat + S.end);                        /* the commit writes the candidate from `start` */
    for (uint32_t i = 0; i < olen && S.start + i < S.end; i++) cand[S.start + i] = out[i];
    std::snprintf(lbl, sizeof lbl, "T8 %s IB0: dword 0 stays 0xC0081000", name);
    expect_u(lbl, cand[0], N48_MIB_NOP_HEAD);
    std::snprintf(lbl, sizeof lbl, "T8 %s IB0: output dwords 0-9 equal the input; CP walk boundaries 0/10/12", name);
    expect_u(lbl, t8_ok(F.cat, cand.data(), S.end), 1u);
    std::vector<uint32_t> mut(cand);                                         /* a translator that emits ONE dword first */
    for (uint32_t i = S.end - 1u; i > S.start; i--) mut[i] = mut[i - 1u];
    mut[S.start] = 0x80000000u;
    std::snprintf(lbl, sizeof lbl, "T8 %s IB0: a mutant emitting one dword first FAILS the check", name);
    expect_u(lbl, t8_ok(F.cat, mut.data(), S.end), 0u);
}
static void t8()
{
    // F98 (family e): translated exactly as the other T tests translate (no descriptor path, no stand-in).
    t8_one("F98", kF98, 0u);
    // F20: its IB0 disguised segment carries a compute dispatch (DISPATCH_DIRECT at dword 135), UNLISTED without switch 57's
    // compute elide, so it is translated here with XLAT12_EXTRA_CS_ELIDE and a STAND-IN classifier that answers "N" for
    // every program - the head's structure (dwords 0-12) is what this checks, not the dispatch.
    t8_one("F20 (CS_ELIDE stand-in)", kF20, 1u);
}

// ---- the planted-break reachability tests: each rung of the lead rule, over REAL bytes with one change ----
static void rungs()
{
    uint32_t off[4], len[4]; offlen(kF52, off, len);
    const uint32_t o1 = off[1], n1 = len[1];
    // P1: a lead on IB 0. IB0 = F52's MID IB1, IB1 = F98's HEAD IB1: switch 69 M 3 must refuse the frame.
    {
        std::vector<uint32_t> cat(kF52.cat + o1, kF52.cat + o1 + n1);
        cat.insert(cat.end(), kXibF98 + kXibF98Ibs[1].off, kXibF98 + kXibF98Ibs[1].off + kXibF98Ibs[1].len);
        const uint32_t o2[4] = { 0u, n1, 0u, 0u }, l2[4] = { n1, kXibF98Ibs[1].len, 0u, 0u };
        Run r {}; run_new(cat.data(), o2, l2, 2u, N48_MIB_XIB_MASK, r);
        expect_u("P1: a MID IB 0 gets no lead (0 rows)", r.ibn[0], 0u);
        expect_u("P1: ... bit 0 of lead_mask stays clear", r.lm & 1u, 0u);
        expect_u("P1: ... and the frame is refused", r.ns, 0u);
    }
    xlat12_ib_segment seg[N48_XV_MAX_SEGS]; uint32_t t = 0u;
    expect_u("control: F52 IB1's lead forms through n48_mib_lead_try", n48_mib_lead_try(kF52.cat, o1, n1, seg, N48_XV_MAX_SEGS, &t), 9u);
    // P6: a head hidden in a payload before the first packet-aligned head (fa != fr).
    {
        std::vector<uint32_t> ib(kF52.cat + o1, kF52.cat + o1 + n1);
        uint32_t i = 0u, planted = 0u;
        while (i < 1168u && !planted) {
            const uint32_t h = ib[i]; uint32_t l = 1u;
            if (((h >> 30) & 3u) == 3u && h != 0xFFFF1000u) {
                l = ((h >> 16) & 0x3FFFu) + 2u;
                if (l >= 14u) {
                    const uint32_t p = i + 1u;
                    ib[p] = XLAT12_SEG_HEAD0; ib[p + 1u] = XLAT12_SEG_HEAD1; ib[p + 2u] = XLAT12_SEG_ACQUIRE;
                    ib[p + 10u] = XLAT12_SEG_HEAD0; ib[p + 11u] = XLAT12_SEG_EVENT_E; planted = 1u;
                }
            }
            i += l;
        }
        expect_u("P6 setup: a packet of >= 14 dwords in the lead carries the planted raw head", planted, 1u);
        uint32_t fa = 0u, fr = 0u, w = 0u; (void)xlat12_ib_seg_probe(ib.data(), n1, &fa, &fr, &w);
        expect_u("P6 setup: ... the raw match now precedes the aligned head", fr < fa ? 1u : 0u, 1u);
        expect_u("P6: fa != fr refuses the lead", n48_mib_lead_try(ib.data(), 0u, n1, seg, N48_XV_MAX_SEGS, &t), 0u);
    }
    // P7: no head at all and the walk stops short of the IB's end.
    {
        std::vector<uint32_t> ib(kF52.cat + o1, kF52.cat + o1 + 1168u);
        expect_u("P7 control: the headless prefix alone is one lead [0, 1168)", n48_mib_lead_try(ib.data(), 0u, 1168u, seg, N48_XV_MAX_SEGS, &t), 1u);
        ib.push_back(0x40000000u);                                          /* a type-1 dword: the walk stops at 1168 */
        expect_u("P7: a walk short of the IB refuses the lead", n48_mib_lead_try(ib.data(), 0u, 1169u, seg, N48_XV_MAX_SEGS, &t), 0u);
        std::vector<uint32_t> ib2(kF52.cat + o1, kF52.cat + o1 + n1); ib2.push_back(0x40000000u);
        expect_u("P7b: the rest's walk short of the IB refuses the lead", n48_mib_lead_try(ib2.data(), 0u, n1 + 1u, seg, N48_XV_MAX_SEGS, &t), 0u);
    }
    // P8: a lead only for a MID start: F20's NOP-HEAD IB0 and F98's HEAD IB1 are refused by the rule itself.
    expect_u("P8: n48_mib_lead_try refuses a NOP-HEAD IB (F20 IB0)", n48_mib_lead_try(kF20.cat, 0u, kXibF20Ibs[0].len, seg, N48_XV_MAX_SEGS, &t), 0u);
    expect_u("P8: n48_mib_lead_try refuses a HEAD IB (F98 IB1)", n48_mib_lead_try(kXibF98, kXibF98Ibs[1].off, kXibF98Ibs[1].len, seg, N48_XV_MAX_SEGS, &t), 0u);
    // P9: a draw-less prefix: F52 IB1 with every draw in [0, 1168) made a NOP of the same length.
    {
        std::vector<uint32_t> ib(kF52.cat + o1, kF52.cat + o1 + n1);
        uint32_t i = 0u, nd = 0u;
        while (i < 1168u) {
            const uint32_t h = ib[i]; uint32_t l = 1u;
            if (((h >> 30) & 3u) == 3u && h != 0xFFFF1000u) {
                l = ((h >> 16) & 0x3FFFu) + 2u;
                const uint32_t op = (h >> 8) & 0xFFu;
                if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) { ib[i] = (h & 0xFFFF00FFu) | 0x1000u; nd++; }
            }
            i += l;
        }
        expect_u("P9 setup: the prefix had draws", nd > 0u ? 1u : 0u, 1u);
        expect_u("P9: a draw-less prefix refuses the lead", n48_mib_lead_try(ib.data(), 0u, n1, seg, N48_XV_MAX_SEGS, &t), 0u);
    }
    // P10: the disguise rule under switch 69 asks for 0xC0081000 EXACTLY: a predicate bit, or a NOP of 11, is refused.
    {
        expect_u("P10 control: F20 IB0's real disguise passes the exact rule", (uint32_t)n48_mib_seg_disguised_head_x(kF20.cat, 0u, kXibF20Ibs[0].len, 1u), 1u);
        std::vector<uint32_t> ib(kF20.cat, kF20.cat + kXibF20Ibs[0].len);
        ib[0] = 0xC0081001u;
        expect_u("P10: 0xC0081001 is refused by the exact rule", (uint32_t)n48_mib_seg_disguised_head_x(ib.data(), 0u, (uint32_t)ib.size(), 1u), 0u);
        expect_u("P10: ... and still accepted by 0.0.504's rule (OFF)", (uint32_t)n48_mib_seg_disguised_head_x(ib.data(), 0u, (uint32_t)ib.size(), 0u), 1u);
        std::vector<uint32_t> ib11(kF20.cat, kF20.cat + kXibF20Ibs[0].len);
        ib11.insert(ib11.begin() + 10, 0x80000000u);                         /* a NOP of 11 over the same pair + one filler */
        ib11[0] = 0xC0091000u;
        expect_u("P10: a NOP of 11 with the head's tail after it is refused by the exact rule", (uint32_t)n48_mib_seg_disguised_head_x(ib11.data(), 0u, (uint32_t)ib11.size(), 1u), 0u);
        uint32_t o2[4] = { 0u, 0u, 0u, 0u }, l2[4] = { (uint32_t)ib.size(), 0u, 0u, 0u };
        Run r {}; run_new(ib.data(), o2, l2, 1u, N48_MIB_XIB_MASK, r);
        expect_u("P10: switch 69 M 3 gives a 0xC0081001 IB 0 no rows", r.ibn[0], 0u);
    }
}

// ---- C4: the census over the real frames, and its line at widest ----
static void census()
{
    n48_mib_xib x {};
    uint32_t off[4], len[4];
    // F52 with switch 69 OFF: the predicate (M 3) rescues it by a lead; nothing DID.
    offlen(kF52, off, len);
    Run d {}, w {}; run_new(kF52.cat, off, len, 2u, 0u, d); run_new(kF52.cat, off, len, 2u, N48_MIB_XIB_MASK, w);
    n48_mib_xib_note(&x, n48_mib_start_class(&kF52.cat[0], len[0]), w.ns, w.lm, w.ibn[0], d.ns, d.lm, d.ibn[0], 0u);
    expect_u("C4 F52 OFF: WOULD lead 1, rescued 1; DID nothing",
             (x.runs == 1u && x.w_lead == 1u && x.w_rescued == 1u && x.w_disg0 == 0u && x.d_lead == 0u && x.d_rescued == 0u) ? 1u : 0u, 1u);
    // F20 with switch 69 M 3: WOULD and DID both count the IB-0 disguise, the lead and the rescue.
    offlen(kF20, off, len);
    run_new(kF20.cat, off, len, 2u, N48_MIB_XIB_MASK, w);
    n48_mib_xib_note(&x, n48_mib_start_class(&kF20.cat[0], len[0]), w.ns, w.lm, w.ibn[0], w.ns, w.lm, w.ibn[0], N48_MIB_XIB_MASK);
    expect_u("C4 F20 M 3: WOULD disg0 1 lead 2 rescued 2; DID disg0 1 lead 1 rescued 1",
             (x.runs == 2u && x.w_disg0 == 1u && x.w_lead == 2u && x.w_rescued == 2u && x.d_disg0 == 1u && x.d_lead == 1u &&
              x.d_rescued == 1u) ? 1u : 0u, 1u);
    // F98 with switch 69 M 2 (the lead only): the real stage answers 0; the predicate rescues it by the IB-0 disguise.
    offlen(kF98, off, len);
    run_new(kF98.cat, off, len, 2u, N48_MIB_XIB_LEAD, d); run_new(kF98.cat, off, len, 2u, N48_MIB_XIB_MASK, w);
    n48_mib_xib_note(&x, n48_mib_start_class(&kF98.cat[0], len[0]), w.ns, w.lm, w.ibn[0], d.ns, d.lm, d.ibn[0], N48_MIB_XIB_LEAD);
    expect_u("C4 F98 M 2: WOULD disg0 +1 rescued +1; DID unchanged",
             (x.w_disg0 == 2u && x.w_rescued == 3u && x.d_disg0 == 1u && x.d_rescued == 1u) ? 1u : 0u, 1u);
    n48_mib_xib wide {};
    std::memset(&wide, 0xFF, sizeof wide);
    char line[600];
    const int n = std::snprintf(line, sizeof line, N48_XIB_FMT, n48_mib_xib_state(3u), " - `gfxneuter 69` REFUSED: a continuous arm stands, unchanged", N48_XIB_ARGS(&wide));
    std::printf("      xib69 line at widest: %d bytes\n", n);
    expect_u("C4: the xib69 line fits the logger's 491-byte body at widest", (n > 0 && n < 491) ? 1u : 0u, 1u);
}

// ---- the kext's wiring (source pins, with order where order matters) ----
static void pins(const char *ahh)
{
    if (!ahh) { std::printf("  SKIP 0.0.505 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *s) { uint32_t c = 0; for (size_t p = src.find(s); p != std::string::npos; p = src.find(s, p + 1)) c++; return c; };
    const char *latch = "gXdBuild.xib = gXibOn & N48_MIB_XIB_MASK; gXdBuild.leadMask = 0u;";
    const char *call  = "gXdBuild.ib_nseg, &tot, &mibseg, gXdBuild.xib, &leadMask);";
    const char *keep  = "gXdBuild.leadMask = ns ? leadMask : 0u;";
    const char *cens  = "gfxsrc_xib_census(f->nib < N48_XV_MAX_IBS ? f->nib : N48_XV_MAX_IBS, ns, leadMask);";
    const char *units = "gXdBuild.mib ? gXdBuild.leadMask : 0u);";
    const char *gate  = "c.lead_mask = gXdBuild.leadMask;";
    const char *reset = "gXdBuild.leadMask = 0u;   // build 0.0.505: and switch 69's leads, for the same reason";
    const char *leads = "if (gXdBuild.leadMask) gfxsrc_xib_leads(";
    const char *rep   = "xib_report_line(\"\");";
    const char *pred  = "nullptr, N48_MIB_XIB_MASK, &wLead);";
    expect_u("PIN 505: switch 69 is latched once per pass, with the lead mask cleared", count(latch), 1u);
    expect_u("PIN 505: the segment stage gets the LATCHED switch and returns the leads", count(call), 1u);
    expect_u("PIN 505: the gate's lead mask is kept only with a non-zero answer", count(keep), 1u);
    expect_u("PIN 505: the census runs on every MIB segment-stage run", count(cens), 1u);
    expect_u("PIN 505: the census's predicate is the stage with BOTH rules into its own scratch", count(pred), 1u);
    expect_u("PIN 505: the unit stage gets the leads (C2)", count(units), 1u);
    expect_u("PIN 505: the gate gets the leads (C3)", count(gate), 1u);
    expect_u("PIN 505: the second reset site clears the leads", count(reset), 1u);
    expect_u("PIN 505: the leads' final statuses are counted", count(leads), 1u);
    expect_u("PIN 505: the xib69 line is printed on every read of the report", count(rep), 1u);
    expect_u("PIN 505: switch 69's verb exists once", count("} else if ((arg & 0xffull) == 69ull) {"), 1u);
    const size_t pL = src.find(latch), pC = src.find(call), pK = src.find(keep), pS = src.find(cens),
                 pU = src.find(units), pX = src.find("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
    expect_u("ORDER 505: latch, stage, keep, census, then the unit stage, then the translate call",
             (pL != std::string::npos && pL < pC && pC < pK && pK < pS && pS < pX && pU != std::string::npos) ? 1u : 0u, 1u);
}

static void checks(const char *ahh)
{
    std::printf("\n== build 0.0.505 (CROSS-IB.md): the IB-0 disguise and the lead segments over run10p's real frames ==\n");
    t1(); t2(); t3(); t4(); t5(); t6(); t7(); t8(); rungs(); census(); pins(ahh);
}

} // namespace xib505
