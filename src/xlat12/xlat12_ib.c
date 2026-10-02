/* xlat12_ib - IB-level driver around xlat12_translate. Contract: xlat12_ib.h. */
#include "xlat12_ib.h"
#include "tests/fixture_r30_blit_ib.h"   /* Apple's real blit IB, the self-test control */
#include "tests/fixture_r44_drawinj.h"   /* Apple's real drawinj render stream, the draw control */
/* 0.0.224: our programs by exact code identity (tools/gfx-shader-ids.py).
 *: XLAT12_SHADER_IDS_HEADER is an OFFLINE-ONLY compile-time switch. Undefined - the kext build
 * and every existing test - it is exactly the shipped table below. tools/m4-xlat/replay.c compiles a SECOND copy of this
 * file with it naming xlat12_shader_ids_candidates.h (the shipped rows followed by rows not yet reviewed), so a candidate
 * row can be judged by the kext's own matcher over captured frames without the kext ever holding it. */
#ifdef XLAT12_SHADER_IDS_HEADER
#include XLAT12_SHADER_IDS_HEADER
#else
#include "xlat12_shader_ids.h"
#endif
#include "xlat12_desc.h"                   /* 0.0.232: gfx10.3 -> gfx12 image and sampler descriptors (tools/gen-desc-xlat.py) */
#include "xlat12_shader_desc.h"            /*: which identities read a client descriptor (tools/gfx-shader-desc.py) */
#include "xlat12_readset.h"                /* D4' (notes/design/D4-PRIME.md item 2): GENERATED read-set rows, tools/gfx-readset.py */
#include "xlat12_scissor.h"                /* build 0.0.499: gfx12 scissor BR is INCLUSIVE - d_scissor */

#define PT(h)   (((h) >> 30) & 3u)
#define PC(h)   (((h) >> 16) & 0x3FFFu)
#define PO(h)   (((h) >> 8) & 0xFFu)
#define PF(h)   ((h) & 0xFFu)

/* opcodes: Mesa src/amd/common/sid.h @4519cc56 */
enum {
    OP_NOP = 0x10, OP_SET_BASE = 0x11, OP_CLEAR_STATE = 0x12, OP_INDEX_BUFFER_SIZE = 0x13,
    OP_DISPATCH_DIRECT = 0x15,
    OP_DISPATCH_INDIRECT = 0x16, OP_COND_EXEC = 0x22, OP_DRAW_INDIRECT = 0x24,
    OP_DRAW_INDEX_INDIRECT = 0x25, OP_DRAW_INDEX_2 = 0x27, OP_CONTEXT_CONTROL = 0x28,
    OP_INDEX_BASE = 0x26, OP_INDEX_TYPE = 0x2A, OP_DRAW_INDEX_AUTO = 0x2D, OP_NUM_INSTANCES = 0x2F,
    OP_DRAW_INDEX_OFFSET_2 = 0x35, OP_WRITE_DATA = 0x37, OP_WAIT_REG_MEM = 0x3C,
    OP_INDIRECT_BUFFER = 0x3F, OP_COPY_DATA = 0x40, OP_PFP_SYNC_ME = 0x42, OP_EVENT_WRITE = 0x46,
    OP_EVENT_WRITE_EOP = 0x47, OP_RELEASE_MEM = 0x49, OP_DMA_DATA = 0x50, OP_ACQUIRE_MEM = 0x58,
    OP_LOAD_UCONFIG_REG = 0x5E, OP_LOAD_SH_REG = 0x5F, OP_LOAD_CONFIG_REG = 0x60,
    OP_LOAD_CONTEXT_REG = 0x61, OP_LOAD_SH_REG_INDEX = 0x63, OP_SET_CONFIG_REG = 0x68,
    OP_SET_CONTEXT_REG = 0x69, OP_SET_SH_REG = 0x76, OP_SET_UCONFIG_REG = 0x79,
    OP_SET_UCONFIG_REG_INDEX = 0x7A, OP_SET_SH_REG_INDEX = 0x9B, OP_LOAD_CONTEXT_REG_INDEX = 0x9F
};

static int is_memloaded(uint32_t op)
{
    return op == OP_CLEAR_STATE || op == OP_LOAD_UCONFIG_REG || op == OP_LOAD_SH_REG ||
           op == OP_LOAD_CONFIG_REG || op == OP_LOAD_CONTEXT_REG || op == OP_LOAD_SH_REG_INDEX ||
           op == OP_LOAD_CONTEXT_REG_INDEX;
}

static int is_set(uint32_t op)
{
    return op == OP_SET_CONTEXT_REG || op == OP_SET_SH_REG || op == OP_SET_UCONFIG_REG ||
           op == OP_SET_SH_REG_INDEX || op == OP_SET_UCONFIG_REG_INDEX;
}

static int is_draw(uint32_t op)
{
    return op == OP_DRAW_INDEX_AUTO || op == OP_DRAW_INDEX_2 || op == OP_DRAW_INDEX_OFFSET_2 ||
           op == OP_DRAW_INDIRECT || op == OP_DRAW_INDEX_INDIRECT;
}

/* Packets copied verbatim by translate (proven in the blit IB, or draw bookkeeping). */
static int is_passthrough(uint32_t op)
{
    return op == OP_RELEASE_MEM || op == OP_ACQUIRE_MEM || op == OP_EVENT_WRITE ||
           op == OP_EVENT_WRITE_EOP || op == OP_DISPATCH_DIRECT || op == OP_DRAW_INDEX_AUTO ||
           op == OP_DRAW_INDEX_2 || op == OP_DRAW_INDEX_OFFSET_2 || op == OP_NUM_INSTANCES ||
           op == OP_INDEX_TYPE || op == OP_PFP_SYNC_ME || op == OP_SET_BASE ||
           /* 0.0.307: INDEX_BASE and INDEX_BUFFER_SIZE. These are the "four unlisted opcodes per frame"
            * the census counted - 20 of each over 12 real SecurityAgent IBs, about two of each per frame. Neither carries
            * anything register-mapped: INDEX_BASE's body is the index buffer's CLIENT VA (addr_lo, addr_hi, observed
            * `00001000 00000004` = 0x400001000) and INDEX_BUFFER_SIZE's is one dword of index count (observed 0x204).
            * Both exist unchanged on gfx12 - RADV emits them with no generation guard at all
            * (re/graphics/src/mesa/src/amd/vulkan/radv_cmd_buffer.c:6259 and :6263) - and DRAW_INDEX_2, which is ALREADY
            * pass-through here, is meaningless without them. Excluding them was the same family of oversight as the
            * user-data windows stopping at 11: an allow-list that never met a real client frame. */
           op == OP_INDEX_BASE || op == OP_INDEX_BUFFER_SIZE;
}

/* Compute SH register offsets (from the 0x2c00 PM4 base) Apple's blit IB programs and r38/r39
 * executed untranslated on gfx12 (r30 vmib dump). */
static int cs_offset_proven(uint32_t off)
{
    return (off >= 0x204u && off <= 0x209u) || off == 0x20cu || off == 0x20du || off == 0x212u ||
           off == 0x213u || (off >= 0x215u && off <= 0x21au) || (off >= 0x222u && off <= 0x228u) ||
           off == 0x22au || (off >= 0x240u && off <= 0x247u);
}

/* Packet length in dwords at in[i] (0xFFFF1000 and TYPE2 are one dword), or 0 if not a packet. */
static uint32_t plen_at(const uint32_t *in, uint32_t i, uint32_t n)
{
    const uint32_t h = in[i];
    if (h == XLAT12_IB_NOP) return 1u;
    if (PT(h) == 2u) return 1u;
    if (PT(h) != 3u) return 0u;
    const uint32_t len = PC(h) + 2u;
    return ((uint64_t)i + len <= (uint64_t)n) ? len : 0u;
}

static int reg_identical(uint32_t dword_index)
{
    uint32_t g12 = 0, cls = 0;
    if (dword_index > 0x3FFFFFFFu) return 0;
    return xlat12_lookup(dword_index << 2, &g12, &cls) && cls == XLAT12_CLS_IDENTICAL;
}

/* 1 when a compute-typed SET_SH_REG / SET_SH_REG_INDEX covers only proven offsets. */
static int cs_set_proven(const uint32_t *p, uint32_t len)
{
    const uint32_t h = p[0], op = PO(h);
    if (!(PF(h) & 2u) || (op != OP_SET_SH_REG && op != OP_SET_SH_REG_INDEX) || len < 3u) return 0;
    const uint32_t off = p[1] & 0xFFFFu;
    if (op == OP_SET_SH_REG_INDEX && (((p[1] >> 28) & 0xFu) != 3u || (p[1] & 0x0FFF0000u))) return 0;
    for (uint32_t k = 0; k < len - 2u; k++)
        if (!cs_offset_proven(off + k)) return 0;
    return 1;
}

static int ctxctl_is_proven(const uint32_t *p, uint32_t len)
{
    return len == 3u && p[0] == XLAT12_IB_CTXCTL_HDR && p[1] == XLAT12_IB_CTXCTL_DW1 &&
           p[2] == XLAT12_IB_CTXCTL_DW2;
}

/* Register-operand check for WRITE_DATA / WAIT_REG_MEM / COPY_DATA: 1 = acceptable as is. */
static int operand_ok(const uint32_t *p, uint32_t len)
{
    const uint32_t op = PO(p[0]);
    if (op == OP_WRITE_DATA) {
        if (len < 4u) return 0;
        if (((p[1] >> 8) & 0xFu) != 0u) return 1;               /* memory destination */
        return reg_identical(p[2]);
    }
    if (op == OP_WAIT_REG_MEM) {
        if (len < 7u) return 0;
        if (((p[1] >> 4) & 3u) == 1u) return 1;                 /* memory space */
        if (!reg_identical(p[2])) return 0;
        if (((p[1] >> 6) & 3u) == 1u && !reg_identical(p[3])) return 0;   /* wr_wait_wr_reg */
        return 1;
    }
    if (op == OP_COPY_DATA) {
        if (len < 6u) return 0;
        if ((p[1] & 0xFu) == 0u && !reg_identical(p[2])) return 0;
        if (((p[1] >> 8) & 0xFu) == 0u && !reg_identical(p[4])) return 0;
        return 1;
    }
    return 1;
}

uint32_t xlat12_ib_walk(const uint32_t *in, uint32_t n)
{
    uint32_t i = 0;
    if (!in) return 0;
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) break;
        i += l;
    }
    return i;
}

static void count_set_regs(const uint32_t *p, uint32_t len, xlat12_ib_census *c)
{
    const uint32_t op = PO(p[0]);
    uint32_t base = 0;
    switch (op) {
    case OP_SET_CONTEXT_REG:       base = 0xa000u; break;
    case OP_SET_SH_REG:
    case OP_SET_SH_REG_INDEX:      base = 0x2c00u; break;
    case OP_SET_UCONFIG_REG:
    case OP_SET_UCONFIG_REG_INDEX: base = 0xc000u; break;
    default: return;
    }
    if (cs_set_proven(p, len)) { c->cs_proven_regs += len - 2u; c->regs += len - 2u; return; }
    const uint32_t off = p[1] & 0xFFFFu;
    for (uint32_t k = 0; k + 2u < len; k++) {
        const uint32_t addr = (base + off + k) << 2;
        uint32_t g12 = 0, cls = XLAT12_CLS_UNKNOWN;
        if (!xlat12_lookup(addr, &g12, &cls)) cls = XLAT12_CLS_UNKNOWN;
        if (cls > XLAT12_CLS_MAX) cls = XLAT12_CLS_UNKNOWN;   /* 0.0.389: MAX is REUSED, which has its own bucket */
        c->reg_cls[cls]++;
        c->regs++;
        if (cls == XLAT12_CLS_UNKNOWN && !c->first_unknown_reg) c->first_unknown_reg = addr;
    }
}

void xlat12_ib_census_run(const uint32_t *in, uint32_t n, xlat12_ib_census *c)
{
    if (!c) return;
    for (uint32_t k = 0; k < sizeof *c / sizeof(uint32_t); k++) ((uint32_t *)c)[k] = 0;
    c->first_unlisted_op = 0xFFFFFFFFu;
    c->first_memloaded_op = 0xFFFFFFFFu;
    if (!in) return;
    uint32_t i = 0, lastWasNop = 0;
    c->first_is_ctxctl = (n >= 1u && PT(in[0]) == 3u && PO(in[0]) == OP_CONTEXT_CONTROL);
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) break;
        const uint32_t h = in[i];
        c->packets++;
        if (h == XLAT12_IB_NOP || PT(h) == 2u) {
            lastWasNop = 1;
        } else {
            const uint32_t op = PO(h);
            lastWasNop = (op == OP_NOP);
            if (op == OP_NOP) {
            } else if (op == OP_CONTEXT_CONTROL) {
                if (ctxctl_is_proven(&in[i], l)) c->ctxctl_proven++;
                else { c->ctxctl_other++; if (c->first_memloaded_op == 0xFFFFFFFFu) c->first_memloaded_op = op; }
            } else if (is_memloaded(op)) {
                if (op == OP_CLEAR_STATE) c->clear_state++; else c->load_reg++;
                if (c->first_memloaded_op == 0xFFFFFFFFu) c->first_memloaded_op = op;
            } else if (is_set(op)) {
                if (op == OP_SET_CONTEXT_REG) c->set_ctx++;
                else if (op == OP_SET_UCONFIG_REG) c->set_ucfg++;
                else if (op == OP_SET_SH_REG) { if (PF(h) & 2u) c->set_sh_cs++; else c->set_sh_gfx++; }
                else c->set_index++;
                count_set_regs(&in[i], l, c);
            } else if (op == OP_COND_EXEC) {
                c->cond_exec++;
            } else if (op == OP_INDIRECT_BUFFER) {
                c->nested_ib++;
            } else if (op == OP_DMA_DATA) {
                c->dma_data++;
            } else if (op == OP_WRITE_DATA || op == OP_WAIT_REG_MEM || op == OP_COPY_DATA) {
                if (op == OP_COPY_DATA) c->copy_data++;
                if (!operand_ok(&in[i], l)) c->reg_operand_bad++;
            } else if (is_passthrough(op)) {
                if (is_draw(op)) c->draws++;
                if (op == OP_DISPATCH_DIRECT) c->dispatches++;
            } else {
                if (op == OP_DISPATCH_INDIRECT) c->dispatches++;
                c->unlisted++;
                if (c->first_unlisted_op == 0xFFFFFFFFu) c->first_unlisted_op = op;
            }
        }
        i += l;
        if (i < n && (i % 128u) == 0u && lastWasNop) c->early_nop_128++;
    }
    c->walk_len = i;
    if (i > 0u && (i % 128u) == 0u && lastWasNop) {
        c->ends_on_nop_128 = 1;
        if (c->early_nop_128 && i < n) c->early_nop_128--;   /* the final boundary is not "early" */
    }
}

static int blank_keeps(const uint32_t *p, uint32_t len)
{
    const uint32_t h = p[0];
    if (h == XLAT12_IB_NOP || PT(h) == 2u) return 1;
    const uint32_t op = PO(h);
    if (op == OP_NOP || op == OP_RELEASE_MEM || op == OP_ACQUIRE_MEM || op == OP_EVENT_WRITE) return 1;
    if (op == OP_CONTEXT_CONTROL) return ctxctl_is_proven(p, len);
    if (op == OP_WRITE_DATA) return len >= 4u && ((p[1] >> 8) & 0xFu) != 0u;
    if (op == OP_WAIT_REG_MEM) return len >= 7u && ((p[1] >> 4) & 3u) == 1u;
    return 0;
}

uint32_t xlat12_ib_blank(const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *kept, uint32_t *blanked)
{
    uint32_t i = 0, nk = 0, nb = 0, blankUntil = 0;
    if (kept) *kept = 0;
    if (blanked) *blanked = 0;
    if ((!in || !out) && n) return XLAT12_ERR_ARG;
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) return (PT(in[i]) == 3u) ? XLAT12_ERR_TRUNCATED : XLAT12_ERR_BAD_PACKET;
        int keep = blank_keeps(&in[i], l) && i >= blankUntil;
        if (in[i] != XLAT12_IB_NOP && PT(in[i]) == 3u && PO(in[i]) == OP_COND_EXEC && l == 5u) {
            keep = 0;                                         /* blank the gate and its whole region */
            const uint32_t end = i + l + (in[i + 4u] & 0x3FFFu);
            if (end > blankUntil) blankUntil = end;
        }
        for (uint32_t k = 0; k < l; k++) out[i + k] = keep ? in[i + k] : XLAT12_IB_NOP;
        if (keep) nk++; else nb++;
        i += l;
    }
    if (kept) *kept = nk;
    if (blanked) *blanked = nb;
    return 0;
}

static void acc_stats(xlat12_stats *t, const xlat12_stats *s)
{
    t->packets_in += s->packets_in; t->packets_out += s->packets_out;
    t->set_packets_in += s->set_packets_in; t->set_packets_out += s->set_packets_out;
    t->set_packets_empty += s->set_packets_empty; t->set_runs_split += s->set_runs_split;
    t->regs_in += s->regs_in; t->regs_identical += s->regs_identical; t->regs_moved += s->regs_moved;
    t->regs_repacked += s->regs_repacked; t->regs_dropped_absent += s->regs_dropped_absent;
    t->regs_dropped_legacy_vs += s->regs_dropped_legacy_vs;
}

static uint32_t tr_region(xlat12_ctx *ctx, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t cap,
                          uint32_t *out_len, xlat12_stats *st, uint32_t *err_op, int allowCond)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        const uint32_t h = in[i];
        const uint32_t l = plen_at(in, i, n);
        if (!l) { *err_op = h; return (PT(h) == 3u) ? XLAT12_ERR_TRUNCATED : XLAT12_ERR_BAD_PACKET; }
        const uint32_t op = (h == XLAT12_IB_NOP || PT(h) == 2u) ? OP_NOP : PO(h);
        st->err_in_dword = i;
        if (h == XLAT12_IB_NOP || PT(h) == 2u || op == OP_NOP || is_passthrough(op) ||
            (op == OP_CONTEXT_CONTROL && ctxctl_is_proven(&in[i], l)) ||
            ((op == OP_WRITE_DATA || op == OP_WAIT_REG_MEM || op == OP_COPY_DATA) && operand_ok(&in[i], l)) ||
            (is_set(op) && cs_set_proven(&in[i], l))) {
            if (cap - o < l) { *err_op = op; return XLAT12_ERR_CAPACITY; }
            for (uint32_t k = 0; k < l; k++) out[o + k] = in[i + k];
            o += l; i += l; continue;
        }
        if (op == OP_CONTEXT_CONTROL || is_memloaded(op)) { *err_op = op; return XLAT12_ERR_MEMLOADED; }
        if (op == OP_WRITE_DATA || op == OP_WAIT_REG_MEM || op == OP_COPY_DATA) {
            *err_op = op; st->err_reg_addr = (l > 2u) ? in[i + 2u] << 2 : 0; return XLAT12_IB_ERR_REG_OPERAND;
        }
        if (is_set(op)) {
            xlat12_stats s1; uint32_t m = 0;
            const xlat12_status s = xlat12_translate(ctx, &in[i], l, &out[o], cap - o, &m, &s1);
            acc_stats(st, &s1);
            if (s != XLAT12_OK) {
                *err_op = op; st->err_reg_addr = s1.err_reg_addr; st->err_name = s1.err_name; return s;
            }
            o += m; i += l; continue;
        }
        if (op == OP_COND_EXEC) {
            if (!allowCond || l != 5u) { *err_op = op; return XLAT12_IB_ERR_COND_EXEC; }
            const uint32_t exec = in[i + 4u] & 0x3FFFu;
            if ((uint64_t)i + l + exec > (uint64_t)n || cap - o < 5u) { *err_op = op; return
                (cap - o < 5u) ? XLAT12_ERR_CAPACITY : XLAT12_IB_ERR_COND_EXEC; }
            uint32_t m = 0;
            const uint32_t s = tr_region(ctx, &in[i + l], exec, &out[o + 5u], cap - o - 5u, &m, st, err_op, 0);
            if (s == XLAT12_ERR_TRUNCATED || s == XLAT12_ERR_BAD_PACKET) { *err_op = op; return XLAT12_IB_ERR_COND_EXEC; }
            if (s != 0u) return s;
            if (m > 0x3FFFu) { *err_op = op; return XLAT12_IB_ERR_COND_EXEC; }
            for (uint32_t k = 0; k < 4u; k++) out[o + k] = in[i + k];
            out[o + 4u] = (in[i + 4u] & ~0x3FFFu) | m;       /* rule 50: exec count over the new region */
            o += 5u + m; i += l + exec; continue;
        }
        *err_op = op;
        return XLAT12_IB_ERR_UNLISTED;
    }
    *out_len = o;
    return 0;
}

uint32_t xlat12_ib_translate(xlat12_ctx *ctx, const uint32_t *in, uint32_t n, uint32_t *out,
                             uint32_t cap, uint32_t *out_len, xlat12_stats *stats, uint32_t *err_op)
{
    xlat12_stats local;
    uint32_t eop = 0xFFFFFFFFu, len = 0;
    xlat12_stats *st = stats ? stats : &local;
    for (uint32_t k = 0; k < sizeof *st / sizeof(uint32_t) && k < 17u; k++) ((uint32_t *)st)[k] = 0;
    st->err_name = 0;
    if (out_len) *out_len = 0;
    if (err_op) *err_op = 0xFFFFFFFFu;
    if ((!in && n) || (!out && cap)) return XLAT12_ERR_ARG;
    const uint32_t s = tr_region(ctx, in, n, out, cap, &len, st, &eop, 1);
    if (err_op) *err_op = eop;
    if (s != 0u) return s;
    st->dwords_out = len;
    if (out_len) *out_len = len;
    return 0;
}

uint32_t xlat12_ib_pad(uint32_t *buf, uint32_t len, uint32_t target)
{
    if (len > target) return XLAT12_IB_ERR_TOO_LONG;
    if (!buf && target) return XLAT12_ERR_ARG;
    for (uint32_t i = len; i < target; i++) buf[i] = XLAT12_IB_NOP;
    return 0;
}

const char *xlat12_ib_status_name(uint32_t s)
{
    switch (s) {
    case XLAT12_IB_ERR_UNLISTED:    return "IB_ERR_UNLISTED";
    case XLAT12_IB_ERR_REG_OPERAND: return "IB_ERR_REG_OPERAND";
    case XLAT12_IB_ERR_COND_EXEC:   return "IB_ERR_COND_EXEC";
    case XLAT12_IB_ERR_TOO_LONG:    return "IB_ERR_TOO_LONG";
    case XLAT12_IB_ERR_DRAW_SHAPE:  return "IB_ERR_DRAW_SHAPE";
    case XLAT12_IB_ERR_VERIFY:      return "IB_ERR_VERIFY";
    case XLAT12_IB_ERR_RING:        return "IB_ERR_RING";
    case XLAT12_IB_ERR_PAIR:        return "IB_ERR_PAIR";
    case XLAT12_IB_ERR_INTERP:      return "IB_ERR_INTERP";
    case XLAT12_IB_ERR_DESC:        return "IB_ERR_DESC";
    default: return xlat12_status_name((xlat12_status)s);
    }
}

uint32_t xlat12_ib_selftest(void)
{
    uint32_t bits = 0, aId = 0, aMv = 0, aRp = 0, gId = 0, gMv = 0, gRp = 0;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0; const char *nm = 0;
        if (!xlat12_table_entry(i, &g10, &g12, &cls, &nm) || g10 < 0x28000u || g10 >= 0x30000u) continue;
        if (!aId && cls == XLAT12_CLS_IDENTICAL)    { aId = g10; gId = g12; }
        if (!aMv && cls == XLAT12_CLS_MOVED)        { aMv = g10; gMv = g12; }
        if (!aRp && cls == XLAT12_CLS_FIELD_REPACK) { aRp = g10; gRp = g12; }
    }
    if (aId && aMv && aRp) {
        const uint32_t hdr = 0xC0016900u;          /* PKT3(SET_CONTEXT_REG, count 1) = offset + one value */
        const uint32_t valRp = 0x400c0041u;
        uint32_t in[9], o[64], n = 0;
        in[0] = hdr; in[1] = (aId - 0x28000u) >> 2; in[2] = 0x11111111u;
        in[3] = hdr; in[4] = (aMv - 0x28000u) >> 2; in[5] = 0x22222222u;
        in[6] = hdr; in[7] = (aRp - 0x28000u) >> 2; in[8] = valRp;
        xlat12_ctx ctx; xlat12_stats st;
        ctx.vs_mode = XLAT12_VS_UNKNOWN;
        if (xlat12_translate(&ctx, in, 9, o, 64, &n, &st) == XLAT12_OK) {
            for (uint32_t i = 0; i < n; ) {
                const uint32_t h = o[i];
                if (PT(h) != 3u) break;
                const uint32_t cnt = PC(h);
                if (PO(h) == OP_SET_CONTEXT_REG && i + 1u + cnt < n) {
                    for (uint32_t k = 0; k < cnt; k++) {
                        const uint32_t addr = 0x28000u + ((o[i + 1] + k) << 2), v = o[i + 2 + k];
                        if (addr == gId && v == 0x11111111u) bits |= 1u;
                        if (addr == gMv && v == 0x22222222u) bits |= 2u;
                        if (addr == gRp && v == xlat12_repack_value(aRp, valRp)) bits |= 4u;
                    }
                }
                i += cnt + 2u;
            }
        }
        {
            const uint32_t clr[2] = { 0xC0001200u, 0u };
            if (xlat12_translate(&ctx, clr, 2, o, 64, &n, &st) == XLAT12_ERR_MEMLOADED) bits |= 8u;
        }
    }
    {
        xlat12_ib_census c;
        xlat12_ib_census_run(kR30BlitIb, 128, &c);
        if (c.walk_len == 128u && c.ctxctl_proven == 1u && c.ends_on_nop_128 == 1u && c.cs_proven_regs == 32u &&
            c.dispatches == 1u && c.unlisted == 0u && c.reg_cls[XLAT12_CLS_UNKNOWN] == 0u) bits |= 16u;
    }
    {
        uint32_t b[128], kept = 0, blanked = 0;
        if (xlat12_ib_blank(kR30BlitIb, 128, b, &kept, &blanked) == 0u && kept == 9u &&
            b[0x64] == XLAT12_IB_NOP && b[0] == kR30BlitIb[0]) bits |= 32u;
    }
    {
        uint32_t t2[256], len = 0, eop = 0;
        xlat12_ctx cx; xlat12_stats xs;
        cx.vs_mode = XLAT12_VS_UNKNOWN;
        if (xlat12_ib_translate(&cx, kR30BlitIb, 128, t2, 256, &len, &xs, &eop) == 0u && len == 125u &&
            xlat12_ib_pad(t2, len, 128) == 0u && xlat12_ib_walk(t2, 128) == 128u) bits |= 64u;
    }
    return bits;
}


/* =================================================================================================================
 * The milestone-2 DRAW policy (xlat12_ib.h).
 * ================================================================================================================= */

enum { OP_DRAW_POLICY_NONE = 0 };

/* SET opcode that addresses a gfx12 MMIO byte address, and the packet's dword base. 0 = not a SET-able block. */
static uint32_t d_set_op(uint32_t a)
{
    if (a >= 0x0b000u && a < 0x0c000u) return OP_SET_SH_REG;         /* dword base 0x2c00 */
    if (a >= 0x28000u && a < 0x2c000u) return OP_SET_CONTEXT_REG;    /* dword base 0xa000 */
    if (a >= 0x30000u && a < 0x40000u) return OP_SET_UCONFIG_REG;    /* dword base 0xc000 */
    return 0u;
}
static uint32_t d_base(uint32_t op)
{
    return op == OP_SET_SH_REG || op == OP_SET_SH_REG_INDEX ? 0x2c00u :
           op == OP_SET_CONTEXT_REG ? 0xa000u :
           op == OP_SET_UCONFIG_REG || op == OP_SET_UCONFIG_REG_INDEX ? 0xc000u : 0u;
}

typedef struct {
    uint32_t *out; uint32_t cap, n;
    int open; uint32_t op, flags, hdr, next_off, count;
} DEmit;

static void d_close(DEmit *e)
{
    if (!e->open) return;
    e->out[e->hdr] = (3u << 30) | (e->count << 16) | (e->op << 8) | e->flags;
    e->open = 0;
}

static uint32_t d_add(DEmit *e, uint32_t g12, uint32_t flags, uint32_t val)
{
    const uint32_t op = d_set_op(g12);
    if (!op) return XLAT12_ERR_RANGE;
    const uint32_t off = (g12 >> 2) - d_base(op);
    if (!(e->open && e->op == op && e->flags == flags && e->next_off == off)) {
        d_close(e);
        if (e->cap - e->n < 3u) return XLAT12_ERR_CAPACITY;
        e->hdr = e->n; e->out[e->n++] = 0u; e->out[e->n++] = off;
        e->open = 1; e->op = op; e->flags = flags; e->count = 0;
    } else if (e->cap - e->n < 1u) {
        return XLAT12_ERR_CAPACITY;
    }
    e->out[e->n++] = val;
    e->count++;
    e->next_off = off + 1u;
    return 0;
}

static uint32_t d_add_index(DEmit *e, uint32_t op_index, uint32_t idx, uint32_t g12, uint32_t flags, uint32_t val)
{
    const uint32_t want = op_index == OP_SET_SH_REG_INDEX ? OP_SET_SH_REG : OP_SET_UCONFIG_REG;
    if (d_set_op(g12) != want) return XLAT12_ERR_RANGE;
    d_close(e);
    if (e->cap - e->n < 3u) return XLAT12_ERR_CAPACITY;
    e->out[e->n++] = (3u << 30) | (1u << 16) | (op_index << 8) | flags;
    e->out[e->n++] = (idx << 28) | ((g12 >> 2) - d_base(op_index));
    e->out[e->n++] = val;
    return 0;
}

/* The zero-valued writes r44/r45 measured at addresses neither gfx10.3 source names (xlat12 class UNKNOWN). */
/* build 0.0.502: 0x28060 STAYS HERE, deliberately. gfx11.5 names that address DB_SPI_VRS_CENTER_LOCATION
 * (mesa gfx115.json, gc_11_5_0_offset.h:4003), but NO gfx10.3 source does (gfx103.json, gc_10_3_0_offset.h, mesa's gfx10.3
 * CLEAR_STATE list all leave it unnamed), so it is not re-classed as MOVED. Its gfx12 counterpart is instead written 0 once
 * by every translation with a ring (d_vrs_prefollow, else d_rings; XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION); a non-zero
 * 0x28060 still refuses here. */
static int d_unknown_zero_listed(uint32_t g10)
{
    static const uint32_t k[] = { 0x28060u, 0x283c0u, 0x283c4u, 0x283c8u, 0x286dcu, 0x3098cu, 0x30af0u, 0x30af4u,
                                  0x30b10u, 0x30b14u, 0x30b18u, 0x30b1cu, 0x30b30u, 0x30b34u, 0x30b38u, 0x30b3cu,
                                  0x30b90u, 0x30b94u, 0x30ba0u, 0x30ba4u,
                                  /* 0.0.306: the 24 further addresses a 12-frame census of REAL SecurityAgent
                                   * IBs found written, always with zero, and which neither source places. Named from Mesa's
                                   * own database where it names them at all:
                                   *   0x0b114 SPI_SHADER_PGM_CHKSUM_VS, 0x0b1c0 SPI_SHADER_REQ_CTRL_VS,
                                   *   0x0b1c4-0x0b1d4 SPI_SHADER_USER_ACCUM_VS_0..3   (gfx12 has no VS stage at all)
                                   *   0x0b888 COMPUTE_REQ_CTRL, 0x0b890-0x0b89c COMPUTE_USER_ACCUM_0..3,
                                   *   0x0b8a8 COMPUTE_SHADER_CHKSUM
                                   * and two where gfx12 has REUSED the address for something else, so dropping the zero is
                                   * not merely tidy, it is what stops us writing a gfx12 register Apple never addressed:
                                   *   0x0b0c4 unnamed on gfx10.3 -> gfx12 SPI_SHADER_GS_OUT_CONFIG_PS (d_synth emits ours)
                                   *   0x0b88c unnamed on gfx10.3 -> gfx12 COMPUTE_STATIC_THREAD_MGMT_SE8
                                   * A non-zero write to any of them still REFUSES, which is the safe direction. */
                                  0x0b0c4u, 0x0b114u, 0x0b1c0u, 0x0b1c4u, 0x0b1c8u, 0x0b1ccu, 0x0b1d0u, 0x0b1d4u,
                                  0x0b888u, 0x0b88cu, 0x0b890u, 0x0b894u, 0x0b898u, 0x0b89cu, 0x0b8a8u,
                                  0x28018u, 0x28058u, 0x2805cu, 0x283a0u, 0x283a4u, 0x283a8u, 0x283acu, 0x283b0u,
                                  0x28834u };
    for (uint32_t i = 0; i < sizeof k / sizeof k[0]; i++) if (k[i] == g10) return 1;
    return 0;
}

/* 0.0.306 — REGISTERS WE DROP ON PURPOSE, because the gfx12 part does not have the gfx10.3 register at that
 * address. This is NOT the `absent` class (gfx12 has no such register anywhere): here the ADDRESS still exists on gfx12 and
 * means SOMETHING ELSE, or means nothing. Passing the value through at the same address - which is exactly what `identical`
 * does for most of a frame - would write Apple's gfx10.3 value into a gfx12 register Apple never intended to touch.
 * Each entry is CONFIRMED from Mesa's own register databases, re/graphics/src/mesa/src/amd/registers/{gfx103,gfx12}.json:
 *
 *   0x2803c  gfx10.3 DB_RESERVED_REG_2  ->  gfx12 DB_STENCIL_WRITE_BASE_HI
 *            A 12-frame census of real SecurityAgent IBs found it written 10 times with 0x10000000. Reserved on gfx10.3,
 *            so the value means nothing; on gfx12 it is the HIGH BITS OF THE STENCIL WRITE BASE. Writing it through would
 *            corrupt the depth/stencil target of every frame, and would present as a mysterious rendering or hang bug
 *            nowhere near its cause. ***DO NOT "RESTORE THIS AS A HARMLESS RESERVED REGISTER".***
 * It holds ONE address, and that is the whole finding. The census also flagged 0x0b004 (RSRC4_PS) and 0x0b404 (RSRC4_HS)
 * as untranslatable, and they ARE - gfx12 names nothing at either address (RSRC4_PS moved to 0x0b01c, which on gfx10.3 was
 * RSRC3_PS) - but `d_reg`'s own switch has dropped both since 0.0.217 (`case 0xb004u: case 0xb404u:` -> dropped_legacy).
 * They only looked open because the census tool classifies with the TABLE alone. Adding them here would be dead code and
 * would double-count, so it is not done; the test asserts the existing path drops them.
 */
static int d_drop_reused_addr(uint32_t g10)
{
    return g10 == 0x2803cu;
}

/* build 0.0.451 item 8 (reviewer-confirmed via decide43's hardware capture): every *_SW_MODE field the
 * GENERATED repack functions (xlat12_repack.h, src/xlat12/gen/gen_tables.py) touch, remapped BY NAME through
 * kXlat12SwModeG10ToG12 (xlat12_desc.h) - the SAME table the T#/S# descriptor path already uses - instead of the
 * generator's plain per-field bit copy. CONFIRMED the generator has NO enum awareness anywhere (src/xlat12/gen/
 * gen_tables.py has no reference to SW_MODE, FORMAT or any enum name at all: every field, everywhere, is copied by
 * bit position and width alone), so a register field literally named SW_MODE/COLOR_SW_MODE is copied exactly like
 * a plain counter or offset would be - correct for those, wrong for an enum whose gfx12 numbering differs from
 * gfx10's (mode 22 ADDR_SW_4KB_D_X must become 2 ADDR3_4KB_2D, not the low 3 bits of 22, which is 6). THE FIX
 * belongs here, in the hand-written per-register override switch in d_reg (below) - the file's own established
 * rule for exactly this situation: every other register needing logic beyond a straight field copy (0xb028's
 * ps_rsrc1, 0x28c44's binner_cntl0, ...) is already special-cased in that SAME switch, never by editing the
 * generator (which has no way to know which fields are enums) or by hand-editing xlat12_repack.h (marked
 * GENERATED - do not edit). `naive12` is what xlat12_repack_value already computed for every OTHER field in the
 * register (trusted unchanged - only SW_MODE itself needs renaming, and every other field's width/position was
 * already correct); `inLo` is the gfx10 SW_MODE field's own bit position (5 bits wide on every register that has
 * one); `outLo`/`outW` are the gfx12 field's. Returns 0 (the caller refuses XLAT12_IB_ERR_SWMODE) when the gfx10
 * mode has no gfx12 counterpart - the SAME refusal-direction choice xlat12_desc.h already makes. */
static int d_swmode_repack(uint32_t v10, uint32_t naive12, uint32_t inLo, uint32_t outLo, uint32_t outW, uint32_t *out12)
{
    const uint32_t g10Mode = (v10 >> inLo) & 0x1Fu;
    uint32_t g12Mode = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < XLAT12_SWMODE_MAP_COUNT; i++)
        if (kXlat12SwModeG10ToG12[i][0] == g10Mode) { g12Mode = kXlat12SwModeG10ToG12[i][1]; break; }
    if (g12Mode == 0xFFFFFFFFu) return 0;
    const uint32_t outMask = ((1u << outW) - 1u) << outLo;
    *out12 = (naive12 & ~outMask) | ((g12Mode << outLo) & outMask);
    return 1;
}

/* build 0.0.499: THE SCISSOR BOTTOM-RIGHT RULE, applied to the gfx12 value d_reg's table row
 * has just produced (the repack for PA_SC_WINDOW/GENERIC/VPORT_n_SCISSOR_BR, the unchanged MOVED value for
 * PA_SC_SCREEN_SCISSOR_BR). gfx10.3's BR is exclusive and gfx12's INCLUSIVE (xlat12_scissor.h, with the mesa quotes),
 * so BR_X/BR_Y -1; a BR component of 0 REFUSES XLAT12_ERR_SCISSOR (fail-closed). Any other register: 0, unchanged.
 * Through 0.0.498 the value was copied unchanged and a translated draw could write one column/row beyond its colour
 * target - in RUN I into WindowServer's sampler heap. */
static uint32_t d_scissor(xlat12_draw_stats *ds, uint32_t g10, uint32_t *v12, int *emit)
{
    uint32_t s12 = *v12;
    const uint32_t sr = xlat12_scissor_br_g12(g10, *v12, &s12);
    if (sr == XLAT12_SCISSOR_EMPTY) { *emit = 0; ds->err_reg = g10; ds->scissor_refused++; return XLAT12_ERR_SCISSOR; }
    if (sr == XLAT12_SCISSOR_ADJUSTED) { *v12 = s12; ds->scissor_adjusted++; }
    return 0;
}

/* One gfx10 register write under the draw policy. *emit = 1 with (*g12, *v12) to write; *stages = 1 when it was
 * VGT_SHADER_STAGES_EN (the caller then emits the synthesized NGG block). */
static uint32_t d_reg(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, xlat12_draw_stats *ds, uint32_t g10,
                      uint32_t val, uint32_t *g12, uint32_t *v12, int *emit, int *stages)
{
    *emit = 0; *stages = 0; *g12 = 0; *v12 = val;
    ds->regs_in++;
    /* legacy-VS slot -> gfx12 NGG slot (radeonsi gfx12: si_state_shaders.cpp:1142 PGM_LO_ES 0xb224, :1147/:1153
     * RSRC1/2_GS; si_descriptors.c:2174 USER_DATA_GS_0 0xb230) */
    switch (g10) {
    /* 0.0.265: with a relocated vertex program these slots are OVERRIDDEN to name our own mapping, not re-homed.
     * The kext reconstructs the VA as ((hi & 0xFF) << 40) | (lo << 8), so lo = va >> 8 and hi = va >> 40. */
    /* 0.0.311: the PER-PROGRAM address the resolver gave for the vertex stage bound right now wins over the
     * segment-wide one, because one segment can bind several vertex programs and only some of them are relocated. */
    case 0xb120u: { const uint64_t r = pf->vs_pgm_va ? pf->vs_pgm_va : (ex ? ex->vs_pgm_va : 0ull);
        *g12 = 0xb224u;                                                                     /* PGM_LO_VS -> PGM_LO_ES */
        if (r) { *v12 = (uint32_t)(r >> 8); ds->vs_pgm_lo = *v12; ds->overridden++; }
        else { ds->vs_pgm_lo = val; ds->rehomed++; }
        *emit = 1; return 0; }
    case 0xb124u: { const uint64_t r = pf->vs_pgm_va ? pf->vs_pgm_va : (ex ? ex->vs_pgm_va : 0ull);
        *g12 = 0xb218u;                                                                     /* PGM_HI_VS -> PGM_HI_ES */
        if (r) { *v12 = (uint32_t)(r >> 40); ds->vs_pgm_hi = *v12; ds->overridden++; }
        else { ds->vs_pgm_hi = val; ds->rehomed++; }
        *emit = 1; return 0; }
    case 0xb128u: *g12 = 0xb228u; *v12 = pf->vs_rsrc1_gs; ds->rehomed++; *emit = 1; return 0; /* RSRC1_VS -> RSRC1_GS */
    case 0xb12cu: {                                                                            /* RSRC2_VS -> RSRC2_GS */
        const uint32_t usg = (val >> 1) & 0x1Fu, msb = val & (1u << 27);
        uint32_t u = usg | (msb ? 32u : 0u);
        ds->vs_user_sgpr = u;   /* Apple's, kept: USER_DATA_GS_0..u-1 -> s8.. behind the merged stage's system SGPRs (0.0.219,) */
        *g12 = 0xb22cu; *v12 = pf->vs_rsrc2_gs | ((u & 0x1Fu) << 1) | ((u & 0x20u) ? (1u << 27) : 0u);
        ds->rehomed++; *emit = 1; return 0; }
    /* RSRC4_VS / RSRC3_VS / LATE_ALLOC_VS: gfx10 VS-slot CU_EN / late-alloc words, dropped. NOT "replaced by RSRC4_GS" (the
     * 0.0.213 comment, review report 8): gfx12 RSRC4_GS has no CU field; the GS/ES CU mask is RSRC3_GS 0xb21c (gc_12_0_0_sh_mask.h
     * :26734-26735), written by d_rings when the caller asks (0.0.217, review report 9). */
    case 0xb104u: case 0xb118u: case 0xb11cu:
    case 0xb004u: case 0xb404u:                 /* gfx10 RSRC4_PS / RSRC4_HS CU_EN: no gfx12 field; RSRC4_PS synthesized */
        ds->dropped_legacy++; return 0;
    case 0xb028u: *g12 = 0xb028u; *v12 = pf->ps_rsrc1; ds->overridden++; *emit = 1; return 0;
    case 0xb02cu: {
        const uint32_t usg = (val >> 1) & 0x1Fu, msb = val & (1u << 27);
        *g12 = 0xb02cu; *v12 = pf->ps_rsrc2 | (usg << 1) | msb;
        ds->ps_user_sgpr = usg | (msb ? 32u : 0u); ds->overridden++; *emit = 1; return 0; }
    case 0x286ccu: *g12 = 0x2865cu; *v12 = pf->ps_input_ena;  ds->overridden++; *emit = 1; return 0;
    case 0x286d0u: *g12 = 0x28660u; *v12 = pf->ps_input_addr; ds->overridden++; *emit = 1; return 0;
    case 0x3096cu: *g12 = 0x3096cu; *v12 = pf->ge_cntl;       ds->overridden++; *emit = 1; return 0;
    /* 0.0.214: Apple's gfx10 binning and NGG subgroup words replaced by radeonsi's gfx12 values for a VS draw
     * (si_state_binning.c:396-406 gfx12 disable path; si_state_shaders.cpp:1189 PRIM_AMP_FACTOR(1)) */
    case 0x28c44u: *g12 = 0x28c44u; *v12 = pf->binner_cntl0;    ds->overridden++; *emit = 1; return 0;
    case 0x28b4cu: *g12 = 0x28b4cu; *v12 = pf->ngg_subgrp_cntl; ds->overridden++; *emit = 1; return 0;
    /* 0.0.215 (review report 8,): Apple's legacy-VS VGT_GS_OUT_PRIM_TYPE (0 POINTLIST, r44 [032d]) is not what
     * gfx12 NGG rasterizes a triangle draw with; radeonsi writes uconfig 0x30998 for every NGG draw, TRISTRIP (2) for
     * triangles (si_state_draw.cpp:1084-1087; gfx12.json enum VGT_GS_OUTPRIM_TYPE). */
    case 0x28a6cu: *g12 = 0x30998u;   /* 0.0.224: XLAT12_EXTRA_APPLE_OUTPRIM keeps Apple's value (ablation) */
        *v12 = (ex && (ex->flags & XLAT12_EXTRA_APPLE_OUTPRIM)) ? val : pf->gs_out_prim_type; ds->overridden++; *emit = 1; return 0;
    /* 0.0.216: SPI_SHADER_IDX_FORMAT (gfx10 0x28708 -> gfx12 0x28648). Apple's legacy VS writes 0 = NONE (r48
     * [2a5], [333]); every NGG draw needs IDX0_EXPORT_FORMAT 1COMP for the SQ_EXP_PRIM (20) export (radv_shader.c:1917,
     * radeonsi gfx12 preamble si_state.c:5079). */
    case 0x28708u: *g12 = 0x28648u;   /* 0.0.224: XLAT12_EXTRA_APPLE_IDXFMT keeps Apple's value (ablation) */
        *v12 = (ex && (ex->flags & XLAT12_EXTRA_APPLE_IDXFMT)) ? val : pf->spi_shader_idx_format; ds->overridden++; *emit = 1; return 0;
    case 0x286c4u: ds->dropped_absent++; return 0;   /* SPI_VS_OUT_CONFIG: gfx12 carries it in SPI_SHADER_GS_OUT_CONFIG_PS */
    case 0x28b54u:                                   /* VGT_SHADER_STAGES_EN: only a plain VS pipeline is translated */
        if ((val & 0x3FFFu) != 0u) { ds->err_reg = g10; return XLAT12_ERR_LEGACY_VS; }
        *g12 = 0x28a98u; *v12 = pf->vs_stages_en; ds->overridden++; *emit = 1; *stages = 1; return 0;
    /* build 0.0.451 item 8: CB_COLORn_ATTRIB3's COLOR_SW_MODE [18:14] (5 bits) -> gfx12 [17:15] (3 bits) -
     * confirmed via decide43's capture (630 draws into the Family A blur surfaces 0x400034000/0x4000ac000, mode
     * 22 ADDR_SW_4KB_D_X, landed on gfx12 raw value 6 instead of 2 ADDR3_4KB_2D). xlat12_tables.h's own
     * gfx10->gfx12 address pairs, unchanged - only the VALUE construction differs from the generic FIELD_REPACK
     * path above (every field of this register OTHER than SW_MODE keeps xlat12_repack_value's own, already-
     * correct, field-by-field copy). */
    case 0x28ee0u: case 0x28ee4u: case 0x28ee8u: case 0x28eecu:
    case 0x28ef0u: case 0x28ef4u: case 0x28ef8u: case 0x28efcu: {
        static const uint32_t kAttrib3G10[8] = { 0x28ee0u, 0x28ee4u, 0x28ee8u, 0x28eecu, 0x28ef0u, 0x28ef4u, 0x28ef8u, 0x28efcu };
        static const uint32_t kAttrib3G12[8] = { 0x28c7cu, 0x28ca0u, 0x28cc4u, 0x28ce8u, 0x28d0cu, 0x28d30u, 0x28d54u, 0x28d78u };
        uint32_t idx = 0; for (uint32_t z = 1; z < 8u; z++) if (kAttrib3G10[z] == g10) idx = z;
        const uint32_t naive = xlat12_repack_value(g10, val);
        uint32_t fixed = 0;
        if (!d_swmode_repack(val, naive, 14u, 15u, 3u, &fixed)) { ds->err_reg = g10; ds->err_op = (val >> 14) & 0x1Fu; return XLAT12_IB_ERR_SWMODE; }
        *g12 = kAttrib3G12[idx]; *v12 = fixed; ds->overridden++; *emit = 1; return 0; }
    /* DB_Z_INFO / DB_STENCIL_INFO's SW_MODE [8:4] (5 bits) -> gfx12 [8:4] (5 bits, width UNCHANGED - the SAME
     * naive-copy bug as CB_COLORn_ATTRIB3, without a truncation artifact: gfx10's raw enum number was passing
     * straight through unremapped, e.g. mode 22 stayed 22 instead of becoming 2). SUSPECTED, not independently
     * measured on hardware the way CB_COLORn_ATTRIB3's was (decide43 did not capture a depth/stencil-bound draw
     * into a 4KB_D_X target) - fixed on the SAME structural grounds: the generator has no enum awareness for THIS
     * field either, and repack_DB_Z_INFO/repack_DB_STENCIL_INFO (xlat12_repack.h) copy it exactly like every other
     * plain bitfield. */
    case 0x28040u: case 0x28044u: {
        const uint32_t naive = xlat12_repack_value(g10, val);
        uint32_t fixed = 0;
        if (!d_swmode_repack(val, naive, 4u, 4u, 5u, &fixed)) { ds->err_reg = g10; ds->err_op = (val >> 4) & 0x1Fu; return XLAT12_IB_ERR_SWMODE; }
        *g12 = (g10 == 0x28040u) ? 0x28018u : 0x2801cu; *v12 = fixed; ds->overridden++; *emit = 1; return 0; }
    default: break;
    }
    if (d_drop_reused_addr(g10)) { ds->dropped_reused++; return 0; }   /* 0.0.306: see d_drop_reused_addr */
    /* 0.0.306: the window is 0..31, not 0..11. Every graphics stage has THIRTY-TWO user-data registers
     * (gfx10.3 and gfx12 both name SPI_SHADER_USER_DATA_*_31), and a 12-frame census of real SecurityAgent IBs found Apple
     * writing 0xffffffff across the whole window - a reset of the SGPR slots - which made 64 registers look untranslatable
     * when the only thing missing was the bound. GS_0..31 is 0xb230..0xb2ac on BOTH parts, so the +0x100 re-home holds for
     * the upper half exactly as for the lower. */
    if (g10 >= 0xb130u && g10 <= 0xb1acu) {           /* USER_DATA_VS_0..31 -> USER_DATA_GS_0..31 */
        *g12 = g10 + 0x100u; ds->rehomed++; *emit = 1; return 0;
    }
    if (g10 >= 0xb430u && g10 <= 0xb4acu) {           /* USER_DATA_HS_0..31: identical address on gfx12 */
        *g12 = g10; *emit = 1; return 0;
    }
    if (g10 >= 0xb900u && g10 <= 0xb93cu) {           /* COMPUTE_USER_DATA_0..15: identical address on gfx12 */
        *g12 = g10; *emit = 1; return 0;
    }
    /* 0.0.227: SPI_SHADER_USER_DATA_PS_0..11 are the same registers on both parts - gfx10.3 regSPI_SHADER_USER_DATA_PS_0
     * 0x2c0c (gc_10_3_0_offset.h) and gfx12 0x19ac + GC base 0x1260 = 0x2c0c (gc_12_0_0_offset.h:7717-7736), i.e. bytes 0xb030 + 4k.
     * The table names only _0, _8 and _9, so a fragment stage with a descriptor table and two resource indices (SkyLight's
     * SimpleTextureFragment: PS_0/1 the class-19 table, PS_4 the texture index, PS_6 the sampler index) refused at _1 (census1). */
    if (g10 >= 0xb030u && g10 <= 0xb0acu) {   /* 0.0.306: PS_0..31, was PS_0..11 */
        *g12 = g10; *emit = 1; return 0;
    }
    if (g10 == 0xb020u) ds->ps_pgm_lo = val;
    if (g10 == 0xb024u) ds->ps_pgm_hi = val;
    if (d_unknown_zero_listed(g10)) {
        if (val != 0u) { ds->err_reg = g10; return XLAT12_ERR_UNKNOWN_REG; }
        ds->dropped_unknown_zero++; return 0;
    }
    uint32_t t12 = 0, cls = XLAT12_CLS_UNKNOWN;
    if (!xlat12_lookup(g10, &t12, &cls)) { ds->err_reg = g10; return XLAT12_ERR_UNKNOWN_REG; }
    switch (cls) {
    /* build 0.0.499: both classes a scissor BR row has (MOVED screen, FIELD_REPACK window/generic/viewport) pass
     * through d_scissor, which leaves every non-scissor register untouched. */
    case XLAT12_CLS_IDENTICAL: case XLAT12_CLS_MOVED:
        *g12 = t12; *emit = 1; return d_scissor(ds, g10, v12, emit);
    case XLAT12_CLS_FIELD_REPACK:
        *g12 = t12; *v12 = xlat12_repack_value(g10, val); *emit = 1; return d_scissor(ds, g10, v12, emit);
    /* 0.0.389 (notes 880 H6, 881): a gfx10.3 address gfx12 REUSES for a different register - xlat12_reused.h.
     * Dropping it (the ABSENT behaviour through 0.0.388) left that gfx12 register holding whatever its context
     * slot last held, and gfx12 has no CLEAR_STATE to have reset it. */
    case XLAT12_CLS_REUSED:
        *g12 = t12; *v12 = xlat12_repack_value(g10, val); *emit = 1;
        ds->reused_regs++;
        if (val != 0u) ds->reused_nonzero++;
        return 0;
    case XLAT12_CLS_ABSENT:
        ds->dropped_absent++; return 0;
    default:
        ds->err_reg = g10; return cls == XLAT12_CLS_LEGACY_VS ? XLAT12_ERR_VS_MODE_UNKNOWN : XLAT12_ERR_UNKNOWN_REG;
    }
}

/* The NGG state a legacy-VS stream never programs (radeonsi gfx12, si_state_shaders.cpp:1229-1235 and :1288-1291). */
static uint32_t d_synth(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, DEmit *e, xlat12_draw_stats *ds)
{
    uint32_t s;
    d_close(e);
    if ((s = d_add(e, 0x287fcu, 0u, pf->ge_max_output)) != 0u) return s;      /* GE_MAX_OUTPUT_PER_SUBGROUP */
    d_close(e);
    if ((s = d_add(e, 0xb01cu, 0u, pf->rsrc4_ps)) != 0u) return s;           /* SPI_SHADER_PGM_RSRC4_PS (gfx12) */
    d_close(e);
    if ((s = d_add(e, 0xb0c4u, 0u, pf->gs_out_config_ps)) != 0u) return s;   /* SPI_SHADER_GS_OUT_CONFIG_PS */
    d_close(e);
    if ((s = d_add(e, 0xb220u, 0u, pf->rsrc4_gs)) != 0u) return s;           /* SPI_SHADER_PGM_RSRC4_GS (gfx12) */
    d_close(e);
    if ((s = d_add(e, 0x3096cu, 0u, pf->ge_cntl)) != 0u) return s;           /* GE_CNTL */
    d_close(e);
    /* 0.0.214: radeonsi gfx12_emit_shader_ngg also writes these for a VS (si_state_shaders.cpp:874-877,
     * :1188 vgt_gs_max_vert_out 1, :1172 VGT_GS_INSTANCE_CNT with gs_num_invocations 0 for a non-GS pipeline) */
    if ((s = d_add(e, 0x28b38u, 0u, pf->gs_max_vert_out)) != 0u) return s;   /* VGT_GS_MAX_VERT_OUT */
    if ((s = d_add(e, 0x28b3cu, 0u, pf->gs_instance_cnt)) != 0u) return s;   /* VGT_GS_INSTANCE_CNT (contiguous) */
    d_close(e);
    if ((s = d_add(e, 0x28b4cu, 0u, pf->ngg_subgrp_cntl)) != 0u) return s;   /* GE_NGG_SUBGRP_CNTL */
    d_close(e);
    /*: XLAT12_EXTRA_SYNTH_IDXPRIM, DEFAULT OFF - the two NGG words found written by NEITHER
     * half of a compositor frame before the render half's first draw. d_reg overrides them only when Apple writes them, so
     * without this their value is whatever the last EXECUTED submission left. With it they are the SAME values the
     * override writes (pf->spi_shader_idx_format, pf->gs_out_prim_type), emitted after every VGT_SHADER_STAGES_EN like the
     * rest of this block. xlat12_ib.h has the values' provenance. */
    if (ex && (ex->flags & XLAT12_EXTRA_SYNTH_IDXPRIM)) {
        if ((s = d_add(e, 0x28648u, 0u, pf->spi_shader_idx_format)) != 0u) return s;   /* SPI_SHADER_IDX_FORMAT (gfx12) */
        d_close(e);
        if ((s = d_add(e, 0x30998u, 0u, pf->gs_out_prim_type)) != 0u) return s;        /* VGT_GS_OUT_PRIM_TYPE (gfx12 uconfig) */
        d_close(e);
        ds->synth_idxprim++;
    }
    ds->synth_blocks++;
    return 0;
}

/* 0.0.217 (review report 9): the gfx12 NGG rings and the GS/ES CU mask (xlat12_ib.h). Values: Mesa
 * ac_emit_cp_gfx11_ge_rings (ac_cmdbuf_cp.c:300-326) with the Navi48 sizes (ac_gpu_info.c:1710-1758); every word checked
 * against gc_12_0_0_sh_mask.h. d_add merges contiguous addresses into one SET packet whose count is the number of
 * values (Mesa PKT3(op, num, 0)), so each ring group is ONE packet with header count 4. */
static uint32_t d_rings(const xlat12_draw_extra *ex, DEmit *e, xlat12_draw_stats *ds,
                        const uint32_t ptr_pos[4], uint32_t ptr_seen, uint32_t vrs_written)
{
    uint32_t s;
    const uint32_t n0 = e->n;
    d_close(e);
    if (ex->ring_va) {
        const uint64_t a = ex->ring_va;
        if ((s = d_add(e, 0x31110u, 0u, 0x12355123u)) != 0u) return s;                          /* SPI_GS_THROTTLE_CNTL1 */
        if ((s = d_add(e, 0x31114u, 0u, 0x0001544Du)) != 0u) return s;                          /* SPI_GS_THROTTLE_CNTL2 */
        if ((s = d_add(e, 0x31118u, 0u, (uint32_t)(a >> 16))) != 0u) return s;                  /* SPI_ATTRIBUTE_RING_BASE */
        if ((s = d_add(e, 0x3111cu, 0u, 0x00020015u)) != 0u) return s;   /* SPI_ATTRIBUTE_RING_SIZE MEM_SIZE 0x15 | L1_POLICY */
        d_close(e);
        if ((s = d_add(e, 0x309a0u, 0u, (uint32_t)((a + XLAT12_GE_RING_POS_OFF) >> 16))) != 0u) return s;  /* GE_POS_RING_BASE */
        if ((s = d_add(e, 0x309a4u, 0u, 0x00002000u)) != 0u) return s;                          /* GE_POS_RING_SIZE */
        if ((s = d_add(e, 0x309a8u, 0u, (uint32_t)((a + XLAT12_GE_RING_PRIM_OFF) >> 16))) != 0u) return s; /* GE_PRIM_RING_BASE */
        if ((s = d_add(e, 0x309acu, 0u, 0x0C6E07FEu)) != 0u) return s;   /* GE_PRIM_RING_SIZE 0x7FE | device | 3 | 3 | FORCE_SE | NOFILL */
        d_close(e);
    }
    if (ex->rsrc3_gs) {
        if ((s = d_add(e, 0xb21cu, 0u, ex->rsrc3_gs)) != 0u) return s;                          /* SPI_SHADER_PGM_RSRC3_GS */
        d_close(e);
    }
    if (ex->ring_va) {   /* 0.0.219 (review report 10, ): the merged GS stage's s0:s1 source, xlat12_ib.h */
        /* (emitted before the preamble delta; see below for the 0.0.220 raster/CB block, which comes last)
         * 0.0.226: the ring-offsets table VA when the caller gives one (0 otherwise, as 0.0.219-0.0.225)
         *
         *: ALL THREE STAGE PAIRS, not just GS. RADV's gfx12 branch is radv_queue.c:564-570 - and it IS the branch
         * that governs gfx1201: ac_gpu_info.c:752 identifies GFX1201 under FAMILY_NV4 with GFX ip ver_minor 0, and
         * :766-768 then sets gfx_level = GFX12 (GFX12_1 needs ver_minor 1), so `gfx_level >= GFX12` takes :564 and the
         * gfx11 branch at :571 is never reached. radeon_emit_64bit_pointer (radv_cs.h:107 ->
         * ac_cmdbuf.h:365-370) is one SET_SH_REG of 2 values at the LO register, which is exactly what two contiguous
         * d_add calls plus d_close produce here. The three destinations, CONFIRMED against the gfx12 header in this
         * tree (re/m2/linux/.../asic_reg/gc/gc_12_0_0_offset.h; byte = (dw + 0x1260) * 4, the same mapping the existing
         * 0xb210 = dw 0x2c84 comment uses):
         *     regSPI_SHADER_USER_DATA_PS_0 0x19ac -> 0xb030   (:7717)   PS   s[0:1]
         *     regSPI_SHADER_PGM_LO_HS      0x1aa4 -> 0xb410   (:7903)   HS   s[0:1]
         *     regSPI_SHADER_PGM_LO_GS      0x1a24 -> 0xb210   (:7799)   GS   s[0:1]
         * The gfx11 offsets would be WRONG here: gfx11 uses R_00B420 for the HS pair, and on gfx12 0xb420 is
         * regSPI_SHADER_PGM_RSRC4_HS (0x1aa8, :7911) - writing the VA there would corrupt RSRC4, not a pointer.
         * These "PGM_LO" names do NOT clobber a program address on gfx12: the merged stages' real program addresses are
         * regSPI_SHADER_PGM_LO_ES 0x1a29 -> 0xb224 (:7809) and regSPI_SHADER_PGM_LO_LS 0x1aa9 -> 0xb424 (:7913), which
         * is what radv_shader.c:2128 / :2179 select for GFX12; 0xb210 / 0xb410 are free and hold s[0:1] instead.
         * FALSIFIERS for the next arm: the fault stays at 0xa7005... -> the pointers were not the cause, chase the
         * 0xE000 stride; the fault MOVES to 0x23f0a800a0 +/- -> the pointers are fixed and the next defect is
         * downstream; the fault becomes CPF/CPC -> the IB is malformed and's execution claim is wrong. */
        const uint32_t g0 = e->n;
        if ((s = d_add(e, 0xb210u, 0u, (uint32_t)ex->gs_sgpr0_va)) != 0u) return s;            /* SPI_SHADER_PGM_LO_GS (dw 0x2c84) */
        if ((s = d_add(e, 0xb214u, 0u, (uint32_t)(ex->gs_sgpr0_va >> 32))) != 0u) return s;    /* SPI_SHADER_PGM_HI_GS (dw 0x2c85) */
        d_close(e);
        ds->gs_sgpr0_dwords = e->n - g0;
        ds->ring_ptr_stages |= 4u;
        /*: HS gets the SAME pointer, IN PLACE - over the value the stream itself wrote - and NOTHING is appended.
         * WHY NOTHING IS APPENDED. Two more SET_SH_REG-of-2 packets grow the extra block by 8 dwords. That was written
         * and MEASURED: the real captured compositor segments in tests/fixture_census1_suite.h (1022 and 1038 dwords)
         * translate to `extra 77 dw, pad 1 dw` - ONE spare dword - and with the grown block every one of them refused
         * XLAT12_IB_ERR_TOO_LONG. A frame that cannot translate cannot commit, so growth here would cost a whole run.
         * WHAT THE HS PAIR ACTUALLY IS, AND WHY THE ZERO-CHECK IS WHAT MAKES THE WRITE SAFE. This is NOT
         * SPI_SHADER_USER_DATA_ADDR_LO/HI_HS - that register is gfx10 0xb408 and is absent from xlat12_tables.h
         * entirely. The only route to gfx12 0xb410/0xb414 is xlat12_tables.h:124,126: gfx10 0xb420/0xb424 =
         * SPI_SHADER_PGM_LO/HI_HS, moved unchanged - Apple's OWN merged-HS program address, not a spare slot. When
         * the stream's own dwords there are both zero it is unused this draw and ours to fill; when either is
         * non-zero Apple has a real program address there, and the zero-check below refuses to write over it.
         * WHY PS IS *NOT* WRITTEN, against's proposal. On gfx12 RADV also puts the pointer in
         * SPI_SHADER_USER_DATA_PS_0/1 (0xb030/0xb034), but Apple's fragment ABI is not RADV's: in the captured textured
         * segments 5 and 6 of fixture_census1_suite.h, 0xb034 carries the class-19 descriptor TABLE (value 4) and
         * 0xb040 / 0xb048 the texture and sampler indices. Overwriting the PS pair was written and MEASURED here and it
         * destroys that binding - the test "textured segment N keeps the class-19 table and the texture and sampler
         * indices" fails. So the PS destination stays Apple's, and ring_ptr_stages records that bit 1 is never set.
         * The positions come from the caller's DPair, recorded by d_ptr_note at every SET_SH_REG(_INDEX) emit, so they
         * name the LAST write to each register in this region; `ptr_seen` bit k = ptr_pos[k] is valid (k = PS lo, PS hi,
         * HS lo, HS hi). A pair the stream never wrote is NOT written at all, and ring_ptr_stages says so. */
        if (ptr_pos && (ptr_seen & 0xCu) == 0xCu && ptr_pos[2] < g0 && ptr_pos[3] < g0) {
            if (e->out[ptr_pos[2]] == 0u && e->out[ptr_pos[3]] == 0u) {
                e->out[ptr_pos[2]] = (uint32_t)ex->gs_sgpr0_va;             /* SPI_SHADER_PGM_LO_HS (dw 0x2d04) */
                e->out[ptr_pos[3]] = (uint32_t)(ex->gs_sgpr0_va >> 32);     /* SPI_SHADER_PGM_HI_HS (dw 0x2d05) */
                ds->ring_ptr_stages |= 2u;
            } else {
                ds->ring_ptr_stages |= 8u;   /* fix: Apple's own HS program address was already there - refuse, do not clobber */
            }
        }
    }
    if (ex->flags & XLAT12_EXTRA_PREAMBLE) {   /* 0.0.218: the gfx12 preamble delta, xlat12_ib.h */
        static const struct { uint32_t a, v; } pre[] = {
            { 0xb0c0u, 0x00000007u },                                                      /* SPI_SHADER_REQ_CTRL_PS (ac_cmdbuf.c:661) */
            { 0xb0c8u, 0u }, { 0xb0ccu, 0u }, { 0xb0d0u, 0u }, { 0xb0d4u, 0u },            /* USER_ACCUM_PS_0-3 (:664-667) */
            { 0xb2c8u, 0u }, { 0xb2ccu, 0u }, { 0xb2d0u, 0u }, { 0xb2d4u, 0u },            /* USER_ACCUM_ESGS_0-3 (:674-677) */
            { 0xb4c8u, 0u }, { 0xb4ccu, 0u }, { 0xb4d0u, 0u }, { 0xb4d4u, 0u },            /* USER_ACCUM_LSHS_0-3 (:684-687) */
            { 0x2882cu, 0u },                                                              /* PA_SU_PRIM_FILTER_CNTL */
            { 0x28a50u, 0u }, { 0x28a70u, 0u }, { 0x28a80u, 0u },                          /* GE_SE/IA/WD_ENHANCE (:759-761) */
            { 0x28c4cu, 0x00800000u },                                                     /* PA_SC_BINNER_CNTL_2 ENABLE_PING_PONG_BIN_ORDER */
            { 0x30950u, 0x7F9A80E1u },                                                     /* GE_GS_THROTTLE */
            { 0x30980u, 0u },                                                              /* GE_USER_VGPR_EN */
            { 0x31128u, 0x00008A4Du }, { 0x3112cu, 0x00401123u },                          /* SPI_GRP_LAUNCH_GUARANTEE_ENABLE/CTRL */
        };
        const uint32_t p0 = e->n;
        for (uint32_t k = 0; k < sizeof pre / sizeof pre[0]; k++)
            if ((s = d_add(e, pre[k].a, 0u, pre[k].v)) != 0u) return s;
        d_close(e);
        ds->preamble_dwords = e->n - p0;
    }
    if (ex->flags & XLAT12_EXTRA_RASTER) {   /* 0.0.220: the raster/CB delta's extra-block writes, xlat12_ib.h */
        const uint32_t r0 = e->n;
        d_close(e);
        if ((s = d_add(e, 0x283d0u, 0u, 0u)) != 0u) return s;                                   /* PA_SC_VRS_OVERRIDE_CNTL */
        if ((s = d_add(e, 0x283e0u, 0u, 0u)) != 0u) return s;                                   /* PA_SC_VRS_INFO */
        if ((s = d_add(e, 0x28bc0u, 0u, 0u)) != 0u) return s;                                   /* PA_SC_HISZ_RENDER_OVERRIDE */
        if ((s = d_add(e, 0x28f00u, 0u, XLAT12_CB_MEM0_INFO)) != 0u) return s;                  /* CB_MEM0_INFO */
        d_close(e);
        ds->raster_dwords = e->n - r0;
    }
    /* build 0.0.502 (xlat12_ib.h XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION): mesa's gfx12 preamble value
     * (ac_cmdbuf.c:702), with the RING - not under XLAT12_EXTRA_RASTER, which is a switch (27, off by default). Every
     * translation the kext's armed policy makes carries ring_va (gfxsrc_policy counts segsWithout, which must be 0: RUN J
     * translated 3,624 segments WITH a ring and 0 without), so every armed translation gets it; a caller with no ring
     * (host suites, the kext's own suite path) gets no gfx12 NGG state from this block at all, and none of this either.
     * THE FALLBACK: when region 0 already wrote it in front of the stream's DB_SHADER_CONTROL (d_vrs_prefollow, 1 dword),
     * `vrs_written` is 1 and nothing is added here. Region 0 only (this function's one call site), so either way it
     * precedes the translation's first draw; no stream write can reach 0x28068 afterwards (no table row lands there), and
     * inside a unit d_unit_blockreg refuses any that would. */
    if (ex->ring_va && !vrs_written) {
        d_close(e);
        if ((s = d_add(e, XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION, 0u, 0u)) != 0u) return s;   /* DB_SPI_VRS_CENTER_LOCATION = 0 */
        d_close(e);
    }
    ds->ring_dwords = e->n - n0;
    return 0;
}

/* 0.0.389 (notes 880 H6, 881): the gfx12-ONLY SC register that has no gfx10.3 source at all, emitted beside the
 * PA_SC_HIZ_INFO the REUSED row produces. Mesa writes PA_SC_HISZ_CONTROL for EVERY gfx12 pixel shader, not only a
 * depth one: radv_shader.c `regs->ps.pa_sc_hisz_control = S_028BBC_ROUND(2); /+ required minimum value +/` and
 * radeonsi si_state_shaders.cpp `shader->ps.pa_sc_hisz_control = S_028BBC_ROUND(2);` - both unconditional inside
 * their `gfx_level >= GFX12` branch, with CONSERVATIVE_Z_EXPORT added ONLY when the fragment shader declares a
 * depth layout. gfx12.json PA_SC_HISZ_CONTROL fields are ROUND [0:2] and CONSERVATIVE_Z_EXPORT [3:4], so a
 * colour-only pass is exactly ROUND(2) = 0x2. It is unreachable from the table (no gfx10.3 register has this
 * address) and is therefore listed in d_policy_g12, like every other synthesized gfx12 destination.
 * It is emitted ONCE per stream write of the reused pair, in its own SET_CONTEXT_REG packet (ctx 0x2ef is not
 * contiguous with 0x2e5/0x2e6), and NOT under any flag: it is part of the same defined-SC-state change. */
static int d_hisz_follow(uint32_t g12, uint32_t *f12, uint32_t *fv)
{
    /* Hung off the SECOND of the reused pair (PA_SC_HIS_INFO), not the first. ctx 0x2ef is contiguous with
     * neither, so the packet costs 3 dwords wherever it goes; hanging it off PA_SC_HIZ_INFO would additionally
     * SPLIT the stream's own 0x2e5+0x2e6 run into two packets and cost 3 more. */
    if (g12 != 0x28b98u) return 0;                 /* gfx12 PA_SC_HIS_INFO, from xlat12_reused.h */
    *f12 = XLAT12_G12_PA_SC_HISZ_CONTROL; *fv = XLAT12_PA_SC_HISZ_CONTROL; return 1;
}

/* 0.0.220: XLAT12_EXTRA_RASTER's follow-on write after one of the stream's own writes, xlat12_ib.h */
static int d_follow(uint32_t g10, uint32_t val, uint32_t *g12, uint32_t *v12)
{
    if (g10 == 0x28c6cu) { *g12 = 0x28c68u; *v12 = (val >> 26) & 0xFu; return 1; }       /* CB_COLOR0_VIEW -> VIEW2 MIP_LEVEL */
    if (g10 == 0x28c74u) { *g12 = 0x28c70u; *v12 = XLAT12_CB0_FDCC_CONTROL; return 1; }  /* CB_COLOR0_ATTRIB -> FDCC_CONTROL */
    return 0;
}

const xlat12_draw_profile *xlat12_ib_m2tri_profile(void)
{
    /* 0.0.222: VS RSRC1_GS 0x020F0000. The merged NGG stage runs in CU mode (WGP_MODE 0): radeonsi compiles every
     * shader with wgp_mode false (gfx/si_shader_llvm.c:174), RADV's radv_should_use_wgp_mode returns false for NGG above gfx10
     * ("can hang ... chips that disable exactly 1 CU per SA for GS" - our RSRC3_GS 0xfffffdfd does), and air-gfx.py now compiles
     * the stage with +cumode, whose register note is 0x020F0000 with byte-identical code. Before 0.0.222: 0x0A0F0000 (WGP_MODE 1).
     * re/graphics/g2/tri/m2tri/manifest.json (the compiler's pair): VS .registers RSRC1_GS 0x0A0F0000 (pre-cumode),
     * RSRC2_GS 0; PS .registers RSRC1_PS 0x020F0000, RSRC2_PS 0, SPI_PS_INPUT_ENA 1, SPI_PS_INPUT_ADDR 1.
     * 0.0.215 (review report 8,): the manifest's "0x28B54": "0x00400000" is LLVM's PAL fragment (the wave-size
     * contribution only), NEVER a pipeline register value. gfx12 VGT_SHADER_STAGES_EN for a wave32 passthrough VS
     * that sends no GS_ALLOC_REQ is PRIMGEN_PASSTHRU_NO_MSG | GS_W32_EN = 0x04400000 (si_state_shaders.cpp:1332-1339;
     * the pairing ac_nir_lower_ngg.c:1604-1617), and VGT_GS_OUT_PRIM_TYPE is TRISTRIP 2.
     * Synthesized, radeonsi gfx12 for a passthrough VS (workgroup 128,
     * triangles, no parameter exports, PS without interpolants): GE_CNTL = PRIMS_PER_SUBGRP 128 | VERTS_PER_SUBGRP 128
     * | PRIM_GRP_SIZE 256 | DIS_PG_SIZE_ADJUST_FOR_STRIP; GE_MAX_OUTPUT_PER_SUBGROUP 128; RSRC4_GS = LATE_ALLOC_GS 127
     * | GLG_FORCE_DISABLE | WAVE_LIMIT 0x3ff; RSRC4_PS = WAVE_LIMIT 0x3ff | LDS_GROUP_SIZE 1; GS_OUT_CONFIG_PS =
     * NO_PC_EXPORT. ISA heads: the first four dwords of each packed record's blob B (sha256 10a69d35.., 27089d1c..).
     * 0.0.219 (review report 10): m2_tri_vs rebuilt on the gfx12 merged NGG layout (eight system SGPRs s0..s7,
     * user data from s8): its head reads merged_wave_info from s3 into s0, `s_bfe_u32 s0, s3, 0x80008` (llvm-objdump). */
    static const xlat12_draw_profile p = {
        0x020F0000u, 0x00000000u, 0x04400000u,   /* 0.0.222: RSRC1_GS WGP_MODE clear - CU mode, as both Mesa drivers on gfx12 NGG */
        0x020F0000u, 0x00000000u, 0x00000001u, 0x00000001u,
        0xA0010080u, 0x00000080u, 0x007F0BFFu, 0x000007FFu, 0x00000400u,
        0x19FC0123u, 0x00000001u, 0x00000001u, 0x00000000u,
        0x00000002u,
        0x00000001u,
        /* 0.0.223 (review report 11): both vertex heads now open with the EXEC = ~0 prologue every Mesa compiler emits for
         * merged/NGG stages (llvm.amdgcn.init.exec(-1) -> s_mov_b32 exec_lo, -1). Taken from the built objects with llvm-objdump:
         * m2_tri_vs.pal.o +0x00 BEFE00C1 D71F0001 020100C1 9300FF03 (packed sha256 efa5d224..), m2_tri_vs_rec.pal.o +0x00 BEFE00C1
         * 7E0E0300 D71F0000 020100C1. The 0.0.219-0.0.222 heads (d71f0001.. / 7e0e0300 d71f0000..) are no longer accepted, nor is
         * the absolute instrument, whose head was the old m2_tri_vs's. */
        { 0xbefe00c1u, 0xd71f0001u, 0x020100c1u, 0x9300ff03u },   /* s_mov_b32 exec_lo, -1 ; v_mbcnt_lo v1, -1, 0 ; s_bfe s0, s3 */
        { 0xa400fff2u, 0x3e800000u, 0xa401f280u, 0xbf87051au },
        { 0xbefe00c1u, 0x7e0e0300u, 0xd71f0000u, 0x020100c1u },   /* s_mov_b32 exec_lo, -1 ; v_mov v7, v0 ; v_mbcnt_lo v0, -1, 0 */
        0ull,   /* 0.0.311: vs_pgm_va - the tri suite relocates nothing, so its output is byte-identical to 0.0.310 */
        0u,     /* 0.0.313: vs_drops_params - tri's stages export what their fragment stages read */
        0u, 0u, /*: ps_inline_tex1 / _samp1 - tri's fragment stages take no inline record */
        0u,     /* M4-DESC-TABLE-IMPL: ps_table_abi1 - tri's fragment stages read no class-19 table */
        0u,     /* 0.0.391: vs_abi_ptr1 - tri's vertex stages have no ABI pointer row */
        XLAT12_PS_ID_NONE, /* 0.0.398: the base profile carries no RESOLVED fragment identity, so the
                            * fill-colour retarget can never fire on it; a resolver's per-program answer sets it */
        0u, 0u  /* D4' (D4-PRIME.md item 2): ps_readset1 / vs_readset1 - tri's stages have no read-set row */
    };
    return &p;
}

/* The gfx12 destinations the draw policy itself writes (re-homed, overridden, synthesized). */
static int d_policy_g12(uint32_t a)
{
    return a == 0xb210u || a == 0xb214u ||                                                   /* 0.0.219 merged GS s0:s1 */
           a == 0xb410u || a == 0xb414u ||   /* merged HS s0:s1 (gfx12 regSPI_SHADER_PGM_LO/HI_HS; the PS pair
                                              * 0xb030/0xb034 is already inside the USER_DATA_PS window below) */
           /* 0.0.306: the user-data windows are 0..31, not 0..11 - see d_reg. The verifier's own bound was
            * the second place the 0..11 assumption lived, and it caught the widened writes before any test did. */
           (a >= 0xb030u && a <= 0xb0acu) ||   /* SPI_SHADER_USER_DATA_PS_0..31, the same registers on both parts */
           (a >= 0xb430u && a <= 0xb4acu) ||   /* SPI_SHADER_USER_DATA_HS_0..31, identical on gfx12 */
           (a >= 0xb900u && a <= 0xb93cu) ||   /* COMPUTE_USER_DATA_0..15, identical on gfx12 */
           a == 0x28c68u || a == 0x28c70u || a == 0x283d0u || a == 0x283e0u || a == 0x28bc0u || a == 0x28f00u ||   /* 0.0.220 */
           a == XLAT12_G12_PA_SC_HISZ_CONTROL ||   /* 0.0.389: synthesized beside PA_SC_HIZ_INFO, see d_hisz_follow */
           a == XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION ||   /* 0.0.502: the last packet of every d_rings block */
           a == 0xb224u || a == 0xb218u || a == 0xb228u || a == 0xb22cu || (a >= 0xb230u && a <= 0xb2acu) ||
           a == 0x28c44u || a == 0x28b4cu || a == 0x28b38u || a == 0x28b3cu ||
           a == 0xb028u || a == 0xb02cu || a == 0x2865cu || a == 0x28660u || a == 0x3096cu || a == 0x28a98u ||
           a == 0x287fcu || a == 0xb01cu || a == 0xb0c4u || a == 0xb220u || a == 0x30998u || a == 0x28648u ||
           (a >= 0x31110u && a <= 0x3111cu) || (a >= 0x309a0u && a <= 0x309acu) || a == 0xb21cu ||  /* 0.0.217 d_rings */
           a == 0xb0c0u || (a >= 0xb0c8u && a <= 0xb0d4u) || (a >= 0xb2c8u && a <= 0xb2d4u) || (a >= 0xb4c8u && a <= 0xb4d4u) ||
           a == 0x2882cu || a == 0x28a50u || a == 0x28a70u || a == 0x28a80u || a == 0x28c4cu || a == 0x30950u ||
           a == 0x30980u || a == 0x31128u || a == 0x3112cu;                                         /* 0.0.218 preamble delta */
}

/* 1 when `a` is the gfx12 destination of some identical / moved / field_repack table entry. */
static int d_table_g12(uint32_t a)
{
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0; const char *nm = 0;
        if (!xlat12_table_entry(i, &g10, &g12, &cls, &nm)) continue;
        if (g12 == a && (cls == XLAT12_CLS_IDENTICAL || cls == XLAT12_CLS_MOVED || cls == XLAT12_CLS_FIELD_REPACK ||
                         cls == XLAT12_CLS_REUSED))
            return 1;
    }
    return 0;
}

/* build 0.0.540 item 5 (xlat12_ib.h XLAT12_EXTRA_TBLCACHE; switch 97) — THE SAME QUESTION FROM A SORTED CACHE, built once.
 * d_tc_state: 0 unbuilt, 1 being built (by the one thread whose compare-and-swap claimed it), 2 built (d_tc_g12[0..d_tc_n) sorted,
 * published by the release store the readers' acquire load pairs with), 3 the table outgrew XLAT12_TC_CAP (never built: every ask
 * answers from the linear walk). No allocation, no lock, no wait: a translation that finds it unbuilt claims and builds it (668 rows,
 * once per boot) or, if another thread holds the claim, answers from the walk. */
static uint32_t d_tc_g12[XLAT12_TC_CAP];
static uint32_t d_tc_n;
static uint32_t d_tc_state;
static int d_tc_cls(uint32_t cls)
{
    return cls == XLAT12_CLS_IDENTICAL || cls == XLAT12_CLS_MOVED || cls == XLAT12_CLS_FIELD_REPACK || cls == XLAT12_CLS_REUSED;
}
static void d_tc_build(void)
{
    uint32_t want = 0u;
    if (!__atomic_compare_exchange_n(&d_tc_state, &want, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    uint32_t nc = 0u;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0; const char *nm = 0;
        if (!xlat12_table_entry(i, &g10, &g12, &cls, &nm) || !d_tc_cls(cls)) continue;
        if (nc >= XLAT12_TC_CAP) { __atomic_store_n(&d_tc_state, 3u, __ATOMIC_RELEASE); return; }
        d_tc_g12[nc++] = g12;
    }
    for (uint32_t i = 1; i < nc; i++) {   /* insertion sort, once */
        const uint32_t v = d_tc_g12[i];
        uint32_t j = i;
        while (j && d_tc_g12[j - 1u] > v) { d_tc_g12[j] = d_tc_g12[j - 1u]; j--; }
        d_tc_g12[j] = v;
    }
    d_tc_n = nc;
    __atomic_store_n(&d_tc_state, 2u, __ATOMIC_RELEASE);
}
static int d_table_g12_tc(uint32_t a)
{
    uint32_t st = __atomic_load_n(&d_tc_state, __ATOMIC_ACQUIRE);
    if (st == 0u) { d_tc_build(); st = __atomic_load_n(&d_tc_state, __ATOMIC_ACQUIRE); }
    if (st != 2u) return d_table_g12(a);
    uint32_t lo = 0u, hi = d_tc_n;
    while (lo < hi) {
        const uint32_t m = lo + ((hi - lo) >> 1);
        if (d_tc_g12[m] == a) return 1;
        if (d_tc_g12[m] < a) lo = m + 1u; else hi = m;
    }
    return 0;
}
uint32_t xlat12_ib_tblcache_answer(uint32_t a, int *linear, int *cached)
{
    if (linear) *linear = d_table_g12(a);
    if (cached) *cached = d_table_g12_tc(a);
    return __atomic_load_n(&d_tc_state, __ATOMIC_ACQUIRE);
}

/* build 0.0.535: xlat12_ib_draw_verify's walk, with `fill` 1 admitting exactly the NCLEAR fill packet
 * (xlat12_ib_nclear_pkt_ok) - only the translation that carries XLAT12_EXTRA_NCLEAR asks with 1.
 * build 0.0.540 item 5: `tc` 1 (only the translation that carries XLAT12_EXTRA_TBLCACHE) asks d_table_g12's question of its
 * cache; 0 asks d_table_g12 itself, as 0.0.539. */
static uint32_t d_verify(const uint32_t *out, uint32_t n, uint32_t fill, uint32_t tc, uint32_t *bad_addr, uint32_t *bad_op);
uint32_t xlat12_ib_draw_verify(const uint32_t *out, uint32_t n, uint32_t *bad_addr, uint32_t *bad_op)
{
    return d_verify(out, n, 0u, 0u, bad_addr, bad_op);
}
uint32_t xlat12_ib_draw_verify_ex(const uint32_t *out, uint32_t n, uint32_t flags, uint32_t *bad_addr, uint32_t *bad_op)
{
    return d_verify(out, n, (flags & XLAT12_EXTRA_NCLEAR) ? 1u : 0u, (flags & XLAT12_EXTRA_TBLCACHE) ? 1u : 0u, bad_addr, bad_op);
}
static uint32_t d_verify(const uint32_t *out, uint32_t n, uint32_t fill, uint32_t tc, uint32_t *bad_addr, uint32_t *bad_op)
{
    uint32_t i = 0;
    if (bad_addr) *bad_addr = 0;
    if (bad_op) *bad_op = 0xFFFFFFFFu;
    if (!out) return XLAT12_ERR_ARG;
    while (i < n) {
        const uint32_t h = out[i], l = plen_at(out, i, n);
        if (!l) { if (bad_op) *bad_op = h; return XLAT12_IB_ERR_VERIFY; }
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        const uint32_t op = PO(h);
        int ok = 0;
        if (op == OP_CONTEXT_CONTROL) ok = ctxctl_is_proven(&out[i], l);
        else if (op == OP_WRITE_DATA || op == OP_WAIT_REG_MEM || op == OP_COPY_DATA) ok = operand_ok(&out[i], l);
        else if (op == OP_DISPATCH_DIRECT) ok = 0;
        else if (op == OP_DMA_DATA) ok = fill && xlat12_ib_nclear_pkt_ok(&out[i], l);   /* build 0.0.535 */
        else if (is_passthrough(op)) ok = 1;
        else if (is_set(op)) {
            ok = 1;
            const uint32_t base = d_base(op), off = out[i + 1] & 0xFFFFu;
            if ((op == OP_SET_SH_REG_INDEX || op == OP_SET_UCONFIG_REG_INDEX) && (out[i + 1] & 0x0FFF0000u)) ok = 0;
            for (uint32_t k = 0; ok && k + 2u < l; k++) {
                const uint32_t a = (base + off + k) << 2;
                if (d_set_op(a) != (op == OP_SET_SH_REG_INDEX ? OP_SET_SH_REG :
                                    op == OP_SET_UCONFIG_REG_INDEX ? OP_SET_UCONFIG_REG : op) ||
                    !(d_policy_g12(a) || (tc ? d_table_g12_tc(a) : d_table_g12(a)))) {
                    ok = 0;
                    if (bad_addr) *bad_addr = a;
                }
            }
        }
        if (!ok) { if (bad_op) *bad_op = op; return XLAT12_IB_ERR_VERIFY; }
        i += l;
    }
    return 0;
}

int xlat12_ib_find_set(const uint32_t *buf, uint32_t n, uint32_t addr, uint32_t *val)
{
    int found = 0;
    uint32_t i = 0;
    while (buf && i < n) {
        const uint32_t h = buf[i], l = plen_at(buf, i, n);
        if (!l) break;
        if (h != XLAT12_IB_NOP && PT(h) == 3u && is_set(PO(h))) {
            const uint32_t base = d_base(PO(h)), off = buf[i + 1] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++)
                if (((base + off + k) << 2) == addr) { if (val) *val = buf[i + 2u + k]; found = 1; }
        }
        i += l;
    }
    return found;
}

uint32_t xlat12_ib_translate_draw(const xlat12_draw_profile *pf, const uint32_t *in, uint32_t n, uint32_t *out,
                                  uint32_t *out_len, xlat12_draw_stats *ds)
{
    return xlat12_ib_translate_draw_ex(pf, 0, in, n, out, out_len, ds);
}

/* 0.0.307: the bound program pair as the stream writes it, and the profile derived from it. `cur` is used
 * only when the caller supplied a resolver; otherwise `base` is used throughout and the output is 0.0.306's byte for
 * byte. `res_*` is the pair `cur` was derived from, so the resolver is called once per CHANGE, not once per write. */
typedef struct {
    uint32_t vs_lo, vs_hi, ps_lo, ps_hi;
    uint64_t res_vs, res_ps;
    xlat12_draw_profile cur;
    int have;
    /* 0.0.313: the last SPI_PS_IN_CONTROL written BEFORE the draw being built, and whether one was written
     * at all. Recorded unconditionally - before the resolver gate below - because the guard that reads it must work even
     * for a caller that supplies no resolver, and because "never seen" has to be distinguishable from "seen as zero". */
    uint32_t ps_in_ctl, ps_in_seen;
    /* (XLAT12_EXTRA_INLINE_DESC): for fragment user-data slots 0..15, the OUTPUT dword holding the value THIS translation
     * last wrote there (ud_seen bit k = written), and which of those positions already hold a gfx12 record (ud_done), so a
     * record two draws share is translated once and a re-written slot starts gfx10 again. Recorded whatever the flag, read
     * only under it. */
    uint32_t ud_pos[16], ud_seen, ud_done;
    /* build 0.0.453 item 1 (Fix E, inv-f84/REPORT.txt): the USER_SGPR count of SPI_SHADER_PGM_RSRC2_PS as
     * WRITTEN BY THIS TRANSLATION (the same field d_reg derives into ds->ps_user_sgpr from the raw gfx10 register
     * value, kept here per-translation instead of on the shared xlat12_draw_stats so the REDIRECTED rung below can
     * read it). `ps_usg_seen` distinguishes "RSRC2_PS was never written in this translation" (keep refusing - a
     * program whose own USER_SGPR count is unknown might still read the slot) from "seen, and it is 0" (ds->ps_user_sgpr
     * alone cannot make that distinction). Recorded unconditionally at both SET_SH_REG and SET_SH_REG_INDEX writes of
     * gfx10 SPI_SHADER_PGM_RSRC2_PS (0xb02c), whether or not the register emits (d_reg always emits it). */
    uint32_t ps_usg, ps_usg_seen;
    /*: the OUTPUT dword holding the last value this translation wrote to each of the four gfx12 ring-offsets
     * destinations the stream itself can name - PS lo 0xb030, PS hi 0xb034, HS lo 0xb410, HS hi 0xb414 - and a bit per
     * position saying it is valid. Recorded unconditionally, read only by d_rings when there is a ring base. */
    uint32_t ptr_pos[4], ptr_seen;
    /* 0.0.391: the same record for the VERTEX stage's thirty-two user-data slots - the OUTPUT
     * dword this translation last wrote each of SPI_SHADER_USER_DATA_VS_0..31 to, and a bit per slot saying it was
     * written at all. Kept separately from `ud_pos` because the two stages have separate user-data spaces and because
     * d_ud_note is keyed on "the gfx12 address equals the gfx10 one", which is true of PS and false of VS (the VS slots
     * are re-homed onto USER_DATA_GS_n, +0x100). Recorded unconditionally, read only by the table step's export. */
    uint32_t vsud_pos[32], vsud_seen;
    /* D4-PRIME-FIXES.md item 4 (D4-4),  — THE INDEX BUFFER IN FORCE, reset per SEGMENT (this struct is
     * zeroed once per xlat12_ib_translate_draw_ex call, the same scope R1-MEMDST.md Q6's INDEX_BASE tracking used).
     * `ib_base` is INDEX_BASE's client VA (lo & ~1 | (hi & 0xFFFF) << 32); `ib_size` is INDEX_BUFFER_SIZE's body (an
     * index COUNT, not bytes); `ib_type` is the INDEX_TYPE value (& 3) from either the dedicated INDEX_TYPE packet or
     * a SET_UCONFIG_REG/_INDEX write to dword offset 0x243 (F48 uses the latter: 0x20000243), with `ib_type_seen`
     * distinguishing "never written" (decline) from a legitimately-zero value - DPair's own zero-init cannot do that
     * for `ib_type` alone, since 0 is DX_INDEX_16 in-range, not "unset". */
    uint64_t ib_base;
    uint32_t ib_size, ib_type, ib_type_seen;
    /* build 0.0.487 (XLAT12_EXTRA_CS_ELIDE, COMPUTE-N.md Q6 P2) — THE COMPUTE STATE THIS TRANSLATION WROTE, compared
     * on the fly: `cs_ok` bit b (d_cs_bit) = compute register b was last written IN THIS TRANSLATION with N's exact value
     * (a later different value clears it); `cs_lo`/`cs_hi` the last PGM_LO/HI, `cs_ud0` the last USER_DATA_0 (the V#
     * base). Zeroed with the rest of DPair once per translation, so nothing is ever inherited from outside it. Written
     * and read ONLY under the flag. */
    uint32_t cs_ok, cs_lo, cs_hi, cs_ud0;
    /* build 0.0.502 (xlat12_ib.h XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION): 0 while region 0 is open and
     * DB_SPI_VRS_CENTER_LOCATION has not been written; XLAT12_VRS_PREFOLLOW once it was written in front of the stream's own
     * gfx12 DB_SHADER_CONTROL (d_vrs_prefollow); XLAT12_VRS_CLOSED once region 0 is over (set by the caller after d_rings,
     * whose fallback packet covers a region 0 that wrote no DB_SHADER_CONTROL). Zeroed with the rest of DPair. */
    uint32_t vrs_state;
    /* build 0.0.535 (XLAT12_EXTRA_NCLEAR, switch 91): the OUTPUT dword where the current run of compute-register-only
     * SET_SH_REG / SET_SH_REG_INDEX packets began (`nc_run_out`), valid while `nc_run_ok` - any other packet ends the run (a
     * DISPATCH_DIRECT leaves it for the elision to read). Written and read ONLY under the flag; zeroed with the rest. */
    uint32_t nc_run_out, nc_run_ok, nc_run_in;   /* 0.0.535 fix round (a): nc_run_in = the run's first INPUT dword */
    /* build 0.0.536 (XLAT12_EXTRA_RECT2D, switch 92): THE PRIMITIVE STATE THIS TRANSLATION WROTE, packed: [5:0] the last
     * VGT_PRIMITIVE_TYPE PRIM_TYPE (0x30908, plain or _INDEX SET_UCONFIG_REG), XLAT12_R2D_PRIM_SEEN once one was written; [23:16]
     * the last VGT_GS_OUT_PRIM_TYPE value this translation emitted (d_reg's override of Apple's 0x28a6c, or d_r2d_draw's own
     * write), XLAT12_R2D_GSO_SEEN once one was. Recorded whatever the flag (it changes no output), read only under it. */
    uint32_t r2d;
    /* build 0.0.537 (XLAT12_EXTRA_PWS, switch 93): bit 1 the flag is set (the barriers are counted), bit 0 THIS pass converts
     * them (the flag is set and the pass is not the fallback); 0 OFF. Set right after the zeroing, read only by d_region's
     * pass-through branch and d_pws. */
    uint32_t pws;
    /* build 0.0.539 (xlat12_ib.h XLAT12_PWS_SLOT_*): the INPUT dword of the Apple top-of-pipe wait slot d_region marked after
     * d_pws converted the barrier 10 dwords before it; 0 none. Set only in a converting pass, dropped by the NOP branch when it meets
     * exactly that dword, cleared at the end of every region. */
    uint32_t pws_slot;
} DPair;
#define XLAT12_R2D_PRIM_SEEN 0x100u
#define XLAT12_R2D_LAST_RECT 0x200u      /* the last draw d_r2d_draw decided was a RECTLIST (bookkeeping; decides nothing) */
#define XLAT12_R2D_GSO_SEEN  0x1000000u
#define XLAT12_VRS_PREFOLLOW 1u
#define XLAT12_VRS_CLOSED    2u
#define D_VS_UD0   0xb130u   /* gfx10.3 SPI_SHADER_USER_DATA_VS_0 byte address; _31 is 0xb1ac (d_reg re-homes the range) */
#define D_VS_UDN   32u
#define D_PS_UD0   0xb030u   /* gfx10.3 SPI_SHADER_USER_DATA_PS_0 byte address (the draw policy passes it through unchanged).
                              * - IS THE PASS-THROUGH A SECOND DEFECT? Partly. In arm5's committed frame the only PS
                              * user-data pair Apple wrote was 0xffffffff / 0xffffffff, and we copied it. There are two
                              * mechanisms, not one: (a) we never emitted a ring-offsets pointer to PS at all, and (b) Apple's
                              * own value reaches 0xb030/0xb034 unchanged. In the FIRST region they collapse into one defect,
                              * because d_rings() is emitted LAST in region 0 (xlat12_ib.c: the `dk == 0u` extra-block call,
                              * after d_region), so the write lands AFTER the pass-through and wins. For draws in regions
                              * 1..N-1 the pass-through is a SEPARATE, STILL-OPEN defect: Apple can rewrite the pair after our
                              * only emission and we would copy it again. NOT fixed here - suppressing or re-writing a register
                              * Apple names is a behaviour change with its own blast radius, and arm5's frame is 1 segment.
                              * Note too that 0xffffffff_ffffffff does not explain the observed fault VA 0xa70059a80000, so the
                              * all-ones value is evidence of a missing pointer, NOT of the faulting address. */
#define D_PS_UDN   16u

/*: the in-place position of a value just written to one of the four ring-offsets destinations (last dword emitted).
 * Keyed on the GFX12 address, not the gfx10 one, because the HS pair is a MOVED entry (g12 != g10) while the PS pair is
 * identical - see d_rings for what is done with these. */
static void d_ptr_note(DPair *p, uint32_t g12, uint32_t pos)
{
    if (!p) return;
    uint32_t k;
    if (g12 == 0xb030u) k = 0u; else if (g12 == 0xb034u) k = 1u;
    else if (g12 == 0xb410u) k = 2u; else if (g12 == 0xb414u) k = 3u; else return;
    p->ptr_pos[k] = pos; p->ptr_seen |= 1u << k;
}

/*: the in-place position of a PS user-data value the emitter has just written (it is the last dword emitted). */
static void d_ud_note(DPair *p, uint32_t g10, uint32_t pos)
{
    if (!p || g10 < D_PS_UD0 || g10 >= D_PS_UD0 + 4u * D_PS_UDN) return;
    const uint32_t k = (g10 - D_PS_UD0) >> 2;
    p->ud_pos[k] = pos; p->ud_seen |= 1u << k; p->ud_done &= ~(1u << k);
}

/* 0.0.391: the in-place position of a VERTEX user-data value just emitted. Keyed on the GFX10
 * address, because that is the one the ABI rows and the stream both speak; d_reg re-homes it to USER_DATA_GS_n. */
static void d_vsud_note(DPair *p, uint32_t g10, uint32_t pos)
{
    if (!p || g10 < D_VS_UD0 || g10 >= D_VS_UD0 + 4u * D_VS_UDN) return;
    const uint32_t k = (g10 - D_VS_UD0) >> 2;
    p->vsud_pos[k] = pos; p->vsud_seen |= 1u << k;
}

/* build 0.0.449 item 2 (F2, review of 0.0.448) — THE PS/VS USER-DATA DWORD RANGE, in the SAME dword-index
 * space `reg_identical`/operand_ok already key registers by (xlat12_lookup's own addressing: dword_index << 2 ==
 * the byte address xlat12_tables.h rows use). O(1) overlap test, so a long auto-increment WRITE_DATA needs NO
 * per-dword loop and therefore no cap (the review's own finding: 0.0.448's 64-dword cap missed a longer write
 * that ran into the range from below). `first`/`last` are INCLUSIVE dword indices. */
#define D_PS_DW0 (D_PS_UD0 >> 2)
#define D_VS_DW0 (D_VS_UD0 >> 2)
/* Exported (not `static`) so a test can drive the exact O(1) arithmetic directly, at any magnitude - the
 * "no cap" and "off-by-one" properties are properties of THIS function, and a test that could only reach it
 * through a real packet would need a genuinely 65+-register-long run of OTHER real, XLAT12_CLS_IDENTICAL
 * registers immediately below the PS or VS range to demonstrate the old cap's failure, which this ISA does not
 * offer (the SH_REG block's own room below PS is 12 dwords; the PS-to-VS gap is 48). Exposing the pure function
 * is exactly the established convention (xlat12_class11_row_ok, xlat12_table_img_desc). */
int xlat12_ud_range_hits(uint32_t first, uint32_t last)
{
    if (last < first) { const uint32_t t = first; first = last; last = t; }   /* defensive: never trust caller order */
    if (first <= D_PS_DW0 + D_PS_UDN - 1u && last >= D_PS_DW0) return 1;
    if (first <= D_VS_DW0 + D_VS_UDN - 1u && last >= D_VS_DW0) return 1;
    return 0;
}

/* build 0.0.453 item 5 (inv-f84/REPORT.txt, XLAT12_EXTRA_DESC_INV_APPLE_HEAD's own check) - see that
 * flag's own comment in xlat12_ib.h for the CONFIRMED evidence. `in[0]` is checked, never a draw-local offset: the
 * caller (xlat12_ib_translate_draw_ex) is always handed one SEGMENT's own dwords starting at index 0. */
int xlat12_ib_head_acquire_mem_covers(const uint32_t *in, uint32_t n, uint32_t needMask)
{
    if (!in || n < 8u) return 0;
    const uint32_t h = in[0];
    if (((h >> 8) & 0xFFu) != 0x58u) return 0;            /* PACKET3 opcode ACQUIRE_MEM */
    if (((h >> 16) & 0x3FFFu) != 6u) return 0;            /* the header's own count FIELD: PACKET3(ACQUIRE_MEM, 6) */
    const uint32_t gcr = in[7];                            /* header + 6 body dwords: ..., POLL_INTERVAL, GCR_CNTL */
    return (gcr & needMask) == needMask;
}

/* build 0.0.449 item 3 (F4, MIB-A1-PATH.md B2) — a non-proven CONTEXT_CONTROL clears the WHOLE carry: it
 * asks the CP to restore per-context state from Apple's gfx10 shadow, which this translator does not track, so
 * any PS/VS value it carried up to this point can no longer be trusted as "what Apple's stream last wrote" -
 * the shadow restore might have changed it by a path this library never saw. Takes the carry directly (may be
 * NULL - a no-op - matching every other carry access, which the caller already gates on the flag). */
static void d_ud_carry_clear(xlat12_ud_carry *carry)
{
    if (!carry) return;
    for (uint32_t z = 0; z < sizeof *carry / sizeof(uint32_t); z++) ((uint32_t *)carry)[z] = 0u;
}

/* build 0.0.448 item 1, build 0.0.449 item 1 (F1: moved off a file-scope static onto CALLER-OWNED
 * `ex->ud_carry` - see xlat12_ud_carry's own comment in xlat12_ib.h) — record Apple's RAW input value for a PS
 * or VS user-data slot, unconditionally (called for every SET_SH_REG / SET_SH_REG_INDEX write to either range,
 * regardless of whether d_reg goes on to emit it). Last write wins, exactly like d_ud_note/d_vsud_note's own
 * "in-place position" bookkeeping - both are updated from the SAME stream in the SAME order. The caller passes
 * NULL when the carry is not in use (the flag off, or - defensively - if ever asked without one); a NULL carry
 * is a no-op, never a crash. */
static void d_ud_carry_note(xlat12_ud_carry *carry, uint32_t g10, uint32_t val)
{
    if (!carry) return;
    if (g10 >= D_PS_UD0 && g10 < D_PS_UD0 + 4u * D_PS_UDN) {
        const uint32_t k = (g10 - D_PS_UD0) >> 2;
        carry->ps_val[k] = val; carry->ps_ok |= 1u << k;
    }
    if (g10 >= D_VS_UD0 && g10 < D_VS_UD0 + 4u * D_VS_UDN) {
        const uint32_t k = (g10 - D_VS_UD0) >> 2;
        carry->vs_val[k] = val; carry->vs_ok |= 1u << k;
    }
}

/* 0.0.398: the ONE identity the fill-colour retarget gates on, matched by NAME from the generated table
 * (xlat12_shader_ids.h) rather than by a hand-copied index - the table's order is generated, so an index here would be
 * a second copy of generated data that a regeneration could silently invalidate. Libc-free; the loop is over short
 * literals and runs only under XLAT12_EXTRA_FILL_COLOR. */
static int d_name_is(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int d_is_fill_color_id(uint32_t id)
{
    return id < XLAT12_SHADER_ID_COUNT && d_name_is(kXlat12ShaderIds[id].name, "ws_B_ColorFill");
}

/* 0.0.398 ( DESIGN items 1-2): THE FILL-COLOUR RETARGET. `ws_B_ColorFill` does not clear with a constant -
 * it LOADS its colour from a 16-byte float4 at the VA its SPI_SHADER_USER_DATA_PS_2/_3 pair names (measured: the pair
 * 0x000c0110 / 0x00000004 = the plain 64-bit VA 0x4000c0110 in arm21's frame 1), and the draw policy passes that pair
 * through verbatim. At each draw whose in-force fragment identity is ws_B_ColorFill, replace the pair with
 * ex->fill_color_va - IN PLACE in the already-translated output, so every other dword of the segment is byte-identical.
 *
 * WHY THE DRAW AND NOT d_reg. The pair is two registers the emitter writes one dword at a time; the 64-bit value is
 * only known once BOTH are in the output, and the identity is only known once the fragment program address has been
 * resolved. d_ud_note records each PS user-data slot's output position as it is emitted, so at the draw both halves and
 * the in-force identity are in hand - which is what this needs and what d_reg cannot see. It is applied like
 * d_inline_desc / d_table_desc, the translator's two other in-place rewrites at a draw. NOTHING here runs with the flag
 * off, so the output is byte-identical by construction.
 *
 * THE PAIR IS A PLAIN 64-BIT POINTER, not a PGM_LO/HI-style VA>>8 pair: word 2 is bits[31:0] and word 3 is bits[63:32]
 * ('s own measurement, and measured the same shape for the plane frame's s2:s3).
 *
 * FAIL-CLOSED (item 2). Not one dword is written unless the flag is set (the caller's job), the in-force PS identity is
 * ws_B_ColorFill, and BOTH slots were written by THIS translation - an inherited pair is a value this translation never
 * saw. A pair that does not reconstruct to a 64-bit VA in WindowServer's range [0x4_00000000, 0x8_00000000) is left
 * alone too: that is the observed shape and anything else is not a colour buffer we understand. Every refusal counts
 * fill_color_refused and changes nothing; a success counts fill_color_retargeted and records the old and new VAs. */
/* build 0.0.536 fix round: noinline, pinning 0.0.535's code shape - the one-block retry in xlat12_ib_translate_draw_ex made the
 * compiler start inlining this into it (+0x150 on that frame); out of line it costs exactly what it cost at 0.0.535. */
static __attribute__((noinline)) void d_fill_color(const xlat12_draw_profile *cp, const xlat12_draw_extra *ex, const DPair *p, uint32_t *out,
                         xlat12_draw_stats *ds)
{
    if (!d_is_fill_color_id(cp->ps_id)) return;
    if (((p->ud_seen >> 2) & 1u) == 0u || ((p->ud_seen >> 3) & 1u) == 0u) { ds->fill_color_refused++; return; }
    const uint32_t lo = out[p->ud_pos[2]], hi = out[p->ud_pos[3]];
    const uint64_t old_va = ((uint64_t)hi << 32) | (uint64_t)lo;
    if (old_va < 0x400000000ull || old_va >= 0x800000000ull) { ds->fill_color_refused++; return; }
    out[p->ud_pos[2]] = (uint32_t)ex->fill_color_va;
    out[p->ud_pos[3]] = (uint32_t)(ex->fill_color_va >> 32);
    ds->fill_color_retargeted++;
    ds->fill_color_old = old_va;
    ds->fill_color_new = ex->fill_color_va;
}

/* D8 (D4-PRIME-FIXES.md item 8,  (B)) — CAP AFTER DEDUPE. Appends `ptrVa` to ds->rs_ptr[], deduping BY
 * PAGE first: a pointer whose page is already present is not appended again (the same physical page named twice is
 * not a different fact - the SAME direction gfx_cp_build.h's n48_cp_merge_dedup already dedupes the frame-level
 * union in). So N distinct pointer VALUES that all land on FEWER than XLAT12_RS_PTR_MAX distinct PAGES never
 * overflow this list, even when N itself is large; only a genuinely NEW page can ever set `rs_over`. Named
 * `d_rs_ptr_add` (D4-PRIME-FIXES.md item 8's own name) - it did not exist before 0.0.441: the two call sites below
 * each carried their own copy of the raw "append, or set rs_over past the cap" pair. */
static void d_rs_ptr_add(xlat12_draw_stats *ds, uint64_t ptrVa)
{
    const uint64_t page = ptrVa & ~(uint64_t)0xFFFull;
    for (uint32_t k = 0; k < ds->rs_nptr && k < XLAT12_RS_PTR_MAX; k++)
        if ((ds->rs_ptr[k] & ~(uint64_t)0xFFFull) == page) return;   /* this page is already named */
    if (ds->rs_nptr < XLAT12_RS_PTR_MAX) ds->rs_ptr[ds->rs_nptr++] = ptrVa;
    else ds->rs_over = 1u;
}

/* build 0.0.455 item 1: forward declaration - the definition (near d_sh_abi_known, xlat12_ib.h's own comment
 * on XLAT12_EXTRA_VS_KNOWN) sits beside d_table_desc's other carry helpers; d_readset_stage below needs it earlier
 * in the file. */
static int d_vs_slot_known(const xlat12_draw_extra *ex, uint32_t seen, const uint32_t *pos, const uint32_t *out, uint32_t k);

/* D4' (notes/design/D4-PRIME.md item 3) — ONE STAGE'S CONTRIBUTION to the accumulated read-set, at ONE draw. `row1` is
 * the profile's ps_readset1 or vs_readset1 (0 = no row: UNKNOWN, declines). `seen`/`pos` are the stage's OWN user-data
 * record (p->ud_seen/ud_pos for PS, p->vsud_seen/vsud_pos for VS), `udn` its width (D_PS_UDN/D_VS_UDN). Declines the
 * WHOLE SEGMENT (ds->rs_declined) whenever the row is missing or its proof bits do not admit it - never a silent
 * zero. A declared slot this stream never wrote (or wrote only before `region_from`, the same "inherited" hazard the
 * ABI-pointer loop refuses) also declines: a page this rule cannot name is not a page it may assume absent.
 * build 0.0.455 item 1 (switch 52): `ex` and `isVs` let the VERTEX call (only) fall back to
 * d_vs_slot_known for a slot written earlier in this translation - the SAME known-slot rule d_table_desc's two
 * vertex branches use. `isVs` is 0 for the PS call (d_readset_accum passes it explicitly): PS has no such rule
 * here (d_sh_abi_known already covers the PS ABI-pointer case, inside d_table_desc, not this generic stage). */
static void d_readset_stage(uint32_t row1, uint32_t seen, const uint32_t *pos, uint32_t done, uint32_t udn,
                            uint32_t region_from, const uint32_t *out, xlat12_draw_stats *ds,
                            const xlat12_draw_extra *ex, int isVs)
{
    if (!row1 || row1 > XLAT12_READSET_COUNT) { ds->rs_declined = 1u; return; }
    const xlat12_readset_row *row = &kXlat12Readset[row1 - 1u];
    if (row->proof_depth1_data_only != 1u) { ds->rs_declined = 1u; return; }
    /* build 0.0.470: a row longer than the loop below walks is declined, never truncated (unreachable today: the
     * generator refuses such a row; this is the same no-silent-truncation rule as xlat12_abi_ptr_n). build 0.0.504:
     * the cap is 5 (ws_J_VfxXghb's five pointers walk in full). */
    if (row->nptr > XLAT12_READSET_PTR_MAX) { ds->rs_declined = 1u; return; }
    /* xlat12_readset.h's own convention (tools/gfx-readset.py, matching tools/gfx-shader-desc.py's classifier):
     * inl_tex/inl_samp are 0xFF = NO inline record, otherwise a DIRECT 0-based user-data slot (Tex_PS_gfx1201:
     * inl_tex 0x00 = s0, inl_samp 0x08 = s8) - NOT xlat12_draw_profile's "N+1, 0 = none" convention. */
    const uint32_t hasImage = (row->inl_tex != 0xffu || row->desc_table != 0u) ? 1u : 0u;
    if (hasImage && row->proof_images_inline != 1u) { ds->rs_declined = 1u; return; }
    for (uint32_t q = 0; q < row->nptr && q < XLAT12_READSET_PTR_MAX; q++) {
        const uint32_t s0 = row->slot[q];
        if ((uint32_t)s0 + 1u >= udn) { ds->rs_declined = 1u; continue; }
        const int k0 = ((seen >> s0) & 1u) &&
                       (pos[s0] >= region_from || (isVs && d_vs_slot_known(ex, seen, pos, out, s0)));
        const int k1 = ((seen >> (s0 + 1u)) & 1u) &&
                       (pos[s0 + 1u] >= region_from || (isVs && d_vs_slot_known(ex, seen, pos, out, s0 + 1u)));
        if (!k0 || !k1) { ds->rs_declined = 1u; continue; }
        if (pos[s0] < region_from || pos[s0 + 1u] < region_from) ds->vs_known_n++;
        d_rs_ptr_add(ds, (uint64_t)out[pos[s0]] | ((uint64_t)out[pos[s0 + 1u]] << 32));
    }
    /* Tex_PS_gfx1201's inline T# (s0): admitted as an IMAGE INPUT for R1-R4, read from the RAW gfx10 record - this
     * runs BEFORE d_inline_desc converts it in place, so out[] still holds Apple's original 8 dwords. */
    if (row->inl_tex != 0xffu) {
        const uint32_t base = row->inl_tex;
        if (base + 8u > udn) { ds->rs_declined = 1u; return; }
        uint32_t seenAll = 1u, anyDone = 0u;
        for (uint32_t k = 0; k < 8u; k++) { seenAll &= (seen >> (base + k)) & 1u; anyDone |= (done >> (base + k)) & 1u; }
        if (!seenAll) { ds->rs_declined = 1u; return; }
        /* D4-PRIME-FIXES.md item 3(c) — THE ALREADY-CONVERTED T# GUARD. A record two draws SHARE (neither rewrites
         * the slots) keeps whatever an EARLIER draw's d_inline_desc (XLAT12_EXTRA_INLINE_DESC) already wrote there -
         * gfx12 bytes - and `out[]` below would be read as a raw gfx10 record instead. `done` is 0 for the vertex
         * stage (only the fragment stage's inline records are ever converted), so this is inert there. */
        if (anyDone) { ds->rs_declined = 1u; ds->rs_decl_why |= XLAT12_RS_WHY_CONVERTED; return; }
        uint32_t rec[8], g[8], dropped = 0;
        for (uint32_t k = 0; k < 8u; k++) rec[k] = out[pos[base + k]];
        if (xlat12_table_img_desc(rec, g, &dropped)) { ds->rs_declined = 1u; return; }
        const uint32_t mode = (g[3] >> 20) & 0x1Fu;
        const uint64_t sva = ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8);
        if (ds->rs_n < XLAT12_RS_IN_MAX) {
            ds->rs_va[ds->rs_n] = sva; ds->rs_mode[ds->rs_n] = mode; ds->rs_proven[ds->rs_n] = 0u; ds->rs_n++;
        } else ds->rs_over = 1u;
    }
}

/* D4' (notes/design/D4-PRIME.md item 3), NARROWED by D4-PRIME-FIXES.md item 1 (D4-1) — BOTH STAGES, at
 * ONE draw whose in-force fragment program binds NO class-19 table (`ps_table_abi1 == 0`). Through 0.0.441 this ran
 * for every draw regardless of `ps_table_abi1`; item 1 makes the two paths MUTUALLY EXCLUSIVE per draw - a
 * table-bound draw is accumulated by d_readset_from_table below, from the table step's OWN export, which proves a
 * STRONGER fact (the redirected heap pages) than re-reading the raw T#/S# slots here ever could, and re-reading them
 * here would in any case read the WRONG bytes once the table step (which runs afterward) rewrites them in place.
 * Called BEFORE d_inline_desc translates any inline record in place - the VS/PS-inline-image branch inside
 * d_readset_stage needs the raw gfx10 bytes. Runs on `out[]`, which already holds this draw's own user-data writes
 * (d_region copies SET_SH_REG packets through verbatim before a draw is reached), so the values read here are
 * exactly what the hardware would read at this draw. Nothing here changes an output dword, return code or existing
 * counter; OFF, this function is never called. */
/* build 0.0.536 fix round: noinline, pinning 0.0.535's code shape - the one-block retry in xlat12_ib_translate_draw_ex made the
 * compiler start inlining this into it (+0x150 on that frame); out of line it costs exactly what it cost at 0.0.535. */
static __attribute__((noinline)) void d_readset_accum(const xlat12_draw_profile *cp, const DPair *p, const uint32_t *out, xlat12_draw_stats *ds,
                            uint32_t region_from, const xlat12_draw_extra *ex)
{
    d_readset_stage(cp->ps_readset1, p->ud_seen, p->ud_pos, p->ud_done, D_PS_UDN, region_from, out, ds, ex, 0);
    d_readset_stage(cp->vs_readset1, p->vsud_seen, p->vsud_pos, 0u, D_VS_UDN, region_from, out, ds, ex, 1);
}

/* D4-PRIME-FIXES.md item 1 (D4-1),  — THE SIBLING OF d_readset_accum ABOVE, for a draw WHOSE IN-FORCE
 * FRAGMENT PROGRAM BINDS A CLASS-19 TABLE (`ps_table_abi1 != 0`). Called AFTER d_table_desc returns 0 for such a
 * draw, so `ds->in_*` is that draw's OWN table export (0.0.390,  part 5): every image input the table step
 * gathered, the two heap pages and the table pointer itself, and the fragment/vertex ABI pointer pages the ABI-row
 * loop resolved. Appends them into the SAME accumulated `rs_*` fields d_readset_stage feeds, so the D4' union covers
 * table-bound and non-table-bound draws of one segment alike (D4-PRIME.md Finding 4's own direction: an
 * unenumerated draw is UNKNOWN, never "reads nothing").
 *
 * DECLINES THE WHOLE SEGMENT on any of the table step's own fail-open signals: `in_over` (the image list did not
 * fit), `!in_ptr_known` / `!in_vptr_known` (the fragment or vertex program has no ABI row: UNKNOWN), or
 * `in_ptr_inherit > 0` (a declared pointer slot this stream never wrote - its value is not ours to name). Each is
 * already the table step's OWN refusal-worthy fact; this only makes the D4' union agree with it instead of trusting
 * a partial list the table step itself would not have trusted. */
static void d_readset_from_table(xlat12_draw_stats *ds)
{
    if (ds->in_over || !ds->in_ptr_known || !ds->in_vptr_known || ds->in_ptr_inherit > 0u) { ds->rs_declined = 1u; return; }
    for (uint32_t q = 0; q < ds->in_n && q < XLAT12_DRAW_IN_MAX; q++) {
        if (ds->rs_n < XLAT12_RS_IN_MAX) {
            ds->rs_va[ds->rs_n] = ds->in_va[q]; ds->rs_mode[ds->rs_n] = ds->in_mode[q]; ds->rs_proven[ds->rs_n] = ds->in_proven[q];
            ds->rs_n++;
        } else ds->rs_over = 1u;
    }
    const uint64_t fixed[3] = { ds->in_tbl_va, ds->in_img_va, ds->in_samp_va };
    for (uint32_t q = 0; q < 3u; q++) if (fixed[q]) d_rs_ptr_add(ds, fixed[q]);
    for (uint32_t q = 0; q < ds->in_nptr && q < XLAT12_ABI_PTR_MAX; q++) d_rs_ptr_add(ds, ds->in_ptr[q]);
    for (uint32_t q = 0; q < ds->in_nvptr && q < XLAT12_ABI_PTR_MAX; q++) d_rs_ptr_add(ds, ds->in_vptr[q]);
}

/* D4-PRIME-FIXES.md item 4 (D4-4),  — A BYTE RANGE AS PAGES, INTO THE ACCUMULATED READ-SET. Every
 * distinct page [va, va+bytes) touches is named (`d_rs_ptr_add` already dedupes by page and caps at
 * XLAT12_RS_PTR_MAX), so a caller need not compute page counts itself. A range spanning MORE THAN 16 PAGES is
 * DECLINED outright rather than silently truncated to its first 16: an index buffer this large is not a shape this
 * enumeration was built to name, and naming only part of it would under-report the rest as absent. `bytes == 0` is a
 * no-op (nothing to name, nothing to decline). */
static void d_rs_range(xlat12_draw_stats *ds, uint64_t va, uint64_t bytes)
{
    if (!bytes) return;
    const uint64_t first = va & ~(uint64_t)0xFFFull;
    const uint64_t last = (va + bytes - 1ull) & ~(uint64_t)0xFFFull;
    const uint64_t pages = ((last - first) >> 12) + 1ull;
    if (pages > 16ull) { ds->rs_declined = 1u; return; }
    for (uint64_t p = first; p <= last; p += 0x1000ull) d_rs_ptr_add(ds, p);
}

int xlat12_iterate256_droppable(const uint32_t rec[8])
{
    if (!rec) return 0;
    const uint32_t w6 = rec[6], type = rec[3] >> 28;
    if (!((w6 >> 10) & 1u)) return 0;                   /* nothing to drop */
    if (w6 & ((1u << 21) | (1u << 20) | (1u << 19))) return 0;   /* COMPRESSION_EN, WRITE_COMPRESS_ENABLE, META_PIPE_ALIGNED */
    if (w6 >> 24) return 0;                             /* META_DATA_ADDRESS_LO */
    if (rec[7]) return 0;                               /* META_DATA_ADDRESS (the rest of it) */
    if (type == 14u || type == 15u) return 0;           /* 2D_MSAA, 2D_MSAA_ARRAY */
    return 1;
}

/*: translate, IN PLACE in `out`, the inline records of the fragment program in force at THIS draw. 0 or XLAT12_IB_ERR_DESC.
 * M4-DESC-KEXT-PORT: a T# whose TRANSLATED gfx12 SW_MODE (w3 [24:20]) is non-zero now needs the same proof the table step
 * demands (ex->desc_tiled_ok(va, mode) == 1, va from the gfx10 record's BASE_ADDRESS w0 / w1 [7:0]); without it the draw refuses
 * err_op XLAT12_TDESC_PROVENANCE. Linear records are admitted as before. Every wsgc1 inline record is linear (gfx12 w3
 * 0x90000204, SW 0), so no captured frame's answer moves. */
/* build 0.0.536 fix round: noinline, pinning 0.0.535's code shape - the one-block retry in xlat12_ib_translate_draw_ex made the
 * compiler start inlining this into it (+0x150 on that frame); out of line it costs exactly what it cost at 0.0.535. */
static __attribute__((noinline)) uint32_t d_inline_desc(const xlat12_draw_profile *cp, const xlat12_draw_extra *ex, DPair *p, uint32_t *out,
                              xlat12_draw_stats *ds, uint32_t draw_at)
{
    const uint32_t spec[2][2] = { { cp->ps_inline_tex1, 8u }, { cp->ps_inline_samp1, 4u } };
    ds->inline_draws++;
    for (uint32_t r = 0; r < 2u; r++) {
        if (!spec[r][0]) continue;
        const uint32_t base = spec[r][0] - 1u, len = spec[r][1];
        uint32_t w[8], o[8], seen = 0, done = 0;
        ds->err_reg = D_PS_UD0 + 4u * base; ds->err_in_dword = draw_at;
        if (base + len > D_PS_UDN) { ds->err_op = 0xFFu; return XLAT12_IB_ERR_DESC; }
        for (uint32_t k = 0; k < len; k++) {
            seen += (p->ud_seen >> (base + k)) & 1u;
            done += (p->ud_done >> (base + k)) & 1u;
        }
        if (seen != len) { ds->err_op = 0xFEu; return XLAT12_IB_ERR_DESC; }   /* inherited from outside: unknown */
        if (done == len) continue;                                             /* this translation already converted it */
        if (done) { ds->err_op = 0xFDu; return XLAT12_IB_ERR_DESC; }           /* half gfx12, half gfx10: refuse */
        for (uint32_t k = 0; k < len; k++) w[k] = out[p->ud_pos[base + k]];
        uint32_t st, dropped = 0;
        if (len == 8u) {
            if (xlat12_iterate256_droppable(w)) { w[6] &= ~(1u << 10); dropped = 1; }
            st = xlat12_img_desc_g10_to_g12(w, o);
            if (!st && ((o[3] >> 20) & 0x1Fu)) {   /* M4-DESC-KEXT-PORT: a tiled surface needs a proven producer */
                const uint64_t sva = ((uint64_t)(w[1] & 0xFFu) << 40) | ((uint64_t)w[0] << 8);
                // build 0.0.451 item 2 (S4): this T#'s OWN element size, from ITS raw gfx10 FORMAT field (w[1]
                // [28:20], unchanged by xlat12_img_desc_g10_to_g12 which only ever writes `o`, not `w`).
                const uint32_t elemBytes = xlat12_format_elem_bytes((w[1] >> 20) & 0x1FFu);
                /* build 0.0.486 (switch 59): the T#-aware ask when the caller wired it; its clamp lands in `o`, OUR
                 * translated copy, before `o` is written to the output. NULL: exactly the ask below, as before. */
                uint32_t clamp = 0u;
                const int provenT = (ex && ex->desc_tiled_okt)
                    ? (ex->desc_tiled_okt(ex->desc_ctx, sva, (o[3] >> 20) & 0x1Fu, elemBytes, w, &clamp) == 1) : -1;
                if (provenT == 1 && clamp == 1u) xlat12_tdesc_clamp_mip0(o);
                if (provenT == 0 ||
                    (provenT < 0 && !(ex && ex->desc_tiled_ok && ex->desc_tiled_ok(ex->desc_ctx, sva, (o[3] >> 20) & 0x1Fu, elemBytes) == 1))) {
                    st = XLAT12_TDESC_PROVENANCE;
                    ds->prov_va = sva; ds->prov_mode = (o[3] >> 20) & 0x1Fu; ds->prov_path = 1u;   /*: name the surface */
                }
            }
        } else {
            st = xlat12_samp_desc_g10_to_g12(w, o);
        }
        if (st) { ds->err_op = st; return XLAT12_IB_ERR_DESC; }
        for (uint32_t k = 0; k < len; k++) { out[p->ud_pos[base + k]] = o[k]; p->ud_done |= 1u << (base + k); }
        if (len == 8u) { ds->inline_img++; ds->inline_iter256 += dropped; } else ds->inline_samp++;
    }
    ds->err_reg = 0; ds->err_in_dword = 0; ds->err_op = 0xFFFFFFFFu;
    return 0;
}

/* ---- M4-DESC-TABLE-IMPL (xlat12_ib.h XLAT12_EXTRA_TABLE_DESC, default off) ------------------------------------------------
 * The class-19 table ABIs, read out of our own gfx1201 objects (llvm-objdump --mcpu=gfx1201; ndw/fnv recomputed from each
 * object's .text and equal to the identity rows ws_D_GPUPass / UberCompositeFragment). Both programs load ONLY the table's
 * +0x00 (image heap base) and +0x10 (sampler heap base), each as a raw 64-bit address, and index them with sext32(idx << 5) /
 * sext32(samp << 4); with samp bit 31 set they replace S# word 1 with the raw next slot:
 *   GPUPass  +0x24 F4002080 F8000010 s_load_b64 s[2:3], s[0:1], 0x10    +0x2c F4002380 F8000000 s_load_b64 s[14:15], s[0:1], 0x0
 *            +0x34 84008408 s_lshl_b32 s0, s8, 4   +0x38 84048504 s_lshl_b32 s4, s4, 5   +0x3c/+0x40 s_ashr_i32 ...,31 (sext)
 *            +0x84 A9800002 / +0x88 A984040E s_add_nc_u64   +0x8c s_load_b128 (S#)   +0x94 s_load_b256 (T# s4)
 *            +0x9c 84048506 s_lshl_b32 s4, s6, 5   +0xa0 85069F08 s_lshr_b32 s6, s8, 31   +0xb8 98010901 s_cselect_b32 s1, s1, s9
 *            +0xd0 A980040E s_add_nc_u64   +0xd8 F4006000 s_load_b256 (T# s6)
 *   Uber     +0x24 / +0x2c the same two s_load_b64   +0x34 84068408 s_lshl_b32 s6, s8, 4   +0x38 84048504 s_lshl_b32 s4, s4, 5
 *            +0x54 85089F08 s_lshr_b32 s8, s8, 31   +0x78 s_load_b128 (S#)   +0x80 s_load_b256 (T# s4)   +0x94 980D090D s_cselect_b32 s13, s13, s9 */
/* build 0.0.447, notes S5-COVERAGE-PART2.md Q1/Q4 step 5 — TWO NEW FIELDS for the class-11 shape (AI:
 * TmuaXh_Isrc_Isrc, work/AI/TmuaXh_Isrc_Isrc.abi.json - "s4:s5 sampler-index table (class 11, apiSlot 0)"): a
 * program with a DIRECT sampler-index SGPR (every row so far) sets `samptbl = 0xff` (sentinel, "no class-11
 * pointer") and `nsamp` is unused; a class-11 program instead points s[samptbl:samptbl+1] at a TABLE of `nsamp`
 * 8-byte {idx, w1} entries (one per texture, entry i at samptbl_va + 8*i - S5-COVERAGE-PART2.md Q1's own
 * disassembly citation for AI: "+0x1c ... s[4:5], null" and "+0x2c ... s[4:5], 0x8"), each used exactly like the
 * existing direct sampler SGPR (same 0x7fffffff mask, same <<4 shift into the class-19 table's OWN sampler heap
 * at table+0x10 - NOT a heap the class-11 pointer itself owns). `samptbl != 0xff` is what tells d_table_desc to
 * take this path instead of reading a->samp directly; every EXISTING row's byte layout is unchanged (samptbl added
 * at the struct's tail, so every existing initializer list still zero-fills it to 0, which this file re-defines as
 * "not 0xff" - see the explicit { ..., 0xffu, 0u } trailer each existing row gets below, so a zero-initialized
 * struct can never be silently misread as class-11). No shipped row sets samptbl yet (AI's own identity has not
 * shipped - notes S5-COVERAGE-PART2.md Q4 step 5 / the brief for this build task: "No entry for AI yet ... make
 * the mechanism table-driven so AI's entry is one row later"), so this path is UNREACHABLE from any real program
 * today and OFF-identity holds trivially. XLAT12_TDESC_TABLE_MAX_SAMP (xlat12_ib.h) bounds nsamp exactly as
 * D_TBL_ABIS bounds ntex (2, AI's own count). */
/* build 0.0.470 (notes/design/NO-SAMPLER-CLASS10.md sections 1-2 and its ADDENDUM) — THE TWO NEW SHAPES, by GROWING
 * this struct rather than adding a second path (the design's decision: a parallel path would need a parallel branch at
 * every reader of ps_table_abi1, and a missed branch is a fail-open):
 *   - NO SAMPLER (T, AP, AR, AV, AW, AX): `samp == 0xff && samptbl == 0xff && textbl1 == 0`. The program image_loads
 *     through the table's image heap only; it never loads table+0x10 and reads no S#. Zero samplers are translated,
 *     nothing is placed at the shadow table's +0x10, and only the table pair and the texture index are redirected.
 *   - CLASS 10 (AZ): `textbl1 != 0` names the FIRST user-data slot + 1 of a 64-bit pointer to Apple's texture-INDEX
 *     table (the texture analogue of class 11's sampler-index table). Texture i's index is ONE dword at byte offset
 *     `tent[i]` of that table (NOT 8*i: AZ's own entries sit at 0x00 / 0x18 / 0x48, its ABI JSON `texture_table` and
 *     its disassembly's three `s_load_b32 ..., s[4:5], <off>`). A class-10 row's `tex[]` is {0xff, 0xff, 0xff}: any
 *     loop that forgets the class-10 branch and puts tex[i] into a slot list refuses SLOT_UNSEEN (fail-closed).
 * `tex[3]` (was tex[2]) because a class-10 row carries three textures; `textbl1` uses 0 = none (zero-fill is safe by
 * construction, unlike samptbl's 0xff sentinel). Every row is checked by d_tbl_row_ok before anything is read. */
typedef struct { uint32_t ndw, fnv; uint8_t table, ntex, tex[3], samp, samptbl, nsamp, textbl1, tent[3]; } DTableAbi;
/* build 0.0.513 ( ORDER (1),): kDTableAbi's ROWS are GENERATED - xlat12_dtable_rows.inc, written by
 * tools/auto-ws/emit_ws.py --policy adopt (main's 38 rows in main's ORDER, so every D_TBL_ABI_*_INDEX below keeps its meaning,
 * then the rows the generator adds, appended). The per-row notes that sat between the rows until 0.0.512 follow VERBATIM, in
 * the rows' order; each names its own row(s). `tools/auto-ws/emit_ws.py --check src/xlat12` (tools/conductor/suites.sh) fails
 * the day a row here drifts from the generated set. */
    /* D4-PRIME-FIXES.md item 3(b)/item 4,  — ws_P_TimgXh_Ialp
     * (re/pc-26.6.2/xlat/windowserver-r2/work/P/TimgXh_Ialp.abi.json, CONFIRMED against the JSON's own `user_data`
     * array): class 19 table at s0:s1, ONE texture at s8:s9 (api_slot 3, "texture location 3"), ONE sampler at
     * s10:s11 (api_slot 0, "sampler location 0") - the SAME GPUPass table shape with a single image instead of two.
     * GATED (xlat12_table_abi_is_gated, below): unlike every other row here, the kext only lets a match against
     * THIS row stand when its own switch is ON - see this function's own comment and AppleHardwareHook.cpp's
     * gfxsrc_pgm_profile. */
    /* build 0.0.445,  — ws_U_TvcmXh_Isrc (re/pc-26.6.2/xlat/windowserver-r2/work/U/TvcmXh_Isrc.abi.json,
     * CONFIRMED against the JSON's own `user_data` array AND its compiled gfx1201 disassembly, `llvm-objdump
     * --mcpu=gfx1201` over the SAME work/U/TvcmXh_Isrc.pal.o): class 19 table at s0:s1 (api_slot 2, "descriptor
     * table"), TWO textures - s8:s9 (api_slot 0, "texture location 0 (attachment_0)") and s10:s11 (api_slot 3,
     * "texture location 3 (img_tex_0A)") - ONE sampler at s12:s13 (api_slot 0, "sampler location 0 (img_samp_0)").
     * Disassembly (CONFIRMED, byte offsets into the .pal.o's own .text): +0x24 F4002080/F8000000 s_load_b64
     * s[2:3],s[0:1],0x0 (image heap base, table+0x0) and +0x3C F4002000/F8000010 s_load_b64 s[0:1],s[0:1],0x10
     * (sampler heap base, table+0x10) - the SAME split GPUPass/Uber/P use; +0x2C 84088508 s_lshl_b32 s8,s8,5 +
     * 0x34 86099F08 s_ashr_i32 s9,s8,31 (sext32(tex0_idx<<5)) and +0x44 840A850A s_lshl_b32 s10,s10,5 + 0x50
     * 860B9F0A s_ashr_i32 s11,s10,31 (sext32(tex3_idx<<5), same shift, second texture) and +0x80 8408840C
     * s_lshl_b32 s8,s12,4 (sext32(samp_idx<<4)); +0x74 A9880802 / +0x90 A9880A02 s_add_nc_u64 (heap base + shifted
     * index); +0x78 F4006404/F8000000 s_load_b256 s[16:23] (T# for texture 0, read by +0x98 D3C00021 image_load -
     * UNFILTERED, no sampler) and +0xAC F4006404/F8000000 s_load_b256 s[16:23] (T# for texture 3, read by +0xC8
     * E7C6C021 image_sample together with +0xA4 F4004000/F8000000 s_load_b128 s[0:3], the S#). Sampler-override
     * bit 31 checked at +0xB4/+0xBC/+0xC4 (s_lshr_b32/s_cmp_lg_u32/s_cselect_b32) - the SAME generic override
     * d_table_desc already declines on (`sv >> 31`), nothing new. Cross-checked against
     * xlat12_shader_ids.h's own ws_U_TvcmXh_Isrc row (desc_slot 0, tex_slot 8, samp_slot 12 - the row's own
     * single-tex/single-samp fields, consistent with the first of the two textures here) and
     * xlat12_shader_desc.h's row (class 3 IMAGE, n_image 2 - exactly the one image_load + one image_sample found
     * above). GATED (xlat12_table_abi_is_gated, below): needs switch 43 ON, exactly as P. */
    /* build 0.0.445,  — ws_Y_TkfhBvcmXh_Isrc (re/pc-26.6.2/xlat/windowserver-r2/work/Y/TkfhBvcmXh_Isrc.abi.json):
     * BYTE-IDENTICAL `user_data` shape to U's above (same roles at the same slots: table s0:s1 api_slot 2, texture
     * s8:s9 api_slot 0, texture s10:s11 api_slot 3, sampler s12:s13 api_slot 0) - this IS's "Y carries SIX
     * class-1/2 user words" (s8,s9,s10,s11,s12,s13): TWO textures, not one, so DTableAbi's two-texture form is
     * needed here too. CONFIRMED against Y's own disassembly (work/Y/TkfhBvcmXh_Isrc.pal.o, llvm-objdump
     * --mcpu=gfx1201): the SAME table+0x0 image-heap / table+0x10 sampler-heap split, at +0x30 F4002000/F8000000
     * s_load_b64 s[0:1],s[0:1],0x0 and +0x28 F4002180/F8000010 s_load_b64 s[6:7],s[0:1],0x10 (Y loads the SAMPLER
     * heap base first, into different registers - instruction order and register choice differ from U's, the ABI
     * shape does not); +0x38 840E840C s_lshl_b32 s14,s12,4 (sampler) and +0x3C 840A850A s_lshl_b32 s10,s10,5
     * (texture 3) with their +0x40/+0x44 s_ashr_i32 sign-extends, then +0x8C 84068508 s_lshl_b32 s6,s8,5 (texture
     * 0) - the SAME sext32(idx<<5)/sext32(idx<<4) shifts as U; +0xB0 E7C6C021 image_sample (texture-3 T# s[16:23]
     * + S# s[24:27]) and +0xC4 D3C00021 image_load (texture-0 T# s[8:15], UNFILTERED) - the SAME one-sample-plus-
     * one-load shape as U, exactly matching xlat12_shader_desc.h's row (class 3 IMAGE, n_image 2). Sampler-
     * override bit 31 checked at +0x90/+0x98/+0xA4, the same generic mechanism. Cross-checked against
     * xlat12_shader_ids.h's own ws_Y_TkfhBvcmXh_Isrc row (desc_slot 0, tex_slot 8, samp_slot 12, matching). GATED,
     * as U and P. */
    /* build 0.0.447, notes S5-COVERAGE-PART2/MIB-A1-PATH Q4 step 2 — ws_AJ_TcimXh_Isrc, ws_AK_TsplXh_Isrc,
     * ws_AL_TimgXh_Isrc (re/pc-26.6.2/xlat/windowserver-r2/work/{AJ,AK,AL}, each program's own abi.json, each
     * CONFIRMED against its own JSON `args` array AND its own compiled gfx1201 .pal.o, `llvm-objdump --mcpu=gfx1201`): all three declare
     * the IDENTICAL shape — class 19 table at s0:s1, ONE texture at s8:s9 ("texture location 3 (img_tex_0A)"), ONE
     * sampler at s10:s11 ("sampler location 0 (img_samp_0)") — the SAME shape as ws_P_TimgXh_Ialp above but with
     * the table's own pointer pair back at s0:s1 (P's row already reads {8,10} too; these three are additive, not
     * a copy of P's identity). Disassembly (byte offsets into each .pal.o's own .text, all three IDENTICAL at
     * these offsets): +0x34 F4002080/F8000010 s_load_b64 s[2:3],s[0:1],0x10 (sampler-heap base) and +0x3C
     * F4002000/F8000000 s_load_b64 s[0:1],s[0:1],0x0 (image-heap base); +0x44 8404840A s_lshl_b32 s4,s10,4
     * (sext32(samp_idx<<4)) and +0x48 84068508 s_lshl_b32 s6,s8,5 (sext32(tex_idx<<5)); +0x80/+0x84 s_add_nc_u64
     * (heap base + shifted index); +0x88 F4004301/F8000000 s_load_b128 s[12:15] (S#) and +0x90
     * F4006000/F8000000 s_load_b256 s[0:7] (T#), read together by the single `image_sample` at +0xB8 (dmask 0xf,
     * 2D, d16) — ONE image_load/image_sample pair, matching xlat12_shader_desc.h's row (class 3 IMAGE, n_image 1).
     * Sampler-override bit 31 checked at +0x9C/+0xAC/+0xB4 (s_lshr_b32/s_cmp_lg_u32/s_cselect_b32), the SAME
     * generic override d_table_desc already declines on. ndw/fnv are the identity rows' own fields, cross-checked
     * by counting dwords to each program's own `s_endpgm` (AJ 0x148+4=332B=83dw; AK 0x15C+4=352B=88dw; AL
     * 0xC0+4=196B=49dw — all three match xlat12_shader_ids.h exactly), and tex_slot=8/samp_slot=10 match each
     * row's own trailing fields. GATED (xlat12_table_abi_is_gated, below): switch 43 ON, exactly as P/U/Y. */
    /* build 0.0.447, notes S5-COVERAGE-PART2/MIB-A1-PATH Q4 step 2 — ws_AE_downsample_4_frag_lph,
     * ws_AM_narrow_blur_11_frag_lph, ws_AD_narrow_blur_23_frag_lph (re/pc-26.6.2/xlat/windowserver-r2/work/{AE,AM,AD},
     * each program's own abi.json, each CONFIRMED against its own JSON `args` array AND its own compiled gfx1201 .pal.o): all three
     * declare a DIFFERENT shape from AJ/AK/AL and P — class 19 table at s0:s1, ONE texture at s4:s5 ("texture
     * location 3 (texture0)"), ONE sampler at s6:s7 ("sampler location 0 (sampler0)"); the two raw buffer
     * pointers each JSON also carries (s8:s9 "u", s10:s11 "edr_scale") are NOT table slots, exactly as
     * xlat12_abi_ptrs.h's existing ws_AM_narrow_blur_11_frag_lph row already treats them. Disassembly (AE/AM/AD,
     * byte offsets into each .pal.o's own .text — AE and AD's sampler/image heap loads are split further apart by
     * extra `s_load_b128/b256` for the two raw pointers, the ABI shape is identical): the sampler-heap base
     * (table+0x10) and image-heap base (table+0x0) are each loaded through a `s_load_b64 s[x:x+1], s[0:1], 0x10`
     * / `0x0` pair; `s_lshl_b32 sN, s6, 4` (sext32(samp_idx<<4), samp = s6) and `s_lshl_b32 sM, s4, 5`
     * (sext32(tex_idx<<5), tex = s4); a single `image_sample`/`image_sample_l` reads the resulting T#/S# pair —
     * ONE image op each, matching xlat12_shader_desc.h's row (class 3 IMAGE, n_image 1). Sampler-override bit 31
     * checked the same generic way (`s_lshr_b32`/`s_cmp_lg_u32`/`s_cselect_b32`) in all three. ndw/fnv are the
     * identity rows' own fields, cross-checked by counting dwords to each program's own `s_endpgm` (AE
     * 0x19C+4=416B=104dw; AM 0x234+4=568B=142dw; AD 0x318+4=796B=199dw — all three match xlat12_shader_ids.h
     * exactly), and tex_slot=4/samp_slot=6 match each row's own trailing fields. GATED, switch 43, as the others. */
    /* build 0.0.448 (S5-COVERAGE-PART2.md Q1, MIB-A1-PATH.md Q4 step 2) — ws_AI_TmuaXh_Isrc_Isrc, THE FIRST
     * CLASS-11 ROW (re/pc-26.6.2/xlat/windowserver-r2/work/AI/TmuaXh_Isrc_Isrc.abi.json, CONFIRMED against the
     * JSON's own `user_data` array AND against a fresh disassembly of the SAME .pal.o with this repo's own LLVM
     * (/opt/homebrew/opt/llvm/bin/llvm-objdump -d --mcpu=gfx1201), independent of the design doc's citation):
     * class 19 table at s0:s1 (api_slot 2, "descriptor table"); TWO textures by DIRECT SGPR, s10 (api_slot 3,
     * "texture location 3 (img_tex_0A)") and s12 (api_slot 4, "texture location 4 (img_tex_1A)") - the class-19
     * table's OWN image-index words, exactly like U/Y's two-texture rows, NOT indirected through the class-11
     * pointer; ONE class-11 sampler-index-table pointer at s4:s5 (api_slot 0, "sampler-index table"), samptbl = 4,
     * nsamp = 2 (the JSON's own `sampler_table.samplers`: location 0 at entry_offset 0, location 1 at entry_offset
     * 8). Disassembly (CONFIRMED, byte offsets into the .pal.o's own .text): +0x38 F4004502/F8000000
     * s_load_b128 s[20:23], s[4:5], 0x0 (BOTH 8-byte class-11 entries in one load: s[20:21] = entry 0, s[22:23] =
     * entry 1 - the compiler batched S5-COVERAGE-PART2.md Q1's two separate s_load_dwordx2 reads into one b128);
     * +0x44 F4002380/F8000000 s_load_b64 s[14:15], s[0:1], 0x0 (image-heap base, table+0x0) and +0x4C
     * F4002000/F8000010 s_load_b64 s[0:1], s[0:1], 0x10 (sampler-heap base, table+0x10) - the SAME split every
     * other row uses; +0x54 8404850A s_lshl_b32 s4, s10, 5 (sext32(tex0_idx<<5), tex0 = s10 DIRECT, not through the
     * class-11 pointer) and +0x58 840C850C s_lshl_b32 s12, s12, 5 (tex1 = s12 DIRECT, in place); +0xB0 84068414
     * s_lshl_b32 s6, s20, 4 and +0xB4 84108416 s_lshl_b32 s16, s22, 4 (sext32(entry_idx<<4), the class-11 entries'
     * own idx words, s20 and s22); +0xC0/+0xC4 s_add_nc_u64 (sampler-heap base + shifted entry idx) and
     * +0xC8/+0xCC s_add_nc_u64 (image-heap base + shifted tex idx); +0xD4/+0xDC s_load_b128 (S# for entry 0 and
     * entry 1) and +0xE8/+0xF0 s_load_b256 (T# for tex1/s12 and tex0/s10); +0x110 E7C6C021 image_sample reads T#
     * s[4:11] (tex0/s10) with S# s[24:27] (entry 0, overridden by s21 when s20 bit 31 is set - the SAME generic
     * bit-31 override xlat12_class11_entry_read already refuses on) and +0x134 E7C6C021 image_sample reads T#
     * s[12:19] (tex1/s12) with S# s[28:31] (entry 1) - entry 0 pairs with tex0/s10 (texcoord0A) and entry 1 with
     * tex1/s12 (texcoord1A), matching the ABI JSON's sampler_table (location 0 -> entry_offset 0, location 1 ->
     * entry_offset 8) and S5-COVERAGE-PART2.md Q1's own citation. ndw/fnv cross-checked against
     * xlat12_shader_ids.h's shipped ws_AI_TmuaXh_Isrc_Isrc row (119u, 0xae6c50d3u, matching exactly) and
     * independently by counting dwords to this .pal.o's own s_endpgm (byte
     * 0x1D8 + 4 = 0x1DC = 476 bytes = 119 dwords). `samp` is the DIRECT-SGPR sentinel (0xffu, unused: class11 !=
     * 0 makes d_table_desc read samptbl/nsamp instead, never a->samp). GATED (xlat12_table_abi_is_gated, below):
     * switch 43 ON, exactly as AJ/AK/AL/AE/AM/AD. */
    /* build 0.0.454 item 3 (S6 batch 1's PENDING rows, 8e4d4c8) — ws_O_TimgXh_Ialp, ws_Q_TimgXh_Isrc,
     * ws_R_TsplXh_Isrc, ws_S_TimgXhu_Idfr (re/pc-26.6.2/xlat/windowserver-r2/work/{O,Q,R,S}, each program's own
     * abi.json `user_data`, CONFIRMED: class 19 at sgprs [0,1] api_slot 2 "descriptor table"; class 1 at [8,9]
     * api_slot 3 "texture location 3 (img_tex_0A)"; class 2 at [10,11] api_slot 0 "sampler location 0
     * (img_samp_0)"; the class-3 buffers at [4,5]/[6,7] are raw pointers, xlat12_abi_ptrs.h's rows) - P/AJ/AK/AL's
     * shape. Disassembly CONFIRMED against each program's own .pal.o (`llvm-objdump -d --mcpu=gfx1201`, this
     * repo's /opt/homebrew/opt/llvm): O and Q, IDENTICAL at these offsets: +0x24 F4002080/F8000010 s_load_b64
     * s[2:3],s[0:1],0x10 (sampler-heap base, table+0x10), +0x2C F4002000/F8000000 s_load_b64 s[0:1],s[0:1],0x0
     * (image-heap base, table+0x0), +0x34 8404840A s_lshl_b32 s4,s10,4 (sext32(samp_idx<<4), samp = s10), +0x38
     * 84068508 s_lshl_b32 s6,s8,5 (sext32(tex_idx<<5), tex = s8), +0x78 F4004301 s_load_b128 s[12:15] (S#), +0x80
     * F4006000 s_load_b256 s[0:7] (T#), +0x98 E7C6C021 image_sample, bit-31 override +0x54 85089F0A s_lshr_b32
     * s8,s10,31 / +0x8C BF078108 s_cmp_lg_u32 / +0x94 980D0B0D s_cselect_b32 s13,s13,s11; s_endpgm at +0xBC (O,
     * 48 dw) and +0xCC (Q, 52 dw). R: the same instructions 0x10 later (+0x34/+0x3C s_load_b64 0x10/0x0, +0x44
     * 8404840A, +0x48 84068508, +0x88 s_load_b128, +0x90 s_load_b256, +0xB8 image_sample; s_endpgm +0x154, 86 dw).
     * S: +0x24/+0x2C s_load_b64 0x10/0x0, +0x34 840C840A s_lshl_b32 s12,s10,4, +0x38 84088508 s_lshl_b32 s8,s8,5,
     * +0x74 F4004001 s_load_b128 s[0:3], +0x7C F4006304 s_load_b256 s[12:19], +0xA0 image_sample; s_endpgm +0x150
     * (85 dw). ndw/fnv are the identity rows' own (xlat12_shader_ids.h, S6 batch 1), cross-checked by counting to
     * each s_endpgm. ONE image op each (xlat12_shader_desc.h class 3, n_image 1). GATED (switch 43), as P/AJ/AK/AL. */
    /* build 0.0.454 item 3 — ws_W_downsample_8_frag_lph, ws_X_narrow_blur_7_frag_lph (work/{W,X}, each
     * abi.json `user_data`, CONFIRMED: class 19 at [0,1] "descriptor table"; class 1 at [4,5] api_slot 3 "texture
     * location 3 (texture0)"; class 2 at [6,7] api_slot 0 "sampler location 0 (sampler0)"; class 3 at [8,9] "u"
     * and [10,11] "edr_scale", raw pointers) - AE/AM/AD's shape. Disassembly CONFIRMED (.pal.o, gfx1201): W +0x1C
     * F4002080/F8000010 s_load_b64 s[2:3],s[0:1],0x10, +0x24 F4002000/F8000000 s_load_b64 s[0:1],s[0:1],0x0, +0x2C
     * 840C8406 s_lshl_b32 s12,s6,4 (samp = s6), +0x30 84048504 s_lshl_b32 s4,s4,5 (tex = s4), +0x78 F4004001
     * s_load_b128 s[0:3] (S#), +0x80 F4006302 s_load_b256 s[12:19] (T#), 16 image_sample_l reading that one T#/S#
     * pair (+0xDC..+0x190), override +0x88 85049F06 / +0x94 BF078104 / +0xD0 98010701; s_endpgm +0x3D4 (246 dw).
     * X: +0x24 F4002300/F8000010 s_load_b64 s[12:13],s[0:1],0x10, +0x2C F4002380/F8000000 s_load_b64
     * s[14:15],s[0:1],0x0, +0x3C 84108406 s_lshl_b32 s16,s6,4, +0x40 84048504 s_lshl_b32 s4,s4,5, +0x88 F4004506
     * s_load_b128 s[20:23], +0x90 F4006302 s_load_b256 s[12:19], 4 image_sample (+0xD0..+0xF4) on that pair,
     * override +0x5C 85069F06 / +0x64 BF078106 / +0xC4 98150715; s_endpgm +0x1D4 (118 dw). GATED (switch 43). */
    /* build 0.0.455 item 4 — ws_AS_TbdsXh_Icir_Isrc, ws_AT_TbdsXh_Isup_Isrc, ws_AU_TcimXh_Isrc, ws_AY_TimgXh_
     * IsrcN3Oc3mtc3nlnlnl (notes/design/NO-SAMPLER-CLASS10.md's reviewer ADDENDUM, S6 batches 2+3 commit 5636278,
     * itself filled from each program's own re-run ABI JSON and gfx1201 disassembly): all four fit TODAY'S
     * direct-sampler DTableAbi shape (no class-10/no-sampler code change needed) - table s0:s1, ONE OR TWO
     * textures by direct SGPR index, ONE direct sampler index; the extra class-3 buffer pointers each program's
     * JSON also carries (lod_bias / a second buffer / colorP for AY) are NOT table slots, exactly as every other
     * row here excludes its own raw-pointer arguments - xlat12_abi_ptrs.h's OWN rows for these four names (already
     * present, ws_AS_TbdsXh_Icir_Isrc/ws_AT_TbdsXh_Isup_Isrc/ws_AU_TcimXh_Isrc/ws_AY_TimgXh_IsrcN3Oc3mtc3nlnlnl,
     * ABI ptrs {0,4,6} / {0,4,6} / {0,4,6} / {0,4,6,10}) transcribe them. AS and AT declare the SAME two-texture
     * shape as ws_D_GPUPass/ws_U_TvcmXh_Isrc/ws_Y_TkfhBvcmXh_Isrc above (table s0, images s8 s10, sampler s12); AU
     * and AY declare the SAME one-texture shape as ws_AJ/ws_AK/ws_AL/ws_O/ws_Q/ws_R/ws_S above (table s0, image
     * s8, sampler s10 for AU; s8/s12 for AY - AY's own trailing fields, per the ADDENDUM table). ndw/fnv are the
     * identity rows' own fields (xlat12_shader_ids.h/xlat12_shader_desc.h, S6 batches 2+3), matching
     * xlat12_shader_desc.h's n_image count (2 for AS/AT, 1 for AU/AY) against the ADDENDUM's own ntex column.
     * AZ (TimgBdrkXhn_IsrcCrd, class 10) is DELIBERATELY NOT ADDED here: NO-SAMPLER-CLASS10.md section 2 says
     * class 10 needs a DTableAbi shape change (textbl1/tent, XLAT12_ABI_PTR_MAX growth) this build task does not
     * make; T/AP/AR/AV/AW/AX (the no-sampler shape) are likewise NOT ADDED - section 1's design (nosamp, omit
     * `sl[ns++] = a->samp`) is not built either. GATED (xlat12_table_abi_is_gated, below): switch 43 ON, exactly
     * as every other row past GPUPass/Uber. */
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 1 + ADDENDUM) — THE NO-SAMPLER ROWS, indices 23-28. Each CONFIRMED
     * by this build against its own re/pc-26.6.2/xlat/windowserver-r2/work/<label>/<name>.abi.json `user_data` - class 19 at
     * sgprs [0,1] "descriptor table", class 3 at [4,5] "buffer location 1 (uniforms)", class 1 at [6,7] "texture location
     * 0 (attachment_0)", and NO class-2 or class-11 word - and against its own .pal.o (llvm-objdump -d --mcpu=gfx1201),
     * where the ONLY loads through s[0:1] are `s_load_b64 ..., s[0:1], 0x0` (F4002000 F8000000, the image-heap base) and
     * the T# `s_load_b256 ..., s[0:1], 0x0` after it; `s_lshl_b32 s2, s6, 5` (84028506) is the texture index; the one
     * image op is an `image_load` (D3C00021) and there is NO image_sample and NO table+0x10 load in any of the six:
     *   T  (Tc3sXhu_Idst)     +0x14 s_load_b64, +0x1C s_lshl, +0x70 image_load; s_endpgm +0x198 = 103 dw, FNV 0xdb5bb19d
     *   AP (Tc4pXh_Idst)      +0x00, +0x08, +0x2C;                           s_endpgm +0x0DC =  56 dw, FNV 0xdd2af62d
     *   AR (TcimBltnXh_Icir)  +0x30, +0x38, +0xBC;                           s_endpgm +0x224 = 138 dw, FNV 0xa5000f40
     *   AV (BdsoXh)           +0x20, +0x28, +0x5C;                           s_endpgm +0x14C =  84 dw, FNV 0x4c6d57de
     *   AW (BlsoXh)           +0x20, +0x28, +0x68;                           s_endpgm +0x15C =  88 dw, FNV 0x814caab6
     *   AX (BvcmXh)           +0x14, +0x1C, +0x58;                           s_endpgm +0x2A4 = 170 dw, FNV 0x0e3b2d5a
     * ndw/fnv recomputed by this build from each .pal.o's .text and equal to the ADDENDUM and to xlat12_shader_ids.h.
     * GATED by switch 43 (kDTblAbiGated) AND by the kext's switch 51 (xlat12_table_abi_new_shape answers 1 for them). */
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2 + ADDENDUM) — THE CLASS-10 ROW, index 29: ws_AZ_TimgBdrkXhn_IsrcCrd.
     * CONFIRMED against work/AZ/TimgBdrkXhn_IsrcCrd.abi.json: `user_data` class 19 [0,1], class 10 [4,5] api 0
     * "texture-index table", class 3 [6,7] / [8,9] / [10,11], class 2 [12,13] "sampler location 0 (img_samp_0)";
     * `texture_table` reg [4,5], textures at entry_offset 0 (location 0), 24 (location 3), 72 (location 9). Against its
     * .pal.o: +0x24 F4000382 `s_load_b32 s14, s[4:5], 0x18`, +0x2C F40003C2 `s_load_b32 s15, s[4:5], 0x48`, +0x5C F4000842
     * `s_load_b32 s33, s[4:5], 0x0` (ONE dword each - so tent = {0x00, 0x18, 0x48}), then +0xA8 840E850E / +0xAC 8410850F /
     * +0xE8 84088521 `s_lshl_b32 ..., 5` (each entry is an image-heap index, x32); +0x38 F4002180 `s_load_b64 s[6:7], s[0:1],
     * 0x10` and +0x40 F4002000 `s_load_b64 s[0:1], s[0:1], 0x0` (the usual two heaps); +0x58 8408840C `s_lshl_b32 s8, s12,
     * 4` and +0x7C 850C9F0C `s_lshr_b32 s12, s12, 31` (the direct sampler and its bit-31 override, as every direct row).
     * ndw/fnv: s_endpgm +0x2D0 = 181 dw, FNV 0xf27e9612 (this build, = ADDENDUM = xlat12_shader_ids.h). textbl1 = 4 + 1.
     * GATED by switch 43 AND switch 51. */
    /* build 0.0.470 (commit 5ec3a96's message; S6 batch 4) — ws_BA_TdfgXh_Isrc and ws_BB_TbvcXh_Isrc_Isrc, indices 30-31:
     * AI's OWN class-11 shape, NOT a new shape (switch 43 only). CONFIRMED against each abi.json: class 19 [0,1], class 11
     * [4,5] api 0 "sampler-index table" (`sampler_table` entries at entry_offset 0 and 8), class 3 [6,7] / [8,9], class 1
     * [10,11] "texture location 3" and [12,13] "texture location 4". Against each .pal.o: BA +0x20 F4002102 `s_load_b64 s[4:5],
     * s[4:5], 0x0` (entry 0), +0x3C 840A850A `s_lshl_b32 s10, s10, 5`, +0x54 840C850C `s_lshl_b32 s12, s12, 5`, +0x74 840E8404
     * `s_lshl_b32 s14, s4, 4` + +0x78 85049F04 (bit 31); BB +0x2C F4004502 `s_load_b128 s[20:23], s[4:5], 0x0` (both
     * entries), +0x9C 84008414 / +0xA0 840E8416 (entry idx x16), +0x48 840C850C / +0xD0 8404850A (tex s12 / s10 x32).
     * NOTE (CONFIRMED): BA's own compile reads only entry 0 - its second image_sample (+0x114) uses an INLINE S# (s[4:7] from
     * `s_mov_b32 s4, 0x80a49` / `s5, 0x7bff00` / s6 = s7 = 0) - so for BA the class-11 step also reads and translates
     * Apple's entry 1, which our program never uses: a malformed entry 1 refuses (fail-closed), never renders wrongly.
     * ndw/fnv: BA +0x1DC = 120 dw FNV 0x8e1812e4, BB +0x338 = 207 dw FNV 0x6be37df7 (this build, = 5ec3a96 = shader_ids). */
    /* build 0.0.470 (5ec3a96) — ws_BC_TimgXh_IsrcCcl, index 32: the direct-sampler one-texture shape (AJ/AK/AL/O/Q/R/S).
     * CONFIRMED against work/BC/TimgXh_IsrcCcl.abi.json (class 19 [0,1], class 3 [4,5] / [6,7], class 1 [8,9] "texture
     * location 3", class 2 [10,11] "sampler location 0") and its .pal.o: +0x24 F4002080 `s_load_b64 s[2:3], s[0:1], 0x10`,
     * +0x2C F4002000 `s_load_b64 s[0:1], s[0:1], 0x0`, +0x34 8406840A `s_lshl_b32 s6, s10, 4`, +0x38 84088508 `s_lshl_b32 s8,
     * s8, 5`, +0x8C 85089F0A `s_lshr_b32 s8, s10, 31`, one image_sample (+0xB0); s_endpgm +0xEC = 60 dw, FNV 0xd53dee91.
     * Switch 43 only (an existing shape). */
    /* build 0.0.484 (notes/design/GLASS.md Q4) — ws_BD_glass_background_lph and ws_BE_glass_background_lph, indices 33-34:
     * the FIRST no-sampler rows with TWO textures (the 0.0.470 no-sampler shape, which d_tbl_row_ok admits for ntex <= 2,
     * had only ever shipped ntex 1). CONFIRMED against each program's own ABI JSON (the GLASS v3 build,
     * work/{BD,BE}/glass_background_lph.abi.json, identical for the two) `user_data`: class 19 at sgprs [0,1] api_slot 2
     * "descriptor table" (u32 0x00102130); class 1 at [4,5] api_slot 3 "texture location 3 (source_texture)" (0x00103012);
     * class 1 at [6,7] api_slot 4 "texture location 4 (sdf_texture)" (0x00104013); class 3 at [8,9] "buffer location 1
     * (u)" and [10,11] "buffer location 6 (edr_scale)" (raw pointers, xlat12_abi_ptrs.h's rows); NO class 2, NO class 11,
     * NO class 10. CONFIRMED against each program's own .pal.o (/opt/homebrew/opt/llvm/bin/llvm-objdump -d --mcpu=gfx1201),
     * the SAME bytes at the same offsets in BD and BE: the ONLY load through s[0:1] is +0x20 F4002600 F8000000
     * `s_load_b64 s[24:25], s[0:1], 0x0` (the image-heap base, table+0x00) - there is NO table+0x10 (sampler-heap) load;
     * +0x28 84008506 `s_lshl_b32 s0, s6, 5` + +0x34 86019F00 `s_ashr_i32 s1, s0, 31` (sdf_texture's index, s6) and +0x64
     * F4006400 F8000000 `s_load_b256 s[16:23], s[0:1], 0x0` (its T#); +0x190 84008504 `s_lshl_b32 s0, s4, 5` + +0x1A0
     * 86019F00 (source_texture's index, s4), +0x1A8 A9980018 `s_add_nc_u64 s[24:25], s[24:25], s[0:1]` and +0x324
     * F400640C F8000000 `s_load_b256 s[16:23], s[24:25], 0x0` (its T#). Every sampler is IN-SHADER: +0x6C BE8100FF
     * 007BFF00 `s_mov_b32 s1, 0x7bff00` / +0x74 BE8000FF 00082A49 `s_mov_b32 s0, 0x82a49` feed +0x80 E7C6C021
     * `image_sample ... s[16:23], s[0:3]` (GLASS.md Q4's sampler risk is about these words' VALUES, not the table shape).
     * ndw/fnv/head recomputed by this build from each .pal.o's .text through the LAST s_endpgm (BD +0x13D8 + 4 = 5084 B =
     * 1271 dw, FNV-1a 0x3858ea3a; BE +0x13D0 + 4 = 5076 B = 1269 dw, FNV-1a 0x0ca63b3c; shared head 0xbe8e017e 0xbefe1d7e
     * 0xbe82017e 0xbefd000c), equal to GLASS.md Q1 and to the tools build's regenerated xlat12_shader_ids.h rows. Two rows
     * because Apple ships two compiles of the function (BD fp16 export, BE f32; GLASS.md Q4: "shipping both avoids
     * choosing"). GATED by switch 43 (kDTblAbiGated) AND by the kext's switch 51 (xlat12_table_abi_new_shape answers 1:
     * samp and samptbl 0xff, no class-10 pointer) - so glass's draws translate only with BOTH ON, exactly as T..AX. */
    /* build 0.0.491 (: auto-ws's emit mode found identity rows with a class-19 table and NO row here) —
     * ws_Z_TimgXh_Isrc, ws_AO_TmuaXh_IsrcCcl_Icir, ws_AN_TmuaXh_Isrc_Isrc, ws_AF_variable_blur_downsample_frag_lph,
     * indices 35-38. Z is frame a's foreground program (the clock, avatar and name): without its row the table step has
     * nothing to run for it and the frame cannot commit. Found by cross-checking EVERY xlat12_shader_ids.h row against its
     * program's ABI JSON (a class-19 `user_data` word) and this table; each row below is the SAME shape as a shipped row
     * and was re-derived from BOTH the program's own re/pc-26.6.2/xlat/windowserver-r2/work/<label>/ .abi.json
     * `user_data` AND its .pal.o (/opt/homebrew/opt/llvm/bin/llvm-objdump -d --mcpu=gfx1201); ndw/fnv recomputed from each
     * .pal.o's .text through the last s_endpgm, equal to the identity row. auto-ws's candidate rows
     * (re/auto/auto-ws/emit/auto/xlat12_dtable_rows.inc) agree for Z, AN and AF, whose auto-ws objects have the SAME
     * .text; auto-ws's AO object is a different compile (fast math,) - AO's row is the SHIPPED identity's.
     *   Z  (54 dw, s_endpgm +0xD4, FNV-1a 0xf91e4dee) - AL's shape: class 19 [0,1] "descriptor table", class 1 [8,9]
     *      "texture location 3 (img_tex_0A)", class 2 [10,11] "sampler location 0 (img_samp_0)" (class 3 at [4,5] and
     *      [6,7] are raw pointers, xlat12_abi_ptrs.h). +0x24 F4002080 F8000010 s_load_b64 s[2:3], s[0:1], 0x10 (sampler
     *      heap); +0x2C F4002000 F8000000 s_load_b64 s[0:1], s[0:1], 0x0 (image heap); +0x34 8404840A s_lshl_b32 s4, s10, 4;
     *      +0x38 84068508 s_lshl_b32 s6, s8, 5; +0x3C/+0x40 s_ashr_i32 ..., 31; +0x54 85089F0A s_lshr_b32 s8, s10, 31 and
     *      +0x94 980D0B0D s_cselect_b32 s13, s13, s11 (the bit-31 override d_table_desc declines); +0x78 F4004301 s_load_b128
     *      (S#), +0x80 F4006000 s_load_b256 (T#), +0x98 E7C6C021 image_sample.
     *   AO (168 dw, s_endpgm +0x29C, 0xb4fc3c24) - the SAME `user_data` as Z/AL/BC (texture s8, sampler s10). +0x3C F4002300
     *      F8000010 s_load_b64 s[12:13], s[0:1], 0x10; +0x44 F4002000 F8000000 s_load_b64 s[0:1], s[0:1], 0x0; +0x4C 840E840A
     *      s_lshl_b32 s14, s10, 4; +0x50 84088508 s_lshl_b32 s8, s8, 5; +0xC0 F4004506 s_load_b128 (S#); +0xC8 F4006300
     *      s_load_b256 (T#); +0xD0 85009F0A s_lshr_b32 s0, s10, 31; +0x110 98150B15 s_cselect_b32 s21, s21, s11; +0x114
     *      E7C6C021 image_sample.
     *   AN (122 dw, s_endpgm +0x1E4, 0x7b3a6dfe) - AI's class-11 shape, byte-identical ABI JSON `user_data` to AI's: class
     *      19 [0,1], class 11 [4,5] "sampler-index table" (sampler_table: 2 entries, entry_offset 0 and 8), class 1 [10,11]
     *      and [12,13]. The SAME instructions at the SAME offsets as AI's .pal.o through +0x134: +0x38 F4004502 s_load_b128
     *      s[20:23], s[4:5], 0x0 (both entries); +0x44 F4002380 s_load_b64 s[14:15], s[0:1], 0x0; +0x4C F4002000 F8000010
     *      s_load_b64 s[0:1], s[0:1], 0x10; +0x54 8404850A s_lshl_b32 s4, s10, 5; +0x58 840C850C s_lshl_b32 s12, s12, 5;
     *      +0xB0 84068414 / +0xB4 84108416 s_lshl_b32 (entry indices << 4); +0x110 / +0x134 E7C6C021 image_sample.
     *   AF (173 dw, s_endpgm +0x2B0, 0x1051f3f6) - AE's shape: class 19 [0,1], class 1 [4,5] "texture location 3
     *      (texture0)", class 2 [6,7] "sampler location 0 (sampler0)". +0x18 F4002000 F8000000 s_load_b64 s[0:1], s[0:1],
     *      0x0; +0x28 84028504 s_lshl_b32 s2, s4, 5; +0x34 86039F02 s_ashr_i32 s3, s2, 31; +0x6C A9800200 s_add_nc_u64;
     *      +0x74 F4006000 s_load_b256 s[0:7] (T#). UNLIKE AE THIS COMPILE NEVER READS ITS SAMPLER: its only S# is IN-SHADER
     *      (+0x40 BE8E0080 s_mov_b32 s14, 0; +0x44 BE8D00FF 007BFF00 s_mov_b32 s13, 0x7bff00; +0x54 BE8C00FF 00082A00
     *      s_mov_b32 s12, 0x82a00; +0x64 BE8F000E s_mov_b32 s15, s14) and there is NO table+0x10 load and no read of s6. The
     *      row still carries samp s6, as the JSON declares: the S# d_table_desc translates and places is then unused by the
     *      program (harmless), and a row that dropped it would disagree with the JSON test_kdtableabi_rows.py checks. The
     *      fail-open direction (R7) is the opposite one - a row claiming no sampler for a program that reads one.
     * GATED by switch 43 (kDTblAbiGated) exactly as every row from P on; none is a new shape (xlat12_table_abi_new_shape
     * answers 0: direct or class 11), so 43 alone admits them. 43 OFF: the kext zeroes ps_table_abi1 (gfxsrc_pgm_profile),
     * which is exactly the no-row state these four had before this build. */
static const DTableAbi kDTableAbi[] = {
#include "xlat12_dtable_rows.inc"
#ifdef XLAT12_TEST_EXTRA_TBL_ROW
    /* build 0.0.471 item 3 — THE B7b TEST'S OWN ROW, TEST-ONLY. Never present in the kext build: the kext's
     * Makefile never defines XLAT12_TEST_EXTRA_TBL_ROW, so kDTableAbi has exactly 39 rows there (38 until 0.0.513, 34 until 0.0.491,
     * 32 until 0.0.484). This
     * row clones ws_AZ_TimgBdrkXhn_IsrcCrd's class-10 shape field for field (table s0, class-10 pointer s4 with
     * entries at +0x00/+0x18/+0x48, direct sampler s12) EXCEPT its `fnv` (0xffffffffu, chosen not to collide with any
     * row xlat12_abi_ptrs.h declares - re-censused: no kXlat12AbiPtrs row anywhere carries it) so xlat12_abi_ptr_row
     * finds NO row for it and d_tbl_abi_declares (its own comment) answers 0 for every slot, including textbl1 - 1u.
     * The row's OWN shape (d_tbl_row_ok) still passes: this isolates the entry-table-slot guard
     * (`class10 && !d_tbl_abi_declares(a, a->textbl1 - 1u)`) from every other rung a malformed row could fail. */
    { 181u, 0xffffffffu, 0u, 3u, { 0xffu, 0xffu, 0xffu }, 12u, 0xffu, 0u, 5u, { 0x00u, 0x18u, 0x48u } },   /* TEST ONLY: clones AZ, no ABI-pointer row */
#endif
};
#define D_TBL_ABIS (sizeof kDTableAbi / sizeof kDTableAbi[0])
/* The 1-based indices of the GATED rows above, exactly as d_table_abi_of below returns them (array position + 1) -
 * the ONLY thing xlat12_table_abi_is_gated needs to know. build 0.0.445: generalised from "P's index alone"
 * to a small list (P, U, Y) - P's OWN index and behaviour are UNCHANGED (still 3, still gated, still admitted only
 * with switch 43 ON), so this is byte-identical for P and additive for U/Y. build 0.0.447: additive again for
 * AJ/AK/AL/AE/AM/AD (indices 6-11) - P/U/Y's own indices and behaviour are UNCHANGED. If kDTableAbi's order ever
 * changes, these constants move with it; nothing outside this file may assume an index without going through
 * xlat12_table_abi_is_gated. */
#define D_TBL_ABI_P_INDEX 3u
#define D_TBL_ABI_U_INDEX 4u
#define D_TBL_ABI_Y_INDEX 5u
#define D_TBL_ABI_AJ_INDEX 6u
#define D_TBL_ABI_AK_INDEX 7u
#define D_TBL_ABI_AL_INDEX 8u
#define D_TBL_ABI_AE_INDEX 9u
#define D_TBL_ABI_AM_INDEX 10u
#define D_TBL_ABI_AD_INDEX 11u
/* build 0.0.448: AI's row (the first class-11 entry) is index 12 - additive, P/U/Y/AJ/AK/AL/AE/AM/AD's own
 * indices and behaviour are UNCHANGED. */
#define D_TBL_ABI_AI_INDEX 12u
/* build 0.0.454 item 3: S6 batch 1's six rows, indices 13-18 - additive, every earlier index UNCHANGED. */
#define D_TBL_ABI_O_INDEX 13u
#define D_TBL_ABI_Q_INDEX 14u
#define D_TBL_ABI_R_INDEX 15u
#define D_TBL_ABI_S_INDEX 16u
#define D_TBL_ABI_W_INDEX 17u
#define D_TBL_ABI_X_INDEX 18u
/* build 0.0.455 item 4: AS/AT/AU/AY, indices 19-22 - additive, every earlier index UNCHANGED. */
#define D_TBL_ABI_AS_INDEX 19u
#define D_TBL_ABI_AT_INDEX 20u
#define D_TBL_ABI_AU_INDEX 21u
#define D_TBL_ABI_AY_INDEX 22u
/* build 0.0.470: the no-sampler rows (23-28), the class-10 row (29), BA/BB (class 11, 30-31) and BC (32) - additive,
 * every earlier index UNCHANGED. */
#define D_TBL_ABI_T_INDEX 23u
#define D_TBL_ABI_AP_INDEX 24u
#define D_TBL_ABI_AR_INDEX 25u
#define D_TBL_ABI_AV_INDEX 26u
#define D_TBL_ABI_AW_INDEX 27u
#define D_TBL_ABI_AX_INDEX 28u
#define D_TBL_ABI_AZ_INDEX 29u
#define D_TBL_ABI_BA_INDEX 30u
#define D_TBL_ABI_BB_INDEX 31u
#define D_TBL_ABI_BC_INDEX 32u
/* build 0.0.484: glass BD/BE (the no-sampler shape with two textures), indices 33-34 - additive, every earlier index
 * UNCHANGED (the XLAT12_TEST_EXTRA_TBL_ROW row stays last, at xlat12_table_abi_count(), which its own test reads). */
#define D_TBL_ABI_BD_INDEX 33u
#define D_TBL_ABI_BE_INDEX 34u
/* build 0.0.491: Z, AO, AN, AF (the identity rows with a class-19 table and no row), indices 35-38 -
 * additive, every earlier index UNCHANGED (the XLAT12_TEST_EXTRA_TBL_ROW row stays last). */
#define D_TBL_ABI_Z_INDEX 35u
#define D_TBL_ABI_AO_INDEX 36u
#define D_TBL_ABI_AN_INDEX 37u
#define D_TBL_ABI_AF_INDEX 38u
/* build 0.0.513: ws_M_TextureCopy (42/0x1ddbfcad), the one row the generator ADDS (xlat12_dtable_rows.inc
 * appends it after main's 38), index 39 - additive, every earlier index UNCHANGED (the XLAT12_TEST_EXTRA_TBL_ROW row stays
 * last). Its ABI JSON (windowserver-r2/work/M/TextureCopy.abi.json) declares class 19 s0:s1 and class 1 s4 and NO sampler, so
 * it is a NO-SAMPLER shape: xlat12_table_abi_new_shape answers 1 and the kext's switch 51 gates it. THE RULE (test N4 in
 * tests/test_xlat12_ib.c, and before it every build since 0.0.470): every row past GPUPass/Uber is in kDTblAbiGated, so
 * switch 43 gates it too - 43 alone never admits a new shape and 51 alone admits nothing. */
#define D_TBL_ABI_M_INDEX 39u
/* Every kDTableAbi row switch 43 gates. A row NOT in this list (GPUPass, UberCompositeFragment) stays unconditional.
 * build 0.0.470: EVERY new row is in this list - including the no-sampler and class-10 rows, which the kext ALSO
 * gates by switch 51 (xlat12_table_abi_new_shape, derived from the row's own fields, never from this list). So 43 alone
 * never admits a new shape and 51 alone admits nothing. */
static const uint32_t kDTblAbiGated[] = { D_TBL_ABI_P_INDEX, D_TBL_ABI_U_INDEX, D_TBL_ABI_Y_INDEX,
    D_TBL_ABI_AJ_INDEX, D_TBL_ABI_AK_INDEX, D_TBL_ABI_AL_INDEX, D_TBL_ABI_AE_INDEX, D_TBL_ABI_AM_INDEX, D_TBL_ABI_AD_INDEX,
    D_TBL_ABI_AI_INDEX, D_TBL_ABI_O_INDEX, D_TBL_ABI_Q_INDEX, D_TBL_ABI_R_INDEX, D_TBL_ABI_S_INDEX, D_TBL_ABI_W_INDEX,
    D_TBL_ABI_AS_INDEX, D_TBL_ABI_AT_INDEX, D_TBL_ABI_AU_INDEX, D_TBL_ABI_AY_INDEX,
    D_TBL_ABI_X_INDEX,
    D_TBL_ABI_T_INDEX, D_TBL_ABI_AP_INDEX, D_TBL_ABI_AR_INDEX, D_TBL_ABI_AV_INDEX, D_TBL_ABI_AW_INDEX, D_TBL_ABI_AX_INDEX,
    D_TBL_ABI_AZ_INDEX, D_TBL_ABI_BA_INDEX, D_TBL_ABI_BB_INDEX, D_TBL_ABI_BC_INDEX,
    D_TBL_ABI_BD_INDEX, D_TBL_ABI_BE_INDEX,
    D_TBL_ABI_Z_INDEX, D_TBL_ABI_AO_INDEX, D_TBL_ABI_AN_INDEX, D_TBL_ABI_AF_INDEX,
    D_TBL_ABI_M_INDEX };
#define D_TBL_ABI_GATED_N (sizeof kDTblAbiGated / sizeof kDTblAbiGated[0])
#define D_TBL_RUNS 8u
#define D_TBL_NOP_HDR 0xC0001000u   /* PACKET3(NOP, count): count [29:16] = body dwords - 1 */
typedef struct {
    uint32_t run_at[D_TBL_RUNS], run_len[D_TBL_RUNS], nrun;   /* the pad runs this translation emitted before a draw */
    uint32_t patched, ppos[D_PS_UDN];                          /* user-data writes redirected, and where they sit */
    uint32_t inv_done;                                         /* M4-DESC-KEXT-PORT: the XLAT12_EXTRA_DESC_INV packet is placed */
    /* build 0.0.453 item 5: computed ONCE, at the top of xlat12_ib_translate_draw_ex (xlat12_ib_head_acquire_mem_covers
     * over THIS translation's own `in`/`n`), read only under XLAT12_EXTRA_DESC_INV_APPLE_HEAD. */
    uint32_t head_inv_ok;
    /* build 0.0.454 item 1 (XLAT12_EXTRA_TABLE_REUSE, switch 48): the translation's ONE reusable shadow - the
     * last fresh placement of a direct-sampler row. `tva` is APPLE'S table VA the placement redirected away from
     * (the export a reusing draw names); `tidx`/`sidx` Apple's index of each record the shadow holds (`tn`/`sn` of
     * `tcap`/`scap`); `A` the shadow table's dword (T# k at A+8+8k, S# q at A+8+8*tcap+4q); `mask` the user-data
     * slots whose CURRENT redirect (ppos above) was written by THIS shadow - a later fresh placement replaces the
     * whole record (or clears `valid`), so a bit here never names another placement's redirect. Written and read only
     * under the flag; zero otherwise. */
    struct { uint64_t tva; uint32_t tidx[4], sidx[2], A, mask; uint8_t valid, tn, tcap, sn, scap, table, samp, pad; } sh;
} DTable;
#define D_SH_TMAX 4u   /* the shadow's T# records at most (its tidx[]) */
#define D_SH_SMAX 2u   /* the shadow's S# records at most (its sidx[]) */
/* M4-DESC-KEXT-PORT (xlat12_ib.h XLAT12_EXTRA_DESC_INV): the upstream gfx12 per-job cache invalidate, byte for byte
 * (gfx_v12_0.c:5129-5149): PACKET3(ACQUIRE_MEM 0x58, 6), CP_COHER_CNTL 0, SIZE, SIZE_HI, BASE, BASE_HI, POLL_INTERVAL, GCR_CNTL. */
static const uint32_t kDescInv[XLAT12_DESC_INV_DWORDS] = {
    0xC0065800u, 0x00000000u, 0xFFFFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00000000u, 0x0000000Au, XLAT12_DESC_INV_GCR
};

int xlat12_desc_has_dcc(const uint32_t in[8])
{
    if (!in) return 0;
    return ((in[6] >> 21) & 1u) && (in[7] || (in[6] >> 24));   /* COMPRESSION_EN and a META_DATA_ADDRESS */
}

/* build 0.0.488 (xlat12_ib.h XLAT12_EXTRA_DCC_STRIP): 1 when `w` (Apple's marker already checked) is of the
 * DCC-DESC.md accept shape. Every clause is a refusal-direction test: a record failing any one is left untouched. */
static int d_dcc_strip_shape(const uint32_t w[8])
{
    if (!xlat12_desc_has_dcc(w)) return 0;              /* COMPRESSION_EN 1 and a non-zero metadata address */
    if ((w[3] >> 28) != 9u) return 0;                   /* TYPE 2D, the only type observed */
    if ((w[5] >> 4) & 0xFu) return 0;                   /* MAX_MIP: mip-chain DCC is out of scope */
    if ((w[3] >> 16) & 0xFu) return 0;                  /* LAST_LEVEL */
    if ((w[3] >> 12) & 0xFu) return 0;                  /* BASE_LEVEL */
    if ((w[6] >> 23) & 1u) return 0;                    /* COLOR_TRANSFORM */
    if ((w[6] >> 20) & 1u) return 0;                    /* WRITE_COMPRESS_ENABLE: a read-only view only */
    if ((w[6] >> 10) & 1u) return 0;                    /* ITERATE_256 (with metadata:'s rule keeps it, and refuses) */
    if (((w[6] >> 15) & 3u) != 2u) return 0;            /* MAX_UNCOMPRESSED_BLOCK_SIZE 256B, the one value mapped */
    return 1;
}

uint32_t xlat12_table_img_desc_ex(const uint32_t in[8], uint32_t out[8], uint32_t *dropped, uint32_t flags)
{
    uint32_t w[8], d = 0, st;
    if (dropped) *dropped = 0;
    if (!in || !out) return 0x100u | XLAT12_DESC_ERR_ARG;
    for (uint32_t k = 0; k < 8u; k++) w[k] = in[k];
    if (!(w[2] >> 31)) return XLAT12_TDESC_NOT_APPLE;              /* gfx10 RESOURCE_LEVEL "must be 1": Apple's marker */
    /* build 0.0.488 (switch 60): the DCC strip, by meaning (xlat12_ib.h XLAT12_EXTRA_DCC_STRIP). Before every other
     * rule, on Apple's own bits: the shape test reads the record as Apple wrote it. COMPRESSION_EN (bit 21) is kept. */
    if ((flags & XLAT12_EXTRA_DCC_STRIP) && d_dcc_strip_shape(w)) {
        w[7] = 0u;                                                  /* META_DATA_ADDRESS (the rest of it) */
        w[6] &= ~((0xFFu << 24) | (1u << 22) | (1u << 19));         /* META_DATA_ADDRESS_LO, ALPHA_IS_ON_MSB, META_PIPE_ALIGNED */
        w[6] = (w[6] & ~(3u << 15)) | (1u << 15);                   /* MAX_UNCOMPRESSED_BLOCK_SIZE 256B: gfx10 2 -> gfx12 1 */
        d |= 4u;
    }
    if (w[6] & (3u << 8)) { w[6] &= ~(3u << 8); d |= 1u; }         /* LLC_NOALLOC: a MALL hint, no gfx12 field (M4-DESC-TABLE.md 3) */
    if (xlat12_iterate256_droppable(w)) { w[6] &= ~(1u << 10); d |= 2u; }   /*'s rule, unchanged */
    st = xlat12_img_desc_g10_to_g12(w, out);
    if (st) return 0x100u | st;
    if (dropped) *dropped = d;
    return 0;
}

uint32_t xlat12_table_img_desc(const uint32_t in[8], uint32_t out[8], uint32_t *dropped)
{
    return xlat12_table_img_desc_ex(in, out, dropped, 0u);
}

/* 0.0.390 ( part 5 (i)): the exported input list, emptied. Called at the head of EVERY per-draw table step. */
static void d_in_clear(xlat12_draw_stats *ds)
{
    for (uint32_t z = 0; z < XLAT12_DRAW_IN_MAX; z++) { ds->in_va[z] = 0ull; ds->in_mode[z] = 0u; ds->in_proven[z] = 0u; }
    for (uint32_t z = 0; z < XLAT12_DRAW_IN_MAX; z++) ds->in_idx[z] = 0u;   /* build 0.0.553: cleared with in_va */
    ds->in_admit = 0u;   /* build 0.0.554: cleared with in_proven, the mask of inputs the stale-admit callback admitted */
    for (uint32_t z = 0; z < XLAT12_ABI_PTR_MAX; z++) { ds->in_ptr[z] = 0ull; ds->in_vptr[z] = 0ull; }
    ds->in_n = 0u; ds->in_over = 0u; ds->in_tbl_va = 0ull; ds->in_img_va = 0ull; ds->in_samp_va = 0ull; ds->in_abi = 0u;
    ds->in_nptr = 0u; ds->in_ptr_known = 0u;
    ds->in_ptr_inherit = 0u;                       /* 0.0.391 */
    ds->in_nvptr = 0u; ds->in_vptr_known = 0u;     /* 0.0.391 */
}

static uint32_t d_tbl_fail(xlat12_draw_stats *ds, uint32_t draw_at, uint32_t code)
{
    ds->err_reg = 0; ds->err_in_dword = draw_at; ds->err_op = code;
    return XLAT12_IB_ERR_DESC;
}

/* A snapshot: ndw (<= 8) dwords read twice through the caller; 0 with *code on a failed read or a difference. */
static int d_tbl_read(const xlat12_draw_extra *ex, uint64_t va, uint32_t ndw, uint32_t *w, uint32_t *code)
{
    uint32_t b[8];
    if (!ex->desc_read(ex->desc_ctx, va, ndw, w) || !ex->desc_read(ex->desc_ctx, va, ndw, b)) { *code = XLAT12_TDESC_READ; return 0; }
    for (uint32_t k = 0; k < ndw; k++) if (w[k] != b[k]) { *code = XLAT12_TDESC_UNSTABLE; return 0; }
    return 1;
}

static uint64_t d_tbl_off(uint32_t v, uint32_t sh) { return (uint64_t)(int64_t)(int32_t)(v << sh); }   /* s_lshl + s_ashr 31 */

/* build 0.0.448 (review addition item 8) — a class-11 row's OWN shape, checked BEFORE any placement:
 * `nsamp` must be nonzero (a row with samptbl set but nothing to place would still redirect the class-11 SGPR
 * pair at a table it wrote no entries into) and must equal `ntex` (S5-COVERAGE-PART2.md Q1: entry i sits at
 * samptbl_va + 8*i, one per texture, by construction - a mismatched count places the wrong number of entries for
 * the images it pairs them with). Pure and exported so a test can drive it directly, exactly like
 * xlat12_class11_entry_read below: d_table_desc calls this SAME function, never a reimplementation. */
int xlat12_class11_row_ok(uint32_t ntex, uint32_t nsamp)
{
    return nsamp != 0u && nsamp == ntex;
}

/* build 0.0.447 (S5-COVERAGE-PART2.md Q1) — see xlat12_ib.h's own comment. d_table_desc's class-11 path below
 * calls this SAME function; nothing here is a reimplementation for tests to drift from. */
uint32_t xlat12_class11_entry_read(const xlat12_draw_extra *ex, uint64_t samptbl_va, uint32_t i, uint32_t *idx)
{
    uint32_t ent[2], code = 0;
    if (!d_tbl_read(ex, samptbl_va + 8ull * i, 2u, ent, &code)) return code;
    if (ent[0] >> 31) return XLAT12_TDESC_SAMP_OVERRIDE;
    if (idx) *idx = ent[0];
    return 0;
}

/* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2) — see xlat12_ib.h's own comment. d_table_desc's class-10 path calls
 * this SAME function. ONE dword at textbl_va + byte offset `off`, read TWICE (d_tbl_read: READ / UNSTABLE). No bit-31
 * rule: a class-10 entry is an image-heap index the program shifts by 5 and sign-extends (AZ's `s_lshl_b32 ..., 5`, no
 * s_lshr 31 on it), exactly like a direct texture index, which carries no override either. */
uint32_t xlat12_class10_entry_read(const xlat12_draw_extra *ex, uint64_t textbl_va, uint32_t off, uint32_t *idx)
{
    uint32_t w = 0, code = 0;
    if (!ex || !ex->desc_read) return XLAT12_TDESC_READ;
    if (!d_tbl_read(ex, textbl_va + (uint64_t)off, 1u, &w, &code)) return code;
    if (idx) *idx = w;
    return 0;
}

/* build 0.0.470 — THE SHAPE OF A ROW, from its own fields (never from a hand list). */
static int d_tbl_c10(const DTableAbi *a) { return a->textbl1 != 0u; }
static int d_tbl_c11(const DTableAbi *a) { return a->samptbl != 0xffu; }
static int d_tbl_nosamp(const DTableAbi *a) { return a->samp == 0xffu && a->samptbl == 0xffu && a->textbl1 == 0u; }

/* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2, "Row check (TOO_MANY)") — a row's OWN shape, checked before
 * anything is read, so a malformed row is a build-time defect refused TOO_MANY, never placed. Every row: table pair in
 * range, 1 <= ntex. CLASS 10: not also class 11, no nsamp, ntex <= 3, the textbl pair in range, a DIRECT sampler in
 * range (AZ's shape; a class-10 no-sampler row was never designed), tex = {0xff,0xff,0xff}, tent 8-aligned and strictly
 * increasing over ntex and zero beyond. EVERY OTHER ROW (direct, no-sampler, class 11): ntex <= 2 (tex[2] is class 10's
 * alone), each tex slot in range, tent all zero; class 11 keeps 0.0.447/0.0.448's own rules (nsamp <= the cap and
 * xlat12_class11_row_ok, the samptbl pair in range); a no-sampler row has nsamp 0; a direct row a sampler slot in range.
 * Every row shipped before 0.0.470 passes (test_xlat12_ib.c N4 asserts it for every row), so this changes no answer
 * for them. */
static int d_tbl_row_ok(const DTableAbi *a)
{
    if (a->table + 1u >= D_PS_UDN || a->ntex < 1u) return 0;
    if (d_tbl_c10(a)) {
        if (d_tbl_c11(a) || a->nsamp || a->ntex > 3u) return 0;
        if (a->textbl1 >= D_PS_UDN) return 0;   /* the pair (textbl1 - 1, textbl1) must both be PS slots */
        if (a->samp >= D_PS_UDN) return 0;
        for (uint32_t i = 0; i < 3u; i++) if (a->tex[i] != 0xffu) return 0;
        for (uint32_t i = 0; i < 3u; i++) {
            if (i >= a->ntex) { if (a->tent[i]) return 0; continue; }
            if (a->tent[i] & 7u) return 0;
            if (i && a->tent[i] <= a->tent[i - 1u]) return 0;
        }
        return 1;
    }
    if (a->ntex > 2u) return 0;
    for (uint32_t i = 0; i < a->ntex; i++) if (a->tex[i] >= D_PS_UDN) return 0;
    for (uint32_t i = 0; i < 3u; i++) if (a->tent[i]) return 0;
    if (d_tbl_c11(a))
        return a->nsamp <= XLAT12_TDESC_TABLE_MAX_SAMP && xlat12_class11_row_ok(a->ntex, a->nsamp) && a->samptbl + 1u < D_PS_UDN;
    if (d_tbl_nosamp(a)) return a->nsamp == 0u;
    return a->samp < D_PS_UDN;
}

/* build 0.0.470 — the S# records a row places: class 11 one per entry (0.0.447), the no-sampler shape NONE, every
 * other row (direct, class 10) exactly one. */
static uint32_t d_tbl_nsamp_eff(const DTableAbi *a)
{
    if (d_tbl_c11(a)) return a->nsamp < 2u ? a->nsamp : 2u;
    return d_tbl_nosamp(a) ? 0u : 1u;
}

/* build 0.0.470 — THE ONE `need` (dwords a fresh placement's body must hold, before alignment and the optional
 * invalidate): [table 8][T# 8 x ntex][S# 4 x nsampEff], then class 11's [entries {k,0} x nsamp] (0.0.447) or class 10's
 * entry region [tent[ntex-1]/4 + 2 dwords, zero, entry i = {i,0} at +tent[i]/4] (NO-SAMPLER-CLASS10.md section 2: AZ
 * 8 + 24 + 4 + 20 = 56). The no-sampler shape: 8 + 8*ntex (T: 16). d_table_desc and xlat12_table_abi_need both call
 * this, so a test of the export is a test of the placement. */
static uint32_t d_tbl_need(const DTableAbi *a)
{
    uint32_t need = 8u + 8u * a->ntex + 4u * d_tbl_nsamp_eff(a);
    if (d_tbl_c11(a)) need += 2u * a->nsamp;
    if (d_tbl_c10(a)) need += (uint32_t)a->tent[a->ntex - 1u] / 4u + 2u;
    return need;
}

/* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2, "Apple's entry-table page must be named") — 1 when this row's
 * ABI-pointer row exists and declares `slot` as the first slot of one of its pointers. d_table_desc requires it for a
 * class-10 row's textbl slot and a class-11 row's samptbl slot (AI retroactively): the entry table is a page the program
 * reads, and the ABI-pointer export is the only place it is named. */
static int d_tbl_abi_declares(const DTableAbi *a, uint32_t slot)
{
    const xlat12_abi_ptrs *ap = xlat12_abi_ptr_row(a->ndw, a->fnv);
    uint32_t over = 0u;
    const uint32_t np = xlat12_abi_ptr_n(ap, &over);
    if (!ap || over) return 0;
    for (uint32_t q = 0; q < np; q++) if (ap->slot[q] == slot) return 1;
    return 0;
}

/* build 0.0.454 item 1 (XLAT12_EXTRA_TABLE_REUSE, switch 48; xlat12_ib.h's own comment on the flag). 1 when
 * user-data slot k still holds the reusable shadow's redirect: the shadow wrote it (sh.mask) and no later write
 * reached the slot (ud_pos is still the redirect's own position, ppos). */
static int d_sh_holds(const DTable *tb, const DPair *p, uint32_t k)
{
    return k < D_PS_UDN && tb->sh.valid && ((tb->sh.mask >> k) & 1u) && ((p->ud_seen >> k) & 1u) && p->ud_pos[k] == tb->ppos[k];
}

/* 1 when slot k is one of row a's OWN table / texture / sampler slots and still holds the shadow's redirect. */
static int d_sh_row_holds(const DTableAbi *a, const DTable *tb, const DPair *p, uint32_t k)
{
    int row = (k == a->table || k == a->table + 1u || k == a->samp);
    /* build 0.0.470: widened to tex[3]. Reached only for a row d_sh_reuse_ok admitted (never a new shape); a
     * class-10 row's tex[] is 0xff, which no slot k < D_PS_UDN ever equals. */
    for (uint32_t i = 0; i < a->ntex && i < 3u; i++) if (k == a->tex[i]) row = 1;
    return row && d_sh_holds(tb, p, k);
}

/* May the draw whose in-force row is `a` keep the reusable shadow? The flag, a live shadow, a direct-sampler row
 * naming the SAME table and sampler slots, the table pair still holding the shadow's redirect, and every texture /
 * sampler slot either still holding it or written in THIS region. A slot written only in an EARLIER region (not by
 * us) answers 0: the draw takes the fresh path, which refuses it exactly as before (SLOT_SHARED, or REDIRECTED). */
static int d_sh_reuse_ok(const xlat12_draw_extra *ex, const DTableAbi *a, const DPair *p, const DTable *tb, uint32_t region_from)
{
    if (!ex || !(ex->flags & XLAT12_EXTRA_TABLE_REUSE) || !tb->sh.valid || a->samptbl != 0xffu || a->ntex > 2u) return 0;
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 6): the two new shapes never reuse a shadow (and never record one:
     * their placement sets sh.valid 0, below). Explicit, not left to the ntex/samp checks that happen to exclude them. */
    if (d_tbl_nosamp(a) || d_tbl_c10(a)) return 0;
    if (a->table != tb->sh.table || a->samp != tb->sh.samp) return 0;
    if (!d_sh_holds(tb, p, a->table) || !d_sh_holds(tb, p, a->table + 1u)) return 0;
    uint32_t sl[3], ns = 0;
    sl[ns++] = a->samp;
    for (uint32_t i = 0; i < a->ntex; i++) {
        if (a->tex[i] == a->table || a->tex[i] == a->table + 1u || a->tex[i] == a->samp) return 0;   /* malformed row */
        sl[ns++] = a->tex[i];
    }
    for (uint32_t i = 0; i < ns; i++) {
        const uint32_t k = sl[i];
        if (d_sh_holds(tb, p, k)) continue;
        if (k >= D_PS_UDN || !((p->ud_seen >> k) & 1u) || p->ud_pos[k] < region_from) return 0;
    }
    return 1;
}

/* 1 when an un-redirected PS slot k this translation wrote still carries exactly that write as far as the
 * re-emission carry can tell: no redirect of ours sits there and the carry (Apple's last raw value for the slot,
 * cleared whole by a non-proven CONTEXT_CONTROL) equals the output dword at the slot's last write. Such a declared
 * ABI-pointer slot is not re-emitted by a REUSING draw and is exported as known by it. Needs the carry. */
static int d_sh_abi_known(const xlat12_draw_extra *ex, const DTable *tb, const DPair *p, const uint32_t *out, uint32_t k)
{
    if (!ex || !(ex->flags & XLAT12_EXTRA_UD_REEMIT) || !ex->ud_carry || k >= D_PS_UDN) return 0;
    if (!((p->ud_seen >> k) & 1u)) return 0;
    if (((tb->patched >> k) & 1u) && p->ud_pos[k] == tb->ppos[k]) return 0;
    if (!((ex->ud_carry->ps_ok >> k) & 1u)) return 0;
    return ex->ud_carry->ps_val[k] == out[p->ud_pos[k]];
}

/* build 0.0.455 item 1 (XLAT12_EXTRA_VS_KNOWN, switch 52; 's known-slot rule; xlat12_ib.h's own
 * comment on the flag). 1 when a VERTEX user-data slot k this translation SAW still carries exactly the write the
 * carry last recorded for it: vs_ok set, vs_val[k] == out[pos[k]]. Mirrors d_sh_abi_known's PS test, but takes the
 * raw (seen, pos) pair rather than a DPair so d_readset_stage - which only ever holds that pair, generically for
 * both stages - can call it too. UNLIKE d_sh_abi_known, no "redirect of ours" exclusion is needed: DTable's own
 * redirect bookkeeping (`patched`/`ppos`) is PS-only (d_table_desc never redirects a vertex slot), so there is no
 * such redirect for a vertex slot to be confused with. */
static int d_vs_slot_known(const xlat12_draw_extra *ex, uint32_t seen, const uint32_t *pos, const uint32_t *out, uint32_t k)
{
    if (!ex || !(ex->flags & XLAT12_EXTRA_VS_KNOWN) || !ex->ud_carry || k >= D_VS_UDN) return 0;
    if (!((seen >> k) & 1u)) return 0;
    if (!((ex->ud_carry->vs_ok >> k) & 1u)) return 0;
    return ex->ud_carry->vs_val[k] == out[pos[k]];
}

/* build 0.0.480 (XLAT12_EXTRA_UNIT) — the unit's own state, or NULL when the flag is off: every unit site tests it. */
#define D_NOINLINE __attribute__((noinline))
static xlat12_unit *d_unit_of(const xlat12_draw_extra *ex)
{
    return (ex && (ex->flags & XLAT12_EXTRA_UNIT)) ? ex->unit : 0;
}

/* P3: open the next deferred block for the placement d_table_desc is about to build, and record everything its
 * placement will need: the storage layout [8][T# x tcap][S# x scap][entries ext_len] (for a reuse-capable direct row
 * under switch 48 the full D_SH_TMAX / D_SH_SMAX capacity, since the room it will have is not known yet - d_unit_finish
 * places only what is used), its alignment, and the OUTPUT dwords its VA must be written to once placed (the table
 * pointer pair, and the class-10 texture-entry or class-11 sampler-entry pointer pair). Returns the zeroed storage, or
 * NULL when the pending list is full or the layout does not fit a slot (d_table_desc then refuses TOO_MANY: never
 * truncated, never overwritten). *tcap / *scap are the layout d_table_desc writes. */
static D_NOINLINE uint32_t *d_unit_pend(xlat12_unit *U, const DTableAbi *a, const DPair *p, int grow, uint32_t nsampEff,
                                        uint32_t need, uint32_t alignMask, uint32_t draw_at, uint32_t *tcap, uint32_t *scap)
{
    const uint32_t cap = (U->pend_cap && U->pend_cap < XLAT12_UNIT_PEND_MAX) ? U->pend_cap : XLAT12_UNIT_PEND_MAX;
    if (U->npend >= cap) return 0;
    const int c10 = d_tbl_c10(a), c11 = d_tbl_c11(a);
    if (grow) { *tcap = D_SH_TMAX; *scap = D_SH_SMAX; }
    const uint32_t sStore = (c10 || c11 || d_tbl_nosamp(a)) ? nsampEff : *scap;
    const uint32_t ext = need - (8u + 8u * a->ntex + 4u * nsampEff);
    if (8u + 8u * *tcap + 4u * sStore + ext > XLAT12_UNIT_PEND_DW) return 0;
    xlat12_unit_pend *b = &U->pend[U->npend];
    b->off = U->npend * XLAT12_UNIT_PEND_DW;
    b->kind = c10 ? 10u : c11 ? 11u : 0u;
    b->tcap = *tcap; b->tn = a->ntex; b->scap = sStore; b->sn = nsampEff; b->nsamp_eff = nsampEff; b->ext_len = ext;
    b->align_mask = alignMask; b->draw_at = draw_at;
    b->tpos[0] = p->ud_pos[a->table]; b->tpos[1] = p->ud_pos[a->table + 1u];
    b->epos[0] = 0xFFFFFFFFu; b->epos[1] = 0xFFFFFFFFu;
    if (c10) { b->epos[0] = p->ud_pos[a->textbl1 - 1u]; b->epos[1] = p->ud_pos[a->textbl1]; }
    else if (c11) { b->epos[0] = p->ud_pos[a->samptbl]; b->epos[1] = p->ud_pos[a->samptbl + 1u]; }
    b->placed_len = 0u;
    uint32_t *B = &U->dw[b->off];
    for (uint32_t k = 0; k < XLAT12_UNIT_PEND_DW; k++) B[k] = 0u;
    U->npend++;
    return B;
}

/* Gather, translate and place the class-19 records of the fragment program in force at THIS draw; redirect its user data. */
/* ---- build 0.0.500 (xlat12_ib.h XLAT12_EXTRA_DRAW_ELIDE, switch 66; notes/design/DRAW-ELIDE.md Q4) -------------------
 * THE POLICY TABLE. A row names a fragment program by its kDTableAbi identity (ndw, fnv), the class that enables it
 * (ex->draw_elide_rows), and whether its refused texture must be a stripped DCC record. Every row's program is store-free:
 * CONFIRMED on its gfx1201 object (re/pc-26.6.2/xlat/windowserver-r2/work/<P>/<name>.pal.o, `llvm-objdump -d --mcpu=gfx1201`,
 *: U TvcmXh_Isrc - image_load 1, image_sample 1, global_load_d16_b16 1, export mrt0 1; Y TkfhBvcmXh_Isrc -
 * image_load 1, image_sample 1, export mrt0 2; AO TmuaXh_IsrcCcl_Icir - image_sample 1, ds_param_load 8, export mrt0 1; not one
 * *_store*, *_atomic* or scratch store in any of the three. SUSPECTED only that the blob's substituted images equal those
 * objects byte for byte (DRAW-ELIDE.md "what I could not establish"). */
/* build 0.0.512: `nl_tex` - 0xFF = the LAST-texture rule (every row before this build); otherwise the ONE texture index the
 * row's refusal may be on while later textures exist (the NOT-LAST case, XLAT12_DE_CLASS_GLASS): every later texture must then
 * be proven by the table step's own probe (d_de_probe_rest), and a refusal on any other texture - its last included - refuses.
 * Moved above d_table_desc (unchanged rows) so the table step can ask it; BD and BA added. */
typedef struct { uint32_t ndw, fnv, require_dcc, cls, row, nl_tex; } DDrawElideRow;
static const DDrawElideRow kDrawElideRows[] = {
    { 192u, 0x92c6ae13u, 1u, XLAT12_DE_CLASS_UY, XLAT12_DE_ROW_U,  0xFFu },   /* ws_U_TvcmXh_Isrc: the clock box over X */
    { 372u, 0x5276813bu, 1u, XLAT12_DE_CLASS_UY, XLAT12_DE_ROW_Y,  0xFFu },   /* ws_Y_TkfhBvcmXh_Isrc: the clock's second layer */
    { 168u, 0xb4fc3c24u, 0u, XLAT12_DE_CLASS_AO, XLAT12_DE_ROW_AO, 0xFFu },   /* ws_AO_TmuaXh_IsrcCcl_Icir: the login panel material */
    /* build 0.0.512 (xlat12_ib.h XLAT12_DE_CLASS_GLASS): the clock's glass passes, both store-free
     * (their gfx1201 objects, CONFIRMED: BD image_sample 2 / image_sample_l 5 / global_load_d16 9 / export mrt0 2;
     * BA image_sample 2 / export mrt0 1; no store, atomic or scratch op in either). BD's kDTableAbi row: textures s4 s6, no
     * sampler - its texture 1 (s6) is the frozen distance surface, the LAST texture; BA's: textures s10 (tex0, the distance
     * surface) and s12 (tex1, the colour ramp) - the refusal it may carry is on texture 0 only (nl_tex 0). */
    { 1271u, 0x3858ea3au, 1u, XLAT12_DE_CLASS_GLASS, XLAT12_DE_ROW_BD, 0xFFu },   /* ws_BD_glass_background_lph */
    { 120u, 0x8e1812e4u, 1u, XLAT12_DE_CLASS_GLASS, XLAT12_DE_ROW_BA, 0u },       /* ws_BA_TdfgXh_Isrc: NOT-LAST, texture 0 */
};
/* The enabled row for a kDTableAbi identity (the LAST match, as the loop always took), or 0. Pure. */
static const DDrawElideRow *d_de_row(const xlat12_draw_extra *ex, uint32_t ndw, uint32_t fnv)
{
    const DDrawElideRow *r = 0;
    for (uint32_t k = 0; k < sizeof kDrawElideRows / sizeof kDrawElideRows[0]; k++)
        if (kDrawElideRows[k].ndw == ndw && kDrawElideRows[k].fnv == fnv && (ex->draw_elide_rows & kDrawElideRows[k].cls))
            r = &kDrawElideRows[k];
    return r;
}

/* build 0.0.512 (XLAT12_DE_CLASS_GLASS, the NOT-LAST case) — THE PROBE. Asked by the table step ONLY at a PROVENANCE refusal on
 * texture `from - 1` of an enabled not-last row (d_de_row(..)->nl_tex == from - 1): every LATER texture of the row is read (the
 * same double read), translated (the same port, the same flags) and asked (the same ask, chosen exactly as the loop chooses it).
 * 1 only when every one of them is PROVEN (the ask answered 1 - a linear record admitted unproven is NOT proven). It writes no
 * byte of `out`, no export, no counter, no placement: its translations go to its own scratch and are thrown away. Noinline:
 * d_table_desc's frame does not carry its locals. */
static D_NOINLINE uint32_t d_de_probe_rest(const xlat12_draw_extra *ex, const DTableAbi *a, uint64_t ibase, const uint32_t *aidx,
                                           uint32_t from)
{
    uint32_t rec[8], g[8], code = 0u, dropped = 0u;
    if (from >= a->ntex) return 0u;
    for (uint32_t i = from; i < a->ntex && i < 3u; i++) {
        if (!d_tbl_read(ex, ibase + d_tbl_off(aidx[i], 5u), 8u, rec, &code)) return 0u;
        dropped = 0u;
        if (xlat12_table_img_desc_ex(rec, g, &dropped, ex->flags)) return 0u;
        const uint32_t dcc = (dropped >> 2) & 1u;
        const uint32_t mode = (g[3] >> 20) & 0x1Fu;
        const uint64_t sva = ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8);
        const uint32_t elemBytes = xlat12_format_elem_bytes((rec[1] >> 20) & 0x1FFu);
        uint32_t clamp = 0u;
        const int proven = dcc ? (ex->desc_dcc_ok ? (ex->desc_dcc_ok(ex->desc_ctx, sva, mode, elemBytes) == 1) : 0)
                         : ex->desc_tiled_okt ? (ex->desc_tiled_okt(ex->desc_ctx, sva, mode, elemBytes, rec, &clamp) == 1)
                         : ex->desc_tiled_ok ? (ex->desc_tiled_ok(ex->desc_ctx, sva, mode, elemBytes) == 1) : 0;
        if (!proven) return 0u;
    }
    return 1u;
}
/* The table step's side of the not-last case (called only at a PROVENANCE refusal on texture `i`, only under the flag): records in
 * ds->de_nl whether this refusal is an enabled not-last row's own texture and whether every later texture proved. Noinline. */
static D_NOINLINE void d_de_nl_note(const xlat12_draw_extra *ex, const DTableAbi *a, uint64_t ibase, const uint32_t *aidx,
                                    uint32_t i, xlat12_draw_stats *ds)
{
    if (!(ex->draw_elide_rows & XLAT12_DE_CLASS_GLASS) || i + 1u >= a->ntex || i >= 3u) return;
    const DDrawElideRow *r = d_de_row(ex, a->ndw, a->fnv);
    if (!r || r->nl_tex != i) return;
    ds->de_nl = (uint8_t)((i + 1u) | (d_de_probe_rest(ex, a, ibase, aidx, i + 1u) ? 0x10u : 0u));
}
/* build 0.0.554 (ADMIT-STALE-112.md section 3) - THE STALE-ADMIT ASK, out of line: d_table_desc's frame (its stack budget)
 * does not carry its locals, and six arguments keep every one in a register (the record's VA and element size are re-derived from `rec`
 * exactly as the caller derives them). Called ONLY from d_table_desc's image loop, only for a TILED record that is NOT DCC-stripped and that
 * every proof ask refused, and only with ex->desc_stale_ok non-NULL. Returns 0 (no), 1 (admitted) or 2 (admitted, and the callback asked
 * for the mip-0 clamp); only the callback's exact 1 admits (any other value, including a negative one, is a no). Counts the ask and the yes. */
static D_NOINLINE uint32_t d_stale_admit(const xlat12_draw_extra *ex, const DTableAbi *a, xlat12_draw_stats *ds, uint32_t at_i,
                                         const uint32_t *rec, uint32_t mode)
{
    uint32_t cl = 0u;
    const uint64_t sva = ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8);
    ds->stale_asked++;
    if (ex->desc_stale_ok(ex->desc_ctx, ((uint64_t)a->ndw << 32) | a->fnv, at_i, rec, sva, mode,
                          xlat12_format_elem_bytes((rec[1] >> 20) & 0x1FFu), &cl) != 1) return 0u;
    ds->stale_admitted++;
    return cl == 1u ? 2u : 1u;
}
/* build 0.0.536 fix round: noinline, pinning 0.0.535's code shape - the one-block retry in xlat12_ib_translate_draw_ex made the
 * compiler start inlining this into it (+0x150 on that frame); out of line it costs exactly what it cost at 0.0.535. */
static __attribute__((noinline)) uint32_t d_table_desc(const xlat12_draw_profile *cp, const xlat12_draw_extra *ex, const DPair *p, DTable *tb, uint32_t *out,
                             xlat12_draw_stats *ds, uint32_t draw_at, uint32_t region_from)
{
    /* 0.0.390 ( part 5 (i)): CLEAR the exported input list first, so a refusal here can never leave the previous
     * draw's list standing for the caller to read as this draw's. Nothing below reads any of these fields. The caller
     * clears it too, before the rung that refuses without entering this function at all. */
    d_in_clear(ds);
    if (cp->ps_table_abi1 > D_TBL_ABIS) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    const DTableAbi *a = &kDTableAbi[cp->ps_table_abi1 - 1u];
    ds->in_abi = cp->ps_table_abi1;
    /* build 0.0.447 (S5-COVERAGE-PART2.md Q1/Q4 step 5) — `class11` is 1 for a program whose sampler arrives
     * through a POINTER to a table of index entries (AI's shape; see the DTableAbi struct's own comment) instead
     * of a direct index SGPR (every row shipped so far). sl[] grows to 6 to hold BOTH slots of that pointer beside
     * table/tex, where the direct-SGPR shape only ever needed 5 (table x2, tex x2, samp x1). */
    const int class11 = d_tbl_c11(a);
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md sections 1-2): the two new shapes, from the row's own fields. */
    const int class10 = d_tbl_c10(a), nosamp = d_tbl_nosamp(a);
    /* a malformed row (more entries than this file tracks, XLAT12_TDESC_TABLE_MAX_SAMP) is a build-time defect,
     * not a stream defect - refused the same way an out-of-range ABI-pointer slot already is (TOO_MANY), rather
     * than silently truncated. Unreachable today: every shipped row has class11 == 0. */
    if (class11 && a->nsamp > XLAT12_TDESC_TABLE_MAX_SAMP) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    /* build 0.0.448 (review addition item 8) — a class-11 row's OWN shape must be internally
     * consistent BEFORE any placement: `nsamp == 0` would place zero entries while the redirect below still
     * points the class-11 SGPR pair at a (degenerate) entries table, and `nsamp != ntex` would place a different
     * number of entries than there are images to pair them with (S5-COVERAGE-PART2.md Q1: "entry i sits at
     * samptbl_va + 8*i", one per texture, by construction) - either is a BUILD-TIME row defect, refused the same
     * way the entry-count-over-max row above is, never silently placed or truncated. AI's real row (ntex 2,
     * nsamp 2) is unaffected; unreachable for every shipped row today (samptbl == 0xff everywhere else). */
    if (class11 && !xlat12_class11_row_ok(a->ntex, a->nsamp)) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    /* build 0.0.470: the WHOLE row's shape (d_tbl_row_ok, which repeats the two class-11 rules above), and - for a
     * class-10 or class-11 row - the entry table's page must be declared by the program's ABI-pointer row, or nothing
     * names that page to the dependency rule (NO-SAMPLER-CLASS10.md section 2; AI's, BA's and BB's rows declare s4). */
    if (!d_tbl_row_ok(a)) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    if (class10 && !d_tbl_abi_declares(a, a->textbl1 - 1u)) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    if (class11 && !d_tbl_abi_declares(a, a->samptbl)) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    uint32_t sl[6], ns = 0, code = 0;
    /* build 0.0.454 item 1 (switch 48): does this draw keep the translation's reusable shadow? 0 without the
     * flag, so every line below is the fresh path's, byte for byte. A slot that still holds the shadow's redirect
     * is not a SLOT_SHARED write of Apple's: its APPLE value is the one the shadow recorded (tva / tidx / sidx). */
    const int reuse = d_sh_reuse_ok(ex, a, p, tb, region_from);
    /* build 0.0.470: the slot list per shape (at most 6: class 11 table 2 + tex 2 + samptbl 2; class 10 table 2 +
     * textbl 2 + samp 1; direct table 2 + tex 2 + samp 1; no-sampler table 2 + tex 2). A class-10 row's texture indices
     * live in Apple's entry table, not in user data, so its textbl pair takes their place; a no-sampler row names NO
     * sampler slot at all (the design's N2: a stream that writes none translates). */
    sl[ns++] = a->table; sl[ns++] = a->table + 1u;
    if (class10) { sl[ns++] = a->textbl1 - 1u; sl[ns++] = a->textbl1; }
    else for (uint32_t i = 0; i < a->ntex && i < 2u; i++) sl[ns++] = a->tex[i];
    if (class11) { sl[ns++] = a->samptbl; sl[ns++] = a->samptbl + 1u; } else if (!nosamp) { sl[ns++] = a->samp; }
    for (uint32_t i = 0; i < ns; i++) {
        if (reuse && d_sh_holds(tb, p, sl[i])) continue;
        if (sl[i] >= D_PS_UDN || !((p->ud_seen >> sl[i]) & 1u)) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_SLOT_UNSEEN);
        if (p->ud_pos[sl[i]] < region_from) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_SLOT_SHARED);
    }
    /* build 0.0.508 (item 8) - THE INDEX-POINTER RUNG, ungated (a refusal). A no-sampler row
     * with ONE texture at s6 (the `{6,0,0}` rows T, AP, AR, AV, AW, AX) reads s6 as an image-heap index. When THIS
     * translation wrote the index slot's high word (s7) NON-ZERO, s6:s7 is a 64-bit value and heap base + 32*s6 names no
     * record the program reads: refuse before anything is read or placed. s7 is never a redirected slot (only the table
     * pair and the index are), so its output dword is Apple's own. A slot 7 this translation did not write is unchanged
     * (the brief's rung is "non-zero", and the census over run10p/run10q found s7 written 0 at every such draw). */
    if (nosamp && a->ntex == 1u && a->tex[0] == 6u && ((p->ud_seen >> 7u) & 1u) && out[p->ud_pos[7u]] != 0u)
        return d_tbl_fail(ds, draw_at, XLAT12_TDESC_IDX_PTR);
    /* the DIRECT-SGPR shape's own index, refused here exactly as before; the class-11 shape's per-texture indices
     * are read a few lines below instead (`sIdx[]`), each refused the SAME way (bit 31 = override). */
    uint32_t sv = 0;
    /* build 0.0.470: a no-sampler row reads no sampler slot (it has none); sv stays 0 and is used nowhere. */
    if (!class11 && !nosamp) {
        if (reuse && d_sh_holds(tb, p, a->samp)) {
            const uint32_t q = out[p->ud_pos[a->samp]];              /* the shadow's S# position it redirected to */
            if (q >= tb->sh.sn) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_UNSTABLE);
            sv = tb->sh.sidx[q];                                      /* Apple's index, checked for bit 31 when placed */
        } else {
            sv = out[p->ud_pos[a->samp]];
            if (sv >> 31) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_SAMP_OVERRIDE);
        }
    }
    const uint64_t tva = reuse ? tb->sh.tva
                               : ((uint64_t)out[p->ud_pos[a->table]] | ((uint64_t)out[p->ud_pos[a->table + 1u]] << 32));
    /* 0.0.390 ( part 5 (ii)): THE RAW-POINTER PAGES THIS PROGRAM'S ABI DECLARES, read from the stream's own
     * user-data writes at the slots xlat12_abi_ptrs.h transcribes from the ABI JSON roles. A program with no row, or a
     * declared slot the stream never wrote, leaves `in_ptr_known` 0 / counts an INHERITED slot - both of which the kext's
     * rule refuses on. Read-only: nothing below consults these and no output dword changes.
     *
     * 0.0.391 ( conditions 2 and 4). TWO CHANGES, both of them the review's:
     *   (2) a declared slot this stream did not write is no longer folded into `in_over`. It is INHERITED - the value
     *       lives in an SGPR some earlier submission left - and that is a different fact from "the list did not fit",
     *       which is what `in_over` means. Both still refuse; they refuse under different names now. A slot INDEX out of
     *       range is a malformed ABI row, i.e. a list problem, and stays `in_over`.
     *   (4) the VERTEX stage's declared pointers are gathered too, from its own user-data record. Through 0.0.390 the
     *       three vertex rows of xlat12_abi_ptrs.h were reachable by no call site at all. */
    {
        const xlat12_abi_ptrs *ap = xlat12_abi_ptr_row(a->ndw, a->fnv);
        if (ap) {
            ds->in_ptr_known = 1u;
            /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2): the count through xlat12_abi_ptr_n - a row declaring
             * more pointers than in_ptr[] holds sets in_over (refused), never silently loses its tail. */
            const uint32_t np = xlat12_abi_ptr_n(ap, &ds->in_over);
            for (uint32_t q = 0; q < np; q++) {
                const uint32_t s0 = ap->slot[q];
                if (s0 + 1u >= D_PS_UDN) { ds->in_over = 1u; continue; }
                /* build 0.0.454 item 1 (switch 48): a REUSING draw names what a fresh placement would - Apple's
                 * table VA for the table pair the shadow still holds (the pair's output dwords are OUR redirect), and
                 * an earlier region's un-redirected write the carry still holds (d_sh_abi_known: the SGPR already
                 * carries that value; the re-emission step skipped it for this draw). Anything else is unchanged. */
                if (reuse && s0 == a->table && d_sh_holds(tb, p, s0) && d_sh_holds(tb, p, s0 + 1u)) {
                    ds->in_ptr[ds->in_nptr++] = tb->sh.tva;
                    continue;
                }
                int known = ((p->ud_seen >> s0) & 1u) && ((p->ud_seen >> (s0 + 1u)) & 1u) &&
                            p->ud_pos[s0] >= region_from && p->ud_pos[s0 + 1u] >= region_from;
                if (!known && reuse)
                    known = ((((p->ud_seen >> s0) & 1u) && p->ud_pos[s0] >= region_from) || d_sh_abi_known(ex, tb, p, out, s0)) &&
                            ((((p->ud_seen >> (s0 + 1u)) & 1u) && p->ud_pos[s0 + 1u] >= region_from) ||
                             d_sh_abi_known(ex, tb, p, out, s0 + 1u));
                if (!known) { ds->in_ptr_inherit++; continue; }
                ds->in_ptr[ds->in_nptr++] = (uint64_t)out[p->ud_pos[s0]] | ((uint64_t)out[p->ud_pos[s0 + 1u]] << 32);
            }
        }
        const xlat12_abi_ptrs *vp = cp->vs_abi_ptr1 && cp->vs_abi_ptr1 <= XLAT12_ABI_PTR_ROWS
                                    ? &kXlat12AbiPtrs[cp->vs_abi_ptr1 - 1u] : 0;
        if (vp && vp->stage == 1u) {
            ds->in_vptr_known = 1u;
            const uint32_t nvp = xlat12_abi_ptr_n(vp, &ds->in_over);   /* build 0.0.470: as the fragment loop */
            for (uint32_t q = 0; q < nvp; q++) {
                const uint32_t s0 = vp->slot[q];
                if (s0 + 1u >= D_VS_UDN) { ds->in_over = 1u; continue; }
                /* build 0.0.455 item 1 (switch 52, 's known-slot rule): a slot written EARLIER in
                 * THIS translation is no longer unconditionally INHERITED - if the carry still holds exactly that
                 * write (d_vs_slot_known), the SGPR still carries Apple's value and the slot is known. OFF (or with
                 * no carry), d_vs_slot_known always answers 0 and this is 0.0.454's expression, character for
                 * character. */
                const int k0 = ((p->vsud_seen >> s0) & 1u) &&
                               (p->vsud_pos[s0] >= region_from || d_vs_slot_known(ex, p->vsud_seen, p->vsud_pos, out, s0));
                const int k1 = ((p->vsud_seen >> (s0 + 1u)) & 1u) &&
                               (p->vsud_pos[s0 + 1u] >= region_from || d_vs_slot_known(ex, p->vsud_seen, p->vsud_pos, out, s0 + 1u));
                if (!k0 || !k1) { ds->in_ptr_inherit++; continue; }
                if (p->vsud_pos[s0] < region_from || p->vsud_pos[s0 + 1u] < region_from) ds->vs_known_n++;
                ds->in_vptr[ds->in_nvptr++] = (uint64_t)out[p->vsud_pos[s0]] | ((uint64_t)out[p->vsud_pos[s0 + 1u]] << 32);
            }
        } else if ((ex->flags & XLAT12_EXTRA_READSET) && cp->vs_readset1 && cp->vs_readset1 <= XLAT12_READSET_COUNT) {
            /* 0.0.444 (C5-RING-REVIEW.md (B) item K(i), D1 hole) — NO xlat12_abi_ptrs.h ROW for this
             * VERTEX program (I, G, V and attr - a table-bound draw's own vertex programs - have none;'s own
             * finding). Through 0.0.443 this left `in_vptr_known` 0 unconditionally, which n48_cp_build_consumer
             * (gfx_cp_build.h) reads as `over` for the WHOLE segment - a table draw with one of these four vertex
             * programs could never be admitted, however clean the rest of the frame. Fall back to the SAME proven
             * read-set row (xlat12_readset.h) d_readset_stage already trusts for the non-table path, under the
             * IDENTICAL admission rule: `proof_depth1_data_only == 1` and, only if the row declares an inline
             * image, `proof_images_inline == 1` too. This is not a weaker proof than the ABI-row path above - it is
             * a DIFFERENT, independently-generated proof of the same fact (every load is a depth-1, data-only read
             * from a proven base; any image op is provably inline). `cp->vs_readset1` is set unconditionally at
             * profile time (xlat12_ib_profile_stage/_for, D4-PRIME.md item 2).
             * 0.0.446 ( fix (7)) — GATED BEHIND SWITCH 40. Through 0.0.445 this fallback ran whatever
             * switch 40 said, so it widened the dependency judgment for I/G/V/attr table draws with 40 OFF - a D4'
             * proof admitted on a boot that never turned D4' on. `ex->flags & XLAT12_EXTRA_READSET` is how this
             * library sees switch 40: the kext sets that flag exactly when its per-pass D4' latch is on
             * (gXpD4Frame.d4, switch 40 with 28), and never otherwise. 40 OFF is 0.0.443 again for these draws
             * (`in_vptr_known` stays 0, the consumer reads `over`, the draw is refused); 40 ON keeps 0.0.444's
             * fallback unchanged. `ex` is non-null here: this function runs only under XLAT12_EXTRA_TABLE_DESC. */
            const xlat12_readset_row *rrow = &kXlat12Readset[cp->vs_readset1 - 1u];
            const uint32_t rHasImage = (rrow->inl_tex != 0xffu || rrow->desc_table != 0u) ? 1u : 0u;
            if (rrow->proof_depth1_data_only == 1u && (!rHasImage || rrow->proof_images_inline == 1u)) {
                ds->in_vptr_known = 1u;
                /* build 0.0.470: the same no-silent-truncation rule for a read-set row (tools/gfx-readset.py
                 * already refuses to emit one over XLAT12_READSET_PTR_MAX; this makes the kext side agree). */
                if (rrow->nptr > XLAT12_READSET_PTR_MAX || rrow->nptr > XLAT12_ABI_PTR_MAX) ds->in_over = 1u;
                for (uint32_t q = 0; q < rrow->nptr && q < XLAT12_READSET_PTR_MAX && q < XLAT12_ABI_PTR_MAX; q++) {
                    const uint32_t s0 = rrow->slot[q];
                    if ((uint32_t)s0 + 1u >= D_VS_UDN) { ds->in_over = 1u; continue; }
                    /* build 0.0.455 item 1 (switch 52, 's known-slot rule): the SAME fallback as
                     * the ABI-pointer branch above - a slot written earlier in this translation is known when the
                     * carry still holds exactly that write. OFF (or with no carry), byte-identical to 0.0.454. */
                    const int k0 = ((p->vsud_seen >> s0) & 1u) &&
                                   (p->vsud_pos[s0] >= region_from || d_vs_slot_known(ex, p->vsud_seen, p->vsud_pos, out, s0));
                    const int k1 = ((p->vsud_seen >> (s0 + 1u)) & 1u) &&
                                   (p->vsud_pos[s0 + 1u] >= region_from || d_vs_slot_known(ex, p->vsud_seen, p->vsud_pos, out, s0 + 1u));
                    if (!k0 || !k1) { ds->in_ptr_inherit++; continue; }
                    if (p->vsud_pos[s0] < region_from || p->vsud_pos[s0 + 1u] < region_from) ds->vs_known_n++;
                    ds->in_vptr[ds->in_nvptr++] = (uint64_t)out[p->vsud_pos[s0]] | ((uint64_t)out[p->vsud_pos[s0 + 1u]] << 32);
                }
            }
            /* An unadmitted (or absent) read-set row leaves `in_vptr_known` 0, exactly 0.0.443's decline. */
        }
    }
    /* build 0.0.470: g[3][8] (a class-10 row places three T#). */
    uint32_t h0[2], h1[2], rec[8], g[3][8], sr[4], sg[2][4], lin = 0, dropped = 0, nllc = 0, nit = 0;
    uint32_t sIdx[2] = { 0u, 0u };   /* class-11 only: each entry's own (mask-checked) sampler index, read below */
    if (!d_tbl_read(ex, tva + XLAT12_DESC_TABLE_IMG_OFF, 2u, h0, &code) || !d_tbl_read(ex, tva + XLAT12_DESC_TABLE_SAMP_OFF, 2u, h1, &code))
        return d_tbl_fail(ds, draw_at, code);
    const uint64_t ibase = (uint64_t)h0[0] | ((uint64_t)h0[1] << 32), sbase = (uint64_t)h1[0] | ((uint64_t)h1[1] << 32);
    /* build 0.0.447 (S5-COVERAGE-PART2.md Q1) — the class-11 pointer's OWN entries, read here (beside the two
     * heap bases, BEFORE the image loop) for the SAME reason 0.0.394 moved the direct-SGPR sampler's address here:
     * a draw refused at XLAT12_TDESC_PROVENANCE inside the image loop below must still export in_samp_va, not leave
     * it stale at 0. Entry i (0-based) sits at samptbl_va + 8*i (S5-COVERAGE-PART2.md Q1: "+0x1c ... null" /
     * "+0x2c ... 0x8"), is READ TWICE like every other class-19 record (d_tbl_read), and is refused on bit 31 -
     * the SAME generic override rule the direct-SGPR path checks above, applied per entry instead of once. */
    uint64_t samptbl_va = 0ull;
    if (class11) {
        samptbl_va = (uint64_t)out[p->ud_pos[a->samptbl]] | ((uint64_t)out[p->ud_pos[a->samptbl + 1u]] << 32);
        for (uint32_t i = 0; i < a->nsamp && i < 2u; i++) {
            const uint32_t st = xlat12_class11_entry_read(ex, samptbl_va, i, &sIdx[i]);
            if (st) return d_tbl_fail(ds, draw_at, st);
        }
    }
    /* 0.0.390; 0.0.394: the S# is set HERE, beside the other two, not after the image loop.
     * measured the mask: a draw refused at XLAT12_TDESC_PROVENANCE returned from the loop below BEFORE the old assignment,
     * so in_samp_va stayed 0 and gfx_cp_build.h's `fixed[3]` read `over` from it - the rule answered `list-overflow` on
     * every provenance refusal instead of reaching `R1-tiled-unproven`. All three fixed heap pages are now named the moment
     * the table and both heaps are known, so the consumer enumerates with all three whatever rung refuses next. The build
     * of `sbase + d_tbl_off(sv, 4u)` is unchanged and is still what the S# read a few lines below uses.
     * build 0.0.447: for class-11, `in_samp_va` names entry 0's own resolved heap address, the same address
     * texture 0's S# resolves to below - a DIFFERENT (documented) choice from the direct-SGPR case for nsamp > 1,
     * since xlat12_draw_stats carries one in_samp_va field; unreached today (no shipped row sets samptbl). */
    ds->in_tbl_va = tva; ds->in_img_va = ibase;
    ds->in_samp_va = sbase + (uint64_t)d_tbl_off(class11 ? sIdx[0] : sv, 4u);
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 1, risk R6 - the field is OVERLOADED for this shape): a no-sampler
     * program reads NO S#, so there is no sampler address to name; the TABLE's own VA stands in. It keeps gfx_cp_build.h's
     * `fixed[3]` from ever reading 0 (which is `over`), and names no page that is not already named (in_tbl_va's), which the
     * consumer's merge accepts. sbase (table+0x10, still read above: the same record, the same refusals) is not used. */
    if (nosamp) ds->in_samp_va = tva;
    if (a->ntex > XLAT12_DRAW_IN_MAX) ds->in_over = 1u;
    /* build 0.0.454 item 1 (switch 48): APPLE'S image index per texture. Fresh (and every draw without the
     * flag): the slot's own output dword, exactly the value read here before. A reusing draw whose texture slot still
     * holds the shadow's redirect reads the shadow record's own Apple index instead - the SAME record is then read,
     * translated, exported and sent through the provenance ask below as a fresh placement would. */
    uint32_t aidx[3] = { 0u, 0u, 0u };   /* build 0.0.470: 3 (class 10) */
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2): a CLASS-10 row's image indices are Apple's entry-table dwords,
     * one at textbl_va + tent[i] each, read TWICE (xlat12_class10_entry_read) - read here, AFTER the three fixed pages are
     * exported above, so a refused entry read still names them. Nothing of the table is written yet. */
    uint64_t textbl_va = 0ull;
    if (class10) {
        textbl_va = (uint64_t)out[p->ud_pos[a->textbl1 - 1u]] | ((uint64_t)out[p->ud_pos[a->textbl1]] << 32);
        for (uint32_t i = 0; i < a->ntex && i < 3u; i++) {
            const uint32_t st = xlat12_class10_entry_read(ex, textbl_va, a->tent[i], &aidx[i]);
            if (st) return d_tbl_fail(ds, draw_at, st);
        }
    }
    for (uint32_t i = 0; !class10 && i < a->ntex && i < 3u; i++) {
        const uint32_t k = a->tex[i];
        if (reuse && d_sh_holds(tb, p, k)) {
            const uint32_t r = out[p->ud_pos[k]];
            if (r >= tb->sh.tn) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_UNSTABLE);
            aidx[i] = tb->sh.tidx[r];
        } else aidx[i] = out[p->ud_pos[k]];
    }
    for (uint32_t i = 0; i < a->ntex; i++) {
        if (!d_tbl_read(ex, ibase + d_tbl_off(aidx[i], 5u), 8u, rec, &code)) return d_tbl_fail(ds, draw_at, code);
        /* build 0.0.512 Part B: the read-only T# observer (xlat12_ib.h tex_note); NULL is today's path */
        if (ex->tex_note) ex->tex_note(ex->desc_ctx, ((uint64_t)a->ndw << 32) | a->fnv, (draw_at & 0xFFFFFFu) | (i << 24), rec,
                                       ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8));
        /* build 0.0.531 item 4b: the same observer with Apple's heap index (xlat12_ib.h tex_note_ix); NULL is today's path */
        if (ex->tex_note_ix) ex->tex_note_ix(ex->desc_ctx, ((uint64_t)a->ndw << 32) | a->fnv, (draw_at & 0xFFFFFFu) | (i << 24),
                                             aidx[i], rec);
        // build 0.0.488 (switch 60): the T# step with the flags, so XLAT12_EXTRA_DCC_STRIP can strip a DCC record
        // (dropped bit 2). OFF, xlat12_table_img_desc_ex(.., flags) is xlat12_table_img_desc: the strip is not reached.
        const uint32_t st = xlat12_table_img_desc_ex(rec, g[i], &dropped, ex->flags);
        if (st) {
            if ((ex->flags & XLAT12_EXTRA_DCC_STRIP) && xlat12_desc_has_dcc(rec)) ds->dcc_refused++;
            return d_tbl_fail(ds, draw_at, st);
        }
        nllc += dropped & 1u; nit += (dropped >> 1) & 1u;
        const uint32_t dcc = (dropped >> 2) & 1u;         /* build 0.0.488: a stripped DCC record */
        const uint32_t mode = (g[i][3] >> 20) & 0x1Fu;   /* gfx12 SQ_IMG_RSRC_WORD3 SW_MODE [24:20] */
        const uint64_t sva = ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8);
        // build 0.0.451 item 2 (S4): this T#'s OWN element size, from ITS gfx10 FORMAT field (rec[1] [28:20],
        // the SAME bits xlat12_img_desc_g10_to_g12 reads), through the translator's format table.
        const uint32_t elemBytes = xlat12_format_elem_bytes((rec[1] >> 20) & 0x1FFu);
        /* build 0.0.486 (switch 59): the T#-aware ask when the caller wired it (else exactly the ask as before). A yes
         * with a clamp sets MAX_MIP / LAST_LEVEL to 0 in g[i] - OUR translated copy, never `rec` - HERE, before the export,
         * the placement and the switch-48 shadow compare below, so a reusing draw compares the clamped record it placed.
         * build 0.0.488: a stripped record asks desc_dcc_ok ONLY (never desc_tiled_ok NOR desc_tiled_okt, both of which
         * a residency copy can answer), whatever its mode; NULL refuses; `clamp` stays 0 for it. Every other record asks
         * exactly what it always did. MERGE 0.0.489: the stripped-record branch is tested FIRST, so 0.0.486's ask never sees
         * a stripped record, and a record that is not stripped takes 0.0.486's path unchanged. */
        uint32_t clamp = 0u;
        int proven = dcc ? (ex->desc_dcc_ok ? (ex->desc_dcc_ok(ex->desc_ctx, sva, mode, elemBytes) == 1) : 0)
                         : ex->desc_tiled_okt ? (ex->desc_tiled_okt(ex->desc_ctx, sva, mode, elemBytes, rec, &clamp) == 1)
                         : ex->desc_tiled_ok ? (ex->desc_tiled_ok(ex->desc_ctx, sva, mode, elemBytes) == 1) : 0;
        if (proven && mode && clamp == 1u) xlat12_tdesc_clamp_mip0(g[i]);
        /* build 0.0.554 (ADMIT-STALE-112.md section 3): THE STALE-ADMIT, between the ask and the export. Only a TILED
         * record that is NOT stripped and that every ask just refused is offered to ex->desc_stale_ok (NULL: never); a stripped record is
         * never offered (counted in stale_dcc). A yes makes the input PROVEN from here on - what the export and the refusal test below
         * read - and marks it in in_admit; the callback's clamp request clamps OUR translated copy exactly as desc_tiled_okt's does. Every
         * later rung (the other textures, the S#, the placement, the dependency gate, the copy guard) still runs. */
        if (!proven && mode && ex->desc_stale_ok) {
            if (dcc) ds->stale_dcc++;
            else {
                const uint32_t r = d_stale_admit(ex, a, ds, (draw_at & 0xFFFFFFu) | (i << 24), rec, mode);
                if (r) { proven = 1; ds->in_admit |= 1u << i; if (r == 2u) xlat12_tdesc_clamp_mip0(g[i]); }
            }
        }
        /* 0.0.390 ( part 5 (i)): EXPORT THE INPUT, whatever the rungs below then do with it. Recorded BEFORE the
         * provenance refusal, so a refused draw's list is the list it actually gathered up to that point - which is what
         * the rule needs in order to say WHICH input was unproven rather than only that one was. */
        if (i < XLAT12_DRAW_IN_MAX) {
            ds->in_va[i] = sva; ds->in_mode[i] = mode; ds->in_proven[i] = proven ? 1u : 0u;
            ds->in_idx[i] = aidx[i];   /* build 0.0.553: the index whose record gave sva; export only */
            if (ds->in_n <= i) ds->in_n = i + 1u;
        }
        if (dcc) { ds->dcc_stripped++; if (!proven) ds->dcc_unproven++; }   /* build 0.0.488 */
        if ((mode || dcc) && !proven) {   /* build 0.0.488: `|| dcc` - a stripped record is never admitted unproven (0.0.554: `proven` includes an admission) */
            ds->prov_va = sva; ds->prov_mode = mode; ds->prov_path = 2u;          /*: name the surface */
            /* build 0.0.512: the not-last case's probe (XLAT12_DE_CLASS_GLASS only; nothing else reaches it) */
            if (ex->flags & XLAT12_EXTRA_DRAW_ELIDE) d_de_nl_note(ex, a, ibase, aidx, i, ds);
            return d_tbl_fail(ds, draw_at, XLAT12_TDESC_PROVENANCE);
        }
        if (!mode && !proven) lin++;
    }
    /* 0.0.394: in_samp_va was already set above, beside in_tbl_va/in_img_va, so a provenance refusal enumerates
     * all three fixed pages. This is still the S# this draw actually reads and the value is identical.
     * build 0.0.447 (S5-COVERAGE-PART2.md Q1, "translate each S#"): the direct-SGPR shape still reads/translates
     * exactly ONE S# (nsampEff 1, unchanged loop trip count and unchanged addresses - OFF-identity for every
     * existing row); the class-11 shape reads/translates ONE S# per entry already validated above (sIdx[]). */
    /* build 0.0.470: d_tbl_nsamp_eff - class 11's per-entry count as before, 1 for direct and class-10 rows (as
     * before for direct), and 0 for a no-sampler row: it reads and translates NO S#. */
    const uint32_t nsampEff = d_tbl_nsamp_eff(a);
    for (uint32_t i = 0; i < nsampEff; i++) {
        const uint32_t idx = class11 ? sIdx[i] : sv;
        if (!d_tbl_read(ex, sbase + d_tbl_off(idx, 4u), 4u, sr, &code)) return d_tbl_fail(ds, draw_at, code);
        const uint32_t st = xlat12_samp_desc_g10_to_g12(sr, sg[i]);
        if (st) return d_tbl_fail(ds, draw_at, 0x110u | st);
    }
    /* build 0.0.454 item 1 (switch 48): A REUSING DRAW PLACES NOTHING NEW. Everything above - the slot rungs,
     * the exports, the reads, the translations and the provenance ask for EVERY T# this draw reads - ran exactly as
     * for a fresh placement. Here: a record the shadow still holds must translate to the bytes it placed (else
     * UNSTABLE); a texture / sampler written in THIS region reuses an identical record already in the shadow or is
     * appended to its spare space (none left: NO_ROOM), and its in-region index is rewritten to that record - and
     * recorded as a redirect (patched/ppos) exactly as a fresh placement's indices are, so the REDIRECTED rung
     * protects every later draw from it. Checked in full before anything is written. */
    if (reuse) {
        /* build 0.0.480: under XLAT12_EXTRA_UNIT the shadow is a DEFERRED block (sh.A is its pending index) whose
         * records live in the caller's storage until the unit is placed; in place it is out[sh.A..], as always. */
        uint32_t *const SB = (ex->flags & XLAT12_EXTRA_UNIT) ? &ex->unit->dw[ex->unit->pend[tb->sh.A].off] : &out[tb->sh.A];
        const uint32_t T0 = 8u, S0 = 8u + 8u * tb->sh.tcap;
        uint32_t rr[2] = { 0u, 0u }, held = 0, nt = tb->sh.tn, nsn = tb->sh.sn, q = 0;
        for (uint32_t i = 0; i < a->ntex && i < 2u; i++) {
            const uint32_t k = a->tex[i];
            uint32_t r = 0xFFFFFFFFu;
            if (d_sh_holds(tb, p, k)) {
                r = out[p->ud_pos[k]];                                    /* < tn: checked where aidx was read */
                for (uint32_t w = 0; w < 8u; w++) if (SB[T0 + 8u * r + w] != g[i][w]) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_UNSTABLE);
                held |= 1u << i;
            } else {
                for (uint32_t z = 0; z < tb->sh.tn && r == 0xFFFFFFFFu; z++) {
                    int same = tb->sh.tidx[z] == aidx[i];
                    for (uint32_t w = 0; w < 8u && same; w++) same = SB[T0 + 8u * z + w] == g[i][w];
                    if (same) r = z;
                }
                if (r == 0xFFFFFFFFu && i == 1u && rr[0] >= tb->sh.tn && aidx[1] == aidx[0]) {   /* appended just above */
                    int same = 1;
                    for (uint32_t w = 0; w < 8u; w++) same = same && g[1][w] == g[0][w];
                    if (same) r = rr[0];
                }
                if (r == 0xFFFFFFFFu) {
                    if (nt >= tb->sh.tcap || nt >= D_SH_TMAX) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_NO_ROOM);
                    r = nt++;
                }
            }
            rr[i] = r;
        }
        if (d_sh_holds(tb, p, a->samp)) {
            q = out[p->ud_pos[a->samp]];                                  /* < sn: checked where sv was read */
            for (uint32_t w = 0; w < 4u; w++) if (SB[S0 + 4u * q + w] != sg[0][w]) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_UNSTABLE);
            held |= 4u;
        } else {
            q = 0xFFFFFFFFu;
            for (uint32_t z = 0; z < tb->sh.sn && q == 0xFFFFFFFFu; z++) {
                int same = tb->sh.sidx[z] == sv;
                for (uint32_t w = 0; w < 4u && same; w++) same = SB[S0 + 4u * z + w] == sg[0][w];
                if (same) q = z;
            }
            if (q == 0xFFFFFFFFu) {
                if (nsn >= tb->sh.scap || nsn >= D_SH_SMAX) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_NO_ROOM);
                q = nsn++;
            }
        }
        /* checked: write the appended records, then rewrite and record the in-region indices */
        for (uint32_t i = 0; i < a->ntex && i < 2u; i++) {
            if ((held >> i) & 1u) continue;
            const uint32_t k = a->tex[i], r = rr[i];
            if (r >= tb->sh.tn) { for (uint32_t w = 0; w < 8u; w++) SB[T0 + 8u * r + w] = g[i][w]; tb->sh.tidx[r] = aidx[i]; }
            out[p->ud_pos[k]] = r;
            tb->patched |= 1u << k; tb->ppos[k] = p->ud_pos[k]; tb->sh.mask |= 1u << k;
        }
        if (!(held & 4u)) {
            const uint32_t k = a->samp;
            if (q >= tb->sh.sn) { for (uint32_t w = 0; w < 4u; w++) SB[S0 + 4u * q + w] = sg[0][w]; tb->sh.sidx[q] = sv; }
            out[p->ud_pos[k]] = q;
            tb->patched |= 1u << k; tb->ppos[k] = p->ud_pos[k]; tb->sh.mask |= 1u << k;
        }
        ds->table_appended += (nt - tb->sh.tn) + (nsn - tb->sh.sn);
        tb->sh.tn = (uint8_t)nt; tb->sh.sn = (uint8_t)nsn;
        if (ex->flags & XLAT12_EXTRA_UNIT) { ex->unit->pend[tb->sh.A].tn = nt; ex->unit->pend[tb->sh.A].sn = nsn; }   /* build 0.0.480 */
        ds->table_draws++; ds->table_img += a->ntex; ds->table_samp += 1u; ds->table_llc += nllc; ds->table_iter256 += nit;
        ds->table_lin_unproven += lin; ds->table_reused++;
        ds->err_reg = 0; ds->err_in_dword = 0; ds->err_op = 0xFFFFFFFFu;
        return 0;
    }
    /* place: one pad run becomes PACKET3(NOP) whose body carries [table 8][T# 8 x ntex][S# 4 x nsampEff], 32-byte
     * aligned - and, for class-11 ONLY, a further [entries {k,0} x nsamp] (S5-COVERAGE-PART2.md Q1/Q4 step 5's own
     * placement list). With XLAT12_EXTRA_DESC_INV the translation's FIRST placement starts its run with the 8-dword
     * cache invalidate, then the NOP. `need`'s extra term is 0 for every existing (non-class-11) row: OFF-identity
     * holds exactly as before this build task. */
    /* build 0.0.453 item 5: skip ONLY when the flag asks AND this translation's own head-check (computed once,
     * at the top of xlat12_ib_translate_draw_ex) confirmed Apple's own segment-head ACQUIRE_MEM already covers
     * every cache XLAT12_DESC_INV_GCR does - never merely because the flag is set. */
    const uint32_t inv = ((ex->flags & XLAT12_EXTRA_DESC_INV) && !tb->inv_done &&
                          !((ex->flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD) && tb->head_inv_ok)) ? XLAT12_DESC_INV_DWORDS : 0u;
    /* build 0.0.470: d_tbl_need - byte-identical to the expression it replaces for every earlier row (the class-10
     * entry-region term is 0 unless textbl1 is set, the S# term 0 only for a no-sampler row). */
    const uint32_t need = d_tbl_need(a);
    /* build 0.0.454 item 2 (switch 48; xlat12_ib.h XLAT12_EXTRA_TABLE_REUSE (a) for the rule and its sources):
     * 16-byte alignment with the flag, 32 without it (byte-identical OFF). */
    const int reuseFlag = (ex->flags & XLAT12_EXTRA_TABLE_REUSE) != 0u;
    const uint64_t alignMask = reuseFlag ? 15ull : 31ull;
    /* build 0.0.480 (P3, xlat12_ib.h XLAT12_EXTRA_UNIT): a unit DEFERS the placement. The records are built below
     * exactly as today, into the caller's pending storage instead of a pad run (`B`), and every VA-dependent word
     * (the two heap bases, the entry pointers, the table pointer redirect) is written 0 here and rewritten by
     * d_unit_finish once the block has a place. `A` is then the block's pending index, which is what sh.A names. */
    xlat12_unit *const U = d_unit_of(ex);
    uint32_t r = 0, A = 0, L = 0, *B;
    /* build 0.0.454 item 1 (switch 48): a direct-sampler row's T# and S# arrays grow into the run's spare
     * dwords (T# first, up to D_SH_TMAX, then S#, up to D_SH_SMAX), so a later draw can reuse this shadow. `need`
     * above is still the minimum, so a placement fits exactly when it did before. Without the flag, and for a
     * class-11 row, tcap = ntex and scap = 1: the layout the previous line of code always built. */
    /* build 0.0.470: the two new shapes never grow a reusable shadow either: tcap = ntex, scap = nsampEff. */
    uint32_t tcap = a->ntex, scap = (class10 || nosamp) ? nsampEff : 1u;
    if (U) {
        /* the pending list is capped, and one more placement REFUSES - it is never truncated or overwritten; an
         * invalidate still owed here would be one the unit never placed (d_unit_inv places it first): refuse too */
        A = U->npend;
        B = inv ? 0 : d_unit_pend(U, a, p, reuseFlag && !class11 && !class10 && !nosamp, nsampEff, need, (uint32_t)alignMask,
                                   draw_at, &tcap, &scap);
        if (!B) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_TOO_MANY);
    } else {
        for (; r < tb->nrun; r++) {
            const uint32_t p0 = tb->run_at[r], L0 = tb->run_len[r];
            if (L0 < inv + 2u || L0 - inv - 2u > 0x3FFFu) continue;
            A = p0 + inv + 1u;
            while ((ex->ib_va + 4ull * A) & alignMask) A++;
            if (A + need <= p0 + L0) break;
        }
        if (r == tb->nrun) return d_tbl_fail(ds, draw_at, XLAT12_TDESC_NO_ROOM);
        const uint32_t p0 = tb->run_at[r];
        L = tb->run_len[r];
        if (reuseFlag && !class11 && !class10 && !nosamp) {
            const uint32_t spare = p0 + L - A - need;
            const uint32_t tx = spare / 8u < D_SH_TMAX - a->ntex ? spare / 8u : D_SH_TMAX - a->ntex;
            const uint32_t sx = (spare - 8u * tx) / 4u < D_SH_SMAX - 1u ? (spare - 8u * tx) / 4u : D_SH_SMAX - 1u;
            tcap = a->ntex + tx; scap = 1u + sx;
        }
        for (uint32_t k = 0; k < inv; k++) out[p0 + k] = kDescInv[k];
        out[p0 + inv] = D_TBL_NOP_HDR | ((L - inv - 2u) << 16);
        for (uint32_t k = p0 + inv + 1u; k < p0 + L; k++) out[k] = 0u;
        if (inv) { tb->inv_done = 1u; ds->table_inv++; }
        B = &out[A];
    }
    const uint64_t va = U ? 0ull : ex->ib_va + 4ull * A, iva = va + 32ull, sva2 = iva + 32ull * tcap;
    B[0] = (uint32_t)iva;  B[1] = (uint32_t)(iva >> 32);
    /* build 0.0.470: a no-sampler row places no S#, so the shadow table's +0x10 stays 0 (the program never loads it). */
    if (nsampEff) { B[4] = (uint32_t)sva2; B[5] = (uint32_t)(sva2 >> 32); }
    for (uint32_t i = 0; i < a->ntex; i++) for (uint32_t k = 0; k < 8u; k++) B[8u + 8u * i + k] = g[i][k];
    for (uint32_t i = 0; i < nsampEff; i++) for (uint32_t k = 0; k < 4u; k++) B[8u + 8u * tcap + 4u * i + k] = sg[i][k];
    /* class-11 ONLY: the redirected pointer table, right after the S# records - entry k = {k, 0}, so the SAME
     * sext32(idx<<4) arithmetic the program already runs against sbase (sva2 above) lands on translated S# k. */
    uint64_t entva = 0ull;
    if (class11) {
        const uint32_t entOff = 8u + 8u * a->ntex + 4u * nsampEff;
        for (uint32_t k = 0; k < a->nsamp; k++) { B[entOff + 2u * k] = k; B[entOff + 2u * k + 1u] = 0u; }
        entva = va + 4ull * entOff;
    }
    /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2) — class-10 ONLY: the redirected texture-entry table, right after
     * the S#. Apple's LAYOUT, our VALUES: entry i sits at the SAME byte offset tent[i] the program loads it from, and is
     * {i, 0} - so the program's own sext32(entry << 5) against the shadow's image heap (iva) lands on translated T# i.
     * Every other dword of the region is the NOP body's zero fill (written above), inside d_tbl_need's tent_max/4 + 2. */
    uint64_t tentva = 0ull;
    if (class10) {
        const uint32_t tentOff = 8u + 8u * a->ntex + 4u * nsampEff;
        for (uint32_t i = 0; i < a->ntex && i < 3u; i++) { B[tentOff + a->tent[i] / 4u] = i; B[tentOff + a->tent[i] / 4u + 1u] = 0u; }
        tentva = va + 4ull * tentOff;
    }
    if (!U) tb->run_len[r] = 0u;   /* used */
    /* redirect: the table pointer to the shadow, each image index to its record, and EITHER the direct sampler
     * index to 0 (unchanged) OR, class-11 only, the class-11 pointer to the redirected entries table above. */
    /* build 0.0.470: a class-10 row redirects its textbl pair (to the entry table above) in place of texture index
     * slots it does not have; a no-sampler row redirects NO sampler slot (it read none). */
    out[p->ud_pos[a->table]] = (uint32_t)va; out[p->ud_pos[a->table + 1u]] = (uint32_t)(va >> 32);
    if (class10) { out[p->ud_pos[a->textbl1 - 1u]] = (uint32_t)tentva; out[p->ud_pos[a->textbl1]] = (uint32_t)(tentva >> 32); }
    else for (uint32_t i = 0; i < a->ntex; i++) out[p->ud_pos[a->tex[i]]] = i;
    if (class11) { out[p->ud_pos[a->samptbl]] = (uint32_t)entva; out[p->ud_pos[a->samptbl + 1u]] = (uint32_t)(entva >> 32); }
    else if (!nosamp) { out[p->ud_pos[a->samp]] = 0u; }
    for (uint32_t i = 0; i < ns; i++) { tb->patched |= 1u << sl[i]; tb->ppos[sl[i]] = p->ud_pos[sl[i]]; }
    /* build 0.0.454 item 1 (switch 48): this placement becomes the translation's reusable shadow (a class-11
     * placement ends the previous one: its redirects are not a direct-sampler shadow's). Its `mask` is exactly the
     * slots just redirected, so d_sh_holds never answers for another placement's redirect. Apple's values are the
     * ones read above, before the redirect. Nothing here runs without the flag. */
    if (reuseFlag) {
        /* build 0.0.470 (NO-SAMPLER-CLASS10.md section 6, the design's B13): the two new shapes end the previous
         * shadow exactly as class 11 does - their redirects of the table pair are not a direct-sampler shadow's. */
        if (class11 || class10 || nosamp) tb->sh.valid = 0u;
        else {
            tb->sh.tva = tva; tb->sh.A = A; tb->sh.mask = 0u;
            for (uint32_t i = 0; i < ns; i++) tb->sh.mask |= 1u << sl[i];
            for (uint32_t i = 0; i < a->ntex && i < 2u; i++) tb->sh.tidx[i] = aidx[i];
            tb->sh.sidx[0] = sv;
            tb->sh.tn = (uint8_t)a->ntex; tb->sh.tcap = (uint8_t)tcap; tb->sh.sn = 1u; tb->sh.scap = (uint8_t)scap;
            tb->sh.table = (uint8_t)a->table; tb->sh.samp = (uint8_t)a->samp; tb->sh.valid = 1u;
        }
    }
    ds->table_draws++; ds->table_img += a->ntex; ds->table_samp += nsampEff; ds->table_llc += nllc; ds->table_iter256 += nit;
    ds->table_lin_unproven += lin; ds->table_dwords += L;   /* build 0.0.480: L 0 for a unit - counted when placed */
    ds->err_reg = 0; ds->err_in_dword = 0; ds->err_op = 0xFFFFFFFFu;
    return 0;
}

static uint64_t d_pair_va(uint32_t lo, uint32_t hi) { return ((uint64_t)(hi & 0xFFu) << 40) | ((uint64_t)lo << 8); }

/* Field-by-field copy of xlat12_draw_profile, in place of a struct assignment: a plain `*dst = *src` on this struct
 * (144 bytes) is lowered by the backend to a call to memcpy(), which the kext build has nothing to link against -
 * see kextcheck's undefined-symbol proof. Named field copies (and small, fixed-trip-count array loops that get fully
 * unrolled at -O2) compile to direct loads/stores instead, exactly like the explicit per-word zero loops already used
 * for DPair/DTable/xlat12_draw_stats elsewhere in this file. */
_Static_assert(sizeof(xlat12_draw_profile) == 160u,
               "xlat12_draw_profile changed size: update d_profile_copy so it still copies every field");
/* D4-PRIME-FIXES.md item 3(a),  — THE READ-SET PROOF CONTRACT'S OWN VERSION. This file's read-set
 * admission rule (`d_readset_stage`'s `pd1 && (!hasImage || pii)`) is written against PROOF VERSION 2 (a STORE no
 * longer zeroes proof_depth1_data_only by itself - it disqualifies only under tools/gfx-readset.py's own rules, and
 * an image op no longer zeroes it either - see xlat12_readset.h's own banner). A generator regression back to
 * version 1's semantics must fail this build loudly rather than silently admitting programs this rule was never
 * proved against. */
_Static_assert(XLAT12_READSET_PROOF_VERSION == 2u,
               "xlat12_readset.h's proof version changed: re-derive d_readset_stage's admission rule against it");
/* build 0.0.504: XLAT12_READSET_PTR_MAX 4 -> 5 (ws_J_VfxXghb reads five user-data pointers). The
 * table-path vertex fallback (d_table_desc's `rrow` loop) copies a read-set row's pointers into in_vptr[], which holds
 * XLAT12_ABI_PTR_MAX: a read-set cap above that could only ever set in_over there. Pin that every row a read-set row
 * can carry also fits the table path, and that the cap is exactly what the generator emitted (tools/gfx-readset.py). */
_Static_assert(XLAT12_READSET_PTR_MAX == 5u && XLAT12_READSET_PTR_MAX <= XLAT12_ABI_PTR_MAX,
               "xlat12_readset.h's pointer cap changed: re-check d_readset_stage and the table-path rrow loop");
static void d_profile_copy(xlat12_draw_profile *dst, const xlat12_draw_profile *src)
{
    dst->vs_rsrc1_gs = src->vs_rsrc1_gs; dst->vs_rsrc2_gs = src->vs_rsrc2_gs; dst->vs_stages_en = src->vs_stages_en;
    dst->ps_rsrc1 = src->ps_rsrc1; dst->ps_rsrc2 = src->ps_rsrc2;
    dst->ps_input_ena = src->ps_input_ena; dst->ps_input_addr = src->ps_input_addr;
    dst->ge_cntl = src->ge_cntl; dst->ge_max_output = src->ge_max_output;
    dst->rsrc4_gs = src->rsrc4_gs; dst->rsrc4_ps = src->rsrc4_ps; dst->gs_out_config_ps = src->gs_out_config_ps;
    dst->binner_cntl0 = src->binner_cntl0; dst->ngg_subgrp_cntl = src->ngg_subgrp_cntl;
    dst->gs_max_vert_out = src->gs_max_vert_out; dst->gs_instance_cnt = src->gs_instance_cnt;
    dst->gs_out_prim_type = src->gs_out_prim_type; dst->spi_shader_idx_format = src->spi_shader_idx_format;
    for (uint32_t k = 0; k < 4u; k++) dst->vs_isa_head[k] = src->vs_isa_head[k];
    for (uint32_t k = 0; k < 4u; k++) dst->ps_isa_head[k] = src->ps_isa_head[k];
    for (uint32_t k = 0; k < 4u; k++) dst->vs_isa_head_rec[k] = src->vs_isa_head_rec[k];
    dst->vs_pgm_va = src->vs_pgm_va;
    dst->vs_drops_params = src->vs_drops_params;
    dst->ps_inline_tex1 = src->ps_inline_tex1; dst->ps_inline_samp1 = src->ps_inline_samp1;
    dst->ps_table_abi1 = src->ps_table_abi1;
    dst->vs_abi_ptr1 = src->vs_abi_ptr1;   /* 0.0.391 */
    dst->ps_id = src->ps_id;               /* 0.0.398: the in-force fragment identity */
    dst->ps_readset1 = src->ps_readset1; dst->vs_readset1 = src->vs_readset1;   /* D4' (D4-PRIME.md item 2) */
}

/* Note a program-address write and, when that stage's address has CHANGED, ask the caller to refresh that stage's fields.
 * Returns 0, or XLAT12_IB_ERR_PAIR when the caller refuses the program. */
static uint32_t d_pair_note(const xlat12_draw_profile *base, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,
                            DPair *p, uint32_t g10, uint32_t val)
{
    uint32_t stage;
    if (!p) return 0u;
    /* BEFORE the resolver gate: SPI_PS_IN_CONTROL is what the interpolation guard reads, and it must be recorded
     * whether or not this caller resolves programs. Last write wins, and only writes seen before the draw count. */
    if (g10 == XLAT12_REG_PS_IN_CONTROL) { p->ps_in_ctl = val; p->ps_in_seen = 1u; }
    if (!ex || !ex->pgm_profile) return 0u;
    if (g10 == 0xb120u) p->vs_lo = val;
    else if (g10 == 0xb124u) p->vs_hi = val;
    else if (g10 == 0xb020u) p->ps_lo = val;
    else if (g10 == 0xb024u) p->ps_hi = val;
    else return 0u;
    stage = (g10 == 0xb120u || g10 == 0xb124u) ? 1u : 0u;
    {
        const uint32_t lo = stage ? p->vs_lo : p->ps_lo, hi = stage ? p->vs_hi : p->ps_hi;
        const uint64_t va = d_pair_va(lo, hi);
        if (!lo) return 0u;                                   /* address not complete yet */
        if (p->have && va == (stage ? p->res_vs : p->res_ps)) return 0u;   /* unchanged */
        if (!p->have) { d_profile_copy(&p->cur, base); p->have = 1; }  /* start from the caller's profile, fill stages over it */
        /* 0.0.311: CLEAR the relocated address before asking. A resolver that answers it for one vertex
         * program and leaves it alone for the next would point the second program's draw at the FIRST one's code - which
         * renders wrong geometry instead of refusing, the failure this translator must never have. Clearing here makes
         * "did not answer" mean "not relocated" rather than "same as last time". */
        /* Both per-program answers are cleared before asking, for the same reason: a resolver that answers for one
         * program and not the next must not leak the first one's answer into it. For vs_drops_params the leak would
         * be in the dangerous direction - a program that DOES export parameters inheriting "drops them" would have
         * its draws served under the interpolation guard instead of refused. */
        if (stage) { p->cur.vs_pgm_va = 0ull; p->cur.vs_drops_params = 0u; p->cur.vs_abi_ptr1 = 0u; p->cur.vs_readset1 = 0u; }
        else { p->cur.ps_inline_tex1 = 0u; p->cur.ps_inline_samp1 = 0u; p->cur.ps_table_abi1 = 0u;
               p->cur.ps_id = XLAT12_PS_ID_NONE; p->cur.ps_readset1 = 0u; }   /* / M4-DESC-TABLE-IMPL / 0.0.398 / D4' (D4-PRIME.md item 2): no leak */
        if (!ex->pgm_profile(ex->pgm_ctx, stage, va, &p->cur)) {
            if (stage) ds->pair_vs_va = va; else ds->pair_ps_va = va;
            return XLAT12_IB_ERR_PAIR;
        }
        /* The same structural rule the segment-wide address is held to (PGM_LO is VA >> 8), applied where a resolver's
         * answer enters: a misaligned or out-of-range relocation is refused, never rounded. */
        if (stage && p->cur.vs_pgm_va &&
            ((p->cur.vs_pgm_va & (XLAT12_RELOC_ALIGN - 1u)) || p->cur.vs_pgm_va >= (1ull << 48))) {
            ds->pair_vs_va = va;
            return XLAT12_IB_ERR_RING;
        }
        if (stage) p->res_vs = va; else p->res_ps = va;
        ds->pairs++;
    }
    return 0u;
}

/* The profile in force for the next register: the resolved one once a pair has been resolved, else the caller's. */
static const xlat12_draw_profile *d_pf(const xlat12_draw_profile *base, const DPair *p)
{
    return (p && p->have) ? &p->cur : base;
}

/* 0.0.409: resolve ONE pre-scanned stage into `cur`, exactly as d_pair_note does (same clears, same
 * structural relocation check), and return 1. 0 = the resolver refused (or the answer is unusable), which makes the
 * whole pre-scan fall back to the base profile. A stage whose LO write is absent or zero never reaches here. */
static int d_pair_pre_stage(const xlat12_draw_extra *ex, xlat12_draw_profile *cur, uint32_t stage, uint32_t lo, uint32_t hi)
{
    const uint64_t va = d_pair_va(lo, hi);
    if (stage) { cur->vs_pgm_va = 0ull; cur->vs_drops_params = 0u; cur->vs_abi_ptr1 = 0u; cur->vs_readset1 = 0u; }
    else { cur->ps_inline_tex1 = 0u; cur->ps_inline_samp1 = 0u; cur->ps_table_abi1 = 0u;
           cur->ps_id = XLAT12_PS_ID_NONE; cur->ps_readset1 = 0u; }   /* same no-leak clears as d_pair_note */
    if (!ex->pgm_profile(ex->pgm_ctx, stage, va, cur)) return 0;
    if (stage && cur->vs_pgm_va &&
        ((cur->vs_pgm_va & (XLAT12_RELOC_ALIGN - 1u)) || cur->vs_pgm_va >= (1ull << 48)))
        return 0;                               /* the same structural rule d_pair_note applies to a relocation */
    return 1;
}

/* 0.0.409: PRE-RESOLVE THE REGION'S PROGRAM PAIR, before d_region() synthesises the pair-dependent
 * state. d_synth() runs at VGT_SHADER_STAGES_EN, which a legacy-VS stream writes BEFORE its SPI_SHADER_PGM_LO/HI
 * pair, so through 0.0.408 SPI_SHADER_GS_OUT_CONFIG_PS came from the base (m2tri) profile whatever program the draw
 * actually bound. This walks [from, to) once for the LAST write of the ES/GS pair (gfx10 0xb120/0xb124) and the PS
 * pair (0xb020/0xb024), resolves each stage through the SAME ex->pgm_profile callback d_pair_note uses, and seeds
 * `p` so the walk that follows sees that pair as already resolved (so the resolver is asked once, not twice).
 *
 * TRANSACTIONAL AND FAIL-CLOSED. The two stages are resolved into a scratch profile; if ANY present stage refuses,
 * NOTHING is committed (no res_*, no cur, no lo/hi) and pair_pre_unresolved is counted - the region then translates
 * from the base profile exactly as 0.0.408 did, and the walk's d_pair_note still refuses an unusable program with
 * XLAT12_IB_ERR_PAIR as before. A stage whose LO was never written, or was written zero, keeps the base value and is
 * neither resolved nor counted. NO DWORD IS EMITTED OR CONSUMED: this changes values only, never the output length.
 * Nothing here runs unless XLAT12_EXTRA_PAIR_PRE is set, so OFF is 0.0.408 byte for byte. */
static void d_pair_pre(const xlat12_draw_profile *base, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,
                       const uint32_t *in, uint32_t n, uint32_t from, uint32_t to, DPair *p)
{
    if (!ex || !ex->pgm_profile || !p) return;
    int hv = 0, hp = 0;
    uint32_t vlo = 0, vhi = 0, plo = 0, phi = 0;
    for (uint32_t i = from; i < to; ) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) break;
        const uint32_t op = PO(h);
        if (h != XLAT12_IB_NOP && PT(h) == 3u && is_set(op)) {
            const uint32_t b = d_base(op), off = in[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (b + off + k) << 2, val = in[i + 2u + k];
                if (g10 == 0xb120u) { vlo = val; hv = 1; }
                else if (g10 == 0xb124u) { vhi = val; }
                else if (g10 == 0xb020u) { plo = val; hp = 1; }
                else if (g10 == 0xb024u) { phi = val; }
            }
        }
        i += l;
    }
    const int rv = hv && vlo != 0u, rp = hp && plo != 0u;
    const int nv = rv && !(p->have && d_pair_va(vlo, vhi) == p->res_vs);
    const int np = rp && !(p->have && d_pair_va(plo, phi) == p->res_ps);
    if (!nv && !np) return;                     /* nothing new to resolve: base, or this very pair already in force */
    xlat12_draw_profile tmp;
    if (p->have) d_profile_copy(&tmp, &p->cur); else d_profile_copy(&tmp, base);
    if (nv && !d_pair_pre_stage(ex, &tmp, 1u, vlo, vhi)) { ds->pair_pre_unresolved++; return; }
    if (np && !d_pair_pre_stage(ex, &tmp, 0u, plo, phi)) { ds->pair_pre_unresolved++; return; }
    if (nv) { p->res_vs = d_pair_va(vlo, vhi); p->vs_lo = vlo; p->vs_hi = vhi; ds->pair_pre_resolved++; }
    if (np) { p->res_ps = d_pair_va(plo, phi); p->ps_lo = plo; p->ps_hi = phi; ds->pair_pre_resolved++; }
    d_profile_copy(&p->cur, &tmp);              /* field-by-field: a struct assignment would call memcpy in the kext */
    p->have = 1;
}

/* =================================================================================================================
 * build 0.0.480 — CONTINUATION UNITS (xlat12_ib.h XLAT12_EXTRA_UNIT; notes/design/CONTINUATION-UNITS.md Q11).
 * Every helper below runs only when d_unit_of() returns non-NULL, i.e. only under the flag; OFF none is reached.
 * They are separate NOINLINE functions so that the unit path adds no locals to translate_draw_ex's own frame (the
 * design's stack budget: translate_draw_ex <= 0x4e8, d_table_desc <= 0x1e8); all their state is the caller's.
 * ================================================================================================================= */
/* The caller's IN fields must describe a unit this translation can walk: 1..XLAT12_UNIT_CONS_MAX constituents, the
 * first at in[0], the rest strictly increasing and inside the stream. Anything else is a caller error (ERR_ARG). */
static int d_unit_args_ok(const xlat12_unit *U, uint32_t n)
{
    if (!U || U->ncons < 1u || U->ncons > XLAT12_UNIT_CONS_MAX || U->cons_in[0] != 0u) return 0;
    for (uint32_t j = 1; j < U->ncons; j++)
        if (U->cons_in[j] <= U->cons_in[j - 1u] || U->cons_in[j] >= n) return 0;
    return 1;
}

/* Reset every OUT and WORK field (never an IN field) at the top of a unit translation, and empty the pool journal:
 * from here on it names this translation's placements only. Field by field: a struct assignment could call memcpy. */
static D_NOINLINE void d_unit_reset(xlat12_unit *U)
{
    for (uint32_t j = 0; j < XLAT12_UNIT_CONS_MAX; j++) U->cons_out[j] = 0u;
    U->cons_seen = 0u; U->last_head_out = 0u; U->stream_end = 0u; U->tail_in_place = 0u; U->slack = 0u;
    U->own_dw = 0u; U->pool_dw = 0u; U->placed = 0u; U->unredir = 0u; U->hs_patched = 0u; U->inv_inline = 0u;
    U->cb_calls = 0u; U->ocur = 0u; U->rings_done = 0u; U->blk_from = 0u; U->blk_to = 0u; U->npend = 0u; U->nhs = 0u; U->hs_lo_open = 0u; U->hs_lo_pos = 0u;
    U->pk_runs = 0u; U->pk_recs = 0u; U->pk_saved = 0u;   /* build 0.0.501: PACK's OUT and WORK fields (never `pack`) */
    U->pk_own[0] = 0xFFFFFFFFu; U->pk_own[1] = 0xFFFFFFFFu; U->pk_np = 0u;
    for (uint32_t j = 0; j < XLAT12_UNIT_PEND_MAX; j++) { U->pk_pr[j] = 0u; U->pk_ph[j] = 0; }
    if (U->pool) U->pool->jn = 0u;
    U->spill_dw = 0u;                        /* build 0.0.522 (switch 76): the spill tier's OUT and its journal */
    if (U->spill) U->spill->jn = 0u;
}

/* A constituent head is reached at input dword `i`: record where it lands in the output (after closing any open SET
 * packet, so it is the head packet's own first dword) and hand the PREVIOUS constituent's finished slice to the
 * caller's provenance feed - that slice can vouch for every draw from this head on, never for one before it (the
 * same "added AFTER it asked" order the kext keeps between separately translated segments). */
static D_NOINLINE void d_unit_head(xlat12_unit *U, DEmit *e)
{
    d_close(e);
    const uint32_t j = U->cons_seen;
    U->cons_out[j] = e->n;
    if (j >= 1u && U->cons_fn && U->cons_out[j] >= U->cons_out[j - 1u]) {
        U->cons_fn(U->cons_ctx, &e->out[U->cons_out[j - 1u]], U->cons_out[j] - U->cons_out[j - 1u]);
        U->cb_calls++;
    }
    U->cons_seen = j + 1u;
}

/* 1 when gfx12 register `g12` is one the extra block (d_rings) of THIS translation wrote: the block is walked as it
 * sits in the output ([blk_from, blk_to), recorded right after d_rings), so the answer is exactly what was emitted -
 * never a second hand-kept list of d_rings' registers that could drift from it. */
static int d_unit_blockreg(const xlat12_unit *U, const uint32_t *out, uint32_t g12)
{
    if (!U->rings_done) return 0;
    for (uint32_t i = U->blk_from; i < U->blk_to; ) {
        const uint32_t h = out[i];
        if (PT(h) != 3u) return 1;                    /* not our own packet shape: answer "ours" and refuse (fail-closed) */
        const uint32_t l = PC(h) + 2u, op = PO(h), b = d_base(op);
        if (i + l > U->blk_to) return 1;
        if (b && l >= 3u) for (uint32_t k = 0; k + 2u < l; k++) if (((b + (out[i + 1u] & 0xFFFFu) + k) << 2) == g12) return 1;
        i += l;
    }
    return 0;
}

/* The HS pointer pair: every lo-then-hi write of gfx12 SPI_SHADER_PGM_LO/HI_HS is remembered, so the unit can
 * patch EVERY zero pair its constituents wrote (d_unit_finish), not only region 0's last. 1 = over the cap (refuse). */
static int d_unit_hs_note(xlat12_unit *U, uint32_t g12, uint32_t pos)
{
    if (g12 == 0xb410u) { U->hs_lo_open = 1u; U->hs_lo_pos = pos; return 0; }
    if (g12 != 0xb414u || !U->hs_lo_open) return 0;
    U->hs_lo_open = 0u;
    if (U->nhs >= XLAT12_UNIT_HS_MAX) return 1;
    U->hs_lo[U->nhs] = U->hs_lo_pos; U->hs_hi[U->nhs] = pos; U->nhs++;
    return 0;
}

/* P4 (design Q3): at a draw of the unit, every user-data slot an EARLIER draw's table step redirected - and that THIS
 * draw's program can read - gets Apple's carried value re-emitted in this region, through the same d_reg/d_add path a
 * live write takes, when the carry holds the slot. The skips are the REDIRECTED rung's own (the reusable shadow's own
 * row slots under switch 48; a slot at or past the RSRC2_PS USER_SGPR count in force). A slot with no carried value is
 * left alone, and the rung refuses it REDIRECTED exactly as it always did. */
static D_NOINLINE uint32_t d_unit_unredirect(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex,
                                             xlat12_draw_stats *ds, DEmit *e, DPair *pr, const DTable *tb,
                                             uint32_t oreg, uint32_t draw_at)
{
    xlat12_unit *const U = ex->unit;
    const xlat12_ud_carry *carry = ex->ud_carry;
    const xlat12_draw_profile *ucp = d_pf(pf, pr);
    const DTableAbi *ua = (ucp->ps_table_abi1 && ucp->ps_table_abi1 <= D_TBL_ABIS) ? &kDTableAbi[ucp->ps_table_abi1 - 1u] : 0;
    const int ureuse = ua && d_sh_reuse_ok(ex, ua, pr, tb, oreg);
    for (uint32_t k = 0; k < D_PS_UDN; k++) {
        if (ureuse && d_sh_row_holds(ua, tb, pr, k)) continue;
        if (pr->ps_usg_seen && k >= pr->ps_usg) continue;
        if (!(((tb->patched >> k) & 1u) && ((pr->ud_seen >> k) & 1u) && pr->ud_pos[k] == tb->ppos[k])) continue;
        if (!carry || !((carry->ps_ok >> k) & 1u)) continue;   /* no carried value: the rung refuses REDIRECTED */
        const uint32_t g10 = D_PS_UD0 + 4u * k;
        uint32_t g12 = 0, v12 = 0, rst; int emit = 0, st = 0;
        if ((rst = d_reg(ucp, ex, ds, g10, carry->ps_val[k], &g12, &v12, &emit, &st)) != 0u) return rst;
        if (!emit) continue;
        if ((rst = d_add(e, g12, 0u, v12)) != 0u) {
            if (rst == XLAT12_ERR_CAPACITY) { ds->err_op = XLAT12_REEMIT_NO_ROOM; ds->err_in_dword = draw_at; return XLAT12_IB_ERR_TOO_LONG; }
            return rst;
        }
        if (g12 == g10) d_ud_note(pr, g10, e->n - 1u);
        U->unredir++;
    }
    d_close(e);
    return 0;
}

/* C4: the translation's cache invalidate, INLINE in a unit's stream right before its first table draw (kDescInv, byte
 * for byte the packet XLAT12_EXTRA_DESC_INV places in a pad run elsewhere), unless it was already placed or the
 * unit's own head covers it (XLAT12_EXTRA_DESC_INV_APPLE_HEAD with head_inv_ok - which the caller may only set for a
 * head that EXECUTES). d_table_desc then finds inv_done and places no invalidate of its own. */
static D_NOINLINE uint32_t d_unit_inv(const xlat12_draw_extra *ex, DEmit *e, DTable *tb, xlat12_draw_stats *ds, uint32_t draw_at)
{
    if (!(ex->flags & XLAT12_EXTRA_DESC_INV) || tb->inv_done) return 0;
    if ((ex->flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD) && tb->head_inv_ok) return 0;
    d_close(e);
    if (e->cap - e->n < XLAT12_DESC_INV_DWORDS) { ds->err_op = XLAT12_UNIT_DRAW_ROOM; ds->err_in_dword = draw_at; return XLAT12_IB_ERR_TOO_LONG; }
    for (uint32_t k = 0; k < XLAT12_DESC_INV_DWORDS; k++) e->out[e->n++] = kDescInv[k];
    tb->inv_done = 1u; ds->table_inv++; ex->unit->inv_inline++;
    return 0;
}

/* P2: a unit's draw packet goes right after its region, and the next region starts right after it. */
static D_NOINLINE uint32_t d_unit_draw(xlat12_unit *U, DEmit *e, const uint32_t *in, uint32_t at, uint32_t l, xlat12_draw_stats *ds)
{
    d_close(e);
    if (e->cap - e->n < l) { ds->err_op = XLAT12_UNIT_DRAW_ROOM; ds->err_in_dword = at; return XLAT12_IB_ERR_TOO_LONG; }
    for (uint32_t k = 0; k < l; k++) e->out[e->n + k] = in[at + k];
    U->ocur = e->n + l;
    return 0;
}

/* P2: where the tail region (after the unit's last draw) starts. At its own input dword `cursor` when the stream is not
 * ahead of it - the dwords in between become the unit's first own free run - else right after the stream. */
static D_NOINLINE uint32_t d_unit_tail(xlat12_unit *U, uint32_t *out, uint32_t cursor)
{
    if (U->ocur > cursor) return U->ocur;
    for (uint32_t k = U->ocur; k < cursor; k++) out[k] = XLAT12_IB_NOP;
    U->tail_in_place = 1u;
    return cursor;
}

/* Where one deferred block lands: `hA` its body's first dword, `vA` that dword's GPU VA. */
static void d_unit_write_block(const xlat12_unit *U, const xlat12_unit_pend *b, uint32_t *hA, uint64_t vA, uint32_t *out)
{
    const uint32_t *src = &U->dw[b->off];
    for (uint32_t k = 0; k < 8u; k++) hA[k] = 0u;
    for (uint32_t k = 0; k < 8u * b->tn; k++) hA[8u + k] = src[8u + k];
    for (uint32_t k = 0; k < 4u * b->sn; k++) hA[8u + 8u * b->tn + k] = src[8u + 8u * b->tcap + k];
    const uint32_t eo = 8u + 8u * b->tn + 4u * b->sn;
    for (uint32_t k = 0; k < b->ext_len; k++) hA[eo + k] = src[8u + 8u * b->tcap + 4u * b->scap + k];
    const uint64_t iva = vA + 32ull, sva2 = iva + 32ull * b->tn, eva = vA + 4ull * eo;
    hA[0] = (uint32_t)iva; hA[1] = (uint32_t)(iva >> 32);
    if (b->nsamp_eff) { hA[4] = (uint32_t)sva2; hA[5] = (uint32_t)(sva2 >> 32); }
    out[b->tpos[0]] = (uint32_t)vA; out[b->tpos[1]] = (uint32_t)(vA >> 32);
    if (b->epos[0] != 0xFFFFFFFFu) { out[b->epos[0]] = (uint32_t)eva; out[b->epos[1]] = (uint32_t)(eva >> 32); }
}

/* The body starts at the first dword of a run [0, len) whose VA is `va` that is at least one past the NOP header and
 * aligned to `mask`: the offset of that dword, or ~0 when `blen` more dwords do not fit. */
static uint32_t d_unit_fit(uint64_t va, uint32_t len, uint32_t mask, uint32_t blen)
{
    uint32_t o = 1u;
    while (o < len && ((va + 4ull * o) & (uint64_t)mask)) o++;
    return (o < len && blen <= len - o) ? o : 0xFFFFFFFFu;
}

/* build 0.0.501 (PACK): d_unit_fit with the first candidate offset `o0` - 1 when the block opens a NOP (its header
 * takes dword 0), 0 when it is appended inside the run's open NOP (the run starts right after the previous block). */
static uint32_t d_unit_fit_from(uint64_t va, uint32_t len, uint32_t mask, uint32_t blen, uint32_t o0)
{
    uint32_t o = o0;
    while (o < len && ((va + 4ull * o) & (uint64_t)mask)) o++;
    return (o < len && blen <= len - o) ? o : 0xFFFFFFFFu;
}

/* build 0.0.501 (notes/design/UNIT-ROOM.md Q3 C2, ; xlat12_ib.h `pack`, switch 67) - THE PACK LOOP.
 * The same blocks, in the same order, first-fit over the same runs as the loop in d_unit_finish (own run 0, own run 1, then
 * the frame pool) - but every block placed in a run goes inside ONE PACKET3(NOP) per run: the run's first block opens it
 * (the header at the run's cursor, the body at the first dword after it aligned to the block's mask), a later block is
 * appended at the first aligned dword at or after the run's cursor, pad dwords are written 0, and the open header's count
 * is rewritten after every block to cover exactly the dwords used since it. Pool placements are journalled per block as
 * today (the run's first entry covers the header), so xlat12_pool_undo restores every one; a full journal refuses the run
 * as today; no room anywhere undoes the pool and refuses XLAT12_TDESC_NO_ROOM naming the draw that asked, as today. The
 * open headers live in the unit's own WORK fields (pk_own, pk_pr/pk_ph), never on this frame. d_unit_finish calls it
 * last (a sibling call: its own frame is gone first), after the HS pairs and U->slack. A header's count is bounded by the blocks one translation places (at most
 * XLAT12_UNIT_PEND_MAX of at most XLAT12_UNIT_PEND_DW body dwords plus their pad), far under PACKET3's 14-bit count. */
static D_NOINLINE uint32_t d_unit_pack(const xlat12_draw_extra *ex, xlat12_unit *U, uint32_t *out, uint32_t n,
                                       xlat12_draw_stats *ds)
{
    /* the same two own runs d_unit_finish computed: [ocur, ocur + slack - after-tail) and [stream_end, n) */
    uint32_t ra[2], rl[2];
    ra[1] = U->stream_end; rl[1] = n - U->stream_end;
    ra[0] = U->ocur; rl[0] = U->slack - rl[1];
    for (uint32_t b = 0; b < U->npend; b++) {
        xlat12_unit_pend *pb = &U->pend[b];
        const uint32_t blen = 8u + 8u * pb->tn + 4u * pb->sn + pb->ext_len;
        const uint64_t mask = (uint64_t)pb->align_mask;
        uint32_t used = 0u;
        for (uint32_t r = 0; r < 2u && !used; r++) {
            const uint32_t open = U->pk_own[r] != 0xFFFFFFFFu;
            const uint64_t va0 = ex->ib_va + 4ull * ra[r];
            const uint32_t o = d_unit_fit_from(va0, rl[r], pb->align_mask, blen, open ? 0u : 1u);
            if (o == 0xFFFFFFFFu) continue;
            used = o + blen;
            if (open) { uint32_t of = 1u; while (of < 32u && ((va0 + 4ull * of) & mask)) of++; U->pk_saved += of > o ? of - o : 0u; }
            else { U->pk_own[r] = ra[r]; U->pk_runs++; }
            for (uint32_t k = open ? 0u : 1u; k < o; k++) out[ra[r] + k] = 0u;
            d_unit_write_block(U, pb, &out[ra[r] + o], va0 + 4ull * o, out);
            ra[r] += used; rl[r] -= used; U->own_dw += used;
            out[U->pk_own[r]] = D_TBL_NOP_HDR | ((ra[r] - U->pk_own[r] - 2u) << 16);
        }
        /* build 0.0.522 (switch 76): tier 0 the frame pool (today), tier 1 the spill tier, LAST; a spill run's open NOP is
         * remembered under key XLAT12_POOL_RUNS + r, so it can never be confused with the pool's run r. U->spill NULL: tier 1
         * offers nothing and this is the pool loop of 0.0.501 exactly. */
        for (uint32_t tier = 0; tier < 2u && !used; tier++) {
        xlat12_pool *pl = tier ? U->spill : U->pool;
        const uint32_t key0 = tier ? XLAT12_POOL_RUNS : 0u;
        for (uint32_t r = 0; pl && !used && r < pl->nrun && r < XLAT12_POOL_RUNS; r++) {
            xlat12_pool_run *pr = &pl->run[r];
            uint32_t *ph = 0;
            for (uint32_t q = 0; q < U->pk_np && q < XLAT12_UNIT_PEND_MAX; q++) if (U->pk_pr[q] == key0 + r) ph = U->pk_ph[q];
            const uint32_t o = d_unit_fit_from(pr->va, pr->len, pb->align_mask, blen, ph ? 0u : 1u);
            if (o == 0xFFFFFFFFu) continue;
            if (pl->jn >= XLAT12_UNIT_PEND_MAX) break;   /* cannot journal it: do not take it */
            if (!ph && U->pk_np >= XLAT12_UNIT_PEND_MAX) break;   /* cannot remember its NOP: do not take it */
            used = o + blen;
            xlat12_pool_jent *j = &pl->j[pl->jn++];
            j->host = pr->host; j->va = pr->va; j->len = pr->len; j->used = used; j->r = r;
            if (ph) { uint32_t of = 1u; while (of < 32u && ((pr->va + 4ull * of) & mask)) of++; U->pk_saved += of > o ? of - o : 0u; }
            else { ph = pr->host; U->pk_pr[U->pk_np] = key0 + r; U->pk_ph[U->pk_np] = ph; U->pk_np++; U->pk_runs++; }
            for (uint32_t k = (ph == pr->host) ? 1u : 0u; k < o; k++) pr->host[k] = 0u;
            d_unit_write_block(U, pb, &pr->host[o], pr->va + 4ull * o, out);
            pr->host += used; pr->va += 4ull * used; pr->len -= used;
            if (tier) U->spill_dw += used; else U->pool_dw += used;
            ph[0] = D_TBL_NOP_HDR | (((uint32_t)(pr->host - ph) - 2u) << 16);
        }
        }
        if (!used) {
            if (U->pool) (void)xlat12_pool_undo(U->pool);
            if (U->spill) (void)xlat12_pool_undo(U->spill);   /* build 0.0.522: both journals, as one refusal */
            return d_tbl_fail(ds, pb->draw_at, XLAT12_TDESC_NO_ROOM);
        }
        pb->placed_len = used; U->placed++; U->pk_recs++;
        ds->table_dwords += used;
    }
    return 0;
}

/* P3 and the unit's close-out, after the tail region: (1) every zero HS pointer pair the unit wrote gets the
 * ring-offsets pointer (: d_rings' rule, now for every pair, not only region 0's last); (2) every deferred block is
 * placed - first-fit in the unit's own free runs (the gap before an in-place tail, then everything after the tail),
 * else first-fit in the frame pool (journalled) - as one PACKET3(NOP) covering exactly what it takes, and its VA words
 * are written; (3) the constituent table must be complete. Any refusal undoes this translation's pool placements. */
static D_NOINLINE uint32_t d_unit_finish(const xlat12_draw_extra *ex, xlat12_unit *U, uint32_t *out, uint32_t n,
                                         uint32_t tail_end, uint32_t cursor, xlat12_draw_stats *ds)
{
    U->stream_end = tail_end;
    U->last_head_out = U->cons_out[U->ncons - 1u];
    if (U->cons_seen != U->ncons) { ds->err_op = 0xE3u; ds->err_in_dword = U->cons_in[U->cons_seen < U->ncons ? U->cons_seen : 0u];
                                    return XLAT12_IB_ERR_DRAW_SHAPE; }   /* a constituent head was never walked past */
    if (ex->ring_va)
        for (uint32_t q = 0; q < U->nhs; q++) {
            const uint32_t lo = U->hs_lo[q], hi = U->hs_hi[q];
            if (out[lo] == 0u && out[hi] == 0u) {
                out[lo] = (uint32_t)ex->gs_sgpr0_va; out[hi] = (uint32_t)(ex->gs_sgpr0_va >> 32);
                ds->ring_ptr_stages |= 2u; U->hs_patched++;
            } else if (out[lo] != (uint32_t)ex->gs_sgpr0_va || out[hi] != (uint32_t)(ex->gs_sgpr0_va >> 32)) {
                ds->ring_ptr_stages |= 8u;   /* Apple's own HS program address: never clobbered */
            }
        }
    uint32_t ra[2], rl[2];
    ra[0] = U->ocur; rl[0] = U->tail_in_place ? cursor - U->ocur : 0u;
    ra[1] = tail_end; rl[1] = n - tail_end;
    U->slack = rl[0] + rl[1];
    if (U->pack) return d_unit_pack(ex, U, out, n, ds);   /* build 0.0.501 (switch 67); OFF: the loop below, unchanged */
    for (uint32_t b = 0; b < U->npend; b++) {
        xlat12_unit_pend *pb = &U->pend[b];
        const uint32_t blen = 8u + 8u * pb->tn + 4u * pb->sn + pb->ext_len;
        uint32_t used = 0u;
        for (uint32_t r = 0; r < 2u && !used; r++) {
            const uint32_t o = d_unit_fit(ex->ib_va + 4ull * ra[r], rl[r], pb->align_mask, blen);
            if (o == 0xFFFFFFFFu) continue;
            used = o + blen;
            uint32_t *h = &out[ra[r]];
            h[0] = D_TBL_NOP_HDR | ((used - 2u) << 16);
            for (uint32_t k = 1u; k < o; k++) h[k] = 0u;
            d_unit_write_block(U, pb, &h[o], ex->ib_va + 4ull * (ra[r] + o), out);
            ra[r] += used; rl[r] -= used; U->own_dw += used;
        }
        /* build 0.0.522 (switch 76): tier 0 the frame pool (today), tier 1 the spill tier, LAST (U->spill NULL: nothing) */
        for (uint32_t tier = 0; tier < 2u && !used; tier++) {
        xlat12_pool *pl = tier ? U->spill : U->pool;
        for (uint32_t r = 0; pl && !used && r < pl->nrun && r < XLAT12_POOL_RUNS; r++) {
            xlat12_pool_run *pr = &pl->run[r];
            const uint32_t o = d_unit_fit(pr->va, pr->len, pb->align_mask, blen);
            if (o == 0xFFFFFFFFu) continue;
            if (pl->jn >= XLAT12_UNIT_PEND_MAX) break;   /* cannot journal it: do not take it */
            used = o + blen;
            xlat12_pool_jent *j = &pl->j[pl->jn++];
            j->host = pr->host; j->va = pr->va; j->len = pr->len; j->used = used; j->r = r;
            pr->host[0] = D_TBL_NOP_HDR | ((used - 2u) << 16);
            for (uint32_t k = 1u; k < o; k++) pr->host[k] = 0u;
            d_unit_write_block(U, pb, &pr->host[o], pr->va + 4ull * o, out);
            pr->host += used; pr->va += 4ull * used; pr->len -= used;
            if (tier) U->spill_dw += used; else U->pool_dw += used;
        }
        }
        if (!used) {
            if (U->pool) (void)xlat12_pool_undo(U->pool);
            if (U->spill) (void)xlat12_pool_undo(U->spill);   /* build 0.0.522: both journals, as one refusal */
            return d_tbl_fail(ds, pb->draw_at, XLAT12_TDESC_NO_ROOM);
        }
        pb->placed_len = used; U->placed++;
        ds->table_dwords += used;
    }
    return 0;
}

uint32_t xlat12_pool_add_free(xlat12_pool *pl, uint32_t *out, uint32_t n, uint64_t va)
{
    uint32_t added = 0, i = 0;
    if (!pl || !out) return 0;
    while (i < n) {
        if (out[i] == XLAT12_IB_NOP) {
            uint32_t z = i;
            while (z < n && out[z] == XLAT12_IB_NOP) z++;
            if (z - i >= XLAT12_POOL_MIN_RUN) {
                if (pl->nrun < XLAT12_POOL_RUNS) {
                    pl->run[pl->nrun].host = &out[i]; pl->run[pl->nrun].va = va + 4ull * i; pl->run[pl->nrun].len = z - i;
                    pl->nrun++; added++;
                } else pl->lost++;
            }
            i = z; continue;
        }
        const uint32_t l = plen_at(out, i, n);   /* a packet: its body is never a run, whatever its dwords hold */
        if (!l) break;                           /* a stream this walk does not understand offers nothing more */
        i += l;
    }
    return added;
}

uint32_t xlat12_pool_undo(xlat12_pool *pl)
{
    uint32_t dw = 0;
    if (!pl) return 0;
    while (pl->jn) {
        const xlat12_pool_jent *j = &pl->j[--pl->jn];
        for (uint32_t k = 0; k < j->used; k++) j->host[k] = XLAT12_IB_NOP;
        if (j->r < XLAT12_POOL_RUNS) { pl->run[j->r].host = j->host; pl->run[j->r].va = j->va; pl->run[j->r].len = j->len; }
        dw += j->used;
    }
    return dw;
}

/* 0.0.307: ONE REGION of the stream - the packets between two draws, or before the first. Extracted
 * UNCHANGED from xlat12_ib_translate_draw_ex so that a stream with several draws can be translated region by region; the
 * single-draw case calls it exactly once and its output is byte-identical (the golden r44 vectors prove it). `e` carries
 * the emitter across the call, `from`/`to` are absolute dword indices into `in`. */

uint32_t xlat12_ib_count_draws(const uint32_t *out, uint32_t n)
{
    uint32_t c = 0, i = 0;
    if (!out) return 0xFFFFFFFFu;
    while (i < n) {
        const uint32_t l = plen_at(out, i, n);
        if (!l) return 0xFFFFFFFFu;   /* not a packet stream: never equal to a draw count */
        if (out[i] != XLAT12_IB_NOP && PT(out[i]) == 3u && is_draw(PO(out[i]))) c++;
        i += l;
    }
    return c;
}

/* Predicate part 4: 1 when THIS translation's own packets in in[0..upto) establish a colour-only write set for the draw at
 * `upto`. Tracked by their gfx10 context dword offsets: DB_Z_INFO 0x10 (0x28040, FORMAT [1:0]), DB_STENCIL_INFO 0x11
 * (0x28044, FORMAT [0]), VGT_STRMOUT_CONFIG 0x2e5 (0x28B94), CB_TARGET_MASK 0x8e (0x28238), CB_COLORn_BASE 0x318 + 0xf n
 * (0x28C60 + 0x3C n). A register written from memory (LOAD_CONTEXT_REG / _INDEX, CLEAR_STATE) or through a register-
 * destination WRITE_DATA / COPY_DATA / WAIT_REG_MEM before the draw is a value this scan cannot see: 0. */
/* build 0.0.512 (XLAT12_DE_CLASS_GLASS rows only, `lcr_absent`): a LOAD_CONTEXT_REG every (offset, count) pair of which names
 * only registers gfx12 does not have (xlat12_lookup class XLAT12_CLS_ABSENT; a count of 0 names none) is the packet the
 * translator DROPS (its own LOAD_CONTEXT_REG rung, : `ds->memloaded_dropped++; i += l; continue;` - never emitted), so
 * it sets no register the GPU runs with and hides nothing from this scan. Decoded exactly as that rung decodes it (addr_lo,
 * addr_hi, then pairs; a malformed body or a count over 0x4000 is not absent). b's BA draw follows one such packet with a count
 * of 0 (run10t F107 IB1 unit k11, input dword 1093: `c0036100 00437478 00000004 00000000 00000000`). Pure. */
static uint32_t d_de_lcr_absent(const uint32_t *in, uint32_t i, uint32_t l)
{
    if (l < 4u || ((l - 3u) & 1u) != 0u) return 0u;
    for (uint32_t k = i + 3u; k + 1u < i + l; k += 2u) {
        const uint32_t off = in[k] & 0xFFFFu, cnt2 = in[k + 1u];
        if (cnt2 > 0x4000u) return 0u;
        for (uint32_t j = 0; j < cnt2; j++) {
            uint32_t t12 = 0, cls = XLAT12_CLS_UNKNOWN;
            if (!xlat12_lookup((0xa000u + off + j) << 2, &t12, &cls) || cls != XLAT12_CLS_ABSENT) return 0u;
        }
    }
    return 1u;
}
static D_NOINLINE uint32_t d_de_writeset_ok(const uint32_t *in, uint32_t n, uint32_t upto, uint32_t lcr_absent)
{
    uint32_t seen = 0, zi = 0, si = 0, so = 0, tm = 0, cbb[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for (uint32_t i = 0; i < upto && i < n;) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) return 0u;
        if (h != XLAT12_IB_NOP && PT(h) == 3u) {
            const uint32_t op = PO(h);
            if (op == OP_SET_CONTEXT_REG && l >= 3u) {
                const uint32_t off = in[i + 1u] & 0xFFFFu;
                for (uint32_t k = 0; k + 2u < l; k++) {
                    const uint32_t r = off + k, v = in[i + 2u + k];
                    if (r == 0x10u) { zi = v; seen |= 1u; }
                    else if (r == 0x11u) { si = v; seen |= 2u; }
                    else if (r == 0x2e5u) { so = v; seen |= 4u; }
                    else if (r == 0x8eu) { tm = v; seen |= 8u; }
                    else if (r >= 0x318u && r <= 0x318u + 7u * 0xfu && (r - 0x318u) % 0xfu == 0u) {
                        const uint32_t cb = (r - 0x318u) / 0xfu;
                        cbb[cb] = v; seen |= 0x10u << cb;
                    }
                }
            } else if (op == OP_LOAD_CONTEXT_REG && lcr_absent && d_de_lcr_absent(in, i, l)) {
                /* build 0.0.512: dropped by the translator, never run (d_de_lcr_absent) - glass rows only */
            } else if (op == OP_LOAD_CONTEXT_REG || op == OP_LOAD_CONTEXT_REG_INDEX || op == OP_CLEAR_STATE) {
                return 0u;
            } else if ((op == OP_WRITE_DATA || op == OP_COPY_DATA) && l >= 2u && ((in[i + 1u] >> 8) & 0xFu) == 0u) {
                return 0u;
            } else if (op == OP_WAIT_REG_MEM && l >= 2u && ((in[i + 1u] >> 6) & 3u) == 1u) {
                return 0u;
            }
        }
        i += l;
    }
    if ((seen & 7u) != 7u || (zi & 3u) || (si & 1u) || so) return 0u;
    for (uint32_t cb = 1; cb < 8u; cb++) {
        if ((seen >> (4u + cb)) & 1u) { if (cbb[cb]) return 0u; }
        else if (!(seen & 8u) || ((tm >> (4u * cb)) & 0xFu)) return 0u;
    }
    return 1u;
}

/* Predicate parts 1-3 and 5 (part 4 above), asked ONLY at the table step's refusal: 1 = elide the draw at `at` (the
 * bookkeeping is done: counted, logged, the exports emptied, the refusal cleared); 0 = refuse with `st` exactly as today. */
static D_NOINLINE uint32_t d_draw_elide(const xlat12_draw_profile *cp, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,
                                        const uint32_t *in, uint32_t n, uint32_t at, uint32_t st)
{
    if (st != XLAT12_IB_ERR_DESC || ds->err_op != XLAT12_TDESC_PROVENANCE) return 0u;
    ds->de_seen++;
    if (!cp->ps_table_abi1 || cp->ps_table_abi1 > D_TBL_ABIS) { ds->de_not_row++; return 0u; }
    const DTableAbi *a = &kDTableAbi[cp->ps_table_abi1 - 1u];
    const uint32_t nt = a->ntex;
    uint32_t last = (nt >= 1u && nt <= XLAT12_DRAW_IN_MAX && ds->in_n == nt && !ds->in_proven[nt - 1u] &&
                     ds->in_va[nt - 1u] == ds->prov_va) ? 1u : 0u;
    for (uint32_t i = 0; last && i + 1u < nt; i++) if (!ds->in_proven[i]) last = 0u;
    const DDrawElideRow *r = d_de_row(ex, a->ndw, a->fnv);
    /* build 0.0.512 (XLAT12_DE_CLASS_GLASS): a NOT-LAST row replaces part 2 - its refusal must be on ITS texture (never its
     * last), every earlier texture proven, and the table step's probe must have proven every LATER one (de_nl bit 4). A refusal
     * on any other texture of such a row, the last included, is not-last. Rows with nl_tex 0xFF: part 2 exactly as before. */
    if (r && r->nl_tex != 0xFFu) {
        const uint32_t t = r->nl_tex;
        uint32_t ok = (t + 1u < nt && nt <= XLAT12_DRAW_IN_MAX && ds->de_nl == (uint8_t)((t + 1u) | 0x10u) && ds->in_n == t + 1u &&
                       !ds->in_proven[t] && ds->in_va[t] == ds->prov_va) ? 1u : 0u;
        for (uint32_t i = 0; ok && i < t; i++) if (!ds->in_proven[i]) ok = 0u;
        last = ok;
    }
    if (!last) { ds->de_not_last++; return 0u; }
    if (!r) { ds->de_not_row++; ds->de_nr_ndw = (uint16_t)a->ndw; ds->de_nr_fnv = a->fnv; return 0u; }
    if (r->require_dcc && ds->dcc_unproven == ds->de_dcc0) { ds->de_no_dcc++; return 0u; }
    if (!d_de_writeset_ok(in, n, at, r->cls == XLAT12_DE_CLASS_GLASS ? 1u : 0u)) { ds->de_writeset++; return 0u; }
    if (ds->draw_elided >= XLAT12_DRAW_ELIDE_MAX) { ds->de_cap++; return 0u; }
    const uint32_t q = ds->draw_elided;
    ds->de_at[q] = at; ds->de_row[q] = (uint8_t)r->row; ds->de_va8[q] = (uint32_t)(ds->prov_va >> 8); ds->de_mode[q] = (uint8_t)ds->prov_mode;
    ds->draw_elided++;
    d_in_clear(ds);   /* the draw reads nothing: no export may name this draw's inputs */
    ds->de_nl = 0u;   /* build 0.0.512 */
    ds->err_op = 0xFFFFFFFFu; ds->err_in_dword = 0u; ds->err_reg = 0u;
    ds->prov_va = 0ull; ds->prov_mode = 0u; ds->prov_path = 0u;
    return 1u;
}

/* build 0.0.552 (xlat12_ib.h XLAT12_DE_CLASS_AN / _AN_SHADOW, switch 110;  PLAN (2)) — THE AN ROW, asked at the
 * table draw's step BEFORE d_table_desc, only under the flag with one of the two AN bits. Keyed on the in-force fragment program's
 * kDTableAbi identity (ndw, fnv) == AN's exactly - no other row, no refusal needed. 1 = elide (ON only; bookkept like
 * d_draw_elide's yes: counted, exports emptied, nothing refused); 0 = go to the table step exactly as without the bit. SHADOW
 * answers 0 always and only counts. Parts 4 and 5 of the refusal-side predicate hold here too (the write set, the cap). */
static D_NOINLINE uint32_t d_de_an(const xlat12_draw_profile *cp, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,
                                   const uint32_t *in, uint32_t n, uint32_t at)
{
    const uint32_t cls = ex->draw_elide_rows & (XLAT12_DE_CLASS_AN | XLAT12_DE_CLASS_AN_SHADOW);
    if (!cls || !cp->ps_table_abi1 || cp->ps_table_abi1 > D_TBL_ABIS) return 0u;
    const DTableAbi *a = &kDTableAbi[cp->ps_table_abi1 - 1u];
    if (a->ndw != XLAT12_DE_AN_NDW || a->fnv != XLAT12_DE_AN_FNV) return 0u;
    if (ds->de_an_seen < 255u) ds->de_an_seen++;
    if (!d_de_writeset_ok(in, n, at, 0u)) { if (ds->de_an_ref_ws < 255u) ds->de_an_ref_ws++; return 0u; }
    if (ds->draw_elided >= XLAT12_DRAW_ELIDE_MAX) { if (ds->de_an_ref_cap < 255u) ds->de_an_ref_cap++; return 0u; }
    if (cls != XLAT12_DE_CLASS_AN) { if (ds->de_an_would < 255u) ds->de_an_would++; return 0u; }   /* SHADOW: counted only */
    const uint32_t q = ds->draw_elided;
    ds->de_at[q] = at; ds->de_row[q] = (uint8_t)XLAT12_DE_ROW_AN; ds->de_va8[q] = 0u; ds->de_mode[q] = 0u;
    ds->draw_elided++;
    d_in_clear(ds);   /* the draw reads nothing: no export may name this draw's inputs */
    return 1u;
}

/* The NOP of the draw's own length where the draw would have gone: in place at out[at..], or a unit's through the same
 * steps d_unit_draw takes (close the open packet, room, write, advance the unit's output cursor). */
static D_NOINLINE uint32_t d_draw_elide_emit(xlat12_unit *U, DEmit *e, uint32_t *out, uint32_t at, uint32_t l, xlat12_draw_stats *ds)
{
    if (l < 2u || l - 2u > 0x3FFFu) { ds->err_op = 0xFFu; ds->err_in_dword = at; return XLAT12_IB_ERR_DRAW_SHAPE; }
    uint32_t *d = out + at;
    if (U) {
        d_close(e);
        if (e->cap - e->n < l) { ds->err_op = XLAT12_UNIT_DRAW_ROOM; ds->err_in_dword = at; return XLAT12_IB_ERR_TOO_LONG; }
        d = e->out + e->n;
        U->ocur = e->n + l;
    }
    d[0] = 0xC0001000u | ((l - 2u) << 16);
    for (uint32_t k = 1; k < l; k++) d[k] = 0u;
    return 0u;
}

/* The backstop (xlat12_ib.h): 1 (refuse, err_op XLAT12_DE_BACKSTOP) when the finished output does not carry exactly
 * draws - draw_elided draw packets. Exported so its answer is tested directly. */
uint32_t xlat12_ib_de_backstop(const uint32_t *out, uint32_t n, xlat12_draw_stats *ds)
{
    if (ds->draw_elided <= ds->draws && xlat12_ib_count_draws(out, n) == ds->draws - ds->draw_elided) return 0u;
    ds->err_op = XLAT12_DE_BACKSTOP; ds->err_reg = 0u; ds->err_in_dword = 0u;
    return 1u;
}

/* ---- build 0.0.487 (xlat12_ib.h XLAT12_EXTRA_CS_ELIDE, switch 57; notes/design/COMPUTE-N.md Q6) ----------------------
 * P2's register set: the bit of gfx10 compute register `g10` in DPair.cs_ok, or -1 for any other address. */
static int d_cs_bit(uint32_t g10)
{
    switch (g10) {
    case 0xb810u: return 0;  case 0xb814u: return 1;  case 0xb818u: return 2;    /* COMPUTE_START_X/Y/Z */
    case 0xb81cu: return 3;  case 0xb820u: return 4;  case 0xb824u: return 5;    /* COMPUTE_NUM_THREAD_X/Y/Z */
    case 0xb830u: return 6;  case 0xb834u: return 7;                              /* COMPUTE_PGM_LO/HI */
    case 0xb848u: return 8;  case 0xb84cu: return 9;                              /* COMPUTE_PGM_RSRC1/2 */
    case 0xb854u: return 10; case 0xb860u: return 11; case 0xb8a0u: return 12;   /* RESOURCE_LIMITS, TMPRING_SIZE, RSRC3 */
    default: break;
    }
    if (g10 >= 0xb900u && g10 <= 0xb91cu) return 13 + (int)((g10 - 0xb900u) >> 2);   /* COMPUTE_USER_DATA_0.._7 */
    return -1;
}
/* P2, per write: N's exact value for bit `b` (COMPUTE-N.md Q1's table, all 226 captured dispatches). */
static int d_cs_want(int b, uint32_t v)
{
    switch (b) {
    case 3:  return v == 0x40u;                   /* NUM_THREAD_X: 64 threads */
    case 4: case 5: return v == 1u;               /* NUM_THREAD_Y/Z */
    case 6:  return 1;                            /* PGM_LO: any - P4 proves what lives there */
    case 7:  return v < 0x100u;                   /* PGM_HI: the address bits only (d_pair_va's own [7:0]) */
    case 8:  return v == XLAT12_CS_N_PGM_RSRC1;
    case 9:  return v == XLAT12_CS_N_PGM_RSRC2;
    case 13: return 1;                            /* USER_DATA_0: the V# base - P5 proves what it is */
    case 14: return v == XLAT12_CS_N_UD1;         /* V# word 1: base_hi 4, stride 16 */
    case 15: return v == XLAT12_CS_N_UD2;         /* V# word 2: 1024 records */
    case 16: return v == XLAT12_CS_N_UD3;         /* V# word 3 */
    default: return v == 0u;                      /* START_X/Y/Z, RESOURCE_LIMITS, TMPRING, RSRC3, USER_DATA_4.._7 (fill) */
    }
}
static void d_cs_note(DPair *p, uint32_t g10, uint32_t v)
{
    const int b = d_cs_bit(g10);
    if (b < 0) return;
    if (b == 6) p->cs_lo = v; else if (b == 7) p->cs_hi = v; else if (b == 13) p->cs_ud0 = v;
    if (d_cs_want(b, v)) p->cs_ok |= 1u << b; else p->cs_ok &= ~(1u << b);
}
/* P2's other half: a register-destination WRITE_DATA / COPY_DATA / WAIT_REG_MEM whose register lies in the compute block
 * [0xb800, 0xba00) (dwords 0x2e00..0x2e7f) FORGETS every compute value this translation recorded - the state in force is
 * then not one this translation proved. Decoded exactly as d_region's own register-write refusal decodes them. */
static void d_cs_forget(DPair *p, const uint32_t *pk, uint32_t l)
{
    const uint32_t op = PO(pk[0]);
    uint32_t first = 0u, last = 0u, hit = 0u;
    if (op == OP_WRITE_DATA && l >= 5u && ((pk[1] >> 8) & 0xFu) == 0u) {
        first = pk[2]; last = ((pk[1] >> 16) & 1u) ? first : first + (l - 4u) - 1u; hit = 1u;
    } else if (op == OP_COPY_DATA && l >= 6u && ((pk[1] >> 8) & 0xFu) == 0u) {
        first = pk[4]; last = ((pk[1] >> 16) & 1u) ? first + 1u : first; hit = 1u;
    } else if (op == OP_WAIT_REG_MEM && l >= 7u && ((pk[1] >> 4) & 3u) != 1u) {
        first = pk[2] < pk[3] ? pk[2] : pk[3]; last = pk[2] < pk[3] ? pk[3] : pk[2]; hit = 1u;
    }
    if (hit && first <= 0x2e7fu && last >= 0x2e00u) p->cs_ok = 0u;
}
/* P5: over [from, to) - the dispatch's end up to the next draw, or the segment's end - the window's OWN writes put in force,
 * at some packet boundary, a colour target 0 bound WITH DCC: CB_COLOR0_INFO with DCC_ENABLE (bit 28) and
 * CB_COLOR0_DCC_BASE == base >> 8, both written in the window and in force together; and no CB_COLOR0_DCC_BASE_EXT write
 * in the window is non-zero (unwritten or 0). A bind that is missing, whose DCC_ENABLE is clear, whose DCC_BASE names
 * another buffer, or that only follows the next draw, is not N's consumer and refuses. "Some boundary", not "the last
 * write": run10c F101/F104/F115 bind N's target with DCC and then rebind colour target 0 to another surface before the
 * draw - the design's "finds" (COMPUTE-N.md Q6 P5, Q3's 206/206) - and the buffer is a DCC key buffer either way. */
static int d_cs_dcc_bound(const uint32_t *in, uint32_t n, uint32_t from, uint32_t to, uint64_t base)
{
    uint32_t info = 0u, infoSeen = 0u, dcc = 0u, dccSeen = 0u, found = 0u;
    for (uint32_t i = from; i < to; ) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) return 0;
        if (h != XLAT12_IB_NOP && PT(h) == 3u && PO(h) == OP_SET_CONTEXT_REG && l >= 3u) {
            const uint32_t off = in[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (0xa000u + off + k) << 2, v = in[i + 2u + k];
                if (g10 == 0x28c70u) { info = v; infoSeen = 1u; }          /* CB_COLOR0_INFO */
                else if (g10 == 0x28c94u) { dcc = v; dccSeen = 1u; }       /* CB_COLOR0_DCC_BASE (ABSENT on gfx12) */
                else if (g10 == 0x28ea0u && v != 0u) return 0;             /* CB_COLOR0_DCC_BASE_EXT (ABSENT on gfx12) */
            }
            if (infoSeen && ((info >> 28) & 1u) && dccSeen && ((uint64_t)dcc << 8) == base) found = 1u;
        }
        i += l;
    }
    return (int)found;
}
/* P2..P5 over the dispatch at in[i] (P1 is the caller's flag test plus translate_draw_ex's ERR_ARG). 0 = PROVEN; else the
 * number of the FIRST part that failed. Not inlined, so its scan never lands on translate_draw_ex's own frame. */
static D_NOINLINE uint32_t d_cs_elide_why(const xlat12_draw_extra *ex, const DPair *pr, const uint32_t *in, uint32_t n,
                                          uint32_t i, uint32_t l, uint32_t to)
{
    if (pr->cs_ok != XLAT12_CS_N_ALL) return 2u;
    if (l != 5u || in[i] != 0xC0031502u || in[i + 1u] != 0x10u || in[i + 2u] != 1u || in[i + 3u] != 1u || in[i + 4u] != 1u)
        return 3u;
    if (!ex->cs_is_n || ex->cs_is_n(ex->cs_ctx, d_pair_va(pr->cs_lo, pr->cs_hi)) != 1) return 4u;
    const uint64_t base = (uint64_t)pr->cs_ud0 | ((uint64_t)(XLAT12_CS_N_UD1 & 0xFFFFu) << 32);
    if (!d_cs_dcc_bound(in, n, i + l, to, base)) return 5u;
    return 0u;
}

/* ---- build 0.0.535 (xlat12_ib.h XLAT12_EXTRA_NCLEAR, switch 91;  fix 1) ---------------------------------------
 * 1 when in[i..i+l) is a SET_SH_REG / SET_SH_REG_INDEX packet writing ONLY compute registers: dword offsets [0x200, 0x280)
 * from the SH base (bytes [0xb800, 0xba00)), the block d_cs_forget guards. */
static int d_nc_is_cs_set(const uint32_t *p, uint32_t l)
{
    const uint32_t h = p[0], op = PO(h);
    if (h == XLAT12_IB_NOP || PT(h) != 3u || (op != OP_SET_SH_REG && op != OP_SET_SH_REG_INDEX) || l < 3u) return 0;
    const uint32_t off = p[1] & 0xFFFFu;
    return off >= 0x200u && off + (l - 2u) <= 0x280u;
}
/* The run's bookkeeping, at the top of d_region's loop for EVERY packet (under the flag only). `on` = the output cursor now. */
static void d_nc_run_note(DPair *pr, const uint32_t *p, uint32_t l, uint32_t on, uint32_t i)
{
    if (p[0] != XLAT12_IB_NOP && PT(p[0]) == 3u && PO(p[0]) == OP_DISPATCH_DIRECT) return;   /* the elision reads the run */
    if (d_nc_is_cs_set(p, l)) { if (!pr->nc_run_ok) { pr->nc_run_ok = 1u; pr->nc_run_out = on; pr->nc_run_in = i; } }
    else pr->nc_run_ok = 0u;
}
/* build 0.0.535 fix round (a) (review: gfx12 CP DMA is not L2-coherent - mesa ac_gpu_info.c sets cp_dma_use_L2 false and
 * cp_sdma_ge_use_system_memory_scope for GFX12 - so the fill's visibility rests on APPLE's OWN barriers around N). THE BARRIERS, in
 * Apple's input, both required:
 *   BEFORE (d_nc_bar_before, over [from, run_in) of this region): a RELEASE_MEM whose control is 0x514 (EVENT_TYPE 0x14
 *   CACHE_FLUSH_AND_INV_TS, EVENT_INDEX 5: end of pipe) writing DATA to fence address A, then a memory WAIT_REG_MEM (bit 4) equal
 *   (function 3) on the same A for that DATA, then only EVENT_WRITE 0x407 (CS_PARTIAL_FLUSH) and finally, IMMEDIATELY before N's
 *   compute run, an ACQUIRE_MEM whose GCR_CNTL (dword 7: mesa pkt3.json GCR_CNTL at pkt3 0x587) has GL2_WB (bit 15);
 *   AFTER (d_nc_bar_after): the first two packets after the dispatch are EVENT_WRITE 0x407 (CS_PARTIAL_FLUSH) and an ACQUIRE_MEM
 *   with GL2_INV (GCR_CNTL bit 14), both before the next draw (the window ends at the next draw).
 * NOP packets between them are skipped; anything else fails the rule. 1 = the barrier holds. */
#define D_NC_GCR_GL2_INV (1u << 14)
#define D_NC_GCR_GL2_WB  (1u << 15)
static int d_nc_bar_before(const uint32_t *in, uint32_t n, uint32_t from, uint32_t run_in)
{
    enum { S_NONE = 0, S_REL, S_WAIT, S_ACQ };
    uint32_t stage = S_NONE, a_lo = 0u, a_hi = 0u, data = 0u;
    for (uint32_t i = from; i < run_in; ) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) return 0;
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        const uint32_t op = PO(h);
        if (op == OP_RELEASE_MEM && l == 8u && in[i + 1u] == 0x514u) {
            stage = S_REL; a_lo = in[i + 3u]; a_hi = in[i + 4u]; data = in[i + 5u];
        } else if (stage == S_REL && op == OP_WAIT_REG_MEM && l == 7u && ((in[i + 1u] >> 4) & 1u) && (in[i + 1u] & 7u) == 3u &&
                   in[i + 2u] == a_lo && in[i + 3u] == a_hi && in[i + 4u] == data) {
            stage = S_WAIT;
        } else if (stage == S_WAIT && op == OP_EVENT_WRITE && l == 2u && in[i + 1u] == 0x407u) {
            /* CS_PARTIAL_FLUSH between the wait and the acquire: allowed */
        } else if (stage == S_WAIT && op == OP_ACQUIRE_MEM && l == 8u && (in[i + 7u] & D_NC_GCR_GL2_WB)) {
            stage = S_ACQ;
        } else stage = S_NONE;   /* anything else after the triplet breaks the chain */
        i += l;
    }
    return stage == S_ACQ;
}
static int d_nc_bar_after(const uint32_t *in, uint32_t n, uint32_t from, uint32_t to)
{
    uint32_t k = 0u;
    for (uint32_t i = from; i < to && k < 2u; ) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) return 0;
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        if (k == 0u && !(PO(h) == OP_EVENT_WRITE && l == 2u && in[i + 1u] == 0x407u)) return 0;
        if (k == 1u && !(PO(h) == OP_ACQUIRE_MEM && l == 8u && (in[i + 7u] & D_NC_GCR_GL2_INV))) return 0;
        k++; i += l;
    }
    return k == 2u;
}
/* E1: the extent over [from, to) (the window P5 scanned). 0 = proven, *va / *size set; 1 = refused. */
static uint32_t d_nc_extent(const uint32_t *in, uint32_t n, uint32_t from, uint32_t to, uint64_t dccBase, uint64_t *va, uint32_t *size)
{
    uint32_t v[9] = { 0u }, seen = 0u, found = 0u;
    uint64_t fva = 0ull; uint32_t fsz = 0u;
    /* 0 BASE 0x28c60, 1 INFO 0x28c70, 2 ATTRIB 0x28c74, 3 VIEW 0x28c6c, 4 DCC_BASE 0x28c94, 5 ATTRIB2 0x28ec0, 6 ATTRIB3 0x28ee0,
     * 7 BASE_EXT 0x28e40, 8 DCC_BASE_EXT 0x28ea0 */
    static const uint32_t kReg[9] = { 0x28c60u, 0x28c70u, 0x28c74u, 0x28c6cu, 0x28c94u, 0x28ec0u, 0x28ee0u, 0x28e40u, 0x28ea0u };
    for (uint32_t i = from; i < to; ) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        if (!l) return 1u;
        if (h != XLAT12_IB_NOP && PT(h) == 3u && PO(h) == OP_SET_CONTEXT_REG && l >= 3u) {
            const uint32_t off = in[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (0xa000u + off + k) << 2;
                for (uint32_t r = 0; r < 9u; r++) if (kReg[r] == g10) { v[r] = in[i + 2u + k]; seen |= 1u << r; }
            }
            /* a boundary where P5's bind holds with every register the extent needs written in the window (BASE, INFO,
             * ATTRIB, VIEW, DCC_BASE, ATTRIB2, ATTRIB3); a boundary before the last of them is not yet a candidate */
            if ((seen & 0x7Fu) == 0x7Fu && ((v[1] >> 28) & 1u) && ((uint64_t)v[4] << 8) == dccBase) {
                if (((seen >> 7) & 1u) && v[7]) return 1u;                             /* BASE_EXT */
                if (((seen >> 8) & 1u) && v[8]) return 1u;                             /* DCC_BASE_EXT */
                if (v[2] || v[3]) return 1u;                                           /* samples/fragments, slice */
                const uint32_t fmt = (v[1] >> 2) & 0x1Fu, bpp = fmt == 10u ? 4u : fmt == 12u ? 8u : 0u;
                const uint32_t h14 = (v[5] & 0x3FFFu) + 1u, w14 = ((v[5] >> 14) & 0x3FFFu) + 1u, mip = v[5] >> 28;
                const uint32_t depth = v[6] & 0x1FFFu, sw = (v[6] >> 14) & 0x1Fu, rtype = (v[6] >> 24) & 3u;
                if (!bpp || mip || depth || sw != 27u || rtype != 1u) return 1u;
                const uint32_t bw = 128u, bh = bpp == 4u ? 128u : 64u;               /* the 64 KiB 2D block */
                const uint64_t want = (uint64_t)((w14 + bw - 1u) / bw * bw) * (uint64_t)((h14 + bh - 1u) / bh * bh) * bpp;
                const uint64_t base = (uint64_t)v[0] << 8;
                if (!base || base >= dccBase || dccBase - base != want || want > 0xFFFFFFFFull || (want & 3u)) return 1u;
                if (found && (fva != base || fsz != (uint32_t)want)) return 1u;        /* every boundary names one extent */
                found = 1u; fva = base; fsz = (uint32_t)want;
            }
        }
        i += l;
    }
    if (!found) return 1u;
    *va = fva; *size = fsz;
    return 0u;
}
/* E2/E3 at a PROVEN N dispatch in[i..i+l) (d_cs_elide_why answered 0), `e` closed. 1 = the fill replaced N's compute run and
 * the dispatch in the output; 0 = fall back to 57's NOP (counted, nothing written). Not inlined: its locals stay off
 * translate_draw_ex's frame. */
static D_NOINLINE uint32_t d_nclear(xlat12_draw_stats *ds, const DPair *pr, DEmit *e, const uint32_t *in, uint32_t n,
                                    uint32_t i, uint32_t l, uint32_t from, uint32_t to)
{
    /* fix round (b): past XLAT12_NCLEAR_MAX fills in this translation, 57's NOP (the kext's check can then name every fill) */
    if (ds->nc_filled >= XLAT12_NCLEAR_MAX) { ds->nc_ref_max = 1u; return 0u; }
    const uint64_t dccBase = (uint64_t)pr->cs_ud0 | ((uint64_t)(XLAT12_CS_N_UD1 & 0xFFFFu) << 32);
    uint64_t va = 0ull; uint32_t size = 0u;
    if (d_nc_extent(in, n, i + l, to, dccBase, &va, &size)) { ds->nc_ref_extent = 1u; return 0u; }
    /* the run: recorded at its first packet, and re-checked in the OUTPUT - every packet from there to the cursor a compute SET */
    const uint32_t p = pr->nc_run_out;
    if (!pr->nc_run_ok || p > e->n || pr->nc_run_in >= i) { ds->nc_ref_run = 1u; return 0u; }
    /* fix round (a): Apple's own barriers on both sides of N, in its input */
    if (!d_nc_bar_before(in, n, from, pr->nc_run_in) || !d_nc_bar_after(in, n, i + l, to)) { ds->nc_ref_barrier = 1u; return 0u; }
    for (uint32_t j = p; j < e->n; ) {
        const uint32_t lj = plen_at(e->out, j, e->n);
        if (!lj || !d_nc_is_cs_set(&e->out[j], lj)) { ds->nc_ref_run = 1u; return 0u; }
        j += lj;
    }
    const uint32_t k = (size + XLAT12_NCLEAR_MAX_BYTES - 1u) / XLAT12_NCLEAR_MAX_BYTES, need = 7u * k;
    if ((uint64_t)p + need > (uint64_t)i + l || (uint64_t)p + need > e->cap) { ds->nc_ref_room = 1u; return 0u; }
    e->n = p;
    for (uint32_t q = 0; q < k; q++) {
        const uint64_t d = va + (uint64_t)q * XLAT12_NCLEAR_MAX_BYTES;
        const uint32_t b = q + 1u < k ? XLAT12_NCLEAR_MAX_BYTES : size - q * XLAT12_NCLEAR_MAX_BYTES;
        e->out[e->n++] = XLAT12_NCLEAR_HDR; e->out[e->n++] = XLAT12_NCLEAR_CTRL;
        e->out[e->n++] = 0u; e->out[e->n++] = 0u;                                      /* data 0, src_hi 0 */
        e->out[e->n++] = (uint32_t)d; e->out[e->n++] = (uint32_t)(d >> 32);
        e->out[e->n++] = b;                                                             /* COMMAND: BYTE_COUNT only */
    }
    ds->nc_va8[ds->nc_filled] = (uint32_t)(va >> 8); ds->nc_len[ds->nc_filled] = size;   /* < XLAT12_NCLEAR_MAX: checked on entry */
    ds->nc_filled++; ds->nc_bytes += size; ds->nc_pkts += k;
    return 1u;
}
uint32_t xlat12_ib_nclear_check(const uint32_t *out, uint32_t n, const xlat12_draw_stats *ds)
{
    if (!out || !ds) return 1u;
    uint32_t pk = 0u, q = 0u;
    uint64_t want = 0ull, left = 0ull;   /* the recorded fill being matched: next destination, bytes still to cover */
    for (uint32_t i = 0; i < n; ) {
        const uint32_t h = out[i], l = plen_at(out, i, n);
        if (!l) return 1u;
        if (h != XLAT12_IB_NOP && PT(h) == 3u && PO(h) == OP_DMA_DATA) {
            if (!xlat12_ib_nclear_pkt_ok(&out[i], l)) return 1u;
            const uint64_t d = (uint64_t)out[i + 4u] | ((uint64_t)out[i + 5u] << 32);
            if (!left && q < ds->nc_filled && q < XLAT12_NCLEAR_MAX) { want = (uint64_t)ds->nc_va8[q] << 8; left = ds->nc_len[q]; q++; }
            if (!left) return 1u;                                    /* a fill packet no recorded fill accounts for */
            if (d != want || out[i + 6u] > left) return 1u;
            want += out[i + 6u]; left -= out[i + 6u];
            pk++;
        } else if (left) return 1u;                                  /* a recorded fill's packets are contiguous */
        i += l;
    }
    const uint32_t rec = ds->nc_filled < XLAT12_NCLEAR_MAX ? ds->nc_filled : XLAT12_NCLEAR_MAX;
    return (pk == ds->nc_pkts && q == rec && !left) ? 0u : 1u;
}
/* build 0.0.535 item 4: the draw's own CB0 and scissors (xlat12_tex_state), from a SET_CONTEXT_REG write. */
static void d_ts_note(xlat12_tex_state *t, uint32_t g10, uint32_t v)
{
    switch (g10) {
    case 0x28c60u: t->cb0 = v; t->seen |= 1u; break;
    case 0x28e40u: t->cb0_ext = v; t->seen |= 2u; break;
    case 0x28204u: t->win_tl = v; t->seen |= 4u; break;
    case 0x28208u: t->win_br = v; t->seen |= 8u; break;
    case 0x28240u: t->gen_tl = v; t->seen |= 0x10u; break;
    case 0x28244u: t->gen_br = v; t->seen |= 0x20u; break;
    case 0x28250u: t->vp_tl = v; t->seen |= 0x40u; break;
    case 0x28254u: t->vp_br = v; t->seen |= 0x80u; break;
    default: break;
    }
}

/* build 0.0.502: 1 when gfx12 DB_SPI_VRS_CENTER_LOCATION (0x28068) = 0 is to be written NOW, in front
 * of the stream's own gfx12 DB_SHADER_CONTROL (0x2806c, gfx10.3 0x2880c's FIELD_REPACK row) - only with a ring (the same
 * scope as d_rings' fallback), only in region 0 (before the first draw) and only once. d_add then merges the two into ONE
 * SET_CONTEXT_REG: +1 dword where d_rings' own packet costs 3, which is what keeps tight units' deferred records placing
 * (decide44 F77/F54: a 3-dword block write left 100 of the 101 own dwords their four records need, NO_ROOM). */
static int d_vrs_prefollow(const xlat12_draw_extra *ex, const DPair *pr, uint32_t g12)
{
    return g12 == 0x2806cu && ex && ex->ring_va && pr->vrs_state == 0u;
}

/* build 0.0.537 fix round item 3 (xhigh review): 1 when the rest of the barrier's region - p[l, rest), the input
 * from just after the barrier up to the region's draw - holds Apple's buried fence828 packet (a 9-dword NOP whose body is a
 * RELEASE_MEM: C0071000 C0064900, gfx_fence828.h n48_f828_find's own pattern). Converting there would move that packet 8 dwords
 * later, off the offset R1's fence identity requires it at (gfx_memdst.h: the exempt row must sit where Apple's input holds it):
 * the per-draw trailer barrier (EVENT_WRITE 0x16 + the barrier + that NOP) is the case. An unwalkable rest also answers 1. Pure. */
static int d_pws_fence_after(const uint32_t *p, uint32_t l, uint32_t rest)
{
    for (uint32_t i = l; i < rest; ) {
        const uint32_t k = plen_at(p, i, rest);
        if (!k) return 1;
        if (p[i] == 0xC0071000u && i + 1u < rest && p[i + 1u] == 0xC0064900u) return 1;
        i += k;
    }
    return 0;
}

/* build 0.0.537 (xlat12_ib.h XLAT12_EXTRA_PWS, switch 93; ) — ONE ACQUIRE_MEM met by d_region's pass-through
 * branch under the flag. Returns 1 when it emitted our RELEASE_MEM(PWS) + ACQUIRE_MEM(PWS) pair in place of Apple's CB/DB barrier
 * (16 dwords at e->n, the release first); 0 when the caller must copy the packet verbatim, as today: any other shape (counted
 * `pws_other`), or Apple's barrier with no room for the pair - the region after the last draw (`rest` 0: never room), fewer
 * than 16 dwords left at the barrier, a fallback pass (pr->pws bit 0 clear), or (fix round item 3) Apple's buried fence828 packet
 * later in the same region (d_pws_fence_after: the pair would move it; counted `pws_fence` too) - counted `pws_noroom`. The encodings and their derivation
 * are on XLAT12_EXTRA_PWS. Nothing else is read or written. */
static D_NOINLINE int d_pws(xlat12_draw_stats *ds, const DPair *pr, DEmit *e, const uint32_t *p, uint32_t l, uint32_t rest)
{
    if (!xlat12_ib_pws_apple_ok(p, l)) { if (ds->pws_other < 0xFFFFu) ds->pws_other++; return 0; }
    if (ds->pws_seen < 0xFFFFu) ds->pws_seen++;
    const int predraw = rest != 0u;
    if (!(pr->pws & 1u) || !predraw || e->cap - e->n < 16u || d_pws_fence_after(p, l, rest)) {
        if (ds->pws_noroom < 0xFFFFu) ds->pws_noroom++;
        if (!predraw && ds->pws_tail < 0xFFFFu) ds->pws_tail++;
        else if (predraw && (pr->pws & 1u) && e->cap - e->n >= 16u && ds->pws_fence < 0xFFFFu) ds->pws_fence++;
        return 0;
    }
    uint32_t *o = &e->out[e->n];
    o[0] = XLAT12_PWS_REL_HDR; o[1] = XLAT12_PWS_REL_W1;
    for (uint32_t k = 2u; k < 8u; k++) o[k] = 0u;
    o[8] = XLAT12_PWS_ACQ_HDR; o[9] = XLAT12_PWS_ACQ_W1; o[10] = 0xFFFFFFFFu; o[11] = 0x01FFFFFFu;
    o[12] = 0u; o[13] = 0u; o[14] = XLAT12_PWS_ACQ_W6; o[15] = XLAT12_PWS_ACQ_W7;
    e->n += 16u;
    if (ds->pws_conv < 0xFFFFu) ds->pws_conv++;
    return 1;
}

uint32_t xlat12_ib_pws_check(const uint32_t *out, uint32_t n)
{
    uint32_t i = 0, pending = 0;
    if (!out) return 1u;
    while (i < n) {
        const uint32_t h = out[i], l = plen_at(out, i, n);
        if (!l) return 1u;
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        const uint32_t op = PO(h);
        const int rel = op == OP_RELEASE_MEM && l >= 2u && (out[i + 1u] & 0x80000000u);
        const int acq = op == OP_ACQUIRE_MEM && l >= 7u && (out[i + 6u] & 0x80000000u);
        if (pending) {   /* the packet right after our release must be our acquire */
            if (!acq || !xlat12_ib_pws_acquire_ok(&out[i], l)) return 1u;
            pending = 0u;
        } else if (acq) {
            return 1u;   /* a PWS acquire with no PWS release directly before it */
        } else if (rel) {
            if (!xlat12_ib_pws_release_ok(&out[i], l)) return 1u;
            pending = 1u;
        }
        i += l;
    }
    return pending;   /* a release at the very end has no acquire */
}

/* build 0.0.539 (xlat12_ib.h XLAT12_PWS_SLOT_*) — d_region's NOP branch at input dword i (a NOP of l dwords): 1 when it is
 * EXACTLY the slot d_region marked after a conversion (the marked dword, re-matched here: SLOT0 SLOT1, 8 dwords) - the caller then
 * emits nothing for it (the room credit, taken only now) and it is counted `pws_slot`; 0 for every other NOP (copied as always). */
static int d_pws_slot_drop(xlat12_draw_stats *ds, const DPair *pr, const uint32_t *in, uint32_t i, uint32_t l)
{
    if (!pr->pws_slot || i != pr->pws_slot) return 0;
    if (!xlat12_ib_pws_slot_pkt(&in[i], l)) return 0;
    if (ds->pws_slot < 0xFFFFu) ds->pws_slot++;
    return 1;
}

static uint32_t d_region(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,
                         const uint32_t *in, uint32_t n, uint32_t *out, uint32_t from, uint32_t to, DEmit *e,
                         DPair *pr, DTable *tb)
{
    uint32_t i = from;
    xlat12_unit *const U = d_unit_of(ex);   /* build 0.0.480: NULL unless XLAT12_EXTRA_UNIT */
    while (i < to) {
        const uint32_t h = in[i], l = plen_at(in, i, n);
        ds->err_in_dword = i;
        uint32_t s = 0;
        /* build 0.0.480: a constituent head of the unit lands here (its output dword, and the previous one's feed) */
        if (U && U->cons_seen < U->ncons && i == U->cons_in[U->cons_seen]) d_unit_head(U, e);
        /* build 0.0.535 (XLAT12_EXTRA_NCLEAR): where N's compute run begins in the output. OFF: one test. */
        if (l && ex && (ex->flags & XLAT12_EXTRA_NCLEAR)) d_nc_run_note(pr, &in[i], l, e->n, i);
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) {
            d_close(e);
            /* build 0.0.539 (XLAT12_PWS_SLOT_*): Apple's disabled top-of-pipe wait slot after a CONVERTED barrier - nothing */
            if (d_pws_slot_drop(ds, pr, in, i, l)) { i += l; continue; }
            if (e->cap - e->n < l) { ds->err_op = OP_NOP; return XLAT12_IB_ERR_TOO_LONG; }
            for (uint32_t k = 0; k < l; k++) out[e->n++] = in[i + k];
            i += l; continue;
        }
        const uint32_t op = PO(h);
        ds->err_op = op;
        /* 0.0.390: count, never decide. Reached only for packets OUTSIDE a NOP body, because the NOP
         * branch above copies a NOP whole and continues. Nothing below reads either counter. */
        if (op == OP_WAIT_REG_MEM) ds->r4_waits++;
        else if (op == OP_WRITE_DATA || op == OP_COPY_DATA || op == OP_RELEASE_MEM) ds->r4_memwrites++;
        /* build 0.0.487 (xlat12_ib.h XLAT12_EXTRA_CS_ELIDE, switch 57; COMPUTE-N.md Q6 items 1-2) — THE COMPUTE-N ELIDE,
         * placed AFTER the R4 counters (a dispatch is neither a wait nor a write: r4 is unchanged) and BEFORE the D7 READSET
         * block (an elided dispatch reads nothing, so it sets no XLAT12_RS_WHY_DISPATCH). A PROVEN N dispatch (P2-P5 in
         * d_cs_elide_why) becomes a same-length PACKET3 NOP in place; anything else falls through to today's code, the
         * READSET decline and then IB_ERR_UNLISTED. OFF (the flag unset) this `if` is one test and nothing else. */
        if (op == OP_DISPATCH_DIRECT && ex && (ex->flags & XLAT12_EXTRA_CS_ELIDE)) {
            const uint32_t why = d_cs_elide_why(ex, pr, in, n, i, l, to);
            ds->cs_seen++;
            if (why == 0u) {
                d_close(e);
                /* build 0.0.535 (XLAT12_EXTRA_NCLEAR, switch 91): the zero fill in place of N's compute run and dispatch,
                 * or - not provable here - 57's NOP below, unchanged. */
                if ((ex->flags & XLAT12_EXTRA_NCLEAR) && d_nclear(ds, pr, e, in, n, i, l, from, to)) { ds->cs_elided++; i += l; continue; }
                if (e->cap - e->n < l) return XLAT12_IB_ERR_TOO_LONG;
                out[e->n++] = XLAT12_CS_ELIDE_NOP;
                for (uint32_t k = 1u; k < l; k++) out[e->n++] = 0u;
                ds->cs_elided++;
                i += l; continue;
            }
            if (why == 2u) ds->cs_ref_p2++;
            else if (why == 3u) ds->cs_ref_p3++;
            else if (why == 4u) ds->cs_ref_p4++;
            else ds->cs_ref_p5++;
        }
        /* D7 (D4-PRIME-FIXES.md item 7,  (B)), REVERTED at reviewer item 4 (0.0.442, review of
         * 0.0.441) — DISPATCH_DIRECT/_INDIRECT (0x15/0x16) and the INDIRECT draws (DRAW_INDIRECT/DRAW_INDEX_INDIRECT,
         * 0x24/0x25) still SET the read-set decline bit under READSET (none of the four name their real inputs
         * statically), but the WHOLE TRANSLATION now REFUSES UNLISTED exactly as 0.0.440 had it - it does NOT copy
         * the packet through. 0.0.441's own text here ("so the frame can still commit while the read-set machinery
         * marks itself INCOMPLETE") was wrong: passing a dispatch/indirect-draw packet through is a translation
         * policy change no switch should silently make, and the review found nothing downstream actually reads
         * rs_decl_why to gate a commit on it - only xlat12_ib_draw_verify's own UNLISTED-opcode walk would have
         * caught the copied-through packet AFTER the fact, one rung later than it should refuse. Gated: with
         * READSET off, this branch does not run and the bit is never set - the existing UNLISTED refusal below
         * (unconditional on the opcode) is reached exactly as before this item existed. */
        if ((op == OP_DISPATCH_DIRECT || op == OP_DISPATCH_INDIRECT || op == OP_DRAW_INDIRECT || op == OP_DRAW_INDEX_INDIRECT) &&
            ex && (ex->flags & XLAT12_EXTRA_READSET)) {
            ds->rs_declined = 1u;
            ds->rs_decl_why |= XLAT12_RS_WHY_DISPATCH;
        }
        /* D4-PRIME-FIXES.md item 4 (D4-4),  — REPLACES 0.0.439's INDEX_BASE-only append (R1-MEMDST.md
         * Q6): INDEX_BASE alone names a BASE, not a read - the actual range an indexed draw touches depends on the
         * draw's own offset/count and the bound INDEX_TYPE's element size, which is what DRAW_INDEX_2 and
         * DRAW_INDEX_OFFSET_2 (below, at the draw itself) resolve through `d_rs_range`. This branch only LATCHES the
         * base into the per-segment DPair, exactly as INDEX_BUFFER_SIZE and INDEX_TYPE do beside it - none of the
         * three alone is a read. `ib_base` is INDEX_BASE's own packing: ADDR_LO bit 0 is reserved (masked), ADDR_HI
         * is 16 bits (Mesa's own INDEX_BASE encoding, radv_cmd_buffer.c:6259). */
        else if (op == OP_INDEX_BASE && ex && (ex->flags & XLAT12_EXTRA_READSET) && l >= 3u) {
            pr->ib_base = ((uint64_t)in[i + 1u] & ~(uint64_t)1u) | (((uint64_t)in[i + 2u] & 0xFFFFull) << 32);
        }
        /* D4-PRIME-FIXES.md item 4 (D4-4) — INDEX_BUFFER_SIZE's body: the bound index buffer's own COUNT (not
         * bytes), used to bounds-check DRAW_INDEX_OFFSET_2's offset + count below. */
        else if (op == OP_INDEX_BUFFER_SIZE && ex && (ex->flags & XLAT12_EXTRA_READSET) && l >= 2u) {
            pr->ib_size = in[i + 1u];
        }
        /* D4-PRIME-FIXES.md item 4 (D4-4) — THE DEDICATED INDEX_TYPE PACKET. F48 instead uses SET_UCONFIG_REG_INDEX
         * to dword offset 0x243 (0x20000243) for the same field - captured beside the generic register loop below,
         * where that packet is otherwise processed. `ib_type_seen` distinguishes "never written" from a
         * legitimately-zero (DX_INDEX_16) value; DPair's own zero-init cannot make that distinction by itself. */
        else if (op == OP_INDEX_TYPE && ex && (ex->flags & XLAT12_EXTRA_READSET) && l >= 2u) {
            pr->ib_type = in[i + 1u] & 3u; pr->ib_type_seen = 1u;
        }
        if (op == OP_CONTEXT_CONTROL) {
            if (ctxctl_is_proven(&in[i], l)) {
                d_close(e);
                if (e->cap - e->n < l) return XLAT12_IB_ERR_TOO_LONG;
                for (uint32_t k = 0; k < l; k++) out[e->n++] = in[i + k];
            } else if (l == 3u && in[i + 1] == 0x80000002u && in[i + 2] == 0x80000000u && i + 3u < n &&
                       ctxctl_is_proven(&in[i + 3], plen_at(in, i + 3u, n))) {
                ds->ctxctl_dropped++;                    /* overridden by the proven form in the very next packet */
            } else {
                /* 0.0.306: every OTHER form asks the CP to restore per-context state from the STATE SHADOW in
                 * memory - Apple's shadow, laid out for gfx10 - which would overwrite exactly the gfx12 state d_synth() and
                 * d_rings() have just synthesised. That is the 0x2803c hazard in another shape: a value that is harmless on
                 * its own part and destructive on ours. The five forms a 12-frame census of real SecurityAgent IBs found are
                 *   80000000/80000000 x50 (the proven one, copied above), 80000002/80000000 x23, 80000002/80000002 x6,
                 *   80010002/00000000 x1, 80000002/00000000 x1
                 * and for every one of them we emit the PROVEN form instead: load nothing, shadow nothing, deterministically.
                 * SUSPECTED that this is CORRECT and not merely safe - it changes what the CP restores across a context
                 * switch - and only a hardware run with a translated frame settles it. */
                if (l != 3u) return XLAT12_ERR_MEMLOADED;
                d_close(e);
                if (e->cap - e->n < 3u) return XLAT12_IB_ERR_TOO_LONG;
                out[e->n++] = XLAT12_IB_CTXCTL_HDR;
                out[e->n++] = XLAT12_IB_CTXCTL_DW1;
                out[e->n++] = XLAT12_IB_CTXCTL_DW2;
                ds->ctxctl_neutralised++;
                /* build 0.0.449 item 3 (F4, MIB-A1-PATH.md B2) — a non-proven CONTEXT_CONTROL (this branch:
                 * every form the proven rung above and the "overridden by the very next packet" rung did not
                 * already account for) asks the CP to restore per-context state from Apple's gfx10 shadow, which
                 * this translator does not track - any PS/VS value carried up to this point can no longer be
                 * trusted as "what Apple's stream last wrote". */
                if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT)) d_ud_carry_clear(ex->ud_carry);
                /* build 0.0.455 item 2 (F4, 0.0.454 review): the SAME restore invalidates switch 48's
                 * reusable shadow (DTable.sh) exactly as it invalidates the carry - the shadow's redirect is only
                 * as trustworthy as the SGPR state it was placed against, and a non-proven CONTEXT_CONTROL may
                 * have overwritten that state from Apple's untracked gfx10 shadow. `tb` is unconditionally
                 * non-NULL (the caller always passes `&tbl`, zeroed whether or not XLAT12_EXTRA_TABLE_DESC is
                 * set), so this is a plain, always-safe write; nothing reads `sh.valid` unless TABLE_DESC is also
                 * on, and nothing sets XLAT12_EXTRA_TABLE_REUSE without it either. */
                if (tb) tb->sh.valid = 0u;
            }
            i += l; continue;
        }
        /* 0.0.306: LOAD_CONTEXT_REG is DROPPED when every register it names is absent on gfx12, and refused
         * otherwise, naming the register. In 26 packets over 12 real SecurityAgent IBs every one loaded only
         * CB_COLOR0/1_CLEAR_WORD0/1 (or nothing at all) - registers gfx12 does not have, because it does fast clears
         * differently - so the whole packet is state the target part cannot hold and nothing has to be read out of the
         * client's memory. A load that named a register gfx12 DOES have would need its memory read and re-emitted as SET
         * packets; that is not built, and this refuses rather than guessing. Body: addr_lo, addr_hi, then (dword offset,
         * count) pairs against the context base 0xa000. */
        if (op == OP_LOAD_CONTEXT_REG) {
            if (l < 4u || ((l - 3u) & 1u) != 0u) { ds->err_op = op; return XLAT12_ERR_MEMLOADED; }
            for (uint32_t k = i + 3u; k + 1u < i + l; k += 2u) {
                const uint32_t off = in[k] & 0xFFFFu, cnt2 = in[k + 1u];
                if (cnt2 > 0x4000u) { ds->err_op = op; return XLAT12_ERR_MEMLOADED; }
                for (uint32_t j = 0; j < cnt2; j++) {
                    const uint32_t g10 = (0xa000u + off + j) << 2;
                    uint32_t t12 = 0, cls = XLAT12_CLS_UNKNOWN;
                    if (!xlat12_lookup(g10, &t12, &cls) || cls != XLAT12_CLS_ABSENT) {
                        ds->err_reg = g10; ds->err_op = op; return XLAT12_ERR_MEMLOADED;
                    }
                }
            }
            ds->memloaded_dropped++;
            i += l; continue;
        }
        if (is_memloaded(op)) return XLAT12_ERR_MEMLOADED;
        if (op == OP_DMA_DATA) {
            if (l == 7u && in[i + 1] == 0x60200001u && in[i + 4] == 0u && in[i + 5] == 0u) {
                ds->prefetch_nopped++; i += l; continue;   /* prefetch_parser, dst_nowhere: reads, writes nothing */
            }
            return XLAT12_IB_ERR_UNLISTED;
        }
        if (op == OP_WRITE_DATA || op == OP_WAIT_REG_MEM || op == OP_COPY_DATA) {
            if (!operand_ok(&in[i], l)) return XLAT12_IB_ERR_REG_OPERAND;
            /* build 0.0.449 item 2 (F2, review of 0.0.448) — THE REGISTER-WRITE HOLE, AS A REFUSAL. Not
             * gated: runs whatever the switches say, like every other refusal-direction rung in this function.
             * 0.0.448 CLEARED the seen bit instead of refusing (which could turn a 0.0.447 REDIRECTED refusal
             * into a silent admission with different bytes - the review's own finding) and its own decoding was
             * off by one; REPLACED here.
             *
             * WRITE_DATA (op 0x37): a register-destination form (DST_SEL [11:8] == 0, CONFIRMED field position -
             * gfx_dep.h's own citation, matching operand_ok above) is header, control, address, address-hi, THEN
             * data (CONFIRMED against Apple's own writeWriteData1RegCmdPacket disassembly, display_pipe_guard.h:
             * dw0 header, dw1 control, dw2 register dword offset, dw3 0 [address-hi, unused for a register
             * destination but still PRESENT], dw4 value) - so ndata = l - 4, NOT l - 3 as 0.0.448 had it.
             * WR_ONE_ADDR (bit 16 of the control dword, the SAME disassembly) writes the SAME register `ndata`
             * times; without it, p[2]..p[2]+ndata-1 are consecutive registers (auto-increment) - checked by
             * RANGE OVERLAP (d_ud_range_hits), not a per-dword loop, so there is NO CAP (0.0.448's 64-dword cap
             * could miss a longer write that ran into the range from below).
             *
             * COPY_DATA (op 0x40): a register-destination form (DST_SEL [11:8] == 0) writes ONE register at
             * p[4], or TWO (p[4], p[4]+1) when COUNT_SEL (bit 16, the standard PM4 field position) selects a
             * 64-bit copy - SUSPECTED, not independently confirmed against a real Apple COPY_DATA build site the
             * way WRITE_DATA's WR_ONE_ADDR is; harmless either way, since refusing on it only ever WIDENS a
             * refusal, never narrows one.
             *
             * WAIT_REG_MEM (op 0x3C), CORRECTED (build 0.0.451 item 4, S2, review of 0.0.450): operand_ok's
             * own "wr_wait_wr_reg" form - OPERATION bits [7:6] of the control dword == 1, register space (MEM_SPACE
             * bits [5:4] != 1). CONFIRMED against the vendored Linux source (re/linux-amdgpu/gfx_v12_0.c):
             * gfx_v12_0_ring_emit_reg_write_reg_wait(ring, reg0, reg1, ref, mask) at line 4739 calls
             * gfx_v12_0_wait_reg_mem(ring, usepfp, mem_space=0, opt=1, addr0=reg0, addr1=reg1, ref, mask, 0x20) at
             * line 4745, and gfx_v12_0_wait_reg_mem (line 430) writes addr0 to dword2 (our p[2]) and addr1 to
             * dword3 (our p[3]) via PACKET3(WAIT_REG_MEM, 5) - the function's OWN NAME says which role each plays:
             * reg0/p[2] is WRITTEN (with `ref`, once the wait condition on reg1/p[3] is met), and reg1/p[3] is the
             * one WAITED ON (polled, never written). The PREVIOUS comment here had this backwards (named p[3] as
             * the write target) and the dedicated check below matched that error, checking only p[3] via
             * xlat12_ud_range_hits and never p[2] with it - operand_ok's OWN p[2] check (`reg_identical`, a
             * single-register IDENTICAL-class lookup, not the range check) already happened to refuse a PS/VS p[2]
             * before this code is ever reached, so the practical gap was narrow, but the dedicated RANGE check
             * belongs on the register this packet ACTUALLY writes, not the one it merely polls. Checked below:
             * both p[2] and p[3], regardless of which the hardware treats as which - the brief's own instruction. */
            if (op == OP_WRITE_DATA && ((in[i + 1] >> 8) & 0xFu) == 0u) {
                const uint32_t oneAddr = (in[i + 1] >> 16) & 1u;
                const uint32_t ndata = l - 4u;   /* operand_ok already proved l >= 4 for WRITE_DATA; may be 0 */
                if (ndata) {
                    const uint32_t first = in[i + 2u];
                    const uint32_t last = oneAddr ? first : first + ndata - 1u;
                    if (xlat12_ud_range_hits(first, last)) return XLAT12_IB_ERR_REG_OPERAND;
                }
            } else if (op == OP_COPY_DATA && ((in[i + 1] >> 8) & 0xFu) == 0u) {
                const uint32_t wide = (in[i + 1] >> 16) & 1u;
                const uint32_t first = in[i + 4u];
                if (xlat12_ud_range_hits(first, wide ? first + 1u : first)) return XLAT12_IB_ERR_REG_OPERAND;
            } else if (op == OP_WAIT_REG_MEM && ((in[i + 1] >> 4) & 3u) != 1u && ((in[i + 1] >> 6) & 3u) == 1u) {
                // p[2] (reg0) is the register this operation WRITES; p[3] (reg1) is the one it polls - both checked
                // (see the banner above): a hit on EITHER refuses.
                if (xlat12_ud_range_hits(in[i + 2u], in[i + 2u]) || xlat12_ud_range_hits(in[i + 3u], in[i + 3u]))
                    return XLAT12_IB_ERR_REG_OPERAND;
            }
            /* build 0.0.487 (P2): a register write into the compute block is state this translation did not prove. */
            if (ex && (ex->flags & XLAT12_EXTRA_CS_ELIDE)) d_cs_forget(pr, &in[i], l);
            d_close(e);
            if (e->cap - e->n < l) return XLAT12_IB_ERR_TOO_LONG;
            for (uint32_t k = 0; k < l; k++) out[e->n++] = in[i + k];
            i += l; continue;
        }
        if (op == OP_SET_CONTEXT_REG || op == OP_SET_SH_REG || op == OP_SET_UCONFIG_REG) {
            const uint32_t base = d_base(op), off = in[i + 1] & 0xFFFFu, flags = PF(h);
            int anyStages = 0;
            d_close(e);
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (base + off + k) << 2;
                uint32_t g12 = 0, v12 = 0; int emit = 0, st = 0;
                if ((s = d_pair_note(pf, ex, ds, pr, g10, in[i + 2u + k])) != 0u) return s;
                /* build 0.0.448 item 1 (MIB-A1-PATH.md Q1 B2): recorded UNCONDITIONALLY, "whether or not it
                 * is emitted" - unlike d_ud_note/d_vsud_note below, which only run inside `if (emit)`. build
                 * 0.0.449 item 1 (F1): the carry is caller-owned (`ex->ud_carry`); gated on the flag so a caller
                 * without UD_REEMIT set never has this struct dereferenced, whatever `ex->ud_carry` holds. */
                if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT)) d_ud_carry_note(ex->ud_carry, g10, in[i + 2u + k]);
                /* build 0.0.487 (P2): the compute state THIS translation writes, by the raw input value. */
                if (op == OP_SET_SH_REG && ex && (ex->flags & XLAT12_EXTRA_CS_ELIDE)) d_cs_note(pr, g10, in[i + 2u + k]);
                /* build 0.0.535 item 4: the draw's own CB0 and scissors, for the T# observer's caller (read-only). */
                if (op == OP_SET_CONTEXT_REG && ex && ex->tex_state) d_ts_note(ex->tex_state, g10, in[i + 2u + k]);
                /* D4-PRIME-FIXES.md item 4 (D4-4),  — INDEX_TYPE VIA SET_UCONFIG_REG. F48 sets
                 * VGT_INDEX_TYPE this way (0x20000243 through the _INDEX form below, not this plain form; kept here
                 * too for a stream that sets it without an index register). A READ of what the stream declares, not
                 * a translation - captured from the RAW input dword regardless of whether the register emits. */
                if (op == OP_SET_UCONFIG_REG && ex && (ex->flags & XLAT12_EXTRA_READSET) && off + k == 0x243u) {
                    pr->ib_type = in[i + 2u + k] & 3u; pr->ib_type_seen = 1u;
                }
                /* build 0.0.536 (XLAT12_EXTRA_RECT2D): VGT_PRIMITIVE_TYPE (dword 0x242 of the uconfig base), recorded always */
                if (op == OP_SET_UCONFIG_REG && off + k == 0x242u)
                    pr->r2d = (pr->r2d & ~0x13Fu) | (in[i + 2u + k] & 0x3Fu) | XLAT12_R2D_PRIM_SEEN;
                if ((s = d_reg(d_pf(pf, pr), ex, ds, g10, in[i + 2u + k], &g12, &v12, &emit, &st)) != 0u) return s;
                /* build 0.0.536 (XLAT12_EXTRA_RECT2D): the VGT_GS_OUT_PRIM_TYPE value this translation emits, recorded always */
                if (emit && g12 == 0x30998u) pr->r2d = (pr->r2d & 0xFFFFu) | ((v12 & 0xFFu) << 16) | XLAT12_R2D_GSO_SEEN;
                /* build 0.0.453 item 1 (Fix E): RSRC2_PS is recorded UNCONDITIONALLY, like d_pair_note above -
                 * d_reg always emits this register (case 0xb02cu), but the note must not depend on that staying true. */
                if (g10 == 0xb02cu) { pr->ps_usg = ds->ps_user_sgpr; pr->ps_usg_seen = 1u; }
                if (emit) {
                    /* build 0.0.480: inside a unit, no later packet may rewrite what the extra block wrote */
                    if (U && d_unit_blockreg(U, out, g12)) { ds->err_reg = g10; ds->err_op = XLAT12_UNIT_BLOCK_REWRITE; return XLAT12_IB_ERR_DRAW_SHAPE; }
                    /* build 0.0.502: DB_SPI_VRS_CENTER_LOCATION = 0 as the first value of the packet that carries the stream's
                     * own region-0 DB_SHADER_CONTROL (the two are contiguous on gfx12: 0x28068, 0x2806c) - 1 dword, not a 3-dword
                     * packet in the extra block. d_vrs_prefollow says when; d_rings covers the region 0 in which it never fires. */
                    if (d_vrs_prefollow(ex, pr, g12)) {
                        if ((s = d_add(e, XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION, flags, 0u)) != 0u) { ds->err_reg = g10; return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s; }
                        pr->vrs_state = XLAT12_VRS_PREFOLLOW;
                    }
                    if ((s = d_add(e, g12, flags, v12)) != 0u) { ds->err_reg = g10; return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s; }
                    ds->regs_emitted++;
                    if (op == OP_SET_SH_REG && g12 == g10) d_ud_note(pr, g10, e->n - 1u);   /*: where this value landed */
                    if (op == OP_SET_SH_REG) d_vsud_note(pr, g10, e->n - 1u);   /* cond 4: the VS half, by gfx10 address */
                    d_ptr_note(pr, g12, e->n - 1u);   /* */
                    if (U && d_unit_hs_note(U, g12, e->n - 1u)) { ds->err_reg = g10; ds->err_op = XLAT12_UNIT_HS_OVER; return XLAT12_IB_ERR_DRAW_SHAPE; }
                    uint32_t f12 = 0, fv = 0;   /* 0.0.220: the raster/CB delta's follow-on write, in the same packet */
                    if (ex && (ex->flags & XLAT12_EXTRA_RASTER) && d_follow(g10, in[i + 2u + k], &f12, &fv)) {
                        if ((s = d_add(e, f12, flags, fv)) != 0u) { ds->err_reg = g10; return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s; }
                        ds->raster_follow++;
                        if (g10 == 0x28c6cu) ds->raster_follow_view++; else ds->raster_follow_attrib++;   /* */
                    }
                    /* 0.0.389: PA_SC_HISZ_CONTROL beside PA_SC_HIZ_INFO. No flag: see d_hisz_follow. */
                    if (d_hisz_follow(g12, &f12, &fv)) {
                        if ((s = d_add(e, f12, flags, fv)) != 0u) { ds->err_reg = g10; return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s; }
                        ds->hisz_control++;
                    }
                }
                anyStages |= st;
            }
            d_close(e);
            /* 0.0.409: with XLAT12_EXTRA_PAIR_PRE the pair is already resolved by d_pair_pre(), so the
             * synthesised NGG state is built from the bound program; OFF (and any caller without the flag) passes `pf`
             * exactly as 0.0.408 did, byte for byte. */
            {
                const xlat12_draw_profile *spf = (ex && (ex->flags & XLAT12_EXTRA_PAIR_PRE)) ? d_pf(pf, pr) : pf;
                if (anyStages && (s = d_synth(spf, ex, e, ds)) != 0u) return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s;
            }
            i += l; continue;
        }
        if (op == OP_SET_SH_REG_INDEX || op == OP_SET_UCONFIG_REG_INDEX) {
            const uint32_t base = d_base(op), off = in[i + 1] & 0xFFFFu, idx = in[i + 1] >> 28, flags = PF(h);
            if (in[i + 1] & 0x0FFF0000u) return XLAT12_ERR_BAD_INDEX;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (base + off + k) << 2;
                uint32_t g12 = 0, v12 = 0; int emit = 0, st = 0;
                if ((s = d_pair_note(pf, ex, ds, pr, g10, in[i + 2u + k])) != 0u) return s;
                /* build 0.0.448 item 1 (MIB-A1-PATH.md Q1 B2): the SET_SH_REG_INDEX / SET_UCONFIG_REG_INDEX
                 * form gets the SAME unconditional carry as the plain form above, gated the same way (item 1,
                 * build 0.0.449 F1). */
                if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT)) d_ud_carry_note(ex->ud_carry, g10, in[i + 2u + k]);
                /* build 0.0.487 (P2): the SAME note for the _INDEX form. */
                if (op == OP_SET_SH_REG_INDEX && ex && (ex->flags & XLAT12_EXTRA_CS_ELIDE)) d_cs_note(pr, g10, in[i + 2u + k]);
                /* D4-PRIME-FIXES.md item 4 (D4-4),  — INDEX_TYPE VIA SET_UCONFIG_REG_INDEX: F48's OWN
                 * form (0x20000243: op 0x7A, dword offset 0x243, index 2). Captured from the RAW input dword. */
                if (op == OP_SET_UCONFIG_REG_INDEX && ex && (ex->flags & XLAT12_EXTRA_READSET) && off + k == 0x243u) {
                    pr->ib_type = in[i + 2u + k] & 3u; pr->ib_type_seen = 1u;
                }
                /* build 0.0.536 (XLAT12_EXTRA_RECT2D): VGT_PRIMITIVE_TYPE through the _INDEX form too, recorded always */
                if (op == OP_SET_UCONFIG_REG_INDEX && off + k == 0x242u)
                    pr->r2d = (pr->r2d & ~0x13Fu) | (in[i + 2u + k] & 0x3Fu) | XLAT12_R2D_PRIM_SEEN;
                if ((s = d_reg(d_pf(pf, pr), ex, ds, g10, in[i + 2u + k], &g12, &v12, &emit, &st)) != 0u) return s;
                if (st) return XLAT12_IB_ERR_DRAW_SHAPE;          /* VGT_SHADER_STAGES_EN is never an index write */
                /* build 0.0.453 item 1 (Fix E): the SAME unconditional RSRC2_PS note as the plain form above. */
                if (g10 == 0xb02cu) { pr->ps_usg = ds->ps_user_sgpr; pr->ps_usg_seen = 1u; }
                if (emit) {
                    if (U && d_unit_blockreg(U, out, g12)) { ds->err_reg = g10; ds->err_op = XLAT12_UNIT_BLOCK_REWRITE; return XLAT12_IB_ERR_DRAW_SHAPE; }   /* build 0.0.480 */
                    if ((s = d_add_index(e, op, idx, g12, flags, v12)) != 0u) { ds->err_reg = g10; return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s; }
                    ds->regs_emitted++;
                    if (op == OP_SET_SH_REG_INDEX && g12 == g10) d_ud_note(pr, g10, e->n - 1u);   /* */
                    if (op == OP_SET_SH_REG_INDEX) d_vsud_note(pr, g10, e->n - 1u);   /* cond 4 */
                    d_ptr_note(pr, g12, e->n - 1u);   /* */
                    if (U && d_unit_hs_note(U, g12, e->n - 1u)) { ds->err_reg = g10; ds->err_op = XLAT12_UNIT_HS_OVER; return XLAT12_IB_ERR_DRAW_SHAPE; }   /* build 0.0.480 */
                }
            }
            i += l; continue;
        }
        if (is_passthrough(op) && !is_draw(op) && op != OP_DISPATCH_DIRECT) {
            d_close(e);
            /* build 0.0.537 (xlat12_ib.h XLAT12_EXTRA_PWS, switch 93): Apple's CB/DB barrier becomes our RELEASE_MEM(PWS) +
             * ACQUIRE_MEM(PWS) in place, only in a region that ends at a draw (to < n) with 16 dwords left; else the verbatim copy
             * below, counted. Every other ACQUIRE_MEM shape is counted and copied as today. OFF: one test. */
            if (op == OP_ACQUIRE_MEM && pr->pws && d_pws(ds, pr, e, &in[i], l, to < n ? to - i : 0u)) {
                /* build 0.0.539 (XLAT12_PWS_SLOT_*): only after the conversion, only a slot inside this region - marked here,
                 * dropped by the NOP branch 10 dwords on (after this barrier's EVENT_WRITE 0xE, copied as always) */
                if (xlat12_ib_pws_slot_at(&in[i], to - i)) pr->pws_slot = i + 10u;
                i += l; continue;
            }
            if (e->cap - e->n < l) return XLAT12_IB_ERR_TOO_LONG;
            for (uint32_t k = 0; k < l; k++) out[e->n++] = in[i + k];
            i += l; continue;
        }
        return XLAT12_IB_ERR_UNLISTED;                         /* COND_EXEC, nested IB, dispatches, anything else */
    }
    pr->pws_slot = 0u;   /* build 0.0.539: a slot is marked and dropped inside one region, never carried to the next */
    return 0;
}

/* build 0.0.536 (xlat12_ib.h XLAT12_EXTRA_RECT2D, switch 92) — ONE DRAW's VGT_GS_OUT_PRIM_TYPE, decided at the draw: RECT_2D
 * before a draw whose VGT_PRIMITIVE_TYPE in force (written in this translation) is RECTLIST, unless RECT_2D is already in force; the
 * profile's own value before any other draw that RECT_2D (this translation's own write) is in force for. A non-RECTLIST draw with
 * no VGT_GS_OUT_PRIM_TYPE of this translation in force is left alone, as today: in run11y and run11v every non-RECTLIST draw after
 * a RECTLIST draw follows an Apple 0x28a6c write in the same IB (translated to the profile's value), and the 60 IBs whose first
 * draw has no 0x28a6c before it all open with a RECTLIST draw. One 3-dword SET_UCONFIG_REG packet at the region's end (the caller
 * calls this last in the region, before the pad). No room: XLAT12_IB_ERR_TOO_LONG / XLAT12_R2D_NO_ROOM, and the public entry
 * falls back to today's output. */
static D_NOINLINE uint32_t d_r2d_draw(const xlat12_draw_profile *cp, DEmit *e, DPair *pr, xlat12_draw_stats *ds, uint32_t draw_at)
{
    const uint32_t rectNow = ((pr->r2d & XLAT12_R2D_PRIM_SEEN) && (pr->r2d & 0x3Fu) == XLAT12_R2D_PRIM_RECTLIST) ? 1u : 0u;
    const uint32_t rect = rectNow;   /* the draw at `draw_at` - never the previous one (XLAT12_R2D_LAST_RECT) */
    pr->r2d = (pr->r2d & ~XLAT12_R2D_LAST_RECT) | (rectNow ? XLAT12_R2D_LAST_RECT : 0u);
    const uint32_t want = rect ? XLAT12_R2D_OUTPRIM : cp->gs_out_prim_type;
    const uint32_t inForce = (pr->r2d & XLAT12_R2D_GSO_SEEN) ? ((pr->r2d >> 16) & 0xFFu) : 0xFFFFFFFFu;
    if (rect && ds->r2d_rect < 0xFFu) ds->r2d_rect++;
    if (rect ? inForce == XLAT12_R2D_OUTPRIM : inForce != XLAT12_R2D_OUTPRIM) return 0u;   /* nothing to change for THIS draw */
    d_close(e);
    const uint32_t s = d_add(e, 0x30998u, 0u, want);   /* VGT_GS_OUT_PRIM_TYPE (gfx12 uconfig): C0017900 00000266 value */
    d_close(e);
    if (s) {
        ds->r2d_noroom = 1u; ds->err_op = XLAT12_R2D_NO_ROOM; ds->err_reg = 0x30998u; ds->err_in_dword = draw_at;
        return s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s;
    }
    pr->r2d = (pr->r2d & 0xFFFFu) | ((want & 0xFFu) << 16) | XLAT12_R2D_GSO_SEEN;
    if (rect) { if (ds->r2d_written < 0xFFu) ds->r2d_written++; }
    else if (ds->r2d_restored < 0xFFu) ds->r2d_restored++;
    return 0u;
}

/* build 0.0.548 item B: the same walk, also answering WHERE it refused (*at: the output dword of the refusing packet - an
 * unwalkable one, a bad VGT_GS_OUT_PRIM_TYPE write, or the draw under RECT_2D - or n for the end rule; 0 when accepted). */
uint32_t xlat12_ib_rect2d_check_at(const uint32_t *out, uint32_t n, uint32_t keep, uint32_t *at)
{
    uint32_t i = 0, prim = 0, primSeen = 0, gso = 0, gsoSeen = 0;
    if (at) *at = 0u;
    if (!out) return 1u;
    while (i < n) {
        const uint32_t h = out[i], l = plen_at(out, i, n);
        if (!l) { if (at) *at = i; return 1u; }
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        const uint32_t op = PO(h);
        if ((op == OP_SET_UCONFIG_REG || op == OP_SET_UCONFIG_REG_INDEX) && l >= 3u) {
            const uint32_t off = out[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t a = (0xc000u + off + k) << 2, v = out[i + 2u + k];
                if (a == 0x30908u) { prim = v & 0x3Fu; primSeen = 1u; }
                if (a == 0x30998u) {
                    if (v != keep && v != XLAT12_R2D_OUTPRIM) { if (at) *at = i; return 1u; }
                    gso = v; gsoSeen = 1u;
                }
            }
        } else if (is_draw(op) && gsoSeen && gso == XLAT12_R2D_OUTPRIM && !(primSeen && prim == XLAT12_R2D_PRIM_RECTLIST)) {
            if (at) *at = i;
            return 1u;   /* RECT_2D in force at a draw that is not a RECTLIST this output wrote */
        }
        i += l;
    }
    /* fix round item 2: RECT_2D still in force at the END of the output would reach whatever the GPU runs next (another
     * segment, IB or frame this translation never saw): refused, so no output can leak it by construction. */
    if (gsoSeen && gso == XLAT12_R2D_OUTPRIM) { if (at) *at = n; return 1u; }
    return 0u;
}

uint32_t xlat12_ib_rect2d_check(const uint32_t *out, uint32_t n, uint32_t keep)
{
    return xlat12_ib_rect2d_check_at(out, n, keep, 0);
}

/* build 0.0.547 item 3 (xlat12_ib.h): the draws of out[0..n) that execute with a RECTLIST VGT_PRIMITIVE_TYPE in force, by the
 * backstop's own walk and reading of 0x30908. Read-only. */
uint32_t xlat12_ib_rectlist_draws(const uint32_t *out, uint32_t n, uint32_t *last_is_rect)
{
    uint32_t i = 0, prim = 0, primSeen = 0, cnt = 0, last = 0;
    if (last_is_rect) *last_is_rect = 0u;
    if (!out) return XLAT12_RL_UNWALKED;
    while (i < n) {
        const uint32_t h = out[i], l = plen_at(out, i, n);
        if (!l) return XLAT12_RL_UNWALKED;
        if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) { i += l; continue; }
        const uint32_t op = PO(h);
        if ((op == OP_SET_UCONFIG_REG || op == OP_SET_UCONFIG_REG_INDEX) && l >= 3u) {
            const uint32_t off = out[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++)
                if (((0xc000u + off + k) << 2) == 0x30908u) { prim = out[i + 2u + k] & 0x3Fu; primSeen = 1u; }
        } else if (is_draw(op)) {
            last = (primSeen && prim == XLAT12_R2D_PRIM_RECTLIST) ? 1u : 0u;
            if (last) cnt++;
        }
        i += l;
    }
    if (last_is_rect) *last_is_rect = last;
    return cnt;
}

/* build 0.0.536 fix round (review MUST-FIX, the stack): XLAT12_EXTRA_RECT2D's FALLBACK IS A SECOND PASS OF THIS ONE FUNCTION,
 * never a second frame. The body below is 0.0.535's, inside one block, with every `return v;` spelled D_R2D_RET(v): it stores v and
 * leaves the block for d_r2d_done. There, only when the pass was flagged (r2dCtl bit 0), refused, and had written a RECT_2D / TRISTRIP
 * packet or found no room for one, the unit's placements are undone and the block runs again from d_r2d_pass with bit 0 clear - today's
 * translation, byte for byte - and the stats keep the flagged pass's r2d_rect / r2d_noroom with r2d_fallback 1. Every piece of the
 * retry state is in two locals of THIS frame (no static: two translations on two threads never share it). OFF, bit 0 is clear from the
 * start, the block runs once, and D_R2D_RET returns exactly what `return` returned. */
#define D_R2D_RET(v) do { r2dSt = (v); goto d_r2d_done; } while (0)
uint32_t xlat12_ib_translate_draw_ex(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, const uint32_t *in,
                                     uint32_t n, uint32_t *out, uint32_t *out_len, xlat12_draw_stats *ds)
{
    /* r2dCtl: bit 0 this pass is flagged; after a fallback, bit 1 and the flagged pass's status [15:8], r2d_rect [23:16], r2d_noroom [31:24] */
    uint32_t r2dCtl = (ex && (ex->flags & XLAT12_EXTRA_RECT2D)) ? 1u : 0u, r2dSt = 0u;
    /* build 0.0.537 (XLAT12_EXTRA_PWS): pwsCtl bit 0 this pass converts; after its fallback, bit 1 and that pass's status [15:8] */
    uint32_t pwsCtl = (ex && (ex->flags & XLAT12_EXTRA_PWS)) ? 1u : 0u;
    /* build 0.0.547 item 3: the flagged pass's refusal (err_op [31:24] | err_reg [23:0], and err_in_dword), kept across the
     * fallback's zeroing of `ds` for r2d_on_op / r2d_on_reg / r2d_on_dw. Written only on the RECT_2D fallback branch below. */
    uint32_t r2dOnA = 0u, r2dOnB = 0u;
d_r2d_pass:
  {
    /* D6 (D4-PRIME-FIXES.md item 6,  (B)) — THE STACK. `xlat12_draw_stats` is large enough that the
     * fallback local this function carried through 0.0.440 (for a caller that passes ds == NULL) added its own
     * frame to EVERY caller's stack sum, whether or not any real caller ever used it - and a grep of every caller in
     * the kext, tools/ and the host tests (0.0.441) found NONE that pass NULL: every one already owns its own
     * xlat12_draw_stats and takes its address. Refusing instead is therefore free (no real caller regresses) and
     * removes the dead local from this function's own frame. */
    if (!ds) D_R2D_RET(XLAT12_ERR_ARG);
    for (uint32_t k = 0; k < sizeof *ds / sizeof(uint32_t); k++) ((uint32_t *)ds)[k] = 0;
    ds->err_op = 0xFFFFFFFFu;
    if (out_len) *out_len = 0;
    if (!pf || !in || !out || n == 0u) D_R2D_RET(XLAT12_ERR_ARG);
    if (ex && ex->ring_va && ((ex->ring_va & (XLAT12_GE_RING_ALIGN - 1u)) ||
                              ex->ring_va > (1ull << 48) - (uint64_t)XLAT12_GE_RING_TOTAL))
        D_R2D_RET(XLAT12_IB_ERR_RING);
    if (ex && ex->gs_sgpr0_va && (!ex->ring_va || (ex->gs_sgpr0_va & 0xFFFull) || ex->gs_sgpr0_va >= (1ull << 48)))
        D_R2D_RET(XLAT12_IB_ERR_RING);   /* 0.0.226: a page-aligned table VA below 2^48, only with a ring base */
    if (ex && ex->vs_pgm_va && ((ex->vs_pgm_va & (XLAT12_RELOC_ALIGN - 1u)) || ex->vs_pgm_va >= (1ull << 48)))
        D_R2D_RET(XLAT12_IB_ERR_RING);   /* 0.0.265: PGM_LO is VA >> 8, so 256-byte alignment is structural, not a preference */
    if (ex && (ex->flags & ~(XLAT12_EXTRA_PREAMBLE | XLAT12_EXTRA_RASTER | XLAT12_EXTRA_APPLE_IDXFMT | XLAT12_EXTRA_APPLE_OUTPRIM |
                             XLAT12_EXTRA_SYNTH_IDXPRIM | XLAT12_EXTRA_RASTER_PER_DRAW | XLAT12_EXTRA_INLINE_DESC |
                             XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV | XLAT12_EXTRA_FILL_COLOR |
                             XLAT12_EXTRA_PAIR_PRE | XLAT12_EXTRA_READSET | XLAT12_EXTRA_UD_REEMIT |
                             XLAT12_EXTRA_DESC_INV_APPLE_HEAD | XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_VS_KNOWN |
                             XLAT12_EXTRA_UNIT | XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_DCC_STRIP | XLAT12_EXTRA_DRAW_ELIDE |
                             XLAT12_EXTRA_NCLEAR | XLAT12_EXTRA_RECT2D | XLAT12_EXTRA_PWS |
                             XLAT12_EXTRA_TBLCACHE)))   /* build 0.0.540 item 5 */
        D_R2D_RET(XLAT12_ERR_ARG);   /* 0.0.219: 0x2 retired; 0.0.224: 0x8 / 0x10 ablation;: 0x20;: 0x40;: 0x80;: 0x400;: 0x800; D4-PRIME.md item 3: 0x1000; build 0.0.448 item 1: 0x2000; build 0.0.453 item 5: 0x4000; build 0.0.454 item 1: 0x8000; build 0.0.455 item 1: 0x10000; build 0.0.480: 0x20000; build 0.0.487: 0x40000; build 0.0.488: 0x80000 (merge 0.0.489: both kept); build 0.0.500: 0x100000 */
    /* build 0.0.500: the draw elide acts only at the table step's refusal, and only for an enabled row class. */
    if (ex && (ex->flags & XLAT12_EXTRA_DRAW_ELIDE) &&
        (!(ex->flags & XLAT12_EXTRA_TABLE_DESC) || !ex->draw_elide_rows ||
         (ex->draw_elide_rows & ~(uint32_t)(XLAT12_DE_CLASS_UY | XLAT12_DE_CLASS_AO | XLAT12_DE_CLASS_GLASS |
                                            XLAT12_DE_CLASS_AN | XLAT12_DE_CLASS_AN_SHADOW)) ||   /* build 0.0.552 */
         ((ex->draw_elide_rows & XLAT12_DE_CLASS_AN) && (ex->draw_elide_rows & XLAT12_DE_CLASS_AN_SHADOW))))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.488: the DCC strip acts only inside the table step; without the step it would strip nothing. */
    if (ex && (ex->flags & XLAT12_EXTRA_DCC_STRIP) && !(ex->flags & XLAT12_EXTRA_TABLE_DESC))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.487 (P1): the elision has nothing to prove the program with without the caller's answer - refused rather
     * than silently eliding nothing (the same "requires its own mechanism" rule as the checks below). */
    if (ex && (ex->flags & XLAT12_EXTRA_CS_ELIDE) && !ex->cs_is_n) D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.536 (XLAT12_EXTRA_RECT2D): one register, one policy - never with the two that write 0x30998 otherwise. */
    if (ex && (ex->flags & XLAT12_EXTRA_RECT2D) && (ex->flags & (XLAT12_EXTRA_APPLE_OUTPRIM | XLAT12_EXTRA_SYNTH_IDXPRIM)))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.535: the fill replaces only an elision; without 57's flag it would replace nothing. */
    if (ex && (ex->flags & XLAT12_EXTRA_NCLEAR) && !(ex->flags & XLAT12_EXTRA_CS_ELIDE)) D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.535 item 4: the observer's state starts empty with every translation (nothing inherited). */
    if (ex && ex->tex_state) {
        xlat12_tex_state *const ts = ex->tex_state;
        ts->seen = 0u; ts->cb0 = ts->cb0_ext = ts->win_tl = ts->win_br = ts->gen_tl = ts->gen_br = ts->vp_tl = ts->vp_br = 0u;
    }
    /* build 0.0.480: the unit path keeps ALL its state in the caller's `ex->unit`; without it, or with a constituent
     * table this stream cannot hold, it is refused rather than run on nothing. */
    if (ex && (ex->flags & XLAT12_EXTRA_UNIT) && !d_unit_args_ok(ex->unit, n)) D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.454 item 1: shadow reuse and the 16-byte placement are refinements of the table step itself. */
    if (ex && (ex->flags & XLAT12_EXTRA_TABLE_REUSE) && !(ex->flags & XLAT12_EXTRA_TABLE_DESC))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.453 item 5: the head-ACQUIRE_MEM check is a refinement of XLAT12_EXTRA_DESC_INV itself - it
     * would guard nothing without the invalidate it might skip. */
    if (ex && (ex->flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD) && !(ex->flags & XLAT12_EXTRA_DESC_INV))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* 0.0.398: the fill-colour retarget writes the pair EXACTLY as the pair is written (a plain 64-bit VA
     * whose low dword is PS_2 and high dword PS_3); a VA that cannot be one - zero, unaligned to the float4, or at or
     * above 2^48 - is a caller error and refuses before anything is translated, like every other handed-in address. */
    if (ex && (ex->flags & XLAT12_EXTRA_FILL_COLOR) &&
        (!ex->fill_color_va || (ex->fill_color_va & 0xFull) || ex->fill_color_va >= (1ull << 48)))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* M4-DESC-KEXT-PORT: the invalidate exists for the table step's records; without the step it would guard nothing */
    if (ex && (ex->flags & XLAT12_EXTRA_DESC_INV) && !(ex->flags & XLAT12_EXTRA_TABLE_DESC))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.448 item 1: the re-emission has nothing to re-emit INTO without the table step. */
    if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT) && !(ex->flags & XLAT12_EXTRA_TABLE_DESC))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.449 item 1 (F1): the carry is CALLER-OWNED now (xlat12_ud_carry's own comment) - the
     * translator keeps no state of its own to fall back on, so a caller that sets the flag but hands in no
     * storage is refused rather than silently doing nothing (the flag would then be a no-op the caller could
     * mistake for "re-emission is running"). */
    if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT) && !ex->ud_carry)
        D_R2D_RET(XLAT12_ERR_ARG);
    /* build 0.0.455 item 1: the known-slot rule has nothing to consult without the carry XLAT12_EXTRA_UD_REEMIT
     * populates - refused rather than silently doing nothing, the same "requires its own mechanism" rule above. */
    if (ex && (ex->flags & XLAT12_EXTRA_VS_KNOWN) && !(ex->flags & XLAT12_EXTRA_UD_REEMIT))
        D_R2D_RET(XLAT12_ERR_ARG);
    /* M4-DESC-TABLE-IMPL: the table step places records at ib_va-relative addresses and snapshots through desc_read */
    if (ex && (ex->flags & XLAT12_EXTRA_TABLE_DESC) && (!ex->desc_read || !ex->ib_va || (ex->ib_va & 3ull) || ex->ib_va >= (1ull << 48)))
        D_R2D_RET(XLAT12_ERR_ARG);
    /*: the per-draw follow-on rule is a rule ABOUT the raster delta; without the delta it would check nothing */
    if (ex && (ex->flags & XLAT12_EXTRA_RASTER_PER_DRAW) && !(ex->flags & XLAT12_EXTRA_RASTER))
        D_R2D_RET(XLAT12_ERR_ARG);
    /*: the synthesis writes the profile's words where Apple wrote nothing; the ablation keeps Apple's words where it did.
     * Together they would give one stream two policies for the same register, so the combination is refused, not resolved. */
    if (ex && (ex->flags & XLAT12_EXTRA_SYNTH_IDXPRIM) && (ex->flags & (XLAT12_EXTRA_APPLE_IDXFMT | XLAT12_EXTRA_APPLE_OUTPRIM)))
        D_R2D_RET(XLAT12_ERR_ARG);

    /* 0.0.307: ONE OR MORE draws. Until now this was "exactly one", which is the shape of the tri suite's
     * frame and of nothing a real client submits: framecheck over a captured SecurityAgent IB reports 11 draws in two of
     * Apple's encoder segments, 9 in the first. Each draw keeps its original dword index; the packets BETWEEN two draws are
     * translated as a region and NOP-padded back out to the next draw's index, exactly as the packets before the first draw
     * always were. With one draw the output is byte-identical to 0.0.306's (the golden r44 vectors prove it). */
    uint32_t i = 0, draws = 0;
    uint32_t da[XLAT12_MAX_DRAWS];
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) { ds->err_in_dword = i; ds->err_op = in[i]; D_R2D_RET(PT(in[i]) == 3u ? XLAT12_ERR_TRUNCATED : XLAT12_ERR_BAD_PACKET); }
        if (in[i] != XLAT12_IB_NOP && PT(in[i]) == 3u && is_draw(PO(in[i]))) {
            if (draws < XLAT12_MAX_DRAWS) da[draws] = i;
            draws++;
        }
        i += l;
    }
    if (draws == 0u || draws > XLAT12_MAX_DRAWS) { ds->err_op = draws; D_R2D_RET(XLAT12_IB_ERR_DRAW_SHAPE); }
    ds->draws = draws;
    ds->draw_in_at = da[0];

    uint32_t cursor = 0;
    xlat12_unit *const U = d_unit_of(ex);   /* build 0.0.480: NULL unless XLAT12_EXTRA_UNIT (all its state is the caller's) */
    if (U) d_unit_reset(U);
    DPair pair;                       /* one per translation, carried across the regions */
    for (uint32_t z = 0; z < sizeof pair / sizeof(uint32_t); z++) ((uint32_t *)&pair)[z] = 0u;
    pair.pws = (pwsCtl & 1u) | (pwsCtl ? 2u : 0u);   /* build 0.0.537 (XLAT12_EXTRA_PWS): 0 OFF; 2 the fallback pass; 3 converting */
    /* build 0.0.449 item 1 (F1): `*ex->ud_carry` is CALLER-OWNED memory (never a file-scope static - the
     * reentrancy race the review found), so it does not get zeroed by DPair's own bulk zero above - zeroed here
     * instead, unconditionally when in use, on the SAME "once per translation" scope DPair's ud_seen/ud_pos
     * already are. This is what STILL makes "no cross-call carry" hold with caller-owned storage: whatever the
     * caller's struct held before this call is gone before any packet is read, so two SEQUENTIAL calls sharing
     * the SAME `ex->ud_carry` (e.g. the kext's one static, reused call after call under its own lock) never see
     * a stale value either - only two callers with SEPARATE storage can now run concurrently without racing. */
    if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT))
        for (uint32_t z = 0; z < sizeof *ex->ud_carry / sizeof(uint32_t); z++) ((uint32_t *)ex->ud_carry)[z] = 0u;
    DTable tbl;                       /* M4-DESC-TABLE-IMPL: read and written only under XLAT12_EXTRA_TABLE_DESC */
    for (uint32_t z = 0; z < sizeof tbl / sizeof(uint32_t); z++) ((uint32_t *)&tbl)[z] = 0u;
    /* build 0.0.453 item 5: computed ONCE per translation, over `in`/`n` exactly as this call received them -
     * `in[0]` is this SEGMENT's own first dword (the caller always calls this function once per segment), never a
     * draw-local offset. Computed unconditionally (cheap: reads at most 8 dwords) so a caller that turns the flag
     * on LATER in the SAME boot never has to wonder whether it was evaluated; read only under the flag. */
    if (ex && (ex->flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD))
        tbl.head_inv_ok = (uint32_t)xlat12_ib_head_acquire_mem_covers(in, n, XLAT12_DESC_INV_GCR);
    const int tdesc = ex && (ex->flags & XLAT12_EXTRA_TABLE_DESC);
    for (uint32_t dk = 0; dk < draws; dk++) {
        DEmit e;
        e.out = out; e.cap = da[dk]; e.n = cursor; e.open = 0; e.op = 0; e.flags = 0; e.hdr = 0; e.next_off = 0; e.count = 0;
        /* build 0.0.480 (P2 COMPACTION): a unit's region is emitted from the OUTPUT cursor, and may use every dword up
         * to the unit's end; `oreg` is where this region starts IN THE OUTPUT - every "written in this region" test
         * below compares output positions against it. In place (no unit) it is `cursor`, exactly as before. */
        if (U) { e.cap = n; e.n = U->ocur; }
        const uint32_t oreg = U ? U->ocur : cursor;
        /* 0.0.409: with the flag, resolve THIS region's program pair before d_region() reaches the
         * VGT_SHADER_STAGES_EN whose d_synth() is the pair-dependent state. OFF, this line does not run. */
        if (ex && (ex->flags & XLAT12_EXTRA_PAIR_PRE)) d_pair_pre(pf, ex, ds, in, n, cursor, da[dk], &pair);
        const uint32_t rs = d_region(pf, ex, ds, in, n, out, cursor, da[dk], &e, &pair, &tbl);
        if (rs != 0u) D_R2D_RET(rs);
        d_close(&e);
        /* the extra block goes in the FIRST region only, last before its pad, exactly as it always did */
        if (dk == 0u && ex && (ex->ring_va || ex->rsrc3_gs || ex->flags)) {   /* 0.0.217-220 */
            if (U) U->blk_from = e.n;   /* build 0.0.480: the block's output range, for the block guard */
            uint32_t s = d_rings(ex, &e, ds, pair.ptr_pos, pair.ptr_seen, pair.vrs_state == XLAT12_VRS_PREFOLLOW ? 1u : 0u);
            if (s != 0u) { ds->err_op = 0xFFu; D_R2D_RET(s == XLAT12_ERR_CAPACITY ? XLAT12_IB_ERR_TOO_LONG : s); }
            if (U) { U->blk_to = e.n; U->rings_done = 1u; }
        }
        if (dk == 0u) pair.vrs_state = XLAT12_VRS_CLOSED;   /* build 0.0.502: region 0 is over; no later region writes 0x28068 */
        /* build 0.0.448 item 1, build 0.0.449 item 3 (F4: the FULL B3 set) (MIB-A1-PATH.md Q1 B2/B3,
         * XLAT12_EXTRA_UD_REEMIT) — THE SLOT_SHARED RE-EMISSION, after d_rings and before the pad and the
         * REDIRECTED rung (the design doc's own placement), so the re-emitted dwords land in real output space,
         * not the NOP pad the table step reuses below. For a table-bound draw (`rcp->ps_table_abi1 != 0`), walk
         * B3's FULL set: d_table_desc's own `sl[]` (table pointer pair, texture index(es), the direct sampler
         * index or the class-11 pointer pair), PLUS this program's declared PS ABI pointer pairs
         * (xlat12_abi_ptrs.h, the SAME row d_table_desc's own ABI-pointer loop uses) and its VS ABI pointer
         * pairs. A slot appearing in more than one list (e.g. AI's class-11 pointer at s4 is also its own row's
         * declared pointer 0) is simply re-emitted once and found already `>= cursor` the second time - harmless.
         * For any slot this translation SAW (`ud_seen`/`vsud_seen`) but at a position an EARLIER region wrote
         * (`ud_pos`/`vsud_pos < cursor`, today's XLAT12_TDESC_SLOT_SHARED or an inherited ABI pointer) AND for
         * which a carried input value exists (`ex->ud_carry`), re-emit it here through d_reg/d_add exactly as a
         * live SET_SH_REG would be (VS re-homes exactly as Apple's own write would, through the SAME d_reg path),
         * then d_ud_note/d_vsud_note it so this region owns its own write. A slot this region ALREADY wrote is
         * untouched (it is never `< cursor`) - it always wins. A slot with NO carry (never written anywhere in
         * this translation) is left for the SLOT_UNSEEN rung / `in_ptr_inherit` below to refuse, unchanged: this
         * is not a cross-segment carry. */
        if (ex && (ex->flags & XLAT12_EXTRA_UD_REEMIT) && tdesc) {
            const xlat12_draw_profile *rcp = d_pf(pf, &pair);
            xlat12_ud_carry *carry = ex->ud_carry;
            if (rcp->ps_table_abi1 && rcp->ps_table_abi1 <= D_TBL_ABIS) {
                const DTableAbi *a = &kDTableAbi[rcp->ps_table_abi1 - 1u];
                const int class11 = d_tbl_c11(a);
                /* build 0.0.470 (the stack): sl[] holds ONLY the row's own slots (at most 6); the ABI-pointer pairs
                 * are walked straight out of their xlat12_abi_ptrs.h row below as list positions nsl.., in the SAME order
                 * the old sl[] tail held them - so the list and every `si` index are what 0.0.455 built, without the
                 * 2 * XLAT12_ABI_PTR_MAX-dword tail (which grew with the cap) on translate_draw_ex's frame. */
                uint32_t sl[6u], nsl = 0;
                sl[nsl++] = a->table; sl[nsl++] = a->table + 1u;
                /* build 0.0.470: d_table_desc's OWN slot list per shape - a class-10 row's textbl pair in place of
                 * texture slots (its tex[] is 0xff), and NO sampler slot for a no-sampler row (the design's "skip the
                 * sampler slot at re-emission"; through 0.0.455 a 0xff would only have been skipped by the range check). */
                if (d_tbl_c10(a)) { sl[nsl++] = a->textbl1 - 1u; sl[nsl++] = a->textbl1; }
                else for (uint32_t ti = 0; ti < a->ntex && ti < 2u; ti++) sl[nsl++] = a->tex[ti];
                if (class11) { sl[nsl++] = a->samptbl; sl[nsl++] = a->samptbl + 1u; }
                else if (!d_tbl_nosamp(a)) { sl[nsl++] = a->samp; }
                /* item 3 (F4): this program's declared PS ABI pointer pairs (d_table_desc's own lookup, reused).
                 * build 0.0.470: its count through xlat12_abi_ptr_n (a longer row is refused by d_table_desc's own
                 * in_over; here it only bounds the walk, which sl[]'s 2 * XLAT12_ABI_PTR_MAX tail always holds). */
                const xlat12_abi_ptrs *ap = xlat12_abi_ptr_row(a->ndw, a->fnv);
                uint32_t apOver = 0u;
                const uint32_t nall = nsl + 2u * xlat12_abi_ptr_n(ap, &apOver);
                /* build 0.0.454 item 1 (switch 48): a draw that will keep the translation's reusable shadow
                 * does not re-emit a slot the shadow still holds for its own row (re-emitting Apple's value there
                 * would discard the shadow), nor a declared ABI-pointer slot an earlier region of this translation
                 * wrote, that holds no redirect of ours and that the carry still holds unchanged (the SGPR already
                 * carries it; d_table_desc exports it as known). 0 without the flag: nothing below changes. The
                 * decision is taken HERE, before any re-emission, and d_table_desc/the REDIRECTED rung reach the
                 * same answer after it: re-emission only ever writes slots this test left SLOT_SHARED, which
                 * d_sh_reuse_ok never counts on. */
                const int sreuse = d_sh_reuse_ok(ex, a, &pair, &tbl, oreg);
                const uint32_t nrow = 3u + (a->ntex < 2u ? a->ntex : 2u);   /* table x2, tex x ntex, samp (direct rows only reuse) */
                for (uint32_t si = 0; si < nall; si++) {
                    const uint32_t slot = si < nsl ? sl[si] : (uint32_t)ap->slot[(si - nsl) / 2u] + ((si - nsl) & 1u);
                    if (slot >= D_PS_UDN) continue;
                    if (sreuse && d_sh_row_holds(a, &tbl, &pair, slot)) continue;
                    if (sreuse && si >= nrow && d_sh_abi_known(ex, &tbl, &pair, out, slot)) continue;
                    if (!((pair.ud_seen >> slot) & 1u) || pair.ud_pos[slot] >= oreg) continue;   /* not SLOT_SHARED */
                    if (!((carry->ps_ok >> slot) & 1u)) continue;                                   /* no carry: SLOT_UNSEEN stays SLOT_UNSEEN */
                    const uint32_t g10 = D_PS_UD0 + 4u * slot;
                    uint32_t g12 = 0, v12 = 0; int emit = 0, st = 0; uint32_t rst;
                    if ((rst = d_reg(rcp, ex, ds, g10, carry->ps_val[slot], &g12, &v12, &emit, &st)) != 0u) D_R2D_RET(rst);
                    if (emit) {
                        if ((rst = d_add(&e, g12, 0u, v12)) != 0u) {
                            /* build 0.0.453 item 4 (inv-f84/REPORT.txt): name THIS draw, not d_region's last
                             * packet (err_op/err_in_dword are otherwise whatever d_region's own walk left behind). */
                            if (rst == XLAT12_ERR_CAPACITY) { ds->err_op = XLAT12_REEMIT_NO_ROOM; ds->err_in_dword = da[dk]; D_R2D_RET(XLAT12_IB_ERR_TOO_LONG); }
                            D_R2D_RET(rst);
                        }
                        ds->ud_reemit_n++;
                        if (g12 == g10) d_ud_note(&pair, g10, e.n - 1u);
                    }
                }
                /* item 3 (F4): this program's declared VS ABI pointer pairs (d_table_desc's own lookup, reused;
                 * `vp->stage == 1u` the same guard d_table_desc uses). VS re-homes onto USER_DATA_GS_n exactly as
                 * a live Apple write would - the SAME d_reg/d_add path, keyed by the gfx10 address (D_VS_UD0-
                 * relative), so `g12` differs from `g10` here and d_vsud_note (not d_ud_note) records the
                 * OUTPUT position, matching d_region's own SET_SH_REG handling for the VS stage. */
                { const xlat12_abi_ptrs *vp = rcp->vs_abi_ptr1 && rcp->vs_abi_ptr1 <= XLAT12_ABI_PTR_ROWS
                                              ? &kXlat12AbiPtrs[rcp->vs_abi_ptr1 - 1u] : 0;
                  if (vp && vp->stage == 1u) {
                      /* build 0.0.470: the count through xlat12_abi_ptr_n, and (the stack) the pairs walked straight
                       * out of the row - position si is slot[si / 2] + (si & 1), exactly the old vsl[] order. */
                      uint32_t vpOver = 0u;
                      const uint32_t nvsl = 2u * xlat12_abi_ptr_n(vp, &vpOver);
                      for (uint32_t si = 0; si < nvsl; si++) {
                          const uint32_t slot = (uint32_t)vp->slot[si / 2u] + (si & 1u);
                          if (slot >= D_VS_UDN) continue;
                          if (!((pair.vsud_seen >> slot) & 1u) || pair.vsud_pos[slot] >= oreg) continue;
                          if (!((carry->vs_ok >> slot) & 1u)) continue;
                          const uint32_t g10 = D_VS_UD0 + 4u * slot;
                          uint32_t g12 = 0, v12 = 0; int emit = 0, st = 0; uint32_t rst;
                          if ((rst = d_reg(rcp, ex, ds, g10, carry->vs_val[slot], &g12, &v12, &emit, &st)) != 0u) D_R2D_RET(rst);
                          if (emit) {
                              if ((rst = d_add(&e, g12, 0u, v12)) != 0u) {
                                  /* build 0.0.453 item 4: the SAME fix as the PS loop above. */
                                  if (rst == XLAT12_ERR_CAPACITY) { ds->err_op = XLAT12_REEMIT_NO_ROOM; ds->err_in_dword = da[dk]; D_R2D_RET(XLAT12_IB_ERR_TOO_LONG); }
                                  D_R2D_RET(rst);
                              }
                              ds->ud_reemit_n++;
                              d_vsud_note(&pair, g10, e.n - 1u);
                          }
                      }
                  } }
                d_close(&e);
            }
        }
        /* build 0.0.480 (P4 UN-REDIRECT, units only): a slot an earlier draw's table step redirected, that THIS draw's
         * program can read, gets Apple's carried value re-emitted here - so the REDIRECTED rung below finds it rewritten -
         * when the carry holds it; otherwise nothing is written and the rung refuses exactly as it always did. */
        if (U && (ex->flags & XLAT12_EXTRA_UD_REEMIT) && tdesc) {
            const uint32_t rs4 = d_unit_unredirect(pf, ex, ds, &e, &pair, &tbl, oreg, da[dk]);
            if (rs4) D_R2D_RET(rs4);
        }
        /* build 0.0.536 (XLAT12_EXTRA_RECT2D, switch 92): THIS draw's VGT_GS_OUT_PRIM_TYPE, decided HERE at the draw and
         * written LAST in its region - after the re-emission and the extra block, before the pad - so nothing of the region
         * comes between it and the draw. r2dCtl bit 0 clear (OFF, and the fallback pass): not called. */
        if (r2dCtl & 1u) { const uint32_t s2 = d_r2d_draw(d_pf(pf, &pair), &e, &pair, ds, da[dk]); if (s2) D_R2D_RET(s2); }
        if (dk == 0u) ds->draw_out_len = e.n;
        /* (xlat12_ib.h XLAT12_EXTRA_RASTER_PER_DRAW): THIS draw's colour-buffer words must already be established by this
         * translation - one VIEW2 and one FDCC_CONTROL follow-on made before it - or it would run with whatever an earlier
         * submission left in two registers the gfx10 stream cannot name. */
        if (ex && (ex->flags & XLAT12_EXTRA_RASTER_PER_DRAW) && (!ds->raster_follow_view || !ds->raster_follow_attrib)) {
            ds->err_op = 0xFDu;
            ds->err_reg = !ds->raster_follow_view ? 0x28c6cu : 0x28c74u;
            ds->err_in_dword = da[dk];
            D_R2D_RET(XLAT12_IB_ERR_DRAW_SHAPE);
        }
        if (!U) {   /* build 0.0.480: a unit has no per-draw pad - its draws follow its regions (P2) */
            ds->pad_dwords += da[dk] - e.n;
            for (uint32_t k = e.n; k < da[dk]; k++) out[k] = XLAT12_IB_NOP;
            if (tdesc && da[dk] > e.n && tbl.nrun < D_TBL_RUNS) { tbl.run_at[tbl.nrun] = e.n; tbl.run_len[tbl.nrun] = da[dk] - e.n; tbl.nrun++; }
        }
        /* 0.0.313: THE INTERPOLATION GUARD, applied to THIS draw with the state in force at THIS draw.
         * Refuses rather than renders: when the bound vertex program's image exports no parameters, the fragment stage
         * must consume none - NUM_INTERP [5:0] and PARAM_GEN [6] both zero - and the register must have been positively
         * written before the draw. A stream that never wrote it leaves a value inherited from some earlier submission
         * that we did not see, so "not established" is refused exactly as a non-zero value is. */
        if (d_pf(pf, &pair)->vs_drops_params) {
            if (!pair.ps_in_seen || (pair.ps_in_ctl & XLAT12_PS_IN_CONSUMES) != 0u) {
                ds->err_reg = XLAT12_REG_PS_IN_CONTROL;
                ds->err_op = pair.ps_in_seen ? pair.ps_in_ctl : 0xFFFFFFFFu;
                ds->err_in_dword = da[dk];
                D_R2D_RET(XLAT12_IB_ERR_INTERP);
            }
            ds->interp_checked++;
        }
        /* D4-PRIME-FIXES.md item 1 (D4-1),  — MIXED FRAMES, ONE UNION BUILT PER DRAW. Through 0.0.441
         * d_readset_accum ran for EVERY draw regardless of `ps_table_abi1`; now the two accumulation paths are
         * MUTUALLY EXCLUSIVE per draw: a draw with no bound table is accumulated here, BEFORE d_inline_desc converts
         * any inline record in place (d_readset_accum's own inline-image branch needs the raw gfx10 bytes); a draw
         * WITH a bound table is accumulated AFTER d_table_desc runs, below, from the table step's OWN export
         * (d_readset_from_table) - re-reading the raw slots here would both duplicate a stronger fact the table step
         * already proves and read the WRONG bytes once the table step rewrites them in place. If the table step
         * itself did not run for such a draw (XLAT12_EXTRA_TABLE_DESC off: the descriptor port), its export is
         * UNKNOWN, never "no pointers" - decline. region_from is `cursor`, the SAME bound the table step's
         * ABI-pointer loop uses for "inherited". Without the flag nothing here runs. */
        if (ex && (ex->flags & XLAT12_EXTRA_READSET)) {
            const xlat12_draw_profile *rcp = d_pf(pf, &pair);
            if (rcp->ps_table_abi1 == 0u) d_readset_accum(rcp, &pair, out, ds, oreg, ex);
            else if (!tdesc) ds->rs_declined = 1u;
        }
        /* D4-PRIME-FIXES.md item 4 (D4-4),  — THIS DRAW'S OWN INDEX RANGE. Read from the draw packet's
         * OWN dwords (`da[dk]` is this draw's header, still untouched here - the draw is copied verbatim below);
         * nothing here changes an output dword. `ib_type`/`ib_base`/`ib_size` are the per-segment DPair state
         * INDEX_BASE/INDEX_BUFFER_SIZE/INDEX_TYPE latched above. Size 0 -> 2 B (DX_INDEX_16), 1 -> 4 B
         * (DX_INDEX_32), 2 -> 1 B (DX_INDEX_8); 3 or never written declines (xlat12_ib.h's own convention: this is
         * not a value we may guess). */
        if (ex && (ex->flags & XLAT12_EXTRA_READSET)) {
            const uint32_t dop = PO(in[da[dk]]);
            const uint32_t dl = plen_at(in, da[dk], n);
            if (dop == OP_DRAW_INDEX_2 && dl >= 6u) {
                if (!pair.ib_type_seen || pair.ib_type >= 3u) { ds->rs_declined = 1u; }
                else {
                    const uint32_t sz = pair.ib_type == 0u ? 2u : pair.ib_type == 1u ? 4u : 1u;
                    const uint64_t addr = (uint64_t)in[da[dk] + 2u] | (((uint64_t)in[da[dk] + 3u] & 0xFFFFull) << 32);
                    const uint64_t count = (uint64_t)in[da[dk] + 4u];
                    d_rs_range(ds, addr, count * (uint64_t)sz);
                }
            } else if (dop == OP_DRAW_INDEX_OFFSET_2 && dl >= 4u) {
                if (!pair.ib_type_seen || pair.ib_type >= 3u || !pair.ib_base) { ds->rs_declined = 1u; }
                else {
                    const uint32_t sz = pair.ib_type == 0u ? 2u : pair.ib_type == 1u ? 4u : 1u;
                    const uint64_t offset = (uint64_t)in[da[dk] + 1u];
                    const uint64_t count = (uint64_t)in[da[dk] + 2u];
                    if (offset + count > (uint64_t)pair.ib_size) { ds->rs_declined = 1u; }
                    else d_rs_range(ds, pair.ib_base + offset * (uint64_t)sz, count * (uint64_t)sz);
                }
            }
        }
        /* (XLAT12_EXTRA_INLINE_DESC, default off): the fragment program in force at THIS draw samples through inline
         * records - convert them in place, or refuse. Without the flag nothing here runs and the output is unchanged. */
        if (ex && (ex->flags & XLAT12_EXTRA_INLINE_DESC)) {
            const xlat12_draw_profile *cp = d_pf(pf, &pair);
            if (cp->ps_inline_tex1 || cp->ps_inline_samp1) {
                const uint32_t si = d_inline_desc(cp, ex, &pair, out, ds, da[dk]);
                if (si) D_R2D_RET(si);
            }
        }
        /* M4-DESC-TABLE-IMPL (XLAT12_EXTRA_TABLE_DESC, default off): no draw may read a user-data write an earlier draw's table
         * step redirected; then, when the fragment program in force reads a class-19 table, gather, translate, place, redirect. */
        if (tdesc) {
            /* 0.0.390 ( part 5 (i)): CLEAR THE EXPORTED INPUT LIST FOR THIS DRAW BEFORE ANY RUNG OF THE TABLE
             * STEP, not only inside d_table_desc. The REDIRECTED rung below refuses WITHOUT entering that function, and
             * a caller that then read the list would read THE PREVIOUS DRAW'S INPUTS as this draw's - the exact shape of
             * the "stale reading" defects this project has already paid for. Nothing else in the translator reads them. */
            d_in_clear(ds);
            /* build 0.0.454 item 1 (switch 48): a draw that keeps the reusable shadow READS the shadow's
             * redirects of its OWN row's table / texture / sampler slots - that is the point - so exactly those
             * slots are exempt. Any other redirected slot the program can read still refuses. 0 without the flag. */
            const xlat12_draw_profile *rcp0 = d_pf(pf, &pair);
            const DTableAbi *ra = (rcp0->ps_table_abi1 && rcp0->ps_table_abi1 <= D_TBL_ABIS) ? &kDTableAbi[rcp0->ps_table_abi1 - 1u] : 0;
            const int rreuse = ra && d_sh_reuse_ok(ex, ra, &pair, &tbl, oreg);
            for (uint32_t k = 0; k < D_PS_UDN; k++) {
                if (rreuse && d_sh_row_holds(ra, &tbl, &pair, k)) continue;
                /* build 0.0.453 item 1 (Fix E, inv-f84/REPORT.txt): a redirected slot this draw's OWN program
                 * cannot read (k is at or past the RSRC2_PS USER_SGPR count IN FORCE, as this translation wrote it) is
                 * not a hazard - the program never loads it. Skip ONLY when RSRC2_PS was actually seen this translation;
                 * if it was never written, ps_usg/ps_usg_seen stay at their zero-init and the rung keeps refusing exactly
                 * as before (0.0.452's behaviour), because an unknown USER_SGPR count might still cover the slot. */
                if (pair.ps_usg_seen && k >= pair.ps_usg) continue;
                if (((tbl.patched >> k) & 1u) && ((pair.ud_seen >> k) & 1u) && pair.ud_pos[k] == tbl.ppos[k])
                    D_R2D_RET(d_tbl_fail(ds, da[dk], XLAT12_TDESC_REDIRECTED));
            }
            const xlat12_draw_profile *cp = d_pf(pf, &pair);
            if (cp->ps_table_abi1) {
                /* build 0.0.480 (C4): a unit's cache invalidate goes INLINE, before its first table draw */
                if (U) { const uint32_t si = d_unit_inv(ex, &e, &tbl, ds, da[dk]); if (si) D_R2D_RET(si); }
                if (ex->flags & XLAT12_EXTRA_DRAW_ELIDE) { ds->de_dcc0 = ds->dcc_unproven; ds->de_nl = 0u; }   /* 0.0.500; 0.0.512 */
                /* build 0.0.552 (XLAT12_DE_CLASS_AN, switch 110): AN's draw becomes a same-length NOP BEFORE its table step (the
                 * refusal-side elide's own state: nothing of its table exists). SHADOW and every other program: d_de_an answers 0. */
                if ((ex->flags & XLAT12_EXTRA_DRAW_ELIDE) && d_de_an(cp, ex, ds, in, n, da[dk])) {
                    const uint32_t la = plen_at(in, da[dk], n);
                    const uint32_t sa = d_draw_elide_emit(U, &e, out, da[dk], la, ds);
                    if (sa) D_R2D_RET(sa);
                    cursor = da[dk] + la;
                    continue;
                }
                const uint32_t st = d_table_desc(cp, ex, &pair, &tbl, out, ds, da[dk], oreg);
                if (st) {
                    /* build 0.0.500 (xlat12_ib.h XLAT12_EXTRA_DRAW_ELIDE): decided HERE, at the table step's return and
                     * nowhere else - a PROVENANCE refusal leaves this draw's table unplaced and unredirected. Admitted: the
                     * draw becomes a same-length NOP and the next region starts after it (no fill-colour step, no read-set
                     * export: the draw reads nothing). Otherwise the refusal stands, byte for byte. */
                    if (!((ex->flags & XLAT12_EXTRA_DRAW_ELIDE) && d_draw_elide(cp, ex, ds, in, n, da[dk], st))) D_R2D_RET(st);
                    const uint32_t le = plen_at(in, da[dk], n);
                    const uint32_t se = d_draw_elide_emit(U, &e, out, da[dk], le, ds);
                    if (se) D_R2D_RET(se);
                    cursor = da[dk] + le;
                    continue;
                }
                /* D4-PRIME-FIXES.md item 1 (D4-1) — this draw's table export is now ds->in_* (d_table_desc's own
                 * write); carry it into the accumulated read-set exactly as d_readset_accum does above. */
                if (ex && (ex->flags & XLAT12_EXTRA_READSET)) d_readset_from_table(ds);
            }
        }
        /* 0.0.398 (default off): retarget the fill's colour pointer, in place, at the ONE draw whose
         * in-force fragment identity is ws_B_ColorFill. Off, d_fill_color is not called and nothing here runs. */
        if (ex && (ex->flags & XLAT12_EXTRA_FILL_COLOR)) d_fill_color(d_pf(pf, &pair), ex, &pair, out, ds);
        {                               /* copy this draw at its own index and carry on with the next region */
            const uint32_t l = plen_at(in, da[dk], n);
            if (U) {                    /* build 0.0.480 (P2): a unit's draw follows its region */
                const uint32_t sd = d_unit_draw(U, &e, in, da[dk], l, ds);
                if (sd) D_R2D_RET(sd);
            } else for (uint32_t k = 0; k < l; k++) out[da[dk] + k] = in[da[dk] + k];
            cursor = da[dk] + l;
        }
    }
    if (ex && (ex->flags & XLAT12_EXTRA_RASTER) && !(ex->flags & XLAT12_EXTRA_RASTER_PER_DRAW) && ds->raster_follow != 2u) {
        ds->err_op = 0xFEu; D_R2D_RET(XLAT12_IB_ERR_DRAW_SHAPE);   /* the per-translation rule, unchanged without's flag */
    }
    /* 0.0.309: THE REGION AFTER THE LAST DRAW is translated like every other region, not copied verbatim
     * under a restricted op list. That list - NOP, draw, EVENT_WRITE, ACQUIRE_MEM, RELEASE_MEM, NUM_INSTANCES - was the
     * shape of the tri suite's tail; a real client segment carries REGISTER WRITES after its last draw and was refused
     * IB_ERR_DRAW_SHAPE for it (measured: segment 0 of a captured SecurityAgent frame, op 0x69 at dword 2607, six dwords
     * past the last draw). Translating it is byte-identical wherever the tail really is only those ops, because every one
     * of them is pass-through: r44's golden vectors are unchanged. */
    {
        DEmit e;
        e.out = out; e.cap = n; e.n = cursor; e.open = 0; e.op = 0; e.flags = 0; e.hdr = 0; e.next_off = 0; e.count = 0;
        /* build 0.0.480 (P2): a unit's tail region starts at its output cursor - or at its own INPUT dword when the
         * stream is not ahead of it, so the final trailer (and the fence828 packet in it) keeps its offset */
        if (U) e.n = d_unit_tail(U, out, cursor);
        const uint32_t rs = d_region(pf, ex, ds, in, n, out, cursor, n, &e, &pair, &tbl);
        if (rs != 0u) D_R2D_RET(rs);
        d_close(&e);
        ds->pad_dwords += n - e.n;
        ds->tail_pad_dwords = n - e.n;   /*: the tail run alone - the only pad an end-of-pipe packet may use */
        for (uint32_t k = e.n; k < n; k++) out[k] = XLAT12_IB_NOP;
        /* build 0.0.480 (P3 + the HS pairs + the constituent table): place every deferred record, patch the VAs */
        if (U) { const uint32_t sf = d_unit_finish(ex, U, out, n, e.n, cursor, ds); if (sf) D_R2D_RET(sf); }
    }
    i = n;
    ds->err_op = 0xFFFFFFFFu; ds->err_in_dword = 0;
    uint32_t badA = 0, badOp = 0;
    if (d_verify(out, n, (ex && (ex->flags & XLAT12_EXTRA_NCLEAR)) ? 1u : 0u, (ex && (ex->flags & XLAT12_EXTRA_TBLCACHE)) ? 1u : 0u,
                 &badA, &badOp) != 0u) {
        ds->err_reg = badA; ds->err_op = badOp;
        if (U && U->pool) (void)xlat12_pool_undo(U->pool);   /* build 0.0.480: a refused unit leaves no pool record behind */
        if (U && U->spill) (void)xlat12_pool_undo(U->spill);   /* build 0.0.522: nor a spill record */
        D_R2D_RET(XLAT12_IB_ERR_VERIFY);
    }
    /* build 0.0.500: the elide's backstop - the draws left in the output are exactly the draws not elided. */
    if (ex && (ex->flags & XLAT12_EXTRA_DRAW_ELIDE) && xlat12_ib_de_backstop(out, n, ds)) {
        if (U && U->pool) (void)xlat12_pool_undo(U->pool);
        if (U && U->spill) (void)xlat12_pool_undo(U->spill);   /* build 0.0.522 */
        D_R2D_RET(XLAT12_IB_ERR_VERIFY);
    }
    /* build 0.0.536 (XLAT12_EXTRA_RECT2D): the translation's own backstop over the whole output - RECT_2D only for RECTLIST. */
    uint32_t r2dAt = 0u;   /* build 0.0.548 item B: where the backstop refused (the OUTPUT dword; n = the end rule) */
    if ((r2dCtl & 1u) && xlat12_ib_rect2d_check_at(out, n, pf->gs_out_prim_type, &r2dAt)) {
        ds->err_op = XLAT12_R2D_BACKSTOP; ds->err_reg = 0x30998u; ds->err_in_dword = r2dAt;
        if (U && U->pool) (void)xlat12_pool_undo(U->pool);
        if (U && U->spill) (void)xlat12_pool_undo(U->spill);
        D_R2D_RET(XLAT12_IB_ERR_VERIFY);
    }
    if (out_len) *out_len = n;
    D_R2D_RET(0);
  }
d_r2d_done:
    /* build 0.0.537 (XLAT12_EXTRA_PWS): a flagged pass refused after it converted runs again without the conversion (the same
     * block, pwsCtl bit 0 clear): today's output where the barriers are concerned, every barrier counted no-room. First, so a
     * RECT_2D fallback below keeps its own rule. */
    if ((pwsCtl & 1u) && r2dSt && ds && ds->pws_conv) {
        xlat12_unit *const Up = d_unit_of(ex);
        if (Up && Up->pool) (void)xlat12_pool_undo(Up->pool);
        if (Up && Up->spill) (void)xlat12_pool_undo(Up->spill);
        pwsCtl = 2u | ((r2dSt < 0xFFu ? r2dSt : 0xFFu) << 8);
        goto d_r2d_pass;
    }
    if ((pwsCtl & 2u) && ds) { ds->pws_fallback = 1u; ds->pws_on_st = (uint8_t)(pwsCtl >> 8); }
    if ((r2dCtl & 1u) && r2dSt && ds && (ds->r2d_written | ds->r2d_restored | ds->r2d_noroom)) {
        xlat12_unit *const Uf = d_unit_of(ex);
        if (Uf && Uf->pool) (void)xlat12_pool_undo(Uf->pool);
        if (Uf && Uf->spill) (void)xlat12_pool_undo(Uf->spill);
        r2dCtl = 2u | ((r2dSt < 0xFFu ? r2dSt : 0xFFu) << 8) | ((uint32_t)ds->r2d_rect << 16) | ((uint32_t)ds->r2d_noroom << 24);
        r2dOnA = ((ds->err_op & 0xFFu) << 24) | (ds->err_reg & 0xFFFFFFu); r2dOnB = ds->err_in_dword;   /* build 0.0.547 */
        goto d_r2d_pass;   /* the fallback: the same block, bit 0 clear - today's translation (never a second frame) */
    }
    if ((r2dCtl & 2u) && ds) {
        ds->r2d_rect = (uint8_t)(r2dCtl >> 16); ds->r2d_noroom = (uint8_t)(r2dCtl >> 24); ds->r2d_fallback = 1u;
        ds->r2d_on_st = (uint8_t)(r2dCtl >> 8);
        ds->r2d_on_op = r2dOnA >> 24; ds->r2d_on_reg = r2dOnA & 0xFFFFFFu; ds->r2d_on_dw = r2dOnB;   /* build 0.0.547 */
    }
    return r2dSt;
}
#undef D_R2D_RET

/* 0.0.224: Apple's encoder segments (xlat12_ib.h). A start is the EVENT_WRITE 0x16 head + ACQUIRE_MEM with EVENT_WRITE 0xE
 * eight dwords after the ACQUIRE_MEM; the per-draw trailer has the same head + ACQUIRE_MEM but a NOP there. */
static int seg_start_at(const uint32_t *in, uint32_t i, uint32_t n)
{
    return (uint64_t)i + 12u <= (uint64_t)n && in[i] == XLAT12_SEG_HEAD0 && in[i + 1u] == XLAT12_SEG_HEAD1 &&
           in[i + 2u] == XLAT12_SEG_ACQUIRE && in[i + 10u] == XLAT12_SEG_HEAD0 && in[i + 11u] == XLAT12_SEG_EVENT_E;
}

uint32_t xlat12_ib_segments(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total)
{
    uint32_t i = 0, ns = 0;
    if (total) *total = 0;
    if (!in || !seg || !seg_start_at(in, 0u, n)) return 0;
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) break;
        if (seg_start_at(in, i, n)) {
            if (ns > 0u && ns <= max) seg[ns - 1u].end = i;
            if (ns < max) { seg[ns].head = i; seg[ns].start = i + 2u; seg[ns].end = 0; seg[ns].draws = 0; seg[ns].draw_at = 0; }
            ns++;
        } else if (in[i] != XLAT12_IB_NOP && PT(in[i]) == 3u && is_draw(PO(in[i])) && ns > 0u && ns <= max) {
            seg[ns - 1u].draws++;
            seg[ns - 1u].draw_at = i;
        }
        i += l;
    }
    if (ns > 0u && ns <= max) seg[ns - 1u].end = i;
    if (total) *total = ns;
    return ns < max ? ns : max;
}

/* 0.0.354 : the search xlat12_ib_segments does NOT do. Contract and rationale in xlat12_ib.h. This is
 * deliberately a copy of the walk above rather than a flag on it: the caller of xlat12_ib_segments must keep getting 0
 * for a stream whose head is not first, because everything downstream of it assumes segment 0 starts the stream. */
uint32_t xlat12_ib_seg_probe(const uint32_t *in, uint32_t n, uint32_t *first_at,
                             uint32_t *first_raw, uint32_t *walked)
{
    uint32_t i = 0, ns = 0, k;
    if (first_at)  *first_at  = XLAT12_SEG_NONE;
    if (first_raw) *first_raw = XLAT12_SEG_NONE;
    if (walked)    *walked    = 0;
    if (!in) return 0;
    while (i < n) {
        const uint32_t l = plen_at(in, i, n);
        if (!l) break;
        if (seg_start_at(in, i, n)) {
            if (ns == 0u && first_at) *first_at = i;
            ns++;
        }
        i += l;
    }
    if (walked) *walked = i;
    if (first_raw)
        for (k = 0; k < n; k++)
            if (seg_start_at(in, k, n)) { *first_raw = k; break; }
    return ns;
}

uint32_t xlat12_ib_shader_fnv(const uint32_t *d, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; d && k < n; k++) { h ^= d[k]; h *= 16777619u; }
    return h;
}
uint32_t xlat12_shader_id_count(void) { return XLAT12_SHADER_ID_COUNT; }
/* build 0.0.504: for the kext's once-per-boot idcap line (xlat12_readset.h is private to this file). */
uint32_t xlat12_readset_count(void) { return XLAT12_READSET_COUNT; }
uint32_t xlat12_readset_ptr_max(void) { return XLAT12_READSET_PTR_MAX; }
uint32_t xlat12_shader_id_max_ndw(void) { return XLAT12_SHADER_ID_MAX_NDW; }

/* 0.0.307: which identities a caller that can read only `cap` dwords is unable to match. Before this, the one
 * caller compared the table's GLOBAL maximum against its buffer and, if the maximum was larger, stopped matching
 * EVERYTHING - silently, with no error, so the suite would simply report every program as "NOT ours". Adding one long
 * program to the table would have done that to the programs that already worked. The cap is now per entry and the caller
 * names what it cannot reach. Returns the count of over-cap entries; for i < that count fills *name and *ndw. */
uint32_t xlat12_shader_id_over_cap(uint32_t cap, uint32_t i, const char **name, uint32_t *ndw)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < XLAT12_SHADER_ID_COUNT; k++) {
        if (kXlat12ShaderIds[k].ndw <= cap) continue;
        if (n == i) {
            if (name) *name = kXlat12ShaderIds[k].name;
            if (ndw) *ndw = kXlat12ShaderIds[k].ndw;
        }
        n++;
    }
    return n;
}
const char *xlat12_shader_id_name(int id)
{
    return (id >= 0 && (uint32_t)id < XLAT12_SHADER_ID_COUNT) ? kXlat12ShaderIds[id].name : "none";
}
/* 0.0.434 (notes/design/PGMID-COPYGUARD.md Part 1): see xlat12_ib.h. */
int xlat12_shader_id_row(uint32_t i, uint32_t *stage, uint32_t *ndw, uint32_t head[4])
{
    if (i >= XLAT12_SHADER_ID_COUNT || !stage || !ndw || !head) return 0;
    const xlat12_shader_id *s = &kXlat12ShaderIds[i];
    *stage = s->stage;
    *ndw = s->ndw;
    head[0] = s->head[0]; head[1] = s->head[1]; head[2] = s->head[2]; head[3] = s->head[3];
    return 1;
}
int xlat12_shader_id_match(uint32_t stage, const uint32_t *code, uint32_t n)
{
    for (uint32_t k = 0; code && n >= 4u && k < XLAT12_SHADER_ID_COUNT; k++) {
        const xlat12_shader_id *s = &kXlat12ShaderIds[k];
        if (s->stage != stage || s->ndw > n || s->ndw < 4u) continue;
        if (code[0] != s->head[0] || code[1] != s->head[1] || code[2] != s->head[2] || code[3] != s->head[3]) continue;
        if (code[s->ndw - 1u] != 0xBFB00000u) continue;                       /* gfx12 s_endpgm */
        if (xlat12_ib_shader_fnv(code, s->ndw) == s->fnv) return (int)k;
    }
    return -1;
}
uint32_t xlat12_ib_ps_user_data(const uint32_t *in, uint32_t n, uint32_t *vals, uint32_t *mask)
{
    if (!in || !vals || !mask) return 0;
    for (uint32_t k = 0; k < XLAT12_PS_USER_DATA_SLOTS; k++) vals[k] = 0;
    *mask = 0;
    uint32_t seen = 0;
    for (uint32_t i = 0; i + 1u < n; ) {
        const uint32_t h = in[i];
        if ((h >> 30) != 3u) { i++; continue; }                      /* TYPE2 NOP (and anything else) is one dword */
        const uint32_t len = ((h >> 16) & 0x3FFFu) + 2u;
        if (i + len > n) break;
        const uint32_t op = (h >> 8) & 0xFFu;
        if ((op == OP_SET_SH_REG || op == OP_SET_SH_REG_INDEX) && !(PF(h) & 2u) && len >= 3u) {
            const uint32_t off = in[i + 1] & 0xFFFFu;               /* dwords from the 0x2c00 SH base */
            for (uint32_t q = 0; q + 2u < len; q++) {
                const uint32_t slot = off + q;
                if (slot >= XLAT12_PS_USER_DATA_OFF && slot < XLAT12_PS_USER_DATA_OFF + XLAT12_PS_USER_DATA_SLOTS) {
                    const uint32_t k = slot - XLAT12_PS_USER_DATA_OFF;
                    if (!((*mask >> k) & 1u)) seen++;
                    *mask |= 1u << k;
                    vals[k] = in[i + 2u + q];
                }
            }
        }
        i += len;
    }
    return seen;
}

/* (xlat12_ib.h): a lookup by the identity's own (stage, ndw, fnv); no row is UNPROVEN, and UNPROVEN is not free. */
uint32_t xlat12_shader_id_desc_class(int id)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return XLAT12_DESCCLS_UNPROVEN;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    for (uint32_t i = 0; i < XLAT12_SHADER_DESC_COUNT; i++)
        if (kXlat12ShaderDesc[i].stage == s->stage && kXlat12ShaderDesc[i].ndw == s->ndw && kXlat12ShaderDesc[i].fnv == s->fnv)
            return kXlat12ShaderDesc[i].cls;
    return XLAT12_DESCCLS_UNPROVEN;
}

int xlat12_shader_id_desc_inline(int id, uint32_t *tex, uint32_t *samp)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return 0;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    if (s->stage != 0u) return 0;
    for (uint32_t i = 0; i < XLAT12_SHADER_DESC_COUNT; i++)
        if (kXlat12ShaderDesc[i].stage == s->stage && kXlat12ShaderDesc[i].ndw == s->ndw && kXlat12ShaderDesc[i].fnv == s->fnv) {
            if (kXlat12ShaderDesc[i].cls != XLAT12_DESCCLS_IMAGE || kXlat12ShaderDesc[i].inl_tex == 0xffu) return 0;
            if (tex) *tex = kXlat12ShaderDesc[i].inl_tex;
            if (samp) *samp = kXlat12ShaderDesc[i].inl_samp;
            return 1;
        }
    return 0;
}

/*: a fragment profile's inline slots (+1 encoded, 0 = none) from its identity's annotation. */
static void d_inline_fill(int id, xlat12_draw_profile *out)
{
    uint32_t t = 0xffu, sm = 0xffu;
    out->ps_inline_tex1 = 0u; out->ps_inline_samp1 = 0u;
    if (xlat12_shader_id_desc_inline(id, &t, &sm)) {
        out->ps_inline_tex1 = t + 1u;
        out->ps_inline_samp1 = sm == 0xffu ? 0u : sm + 1u;
    }
}

/* M4-DESC-TABLE-IMPL: the table ABI of a fragment identity, by the identity's own (ndw, fnv), and only for a row the machine-code
 * annotation classes IMAGE (xlat12_shader_desc.h). 0 = none. */
static uint32_t d_table_abi_of(int id)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return 0u;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    if (s->stage != 0u || xlat12_shader_id_desc_class(id) != XLAT12_DESCCLS_IMAGE) return 0u;
    for (uint32_t i = 0; i < D_TBL_ABIS; i++)
        if (kDTableAbi[i].ndw == s->ndw && kDTableAbi[i].fnv == s->fnv) return i + 1u;
    return 0u;
}

/* D4-PRIME-FIXES.md item 3(b)/item 4, ; generalised build 0.0.445 from "P's index
 * alone" to the small kDTblAbiGated list — see xlat12_ib.h's own comment. Pure; no state, no switch read - the
 * switch itself lives in the kext, which is the only thing that may decide what OFF means for hardware output.
 * P's own answer is UNCHANGED (still 1 for index 3, checked first in the list); U and Y (indices 4 and 5) are
 * ADDITIVE. */
int xlat12_table_abi_is_gated(uint32_t abi1)
{
    for (uint32_t i = 0; i < D_TBL_ABI_GATED_N; i++) if (kDTblAbiGated[i] == abi1) return 1;
    return 0;
}

/* build 0.0.470 (NO-SAMPLER-CLASS10.md section 3) — see xlat12_ib.h. DERIVED from the row's own fields (the same
 * predicates d_table_desc branches on), never from a hand list, so a future no-sampler or class-10 row is gated by the
 * kext's switch 51 the moment it exists. Pure; 0 for abi1 0 or out of range. */
int xlat12_table_abi_new_shape(uint32_t abi1)
{
    if (!abi1 || abi1 > D_TBL_ABIS) return 0;
    const DTableAbi *a = &kDTableAbi[abi1 - 1u];
    return d_tbl_c10(a) || d_tbl_nosamp(a);
}

/* build 0.0.470 — see xlat12_ib.h: the row count, a row's own shape check and its placement `need`, for tests
 * (the SAME d_tbl_row_ok / d_tbl_need d_table_desc calls). */
uint32_t xlat12_table_abi_count(void) { return (uint32_t)D_TBL_ABIS; }
int xlat12_table_abi_row_ok(uint32_t abi1)
{
    if (!abi1 || abi1 > D_TBL_ABIS) return 0;
    return d_tbl_row_ok(&kDTableAbi[abi1 - 1u]);
}
uint32_t xlat12_table_abi_need(uint32_t abi1)
{
    if (!abi1 || abi1 > D_TBL_ABIS) return 0u;
    return d_tbl_need(&kDTableAbi[abi1 - 1u]);
}
/* build 0.0.484 — see xlat12_ib.h: the row d_table_abi_of would pick for an identity with this (ndw, fnv), found the
 * SAME way (first row whose ndw and fnv both match), without needing that identity to be in xlat12_shader_ids.h. */
uint32_t xlat12_table_abi_find(uint32_t ndw, uint32_t fnv)
{
    for (uint32_t i = 0; i < D_TBL_ABIS; i++)
        if (kDTableAbi[i].ndw == ndw && kDTableAbi[i].fnv == fnv) return i + 1u;
    return 0u;
}

/* 0.0.391: the VERTEX ABI-pointer row of a vertex identity, by that identity's own (ndw, fnv)
 * AT STAGE 1. index + 1 into kXlat12AbiPtrs, 0 = none. This is the call site found missing: without it the three
 * vertex rows are data nothing reads, and ViewportToNDC's three buffer pointers are carried and never checked. */
static uint32_t d_vs_abi_ptr_of(int id)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return 0u;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    if (s->stage != 1u) return 0u;
    for (uint32_t i = 0; i < XLAT12_ABI_PTR_ROWS; i++)
        if (kXlat12AbiPtrs[i].ndw == s->ndw && kXlat12AbiPtrs[i].fnv == s->fnv && kXlat12AbiPtrs[i].stage == 1u)
            return i + 1u;
    return 0u;
}

/* D4' (notes/design/D4-PRIME.md item 2) — THE xlat12_readset.h ROW of an identity, by that identity's own (ndw, fnv)
 * AT ITS OWN STAGE. index + 1 into kXlat12Readset, 0 = none (UNKNOWN, never "declares no pointers" -
 * xlat12_readset.h's own banner). Exactly the d_table_abi_of / d_vs_abi_ptr_of pattern, for either stage: the row's
 * OWN `stage` field (0 = fragment, 1 = vertex) is asked rather than assumed. proof_depth1_data_only /
 * proof_images_inline are NOT consulted here: this only finds the row; the caller (d_readset_accum, below) is where
 * admission is decided, so the decision stays in ONE place. */
static uint32_t d_readset_of(int id, uint32_t stage)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return 0u;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    if (s->stage != stage) return 0u;
    for (uint32_t i = 0; i < XLAT12_READSET_COUNT; i++)
        if (kXlat12Readset[i].ndw == s->ndw && kXlat12Readset[i].fnv == s->fnv && kXlat12Readset[i].stage == stage)
            return i + 1u;
    return 0u;
}

int xlat12_shader_id_desc_table(int id, uint32_t *table, uint32_t *ntex, uint32_t tex[2], uint32_t *samp)
{
    const uint32_t k = d_table_abi_of(id);
    if (!k) return 0;
    const DTableAbi *a = &kDTableAbi[k - 1u];
    /* build 0.0.470: a CLASS-10 row's texture indices are not user-data slots at all (they live in Apple's entry
     * table) and it has THREE, which this API's tex[2] cannot carry - a caller walking tex[t] for t < ntex would read
     * past its array. So this function does not describe a class-10 row: 0, exactly as for a program with no row. */
    if (d_tbl_c10(a)) return 0;
    if (table) *table = a->table;
    if (ntex) *ntex = a->ntex;
    if (tex) { tex[0] = a->tex[0]; tex[1] = a->tex[1]; }
    if (samp) *samp = a->samp;
    return 1;
}

int xlat12_shader_id_desc_free(int id)
{
    const uint32_t c = xlat12_shader_id_desc_class(id);
    return c == XLAT12_DESCCLS_NONE || c == XLAT12_DESCCLS_RING;
}

const char *xlat12_desc_class_name(uint32_t cls)
{
    static const char *const n[] = { "UNPROVEN", "NONE", "RING", "IMAGE", "BUFFER" };
    return cls < 5u ? n[cls] : "?";
}

int xlat12_shader_id_sampling(int id, uint32_t *desc, uint32_t *tex, uint32_t *samp)
{
    if (id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return 0;
    const xlat12_shader_id *s = &kXlat12ShaderIds[id];
    if (s->stage != 0u || s->tex_slot == 0xffu || s->samp_slot == 0xffu || s->desc_slot == 0xffu) return 0;
    if (desc) *desc = s->desc_slot;
    if (tex) *tex = s->tex_slot;
    if (samp) *samp = s->samp_slot;
    return 1;
}

/* 0.0.309: fill ONE stage's fields from one identity, for the per-stage resolver
 * (xlat12_draw_extra.pgm_profile). `io` is the caller's two-element cache - io[0] vertex, io[1] fragment - kept across
 * calls because SPI_SHADER_GS_OUT_CONFIG_PS is the one field that depends on BOTH stages; this function updates the
 * entry for the stage it filled and recomputes that field from both. Calling it for a vertex identity and then a
 * fragment one, starting from the m2tri profile with io = {0,0}, produces exactly what xlat12_ib_profile_for(vs, ps)
 * produces - which the test asserts over every identity pair, so the two cannot drift apart. */
uint32_t xlat12_ib_profile_stage(int id, xlat12_draw_profile *out, uint32_t io[2])
{
    if (!out || !io || id < 0 || (uint32_t)id >= XLAT12_SHADER_ID_COUNT) return XLAT12_ERR_ARG;
    const xlat12_shader_id *e = &kXlat12ShaderIds[id];
    if (e->stage == 1u) {
        out->vs_rsrc1_gs = e->w[0]; out->vs_rsrc2_gs = e->w[1];
        for (uint32_t k = 0; k < 4u; k++) out->vs_isa_head[k] = e->head[k];
        out->vs_abi_ptr1 = d_vs_abi_ptr_of(id);   /* 0.0.391 */
        out->vs_readset1 = d_readset_of(id, 1u);  /* D4' (D4-PRIME.md item 2) */
        io[0] = e->io;
    } else {
        out->ps_rsrc1 = e->w[0]; out->ps_rsrc2 = e->w[1]; out->ps_input_ena = e->w[2]; out->ps_input_addr = e->w[3];
        for (uint32_t k = 0; k < 4u; k++) out->ps_isa_head[k] = e->head[k];
        io[1] = e->io;
        d_inline_fill(id, out);   /* */
        out->ps_table_abi1 = d_table_abi_of(id);   /* M4-DESC-TABLE-IMPL */
        out->ps_id = (uint32_t)id;   /* 0.0.398: the identity the fill-colour retarget gates on */
        out->ps_readset1 = d_readset_of(id, 0u);   /* D4' (D4-PRIME.md item 2) */
    }
    out->gs_out_config_ps = (io[0] ? ((io[0] - 1u) & 0x1Fu) : 0x400u) | ((io[1] & 0x3Fu) << 11);
    return 0;
}

uint32_t xlat12_ib_profile_for(int vs_id, int ps_id, xlat12_draw_profile *out)
{
    if (!out || vs_id < 0 || ps_id < 0 || (uint32_t)vs_id >= XLAT12_SHADER_ID_COUNT || (uint32_t)ps_id >= XLAT12_SHADER_ID_COUNT)
        return XLAT12_ERR_ARG;
    const xlat12_shader_id *v = &kXlat12ShaderIds[vs_id], *p = &kXlat12ShaderIds[ps_id];
    if (v->stage != 1u || p->stage != 0u) return XLAT12_ERR_ARG;
    const uint32_t *src = (const uint32_t *)xlat12_ib_m2tri_profile();
    uint32_t *dst = (uint32_t *)out;
    for (uint32_t k = 0; k < sizeof *out / sizeof(uint32_t); k++) dst[k] = src[k];
    out->vs_rsrc1_gs = v->w[0]; out->vs_rsrc2_gs = v->w[1];
    out->ps_rsrc1 = p->w[0]; out->ps_rsrc2 = p->w[1]; out->ps_input_ena = p->w[2]; out->ps_input_addr = p->w[3];
    for (uint32_t k = 0; k < 4u; k++) { out->vs_isa_head[k] = v->head[k]; out->ps_isa_head[k] = p->head[k]; }
    /* 0.0.226: SPI_SHADER_GS_OUT_CONFIG_PS from the records (xlat12_ib.h); m2tri gives 0x400, the profile's constant */
    out->gs_out_config_ps = (v->io ? ((v->io - 1u) & 0x1Fu) : 0x400u) | ((p->io & 0x3Fu) << 11);
    d_inline_fill(ps_id, out);   /*, as profile_stage does, so the two stay identical */
    out->ps_table_abi1 = d_table_abi_of(ps_id);   /* M4-DESC-TABLE-IMPL, likewise */
    out->vs_abi_ptr1 = d_vs_abi_ptr_of(vs_id);    /* 0.0.391, likewise */
    out->ps_id = (uint32_t)ps_id;                 /* 0.0.398, likewise */
    out->ps_readset1 = d_readset_of(ps_id, 0u);   /* D4' (D4-PRIME.md item 2), likewise */
    out->vs_readset1 = d_readset_of(vs_id, 1u);   /* D4' (D4-PRIME.md item 2), likewise */
    return 0;
}

void xlat12_attr_ring_descriptor(uint64_t va, uint32_t size, uint32_t desc[4])
{
    if (!desc) return;
    desc[0] = (uint32_t)va;
    desc[1] = ((uint32_t)(va >> 32) & 0xFFFFu) | (3u << 30);
    desc[2] = size;
    desc[3] = XLAT12_ATTR_DESC_WORD3;
}

uint32_t xlat12_ib_draw_selftest(void)
{
    static uint32_t o[1022], mod[1022];
    const uint32_t n = 1022u;
    const xlat12_draw_profile *pf = xlat12_ib_m2tri_profile();
    xlat12_draw_stats ds;
    uint32_t bits = 0, len = 0, v = 0;
    if (xlat12_ib_translate_draw(pf, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n) {
        bits |= 1u;
        int same = ds.draw_in_at == 0x3dfu;
        for (uint32_t k = ds.draw_in_at; same && k < n; k++) if (o[k] != kR44DrawInjIb[k]) same = 0;
        if (same) bits |= 2u;
        uint32_t ba = 0, bo = 0;
        if (xlat12_ib_draw_verify(o, n, &ba, &bo) == 0u) bits |= 4u;
        int rh = xlat12_ib_find_set(o, n, 0xb224u, &v) && v == 0x04000289u;
        rh = rh && xlat12_ib_find_set(o, n, 0xb240u, &v) && v == 0x00003000u;
        rh = rh && xlat12_ib_find_set(o, n, 0xb244u, &v) && v == 0x00000004u;
        rh = rh && xlat12_ib_find_set(o, n, 0x28a98u, &v) && v == pf->vs_stages_en;
        rh = rh && xlat12_ib_find_set(o, n, 0x3096cu, &v) && v == pf->ge_cntl;
        rh = rh && xlat12_ib_find_set(o, n, 0x28c44u, &v) && v == pf->binner_cntl0;
        rh = rh && xlat12_ib_find_set(o, n, 0x28b4cu, &v) && v == pf->ngg_subgrp_cntl;
        rh = rh && xlat12_ib_find_set(o, n, 0x28b38u, &v) && v == pf->gs_max_vert_out;
        rh = rh && xlat12_ib_find_set(o, n, 0x30998u, &v) && v == pf->gs_out_prim_type;   /* 0.0.215 */
        rh = rh && xlat12_ib_find_set(o, n, 0x28648u, &v) && v == pf->spi_shader_idx_format;   /* 0.0.216 */
        rh = rh && !xlat12_ib_find_set(o, n, 0xb120u, &v) && !xlat12_ib_find_set(o, n, 0x28b54u, &v);
        if (rh) bits |= 8u;
        xlat12_ib_census c; xlat12_ib_census_run(o, n, &c);
        if (c.dma_data == 0u && c.ctxctl_other == 0u && ds.prefetch_nopped == 2u && ds.ctxctl_dropped == 1u &&
            c.draws == 1u && c.reg_cls[XLAT12_CLS_UNKNOWN] >= 0u) bits |= 16u;
    }
    {   /* refusal controls: a non-zero write to a listed unnamed address, and a second draw */
        uint32_t ok = 0;
        for (uint32_t k = 0; k < n; k++) mod[k] = kR44DrawInjIb[k];
        for (uint32_t k = 0; k + 2u < n; k++)            /* [029b] SET_CONTEXT_REG 0x286dc = 0 -> 1 */
            if (mod[k] == 0xC0016900u && mod[k + 1] == ((0x286dcu - 0x28000u) >> 2)) { mod[k + 2] = 1u; break; }
        if (xlat12_ib_translate_draw(pf, mod, n, o, &len, &ds) == XLAT12_ERR_UNKNOWN_REG && ds.err_reg == 0x286dcu) ok++;
        /* 0.0.307, a DELIBERATE CHANGE: a second draw used to REFUSE with DRAW_SHAPE. It is now translated -
         * a real client frame has 9 draws in one of Apple's encoder segments - and what must hold is that BOTH draws stay
         * at their own dword index, which is the in-place contract the whole layout rests on. The refusal control moves to
         * a stream with NO draw, which still refuses. */
        for (uint32_t k = 0; k < n; k++) mod[k] = kR44DrawInjIb[k];
        mod[0x3ec] = 0xC0012D00u; mod[0x3ed] = 3u; mod[0x3ee] = 2u;   /* a second DRAW_INDEX_AUTO in the tail NOP */
        mod[0x3ef] = 0xC0041000u;                                       /* the rest of that NOP stays a NOP */
        if (xlat12_ib_translate_draw(pf, mod, n, o, &len, &ds) == 0u && ds.draws == 2u && len == n &&
            o[0x3df] == mod[0x3df] && o[0x3e0] == mod[0x3e0] && o[0x3e1] == mod[0x3e1] &&
            o[0x3ec] == mod[0x3ec] && o[0x3ed] == mod[0x3ed] && o[0x3ee] == mod[0x3ee]) ok++;
        for (uint32_t k = 0; k < n; k++) mod[k] = kR44DrawInjIb[k];
        mod[0x3df] = 0xC0011000u; mod[0x3e0] = 0u; mod[0x3e1] = 0u;   /* the only draw becomes a 3-dword NOP: no draw at all */
        if (xlat12_ib_translate_draw(pf, mod, n, o, &len, &ds) == XLAT12_IB_ERR_DRAW_SHAPE && ds.err_op == 0u) ok++;
        if (ok == 3u) bits |= 32u;
    }
    {   /* 0.0.217: the ring/CU block's exact dwords for base 0x4005A0000, last before the pad; the draw keeps its offset.
         * 0.0.219: + SPI_SHADER_PGM_LO/HI_GS = 0 (the merged GS stage's s0:s1) */
        static const xlat12_draw_extra ex = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, 0u, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra bad = { 0x4005A1000ull, XLAT12_RSRC3_GS_CU_EN, 0u, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const uint32_t want[19] = { 0xC0047900u, 0x00000444u, 0x12355123u, 0x0001544Du, 0x0004005Au, 0x00020015u,
                                           0xC0047900u, 0x00000268u, 0x000400B2u, 0x00002000u, 0x000400F2u, 0x0C6E07FEu,
                                           0xC0017600u, 0x00000087u, 0xFFFFFDFDu,
                                           0xC0027600u, 0x00000084u, 0x00000000u, 0x00000000u };
        /* 0.0.502: r44's region 0 writes DB_SHADER_CONTROL, so DB_SPI_VRS_CENTER_LOCATION = 0 (mesa
         * ac_cmdbuf.c:702) is merged in front of it (d_vrs_prefollow) and the block itself is unchanged: checked by value */
        uint32_t ok = 0;
        if (xlat12_ib_translate_draw_ex(pf, &ex, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n && ds.ring_dwords == 19u &&
            ds.gs_sgpr0_dwords == XLAT12_GS_SGPR0_DWORDS && (ds.ring_ptr_stages & 6u) == 6u &&   /*: GS appended, HS in place */
            ds.draw_in_at == 0x3dfu && ds.draw_out_len >= 19u && xlat12_ib_draw_verify(o, n, 0, 0) == 0u &&
            xlat12_ib_find_set(o, n, XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION, &v) && v == 0u) {
            int same = 1;
            for (uint32_t k = 0; k < 19u; k++) if (o[ds.draw_out_len - 19u + k] != want[k]) same = 0;
            for (uint32_t k = ds.draw_in_at; same && k < n; k++) if (o[k] != kR44DrawInjIb[k]) same = 0;
            if (same) ok++;
        }
        if (xlat12_ib_translate_draw_ex(pf, &bad, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_RING) ok++;
        if (ok == 2u) bits |= 64u;
    }
    {   /* 0.0.218: the preamble delta on top of the ring/CU block. 0.0.219: RSRC2_GS keeps Apple's USER_SGPR 8
         * (0x10), PGM_LO/HI_GS are written, and the retired flag 0x2 is refused */
        static const xlat12_draw_extra ex2 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_PREAMBLE, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra ex3 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_PREAMBLE | 0x2u, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const struct { uint32_t a, v; } chk[] = {
            { 0xb22cu, 0x00000010u }, { 0xb210u, 0u }, { 0xb214u, 0u }, { 0xb0c0u, 0x7u }, { 0xb2c8u, 0u }, { 0xb2d4u, 0u }, { 0xb0d4u, 0u }, { 0xb4c8u, 0u },
            { 0x2882cu, 0u }, { 0x28a80u, 0u }, { 0x28c4cu, 0x00800000u }, { 0x30950u, 0x7F9A80E1u }, { 0x30980u, 0u },
            { 0x31128u, 0x00008A4Du }, { 0x3112cu, 0x00401123u }, { 0xb21cu, XLAT12_RSRC3_GS_CU_EN }, { 0x309acu, 0x0C6E07FEu },
            { XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION, 0u } /* 0.0.502 */ };
        int ok = xlat12_ib_translate_draw_ex(pf, &ex2, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n &&
                 ds.ring_dwords == 19u + XLAT12_PREAMBLE_DWORDS && ds.preamble_dwords == XLAT12_PREAMBLE_DWORDS &&
                 ds.vs_user_sgpr == 8u && ds.draw_in_at == 0x3dfu &&
                 o[0x3df] == kR44DrawInjIb[0x3df] && xlat12_ib_draw_verify(o, n, 0, 0) == 0u;
        for (uint32_t k = 0; ok && k < sizeof chk / sizeof chk[0]; k++)
            ok = xlat12_ib_find_set(o, n, chk[k].a, &v) && v == chk[k].v;
        ok = ok && xlat12_ib_translate_draw_ex(pf, &ex3, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_ERR_ARG;
        if (ok) bits |= 128u;
    }
    {   /* 0.0.220: the raster/CB delta on top of the ring/CU block.
         * 0.0.389 (notes 881): WITHOUT the preamble delta, where this block used PREAMBLE|RASTER through 0.0.388.
         * Change A spends 7 of this stream's pad dwords (the reused PA_SC_HIZ_INFO/HIS_INFO pair, 4, plus the
         * synthesized PA_SC_HISZ_CONTROL packet, 3) and had measured preamble+raster+rings as leaving exactly
         * ONE spare dword here, so the two blocks together no longer fit r44. That is checked POSITIVELY below
         * rather than dropped. Raster alone is what the armed path sets and what measured REQUIRED. */
        static const xlat12_draw_extra ex4 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_RASTER, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra ex4b = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_PREAMBLE | XLAT12_EXTRA_RASTER, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const struct { uint32_t a, v; } chk4[] = {
            { 0x28c68u, 0u }, { 0x28c70u, XLAT12_CB0_FDCC_CONTROL }, { 0x283d0u, 0u }, { 0x283e0u, 0u }, { 0x28bc0u, 0u },
            { 0x28f00u, XLAT12_CB_MEM0_INFO }, { 0x28c64u, 0u }, { 0x28c6cu, 0u }, { 0xb210u, 0u }, { 0xb22cu, 0x10u },
            { XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION, 0u } /* 0.0.502 */ };
        int ok = xlat12_ib_translate_draw_ex(pf, &ex4, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n &&
                 ds.ring_dwords == 19u + XLAT12_RASTER_DWORDS && ds.raster_dwords == XLAT12_RASTER_DWORDS &&
                 ds.raster_follow == 2u && ds.draw_in_at == 0x3dfu && o[0x3df] == kR44DrawInjIb[0x3df] &&
                 xlat12_ib_draw_verify(o, n, 0, 0) == 0u;
        for (uint32_t k = 0; ok && k < sizeof chk4 / sizeof chk4[0]; k++)
            ok = xlat12_ib_find_set(o, n, chk4[k].a, &v) && v == chk4[k].v;
        /* 0.0.389: the reused pair IS emitted (Apple writes it in this stream) and PA_SC_HISZ_CONTROL sits beside it */
        ok = ok && ds.reused_regs == 2u && ds.reused_nonzero == 0u && ds.hisz_control == 1u &&
             xlat12_ib_find_set(o, n, 0x28b94u, &v) && v == 0u &&
             xlat12_ib_find_set(o, n, 0x28b98u, &v) && v == 0u &&
             xlat12_ib_find_set(o, n, XLAT12_G12_PA_SC_HISZ_CONTROL, &v) && v == XLAT12_PA_SC_HISZ_CONTROL;
        /* 0.0.389: and preamble+raster together no longer fit this stream - stated, not silently lost */
        ok = ok && xlat12_ib_translate_draw_ex(pf, &ex4b, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_TOO_LONG;
        if (ok) bits |= 256u;
    }
    {   /* 0.0.224: two r44 segments end to end split into two with one draw each, and both translate in place; a stream
         * that begins at a trailer-shaped or state packet is not a segment start */
        static uint32_t two[2048], so[1022];
        static const xlat12_draw_extra ex5 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_RASTER, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   /* 0.0.389: raster only, see bit 256 */
        for (uint32_t s2 = 0; s2 < 2u; s2++) {
            two[1024u * s2] = XLAT12_SEG_HEAD0; two[1024u * s2 + 1u] = XLAT12_SEG_HEAD1;
            for (uint32_t k = 0; k < n; k++) two[1024u * s2 + 2u + k] = kR44DrawInjIb[k];
        }
        xlat12_ib_segment sg[4];
        uint32_t tot = 0;
        const uint32_t ns = xlat12_ib_segments(two, 2048u, sg, 4u, &tot);
        int ok = ns == 2u && tot == 2u && sg[0].head == 0u && sg[0].start == 2u && sg[0].end == 1024u && sg[1].head == 1024u &&
                 sg[1].start == 1026u && sg[1].end == 2048u && sg[0].draws == 1u && sg[1].draws == 1u && sg[1].draw_at == 1026u + 0x3dfu;
        for (uint32_t s2 = 0; ok && s2 < 2u; s2++)
            ok = xlat12_ib_translate_draw_ex(pf, &ex5, &two[sg[s2].start], sg[s2].end - sg[s2].start, so, &len, &ds) == 0u &&
                 len == 1022u && so[0x3df] == kR44DrawInjIb[0x3df];
        ok = ok && xlat12_ib_segments(&two[2], 2046u, sg, 4u, &tot) == 0u && tot == 0u;
        ok = ok && xlat12_ib_segments(two, 2048u, sg, 1u, &tot) == 1u && tot == 2u;   /* more starts than slots: counted, not filled */
        if (ok) bits |= 512u;
    }
    {   /* 0.0.224: the ablation flags emit Apple's own SPI_SHADER_IDX_FORMAT / VGT_GS_OUT_PRIM_TYPE values (r44: both 0) */
        static const xlat12_draw_extra ex6 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, XLAT12_EXTRA_APPLE_IDXFMT | XLAT12_EXTRA_APPLE_OUTPRIM, 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        int ok = xlat12_ib_translate_draw_ex(pf, &ex6, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n &&
                 xlat12_ib_find_set(o, n, 0x30998u, &v) && v == 0u && xlat12_ib_find_set(o, n, 0x28648u, &v) && v == 0u &&
                 xlat12_ib_draw_verify(o, n, 0, 0) == 0u;
        ok = ok && xlat12_ib_translate_draw_ex(pf, 0, kR44DrawInjIb, n, o, &len, &ds) == 0u &&
             xlat12_ib_find_set(o, n, 0x30998u, &v) && v == pf->gs_out_prim_type && xlat12_ib_find_set(o, n, 0x28648u, &v) &&
             v == pf->spi_shader_idx_format;
        if (ok) bits |= 1024u;
    }
    {   /* 0.0.226: PGM_LO/HI_GS carry the ring-offsets table VA when given (low / high 32 bits), a misaligned table VA or one
         * without a ring base is refused, the attribute-ring descriptor matches Mesa's layout for the 0x400800000 rings, and
         * GS_OUT_CONFIG_PS is 0x400 for a pair without parameters and 0x800 for one parameter and one interpolant */
        static const xlat12_draw_extra ex7 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, 0u, 0x4005A0000ull + XLAT12_GE_RING_DESC_OFF, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra ex8 = { 0x4005A0000ull, XLAT12_RSRC3_GS_CU_EN, 0u, 0x4005A0000ull + XLAT12_GE_RING_DESC_OFF + 4u, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra ex9 = { 0u, XLAT12_RSRC3_GS_CU_EN, 0u, 0x401020000ull, 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t d[4];
        int ok = xlat12_ib_translate_draw_ex(pf, &ex7, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n &&
                 xlat12_ib_find_set(o, n, 0xb210u, &v) && v == 0x01020000u && xlat12_ib_find_set(o, n, 0xb214u, &v) && v == 4u &&
                 xlat12_ib_draw_verify(o, n, 0, 0) == 0u;
        ok = ok && xlat12_ib_translate_draw_ex(pf, &ex8, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_RING &&
             xlat12_ib_translate_draw_ex(pf, &ex9, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_RING;
        xlat12_attr_ring_descriptor(0x400800000ull, XLAT12_GE_RING_TOTAL, d);
        ok = ok && d[0] == 0x00800000u && d[1] == 0xC0000004u && d[2] == 0x00A80000u && d[3] == 0x0043FFACu;
        int v0 = -1, v1 = -1, f0 = -1, f1 = -1;
        for (uint32_t k = 0; k < XLAT12_SHADER_ID_COUNT; k++) {
            const xlat12_shader_id *sid = &kXlat12ShaderIds[k];
            if (sid->stage == 1u && sid->io == 0u && v0 < 0) v0 = (int)k;
            if (sid->stage == 1u && sid->io == 1u && v1 < 0) v1 = (int)k;
            if (sid->stage == 0u && sid->io == 0u && f0 < 0) f0 = (int)k;
            if (sid->stage == 0u && sid->io == 1u && f1 < 0) f1 = (int)k;
        }
        xlat12_draw_profile pp;
        ok = ok && v0 >= 0 && v1 >= 0 && f0 >= 0 && f1 >= 0 && xlat12_ib_profile_for(v0, f0, &pp) == 0u && pp.gs_out_config_ps == 0x400u &&
             xlat12_ib_profile_for(v1, f1, &pp) == 0u && pp.gs_out_config_ps == 0x800u;
        if (ok) bits |= 2048u;
    }
    {   /* 0.0.265: a RELOCATED vertex program. SPI_SHADER_PGM_LO/HI_ES name our own arena VA rather than
         * re-homing Apple's value, the gfx10 slot is gone, the output still verifies, and a relocation that is not
         * 256-byte aligned (PGM_LO is VA >> 8) or sits at or above 2^48 is REFUSED rather than silently truncated. */
        const uint64_t rva = 0x23F0000000ull + XLAT12_RELOC_ARENA_OFF;      /* our ring VA + the arena offset */
        /* positional static const, as the ex7..ex9 block above: this file compiles as C11 AND as C++17
         * under -Werror (make kextcheck), where a compound literal is a clang extension. */
        static const xlat12_draw_extra exr    = { 0u, 0u, 0u, 0u, 0x23F0000000ull + XLAT12_RELOC_ARENA_OFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra exbad1 = { 0u, 0u, 0u, 0u, 0x23F0000000ull + XLAT12_RELOC_ARENA_OFF + 0x80u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const xlat12_draw_extra exbad2 = { 0u, 0u, 0u, 0u, 1ull << 48, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t lo = 0, hi = 0;
        int ok = xlat12_ib_translate_draw_ex(pf, &exr, kR44DrawInjIb, n, o, &len, &ds) == 0u && len == n &&
                 xlat12_ib_find_set(o, n, 0xb224u, &lo) && lo == (uint32_t)(rva >> 8) &&
                 xlat12_ib_find_set(o, n, 0xb218u, &hi) && hi == (uint32_t)(rva >> 40) &&
                 !xlat12_ib_find_set(o, n, 0xb120u, &v) && !xlat12_ib_find_set(o, n, 0xb124u, &v) &&
                 xlat12_ib_draw_verify(o, n, 0, 0) == 0u;
        /* the VA the kext reconstructs from the two halves is the one we asked for */
        ok = ok && ((((uint64_t)(hi & 0xFFu)) << 40) | ((uint64_t)lo << 8)) == rva;
        /* and without a relocation the slot is re-homed exactly as before */
        ok = ok && xlat12_ib_translate_draw_ex(pf, 0, kR44DrawInjIb, n, o, &len, &ds) == 0u &&
             xlat12_ib_find_set(o, n, 0xb224u, &v) && v == 0x04000289u;
        ok = ok && xlat12_ib_translate_draw_ex(pf, &exbad1, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_RING &&
             xlat12_ib_translate_draw_ex(pf, &exbad2, kR44DrawInjIb, n, o, &len, &ds) == XLAT12_IB_ERR_RING;
        if (ok) bits |= 4096u;
    }
    return bits;
}
