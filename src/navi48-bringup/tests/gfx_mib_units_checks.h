// gfx_mib_units_checks.h — build 0.0.480 (notes/design/CONTINUATION-UNITS.md Q10/Q11): THE CONTINUATION-UNIT CHECKS,
// included by gfx_mib_test.cpp (so they run in the `gfx_mib` suite, which already links xlat12_ib.c and reads
// AppleHardwareHook.cpp for its pins). Everything here drives the REAL code in the KEXT'S ORDER over decide44's real bytes
// (tests/fixture_units_decide44.h): the segment stage (n48_mib_segment / xlat12_ib_segments), the unit stage
// (n48_mib_units), the policy decision (n48_mib_unit_flag, n48_mib_head_executes), the translator
// (xlat12_ib_translate_draw_ex with XLAT12_EXTRA_UNIT, the frame pool, the per-constituent feed into a real n48_dl), the
// fence slice (n48_f828_find over the final constituent) and the COMMIT gate's segment rungs (n48_cm_gate). The ten planted
// breaks of the design's Q10 are made in the REAL files by the builder (not here) and each must turn a check below FAIL.
#pragma once
#include <vector>
#include <string>
#include "gfx_desc_port.h"
#include "gfx_fillset.h"     // build 0.0.481 (F2): n48_fs_identify_fill, the one-segment fill rule
#include "gfx_cp_build.h"    // build 0.0.481 (F2): n48_cp_build_input_free, the same rule
#include "gfx_rasterarm.h"   // build 0.0.501: n48_ra_set, the verb values switch 67 sets through
#include "gfx_unitdefer.h"   // build 0.0.506: switch 70, the deferred room retry (the kext's own pure steps)
#include "fixture_units_decide44.h"

namespace u480 {

static int gPrint = 0;
static uint32_t gMiss = 0;
static int u_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const UMem &m : kUMem)
        if (m.va == va && m.ndw == ndw) { for (uint32_t k = 0; k < ndw; k++) out[k] = kUMemW[m.off + k]; return 1; }
    gMiss++;
    return 0;
}
// provenance: 0 = modelled PROVEN (the design's census convention); 1 = LIST-BACKED for the one class design Q4 names: an
// ask, inside a unit, for a surface an EARLIER CONSTITUENT of that unit drew into (Apple's own CB0 writes in that
// constituent's input, gfx10 CB_COLOR0_BASE 0x28c60 / BASE_EXT 0x28e40) is proven only when the frame-local list holds it
// - which separately translated segments had, because the kext fed each segment's output before the next one asked;
// every other ask is the cross-frame ledger's (proven here).
static int gProvMode = 0;
static n48_dl gFl;
static n48_mib_defer gD;   // build 0.0.511: the kext's gUnitDefer (was run_frame's local static): the ask callback notes into it
static std::vector<uint64_t> gFrameTargets;
static std::vector<std::vector<uint64_t>> gConsTargets;   // per constituent of the unit being translated
static struct xlat12_unit_s *gCurUnit = nullptr;
static uint32_t gAsks = 0, gListProven = 0, gListAsks = 0;
static int u_tiled(void *, uint64_t va, uint32_t mode, uint32_t)
{
    gAsks++;
    if (gProvMode == 0 || !gCurUnit || !gCurUnit->cons_seen) return 1;
    const uint32_t j = gCurUnit->cons_seen - 1u;   // the constituent the asking draw is in
    bool earlier = false;
    for (uint32_t q = 0; q < j && q < gConsTargets.size(); q++) for (uint64_t t : gConsTargets[q]) earlier |= (t == va);
    if (!earlier) return 1;
    gListAsks++;
    const int ok = n48_dl_tiled_ok(&gFl, 5u, va, mode) ? 1 : 0;
    gListProven += (uint32_t)ok;
    if (!ok) n48_mib_defer_ask_note(&gD, va);   // build 0.0.511: the kext's gfxsrc_defer_ask_failed
    return ok;
}
static void frame_targets(const uint32_t *w, uint32_t n)
{
    gFrameTargets.clear();
    uint32_t base = 0, ext = 0, have = 0;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t h = w[i];
        if (h == XLAT12_IB_NOP || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u, op = (h >> 8) & 0xFFu;
        if (op == 0x69u && i + l <= n)
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t a = 0x28000u + 4u * ((w[i + 1] & 0xFFFFu) + k);
                if (a == 0x28c60u) { base = w[i + 2 + k]; have |= 1u; }
                if (a == 0x28e40u) { ext = w[i + 2 + k]; have |= 2u; }
            }
        if ((op == 0x2Du || op == 0x27u || op == 0x35u) && have == 3u && base) {
            const uint64_t va = ((uint64_t)(ext & 0xFFu) << 40) | ((uint64_t)base << 8);
            bool dup = false; for (uint64_t t : gFrameTargets) dup |= (t == va);
            if (!dup) gFrameTargets.push_back(va);
        }
        i += l;
    }
}
static int id_by_name(const char *nm)
{
    for (int i = 0; i < (int)xlat12_shader_id_count(); i++) if (!strcmp(xlat12_shader_id_name(i), nm)) return i;
    return -1;
}
static uint32_t gIo[2];
// build 0.0.491: kDTableAbi gained rows for ws_Z_TimgXh_Isrc, ws_AO_TmuaXh_IsrcCcl_Icir,
// ws_AN_TmuaXh_Isrc_Isrc and ws_AF_variable_blur_downsample_frag_lph - four programs decide44's frames bind. With those rows
// live, their draws take the table step, and the decide44 fixture (kUMem) never recorded their table memory: the kext
// running 0.0.480's build never read it (no row), so offline those draws refuse READ 0xF4. On hardware the reads are live.
// gHide491 = 1 is a TEST-LOCAL resolver that answers exactly as it did before 0.0.491 (those four rows zeroed, the
// state they had with no row); the translator and the kext are unchanged. checks()/checks481() prove the unit, pool,
// fence, carry and retry MECHANICS on the frames as the fixture recorded them, so they run with gHide491 = 1 and their
// expectations are unchanged; checks491() runs the live rows (the new strings) and the one attribution check.
static int gHide491 = 0;
static int is_row491(uint32_t abi1)
{
    static const uint32_t k[4][2] = { { 54u, 0xf91e4deeu }, { 168u, 0xb4fc3c24u }, { 122u, 0x7b3a6dfeu }, { 173u, 0x1051f3f6u } };
    for (uint32_t i = 0; i < 4u; i++) { const uint32_t r = xlat12_table_abi_find(k[i][0], k[i][1]); if (r && r == abi1) return 1; }
    return 0;
}
static int u_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    for (const UPgm &p : kUPgm)
        if (p.stage == stage && p.va == va) {
            const int id = id_by_name(p.name);
            if (id < 0 || xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
            if (gHide491 && stage == 0u && out->ps_table_abi1 && is_row491(out->ps_table_abi1)) out->ps_table_abi1 = 0u;
            if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
            return 1;
        }
    return 0;
}
static void u_feed(void *, const uint32_t *out, uint32_t n) { (void)n48_dl_from_output(&gFl, 5u, out, n, nullptr); }

// ---- one frame through the kext's order ------------------------------------------------------------------------------
struct Cfg {
    int units = 1;            // switch 55
    int singles = 0;          // offline what-if ONLY (NOT the contract): singles get XLAT12_EXTRA_UNIT too
    int s49 = 1;              // switch 49 (with P5: only for a head n48_mib_head_executes admits)
    int pool = 1;             // the frame pool handed to units
    int feed = 1;             // switch 45: the frame-local list fed (per constituent inside a unit, then per segment)
    uint32_t pendCap = 0;     // xlat12_unit.pend_cap (0 = XLAT12_UNIT_PEND_MAX)
    const uint32_t *headOverride = nullptr;   // a copy of the input whose head dwords the P5 decision reads (the disguise)
    // build 0.0.481
    int retry = 0;            // switch 56 (a single refused for room retried through the unit path)
    uint32_t kindEncoder = 1; // the segment kind the pass declared (the kext's gXdBuild.kind == ENCODER)
    int cgRefuse = -1;        // the copy guard refuses THIS segment after it translated (the kext's N48_SEG_COPY_OVERLAP)
    int earlyAfterK = -1;     // F3: right after segment k, a unit whose translation refuses BEFORE d_unit_reset
    n48_mib_xlat_fn xlat = nullptr;   // the retry's translator call (nullptr = the real one; a test injects a faulty one)
    uint32_t pack = 0;        // build 0.0.501: switch 67, latched into the unit state once per frame (the kext: once per pass)
    // build 0.0.506 (switch 70)
    uint32_t defer = 0;                      // switch 70, latched once per frame (n48_mib_defer_begin, the kext's pass top)
    uint32_t deferCap = N48_MIB_DEFER_MAX;   // the cap handed to n48_mib_defer_take (the kext: N48_MIB_DEFER_MAX)
    uint32_t fence71 = 0;     // build 0.0.508: switch 71, latched once per frame (the kext: gXdBuild.fence71 at the pass top)
    int poolLate = 0;         // TEST CONDITION: pass 0's free runs reach the pool only at pass 1 (1) or never (2) - a pool that is
                              // "still small" at every first attempt, the situation, on decide44's bytes
};
struct UnitRes {
    uint32_t from, to, head, cons, st, op, at, placed, own, pool, unredir, inv, hs, cb, lastHead, slack, tailIn, isUnit;
    uint32_t fenceWhy, fenceCand, fenceWhyWhole, fenceCandWhole, firstTpos;
    uint32_t retryWhy, undone;   // build 0.0.481: n48_mib_retry_single's `why`; n48_mib_unit_undo's dwords
    uint32_t pkRuns, pkRecs;     // build 0.0.501: the unit state's PACK counters right after the call
    std::vector<uint32_t> out;   // the translate output right after the call (before later pool placements)
};
struct FrameRes { std::vector<UnitRes> u; std::vector<uint32_t> cand; std::string s; uint32_t ns = 0, nu = 0; uint32_t off[2] = {0, 0}, nn[2] = {0, 0};
                  xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t nib = 0; uint32_t gate = 0xFFFFFFFFu, gateDetail = 0;
                  // build 0.0.481
                  uint32_t nsegPre = 0, unitMap = 0, retries = 0, earlySt = 0, earlyUndone = 0, sentinels = 0;
                  uint32_t vkBlocks = 0, vkOk = 0, vkBad = 0, vkNotNop = 0, vkOverlap = 0, vkEntBad = 0;
                  // build 0.0.506 (switch 70): units deferred / capped; translate calls per unit (the main call only; switch
                  // 56's retry is its own); the fail-open probe: retries whose list differed from the first attempt's (the
                  // mirror's OWN copy, not the helper's), entries only a LATER unit fed that were probed at a retry, and of those
                  // the ones the retry's list proved (must be 0)
                  uint32_t deferred = 0, capped = 0, retried = 0; std::vector<uint32_t> attempts; std::vector<uint32_t> deferredK;
                  uint32_t listMismatch = 0, laterProbed = 0, laterProven = 0, rollbackBad = 0;
                  uint32_t rollbackShorter = 0;   // build 0.0.508 (LOW-1): deferrals whose attempt REMOVED an entry (kept removed)
                };
static xlat12_pool gPool;
static xlat12_unit gU;
static xlat12_ud_carry gCarry;
static char stch(uint32_t st, uint32_t op)
{
    if (st == 0u) return '.';
    if (st == XLAT12_IB_ERR_DESC) switch (op) {
        case XLAT12_TDESC_PROVENANCE: return 'P'; case XLAT12_TDESC_NO_ROOM: return 'M'; case XLAT12_TDESC_REDIRECTED: return 'E';
        case XLAT12_TDESC_TOO_MANY: return 'T'; case XLAT12_TDESC_SLOT_UNSEEN: return 'U'; case XLAT12_TDESC_READ: return 'R'; default: return '?'; }
    if (st == XLAT12_IB_ERR_PAIR) return 'A';
    if (st == XLAT12_IB_ERR_TOO_LONG) return 'X';
    if (st == N48_SEG_COPY_OVERLAP) return 'C';
    return 'Z';
}
static void base_flags(xlat12_draw_extra &ex)
{
    ex.pgm_profile = &u_profile;
    ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull;
    ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
    ex.flags |= XLAT12_EXTRA_PAIR_PRE | XLAT12_EXTRA_READSET;
    ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;
    ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &gCarry;
    ex.flags |= XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_VS_KNOWN;
    ex.desc_read = &u_read; ex.desc_tiled_ok = &u_tiled;
}
// build 0.0.481: the translator call the kext's gfxsrc_xlat_m2tri makes (n48_mib_retry_single's `fn`).
static uint32_t u_xlat(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    return xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), ex, in, n, out, olen, ds);
}
// one deferred block of a translated unit/retry, as its own pointer names it (the review's record verifier, r480/vk)
struct VkBlk { uint64_t va; std::vector<uint32_t> want; uint64_t eva_want; uint32_t eva_ok; };
static void vk_record(std::vector<VkBlk> &vk, const uint32_t *out)
{
    for (uint32_t b = 0; b < gU.npend; b++) {
        const xlat12_unit_pend *pb = &gU.pend[b];
        VkBlk v; v.va = (uint64_t)out[pb->tpos[0]] | ((uint64_t)out[pb->tpos[1]] << 32);
        const uint32_t blen = 8u + 8u * pb->tn + 4u * pb->sn + pb->ext_len;
        v.want.assign(blen, 0u);
        const uint32_t *src = &gU.dw[pb->off];
        for (uint32_t q = 0; q < 8u * pb->tn; q++) v.want[8 + q] = src[8 + q];
        for (uint32_t q = 0; q < 4u * pb->sn; q++) v.want[8 + 8 * pb->tn + q] = src[8 + 8 * pb->tcap + q];
        const uint32_t eo = 8u + 8u * pb->tn + 4u * pb->sn;
        for (uint32_t q = 0; q < pb->ext_len; q++) v.want[eo + q] = src[8 + 8 * pb->tcap + 4 * pb->scap + q];
        const uint64_t iva = v.va + 32ull, sva2 = iva + 32ull * pb->tn;
        v.want[0] = (uint32_t)iva; v.want[1] = (uint32_t)(iva >> 32);
        if (pb->nsamp_eff) { v.want[4] = (uint32_t)sva2; v.want[5] = (uint32_t)(sva2 >> 32); }
        v.eva_want = 0; v.eva_ok = 1u;
        if (pb->epos[0] != 0xFFFFFFFFu) { v.eva_want = v.va + 4ull * eo;
            v.eva_ok = (((uint64_t)out[pb->epos[0]] | ((uint64_t)out[pb->epos[1]] << 32)) == v.eva_want) ? 1u : 0u; }
        vk.push_back(v);
    }
}
static FrameRes run_frame(const uint32_t *const *ibw, const uint32_t *ibn, const uint64_t *ibva, uint32_t nib, const Cfg &cfg)
{
    FrameRes R; R.nib = nib;
    std::vector<uint32_t> cat;
    for (uint32_t k = 0; k < nib; k++) { R.off[k] = (uint32_t)cat.size(); R.nn[k] = ibn[k]; cat.insert(cat.end(), ibw[k], ibw[k] + ibn[k]); }
    const uint32_t N = (uint32_t)cat.size();
    frame_targets(cat.data(), N);
    uint32_t tot = 0, ibnseg[2] = {0, 0}; n48_mib_seg_diag dg {};
    xlat12_ib_segment segs[N48_XV_MAX_SEGS];
    uint32_t ns = nib >= 2u ? n48_mib_segment(cat.data(), nib, R.off, R.nn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 0u, nullptr)
                            : xlat12_ib_segments(cat.data(), N, segs, N48_XV_MAX_SEGS, &tot);
    if (ns != tot) ns = 0;
    R.ns = ns;
    R.nsegPre = n48_mib_nseg_of(ns, tot, N48_XV_MAX_SEGS);   // the kext's gXdBuild.nsegPre, BEFORE the unit stage (F2)
    static xlat12_ib_segment units[N48_XV_MAX_SEGS]; static uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS];
    uint32_t nu = ns;
    for (uint32_t k = 0; k < ns; k++) { units[k] = segs[k]; cf[k] = k; cc[k] = 1u; }
    if (cfg.units && ns && cfg.kindEncoder) {
        const uint32_t u = n48_mib_units(cat.data(), nib, R.off, R.nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        if (u) { nu = u; R.unitMap = 1u; }
    }
    R.nu = nu;
    for (uint32_t k = 0; k < nu; k++) R.segs[k] = units[k];
    gPool.nrun = 0; gPool.lost = 0; gPool.jn = 0;
    n48_dl_clear(&gFl);
    gU.pack = cfg.pack;   // build 0.0.501: the kext's once-per-pass latch (gUnitState.pack), before any unit or retry
    n48_mib_defer &D = gD; n48_mib_defer_begin(&D, cfg.defer);   // build 0.0.506: the kext's pass-top latch
    static xlat12_pool late; late.nrun = 0; late.lost = 0; late.jn = 0;   // cfg.poolLate's withheld runs
    std::vector<std::vector<n48_dl_ent>> firstList(nu);   // the mirror's OWN copy of the list before each first attempt
    R.attempts.assign(nu, 0u);
    R.u.assign(nu, UnitRes {});
    std::vector<std::string> ks(nu); std::vector<int> kpipe(nu, 0);
    R.cand = cat;
    std::vector<VkBlk> vk;
    uint32_t segIb = 0;
    std::vector<n48_dl_ent> mirAdds, loadList;   // build 0.0.511: the mirror's OWN record of the earlier retries' new entries
    for (uint32_t dpass = 0; dpass < 2u; dpass++) {   // build 0.0.506: the kext's two passes
    if (dpass) {
        if (!D.n) break;
        segIb = 0;
        if (cfg.poolLate == 1) for (uint32_t r = 0; r < late.nrun && gPool.nrun < XLAT12_POOL_RUNS; r++) gPool.run[gPool.nrun++] = late.run[r];
    }
    for (uint32_t k = 0; k < nu; k++) {
        const uint32_t from = units[k].start, to = units[k].end, n = to - from;
        if (dpass) {
            const uint32_t di = n48_mib_defer_index(&D, k);
            if (di == N48_MIB_DEFER_NONE) continue;
            (void)n48_mib_defer_load_aug(&D, di, &gFl, &gCarry);   // build 0.0.511: the kext's gfxsrc_defer_load
            R.retried++;
            // THE FAIL-OPEN PROBE: the list this retry runs against must be the one the first attempt ran against
            // (compared with the mirror's own copy), and no entry that only a LATER unit fed may be proven by it.
            // build 0.0.511: ... followed only by entries the mirror itself saw an EARLIER retry feed, for VAs the first
            // attempt's list lacks (the augmentation), never in place of one
            auto inAdds = [&](uint64_t va, uint32_t mode) { for (const n48_dl_ent &e : mirAdds) if (e.va == va && e.mode == mode) return true; return false; };
            bool same = gFl.n >= firstList[k].size();
            for (uint32_t q = 0; same && q < firstList[k].size(); q++) same = gFl.e[q].va == firstList[k][q].va && gFl.e[q].mode == firstList[k][q].mode;
            for (uint32_t q = (uint32_t)firstList[k].size(); same && q < gFl.n; q++) {
                bool inFirst = false;
                for (const n48_dl_ent &e : firstList[k]) inFirst |= (e.va == gFl.e[q].va);
                same = !inFirst && inAdds(gFl.e[q].va, gFl.e[q].mode);
            }
            if (!same) R.listMismatch++;
            loadList.assign(gFl.e, gFl.e + gFl.n);
            for (uint32_t j = k + 1u; j < nu; j++) {
                if (R.u[j].st || R.u[j].out.empty()) continue;
                static n48_dl later; std::memset(&later, 0, sizeof later);
                (void)n48_dl_from_output(&later, 5u, R.u[j].out.data(), (uint32_t)R.u[j].out.size(), nullptr);
                for (uint32_t q = 0; q < later.n; q++) {
                    bool inFirst = false;
                    for (const n48_dl_ent &e : firstList[k]) inFirst |= (e.va == later.e[q].va && e.mode == later.e[q].mode);
                    if (inFirst || inAdds(later.e[q].va, later.e[q].mode)) continue;   // 0.0.511: an earlier retry fed it too
                    R.laterProbed++;
                    if (n48_dl_tiled_ok(&gFl, 5u, later.e[q].va, later.e[q].mode)) R.laterProven++;
                }
            }
        }
        while (segIb + 1u < nib && from >= R.off[segIb] + R.nn[segIb]) { segIb++; if (!dpass) kpipe[k]++; }
        xlat12_draw_extra ex {};
        base_flags(ex);
        const uint32_t *hb = cfg.headOverride ? cfg.headOverride : cat.data();
        if (cfg.s49 && n48_mib_head_executes(hb, units[k].head, N)) ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
        ex.ib_va = n48_mib_seg_va(ibva[segIb], from, R.off[segIb]);
        // the kext's gfxsrc_unit_setup (n48_mib_unit_setup) for EVERY segment of a pass that formed units: F3's journal
        // reset first; a unit of >= 2 constituents (or, offline what-if only, a single) gets the unit flag
        gU.pend_cap = cfg.pendCap;
        uint32_t isUnit = 0u;
        if (R.unitMap) {
            const uint32_t want = (n48_mib_unit_flag(cc[k]) || (cfg.singles && cc[k] == 1u)) ? cc[k] : 0u;
            isUnit = n48_mib_unit_setup(&ex, &gU, &gPool, cfg.pool, want, &segs[cf[k]], from, cfg.feed ? &u_feed : nullptr, nullptr);
        }
        if (isUnit) {
            gConsTargets.assign(cc[k], std::vector<uint64_t>());
            for (uint32_t j = 0; j < cc[k]; j++) {
                frame_targets(&cat[segs[cf[k] + j].head], segs[cf[k] + j].end - segs[cf[k] + j].head);
                gConsTargets[j] = gFrameTargets;
            }
            gCurUnit = &gU;
        } else {
            gCurUnit = nullptr;
        }
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        uint32_t olen = 0;
        uint32_t *out = &R.cand[from];
        if (D.on && R.unitMap && !dpass) {   // build 0.0.506: the kext's gfxsrc_defer_pre, and the mirror's own copy
            n48_mib_defer_pre(&D, &gFl, &gCarry);
            firstList[k].assign(gFl.e, gFl.e + gFl.n);
        }
        R.attempts[k]++;
        n48_mib_defer_ask_clear(&D);   // build 0.0.511: the kext clears the attempt's failed asks before the translate
        uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &cat[from], n, out, &olen, &ds);
        gCurUnit = nullptr;
        // build 0.0.481 (switch 56): the kext's retry, right after the translate call, before the copy guard
        uint32_t rwhy = N48_MIB_RETRY_NOT;
        if (cfg.retry && n48_mib_retry_wanted((uint32_t)cfg.units, (uint32_t)cfg.retry, R.unitMap, cfg.kindEncoder, cc[k], st, ds.err_op)) {
            n48_mib_defer_ask_clear(&D);   // build 0.0.511: and before switch 56's retry (gfxsrc_unit_retry)
            st = n48_mib_retry_single(cfg.xlat ? cfg.xlat : &u_xlat, &ex, &gU, &gPool, cfg.pool, &segs[cf[k]], from, &cat[from], n,
                                      out, &olen, &ds, st, &rwhy);
            if (rwhy == N48_MIB_RETRY_OK || rwhy == N48_MIB_RETRY_REFUSED || rwhy == N48_MIB_RETRY_SENTINEL_LEFT) { isUnit = 2u; R.retries++; }
        }
        // build 0.0.506 (switch 70): the kext's gfxsrc_defer_take, before the copy guard and everything after it
        if (D.on) {
            const uint32_t capped0 = D.capped;
            // build 0.0.511: the kext's gfxsrc_defer_take - the consumer (take_prov) first, then the room deferral with its
            // write set (take_w)
            // build 0.0.512 (C3): the consumer rule keyed on the refusing VA, as the kext now calls it
            if (n48_mib_defer_take_prov_va(&D, dpass, k, R.unitMap, st, ds.err_op, ds.prov_va, cfg.deferCap, &gFl, cat.data(), N, from, to) ||
                n48_mib_defer_take_w(&D, dpass, k, R.unitMap, isUnit, st, ds.err_op, cfg.deferCap, &gFl, cat.data(), N, from, to)) {
                std::memcpy(out, &cat[from], 4u * n);
                (void)n48_mib_unit_undo(isUnit, st, &gPool);
                R.deferred++; R.deferredK.push_back(k);
                // the refused attempt's per-constituent feeds are gone again. build 0.0.508 (LOW-1): only its ADDITIONS are
                // taken back - every live entry stood before the attempt (its removals stay removed, so the list may be shorter)
                bool rb = gFl.n <= firstList[k].size();
                for (uint32_t q = 0; rb && q < gFl.n; q++) {
                    bool in = false;
                    for (const n48_dl_ent &e : firstList[k]) in |= (e.va == gFl.e[q].va && e.mode == gFl.e[q].mode);
                    rb = in;
                }
                if (!rb) R.rollbackBad++;
                if (gFl.n < firstList[k].size()) R.rollbackShorter++;
                continue;
            }
            R.capped += D.capped - capped0;
        }
        // the copy guard: the kext asks it only of a segment that translated; a test names the one it refuses
        if (!st && (int)k == cfg.cgRefuse) st = N48_SEG_COPY_OVERLAP;
        UnitRes r {};
        r.from = from; r.to = to; r.head = units[k].head; r.cons = cc[k]; r.st = st; r.op = ds.err_op; r.at = ds.err_in_dword; r.isUnit = isUnit;
        r.retryWhy = rwhy;
        r.out.assign(out, out + n);
        if (isUnit) { r.placed = gU.placed; r.own = gU.own_dw; r.pool = gU.pool_dw; r.unredir = gU.unredir; r.inv = gU.inv_inline;
                      r.hs = gU.hs_patched; r.cb = gU.cb_calls; r.lastHead = gU.last_head_out; r.slack = gU.slack; r.tailIn = gU.tail_in_place;
                      r.firstTpos = 0xFFFFFFFFu; for (uint32_t b = 0; b < gU.npend; b++) if (gU.pend[b].tpos[0] < r.firstTpos) r.firstTpos = gU.pend[b].tpos[0];
                      r.pkRuns = gU.pk_runs; r.pkRecs = gU.pk_recs; }
        if (isUnit && !st) vk_record(vk, out);
        // the kext's order after the translate: the frame-local feed (the last constituent's slice), the fence slice,
        // the restore, then gfxsrc_unit_after (n48_mib_unit_undo on a refusal, the pool fed by a translated segment)
        if (!st && cfg.feed) (void)n48_dl_from_output(&gFl, 5u, out + r.lastHead, olen - r.lastHead, nullptr);
        r.fenceWhy = r.fenceWhyWhole = 99u;
        if (!st) {
            n48_f828 fr {}, fw {};
            // build 0.0.508 (switch 71): the kext's choice at its two fence searches, latched once per frame
            r.fenceWhy = cfg.fence71 ? n48_f828_find_last(out + r.lastHead, olen - r.lastHead, &fr)
                                     : n48_f828_find(out + r.lastHead, olen - r.lastHead, &fr);
            r.fenceCand = fr.candidates;   // C3: the final constituent
            r.fenceWhyWhole = n48_f828_find(out, olen, &fw); r.fenceCandWhole = fw.candidates;
        }
        if (st) std::memcpy(out, &cat[from], 4u * n);
        if (cfg.units) {
            r.undone = n48_mib_unit_undo(isUnit, st, &gPool);
            if (!st && cfg.pool) xlat12_pool_add_free((cfg.poolLate && !dpass) ? &late : &gPool, out, olen, ex.ib_va);
        }
        if (D.on) n48_mib_defer_done(&D, dpass, k, &gFl, st, cat.data(), N, from, to);   // build 0.0.512: the kext's gfxsrc_defer_done
        if (dpass && !st)   // build 0.0.511: the mirror's own record of this retry's new entries (vs the list it was handed)
            for (uint32_t q = 0; q < gFl.n; q++) {
                bool was = false;
                for (const n48_dl_ent &e : loadList) was |= (n48_mib_defer_ent_same(&e, &gFl.e[q]) != 0u);
                if (!was) mirAdds.push_back(gFl.e[q]);
            }
        ks[k].clear();
        for (uint32_t u = 0; u < cc[k]; u++) ks[k] += (u == 0 ? stch(st, ds.err_op) : (st ? 'm' : '.'));
        R.u[k] = r;
        // F3: a UNIT whose translation refuses BEFORE the translator's own journal reset (d_unit_reset) - here a
        // two-constituent stream with no draw, refused DRAW_SHAPE by translate_draw_ex's draw count - driven through the
        // kext's own setup and after-step, right after segment k. It must take back NOTHING of segment k's placements.
        if ((int)k == cfg.earlyAfterK && !dpass) {
            static uint32_t eIn[16], eOut[16];
            for (uint32_t q = 0; q < 16u; q++) eIn[q] = XLAT12_IB_NOP;
            xlat12_ib_segment ecs[2] {}; ecs[0].head = 0u; ecs[1].head = 4u;
            xlat12_draw_extra ex2 {}; base_flags(ex2); ex2.ib_va = 0x4100000000ull;
            (void)n48_mib_unit_setup(&ex2, &gU, &gPool, cfg.pool, 2u, ecs, 0u, nullptr, nullptr);
            static xlat12_draw_stats ds2; ds2 = xlat12_draw_stats {}; uint32_t ol2 = 0;
            R.earlySt = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex2, eIn, 16u, eOut, &ol2, &ds2);
            R.earlyUndone = n48_mib_unit_undo(1u, R.earlySt, &gPool);
        }
    }
    }   // build 0.0.506: the two passes
    for (uint32_t k = 0; k < nu; k++) { for (int q = 0; q < kpipe[k]; q++) R.s += '|'; R.s += ks[k]; }
    // the gate's segment rungs over the REAL table and statuses (the rewrite evidence set positive, as mib_frame_from does)
    if (nu) {
        n48_cm_frame c {};
        mib_frame_from(c, units, nu, R.nn, nib, N);
        for (uint32_t k = 0; k < nu; k++) { c.seg[k].status = R.u[k].st; c.seg[k].out_len = R.u[k].st ? 0u : units[k].end - units[k].start; }
        R.gate = n48_cm_gate(&c, &R.gateDetail);
    }
    // the review's record verifier over the FINAL candidate: every deferred block where its pointer says, inside a NOP body,
    // no two overlapping, the entry pointers right; and no sentinel word anywhere in the frame
    {
        std::vector<std::pair<uint32_t, uint32_t>> iv;
        for (const VkBlk &v : vk) {
            R.vkBlocks++;
            int found = -1; uint32_t idx = 0;
            for (uint32_t q = 0; q < nib; q++) if (v.va >= ibva[q] && v.va + 4ull * v.want.size() <= ibva[q] + 4ull * R.nn[q]) { found = (int)q; idx = R.off[q] + (uint32_t)((v.va - ibva[q]) / 4u); }
            if (found < 0) { R.vkBad++; continue; }
            int same = 1; for (uint32_t q = 0; q < v.want.size(); q++) if (R.cand[idx + q] != v.want[q]) same = 0;
            if (!v.eva_ok) R.vkEntBad++;
            uint32_t i = R.off[found], inNop = 0; const uint32_t e = R.off[found] + R.nn[found];
            while (i < e) { const uint32_t h = R.cand[i]; const uint32_t l = (h == XLAT12_IB_NOP || (h >> 30) == 2u) ? 1u : ((h >> 30) == 3u ? ((h >> 16) & 0x3fffu) + 2u : 0u);
              if (!l) break; if (idx >= i && idx < i + l) { inNop = ((h >> 30) == 3u && ((h >> 8) & 0xffu) == 0x10u && idx > i && idx + v.want.size() <= i + l); break; } i += l; }
            if (!inNop) R.vkNotNop++;
            for (auto &p : iv) if (idx < p.second && p.first < idx + (uint32_t)v.want.size()) R.vkOverlap++;
            iv.push_back({idx, idx + (uint32_t)v.want.size()});
            if (same) R.vkOk++; else R.vkBad++;
        }
        for (uint32_t q = 0; q < N; q++) if (R.cand[q] == N48_MIB_RETRY_SENTINEL) R.sentinels++;
    }
    return R;
}
// a unit result by index that never reads past the vector (a planted break can change how many units there are)
static const UnitRes &ures(const FrameRes &R, size_t i) { static const UnitRes none {}; return i < R.u.size() ? R.u[i] : none; }
static const uint32_t *const kF54W[1] = { kU54Ib0 };  static const uint32_t kF54N[1] = { 6112u };  static const uint64_t kF54V[1] = { kU54Ib0Va };
static const uint32_t *const kF59W[1] = { kU59Ib0 };  static const uint32_t kF59N[1] = { 8848u };  static const uint64_t kF59V[1] = { kU59Ib0Va };
static const uint32_t *const kF77W[2] = { kU77Ib0, kU77Ib1 };  static const uint32_t kF77N[2] = { 1152u, 14944u };
static const uint64_t kF77V[2] = { kU77Ib0Va, kU77Ib1Va };
static const uint32_t *const kF115W[1] = { kU115Ib0 };  static const uint32_t kF115N[1] = { 2448u };  static const uint64_t kF115V[1] = { kU115Ib0Va };

// ---- helpers over a translated unit's own bytes ------------------------------------------------------------------------
// Walk a buffer in EXECUTED order from `at` (NOP bodies skipped, a one-dword NOP one dword) up to `end`; returns 1 when an
// ACQUIRE_MEM whose GCR_CNTL covers XLAT12_DESC_INV_GCR executes before the first draw past dword `after` (the first
// table-pointer redirect: that draw is the records' first reader).
static int inv_before_first_draw(const uint32_t *w, uint32_t at, uint32_t end, uint32_t after)
{
    int inv = 0;
    for (uint32_t i = at; i < end; ) {
        const uint32_t h = w[i];
        if (h == XLAT12_IB_NOP || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) return 0;
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u, op = (h >> 8) & 0xFFu;
        if (op == 0x58u && l == 8u && (w[i + 7] & XLAT12_DESC_INV_GCR) == XLAT12_DESC_INV_GCR) inv = 1;
        if ((op == 0x2Du || op == 0x27u || op == 0x35u) && i > after) return inv;
        i += l;
    }
    return inv;
}
// every SET_SH_REG write of gfx12 SPI_SHADER_PGM_LO/HI_HS in a unit output: 1 when every one holds the ring pointer
static int hs_pairs_patched(const uint32_t *w, uint32_t n, uint64_t ptr, uint32_t *count)
{
    uint32_t c = 0; int ok = 1;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t h = w[i];
        if (h == XLAT12_IB_NOP || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u, op = (h >> 8) & 0xFFu;
        if (op == 0x76u)
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t a = 4u * (0x2c00u + (w[i + 1] & 0xFFFFu) + k);
                if (a == 0xb410u) { c++; if (w[i + 2 + k] != (uint32_t)ptr) ok = 0; }
                if (a == 0xb414u) { if (w[i + 2 + k] != (uint32_t)(ptr >> 32)) ok = 0; }
            }
        i += l;
    }
    if (count) *count = c;
    return ok;
}

static void checks(const char *ahh)
{
    std::printf("\n== 0.0.480: CONTINUATION UNITS over decide44's real bytes, in the kext's order ==\n");
    gHide491 = 1;   // build 0.0.491: the mechanics on the frames as recorded (see gHide491); checks491 runs the live rows
    // ---- C1: THE UNIT RULE ----
    {
        std::vector<uint32_t> cat(kU77Ib0, kU77Ib0 + 1152u); cat.insert(cat.end(), kU77Ib1, kU77Ib1 + 14944u);
        const uint32_t off[2] = { 0u, 1152u }, nn[2] = { 1152u, 14944u };
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS], ibns[2], tot = 0;
        n48_mib_seg_diag dg {};
        const uint32_t ns = n48_mib_segment(cat.data(), 2u, off, nn, segs, N48_XV_MAX_SEGS, ibns, &tot, &dg, 0u, nullptr);
        expect_u("C1 F77: the segment stage names 14 segments (2 + 12)", ns, 14u);
        const uint32_t nu = n48_mib_units(cat.data(), 2u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        expect_u("C1 F77: 8 units", nu, 8u);
        std::string shape; for (uint32_t k = 0; k < nu; k++) shape += (char)('0' + cc[k]);
        if (gPrint) std::printf("      F77 constituents per unit: %s\n", shape.c_str());
        expect_u("C1 F77: constituents per unit 2|1 2 4 1 1 2 1 (IB0 k1 and IB1 k2/k4/k6-k8/k12 continue)", shape == "21241121" ? 1u : 0u, 1u);
        expect_u("C1 F77: IB0's last unit ends at the IB boundary", units[0].end, 1152u);
        expect_u("C1 F77: IB1's first unit starts at IB1's own head (+2)", units[1].start, 1152u + 2u);
        expect_u("C1 F77: unit [1218,3664) of IB1", (units[2].start - 1152u) * 100000u + (units[2].end - 1152u), 1218u * 100000u + 3664u);
        expect_u("C1 F77: unit [3666,6384) of IB1 (4 constituents)", (units[3].start - 1152u) * 100000u + (units[3].end - 1152u), 3666u * 100000u + 6384u);
        uint32_t ok = 1;
        for (uint32_t k = 0; k < nu; k++)
            for (uint32_t j = 1; j < cc[k]; j++) ok &= (uint32_t)n48_mib_seg_continues(cat.data(), segs[cf[k] + j].start, segs[cf[k] + j].end);
        expect_u("C1 F77: every later constituent writes no CB0 VIEW before its first draw", ok, 1u);
        uint32_t heads = 1;
        for (uint32_t k = 0; k < nu; k++) if (k != 0u && k != 1u) heads &= (uint32_t)!n48_mib_seg_continues(cat.data(), segs[cf[k]].start, segs[cf[k]].end);
        expect_u("C1 F77: every unit's head segment (not first of its IB) sets its own CB0 VIEW", heads, 1u);
        // the gate over the unit table: the tiling, kind, length and COVER-per-IB rungs pass it
        n48_cm_frame c {}; uint32_t det = 0;
        mib_frame_from(c, units, nu, nn, 2u, 16096u);
        expect_u("C1 F77: the gate's segment rungs accept the unit table (answers OK)", n48_cm_gate(&c, &det), (uint32_t)N48_CM_OK);
        // a unit spanning the IB boundary is refused by COVER-per-IB (the design's planted break 2, at the gate)
        xlat12_ib_segment bad[N48_XV_MAX_SEGS]; for (uint32_t k = 0; k < nu; k++) bad[k] = units[k];
        bad[0].end = units[1].end; for (uint32_t k = 1; k + 1 < nu; k++) bad[k] = units[k + 1];
        mib_frame_from(c, bad, nu - 1u, nn, 2u, 16096u);
        expect_u("C1 F77: a unit merged across the IB boundary is refused COVER by the gate", n48_cm_gate(&c, &det), (uint32_t)N48_CM_COVER);
        // THE IB RULE ITSELF: a two-IB frame whose IB1 is ONE continuation segment (F77 IB1's [1218,2257) constituent that
        // writes no CB0 VIEW, with its own head) - the predicate alone would merge it into IB0's last unit; one IB only
        // keeps it apart.
        const uint32_t c0 = segs[cf[2] + 1u].head, c1 = segs[cf[2] + 1u].end;
        std::vector<uint32_t> two(kU77Ib0, kU77Ib0 + 1152u); two.insert(two.end(), cat.begin() + c0, cat.begin() + c1);
        const uint32_t off2[2] = { 0u, 1152u }, nn2[2] = { 1152u, c1 - c0 };
        xlat12_ib_segment s2[N48_XV_MAX_SEGS], u2s[N48_XV_MAX_SEGS]; uint32_t cf2[N48_XV_MAX_SEGS], cc2[N48_XV_MAX_SEGS], ib2[2], t2 = 0;
        n48_mib_seg_diag dg2 {};
        const uint32_t ns2 = n48_mib_segment(two.data(), 2u, off2, nn2, s2, N48_XV_MAX_SEGS, ib2, &t2, &dg2, 0u, nullptr);
        expect_u("C1 IB rule: IB1 (one continuation segment) is itself a continuation by the predicate",
                 ns2 == 3u ? (uint32_t)n48_mib_seg_continues(two.data(), s2[2].start, s2[2].end) : 99u, 1u);
        const uint32_t nu2 = n48_mib_units(two.data(), 2u, off2, nn2, s2, ns2, XLAT12_UNIT_CONS_MAX, u2s, cf2, cc2, N48_XV_MAX_SEGS, 0u);
        expect_u("C1 IB rule: ... and still becomes its OWN unit (no unit crosses the IB boundary)", nu2 * 100u + (nu2 == 2u ? u2s[1].start : 0u), 2u * 100u + 1154u);
    }
    {
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS], tot = 0;
        const uint32_t off[1] = { 0u }, nn[1] = { 8848u };
        const uint32_t ns = xlat12_ib_segments(kU59Ib0, 8848u, segs, N48_XV_MAX_SEGS, &tot);
        const uint32_t nu = n48_mib_units(kU59Ib0, 1u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        expect_u("C1 F59 IB0: 9 segments -> 4 units", ns * 100u + nu, 904u);
        expect_u("C1 F59 IB0: unit [1458,4256) has 6 constituents and 12 draws", units[1].start * 1000000u + units[1].end * 100u + cc[1] * 10u, 1458u * 1000000u + 4256u * 100u + 60u);
        expect_u("C1 F59 IB0: ... 12 draws", units[1].draws, 12u);
        // the cap: with at most 4 constituents per unit, the fifth continuation starts its own unit (translates alone)
        const uint32_t nu4 = n48_mib_units(kU59Ib0, 1u, off, nn, segs, ns, 4u, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        expect_u("C1 F59 IB0: a 4-constituent cap splits the 6-constituent unit 4 + 2", nu4 * 100u + cc[1] * 10u + cc[2], 5u * 100u + 42u);
    }
    // ---- ORACLE CHAIN (design Q10) on F59 IB0's second constituent [the unit k1's k2], with the real translator ----
    {
        xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t tot = 0;
        (void)xlat12_ib_segments(kU59Ib0, 8848u, segs, N48_XV_MAX_SEGS, &tot);
        const uint32_t from = segs[2].start, n = segs[2].end - segs[2].start;
        std::vector<uint32_t> out(n);
        xlat12_draw_stats ds {}; uint32_t olen = 0;
        xlat12_draw_extra ex {}; base_flags(ex); ex.ib_va = kU59Ib0Va + 4ull * from;
        uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &kU59Ib0[from], n, out.data(), &olen, &ds);
        expect_u("ORACLE 1 (10b flags, a continuation alone): TOO_LONG err_op 0xFF (the extra block, d_rings)", st * 1000u + ds.err_op, 23u * 1000u + 0xFFu);
        ex.ring_va = 0; ex.gs_sgpr0_va = 0; ex.rsrc3_gs = 0;   // the block shrinks to the raster delta's 12 dwords
        st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &kU59Ib0[from], n, out.data(), &olen, &ds);
        if (gPrint) std::printf("      ORACLE 2: st %u op %#x reg %#x at %u (n %u)\n", st, ds.err_op, ds.err_reg, ds.err_in_dword, n);
        // The design's 0xFD step removed the WHOLE block (a prototype probe); the real translator always carries the raster
        // delta's 12 dwords with XLAT12_EXTRA_RASTER, and those alone still overflow this continuation's region 0.
        expect_u("ORACLE 2 (ring block off, raster delta kept): still TOO_LONG 0xFF - the 12 raster dwords alone do not fit",
                 st * 1000u + ds.err_op, 23u * 1000u + 0xFFu);
        ex.flags &= ~(uint32_t)(XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW);
        st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &kU59Ib0[from], n, out.data(), &olen, &ds);
        expect_u("ORACLE 3 (raster accepted as inherited): DESC SLOT_UNSEEN 0xF1 (the table pointer is inherited)", st * 1000u + ds.err_op, 29u * 1000u + 0xF1u);
    }
    {
        // ORACLE 4: the merged unit IN PLACE (the unit's bytes, no XLAT12_EXTRA_UNIT) - the re-emission wall
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS], tot = 0;
        std::vector<uint32_t> cat(kU77Ib0, kU77Ib0 + 1152u); cat.insert(cat.end(), kU77Ib1, kU77Ib1 + 14944u);
        const uint32_t off[2] = { 0u, 1152u }, nn[2] = { 1152u, 14944u }; uint32_t ibns[2]; n48_mib_seg_diag dg {};
        const uint32_t ns = n48_mib_segment(cat.data(), 2u, off, nn, segs, N48_XV_MAX_SEGS, ibns, &tot, &dg, 0u, nullptr);
        (void)n48_mib_units(cat.data(), 2u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        const uint32_t from = units[3].start, n = units[3].end - from;
        std::vector<uint32_t> out(n); xlat12_draw_stats ds {}; uint32_t olen = 0;
        xlat12_draw_extra ex {}; base_flags(ex); ex.ib_va = kU77Ib1Va + 4ull * (from - 1152u);
        ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
        const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &cat[from], n, out.data(), &olen, &ds);
        if (gPrint) std::printf("      ORACLE 4: st %u op %#x at %u\n", st, ds.err_op, ds.err_in_dword);
        expect_u("ORACLE 4 (F77's 10-draw unit merged IN PLACE, no unit flag): TOO_LONG REEMIT_NO_ROOM 0xFC", st * 1000u + ds.err_op, 23u * 1000u + 0xFCu);
    }
    // ---- THE KEXT'S ORDER, FULL FRAMES ----
    {
        Cfg off; off.units = 0;
        Cfg on;
        const FrameRes A = run_frame(kF77W, kF77N, kF77V, 2u, off);
        const FrameRes B = run_frame(kF77W, kF77N, kF77V, 2u, on);
        if (gPrint) std::printf("      F77 55 OFF %s | 55 ON %s  (gate %u detail %#x)\n", A.s.c_str(), B.s.c_str(), B.gate, B.gateDetail);
        expect_u("F77 55 OFF: ?X|..X.XXX...XM (today's string; the fixture fills the capture's gaps, so k7 reaches NO_ROOM)", A.s == "?X|..X.XXX...XM" ? 1u : 0u, 1u);
        expect_u("F77 55 ON:  ?m|...........M (every unit translates; the single last segment k7 stays NO_ROOM - C2)", B.s == "?m|...........M" ? 1u : 0u, 1u);
        const UnitRes &u2 = ures(B, 2), &u3 = ures(B, 3), &u7 = ures(B, 7);
        expect_u("F77 unit [1218,3664): 2 constituents, 3 deferred blocks placed, 0 un-redirects", u2.cons * 10000u + u2.placed * 100u + u2.unredir, 2u * 10000u + 300u);
        expect_u("F77 unit [3666,6384): 4 constituents, 5 blocks placed, EXACTLY 2 un-redirects (P4)", u3.cons * 10000u + u3.placed * 100u + u3.unredir, 4u * 10000u + 502u);
        expect_u("F77 unit [3666,6384): 3 per-constituent feeds (one at each later head)", u3.cb, 3u);
        expect_u("F77 unit [3666,6384): its final-constituent fence slice is found OK with ONE candidate", u3.fenceWhy * 10u + u3.fenceCand, (uint32_t)N48_F828_OK * 10u + 1u);
        expect_u("F77 single k7 (C2: no unit flag): NO_ROOM 0xF8, exactly as 55 OFF would place it in place", u7.isUnit * 1000u + u7.st * 10u + (u7.op == 0xF8u), 29u * 10u + 1u);
        uint32_t hsn = 0; int hsok = 1;
        for (const UnitRes &u : B.u) if (u.isUnit && !u.st) { uint32_t c = 0; hsok &= hs_pairs_patched(u.out.data(), (uint32_t)u.out.size(), 0x23f0a80000ull, &c); hsn += c; }
        expect_u("F77 55 ON: every HS pointer-pair write in every translated unit holds the ring pointer (none left zero)", (uint32_t)hsok * 1000u + (hsn >= 3u), 1001u);
        int invok = 1;
        for (const UnitRes &u : B.u) if (u.isUnit && !u.st && u.placed) invok &= inv_before_first_draw(B.cand.data(), u.head, u.to, u.from + u.firstTpos);
        expect_u("F77 55 ON: an executed covering ACQUIRE_MEM precedes the first draw of every unit that placed records", (uint32_t)invok, 1u);
        expect_u("F77 55 ON: the frame candidate walks cleanly end to end (every record is inside a NOP body)",
                 n48_f828_walk_ok(B.cand.data(), (uint32_t)B.cand.size(), nullptr), 1u);
        // ON IDENTITY: every single-constituent segment's translation is byte-identical to 55 OFF's
        uint32_t singles = 0, same = 0;
        for (const UnitRes &b : B.u) if (b.cons == 1u)
            for (const UnitRes &a : A.u) if (a.from == b.from) { singles++; same += (a.st == b.st && a.out == b.out) ? 1u : 0u; }
        expect_u("F77 ON IDENTITY: every single segment (4) translates byte-identically with 55 ON", singles * 1000u + same, 4u * 1000u + 4u);
        expect_u("C3 F77: over the WHOLE 4-constituent unit the fence would read AMBIGUOUS (two candidates) - the slice is the rule",
                 u3.fenceWhyWhole * 10u + u3.fenceCandWhole, (uint32_t)N48_F828_AMBIGUOUS * 10u + 2u);
        // the what-if the contract does NOT ship: singles through the unit path too -> k7 translates from the pool
        Cfg sd; sd.singles = 1;
        const FrameRes S = run_frame(kF77W, kF77N, kF77V, 2u, sd);
        if (gPrint) std::printf("      F77 what-if (singles deferred too, NOT the contract): %s, k7 pool %u dw\n", S.s.c_str(), ures(S, 7).pool);
        expect_u("F77 what-if (singles deferred too - offline only): ?m|............ with k7's records partly in the pool",
                 (S.s == "?m|............" ? 1u : 0u) * 10u + (ures(S, 7).pool > 0u), 11u);
    }
    {
        Cfg off; off.units = 0; Cfg on;
        const FrameRes A = run_frame(kF54W, kF54N, kF54V, 1u, off);
        const FrameRes B = run_frame(kF54W, kF54N, kF54V, 1u, on);
        if (gPrint) std::printf("      F54 55 OFF %s gate %u | 55 ON %s gate %u\n", A.s.c_str(), A.gate, B.s.c_str(), B.gate);
        expect_u("F54 55 OFF: the gate refuses a segment (SEG_REFUSED)", A.gate, (uint32_t)N48_CM_SEG_REFUSED);
        expect_u("F54 55 ON: every unit and segment translates (......)", B.s == "......" ? 1u : 0u, 1u);
        expect_u("F54 55 ON: the COMMIT gate's segment rungs pass the whole frame (OK)", B.gate, (uint32_t)N48_CM_OK);
        uint32_t pool = 0; for (const UnitRes &u : B.u) pool += u.pool;
        expect_u("F54 55 ON: the second unit takes records from the pool (an EARLIER unit's NOP run)", pool > 0u ? 1u : 0u, 1u);
        if (gPrint) for (const UnitRes &u : B.u) std::printf("      F54 unit @%u cons %u st %u fence slice %u/%u whole %u/%u\n", u.from, u.cons, u.st, u.fenceWhy, u.fenceCand, u.fenceWhyWhole, u.fenceCandWhole);
        const UnitRes &fin = B.u.back();
        expect_u("F54 55 ON: the final unit has 3 constituents", fin.cons, 3u);
        expect_u("C3 F54: the final unit's fence over its LAST constituent is OK with one candidate", fin.fenceWhy * 10u + fin.fenceCand, (uint32_t)N48_F828_OK * 10u + 1u);
        expect_u("F54 55 ON: the frame candidate walks cleanly (pool records are inside NOP bodies)", n48_f828_walk_ok(B.cand.data(), (uint32_t)B.cand.size(), nullptr), 1u);
        uint32_t singles = 0, same = 0;
        for (const UnitRes &b : B.u) if (b.cons == 1u)
            for (const UnitRes &a : A.u) if (a.from == b.from) { singles++; same += (a.st == b.st && a.out == b.out) ? 1u : 0u; }
        expect_u("F54 ON IDENTITY: every single segment translates byte-identically with 55 ON", singles * 1000u + same, 1u * 1000u + 1u);
    }
    {
        // F59 IB0's 6-constituent unit: placed what fits its own leftover, then NO_ROOM - the only earlier unit of the frame
        // (compute k0) refused, so the pool is empty; a refused unit's own bytes are restored and nothing is left in any pool
        Cfg on;
        const FrameRes B = run_frame(kF59W, kF59N, kF59V, 1u, on);
        if (gPrint) std::printf("      F59 IB0 55 ON %s; unit placed %u own %u slack %u unredir %u\n", B.s.c_str(), ures(B, 1).placed, ures(B, 1).own, ures(B, 1).slack, ures(B, 1).unredir);
        expect_u("F59 IB0 55 ON: ZMmmmmm.. (compute k0 refused; the 6-constituent unit NO_ROOM)", B.s == "ZMmmmmm.." ? 1u : 0u, 1u);
        expect_u("F59 IB0 unit: 4 blocks placed in its own leftover, 2 un-redirects, before NO_ROOM", ures(B, 1).placed * 10u + ures(B, 1).unredir, 42u);
        uint32_t same = 1; for (uint32_t k = ures(B, 1).from; k < ures(B, 1).to; k++) same &= (uint32_t)(B.cand[k] == kU59Ib0[k]);
        expect_u("F59 IB0 unit refused: its candidate bytes are Apple's again", same, 1u);
    }
    // ---- P3's JOURNAL: a unit that places into the pool and then refuses leaves the pool exactly as it found it ----
    {
        std::vector<uint32_t> donor(200u, XLAT12_IB_NOP);
        const std::vector<uint32_t> before = donor;
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS], tot = 0;
        const uint32_t off[1] = { 0u }, nn[1] = { 8848u };
        const uint32_t ns = xlat12_ib_segments(kU59Ib0, 8848u, segs, N48_XV_MAX_SEGS, &tot);
        (void)n48_mib_units(kU59Ib0, 1u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        gPool.nrun = 0; gPool.lost = 0; gPool.jn = 0;
        (void)xlat12_pool_add_free(&gPool, donor.data(), 30u, 0x4100000000ull);   // ONE small run: fits one block, not two
        const uint32_t from = units[1].start, n = units[1].end - from;
        std::vector<uint32_t> out(n); xlat12_draw_stats ds {}; uint32_t olen = 0;
        xlat12_draw_extra ex {}; base_flags(ex); ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_UNIT;
        ex.ib_va = kU59Ib0Va + 4ull * from; ex.unit = &gU;
        gU.ncons = cc[1]; for (uint32_t j = 0; j < cc[1]; j++) gU.cons_in[j] = j ? segs[cf[1] + j].head - from : 0u;
        gU.pool = &gPool; gU.cons_fn = nullptr; gU.pend_cap = 0;
        const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &kU59Ib0[from], n, out.data(), &olen, &ds);
        if (gPrint) std::printf("      JOURNAL: st %u op %#x pool %u placed %u\n", st, ds.err_op, gU.pool_dw, gU.placed);
        expect_u("P3 journal: the F59 unit refuses NO_ROOM after taking the small pool run", st * 1000u + ds.err_op + (gU.pool_dw ? 0u : 99999u), 29u * 1000u + 0xF8u);
        expect_u("P3 journal: the donor's NOP run is NOP again, dword for dword", donor == before ? 1u : 0u, 1u);
        expect_u("P3 journal: the pool run is offered again whole (30 dw at its own VA)", gPool.run[0].len * 10u + (gPool.run[0].host == donor.data()), 301u);
        // the KEXT's undo (its restore point, e.g. the copy guard refusing a unit that TRANSLATED): a successful placement
        // into the pool is taken back by xlat12_pool_undo
        gPool.nrun = 0; gPool.jn = 0;
        (void)xlat12_pool_add_free(&gPool, donor.data(), 200u, 0x4100000000ull);
        Cfg dummy; (void)dummy;
        gPool.j[0].host = donor.data(); gPool.j[0].va = 0x4100000000ull; gPool.j[0].len = 200u; gPool.j[0].used = 20u; gPool.j[0].r = 0u; gPool.jn = 1u;
        for (uint32_t k = 0; k < 20u; k++) donor[k] = 0x1234u;
        gPool.run[0].host += 20; gPool.run[0].va += 80u; gPool.run[0].len -= 20u;
        const uint32_t und = xlat12_pool_undo(&gPool);
        expect_u("P3 journal: xlat12_pool_undo restores 20 dwords, the donor and the run", und * 100u + (donor == before) * 10u + (gPool.run[0].len == 200u), 2011u);
        expect_u("P3 journal: undo is idempotent (the journal is emptied)", xlat12_pool_undo(&gPool), 0u);
    }
    // ---- P4's carry rule: a redirected slot the carry does NOT hold is never un-redirected (it refuses REDIRECTED) ----
    {
        std::vector<uint32_t> cat(kU77Ib0, kU77Ib0 + 1152u); cat.insert(cat.end(), kU77Ib1, kU77Ib1 + 14944u);
        const uint32_t off[2] = { 0u, 1152u }, nn[2] = { 1152u, 14944u }; uint32_t ibns[2], tot = 0; n48_mib_seg_diag dg {};
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS];
        const uint32_t ns = n48_mib_segment(cat.data(), 2u, off, nn, segs, N48_XV_MAX_SEGS, ibns, &tot, &dg, 0u, nullptr);
        (void)n48_mib_units(cat.data(), 2u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        const uint32_t from = units[3].start, to = units[3].end;
        std::vector<uint32_t> draws;
        for (uint32_t i = from; i < to; ) { const uint32_t h = cat[i]; uint32_t l = 1;
            if (h != XLAT12_IB_NOP && (h >> 30) == 3u) { l = ((h >> 16) & 0x3FFFu) + 2u; const uint32_t op = (h >> 8) & 0xFFu;
                if (op == 0x2Du || op == 0x27u || op == 0x35u) draws.push_back(i); }
            i += l; }
        // The unit's 5th draw (dword 1244 of the unit) reads a slot an earlier draw redirected. With a NON-proven CONTEXT_CONTROL
        // right before it, the carry is cleared (it may no longer describe Apple's SGPR state): P4 must NOT re-emit - the
        // REDIRECTED rung refuses, exactly as without units. (Measured over every draw: draws 4-7 refuse 0xF9, the rest
        // SLOT_SHARED 0xF2 or later; the carry-bit planted break turns draw 4's 0xF9 into a silent re-emission.)
        for (uint32_t d = 4; d < 5u && d < draws.size(); d++) {
            std::vector<uint32_t> in(cat.begin() + from, cat.begin() + draws[d]);
            in.push_back(0xC0012800u); in.push_back(0x80000002u); in.push_back(0x80000002u);   // a NON-proven CONTEXT_CONTROL
            in.insert(in.end(), cat.begin() + draws[d], cat.begin() + to);
            std::vector<uint32_t> out(in.size()); xlat12_draw_stats ds {}; uint32_t olen = 0;
            xlat12_draw_extra ex {}; base_flags(ex); ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_UNIT;
            ex.ib_va = kU77Ib1Va + 4ull * (from - 1152u); ex.unit = &gU;
            gU.ncons = cc[3];
            for (uint32_t j = 0; j < cc[3]; j++) { const uint32_t h = segs[cf[3] + j].head; gU.cons_in[j] = j ? h - from + (h > draws[d] ? 3u : 0u) : 0u; }
            gU.pool = nullptr; gU.cons_fn = nullptr; gU.pend_cap = 0;
            const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in.data(), (uint32_t)in.size(), out.data(), &olen, &ds);
            if (gPrint) std::printf("      P4 probe: CONTEXT_CONTROL before draw %u (@%u): st %u op %#x at %u unredir %u\n", d, draws[d] - from, st, ds.err_op, ds.err_in_dword, gU.unredir);
            expect_u("P4: a redirected slot the carry does not hold (cleared by a non-proven CONTEXT_CONTROL) refuses REDIRECTED at that draw",
                     st * 1000u + ds.err_op, 29u * 1000u + 0xF9u);
            expect_u("P4: ... at the draw itself (unit dword 1247), with no un-redirect made", ds.err_in_dword * 10u + gU.unredir, 1247u * 10u);
        }
    }
    // ---- the pending-record cap refuses TOO_MANY, never truncates ----
    {
        Cfg on; on.pendCap = 4u;
        const FrameRes B = run_frame(kF77W, kF77N, kF77V, 2u, on);
        if (gPrint) std::printf("      CAP 4: %s unit3 st %u op %#x\n", B.s.c_str(), ures(B, 3).st, ures(B, 3).op);
        expect_u("P3 cap: the 5-block unit with a cap of 4 REFUSES TOO_MANY 0xFA (never truncated)", ures(B, 3).st * 1000u + ures(B, 3).op, 29u * 1000u + 0xFAu);
        expect_u("P3 cap: the 3-block unit under the same cap still translates", ures(B, 2).st, 0u);
    }
    // ---- C2's provenance feed (design Q4): F115 IB0's unit, LIST-BACKED provenance ----
    {
        gProvMode = 1; gAsks = 0; gListProven = 0; gListAsks = 0;
        Cfg on;
        const FrameRes B = run_frame(kF115W, kF115N, kF115V, 1u, on);
        if (gPrint) std::printf("      F115 IB0 list-backed: %s asks %u needing the list %u list-proven %u cb %u\n", B.s.c_str(), gAsks, gListAsks, gListProven, ures(B, 0).cb);
        expect_u("C2 F115 IB0 (list-backed provenance): the unit translates - a later constituent's ask vouched by an earlier one",
                 ures(B, 0).st, 0u);
        expect_u("C2 F115 IB0: every ask for an earlier constituent's target (>= 1) was proven by the frame-local list",
                 (gListAsks > 0u && gListProven == gListAsks) ? 1u : 0u, 1u);
        Cfg nofeed; nofeed.feed = 0;
        const FrameRes C = run_frame(kF115W, kF115N, kF115V, 1u, nofeed);
        if (gPrint) std::printf("      F115 IB0 no feed: %s st %u op %#x at %u\n", C.s.c_str(), ures(C, 0).st, ures(C, 0).op, ures(C, 0).at);
        expect_u("C2 F115 IB0 control: with no frame-local feed the same unit refuses PROVENANCE 0xF7", ures(C, 0).st * 1000u + ures(C, 0).op, 29u * 1000u + 0xF7u);
        gProvMode = 0;
    }
    // ---- C4 / P5: the invalidate precedes the records' first reader when the head does NOT execute ----
    {
        // F77's 4-constituent unit, its head DISGUISED (dword 0 := Apple's 10-dword NOP over the EVENT_WRITE + ACQUIRE_MEM):
        // n48_mib_head_executes answers 0, switch 49 is withheld, and the translator must place its own invalidate inline.
        std::vector<uint32_t> cat(kU77Ib0, kU77Ib0 + 1152u); cat.insert(cat.end(), kU77Ib1, kU77Ib1 + 14944u);
        Cfg on; const FrameRes R0 = run_frame(kF77W, kF77N, kF77V, 2u, on);
        const uint32_t head = ures(R0, 3).head;
        expect_u("P5: a real encoder head executes", (uint32_t)n48_mib_head_executes(cat.data(), head, (uint32_t)cat.size()), 1u);
        std::vector<uint32_t> disg = cat; disg[head] = N48_MIB_NOP_HEAD;
        expect_u("P5: the NOP-disguised head does NOT execute", (uint32_t)n48_mib_head_executes(disg.data(), head, (uint32_t)disg.size()), 0u);
        Cfg d; d.headOverride = disg.data();
        const FrameRes R1 = run_frame(kF77W, kF77N, kF77V, 2u, d);
        std::vector<uint32_t> exec = R1.cand; exec[head] = N48_MIB_NOP_HEAD;   // what the CP runs: the disguise stays in front
        expect_u("C4 (head executes): no inline invalidate - the head's own ACQUIRE_MEM covers", ures(R0, 3).inv, 0u);
        expect_u("C4 (head disguised): the unit still translates, with ONE inline invalidate", ures(R1, 3).st * 10u + ures(R1, 3).inv, 1u);
        expect_u("C4 (head disguised): an EXECUTED covering ACQUIRE_MEM precedes the first draw (the disguised one does not count)",
                 (uint32_t)inv_before_first_draw(exec.data(), head, ures(R1, 3).to, ures(R1, 3).from + ures(R1, 3).firstTpos), 1u);
    }
    // ---- the policy decision and the kext's wiring (source pins beside the reachability above) ----
    expect_u("C2: n48_mib_unit_flag is 0 for a single segment", (uint32_t)n48_mib_unit_flag(1u), 0u);
    expect_u("C2: n48_mib_unit_flag is 1 for 2..XLAT12_UNIT_CONS_MAX constituents", (uint32_t)(n48_mib_unit_flag(2u) && n48_mib_unit_flag(XLAT12_UNIT_CONS_MAX)), 1u);
    {
        char line[600];
        const int w = std::snprintf(line, sizeof line, N48_UNITS480_FMT, "OFF (default)", "`gfxneuter 55` read it only, unchanged",
                                    ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        const int wb = std::snprintf(line, sizeof line, N48_UNITS480B_FMT, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        if (gPrint) std::printf("      units480 / units480b at 20-digit counters: %d / %d bytes\n", w, wb);
        expect_u("units480 line fits the logger's 491-byte body at 20-digit counters", (w > 0 && w < 491) ? 1u : 0u, 1u);
        expect_u("units480b line fits the logger's 491-byte body at 20-digit counters", (wb > 0 && wb < 491) ? 1u : 0u, 1u);
        xlat12_unit mx {}; mx.slack = mx.own_dw = mx.pool_dw = mx.placed = mx.unredir = mx.inv_inline = mx.hs_patched = mx.cb_calls = ~0u;
        // build 0.0.481: the LONGEST why string (a switch-56 retried single, refused)
        const int w2 = std::snprintf(line, sizeof line, N48_UNIT_FMT, N48_UNIT_ARGS(~0ull, ~0u, ~0u, ~0u, ~0u, ~0u, &mx, ~0u, ~0u,
                                                                                   n48_mib_unit_why(2u, 1u)));
        if (gPrint) std::printf("      unit480 at maximum fields: %d bytes\n", w2);
        expect_u("unit480 line fits the logger's 491-byte body at maximum fields", (w2 > 0 && w2 < 491) ? 1u : 0u, 1u);
    }
    if (!ahh) { std::printf("  SKIP 0.0.480 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *s) { uint32_t c = 0; for (size_t p = src.find(s); p != std::string::npos; p = src.find(s, p + 1)) c++; return c; };
    expect_u("PIN 480: the unit stage calls n48_mib_units over the segment stage's answer, once",
             count("gUnitSegs, ns, XLAT12_UNIT_CONS_MAX, segs, gUnitCFirst, gUnitCCount, N48_XV_MAX_SEGS,\n"
                   "                                          gXdBuild.mib ? gXdBuild.leadMask : 0u);"), 1u);   // build 0.0.505 (C2)
    // build 0.0.481: gfxsrc_unit_setup's body is gfx_mib.h's n48_mib_unit_setup now (F3); the flag decision is unchanged
    expect_u("PIN 480: the unit flag is decided by n48_mib_unit_flag alone", count("n48_mib_unit_flag(cc) ? cc : 0u"), 1u);
    expect_u("PIN 480: switch 49's credit is taken back for a head n48_mib_head_executes refuses (P5)",
             count("if ((ex.flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD) && !n48_mib_head_executes(gXdIb, segs[k].head, n)) {\n"
                   "                ex.flags &= ~(uint32_t)XLAT12_EXTRA_DESC_INV_APPLE_HEAD;"), 1u);
    {   // ORDER: the take-back follows switch 49's own statement and precedes the translate call
        const size_t p49 = src.find("if (gDescInvAppleHeadOn) { ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD; gDescInvAppleHeadS.segsOn++; }");
        const size_t pP5 = src.find("!n48_mib_head_executes(gXdIb, segs[k].head, n)");
        const size_t pXl = src.find("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
        expect_u("ORDER 480: P5's take-back sits between switch 49's statement and the translate call",
                 (p49 != std::string::npos && p49 < pP5 && pP5 < pXl) ? 1u : 0u, 1u);
    }
    expect_u("PIN 480: the fence find runs over the final constituent's slice", count("n48_f828_find(out + fs, olen - fs, &fr)"), 1u);
    expect_u("PIN 480: the fence apply runs over the same slice", count("n48_f828_apply(out + fs, olen - fs, &fr,"), 1u);
    expect_u("PIN 480: the MIB-0 fence census asks the same slice", count("n48_f828_find(out + fsC, olen - fsC, &mibfr)"), 1u);
    expect_u("PIN 480: the frame-local feed after a unit covers its last constituent only",
             count("n48_dl_from_output(&gXdFrameLocal, dctxKey, out + flFrom, olen - flFrom, nullptr)"), 1u);
    // build 0.0.481: through gfx_mib.h's n48_mib_unit_undo (a unit OR a switch-56 retried single)
    // build 0.0.522 (switch 76): the same site now takes back the spill tier's records too (gfx_mib.h n48_mib_unit_undo2).
    expect_u("PIN 480: a refused unit's pool records are undone at the restore", count("const uint32_t undone = n48_mib_unit_undo2(isUnit, st, &gUnitPool, &gUnitState);"), 1u);
    expect_u("PIN 480: the per-segment after-step runs only while 55 is on",
             count("if (unitsOn) gfxsrc_unit_after(k, segIb, isUnit, st, ds.err_op, out, olen, &ex, build, dp);"), 1u);
    // ORDER (reachability, not a literal alone): the restore precedes the after-step, which precedes the loop's end
    const size_t pRestore = src.find("if (st && build) memcpy(&gXdNew[from], &gXdIb[from], (size_t)(to - from) * 4u);");
    const size_t pAfter = src.find("if (unitsOn) gfxsrc_unit_after(");
    const size_t pForm = src.find("const uint32_t unitMap = unitsOn ? gfxsrc_units_form(");
    const size_t pNseg = src.find("f->nseg = ns;\n    gXdBuild.nseg = (ns == total");
    const size_t pLoop = src.find("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {\n        const uint32_t from = segs[k].start");
    expect_u("ORDER 480: units are formed before nseg is recorded and before the segment loop",
             (pForm != std::string::npos && pForm < pNseg && pNseg < pLoop) ? 1u : 0u, 1u);
    expect_u("ORDER 480: the pool undo/feed step follows the policy's own restore", (pRestore != std::string::npos && pRestore < pAfter) ? 1u : 0u, 1u);
}

// =====================================================================================================================
// build 0.0.481 (: F2, F3, the census label, switch 56). Every check drives the kext's OWN pure steps
// (gfx_mib.h n48_mib_nseg_of, n48_mib_unit_setup, n48_mib_retry_wanted, n48_mib_retry_single, n48_mib_unit_undo) through
// run_frame in the kext's order over decide44's real bytes, or directly; the kext wiring is pinned by content below.
// =====================================================================================================================
// A translator call that leaves ONE dword of the retried segment unwritten (the sentinel the retry filled survives there),
// or writes the sentinel into a dword the retry placed in the pool: the retry's proof must refuse either.
static uint32_t gFaultAt = 0u;
static uint32_t xlat_leave_one(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    const uint32_t k = gFaultAt < n ? gFaultAt : n - 1u, keep = out[k];
    const uint32_t st = u_xlat(ex, in, n, out, olen, ds);
    if (!st) out[k] = keep;
    return st;
}
static uint32_t xlat_leak_pool(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    const uint32_t st = u_xlat(ex, in, n, out, olen, ds);
    if (!st && gPool.jn) gPool.j[gPool.jn - 1u].host[gPool.j[gPool.jn - 1u].used - 1u] = N48_MIB_RETRY_SENTINEL;
    return st;
}
static void checks481(const char *ahh)
{
    std::printf("\n== 0.0.481: F2, F3, the census label and SWITCH 56 over decide44's real bytes, in the kext's order ==\n");
    gHide491 = 1;   // build 0.0.491: as checks() (see gHide491)
    // ---- F2: THE COUNT A ONE-SEGMENT RULE READS ----
    {
        // F77 IB1's 4-constituent unit [3666,6384) as a ONE-IB frame of its own (real bytes, from its head): the segment
        // stage names 4 segments, switch 55 makes them ONE unit.
        std::vector<uint32_t> cat(kU77Ib0, kU77Ib0 + 1152u); cat.insert(cat.end(), kU77Ib1, kU77Ib1 + 14944u);
        const uint32_t off[2] = { 0u, 1152u }, nn[2] = { 1152u, 14944u }; uint32_t ibns[2], tot = 0; n48_mib_seg_diag dg {};
        xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS]; uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS];
        const uint32_t ns = n48_mib_segment(cat.data(), 2u, off, nn, segs, N48_XV_MAX_SEGS, ibns, &tot, &dg, 0u, nullptr);
        (void)n48_mib_units(cat.data(), 2u, off, nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, 0u);
        const uint32_t h = units[3].head, e = units[3].end;
        static std::vector<uint32_t> one; one.assign(cat.begin() + h, cat.begin() + e);
        const uint32_t *const w[1] = { one.data() }; const uint32_t wn[1] = { e - h }; const uint64_t wv[1] = { kU77Ib1Va + 4ull * (h - 1152u) };
        Cfg on; Cfg off55; off55.units = 0;
        const FrameRes A = run_frame(w, wn, wv, 1u, off55), B = run_frame(w, wn, wv, 1u, on);
        if (gPrint) std::printf("      F2 frame: 55 OFF ns %u nu %u pre %u | 55 ON ns %u nu %u pre %u %s\n", A.ns, A.nu, A.nsegPre, B.ns, B.nu, B.nsegPre, B.s.c_str());
        expect_u("F2: the one-IB frame's segment stage names 4 segments and 55 makes them ONE unit", B.ns * 10u + B.nu, 41u);
        expect_u("F2: the count the kext records BEFORE the unit stage (n48_mib_nseg_of) is the segment stage's 4", B.nsegPre, 4u);
        expect_u("F2: with 55 OFF that count IS the policy's nseg (OFF identity: nsegPre == nseg)", A.nsegPre * 10u + A.nu, 44u);
        n48_fs fs {}; fs.on = 1u; n48_fs_open(&fs);
        const uint64_t cb0 = kN48FsMemberVa[0];
        expect_u("F2: n48_fs_identify_fill over the segment stage's count refuses the merged frame (not a fill)",
                 n48_fs_identify_fill(&fs, B.nsegPre, 1u, cb0), 0u);
        expect_u("F2 hazard (why the fix): over the UNIT count the same frame would pass as a fill (K2's failure)",
                 n48_fs_identify_fill(&fs, B.nu, 1u, cb0), 1u);
        expect_u("F2 control: a real one-segment fill frame still identifies (nsegPre 1)", n48_fs_identify_fill(&fs, 1u, 1u, cb0), 1u);
        n48_cp_consumer ci {};
        expect_u("F2: n48_cp_build_input_free over the segment stage's count refuses it too",
                 n48_cp_build_input_free(&ci, B.nsegPre, 1u, 0u, 0u, 0x23f0a81000ull, 1u, 1u), 0u);
        expect_u("F2 hazard: over the unit count it would build", n48_cp_build_input_free(&ci, B.nu, 1u, 0u, 0u, 0x23f0a81000ull, 1u, 1u), 1u);
    }
    // ---- F3: A UNIT REFUSED BEFORE THE TRANSLATOR'S OWN RESET TAKES BACK NOTHING OF AN EARLIER UNIT ----
    {
        Cfg on; const FrameRes A = run_frame(kF54W, kF54N, kF54V, 1u, on);
        int kp = -1; for (size_t i = 0; i < A.u.size() && kp < 0; i++) if (A.u[i].isUnit && !A.u[i].st && A.u[i].pool) kp = (int)i;
        expect_u("F3 setup: F54 has a translated unit that placed records in the frame pool", kp >= 0 ? 1u : 0u, 1u);
        Cfg inj = on; inj.earlyAfterK = kp;
        const FrameRes B = run_frame(kF54W, kF54N, kF54V, 1u, inj);
        if (gPrint) std::printf("      F3: after F54 unit %d (pool %u dw): early unit st %u, undo took back %u dw\n", kp, kp >= 0 ? A.u[(size_t)kp].pool : 0u, B.earlySt, B.earlyUndone);
        expect_u("F3: the injected unit is refused by translate_draw_ex BEFORE d_unit_reset (DRAW_SHAPE, no draw)", B.earlySt, (uint32_t)XLAT12_IB_ERR_DRAW_SHAPE);
        expect_u("F3: the after-step undo takes back NOTHING (the setup emptied the journal first)", B.earlyUndone, 0u);
        expect_u("F3: the frame candidate is byte-identical to the frame without the injected unit", B.cand == A.cand ? 1u : 0u, 1u);
        expect_u("F3: and every record still verifies where its pointer says", B.vkBlocks * 1000u + B.vkOk, A.vkBlocks * 1000u + A.vkBlocks);
    }
    // ---- SWITCH 56: THE CONDITION (pure) ----
    {
        const uint32_t D = XLAT12_IB_ERR_DESC, L = XLAT12_IB_ERR_TOO_LONG;
        struct { uint32_t on55, on56, map, enc, nc, st, op, want; const char *what; } t[] = {
            { 1, 1, 1, 1, 1, D, XLAT12_TDESC_NO_ROOM, 1, "a single refused NO_ROOM 0xF8 is retried" },
            { 1, 1, 1, 1, 1, L, XLAT12_REEMIT_NO_ROOM, 1, "a single refused REEMIT_NO_ROOM 0xFC is retried" },
            { 1, 1, 1, 1, 1, 0, 0xFFFFFFFFu, 0, "a single that TRANSLATED is never retried" },
            { 1, 1, 1, 1, 1, D, XLAT12_TDESC_PROVENANCE, 0, "PROVENANCE is never retried" },
            { 1, 1, 1, 1, 1, D, XLAT12_TDESC_REDIRECTED, 0, "REDIRECTED is never retried" },
            { 1, 1, 1, 1, 1, D, XLAT12_TDESC_SLOT_UNSEEN, 0, "SLOT_UNSEEN is never retried" },
            { 1, 1, 1, 1, 1, D, XLAT12_TDESC_TOO_MANY, 0, "TOO_MANY is never retried" },
            { 1, 1, 1, 1, 1, L, 0xFFu, 0, "a TOO_LONG at the extra block (0xFF) is never retried" },
            { 1, 1, 1, 1, 1, XLAT12_IB_ERR_PAIR, XLAT12_TDESC_NO_ROOM, 0, "a PAIR refusal is never retried" },
            { 1, 1, 1, 1, 1, N48_SEG_COPY_OVERLAP, XLAT12_TDESC_NO_ROOM, 0, "a copy-guard refusal is never retried" },
            { 1, 1, 1, 1, 2, D, XLAT12_TDESC_NO_ROOM, 0, "a UNIT (2 constituents) is never retried" },
            { 1, 1, 1, 0, 1, D, XLAT12_TDESC_NO_ROOM, 0, "a HEADLESS (non-ENCODER) segment is never retried" },
            { 1, 1, 0, 1, 1, D, XLAT12_TDESC_NO_ROOM, 0, "no unit table this pass: nothing is retried" },
            { 0, 1, 1, 1, 1, D, XLAT12_TDESC_NO_ROOM, 0, "55 OFF: 56 is inert" },
            { 1, 0, 1, 1, 1, D, XLAT12_TDESC_NO_ROOM, 0, "56 OFF: nothing is retried" },
        };
        for (const auto &c : t) {
            char lbl[160]; std::snprintf(lbl, sizeof lbl, "56 COND: %s", c.what);
            expect_u(lbl, (uint32_t)n48_mib_retry_wanted(c.on55, c.on56, c.map, c.enc, c.nc, c.st, c.op), c.want);
        }
        expect_u("56: the state string says INERT when 55 is OFF",
                 std::strstr(n48_mib_retry_state(0u, 1u), "INERT") != nullptr && std::strcmp(n48_mib_retry_state(1u, 1u), "ON") == 0 ? 1u : 0u, 1u);
    }
    // ---- SWITCH 56 in the kext's order over real frames ----
    {
        struct Fx { const uint32_t *const *w; const uint32_t *n; const uint64_t *v; uint32_t nib; const char *name; } fx[] = {
            { kF77W, kF77N, kF77V, 2u, "F77" }, { kF54W, kF54N, kF54V, 1u, "F54" }, { kF59W, kF59N, kF59V, 1u, "F59" },
            { kF115W, kF115N, kF115V, 1u, "F115" } };
        uint32_t singlesOk = 0, singlesSame = 0, unitsN = 0, unitsSame = 0, retries = 0, retriedOk = 0, nonRoom = 0, nonRoomRetried = 0;
        uint32_t sent = 0, vkb = 0, vkok = 0, vkbad = 0;
        for (const Fx &f : fx) {
            Cfg on55; Cfg on56; on56.retry = 1; Cfg off; off.units = 0; Cfg off56only = off; off56only.retry = 1;
            const FrameRes A = run_frame(f.w, f.n, f.v, f.nib, on55), B = run_frame(f.w, f.n, f.v, f.nib, on56);
            const FrameRes O = run_frame(f.w, f.n, f.v, f.nib, off), O6 = run_frame(f.w, f.n, f.v, f.nib, off56only);
            if (gPrint) std::printf("      %s 55 ON %s | 55+56 ON %s (retries %u, blocks %u ok %u)\n", f.name, A.s.c_str(), B.s.c_str(), B.retries, B.vkBlocks, B.vkOk);
            {   char lbl[160]; std::snprintf(lbl, sizeof lbl, "56 INERT with 55 OFF (%s): no retry, candidate byte-identical to 55+56 OFF", f.name);
                expect_u(lbl, (O6.retries == 0u && O6.cand == O.cand && O6.s == O.s) ? 1u : 0u, 1u); }
            for (size_t i = 0; i < B.u.size() && i < A.u.size(); i++) {
                const UnitRes &a = A.u[i], &b = B.u[i];
                if (a.cons == 1u && a.st == 0u) { singlesOk++; singlesSame += (b.st == 0u && b.out == a.out && b.retryWhy == N48_MIB_RETRY_NOT) ? 1u : 0u; }
                if (a.cons >= 2u) { unitsN++; unitsSame += (b.st == a.st && b.out == a.out) ? 1u : 0u; }
                const int room = a.cons == 1u && ((a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_NO_ROOM) || (a.st == XLAT12_IB_ERR_TOO_LONG && a.op == XLAT12_REEMIT_NO_ROOM));
                if (room) { retries += (b.retryWhy != N48_MIB_RETRY_NOT) ? 1u : 0u; retriedOk += b.st == 0u ? 1u : 0u; }
                if (a.cons == 1u && a.st && !room) { nonRoom++; nonRoomRetried += (b.retryWhy != N48_MIB_RETRY_NOT) ? 1u : 0u; }
            }
            sent += B.sentinels; vkb += B.vkBlocks; vkok += B.vkOk; vkbad += B.vkBad + B.vkNotNop + B.vkOverlap + B.vkEntBad;
            expect_u(f.name, n48_f828_walk_ok(B.cand.data(), (uint32_t)B.cand.size(), nullptr), 1u);
        }
        if (gPrint) std::printf("      56 over 4 frames: singles ok %u same %u; units %u same %u; room singles retried %u (translated %u); non-room singles %u retried %u; blocks %u ok %u bad %u; sentinels %u\n",
                                singlesOk, singlesSame, unitsN, unitsSame, retries, retriedOk, nonRoom, nonRoomRetried, vkb, vkok, vkbad, sent);
        expect_u("56 ON IDENTITY: every single that translates with 56 OFF is byte-identical with 56 ON (and never retried)", singlesOk * 1000u + singlesSame, singlesOk * 1001u);
        expect_u("56 ON IDENTITY: ... and there are such singles (non-vacuous)", singlesOk >= 4u ? 1u : 0u, 1u);
        expect_u("56: every unit translates byte-identically with 56 ON", unitsN * 1000u + unitsSame, unitsN * 1001u);
        expect_u("56: every room-refused single is retried, and at least one translates", (retries >= 1u && retriedOk >= 1u) ? 1u : 0u, 1u);
        expect_u("56: no single refused for any other reason is retried (and there are such singles)", nonRoom * 1000u + nonRoomRetried, nonRoom * 1000u);
        expect_u("56: ... at least one non-room single refusal is present (non-vacuous)", nonRoom >= 1u ? 1u : 0u, 1u);
        expect_u("56: every deferred record over the four frames lands where its pointer says, in a NOP body, no overlap, entry pointers right",
                 (vkb > 0u && vkok == vkb && vkbad == 0u) ? 1u : 0u, 1u);
        expect_u("56: no sentinel word survives anywhere in any frame candidate", sent, 0u);
    }
    {
        // F77: k7 (the single refused NO_ROOM) retried through the unit path translates, records partly in the pool
        Cfg on56; on56.retry = 1;
        const FrameRes B = run_frame(kF77W, kF77N, kF77V, 2u, on56);
        const UnitRes &k7 = ures(B, 7);
        if (gPrint) std::printf("      F77 56 ON: %s k7 why %u st %u placed %u own %u pool %u\n", B.s.c_str(), k7.retryWhy, k7.st, k7.placed, k7.own, k7.pool);
        expect_u("56 F77: ?m|............ (k7 retried and translated)", B.s == "?m|............" ? 1u : 0u, 1u);
        expect_u("56 F77 k7: retried (why OK), a one-constituent unit, records placed with some in the pool",
                 k7.retryWhy * 1000u + k7.isUnit * 100u + (k7.placed > 0u) * 10u + (k7.pool > 0u), (uint32_t)N48_MIB_RETRY_OK * 1000u + 211u);
        uint32_t refused = 0; for (const UnitRes &u : B.u) refused += u.st ? 1u : 0u;
        expect_u("56 F77: the only refused row left is IB0's first unit (its '?', not a room refusal); the gate says SEG_REFUSED for it alone",
                 refused * 1000u + ures(B, 0).st * 0u + (ures(B, 0).st != 0u) * 100u + (B.gate == (uint32_t)N48_CM_SEG_REFUSED), 1101u);
        expect_u("56 F77 k7: its final-constituent fence slice is the whole segment (fence slice start 0)", k7.lastHead, 0u);
        // REFUSED LATER: the copy guard refuses the retried k7 after it translated -> its pool records are undone and the
        // frame candidate is EXACTLY 56 OFF's (k7 refused and restored there)
        Cfg cg = on56; cg.cgRefuse = 7;
        const FrameRes C = run_frame(kF77W, kF77N, kF77V, 2u, cg);
        Cfg on55; const FrameRes A = run_frame(kF77W, kF77N, kF77V, 2u, on55);
        if (gPrint) std::printf("      F77 56 ON, k7 refused by the copy guard: %s undone %u (pool %u)\n", C.s.c_str(), ures(C, 7).undone, ures(C, 7).pool);
        expect_u("56 REFUSED LATER: the retried k7 refused by the copy guard takes back exactly its pool dwords", ures(C, 7).undone * 10u + (ures(C, 7).pool > 0u),
                 ures(C, 7).pool * 10u + 1u);
        expect_u("56 REFUSED LATER: the frame candidate is byte-identical to 55 ON / 56 OFF's", C.cand == A.cand ? 1u : 0u, 1u);
        // THE SENTINEL: a translator that leaves one dword unwritten, or leaks the sentinel into a pool dword, is refused
        for (uint32_t at : { 0u, 17u, 500u, 1133u }) {
            gFaultAt = at;
            Cfg bad = on56; bad.xlat = &xlat_leave_one;
            const FrameRes S = run_frame(kF77W, kF77N, kF77V, 2u, bad);
            char lbl[160]; std::snprintf(lbl, sizeof lbl, "56 SENTINEL: a retry that leaves dword %u unwritten is refused VERIFY 0xE4 at that dword", at);
            expect_u(lbl, ures(S, 7).retryWhy * 1000000u + ures(S, 7).st * 10000u + (ures(S, 7).op == N48_MIB_RETRY_SENTINEL_OP) * 1000u + (ures(S, 7).at == (at < ures(S, 7).to - ures(S, 7).from ? at : ures(S, 7).to - ures(S, 7).from - 1u)),
                     (uint32_t)N48_MIB_RETRY_SENTINEL_LEFT * 1000000u + (uint32_t)XLAT12_IB_ERR_VERIFY * 10000u + 1001u);
            std::snprintf(lbl, sizeof lbl, "56 SENTINEL (dword %u): nothing survives - the candidate is 56 OFF's, no sentinel anywhere", at);
            expect_u(lbl, (S.cand == A.cand && S.sentinels == 0u) ? 1u : 0u, 1u);
        }
        {
            Cfg leak = on56; leak.xlat = &xlat_leak_pool;
            const FrameRes S = run_frame(kF77W, kF77N, kF77V, 2u, leak);
            expect_u("56 SENTINEL LEAK: a sentinel in a pool dword the retry placed refuses it (err_in_dword = n, past the segment)",
                     ures(S, 7).retryWhy * 10u + (ures(S, 7).at == ures(S, 7).to - ures(S, 7).from), (uint32_t)N48_MIB_RETRY_SENTINEL_LEFT * 10u + 1u);
            expect_u("56 SENTINEL LEAK: the pool is undone and the candidate is 56 OFF's", (S.cand == A.cand && S.sentinels == 0u) ? 1u : 0u, 1u);
        }
        // an input that itself holds the sentinel word is not retried at all (its first refusal stands)
        {
            std::vector<uint32_t> ib1(kU77Ib1, kU77Ib1 + 14944u);
            const uint32_t k7from = ures(A, 7).from - 1152u;
            uint32_t at = 0;
            for (uint32_t i = k7from; i < 14944u && !at; ) {   // the first multi-dword NOP body in k7's input
                const uint32_t hh = ib1[i];
                if (hh == XLAT12_IB_NOP || (hh >> 30) != 3u) { i++; continue; }
                if (((hh >> 8) & 0xffu) == 0x10u && ((hh >> 16) & 0x3fffu) >= 1u) { at = i + 1u; break; }
                i += ((hh >> 16) & 0x3fffu) + 2u;
            }
            expect_u("56 SENTINEL-IN setup: k7's input has a NOP body to carry the word", at ? 1u : 0u, 1u);
            ib1[at] = N48_MIB_RETRY_SENTINEL;
            const uint32_t *const w[2] = { kU77Ib0, ib1.data() };
            const FrameRes S = run_frame(w, kF77N, kF77V, 2u, on56);
            expect_u("56 SENTINEL-IN: an input holding the sentinel word is not retried (NO_ROOM stands)",
                     ures(S, 7).retryWhy * 1000u + (ures(S, 7).st == XLAT12_IB_ERR_DESC && ures(S, 7).op == XLAT12_TDESC_NO_ROOM),
                     (uint32_t)N48_MIB_RETRY_SENTINEL_IN * 1000u + 1u);
        }
    }
    // ---- the census label ----
    expect_u("LABEL: n48_mib_units_mark is \" units\" for a pass that formed units, \"\" otherwise",
             (std::strcmp(n48_mib_units_mark(1u), " units") == 0 && n48_mib_units_mark(0u)[0] == '\0') ? 1u : 0u, 1u);
    {
        char line[700];
        const int w = std::snprintf(line, sizeof line, N48_RETRY481_FMT, n48_mib_retry_state(0u, 1u),
                                    "`gfxneuter 56` REFUSED - a continuous arm stands",   /* the longest `how` any call site passes */
                                    ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        if (gPrint) std::printf("      retry481 at 20-digit counters and the longest state/how: %d bytes\n", w);
        expect_u("retry481 line fits the logger's 491-byte body at 20-digit counters", (w > 0 && w < 491) ? 1u : 0u, 1u);
        uint32_t mw = 0; for (uint32_t a : { 0u, 1u }) for (uint32_t b : { 0u, 1u }) { const uint32_t l = (uint32_t)std::strlen(n48_mib_retry_state(a, b)); if (l > mw) mw = l; }
        expect_u("retry481: the measured state string is the longest one", (uint32_t)std::strlen(n48_mib_retry_state(0u, 1u)), mw);
    }
    // ---- the kext wiring (pins by content, beside the reachability above) ----
    if (!ahh) { std::printf("  SKIP 0.0.481 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *s) { uint32_t c = 0; for (size_t p = src.find(s); p != std::string::npos; p = src.find(s, p + 1)) c++; return c; };
    auto at = [&](const char *s) { return src.find(s); };
    // build 0.0.530: the identity is computed inside the pure dispatcher n48_fs85_frame (gfx_fs85.h), handed nsegPre.
    expect_u("PIN 481 F2: the fill identity reads the segment stage's count, and only that", count("fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane, tgtVa,") * 10u + count("fsEligible, gXdBuild.nseg,") + count("n48_fs_identify_fill(&gFs, gXdBuild.nseg"), 10u);
    expect_u("PIN 481 F2: the input-free builder reads the same count", count("n48_cp_build_input_free(&fi, gXdBuild.nsegPre, gXdBuild.fill,") * 10u + count("n48_cp_build_input_free(&fi, gXdBuild.nseg,"), 10u);
    {
        const size_t pPre = at("const uint32_t nsegPre = n48_mib_nseg_of(ns, total, N48_XV_MAX_SEGS);");
        const size_t pForm = at("const uint32_t unitMap = unitsOn ? gfxsrc_units_form(");
        const size_t pRec = at("gXdBuild.nsegPre = nsegPre; gXdBuild.units = unitMap;");
        const size_t pLoop = at("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {\n        const uint32_t from = segs[k].start");
        expect_u("ORDER 481 F2: nsegPre is taken BEFORE the unit stage, recorded after it, before the segment loop",
                 (pPre != std::string::npos && pPre < pForm && pForm < pRec && pRec < pLoop) ? 1u : 0u, 1u);
        expect_u("PIN 481: nsegPre and units are cleared wherever nseg is (the pass top and the policy-skipped branch)",
                 count("gXdBuild.nsegPre = 0u; gXdBuild.units = 0u;"), 2u);
    }
    expect_u("PIN 481 F3: gfxsrc_unit_setup is n48_mib_unit_setup with the frame pool as the journal it empties",
             count("return n48_mib_unit_setup(ex, &gUnitState, &gUnitPool, (dp && build) ? 1 : 0,"), 1u);
    expect_u("PIN 481 F3: ... called for EVERY segment of a pass that formed units", count("uint32_t isUnit = unitMap ? gfxsrc_unit_setup(&ex, k, from, dp, build) : 0u;"), 1u);
    expect_u("PIN 481 56: the switch is read once per pass", count("const uint32_t retryOn = gRetryOn;"), 1u);
    expect_u("PIN 481 56: the retry condition is n48_mib_retry_wanted over 55, 56, the unit table, the kind and this row",
             count("if (retryOn && n48_mib_retry_wanted(unitsOn, retryOn, unitMap, gXdBuild.kind == N48_CM_KIND_ENCODER ? 1u : 0u,"), 1u);
    expect_u("PIN 481 56: the retry is n48_mib_retry_single over the SAME translator call and the unit state/pool",
             count("n48_mib_retry_single(&gfxsrc_xlat_m2tri, ex, &gUnitState, &gUnitPool, (dp && build) ? 1 : 0,"), 1u);
    {
        const size_t pXl = at("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
        const size_t pRt = at("st = gfxsrc_unit_retry(&ex, k, from, to - from, out, &olen, &ds, st, dp, build, &rwhy);");
        const size_t pCg = at("const uint32_t cgReason = navi48_cg_seg_check();");
        const size_t pSt = at("gMibSegSt[k] = st; gMibSegOp[k] = ds.err_op;");
        const size_t pFl = at("n48_dl_from_output(&gXdFrameLocal, dctxKey, out + flFrom, olen - flFrom, nullptr)");
        const size_t pFe = at("const uint32_t why = gXdBuild.fence71 ? n48_f828_find_last(out + fs, olen - fs, &fr)");   // build 0.0.508: the switch-16 search (71 chooses)
        const size_t pRs = at("if (st && build) memcpy(&gXdNew[from], &gXdIb[from], (size_t)(to - from) * 4u);");
        const size_t pAf = at("if (unitsOn) gfxsrc_unit_after(k, segIb, isUnit, st, ds.err_op, out, olen, &ex, build, dp);");
        expect_u("ORDER 481 56: translate < retry < copy guard < status record < frame-local feed < fence < restore < after-step (undo)",
                 (pXl != std::string::npos && pXl < pRt && pRt < pCg && pCg < pSt && pSt < pFl && pFl < pFe && pFe < pRs && pRs < pAf) ? 1u : 0u, 1u);
        expect_u("PIN 481 56: a retried single is marked 2 so the fence slice, the feed and the after-step treat it as a unit",
                 count("if (rwhy == N48_MIB_RETRY_OK || rwhy == N48_MIB_RETRY_REFUSED || rwhy == N48_MIB_RETRY_SENTINEL_LEFT) isUnit = 2u;"), 1u);
    }
    expect_u("PIN 481 56: the verb's own guard (56u) and its ON/OFF/read values", count("n48_cm_cont_switch_refused(56u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("PIN 481 56: the report line on every gfxneuter report", count("retry_report_line(\"gfxneuter report\");"), 1u);
    expect_u("PIN 481 LABEL: the mibseg-detail line carries the units marker of THIS pass",
             count("HWLOG(N48_MIBSEG_DETAIL_FMT, (unsigned long long)(gXdC.judged + 1u), f->nib, nsClamped, n48_mib_units_mark(unitMap),"), 1u);
    expect_u("PIN 481 LABEL: the WSF line carries it only when the policy ran for THIS frame", count("n48_mib_units_mark(ranPolicy ? gXdBuild.units : 0u));"), 1u);
    // 0.0.448's one-frame property (the policy folded into gfxsrc_decide_frame): 0.0.480 had separated the attribute from
    // the function by inserting its helpers in between. The attribute must sit directly on gfxsrc_policy, and only there.
    expect_u("PIN 481 STACK: always_inline sits directly on gfxsrc_policy (0.0.448's single submit frame)",
             count("__attribute__((always_inline))\nstatic void gfxsrc_policy(const GfxcVm &vm,") * 10u + count("__attribute__((always_inline))\n"), 11u);
}

// build 0.0.491 — THE LIVE ROWS. decide44's frames with Z/AO/AN/AF's kDTableAbi rows live, as the
// 0.0.491 translator runs them (switch 43 ON: this test's resolver never gates a row). The draws of those four programs
// now take the table step and refuse READ 0xF4 ('R') OFFLINE, because the decide44 fixture never recorded their table
// memory - the kext never read it then (no row). On hardware the reads are live. The strings below are those frames' new
// strings; the attribution check at the end reproduces the OLD strings exactly with only the four rows hidden.
static void checks491()
{
    std::printf("\n== 0.0.491: decide44's frames with the Z/AO/AN/AF table rows live ==\n");
    expect_u("491 setup: all four rows exist (Z, AO, AN, AF found by their own ndw/fnv)",
             (xlat12_table_abi_find(54u, 0xf91e4deeu) && xlat12_table_abi_find(168u, 0xb4fc3c24u) &&
              xlat12_table_abi_find(122u, 0x7b3a6dfeu) && xlat12_table_abi_find(173u, 0x1051f3f6u)) ? 1u : 0u, 1u);
    gHide491 = 0;
    const uint32_t miss0 = gMiss;
    Cfg off; off.units = 0; Cfg on; Cfg sd; sd.singles = 1; Cfg on56; on56.retry = 1;
    // build 0.0.502: the same unit configurations with PACK (switch 67) ON - see the CHANGED PIN below
    Cfg onP = on; onP.pack = 1u; Cfg sdP = sd; sdP.pack = 1u; Cfg on56P = on56; on56P.pack = 1u;
    const FrameRes A77 = run_frame(kF77W, kF77N, kF77V, 2u, off), B77 = run_frame(kF77W, kF77N, kF77V, 2u, on);
    const FrameRes S77 = run_frame(kF77W, kF77N, kF77V, 2u, sd), R77 = run_frame(kF77W, kF77N, kF77V, 2u, on56);
    const FrameRes A54 = run_frame(kF54W, kF54N, kF54V, 1u, off), B54 = run_frame(kF54W, kF54N, kF54V, 1u, on);
    const FrameRes B77p = run_frame(kF77W, kF77N, kF77V, 2u, onP), S77p = run_frame(kF77W, kF77N, kF77V, 2u, sdP);
    const FrameRes R77p = run_frame(kF77W, kF77N, kF77V, 2u, on56P), B54p = run_frame(kF54W, kF54N, kF54V, 1u, onP);
    const uint32_t missLive = gMiss - miss0;
    if (gPrint) std::printf("      491 live: F77 OFF %s ON %s what-if %s 56 %s | F54 OFF %s gate %u ON %s gate %u | fixture misses %u\n",
                            A77.s.c_str(), B77.s.c_str(), S77.s.c_str(), R77.s.c_str(), A54.s.c_str(), A54.gate, B54.s.c_str(), B54.gate, missLive);
    if (gPrint) std::printf("      491 live, pack ON: F77 ON %s what-if %s 56 %s | F54 ON %s gate %u\n",
                            B77p.s.c_str(), S77p.s.c_str(), R77p.s.c_str(), B54p.s.c_str(), B54p.gate);
    // each: the NEW string with the rows live. Cause (every 'R'): the fixture has no table memory for Z/AO/AN/AF's draws.
    expect_u("491 F77 55 OFF (rows live): ?X|..XRXXX..RXM - Z/AO/AN/AF draws refuse READ offline (fixture never recorded their table memory)",
             A77.s == "?X|..XRXXX..RXM" ? 1u : 0u, 1u);
    // build 0.0.502: CHANGED PIN (pack OFF). Every translation with a ring now writes gfx12 DB_SPI_VRS_CENTER_LOCATION
    // (0x28068) = 0 once before its first draw - mesa's gfx12 preamble value, ac_cmdbuf.c:702 `ac_pm4_set_reg(pm4,
    // R_028068_DB_SPI_VRS_CENTER_LOCATION, 0);` - which costs a unit ONE dword (merged in front of its DB_SHADER_CONTROL).
    // With the rows live, F77's and F54's unit 1 (k1) sat exactly at the room limit, so with pack OFF it now refuses
    // TDESC_NO_ROOM ('M', 's known cost, reported not hidden) where 0.0.501 translated it ('.'). THE RESTORE
    // CHECK: with PACK ON (switch 67, 0.0.501) the SAME configurations give 0.0.501's strings back exactly (B77p..B54p).
    expect_u("491 F77 55 ON  (rows live, pack OFF): ?m|.MmRmmm..RmM - 0.0.502: k1 NO_ROOM (the VRS dword); the units holding Z/AO/AN/AF draws refuse READ",
             B77.s == "?m|.MmRmmm..RmM" ? 1u : 0u, 1u);
    expect_u("491 F77 what-if (rows live, pack OFF): ?m|.MmRmmm..Rm. - 0.0.502: k1 NO_ROOM; the same cause", S77.s == "?m|.MmRmmm..Rm." ? 1u : 0u, 1u);
    expect_u("491 F77 56 ON (rows live, pack OFF): ?m|.MmRmmm..Rm. - 0.0.502: k1 NO_ROOM; k7 still retried and translated", R77.s == "?m|.MmRmmm..Rm." ? 1u : 0u, 1u);
    expect_u("491 F54 55 OFF (rows live): R.XRXX, the gate still refuses (SEG_REFUSED) - the same cause",
             (A54.s == "R.XRXX" ? 1u : 0u) * 10u + (A54.gate == (uint32_t)N48_CM_SEG_REFUSED), 11u);
    expect_u("491 F54 55 ON  (rows live, pack OFF): RMmRmm, the gate refuses (SEG_REFUSED) - 0.0.502: k1 NO_ROOM (the VRS dword)",
             (B54.s == "RMmRmm" ? 1u : 0u) * 10u + (B54.gate == (uint32_t)N48_CM_SEG_REFUSED), 11u);
    expect_u("491 + 0.0.502 RESTORE (pack ON): F77 ?m|...Rmmm..RmM / ?m|...Rmmm..Rm. / 56 ?m|...Rmmm..Rm.; F54 R..Rmm SEG_REFUSED - 0.0.501's strings exactly",
             (B77p.s == "?m|...Rmmm..RmM" && S77p.s == "?m|...Rmmm..Rm." && R77p.s == "?m|...Rmmm..Rm." &&
              B54p.s == "R..Rmm" && B54p.gate == (uint32_t)N48_CM_SEG_REFUSED) ? 1u : 0u, 1u);
    expect_u("491: the fixture misses reads with the rows live (the READ refusals are fixture gaps)", missLive > 0u ? 1u : 0u, 1u);
    // ATTRIBUTION: the SAME frames with ONLY the four rows hidden (the test-local resolver) reproduce the OLD strings and
    // gates exactly - so every change above is those four rows, nothing else in 0.0.491.
    gHide491 = 1;
    const uint32_t miss1 = gMiss;
    const FrameRes a77 = run_frame(kF77W, kF77N, kF77V, 2u, off), b77 = run_frame(kF77W, kF77N, kF77V, 2u, on);
    const FrameRes s77 = run_frame(kF77W, kF77N, kF77V, 2u, sd), r77 = run_frame(kF77W, kF77N, kF77V, 2u, on56);
    const FrameRes a54 = run_frame(kF54W, kF54N, kF54V, 1u, off), b54 = run_frame(kF54W, kF54N, kF54V, 1u, on);
    const uint32_t missHidden = gMiss - miss1;
    // every row whose status the four rows changed now refuses READ 0xF4 (never another reason), row by row, frame by frame
    uint32_t changed = 0, changedRead = 0, shapeOk = 1;
    // build 0.0.502: the unit configurations compared with PACK ON (B77p..B54p): with pack OFF the VRS dword's NO_ROOM
    // (pinned above) would be a second change besides the four rows; pack ON removes it, so this stays the rows' attribution
    const FrameRes *L[6] = { &A77, &B77p, &S77p, &R77p, &A54, &B54p }, *H[6] = { &a77, &b77, &s77, &r77, &a54, &b54 };
    for (uint32_t f = 0; f < 6u; f++) {
        if (L[f]->u.size() != H[f]->u.size()) { shapeOk = 0; continue; }
        for (size_t i = 0; i < L[f]->u.size(); i++)
            if (L[f]->u[i].st != H[f]->u[i].st || L[f]->u[i].op != H[f]->u[i].op) {
                changed++;
                changedRead += (L[f]->u[i].st == (uint32_t)XLAT12_IB_ERR_DESC && L[f]->u[i].op == (uint32_t)XLAT12_TDESC_READ) ? 1u : 0u;
            }
    }
    expect_u("491: every row the four table rows changed now refuses READ 0xF4 and nothing else (and there are such rows)",
             (shapeOk && changed > 0u && changedRead == changed) ? 1u : 0u, 1u);
    if (gPrint) std::printf("      491 hidden: F77 OFF %s ON %s what-if %s 56 %s | F54 OFF %s gate %u ON %s gate %u | fixture misses %u\n",
                            a77.s.c_str(), b77.s.c_str(), s77.s.c_str(), r77.s.c_str(), a54.s.c_str(), a54.gate, b54.s.c_str(), b54.gate, missHidden);
    expect_u("491 ATTRIBUTION: with ONLY Z/AO/AN/AF's rows hidden the old strings and gates reproduce exactly "
             "(F77 ?X|..X.XXX...XM / ?m|...........M / ?m|............ / 56 ?m|............; F54 ..X.XX SEG_REFUSED / ...... OK)",
             (a77.s == "?X|..X.XXX...XM" && b77.s == "?m|...........M" && s77.s == "?m|............" && r77.s == "?m|............" &&
              a54.s == "..X.XX" && a54.gate == (uint32_t)N48_CM_SEG_REFUSED && b54.s == "......" && b54.gate == (uint32_t)N48_CM_OK) ? 1u : 0u, 1u);
    gHide491 = 0;
}

// =====================================================================================================================
// build 0.0.501 (notes/design/UNIT-ROOM.md Q3) — SWITCH 67, PACK, IN THE KEXT'S ORDER over decide44's
// real bytes (the 481 mechanics: gHide491 = 1): run_frame latches `pack` into the unit state once per frame, as the kext
// latches gUnitState.pack once per pass, so every unit AND switch 56's retry (n48_mib_retry_single, the same gU) run it.
// =====================================================================================================================
static void checks501(const char *ahh)
{
    std::printf("\n== 0.0.501: switch 67 (PACK) over decide44's real bytes, in the kext's order ==\n");
    gHide491 = 1;
    struct Fx { const uint32_t *const *w; const uint32_t *n; const uint64_t *v; uint32_t nib; const char *name; } fx[] = {
        { kF77W, kF77N, kF77V, 2u, "F77" }, { kF54W, kF54N, kF54V, 1u, "F54" }, { kF59W, kF59N, kF59V, 1u, "F59" },
        { kF115W, kF115N, kF115V, 1u, "F115" } };
    uint32_t sameS = 0, differ = 0, vkb = 0, vkok = 0, vkbad = 0, sent = 0, runs = 0, recs = 0, placed = 0, poolUnits = 0, walkOk = 0;
    uint32_t offRuns = 0, offRecs = 0;
    for (const Fx &f : fx) {
        Cfg p0; p0.retry = 1; Cfg p1 = p0; p1.pack = 1u;
        const FrameRes B0 = run_frame(f.w, f.n, f.v, f.nib, p0), B1 = run_frame(f.w, f.n, f.v, f.nib, p1);
        if (gPrint) std::printf("      501 %s 55+56: pack OFF %s | pack ON %s (blocks %u ok %u)\n", f.name, B0.s.c_str(), B1.s.c_str(), B1.vkBlocks, B1.vkOk);
        sameS += (B1.s == B0.s && B1.gate == B0.gate) ? 1u : 0u;
        differ += (B1.cand != B0.cand) ? 1u : 0u;
        vkb += B1.vkBlocks; vkok += B1.vkOk; vkbad += B1.vkBad + B1.vkNotNop + B1.vkOverlap + B1.vkEntBad; sent += B1.sentinels + B0.sentinels;
        walkOk += n48_f828_walk_ok(B1.cand.data(), (uint32_t)B1.cand.size(), nullptr) ? 1u : 0u;
        for (const UnitRes &u : B1.u) if (u.isUnit && !u.st) { runs += u.pkRuns; recs += u.pkRecs; placed += u.placed; poolUnits += u.pool ? 1u : 0u; }
        for (const UnitRes &u : B0.u) if (u.isUnit && !u.st) { offRuns += u.pkRuns; offRecs += u.pkRecs; }
    }
    if (gPrint) std::printf("      501 over 4 frames: same strings %u, candidates changed %u; blocks %u ok %u bad %u; NOPs %u records %u (placed %u), units using the pool %u\n",
                            sameS, differ, vkb, vkok, vkbad, runs, recs, placed, poolUnits);
    expect_u("501 T2: pack ON gives every frame the SAME row strings and gate as pack OFF (decide44's units fit either way)", sameS, 4u);
    expect_u("501 T3 (host): pack ON changes the candidate (records moved into shared NOPs) - so pack OFF is not pack ON", differ >= 1u ? 1u : 0u, 1u);
    expect_u("501 T4 (host): every record with pack ON lands where its pointer says, inside ONE NOP body, no overlap, entry pointers right",
             (vkb > 0u && vkok == vkb && vkbad == 0u) ? 1u : 0u, 1u);
    expect_u("501: every frame candidate with pack ON walks (every packet header and NOP count consistent)", walkOk, 4u);
    expect_u("501: every record placed is a PACK record, and some NOP carries two or more (runs < records)",
             (recs == placed && recs > 0u && runs < recs) ? 1u : 0u, 1u);
    expect_u("501: with pack OFF the PACK counters stay 0 (d_unit_finish's own loop ran)", offRuns * 1000u + offRecs, 0u);
    expect_u("501: no sentinel word anywhere, pack ON or OFF", sent, 0u);
    // T5d: switch 56's retry with pack ON (F77 k7: the single refused NO_ROOM, retried through the unit path, records in the pool)
    {
        Cfg on56; on56.retry = 1; on56.pack = 1u;
        const FrameRes B = run_frame(kF77W, kF77N, kF77V, 2u, on56);
        const UnitRes &k7 = ures(B, 7);
        if (gPrint) std::printf("      501 F77 56+67 ON: %s k7 why %u st %u placed %u own %u pool %u NOPs %u\n", B.s.c_str(), k7.retryWhy, k7.st, k7.placed, k7.own, k7.pool, k7.pkRuns);
        expect_u("501 T5d: F77 k7 retried with pack ON translates (why OK), a unit with records in its own run and the pool, and NO sentinel survives",
                 k7.retryWhy * 10000u + (k7.st == 0u) * 1000u + (k7.pool > 0u) * 100u + (k7.pkRecs == k7.placed) * 10u + (B.sentinels == 0u),
                 (uint32_t)N48_MIB_RETRY_OK * 10000u + 1111u);
        Cfg on56off = on56; on56off.pack = 0u;
        const FrameRes A = run_frame(kF77W, kF77N, kF77V, 2u, on56off);
        for (uint32_t at : { 0u, 500u }) {
            gFaultAt = at;
            Cfg bad = on56; bad.xlat = &xlat_leave_one;
            const FrameRes S = run_frame(kF77W, kF77N, kF77V, 2u, bad);
            Cfg on55; on55.pack = 1u; const FrameRes N = run_frame(kF77W, kF77N, kF77V, 2u, on55);
            char lbl[160]; std::snprintf(lbl, sizeof lbl, "501 T5d SENTINEL (pack ON): a retry leaving dword %u unwritten is refused and nothing survives", at);
            expect_u(lbl, (ures(S, 7).retryWhy == N48_MIB_RETRY_SENTINEL_LEFT && S.cand == N.cand && S.sentinels == 0u) ? 1u : 0u, 1u);
        }
        (void)A;
    }
    // the unitpack67 line at 20-digit counters and the longest how/note
    {
        char line[700];
        const int w = std::snprintf(line, sizeof line, N48_CM_UNITPACK_FMT, "OFF (default)",
                                    " - `gfxneuter 67` REFUSED: a continuous arm stands, unchanged", " - INERT: needs 55 ON",
                                    ~0ull, ~0ull, ~0ull, ~0ull);
        if (gPrint) std::printf("      unitpack67 at 20-digit counters: %d bytes\n", w);
        expect_u("501: the unitpack67 line fits the logger's 491-byte body at 20-digit counters", (w > 0 && w < 491) ? 1u : 0u, 1u);
        expect_u("501: switch 67 is mid-arm guarded; a read is not refused while armed",
                 n48_cm_cont_switch_guarded(67u) * 10u + n48_cm_cont_switch_refused(67u, 1u, 1u, N48_CM_SHOT_ARMED), 10u);
        uint32_t v = 7u;
        expect_u("501: 67's values: M 1 ON, M 2 OFF, M 0/3 unchanged (n48_ra_set)",
                 (n48_ra_set(1u, &v) && v == 1u && n48_ra_set(2u, &v) && v == 0u && !n48_ra_set(0u, &v) && !n48_ra_set(3u, &v) && v == 0u) ? 1u : 0u, 1u);
    }
    gHide491 = 0;
    // ---- the kext wiring, IN THE KEXT'S ORDER (reachability over the real AppleHardwareHook.cpp) ----
    if (!ahh) { std::printf("  SKIP 0.0.501 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *t) { uint32_t c = 0; for (size_t p = src.find(t); p != std::string::npos; p = src.find(t, p + 1)) c++; return c; };
    auto at = [&](const char *t) { return src.find(t); };
    const char *latch = "gUnitState.pack = gUnitPackOn ? 1u : 0u;";
    expect_u("PIN 501: switch 67 is declared OFF", count("static volatile uint32_t gUnitPackOn { 0u };"), 1u);
    expect_u("PIN 501: the latch exists exactly once, and it is the ONLY write of gUnitState.pack", count(latch) * 10u + count("gUnitState.pack ="), 11u);
    expect_u("PIN 501: gUnitPackOn is read by the latch, the report line and the verb only (never inside the segment loop)",
             count("gUnitPackOn ?") + count("gUnitPackOn &&") * 10u + count("= gUnitPackOn;") * 100u, 112u);
    {
        const size_t pTop = at("gXdBuild.drawElided = 0u; // build 0.0.500: nor switch 66's");
        const size_t pLatch = at(latch);
        const size_t pLoop = at("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {\n        const uint32_t from = segs[k].start");
        const size_t pSetup = at("uint32_t isUnit = unitMap ? gfxsrc_unit_setup(&ex, k, from, dp, build) : 0u;");
        const size_t pXl = at("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
        const size_t pRt = at("st = gfxsrc_unit_retry(&ex, k, from, to - from, out, &olen, &ds, st, dp, build, &rwhy);");
        const size_t pAf = at("if (unitsOn) gfxsrc_unit_after(k, segIb, isUnit, st, ds.err_op, out, olen, &ex, build, dp);");
        expect_u("ORDER 501: pass top < the 67 latch < the segment loop < unit setup < translate < 56's retry < after-step",
                 (pTop != std::string::npos && pTop < pLatch && pLatch < pLoop && pLoop < pSetup && pSetup < pXl && pXl < pRt && pRt < pAf) ? 1u : 0u, 1u);
        // the latch sits in the SAME function as the loop (no function boundary between them): no line in between starts
        // a function definition at column 0 ("static " / "__attribute__")
        const std::string mid = src.substr(pLatch, pLoop - pLatch);
        expect_u("ORDER 501: the latch and the segment loop are in the same function (no definition between them)",
                 (mid.find("\nstatic ") == std::string::npos && mid.find("\n__attribute__") == std::string::npos) ? 1u : 0u, 1u);
        const size_t fAf = at("static __attribute__((noinline)) void gfxsrc_unit_after(");
        const size_t pRet = src.find("    if (!isUnit) return;\n    if (gUnitState.pack) {", fAf);
        expect_u("PIN 501: the PACK counters accumulate in gfxsrc_unit_after, for units/retried singles only, after the undo",
                 (fAf != std::string::npos && pRet != std::string::npos && pRet < pAf) ? 1u : 0u, 1u);
    }
    expect_u("PIN 501: the verb's selector exists exactly once", count("(arg & 0xffull) == 67ull"), 1u);
    expect_u("PIN 501: the verb sets through n48_ra_set", count("changed67 = n48_ra_set(m, &fpk);"), 1u);
    expect_u("PIN 501: the verb prints the bounded unitpack67 line", count("HWLOG(N48_CM_UNITPACK_FMT,"), 1u);
}

// =====================================================================================================================
// build 0.0.506 ( (1), gfx_unitdefer.h): SWITCH 70, THE DEFERRED ROOM RETRY, through run_frame in the kext's
// order (the two passes, n48_mib_defer_pre / _take / _index / _load) over decide44's real bytes.
// =====================================================================================================================
static void checks506(const char *ahh)
{
    std::printf("\n== 0.0.506: switch 70 (the deferred room retry) over decide44's real bytes, in the kext's order ==\n");
    gHide491 = 1;
    struct Fx { const uint32_t *const *w; const uint32_t *n; const uint64_t *v; uint32_t nib; const char *name; } fx[] = {
        { kF77W, kF77N, kF77V, 2u, "F77" }, { kF54W, kF54N, kF54V, 1u, "F54" }, { kF59W, kF59N, kF59V, 1u, "F59" },
        { kF115W, kF115N, kF115V, 1u, "F115" } };
    // ---- the pure decision: only a room refusal of a unit / retried single, only in pass 0, only with 70 and a unit table ----
    {
        const uint32_t E = XLAT12_IB_ERR_DESC, R = XLAT12_TDESC_NO_ROOM;
        uint32_t bad = 0;
        bad += !n48_mib_defer_wanted(1u, 1u, 0u, 1u, E, R);
        bad += !n48_mib_defer_wanted(1u, 1u, 0u, 2u, E, R);
        bad += n48_mib_defer_wanted(0u, 1u, 0u, 1u, E, R);                         // switch OFF
        bad += n48_mib_defer_wanted(1u, 0u, 0u, 1u, E, R);                         // no unit table (55 OFF)
        bad += n48_mib_defer_wanted(1u, 1u, 1u, 1u, E, R);                         // the deferred pass never defers again
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 0u, E, R);                         // a plain single (no unit path)
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 1u, E, XLAT12_TDESC_PROVENANCE);   // any other reason is final
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 1u, E, XLAT12_TDESC_TOO_MANY);
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 1u, XLAT12_IB_ERR_TOO_LONG, XLAT12_REEMIT_NO_ROOM);
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 1u, XLAT12_IB_ERR_PAIR, R);
        bad += n48_mib_defer_wanted(1u, 1u, 0u, 1u, 0u, R);
        expect_u("506 T0: defer only ERR_DESC/NO_ROOM of a unit or retried single, in pass 0, with 70 and a unit table", bad, 0u);
    }
    // ---- the snapshot helpers: the carry and the list come back exactly; counters are never rolled back ----
    {
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        n48_mib_defer_begin(&d, 1u);
        fl.n = 2u; fl.e[0].va = 0x401000000ull; fl.e[0].mode = 3u; fl.e[1].va = 0x402000000ull; fl.e[1].mode = 1u; fl.asked = 77u;
        c.ps_val[3] = 0xabcdu; c.ps_ok = 8u;
        n48_mib_defer_pre(&d, &fl, &c);
        fl.e[2].va = 0x403000000ull; fl.n = 3u; c.ps_val[3] = 0u; c.ps_ok = 0u;           // the refused attempt fed one more
        const uint32_t took = n48_mib_defer_take(&d, 0u, 5u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, N48_MIB_DEFER_MAX, &fl);
        const uint32_t rolled = (fl.n == 2u && fl.e[1].va == 0x402000000ull) ? 1u : 0u;
        fl.e[0].va = 0x7777000000ull; fl.n = 5u; fl.asked = 99u;                           // later units feed the live list
        n48_mib_defer_load(&d, n48_mib_defer_index(&d, 5u), &fl, &c);
        expect_u("506 T0b: take records unit 5, rolls the live list back, load restores list AND carry, counters untouched",
                 took * 100000u + rolled * 10000u + (fl.n == 2u && fl.e[0].va == 0x401000000ull && fl.e[1].mode == 1u) * 1000u +
                 (c.ps_val[3] == 0xabcdu && c.ps_ok == 8u) * 100u + (fl.asked == 99u) * 10u + (n48_mib_defer_index(&d, 6u) == N48_MIB_DEFER_NONE),
                 111111u);
    }
    uint32_t lateRescued = 0;
    uint32_t offBad = 0, offDef = 0, lateOk = 0, lateDef = 0, lateRet = 0, lateVk = 0, lateVkBad = 0, lateSent = 0, lateWalk = 0, lateNoM = 0;
    uint32_t mism = 0, probed = 0, proven = 0, rbBad = 0, never = 0, neverSame = 0, neverTwice = 0, nonRoom = 0, nonRoomBad = 0, frames = 0;
    for (const Fx &f : fx) for (int retry = 0; retry < 2; retry++) for (uint32_t pack = 0; pack < 2; pack++) for (int late = 0; late < 3; late++) {
        Cfg c0; c0.retry = retry; c0.pack = pack; c0.poolLate = late; Cfg c1 = c0; c1.defer = 1u;
        const FrameRes A = run_frame(f.w, f.n, f.v, f.nib, c0), B = run_frame(f.w, f.n, f.v, f.nib, c1);
        Cfg cn = c0; cn.poolLate = 0; const FrameRes N = run_frame(f.w, f.n, f.v, f.nib, cn);   // 70 OFF, the normal pool
        frames++;
        if (gPrint) std::printf("      506 %s r%d p%u late%d: 70 OFF %s | 70 ON %s deferred %u capped %u retried %u mismatch %u probed %u proven %u vk %u/%u\n",
                                f.name, retry, pack, late, A.s.c_str(), B.s.c_str(), B.deferred, B.capped, B.retried, B.listMismatch, B.laterProbed, B.laterProven, B.vkOk, B.vkBlocks);
        // OFF: nothing is deferred and every unit is translated exactly once (the translate call count)
        offDef += A.deferred + A.retried;
        for (uint32_t a : A.attempts) offBad += (a != 1u) ? 1u : 0u;
        // ON: every deferred unit is retried exactly once (2 main translate calls), everything else once
        for (uint32_t k = 0; k < B.attempts.size(); k++) {
            bool isDef = false; for (uint32_t d : B.deferredK) isDef |= (d == k);
            if (B.attempts[k] != (isDef ? 2u : 1u)) neverTwice++;
            // a unit refused for any OTHER reason is never deferred
            if (B.u[k].st && !(B.u[k].st == XLAT12_IB_ERR_DESC && B.u[k].op == XLAT12_TDESC_NO_ROOM)) { nonRoom++; if (isDef) nonRoomBad++; }
        }
        mism += B.listMismatch; probed += B.laterProbed; proven += B.laterProven; rbBad += B.rollbackBad;
        if (late == 1) {
            lateDef += B.deferred; lateRet += B.retried;
            // no unit the NORMAL pool translates (70 OFF) is lost with the withheld pool + 70 ON, and every deferred unit that
            // translates at its retry is one the first attempt refused for room
            uint32_t lost = 0; for (uint32_t k = 0; k < N.u.size(); k++) if (!ures(N, k).st && ures(B, k).st) lost++;
            lateNoM += lost == 0u ? 1u : 0u;
            for (uint32_t d : B.deferredK) lateRescued += ures(B, d).st == 0u ? 1u : 0u;
            lateOk += (B.deferred == B.retried) ? 1u : 0u;
            lateVk += B.vkBlocks; lateVkBad += B.vkBad + B.vkNotNop + B.vkOverlap + B.vkEntBad + (B.vkBlocks - B.vkOk - B.vkBad);
            lateSent += B.sentinels;
            lateWalk += n48_f828_walk_ok(B.cand.data(), (uint32_t)B.cand.size(), nullptr) ? 1u : 0u;
        }
        if (late == 2) {   // the pool never grows: every retry still refuses, and the frame is EXACTLY 70 OFF's
            never += B.deferred;
            neverSame += (B.s == A.s && B.cand == A.cand && B.gate == A.gate && B.gateDetail == A.gateDetail) ? 1u : 0u;
        }
    }
    const uint32_t per = frames / 3u;   // frames per poolLate value
    expect_u("506 T1 OFF IDENTITY: with 70 OFF nothing is deferred or retried and every unit is translated exactly once",
             offDef * 1000u + offBad, 0u);
    expect_u("506 T2: a small pool at every first attempt (poolLate 1): units ARE deferred and every one is retried",
             (lateDef > 0u && lateRet == lateDef && lateOk == per) ? 1u : 0u, 1u);
    expect_u("506 T2: ... and with 70 ON no unit the NORMAL pool translates is lost, in every poolLate-1 frame", lateNoM, per);
    if (gPrint) std::printf("      506 poolLate 1: deferred %u, of them translated at the retry %u\n", lateDef, lateRescued);
    expect_u("506 T2: ... and the retries rescue units (most deferred units translate at the end)", (lateRescued > 0u && lateRescued * 2u > lateDef) ? 1u : 0u, 1u);
    expect_u("506 T2: ... every record of every translated unit lands where its pointer says, in ONE NOP body, no overlap (final candidate)",
             (lateVk > 0u && lateVkBad == 0u) ? 1u : 0u, 1u);
    expect_u("506 T2: ... no sentinel anywhere, and every final candidate walks", lateSent * 1000u + (per - lateWalk), 0u);
    expect_u("506 T3 FAIL-OPEN: every retry runs against EXACTLY the list its first attempt saw (the mirror's own copy), followed "
             "(0.0.511) only by entries an EARLIER retry fed, for VAs that list lacks", mism, 0u);
    expect_u("506 T3 FAIL-OPEN: entries only a LATER unit fed were probed at the retries (the test is not vacuous)", probed > 0u ? 1u : 0u, 1u);
    expect_u("506 T3 FAIL-OPEN: NONE of them is proven by the list a retry runs against", proven, 0u);
    expect_u("506 T3b: at a deferral no entry the attempt ADDED survives in the live list (its feeds vouch for nothing; 0.0.508 LOW-1: its removals stay)", rbBad, 0u);
    expect_u("506 T4 RETRY ONCE: every deferred unit is translated exactly twice, every other unit once", neverTwice, 0u);
    expect_u("506 T4: a pool that never grows (poolLate 2): units deferred, and every such frame ends EXACTLY as 70 OFF (strings, candidate, gate)",
             (never > 0u && neverSame == per) ? 1u : 0u, 1u);
    expect_u("506 T5 NON-ROOM: refusals for other reasons exist in these frames and none of them is deferred",
             (nonRoom > 0u && nonRoomBad == 0u) ? 1u : 0u, 1u);
    // ---- a REAL rescue with no test condition: F59's 7-constituent k1 refuses room at its turn, translates at the end (pack ON) ----
    {
        Cfg c0; c0.retry = 1; c0.pack = 1u; Cfg c1 = c0; c1.defer = 1u;
        const FrameRes A = run_frame(kF59W, kF59N, kF59V, 1u, c0), B = run_frame(kF59W, kF59N, kF59V, 1u, c1);
        const UnitRes &a1 = ures(A, 1), &b1 = ures(B, 1);
        if (gPrint) std::printf("      506 F59 pack ON: 70 OFF %s | 70 ON %s; k1 st %u -> %u own %u pool %u placed %u\n", A.s.c_str(), B.s.c_str(), a1.st, b1.st, b1.own, b1.pool, b1.placed);
        expect_u("506 T6: F59 k1 (7 constituents) refuses NO_ROOM with 70 OFF and translates with 70 ON, its records partly in the pool",
                 (a1.st == XLAT12_IB_ERR_DESC && a1.op == XLAT12_TDESC_NO_ROOM) * 1000u + (b1.st == 0u) * 100u + (b1.pool > 0u) * 10u +
                 (B.deferred == 1u && B.retried == 1u), 1111u);
        expect_u("506 T6: ... every other unit of F59 keeps its 70 OFF status", [&] { uint32_t d = 0; for (uint32_t k = 0; k < A.u.size(); k++) if (k != 1u && ures(A, k).st != ures(B, k).st) d++; return d; }(), 0u);
        expect_u("506 T6: ... its records verify in the final candidate, no sentinel", (B.vkBlocks > 0u && B.vkOk == B.vkBlocks && B.vkNotNop + B.vkOverlap + B.vkEntBad + B.sentinels == 0u) ? 1u : 0u, 1u);
    }
    // ---- THE CAP: past it the refusal stands, where it was, as today ----
    {
        Cfg c; c.retry = 1; c.poolLate = 1; c.defer = 1u; c.deferCap = 1u;
        const FrameRes B = run_frame(kF77W, kF77N, kF77V, 2u, c);
        Cfg c4 = c; c4.deferCap = N48_MIB_DEFER_MAX; const FrameRes B4 = run_frame(kF77W, kF77N, kF77V, 2u, c4);
        uint32_t mLeft = 0; for (const UnitRes &u : B.u) mLeft += (u.st == XLAT12_IB_ERR_DESC && u.op == XLAT12_TDESC_NO_ROOM) ? 1u : 0u;
        if (gPrint) std::printf("      506 cap 1: %s deferred %u capped %u | cap %u: %s deferred %u\n", B.s.c_str(), B.deferred, B.capped, N48_MIB_DEFER_MAX, B4.s.c_str(), B4.deferred);
        expect_u("506 T7 CAP: with a cap of 1 exactly one unit is deferred, the next room refusal is counted capped and stays refused",
                 (B.deferred == 1u && B.capped >= 1u && mLeft >= 1u && B4.deferred >= 2u && B4.capped == 0u) ? 1u : 0u, 1u);
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl);
        n48_mib_defer_begin(&d, 1u);
        uint32_t took = 0;
        for (uint32_t k = 0; k < N48_MIB_DEFER_MAX + 3u; k++)
            took += n48_mib_defer_take(&d, 0u, k, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, 1000u, &fl);   // a cap over the table
        expect_u("506 T7 CAP: the table never holds more than N48_MIB_DEFER_MAX, whatever cap is asked", took * 100u + d.n * 10u + (d.capped == 3u),
                 N48_MIB_DEFER_MAX * 110u + 1u);
    }
    // ---- the defer70 line at 20-digit counters and the longest how/note; the guard; the verb values ----
    {
        char line[700];
        const int w = std::snprintf(line, sizeof line, N48_DEFER70_FMT, "OFF (default)",
                                    " - `gfxneuter 70` REFUSED: a continuous arm stands, unchanged", " - INERT: needs 55 ON",
                                    ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        if (gPrint) std::printf("      defer70 at 20-digit counters: %d bytes\n", w);
        expect_u("506: the defer70 line fits the logger's 491-byte body at 20-digit counters", (w > 0 && w < 491) ? 1u : 0u, 1u);
        expect_u("506: switch 70 is mid-arm guarded; a read is not refused while armed",
                 n48_cm_cont_switch_guarded(70u) * 10u + n48_cm_cont_switch_refused(70u, 1u, 1u, N48_CM_SHOT_ARMED), 10u);
        uint32_t v = 7u;
        expect_u("506: 70's values: M 1 ON, M 2 OFF, M 0/3 unchanged (n48_ra_set)",
                 (n48_ra_set(1u, &v) && v == 1u && n48_ra_set(2u, &v) && v == 0u && !n48_ra_set(0u, &v) && !n48_ra_set(3u, &v) && v == 0u) ? 1u : 0u, 1u);
    }
    gHide491 = 0;
    // ---- the kext wiring, IN THE KEXT'S ORDER (reachability over the real AppleHardwareHook.cpp) ----
    if (!ahh) { std::printf("  SKIP 0.0.506 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *t) { uint32_t c = 0; for (size_t p = src.find(t); p != std::string::npos; p = src.find(t, p + 1)) c++; return c; };
    auto at = [&](const char *t) { return src.find(t); };
    // build 0.0.511: the pass-top latch is the call of the noinline gfxsrc_defer_begin (which counts the previous pass's bounds
    // and runs n48_mib_defer_begin(&gUnitDefer, gUnitDeferOn) - pinned below)
    const char *latch = "gfxsrc_defer_begin();   // build 0.0.506 (switch 70): read ONCE per pass";
    const char *passes = "for (gUnitDefer.pass = 0u; gUnitDefer.pass < 2u; gUnitDefer.pass++) {\n"
                         "    if (gUnitDefer.pass) { if (!gUnitDefer.n) break; segIb = 0u; }";
    const char *skip = "        if (gUnitDefer.pass && !gfxsrc_defer_load(k)) continue;";
    const char *pre = "if (gUnitDefer.on && unitMap && !gUnitDefer.pass) gfxsrc_defer_pre();";
    const char *take = "        if (gUnitDefer.on && gfxsrc_defer_take(k, from, to, build, unitMap, isUnit, st, ds.err_op, ds.prov_va)) continue;";   // 0.0.512 C3
    const char *done = "if (gUnitDefer.on) gfxsrc_defer_done(k, from, to, st, ds.err_op);";   // build 0.0.511/0.0.512: every segment (kills, retry adds)
    const char *close = "    }   // build 0.0.506: the two passes (gUnitDefer.pass)";
    expect_u("PIN 506: switch 70 is declared OFF", count("static volatile uint32_t gUnitDeferOn { 0u };"), 1u);
    expect_u("PIN 506: the latch exists exactly once, the only n48_mib_defer_begin (inside gfxsrc_defer_begin)",
             count(latch) * 10u + count("n48_mib_defer_begin(") + count("static __attribute__((noinline)) void gfxsrc_defer_begin(void)\n{\n"
             "    gUnitDefer511.wallOver += gUnitDefer.wall_over; gUnitDefer511.askOver += gUnitDefer.ask_over;\n"
             "    gUnitDefer511.addsOver += gUnitDefer.adds_over; gUnitDefer511.provCapped += gUnitDefer.prov_capped;\n"
             "    gUnitDefer512.conflicts += gUnitDefer.conflicts; gUnitDefer512.droppedC2 += gUnitDefer.dropped_c2;   // build 0.0.512\n"
             "    gUnitDefer512.killOver += gUnitDefer.kill_over;\n"
             "    n48_mib_defer_begin(&gUnitDefer, gUnitDeferOn);\n}") * 100u, 111u);
    expect_u("PIN 506: gUnitDeferOn is read by the latch, the report line and the verb only",
             count("gUnitDeferOn ?") + count("gUnitDeferOn &&") * 10u + count("= gUnitDeferOn;") * 100u + count(", gUnitDeferOn)") * 1000u, 1111u);
    expect_u("PIN 506: the helpers hand the kext's own list, carry, pool and cap to the pure steps",
             count("n48_mib_defer_pre(&gUnitDefer, &gXdFrameLocal, &gPolicyUdCarry);") +
             count("n48_mib_defer_take_w(&gUnitDefer, gUnitDefer.pass, k, unitMap, isUnit, st, op, N48_MIB_DEFER_MAX, &gXdFrameLocal,") * 10u +
             count("gUnitDeferAug = n48_mib_defer_load_aug(&gUnitDefer, di, &gXdFrameLocal, &gPolicyUdCarry);") * 100u +
             count("gUnitDeferS.undone += n48_mib_unit_undo2(isUnit, st, &gUnitPool, &gUnitState);") * 1000u, 1111u);   // 0.0.522: + the spill tier
    // the take helper restores Apple's bytes itself, and only after the pure step said "deferred"; pass 1's load finds the unit
    {
        const size_t fT = at("static __attribute__((noinline)) uint32_t gfxsrc_defer_take(");
        const size_t pNo = src.find("        return 0u;\n    }\n    if (build) memcpy(&gXdNew[from], &gXdIb[from], (size_t)(to - from) * 4u);", fT);
        const size_t fL = at("static __attribute__((noinline)) uint32_t gfxsrc_defer_load(uint32_t k)");
        const size_t pIdx = src.find("    const uint32_t di = n48_mib_defer_index(&gUnitDefer, k);\n    if (di == N48_MIB_DEFER_NONE) return 0u;\n", fL);
        const size_t pAug = src.find("    gUnitDeferAug = n48_mib_defer_load_aug(&gUnitDefer, di, &gXdFrameLocal, &gPolicyUdCarry);", fL);
        expect_u("PIN 506: the take helper restores Apple's bytes after a deferral; the load helper loads the unit's own snapshot (0.0.511: augmented)",
                 (fT != std::string::npos && pNo != std::string::npos && pNo - fT < 1800u && fL != std::string::npos && pIdx != std::string::npos &&
                  pIdx - fL < 300u && pAug != std::string::npos && pAug > pIdx && pAug - pIdx < 400u) ? 1u : 0u, 1u);
    }
    expect_u("PIN 506: each step is called exactly once (pass skip + load, pre, take + restore + continue, done, the close)",
             count(passes) + count(skip) * 10u + count(pre) * 100u + count(take) * 1000u + count(done) * 10000u + count(close) * 100000u, 111111u);
    expect_u("PIN 506: gfxsrc_defer_load / _pre / _take / _done / _begin have no other caller (definition + one call each)",
             count("gfxsrc_defer_begin(") * 10000u + count("gfxsrc_defer_load(") * 1000u + count("gfxsrc_defer_pre(") * 100u +
             count("gfxsrc_defer_take(") * 10u + count("gfxsrc_defer_done("), 22222u);
    {
        const size_t pTop = at("gXdBuild.xib = gXibOn & N48_MIB_XIB_MASK; gXdBuild.leadMask = 0u;   // build 0.0.505 (switch 69): read ONCE per pass");
        const size_t pLatch = at(latch), pClear = at("n48_dl_clear(&gXdFrameLocal);"), pPass = at(passes);
        const size_t pLoop = at("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {\n        const uint32_t from = segs[k].start");
        const size_t pSkip = at(skip), pSetup = at("uint32_t isUnit = unitMap ? gfxsrc_unit_setup(&ex, k, from, dp, build) : 0u;");
        const size_t pPre = at(pre), pXl = at("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
        const size_t pRt = at("st = gfxsrc_unit_retry(&ex, k, from, to - from, out, &olen, &ds, st, dp, build, &rwhy);");
        const size_t pTake = at(take), pDe = at("if (ex.flags & XLAT12_EXTRA_DRAW_ELIDE) {\n            gXdBuild.drawElided += ds.draw_elided;");
        const size_t pCg = at("const uint32_t cgReason = navi48_cg_seg_check();");
        const size_t pFl = at("n48_dl_from_output(&gXdFrameLocal, dctxKey, out + flFrom, olen - flFrom, nullptr)");
        const size_t pFence = at("if (gXdFence828 && arm == N48_SD_ARM_COMMIT && !st && build && olen == (to - from) && !gXdF828Pending && fenceOffered) {");
        const size_t pAf = at("if (unitsOn) gfxsrc_unit_after(k, segIb, isUnit, st, ds.err_op, out, olen, &ex, build, dp);");
        const size_t pDone = at(done), pClose = at(close), pR1 = at("if (gMdMode && build && gXdBuild.ok) {");
        const size_t np = std::string::npos;
        expect_u("ORDER 506: pass top < the 70 latch < the list clear < the two passes < the segment loop < pass-1 skip/load < unit setup < pre < translate < 56's retry < TAKE",
                 (pTop != np && pTop < pLatch && pLatch < pClear && pClear < pPass && pPass < pLoop && pLoop < pSkip && pSkip < pSetup &&
                  pSetup < pPre && pPre < pXl && pXl < pRt && pRt < pTake) ? 1u : 0u, 1u);
        expect_u("ORDER 506: TAKE < draw-elide counters/backstop < copy guard < frame-local feed < fence < after-step < done < the passes' close < R1",
                 (pTake != np && pTake < pDe && pDe < pCg && pCg < pFl && pFl < pFence && pFence < pAf && pAf < pDone && pDone < pClose && pClose < pR1) ? 1u : 0u, 1u);
        const std::string mid = src.substr(pLatch, pClose - pLatch);
        expect_u("ORDER 506: the latch, the passes and every step are in ONE function (no definition between them)",
                 (mid.find("\nstatic ") == np && mid.find("\n__attribute__") == np) ? 1u : 0u, 1u);
        // nothing reads the frame-local list after the passes (so the list the last retry leaves behind is never evidence)
        const size_t pEnd = src.find("\n}\n", pClose);
        const std::string tail = src.substr(pClose, pEnd - pClose);
        expect_u("PIN 506: gfxsrc_policy never touches gXdFrameLocal after the passes", tail.find("gXdFrameLocal") == np ? 1u : 0u, 1u);
    }
    expect_u("PIN 506: the verb's selector exists exactly once", count("(arg & 0xffull) == 70ull"), 1u);
    expect_u("PIN 506: the verb sets through n48_ra_set", count("changed70 = n48_ra_set(m, &fdf);"), 1u);
    expect_u("PIN 506: the verb prints the bounded defer70 line", count("HWLOG(N48_DEFER70_FMT,"), 1u);
}

// =====================================================================================================================
// build 0.0.508 — (B) LOW-1 of the 0.0.506 review (n48_mib_defer_take keeps the refused attempt's REMOVALS) and (A) the
// kext wiring of switch 71 (the last-candidate fence rule, gfx_fence828.h n48_f828_find_last; its pure tests are
// gfx_fence828_test.cpp section 16).
// =====================================================================================================================
static void checks508(const char *ahh)
{
    std::printf("\n== 0.0.508: LOW-1 (the deferral keeps the attempt's removals) and switch 71's wiring ==\n");
    // ---- LOW-1: producer p feeds S; deferred unit k re-feeds S DIFFERENTLY (dropping p's entry) and is refused NO_ROOM; a later
    //      unit j samples S in p's shape. Fixed: j is NOT proven by p's stale entry. 0.0.507's full rollback restored it. ----
    {
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        const uint64_t ctx = 5u, S = 0x401480000ull, U = 0x402000000ull, T = 0x403000000ull;
        n48_mib_defer_begin(&d, 1u);
        fl.feedTok = 11u; n48_dl_set(&fl, ctx, S, 3u, 0x10000ull, 0u, 0x40000ull);   // p: S, tiled mode 3
        fl.feedTok = 12u; n48_dl_set(&fl, ctx, U, 1u, 0x20000ull, 0u, 0x40000ull);   // an unrelated producer: U
        fl.feedTok = 0u;
        const uint32_t pOk = (uint32_t)n48_dl_tiled_ok(&fl, ctx, S, 3u);
        n48_mib_defer_pre(&d, &fl, &c);                                               // before k's first attempt
        const uint64_t drop0 = fl.reFeedDropped;
        n48_dl_set(&fl, ctx, S, 1u, 0x10000ull, 0u, 0x40000ull);                      // k: S re-fed in ANOTHER mode: p's entry dropped
        n48_dl_set(&fl, ctx, T, 3u, 0x30000ull, 0u, 0x40000ull);                      // k: a new entry T
        const uint32_t kDropped = (uint32_t)(fl.reFeedDropped - drop0);
        const uint32_t took = n48_mib_defer_take(&d, 0u, 4u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, N48_MIB_DEFER_MAX, &fl);
        const uint32_t jS = (uint32_t)n48_dl_tiled_ok(&fl, ctx, S, 3u);              // j samples S in p's shape
        const uint32_t jT = (uint32_t)n48_dl_tiled_ok(&fl, ctx, T, 3u);              // k's own addition
        const uint32_t jU = (uint32_t)n48_dl_tiled_ok(&fl, ctx, U, 1u);              // untouched by k
        if (gPrint) std::printf("      508 LOW-1: p proven %u, k dropped %u, took %u; after: S %u T %u U %u n %u\n", pOk, kDropped, took, jS, jT, jU, fl.n);
        expect_u("508 LOW-1: the premise holds (p's S proven before k; k's differing re-feed DROPS it; k is deferred)",
                 pOk * 100u + kDropped * 10u + took, 111u);
        expect_u("508 LOW-1: after the deferral a later unit j is NOT proven by p's stale S", jS, 0u);
        expect_u("508 LOW-1: ... k's own addition T is gone too (additions are taken back)", jT, 0u);
        expect_u("508 LOW-1: ... and the entry k never touched (U) stands", jU * 10u + fl.n, 11u);
        // the deferred unit's OWN retry still runs against the list as it stood before its first attempt (p's S included)
        n48_mib_defer_load(&d, n48_mib_defer_index(&d, 4u), &fl, &c);
        expect_u("508 LOW-1: ... k's retry snapshot is unchanged (S proven for k's own reads, as at its first attempt)",
                 (uint32_t)n48_dl_tiled_ok(&fl, ctx, S, 3u) * 10u + fl.n, 12u);
    }
    {   // a matching re-feed (flags refresh) keeps the entry with the snapshot's flags; a switch-58 REPLACE drops it (fail-closed)
        static n48_mib_defer d; static n48_dl fl; std::memset(&fl, 0, sizeof fl); xlat12_ud_carry c {};
        n48_mib_defer_begin(&d, 1u);
        fl.feedTok = 11u; n48_dl_set(&fl, 5u, 0x401480000ull, 3u, 0x10000ull, 1u, 0x40000ull);
        n48_dl_set(&fl, 5u, 0x402000000ull, 3u, 0x20000ull, 1u, 0x40000ull);
        n48_mib_defer_pre(&d, &fl, &c);
        fl.feedTok = 12u; fl.replace = 1u;
        n48_dl_set(&fl, 5u, 0x401480000ull, 3u, 0x10000ull, 2u, 0x40000ull);   // same shape: flags only
        n48_dl_set(&fl, 5u, 0x402000000ull, 1u, 0x20000ull, 2u, 0x40000ull);   // 58: replaced by the refused attempt's producer
        (void)n48_mib_defer_take(&d, 0u, 1u, 1u, 1u, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM, N48_MIB_DEFER_MAX, &fl);
        expect_u("508 LOW-1: a same-shape re-feed keeps the entry with the snapshot's flags; a replaced entry is dropped",
                 (fl.n == 1u && fl.e[0].va == 0x401480000ull && fl.e[0].flags == 1u && fl.e[0].tok == 11u) ? 1u : 0u, 1u);
    }
    // ---- switch 71 through the mirror of the kext's order over decide44's real frames: every answer that is OK with 71 OFF is
    //      the SAME with 71 ON; the only changes are AMBIGUOUS -> OK over two candidates (the last-candidate rule's flips) ----
    {
        gHide491 = 1;
        struct Fx { const uint32_t *const *w; const uint32_t *n; const uint64_t *v; uint32_t nib; } fx[] = {
            { kF77W, kF77N, kF77V, 2u }, { kF54W, kF54N, kF54V, 1u }, { kF59W, kF59N, kF59V, 1u }, { kF115W, kF115N, kF115V, 1u } };
        uint32_t asked = 0, okOff = 0, okSame = 0, flips = 0, other = 0, strBad = 0;
        for (const Fx &f : fx) {
            Cfg c0; c0.retry = 1; c0.pack = 1u; Cfg c1 = c0; c1.fence71 = 1u;
            const FrameRes A = run_frame(f.w, f.n, f.v, f.nib, c0), B = run_frame(f.w, f.n, f.v, f.nib, c1);
            strBad += (A.s == B.s && A.u.size() == B.u.size()) ? 0u : 1u;
            for (uint32_t k = 0; k < A.u.size() && k < B.u.size(); k++) {
                if (A.u[k].fenceWhy == 99u) continue;
                asked++;
                const uint32_t a = A.u[k].fenceWhy, b = B.u[k].fenceWhy;
                if (a == N48_F828_OK) { okOff++; okSame += (b == a && B.u[k].fenceCand == A.u[k].fenceCand) ? 1u : 0u; }
                else if (a == N48_F828_AMBIGUOUS && b == N48_F828_OK && A.u[k].fenceCand == 2u) flips++;
                else if (a != b) other++;
                if (gPrint && a != b) std::printf("      508 71 k%u: OFF %s (cands %u) ON %s (cands %u)\n", k,
                                                  n48_f828_reason_name(a), A.u[k].fenceCand, n48_f828_reason_name(b), B.u[k].fenceCand);
            }
        }
        gHide491 = 0;
        if (gPrint) std::printf("      508 71 ON vs OFF over decide44 (kext order): asked %u, OK with 71 OFF %u (same with 71 ON %u), "
                                "AMBIGUOUS->OK %u, other changes %u\n", asked, okOff, okSame, flips, other);
        expect_u("508 71: over decide44's real frames in the kext's order, every OK answer with 71 OFF is unchanged with 71 ON",
                 (asked > 0u && okOff > 0u && okSame == okOff) ? 1u : 0u, 1u);
        expect_u("508 71: ... the only changes are AMBIGUOUS -> OK over two candidates (none other; the translation is identical)",
                 other * 10u + strBad, 0u);
        expect_u("508 71: ... and there are such flips (the rule is exercised on real unit slices)", flips > 0u ? 1u : 0u, 1u);
    }
    {   // the line, the guard, the values
        char line[700];
        const int w = std::snprintf(line, sizeof line, N48_FENCE71_FMT, "ON (last-candidate)",
                                    " - `gfxneuter 71` REFUSED: a continuous arm stands, unchanged",
                                    ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        if (gPrint) std::printf("      fence71 at 20-digit counters: %d bytes\n", w);
        expect_u("508: the fence71 line fits the logger's 491-byte body at 20-digit counters", (w > 0 && w < 491) ? 1u : 0u, 1u);
        expect_u("508: switch 71 is mid-arm guarded; a read is not refused while armed",
                 n48_cm_cont_switch_guarded(71u) * 10u + n48_cm_cont_switch_refused(71u, 1u, 1u, N48_CM_SHOT_ARMED), 10u);
    }
    // ---- the kext wiring, IN THE KEXT'S ORDER (reachability over the real AppleHardwareHook.cpp) ----
    if (!ahh) { std::printf("  SKIP 0.0.508 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string src;
    { FILE *f = std::fopen(ahh, "rb"); if (f) { char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) src.append(b, r); std::fclose(f); } }
    auto count = [&](const char *t) { uint32_t c = 0; for (size_t p = src.find(t); p != std::string::npos; p = src.find(t, p + 1)) c++; return c; };
    auto at = [&](const char *t) { return src.find(t); };
    const size_t np = std::string::npos;
    const char *latch = "gXdBuild.fence71 = gFence71On ? 1u : 0u;";
    const char *siteC = "const uint32_t whyC = gXdBuild.fence71 ? n48_f828_find_last(out + fsC, olen - fsC, &mibfr)\n"
                        "                                                   : n48_f828_find(out + fsC, olen - fsC, &mibfr);";
    const char *siteF = "const uint32_t why = gXdBuild.fence71 ? n48_f828_find_last(out + fs, olen - fs, &fr)\n"
                        "                                                  : n48_f828_find(out + fs, olen - fs, &fr);";
    expect_u("PIN 508: switch 71 is declared OFF", count("static volatile uint32_t gFence71On { 0u };"), 1u);
    expect_u("PIN 508: gFence71On is read by the latch, the report line and the verb only",
             count("gFence71On ?") * 10u + count("= gFence71On;") + count("gFence71On &&") * 100u, 21u);
    expect_u("PIN 508: the latch exists exactly once; gXdBuild.fence71 is written nowhere else", count(latch) * 10u + count("gXdBuild.fence71 ="), 11u);
    expect_u("PIN 508: BOTH fence searches choose by the latch: census site (ON find_last, OFF find), exactly once",
             count(siteC), 1u);
    expect_u("PIN 508: ... and the switch-16 site (ON find_last, OFF find), exactly once", count(siteF), 1u);
    expect_u("PIN 508: no other fence search in the file (2 find_last calls, 2 find calls on out)",
             count("n48_f828_find_last(") * 10u + count("n48_f828_find(out"), 22u);
    expect_u("PIN 508: the census feeds the chosen answer; the fence site counts it", count("n48_mib0_note_f828(&gMib0, whyC);") * 10u +
             count("if (gXdBuild.fence71) fence71_tally(0u, why, fr.candidates);"), 11u);
    {
        const size_t pFn = at("static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp, uint64_t dctxKey,");
        const size_t pTop = at("gXdBuild.xib = gXibOn & N48_MIB_XIB_MASK; gXdBuild.leadMask = 0u;   // build 0.0.505 (switch 69): read ONCE per pass");
        const size_t pLatch = at(latch);
        const size_t pPass = at("for (gUnitDefer.pass = 0u; gUnitDefer.pass < 2u; gUnitDefer.pass++) {");
        const size_t pC = at(siteC), pF = at(siteF);
        const size_t pApply = at("const uint32_t aw = n48_f828_apply(out + fs, olen - fs, &fr, slotVa, fencePageVa, want);");
        expect_u("ORDER 508: gfxsrc_policy < pass top < the 71 LATCH < the segment passes < census search < fence search < apply",
                 (pFn != np && pFn < pTop && pTop < pLatch && pLatch < pPass && pPass < pC && pC < pF && pF < pApply) ? 1u : 0u, 1u);
        const std::string mid = (pFn != np && pApply != np && pApply > pFn) ? src.substr(pFn + 10u, pApply - pFn - 10u) : std::string("\nstatic ");
        expect_u("ORDER 508: the latch and both searches are in ONE function (gfxsrc_policy)",
                 (mid.find("\nstatic ") == np && mid.find("\n__attribute__") == np) ? 1u : 0u, 1u);
    }
    expect_u("PIN 508: the verb's selector exists exactly once", count("(arg & 0xffull) == 71ull"), 1u);
    expect_u("PIN 508: the verb is guarded and sets through n48_ra_set",
             count("n48_cm_cont_switch_refused(71u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") * 10u +
             count("changed71 = n48_ra_set(m, &f71); if (changed71) gFence71On = f71;"), 11u);
    expect_u("PIN 508: the verb prints the bounded fence71 line", count("HWLOG(N48_FENCE71_FMT,"), 1u);
}

} // namespace u480
