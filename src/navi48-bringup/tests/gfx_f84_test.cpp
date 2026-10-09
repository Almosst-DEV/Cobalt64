// gfx_f84_test.cpp — build 0.0.454 item 4: THE F84 HARNESS AS A HOST SUITE (; the investigation's
// scratch harness inv-f84/h84.cpp, turned into a pinned test).
//
// decide44's first Family A frame, F84 (WindowServer pid 2597, two IBs, twelve segments), through the REAL translator
// (src/xlat12/xlat12_ib.c, the same file the kext compiles) segment by segment exactly as the kext's gfxsrc_policy
// hands them over: the MIB segmenter (gfx_mib.h n48_mib_segment), each segment's own client VA (n48_mib_seg_va), and
// decide44's flag set - switch 27 (XLAT12_EXTRA_RASTER + RSRC3_GS), 34 (PAIR_PRE), 40 (READSET), the descriptor port
// (INLINE_DESC | TABLE_DESC | DESC_INV), 43 (every gated table row admitted), 44 (UD_REEMIT with a carry) and 45 (the
// frame-local provenance list, n48_dl, fed by every translated segment's output). Provenance: the five surfaces
// decide44's resprov had RECORDED before F84 (: 0x400003000, 0x400032000, 0x4000ba000, 0x400031000,
// 0x400025000), then the frame-local list - the investigation's "realistic" mode, which reproduced every one of the
// kext's nine logged PROVENANCE VAs. Client memory and program bytes come from tests/fixture_f84_decide44.h (generated
// from the capture; see its header).
//
// PINNED, one string per configuration (one character per segment, IB0 | IB1: '.' translated, 'P' PROVENANCE, 'M'
// NO_ROOM, 'E' REDIRECTED, 'S' SLOT_SHARED, 'X' any other status):
//   the new switches 48/49/50 OFF      XPPXPPP|PXPPP   decide44 read EPPXPPP|PXPPP on 0.0.451; seg 0's E (a false
//                                                      REDIRECTED) is gone because 0.0.453's fix E is UNGATED, so seg 0
//                                                      now stops at its next refusal, the per-translation raster rule
//                                                      (DRAW_SHAPE 0xFE, fix D's target) - pinned below by status.
//   49 + 50 (0.0.453's best)           .M.....|MX...   seg 1 NO_ROOM (AI's class-11 placement), seg 7 NO_ROOM, seg 8
//                                                      TOO_LONG / XLAT12_REEMIT_NO_ROOM; output bytes pinned to 0.0.453's.
//   44-50 ON (48 + 49 + 50)            .......|.....   every segment.
// plus 48 + 50 without 49, 48 alone, and 44 OFF. Then an EMULATED SHADER walks every table draw of every translated
// segment: the program's own address arithmetic over the TRANSLATED segment's own bytes must land on exactly the gfx12
// translation of the records APPLE'S stream names (read from the fixture's client memory by Apple's own user data).
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ src/navi48-bringup/tests/gfx_f84_test.cpp \
//         src/xlat12/xlat12.c src/xlat12/xlat12_ib.c -o /tmp/f84test && /tmp/f84test
// `/tmp/f84test --print` prints each configuration's per-segment status and output hash instead of checking them.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "xlat12_desc.h"   // the generated S# translator the emulated shader's reference uses
#include "gfx_mib.h"
#include "gfx_desc_port.h"
#include "fixture_f84_decide44.h"
#include "fixture_dcc_run10e.h"   // build 0.0.488 (DCC-DESC.md Q4 T3/T4)
#include "fixture_drawelide.h"    // build 0.0.500 (DRAW-ELIDE.md Q4 T2-T4)
#include "gfx_bb552.h"            // build 0.0.552 (switch 110): n48_an110_rows
#include "fixture_drawelide512.h" // build 0.0.512 (switch 66 M 7: the glass rows BD / BA)

static int gFail = 0, gRun = 0, gPrint = 0;
static void expect(const char *what, bool ok)
{
    gRun++;
    if (!ok) { gFail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

// ---- the fixture's client memory, program bytes and the resprov model ------------------------------------------------
static uint32_t gMiss = 0;
static int f84_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const F84Mem &m : kF84Mem)
        if (va >= m.va && va + 4ull * ndw <= m.va + 4ull * m.ndw && !((va - m.va) & 3u)) {
            const uint32_t o = m.off + (uint32_t)((va - m.va) / 4u);
            for (uint32_t k = 0; k < ndw; k++) out[k] = kF84MemW[o + k];
            return 1;
        }
    gMiss++;
    return 0;
}
static const uint64_t kF84Rp[5] = { 0x400003000ull, 0x400032000ull, 0x4000ba000ull, 0x400031000ull, 0x400025000ull };
static n48_dl gFl;
static uint32_t gAsks = 0;
static int f84_tiled(void *, uint64_t va, uint32_t mode, uint32_t)
{
    gAsks++;
    for (uint64_t r : kF84Rp) if (r == va) return 1;
    return n48_dl_tiled_ok(&gFl, 5u, va, mode) ? 1 : 0;
}
static int id_by_name(const char *nm)
{
    for (int i = 0; i < (int)xlat12_shader_id_count(); i++) if (!strcmp(xlat12_shader_id_name(i), nm)) return i;
    return -1;
}
static uint32_t gIo[2], gPgmBad = 0;
// the fixture's program for (stage, va): its identity, or -1
static int f84_pgm_id(uint32_t stage, uint64_t va)
{
    for (const F84Pgm &p : kF84Pgm)
        if (p.stage == stage && p.va == va) {
            const int id = xlat12_shader_id_match(stage, &kF84PgmW[p.off], p.ndw);
            if (id < 0 || strcmp(xlat12_shader_id_name(id), p.name)) { gPgmBad++; return -1; }
            return id;
        }
    if (stage == 0u) for (const F84Known &k : kF84Known) if (k.va == va) return id_by_name(k.name);
    return -1;
}
static int f84_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    const int id = f84_pgm_id(stage, va);
    if (id < 0) return 0;
    if (xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
    if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;   // h84's own rule, as the kext resolves it
    return 1;
}

// ---- one whole frame through the translator ---------------------------------------------------------------------------
struct SegOut { uint32_t st, op, in_dword, reused, appended, tables; uint32_t hash, hash498, undone, scAdj, hashNoVrs, vrsCost, vrsUndone, hashStrip; uint64_t va; uint32_t from, n; std::vector<uint32_t> out;
                uint32_t rs_declined; std::vector<uint64_t> rs_va, rs_ptr;
                uint64_t in_tbl, in_img, in_samp, in_va0; uint32_t in_n, in_abi; };
struct Frame { std::string s; std::vector<SegOut> seg; };
static std::vector<uint32_t> gCat;
static uint32_t gOff[2], gNn[2];
static char tdchar(uint32_t st, uint32_t op)
{
    if (st == 0u) return '.';
    if (st == XLAT12_IB_ERR_DESC) switch (op) {
        case XLAT12_TDESC_PROVENANCE: return 'P'; case XLAT12_TDESC_SLOT_UNSEEN: return 'U'; case XLAT12_TDESC_SLOT_SHARED: return 'S';
        case XLAT12_TDESC_REDIRECTED: return 'E'; case XLAT12_TDESC_NO_ROOM: return 'M'; case XLAT12_TDESC_READ: return 'R';
        case XLAT12_TDESC_TOO_MANY: return 'T'; default: return '?'; }
    if (st == XLAT12_IB_ERR_PAIR) return 'A';
    return 'X';
}
// build 0.0.502: every translation with a ring now writes gfx12 DB_SPI_VRS_CENTER_LOCATION (0x28068) = 0
// once in region 0 - mesa's gfx12 preamble value, ac_cmdbuf.c:702 `ac_pm4_set_reg(pm4, R_028068_DB_SPI_VRS_CENTER_LOCATION,
// 0);` - EITHER merged in front of the stream's own DB_SHADER_CONTROL (0x2806c, 1 dword) OR as the extra block's last
// packet (3 dwords). vrs_restore takes exactly that write back out (the same rule as src/xlat12/tests/test_xlat12_ib.c's
// vrs_restore): a standalone packet becomes three one-dword NOPs; a merged value leaves its packet (count - 1, offset
// 0x1b), everything up to the region's pad shifts back down one dword and the freed dword becomes the pad's first NOP. *cost is the
// dwords the write took (3, 1; 0 = not undone). Returns 1 when it undid exactly one 0x28068 = 0 write.
static bool f84_is_draw(uint32_t op)   // xlat12_ib.c is_draw: DRAW_INDEX_AUTO, _2, _OFFSET_2, DRAW_INDIRECT, DRAW_INDEX_INDIRECT
{
    return op == 0x2du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u;
}
static uint32_t f84_vrs_restore(std::vector<uint32_t> &b, uint32_t n, uint32_t *cost)
{
    uint32_t c = 0, at = 0, v = 1u, sh = 0, fd = n;
    *cost = 0u;
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = b[i];
        if (h == 0xFFFF1000u || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
        if (fd == n && f84_is_draw((h >> 8) & 0xFFu)) fd = i;
        if (((h >> 8) & 0xFFu) == 0x69u && l >= 3u && i + l <= n)
            for (uint32_t k = 0; k + 2u < l; k++)
                if (0x28000u + (((b[i + 1u] & 0xFFFFu) + k) << 2) == 0x28068u) {
                    c++; at = i; v = b[i + 2u + k];
                    sh = (l == 3u) ? 1u : ((k == 0u && (b[i + 1u] & 0xFFFFu) == 0x1Au) ? 2u : 3u);
                }
        i += l;
    }
    if (c != 1u || v != 0u || at >= fd) return 0u;
    if (sh == 1u) { b[at] = b[at + 1u] = b[at + 2u] = 0xFFFF1000u; *cost = 3u; return 1u; }
    // merged: the region's pad is the first one-dword NOP after the packet (the extra block sits last before it; on F84
    // that pad can lie past the first draw, so everything up to it moves back one dword, draws included)
    uint32_t pad = n;
    for (uint32_t i = at; i < n;) {
        const uint32_t h = b[i];
        if (h == 0xFFFF1000u) { pad = i; break; }
        if ((h >> 30) != 3u) { i++; continue; }
        i += ((h >> 16) & 0x3FFFu) + 2u;
    }
    if (sh != 2u || pad >= n || at + 3u >= pad) return 0u;
    b[at] -= 1u << 16; b[at + 1u] += 1u;
    for (uint32_t q = at + 2u; q + 1u < pad; q++) b[q] = b[q + 1u];
    b[pad - 1u] = 0xFFFF1000u;
    *cost = 1u;
    return 1u;
}
static Frame run(uint32_t add, int no44)
{
    Frame f;
    xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t ibnseg[2] = { 0u, 0u }, tot = 0;
    n48_mib_seg_diag dg {};
    const uint32_t ns = n48_mib_segment(gCat.data(), 2u, gOff, gNn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 0u, nullptr);
    n48_dl_clear(&gFl);
    static xlat12_ud_carry carry;
    uint32_t segIb = 0;
    for (uint32_t k = 0; k < ns; k++) {
        const uint32_t from = segs[k].start, n = segs[k].end - segs[k].start;
        while (segIb + 1u < 2u && from >= gOff[segIb] + gNn[segIb]) segIb++;
        xlat12_draw_extra ex {};
        ex.pgm_profile = &f84_profile;
        ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull;
        ex.flags = XLAT12_EXTRA_RASTER | XLAT12_EXTRA_PAIR_PRE | XLAT12_EXTRA_READSET |
                   XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;
        ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
        if (!no44) { ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &carry; }
        ex.flags |= add;
        ex.ib_va = n48_mib_seg_va(kF84IbVa[segIb], from, gOff[segIb]);
        ex.desc_read = &f84_read; ex.desc_tiled_ok = &f84_tiled;
        gIo[0] = gIo[1] = 0u;
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        SegOut so {}; so.out.assign(n + 16u, 0xDEADBEEFu); so.va = ex.ib_va; so.from = from; so.n = n;
        uint32_t olen = 0;
        so.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gCat[from], n, so.out.data(), &olen, &ds);
        so.op = ds.err_op; so.in_dword = ds.err_in_dword; so.reused = ds.table_reused; so.appended = ds.table_appended;
        so.tables = ds.table_draws;
        so.in_tbl = ds.in_tbl_va; so.in_img = ds.in_img_va; so.in_samp = ds.in_samp_va; so.in_va0 = ds.in_va[0];
        so.in_n = ds.in_n; so.in_abi = ds.in_abi;   // the LAST table draw's own export (d_in_clear empties it per draw)
        so.rs_declined = ds.rs_declined;   // the READSET (switch 40) union the table step's exports feed, per segment
        for (uint32_t q = 0; q < ds.rs_n && q < XLAT12_RS_IN_MAX; q++) so.rs_va.push_back(ds.rs_va[q]);
        for (uint32_t q = 0; q < ds.rs_nptr && q < XLAT12_RS_PTR_MAX; q++) so.rs_ptr.push_back(ds.rs_ptr[q]);
        uint32_t h = 2166136261u;
        if (so.st == 0u) for (uint32_t i = 0; i < olen; i++) { h ^= so.out[i]; h *= 16777619u; }
        for (uint32_t i = n; i < n + 16u; i++) if (so.out[i] != 0xDEADBEEFu) h = 0u;   // wrote past the segment: never a pin
        so.hash = so.st == 0u ? h : 0u;
        // build 0.0.499: the output with every gfx12 scissor BR put back to gfx10's exclusive value (+1 in
        // BR_X and BR_Y) - gfx12's BR is INCLUSIVE (mesa si_state.c gfx12 `S_028208_BR_X(state->width - 1) |    /+ inclusive
        // +/`, si_state_viewport.c gfx12 `S_028254_BR_X(final.maxx - 1)`, ac_cmdbuf.c gfx12 `BR_X(65535) ... inclusive
        // bounds` for 0x28184/0x28244), so 0.0.499 emits every BR -1. Undone, the output must be 0.0.453's byte for byte.
        so.scAdj = ds.scissor_adjusted; so.undone = 0u; so.hash498 = 0u; so.hashNoVrs = 0u; so.vrsCost = 0u; so.vrsUndone = 0u; so.hashStrip = 0u;
        if (so.st == 0u) {
            std::vector<uint32_t> u(so.out.begin(), so.out.begin() + olen);
            // build 0.0.502: the VRS-centre write out first (f84_vrs_restore), so hashNoVrs is 0.0.501's output
            so.vrsUndone = f84_vrs_restore(u, olen, &so.vrsCost);
            { uint32_t h3 = 2166136261u; for (uint32_t i = 0; i < olen; i++) { h3 ^= u[i]; h3 *= 16777619u; } so.hashNoVrs = h ? h3 : 0u; }
            // ... and its NON-NOP packets only (every NOP packet - pad dwords and deferred-record bodies - skipped): a merged
            // write moves everything up to the pad by one dword, so a 16-byte-aligned record NOP there re-pads (F84 seg 0 @967:
            // count 0x26 -> 0x25) and only this hash can equal 0.0.501's (kStrip501)
            { uint32_t h4 = 2166136261u;
              for (uint32_t i = 0; i < olen;) {
                  const uint32_t hh = u[i];
                  if ((hh >> 30) != 3u) { i++; continue; }
                  const uint32_t l = ((hh >> 16) & 0x3FFFu) + 2u;
                  if (hh == 0xFFFF1000u) { i++; continue; }
                  if (((hh >> 8) & 0xFFu) == 0x10u) { i += l; continue; }
                  for (uint32_t q = i; q < i + l && q < olen; q++) { h4 ^= u[q]; h4 *= 16777619u; }
                  i += l;
              }
              so.hashStrip = h ? h4 : 0u; }
            for (uint32_t i = 0; i < olen;) {
                const uint32_t hh = u[i];
                if (hh == 0xFFFF1000u || (hh >> 30) == 2u) { i++; continue; }
                if ((hh >> 30) != 3u) break;
                const uint32_t l = ((hh >> 16) & 0x3FFFu) + 2u;
                if (i + l > olen) break;
                if (((hh >> 8) & 0xFFu) == 0x69u)
                    for (uint32_t q = 0; q + 2u < l; q++) {
                        const uint32_t a = 0x28000u + (((u[i + 1u] & 0xFFFFu) + q) << 2);
                        if (a == 0x28184u || a == 0x28208u || a == 0x28244u || (a >= 0x28254u && a <= 0x282ccu && ((a - 0x28254u) & 7u) == 0u)) {
                            u[i + 2u + q] += 0x00010001u; so.undone++;
                        }
                    }
                i += l;
            }
            uint32_t h2 = 2166136261u;
            for (uint32_t i = 0; i < olen; i++) { h2 ^= u[i]; h2 *= 16777619u; }
            so.hash498 = h ? h2 : 0u;
        }
        if (k == ibnseg[0]) f.s += '|';
        f.s += tdchar(so.st, so.op);
        if (so.st == 0u) n48_dl_from_output(&gFl, 5u, so.out.data(), olen, nullptr);   // switch 45, the kext's own feed rule
        f.seg.push_back(so);
    }
    return f;
}

// ---- the emulated shader over one translated segment -------------------------------------------------------------------
static uint64_t sx(uint32_t v, uint32_t sh) { return (uint64_t)(int64_t)(int32_t)(v << sh); }
static bool out_read(const SegOut &s, uint64_t va, uint32_t ndw, uint32_t *w)
{
    if (va < s.va || ((va - s.va) & 3u) || va + 4ull * ndw > s.va + 4ull * s.n) return false;
    for (uint32_t k = 0; k < ndw; k++) w[k] = s.out[(uint32_t)((va - s.va) / 4u) + k];
    return true;
}
static bool is_draw_op(uint32_t op) { return op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u; }
// returns the number of table draws checked; *bad counts mismatches (with a line each)
static std::vector<uint64_t> gSurf, gTbl;   // emulate(): every T# surface / Apple table VA the draws it walked read
// emulate(): the LAST table draw's APPLE values - what a fresh placement exports for it
static struct { uint64_t tbl, img, samp, va0; uint32_t ntex; bool ok, lastDrawIsTable; } gLast;
static uint32_t emulate(const SegOut &s, uint32_t segNo, uint32_t *bad)
{
    const uint32_t *in = &gCat[s.from];
    const uint32_t *out = s.out.data();
    const int idAI = id_by_name("ws_AI_TmuaXh_Isrc_Isrc");
    uint32_t checked = 0, i = 0;
    while (i < s.n) {
        const uint32_t h = in[i];
        uint32_t l = 1u;
        if (h != XLAT12_IB_NOP && (h >> 30) == 3u) l = ((h >> 16) & 0x3FFFu) + 2u;
        if (h != XLAT12_IB_NOP && (h >> 30) == 3u && is_draw_op((h >> 8) & 0xFFu)) {
            uint32_t lo = 0, hi = 0;
            gLast.lastDrawIsTable = false;
            if (xlat12_ib_find_set(in, i, 0xb020u, &lo) && lo) {
                xlat12_ib_find_set(in, i, 0xb024u, &hi);
                const uint64_t pva = ((uint64_t)(hi & 0xFFu) << 40) | ((uint64_t)lo << 8);
                const int id = f84_pgm_id(0u, pva);
                uint32_t tb = 0, ntex = 0, tex[2] = { 0, 0 }, samp = 0;
                if (id >= 0 && xlat12_shader_id_desc_table(id, &tb, &ntex, tex, &samp)) {
                    const bool ai = id == idAI;
                    uint32_t a0 = 0, a1 = 0, o0 = 0, o1 = 0;
                    bool ok = xlat12_ib_find_set(in, i, 0xb030u + 4u * tb, &a0) && xlat12_ib_find_set(in, i, 0xb034u + 4u * tb, &a1) &&
                              xlat12_ib_find_set(out, i, 0xb030u + 4u * tb, &o0) && xlat12_ib_find_set(out, i, 0xb034u + 4u * tb, &o1);
                    const uint64_t atva = (uint64_t)a0 | ((uint64_t)a1 << 32), otva = (uint64_t)o0 | ((uint64_t)o1 << 32);
                    uint32_t ah0[2], ah1[2], oh0[2], oh1[2];
                    ok = ok && f84_read(nullptr, atva, 2u, ah0) && f84_read(nullptr, atva + 0x10u, 2u, ah1) &&
                         out_read(s, otva, 2u, oh0) && out_read(s, otva + 0x10u, 2u, oh1);
                    const uint64_t aib = (uint64_t)ah0[0] | ((uint64_t)ah0[1] << 32), asb = (uint64_t)ah1[0] | ((uint64_t)ah1[1] << 32);
                    const uint64_t oib = (uint64_t)oh0[0] | ((uint64_t)oh0[1] << 32), osb = (uint64_t)oh1[0] | ((uint64_t)oh1[1] << 32);
                    for (uint32_t t = 0; ok && t < ntex; t++) {
                        uint32_t ai_ = 0, oi = 0, rec[8], ref[8], got[8], d = 0;
                        ok = xlat12_ib_find_set(in, i, 0xb030u + 4u * tex[t], &ai_) && xlat12_ib_find_set(out, i, 0xb030u + 4u * tex[t], &oi) &&
                             f84_read(nullptr, aib + sx(ai_, 5u), 8u, rec) && xlat12_table_img_desc(rec, ref, &d) == 0u &&
                             out_read(s, oib + sx(oi, 5u), 8u, got) && !memcmp(ref, got, 32);
                        if (ok) gSurf.push_back(((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8));
                    }
                    if (ok) gTbl.push_back(atva);
                    gLast.tbl = atva; gLast.img = aib; gLast.ntex = ntex; gLast.ok = ok; gLast.lastDrawIsTable = true;
                    gLast.va0 = gSurf.empty() ? 0ull : gSurf[gSurf.size() - ntex];
                    // the sampler(s): direct index, or (AI) the class-11 entries table at s4:s5, one entry per texture
                    const uint32_t nsm = ai ? 2u : 1u;
                    for (uint32_t q = 0; ok && q < nsm; q++) {
                        uint32_t ai_ = 0, oi = 0, sr[4], ref[4], got[4];
                        if (ai) {
                            uint32_t p0 = 0, p1 = 0, r0 = 0, r1 = 0, ae[2], oe[2];
                            ok = xlat12_ib_find_set(in, i, 0xb030u + 16u, &p0) && xlat12_ib_find_set(in, i, 0xb034u + 16u, &p1) &&
                                 xlat12_ib_find_set(out, i, 0xb030u + 16u, &r0) && xlat12_ib_find_set(out, i, 0xb034u + 16u, &r1) &&
                                 f84_read(nullptr, ((uint64_t)p0 | ((uint64_t)p1 << 32)) + 8u * q, 2u, ae) &&
                                 out_read(s, ((uint64_t)r0 | ((uint64_t)r1 << 32)) + 8u * q, 2u, oe);
                            ai_ = ae[0]; oi = oe[0];
                        } else {
                            ok = xlat12_ib_find_set(in, i, 0xb030u + 4u * samp, &ai_) && xlat12_ib_find_set(out, i, 0xb030u + 4u * samp, &oi);
                        }
                        if (q == 0u) gLast.samp = asb + sx(ai_, 4u);
                        ok = ok && !(ai_ >> 31) && !(oi >> 31) && f84_read(nullptr, asb + sx(ai_, 4u), 4u, sr) &&
                             xlat12_samp_desc_g10_to_g12(sr, ref) == 0u && out_read(s, osb + sx(oi, 4u), 4u, got) && !memcmp(ref, got, 16);
                    }
                    checked++;
                    if (!ok) { (*bad)++; printf("  emulated shader MISMATCH: seg %u draw at dword %u (%s)\n", segNo, i, xlat12_shader_id_name(id)); }
                }
            }
        }
        i += l ? l : 1u;
    }
    return checked;
}

// =======================================================================================================================
// build 0.0.488 (an internal design note Q4 T3/T4; switch 60, XLAT12_EXTRA_DCC_STRIP) — THE DCC STRIP ON CAPTURED FRAMES.
// tests/fixture_dcc_run10e.h: run10e F56 IB0 (family a; its segment 0 is U's clock composite, the draw the kext logged as
// `r0 0/0x106/1043`) and F44 IB1 (segment 0 draws S into the fp16 layer 0x400460000, segment 1 draws BA, which reads it
// back through heap index 19 - a DCC T#), with the client memory and programs the translator reads, from the capture.
// The proofs: desc_dcc_ok is the kext's OWN n48_dl_dcc_ok (gfx_desc_port.h) over an EMPTY producer ledger and the
// frame-local list this harness feeds exactly as the kext does (each translated segment's own output, in order, only while
// switch 45 is on); desc_tiled_ok answers from the frame-local list plus a small ASSUMED set standing in for the rungs this
// build does not touch (F56: the composite X, which run10e's ledger held after 58; F44: 0x400034000 and 0x4007c8000,
// written by F44's own IB0 segments, and 0x401400000 / 0x4000be000, static sources - the 0.0.486 work). F44's two
// DISPATCH_DIRECT packets (N, BufferClear_CS) are replaced by same-length NOPs, standing in for 0.0.487's N elide (not in
// this tree): with Apple's bytes as they are, segment 0 refuses UNLISTED at the dispatch (checked below).
// =======================================================================================================================
struct DccFx { const uint32_t *ib; uint32_t n; uint64_t va; const DccMem *mem; uint32_t nmem; const uint32_t *memw;
               const DccPgm *pgm; uint32_t npgm; const uint32_t *pgmw; };
static const DccFx kFx56 = { kDcc56Ib0, (uint32_t)(sizeof kDcc56Ib0 / 4u), kDcc56Ib0Va, kDcc56Mem, (uint32_t)(sizeof kDcc56Mem / sizeof kDcc56Mem[0]),
                             kDcc56MemW, kDcc56Pgm, (uint32_t)(sizeof kDcc56Pgm / sizeof kDcc56Pgm[0]), kDcc56PgmW };
static const DccFx kFx44 = { kDcc44Ib1, (uint32_t)(sizeof kDcc44Ib1 / 4u), kDcc44Ib1Va, kDcc44Mem, (uint32_t)(sizeof kDcc44Mem / sizeof kDcc44Mem[0]),
                             kDcc44MemW, kDcc44Pgm, (uint32_t)(sizeof kDcc44Pgm / sizeof kDcc44Pgm[0]), kDcc44PgmW };
static const DccFx *gDfx;
static uint32_t gDMiss, gDPgmBad, gDAsks, gDViaFl, gDViaLed;
static n48_dl gDLed, gDFl;
static uint32_t gDFlOn, gDDccMode;   // gDDccMode: 0 = n48_dl_dcc_ok, 1 = NULL callback
static std::vector<uint64_t> gDAssume;
static int dcc_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (uint32_t i = 0; i < gDfx->nmem; i++) {
        const DccMem &m = gDfx->mem[i];
        if (va >= m.va && va + 4ull * ndw <= m.va + 4ull * m.ndw && !((va - m.va) & 3u)) {
            for (uint32_t k = 0; k < ndw; k++) out[k] = gDfx->memw[m.off + (uint32_t)((va - m.va) / 4u) + k];
            return 1;
        }
    }
    gDMiss++;
    return 0;
}
static int dcc_pgm_id(uint32_t stage, uint64_t va)
{
    for (uint32_t i = 0; i < gDfx->npgm; i++) {
        const DccPgm &p = gDfx->pgm[i];
        if (p.stage == stage && p.va == va) {
            const int id = xlat12_shader_id_match(stage, &gDfx->pgmw[p.off], p.ndw);
            if (id < 0 || strcmp(xlat12_shader_id_name(id), p.name)) { gDPgmBad++; return -1; }
            return id;
        }
    }
    return -1;
}
static int dcc_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    const int id = dcc_pgm_id(stage, va);
    if (id < 0) return 0;
    if (xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
    if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
    return 1;
}
static int dcc_tiled(void *, uint64_t va, uint32_t mode, uint32_t)
{
    for (uint64_t a : gDAssume) if (a == va) return 1;
    return (gDFlOn && n48_dl_tiled_ok(&gDFl, 5u, va, mode)) ? 1 : 0;
}
static int dcc_dccok(void *, uint64_t va, uint32_t mode, uint32_t)
{
    gDAsks++;
    const uint32_t via = n48_dl_dcc_ok(&gDLed, &gDFl, gDFlOn, 5u, va, mode, 0ull, 0u);   // the kext's own answer, no resprov
    if (via == N48_DL_DCC_FRAMELOCAL) gDViaFl++;
    if (via == N48_DL_DCC_LEDGER) gDViaLed++;
    return via != N48_DL_DCC_NONE ? 1 : 0;
}
struct DSeg { uint32_t from, n, st, op, at, stripped, unproven, refused, tables; uint64_t prov; uint32_t pmode; std::vector<uint32_t> out; };
static const uint32_t kDccBaseFlags = XLAT12_EXTRA_RASTER | XLAT12_EXTRA_PAIR_PRE | XLAT12_EXTRA_READSET | XLAT12_EXTRA_INLINE_DESC |
                                      XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV | XLAT12_EXTRA_UD_REEMIT | XLAT12_EXTRA_TABLE_REUSE |
                                      XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW;   // f84's "44-50 ON"
// segment the fixture's ONE IB as the kext does, then translate the listed segments IN THE ORDER GIVEN, feeding the
// frame-local list from each translated one (only while gDFlOn - the kext's `if (dp && gXdFrameLocalOn && st == 0u)`)
static std::vector<DSeg> dcc_run(const DccFx &fx, const std::vector<uint32_t> &ib, std::vector<uint32_t> order, uint32_t flags,
                                 std::vector<std::pair<uint32_t, uint32_t>> *bounds)
{
    gDfx = &fx;
    xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t off[1] = { 0u }, nn[1] = { (uint32_t)ib.size() }, ibnseg[1] = { 0u }, tot = 0;
    n48_mib_seg_diag dg {};
    const uint32_t ns = n48_mib_segment(ib.data(), 1u, off, nn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 0u, nullptr);
    if (bounds) { bounds->clear(); for (uint32_t k = 0; k < ns; k++) bounds->push_back({ segs[k].start, segs[k].end }); }
    std::memset(&gDLed, 0, sizeof gDLed); std::memset(&gDFl, 0, sizeof gDFl);
    std::vector<DSeg> r;
    static xlat12_ud_carry carry;
    for (uint32_t k : order) {
        DSeg d {};
        if (k >= ns) { d.st = 0xFFFFFFFFu; r.push_back(d); continue; }
        d.from = segs[k].start; d.n = segs[k].end - segs[k].start;
        xlat12_draw_extra ex {};
        ex.pgm_profile = &dcc_profile; ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
        ex.flags = flags; ex.ud_carry = &carry;
        ex.ib_va = n48_mib_seg_va(fx.va, d.from, 0u);
        ex.desc_read = &dcc_read; ex.desc_tiled_ok = &dcc_tiled; ex.desc_dcc_ok = gDDccMode == 1u ? nullptr : &dcc_dccok;
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        d.out.assign(d.n + 16u, 0xDEADBEEFu); uint32_t olen = 0;
        d.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &ib[d.from], d.n, d.out.data(), &olen, &ds);
        d.op = ds.err_op; d.at = ds.err_in_dword; d.prov = ds.prov_va; d.pmode = ds.prov_mode; d.tables = ds.table_draws;
        d.stripped = ds.dcc_stripped; d.unproven = ds.dcc_unproven; d.refused = ds.dcc_refused;
        if (d.st == 0u && gDFlOn) (void)n48_dl_from_output(&gDFl, 5u, d.out.data(), olen, nullptr);
        r.push_back(d);
    }
    return r;
}
// the emulated shader for ONE table draw at segment-relative dword `at`: follow the translated segment's own user data to
// the placed T# of texture slot `texSlot`; 1 when it equals `want`
static bool dcc_placed(const DSeg &s, uint64_t segVa, const std::vector<uint32_t> &ib, uint32_t at, uint32_t texSlot, const uint32_t want[8])
{
    uint32_t lo = 0, hi = 0, ix = 0, t[2];
    const uint32_t *o = s.out.data();
    if (!xlat12_ib_find_set(o, at, 0xb030u, &lo) || !xlat12_ib_find_set(o, at, 0xb034u, &hi) ||
        !xlat12_ib_find_set(o, at, 0xb030u + 4u * texSlot, &ix)) return false;
    (void)ib;
    auto rd = [&](uint64_t va, uint32_t ndw, uint32_t *w) {
        if (va < segVa || ((va - segVa) & 3u) || va + 4ull * ndw > segVa + 4ull * s.n) return false;
        for (uint32_t k = 0; k < ndw; k++) w[k] = o[(uint32_t)((va - segVa) / 4u) + k];
        return true; };
    const uint64_t tb = (uint64_t)lo | ((uint64_t)hi << 32);
    if (!rd(tb, 2u, t)) return false;
    const uint64_t heap = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
    uint32_t w[8];
    if (!rd(heap + (uint64_t)(int64_t)(int32_t)(ix << 5), 8u, w)) return false;
    return !memcmp(w, want, 32);
}
static uint32_t dcc_nop_dispatches(std::vector<uint32_t> &ib)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < ib.size();) {
        const uint32_t h = ib[i];
        if (h == 0xFFFF1000u || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
        if (((h >> 8) & 0xFFu) == 0x15u) { for (uint32_t k = 0; k < l && i + k < ib.size(); k++) ib[i + k] = 0xFFFF1000u; c++; }
        i += l;
    }
    return c;
}
static void dcc_strip_captured()
{
    const uint32_t ON = kDccBaseFlags | XLAT12_EXTRA_DCC_STRIP, OFF = kDccBaseFlags;
    /* DCC-DESC.md Q2 (A), quoted: run10e index 46 -> `04010800 c0660000 001fc05f 90300fac 00000000 00400000 00228000 00000000` */
    static const uint32_t k46G12[8] = { 0x04010800u, 0xc0660000u, 0x001fc05fu, 0x90300facu, 0u, 0x00400000u, 0x00228000u, 0u };
    /* the design's strip reference for run10e index 19 (src/xlat12/tests/fixture_dcc_trows.h, "run10e F33 0x4000b0260 idx 19") */
    static const uint32_t k19G12[8] = { 0x04004600u, 0xc0720000u, 0x001fc05fu, 0x90300facu, 0u, 0x00400000u, 0x00228000u, 0u };
    char line[256];
    // ---- T3: F56 IB0 segment 0, U's draw (IB0 dword 1045 = segment dword 1043) ----
    { std::vector<uint32_t> ib(kFx56.ib, kFx56.ib + kFx56.n);
      std::vector<std::pair<uint32_t, uint32_t>> b;
      gDMiss = gDPgmBad = 0; gDFlOn = 1u; gDDccMode = 0u; gDAssume = { 0x404800000ull };   // X, as run10e's ledger held it
      std::vector<DSeg> r = dcc_run(kFx56, ib, { 0u }, OFF, &b);
      expect("T3 F56: the kext's segmenter puts segment 0 at IB0 [2, 1051)", !b.empty() && b[0].first == 2u && b[0].second == 1051u);
      snprintf(line, sizeof line, "T3 F56 seg 0, 60 OFF: refused 0x106 at segment dword 1043 - the kext's own `r0 0/0x106/1043` (st %u op %#x at %u)",
               r[0].st, r[0].op, r[0].at);
      expect(line, r[0].st == XLAT12_IB_ERR_DESC && r[0].op == 0x106u && r[0].at == 1043u && r[0].stripped == 0u);
      gDAsks = 0;
      r = dcc_run(kFx56, ib, { 0u }, ON, nullptr);
      snprintf(line, sizeof line, "T3 F56 seg 0, 60 ON, dcc_ok 0 (empty ledger + frame-local): 0xf7 at 1043 naming 0x401080000 mode 3 (op %#x va %#llx)",
               r[0].op, (unsigned long long)r[0].prov);
      expect(line, r[0].st == XLAT12_IB_ERR_DESC && r[0].op == XLAT12_TDESC_PROVENANCE && r[0].at == 1043u && r[0].prov == 0x401080000ull &&
                   r[0].pmode == 3u && r[0].stripped == 1u && r[0].unproven == 1u && gDAsks == 1u);
      /* resprov alone: desc_tiled_ok answers YES for every surface, the clock layer included - and the strip still refuses */
      gDAssume = { 0x404800000ull, 0x401080000ull };
      r = dcc_run(kFx56, ib, { 0u }, ON, nullptr);
      expect("T3 F56 seg 0, 60 ON, desc_tiled_ok 1 for 0x401080000 (resprov's stand-in) but dcc_ok 0: still 0xf7 on 0x401080000",
             r[0].st == XLAT12_IB_ERR_DESC && r[0].op == XLAT12_TDESC_PROVENANCE && r[0].prov == 0x401080000ull);
      gDDccMode = 1u;
      r = dcc_run(kFx56, ib, { 0u }, ON, nullptr);
      expect("T3 F56 seg 0, 60 ON, no desc_dcc_ok at all (NULL): 0xf7 on 0x401080000",
             r[0].st == XLAT12_IB_ERR_DESC && r[0].op == XLAT12_TDESC_PROVENANCE && r[0].prov == 0x401080000ull);
      gDDccMode = 0u; gDAssume = { 0x404800000ull };
      /* dcc_ok 1: the producer ledger holds the clock layer (as F44's commit would feed it) */
      { static uint32_t cb[64]; uint32_t k = 0;
        cb[k++] = 0xC0016900u; cb[k++] = (0x28c60u - 0x28000u) >> 2; cb[k++] = (uint32_t)(0x401080000ull >> 8);
        cb[k++] = 0xC0016900u; cb[k++] = (0x28e40u - 0x28000u) >> 2; cb[k++] = 0u;
        cb[k++] = 0xC0016900u; cb[k++] = (0x28c7cu - 0x28000u) >> 2; cb[k++] = 3u << 15;
        cb[k++] = 0xC0016900u; cb[k++] = (0x28850u - 0x28000u) >> 2; cb[k++] = 0xFu;
        cb[k++] = 0xC0012D00u; cb[k++] = 3u; cb[k++] = 2u;
        std::memset(&gDLed, 0, sizeof gDLed); gDfx = &kFx56;
        xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t off[1] = { 0u }, nn[1] = { (uint32_t)ib.size() }, ibn[1] = { 0u }, tot = 0;
        n48_mib_seg_diag dg {}; (void)n48_mib_segment(ib.data(), 1u, off, nn, segs, N48_XV_MAX_SEGS, ibn, &tot, &dg, 0u, nullptr);
        xlat12_draw_extra ex {};
        ex.pgm_profile = &dcc_profile; ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
        static xlat12_ud_carry carry; ex.flags = ON; ex.ud_carry = &carry;
        ex.ib_va = n48_mib_seg_va(kFx56.va, segs[0].start, 0u); ex.desc_read = &dcc_read; ex.desc_tiled_ok = &dcc_tiled; ex.desc_dcc_ok = &dcc_dccok;
        std::memset(&gDFl, 0, sizeof gDFl);
        const uint32_t fed = n48_dl_from_output(&gDLed, 5u, cb, k, nullptr);
        gDViaLed = gDViaFl = 0;
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        DSeg d {}; d.from = segs[0].start; d.n = segs[0].end - segs[0].start; d.out.assign(d.n + 16u, 0xDEADBEEFu); uint32_t olen = 0;
        d.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &ib[d.from], d.n, d.out.data(), &olen, &ds);
        uint32_t tex8[8], d8 = 0, apple8 = 0, rec8[8];
        const bool placed46 = d.st == 0u && dcc_placed(d, ex.ib_va, ib, 1043u, 10u, k46G12);
        uint32_t atl = 0, ath = 0, ah[2] = { 0u, 0u };   // Apple's own table (s0:s1) and its image heap, from the fixture
        const bool haveX = xlat12_ib_find_set(&ib[d.from], 1043u, 0xb030u, &atl) && xlat12_ib_find_set(&ib[d.from], 1043u, 0xb034u, &ath) &&
                           dcc_read(nullptr, (uint64_t)atl | ((uint64_t)ath << 32), 2u, ah) &&
                           xlat12_ib_find_set(&ib[d.from], 1043u, 0xb030u + 32u, &apple8) &&
                           dcc_read(nullptr, ((uint64_t)ah[0] | ((uint64_t)ah[1] << 32)) + 32ull * apple8, 8u, rec8) &&
                           xlat12_table_img_desc(rec8, tex8, &d8) == 0u;
        const bool placedX = haveX && d.st == 0u && dcc_placed(d, ex.ib_va, ib, 1043u, 8u, tex8);
        snprintf(line, sizeof line, "T3 F56 seg 0, 60 ON, the LEDGER holds 0x401080000 (mode 3): translates; the emulated shader reads, through s10, "
                 "the design's gfx12 bytes for index 46 and, through s8, X's record (st %u fed %u via ledger %u)", d.st, fed, gDViaLed);
        expect(line, fed == 1u && d.st == 0u && ds.dcc_stripped == 1u && ds.dcc_unproven == 0u && gDViaLed == 1u && placed46 && placedX);
        std::memset(&gDLed, 0, sizeof gDLed); }
      snprintf(line, sizeof line, "T3 F56: every client read and program lookup served by the fixture (misses %u, bad %u)", gDMiss, gDPgmBad);
      expect(line, gDMiss == 0u && gDPgmBad == 0u); }
    // ---- T4: F44 IB1 segments 0 (S writes 0x400460000) and 1 (BA reads it: heap index 19, a DCC T#) - FRAME-LOCAL ----
    { std::vector<uint32_t> raw(kFx44.ib, kFx44.ib + kFx44.n), ib = raw;
      std::vector<std::pair<uint32_t, uint32_t>> b;
      gDMiss = gDPgmBad = 0; gDDccMode = 0u;
      const std::vector<uint64_t> assume = { 0x400034000ull, 0x4007c8000ull, 0x401400000ull, 0x4000be000ull };
      gDAssume = assume; gDFlOn = 1u;
      std::vector<DSeg> r = dcc_run(kFx44, raw, { 0u, 1u }, ON, &b);
      expect("T4 F44 IB1: the kext's segmenter puts segments 0 and 1 at [2, 2912) and [2914, 4272)",
             b.size() >= 2u && b[0].first == 2u && b[0].second == 2912u && b[1].first == 2914u && b[1].second == 4272u);
      snprintf(line, sizeof line, "T4 F44 IB1 as Apple wrote it: segment 0 refuses UNLISTED at N's dispatch (op %#x) - 0.0.487's elide is required",
               r[0].op);
      expect(line, r[0].st == XLAT12_IB_ERR_UNLISTED && r[0].op == 0x15u);
      const uint32_t nd = dcc_nop_dispatches(ib);
      expect("T4 the stand-in for 0.0.487's elide: F44 IB1's two DISPATCH_DIRECT packets become same-length NOPs", nd == 2u);
      gDViaFl = gDViaLed = 0; gDAsks = 0;
      r = dcc_run(kFx44, ib, { 0u, 1u }, ON, nullptr);
      snprintf(line, sizeof line, "T4 REACHABILITY, 60 ON + 45 ON, in submission order: seg 0 translates and is fed; seg 1's stripped T# "
               "(0x400460000) is PROVEN BY THE FRAME-LOCAL LIST and seg 1 translates (st %u/%u stripped %u via-fl %u)", r[0].st, r[1].st, r[1].stripped, gDViaFl);
      expect(line, r[0].st == 0u && r[1].st == 0u && r[1].stripped == 1u && r[1].unproven == 0u && gDAsks == 1u && gDViaFl == 1u && gDViaLed == 0u);
      expect("T4 ... and BA's placed T# for index 19 is the design's stripped gfx12 record",
             r[1].st == 0u && dcc_placed(r[1], n48_mib_seg_va(kFx44.va, r[1].from, 0u), ib, 1339u, 10u, k19G12));
      r = dcc_run(kFx44, ib, { 0u, 1u }, OFF, nullptr);
      snprintf(line, sizeof line, "T4 60 OFF: seg 0 translates, seg 1 refuses 0x106 at BA's draw (segment dword 1339) (st %u/%u op %#x at %u)",
               r[0].st, r[1].st, r[1].op, r[1].at);
      expect(line, r[0].st == 0u && r[1].st == XLAT12_IB_ERR_DESC && r[1].op == 0x106u && r[1].at == 1339u);
      gDFlOn = 0u;
      r = dcc_run(kFx44, ib, { 0u, 1u }, ON, nullptr);
      expect("T4 60 ON, 45 OFF: nothing feeds the frame-local list, seg 1 refuses 0xf7 naming 0x400460000",
             r[1].st == XLAT12_IB_ERR_DESC && r[1].op == XLAT12_TDESC_PROVENANCE && r[1].prov == 0x400460000ull && r[1].at == 1339u);
      gDAssume = assume; gDAssume.push_back(0x400460000ull);
      r = dcc_run(kFx44, ib, { 0u, 1u }, ON, nullptr);
      expect("T4 60 ON, 45 OFF, desc_tiled_ok 1 for 0x400460000 (resprov's stand-in): still 0xf7 - resprov cannot prove a stripped T#",
             r[1].st == XLAT12_IB_ERR_DESC && r[1].op == XLAT12_TDESC_PROVENANCE && r[1].prov == 0x400460000ull);
      gDAssume = assume; gDFlOn = 1u;
      r = dcc_run(kFx44, ib, { 1u, 0u }, ON, nullptr);
      expect("T4 60 ON + 45 ON, CONSUMER BEFORE PRODUCER (seg 1 translated first): 0xf7 on 0x400460000, then seg 0 translates",
             r[0].st == XLAT12_IB_ERR_DESC && r[0].op == XLAT12_TDESC_PROVENANCE && r[0].prov == 0x400460000ull && r[1].st == 0u);
      snprintf(line, sizeof line, "T4 F44: every client read and program lookup served by the fixture (misses %u, bad %u)", gDMiss, gDPgmBad);
      expect(line, gDMiss == 0u && gDPgmBad == 0u); }
    gDAssume.clear();
}

// =======================================================================================================================
// build 0.0.500 (an internal design note Q4 T2-T4, the reviewer's AO row; switch 66, XLAT12_EXTRA_DRAW_ELIDE) — THE DRAW
// ELIDE ON CAPTURED FRAME-a UNITS. tests/fixture_drawelide.h: frame a (1152|14944) unit k0 (IB0: U's clock composite at unit
// dword 1043, Y's second clock layer at 1116) of run10g F62, run10f F98 and run10p F57, and unit k3 (IB1, 4 constituents:
// AO's two panel-material draws at 1020 and 1060) of run10p F57, each through the kext's unit path with the harness's own flag
// set and ONLY the proofs that harness gave (X 0x404800000 tiled; nothing else). The clock layers and AO's 0x4005f8000 are
// unproven, exactly as on hardware (run10g/run10p: `r0 0/0xf7/1043`, `r1 3/0xf7/1020`).
// =======================================================================================================================
static const DeFx *gEfx;
static uint32_t gEMiss, gEPgmMiss, gEPgmBad; static uint64_t gEPgmMissVa;
static std::vector<uint64_t> gETiled, gEDcc; static uint64_t gEDropRead;   // gEDropRead: a record read that must fail
// gEPatch*: one 8-dword record (at gEPatchVa) served with words replaced (mask/set per word) - a mutated T#
static uint64_t gEPatchVa; static uint32_t gEPatchClr[8], gEPatchSet[8];
static int de_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    if (gEDropRead && va == gEDropRead) { gEMiss++; return 0; }
    for (uint32_t i = 0; i < gEfx->nmem; i++) {
        const DeMem &m = gEfx->mem[i];
        if (va >= m.va && va + 4ull * ndw <= m.va + 4ull * m.ndw && !((va - m.va) & 3u)) {
            for (uint32_t k = 0; k < ndw; k++) out[k] = gEfx->memw[m.off + (uint32_t)((va - m.va) / 4u) + k];
            if (gEPatchVa && va == gEPatchVa && ndw == 8u) for (uint32_t k = 0; k < 8u; k++) out[k] = (out[k] & ~gEPatchClr[k]) | gEPatchSet[k];
            return 1;
        }
    }
    gEMiss++;
    return 0;
}
static int de_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    for (uint32_t i = 0; i < gEfx->npgm; i++) {
        const DePgm &p = gEfx->pgm[i];
        if (p.stage != stage || p.va != va) continue;
        // build 0.0.512: ndw 0 = identified by the harness's program map (its image is longer than the capture): by NAME
        int id = -1;
        if (!p.ndw) { for (uint32_t q = 0; q < xlat12_shader_id_count(); q++) if (!strcmp(xlat12_shader_id_name((int)q), p.name)) id = (int)q; }
        else id = xlat12_shader_id_match(stage, &gEfx->pgmw[p.off], p.ndw);
        if (id < 0 || strcmp(xlat12_shader_id_name(id), p.name)) { gEPgmBad++; return 0; }
        if (xlat12_ib_profile_stage(id, out, gIo) != 0u) return 0;
        if (stage == 1u) out->vs_drops_params = (gIo[0] == 0u) ? 1u : 0u;
        return 1;
    }
    gEPgmMiss++; gEPgmMissVa = va;
    return 0;
}
static int de_tiled(void *, uint64_t va, uint32_t, uint32_t) { for (uint64_t a : gETiled) if (a == va) return 1; return 0; }
static int de_dcc(void *, uint64_t va, uint32_t, uint32_t) { for (uint64_t a : gEDcc) if (a == va) return 1; return 0; }
static int de_csn(void *, uint64_t) { return 1; }
struct ERes { uint32_t st, op, at; uint64_t prov; std::vector<uint32_t> out; xlat12_draw_stats ds; };
// build 0.0.512 Part B: the T# observer as the kext would hang it (counts, and BA's texture 0 at 2411 naming the surface)
static int gTexNoteOn; static uint32_t gTexNoteN, gTexNoteBa;
static void de_tex_note(void *, uint64_t id, uint32_t at_i, const uint32_t rec[8], uint64_t va)
{
    gTexNoteN++;
    if (id == ((120ull << 32) | 0x8e1812e4ull) && (at_i & 0xFFFFFFu) == 2411u && (at_i >> 24) == 0u && va == 0x402380000ull &&
        va == (((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8))) gTexNoteBa++;
}
static xlat12_unit gEU; static xlat12_pool gEPool; static xlat12_ud_carry gECarry;
// one translation of `in` (the fixture's unit, or a mutated copy) - as the unit the kext forms (unit 1), or IN PLACE (unit 0,
// a single segment: `in` is then that segment alone). rows 0 = switch 66 OFF (the flag unset).
static ERes de_run(const DeFx &fx, const std::vector<uint32_t> &in, uint32_t rows, int unit, int provAll)
{
    gEfx = &fx;
    const uint64_t own = 0x23f0000000ull;
    xlat12_draw_extra ex {};
    ex.pgm_profile = &de_profile; ex.ring_va = own; ex.gs_sgpr0_va = own + 0xa80000ull; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
    ex.flags = fx.flags & ~(uint32_t)XLAT12_EXTRA_UNIT;
    ex.fill_color_va = own + 0xA81000ull; ex.cs_ctx = nullptr; ex.cs_is_n = &de_csn; ex.ud_carry = &gECarry;
    ex.ib_va = fx.va; ex.desc_read = &de_read; ex.desc_tiled_ok = &de_tiled; ex.desc_dcc_ok = &de_dcc;
    if (rows) { ex.flags |= XLAT12_EXTRA_DRAW_ELIDE; ex.draw_elide_rows = rows; }
    if (gTexNoteOn) ex.tex_note = &de_tex_note;   // build 0.0.512 Part B
    std::vector<uint64_t> keepT = gETiled, keepD = gEDcc;
    if (provAll) { gETiled.clear(); gEDcc.clear();
                   for (uint32_t i = 0; i < fx.nmem; i++) for (uint32_t k = 0; k + 8u <= fx.mem[i].ndw || k == 0u; k += 8u) {
                       if (fx.mem[i].ndw != 8u) break;
                       const uint32_t *w = &fx.memw[fx.mem[i].off];
                       const uint64_t va = ((uint64_t)(w[1] & 0xFFu) << 40) | ((uint64_t)w[0] << 8);
                       gETiled.push_back(va); gEDcc.push_back(va); break; } }
    gEPool.nrun = 0; gEPool.lost = 0; gEPool.jn = 0;
    static uint32_t donor[4096];
    for (uint32_t k = 0; k < 4096u; k++) donor[k] = XLAT12_IB_NOP;
    (void)xlat12_pool_add_free(&gEPool, donor, 4096u, 0x23f0b00000ull);
    if (unit) {
        ex.flags |= XLAT12_EXTRA_UNIT; ex.unit = &gEU;
        gEU.ncons = fx.ncons; for (uint32_t j = 0; j < fx.ncons; j++) gEU.cons_in[j] = fx.cons_in[j];
        gEU.pool = &gEPool; gEU.cons_fn = nullptr; gEU.cons_ctx = nullptr; gEU.pend_cap = 0u;
    }
    ERes r {};
    r.out.assign(in.size() + 16u, 0xDEADBEEFu); uint32_t olen = 0;
    r.ds = xlat12_draw_stats {};
    r.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in.data(), (uint32_t)in.size(), r.out.data(), &olen, &r.ds);
    r.op = r.ds.err_op; r.at = r.ds.err_in_dword; r.prov = r.ds.prov_va;
    r.out.resize(in.size());
    gETiled = keepT; gEDcc = keepD;
    return r;
}
static void de_fx_proofs(const DeFx &fx)
{
    gETiled.assign(fx.tiled, fx.tiled + fx.ntiled); gEDcc.assign(fx.dcc, fx.dcc + fx.ndcc);
    gEDropRead = 0ull; gEPatchVa = 0ull;
    for (uint32_t k = 0; k < 8u; k++) { gEPatchClr[k] = 0u; gEPatchSet[k] = 0u; }
}
// the fixture's 8-dword T# record whose surface (w0 << 8 | w1[7:0] << 40) is `surf`: its VA, or 0
static uint64_t de_rec_of(const DeFx &fx, uint64_t surf)
{
    for (uint32_t i = 0; i < fx.nmem; i++) if (fx.mem[i].ndw == 8u) {
        const uint32_t *w = &fx.memw[fx.mem[i].off];
        if ((((uint64_t)(w[1] & 0xFFu) << 40) | ((uint64_t)w[0] << 8)) == surf) return fx.mem[i].va; }
    return 0ull;
}
static uint32_t de_nops6(const std::vector<uint32_t> &o)   // `c0041000 0 0 0 0 0` packets in the output
{
    uint32_t c = 0;
    for (size_t i = 0; i + 6u <= o.size(); i++)
        if (o[i] == 0xC0041000u && !o[i + 1] && !o[i + 2] && !o[i + 3] && !o[i + 4] && !o[i + 5]) c++;
    return c;
}
// the SET_CONTEXT_REG packet writing context dword `reg` before input dword `upto`: the index of its value, or ~0
static uint32_t de_ctx_at(const std::vector<uint32_t> &in, uint32_t reg, uint32_t upto)
{
    uint32_t hit = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < upto && i < in.size();) {
        const uint32_t h = in[i];
        if (h == XLAT12_IB_NOP || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
        if (((h >> 8) & 0xFFu) == 0x69u) { const uint32_t off = in[i + 1] & 0xFFFFu; if (reg >= off && reg < off + l - 2u) hit = i + 2u + (reg - off); }
        i += l;
    }
    return hit;
}
static void drawelide_captured()
{
    char line[320];
    const uint32_t UY = XLAT12_DE_CLASS_UY, AO = XLAT12_DE_CLASS_AO;
    // ---- T2 / T4 / OFF identity: unit k0 of three runs ----
    struct K0 { const DeFx *fx; uint64_t uva, yva; };
    const K0 k0s[3] = { { &kDeFxG62K0, 0x401080000ull, 0x401660000ull }, { &kDeFxF98K0, 0x401080000ull, 0x4023c0000ull },
                        { &kDeFxP57K0, 0x401160000ull, 0x4015c0000ull } };
    for (const K0 &k : k0s) {
        const DeFx &fx = *k.fx; const std::vector<uint32_t> in(fx.in, fx.in + fx.n);
        de_fx_proofs(fx); gEMiss = gEPgmMiss = gEPgmBad = 0;
        const ERes off = de_run(fx, in, 0u, 1, 0);
        snprintf(line, sizeof line, "T2 %s, 66 OFF: refused 0xf7 at 1043 naming U's clock layer %#llx (st %u op %#x at %u va %#llx)",
                 fx.tag, (unsigned long long)k.uva, off.st, off.op, off.at, (unsigned long long)off.prov);
        expect(line, off.st == XLAT12_IB_ERR_DESC && off.op == XLAT12_TDESC_PROVENANCE && off.at == 1043u && off.prov == k.uva &&
                     off.ds.draw_elided == 0u && off.ds.de_seen == 0u);
        const ERes ao = de_run(fx, in, AO, 1, 0);
        snprintf(line, sizeof line, "T2 %s, 66 ON with the AO class only: U is not a row - the SAME refusal, the SAME output bytes "
                 "(not-row %u census %u/%#x)", fx.tag, ao.ds.de_not_row, ao.ds.de_nr_ndw, ao.ds.de_nr_fnv);
        expect(line, ao.st == off.st && ao.op == off.op && ao.at == off.at && ao.out == off.out && ao.ds.de_seen == 1u &&
                     ao.ds.de_not_row == 1u && ao.ds.de_nr_ndw == 192u && ao.ds.de_nr_fnv == 0x92c6ae13u && ao.ds.draw_elided == 0u);
        const ERes on = de_run(fx, in, UY, 1, 0);
        snprintf(line, sizeof line, "T2 %s, 66 ON (U/Y): the unit TRANSLATES with U (1043) and Y (1116) elided, naming %#llx and %#llx "
                 "(st %u op %#x elided %u at %u/%u rows %u/%u va %#llx/%#llx)", fx.tag, (unsigned long long)k.uva, (unsigned long long)k.yva,
                 on.st, on.op, on.ds.draw_elided, on.ds.de_at[0], on.ds.de_at[1], (uint32_t)on.ds.de_row[0], (uint32_t)on.ds.de_row[1],
                 (unsigned long long)on.ds.de_va8[0] << 8, (unsigned long long)on.ds.de_va8[1] << 8);
        expect(line, on.st == 0u && on.ds.draw_elided == 2u && on.ds.de_seen == 2u && on.ds.de_at[0] == 1043u && on.ds.de_at[1] == 1116u &&
                     on.ds.de_row[0] == XLAT12_DE_ROW_U && on.ds.de_row[1] == XLAT12_DE_ROW_Y && on.ds.de_va8[0] * 256ull == k.uva &&
                     on.ds.de_va8[1] * 256ull == k.yva && on.ds.de_mode[0] == 3u && on.ds.de_mode[1] == 3u && on.ds.dcc_unproven == 2u);
        expect("T2 ... both draws are same-length NOPs (c0041000 0 0 0 0 0) and NO draw packet is left (the backstop's count)",
               on.st == 0u && de_nops6(on.out) == 2u && xlat12_ib_count_draws(on.out.data(), (uint32_t)on.out.size()) == 0u);
        expect("T2 ... the exports name nothing (d_in_clear: in_n, in_abi, the table/heap/sampler pages all 0) and no refusal is left",
               on.ds.in_n == 0u && on.ds.in_abi == 0u && on.ds.in_tbl_va == 0ull && on.ds.in_img_va == 0ull && on.ds.in_samp_va == 0ull &&
               on.ds.prov_va == 0ull && on.ds.err_op == 0xFFFFFFFFu);
        expect("T2 ... no deferred table block was opened for either draw (nothing placed, nothing redirected)",
               on.st == 0u && gEU.npend == 0u && gEU.placed == 0u);
        // every ask proven (the stand-in for "the layers were proven"): the same unit translates WITH both draws
        const ERes pv = de_run(fx, in, 0u, 1, 1);
        snprintf(line, sizeof line, "T2 %s positive control, every ask proven, 66 OFF: translates with both draws kept (st %u draws %u)",
                 fx.tag, pv.st, xlat12_ib_count_draws(pv.out.data(), (uint32_t)pv.out.size()));
        expect(line, pv.st == 0u && xlat12_ib_count_draws(pv.out.data(), (uint32_t)pv.out.size()) == 2u && de_nops6(pv.out) == 0u);
        expect("T2 ... r4 unchanged by the elide (the same WAIT_REG_MEM / memory-write counts as the proven translation)",
               on.ds.r4_waits == pv.ds.r4_waits && on.ds.r4_memwrites == pv.ds.r4_memwrites);
        expect("T2 ... the scissor rule (0.0.499) unaffected: the same BR adjustments, none refused",
               on.ds.scissor_adjusted == pv.ds.scissor_adjusted && on.ds.scissor_adjusted > 0u && on.ds.scissor_refused == 0u);
        // T4: the frame-local / ledger feed records no target from the elided draws (n48_dl_from_output: draws only)
        n48_dl dl; std::memset(&dl, 0, sizeof dl);
        const uint32_t addedOn = n48_dl_from_output(&dl, 5u, on.out.data(), (uint32_t)on.out.size(), nullptr);
        std::memset(&dl, 0, sizeof dl);
        const uint32_t addedPv = n48_dl_from_output(&dl, 5u, pv.out.data(), (uint32_t)pv.out.size(), nullptr);
        snprintf(line, sizeof line, "T4 %s: n48_dl_from_output over the elided output records NOTHING (%u); over the proven one, X (%u)",
                 fx.tag, addedOn, addedPv);
        expect(line, addedOn == 0u && addedPv >= 1u && n48_dl_tiled_ok(&dl, 5u, 0x404800000ull, 3u));
        snprintf(line, sizeof line, "T2 %s: every client read and program lookup served by the fixture (misses %u, pgm %u, bad %u)",
                 fx.tag, gEMiss, gEPgmMiss, gEPgmBad);
        expect(line, gEMiss == 0u && gEPgmMiss == 0u && gEPgmBad == 0u);
    }
    const DeFx &g = kDeFxG62K0; const std::vector<uint32_t> gin(g.in, g.in + g.n);
    // ---- T3: Y's refusal is real - with U's draw already a NOP in the input, only the Y read is left ----
    { std::vector<uint32_t> in = gin;
      for (uint32_t k = 0; k < 6u; k++) in[1043u + k] = k ? 0u : 0xC0041000u;
      de_fx_proofs(g);
      const ERes a = de_run(g, in, AO, 1, 0);
      snprintf(line, sizeof line, "T3 G62 k0 with U pre-NOPed, 66 ON AO only: refuses 0xf7 at 1116 naming Y's stripped layer 0x401660000 "
               "(st %u op %#x at %u va %#llx dcc-unproven %u)", a.st, a.op, a.at, (unsigned long long)a.prov, a.ds.dcc_unproven);
      expect(line, a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1116u && a.prov == 0x401660000ull &&
                   a.ds.dcc_unproven == 1u && a.ds.de_not_row == 1u && a.ds.de_nr_ndw == 372u);
      const ERes b = de_run(g, in, UY, 1, 0);
      expect("T3 ... and with U/Y: translates, Y elided (1 of 1)", b.st == 0u && b.ds.draw_elided == 1u && b.ds.de_row[0] == XLAT12_DE_ROW_Y); }
    // ---- ORDERING: in place (the unit's first constituent alone, a single segment): the elided translation is the refused one's
    //      output up to the draw, byte for byte (no record placed in the pad, no slot redirected), then the NOP ----
    { const std::vector<uint32_t> seg(gin.begin(), gin.begin() + g.cons_in[1]);
      de_fx_proofs(g);
      const ERes off = de_run(g, seg, 0u, 0, 0), on = de_run(g, seg, UY, 0, 0);
      bool same = off.st == XLAT12_IB_ERR_DESC && on.st == 0u && seg.size() == 1049u;
      for (uint32_t k = 0; same && k < 1043u; k++) same = off.out[k] == on.out[k];
      snprintf(line, sizeof line, "ORDER in place: dwords [0, 1043) of the elided output == the refused translation's (the table step "
               "placed and redirected nothing), then c0041000 0 0 0 0 0 at 1043 (st %u/%u)", off.st, on.st);
      expect(line, same && on.out[1043] == 0xC0041000u && !on.out[1044] && !on.out[1045] && !on.out[1046] && !on.out[1047] && !on.out[1048]); }
    // ---- NOT THE LAST TEXTURE: X (texture 0, s8) unproven - the refusal is on texture 0, never elided ----
    { de_fx_proofs(g); gETiled.clear();
      const ERes a = de_run(g, gin, UY, 1, 0);
      snprintf(line, sizeof line, "NOT-LAST G62 k0, X unproven: refuses 0xf7 at 1043 naming X 0x404800000, not-last 1, nothing elided "
               "(st %u op %#x va %#llx)", a.st, a.op, (unsigned long long)a.prov);
      expect(line, a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u && a.prov == 0x404800000ull &&
                   a.ds.de_not_last == 1u && a.ds.draw_elided == 0u); }
    // ---- EARLIER TEXTURE NOT PROVEN: X (texture 0) made a LINEAR record the step admits unproven (lin), so the refusal lands on
    //      the LAST texture - and must still not elide: every earlier texture must be PROVEN, not merely admitted ----
    { de_fx_proofs(g); gETiled.clear();
      gEPatchVa = de_rec_of(g, 0x404800000ull); gEPatchClr[3] = 0x1Fu << 20;   /* gfx10 T# word 3 SW_MODE [24:20] -> 0 (linear) */
      const ERes a = de_run(g, gin, UY, 1, 0);
      snprintf(line, sizeof line, "EARLIER G62 k0, X linear and unproven: U refuses 0xf7 at 1043 on its LAST texture (the clock layer) but "
               "is NOT elided - not-last 1 (st %u op %#x va %#llx lin %u nl %u el %u)", a.st, a.op, (unsigned long long)a.prov,
               a.ds.table_lin_unproven, a.ds.de_not_last, a.ds.draw_elided);
      expect(line, gEPatchVa != 0ull && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u &&
                   a.prov == 0x401080000ull && a.ds.de_not_last == 1u && a.ds.draw_elided == 0u);
      gEPatchVa = 0ull; }
    // ---- NO DCC: U's clock layer made a plain (non-DCC) tiled record - a U/Y row requires a stripped DCC texture ----
    { de_fx_proofs(g);
      gEPatchVa = de_rec_of(g, 0x401080000ull);
      /* the metadata address and the gfx10-only DCC hints gone (DCC-DESC.md T5's "COMPRESSION_EN without metadata" shape):
       * no DCC record any more, a plain tiled T# the port translates as today */
      gEPatchClr[6] = (0xFFu << 24) | (1u << 19) | (1u << 22) | (3u << 15); gEPatchClr[7] = 0xFFFFFFFFu;
      const ERes a = de_run(g, gin, UY, 1, 0);
      snprintf(line, sizeof line, "NO-DCC G62 k0, U's layer without DCC metadata: 0xf7 at 1043 naming it, no-dcc 1, nothing elided "
               "(st %u op %#x va %#llx stripped %u nd %u el %u)", a.st, a.op, (unsigned long long)a.prov, a.ds.dcc_stripped, a.ds.de_no_dcc,
               a.ds.draw_elided);
      expect(line, gEPatchVa != 0ull && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u &&
                   a.prov == 0x401080000ull && a.ds.dcc_stripped == 0u && a.ds.de_no_dcc == 1u && a.ds.draw_elided == 0u);
      gEPatchVa = 0ull; }
    // ---- NOT PROVENANCE: U's layer T# unreadable (a READ refusal at the same draw) - never examined ----
    { de_fx_proofs(g);
      const ERes base = de_run(g, gin, 0u, 1, 0);
      // the T# the refusal read last: its record is the 8-dword read at the image heap, whose VA names the surface
      for (uint32_t i = 0; i < g.nmem; i++) if (g.mem[i].ndw == 8u) {
          const uint32_t *w = &g.memw[g.mem[i].off];
          if ((((uint64_t)(w[1] & 0xFFu) << 40) | ((uint64_t)w[0] << 8)) == base.prov) gEDropRead = g.mem[i].va; }
      const ERes a = de_run(g, gin, UY, 1, 0);
      snprintf(line, sizeof line, "NOT-PROV G62 k0, U's layer record unreadable: refuses READ (op %#x) at 1043, de_seen 0, nothing elided",
               a.op);
      expect(line, gEDropRead != 0ull && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_READ && a.at == 1043u &&
                   a.ds.de_seen == 0u && a.ds.draw_elided == 0u);
      gEDropRead = 0ull; }
    // ---- THE WRITE SET (fail-open): depth, stencil, streamout or a bound CB1 before the draw - refused, counted ----
    { struct Mut { const char *what; uint32_t reg; uint32_t val; };
      const Mut mut[4] = { { "DB_Z_INFO FORMAT 1 (a depth surface)", 0x10u, 1u }, { "DB_STENCIL_INFO FORMAT 1", 0x11u, 1u },
                           { "VGT_STRMOUT_CONFIG 1 (streamout)", 0x2e5u, 1u }, { "CB_COLOR1_BASE bound", 0x318u + 0xfu, 0x4048100u } };
      for (const Mut &m : mut) {
          std::vector<uint32_t> in = gin;
          const uint32_t at = de_ctx_at(in, m.reg, 1043u);
          if (at != 0xFFFFFFFFu) in[at] = m.val;
          de_fx_proofs(g);
          const ERes a = de_run(g, in, UY, 1, 0);
          snprintf(line, sizeof line, "WRITESET G62 k0 with %s written before U: refuses 0xf7 at 1043, write-set 1, nothing elided "
                   "(found %u st %u op %#x ws %u el %u)", m.what, at != 0xFFFFFFFFu, a.st, a.op, a.ds.de_writeset, a.ds.draw_elided);
          expect(line, at != 0xFFFFFFFFu && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u &&
                       a.ds.de_writeset == 1u && a.ds.draw_elided == 0u);
      }
      // a register-destination WRITE_DATA before the draw (a register write the scan cannot see through): the 8-dword NOP
      // `c0061000 ...` near the unit's head becomes WRITE_DATA (DST_SEL 0) of 4 dwords to CB_BLEND_RED..ALPHA (context dwords
      // 0xa105-0xa108, IDENTICAL on gfx12, so the translator itself admits the packet and the elide is what must refuse)
      std::vector<uint32_t> in = gin;
      uint32_t nopAt = 0xFFFFFFFFu;
      for (uint32_t i = 0; i + 8u < 1043u; i++) if (in[i] == 0xC0061000u) { nopAt = i; break; }
      if (nopAt != 0xFFFFFFFFu) { in[nopAt] = 0xC0063700u; in[nopAt + 1] = 0x00000000u; in[nopAt + 2] = 0xa105u; in[nopAt + 3] = 0u;
                                  for (uint32_t k = 4; k < 8u; k++) in[nopAt + k] = 0u; }
      de_fx_proofs(g);
      const ERes a = de_run(g, in, UY, 1, 0);
      snprintf(line, sizeof line, "WRITESET G62 k0 with a register WRITE_DATA before U: refuses 0xf7 at 1043, write-set 1, nothing elided "
               "(st %u op %#x ws %u el %u)", a.st, a.op, a.ds.de_writeset, a.ds.draw_elided);
      expect(line, nopAt != 0xFFFFFFFFu && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u &&
                   a.ds.de_writeset == 1u && a.ds.draw_elided == 0u); }
    // ---- THE CAP: a third elidable draw in one translation (Y's constituent appended again) - refused, counted ----
    { std::vector<uint32_t> in = gin; in.insert(in.end(), gin.begin() + g.cons_in[1], gin.end());
      DeFx f3 = g; f3.ncons = 3u; f3.cons_in[2] = (uint32_t)gin.size();
      de_fx_proofs(g);
      const ERes a = de_run(f3, in, UY, 1, 0);
      snprintf(line, sizeof line, "CAP G62 k0 + Y's constituent again: two elided, the third refused (cap %u st %u op %#x at %u)",
               a.ds.de_cap, a.st, a.op, a.at);
      expect(line, a.ds.draw_elided == XLAT12_DRAW_ELIDE_MAX && a.ds.de_cap == 1u && a.st == XLAT12_IB_ERR_DESC &&
                   a.op == XLAT12_TDESC_PROVENANCE); }
    // ---- AO (the reviewer's second class): run10p F57 unit k3 ----
    { const DeFx &f = kDeFxP57K3; const std::vector<uint32_t> in(f.in, f.in + f.n);
      de_fx_proofs(f); gEMiss = gEPgmMiss = gEPgmBad = 0;
      const ERes off = de_run(f, in, 0u, 1, 0);
      snprintf(line, sizeof line, "AO P57 k3, 66 OFF: refuses 0xf7 at 1020 naming 0x4005f8000 (st %u op %#x at %u va %#llx)",
               off.st, off.op, off.at, (unsigned long long)off.prov);
      expect(line, off.st == XLAT12_IB_ERR_DESC && off.op == XLAT12_TDESC_PROVENANCE && off.at == 1020u && off.prov == 0x4005f8000ull);
      const ERes uy = de_run(f, in, UY, 1, 0);
      snprintf(line, sizeof line, "AO P57 k3, 66 M 1 (U/Y only): the AO row is NOT active - the same refusal, not-row 1, census 168/0xb4fc3c24 "
               "(st %u op %#x at %u nr %u/%#x)", uy.st, uy.op, uy.at, uy.ds.de_nr_ndw, uy.ds.de_nr_fnv);
      expect(line, uy.st == off.st && uy.op == off.op && uy.at == 1020u && uy.out == off.out && uy.ds.de_not_row == 1u &&
                   uy.ds.de_nr_ndw == 168u && uy.ds.de_nr_fnv == 0xb4fc3c24u && uy.ds.draw_elided == 0u);
      gEPgmMiss = 0;
      const ERes m3 = de_run(f, in, UY | AO, 1, 0);
      snprintf(line, sizeof line, "AO P57 k3, 66 M 3: both AO draws elided (1020, 1060), then the NEXT refusal: PAIR at 1159, the fragment "
               "program at %#llx no capture holds (st %u op %#x at %u el %u)", (unsigned long long)gEPgmMissVa, m3.st, m3.op, m3.at,
               m3.ds.draw_elided);
      expect(line, m3.ds.draw_elided == 2u && m3.ds.de_at[0] == 1020u && m3.ds.de_at[1] == 1060u && m3.ds.de_row[0] == XLAT12_DE_ROW_AO &&
                   m3.ds.de_row[1] == XLAT12_DE_ROW_AO && m3.ds.de_va8[0] * 256ull == 0x4005f8000ull && m3.ds.dcc_unproven == 0u &&
                   m3.st == XLAT12_IB_ERR_PAIR && m3.at == 1159u && gEPgmMiss >= 1u && gEPgmMissVa == 0x4011f4300ull);
      const ERes ao = de_run(f, in, AO, 1, 0);
      expect("AO P57 k3, the AO class alone: the same two elisions", ao.ds.draw_elided == 2u && ao.st == m3.st && ao.at == m3.at);
      snprintf(line, sizeof line, "AO P57 k3: every client read served by the fixture, no program mismatched (misses %u, bad %u)", gEMiss, gEPgmBad);
      expect(line, gEMiss == 0u && gEPgmBad == 0u); }
    // ---- build 0.0.552 (switch 110, xlat12_ib.h XLAT12_DE_CLASS_AN / _AN_SHADOW): THE AN ROW. P57 k3 with its two panel-material
    //      draws' fragment program RELABELLED: the harness identifies it by NAME (ndw 0) as AN (ws_AN_TmuaXh_Isrc_Isrc, {122,
    //      0x7b3a6dfe}) or as AI (ws_AI_TmuaXh_Isrc_Isrc, {119, 0xae6c50d3}: AN's own function compiled a second time), so the SAME
    //      stream carries two draws under that identity. AN draws commit on hardware, so the row must act BEFORE the table step.
    { const DeFx &f0 = kDeFxP57K3; const std::vector<uint32_t> in(f0.in, f0.in + f0.n);
      static DePgm pgAn[4], pgAi[4];
      for (uint32_t i = 0; i < 4u; i++) { pgAn[i] = f0.pgm[i]; pgAi[i] = f0.pgm[i]; }
      pgAn[0].ndw = 0u; pgAn[0].name = "ws_AN_TmuaXh_Isrc_Isrc";
      pgAi[0].ndw = 0u; pgAi[0].name = "ws_AI_TmuaXh_Isrc_Isrc";
      DeFx fAn = f0, fAi = f0; fAn.pgm = pgAn; fAi.pgm = pgAi; fAn.tag = "P57K3 as AN"; fAi.tag = "P57K3 as AI";
      const uint32_t AN = XLAT12_DE_CLASS_AN, ANS = XLAT12_DE_CLASS_AN_SHADOW, M7 = UY | AO | XLAT12_DE_CLASS_GLASS;
      expect("AN552 the row's key is AN's kDTableAbi identity and the bits are gfx_bb552.h's (0x8 ON, 0x10 SHADOW, row 6)",
             std::strcmp(f0.pgm[0].name, "ws_AO_TmuaXh_IsrcCcl_Icir") == 0 && XLAT12_DE_AN_NDW == 122u && XLAT12_DE_AN_FNV == 0x7b3a6dfeu &&
             AN == 0x8u && ANS == 0x10u && XLAT12_DE_ROW_AN == 6u);
      for (int prov = 0; prov < 2; prov++) {
          de_fx_proofs(fAn); gEMiss = gEPgmMiss = gEPgmBad = 0;
          const ERes base = de_run(fAn, in, M7, 1, prov);         // 66 ON (M 7), 110 OFF
          const ERes none = de_run(fAn, in, 0u, 1, prov);         // 66 OFF
          const ERes sh = de_run(fAn, in, M7 | ANS, 1, prov);     // 110 SHADOW
          const ERes on = de_run(fAn, in, M7 | AN, 1, prov);      // 110 ON
          const ERes onA = de_run(fAn, in, AN, 1, prov);          // the AN bit alone
          snprintf(line, sizeof line, "AN552 prov %d: 110 OFF (66 M 7) st %u op %#x at %u; SHADOW st %u at %u seen %u would %u; ON st %u "
                   "op %#x at %u elided %u at %u/%u rows %u/%u", prov, base.st, base.op, base.at, sh.st, sh.at, (uint32_t)sh.ds.de_an_seen,
                   (uint32_t)sh.ds.de_an_would, on.st, on.op, on.at, on.ds.draw_elided, on.ds.de_at[0], on.ds.de_at[1],
                   (uint32_t)on.ds.de_row[0], (uint32_t)on.ds.de_row[1]);
          std::printf("      %s\n", line);
          expect("AN552 OFF identity: 110 OFF with 66 ON sees no AN (seen 0, would 0), and AN is not one of 66's rows (nothing elided as AN)",
                 base.ds.de_an_seen == 0u && base.ds.de_an_would == 0u && base.ds.draw_elided == 0u);
          expect("AN552 SHADOW changes no verdict: the same status, refusal, dword and output bytes as 110 OFF; nothing elided",
                 sh.st == base.st && sh.op == base.op && sh.at == base.at && sh.prov == base.prov && sh.out == base.out &&
                 sh.ds.draw_elided == 0u && sh.ds.de_seen == base.ds.de_seen && sh.ds.in_n == base.ds.in_n);
          expect("AN552 SHADOW counts: AN seen >= 1 and every one it saw would have been elided (write set and cap hold)",
                 sh.ds.de_an_seen >= 1u && sh.ds.de_an_would == sh.ds.de_an_seen && sh.ds.de_an_ref_ws == 0u && sh.ds.de_an_ref_cap == 0u);
          expect("AN552 ON: BOTH AN draws (1020, 1060) are NOPed before their table step - row AN, no surface named, no refusal left there",
                 on.ds.draw_elided == 2u && on.ds.de_at[0] == 1020u && on.ds.de_at[1] == 1060u && on.ds.de_row[0] == XLAT12_DE_ROW_AN &&
                 on.ds.de_row[1] == XLAT12_DE_ROW_AN && on.ds.de_va8[0] == 0u && on.ds.de_va8[1] == 0u && on.ds.de_an_seen == 2u &&
                 on.ds.de_an_would == 0u && on.at != 1020u && on.at != 1060u);
          { // in place: the unit's first constituent alone (a single segment, [0, 1114)) - both AN draws are in it
            const std::vector<uint32_t> seg(in.begin(), in.begin() + 1060u);   // up to the second AN draw: the first alone
            const ERes sOff = de_run(fAn, seg, M7, 0, prov), sOn = de_run(fAn, seg, M7 | AN, 0, prov);
            bool pre = sOn.out.size() == seg.size() && sOff.out.size() == seg.size();
            for (uint32_t k = 0; pre && k < 1020u; k++) pre = sOn.out[k] == sOff.out[k];
            snprintf(line, sizeof line, "AN552 ON in place [0, 1060): TRANSLATES; dwords [0, 1020) == 110 OFF's (nothing placed before the "
                     "draw), then a NOP of the draw's own length at 1020, row AN, exactly draws - 1 draw packets left (st %u/%u el %u op %#x at %u)",
                     sOff.st, sOn.st, sOn.ds.draw_elided, sOn.op, sOn.at);
            const uint32_t dl = ((seg[1020] >> 16) & 0x3FFFu) + 2u;   // the draw's own length
            bool nop = sOn.st == 0u && sOn.ds.draw_elided == 1u && sOn.ds.de_row[0] == XLAT12_DE_ROW_AN && sOn.ds.de_at[0] == 1020u &&
                       (seg[1020] >> 30) == 3u && sOn.out[1020] == (0xC0001000u | ((dl - 2u) << 16)) &&
                       xlat12_ib_count_draws(sOn.out.data(), (uint32_t)sOn.out.size()) == sOn.ds.draws - 1u;
            for (uint32_t k = 1; nop && k < dl; k++) nop = sOn.out[1020u + k] == 0u;
            expect(line, pre && nop); }
          expect("AN552 ON: the translation then stops where 66 M 3 stopped with AO elided (PAIR at 1159: the next program is not in the capture)",
                 on.st == XLAT12_IB_ERR_PAIR && on.at == 1159u);
          expect("AN552 ON with the AN bit alone: the same two elisions and the same stop",
                 onA.ds.draw_elided == 2u && onA.st == on.st && onA.at == on.at && onA.out == on.out);
          expect("AN552 66 OFF (no flag): no AN counter moves", none.ds.de_an_seen == 0u && none.ds.draw_elided == 0u);
          // AI: AN's function compiled a second time, a DIFFERENT identity: never matched, under ON or SHADOW
          de_fx_proofs(fAi); gEMiss = gEPgmMiss = gEPgmBad = 0;
          const ERes ai0 = de_run(fAi, in, M7, 1, prov), aiOn = de_run(fAi, in, M7 | AN, 1, prov), aiSh = de_run(fAi, in, M7 | ANS, 1, prov);
          snprintf(line, sizeof line, "AN552 prov %d: AI {119, 0xae6c50d3} is NOT AN - ON and SHADOW translate exactly as 110 OFF (st %u/%u/%u "
                   "at %u/%u/%u, AN seen %u/%u)", prov, ai0.st, aiOn.st, aiSh.st, ai0.at, aiOn.at, aiSh.at, (uint32_t)aiOn.ds.de_an_seen,
                   (uint32_t)aiSh.ds.de_an_seen);
          expect(line, aiOn.st == ai0.st && aiOn.at == ai0.at && aiOn.out == ai0.out && aiSh.out == ai0.out && aiOn.ds.draw_elided == 0u &&
                       aiOn.ds.de_an_seen == 0u && aiSh.ds.de_an_seen == 0u && aiSh.ds.de_an_would == 0u);
          // the real AO row: ON and SHADOW of 110 change nothing for AO (not AN)
          de_fx_proofs(f0);
          const ERes ao0 = de_run(f0, in, M7, 1, prov), aoOn = de_run(f0, in, M7 | AN, 1, prov);
          expect("AN552 the unrelabelled AO frame: 110 ON leaves 66's AO elisions and the output exactly as 110 OFF",
                 aoOn.st == ao0.st && aoOn.at == ao0.at && aoOn.out == ao0.out && aoOn.ds.draw_elided == ao0.ds.draw_elided &&
                 aoOn.ds.de_an_seen == 0u && (prov || (aoOn.ds.de_row[0] == XLAT12_DE_ROW_AO && aoOn.ds.draw_elided == 2u)));
          snprintf(line, sizeof line, "AN552 prov %d: every client read served by the fixture, no program mismatched (misses %u, bad %u)", prov,
                   gEMiss, gEPgmBad);
          expect(line, gEMiss == 0u && gEPgmBad == 0u);
      }
      // the cap: with the two AN draws the cap (2) is reached - a THIRD elision candidate is refused. Shown on the committed shape: the
      // AN bit with a pre-elided pair is not constructible here, so the cap is driven by d_de_an's own counters: 66's AO rows off, AN
      // ON over a stream whose first two table draws are AN - both elided, de_an_ref_cap 0 (the cap is not exceeded by two).
      { de_fx_proofs(fAn); const ERes c2 = de_run(fAn, in, AN, 1, 1);
        expect("AN552 two AN draws fit the cap exactly (ref-cap 0)", c2.ds.draw_elided == 2u && c2.ds.de_an_ref_cap == 0u); }
      // the kext's latch: 110's bit only beside 66's rows
      expect("AN552 n48_an110_rows: OFF adds nothing; ON / SHADOW add 0x8 / 0x10 only when 66's rows are non-zero; 66 OFF stays 0",
             n48_an110_rows(N48_BB_OFF, 7u) == 7u && n48_an110_rows(N48_BB_ON, 7u) == (7u | AN) && n48_an110_rows(N48_BB_SHADOW, 3u) == (3u | ANS) &&
             n48_an110_rows(N48_BB_ON, 0u) == 0u && n48_an110_rows(N48_BB_SHADOW, 0u) == 0u && n48_an110_rows(N48_BB_OFF, 0u) == 0u); }
    // ---- ARGUMENTS ----
    { const DeFx &f = g; de_fx_proofs(f); gEfx = &f;
      xlat12_draw_extra ex {}; ex.pgm_profile = &de_profile; ex.flags = XLAT12_EXTRA_DRAW_ELIDE; ex.draw_elide_rows = UY;
      static xlat12_draw_stats ds; uint32_t olen = 0; std::vector<uint32_t> o(gin.size());
      expect("ARG: the flag without TABLE_DESC is ERR_ARG",
             xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, gin.data(), (uint32_t)gin.size(), o.data(), &olen, &ds) == XLAT12_ERR_ARG); }
}


// build 0.0.512 (switch 66 M 7 = XLAT12_DE_CLASS_GLASS; ) — THE GLASS ROWS ON CAPTURED b / e UNITS.
// tests/fixture_drawelide512.h: run10t F107 (b, 8736|14544) unit k11, a single segment (e's k9 is the harness's: its BD record is a stand-in):
// BD (ws_BD_glass_background_lph) at 1031 reads the frozen distance surface 0x402380000 (stripped DCC) as its LAST texture; BA
// (ws_BA_TdfgXh_Isrc) at 2411 reads it as texture 0 and the 256x1 colour ramp 0x400436000 (proven: switch 59's stand-in) as
// texture 1, and draws into the clock layer 0x401080000 (CB0 at input dword 1347). Proofs = the harness's (b512/h).
// =======================================================================================================================
static uint32_t de_len_at(const std::vector<uint32_t> &in, uint32_t at) { return ((in[at] >> 16) & 0x3FFFu) + 2u; }
static bool de_is_nop_at(const std::vector<uint32_t> &o, uint32_t at, uint32_t l)
{
    if (o[at] != (0xC0001000u | ((l - 2u) << 16))) return false;
    for (uint32_t k = 1; k < l; k++) if (o[at + k]) return false;
    return true;
}
// a copy of `fx` whose T# records naming surface `from` name `to` instead (both the BD and the BA record, whatever heap VA)
struct DeReloc { DeFx fx; std::vector<uint32_t> memw; uint32_t n; };
static void de_reloc(const DeFx &fx, uint64_t from, uint64_t to, DeReloc &r)
{
    r.fx = fx; r.memw.assign(fx.memw, fx.memw + (fx.mem[fx.nmem - 1].off + fx.mem[fx.nmem - 1].ndw)); r.n = 0;
    for (uint32_t i = 0; i < fx.nmem; i++) if (fx.mem[i].ndw == 8u) {
        uint32_t *w = &r.memw[fx.mem[i].off];
        if ((((uint64_t)(w[1] & 0xFFu) << 40) | ((uint64_t)w[0] << 8)) != from) continue;
        w[0] = (uint32_t)(to >> 8); w[1] = (w[1] & ~0xFFu) | (uint32_t)((to >> 40) & 0xFFu); r.n++;
    }
    r.fx.memw = r.memw.data();
}
static bool de_dl_has(const n48_dl &dl, uint64_t va) { for (uint32_t q = 0; q < dl.n; q++) if (dl.e[q].va == va) return true; return false; }
static void drawelide512_captured()
{
    char line[400];
    const uint32_t UY = XLAT12_DE_CLASS_UY, AO = XLAT12_DE_CLASS_AO, GL = XLAT12_DE_CLASS_GLASS, M7 = UY | AO | GL;
    const uint64_t SDF = 0x402380000ull, RAMP = 0x400436000ull, CLOCK = 0x401080000ull;
    const DeFx *fxs[1] = { &kDeFxT107K11 };
    for (const DeFx *pf : fxs) {
        const DeFx &fx = *pf; const std::vector<uint32_t> in(fx.in, fx.in + fx.n);
        de_fx_proofs(fx); gEMiss = gEPgmMiss = gEPgmBad = 0;
        const ERes off = de_run(fx, in, 0u, 0, 0);
        snprintf(line, sizeof line, "G512 %s, 66 OFF: refused 0xf7 at 1031 (BD) naming the distance surface %#llx (st %u op %#x at %u va %#llx)",
                 fx.tag, (unsigned long long)SDF, off.st, off.op, off.at, (unsigned long long)off.prov);
        expect(line, off.st == XLAT12_IB_ERR_DESC && off.op == XLAT12_TDESC_PROVENANCE && off.at == 1031u && off.prov == SDF &&
                     off.ds.de_seen == 0u && off.ds.de_nl == 0u);
        const ERes m3 = de_run(fx, in, UY | AO, 0, 0);
        snprintf(line, sizeof line, "G512 %s, 66 M 3 (834): BD is no row there - the SAME refusal and the SAME output bytes as OFF "
                 "(not-row %u census %u/%#x nl %u)", fx.tag, m3.ds.de_not_row, m3.ds.de_nr_ndw, m3.ds.de_nr_fnv, (uint32_t)m3.ds.de_nl);
        expect(line, m3.st == off.st && m3.op == off.op && m3.at == off.at && m3.out == off.out && m3.ds.de_not_row == 1u &&
                     m3.ds.de_nr_ndw == 1271u && m3.ds.de_nr_fnv == 0x3858ea3au && m3.ds.draw_elided == 0u && m3.ds.de_nl == 0u);
        const ERes on = de_run(fx, in, M7, 0, 0);
        const uint32_t lbd = de_len_at(in, 1031u), lba = de_len_at(in, 2411u);
        snprintf(line, sizeof line, "G512 %s, 66 M 7: TRANSLATES with BD (1031) and BA (2411) elided, both naming %#llx mode 3 (st %u op %#x "
                 "elided %u at %u/%u rows %u/%u va %#llx/%#llx nl %u)", fx.tag, (unsigned long long)SDF, on.st, on.op, on.ds.draw_elided,
                 on.ds.de_at[0], on.ds.de_at[1], (uint32_t)on.ds.de_row[0], (uint32_t)on.ds.de_row[1], (unsigned long long)on.ds.de_va8[0] << 8,
                 (unsigned long long)on.ds.de_va8[1] << 8, (uint32_t)on.ds.de_nl);
        expect(line, on.st == 0u && on.ds.draw_elided == 2u && on.ds.de_seen == 2u && on.ds.de_at[0] == 1031u && on.ds.de_at[1] == 2411u &&
                     on.ds.de_row[0] == XLAT12_DE_ROW_BD && on.ds.de_row[1] == XLAT12_DE_ROW_BA && on.ds.de_va8[0] * 256ull == SDF &&
                     on.ds.de_va8[1] * 256ull == SDF && on.ds.de_mode[0] == 3u && on.ds.de_mode[1] == 3u && on.ds.de_not_last == 0u &&
                     on.ds.de_nl == 0u && on.ds.in_n == 0u);
        expect("G512 ... both draws are same-length NOPs where they stood, and the output carries exactly draws - 2 draw packets",
               on.st == 0u && de_is_nop_at(on.out, 1031u, lbd) && de_is_nop_at(on.out, 2411u, lba) &&
               xlat12_ib_count_draws(on.out.data(), (uint32_t)on.out.size()) == on.ds.draws - 2u);
        const ERes gl = de_run(fx, in, GL, 0, 0);
        expect("G512 ... the GLASS class alone: the same two elisions, the same bytes", gl.st == 0u && gl.out == on.out && gl.ds.draw_elided == 2u);
        // A3: an elided draw feeds NO ledger / frame-local entry - BA's target (the clock layer) is recorded only when BA ran
        const ERes pv = de_run(fx, in, 0u, 0, 1);
        n48_dl dl; std::memset(&dl, 0, sizeof dl);
        (void)n48_dl_from_output(&dl, 5u, on.out.data(), (uint32_t)on.out.size(), nullptr);
        const bool onClock = de_dl_has(dl, CLOCK);
        std::memset(&dl, 0, sizeof dl);
        (void)n48_dl_from_output(&dl, 5u, pv.out.data(), (uint32_t)pv.out.size(), nullptr);
        const bool pvClock = de_dl_has(dl, CLOCK);
        snprintf(line, sizeof line, "G512 %s LEDGER: n48_dl_from_output over the elided output records NO entry for the clock layer %#llx "
                 "(%u); over the every-ask-proven translation (BA kept, st %u) it does (%u) - the positive control",
                 fx.tag, (unsigned long long)CLOCK, onClock, pv.st, pvClock);
        expect(line, !onClock && pv.st == 0u && pvClock && xlat12_ib_count_draws(pv.out.data(), (uint32_t)pv.out.size()) == pv.ds.draws);
        snprintf(line, sizeof line, "G512 %s: every client read and program lookup served by the fixture (misses %u, pgm %u, bad %u)",
                 fx.tag, gEMiss, gEPgmMiss, gEPgmBad);
        expect(line, gEMiss == 0u && gEPgmMiss == 0u && gEPgmBad == 0u);
    }
    const DeFx &b = kDeFxT107K11; const std::vector<uint32_t> bin(b.in, b.in + b.n);
    // ---- NOT-LAST, NARROW: BA's second input (the ramp) unproven - BA must refuse (a second unproven input), BD still elided ----
    { de_fx_proofs(b);
      std::vector<uint64_t> t; for (uint64_t a : gETiled) if (a != RAMP) t.push_back(a); gETiled = t;
      const ERes a = de_run(b, bin, M7, 0, 0);
      snprintf(line, sizeof line, "G512 NOT-LAST b k11, the ramp %#llx unproven too: BD elided, BA refused 0xf7 at 2411 naming %#llx, not-last 1 "
               "(st %u op %#x at %u va %#llx el %u nl %u nlast %u)", (unsigned long long)RAMP, (unsigned long long)SDF, a.st, a.op, a.at,
               (unsigned long long)a.prov, a.ds.draw_elided, (uint32_t)a.ds.de_nl, a.ds.de_not_last);
      expect(line, a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 2411u && a.prov == SDF && a.ds.draw_elided == 1u &&
                   a.ds.de_row[0] == XLAT12_DE_ROW_BD && a.ds.de_not_last == 1u && a.ds.de_nl == 1u); }
    // ---- NOT-LAST: the ramp's record unreadable - the probe fails closed, BA refused ----
    { de_fx_proofs(b); gEDropRead = de_rec_of(b, RAMP);
      const ERes a = de_run(b, bin, M7, 0, 0);
      snprintf(line, sizeof line, "G512 NOT-LAST b k11, the ramp's T# unreadable: BA refused at 2411 on %#llx, not-last 1 (st %u op %#x el %u)",
               (unsigned long long)SDF, a.st, a.op, a.ds.draw_elided);
      expect(line, gEDropRead != 0ull && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 2411u && a.prov == SDF &&
                   a.ds.draw_elided == 1u && a.ds.de_not_last == 1u);
      gEDropRead = 0ull; }
    // ---- BA's LAST texture: the distance surface proven, the ramp not - BA refuses on texture 1; BA's row admits texture 0 ONLY ----
    { de_fx_proofs(b); gEDcc.push_back(SDF);
      std::vector<uint64_t> t; for (uint64_t a : gETiled) if (a != RAMP) t.push_back(a); gETiled = t;
      const ERes a = de_run(b, bin, M7, 0, 0);
      snprintf(line, sizeof line, "G512 LAST-ONLY b k11, SDF proven, ramp not: BD translates, BA refused 0xf7 at 2411 naming the ramp %#llx, "
               "not-last 1, nothing elided (st %u op %#x at %u va %#llx el %u)", (unsigned long long)RAMP, a.st, a.op, a.at,
               (unsigned long long)a.prov, a.ds.draw_elided);
      expect(line, a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 2411u && a.prov == RAMP &&
                   a.ds.draw_elided == 0u && a.ds.de_not_last == 1u); }
    // ---- BY ROLE, NEVER BY VA: the distance surface moved to another VA (run10s had it at 0x4015e0000) - the same two elisions ----
    { static DeReloc r; de_reloc(b, SDF, 0x4015e0000ull, r);
      de_fx_proofs(b);
      const ERes a = de_run(r.fx, bin, M7, 0, 0);
      snprintf(line, sizeof line, "G512 ROLE b k11 with the distance surface at 0x4015e0000 (%u records moved): BD and BA elided naming it "
               "(st %u el %u va %#llx/%#llx)", r.n, a.st, a.ds.draw_elided, (unsigned long long)a.ds.de_va8[0] << 8,
               (unsigned long long)a.ds.de_va8[1] << 8);
      expect(line, r.n >= 1u && a.st == 0u && a.ds.draw_elided == 2u && a.ds.de_va8[0] * 256ull == 0x4015e0000ull &&
                   a.ds.de_va8[1] * 256ull == 0x4015e0000ull && a.ds.de_row[1] == XLAT12_DE_ROW_BA); }
    // ---- REQUIRE DCC: the distance surface without DCC metadata (a plain tiled record) - BD refused no-dcc ----
    { de_fx_proofs(b); gEPatchVa = de_rec_of(b, SDF);
      gEPatchClr[6] = (0xFFu << 24) | (1u << 19) | (1u << 22) | (3u << 15); gEPatchClr[7] = 0xFFFFFFFFu;
      const ERes a = de_run(b, bin, M7, 0, 0);
      snprintf(line, sizeof line, "G512 NO-DCC b k11, BD's distance surface without DCC metadata: refused 0xf7 at 1031, no-dcc 1 (st %u op %#x nd %u el %u)",
               a.st, a.op, a.ds.de_no_dcc, a.ds.draw_elided);
      expect(line, gEPatchVa != 0ull && a.st == XLAT12_IB_ERR_DESC && a.at == 1031u && a.ds.de_no_dcc == 1u && a.ds.draw_elided == 0u);
      gEPatchVa = 0ull; }
    // ---- THE WRITE SET: the zero-count LOAD_CONTEXT_REG before BA (input 1093) turned into a register WRITE_DATA - BA refused ----
    { std::vector<uint32_t> in = bin;
      const bool lcr = in[1093] == 0xC0036100u && in[1096] == 0u && in[1097] == 0u;
      in[1093] = 0xC0033700u; in[1094] = 0u; in[1095] = 0xa105u; in[1096] = 0u; in[1097] = 0u;   /* WRITE_DATA DST_SEL 0: CB_BLEND_RED */
      de_fx_proofs(b);
      const ERes a = de_run(b, in, M7, 0, 0);
      snprintf(line, sizeof line, "G512 WRITESET b k11, the empty LOAD_CONTEXT_REG at 1093 (found %u) made a register WRITE_DATA: BD elided, BA "
               "refused, write-set 1 (st %u op %#x at %u ws %u el %u)", lcr, a.st, a.op, a.at, a.ds.de_writeset, a.ds.draw_elided);
      expect(line, lcr && a.st == XLAT12_IB_ERR_DESC && a.at == 2411u && a.ds.de_writeset == 1u && a.ds.draw_elided == 1u); }
    // ---- the empty-LOAD_CONTEXT_REG admission is the GLASS rows' only: an empty LOAD_CONTEXT_REG before U (frame a k0) still refuses ----
    { const DeFx &g = kDeFxG62K0; std::vector<uint32_t> in(g.in, g.in + g.n);
      uint32_t nopAt = 0xFFFFFFFFu;
      for (uint32_t i = 0; i + 8u < 1043u; i++) if (in[i] == 0xC0061000u) { nopAt = i; break; }
      if (nopAt != 0xFFFFFFFFu) { in[nopAt] = 0xC0036100u; in[nopAt + 1] = 0x00437478u; in[nopAt + 2] = 4u; in[nopAt + 3] = 0u; in[nopAt + 4] = 0u;
                                  in[nopAt + 5] = 0xC0011000u; in[nopAt + 6] = 0u; in[nopAt + 7] = 0u; }
      de_fx_proofs(g);
      const ERes a = de_run(g, in, M7, 1, 0);
      snprintf(line, sizeof line, "G512 WRITESET G62 k0 with an EMPTY LOAD_CONTEXT_REG before U: U is no glass row - refused, write-set 1, "
               "nothing elided (found %u st %u op %#x at %u ws %u el %u)", nopAt != 0xFFFFFFFFu, a.st, a.op, a.at, a.ds.de_writeset, a.ds.draw_elided);
      expect(line, nopAt != 0xFFFFFFFFu && a.st == XLAT12_IB_ERR_DESC && a.op == XLAT12_TDESC_PROVENANCE && a.at == 1043u &&
                   a.ds.de_writeset == 1u && a.ds.draw_elided == 0u); }
    // ---- the T# observer (Part B's read path): non-NULL translates byte-identically and sees BA's two records ----
    { de_fx_proofs(b);
      const ERes a = de_run(b, bin, M7, 0, 0);
      gTexNoteN = 0; gTexNoteBa = 0; gTexNoteOn = 1;
      const ERes c = de_run(b, bin, M7, 0, 0);
      gTexNoteOn = 0;
      const uint32_t seenN = gTexNoteN, seenBa = gTexNoteBa;
      /* every ask proven, 66 OFF: BD and BA translate and PLACE their T# records - the observer must not change one byte */
      const ERes pa = de_run(b, bin, 0u, 0, 1);
      gTexNoteOn = 1;
      const ERes pc = de_run(b, bin, 0u, 0, 1);
      gTexNoteOn = 0;
      expect("G512 OBSERVER b k11, every ask proven (the records PLACED): tex_note set - the same status and bytes",
             pa.st == 0u && pc.st == pa.st && pc.out == pa.out);
      snprintf(line, sizeof line, "G512 OBSERVER b k11: tex_note set - the same status and bytes; it saw %u record(s), BA's texture 0 at 2411 "
               "naming %#llx (%u)", seenN, (unsigned long long)SDF, seenBa);
      expect(line, c.st == a.st && c.out == a.out && seenN >= 3u && seenBa == 1u); }
}

int main(int argc, char **argv)
{
    gPrint = argc > 1 && !strcmp(argv[1], "--print");
    printf("gfx_f84 (0.0.454 item 4): decide44's F84 through the real translator, decide44's flags, switches 48/49/50\n");
    gCat.assign(kF84Ib0, kF84Ib0 + sizeof kF84Ib0 / 4u);
    gCat.insert(gCat.end(), kF84Ib1, kF84Ib1 + sizeof kF84Ib1 / 4u);
    gOff[0] = 0u; gOff[1] = (uint32_t)(sizeof kF84Ib0 / 4u); gNn[0] = gOff[1]; gNn[1] = (uint32_t)(sizeof kF84Ib1 / 4u);
    struct Cfg { const char *name; uint32_t add; int no44; const char *want; };
    static const Cfg kCfg[] = {
        { "48/49/50 OFF (decide44's switches; fix E ungated since 0.0.453)", 0u, 0, "XPPXPPP|PXPPP" },
        { "49 + 50 (0.0.453)", XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW, 0, ".M.....|MX..." },
        { "44-50 ON (48 + 49 + 50)", XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW, 0, ".......|....." },
        { "48 + 50, no 49", XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_RASTER_PER_DRAW, 0, ".M.....|....." },
        { "48 alone", XLAT12_EXTRA_TABLE_REUSE, 0, "XPPXPPP|PXPPP" },
        { "44 OFF, 49 + 50", XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW, 1, ".MS.SSS|PESSS" },
        { "44 OFF, 48 + 49 + 50", XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW, 1, "..S.SSS|P.SSS" },
        /* build 0.0.455 item 1 (switch 52, 's known-slot rule): config 2 (44-50 ON) plus 52. The
         * per-segment translate/refuse pattern is UNCHANGED from config 2 - switch 52 only reclassifies a vertex
         * pointer slot as known-by-carry inside an ALREADY-translated segment's read-set/export bookkeeping; it
         * never changes a segment's own st/op. */
        { "44-50 + 52 ON", XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_RASTER_PER_DRAW | XLAT12_EXTRA_VS_KNOWN,
          0, ".......|....." },
    };
    // 0.0.453's OWN output for "49 + 50", per segment (FNV-1a over the translated dwords; 0 = refused), computed by this
    // suite built against 0.0.453's xlat12_ib.c (f88c8c3) - the byte-identity pin for switch 48 OFF.
    static const uint32_t kHash453[12] = { 0xfd6cf5d7u, 0u, 0xc11dbddau, 0x0ee0bb51u, 0x8f93aa9cu, 0xd0704a12u, 0x51e1b3a2u,
                                           0u, 0u, 0xa7d9736fu, 0xc3361d6cu, 0xcec5d27eu };
    Frame fr[sizeof kCfg / sizeof kCfg[0]];
    for (uint32_t c = 0; c < sizeof kCfg / sizeof kCfg[0]; c++) {
        gMiss = 0; gPgmBad = 0;
        fr[c] = run(kCfg[c].add, kCfg[c].no44);
        if (gPrint) {
            printf("config %u %-40s %s\n", c, kCfg[c].name, fr[c].s.c_str());
            for (uint32_t k = 0; k < fr[c].seg.size(); k++)
                printf("  seg %2u st %2u op %#x at %u tables %u reused %u appended %u hash %#010x strip %#010x vrs %u/%u\n", k, fr[c].seg[k].st, fr[c].seg[k].op,
                       fr[c].seg[k].in_dword, fr[c].seg[k].tables, fr[c].seg[k].reused, fr[c].seg[k].appended, fr[c].seg[k].hash,
                       fr[c].seg[k].hashStrip, fr[c].seg[k].vrsUndone, fr[c].seg[k].vrsCost);
            continue;
        }
        char line[256];
        snprintf(line, sizeof line, "%s: %s (want %s)", kCfg[c].name, fr[c].s.c_str(), kCfg[c].want);
        expect(line, fr[c].s == kCfg[c].want);
        snprintf(line, sizeof line, "%s: every client read and program lookup served by the fixture (misses %u, bad %u)",
                 kCfg[c].name, gMiss, gPgmBad);
        expect(line, gMiss == 0u && gPgmBad == 0u);
    }
    if (gPrint) return 0;
    expect("the frame is 12 segments, 7 in IB0 and 5 in IB1", fr[0].seg.size() == 12u);
    if (fr[0].seg.size() != 12u) { printf("gfx_f84: %d run, %d failed\n", gRun, gFail); return 1; }
    // why seg 0 no longer reads decide44's E: the ungated fix E lets it on to the per-translation raster rule
    expect("OFF: seg 0 is DRAW_SHAPE 0xFE (the per-translation raster rule), not decide44's REDIRECTED",
           fr[0].seg[0].st == XLAT12_IB_ERR_DRAW_SHAPE && fr[0].seg[0].op == 0xFEu);
    // item 1's two named segments: seg 7 (IB1 seg 0) NO_ROOM and seg 8 (IB1 seg 1) TOO_LONG with 48 OFF; both translate ON
    expect("48 OFF (0.0.453): seg 7 refuses NO_ROOM", fr[1].seg[7].st == XLAT12_IB_ERR_DESC && fr[1].seg[7].op == XLAT12_TDESC_NO_ROOM);
    expect("48 OFF (0.0.453): seg 8 refuses TOO_LONG / XLAT12_REEMIT_NO_ROOM",
           fr[1].seg[8].st == XLAT12_IB_ERR_TOO_LONG && fr[1].seg[8].op == XLAT12_REEMIT_NO_ROOM);
    expect("48 ON: segs 7 and 8 translate, each by REUSING a shadow (reused > 0)",
           fr[2].seg[7].st == 0u && fr[2].seg[8].st == 0u && fr[2].seg[7].reused > 0u && fr[2].seg[8].reused > 0u);
    // item 2: seg 1 (AI's class-11 placement) fits only with 49's skip AND 48's 16-byte alignment
    expect("seg 1: 49 alone NO_ROOM, 48 alone (no 49) NO_ROOM, 48 + 49 translates",
           fr[1].seg[1].op == XLAT12_TDESC_NO_ROOM && fr[3].seg[1].op == XLAT12_TDESC_NO_ROOM && fr[2].seg[1].st == 0u && fr[2].seg[1].tables == 1u);
    // OFF identity on real data: switch 48 OFF gives 0.0.453's bytes, segment for segment.
    // build 0.0.499: CHANGED PIN. The scissor BRs are now emitted -1 (gfx12 inclusive; mesa quotes at the undo in run()),
    // so the output is pinned to 0.0.499's hashes (kHash499), AND the same output with every scissor BR put back (+1) must
    // still be 0.0.453's byte for byte (kHash453) - the proof that the scissor BRs are the ONLY bytes 0.0.499 moved.
    // build 0.0.502: CHANGED PIN. Every translated segment now also writes gfx12 DB_SPI_VRS_CENTER_LOCATION = 0 once
    // before its first draw (mesa ac_cmdbuf.c:702 `ac_pm4_set_reg(pm4, R_028068_DB_SPI_VRS_CENTER_LOCATION, 0);`) - on F84
    // always MERGED in front of the stream's own DB_SHADER_CONTROL (1 dword). So: (1) the raw output is pinned to 0.0.502's
    // hashes (kHash502); (2) with that ONE write taken back out (f84_vrs_restore) segs 2,3,4,5,9,10 are 0.0.499's
    // (= 0.0.501's) byte for byte (kHash499) and, with their scissor BRs put back, 0.0.453's (kHash453); (3) segs 0, 6 and 11
    // hold a 16-byte-aligned deferred-record NOP inside the shifted range, which re-pads by the one dword (seg 0 @967: NOP
    // count 0x26 -> 0x25), so for them - and for every translated segment - the NON-NOP packet stream is 0.0.501's exactly
    // (kStrip501: this suite's hashStrip computed against c7d8ba3's xlat12_ib.c, i.e. 0.0.501 - the VRS patch's parent).
    static const uint32_t kHash499[12] = { 0x8ec0963cu, 0u, 0xcaa10004u, 0x6c47a667u, 0xefbb8ca0u, 0x435b4dc6u, 0xa6437b16u,
                                           0u, 0u, 0x06fa5ee9u, 0x7a895415u, 0x5d8fbd5fu };
    static const uint32_t kHash502[12] = { 0x6f39c507u, 0u, 0xa0925531u, 0xa89b2ca6u, 0x8b20c74du, 0x163d986fu, 0x33e52c4bu,
                                           0u, 0u, 0x5206a06cu, 0xe5c92f18u, 0x2b4e5950u };
    static const uint32_t kStrip501[12] = { 0x01b8b1aeu, 0u, 0x2630fb77u, 0x3ee5e567u, 0xa828ebc7u, 0x8801db49u, 0xb5781341u,
                                            0u, 0u, 0x3e899c92u, 0x90708d6fu, 0x23f86ae5u };
    static const uint32_t kExact = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 9) | (1u << 10);   // exact restores
    { bool same = true, strip = true, undo = true; uint32_t touched = 0, nv = 0, ntr = 0, exact = 0; char line[200];
      for (uint32_t k = 0; k < 12u; k++) {
          const SegOut &sg = fr[1].seg[k];
          if (sg.hash != kHash502[k]) {
              same = false; snprintf(line, sizeof line, "  seg %u hash %#010x, 0.0.502 %#010x", k, sg.hash, kHash502[k]); printf("%s\n", line); }
          if (sg.hashStrip != kStrip501[k] || (sg.st == 0u && (sg.vrsUndone != 1u || sg.vrsCost != 1u))) {
              strip = false; snprintf(line, sizeof line, "  seg %u VRS-restored non-NOP hash %#010x, 0.0.501 %#010x (undone %u cost %u)", k,
                                      sg.hashStrip, kStrip501[k], sg.vrsUndone, sg.vrsCost); printf("%s\n", line); }
          if (sg.st == 0u && sg.hashNoVrs == kHash499[k]) exact |= 1u << k;
          if ((kExact >> k) & 1u) {
              if (sg.hash498 != kHash453[k] || sg.undone != sg.scAdj) {
                  undo = false; snprintf(line, sizeof line, "  seg %u VRS-restored scissor-undone hash %#010x, 0.0.453 %#010x (undone %u, translator adjusted %u)",
                                         k, sg.hash498, kHash453[k], sg.undone, sg.scAdj); printf("%s\n", line); }
              touched += sg.undone;
          }
          nv += sg.vrsUndone; ntr += sg.st == 0u ? 1u : 0u;
      }
      expect("48 OFF: every segment's output is 0.0.502's, byte for byte (FNV over the translated dwords)", same);
      expect("48 OFF (0.0.502): every translated segment writes 0x28068 = 0 exactly once before its first draw, merged (1 dword), and "
             "with it taken back out its non-NOP packets are 0.0.501's exactly", strip && nv == ntr && ntr == 9u);
      snprintf(line, sizeof line, "48 OFF (0.0.502): VRS-restored output is 0.0.499's byte for byte on exactly segs 2,3,4,5,9,10 (mask %#x)", exact);
      expect(line, exact == kExact);
      expect("48 OFF: on those segments, with the VRS write out and the scissor BRs put back (+1), the output is 0.0.453's, byte for byte",
             undo && touched > 0u); }
    // 48 ON changes only what it must: the segments 0.0.453 already translated keep the SAME status
    { bool ok = true; for (uint32_t k = 0; k < 12u; k++) if (fr[1].seg[k].st == 0u && fr[2].seg[k].st != 0u) ok = false;
      expect("48 ON: no segment 0.0.453 translated is refused", ok); }
    // THE EMULATED SHADER: every table draw of every translated segment, with 48 ON and OFF
    for (uint32_t c : { 1u, 2u, 6u }) {
        uint32_t checked = 0, bad = 0, segs = 0;
        for (uint32_t k = 0; k < 12u; k++) if (fr[c].seg[k].st == 0u) { segs++; checked += emulate(fr[c].seg[k], k, &bad); }
        char line[200];
        snprintf(line, sizeof line, "%s: the emulated shader reads Apple's records, translated, at all %u table draws of %u translated segments (%u bad)",
                 kCfg[c].name, checked, segs, bad);
        expect(line, bad == 0u && checked > 0u);
    }
    { uint32_t checked = 0, bad = 0;
      checked += emulate(fr[2].seg[7], 7u, &bad); checked += emulate(fr[2].seg[8], 8u, &bad);
      char line[160]; snprintf(line, sizeof line, "44-50 ON: segs 7 and 8's %u table draws (the reused shadows) read the right records", checked);
      expect(line, bad == 0u && checked >= 2u); }
    // THE EXPORTS ON REAL DATA (item 1's "fill the exports exactly as a fresh placement would"). (i) Each translated
    // segment's LAST table draw - in seg 8 a draw that REUSES the shadow - exports APPLE'S table, image heap, S# and
    // surface, exactly the values the emulated shader resolved from Apple's own user data. (ii) Every surface and
    // Apple table the table draws read is in the segment's accumulated READSET union (built from each draw's in_*),
    // unless the segment declined its read-set for another reason (an INHERITED vertex pointer: segs 2, 4-6, 9-11
    // decline exactly as they do with 48 OFF, and seg 8's last draw inherits two - a cross-segment fact, not this build's).
    { uint32_t lastOk = 0, lastBad = 0, reusedLast = 0;
      for (uint32_t k = 0; k < 12u; k++) {
          const SegOut &s = fr[2].seg[k];
          if (s.st != 0u || !s.tables) continue;
          gSurf.clear(); gTbl.clear(); gLast = {}; uint32_t b = 0; emulate(s, k, &b);
          // the export is the segment's LAST DRAW's (every draw clears it first): a table draw's is Apple's values, any
          // other draw's is empty
          const bool same = gLast.lastDrawIsTable
                            ? (gLast.ok && s.in_tbl == gLast.tbl && s.in_img == gLast.img && s.in_samp == gLast.samp &&
                               s.in_n == gLast.ntex && s.in_va0 == gLast.va0 && s.in_abi != 0u)
                            : (s.in_abi == 0u && s.in_n == 0u && s.in_tbl == 0u);
          if (same) lastOk++; else { lastBad++; printf("  seg %u last table draw export: tbl %#llx img %#llx samp %#llx va0 %#llx n %u; Apple %#llx %#llx %#llx %#llx %u\n",
              k, (unsigned long long)s.in_tbl, (unsigned long long)s.in_img, (unsigned long long)s.in_samp, (unsigned long long)s.in_va0, s.in_n,
              (unsigned long long)gLast.tbl, (unsigned long long)gLast.img, (unsigned long long)gLast.samp, (unsigned long long)gLast.va0, gLast.ntex); }
          if (k == 8u && s.reused && gLast.lastDrawIsTable) reusedLast = 1u;
      }
      char line[200]; snprintf(line, sizeof line, "44-50 ON: every translated segment's last table draw exports APPLE'S table/heap/S#/surface (%u right, %u wrong)", lastOk, lastBad);
      expect(line, lastBad == 0u && lastOk >= 10u);
      expect("44-50 ON: seg 8 reuses a shadow and ends on a table draw (so the check above covers a reusing draw's export)", reusedLast == 1u); }
    { uint32_t named = 0, missing = 0, declined = 0;
      for (uint32_t k = 0; k < 12u; k++) {
          const SegOut &s = fr[2].seg[k];
          if (s.st != 0u) continue;
          gSurf.clear(); gTbl.clear(); uint32_t b = 0; emulate(s, k, &b);
          if (s.rs_declined) { declined++; continue; }
          for (uint64_t v : gSurf) { bool f = false; for (uint64_t r : s.rs_va) if (r == v) f = true; if (f) named++; else { missing++; printf("  seg %u: surface %#llx not in the read-set\n", k, (unsigned long long)v); } }
          for (uint64_t v : gTbl) { bool f = false; for (uint64_t r : s.rs_ptr) if ((r & ~0xFFFull) == (v & ~0xFFFull)) f = true; if (f) named++; else { missing++; printf("  seg %u: table %#llx not in the read-set\n", k, (unsigned long long)v); } }
      }
      char line[200]; snprintf(line, sizeof line, "44-50 ON: every surface and table the table draws read is in its segment's read-set (%u named, %u missing, %u segments declined)",
                               named, missing, declined);
      expect(line, missing == 0u && named > 0u);
      /* build 0.0.455 item 1 (switch 52): the SAME 10-of-12 decline WITH THE NEW SWITCH OFF - the
       * known-slot rule must change NOTHING here ('s evidence: 12 declining draws, 10 of 12 segments). */
      expect("44-50 ON, 52 OFF: exactly 10 of 12 segments decline their read-set (unchanged from's evidence)",
             declined == 10u); }
    /* build 0.0.455 item 1: the SAME check, WITH SWITCH 52 ON (config 7, 44-50 + 52) - every vertex pointer
     * slot named (an earlier-region write the carry still holds) is now admitted, so NO segment declines its
     * read-set purely for that reason. F84 has no OTHER decline cause left once 44-50 are ON (the same 12
     * segments this whole file otherwise proves translate and export correctly), so the count must reach 0. */
    { uint32_t named = 0, missing = 0, declined = 0;
      for (uint32_t k = 0; k < 12u; k++) {
          const SegOut &s = fr[7].seg[k];
          if (s.st != 0u) continue;
          gSurf.clear(); gTbl.clear(); uint32_t b = 0; emulate(s, k, &b);
          if (s.rs_declined) { declined++; continue; }
          for (uint64_t v : gSurf) { bool f = false; for (uint64_t r : s.rs_va) if (r == v) f = true; if (f) named++; else { missing++; printf("  seg %u: surface %#llx not in the read-set\n", k, (unsigned long long)v); } }
          for (uint64_t v : gTbl) { bool f = false; for (uint64_t r : s.rs_ptr) if ((r & ~0xFFFull) == (v & ~0xFFFull)) f = true; if (f) named++; else { missing++; printf("  seg %u: table %#llx not in the read-set\n", k, (unsigned long long)v); } }
      }
      char line[200]; snprintf(line, sizeof line, "44-50 + 52 ON: every surface and table the table draws read is in its segment's read-set (%u named, %u missing, %u segments declined)",
                               named, missing, declined);
      expect(line, missing == 0u && named > 0u);
      expect("44-50 + 52 ON: 0 segments decline their read-set (the known-slot rule closes every decline)",
             declined == 0u); }
    // non-vacuity of the emulated shader: corrupt ONE dword of a placed T# in seg 8's output and it must notice
    { SegOut s8 = fr[2].seg[8]; uint32_t bad = 0, tbl = 0;
      // find the first shadow table: the first NOP body the table step placed (a NOP header followed by a table whose
      // +0x00 pointer points into this segment) - simplest: flip every dword of every NOP body that emulate() reads
      for (uint32_t i = 0; i + 1u < s8.n; i++) if (s8.out[i] != XLAT12_IB_NOP && (s8.out[i] & 0xC000FF00u) == 0xC0001000u) { tbl++; for (uint32_t k = i + 1u; k < i + 2u + ((s8.out[i] >> 16) & 0x3FFFu) && k < s8.n; k++) s8.out[k] ^= 0x00000100u; }
      emulate(s8, 8u, &bad);
      char line[160]; snprintf(line, sizeof line, "BREAK-check: seg 8 with its %u NOP bodies' dwords flipped - the emulated shader reports %u bad draws", tbl, bad);
      expect(line, tbl > 0u && bad > 0u); }
    dcc_strip_captured();   // build 0.0.488
    drawelide_captured();   // build 0.0.500
    drawelide512_captured();   // build 0.0.512
    printf("gfx_f84: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
