// gfx_mib_defer511_checks.h — build 0.0.511 ( (IB0), (1); gfx_unitdefer.h THE ORDERING HOLE): switch 70's
// consumer deferral and the retry augmentation, over run10t F107's REAL bytes (tests/fixture_defer511_run10t.h), in the kext's
// order. Included by gfx_mib_test.cpp (the `mib` suite: it links xlat12_ib.c and reads AppleHardwareHook.cpp for the pins).
//
// THE MODEL is the harness's (scratchpad f86/fu2.cpp, config XIB 3, PACK, 70, 56, 45, DE 3, flags 0xDFFC4), ported: the
// segment stage (n48_mib_segment, xib 3), the unit stage (n48_mib_units), the translator with the frame pool and the
// per-constituent feed, switch 56's retry, switch 70's two passes. Provenance: an ask the harness proved OUTSIDE the frame
// (ledger / resprov / grant stand-ins: kD511Led) is proven; every other ask only through the frame-local list, exactly as the
// kext's gfxsrc_desc_tiled_ok asks the list last. TEST ASSUMPTIONS (from the fixture): the uncaptured descriptor-heap slots
// (STANDIN) and frame b's uncaptured k2 slot 0x4000b0860 -> 0x401428000 (SLOTVA).
//
// `fix` 0 drives 0.0.510's calls (n48_mib_defer_take / _load, no consumer deferral, no augmentation); `fix` 1 the kext's 0.0.511
// calls (n48_mib_defer_take_prov, _take_w, _load_aug, _retry_adds, the ask notes) - the kext's wiring is pinned at the end.
#pragma once
#include <vector>
#include <string>
#include "gfx_desc_port.h"
#include "gfx_unitdefer.h"
#include "fixture_defer511_run10t.h"

namespace d511 {

static const uint64_t kCtx = 7u;
static const uint32_t kFlags = 0xDFFC4u;                        // the harness's flag word (tools/m4-xlat convention)
static const uint64_t kOwnBase = 0x400000000ull + (511ull << 28);
static n48_mib_defer gD;                                         // the kext's gUnitDefer
static n48_dl gFl;                                               // the kext's gXdFrameLocal
static xlat12_pool gPool;
static xlat12_unit gU;
static xlat12_ud_carry gCarry;
static uint32_t gFrameCtx = 107u, gMiss = 0u, gIo[2];

static int m_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const d511_mem &m : kD511Mem)
        if (m.va == va && m.ndw == ndw) { for (uint32_t k = 0; k < ndw; k++) out[k] = kD511MemW[m.off + k]; return 1; }
    gMiss++;
    return 0;
}
static int id_by_name(const char *nm)
{
    for (int i = 0; i < (int)xlat12_shader_id_count(); i++) if (!std::strcmp(xlat12_shader_id_name(i), nm)) return i;
    return -1;
}
static int m_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    for (const d511_pgm &p : kD511Pgm)
        if (p.stage == stage && p.va == va) {
            const int id = id_by_name(p.name);
            if (id < 0 || xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
            if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
            return 1;
        }
    return 0;
}
static int led(uint64_t va, uint32_t mode, uint32_t dcc)
{
    for (const d511_led &l : kD511Led) if (l.va == va && l.mode == mode && l.dcc == dcc) return 1;
    return 0;
}
// the kext's order: the ledger/resprov (here: the outside set) first, the frame-local list last; a no is the attempt's FAILED
// ask (gfxsrc_defer_ask_failed -> n48_mib_defer_ask_note)
static int m_tiled(void *, uint64_t va, uint32_t mode, uint32_t)
{
    if (led(va, mode, 0u)) return 1;
    if (n48_dl_tiled_ok(&gFl, kCtx, va, mode)) return 1;
    n48_mib_defer_ask_note(&gD, va);
    return 0;
}
static int m_dcc(void *, uint64_t va, uint32_t mode, uint32_t)
{
    if (led(va, mode, 1u)) return 1;
    if (n48_dl_dcc_ok(nullptr, &gFl, 1u, kCtx, va, mode, 0ull, 0u) != N48_DL_DCC_NONE) return 1;
    n48_mib_defer_ask_note(&gD, va);
    return 0;
}
static int m_csn(void *, uint64_t) { return 1; }
static void m_feed(void *, const uint32_t *o, uint32_t n) { (void)n48_dl_from_output(&gFl, kCtx, o, n, nullptr); }
static uint32_t m_xl(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    return xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), ex, in, n, out, olen, ds);
}
static char letter(uint32_t st, uint32_t op)
{
    if (!st) return '.';
    if (st == XLAT12_IB_ERR_DESC) return op == 0xF7u ? 'P' : op == 0xF8u ? 'M' : op == 0xF6u ? 'N' : 'D';
    if (st == XLAT12_IB_ERR_TOO_LONG) return op == 0xFFu ? 'X' : op == 0xFCu ? 'R' : 'L';
    if (st == XLAT12_IB_ERR_PAIR) return 'A';
    if (st == XLAT12_IB_ERR_VERIFY) return 'V';
    return 'Z';
}

struct Cfg {
    uint32_t on70 = 1u;                   // switch 70 latched
    uint32_t fix = 2u;                    // 2 = the kext's 0.0.512 calls (take_prov_va, n48_mib_defer_done), 1 = 0.0.511's, 0 = 0.0.510's
    uint32_t starve = 0u;                 // TEST CONDITION: pass 1 finds the pool empty (every retry is refused room again)
    uint32_t cap = N48_MIB_DEFER_MAX;     // the cap handed to the takes
};
struct Res {
    std::string s, ib0;
    uint32_t nu = 0u, provDef = 0u, roomDef = 0u, retried = 0u, laterProbed = 0u, laterProven = 0u, provCapped = 0u;
    std::vector<uint32_t> st, op, attempts, why;          // per unit: final status/op, translate calls, deferral kind (9 = none)
    std::vector<uint64_t> hash;                           // per unit: FNV of its final candidate slice
    uint32_t k2Aug = 0u, k2SnapHas = 9u, k2LoadHas = 9u, k2Wk = 99u; uint64_t k2Wva = 0ull;
};
static int fl_has(const n48_dl_ent *e, uint32_t n, uint64_t va)
{
    for (uint32_t q = 0; q < n; q++) if (e[q].va == va) return 1;
    return 0;
}

static Res run(const Cfg &cfg)
{
    Res R;
    const uint32_t nib = kD511Nib;
    uint32_t off[4] = { 0, 0, 0, 0 }, nn[4] = { 0, 0, 0, 0 }, n = 0;
    for (uint32_t k = 0; k < nib; k++) { off[k] = kD511IbOff[k]; nn[k] = kD511IbLen[k]; n += nn[k]; }
    const uint32_t *in = kD511F107;
    static xlat12_ib_segment segs[N48_XV_MAX_SEGS], orig[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS];
    static uint32_t cfirst[N48_XV_MAX_SEGS], ccount[N48_XV_MAX_SEGS];
    uint32_t ibnseg[4] = { 0, 0, 0, 0 }, tot = 0, leadMask = 0u;
    n48_mib_seg_diag dg {};
    const uint32_t ns = n48_mib_segment(in, nib, off, nn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 3u, &leadMask);
    if (!ns || ns != tot) return R;
    for (uint32_t k = 0; k < ns; k++) { orig[k] = segs[k]; cfirst[k] = k; ccount[k] = 1u; }
    const uint32_t nu = n48_mib_units(in, nib, off, nn, orig, ns, XLAT12_UNIT_CONS_MAX, units, cfirst, ccount, N48_XV_MAX_SEGS, leadMask);
    if (!nu) return R;
    R.nu = nu;
    R.st.assign(nu, 0xFFFFFFFFu); R.op.assign(nu, 0u); R.attempts.assign(nu, 0u); R.why.assign(nu, 9u); R.hash.assign(nu, 0ull);
    std::memset(&gU, 0, sizeof gU); gU.pack = 1u;
    gPool.nrun = 0; gPool.lost = 0; gPool.jn = 0;
    n48_dl_clear(&gFl);
    gCarry = xlat12_ud_carry {};
    n48_mib_defer_begin(&gD, cfg.on70);
    std::vector<uint32_t> cand(in, in + n);
    std::vector<std::vector<n48_dl_ent>> fed(nu);         // the mirror's own record: what each unit's FINAL output fed
    uint32_t segIb = 0;
    for (gD.pass = 0u; gD.pass < 2u; gD.pass++) {
        if (gD.pass) { if (!gD.n) break; segIb = 0u; if (cfg.starve) gPool.nrun = 0u; }
        for (uint32_t k = 0; k < nu; k++) {
            const bool second = gD.pass != 0u;
            uint32_t addN = 0u;
            if (second) {
                const uint32_t di = n48_mib_defer_index(&gD, k);
                if (di == N48_MIB_DEFER_NONE) continue;
                if (cfg.fix) addN = n48_mib_defer_load_aug(&gD, di, &gFl, &gCarry);
                else n48_mib_defer_load(&gD, di, &gFl, &gCarry);
                R.retried++;
                if (k == 2u) {
                    R.k2Aug = addN;
                    R.k2SnapHas = (uint32_t)fl_has(gD.snap[di].e, gD.snap[di].n, 0x401428000ull);
                    R.k2LoadHas = (uint32_t)fl_has(gFl.e, gFl.n, 0x401428000ull);
                }
                // THE FAIL-OPEN PROBE: no entry only a LATER unit (pass 0) fed - a VA no earlier unit fed - is proven here
                for (uint32_t j = k + 1u; j < nu; j++)
                    for (const n48_dl_ent &e : fed[j]) {
                        bool earlier = false;
                        for (uint32_t i = 0; i < k && !earlier; i++) for (const n48_dl_ent &f : fed[i]) earlier |= (f.va == e.va);
                        if (earlier) continue;
                        R.laterProbed++;
                        if (n48_dl_tiled_ok(&gFl, kCtx, e.va, e.mode)) R.laterProven++;
                    }
            }
            const uint32_t from = units[k].start, to = units[k].end;
            while (segIb + 1u < nib && from >= off[segIb] + nn[segIb]) segIb++;
            xlat12_draw_extra ex {};
            ex.pgm_profile = &m_profile; ex.ring_va = kOwnBase; ex.gs_sgpr0_va = kOwnBase + 0xa80000ull;
            ex.flags = kFlags & ~(uint32_t)(XLAT12_EXTRA_UD_REEMIT | XLAT12_EXTRA_VS_KNOWN | XLAT12_EXTRA_DESC_INV_APPLE_HEAD |
                                            XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_DCC_STRIP | XLAT12_EXTRA_INLINE_DESC |
                                            XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV | XLAT12_EXTRA_FILL_COLOR |
                                            XLAT12_EXTRA_CS_ELIDE);
            ex.flags |= XLAT12_EXTRA_READSET;
            ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
            ex.flags |= XLAT12_EXTRA_FILL_COLOR; ex.fill_color_va = kOwnBase + 0xA81000ull;
            ex.flags |= XLAT12_EXTRA_CS_ELIDE; ex.cs_ctx = &gFrameCtx; ex.cs_is_n = &m_csn;
            ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV | XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
            if (!n48_mib_head_executes(in, units[k].head, n)) ex.flags &= ~(uint32_t)XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
            ex.flags |= XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_UD_REEMIT | XLAT12_EXTRA_VS_KNOWN; ex.ud_carry = &gCarry;
            ex.ib_va = n48_mib_seg_va(kD511IbVa[segIb], from, off[segIb]);
            ex.desc_read = &m_read; ex.desc_tiled_ok = &m_tiled; ex.desc_dcc_ok = &m_dcc;
            ex.flags |= XLAT12_EXTRA_DCC_STRIP;
            ex.flags |= XLAT12_EXTRA_DRAW_ELIDE; ex.draw_elide_rows = 3u;
            uint32_t isUnit = n48_mib_unit_setup(&ex, &gU, &gPool, 1, n48_mib_unit_flag(ccount[k]) ? ccount[k] : 0u, &orig[cfirst[k]],
                                                 from, &m_feed, nullptr);
            if (gD.on && !second) n48_mib_defer_pre(&gD, &gFl, &gCarry);
            static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
            uint32_t olen = 0;
            uint32_t *out = &cand[from];
            R.attempts[k]++;
            n48_mib_defer_ask_clear(&gD);
            uint32_t st = m_xl(&ex, &in[from], to - from, out, &olen, &ds);
            if (n48_mib_retry_wanted(1u, 1u, 1u, 1u, ccount[k], st, ds.err_op)) {
                uint32_t rwhy = 0;
                n48_mib_defer_ask_clear(&gD);
                st = n48_mib_retry_single(&m_xl, &ex, &gU, &gPool, 1, &orig[cfirst[k]], from, &in[from], to - from, out, &olen, &ds, st, &rwhy);
                if (rwhy == N48_MIB_RETRY_OK || rwhy == N48_MIB_RETRY_REFUSED || rwhy == N48_MIB_RETRY_SENTINEL_LEFT) isUnit = 2u;
            }
            if (!second) {
                uint32_t took = 0u, prov = 0u;
                const uint32_t pc0 = gD.prov_capped;
                if (cfg.fix == 1u && n48_mib_defer_take_prov(&gD, 0u, k, 1u, st, ds.err_op, cfg.cap, &gFl, in, n, from, to)) took = prov = 1u;
                else if (cfg.fix == 2u && n48_mib_defer_take_prov_va(&gD, 0u, k, 1u, st, ds.err_op, ds.prov_va, cfg.cap, &gFl, in, n, from, to)) took = prov = 1u;
                else if (cfg.fix ? n48_mib_defer_take_w(&gD, 0u, k, 1u, isUnit, st, ds.err_op, cfg.cap, &gFl, in, n, from, to)
                                 : n48_mib_defer_take(&gD, 0u, k, 1u, isUnit, st, ds.err_op, cfg.cap, &gFl)) took = 1u;
                R.provCapped += gD.prov_capped - pc0;
                if (took) {
                    std::memcpy(out, &in[from], 4u * (to - from));
                    (void)n48_mib_unit_undo(isUnit, st, &gPool);
                    R.why[k] = prov ? N48_MIB_DEFER_WHY_PROV : N48_MIB_DEFER_WHY_ROOM;
                    if (prov) { R.provDef++; if (k == 2u) { R.k2Wk = gD.wk[gD.n - 1u]; R.k2Wva = gD.wva[gD.n - 1u]; } }
                    else R.roomDef++;
                    continue;
                }
            }
            if (st) std::memcpy(out, &in[from], 4u * (to - from));
            if (isUnit && st) xlat12_pool_undo(&gPool);
            if (!st) xlat12_pool_add_free(&gPool, out, olen, ex.ib_va);
            fed[k].clear();
            if (!st) {
                const uint32_t flFrom = isUnit ? gU.last_head_out : 0u;
                (void)n48_dl_from_output(&gFl, kCtx, out + flFrom, olen - flFrom, nullptr);
                static n48_dl mine; std::memset(&mine, 0, sizeof mine);
                (void)n48_dl_from_output(&mine, kCtx, out, olen, nullptr);
                fed[k].assign(mine.e, mine.e + mine.n);
            }
            if (cfg.fix == 1u) n48_mib_defer_retry_adds(&gD, gD.pass, &gFl, st);
            else if (cfg.fix == 2u) n48_mib_defer_done(&gD, gD.pass, k, &gFl, st, in, n, from, to);
            R.st[k] = st; R.op[k] = st ? ds.err_op : 0u;
            uint64_t h = 1469598103934665603ull;
            for (uint32_t q = from; q < to; q++) { h ^= cand[q]; h *= 1099511628211ull; }
            R.hash[k] = h;
        }
    }
    uint32_t pib = 0;
    for (uint32_t k = 0; k < nu; k++) {
        uint32_t ib = 0;
        while (ib + 1u < nib && units[k].start >= off[ib] + nn[ib]) ib++;
        if (k && ib != pib) { if (R.ib0.empty()) R.ib0 = R.s; R.s += '|'; }
        pib = ib;
        const char c = letter(R.st[k], R.op[k]);
        if (ccount[k] > 1u) { R.s += '('; R.s += std::to_string(ccount[k]); R.s += c; R.s += ')'; } else R.s += c;
    }
    return R;
}

static std::string read_src(const char *p)
{
    std::string s;
    FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, r); std::fclose(f); }
    return s;
}

static void checks(const char *ahh)
{
    std::printf("\n== 0.0.511: switch 70's ordering hole (consumer deferral + retry augmentation) over run10t F107's real bytes ==\n");
    Cfg c0; c0.fix = 0u;
    const Res A = run(c0);
    Cfg c1; c1.fix = 1u;
    const Res B = run(c1);
    std::printf("      F107 70 ON: 0.0.510 %s | 0.0.511 %s (reads missed %u)\n", A.s.c_str(), B.s.c_str(), gMiss);
    expect_u("511 D1 (0.0.510's calls): F107 IB0 is `.(6.)P.` - k1 deferred for room and translated at its retry, k2 refused PROVENANCE",
             (A.ib0 == ".(6.)P." && A.nu == 12u && A.roomDef == 1u && A.provDef == 0u && A.why[1] == N48_MIB_DEFER_WHY_ROOM &&
              A.st[1] == 0u && A.st[2] == (uint32_t)XLAT12_IB_ERR_DESC && A.op[2] == (uint32_t)XLAT12_TDESC_PROVENANCE) ? 1u : 0u, 1u);
    expect_u("511 D2 (the kext's 0.0.511 calls): F107 IB0 is `.(6.)..` - k2 TRANSLATES", (B.ib0 == ".(6.).." && B.st[2] == 0u) ? 1u : 0u, 1u);
    expect_u("511 D2 ... k2 was deferred as a CONSUMER of k1's writes (0x401428000) and retried once, after k1",
             (B.provDef == 1u && B.roomDef == 1u && B.why[2] == N48_MIB_DEFER_WHY_PROV && B.k2Wk == 1u && B.k2Wva == 0x401428000ull &&
              B.attempts[2] == 2u && B.attempts[1] == 2u && B.retried == 2u) ? 1u : 0u, 1u);
    expect_u("511 D2 ... PROVED FROM k1's RETRY: 0x401428000 is not in k2's own snapshot, it is in the list k2's retry was handed "
             "(an earlier-retry entry)", (B.k2SnapHas == 0u && B.k2LoadHas == 1u && B.k2Aug >= 1u) ? 1u : 0u, 1u);
    {
        uint32_t diff = 0u;
        for (uint32_t k = 0; k < B.nu && k < A.nu; k++) if (k != 2u && (A.st[k] != B.st[k] || A.hash[k] != B.hash[k])) diff++;
        expect_u("511 D3 NO OTHER CHANGE: every unit but k2 keeps 0.0.510's status and candidate bytes", diff, 0u);
        expect_u("511 D3 a consumer of a surface NO deferred unit writes is not deferred: IB1's k11 (PROVENANCE on 0x402380000) "
                 "keeps its refusal, translated once", (B.st[11] == (uint32_t)XLAT12_IB_ERR_DESC && B.op[11] == 0xF7u &&
                 B.why[11] == 9u && B.attempts[11] == 1u) ? 1u : 0u, 1u);
    }
    expect_u("511 D5 FAIL-OPEN PROBE: entries only a LATER unit fed are probed at the retries and NONE is proven by a retry's list",
             (B.laterProbed > 0u && B.laterProven == 0u) ? 1u : 0u, 1u);
    {
        Cfg cs; cs.starve = 1u; cs.fix = 1u;
        const Res S = run(cs);
        std::printf("      F107 starved pass 1: %s\n", S.s.c_str());
        expect_u("511 D4 FAIL-CLOSED: when k1's retry is itself refused (no room at the end), k2 is deferred, retried with nothing "
                 "added, and STAYS refused PROVENANCE", (S.ib0 == ".(6M)P." && S.why[2] == N48_MIB_DEFER_WHY_PROV && S.k2Aug == 0u &&
                 S.st[2] == (uint32_t)XLAT12_IB_ERR_DESC && S.op[2] == 0xF7u) ? 1u : 0u, 1u);
    }
    {
        Cfg a0; a0.on70 = 0u; a0.fix = 0u; Cfg b0 = a0; b0.fix = 2u;   /* 0.0.512: the newest calls, 70 OFF */
        const Res X = run(a0), Y = run(b0);
        uint32_t diff = X.s != Y.s ? 1u : 0u;
        for (uint32_t k = 0; k < X.nu; k++) diff += (X.st[k] != Y.st[k] || X.hash[k] != Y.hash[k]) ? 1u : 0u;
        expect_u("511 D6 OFF IDENTITY: 70 OFF - nothing deferred, and the 0.0.511 calls give 0.0.510's frame byte for byte",
                 (diff == 0u && Y.provDef + Y.roomDef + Y.retried == 0u && gD.nwall == 0u && gD.nask == 0u) ? 1u : 0u, 1u);
    }
    {
        Cfg cc; cc.cap = 1u; cc.fix = 1u;
        const Res C = run(cc);
        expect_u("511 D7 CAP: with a cap of 1 k1 takes the one deferral; k2 (a consumer) is counted prov_capped and stays refused",
                 (C.roomDef == 1u && C.provDef == 0u && C.provCapped == 1u && C.st[2] == (uint32_t)XLAT12_IB_ERR_DESC) ? 1u : 0u, 1u);
    }
    // ---- the pure steps ----
    {
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        n48_mib_defer_begin(&d, 1u);
        // snapshot: X (tok 1); earlier retries fed X differently (tok 9) and Y
        fl.e[0].va = 0x5000000000ull; fl.e[0].mode = 3u; fl.e[0].tok = 1u; fl.n = 1u;
        n48_mib_defer_pre(&d, &fl, &c);
        (void)n48_mib_defer_take(&d, 0u, 4u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, N48_MIB_DEFER_MAX, &fl);
        d.adds[0] = fl.e[0]; d.adds[0].tok = 9u; d.adds[0].mode = 2u;
        d.adds[1].va = 0x5100000000ull; d.adds[1].mode = 3u; d.nadds = 2u;
        const uint32_t added = n48_mib_defer_load_aug(&d, 0u, &fl, &c);
        /* build 0.0.512 (C1): the snapshot's X was written by a unit BEFORE the earlier retry that re-fed X (tok 9) - its
         * entry is AMBIGUOUS now: dropped, and the retry's entry is not added either (fail-closed; 0.0.511 kept the older one) */
        expect_u("512 D8 AUGMENTATION NEVER REPLACES, AND AN AMBIGUOUS VA PROVES NOTHING: the snapshot's X (an earlier retry re-fed it) "
                 "is dropped, the retry's X is not added; only the VA nobody else touched (Y) is added",
                 (added == 1u && fl.n == 1u && fl.e[0].va == 0x5100000000ull && d.conflicts == 1u && d.loaded.n == 1u) ? 1u : 0u, 1u);
        // ORDERING: a pass-0 unit's feeds (the GPU runs it AFTER the deferred units) never become an augmentation
        fl.e[fl.n].va = 0x5200000000ull; fl.e[fl.n].mode = 3u; fl.n++;
        const uint32_t n0 = d.nadds;
        n48_mib_defer_retry_adds(&d, 0u, &fl, 0u);
        const uint32_t afterPass0 = d.nadds - n0;
        n48_mib_defer_retry_adds(&d, 1u, &fl, 1u);
        const uint32_t afterRefused = d.nadds - n0;
        n48_mib_defer_retry_adds(&d, 1u, &fl, 0u);
        expect_u("511 D8 ORDERING: pass 0 (a LATER unit) adds nothing; a refused retry adds nothing; a translated retry adds exactly "
                 "what it gained over its handed list", (afterPass0 == 0u && afterRefused == 0u && d.nadds - n0 == 1u &&
                 d.adds[d.nadds - 1u].va == 0x5200000000ull) ? 1u : 0u, 1u);
    }
    {
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        // a SET_CONTEXT_REG writing CB_COLOR0_BASE (0xA318) = 0x04014280 and DB_Z_WRITE_BASE (0xA012) = 0x04000100, then NOPs
        const uint32_t ib[12] = { 0xC0016900u, 0x318u, 0x04014280u, 0xC0016900u, 0x012u, 0x04000100u, 0xFFFF1000u, 0xFFFF1000u,
                                  0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u };
        n48_mib_defer_begin(&d, 1u);
        n48_mib_defer_pre(&d, &fl, &c);
        // unit 0: a consumer asking 0x401428000 BEFORE anything is deferred - not deferred
        n48_mib_defer_ask_note(&d, 0x401428000ull);
        const uint32_t early = n48_mib_defer_take_prov(&d, 0u, 0u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 4u, &fl, ib, 12u, 0u, 12u);
        // unit 1: deferred for room; its write set is recorded from its input
        const uint32_t room = n48_mib_defer_take_w(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, ib, 12u, 0u, 12u);
        n48_mib_defer_ask_clear(&d); n48_mib_defer_ask_note(&d, 0x7700000000ull);
        const uint32_t other = n48_mib_defer_take_prov(&d, 0u, 2u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 4u, &fl, ib, 12u, 6u, 12u);
        n48_mib_defer_ask_clear(&d); n48_mib_defer_ask_note(&d, 0x401428000ull);
        const uint32_t notProv = n48_mib_defer_take_prov(&d, 0u, 3u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, ib, 12u, 6u, 12u);
        const uint32_t pass1 = n48_mib_defer_take_prov(&d, 1u, 3u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 4u, &fl, ib, 12u, 6u, 12u);
        const uint32_t cons = n48_mib_defer_take_prov(&d, 0u, 3u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 4u, &fl, ib, 12u, 6u, 12u);
        expect_u("511 D9 the consumer rule: only PROVENANCE, only pass 0, only a failed ask in the write set of an EARLIER deferred "
                 "unit (CB0 and DB bases read from its input); the consumer records the writer",
                 (early == 0u && room == 1u && d.nwall == 2u && other == 0u && notProv == 0u && pass1 == 0u && cons == 1u &&
                  d.why[1] == N48_MIB_DEFER_WHY_PROV && d.wk[1] == 1u && d.wva[1] == 0x401428000ull) ? 1u : 0u, 1u);
        n48_mib_defer_begin(&d, 0u);
        n48_mib_defer_ask_note(&d, 0x401428000ull);
        expect_u("511 D9 70 OFF: no ask is noted, nothing can be deferred", (d.nask == 0u &&
                 n48_mib_defer_take_prov(&d, 0u, 3u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 4u, &fl, ib, 12u, 0u, 12u) == 0u) ? 1u : 0u, 1u);
    }
    // ---- build 0.0.512 (C1-C3, the 0.0.511 review's MEDIUM-1/MEDIUM-2/LOW-1) ----
    {
        Cfg c2;   // the kext's 0.0.512 calls
        const Res Z = run(c2);
        Cfg c1b; c1b.fix = 1u;
        const Res Y = run(c1b);
        uint32_t diff = Z.s != Y.s ? 1u : 0u;
        for (uint32_t k = 0; k < Z.nu && k < Y.nu; k++) diff += (Z.st[k] != Y.st[k] || Z.hash[k] != Y.hash[k]) ? 1u : 0u;
        std::printf("      F107 70 ON, 0.0.512 calls: %s (0.0.511: %s)\n", Z.s.c_str(), Y.s.c_str());
        expect_u("512 D10 the real F107: the 0.0.512 calls keep 0.0.511's frame (`.(6.)..` IB0, k2 a consumer of k1, every unit's status "
                 "and bytes)", (diff == 0u && Z.ib0 == ".(6.).." && Z.why[2] == N48_MIB_DEFER_WHY_PROV && Z.st[2] == 0u) ? 1u : 0u, 1u);
        expect_u("512 D10 ... k2 still PROVED FROM k1's RETRY (0x401428000 absent from its snapshot, present in its handed list)",
                 (Z.k2SnapHas == 0u && Z.k2LoadHas == 1u && Z.k2Aug >= 1u) ? 1u : 0u, 1u);
        expect_u("512 D10 FAIL-OPEN PROBE with the 0.0.512 calls: nothing only a LATER unit fed is proven at a retry",
                 (Z.laterProbed > 0u && Z.laterProven == 0u) ? 1u : 0u, 1u);
    }
    {   // C1 (i)+(ii): kA -> X(A), kB -> X(B) (a retry that changed what it was handed), k3 reads X as A: REFUSED
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        const uint64_t X = 0x5000000000ull; const uint32_t mA = 3u, mB = 2u;
        n48_mib_defer_begin(&d, 1u);
        // pass 0: kA (unit 1), kB (unit 2), k3 (unit 3) all deferred for room (their inputs name nothing: the list says it all)
        for (uint32_t k = 1; k <= 3u; k++) {
            n48_mib_defer_pre(&d, &fl, &c);
            (void)n48_mib_defer_take_w(&d, 0u, k, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, nullptr, 0u, 0u, 0u);
        }
        d.pass = 1u;
        // kA's retry feeds X(A)
        (void)n48_mib_defer_load_aug(&d, 0u, &fl, &c);
        fl.e[fl.n].ctx = 5u; fl.e[fl.n].va = X; fl.e[fl.n].mode = mA; fl.e[fl.n].tok = 1u; fl.n++;
        n48_mib_defer_done(&d, 1u, 1u, &fl, 0u, nullptr, 0u, 0u, 0u);
        // kB's retry is handed X(A), re-feeds X as B (the differing re-feed replaces it)
        const uint32_t augB = n48_mib_defer_load_aug(&d, 1u, &fl, &c);
        const uint32_t bHasA = n48_dl_tiled_ok(&fl, 5u, X, mA) ? 1u : 0u;
        for (uint32_t q = 0; q < fl.n; q++) if (fl.e[q].va == X) { fl.e[q].mode = mB; fl.e[q].tok = 2u; }
        n48_mib_defer_done(&d, 1u, 2u, &fl, 0u, nullptr, 0u, 0u, 0u);
        // k3's retry
        (void)n48_mib_defer_load_aug(&d, 2u, &fl, &c);
        const uint32_t k3A = n48_dl_tiled_ok(&fl, 5u, X, mA) ? 1u : 0u, k3B = n48_dl_tiled_ok(&fl, 5u, X, mB) ? 1u : 0u;
        uint32_t addsX = 0u; for (uint32_t a = 0; a < d.nadds; a++) if (d.adds[a].va == X) addsX++;
        expect_u("512 C1 kA->X(A), kB->X(B), k3 reads X as A: REFUSED; X as B (the LATEST writer) proven; adds[] holds ONE X (kB's)",
                 (augB == 1u && bHasA == 1u && k3A == 0u && k3B == 1u && addsX == 1u) ? 1u : 0u, 1u);
    }
    {   // C1 (iii): kA -> X; a pass-0 unit between (kp) re-feeds X, kmid drops it; k3 reads X: NOT proven by kA's X
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        const uint64_t X = 0x5300000000ull;
        n48_mib_defer_begin(&d, 1u);
        n48_mib_defer_pre(&d, &fl, &c);   // kA (unit 1) deferred
        (void)n48_mib_defer_take_w(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, nullptr, 0u, 0u, 0u);
        n48_mib_defer_pre(&d, &fl, &c);   // kp (unit 2) translated in pass 0: feeds X(p)
        fl.e[fl.n].ctx = 5u; fl.e[fl.n].va = X; fl.e[fl.n].mode = 1u; fl.e[fl.n].tok = 7u; fl.n++;
        n48_mib_defer_done(&d, 0u, 2u, &fl, 0u, nullptr, 0u, 0u, 0u);
        n48_mib_defer_pre(&d, &fl, &c);   // kmid (unit 3) translated in pass 0: a differing re-feed DROPS X
        fl.n = 0u;
        n48_mib_defer_done(&d, 0u, 3u, &fl, 0u, nullptr, 0u, 0u, 0u);
        n48_mib_defer_pre(&d, &fl, &c);   // k3 (unit 4) deferred for room: its snapshot lacks X
        (void)n48_mib_defer_take_w(&d, 0u, 4u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, nullptr, 0u, 0u, 0u);
        d.pass = 1u;
        (void)n48_mib_defer_load_aug(&d, 0u, &fl, &c);   // kA's retry writes X(A)
        fl.e[fl.n].ctx = 5u; fl.e[fl.n].va = X; fl.e[fl.n].mode = 3u; fl.e[fl.n].tok = 1u; fl.n++;
        n48_mib_defer_done(&d, 1u, 1u, &fl, 0u, nullptr, 0u, 0u, 0u);
        const uint32_t aug = n48_mib_defer_load_aug(&d, 1u, &fl, &c);   // k3's retry
        const uint32_t k3A = n48_dl_tiled_ok(&fl, 5u, X, 3u) ? 1u : 0u;
        uint32_t kills = 0u; for (uint32_t q = 0; q < d.nkill; q++) if (d.kill_va[q] == X && d.kill_kind[q] == N48_MIB_KILL_PASS0) kills++;
        expect_u("512 C1 kA->X, pass-0 units between re-feed and DROP X, k3 reads X: NOT proven by kA's X (not resurrected); the "
                 "pass-0 changes are in the kill set", (k3A == 0u && aug == 0u && kills == 2u) ? 1u : 0u, 1u);
    }
    {   // C2: k0 wrote X, kA (deferred) rewrites X in another format, k2 reads X (k0's format): not proven by k0's entry; deferred; retried
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        const uint64_t X = 0x401428000ull;
        const uint32_t ibA[3] = { 0xC0016900u, 0x318u, (uint32_t)(X >> 8) };   // kA's input: CB_COLOR0_BASE = X
        n48_mib_defer_begin(&d, 1u);
        fl.e[0].ctx = 5u; fl.e[0].va = X; fl.e[0].mode = 3u; fl.e[0].tok = 1u; fl.n = 1u;   // k0 (translated) fed X(mode 3)
        n48_mib_defer_pre(&d, &fl, &c);
        const uint32_t tA = n48_mib_defer_take_w(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, ibA, 3u, 0u, 3u);
        // k2's pass-0 attempt asks X in k0's format
        n48_mib_defer_pre(&d, &fl, &c);
        const uint32_t k2proven = n48_dl_tiled_ok(&fl, 5u, X, 3u) ? 1u : 0u;
        const uint32_t tC = n48_mib_defer_take_prov_va(&d, 0u, 2u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, X, 4u, &fl, nullptr, 0u, 0u, 0u);
        d.pass = 1u;
        (void)n48_mib_defer_load_aug(&d, 0u, &fl, &c);   // kA's retry: its own snapshot (X mode 3), re-feeds X in mode 2
        const uint32_t kASees = n48_dl_tiled_ok(&fl, 5u, X, 3u) ? 1u : 0u;
        for (uint32_t q = 0; q < fl.n; q++) if (fl.e[q].va == X) { fl.e[q].mode = 2u; fl.e[q].tok = 2u; }
        n48_mib_defer_done(&d, 1u, 1u, &fl, 0u, nullptr, 0u, 0u, 0u);
        (void)n48_mib_defer_load_aug(&d, 1u, &fl, &c);   // k2's retry
        const uint32_t k2Old = n48_dl_tiled_ok(&fl, 5u, X, 3u) ? 1u : 0u, k2New = n48_dl_tiled_ok(&fl, 5u, X, 2u) ? 1u : 0u;
        expect_u("512 C2 k0 wrote X, kA (deferred) rewrites X: a later pass-0 reader of X is NOT proven by k0's entry (dropped at kA's "
                 "deferral), is deferred as kA's consumer, and at its retry X proves only in kA's new format",
                 (tA == 1u && d.dropped_c2 == 1u && k2proven == 0u && tC == 1u && d.why[1] == N48_MIB_DEFER_WHY_PROV && kASees == 1u &&
                  k2Old == 0u && k2New == 1u) ? 1u : 0u, 1u);
    }
    {   // C3: the consumer rule keys on the REFUSING VA; an unrelated failed ask on a write-set VA defers nothing
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        const uint32_t ib[3] = { 0xC0016900u, 0x318u, 0x04014280u };
        n48_mib_defer_begin(&d, 1u);
        n48_mib_defer_pre(&d, &fl, &c);
        (void)n48_mib_defer_take_w(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, ib, 3u, 0u, 3u);
        n48_mib_defer_ask_clear(&d); n48_mib_defer_ask_note(&d, 0x401428000ull);   // a failed ask on the write set ...
        const uint32_t other = n48_mib_defer_take_prov_va(&d, 0u, 2u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 0x7700000000ull, 4u, &fl, nullptr, 0u, 0u, 0u);
        const uint32_t none = n48_mib_defer_take_prov_va(&d, 0u, 2u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 0ull, 4u, &fl, nullptr, 0u, 0u, 0u);
        const uint32_t hit = n48_mib_defer_take_prov_va(&d, 0u, 2u, 1u, XLAT12_IB_ERR_DESC, 0xF7u, 0x401428000ull, 4u, &fl, nullptr, 0u, 0u, 0u);
        expect_u("512 C3 ... but the refusal on ANOTHER surface (or none named) is final; the refusal ON the write set is a consumer",
                 (other == 0u && none == 0u && hit == 1u && d.wva[1] == 0x401428000ull && d.wk[1] == 1u) ? 1u : 0u, 1u);
    }
    {   // the kill set full: a retry is handed NOTHING of the frame-local list (fail-closed)
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        n48_mib_defer_begin(&d, 1u);
        fl.e[0].ctx = 5u; fl.e[0].va = 0x5500000000ull; fl.e[0].mode = 3u; fl.n = 1u;
        n48_mib_defer_pre(&d, &fl, &c);
        (void)n48_mib_defer_take_w(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 4u, &fl, nullptr, 0u, 0u, 0u);
        for (uint32_t q = 0; q <= N48_MIB_DEFER_KILL_MAX; q++) n48_mib_defer_kill_add(&d, 0x6000000000ull + 0x1000ull * q, 0u, N48_MIB_KILL_PASS0);
        d.pass = 1u;
        (void)n48_mib_defer_load_aug(&d, 0u, &fl, &c);
        expect_u("512 C1 KILL SET FULL: the retry's list is EMPTY (its snapshot entry dropped), counted", (d.kill_over == 1u && fl.n == 0u &&
                 d.loaded.n == 0u) ? 1u : 0u, 1u);
        char line[700];
        const int w5 = std::snprintf(line, sizeof line, N48_DEFER70C_FMT, ~0ull, ~0ull, ~0ull);
        expect_u("512: the kill-set defer70 line fits the logger's 491-byte body at its widest", (w5 > 0 && w5 < 491) ? 1u : 0u, 1u);
    }
    {
        char line[700];
        const int w1 = std::snprintf(line, sizeof line, N48_DEFER70B_FMT, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        const int w2 = std::snprintf(line, sizeof line, N48_DEFER70_PROV_FMT, ~0ull, 4294967295u, ~0ull, 4294967295u, 16u);
        const int w3 = std::snprintf(line, sizeof line, N48_DEFER70_RETRY_FMT, ~0ull, 4294967295u, "provenance", 4294967295u, "REFUSED", 0xffffffffu, 16u);
        const int w4 = std::snprintf(line, sizeof line, N48_DEFER70_ROOM_FMT, ~0ull, 4294967295u, 16u);
        expect_u("511: the four defer70 lines fit the logger's 491-byte body at their widest",
                 (w1 > 0 && w1 < 491 && w2 > 0 && w2 < 491 && w3 > 0 && w3 < 491 && w4 > 0 && w4 < 491) ? 1u : 0u, 1u);
    }
    // ---- THE KEXT's WIRING (reachability and order over the real AppleHardwareHook.cpp) ----
    if (!ahh) { std::printf("  SKIP 0.0.511 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    const std::string src = read_src(ahh);
    auto count = [&](const char *t) { uint32_t c = 0; for (size_t p = src.find(t); p != std::string::npos; p = src.find(t, p + 1)) c++; return c; };
    const size_t np = std::string::npos;
    auto fn = [&](const char *head) { const size_t a = src.find(head); if (a == np) return std::string(); const size_t e = src.find("\n}\n", a); return src.substr(a, e == np ? np : e - a); };
    const std::string tk = fn("static __attribute__((noinline)) uint32_t gfxsrc_defer_take(");
    const std::string ld = fn("static __attribute__((noinline)) uint32_t gfxsrc_defer_load(uint32_t k)");
    const std::string dn = fn("static __attribute__((noinline)) void gfxsrc_defer_done(uint32_t k, uint32_t from, uint32_t to, uint32_t st, uint32_t op)");
    const std::string ti = fn("static int gfxsrc_desc_tiled_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, const uint32_t *t10, uint32_t *clamp) {");
    const std::string dc = fn("static int gfxsrc_desc_dcc_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes) {");
    const std::string rt = fn("static __attribute__((noinline)) uint32_t gfxsrc_unit_retry(");
    const size_t tP = tk.find("if (n48_mib_defer_take_prov_va(&gUnitDefer, gUnitDefer.pass, k, unitMap, st, op, provVa, N48_MIB_DEFER_MAX, &gXdFrameLocal,");   // 0.0.512 C3
    const size_t tW = tk.find("} else if (!n48_mib_defer_take_w(&gUnitDefer, gUnitDefer.pass, k, unitMap, isUnit, st, op, N48_MIB_DEFER_MAX, &gXdFrameLocal,");
    const size_t tR = tk.find("if (build) memcpy(&gXdNew[from], &gXdIb[from], (size_t)(to - from) * 4u);");
    expect_u("PIN 511: gfxsrc_defer_take asks the CONSUMER rule first, then the room deferral with its write set, then restores Apple's bytes",
             (!tk.empty() && tP != np && tW != np && tR != np && tP < tW && tW < tR && tk.find("n48_mib_defer_take(") == np &&
              tk.find("n48_mib_defer_take_prov(") == np) ? 1u : 0u, 1u);
    expect_u("PIN 511: the load helper hands the retry its snapshot AUGMENTED (n48_mib_defer_load_aug), not the bare snapshot",
             (!ld.empty() && ld.find("gUnitDeferAug = n48_mib_defer_load_aug(&gUnitDefer, di, &gXdFrameLocal, &gPolicyUdCarry);") != np &&
              ld.find("n48_mib_defer_load(") == np) ? 1u : 0u, 1u);
    expect_u("PIN 512: the done helper records the kill set and hands a translated retry's new entries on (n48_mib_defer_done, both "
             "passes), before its pass-1 counting",
             (!dn.empty() && dn.find("n48_mib_defer_done(&gUnitDefer, gUnitDefer.pass, k, &gXdFrameLocal, st, gXdIb, gXdBuild.n, from, to);") != np &&
              dn.find("n48_mib_defer_done(") < dn.find("if (!gUnitDefer.pass) return;") && dn.find("n48_mib_defer_retry_adds(") == np) ? 1u : 0u, 1u);
    expect_u("PIN 511: both ask callbacks note a FAILED ask (tiled: after every source said no; dcc: the NONE answer)",
             (ti.find("    if (gXdFrameLocalOn && n48_dl_tiled_ok(&gXdFrameLocal, c->ctx, va, mode)) return 1;\n    gfxsrc_defer_ask_failed(va);") != np &&
              dc.find("else gfxsrc_defer_ask_failed(va);") != np &&
              count("static void gfxsrc_defer_ask_failed(uint64_t va) { n48_mib_defer_ask_note(&gUnitDefer, va); }") == 1u) ? 1u : 0u, 1u);
    {
        const size_t pClr = src.find("        n48_mib_defer_ask_clear(&gUnitDefer);   // build 0.0.511 (switch 70): this attempt's failed asks only\n"
                                     "        uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
        const size_t pTake = src.find("        if (gUnitDefer.on && gfxsrc_defer_take(k, from, to, build, unitMap, isUnit, st, ds.err_op, ds.prov_va)) continue;");
        const size_t pFl = src.find("n48_dl_from_output(&gXdFrameLocal, dctxKey, out + flFrom, olen - flFrom, nullptr)");
        const size_t pDone = src.find("        if (gUnitDefer.on) gfxsrc_defer_done(k, from, to, st, ds.err_op);");
        expect_u("ORDER 511: the ask clear right before the translate < the take < the frame-local feed < done (every segment)",
                 (pClr != np && pTake != np && pFl != np && pDone != np && pClr < pTake && pTake < pFl && pFl < pDone) ? 1u : 0u, 1u);
        const size_t rC = rt.find("n48_mib_defer_ask_clear(&gUnitDefer);"), rS = rt.find("const uint32_t st = n48_mib_retry_single(");
        expect_u("ORDER 511: switch 56's retry clears the first attempt's failed asks before it translates again",
                 (rC != np && rS != np && rC < rS) ? 1u : 0u, 1u);
    }
    expect_u("PIN 511: bare 70 prints the ordering counts line", count("HWLOG(N48_DEFER70B_FMT,"), 1u);
    expect_u("PIN 512: ... and the kill-set counts line", count("HWLOG(N48_DEFER70C_FMT,"), 1u);
}

}   // namespace d511
