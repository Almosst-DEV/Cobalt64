// pws93_real_drv.cpp — build 0.0.537 fix round (review SHOULD items 2 and 3): SWITCH 93 UNDER THE REAL RECIPE FLAGS, over captured
// frames. Driven by tools/test_pws93_corpus.py (which writes the manifest this reads); not a suite of its own.
//
// Each frame goes through the kext's own pure steps in the kext's order (gfxsrc_policy): the multi-IB segment stage
// (gfx_mib.h n48_mib_segment, switch 69's M 3), the unit stage (n48_mib_units, switch 55), per unit the kext's flag set - the
// descriptor port (INLINE_DESC | TABLE_DESC | DESC_INV), 44 UD_REEMIT with a carry, 48 TABLE_REUSE, 49 DESC_INV_APPLE_HEAD where the
// head runs, 52 VS_KNOWN, 60 DCC_STRIP, 66 DRAW_ELIDE rows UY|AO|GLASS, 57 CS_ELIDE, 91 NCLEAR, 92 RECT2D, the raster delta and
// PAIR_PRE / READSET, a ring - then n48_mib_unit_setup (the frame pool), the translator, switch 56's n48_mib_retry_single, the kext's
// pairing check (xlat12_ib_pws_check), the restore and the pool feed. The frame runs TWICE from the same start: pass A without
// XLAT12_EXTRA_PWS, pass B with it where the kext sets it (the head gate). After each pass: R1's scan over the whole candidate
// (gfx_memdst.h n48_md_scan, segStart = the units' starts, as the kext) and fence828's identity over each unit's final constituent.
//
// MODEL LIMITS (stated, not hidden): client memory is the capture's own USER/USER2 regions of that frame (a record the capture did
// not keep refuses READ, as it would offline in every harness); provenance (desc_tiled_ok / desc_dcc_ok) answers PROVEN for every
// ask (the optimistic case: MORE records are placed than a kext whose ledger refuses some, so the pad left for the pair is SMALLER -
// the pessimistic case for PWS); program identities come from the capture's PGM-VS/PGM-PS regions (xlat12_shader_id_match +
// xlat12_ib_profile_stage, rows 43/51 admitted); switch 70's deferral and 78's redo are not modelled.
//
// Output, one line per unit and pass: U <frame> <pass> <k> <from> <to> <cons> <isUnit> <flagged> <st> <err_op> <seen> <conv>
//   <noroom> <tail> <fallback> <fenceIdent> <pairRefused> <slot> (build 0.0.539: ds.pws_slot, the slots dropped); then per frame
// and pass: R <frame> <pass> <originBad> <kindBad> <segsOk>; and for the capsule probe: C <frame> <dword> <convWith> <convWithout>
// <slotted> <slotWith> <slotWithout> (0.0.539: printed only where the frame holds Apple's barrier at that dword; `slotted` 1 when
// EVENT_WRITE 0xE and Apple's disabled slot follow it). `--at d1,d2,...` probes those dwords in EVERY frame of the manifest (match
// by dword position: a PS address moves between boots,); `<frame> <dword>` pairs probe one frame each.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_xlat_verdict.h"
#include "gfx_mib.h"
#include "gfx_memdst.h"
#include "gfx_fence828.h"

struct Reg { std::string kind; uint64_t va; std::vector<uint32_t> w; };
static std::vector<Reg> gRegs;
static uint32_t gIo[2];

static std::vector<uint32_t> load(const std::string &p)
{
    std::vector<uint32_t> v;
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return v;
    uint32_t b[4096]; size_t n;
    while ((n = std::fread(b, 4, 4096, f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f);
    return v;
}
static int r_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const Reg &r : gRegs) {
        if (r.kind != "USER" && r.kind != "USER2") continue;
        if (va < r.va || ((va - r.va) & 3ull)) continue;
        const uint64_t at = (va - r.va) / 4u;
        if (at + ndw > r.w.size()) continue;
        std::memcpy(out, &r.w[at], 4u * ndw);
        return 1;
    }
    return 0;
}
static int r_tiled(void *, uint64_t, uint32_t, uint32_t) { return 1; }
static int r_dcc(void *, uint64_t, uint32_t, uint32_t) { return 1; }
static int r_cs_is_n(void *, uint64_t va) { return va == 0x400017a00ull ? 1 : 0; }
static int r_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    const char *want = stage ? "PGM-VS" : "PGM-PS";
    for (const Reg &r : gRegs) {
        if (r.kind != want || r.va != va || r.w.empty()) continue;
        const int id = xlat12_shader_id_match(stage, r.w.data(), (uint32_t)r.w.size());
        if (id < 0 || xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
        if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
        return 1;
    }
    return 0;
}

#ifdef P93_DECIDE44
// decide44's four frames with the client memory and program identities the 0.0.480 builder recorded (tests/fixture_units_decide44.h:
// a read the capture could not answer is FILLED there with a plausible record of its size - it measures ROOM, never correctness)
#include "fixture_units_decide44.h"
static int f_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const UMem &m : kUMem)
        if (m.va == va && m.ndw == ndw) { for (uint32_t k = 0; k < ndw; k++) out[k] = kUMemW[m.off + k]; return 1; }
    return 0;
}
static int f_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    for (const UPgm &p : kUPgm)
        if (p.stage == stage && p.va == va) {
            int id = -1;
            for (int i = 0; i < (int)xlat12_shader_id_count(); i++) if (!std::strcmp(xlat12_shader_id_name(i), p.name)) id = i;
            if (id < 0 || xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
            if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
            return 1;
        }
    return 0;
}
#endif
static int (*gRead)(void *, uint64_t, uint32_t, uint32_t *) = &r_read;
static int (*gProfile)(void *, uint32_t, uint64_t, xlat12_draw_profile *) = &r_profile;

static xlat12_pool gPool;
static xlat12_unit gU;
static xlat12_ud_carry gCarry;
static uint32_t u_xlat(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    return xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), ex, in, n, out, olen, ds);
}
// gMode 0: the real recipe (above); 1: no extra block at all (the simple corpus mode); 2: the recipe's ring and raster delta only
// (switches 27 + 50 and the open ring: the parts that need no client memory and no program identity), with 57 + 91 + 92.
static int gMode = 0;
// build 0.0.540 item 5: `--tc` adds XLAT12_EXTRA_TBLCACHE (switch 97) to every unit, in every mode (tools/test_tc97_corpus.py
// compares every U / R line, and the whole candidate's FNV, with and without it).
static int gTc = 0;
static void base_flags(xlat12_draw_extra &ex)
{
    if (gMode == 1) return;
    if (gMode == 2) {
        ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull;
        ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
        ex.flags |= XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR | XLAT12_EXTRA_RECT2D; ex.cs_is_n = &r_cs_is_n;
        return;
    }
    ex.pgm_profile = gProfile;
    ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull;
    ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
    ex.flags |= XLAT12_EXTRA_PAIR_PRE | XLAT12_EXTRA_READSET;
    ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;
    ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &gCarry;
    ex.flags |= XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_VS_KNOWN;
    ex.flags |= XLAT12_EXTRA_DCC_STRIP | XLAT12_EXTRA_DRAW_ELIDE;
    ex.draw_elide_rows = XLAT12_DE_CLASS_UY | XLAT12_DE_CLASS_AO | XLAT12_DE_CLASS_GLASS;
    ex.flags |= XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR | XLAT12_EXTRA_RECT2D;
    ex.cs_is_n = &r_cs_is_n;
    ex.desc_read = gRead; ex.desc_tiled_ok = &r_tiled; ex.desc_dcc_ok = &r_dcc;
}

static uint64_t gSlotTot = 0;   // build 0.0.539: slots dropped by the last run_pass (translated units only)
struct Frame { uint32_t no = 0, nib = 0; uint64_t va[N48_XV_MAX_IBS] = {}; uint32_t off[N48_XV_MAX_IBS] = {}, nn[N48_XV_MAX_IBS] = {};
               std::vector<uint32_t> cat; };
struct PassRes { uint32_t conv = 0; };

// one pass of one frame; returns total conversions
static uint32_t run_pass(const Frame &F, const std::vector<uint32_t> &cat, uint32_t pass, bool print)
{
    const uint32_t N = (uint32_t)cat.size();
    gSlotTot = 0;
    static xlat12_ib_segment segs[N48_XV_MAX_SEGS], units[N48_XV_MAX_SEGS];
    static uint32_t cf[N48_XV_MAX_SEGS], cc[N48_XV_MAX_SEGS], ibnseg[N48_XV_MAX_IBS];
    uint32_t tot = 0, lead = 0; n48_mib_seg_diag dg {};
    uint32_t ns = n48_mib_segment(cat.data(), F.nib, F.off, F.nn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 3u, &lead);
    if (!ns || ns != tot) { if (print) std::printf("Z %u %u\n", F.no, pass); return 0; }
    uint32_t nu = ns, unitMap = 0;
    for (uint32_t k = 0; k < ns; k++) { units[k] = segs[k]; cf[k] = k; cc[k] = 1u; }
    const uint32_t u = gMode ? 0u : n48_mib_units(cat.data(), F.nib, F.off, F.nn, segs, ns, XLAT12_UNIT_CONS_MAX, units, cf, cc, N48_XV_MAX_SEGS, lead);
    if (u) { nu = u; unitMap = 1u; }
    gPool.nrun = 0; gPool.lost = 0; gPool.jn = 0;
    std::memset(&gCarry, 0, sizeof gCarry);
    std::vector<uint32_t> cand = cat;
    uint32_t convTot = 0, segsOk = 1, segIb = 0;
    static uint32_t segStart[N48_XV_MAX_SEGS];
    for (uint32_t k = 0; k < nu; k++) {
        const uint32_t from = units[k].start, to = units[k].end, n = to - from;
        segStart[k] = from;
        while (segIb + 1u < F.nib && from >= F.off[segIb] + F.nn[segIb]) segIb++;
        xlat12_draw_extra ex {};
        base_flags(ex);
        if (gTc) ex.flags |= XLAT12_EXTRA_TBLCACHE;   // build 0.0.540 item 5
        if (gMode == 0 && n48_mib_head_executes(cat.data(), units[k].head, N)) ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
        uint32_t flagged = 0;
        if (pass && n48_mib_start_runs(cat.data(), &units[k], N)) { ex.flags |= XLAT12_EXTRA_PWS; flagged = 1; }
        ex.ib_va = n48_mib_seg_va(F.va[segIb], from, F.off[segIb]);
        uint32_t isUnit = 0;
        if (unitMap) isUnit = n48_mib_unit_setup(&ex, &gU, &gPool, 1, n48_mib_unit_flag(cc[k]) ? cc[k] : 0u, &segs[cf[k]], from, nullptr, nullptr);
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        uint32_t olen = 0;
        uint32_t *out = &cand[from];
        uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &cat[from], n, out, &olen, &ds);
        uint32_t rwhy = N48_MIB_RETRY_NOT;
        if (n48_mib_retry_wanted(1u, 1u, unitMap, 1u, cc[k], st, ds.err_op)) {
            st = n48_mib_retry_single(&u_xlat, &ex, &gU, &gPool, 1, &segs[cf[k]], from, &cat[from], n, out, &olen, &ds, st, &rwhy);
            if (rwhy == N48_MIB_RETRY_OK || rwhy == N48_MIB_RETRY_REFUSED || rwhy == N48_MIB_RETRY_SENTINEL_LEFT) isUnit = 2u;
        }
        uint32_t pairRef = 0;
        if (flagged && !st && xlat12_ib_pws_check(out, olen) != 0u) { st = XLAT12_IB_ERR_VERIFY; pairRef = 1; }   // the kext's pws93_seg
        // fence828's identity over the final constituent: the buried RELEASE_MEM found where Apple's input holds it
        uint32_t fenceIdent = 2;   // 2 = no candidate
        if (!st) {
            const uint32_t lh = isUnit ? gU.last_head_out : 0u;
            n48_f828 fr {};
            if (lh < olen && n48_f828_find(out + lh, olen - lh, &fr) == N48_F828_OK) {
                const uint32_t g = from + lh + fr.rel_at;
                fenceIdent = (g >= 1u && g < N && cat[g - 1u] == N48_MD_F828_NOP9 && cat[g] == N48_MD_F828_RELMEM) ? 1u : 0u;
            }
        }
        if (st) { std::memcpy(out, &cat[from], 4u * n); segsOk = 0; }
        (void)n48_mib_unit_undo(isUnit, st, &gPool);
        if (!st) xlat12_pool_add_free(&gPool, out, olen, ex.ib_va);
        if (!st) convTot += ds.pws_conv;
        if (!st) gSlotTot += ds.pws_slot;
        if (print)
            std::printf("U %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", F.no, pass, k, from, to, cc[k], isUnit, flagged, st,
                        ds.err_op, ds.pws_seen, ds.pws_conv, ds.pws_noroom, ds.pws_tail, ds.pws_fallback, fenceIdent, pairRef,
                        (unsigned)ds.pws_slot);
    }
    if (print) {
        static n48_md_scan_result r;
        (void)n48_md_scan(cand.data(), N, cat.data(), N, 0ull, 0ull, nullptr, 0u, segStart, nu, 0ull, 0u, &r);
        uint32_t originBad = 0;
        for (uint32_t q = 0; q < r.n; q++) if (!r.d[q].originOk) originBad++;
        uint32_t hf = 2166136261u;   // build 0.0.540 item 5: the whole candidate's FNV-1a, appended (decimal)
        for (uint32_t q = 0; q < N * 4u; q++) { hf ^= reinterpret_cast<const uint8_t *>(cand.data())[q]; hf *= 16777619u; }
        std::printf("R %u %u %u %u %u %u\n", F.no, pass, originBad, r.kindBad, segsOk, hf);
    }
    return convTot;
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <manifest> [<frame> <dword>]... | --decide44\n", argv[0]); return 2; }
#ifdef P93_DECIDE44
    if (!std::strcmp(argv[1], "--decide44")) {
        gRead = &f_read; gProfile = &f_profile;
        if (argc > 2 && !std::strcmp(argv[2], "--tc")) gTc = 1;   // build 0.0.540 item 5
        struct { uint32_t no, nib; const uint32_t *w[2]; uint32_t n[2]; uint64_t va[2]; } fr[4] = {
            { 54u, 1u, { kU54Ib0, nullptr }, { 6112u, 0u }, { kU54Ib0Va, 0ull } },
            { 59u, 1u, { kU59Ib0, nullptr }, { 8848u, 0u }, { kU59Ib0Va, 0ull } },
            { 77u, 2u, { kU77Ib0, kU77Ib1 }, { 1152u, 14944u }, { kU77Ib0Va, kU77Ib1Va } },
            { 115u, 1u, { kU115Ib0, nullptr }, { 2448u, 0u }, { kU115Ib0Va, 0ull } } };
        for (auto &q : fr) {
            Frame F; F.no = q.no;
            for (uint32_t k = 0; k < q.nib; k++) { F.va[k] = q.va[k]; F.off[k] = (uint32_t)F.cat.size(); F.nn[k] = q.n[k]; F.cat.insert(F.cat.end(), q.w[k], q.w[k] + q.n[k]); F.nib++; }
            (void)run_pass(F, F.cat, 0u, true);
            (void)run_pass(F, F.cat, 1u, true);
            // every Apple barrier of the frame, probed one at a time: converted (1) or not (0) under the real flags
            for (uint32_t i = 0; i + 8u <= F.cat.size(); i++) {
                if (!xlat12_ib_pws_apple_ok(&F.cat[i], 8u)) continue;
                std::vector<uint32_t> mut = F.cat; mut[i + 7u] ^= 0x40u;
                const uint32_t with = run_pass(F, F.cat, 1u, false), without = run_pass(F, mut, 1u, false);
                std::printf("C %u %u %u %u\n", F.no, i, with, without);
            }
        }
        return 0;
    }
#endif
    FILE *m = std::fopen(argv[1], "r");
    if (!m) return 2;
    std::vector<std::pair<uint32_t, uint32_t>> probes;
    std::vector<uint32_t> atAll;   // build 0.0.539: `--at d1,d2,...` - these dwords in every frame
    for (int a = 2; a < argc; ) {
        if (!std::strcmp(argv[a], "--mode") && a + 1 < argc) { gMode = std::atoi(argv[a + 1]); a += 2; continue; }
        if (!std::strcmp(argv[a], "--tc")) { gTc = 1; a += 1; continue; }   // build 0.0.540 item 5
        if (!std::strcmp(argv[a], "--at") && a + 1 < argc) {
            const std::string v = argv[a + 1];
            for (size_t p = 0; p < v.size(); ) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                atAll.push_back((uint32_t)std::strtoul(v.substr(p, q - p).c_str(), nullptr, 0));
                p = q + 1;
            }
            a += 2; continue;
        }
        if (a + 1 < argc) probes.push_back({ (uint32_t)std::strtoul(argv[a], nullptr, 0), (uint32_t)std::strtoul(argv[a + 1], nullptr, 0) });
        a += 2;
    }
    char line[4096];
    Frame F;
    while (std::fgets(line, sizeof line, m)) {
        char kind[32] = {}, path[2048] = {};
        unsigned long long va = 0; unsigned a1 = 0, a2 = 0;
        if (line[0] == 'F') { F = Frame {}; gRegs.clear(); std::sscanf(line, "F %u", &a1); F.no = a1; }
        else if (line[0] == 'I') {
            std::sscanf(line, "I %llx %u %2047s", &va, &a1, path);
            if (F.nib >= N48_XV_MAX_IBS) continue;
            std::vector<uint32_t> w = load(path);
            F.va[F.nib] = va; F.off[F.nib] = (uint32_t)F.cat.size(); F.nn[F.nib] = (uint32_t)w.size();
            F.cat.insert(F.cat.end(), w.begin(), w.end()); F.nib++;
        } else if (line[0] == 'R') {
            if (gMode) continue;   // modes 1 and 2 read no client memory and no program: never open a region file (iCloud may have evicted it)
            std::sscanf(line, "R %31s %llx %u %2047s", kind, &va, &a2, path);
            Reg r; r.kind = kind; r.va = va; r.w = load(path); if (a2 < r.w.size()) r.w.resize(a2);
            gRegs.push_back(r);
        } else if (line[0] == 'E') {
            if (F.cat.empty()) continue;
            (void)run_pass(F, F.cat, 0u, true);
            (void)run_pass(F, F.cat, 1u, true);
            std::vector<uint32_t> here;
            for (auto &p : probes) if (p.first == F.no) here.push_back(p.second);
            for (uint32_t d : atAll) here.push_back(d);
            for (uint32_t d : here) {
                if (d + 8u > F.cat.size() || !xlat12_ib_pws_apple_ok(&F.cat[d], 8u)) continue;   // Apple's barrier at that dword only
                std::vector<uint32_t> mut = F.cat;
                mut[d + 7u] ^= 0x40u;   // this one barrier no longer matches (GCR bit 6): every other input byte is the same
                const uint32_t with = run_pass(F, F.cat, 1u, false); const uint64_t sw = gSlotTot;
                const uint32_t without = run_pass(F, mut, 1u, false); const uint64_t so = gSlotTot;
                const uint32_t slotted = (d + 18u <= F.cat.size() && xlat12_ib_pws_slot_at(&F.cat[d], 18u)) ? 1u : 0u;
                std::printf("C %u %u %u %u %u %llu %llu\n", F.no, d, with, without, slotted, (unsigned long long)sw, (unsigned long long)so);
            }
        }
    }
    std::fclose(m);
    return 0;
}
