// gfx_capture_scan.h — what a GFX IB points at, for the source capture (0.0.282). Pure C, host-tested by
// tests/gfx_capture_scan_test.cpp; the kext compiles the SAME header.
//
// n48_gcap_scan walks one gfx10.3 IB (type-3 packets, type-2 fillers, stops at anything else or a packet running past n) and
// lists the client memory it names, in stream order:
//   - PROGRAMS: SET_SH_REG (0x76) / SET_SH_REG_INDEX (0x9b) writes to SPI_SHADER_PGM_LO_{PS,VS,GS,ES,HS,LS} or COMPUTE_PGM_LO (absolute
//     dwords 0x2c08/0x2c48/0x2c88/0x2cc8/0x2d08/0x2d48/0x2e0c, gc_10_3_0_offset.h + GC base 0x1260) followed by PGM_HI in the same
//     packet: VA = (HI & 0xff) << 40 | LO << 8 (: LO holds VA >> 8). One item per distinct VA per IB.
//   - USER DATA POINTERS: every adjacent (lo, hi) pair in a SET_SH_REG / SET_SH_REG_INDEX body with 1 <= hi <= 0xff, lo 4-aligned and
//     (hi << 32 | lo) >= startVa (G2, MILESTONE2-DESIGN: buffers and the class-19 table are 64-bit SGPR pairs). A pair is consumed
//     when it matches, so lo of one pair is never the hi of the next.
//   - LOAD_CONFIG/CONTEXT/SH_REG and their _INDEX forms (0x60, 0x61, 0x5f, 0x63, 0x9f): address = hi << 32 | lo & ~3, dwords = the
//     largest (offset + count) of its (offset, count) pairs (offset low 16 bits), 256 when that is 0 or over 1024.
//   - DRAW_INDEX_2 (0x27: size, lo, hi, count; Mesa sid.h:63) and INDEX_BASE (0x26: lo, hi; sid.h:62): the index buffer, 64 dwords.
//   - DMA_DATA (0x50: control, src lo, src hi, ...): its source, 64 dwords.
//   - nested INDIRECT_BUFFER (0x3f: lo, hi, control): its dwords (control & 0xfffff).
//   - RENDER AND DEPTH TARGETS at the end of the walk: the last CB_COLORn_BASE (context dword 0xa318 + 15n, n < 8) with its
//     CB_COLORn_BASE_EXT (0xa390 + n, low 8 bits) and the last DB_{Z,STENCIL}_{READ,WRITE}_BASE (0xa012-0xa015) with _HI
//     (0xa01a-0xa01d), 16 dwords each; `index` carries n. 0.0.283: the base holds VA >> 8 and the EXT/HI byte VA >> 40
//     (Mesa ac_descriptors.c:1477 cb_color_base = va >> 8, :1022 db_depth_base = va >> 8, si_state.c:2775/:2888 the >> 32 of those),
//     as PGM_LO does; 0.0.282 read them as full addresses.
// Items beyond `max` are counted in *total but not stored.
#ifndef N48_GFX_CAPTURE_SCAN_H
#define N48_GFX_CAPTURE_SCAN_H

#include <stdint.h>

enum {
    N48_GCAP_PGM = 0x10,      /* + stage: PS 0, VS 1, GS 2, ES 3, HS 4, LS 5, CS 6 */
    N48_GCAP_USER = 0x20, N48_GCAP_USER2 = 0x21, N48_GCAP_LOAD = 0x30, N48_GCAP_INDEX = 0x31, N48_GCAP_DMA = 0x32,
    N48_GCAP_NESTED = 0x33, N48_GCAP_CB = 0x40, N48_GCAP_DB = 0x41,
    /* build 0.0.450 item 4: THE CENSUS. Reuses n48_gcap_region_hdr's existing `extent`/`dword`/`parentVa` fields
     * (no new record layout - see the banner near n48_gcap_census_ib, below) rather than a new struct: extent =
     * sgpr << 16 | idx, dword = the draw's own IB position, parentVa = the table VA (s0:s1) the draw's own SGPRs
     * named. `va`/`want`/`got` are 0 - a census row records a REGISTER FACT (the SGPR held this value at this draw),
     * not a memory read, so it carries no body and no page-type claim. */
    N48_GCAP_CENSUS = 0x50
};

typedef struct { uint32_t kind, index, dword, want; uint64_t va; } n48_gcap_item;

/* ---- The capture ring's record bodies (amd/n48cap.h wraps each in its 24-byte header). tools/m4-xlat/capdecode.py decodes these
 * exact layouts; tests/gfx_capture_scan_test.cpp checks the sizes and offsets the decoder assumes. All little-endian. ---- */
enum { N48_GCAP_REC_ARM = 1, N48_GCAP_REC_FRAME = 2, N48_GCAP_REC_IB = 3, N48_GCAP_REC_REGION = 4 };
typedef struct { uint32_t version, ringBytes, ctxs, pad; } n48_gcap_arm_hdr;                                   /* + ctxs x arm_row */
typedef struct { uint32_t seq; int32_t pid; char name[20]; uint32_t state; uint64_t ctx, task, root; } n48_gcap_arm_row;   /* 56 */
typedef struct {
    uint32_t frame, srcCheck, flags, vmidField, count, stamp;
    uint64_t ctx2Root, startVa, readRoot;
    int32_t  ownerPid; uint32_t ownerSeq; char ownerName[20];
    int32_t  threadPid; char threadName[20];
    uint32_t rptr, wptr, nprobe, infoBytes, chanId, readerWhy;
} n48_gcap_frame_hdr;                                                     /* 128; + nprobe x probe + infoBytes of the submit info */
typedef struct { int32_t pid; uint32_t seq; uint64_t root; uint32_t got, dw0, dw1, pad; } n48_gcap_probe;   /* 32 */
typedef struct { uint32_t frame, index; uint64_t va; uint32_t len, got, sysPages, walkLen, fnv, bodyDwords; uint64_t root; } n48_gcap_ib_hdr;   /* 48 + bodyDwords */
typedef struct {
    uint32_t frame, kind; uint64_t va, parentVa; uint32_t ib, dword, want, got, isSys, ref; uint64_t firstPage;
    uint32_t fnv, extent; uint64_t key;
} n48_gcap_region_hdr;                                                    /* 72 + got dwords unless ref */

static inline int n48_gcap_pgm_stage(uint32_t absReg)
{
    switch (absReg) {
    case 0x2c08u: return 0; case 0x2c48u: return 1; case 0x2c88u: return 2; case 0x2cc8u: return 3;
    case 0x2d08u: return 4; case 0x2d48u: return 5; case 0x2e0cu: return 6; default: return -1;
    }
}

static inline int n48_gcap_is_va(uint64_t startVa, uint32_t lo, uint32_t hi)
{
    if (hi == 0u || hi > 0xffu || (lo & 3u) != 0u) return 0;
    return ((((uint64_t)hi) << 32) | lo) >= startVa;
}

static inline void n48_gcap_push(n48_gcap_item *it, uint32_t max, uint32_t *cnt, uint32_t kind, uint32_t index, uint32_t dword,
                                 uint32_t want, uint64_t va)
{
    if (*cnt < max) { it[*cnt].kind = kind; it[*cnt].index = index; it[*cnt].dword = dword; it[*cnt].want = want; it[*cnt].va = va; }
    (*cnt)++;
}

/* 0.0.395 (notes/design/R5-REDESIGN.md v2 "the one-dword filler, again"): THE SCANNER'S FLAGS.
 *
 * WHY A FLAG AND NOT AN UNCONDITIONAL BRANCH. fixed the one-dword filler (`0xFFFF1000` = PACKET3(NOP, 0x3FFF),
 * one dword, proven on this silicon) in `n48_cp_scan_frame` but NOT here, so `n48_gcap_scan` still sizes it by its count
 * field and claims 16385 dwords: 110 of arm20's 265 captured IB bodies stop early, median coverage 43.2%, stop dword
 * `0xFFFF1000` in 110 of 110. The redesign's "readable" predicate needs the fixed walk. But the 0.0.394 decide path also
 * calls `n48_gcap_scan`, and flipping the branch on for it would change `f.npgm`, `f.target_vram`, `scanOverFrame` and
 * every count derived from them - i.e. the DECISION and the existing report lines. The hard build rule is that with the
 * NEW switch OFF those are byte identical to 0.0.394, so the fix is carried as a FLAG the new path sets and the old
 * callers do not. `n48_gcap_scan` below is exactly 0.0.394's entry point (flags 0) and is untouched. */
#define N48_GCAP_F_FILLER 0x1u

/* Returns the dwords walked (the well-formed prefix). `flags` 0 is 0.0.394's walk, byte for byte. */
static inline uint32_t n48_gcap_scan_ex(const uint32_t *d, uint32_t n, uint64_t startVa, n48_gcap_item *it, uint32_t max,
                                        uint32_t *total, uint32_t flags)
{
    uint32_t cnt = 0, i = 0;
    uint32_t cbBase[8] = { 0 }, cbExt[8] = { 0 }, dbBase[4] = { 0 }, dbHi[4] = { 0 }, cbSeen = 0, dbSeen = 0;
    uint64_t pgm[32]; uint32_t npgm = 0;
    while (i < n) {
        const uint32_t h = d[i];
        const uint32_t type = h >> 30;
        /* THE FIX, and only under the flag: one dword, not the 16385 its count field claims. */
        if (flags & N48_GCAP_F_FILLER) { if (h == 0xFFFF1000u) { i++; continue; } }
        if (type == 2u) { i++; continue; }
        if (type != 3u) break;
        const uint32_t op = (h >> 8) & 0xffu, cnt3 = (h >> 16) & 0x3fffu, plen = cnt3 + 2u;
        if (plen > n - i) break;
        const uint32_t *b = &d[i + 1u];
        const uint32_t nb = cnt3 + 1u;
        if ((op == 0x76u || op == 0x9bu) && nb >= 2u) {
            const uint32_t reg0 = 0x2c00u + (b[0] & 0xffffu);
            for (uint32_t k = 1; k < nb; k++) {
                const int st = n48_gcap_pgm_stage(reg0 + (k - 1u));
                if (st >= 0 && k + 1u < nb) {
                    const uint64_t va = (((uint64_t)(b[k + 1u] & 0xffu)) << 40) | (((uint64_t)b[k]) << 8);
                    uint32_t dup = 0;
                    for (uint32_t q = 0; q < npgm; q++) if (pgm[q] == va) dup = 1;
                    if (!dup && va != 0u && npgm < 32u) {
                        pgm[npgm++] = va;
                        n48_gcap_push(it, max, &cnt, N48_GCAP_PGM + (uint32_t)st, (uint32_t)st, i + 1u + k, 1024u, va);
                    }
                }
            }
            for (uint32_t k = 1; k + 1u < nb; k++) {
                if (!n48_gcap_is_va(startVa, b[k], b[k + 1u])) continue;
                n48_gcap_push(it, max, &cnt, N48_GCAP_USER, reg0 + (k - 1u), i + 1u + k, 256u, (((uint64_t)b[k + 1u]) << 32) | b[k]);
                k++;
            }
        } else if ((op == 0x60u || op == 0x61u || op == 0x5fu || op == 0x63u || op == 0x9fu) && nb >= 2u) {
            uint32_t words = 0;
            for (uint32_t k = 2; k + 1u < nb; k += 2u) {
                const uint32_t e = (b[k] & 0xffffu) + b[k + 1u];
                if (e > words) words = e;
            }
            if (words == 0u || words > 1024u) words = 256u;
            n48_gcap_push(it, max, &cnt, N48_GCAP_LOAD, op, i, words, (((uint64_t)b[1]) << 32) | (b[0] & ~3u));
        } else if (op == 0x27u && nb >= 4u) {
            n48_gcap_push(it, max, &cnt, N48_GCAP_INDEX, op, i, 64u, (((uint64_t)b[2]) << 32) | (b[1] & ~1u));
        } else if (op == 0x26u && nb >= 2u) {
            n48_gcap_push(it, max, &cnt, N48_GCAP_INDEX, op, i, 64u, (((uint64_t)b[1]) << 32) | (b[0] & ~1u));
        } else if (op == 0x50u && nb >= 3u) {
            n48_gcap_push(it, max, &cnt, N48_GCAP_DMA, op, i, 64u, (((uint64_t)b[2]) << 32) | b[1]);
        } else if (op == 0x3fu && nb >= 3u) {
            n48_gcap_push(it, max, &cnt, N48_GCAP_NESTED, op, i, b[2] & 0xfffffu, (((uint64_t)b[1]) << 32) | (b[0] & ~3u));
        } else if (op == 0x69u && nb >= 2u) {
            const uint32_t reg0 = 0xa000u + (b[0] & 0xffffu);
            for (uint32_t k = 1; k < nb; k++) {
                const uint32_t reg = reg0 + (k - 1u), v = b[k];
                if (reg >= 0xa318u && reg < 0xa318u + 15u * 8u && ((reg - 0xa318u) % 15u) == 0u) {
                    cbBase[(reg - 0xa318u) / 15u] = v; cbSeen |= 1u << ((reg - 0xa318u) / 15u);
                } else if (reg >= 0xa390u && reg < 0xa398u) {
                    cbExt[reg - 0xa390u] = v;
                } else if (reg >= 0xa012u && reg <= 0xa015u) {
                    dbBase[reg - 0xa012u] = v; dbSeen |= 1u << (reg - 0xa012u);
                } else if (reg >= 0xa01au && reg <= 0xa01du) {
                    dbHi[reg - 0xa01au] = v;
                }
            }
        }
        i += plen;
    }
    for (uint32_t c = 0; c < 8u; c++)
        if (cbSeen & (1u << c))
            n48_gcap_push(it, max, &cnt, N48_GCAP_CB, c, 0u, 16u, (((uint64_t)(cbExt[c] & 0xffu)) << 40) | (((uint64_t)cbBase[c]) << 8));
    for (uint32_t c = 0; c < 4u; c++)
        if (dbSeen & (1u << c))
            n48_gcap_push(it, max, &cnt, N48_GCAP_DB, c, 0u, 16u, (((uint64_t)(dbHi[c] & 0xffu)) << 40) | (((uint64_t)dbBase[c]) << 8));
    if (total) *total = cnt;
    return i;
}

/* 0.0.394's entry point, byte for byte: the same walk with NO filler branch. Every existing caller uses THIS. */
static inline uint32_t n48_gcap_scan(const uint32_t *d, uint32_t n, uint64_t startVa, n48_gcap_item *it, uint32_t max, uint32_t *total)
{
    return n48_gcap_scan_ex(d, n, startVa, it, max, total, 0u);
}

/* Pointer pairs inside a region already read (one level down): (lo, hi) pairs as n48_gcap_is_va accepts, a matched pair consumed.
 * Returns how many were found; stores up to max VAs and their dword positions. */
static inline uint32_t n48_gcap_pairs(const uint32_t *d, uint32_t n, uint64_t startVa, uint64_t *va, uint32_t *pos, uint32_t max)
{
    uint32_t found = 0;
    for (uint32_t q = 0; q + 1u < n; q++) {
        if (!n48_gcap_is_va(startVa, d[q], d[q + 1u])) continue;
        if (found < max) { va[found] = (((uint64_t)d[q + 1u]) << 32) | d[q]; pos[found] = q; }
        found++;
        q++;
    }
    return found;
}

/* build 0.0.447 (MIB-A1-PATH.md Q3 / ) — THE SLOT CENSUS'S OWN GAP: a table draw's per-texture
 * IMAGE INDEX (e.g. U's second texture, index 46 - byte 1472; Y's, index 21 - byte 672) is a PLAIN SGPR integer
 * written in the SAME SET_SH_REG body as the class-19 table pointer (PS_0:PS_1), not inside the table's OWN
 * dereferenced content - n48_gcap_pairs above finds the two HEAP BASE pointers stored THERE, never a draw's own
 * index. Every kDTableAbi shape shipped so far (xlat12_ib.c) places its tex/samp SGPRs within a few dwords of the
 * table (P/AJ/AK/AL: table+8/+10; U/Y: table+8/+10/+12; AE/AM/AD: table+4/+6), each argument occupying a 64-bit
 * SGPR PAIR from the table's own position onward (G2/MILESTONE2-DESIGN, the SAME convention n48_gcap_scan's own
 * "USER DATA POINTERS" banner cites) - so this walks the ORIGINAL IB body `d[]` in FIXED STRIDES OF 2 starting
 * right after the table's own two words, for `window` dwords (bounded by `n`), never at an odd offset from it:
 * a stray zero word immediately before a small nonzero one (e.g. an unused hi word followed by a real index) must
 * never be misread as one shifted-by-one pair, which an UNALIGNED (stride-1) scan risks whenever the index is
 * itself in a VA-shaped range (idx in 1..0xff paired with a preceding 0 looks exactly like a tiny VA - is_va only
 * needs hi in 1..0xff and lo 4-aligned). Each 2-word slot (lo, hi) is EITHER a raw buffer pointer (is_va: skip, not
 * an index - P's uniforms/lod_bias) OR a candidate index at `lo` when nonzero and under the bound (hi is 0 or an
 * override word for every ABI row seen so far, never itself an index). Deduplicated, capped at `max`. This is a
 * CENSUS heuristic, not an ABI match: it names CANDIDATE indices generically, the same way n48_gcap_scan names
 * candidate pointers generically, for gcap_region (N48_GCAP_USER2) to dereference — a spurious candidate costs one
 * wasted capture slot, never a wrong translation (nothing here writes or decides). */
#define N48_GCAP_USER_IDX_WINDOW 16u
#define N48_GCAP_USER_IDX_MAX 4u
#define N48_GCAP_USER_IDX_BOUND 0x00010000u
static inline uint32_t n48_gcap_user_indices(const uint32_t *d, uint32_t n, uint64_t startVa, uint32_t tableDword,
                                             uint32_t *idx, uint32_t max)
{
    uint32_t found = 0;
    const uint32_t winEnd = tableDword + 2u + N48_GCAP_USER_IDX_WINDOW;
    const uint32_t end = winEnd < n ? winEnd : n;
    for (uint32_t q = tableDword + 2u; q + 1u < end; q += 2u) {
        if (n48_gcap_is_va(startVa, d[q], d[q + 1u])) continue;   /* a buffer pointer, not an index */
        const uint32_t v = d[q];
        if (v && v < N48_GCAP_USER_IDX_BOUND) {
            int dup = 0;
            for (uint32_t z = 0; z < found && z < max; z++) if (idx[z] == v) { dup = 1; break; }
            if (!dup) { if (found < max) idx[found] = v; found++; }
        }
    }
    return found;
}

/* build 0.0.450 item 4 (THE CENSUS CAPTURE, exactly per the review of 0.0.448 measured on decide42's 266 IB
 * bodies: at most 2 distinct table VAs per frame (median 1), at most 29 (table, SGPR, index) rows per frame (median
 * 15, p95 26); without the filler fix 106 of 266 IB walks stop early). A SEPARATE, census-ONLY pass the kext runs
 * AFTER n48_gcap_scan (which stays untouched) over each IB of a frame, in order, with a CALLER-OWNED `ps[16]` array
 * that persists ACROSS every IB of the frame ("PS user data tracked per slot across IBs" - the review's own words):
 * SPI_SHADER_USER_DATA_PS_0..15 (gc_10_3_0_offset.h regSPI_SHADER_USER_DATA_PS_0 0x2c0c, the SAME absolute-dword
 * addressing n48_gcap_scan_ex's own USER DATA POINTERS banner uses for `it[].index`; xlat12_ib.c's D_PS_UD0 0xb030
 * byte address is the same register, 0x2c0c * 4). At each DRAW (xlat12_ib.c's own `is_draw`: DRAW_INDEX_AUTO 0x2D,
 * DRAW_INDEX_2 0x27, DRAW_INDEX_OFFSET_2 0x35, DRAW_INDIRECT 0x24, DRAW_INDEX_INDIRECT 0x25) whose s0:s1
 * (ps[0]:ps[1], the class-19 table pointer's own convention) is VA-shaped (n48_gcap_is_va), one row is produced for
 * each EVEN slot 2..14 that holds a small nonzero integer under N48_GCAP_CENSUS_BOUND - the SAME "small integer"
 * bound n48_gcap_user_indices above already uses - tagged with the SGPR (the slot number, 2/4/6/8/10/12/14) that
 * held it, which REPLACES that function's fixed stride-2 window: a window measured from the table's own dword
 * position can cross a LATER packet's own body (the review's own critique), while an SGPR is the register file
 * itself and is correct wherever in the IB stream (or however many packets) it was last written. Rows beyond `max`
 * are counted in *total but not stored - the caller's own per-frame budget (kGcapCensusPerFrame in
 * AppleHardwareHook.cpp, separate from n48_gcap_scan's regionsLeft) sizes `max` and does the actual charging; this
 * function performs no I/O, allocates nothing and writes only `ps`/`rows`/`*total`.
 * THE FILLER STEP (N48_GCAP_F_FILLER, one dword, not the 16385 its own count field claims - banner above
 * n48_gcap_scan_ex): applied UNCONDITIONALLY here (this is a NEW pass with no old caller to keep byte-identical, so
 * there is no flag - unlike n48_gcap_scan_ex, which keeps the flag for its EXISTING callers). Without it a filler
 * dword partway through an IB is walked as a 16384-dword type-3 body and the rest of the IB (every draw past it) is
 * never reached - exactly the 106-of-266 failure the review measured. */
enum { N48_GCAP_CENSUS_SLOT_LO = 2u, N48_GCAP_CENSUS_SLOT_HI = 14u, N48_GCAP_CENSUS_BOUND = 0x00010000u };
#define N48_GCAP_PS_UD0 0x2c0cu   /* absolute dword register of SPI_SHADER_USER_DATA_PS_0 (gc_10_3_0_offset.h; xlat12_ib.c D_PS_UD0 0xb030 byte addr / 4) */

typedef struct { uint64_t va; uint32_t sgpr, idx, dword; } n48_gcap_census_row;

static inline uint32_t n48_gcap_census_ib(const uint32_t *d, uint32_t n, uint64_t startVa, uint32_t *ps,
                                          n48_gcap_census_row *rows, uint32_t max, uint32_t *total)
{
    uint32_t cnt = 0, i = 0;
    while (i < n) {
        const uint32_t h = d[i];
        if (h == 0xFFFF1000u) { i++; continue; }         /* the one-dword filler, unconditionally (see banner above) */
        const uint32_t type = h >> 30;
        if (type == 2u) { i++; continue; }
        if (type != 3u) break;
        const uint32_t op = (h >> 8) & 0xffu, cnt3 = (h >> 16) & 0x3fffu, plen = cnt3 + 2u;
        if (plen > n - i) break;
        const uint32_t *b = &d[i + 1u];
        const uint32_t nb = cnt3 + 1u;
        if (op == 0x76u || op == 0x9bu) {                 /* SET_SH_REG / SET_SH_REG_INDEX: track every PS_0..15 write */
            const uint32_t reg0 = 0x2c00u + (b[0] & 0xffffu);
            for (uint32_t k = 1; k < nb; k++) {
                const uint32_t reg = reg0 + (k - 1u);
                if (reg >= N48_GCAP_PS_UD0 && reg < N48_GCAP_PS_UD0 + 16u) ps[reg - N48_GCAP_PS_UD0] = b[k];
            }
        } else if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) {   /* is_draw (xlat12_ib.c) */
            if (n48_gcap_is_va(startVa, ps[0], ps[1])) {
                const uint64_t tva = (((uint64_t)ps[1]) << 32) | ps[0];
                for (uint32_t s = N48_GCAP_CENSUS_SLOT_LO; s <= N48_GCAP_CENSUS_SLOT_HI; s += 2u) {
                    const uint32_t v = ps[s];
                    if (v && v < N48_GCAP_CENSUS_BOUND) {
                        if (cnt < max) { rows[cnt].va = tva; rows[cnt].sgpr = s; rows[cnt].idx = v; rows[cnt].dword = i; }
                        cnt++;
                    }
                }
            }
        }
        i += plen;
    }
    if (total) *total = cnt;
    return i;
}

/* =====================================================================================================================
 * build 0.0.457 — SWITCH 54: THE COLOUR-TARGET RUNG EXAMINES EVERY ITEM AND EVERY DRAW.
 *
 * WHAT LEFT SUSPECTED, SETTLED OFFLINE ON THE RUNS' OWN BYTES. The gather (AppleHardwareHook.cpp,
 * gfxsrc_decide_frame) hands n48_gcap_scan an item cap of 64 and refuses the frame (`target_vram` = 1, the
 * fail-closed clause "an item past the 64th was never examined") when an IB names more. The CB/DB items are pushed
 * AFTER the packet loop, so an IB past the cap never has its colour target examined at all. Every two-head 7|5 frame
 * (IB lengths 15520|7616, e.g. mibA1 F50/F75, decide46 F48/F92/F108/F124, decide44 F84) names 161-164 items in IB 0
 * (its CB items at #149-#152) and 108 in IB 1 (its CB items at #96): both IBs overflow, so the frame is refused at
 * `target-in-vram` whatever its targets are. Seven programs of IB 0 and one of IB 1 also sit past item 64, so the
 * program rungs never saw them either - which is why the overflow clause cannot simply be dropped.
 *
 * A SECOND, OLDER GAP, FOUND IN THE SAME BYTES. n48_gcap_scan reports only the LAST CB_COLORn_BASE of each slot at the
 * END of the walk. F84's IB 0 draws into seven different colour targets (CB0 = 0x400800000, 0x401348000, 0x400708000,
 * 0x4012a0000, 0x400788000, 0x400034000, 0x4000ac000) and the scan names one of them. So "every colour target
 * examined" was not true of ANY frame with more than one render pass, overflow or not.
 *
 * THE FIX (switch 54 ON; OFF is 0.0.456's exact call and clause, see n48_gcap_tv_ib):
 *   1. the item scan runs with the whole item array the kext already owns (N48_GCAP_TV_ITEMS, 512 - the R5′ build
 *      already uses it) and with the one-dword filler fix, so every item of an IB up to 512 is examined by the SAME
 *      item loop (programs and colour targets alike), and an IB naming more than 512 still refuses (N5, unchanged);
 *   2. a scan that stops before the IB's end refuses (the dwords past the stop were never examined);
 *   3. a SECOND, colour-target-only pass (n48_gcap_cbt_ib) walks every dword of every IB of the frame, carrying the
 *      context-register state across the frame's IBs, and records the colour target of EVERY active slot at EVERY
 *      draw, plus each IB's end state. Anything it cannot see refuses: a nested IB, a context-register load, RMW or
 *      indexed write that touches the colour-target registers, an active slot whose BASE or BASE_EXT this frame never
 *      wrote (inherited from before the frame), or more distinct targets than its table holds;
 *   4. every target the pass recorded is resolved by the caller (n48_gcap_tv_resolve) with EXACTLY the per-target rule
 *      the item loop applies: a target in VRAM refuses unless N1 (switch 12) is on, and a target that does not resolve
 *      refuses (N5).
 * Every clause above can only SET the flag. The only frame ON admits that OFF refused is one whose sole refusal was the
 * 64-item cap and whose every item and every per-draw target then passed the same per-target rule.
 * NOT covered (SUSPECTED residuals, unchanged from 0.0.456 except where item 5 below closes part of it): DMA_DATA with
 * a register destination, or a colour target restored from a CONTEXT_CONTROL shadow; a VA-0 base on an active
 * slot is skipped exactly as the item loop skips it (: Apple zeroes unbound slots).
 *
 * build 0.0.472 item 5 (F1, review of 0.0.457) — CLOSES THE COPY_DATA/WRITE_DATA/WAIT_REG_MEM PART OF THE GAP
 * ABOVE. xlat12_ib.c's own translator (operand_ok/reg_identical) PASSES a register-destination WRITE_DATA, COPY_DATA or
 * the write-half of a WAIT_REG_MEM through UNCHANGED whenever the destination register is XLAT12_CLS_IDENTICAL - which
 * every CB_COLORn_BASE/CB_COLORn_BASE_EXT/CB_TARGET_MASK register is, being a plain address/mask with nothing gfx12
 * remaps. So such a packet targeting one of those registers is not a hypothetical: it is a write our per-draw pass
 * (n48_gcap_cbt_ib, below) walked straight past, because it recognised no opcode for it and fell through the if/else-if
 * chain doing nothing - the SAME "residual" this banner already named for COPY_DATA/WRITE_DATA before this build task,
 * now closed for the register-destination form of all three (the memory-destination and pure-wait forms carry no
 * write, so they are correctly left alone). Same decode as xlat12_ib.c's own register-operand rung (0.0.449 item 2,
 * 0.0.451 item 4): WRITE_DATA's DST_SEL (control bits [11:8]) selects register vs memory, WR_ONE_ADDR (bit 16) picks
 * repeat-one vs auto-increment; COPY_DATA's DST_SEL (bits [11:8]) and COUNT_SEL (bit 16) do the same for its one/two
 * destination registers; WAIT_REG_MEM only WRITES in its wr_wait_wr_reg form (OPERATION bits [7:6] == 1, and not
 * MEM_SPACE bits [5:4] == 1), and only its FIRST register (reg0) is written - reg1 is polled, never written (the SAME
 * 0.0.451 item 4 correction). Detected here as UNSEEN (N48_CBT_U_WRITE), never as a translate-time refusal: this
 * header's job is to say what the per-draw pass could not account for, not to change what the translator allows.
 * ===================================================================================================================== */
#define N48_GCAP_TV_ITEMS 512u
#define N48_GCAP_CBT_MAX  32u
enum {
    N48_TV_WHY_OVER   = 0x01u,   /* an IB named more items than the cap: the rest were never examined */
    N48_TV_WHY_WALK   = 0x02u,   /* a scan stopped before the IB's end: the dwords past the stop were never examined */
    N48_TV_WHY_UNSEEN = 0x04u,   /* a colour target the per-draw pass could not see (see n48_gcap_cbt.unseen_why) */
    N48_TV_WHY_CBOVER = 0x08u,   /* more distinct per-draw targets than N48_GCAP_CBT_MAX */
    N48_TV_WHY_UNRES  = 0x10u,   /* a per-draw target's page walk failed */
    N48_TV_WHY_VRAM   = 0x20u,   /* a per-draw target resolved into VRAM (refuses only while N1 is off) */
    N48_TV_WHY_ITEM   = 0x40u    /* the item loop's own per-item clauses (0.0.456's) set it */
};
enum { N48_CBT_U_NESTED = 0x1u, N48_CBT_U_LOAD = 0x2u, N48_CBT_U_INHERIT = 0x4u,
       /* build 0.0.472 item 5 — a register-destination WRITE_DATA/COPY_DATA/WAIT_REG_MEM named a colour-target
        * register (n48_gcap_cb_reg/n48_gcap_cb_range). */
       N48_CBT_U_WRITE = 0x8u };

typedef struct {
    uint32_t base[8], ext[8];          /* CB_COLORn_BASE / CB_COLORn_BASE_EXT as this frame last wrote them */
    uint32_t written, ext_written;     /* bit n: this frame wrote slot n's BASE / BASE_EXT */
    uint32_t mask, mask_written;       /* CB_TARGET_MASK (context 0xa08e), and whether this frame wrote it */
    uint32_t draws, unseen, unseen_why;
    uint32_t n;                        /* distinct target VAs named; past N48_GCAP_CBT_MAX they are counted, not stored */
    uint64_t va[N48_GCAP_CBT_MAX];
} n48_gcap_cbt;

static inline void n48_gcap_cbt_reset(n48_gcap_cbt *s)
{
    if (!s) return;
    for (uint32_t c = 0; c < 8u; c++) { s->base[c] = 0u; s->ext[c] = 0u; }
    s->written = s->ext_written = s->mask = s->mask_written = 0u;
    s->draws = s->unseen = s->unseen_why = s->n = 0u;
    for (uint32_t q = 0; q < N48_GCAP_CBT_MAX; q++) s->va[q] = 0ull;
}

/* The context registers that name a colour target: CB_COLORn_BASE (0xa318 + 15n), CB_COLORn_BASE_EXT (0xa390 + n) and
 * CB_TARGET_MASK (0xa08e). Absolute context dwords, the same addressing n48_gcap_scan_ex uses. */
static inline int n48_gcap_cb_reg(uint32_t reg)
{
    if (reg >= 0xa318u && reg < 0xa318u + 15u * 8u && ((reg - 0xa318u) % 15u) == 0u) return 1;
    if (reg >= 0xa390u && reg < 0xa398u) return 1;
    return reg == 0xa08eu;
}
static inline int n48_gcap_cb_range(uint32_t reg0, uint32_t cnt)
{
    if (cnt > 0x400u) cnt = 0x400u;    /* a bounded loop; the whole context space is 0x400 dwords */
    for (uint32_t r = 0; r < cnt; r++) if (n48_gcap_cb_reg(reg0 + r)) return 1;
    return 0;
}
static inline void n48_gcap_cbt_add(n48_gcap_cbt *s, uint64_t va)
{
    if (!va) return;                   /* the item loop's own `it.va` test: Apple's zeroed base of an unbound slot */
    const uint32_t m = s->n < N48_GCAP_CBT_MAX ? s->n : N48_GCAP_CBT_MAX;
    for (uint32_t q = 0; q < m; q++) if (s->va[q] == va) return;
    if (s->n < N48_GCAP_CBT_MAX) s->va[s->n] = va;
    s->n++;
}
static inline uint64_t n48_gcap_cbt_va(const n48_gcap_cbt *s, uint32_t c)
{
    return (((uint64_t)(s->ext[c] & 0xffu)) << 40) | (((uint64_t)s->base[c]) << 8);
}
/* A draw (xlat12_ib.c's is_draw, plus the other gfx10 draw opcodes, so an unrecognised draw can only ADD targets). */
static inline int n48_gcap_is_draw(uint32_t op)
{
    return op == 0x24u || op == 0x25u || op == 0x27u || op == 0x2cu || op == 0x2du || op == 0x2eu || op == 0x30u ||
           op == 0x35u || op == 0x38u;
}

/* One IB of the frame, in submission order; `s` carries the context state from the frame's earlier IBs (reset it once per
 * frame). Returns the dwords walked; a return below `n` means the rest of the IB was not examined. */
static inline uint32_t n48_gcap_cbt_ib(const uint32_t *d, uint32_t n, n48_gcap_cbt *s)
{
    uint32_t i = 0;
    if (!d || !s) return 0u;
    while (i < n) {
        const uint32_t h = d[i];
        if (h == 0xFFFF1000u) { i++; continue; }          /* the one-dword filler */
        const uint32_t type = h >> 30;
        if (type == 2u) { i++; continue; }
        if (type != 3u) break;
        const uint32_t op = (h >> 8) & 0xffu, cnt3 = (h >> 16) & 0x3fffu, plen = cnt3 + 2u;
        if (plen > n - i) break;
        const uint32_t *b = &d[i + 1u];
        const uint32_t nb = cnt3 + 1u;
        if (op == 0x69u && nb >= 2u) {                    /* SET_CONTEXT_REG */
            const uint32_t reg0 = 0xa000u + (b[0] & 0xffffu);
            for (uint32_t k = 1; k < nb; k++) {
                const uint32_t reg = reg0 + (k - 1u), v = b[k];
                if (reg >= 0xa318u && reg < 0xa318u + 15u * 8u && ((reg - 0xa318u) % 15u) == 0u) {
                    const uint32_t c = (reg - 0xa318u) / 15u; s->base[c] = v; s->written |= 1u << c;
                } else if (reg >= 0xa390u && reg < 0xa398u) {
                    const uint32_t c = reg - 0xa390u; s->ext[c] = v; s->ext_written |= 1u << c;
                } else if (reg == 0xa08eu) {
                    s->mask = v; s->mask_written = 1u;
                }
            }
        } else if (op == 0x6au || op == 0x51u) {          /* SET_CONTEXT_REG_INDEX / CONTEXT_REG_RMW: not parsed */
            if (nb >= 1u && n48_gcap_cb_range(0xa000u + (b[0] & 0xffffu), op == 0x51u ? 1u : (nb > 1u ? nb - 1u : 1u))) {
                s->unseen++; s->unseen_why |= N48_CBT_U_LOAD;
            }
        } else if (op == 0x61u) {                         /* LOAD_CONTEXT_REG: (offset, count) pairs from b[2] */
            for (uint32_t k = 2; k + 1u < nb; k += 2u)
                if (n48_gcap_cb_range(0xa000u + (b[k] & 0xffffu), b[k + 1u])) { s->unseen++; s->unseen_why |= N48_CBT_U_LOAD; break; }
        } else if (op == 0x9fu) {                         /* LOAD_CONTEXT_REG_INDEX: its body is not parsed here */
            s->unseen++; s->unseen_why |= N48_CBT_U_LOAD;
        } else if (op == 0x33u || op == 0x3fu) {          /* a nested IB: its draws are not walked here */
            s->unseen++; s->unseen_why |= N48_CBT_U_NESTED;
        } else if (op == 0x37u && nb >= 3u) {              /* WRITE_DATA: build 0.0.472 item 5 (F1) */
            /* Register-destination form (DST_SEL, control-dword b[0] bits [11:8], == 0 - a nonzero value is a memory
             * destination and none of our business). WR_ONE_ADDR (b[0] bit 16) repeats b[1] `ndata` times; without it
             * b[1]..b[1]+ndata-1 are consecutive registers (auto-increment) - the SAME decode xlat12_ib.c's own
             * WRITE_DATA rung uses (0.0.449 item 2, F2), so `ndata = nb - 3` matches its `l - 4` (nb = l - 1 here). */
            if (((b[0] >> 8) & 0xfu) == 0u) {
                const uint32_t oneAddr = (b[0] >> 16) & 1u, ndata = nb - 3u;
                if (ndata && n48_gcap_cb_range(b[1], oneAddr ? 1u : ndata)) { s->unseen++; s->unseen_why |= N48_CBT_U_WRITE; }
            }
        } else if (op == 0x40u && nb >= 4u) {              /* COPY_DATA: build 0.0.472 item 5 (F1) */
            /* Register-destination form (DST_SEL, control-dword b[0] bits [11:8], == 0); COUNT_SEL (bit 16) selects a
             * 64-bit (two-register) copy. The SOURCE side (bits [3:0]) is not this rung's business: a register SOURCE
             * cannot itself write a colour target. */
            if (((b[0] >> 8) & 0xfu) == 0u) {
                const uint32_t wide = (b[0] >> 16) & 1u;
                if (n48_gcap_cb_range(b[3], wide ? 2u : 1u)) { s->unseen++; s->unseen_why |= N48_CBT_U_WRITE; }
            }
        } else if (op == 0x3cu && nb >= 2u) {              /* WAIT_REG_MEM: build 0.0.472 item 5 (F1) */
            /* Only the wr_wait_wr_reg form (control-dword b[0] OPERATION bits [7:6] == 1, and NOT MEM_SPACE bits
             * [5:4] == 1) WRITES a register, and only its FIRST register (b[1]) - the second (b[2], when present) is
             * polled, never written (xlat12_ib.c's own 0.0.451 item 4 correction; the SAME banner this header already
             * carried before this build task named the earlier, backwards version of this same rule). The ordinary
             * wait/compare form writes nothing at all. */
            if (((b[0] >> 4) & 3u) != 1u && ((b[0] >> 6) & 3u) == 1u && n48_gcap_cb_reg(b[1])) {
                s->unseen++; s->unseen_why |= N48_CBT_U_WRITE;
            }
        } else if (op == 0x12u) {                         /* CLEAR_STATE: nothing this frame wrote survives it */
            s->written = 0u; s->ext_written = 0u; s->mask_written = 0u;
        } else if (n48_gcap_is_draw(op)) {
            s->draws++;
            for (uint32_t c = 0; c < 8u; c++) {
                const uint32_t active = s->mask_written ? (((s->mask >> (4u * c)) & 0xfu) != 0u) : 1u;
                if (!active) continue;
                if (!((s->written >> c) & 1u) || !((s->ext_written >> c) & 1u)) {
                    s->unseen++; s->unseen_why |= N48_CBT_U_INHERIT; continue;
                }
                n48_gcap_cbt_add(s, n48_gcap_cbt_va(s, c));
            }
        }
        i += plen;
    }
    /* and the IB's end state, as n48_gcap_scan names it: every slot this frame has written */
    for (uint32_t c = 0; c < 8u; c++) if ((s->written >> c) & 1u) n48_gcap_cbt_add(s, n48_gcap_cbt_va(s, c));
    return i;
}

/* THE SCAN STEP OF ONE IB FOR THE `target-in-vram` RUNG, the one call the kext makes per IB. Returns how many items the
 * caller's item loop must examine; sets *refuse when the fail-closed clause fires (the caller applies it under
 * gXdTvramFailClosed). OFF (`on` 0): 0.0.456's call and clause exactly - n48_gcap_scan with a cap of 64, refuse past it,
 * examine min(total, 64); `why` and `s` are not touched. ON: see the banner above. */
static inline uint32_t n48_gcap_tv_ib(uint32_t on, const uint32_t *d, uint32_t n, uint64_t startVa, n48_gcap_item *it,
                                      uint32_t *total, uint32_t *refuse, uint32_t *why, n48_gcap_cbt *s)
{
    uint32_t t = 0u;
    if (!on) {
        (void)n48_gcap_scan(d, n, startVa, it, 64u, &t);
        if (t > 64u) *refuse = 1u;
        *total = t;
        return t < 64u ? t : 64u;
    }
    const uint32_t walked = n48_gcap_scan_ex(d, n, startVa, it, N48_GCAP_TV_ITEMS, &t, N48_GCAP_F_FILLER);
    if (walked != n) { *refuse = 1u; *why |= N48_TV_WHY_WALK; }
    if (t > N48_GCAP_TV_ITEMS) { *refuse = 1u; *why |= N48_TV_WHY_OVER; }
    if (!s) { *refuse = 1u; *why |= N48_TV_WHY_UNSEEN; }   /* no per-draw pass: its targets were never examined */
    else if (n48_gcap_cbt_ib(d, n, s) != n) { *refuse = 1u; *why |= N48_TV_WHY_WALK; }
    *total = t;
    return t < N48_GCAP_TV_ITEMS ? t : N48_GCAP_TV_ITEMS;
}

/* The resolver the caller supplies: 1 when `va`'s page resolved, with *is_sys 1 for host memory. */
typedef uint32_t (*n48_gcap_page_fn)(void *ud, uint64_t va, uint32_t *is_sys);

/* The frame's per-draw targets, judged by the item loop's own per-target rule. Returns 1 when target_vram must be set.
 * build 0.0.472 item 6 (F4, instrument only) — `vram_seen`, OPTIONAL (a caller not interested in the count passes
 * nullptr): incremented for every per-draw target that resolves into VRAM, WHETHER OR NOT `drop` (N1) is on. Through
 * this build task the count was folded into `*why`'s N48_TV_WHY_VRAM bit, which N1 gates (`!drop`) - so a boot run
 * with N1 ON, where every target really was in VRAM and N1 correctly let every one of them through, read VRAM 0 in
 * the tvscan457 report line: indistinguishable from a boot where no target was ever in VRAM at all. The REFUSAL
 * decision (`r`/`*why`'s N48_TV_WHY_VRAM bit) is UNCHANGED: still set, and still only, when `!drop`. */
static inline uint32_t n48_gcap_tv_resolve(const n48_gcap_cbt *s, n48_gcap_page_fn fn, void *ud, uint32_t drop,
                                           uint32_t fail_closed, uint32_t *why, uint32_t *vram_seen)
{
    uint32_t r = 0u;
    if (!s) { if (fail_closed) { *why |= N48_TV_WHY_UNSEEN; return 1u; } return 0u; }
    if (s->unseen && fail_closed) { r = 1u; *why |= N48_TV_WHY_UNSEEN; }
    if (s->n > N48_GCAP_CBT_MAX && fail_closed) { r = 1u; *why |= N48_TV_WHY_CBOVER; }
    const uint32_t m = s->n < N48_GCAP_CBT_MAX ? s->n : N48_GCAP_CBT_MAX;
    for (uint32_t q = 0; q < m; q++) {
        uint32_t sys = 0u;
        const uint32_t ok = fn ? fn(ud, s->va[q], &sys) : 0u;
        if (ok && !sys) {
            if (vram_seen) (*vram_seen)++;   /* counted whether or not N1 is on */
            if (!drop) { r = 1u; *why |= N48_TV_WHY_VRAM; }   /* the refusal itself: UNCHANGED, still !drop only */
        }
        if (!ok && fail_closed) { r = 1u; *why |= N48_TV_WHY_UNRES; }
    }
    return r;
}

#endif
