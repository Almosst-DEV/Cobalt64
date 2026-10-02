/* xlat12_headless.c - the headless-pass recogniser. Contract, scope and failure modes: xlat12_headless.h. OFFLINE ONLY. */
#include "xlat12_headless.h"

#define HL_PT(h) (((h) >> 30) & 3u)
#define HL_PC(h) (((h) >> 16) & 0x3FFFu)
#define HL_PO(h) (((h) >> 8) & 0xFFu)

enum {
    HL_NOP = 0x10u, HL_CONTEXT_CONTROL = 0x28u, HL_DRAW_INDEX_AUTO = 0x2Du, HL_NUM_INSTANCES = 0x2Fu, HL_WRITE_DATA = 0x37u,
    HL_WAIT_REG_MEM = 0x3Cu, HL_EVENT_WRITE = 0x46u, HL_RELEASE_MEM = 0x49u, HL_ACQUIRE_MEM = 0x58u,
    HL_SET_CONTEXT_REG = 0x69u, HL_SET_SH_REG = 0x76u, HL_SET_UCONFIG_REG = 0x79u, HL_SET_UCONFIG_REG_INDEX = 0x7Au,
    HL_SET_SH_REG_INDEX = 0x9Bu
};

/* H7. Chosen from's pass-1 table: every register below is one the translated pass CANNOT get right by inheritance - the
 * two programs and their resource words (the resolver and d_reg's overrides key on them), the fragment inputs (the
 * interpolation guard reads 0x286d8 and refuses unless it was seen), colour target 0 (the pass's whole output), the
 * primitive type, and the raster / blend / depth words that decide what reaches that target. */
const uint32_t kXlat12HlRequired[XLAT12_HL_REQUIRED] = {
    0x28b54u,                                   /* VGT_SHADER_STAGES_EN */
    0x0b120u, 0x0b124u, 0x0b128u, 0x0b12cu,     /* SPI_SHADER_PGM_LO/HI/RSRC1/RSRC2_VS */
    0x0b020u, 0x0b024u, 0x0b028u, 0x0b02cu,     /* SPI_SHADER_PGM_LO/HI/RSRC1/RSRC2_PS */
    0x286ccu, 0x286d0u, 0x286d8u,               /* SPI_PS_INPUT_ENA / _ADDR / SPI_PS_IN_CONTROL */
    0x28714u,                                   /* SPI_SHADER_COL_FORMAT */
    0x28c60u, 0x28c6cu, 0x28c70u, 0x28e40u,     /* CB_COLOR0_BASE / VIEW / INFO / BASE_EXT */
    0x28238u, 0x2823cu,                         /* CB_TARGET_MASK / CB_SHADER_MASK */
    0x28808u, 0x28780u,                         /* CB_COLOR_CONTROL / CB_BLEND0_CONTROL */
    0x28204u, 0x28208u,                         /* PA_SC_WINDOW_SCISSOR_TL / BR */
    0x28810u, 0x28818u,                         /* PA_CL_CLIP_CNTL / PA_CL_VTE_CNTL */
    0x28800u,                                   /* DB_DEPTH_CONTROL */
    0x30908u,                                   /* VGT_PRIMITIVE_TYPE */
};

const char *xlat12_hl_reason_name(uint32_t why)
{
    static const char *const n[XLAT12_HL_REASONS] = {
        "headless-ok", "arg", "not-ctxctl", "has-encoder-head", "walk", "foreign-packet", "draw-count", "tail-after-draw",
        "incomplete-state", "no-trailer", "too-many-passes" };
    return why < XLAT12_HL_REASONS ? n[why] : "?";
}

/* One packet's length, 0 when it is malformed or runs past n (the same rule xlat12_ib_walk applies). */
static uint32_t hl_plen(const uint32_t *in, uint32_t i, uint32_t n)
{
    const uint32_t h = in[i];
    if (h == XLAT12_IB_NOP || HL_PT(h) == 2u) return 1u;
    if (HL_PT(h) != 3u) return 0u;
    const uint32_t l = HL_PC(h) + 2u;
    return (uint64_t)i + l <= (uint64_t)n ? l : 0u;
}
static int hl_is_nop(uint32_t h) { return h == XLAT12_IB_NOP || HL_PT(h) == 2u || HL_PO(h) == HL_NOP; }
/* The encoder head exactly as xlat12_ib_segments' seg_start_at defines it (xlat12_ib.c) - repeated, not relaxed. */
static int hl_seg_head(const uint32_t *in, uint32_t i, uint32_t n)
{
    return (uint64_t)i + 12u <= (uint64_t)n && in[i] == XLAT12_SEG_HEAD0 && in[i + 1u] == XLAT12_SEG_HEAD1 &&
           in[i + 2u] == XLAT12_SEG_ACQUIRE && in[i + 10u] == XLAT12_SEG_HEAD0 && in[i + 11u] == XLAT12_SEG_EVENT_E;
}
static int hl_ctxctl_proven(const uint32_t *in, uint32_t i, uint32_t l)
{
    return l == 3u && in[i] == XLAT12_IB_CTXCTL_HDR && in[i + 1u] == XLAT12_IB_CTXCTL_DW1 && in[i + 2u] == XLAT12_IB_CTXCTL_DW2;
}
static int hl_alphabet(uint32_t op)
{
    switch (op) {
    case HL_CONTEXT_CONTROL: case HL_SET_CONTEXT_REG: case HL_SET_SH_REG: case HL_SET_SH_REG_INDEX: case HL_SET_UCONFIG_REG:
    case HL_SET_UCONFIG_REG_INDEX: case HL_NUM_INSTANCES: case HL_WRITE_DATA: case HL_RELEASE_MEM: case HL_WAIT_REG_MEM:
    case HL_EVENT_WRITE: case HL_ACQUIRE_MEM: case HL_DRAW_INDEX_AUTO: case HL_NOP:
        return 1;
    default:
        return 0;
    }
}
/* Mark the registers a SET packet writes in the completeness bitmap. */
static void hl_mark(const uint32_t *in, uint32_t i, uint32_t l, uint32_t op, uint32_t *seen)
{
    const uint32_t base = op == HL_SET_CONTEXT_REG ? 0xA000u
                        : (op == HL_SET_SH_REG || op == HL_SET_SH_REG_INDEX) ? 0x2C00u
                        : (op == HL_SET_UCONFIG_REG || op == HL_SET_UCONFIG_REG_INDEX) ? 0xC000u : 0u;
    if (!base || l < 3u) return;
    const uint32_t off = in[i + 1u] & 0xFFFFu;
    for (uint32_t k = 0; k + 2u < l; k++) {
        const uint32_t g10 = (base + off + k) << 2;
        for (uint32_t r = 0; r < XLAT12_HL_REQUIRED; r++)
            if (kXlat12HlRequired[r] == g10) { seen[r >> 5] |= 1u << (r & 31u); break; }
    }
}
static uint32_t hl_refuse(xlat12_hl_report *rep, uint32_t why, uint32_t at, uint32_t detail, uint32_t passes)
{
    if (rep) { rep->why = why; rep->at = at; rep->detail = detail; rep->passes = passes; }
    return 0u;
}

uint32_t xlat12_ib_headless_passes_m(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total,
                                     xlat12_hl_report *rep, uint32_t skip)
{
    if (total) *total = 0u;
    if (rep) { rep->why = XLAT12_HL_OK; rep->at = 0u; rep->detail = 0u; rep->passes = 0u; }
    if (!in || !seg || n == 0u || max == 0u) return hl_refuse(rep, XLAT12_HL_ARG, 0u, 0u, 0u);

    /* H1: the stream opens with the proven CONTEXT_CONTROL */
    if (!(skip & XLAT12_HL_SKIP_H1) && !hl_ctxctl_proven(in, 0u, hl_plen(in, 0u, n)))
        return hl_refuse(rep, XLAT12_HL_NOT_CTXCTL, 0u, in[0], 0u);

    /* H2 + H3 + H4 in one walk: packet-aligned, as xlat12_ib_segments walks */
    uint32_t i = 0u;
    while (i < n) {
        const uint32_t l = hl_plen(in, i, n);
        if (!l) break;
        if (!(skip & XLAT12_HL_SKIP_H2) && hl_seg_head(in, i, n)) return hl_refuse(rep, XLAT12_HL_HAS_HEAD, i, 0u, 0u);
        if (!(skip & XLAT12_HL_SKIP_H4) && !hl_is_nop(in[i])) {
            const uint32_t op = HL_PO(in[i]);
            if (!hl_alphabet(op)) return hl_refuse(rep, XLAT12_HL_FOREIGN, i, op, 0u);
            /* a CONTEXT_CONTROL is only ever a pass opener, and only in the proven form */
            if (op == HL_CONTEXT_CONTROL && !hl_ctxctl_proven(in, i, l)) return hl_refuse(rep, XLAT12_HL_FOREIGN, i, op, 0u);
            /* SET_*_INDEX with a non-zero index field outside the SH/UCONFIG forms the draw policy handles is still refused
             * there; here only the opcode is judged */
        }
        i += l;
    }
    if (!(skip & XLAT12_HL_SKIP_H3) && i != n) return hl_refuse(rep, XLAT12_HL_WALK, i, i, 0u);
    const uint32_t walked = i;

    /* H5-H8: split into passes at every proven CONTEXT_CONTROL and judge each */
    uint32_t np = 0u, start = 0u, draws = 0u, drawAt = 0u, lastTrailer = 0u;
    uint32_t seen[(XLAT12_HL_REQUIRED + 31u) / 32u] = { 0u };
    i = 0u;
    while (i <= walked) {
        const int atEnd = i >= walked;
        const uint32_t l = atEnd ? 0u : hl_plen(in, i, n);
        const int opener = !atEnd && hl_ctxctl_proven(in, i, l);
        if (atEnd || (opener && i != start)) {
            /* close the pass [start, i) */
            if (!(skip & XLAT12_HL_SKIP_H5) && draws != 1u) return hl_refuse(rep, XLAT12_HL_DRAWS, start, draws, np);
            /* H8: every pass ends in its own trailer (ACQUIRE_MEM after the draw; NOPs after that are allowed) */
            if (!(skip & XLAT12_HL_SKIP_H8) && !lastTrailer) return hl_refuse(rep, XLAT12_HL_TRAILER, start, drawAt, np);
            if (!(skip & XLAT12_HL_SKIP_H7))
                for (uint32_t r = 0; r < XLAT12_HL_REQUIRED; r++)
                    if (!((seen[r >> 5] >> (r & 31u)) & 1u)) return hl_refuse(rep, XLAT12_HL_STATE, start, kXlat12HlRequired[r], np);
            if (np < max) {
                seg[np].head = start; seg[np].start = start; seg[np].end = i; seg[np].draws = draws; seg[np].draw_at = drawAt;
            }
            np++;
            if (atEnd) break;
            start = i; draws = 0u; drawAt = 0u; lastTrailer = 0u;
            for (uint32_t w = 0; w < sizeof seen / sizeof seen[0]; w++) seen[w] = 0u;
        }
        if (!l) break;
        const uint32_t h = in[i];
        if (hl_is_nop(h)) { i += l; continue; }
        const uint32_t op = HL_PO(h);
        if (op == HL_DRAW_INDEX_AUTO) {
            draws++; drawAt = i; lastTrailer = 0u;
        } else if (draws >= 1u) {
            /* H6: after the draw only the trailer */
            if (op == HL_ACQUIRE_MEM) lastTrailer = 1u;
            else if (!(skip & XLAT12_HL_SKIP_H6)) return hl_refuse(rep, XLAT12_HL_TAIL, i, op, np);
        } else {
            hl_mark(in, i, l, op, seen);
        }
        i += l;
    }
    if (np > max) return hl_refuse(rep, XLAT12_HL_TOO_MANY, 0u, np, np);
    if (total) *total = np;
    if (rep) { rep->why = XLAT12_HL_OK; rep->passes = np; }
    return np;
}

uint32_t xlat12_ib_headless_passes(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total,
                                   xlat12_hl_report *rep)
{
    return xlat12_ib_headless_passes_m(in, n, seg, max, total, rep, 0u);
}
