/* xlat12_ib - the IB-level driver around xlat12_translate.
 *
 * xlat12_translate works on runs of SET_* packets and refuses the memory-loaded class. A real
 * Apple IB also carries CONTEXT_CONTROL, COND_EXEC, sync packets (WRITE_DATA / RELEASE_MEM /
 * WAIT_REG_MEM / ACQUIRE_MEM / EVENT_WRITE), draws and dispatches. This layer walks a whole IB and:
 *   census     counts packets and registers by xlat12 class, flags the memory-loaded class,
 *              register operands in non-SET packets, nested IBs, DMA_DATA and unlisted opcodes;
 *   blank      (an INSTRUMENT) keeps only the packets proven on this silicon by milestone 1's blit IB
 *              and rewrites every other packet into one-dword NOPs (0xFFFF1000), SAME LENGTH;
 *   translate  translates SET_* packets with xlat12, passes the proven packets verbatim, recomputes
 *              COND_EXEC exec counts over their translated regions (rule 50), and refuses anything
 *              else - never passes an unlisted opcode or an untranslatable register operand through;
 *   pad        fills a translated IB to the original length with one-dword NOPs (in-place layout).
 * Plain C that also compiles as C++; no heap, no floating point, no libc.
 */
#ifndef XLAT12_IB_H
#define XLAT12_IB_H
#include <stdint.h>
#include "xlat12.h"
#include "xlat12_abi_ptrs.h"   /* 0.0.390 ( part 5 (ii)): XLAT12_ABI_PTR_MAX and the ABI rows */

#ifdef __cplusplus
extern "C" {
#endif

/* Status values beyond xlat12_status. */
enum {
    XLAT12_IB_ERR_UNLISTED     = 20, /* an opcode outside the proven/translatable set */
    XLAT12_IB_ERR_REG_OPERAND  = 21, /* WRITE_DATA/WAIT_REG_MEM/COPY_DATA on a non-identical register */
    XLAT12_IB_ERR_COND_EXEC    = 22, /* COND_EXEC region malformed or nested */
    XLAT12_IB_ERR_TOO_LONG     = 23, /* translated IB longer than the in-place target */
    XLAT12_IB_ERR_DRAW_SHAPE   = 24, /* draw policy: not exactly one draw, or a tail packet it does not keep */
    XLAT12_IB_ERR_VERIFY       = 25, /* draw policy: the output failed its own gfx12 legality walk */
    XLAT12_IB_ERR_RING         = 26, /* 0.0.217: GE ring base not 64 KiB aligned, or the rings end above 2^48 */
    XLAT12_IB_ERR_PAIR         = 27, /* 0.0.307: the caller's resolver refused the program pair a draw binds */
    XLAT12_IB_ERR_INTERP       = 28, /* 0.0.313: this draw's fragment stage consumes an interpolated attribute
                                      * (SPI_PS_IN_CONTROL NUM_INTERP or PARAM_GEN non-zero), or the register was not
                                      * positively established before the draw, while the bound vertex program's image
                                      * exports none. err_reg is 0x286d8; err_op is the value, or ~0 when never seen. */
    XLAT12_IB_ERR_DESC         = 29, /* (XLAT12_EXTRA_INLINE_DESC only): this draw's fragment program samples through an
                                      * INLINE T#/S# in its user data and the record could not be translated: a slot not
                                      * written by THIS translation (inherited = unknown), a record half-translated by an
                                      * earlier draw, or xlat12_img/samp_desc_g10_to_g12 refused (err_op its code). err_reg
                                      * is the gfx10 address of the record's first user-data register. */
    XLAT12_IB_ERR_SWMODE       = 30  /* build 0.0.451 item 8 (reviewer-confirmed via decide43): a CB_COLORn_ATTRIB3,
                                      * DB_Z_INFO or DB_STENCIL_INFO write names a gfx10 COLOR_SW_MODE/SW_MODE this build's
                                      * kXlat12SwModeG10ToG12 (xlat12_desc.h) does not map - the SAME table and the SAME
                                      * refusal-direction choice the T#/S# descriptor path already makes for an unmapped
                                      * SW_MODE. err_reg is the gfx10 address of the register; err_op is the raw gfx10
                                      * SW_MODE value that had no gfx12 counterpart. */
};

#define XLAT12_IB_NOP 0xFFFF1000u   /* PACKET3(NOP, 0x3FFF): one dword, proven on this silicon */

/* The exact CONTEXT_CONTROL Apple's blit IB carries, executed untranslated in r38/r39. */
/* 0.0.307: the most draws one translation handles. A captured SecurityAgent frame has 11 in two of Apple's
 * encoder segments (9 + 2); the tri suite has one. The cap bounds a stack array, and exceeding it REFUSES. */
#define XLAT12_MAX_DRAWS 32u
#define XLAT12_IB_CTXCTL_HDR 0xC0012800u
#define XLAT12_IB_CTXCTL_DW1 0x80000000u
#define XLAT12_IB_CTXCTL_DW2 0x80000000u

typedef struct {
    uint32_t walk_len;           /* dwords of well-formed TYPE3/TYPE2 PM4 from dword 0 */
    uint32_t packets;
    uint32_t ends_on_nop_128;    /* the walk ends at a multiple of 128 dwords right after a NOP */
    uint32_t early_nop_128;      /* earlier NOP-then-128-boundary points (a second IB may follow) */
    uint32_t first_is_ctxctl;    /* dword 0 is CONTEXT_CONTROL */
    uint32_t ctxctl_proven, ctxctl_other;   /* CONTEXT_CONTROL exact proven form / any other form */
    uint32_t clear_state, load_reg;         /* memory-loaded class (LOAD_* incl. _INDEX, CLEAR_STATE) */
    uint32_t cond_exec, nested_ib, draws, dispatches, dma_data, copy_data;
    uint32_t reg_operand_bad;    /* WRITE_DATA/WAIT_REG_MEM/COPY_DATA on a non-identical register */
    uint32_t unlisted;           /* opcodes outside the proven/translatable set */
    uint32_t set_ctx, set_sh_gfx, set_sh_cs, set_ucfg, set_index;
    uint32_t regs;               /* registers in SET packets */
    uint32_t reg_cls[7];         /* by Xlat12Class (0.0.389: 7 classes, REUSED is the last); untabled addresses count as UNKNOWN */
    uint32_t cs_proven_regs;     /* compute SET_SH_REG registers in the proven blit set */
    uint32_t first_unlisted_op;  /* 0xFFFFFFFF none */
    uint32_t first_unknown_reg;  /* gfx10 byte address, 0 none */
    uint32_t first_memloaded_op; /* 0xFFFFFFFF none */
} xlat12_ib_census;

/* Length of well-formed PM4 from dword 0 (stops at the first non-TYPE2/TYPE3 header or a packet
 * that runs past n). */
uint32_t xlat12_ib_walk(const uint32_t *in, uint32_t n);

/* Census over in[0..n) (n is normally xlat12_ib_walk's result). */
void xlat12_ib_census_run(const uint32_t *in, uint32_t n, xlat12_ib_census *c);

/* Blank: out[0..n) = in with every non-proven packet replaced by NOPs. *kept / *blanked count
 * packets. Returns 0, or XLAT12_ERR_TRUNCATED / XLAT12_ERR_BAD_PACKET on a malformed stream. */
uint32_t xlat12_ib_blank(const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *kept, uint32_t *blanked);

/* Translate a whole IB. Returns 0 (OK) or an xlat12_status / XLAT12_IB_ERR_*; *err_op names the
 * failing opcode, stats->err_reg_addr the register where known. */
uint32_t xlat12_ib_translate(xlat12_ctx *ctx, const uint32_t *in, uint32_t n, uint32_t *out,
                             uint32_t cap, uint32_t *out_len, xlat12_stats *stats, uint32_t *err_op);

/* In-place layout: NOP-fill buf[len..target). Returns 0, or XLAT12_IB_ERR_TOO_LONG if len > target. */
uint32_t xlat12_ib_pad(uint32_t *buf, uint32_t len, uint32_t target);

const char *xlat12_ib_status_name(uint32_t s);

/* The positive control the kext runs before any scan (rule 25), host-tested so the control itself is
 * right (r40 lost its census to a malformed control). Bits: 0 identical CTX register keeps
 * its address and value, 1 a moved one lands at its gfx12 address, 2 a repacked value equals
 * xlat12_repack_value, 3 CLEAR_STATE is refused ERR_MEMLOADED, 4 the census of Apple's r30 blit IB,
 * 5 its blank (9 packets kept, the dispatch NOPed), 6 its translation (128 -> 125, pads to 128). */
#define XLAT12_IB_SELFTEST_ALL 0x7Fu
uint32_t xlat12_ib_selftest(void);

/* ---- The milestone-2 DRAW policy ----------------------------------------------------------------
 * Apple's render encoder emits a gfx10 LEGACY-VS pipeline (r44/r45: VGT_SHADER_STAGES_EN 0x00810000, PRIMGEN_EN 0,
 * the vertex program in SPI_SHADER_PGM_LO_VS). gfx12 has only the NGG vertex path. For a draw whose two stages are
 * OUR packed gfx1201 records, xlat12_ib_translate_draw:
 *   - re-homes the legacy-VS slot onto gfx12's NGG slot as radeonsi programs it on gfx12 (SPI_SHADER_PGM_LO_ES
 *     0xb224, PGM_HI_ES 0xb218, RSRC1_GS 0xb228, RSRC2_GS 0xb22c, USER_DATA_GS_n 0xb230+4n);
 *   - takes RSRC1/RSRC2 and VGT_SHADER_STAGES_EN / SPI_PS_INPUT_ENA/ADDR from the records' own register notes (the
 *     profile), keeping Apple's USER_SGPR count;
 *   - after every VGT_SHADER_STAGES_EN, synthesizes the NGG state a legacy stream never programs (radeonsi's gfx12
 *     recipe for a passthrough VS: GE_MAX_OUTPUT_PER_SUBGROUP, RSRC4_GS, RSRC4_PS, SPI_SHADER_GS_OUT_CONFIG_PS,
 *     GE_CNTL), and overrides Apple's GE_CNTL writes with the same value;
 *   - NOPs Apple's shader-prefetch DMA_DATA (control 0x60200001: prefetch_parser, dst_nowhere) and the CONTEXT_CONTROL
 *     80000002/80000000 that the proven 80000000/80000000 form immediately follows;
 *   - drops the gfx10 VS-slot CU_EN / late-alloc words (RSRC3_VS, LATE_ALLOC_VS, RSRC4_VS, gfx10 RSRC4_PS/HS); gfx12's
 *     GS/ES CU-enable mask is SPI_SHADER_PGM_RSRC3_GS 0xb21c (CU_EN[31:0]; gfx12 RSRC4_GS has NO CU field), written only by
 *     the ring/CU block of xlat12_ib_translate_draw_ex (0.0.217, review report 9), and the measured ZERO writes
 *     to addresses gfx10.3 does not name (a non-zero value there refuses);
 *   - sends everything else through the xlat12 table and refuses what it does not cover.
 * Layout is IN PLACE: the packets before the single draw shrink, NOP padding restores the draw's original dword index,
 * and the draw and the packets after it keep their offsets. `out` has room for n dwords; *out_len == n on success.
 * The output is then walked by xlat12_ib_draw_verify (every SET address a legitimate gfx12 destination, no
 * memory-loaded, prefetch, COND_EXEC, nested-IB or unlisted packet) and refused if it fails. */
/* 0.0.398: no fragment identity has been resolved for the profile in force. XLAT12_EXTRA_FILL_COLOR
 * retargets only a profile whose ps_id names ws_B_ColorFill, so NONE is the fail-closed default (the m2tri base
 * profile and every caller that supplies no resolver carry it). */
#define XLAT12_PS_ID_NONE 0xFFFFFFFFu
typedef struct {
    uint32_t vs_rsrc1_gs, vs_rsrc2_gs, vs_stages_en;            /* our VS record's register notes (gfx12 values) */
    uint32_t ps_rsrc1, ps_rsrc2, ps_input_ena, ps_input_addr;   /* our PS record's register notes (gfx12 values) */
    uint32_t ge_cntl, ge_max_output, rsrc4_gs, rsrc4_ps, gs_out_config_ps;   /* synthesized NGG state */
    uint32_t binner_cntl0, ngg_subgrp_cntl, gs_max_vert_out, gs_instance_cnt; /* 0.0.214: radeonsi gfx12 values */
    uint32_t gs_out_prim_type;   /* 0.0.215 (review report 8,): gfx12 uconfig VGT_GS_OUT_PRIM_TYPE for every Apple
                                  * 0x28a6c write (radeonsi si_state_draw.cpp:1084-1087: TRISTRIP 2 for triangles) */
    uint32_t spi_shader_idx_format; /* 0.0.216: gfx12 SPI_SHADER_IDX_FORMAT for every Apple 0x28708 write:
                                  * IDX0_EXPORT_FORMAT 1COMP, the primitive export's format (radv_shader.c:1917-1922,
                                  * si_state.c:5079-5080); Apple's legacy-VS value is 0 = SPI_SHADER_NONE */
    uint32_t vs_isa_head[4], ps_isa_head[4];                    /* each record's first four ISA dwords */
    uint32_t vs_isa_head_rec[4];  /* 0.0.221: the RECORDING m2_tri_vs (tools/b1-tests/m2instr/build_rec.py), LLVM's own
                                   * head; the kext accepts it by exact match as a second identity and logs which one it saw */
    /* 0.0.218's vs_user_sgpr (6) is gone (0.0.219, , review report 10): the gfx12 VS is a merged NGG stage with eight
     * system SGPRs s0..s7 in front of its user data, so Apple's RSRC2 USER_SGPR 8 (USER_DATA_GS_0-7 -> s8-s15) is kept as written */
    /* 0.0.311: THIS VERTEX PROGRAM'S RELOCATED COPY, per program rather than per segment. `xlat12_draw_extra.
     * vs_pgm_va` names ONE address for a whole translation, which was right when a segment bound one vertex program; a real
     * client segment binds several ( measured nine draws changing program within one segment), and pointing all of them
     * at one relocated copy would render the wrong geometry rather than refuse - the worst failure this translator has.
     * The per-stage resolver already receives Apple's address and is called once per CHANGE, so it is the right place to
     * answer "where does THIS program live". 0 = not relocated; the extra's segment-wide value, then re-homing Apple's own
     * address, follow in that order. d_pair_note CLEARS this before every vertex resolve, so a resolver that answers for one
     * program and forgets the next cannot leak the first one's address into it. */
    uint64_t vs_pgm_va;
    /* 0.0.313: THE VERTEX PROGRAM BOUND HERE EXPORTS NO PARAMETERS. Our gfx1201 image of a program may export
     * fewer attributes than Apple's did - the relocated RectPosTexFast_VS exports none where Apple exported one - and on
     * gfx11+ a parameter is not an export but a store to an attribute ring we do not have for a client frame. That is
     * HARMLESS exactly when the fragment stage consumes nothing the vertex stage exports, and WRONG otherwise, and which
     * one it is is written three registers away in the same segment: SPI_PS_IN_CONTROL's NUM_INTERP [5:0] and PARAM_GEN
     * [6] (gc_10_3_0_sh_mask.h). With this set, a draw is REFUSED XLAT12_IB_ERR_INTERP unless BOTH are zero AND the
     * register was positively seen in this stream before the draw - "not established" must never be read as zero, because
     * a stale zero would serve a draw that should refuse and paint garbage. The caller sets it from its own image's
     * parameter count; 0 keeps every earlier caller's behaviour exactly. */
    uint32_t vs_drops_params;
    /*: THE FRAGMENT PROGRAM BOUND HERE SAMPLES THROUGH AN INLINE T# (and S#) held in its own user data: user-data slot
     * (ps_inline_tex1 - 1) .. +7 is the image record and (ps_inline_samp1 - 1) .. +3 the sampler; 0 = none. Filled by
     * xlat12_ib_profile_stage / _for from xlat12_shader_desc.h's machine-code annotation, CLEARED by d_pair_note before every
     * fragment resolve (so one program's slots never leak into the next), and READ ONLY under XLAT12_EXTRA_INLINE_DESC. */
    uint32_t ps_inline_tex1, ps_inline_samp1;
    /* M4-DESC-TABLE-IMPL (XLAT12_EXTRA_TABLE_DESC): THE FRAGMENT PROGRAM BOUND HERE SAMPLES THROUGH A CLASS-19 TABLE - index + 1
     * into xlat12_ib.c's table-ABI list (the slots of the table pointer, of each image index and of the sampler index), 0 = none.
     * Filled and cleared exactly as ps_inline_tex1, READ ONLY under XLAT12_EXTRA_TABLE_DESC. */
    uint32_t ps_table_abi1;
    /* 0.0.391: THE VERTEX PROGRAM BOUND HERE DECLARES RAW POINTER ARGUMENTS - index + 1 into
     * xlat12_abi_ptrs.h's rows for its (ndw, fnv) identity AT STAGE 1, 0 = no row (UNKNOWN, never "declares none").
     * Filled and cleared exactly as vs_pgm_va and vs_drops_params are, READ ONLY under XLAT12_EXTRA_TABLE_DESC, and read
     * only to EXPORT the pointer pages - no output dword, return code or counter depends on it. APPENDED at the end of
     * the struct so the positional initialiser in xlat12_ib_m2tri_profile keeps every other field's position. */
    uint32_t vs_abi_ptr1;
    /* 0.0.398 ( DESIGN item 1): WHICH IDENTITY the fragment stage bound here IS - an index into
     * xlat12_shader_ids.h, or XLAT12_PS_ID_NONE when none has been resolved. The FILL-COLOUR RETARGET
     * (XLAT12_EXTRA_FILL_COLOR) rewrites the fill's SPI_SHADER_USER_DATA_PS_2/_3 pair only when this is ws_B_ColorFill.
     * Filled for the fragment stage by xlat12_ib_profile_stage / xlat12_ib_profile_for (which is how the kext's
     * resolver fills it); CLEARED by d_pair_note before every fragment resolve so one program's identity can never
     * leak into the next; READ ONLY under XLAT12_EXTRA_FILL_COLOR, and NONE means "do not retarget". APPENDED, so the
     * positional initialiser in xlat12_ib_m2tri_profile keeps every other field's position. */
    uint32_t ps_id;
    /* D4' (an internal design note item 2): THE xlat12_readset.h ROW of the identity bound HERE - index + 1 into
     * kXlat12Readset, 0 = no row (UNKNOWN, never "declares none"). ps_readset1 for the fragment stage, vs_readset1
     * for the vertex stage; filled by xlat12_ib_profile_stage/_for beside ps_table_abi1/vs_abi_ptr1, cleared exactly
     * as they are, READ ONLY under XLAT12_EXTRA_READSET. APPENDED at the very end, so the positional initialiser in
     * xlat12_ib_m2tri_profile keeps every other field's position. */
    uint32_t ps_readset1, vs_readset1;
} xlat12_draw_profile;

/* 0.0.390 ( part 5 (i)): image inputs of ONE draw the exported list can carry. The class-19 table ABIs bind at
 * most TWO today (kDTableAbi's `ntex`), and arm13's plane frame binds exactly two; 4 is headroom, and a draw that binds
 * more sets `in_over`, which the kext's rule REFUSES on rather than reading a truncated list as a complete one. */
#define XLAT12_DRAW_IN_MAX 4u

/* build 0.0.447 (S5-COVERAGE-PART2.md Q1) — the class-11 sampler-index-table shape's own cap: AI's own two
 * entries (one per texture, entry i at samptbl_va + 8*i), matching DTableAbi's `tex[2]` / `ntex` cap. A DTableAbi
 * row with `nsamp` over this is refused as malformed (XLAT12_TDESC_TOO_MANY) rather than silently truncated. */
#define XLAT12_TDESC_TABLE_MAX_SAMP 2u

/* D4' (an internal design note item 4/G): the ACCUMULATED read-set's own storage caps - the segment-wide union over
 * every draw can legitimately be larger than one draw's XLAT12_DRAW_IN_MAX/XLAT12_ABI_PTR_MAX (a multi-draw segment
 * binding several distinct readset-covered programs). These are xlat12's own headroom; the kext's own caps
 * (gfx_cp_build.h's N48_CP_D4_IN_MAX/N48_CP_D4_PTR_MAX) are the ones the RULE enforces and match these exactly. */
#define XLAT12_RS_IN_MAX  16u
#define XLAT12_RS_PTR_MAX 64u

/* D7 (D4-PRIME-FIXES.md item 7,  (B)) — WHY `rs_declined` WAS SET, one bit per cause, ORed into
 * `xlat12_draw_stats.rs_decl_why`. The first (and, as of 0.0.441, only) cause: a DISPATCH_DIRECT/_INDIRECT or an
 * INDIRECT draw (DRAW_INDIRECT/DRAW_INDEX_INDIRECT) under XLAT12_EXTRA_READSET - none of the four name their real
 * read-set statically (a dispatch's or an indirect draw's true inputs are computed GPU-side), so the whole segment's
 * read-set is declined rather than silently omitting them. Printed on the kext's `d4:` report line. */
#define XLAT12_RS_WHY_DISPATCH 0x1u
/* D4-PRIME-FIXES.md item 3(c),  — AN INLINE T#/S# RECORD A PRIOR DRAW OF THIS SEGMENT ALREADY CONVERTED
 * TO A GFX12 RECORD (`DPair.ud_done` has a bit set in the row's [inl_tex, inl_tex+8) range). d_readset_accum's
 * inline-image read runs BEFORE d_inline_desc on THIS draw (it needs the raw gfx10 bytes), but a record two draws
 * SHARE (neither draw rewrites the slots between them) keeps whatever an EARLIER draw's d_inline_desc already wrote
 * there - gfx12 bytes, not gfx10 - so reading it as a gfx10 T# on this draw would decode garbage instead of the
 * record xlat12_table_img_desc expects. Declining under its own name, rather than folding it into a generic
 * decline, lets a run tell "this program has no row" apart from "this program's own record was already rewritten". */
#define XLAT12_RS_WHY_CONVERTED 0x2u

typedef struct {
    uint32_t regs_in, regs_emitted, dropped_absent, dropped_unknown_zero, dropped_legacy, rehomed, overridden;
    uint32_t dropped_reused;   /* 0.0.306: gfx12 has a DIFFERENT register, or none, at that gfx10.3 address */
    uint32_t synth_blocks, prefetch_nopped, ctxctl_dropped, draw_in_at, draw_out_len, pad_dwords;
    /*: `pad_dwords` is the SUM of every pad run - one before each draw plus the tail. An END-OF-PIPE packet may
     * only use the TAIL run (a RELEASE_MEM placed before a draw would signal completion of work that has not been
     * issued), so the tail run is counted APART. It is exactly the `n - e.n` the tail region NOP-fills, i.e. dwords
     * this translation wrote itself; it is never an input dword that happened to be a NOP. 0 = no tail pad at all. */
    uint32_t tail_pad_dwords;
    uint32_t draws;               /* 0.0.307: draws in the stream; the layout keeps each at its own index */
    uint32_t pairs;               /* 0.0.307: program changes the resolver was asked about */
    uint64_t pair_vs_va, pair_ps_va;  /* the address it refused, when the status is XLAT12_IB_ERR_PAIR */
    uint32_t ctxctl_neutralised;  /* 0.0.306: a non-proven CONTEXT_CONTROL replaced by load-nothing/shadow-nothing */
    uint32_t memloaded_dropped;   /* 0.0.306: a LOAD_CONTEXT_REG whose every register is absent on gfx12 */
    uint32_t vs_pgm_lo, vs_pgm_hi, ps_pgm_lo, ps_pgm_hi, vs_user_sgpr, ps_user_sgpr;
    uint32_t err_op, err_reg, err_in_dword;
    uint32_t ring_dwords;        /* 0.0.217: dwords of the extra block (ring/CU + 0.0.218 preamble) before the NOP pad (0 = none) */
    uint32_t preamble_dwords;    /* 0.0.218: of which the gfx12 preamble delta */
    uint32_t gs_sgpr0_dwords;    /* 0.0.219: dwords of the SPI_SHADER_PGM_LO/HI_GS packet in the extra block (4, or 0 without a ring base) */
    /*: which of RADV's THREE gfx12 ring-offsets destinations this translation actually wrote, as a bitmask -
     * 1 = PS (SPI_SHADER_USER_DATA_PS_0/1, 0xb030), 2 = HS (SPI_SHADER_PGM_LO/HI_HS, 0xb410) written, 4 = GS (0xb210).
     * GS is appended to the extra block, so it is set whenever there is a ring base. PS and HS are written IN PLACE over
     * the stream's own writes and are set only when the stream wrote that pair - see d_rings for why nothing is appended.
     * fix: 8 = HS write REFUSED - the stream wrote that pair, but the dwords Apple left there were already
     * non-zero (its own HS program address, xlat12_tables.h:124,126), so d_rings' zero-check left them alone. Read
     * together: 0x4 = HS pair never in the stream, 0x6 = HS pair was zero and we wrote it, 0xC = HS pair was
     * non-zero and the write was refused. */
    uint32_t ring_ptr_stages;
    uint32_t raster_dwords;      /* 0.0.220: dwords of the raster/CB delta in the extra block (12, or 0) */
    /* 0.0.389 (notes 880 H6, 881): the gfx10.3 addresses gfx12 REUSES for a different register (xlat12_reused.h).
     * `reused_regs` = writes re-emitted at the same address as the gfx12 register that really lives there;
     * `reused_nonzero` = of those, how many carried a NON-ZERO gfx10.3 value (Apple's compositor writes 0 to both,
     * so a non-zero here means the value mapping threw a real streamout request away and must be read);
     * `hisz_control` = PA_SC_HISZ_CONTROL packets synthesized beside them (see d_hisz_follow). */
    uint32_t reused_regs, reused_nonzero, hisz_control;
    uint32_t raster_follow;      /* 0.0.220: follow-on writes made (VIEW2, FDCC_CONTROL; must be 2 with XLAT12_EXTRA_RASTER) */
    uint32_t interp_checked;     /* 0.0.313: draws the interpolation guard positively cleared */
    uint32_t synth_idxprim;      /*: synthesis blocks that also carried SPI_SHADER_IDX_FORMAT + VGT_GS_OUT_PRIM_TYPE */
    uint32_t raster_follow_view, raster_follow_attrib;   /*: raster_follow split by kind (VIEW2 after VIEW, FDCC after ATTRIB) */
    uint32_t inline_draws;       /*: draws whose fragment program's inline records were checked (XLAT12_EXTRA_INLINE_DESC) */
    uint32_t inline_img, inline_samp;   /*: records translated in place (each once, however many draws share it) */
    uint32_t inline_iter256;     /*: of inline_img, records whose ITERATE_256 was dropped under xlat12_iterate256_droppable */
    /* M4-DESC-TABLE-IMPL (XLAT12_EXTRA_TABLE_DESC): draws whose table records were gathered, translated and placed; records
     * translated; of the T#, those whose LLC_NOALLOC / ITERATE_256 was dropped, those over a LINEAR surface whose producer the
     * caller could not prove (counted, not refused); dwords of pad turned into NOP bodies carrying records */
    uint32_t table_draws, table_img, table_samp, table_llc, table_iter256, table_lin_unproven, table_dwords;
    uint32_t table_inv;          /* M4-DESC-KEXT-PORT (XLAT12_EXTRA_DESC_INV): cache invalidates emitted (at most one per translation) */
    /*: WHICH SURFACE the PROVENANCE refusal names. hp7 counted 69 such refusals and logged no address at all, so whether
     * F3's own texture (0x400006000) was among them could only be SUSPECTED. Written ONLY when err_op is
     * XLAT12_TDESC_PROVENANCE: prov_va = the gfx10 record's BASE_ADDRESS, prov_mode = the TRANSLATED gfx12 SW_MODE the caller
     * was asked to prove, prov_path = 1 inline record / 2 class-19 table record. 0 otherwise. */
    uint64_t prov_va;
    uint32_t prov_mode, prov_path;
    /* 0.0.390 ( part 5 (i)) — **THE CONSUMER'S INPUT LIST.** Until now the ONLY thing that survived d_table_desc
     * was `prov_va` - the ONE surface the FIRST provenance refusal named - so a caller could never ask "what does this
     * frame read", only "which read refused first".'s rule is per-consumer positive provenance, and that needs the
     * WHOLE list: every image input the class-19 table step gathered, its translated gfx12 SW_MODE (0 = linear), whether
     * the caller's desc_tiled_ok POSITIVELY proved it, and the two heap addresses the records were read through.
     * WRITTEN ON EVERY TABLE-PATH DRAW, refused or not, and CLEARED at the head of each d_table_desc so one draw's list
     * never leaks into the next. NOTHING in the translator reads them: they are an export, and the output dwords, the
     * return codes and every existing counter are byte for byte what they were at 0.0.389.
     * `in_over` 1 = the draw bound more inputs than this list can carry, so the list is INCOMPLETE and the kext's rule
     * refuses on it (an incomplete read-set fails OPEN in exactly the hazard's direction - gfx_dep.h's own words). */
    uint64_t in_va[XLAT12_DRAW_IN_MAX];
    uint32_t in_mode[XLAT12_DRAW_IN_MAX];
    uint32_t in_proven[XLAT12_DRAW_IN_MAX];
    /* build 0.0.553: APPLE'S IMAGE-TABLE INDEX of each input, exactly the `aidx[i]` whose record
     * (image heap + sext32(idx << 5)) gave in_va[i]. GPUPass names its LUT per draw (its ABI: s6 = combinedLUT index, the
     * row's tex[1]), and on hardware-cursor boots that index is 3, not the fixed 4 the kext's LUT learn read. EXPORT ONLY:
     * written beside in_va and cleared with it (d_in_clear); nothing in the translator reads it, and no output dword, return
     * code or other counter depends on it. */
    uint32_t in_idx[XLAT12_DRAW_IN_MAX];
    /* build 0.0.554 (an internal design note section 3): bit i set = input i's tiled T# was NOT proven by
     * the caller's asks and was ADMITTED by xlat12_draw_extra.desc_stale_ok (its in_proven[i] is then 1). Written beside in_proven
     * and cleared with it (d_in_clear). EXPORT ONLY: no rung of the translator reads it. 0 whenever desc_stale_ok is NULL. */
    uint32_t in_admit;
    /* build 0.0.554: cumulative per translation (like dcc_stripped): `stale_asked` = desc_stale_ok calls, `stale_admitted` =
     * of those the callback answered 1, `stale_dcc` = DCC-stripped records the asks refused that were NEVER offered to the
     * callback (a DCC record is not admitted, ever). All 0 while desc_stale_ok is NULL. */
    uint32_t stale_asked, stale_admitted, stale_dcc;
    uint32_t in_n, in_over;
    uint64_t in_tbl_va;     /* the class-19 descriptor table the fragment program's s0:s1 names */
    uint64_t in_img_va;     /* the image heap base read from table +0x00 */
    uint64_t in_samp_va;    /* the S# actually read: sampler heap base + sext32(idx << 4) */
    uint32_t in_abi;        /* which table ABI row was in force (ps_table_abi1); 0 = the table step did not run */
    /* 0.0.390 ( part 5 (ii)): the RAW-POINTER pages this program's ABI declares, resolved from the stream's own
     * user-data writes through xlat12_abi_ptrs.h's rows. `in_ptr_known` 0 means the program has NO ABI row, which is
     * UNKNOWN and must refuse - it is never "this program declares no pointers". */
    uint64_t in_ptr[XLAT12_ABI_PTR_MAX];
    uint32_t in_nptr, in_ptr_known;
    /* 0.0.391 — A DECLARED POINTER SLOT THIS STREAM DID NOT WRITE IS **NOT** A LIST THAT DID NOT
     * FIT, and through 0.0.390 both set `in_over` and both came out of the kext's rule as `list-overflow`. Measured on the
     * real committing frame (tests/fixture_arm16_ws.h, xlat12_ib_find_set over SPI_SHADER_USER_DATA_PS_0+4k): the stream
     * writes PS slots 0,1,2,3 and NOTHING else, so GPUPass's declared s10:s11 and s12:s13 are inherited from whatever an
     * earlier submission left in those SGPRs - a value this translation never saw and cannot name. That is UNKNOWN, it
     * still refuses, and it now refuses under its OWN name so a run can tell the two apart in one log line.
     * COUNTED, not a flag, so the read-out can say HOW MANY slots were inherited. Both causes land here: a slot whose
     * user-data write this stream never made at all, and one written only BEFORE this region (shared with an earlier
     * draw, which the table slots themselves already refuse as XLAT12_TDESC_SLOT_SHARED). A slot INDEX out of range is a
     * malformed ABI row, not an inherited value, and stays `in_over`. */
    uint32_t in_ptr_inherit;
    /* 0.0.391 — THE VERTEX STAGE'S DECLARED POINTER PAGES. found xlat12_abi_ptrs.h's three
     * VERTEX rows reachable by nothing: the only lookup ran off a kDTableAbi row, which is fragment-only, so
     * ViewportToNDC's vertexArray / texCoords / mvpMatrix pointers were carried by the table and asked by no rule.
     * They are gathered here now, from the stream's own SPI_SHADER_USER_DATA_VS_0+4k writes, in their own array so that
     * neither stage's list can push the other's out of one shared four-entry one. `in_vptr_known` 0 = the vertex program
     * has NO ABI row (UNKNOWN, refuse); inherited vertex slots are counted into `in_ptr_inherit` beside the fragment's. */
    uint64_t in_vptr[XLAT12_ABI_PTR_MAX];
    uint32_t in_nvptr, in_vptr_known;
    /* 0.0.390 — THE CONSUMER'S OWN WAITS AND MEMORY-DESTINATION WRITES, counted as the regions of
     * this translation are walked.'s census over 266 captured IBs found ZERO of either in the 1040/1472-dw plane
     * shape, so R4 is vacuous for it - and it is a RUNG anyway, because names the hazard it stands in for:
     * `operand_ok` passes a memory-space WRITE_DATA / WAIT_REG_MEM **without resolving the destination page**, so
     * committing an IB that carries one is a GPU write to an unverified client VA.
     * NOT counted: a packet inside a NOP body (the region walker copies a NOP whole, so the plane frame's NOP-wrapped
     * dead-page RELEASE_MEM never reaches the count - which is exactly R4's "except the NOP-wrapped dead fence"); and
     * DMA_DATA, because a DMA_DATA that is not the dst-nowhere prefetch form already REFUSES the translation
     * (XLAT12_IB_ERR_UNLISTED), so a writing one can never be in a committed segment. */
    uint32_t r4_waits;        /* WAIT_REG_MEM packets */
    uint32_t r4_memwrites;    /* WRITE_DATA + COPY_DATA + RELEASE_MEM packets */
    /* 0.0.398 ( DESIGN items 1-2) — THE FILL-COLOUR RETARGET, as a reading. `fill_color_retargeted`
     * counts draws whose ws_B_ColorFill SPI_SHADER_USER_DATA_PS_2/_3 pair this translation rewrote;
     * `fill_color_refused` counts the fail-closed cases (the pair was not written by THIS translation, or it did not
     * reconstruct to a 64-bit VA in WindowServer's range); old / new are the last rewrite's old and new VAs. ALL ZERO
     * unless XLAT12_EXTRA_FILL_COLOR is set: the counters are bumped only inside d_fill_color. APPENDED. */
    uint32_t fill_color_retargeted, fill_color_refused;
    uint64_t fill_color_old, fill_color_new;
    /* 0.0.409 — THE PAIR PRE-RESOLVE, as a reading. Both are zero unless XLAT12_EXTRA_PAIR_PRE is set:
     * d_pair_pre() is the only writer. `pair_pre_resolved` counts STAGES whose profile the pre-scan resolved through
     * ex->pgm_profile (0..2 per pre-scan); `pair_pre_unresolved` counts PRE-SCANS that fell back to the base profile
     * because a resolver call refused or the relocation address was structurally invalid. A stage whose PGM_LO write
     * is absent or zero is neither resolved nor counted: it simply keeps the base value. APPENDED. */
    uint32_t pair_pre_resolved, pair_pre_unresolved;
    /* D4' (an internal design note item 3, an internal design note Q5/Q6) — THE PER-DRAW READ-SET, ACCUMULATED
     * OVER EVERY DRAW OF THE SEGMENT, NEVER CLEARED PER DRAW (contrast in_*, which d_in_clear empties at each draw -
     * the fail-open Finding 4 named). Written ONLY under XLAT12_EXTRA_READSET; all zero on every default build.
     * `rs_declined` 1 = at least one draw's PS or VS program had no admitted xlat12_readset.h row, so the caller must
     * treat the WHOLE segment's read-set as INCOMPLETE (the same direction `in_over` already fails in). `rs_over` 1 =
     * a list did not fit this header's own storage (XLAT12_RS_IN_MAX/PTR_MAX). `rs_va`/`rs_mode`/`rs_proven` are
     * ADMITTED INLINE IMAGE inputs (Tex_PS's T# at s0, R1-R4 admits them exactly like a table-path image). `rs_ptr`
     * is every raw pointer page an admitted row declares, PLUS every INDEX_BASE client VA the stream carries
     * (R1-MEMDST.md Q6: INDEX_BASE is already a recognised, cheaply-decoded pass-through opcode - body[0]/[1] are the
     * index buffer's client VA - so this names it as a buffer input rather than leaving the gap unenumerated).
     * Appended, never deduped here: the kext's n48_cp_merge_dedup dedupes by page. `r4_waits`/`r4_memwrites` (above)
     * are ALREADY accumulated over the whole segment regardless of this flag (d_region never resets them per draw),
     * so the D4' consumer builder carries them unchanged - this is the R1-MEMDST.md Q5 binding. */
    uint32_t rs_declined, rs_over;
    uint32_t rs_decl_why;   /* D7: one bit per rs_declined cause (XLAT12_RS_WHY_*), ORed, never cleared per draw */
    uint32_t rs_n;
    uint64_t rs_va[XLAT12_RS_IN_MAX];
    uint32_t rs_mode[XLAT12_RS_IN_MAX], rs_proven[XLAT12_RS_IN_MAX];
    uint32_t rs_nptr;
    uint64_t rs_ptr[XLAT12_RS_PTR_MAX];
    /* build 0.0.448 item 1 (XLAT12_EXTRA_UD_REEMIT) — the count of PS/VS user-data slots this translation
     * re-emitted to cure a SLOT_SHARED table draw. Zero unless the flag is set. APPENDED. */
    uint32_t ud_reemit_n;
    /* build 0.0.454 item 1 (XLAT12_EXTRA_TABLE_REUSE, switch 48): table draws that kept an earlier shadow of
     * this translation instead of placing a new one, and the T#/S# records appended to a shadow's spare space for
     * them. Zero unless the flag is set. APPENDED. */
    uint32_t table_reused, table_appended;
    /* build 0.0.455 item 1 (XLAT12_EXTRA_VS_KNOWN, switch 52): the count of VERTEX user-data slots this
     * translation admitted as known-by-carry (d_vs_slot_known) instead of declining them as inherited. Zero
     * unless the flag is set. APPENDED. */
    uint32_t vs_known_n;
    /* build 0.0.488 (XLAT12_EXTRA_DCC_STRIP, switch 60): table T# records stripped and translated; of them, those
     * desc_dcc_ok did not prove (the draw refused PROVENANCE); and DCC records (xlat12_desc_has_dcc) the step refused -
     * outside the accept shape, or refused by the port after the strip. Zero unless the flag is set. APPENDED. */
    uint32_t dcc_stripped, dcc_unproven, dcc_refused;
    /* build 0.0.487 (XLAT12_EXTRA_CS_ELIDE, switch 57; an internal design note Q6/Q7): DISPATCH_DIRECT packets the
     * elision branch examined (`cs_seen`), replaced by a same-length NOP (`cs_elided`), and refused, by the FIRST predicate
     * part that failed (P2 state, P3 packet, P4 program, P5 DCC bind; a refused one then falls through to UNLISTED exactly
     * as with the flag off). All zero unless the flag is set. APPENDED. */
    uint32_t cs_seen, cs_elided, cs_ref_p2, cs_ref_p3, cs_ref_p4, cs_ref_p5;
    /* build 0.0.499 (xlat12_scissor.h): scissor BOTTOM-RIGHT writes d_reg emitted with BR_X/BR_Y -1
     * (gfx12's BR is inclusive), and those it REFUSED XLAT12_ERR_SCISSOR (a BR component of 0). No flag: always on.
     * APPENDED. */
    uint32_t scissor_adjusted, scissor_refused;
    /* build 0.0.500 (XLAT12_EXTRA_DRAW_ELIDE, switch 66; an internal design note Q4): draws replaced by a same-length
     * NOP (`draw_elided`, never above XLAT12_DRAW_ELIDE_MAX), the PROVENANCE refusals the elide examined (`de_seen`), and
     * those it left refused, by the FIRST predicate part that failed: not on the row's last texture with every earlier one
     * proven (`de_not_last`), the in-force fragment program not an enabled row (`de_not_row`: `de_nr_ndw`/`de_nr_fnv` name
     * it, the usable-OS census), a row that requires a stripped DCC texture refused on another (`de_no_dcc`), the write set
     * (`de_writeset`: depth, stencil, streamout, a bound CB1-7 or a register written from memory) and the cap (`de_cap`).
     * Per elision q < draw_elided: its input dword `de_at` (relative to this translation's `in`), the row (`de_row`:
     * XLAT12_DE_ROW_U / _Y / _AO), and the surface it could not prove (`de_va8` = its VA >> 8 - a T# surface is 256-byte
     * aligned and 40-bit, so this is exact - and `de_mode`). `de_dcc0` is the translator's own scratch (dcc_unproven before
     * the table step). The refusal counters are bytes: a translation stops at its first refusal, so each is 0 or 1. PACKED
     * (48 bytes): every caller's stack-local xlat12_draw_stats (the render verbs' render_draw_write / render_suite_write)
     * grows by at most 0x30. All zero unless the flag is set. APPENDED. */
    uint32_t draw_elided, de_seen, de_dcc0, de_nr_fnv;
    uint32_t de_at[2], de_va8[2];
    uint16_t de_nr_ndw;
    uint8_t de_not_last, de_not_row, de_no_dcc, de_writeset, de_cap, de_row[2], de_mode[2];
    /* build 0.0.512 (XLAT12_DE_CLASS_GLASS, the NOT-LAST case): the table step's answer for the draw it just refused, set
     * ONLY under the flag with the GLASS class, only for a row with a not-last texture (kDrawElideRows `nl_tex`), only when the
     * refusal is PROVENANCE on exactly that texture: bits [3:0] = that texture's index + 1, bit 4 = every LATER texture of the
     * row was read, translated and PROVEN by the same ask (d_de_probe_rest). 0 otherwise - and reset before every table step.
     * Takes the struct's former tail padding byte: sizeof is unchanged (the render verbs' stack-local stats do not grow). */
    uint8_t de_nl;
    /* build 0.0.535 (XLAT12_EXTRA_NCLEAR, switch 91;  fix 1): proven N dispatches whose elision was replaced by a
     * CP DMA_DATA zero fill of their colour target (`nc_filled`, never above XLAT12_NCLEAR_MAX recorded), the fill bytes and
     * packets in total, per fill its VA >> 8 (`nc_va8`: a colour target is 256-byte aligned and below 2^48) and length, and the
     * dispatches that FELL BACK to 57's plain NOP, by the first reason: no provable extent (`nc_ref_extent`), no room in N's
     * own dwords (`nc_ref_room`), no provable compute run to reuse (`nc_ref_run`), Apple's barriers around N not both present
     * (`nc_ref_barrier`, fix round (a)), XLAT12_NCLEAR_MAX fills already made (`nc_ref_max`, fix round (b)). All zero unless the
     * flag is set. APPENDED (36 bytes: every caller's stack-local stats grows by at most 0x28). */
    uint32_t nc_filled, nc_bytes, nc_pkts;
    uint32_t nc_va8[2], nc_len[2];
    uint8_t nc_ref_extent, nc_ref_room, nc_ref_run, nc_ref_barrier, nc_ref_max;
    /* build 0.0.536 (XLAT12_EXTRA_RECT2D, switch 92;  defect 2): draws whose VGT_PRIMITIVE_TYPE in force (written in
     * this translation) is RECTLIST (`r2d_rect`), VGT_GS_OUT_PRIM_TYPE = RECT_2D writes placed before such a draw (`r2d_written`),
     * profile-value writes placed before a non-RECTLIST draw that followed one or saw no write of its own (`r2d_restored`), a
     * write that found no room (`r2d_noroom`), and - when the flagged translation failed after it wrote or lacked room - the
     * fallback to today's output (`r2d_fallback` 1, `r2d_on_st` the flagged pass's status; `r2d_rect` and `r2d_noroom` are the
     * flagged pass's, the rest the fallback's). All zero unless the flag is set. APPENDED: 6 bytes. */
    uint8_t r2d_rect, r2d_written, r2d_restored, r2d_noroom, r2d_fallback, r2d_on_st;
    /* build 0.0.537 (XLAT12_EXTRA_PWS, switch 93; ): Apple's CB/DB barrier ACQUIRE_MEM (exactly XLAT12_PWS_APPLE_*)
     * met by this translation (`pws_seen`), converted into our RELEASE_MEM(PWS) + ACQUIRE_MEM(PWS) pair (`pws_conv`), copied
     * verbatim for want of room (`pws_noroom`: no 16 dwords at the barrier, the region after the last draw - never room, R1's
     * fence identity - or every barrier of a fallback pass), of them the ones in the region after the last draw (`pws_tail`), and
     * ACQUIRE_MEM packets of any other shape passed through unchanged (`pws_other`: the compute barriers a8c40000 / 80c40000,
     * 86007fc0, anything else). `pws_fallback` 1 when the flagged pass was refused after it converted and the translation was run
     * again without the conversion (today's output, byte for byte); `pws_on_st` that flagged pass's status. All zero unless the
     * flag is set. APPENDED: 12 bytes. */
    uint16_t pws_seen, pws_conv, pws_noroom, pws_tail, pws_other;
    uint8_t pws_fallback, pws_on_st;
    /* fix round item 3: of `pws_noroom`, the barriers left verbatim because Apple's buried fence828 packet follows in the same region
     * (converting would move it off R1's fence identity). APPENDED: 2 bytes. */
    uint16_t pws_fence;
    /* build 0.0.539 ( build spec; XLAT12_PWS_SLOT_*): of `pws_conv`, the converted barriers whose Apple top-of-pipe
     * wait slot (the disabled 8-dword NOP right after the barrier's EVENT_WRITE 0xE) was DROPPED, so the pair took no pad. Counted in
     * d_region's NOP branch when it meets exactly the marked slot; 0 in a fallback pass and with the flag OFF. APPENDED: 2 bytes. */
    uint16_t pws_slot;
    /* build 0.0.547 item 3: WHY the XLAT12_EXTRA_RECT2D flagged pass was refused, kept across its fallback (set
     * only with r2d_fallback 1): that pass's err_op, err_reg and err_in_dword (XLAT12_R2D_BACKSTOP 0xD3 = the end rule / a RECT_2D
     * leak, XLAT12_R2D_NO_ROOM 0xD2, otherwise the first refusal's own code). All zero unless the flag is set and it fell back.
     * APPENDED: 12 bytes. */
    uint32_t r2d_on_op, r2d_on_reg, r2d_on_dw;
    /* build 0.0.552 (XLAT12_DE_CLASS_AN / _AN_SHADOW, switch 110): draws whose in-force fragment program was AN's identity
     * while either bit was set (`de_an_seen`), of them the ones the predicate answered yes for under SHADOW (`de_an_would`), and
     * the ones it refused by the first failing part: the write set (`de_an_ref_ws`), the cap (`de_an_ref_cap`). Saturating at 255.
     * All zero unless a bit is set. APPENDED into the struct's former tail padding: sizeof is unchanged. */
    uint8_t de_an_seen, de_an_would, de_an_ref_ws, de_an_ref_cap;
} xlat12_draw_stats;
/* build 0.0.535 item 4 (: tex531 printed the frame's FIRST gathered target, not the draw's). THE DRAW's OWN COLOUR
 * TARGET 0 AND SCISSORS, as THIS translation's own SET_CONTEXT_REG writes left them at the moment the T# observer is called
 * (xlat12_draw_extra.tex_state). `seen` bit k = register k below was written in this translation (0: inherited, unknown):
 * 0 CB_COLOR0_BASE (0x28c60), 1 CB_COLOR0_BASE_EXT (0x28e40), 2 PA_SC_WINDOW_SCISSOR_TL (0x28204), 3 _BR (0x28208),
 * 4 PA_SC_GENERIC_SCISSOR_TL (0x28240), 5 _BR (0x28244), 6 PA_SC_VPORT_SCISSOR_0_TL (0x28250), 7 _BR (0x28254). Raw gfx10
 * values. Read-only instrumentation: filled only when the pointer is non-NULL, never read by the translator. */
typedef struct { uint32_t seen, cb0, cb0_ext, win_tl, win_br, gen_tl, gen_br, vp_tl, vp_br; } xlat12_tex_state;

/* ---- 0.0.217 (review report 9): the gfx12 NGG ring state and the GS/ES CU mask ------------------------------
 * A gfx10 stream never programs them and the kernel does not either; every gfx12 driver does before a draw (Mesa
 * ac_emit_cp_gfx11_ge_rings, ac_cmdbuf_cp.c:294-332; radeonsi si_state_shaders.cpp:4585; RADV radv_queue.c:626). Mesa's
 * Navi48 sizes (ac_gpu_info.c:1710-1758, 4 SEs): attribute 0x160000 per SE -> 0x580000; position 16384 x 16 B per SE, region
 * align(0x40000 x 16, 64 KiB) = 0x400000; primitive 16368 x 4 B per SE, region 0x100000; one mapping of 0xA80000 bytes:
 *   SPI_GS_THROTTLE_CNTL1/2 0x31110/4 = 0x12355123 / 0x0001544D, SPI_ATTRIBUTE_RING_BASE/SIZE 0x31118/c = base>>16 /
 *   MEM_SIZE 0x15 | L1_POLICY (0x00020015); GE_POS_RING_BASE/SIZE 0x309a0/4 = (base+0x580000)>>16 / 0x2000;
 *   GE_PRIM_RING_BASE/SIZE 0x309a8/c = (base+0x980000)>>16 / 0x0C6E07FE; SPI_SHADER_PGM_RSRC3_GS 0xb21c = 0xfffffdfd
 *   (ac_cmdbuf.c:672-673). Emitted as SET_UCONFIG_REG x2 (count 4, header 0xC0047900) + SET_SH_REG (count 1): 15 dwords. */
#define XLAT12_GE_RING_TOTAL    0x00A80000u
#define XLAT12_GE_RING_POS_OFF  0x00580000u
#define XLAT12_GE_RING_PRIM_OFF 0x00980000u
#define XLAT12_GE_RING_ALIGN    0x00010000u
#define XLAT12_RSRC3_GS_CU_EN   0xFFFFFDFDu
/* 0.0.218 (review report 9 experiment B): the gfx12 preamble delta, radeonsi's gfx12_init_graphics_preamble_state
 * (ac_cmdbuf.c:660-846) for every register the translated stream leaves undefined, values from gfx12.json field positions:
 *   SET_SH_REG      SPI_SHADER_REQ_CTRL_PS 0xb0c0 = 0x7; SPI_SHADER_USER_ACCUM_PS_0-3 0xb0c8-d4, _ESGS_0-3 0xb2c8-d4, _LSHS_0-3
 *                   0xb4c8-d4 = 0 (r51 read garbage 7-bit CONTRIBUTIONs in all twelve)
 *   SET_CONTEXT_REG PA_SU_PRIM_FILTER_CNTL 0x2882c = 0; GE_SE/IA/WD_ENHANCE 0x28a50/70/80 = 0; PA_SC_BINNER_CNTL_2 0x28c4c = 0x00800000
 *   SET_UCONFIG_REG GE_GS_THROTTLE 0x30950 = 0x7F9A80E1; GE_USER_VGPR_EN 0x30980 = 0; SPI_GRP_LAUNCH_GUARANTEE_ENABLE/CTRL
 *                   0x31128/c = 0x8A4D / 0x00401123
 * 46 dwords, emitted after the ring/CU block.
 * 0.0.219 (review report 10): with a ring base the block also carries SET_SH_REG SPI_SHADER_PGM_LO_GS 0xb210 = 0 and
 * SPI_SHADER_PGM_HI_GS 0xb214 = 0 (gc_12_0_0_offset.h regSPI_SHADER_PGM_LO_GS 0x1a24 / _HI_GS 0x1a25, BASE_IDX 0, + GC base 0x1260
 * = dw 0x2c84 / 0x2c85): the merged GS stage loads its s0:s1 (ring_offsets) from that pair (radv_queue.c:563-566,
 * si_descriptors.c:2871) and no translated gfx10 stream writes it. One packet, count 2, after the RSRC3_GS write:
 * C0027600 00000084 00000000 00000000; the ring + CU block is 19 dwords. Flag 0x2 (0.0.218 XLAT12_EXTRA_VS_USER_SGPR, the
 * record's USER_SGPR 6) is RETIRED: its premise, user data in front of the system SGPRs, was wrong, and
 * xlat12_ib_translate_draw_ex refuses every flag but 0x1 and 0x4 (XLAT12_ERR_ARG). */
#define XLAT12_EXTRA_PREAMBLE      0x1u
#define XLAT12_PREAMBLE_DWORDS     46u
#define XLAT12_GS_SGPR0_DWORDS     4u
/* 0.0.220: XLAT12_EXTRA_RASTER - gfx12 colour-buffer and rasterizer words the translated gfx10 stream leaves
 * undefined (r53's after-run sweep read power-on values in each), with radeonsi's / ac's gfx12 value:
 *   follow-on writes, merged into the stream's own packet right after its write (1 dword each):
 *     after CB_COLOR0_VIEW   (gfx10 0x28c6c -> gfx12 0x28c64): CB_COLOR0_VIEW2 0x28c68 = MIP_LEVEL of gfx10.3 CB_COLOR0_VIEW [29:26]
 *                            (gfx12 moved MIP_LEVEL to VIEW2 [4:0], ac_descriptors.c:1376; r53 read MIP_LEVEL 3)
 *     after CB_COLOR0_ATTRIB (gfx10 0x28c74 -> gfx12 0x28c6c): CB_COLOR0_FDCC_CONTROL 0x28c70 = 0x10000004
 *                            (ac_descriptors.c:1384-1388: MAX_UNCOMPRESSED_BLOCK_SIZE 256B | ENABLE_MAX_COMP_FRAG_OVERRIDE, one
 *                            sample, no DCC modifier; r53 read 0x5d000004)
 *   the extra block, after the preamble delta, 12 dwords: PA_SC_VRS_OVERRIDE_CNTL 0x283d0 = 0 (si_state.c:1710, VRS unused;
 *     r53 0xd3), PA_SC_VRS_INFO 0x283e0 = 0 (ac_cmdbuf.c:727; r53 0x26), PA_SC_HISZ_RENDER_OVERRIDE 0x28bc0 = 0 (ac_cmdbuf.c:773;
 *     r53 0x2f2), CB_MEM0_INFO 0x28f00 = 0x24 (ac_cmdbuf.c:785-787 with cache_cb_gl2 off: TEMPORAL_READ 4 | TEMPORAL_WRITE 4; r53 0).
 * A stream in which not exactly both follow-on writes happened is refused XLAT12_IB_ERR_DRAW_SHAPE with err_op 0xFE. */
#define XLAT12_EXTRA_RASTER        0x4u
#define XLAT12_RASTER_DWORDS       12u
/* 0.0.389 (notes 880 H6, 881): gfx12 PA_SC_HISZ_CONTROL, ctx 0x2ef -> 0x28000 + 0x2ef*4 = 0x28bbc
 * (ref/linux-asic-reg/gc_12_0_0_offset.h regPA_SC_HISZ_CONTROL 0x02ef BASE_IDX 1, mesa R_028BBC). Its value for a
 * colour-only pass is ROUND(2) with CONSERVATIVE_Z_EXPORT 0 - what BOTH mesa drivers give every gfx12 pixel
 * shader that declares no depth layout (radv_shader.c, radeonsi si_state_shaders.cpp; "required minimum value").
 * NO gfx10.3 register has this address, so it can only be synthesized: d_hisz_follow emits it beside the
 * PA_SC_HIS_INFO that xlat12_reused.h's row produces, and d_policy_g12 lists it for the output verifier. */
#define XLAT12_G12_PA_SC_HISZ_CONTROL 0x28bbcu
/* build 0.0.502: gfx12 DB_SPI_VRS_CENTER_LOCATION, ctx 0x01a -> 0x28000 + 0x1a*4 = 0x28068
 * (re/m2/linux/.../gc_12_0_0_offset.h:8157 `regDB_SPI_VRS_CENTER_LOCATION 0x001a`, BASE_IDX 1; mesa gfx12.json: eight 4-bit
 * fields CENTER_X/Y_OFFSET_1X1/2X1/1X2/2X2, the per-VRS-rate pixel-centre offsets in 1/16 px). mesa's gfx12 preamble writes
 * it 0 unconditionally: ac_cmdbuf.c:702 `ac_pm4_set_reg(pm4, R_028068_DB_SPI_VRS_CENTER_LOCATION, 0);`. Nothing in a
 * translated gfx10.3 stream reaches it (gfx10.3 0x28068 is DB_Z_READ_BASE_HI, moved to 0x28024; gfx10.3 0x28060 is a
 * DROPPED zero - no gfx10.3 source names it, only gfx11.5's), so until 0.0.502 it held whatever the part last left there:
 *'s per-16x16-region sub-pixel offsets in every interpolated attribute. Every translation that carries the ring
 * (ex->ring_va != 0, which every armed translation has; independent of switch 27, XLAT12_EXTRA_RASTER, off by default)
 * writes it 0 EXACTLY ONCE, in region 0, before its first draw, in one of two places:
 *   - in front of the stream's own region-0 gfx12 DB_SHADER_CONTROL (0x2806c, contiguous), merged into that packet:
 *     1 dword (d_vrs_prefollow) - the common case, and the cheap one, because tight units place deferred records in the
 *     same room (a 3-dword write refused decide44 F77/F54 units NO_ROOM);
 *   - otherwise as the LAST packet of d_rings' extra block: one SET_CONTEXT_REG of one value, XLAT12_VRS_CENTER_DWORDS. */
#define XLAT12_G12_DB_SPI_VRS_CENTER_LOCATION 0x28068u
#define XLAT12_VRS_CENTER_DWORDS              3u
#define XLAT12_PA_SC_HISZ_CONTROL     0x00000002u
#define XLAT12_CB0_FDCC_CONTROL    0x10000004u
#define XLAT12_CB_MEM0_INFO        0x00000024u
/* build 0.0.449 item 1 (F1, review of 0.0.448: "the only mutable file-scope static in src/xlat12", a
 * reentrancy race between the policy thread and any other caller that overlaps it) — MIB-A1-PATH.md design B1,
 * EXACTLY: Apple's recorded PS and VS user-data input values, CALLER-OWNED. `ps_val[k]` / `vs_val[k]` is the raw
 * gfx10 dword last seen at PS/VS user-data slot k THIS TRANSLATION (xlat12_ib_translate_draw_ex zeroes the whole
 * struct at the top of every call - the same "no cross-call carry" point DPair's own zero-init already is, now on
 * caller-owned memory instead of a stack-local DPair field or a file-scope static); `ps_ok` / `vs_ok` bit k = a
 * value was recorded for slot k in THIS translation. Read only under XLAT12_EXTRA_UD_REEMIT. Two interleaved
 * translations that each pass their OWN `xlat12_ud_carry` can never see each other's values - the fix F1 asks for. */
typedef struct { uint32_t ps_val[16], ps_ok; uint32_t vs_val[32], vs_ok; } xlat12_ud_carry;
typedef struct {
    uint64_t ring_va;   /* 0 = no ring packets; else the 64 KiB-aligned GPU VA of a mapping of >= XLAT12_GE_RING_TOTAL bytes */
    uint32_t rsrc3_gs;  /* 0 = not written; else the SPI_SHADER_PGM_RSRC3_GS value */
    uint32_t flags;     /* 0.0.218: XLAT12_EXTRA_* */
    uint64_t gs_sgpr0_va; /* 0.0.226: with a ring base, the merged stage's s[0:1] ring_offsets pointer = this VA,
                           * low / high 32 bits as RADV's radeon_emit_64bit_pointer (radv_queue.c:563-570); 0 = 0 as before.
                           *: aimed at ALL THREE gfx12 destinations RADV writes - SPI_SHADER_USER_DATA_PS_0/1 (0xb030),
                           * SPI_SHADER_PGM_LO/HI_HS (0xb410) and SPI_SHADER_PGM_LO/HI_GS (0xb210). GS is appended; PS and HS
                           * are written IN PLACE over the stream's own writes, because the extra block cannot grow (d_rings).
                           * `ring_ptr_stages` in the stats says which of the three this translation actually reached. */
    uint64_t vs_pgm_va;   /* 0.0.265: when non-zero, SPI_SHADER_PGM_LO/HI_ES name THIS address — a relocated vertex
                           * program in our OWN mapping — instead of re-homing Apple's value. Must be XLAT12_RELOC_ALIGN aligned
                           * (PGM_LO is VA >> 8) and below 2^48, or the translate refuses XLAT12_IB_ERR_RING. */
    /* 0.0.307: SEVERAL DRAWS, SEVERAL PROGRAMS. A real client segment binds a different vertex or fragment
     * program for different draws (a captured SecurityAgent segment has 9 draws and changes program within itself), and
     * ONE profile cannot serve them: the profile carries that program's own RSRC1/2_GS, or its RSRC1/2_PS and
     * SPI_PS_INPUT_ENA/ADDR. When `pgm_profile` is non-null it is called whenever a stage's program ADDRESS CHANGES -
     * with Apple's address exactly as the stream writes it - and must update that stage's fields in `out`, returning 1;
     * returning 0 REFUSES the translation (XLAT12_IB_ERR_PAIR) with the address in the stats.
     * PER STAGE, not per pair, because that is the order a stream writes them in: a run sets PGM_LO/HI for one stage and
     * then that same stage's RSRC words, so the fields are ready exactly when they are consumed. Waiting for both stages
     * would leave the first stage's RSRC writes using the wrong profile.
     * `out` starts as a copy of the profile passed to the translator, so a callee need only fill its own stage.
     * NULL keeps 0.0.306's behaviour EXACTLY: the one profile passed to the translator is used throughout. */
    void *pgm_ctx;
    int (*pgm_profile)(void *ctx, uint32_t stage, uint64_t va, xlat12_draw_profile *out);   /* stage: 1 vertex, 0 fragment */
    /* M4-DESC-TABLE-IMPL (XLAT12_EXTRA_TABLE_DESC only; ignored otherwise, and zero in every existing caller):
     *   ib_va          the GPU VA of in[0] - the translation is in place, so out[k] executes and is read at ib_va + 4k
     *   desc_read      a READ-ONLY snapshot of ndw dwords at a client VA, through the submitter's page table; 1 = all read
     *   desc_tiled_ok  1 only when the caller PROVES the surface at `va` was last written by a translated colour target in
     *                  gfx12 swizzle mode `g12_sw_mode`; NULL or 0 refuses every non-linear T# (M4-DESC-TABLE.md section 4) -
     *                  on the table path, and since M4-DESC-KEXT-PORT on the XLAT12_EXTRA_INLINE_DESC path as well.
     *                  build 0.0.451 item 2 (S4, review of 0.0.450): `elem_bytes` is THIS T#'s own bytes-per-
     *                  element, from its gfx10 FORMAT field through xlat12_desc.h's xlat12_format_elem_bytes (0 =
     *                  unknown format) - the caller compares it against what it recorded for `va`, because the
     *                  pitch-only shape check alone cannot tell two different real bpps apart at the same (mode, VA)
     *                  when the pitches happen to collide (a 128 bpp 4KB_D_X resource with w <= 16 also satisfies
     *                  the 64 bpp kind's pitch formula; a 16 bpp resource with w <= 32 satisfies the 32 bpp kind's) */
    uint64_t ib_va;
    void *desc_ctx;
    int (*desc_read)(void *ctx, uint64_t va, uint32_t ndw, uint32_t *out);
    int (*desc_tiled_ok)(void *ctx, uint64_t va, uint32_t g12_sw_mode, uint32_t elem_bytes);
    /* 0.0.398 ( DESIGN item 1): with XLAT12_EXTRA_FILL_COLOR, the GPU VA of a 16-byte float4 that the
     * fill's colour pointer (SPI_SHADER_USER_DATA_PS_2/_3) is retargeted to. The kext hands it in ONLY when
     * `accel gfxneuter 31` placed the buffer in the relocation arena, so it is a WindowServer VA in our own mapped
     * VRAM. Must be 16-byte aligned and below 2^48, else XLAT12_ERR_ARG. 0 with the flag off. APPENDED, so every
     * positional initialiser keeps its fields. */
    uint64_t fill_color_va;
    /* build 0.0.449 item 1 (F1): CALLER-OWNED storage for XLAT12_EXTRA_UD_REEMIT's carry (see
     * xlat12_ud_carry's own comment). Required (non-NULL) whenever the flag is set - XLAT12_ERR_ARG otherwise -
     * exactly like every other flag-conditional pointer this struct already validates (desc_read under
     * TABLE_DESC). NULL and the flag unset (every caller before this build task) is 0.0.448's behaviour, byte for
     * byte: nothing reads this field. APPENDED, so every positional initialiser keeps its fields. */
    xlat12_ud_carry *ud_carry;
    /* build 0.0.480 (XLAT12_EXTRA_UNIT, below): the unit's CALLER-OWNED state; required under the flag, read under
     * it only. APPENDED, so every positional initialiser keeps its fields. */
    struct xlat12_unit_s *unit;
    /* build 0.0.488 (XLAT12_EXTRA_DCC_STRIP): the ONLY proof a stripped DCC T# may have - 1 when the caller proves
     * the surface at `va` was written by a translated colour target (the kext: the producer ledger or the frame-local
     * list, NEVER a residency copy). Same arguments as desc_tiled_ok; NULL or anything but 1 refuses. Read only for a
     * stripped record. APPENDED, so every positional initialiser keeps its fields. */
    int (*desc_dcc_ok)(void *ctx, uint64_t va, uint32_t g12_sw_mode, uint32_t elem_bytes);
    /* build 0.0.487 (XLAT12_EXTRA_CS_ELIDE, below): predicate part P4. Called ONLY under the flag, ONLY for a
     * dispatch whose packet (P3) and in-force compute state (P2) already match N exactly, with the program VA as the
     * stream writes it (COMPUTE_PGM_LO << 8 | COMPUTE_PGM_HI[7:0] << 40); 1 = the caller PROVES that VA holds N's own
     * gfx1201 port (the kext: THIS frame's gathered program at that VA, compute stage, key 0x9b226a39a78fe876, bytes
     * ours), anything else refuses. Required (non-NULL) under the flag - XLAT12_ERR_ARG otherwise. APPENDED. */
    void *cs_ctx;
    int (*cs_is_n)(void *ctx, uint64_t va);
    /* build 0.0.486 (switch 59, an internal design note Q3): THE T#-AWARE ASK. When non-NULL it is called INSTEAD of
     * desc_tiled_ok, at the same two sites (the table step and XLAT12_EXTRA_INLINE_DESC), with the same first four arguments
     * plus `g10`, the raw gfx10 record being placed (read-only), and `clamp`: a yes (1) with *clamp 1 makes the translator set
     * MAX_MIP and LAST_LEVEL to 0 in ITS OWN translated copy (xlat12_tdesc_clamp_mip0), before the table step's shadow compare,
     * never in the gfx10 record. NULL (every caller before this build task, and the kext while switch 59 is off) is
     * desc_tiled_ok's path, byte for byte. APPENDED, so every positional initialiser keeps its fields. */
    int (*desc_tiled_okt)(void *ctx, uint64_t va, uint32_t g12_sw_mode, uint32_t elem_bytes, const uint32_t *g10, uint32_t *clamp);
    /* build 0.0.500 (XLAT12_EXTRA_DRAW_ELIDE, below): the row CLASSES the elide may act for - XLAT12_DE_CLASS_UY (U and
     * Y, the clock composite) and/or XLAT12_DE_CLASS_AO (AO, the login panel material). Read only under the flag; under it,
     * 0 or any other bit is XLAT12_ERR_ARG. APPENDED, so every positional initialiser keeps its fields. */
    uint32_t draw_elide_rows;
    /* build 0.0.512 Part B: THE T# OBSERVER - read-only instrumentation. When non-NULL the table
     * step calls it once per texture record it READ (after the two identical reads, before the port translates it), with
     * desc_ctx, the in-force fragment program's kDTableAbi identity (ndw << 32 | fnv), the draw's input dword | texture index
     * << 24, the raw gfx10 record (8 dwords, read-only) and the surface VA it names. It returns nothing and is told nothing
     * the translator depends on: NULL (every caller before this build) and non-NULL translate byte-identically. APPENDED. */
    void (*tex_note)(void *ctx, uint64_t ps_id, uint32_t at_i, const uint32_t rec[8], uint64_t va);
    /* build 0.0.531 item 4b: THE SAME OBSERVER WITH APPLE's HEAP INDEX - read-only instrumentation.
     * When non-NULL it is called right after tex_note, for the same record, with the same arguments plus `heap_idx`: the image
     * index the table step used for this texture (aidx: the texture slot's dword, a shadow redirect's Apple index, or a class-10
     * entry's dword), i.e. the record read at the image heap + heap_idx * 32. Told nothing the translator depends on: NULL (every
     * caller before this build) and non-NULL translate byte-identically. APPENDED. */
    void (*tex_note_ix)(void *ctx, uint64_t ps_id, uint32_t at_i, uint32_t heap_idx, const uint32_t rec[8]);
    /* build 0.0.535 item 4: when non-NULL, zeroed at the translation's start and kept up to date with the draw's own CB0 and
     * scissors (xlat12_tex_state) so tex_note / tex_note_ix callers can read the state in force at the draw they are told about.
     * Told nothing the translator depends on: NULL and non-NULL translate byte-identically. APPENDED. */
    xlat12_tex_state *tex_state;
    /* build 0.0.554 (ADMIT-STALE-112.md section 3; the kext's switch 112): THE STALE-ADMIT CALLBACK. When non-NULL it
     * is called ONLY from the table step's image loop (d_table_desc), ONLY for a record that is TILED (translated gfx12 SW_MODE
     * non-zero), NOT DCC-stripped, and that every proof ask (desc_tiled_okt / desc_tiled_ok) has just refused: never from the
     * inline path (d_inline_desc), never from the glass probe (d_de_probe_rest), never for a linear or a stripped record.
     * Arguments: desc_ctx; the fragment program's kDTableAbi identity (ndw << 32 | fnv); the draw's input dword | texture index
     * << 24 (tex_note's at_i); the raw gfx10 record (8 dwords, read-only); the surface VA it names; its translated gfx12 SW_MODE;
     * its bytes per element; and `clamp`, which a yes may set to 1 (the table step then clamps MAX_MIP / LAST_LEVEL to 0 in ITS OWN
     * translated copy, as for desc_tiled_okt). Exactly 1 admits: the input then exports in_proven 1 and in_admit bit i, and every
     * later rung (the other textures, the S#, placement, the dependency gate R2-R5', the copy guard) still runs. NULL (every caller
     * before this build, and the kext while switch 112 is OFF) is 0.0.553's translation byte for byte. APPENDED. */
    int (*desc_stale_ok)(void *ctx, uint64_t ps_id, uint32_t at_i, const uint32_t *rec, uint64_t va, uint32_t g12_sw_mode,
                         uint32_t elem_bytes, uint32_t *clamp);
} xlat12_draw_extra;
/* build 0.0.486: the mip-0 clamp of a TRANSLATED gfx12 image record - MAX_MIP (gfx12 word 1 [16:12]) and LAST_LEVEL (gfx12
 * word 3 [19:15]), the positions xlat12_desc.h writes them at. Nothing else changes. */
static inline void xlat12_tdesc_clamp_mip0(uint32_t *g12)
{
    g12[1] &= ~(0x1Fu << 12);
    g12[3] &= ~(0x1Fu << 15);
}
/* 0.0.226: RADV's ring-offsets table for the merged vertex stage. tri allocates one page right after the GE rings in the
 * same buffer (ring base + XLAT12_GE_RING_TOTAL) and writes the attribute-ring descriptor at +XLAT12_RING_PS_ATTR_OFF (RING_PS_ATTR 10
 * x 16, radv_constants.h:70); the stage loads it from s0:s1 + 0xa0 and ORs STRIDE(16 x params) into word 1. The descriptor is
 * ac_build_attr_ring_descriptor (ac_descriptors.c:869-927) for va = the GE rings buffer, size = the whole rings (radv_queue.c:337):
 *   word 0 va, word 1 BASE_ADDRESS_HI [15:0] = va >> 32 | STRIDE [29:16] 0 | SWIZZLE_ENABLE [31:30] 3, word 2 size,
 *   word 3 DST_SEL_X..W [11:0] 4 5 6 7 | FORMAT [17:12] GFX11_FORMAT_32_32_32_32_FLOAT 63 | INDEX_STRIDE [22:21] 2 |
 *          OOB_SELECT [29:28] STRUCTURED_WITH_OFFSET 0 = 0x0043FFAC (field positions: Mesa src/amd/registers/gfx12-rsrc.json). */
#define XLAT12_GE_RING_DESC_OFF   XLAT12_GE_RING_TOTAL
#define XLAT12_GE_RING_DESC_PAGE  0x1000u
#define XLAT12_RING_PS_ATTR_OFF   0xA0u
#define XLAT12_ATTR_DESC_WORD3    0x0043FFACu
/* 0.0.265: VERTEX RELOCATION. Our compiled gfx1201 vertex stages can exceed Apple's own shader allocation, so the
 * program is written into kext-owned, Apple-mapped memory and the translated stream's SPI_SHADER_PGM_LO/HI_ES is OVERRIDDEN to
 * name it, instead of re-homing Apple's value (which is what 0.0.213-0.0.264 did). The arena is the tail of the ring region,
 * above the ring-offsets descriptor page: [+0xA81000, +0xA8F000) = 56 KiB inside leaf 168, which `ringmap` already builds and
 * maps R/W/X (ringmap_leaf sets EXECUTABLE). PGM_LO holds VA >> 8, so a relocated program must be 256-byte aligned.
 *
 * 0.0.413 (an internal design note): the arena SHRINKS from 0xF000 to 0xE000 (60 -> 56 KiB) so
 * that its LAST 4 KiB, [+0xA8F000, +0xA90000), can hold the fence828 owned slot array (1024 four-byte slots, one per
 * commit ordinal). `XLAT12_FENCE_PAGE_OFF/BYTES` name that page here because the arena and the fence page must never
 * overlap; a host test asserts the arithmetic. */
#define XLAT12_RELOC_ARENA_OFF    0x00A81000u
#define XLAT12_RELOC_ARENA_BYTES  0x0000E000u
#define XLAT12_RELOC_ALIGN        0x100u
/* The fence828 owned slot page: FIXED at the last 4 KiB before the L1 block. It is NOT derived from the arena, so
 * that the arena growing into it (the B2 hazard) is a detectable overlap rather than a silently moved page. */
#define XLAT12_FENCE_PAGE_OFF     0x00A8F000u
#define XLAT12_FENCE_PAGE_BYTES   0x00001000u
/* 0.0.313: the one register the interpolation guard reads, and the bits that mean "this fragment stage
 * consumes something the vertex stage must have exported". gfx10.3 SPI_PS_IN_CONTROL (gc_10_3_0_sh_mask.h:22050-22056):
 * NUM_INTERP [5:0] mask 0x3F is how many interpolants the PS pulls from the SPI, and PARAM_GEN [6] mask 0x40 makes the
 * SPI synthesise one more. Both zero is the only state in which a vertex stage that exports no parameters is faithful.
 * (On gfx12 the same field lives in SPI_SHADER_GS_OUT_CONFIG_PS; this is the gfx10.3 address, which is what we read from
 * Apple's stream.) */
#define XLAT12_REG_PS_IN_CONTROL  0x286d8u
#define XLAT12_PS_IN_CONSUMES     0x7Fu
void xlat12_attr_ring_descriptor(uint64_t va, uint32_t size, uint32_t desc[4]);

const xlat12_draw_profile *xlat12_ib_m2tri_profile(void);
uint32_t xlat12_ib_translate_draw(const xlat12_draw_profile *pf, const uint32_t *in, uint32_t n, uint32_t *out,
                                  uint32_t *out_len, xlat12_draw_stats *ds);
/* The same, plus the ring/CU block (ex may be NULL = identical to xlat12_ib_translate_draw). The block is emitted after the
 * last translated packet, inside the NOP pad before the draw; the draw and its tail keep their offsets. */
uint32_t xlat12_ib_translate_draw_ex(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, const uint32_t *in,
                                     uint32_t n, uint32_t *out, uint32_t *out_len, xlat12_draw_stats *ds);
uint32_t xlat12_ib_draw_verify(const uint32_t *out, uint32_t n, uint32_t *bad_addr, uint32_t *bad_op);
/* Last value a SET/SET_INDEX packet in buf[0..n) writes at gfx12 byte address `addr`: 1 found, 0 not. */
int xlat12_ib_find_set(const uint32_t *buf, uint32_t n, uint32_t addr, uint32_t *val);

/* Positive control over r44's real drawinj stream (tests/fixture_r44_drawinj.h). Bits: 0 translates to 1022,
 * 1 the draw and tail keep their offsets byte-for-byte, 2 the output verifies, 3 the re-homed VS program / vertex
 * buffer user data / stage enable read back, 4 both prefetches and the extra CONTEXT_CONTROL are gone, 5 refusal
 * controls (a non-zero unnamed write, a second draw), 6 (0.0.217) the ring/CU block's exact 15 dwords before the pad with the
 * draw kept at its offset and the output verified, and a misaligned ring base refused; 7 (0.0.218) the preamble delta (46 dw,
 * 61 with the ring/CU block) with its values, RSRC2_GS USER_SGPR 6 from the record, draw kept, output verified. */
#define XLAT12_IB_DRAW_SELFTEST_ALL 0x1FFFu  /* 0.0.220: bit 8 = the raster/CB delta; 0.0.224: bit 9 segments, bit 10 ablation flags;
                                               * 0.0.226: bit 11 the descriptor table pointer, the descriptor and GS_OUT_CONFIG_PS;
                                               * 0.0.265: bit 12 the relocated vertex program */
uint32_t xlat12_ib_draw_selftest(void);

/* ---- 0.0.224 (notes sections 363-364): several draws in one command buffer ----------------------------------------------------
 * census1 (`tri suite --dump`, eight render passes in ONE command buffer, never committed): Apple writes each render encoder as
 * ONE segment, end to end in the command buffer's host memory and NOT page-aligned (a longer pass spills into the next page):
 *   start    C0004600 00000016 (EVENT_WRITE), ACQUIRE_MEM C0065800 + 7, and C0004600 0000000E eight dwords after the ACQUIRE_MEM;
 *   state    the encoder's init block, then its draw block (PGM_LO/HI_VS/PS, USER_DATA_VS/PS, VGT_SHADER_STAGES_EN ...);
 *   draw     one DRAW_INDEX_AUTO;
 *   trailer  C0004600 00000016 + ACQUIRE_MEM + NOP (C0071000, where a start has EVENT_WRITE 0xE) + RELEASE_MEM + NOP pad.
 * Every segment re-emits its own pipeline state, so each one translates on its own with xlat12_ib_translate_draw_ex over
 * [start, end) = from after its head to the next segment head (or the walk end). */
#define XLAT12_SEG_HEAD0    0xC0004600u
#define XLAT12_SEG_HEAD1    0x00000016u
#define XLAT12_SEG_ACQUIRE  0xC0065800u
#define XLAT12_SEG_EVENT_E  0x0000000Eu
typedef struct { uint32_t head, start, end, draws, draw_at; } xlat12_ib_segment;
/* Split in[0..n), which must begin at a segment start, into segments. Fills seg[0..min(total, max)); returns that count, or 0
 * when in[0] is not a segment start. *total = every segment start found (the caller refuses total > max). */
uint32_t xlat12_ib_segments(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total);

/* 0.0.354 : WHAT THE EARLY EXIT AT THE TOP OF xlat12_ib_segments HIDES, AS A READING.
 * xlat12_ib_segments returns 0 unless seg_start_at matches at dword 0, and it never searches for a later head, so its
 * caller cannot tell "this stream carries no segment head at all" from "this stream's head is simply not first". This
 * function is that distinction and nothing else: read-only over in[0..n), no output stream, no state, and it does not
 * change what xlat12_ib_segments does.
 *   return      = segment heads found by walking the packet chain from dword 0, exactly as xlat12_ib_segments walks it
 *   *first_at   = the first such offset, or XLAT12_SEG_NONE when there is none
 *   *first_raw  = the first offset at which seg_start_at matches with NO packet walk. A raw match the walk does not also
 *                 report lies inside some packet's body - a false positive, and worth being able to see as one.
 *   *walked     = dwords the packet walk covered (a zero-length packet stops it short of n)
 * first_at == 0 means xlat12_ib_segments would have scanned this stream; first_at > 0 means the early exit, and only the
 * early exit, is why it returned 0; a return of 0 means no head exists anywhere and the early exit is NOT the reason. */
#define XLAT12_SEG_NONE 0xFFFFFFFFu
uint32_t xlat12_ib_seg_probe(const uint32_t *in, uint32_t n, uint32_t *first_at,
                             uint32_t *first_raw, uint32_t *walked);

/* 0.0.224 : the ablation flags (xlat12_draw_extra.flags). The draw policy normally overrides Apple's legacy-VS
 * SPI_SHADER_IDX_FORMAT (0.0.216) and VGT_GS_OUT_PRIM_TYPE (0.0.215); with the flag it emits Apple's own value re-homed to the
 * same gfx12 register instead, so a run can measure whether the override is needed. */
#define XLAT12_EXTRA_APPLE_IDXFMT  0x8u
#define XLAT12_EXTRA_APPLE_OUTPRIM 0x10u
/*: XLAT12_EXTRA_SYNTH_IDXPRIM - DEFAULT OFF; nothing in the kext sets it. With it, the NGG block
 * d_synth emits after every VGT_SHADER_STAGES_EN also carries gfx12 SPI_SHADER_IDX_FORMAT 0x28648 = pf->spi_shader_idx_format
 * and VGT_GS_OUT_PRIM_TYPE 0x30998 = pf->gs_out_prim_type - the two words found written by NEITHER half of a compositor frame
 * before the render half's first draw, so without it they hold whatever the last executed submission left. The values are the
 * ones d_reg's override already writes whenever Apple does write the register (Apple's legacy-VS value is 0 for both on every
 * wsgc1 frame that writes them, F2/F4/F6/F8/F9/F11 per halfcheck), i.e. m2tri's IDX0_EXPORT_FORMAT SPI_SHADER_1COMP (1) - radeonsi
 * programs exactly that once in its gfx12 preamble (re/graphics/src/mesa/src/gallium/drivers/radeonsi/si_state.c:5079-5080) - and
 * OUTPRIM_TYPE TRISTRIP (2), which radeonsi writes to the gfx11+ uconfig address for every NGG draw (si_state_draw.cpp:1084-1087;
 * enum values from Mesa src/amd/registers/gfx12.json). Refused with the two ablation flags (one register, two policies). */
#define XLAT12_EXTRA_SYNTH_IDXPRIM 0x20u
/*: XLAT12_EXTRA_RASTER_PER_DRAW - DEFAULT OFF; nothing in the kext sets it. XLAT12_EXTRA_RASTER's
 * follow-on rule is "exactly two follow-on writes per TRANSLATION" (raster_follow != 2 refuses 0xFE), which is the shape of the
 * tri suite's one-draw stream and of nothing else: the compositor's two-draw render halves write CB_COLOR0_VIEW and _ATTRIB
 * before EACH draw, so they carry four and refuse. It is also blind in both directions: a stream that writes VIEW
 * twice and ATTRIB never counts 2 and passes. With this flag (valid only together with XLAT12_EXTRA_RASTER) the rule is per
 * DRAW instead: at every draw, this translation must already have made at least one VIEW2 follow-on AND one FDCC_CONTROL
 * follow-on (each follow-on is emitted in the same packet right after its source write, so the VIEW2 in force at a draw is
 * always the one derived from the VIEW in force). A draw with either missing refuses XLAT12_IB_ERR_DRAW_SHAPE, err_op 0xFD,
 * err_reg the gfx10 register not yet written, err_in_dword the draw. Without the flag the old rule is untouched. */
#define XLAT12_EXTRA_RASTER_PER_DRAW 0x40u
/*: XLAT12_EXTRA_INLINE_DESC - DEFAULT OFF; nothing in the kext sets it. With it, at every draw whose
 * fragment profile names inline records (ps_inline_tex1 / ps_inline_samp1), the translated stream's SPI_SHADER_USER_DATA_PS
 * values for those slots - which the draw policy otherwise passes through as Apple's gfx10 records - are rewritten IN PLACE to
 * gfx12 records by the suite route's own xlat12_img_desc_g10_to_g12 / xlat12_samp_desc_g10_to_g12 (xlat12_desc.h). Every slot of
 * a record must have been written by THIS translation (an inherited value is unknown and refuses), a record shared by two draws
 * is translated once (a record half-translated refuses), and ITERATE_256 is cleared ONLY under xlat12_iterate256_droppable - so
 * a record with compression metadata still refuses exactly as the generated translator does. Refusal: XLAT12_IB_ERR_DESC.
 * (notes/M4-DESC-KEXT-PORT.md): a T# whose translated gfx12 SW_MODE is non-zero also needs ex->desc_tiled_ok's proof,
 * exactly as the table step does (err_op XLAT12_TDESC_PROVENANCE, err_reg the slot); linear records are admitted as before. */
#define XLAT12_EXTRA_INLINE_DESC 0x80u
/* (notes/M4-DESC-TABLE-IMPL.md, option (c) of M4-DESC-TABLE.md): XLAT12_EXTRA_TABLE_DESC - DEFAULT OFF; nothing in
 * the kext sets it. At every draw whose fragment program samples through a class-19 table (profile ps_table_abi1: GPUPass,
 * UberCompositeFragment), the records the program reads are SNAPSHOTTED through ex->desc_read (each read twice, refused on a
 * difference), translated (Apple's T# marker word 2 bit 31 required; LLC_NOALLOC dropped; ITERATE_256 dropped only under
 * xlat12_iterate256_droppable; then the generated xlat12_desc.h translators; a non-linear T# only with ex->desc_tiled_ok's proof),
 * and PLACED IN THE SEGMENT'S OWN PAD as the body of one type-3 NOP: a shadow table (image base at +0x00, sampler base at +0x10,
 * the only two dwords pairs the programs read) followed by the records, 32-byte aligned. The table pointer's user-data values are
 * redirected to it and the index values rewritten to 0, 1 / 0. Nothing outside this translation's output is written. Refusal:
 * XLAT12_IB_ERR_DESC with err_reg 0, err_in_dword the draw and err_op one of XLAT12_TDESC_* or 0x100 | (T# translator code),
 * 0x110 | (S# translator code). Requires ib_va (dword-aligned, below 2^48) and desc_read, else XLAT12_ERR_ARG. */
#define XLAT12_EXTRA_TABLE_DESC 0x100u
/* (notes/M4-DESC-KEXT-PORT.md): XLAT12_EXTRA_DESC_INV - DEFAULT OFF; valid only together with XLAT12_EXTRA_TABLE_DESC
 * (else XLAT12_ERR_ARG). The table step's records sit in the IB's own host pages and the programs read them with s_load, i.e.
 * through the scalar cache (K$) and GL2; a line cached from an EARLIER IB at the same address could serve old records. With this
 * flag the FIRST placement of a translation starts its pad run with one gfx12 ACQUIRE_MEM that invalidates those caches, then the
 * NOP carrying the records; the run precedes the draw that reads them (runs are only ever taken from before the current draw),
 * and every later table draw of the translation follows it. The packet is, byte for byte, the one the upstream gfx12 kernel
 * driver emits before every job (ref/linux-amdgpu gfx_v12_0.c:5129-5149 gfx_v12_0_emit_mem_sync): PACKET3(ACQUIRE_MEM, 6),
 * CP_COHER_CNTL 0, CP_COHER_SIZE 0xffffffff, SIZE_HI 0xffffff, BASE 0, BASE_HI 0, POLL_INTERVAL 0xA, GCR_CNTL = GLI_INV ALL |
 * GLM_WB | GLM_INV | GLK_INV | GLV_INV | GL1_INV | GL2_INV | GL2_WB = 0xC3B1 (field positions: mesa src/amd/registers/pkt3.json
 * GCR_CNTL, the same for gfx10/gfx103/gfx11/gfx12: GLI_INV [1:0] (GLI_ALL = 1), GLM_WB 4, GLM_INV 5, GLK_INV 7, GLV_INV 8, GL1_INV
 * 9, GL2_INV 14, GL2_WB 15; radeonsi's gfx12 barrier maps INV_SMEM -> GLK_INV, INV_ICACHE -> GLI_INV(ALL), INV_L2 -> GL2_INV |
 * GL2_WB, si_barrier.c:165-181). 8 dwords. Whether it is SUFFICIENT is a hardware question the translator cannot answer. */
#define XLAT12_EXTRA_DESC_INV   0x200u
#define XLAT12_DESC_INV_DWORDS  8u
#define XLAT12_DESC_INV_GCR     0x0000C3B1u
/* 0.0.398 ( DESIGN item 1,): XLAT12_EXTRA_FILL_COLOR - DEFAULT OFF, and nothing in the kext sets
 * it unless `accel gfxneuter 31 | M << 8` placed a 16-byte float4 in the relocation arena (gfx_reloc.h). The fill's
 * colour is NOT a constant: `ws_B_ColorFill`'s IB writes SPI_SHADER_USER_DATA_PS_2/_3 as the 64-bit VA of a 16-byte
 * float4 it LOADS ("buffer location 0 (color)";  measured 0x4000c0110 in arm21's frame 1), and the
 * translator passes that pair through verbatim. With this flag, at every draw whose IN-FORCE fragment identity is
 * ws_B_ColorFill (profile.ps_id), that pair - if THIS translation wrote it and it reconstructs to a 64-bit VA in
 * WindowServer's range - is rewritten to ex->fill_color_va, and the rewrite is recorded in the stats. FAIL-CLOSED:
 * either condition failing leaves the draw untouched and counts fill_color_refused. OFF, not one output dword,
 * counter or return code changes: the flag is never set and d_fill_color is never called. */
#define XLAT12_EXTRA_FILL_COLOR 0x400u
/* 0.0.409: XLAT12_EXTRA_PAIR_PRE - DEFAULT OFF; nothing in the kext sets it unless
 * `accel gfxneuter 34 | 1 << 8` is thrown. ON, the translator RESOLVES THE SEGMENT'S PROGRAM PAIR BEFORE IT
 * SYNTHESISES THE PAIR-DEPENDENT STATE. Today d_synth() runs at a VGT_SHADER_STAGES_EN that a legacy stream writes
 * BEFORE its SPI_SHADER_PGM_LO/HI pair, so SPI_SHADER_GS_OUT_CONFIG_PS is built from the caller's base (m2tri)
 * profile - NUM_INTERP 0, NO_PC_EXPORT 1 - and SPI_PS_INPUT_ENA/ADDR take the base program's values too. The
 * pre-scan walks the region's SET packets for gfx10 SPI_SHADER_PGM_LO/HI_VS (0xb120/0xb124, re-homed to the merged
 * NGG stage) and _PS (0xb020/0xb024), reconstructs each stage's 64-bit VA the way d_pair_note does (d_pair_va) and
 * resolves it through the SAME ex->pgm_profile callback; d_synth then synthesises SPI_SHADER_GS_OUT_CONFIG_PS from
 * d_pf(pf, &pair) instead of the base, and d_reg's PSP_INPUT_ENA/ADDR override follows d_pf for free because
 * d_pair_note is pre-seeded and the walk sees the pair as unchanged. NO OUTPUT DWORD IS ADDED OR REMOVED - only the
 * VALUES of registers the policy already wrote change. FAIL-CLOSED: a stage whose LO write is absent or zero keeps
 * the base value, and a resolver refusal discards the whole pre-scan (base values, counted in pair_pre_unresolved).
 * OFF, not one output dword, counter or return code changes: d_pair_pre() is never called and d_synth still takes
 * `pf` exactly as it did through 0.0.408. */
#define XLAT12_EXTRA_PAIR_PRE 0x800u
/* D4' (an internal design note item 3, an internal design note Q5): XLAT12_EXTRA_READSET - DEFAULT OFF; nothing in
 * the kext sets it unless `accel gfxneuter 40 | 1 << 8` is ON. ON, at EVERY draw the translator resolves the in-force
 * PS and VS identities' src/xlat12/xlat12_readset.h rows (xlat12_draw_profile.ps_readset1 / vs_readset1, filled by
 * xlat12_ib_profile_stage/_for from the SAME identity match ps_table_abi1/vs_abi_ptr1 already use) and ACCUMULATES
 * every admitted pointer and admitted inline image into xlat12_draw_stats' rs_* fields, and every INDEX_BASE VA seen
 * in the stream (already a recognised, cheaply-decoded pass-through opcode; an internal design note Q6) into
 * rs_ptr[] - ACROSS EVERY DRAW OF THE SEGMENT, NEVER CLEARED PER DRAW (unlike in_*, which d_in_clear empties at each
 * draw - the fix for 0.0.438's Finding-4 interim refusal, for the programs this row set covers). A draw whose PS or
 * VS program has no row, or whose row's proof bits do not admit it (proof_depth1_data_only != 1, or an image op
 * whose proof_images_inline != 1), sets rs_declined for the WHOLE segment - a missing or unproven row is UNKNOWN,
 * never "no pointers" (xlat12_readset.h's own banner). OFF, not one output dword, counter or return code changes:
 * d_readset_accum is never called and every rs_* field stays zero. */
#define XLAT12_EXTRA_READSET 0x1000u
/* build 0.0.448 item 1 (an internal design note Q1, "B2/B3 WITHIN ONE TRANSLATION, no cross-segment
 * carry"), build 0.0.449 item 1 (F1 fix - the carry moved off a file-scope static): XLAT12_EXTRA_UD_REEMIT
 * - DEFAULT OFF; nothing in the kext sets it unless `accel gfxneuter 44 | 1 << 8` is ON. REQUIRES
 * XLAT12_EXTRA_TABLE_DESC (refused with ERR_ARG otherwise - the mechanism has nothing to re-emit INTO without the
 * table step) AND a non-NULL `ex->ud_carry` (refused with ERR_ARG otherwise - F1: the translator keeps no mutable
 * state of its own to fall back on). ON, at a table-bound draw whose table pointer, texture index, sampler
 * index/class-11 pointer slot, OR declared PS/VS ABI pointer pair (item 3, F4: xlat12_abi_ptrs.h's rows,
 * MIB-A1-PATH.md B3's full set) was written by an EARLIER REGION of THIS SAME translation - today's
 * XLAT12_TDESC_SLOT_SHARED refusal, or an inherited ABI pointer - the translator re-emits a SET_SH_REG carrying
 * APPLE'S OWN recorded input value for that slot (the SAME raw gfx10 dword d_table_desc's own
 * SLOT_UNSEEN/SLOT_SHARED rungs would otherwise refuse against, from `ex->ud_carry`), through the SAME
 * d_reg/d_add path a live write takes, INSIDE THIS REGION - so this region owns its own write of the slot and
 * the SLOT_SHARED condition (ud_pos < region_from) no longer holds. A slot this region ALREADY wrote is left
 * alone: it always wins over the carry. This does NOT fix SLOT_UNSEEN (a slot never written anywhere in this
 * translation has no carried value at all) and it NEVER crosses a translation - `*ex->ud_carry` is zeroed at the
 * top of every xlat12_ib_translate_draw_ex call exactly as DPair's own ud_seen/ud_pos already are, on
 * CALLER-OWNED memory (F1), so two translations that pass separate `xlat12_ud_carry` structs never share a
 * value; a non-proven CONTEXT_CONTROL clears the whole carry (item 3, F4's B2 rule); a program using INLINE
 * descriptors instead of a table (ps_table_abi1 == 0) is untouched, so an inline record is never re-emitted.
 * Room shortfall in THIS region's own dword budget fails closed as XLAT12_IB_ERR_TOO_LONG, the SAME conversion
 * d_rings already uses, with no partial write (d_add either appends a whole packet or fails before writing
 * anything). build 0.0.453 item 4 (inv-f84/REPORT.txt): unlike d_rings (which stamps err_op 0xFF before
 * returning), a re-emission capacity failure used to leave err_op/err_in_dword exactly as d_region's LAST packet
 * left them - not this draw's own dword - because the failure happens AFTER d_region has already returned 0 for
 * this region. It now stamps XLAT12_REEMIT_NO_ROOM and err_in_dword = da[dk] (the draw itself), so the log blames
 * the draw whose re-emission ran out of room, not whatever register d_region's own pad walk last touched;
 * a shortfall in the table's OWN pad-run placement still fails closed as XLAT12_TDESC_NO_ROOM exactly
 * as it always did - re-emission can only SHRINK that room, never bypass the existing check for it. OFF, not one
 * output dword, counter or return code changes: the re-emission step is never reached and ud_reemit_n stays
 * zero. */
#define XLAT12_EXTRA_UD_REEMIT 0x2000u
/* build 0.0.453 item 5 (inv-f84/REPORT.txt, CONFIRMED): XLAT12_EXTRA_DESC_INV_APPLE_HEAD - DEFAULT OFF;
 * valid only together with XLAT12_EXTRA_DESC_INV (refused with ERR_ARG otherwise - the same "requires its own
 * mechanism" rule XLAT12_EXTRA_UD_REEMIT/XLAT12_EXTRA_RASTER_PER_DRAW are already held to). CONFIRMED (ref/linux-
 * amdgpu gfx_v12_0.c:5129-5149 gfx_v12_0_emit_mem_sync; mesa src/amd/registers/pkt3.json's GCR_CNTL field layout;
 * every one of F84's 6 checked segments, both IBs, offline against the real capture): every segment Apple submits
 * opens - dword 0, before our own placement or anything else in the segment - with PACKET3(ACQUIRE_MEM, 6),
 * GCR_CNTL 0x0001c3f1. That is a STRICT SUPERSET of XLAT12_DESC_INV_GCR (0xC3B1): every bit XLAT12_EXTRA_DESC_INV's
 * own 8-dword invalidate sets (GLI_INV=ALL[0], GLM_WB[4], GLM_INV[5], GLK_INV[7], GLV_INV[8], GL1_INV[9],
 * GL2_INV[14], GL2_WB[15]) is ALSO set by Apple's own packet, plus GLK_WB[6] (a write-back on the SAME cache our
 * own GLK_INV already covers - a superset action, not a narrower one) and SEQ=SEQ_FORWARD[17:16] (an ordering
 * field, not a cache-scope bit). Both GL1_RANGE[3:2] and GL2_RANGE[12:11] are 0 = GL1_ALL/GL2_ALL on Apple's
 * packet (mesa enums GCR_GL1_RANGE/GCR_GL2_RANGE) - an UNSCOPED, whole-cache invalidate, exactly as our own
 * (which also uses RANGE 0/ALL), so CP_COHER_BASE/SIZE play no part in either packet's actual coverage.
 * ON, xlat12_ib_head_acquire_mem_covers(in, n, XLAT12_DESC_INV_GCR) is checked ONCE per translation (`in`/`n` are
 * the SAME segment bytes this whole call receives - xlat12_ib_translate_draw_ex is always called once per
 * SEGMENT, so in[0] is the segment's own first dword by construction, never a draw-local offset); if it is TRUE,
 * the translation's own 8-dword ACQUIRE_MEM is skipped (XLAT12_DESC_INV_DWORDS worth of pad never spent) because
 * Apple's own, already-executed, unconditionally-first packet already invalidated everything ours would. If the
 * check is FALSE (a segment that does NOT open this way - a different driver path, a future capture, a malformed
 * stream), NOTHING changes: the translation's own invalidate is placed exactly as XLAT12_EXTRA_DESC_INV alone
 * already does. OFF (the default), this flag changes nothing regardless of what dword 0 holds - the same
 * conservative default XLAT12_EXTRA_DESC_INV itself has. */
#define XLAT12_EXTRA_DESC_INV_APPLE_HEAD 0x4000u
/* build 0.0.454 items 1 and 2 (inv-f84/REPORT.txt "FIX T", seg 1;  item 3) - switch 48:
 * XLAT12_EXTRA_TABLE_REUSE - DEFAULT OFF; valid only together with XLAT12_EXTRA_TABLE_DESC (ERR_ARG otherwise).
 * Nothing in the kext sets it unless `accel gfxneuter 48 | 1 << 8` is thrown. OFF, not one output dword, counter or
 * return code changes. ON, two things change in the table step, and only there:
 * (a) THE PLACEMENT ALIGNMENT. The shadow table and its records are placed 16-byte aligned instead of 32. The records
 *     are read by the fragment program with s_load_b64 / s_load_b128 / s_load_b256 (the T#/S# then sit in SGPRs; an
 *     image instruction reads them from SGPRs, never from memory), and on gfx12 a scalar load needs only a DWORD-
 *     aligned address: Mesa's AMD NIR lowering gives every SMEM load `res.align = MIN2(4, bytes)` for all gfx levels
 *     ("Generally, require an alignment of 4.", re/graphics/src/mesa src/amd/common/nir/ac_nir_lower_mem_access_bit_sizes.c
 *     lower_mem_access_cb, the is_smem branch, loads up to 64 bytes), ACO's scalar global load asserts only
 *     `nir_intrinsic_align(instr) >= MIN2(bytes_needed, 4)` before emitting s_load_dwordx8/x4
 *     (src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp visit_load_global_smem), and RADV exposes
 *     VK_EXT_descriptor_buffer unconditionally with descriptorBufferOffsetAlignment = 4 (src/amd/vulkan/
 *     radv_physical_device.c), i.e. a set's T#s are fetched by s_load_b256 from a base that is only 4-byte aligned.
 *     16 is kept as a margin over that rule; it costs at most 3 dwords of pad where 32 cost up to 7 (F84 seg 1, AI's
 *     class-11 placement: 1 + 36 + 3 <= 43 with switch 49, where 1 + 36 + 7 did not fit).
 * (b) SHADOW REUSE. A fresh placement for a direct-sampler row (every row but class 11) sizes its T# and S# arrays to
 *     the spare dwords of its pad run (up to D_SH_TMAX / D_SH_SMAX records) and is remembered as the translation's
 *     reusable shadow. A LATER table draw of the SAME translation keeps that shadow instead of re-placing, when the
 *     table pair still holds the shadow's redirect (ud_pos == the redirect's position) and the row names the SAME
 *     table and sampler slots; each of its texture/sampler slots must either still hold the shadow's redirect or have
 *     been written in THIS region (a slot rewritten in an earlier region is not reused: the draw takes the fresh path
 *     and refuses exactly as before). New T#s (and the S#, when the sampler slot was rewritten in this region) are
 *     translated, sent through the SAME provenance ask as a fresh placement, and appended to the shadow's spare
 *     records; the in-region index is rewritten to the record's position. The draw's exports (in_tbl_va, in_img_va,
 *     in_samp_va, in_va/in_mode/in_proven, in_ptr/in_vptr) are filled with APPLE'S values exactly as a fresh
 *     placement would fill them. Under XLAT12_EXTRA_UD_REEMIT a reusing draw does not re-emit the slots the shadow
 *     still holds, nor an un-redirected declared PS ABI-pointer slot whose value this translation wrote in an earlier
 *     region and the carry still holds unchanged (that SGPR already carries Apple's value; the export names it).
 *     Refusals: no spare record left -> XLAT12_TDESC_NO_ROOM; a held record whose Apple bytes no longer translate to
 *     what was placed -> XLAT12_TDESC_UNSTABLE. Counted in table_reused / table_appended. */
#define XLAT12_EXTRA_TABLE_REUSE 0x8000u
/* build 0.0.455 item 1 ('s known-slot rule) — XLAT12_EXTRA_VS_KNOWN - DEFAULT OFF; valid only
 * together with XLAT12_EXTRA_UD_REEMIT (refused with ERR_ARG otherwise - the same "requires its own mechanism"
 * rule XLAT12_EXTRA_TABLE_REUSE/XLAT12_EXTRA_RASTER_PER_DRAW are already held to): this rule has nothing to
 * consult without the carry XLAT12_EXTRA_UD_REEMIT populates. Nothing in the kext sets it unless `accel
 * gfxneuter 52 | 1 << 8` is thrown.
 *
 * WHAT IT CHANGES. found that F84's remaining table-draw declines (10 of 12 translated segments) are NOT
 * cross-segment: every one reads a VERTEX pointer slot (4 and/or 6) written EARLIER IN THE SAME translation, in an
 * earlier region - today unconditionally counted INHERITED (XLAT12_TDESC_SLOT_UNSEEN's sibling, "position <
 * region_from") because a later region cannot in general assume an earlier one's SGPR write still holds (a
 * REDIRECT of ours could sit there, or nothing at all if the earlier write was itself to a stale slot). ON, such a
 * slot is instead asked of d_vs_slot_known: 1 only when the carry (ex->ud_carry) still holds EXACTLY that write
 * for it (vs_ok bit set, vs_val[k] == out[pos[k]] - the identical "still true" test d_sh_abi_known already uses
 * for the PS case) - i.e. nothing has redirected or overwritten that SGPR since Apple's own last write to it. When
 * true, the slot is treated as known (its value is read and used, or the read-set names its page) exactly as a
 * slot written in THIS region already is; when false, it declines exactly as before. Applies in BOTH vertex
 * branches of d_table_desc (the declared-ABI-pointer loop and the read-set-row fallback) and in d_readset_stage's
 * OWN vertex call - the three places named. UNLIKE d_sh_abi_known's PS case, there is no "redirect of ours"
 * to exclude here: DTable's own redirect bookkeeping (`patched`/`ppos`) is PS-only (the table/texture/sampler
 * slots d_table_desc redirects are always PS SGPRs) - d_table_desc never redirects a VERTEX user-data slot, so a
 * vertex slot the carry still matches is, by construction, never one of our own redirects. Admitted slots are
 * counted in `vs_known_n`. OFF, not one output dword, counter or return code changes: d_vs_slot_known's flag check
 * fails first, so every site below evaluates to EXACTLY its pre-0.0.455 expression. */
#define XLAT12_EXTRA_VS_KNOWN 0x10000u
/* build 0.0.480 (an internal design note Q11, contract items C2 and C4; switch 55 in the kext) —
 * XLAT12_EXTRA_UNIT - DEFAULT OFF; nothing in the kext sets it unless `accel gfxneuter 55 | 1 << 8` is ON, and then ONLY
 * for a CONTINUATION UNIT of two or more constituents (gfx_mib.h n48_mib_units: one non-continuation encoder segment
 * plus every continuation that follows it in the SAME IB). Requires a non-NULL `ex->unit` (XLAT12_ERR_ARG otherwise:
 * every buffer this path needs is CALLER-OWNED - the pending records, the constituent table, the frame pool). ON:
 *   P2 COMPACTION  the unit's regions are laid out one after another from an OUTPUT cursor instead of NOP-padding each
 *                  draw back to its input dword; every comparison of an output position against "this region's start"
 *                  (re-emission, table-slot SLOT_SHARED, d_sh_reuse_ok, the read-set's inherited rule) takes the
 *                  region's OUTPUT start. The region after the last draw is emitted AT ITS INPUT DWORD whenever the
 *                  stream is not ahead of it (so the final trailer, and the fence828 packet in it, keep their offset).
 *   P3 DEFERRED    a fresh table placement is NOT put in a pad run: its records are built exactly as today into the
 *      RECORDS     caller's pending list (at most XLAT12_UNIT_PEND_MAX; one more refuses XLAT12_TDESC_TOO_MANY, never
 *                  truncates), the redirect positions are remembered, and when the unit's stream is complete every
 *                  pending block is placed - first-fit in the unit's own leftover NOP runs, else first-fit in
 *                  `unit->pool` (free NOP runs of units the caller translated EARLIER in the same frame, any IB, each
 *                  journalled so the caller can undo them with xlat12_pool_undo) - and only then are the VA-dependent
 *                  words (the shadow table's two heap bases, the class-10/11 entry pointers, the table pointer
 *                  redirects) written. No room anywhere refuses XLAT12_TDESC_NO_ROOM naming the draw that asked.
 *   C4 INVALIDATE  under XLAT12_EXTRA_DESC_INV the 8-dword ACQUIRE_MEM goes INLINE, in the stream right before the
 *                  unit's first table draw, unless XLAT12_EXTRA_DESC_INV_APPLE_HEAD and the unit's own head covers it
 *                  (the caller must set that flag only for a head that EXECUTES - gfx_mib.h n48_mib_head_executes).
 *   P4 UN-REDIRECT with XLAT12_EXTRA_UD_REEMIT, a later draw whose program can read a slot an earlier draw's table step
 *                  redirected gets Apple's carried value for that slot re-emitted in its own region (d_reg / d_add,
 *                  exactly as a live write) when the carry holds the slot; without the carry it refuses REDIRECTED as
 *                  it always did.
 *   HS PAIRS       every zero SPI_SHADER_PGM_LO/HI_HS pair write of the unit gets the ring-offsets pointer,
 *                  not only region 0's last one - so a unit patches what its constituents would each have patched.
 *   BOOKKEEPING    the output position of every constituent head is exported (`unit->cons_out`, and the fence slice
 *                  start `unit->last_head_out`), and `unit->cons_fn` (optional) is called at each constituent head
 *                  with the PREVIOUS constituent's finished output slice (the kext's frame-local provenance feed).
 *   BLOCK GUARD    a later packet of the unit that rewrites a register the once-per-translation extra block (d_rings)
 *                  wrote refuses XLAT12_IB_ERR_DRAW_SHAPE, err_op XLAT12_UNIT_BLOCK_REWRITE (fail-closed: a unit
 *                  would otherwise leave Apple's value where separate translation re-emitted ours).
 * OFF, not one output dword, counter or return code changes: every site tests U (the flag's own pointer) first. */
#define XLAT12_EXTRA_UNIT 0x20000u
/* build 0.0.487 (an internal design note Q6 items 1-2, contract C2; switch 57 in the kext) — XLAT12_EXTRA_CS_ELIDE
 * - DEFAULT OFF; nothing in the kext sets it unless `accel gfxneuter 57 | 1 << 8` is ON. Requires `ex->cs_is_n`
 * (XLAT12_ERR_ARG otherwise). ON, a DISPATCH_DIRECT is replaced IN PLACE by a same-length PACKET3 NOP
 * (XLAT12_CS_ELIDE_NOP and four zero dwords) ONLY when it is PROVEN to be Apple's compute clear N (BufferClear_CS, the
 * DCC fast clear to key 0000 whose only consumer, CB_COLOR0_DCC_BASE, this translator drops - COMPUTE-N.md Q4), by all
 * five parts, every one segment-local (this translation's own dwords, nothing inherited from outside it):
 *   P1  the flag is set and `ex->cs_is_n` is present;
 *   P2  THIS translation wrote the in-force compute state, with exactly N's values: COMPUTE_START_X/Y/Z 0,
 *       NUM_THREAD_X/Y/Z 0x40/1/1, PGM_LO (any) and PGM_HI (< 0x100), RSRC1 0x400c0041, RSRC2 0x90, RESOURCE_LIMITS 0,
 *       TMPRING_SIZE 0, RSRC3 0, USER_DATA_0 (the V# base, any), USER_DATA_1 0x00100004, _2 0x400, _3 0x1104bfac,
 *       _4.._7 0 (the fill). A register-destination WRITE_DATA/COPY_DATA/WAIT_REG_MEM into the compute block forgets it all;
 *   P3  the packet is exactly `c0031502 00000010 00000001 00000001 00000001` (16 x 1 x 1 groups, COMPUTE_SHADER_EN);
 *   P4  `ex->cs_is_n(ex->cs_ctx, PGM_LO << 8 | PGM_HI << 40)` answers 1;
 *   P5  a forward scan from the dispatch to the next draw (or the end of the segment) finds a colour target 0 bound WITH
 *       DCC by the window's own writes - at some packet boundary CB_COLOR0_INFO has DCC_ENABLE (bit 28) and
 *       CB_COLOR0_DCC_BASE == base >> 8 (base = USER_DATA_0 | (USER_DATA_1 & 0xffff) << 32), both written in the window -
 *       and no CB_COLOR0_DCC_BASE_EXT written in the window is non-zero.
 * The branch sits BEFORE the D7 READSET block, so an elided dispatch sets no XLAT12_RS_WHY_DISPATCH (it reads nothing);
 * it runs AFTER the R4 counters (a DISPATCH is neither a wait nor a write, so r4 is unchanged). A dispatch that fails any
 * part falls through to today's code - the READSET decline, then IB_ERR_UNLISTED - and is counted by the first part that
 * failed (cs_ref_p2..p5). Nothing else changes: every other packet is kept byte for byte (Apple's triplet and barriers
 * included), the zero-draw refusal stays, and xlat12_ib_draw_verify's DISPATCH_DIRECT backstop stays, so a dispatch
 * can never reach the ring through this path. OFF, not one output dword, counter or return code changes. */
#define XLAT12_EXTRA_CS_ELIDE 0x40000u
#define XLAT12_CS_ELIDE_NOP   0xC0031000u   /* PACKET3(NOP, count 3): header + 4 body dwords = DISPATCH_DIRECT's 5 */
#define XLAT12_CS_N_PGM_RSRC1 0x400c0041u
#define XLAT12_CS_N_PGM_RSRC2 0x00000090u
#define XLAT12_CS_N_UD1       0x00100004u
#define XLAT12_CS_N_UD2       0x00000400u
#define XLAT12_CS_N_UD3       0x1104bfacu
#define XLAT12_CS_N_ALL       0x001FFFFFu   /* P2: the 21 compute registers above, one bit each (d_cs_bit) */
#define XLAT12_UNIT_CONS_MAX  8u    /* constituents one unit may carry (corpus maximum 6, design Q6) */
#define XLAT12_UNIT_PEND_MAX  16u   /* deferred placements one translation may carry (corpus maximum 8, design Q2) */
#define XLAT12_UNIT_PEND_DW   64u   /* storage per deferred placement: the largest body is AZ's 56 dwords */
#define XLAT12_UNIT_HS_MAX    32u   /* HS pointer-pair writes one unit may carry (over it refuses, never skips) */
#define XLAT12_POOL_RUNS      64u   /* free runs one frame pool holds (over it the run is not offered: `lost`) */
#define XLAT12_POOL_MIN_RUN   8u    /* a free run shorter than this is never offered (design Q2: >= 8 dw) */
#define XLAT12_UNIT_BLOCK_REWRITE 0xE1u   /* err_op with XLAT12_IB_ERR_DRAW_SHAPE: a d_rings register rewritten later in a unit */
#define XLAT12_UNIT_HS_OVER       0xE2u   /* err_op with XLAT12_IB_ERR_DRAW_SHAPE: more HS pair writes than XLAT12_UNIT_HS_MAX */
#define XLAT12_UNIT_DRAW_ROOM     0xFBu   /* err_op with XLAT12_IB_ERR_TOO_LONG: a draw packet did not fit the unit */
/* One free run of one-dword NOPs (XLAT12_IB_NOP) in the caller's candidate buffer: `host` its first dword, `va` the
 * GPU VA that dword executes at, `len` dwords. */
typedef struct { uint32_t *host; uint64_t va; uint32_t len; } xlat12_pool_run;
/* A journal entry: pool run `r` was `host`/`va`/`len` before this translation took its first `used` dwords. */
typedef struct { uint32_t *host; uint64_t va; uint32_t len, used, r; } xlat12_pool_jent;
/* THE FRAME POOL, CALLER-OWNED. The caller clears it (nrun = 0, jn = 0) at the start of every frame, adds the free
 * runs of each unit it translated (xlat12_pool_add_free) AFTER that unit's final status is OK, and undoes a refused
 * unit's placements (xlat12_pool_undo) wherever it restores that unit's bytes. The translator resets `jn` at the top
 * of every XLAT12_EXTRA_UNIT call, so the journal always names exactly the last unit's placements. */
typedef struct {
    xlat12_pool_run run[XLAT12_POOL_RUNS];
    uint32_t nrun, lost;
    uint32_t jn;
    xlat12_pool_jent j[XLAT12_UNIT_PEND_MAX];
} xlat12_pool;
/* One deferred placement (P3): its body is dw[off .. off + tcap*8 + scap*4 + 8 + ext_len) in the unit's storage, laid
 * out [table 8][T# x tcap][S# x scap][class-10/11 entry region ext_len]; placed as [8][T# x tn][S# x sn][entries]. */
typedef struct {
    uint32_t off, kind, tcap, tn, scap, sn, ext_len, nsamp_eff, align_mask, draw_at;
    uint32_t tpos[2], epos[2];     /* output dwords of the table pointer pair / the class-10 or -11 pointer pair (~0 = none) */
    uint32_t placed_len;           /* dwords the placement took (NOP header included); 0 until placed */
} xlat12_unit_pend;
/* The per-constituent provenance feed: the finished output of ONE constituent, `n` dwords at `out`. */
typedef void (*xlat12_unit_cons_fn)(void *ctx, const uint32_t *out, uint32_t n);
/* THE UNIT'S STATE, CALLER-OWNED (never on translate_draw_ex's frame). IN fields are the caller's and are never
 * written; everything after them is reset at the top of every XLAT12_EXTRA_UNIT call. */
typedef struct xlat12_unit_s {
    /* IN */
    uint32_t ncons;                            /* constituents, 1 .. XLAT12_UNIT_CONS_MAX */
    uint32_t cons_in[XLAT12_UNIT_CONS_MAX];    /* each constituent head's INPUT dword relative to in[0]; cons_in[0] 0 */
    xlat12_pool *pool;                         /* NULL = the unit's own leftover only */
    void *cons_ctx;
    xlat12_unit_cons_fn cons_fn;               /* NULL = no per-constituent feed */
    uint32_t pend_cap;                         /* deferred placements allowed, 0 = XLAT12_UNIT_PEND_MAX (never above it) */
    /* build 0.0.501 (an internal design note Q3 C1, ; switch 67 in the kext) - PACK. 0 = today: every
     * deferred block is its own PACKET3(NOP). 1 = PACK: every block placed in one free run (the unit's own runs, or one
     * frame-pool run) goes inside ONE PACKET3(NOP) opened by that run's first block; later blocks are appended at their own
     * alignment inside it and the header's count is rewritten after every append (pad dwords written 0). Journal, undo,
     * exhaustion (NO_ROOM after xlat12_pool_undo) exactly as today. Read only under XLAT12_EXTRA_UNIT (d_unit_finish); never
     * written by the translator and not reset by d_unit_reset (an IN field: the kext latches it once per pass). */
    uint32_t pack;
    /* build 0.0.522 ( FIRST BUILD; switch 76 in the kext) - THE SPILL TIER. NULL = today (the loops below never
     * look at it). Non-NULL: a SECOND xlat12_pool, tried LAST - after the unit's own runs and after `pool` - by both placement
     * loops (d_unit_finish, d_unit_pack), with exactly the pool's first-fit, journal, undo and exhaustion (NO_ROOM after undoing
     * BOTH journals). Its runs are the kext's own GPU-visible memory (one arena slice; the kext MM-writes the used dwords and
     * reads them back before the IB write): the records are never executed, so the NOP header a placement writes there is just
     * a dword. An IN field like `pool`: never written by the translator except through its journal/runs (reset: `jn` 0). */
    xlat12_pool *spill;
    /* OUT */
    uint32_t spill_dw;                         /* build 0.0.522: record dwords placed in `spill` (0 when it is NULL) */
    uint32_t cons_out[XLAT12_UNIT_CONS_MAX];   /* each constituent head's OUTPUT dword (cons_out[0] 0) */
    uint32_t cons_seen;                        /* heads reached, 1 .. ncons */
    uint32_t last_head_out;                    /* cons_out[ncons - 1]: the fence slice is [last_head_out, olen) */
    uint32_t stream_end;                       /* the output dword after the tail region */
    uint32_t tail_in_place;                    /* 1 = the tail region kept its input dword */
    uint32_t slack;                            /* own free dwords before any record was placed (compaction slack) */
    uint32_t own_dw, pool_dw, placed;          /* record dwords placed in the unit's own runs / in the pool; blocks placed */
    uint32_t unredir, hs_patched, inv_inline, cb_calls;
    uint32_t pk_runs, pk_recs, pk_saved;       /* 0.0.501 (pack 1): NOPs opened; blocks placed; dwords an appended block took
                                                  less than a NOP of its own at the same place (its header + alignment) */
    /* WORK */
    uint32_t ocur, rings_done, blk_from, blk_to, npend, nhs, hs_lo_open, hs_lo_pos;
    uint32_t hs_lo[XLAT12_UNIT_HS_MAX], hs_hi[XLAT12_UNIT_HS_MAX];
    xlat12_unit_pend pend[XLAT12_UNIT_PEND_MAX];
    uint32_t dw[XLAT12_UNIT_PEND_MAX * XLAT12_UNIT_PEND_DW];
    /* 0.0.501 (pack 1): the open NOP of each own run (its header's output dword, ~0 = none) and of each pool run this
     * translation wrote into (pool run index, header's host dword; pk_np of them - at most one per block). Here, not on
     * d_unit_finish's stack (the kext's stack budget). */
    uint32_t pk_own[2], pk_np;
    uint32_t pk_pr[XLAT12_UNIT_PEND_MAX];
    uint32_t *pk_ph[XLAT12_UNIT_PEND_MAX];
} xlat12_unit;
/* Add every run of >= XLAT12_POOL_MIN_RUN one-dword NOPs in out[0..n) to the pool (`va` = the GPU VA of out[0]).
 * Returns the runs added. A full pool counts `lost` and offers nothing more. */
uint32_t xlat12_pool_add_free(xlat12_pool *pl, uint32_t *out, uint32_t n, uint64_t va);
/* Undo the last unit translation's pool placements (newest first): each taken prefix is NOP-filled again and its run
 * restored. Idempotent (the journal is emptied). Returns the dwords restored. */
uint32_t xlat12_pool_undo(xlat12_pool *pl);
enum {
    XLAT12_TDESC_SLOT_UNSEEN   = 0xF1u, /* a slot the program reads was not written by THIS translation (inherited = unknown) */
    XLAT12_TDESC_SLOT_SHARED   = 0xF2u, /* a slot was written before the previous draw, so an earlier draw also reads that write */
    XLAT12_TDESC_SAMP_OVERRIDE = 0xF3u, /* sampler index bit 31: the program would replace S# word 1 with the next slot's raw word */
    XLAT12_TDESC_READ          = 0xF4u, /* a snapshot read failed */
    XLAT12_TDESC_UNSTABLE      = 0xF5u, /* the two snapshot reads differ */
    XLAT12_TDESC_NOT_APPLE     = 0xF6u, /* a T# without word 2 bit 31 (RESOURCE_LEVEL, "must be 1"): not the gfx10 record expected */
    XLAT12_TDESC_PROVENANCE    = 0xF7u, /* a non-linear T# whose surface no translated colour target is proven to have written */
    XLAT12_TDESC_NO_ROOM       = 0xF8u, /* no pad run of this translation can carry the shadow */
    XLAT12_TDESC_REDIRECTED    = 0xF9u, /* a later draw reads a user-data write this translation redirected */
    XLAT12_TDESC_TOO_MANY      = 0xFAu, /* more table draws than one translation tracks */
    /* build 0.0.508 (item 8): a NO-SAMPLER row with one texture at s6 (T, AP, AR, AV, AW, AX -
     * `{6,0,0}`) whose index slot's HIGH word (s7) was written NON-ZERO by this translation: s6:s7 would be a 64-bit value,
     * not a texture index, so heap base + 32*s6 is not the program's T#. Refused (fail-closed), never read. */
    XLAT12_TDESC_IDX_PTR       = 0xF0u,
    /* build 0.0.453 item 4 (inv-f84/REPORT.txt): a re-emission (XLAT12_EXTRA_UD_REEMIT) that ran out of room
     * in THIS region's own dword budget. Returned with XLAT12_IB_ERR_TOO_LONG (NOT _ERR_DESC - it is not a TDESC
     * rung), so this value shares the TDESC block's numbering only for readability, not its status code. */
    XLAT12_REEMIT_NO_ROOM      = 0xFCu
};
/* The T# step of XLAT12_EXTRA_TABLE_DESC, exposed for tests: 0 and out = the gfx12 record, or XLAT12_TDESC_NOT_APPLE, or
 * 0x100 | the generated translator's code. *dropped: bit 0 LLC_NOALLOC dropped, bit 1 ITERATE_256 dropped.
 * build 0.0.488: exactly xlat12_table_img_desc_ex(in, out, dropped, 0) - never strips (the readset's inline use). */
uint32_t xlat12_table_img_desc(const uint32_t in[8], uint32_t out[8], uint32_t *dropped);
/* build 0.0.488 (an internal design note Q2 option (A) and Q4; switch 60 in the kext) — XLAT12_EXTRA_DCC_STRIP,
 * DEFAULT OFF; valid only together with XLAT12_EXTRA_TABLE_DESC (ERR_ARG otherwise: it acts only in the table step).
 * Nothing in the kext sets it unless `accel gfxneuter 60 | 1 << 8` (= 316) is ON.
 *
 * WHAT IT CHANGES. A gfx10 T# with DCC metadata (a META_DATA_ADDRESS in word 7 / word 6 [31:24]) is refused today on word 7
 * (0x100 | XLAT12_DESC_ERR_UNMAPPED = 0x106): gfx12 has no separate metadata surface. ON, the TABLE path (never the inline
 * path, never the readset's inline read) translates such a record BY MEANING, only in the DCC-DESC.md accept shape: Apple's
 * marker (w2 bit 31), COMPRESSION_EN (w6 bit 21) 1, a non-zero metadata address, TYPE (w3 [31:28]) 9, MAX_MIP (w5 [7:4]),
 * LAST_LEVEL (w3 [19:16]) and BASE_LEVEL (w3 [15:12]) all 0, COLOR_TRANSFORM (w6 bit 23) 0, WRITE_COMPRESS_ENABLE (w6 bit 20)
 * 0, ITERATE_256 (w6 bit 10) 0 and MAX_UNCOMPRESSED_BLOCK_SIZE (w6 [16:15]) 2 (256B). Any other record is left exactly as
 * it was, so it takes today's answer (0x106 for every other metadata-carrying record). The strip, before the generated port:
 *   w7 = 0 and w6 [31:24] = 0           the metadata address (gfx12 has no metadata surface)
 *   w6 bit 19 = 0                        META_PIPE_ALIGNED, a gfx10-only metadata hint
 *   w6 bit 22 = 0                        ALPHA_IS_ON_MSB, a gfx10-only DCC hint; gfx12's bit 22 is COMPRESSION_ACCESS_MODE,
 *                                        which must not inherit it (the generated port leaves gfx12 [23:22] zero)
 *   w6 [16:15] 2 -> 1                    MAX_UNCOMPRESSED_BLOCK_SIZE 256B by MEANING: Mesa's gfx10 builder writes
 *                                        V_028C78_MAX_BLOCK_SIZE_256B (2), its gfx12 builder writes 1 (256B)
 *   COMPRESSION_EN KEPT (1)              inert on gfx12 unless the page's PTE bit 58 is set; clearing it is the one choice
 *                                        that reads raw compressed bytes if it is set (DCC-DESC.md Q2)
 *   MAX_COMPRESSED_BLOCK_SIZE kept       both Mesa builders take the same surface value
 * then the generated port as always (so FORMAT/SW_MODE/every other field is translated and refused exactly as before). A
 * stripped record sets *dropped bit 2 (4). In the table step a stripped T# is ADMITTED ONLY through ex->desc_dcc_ok (below;
 * NULL refuses), NEVER through desc_tiled_ok - whatever its gfx12 SW_MODE, linear included - and an unproven one refuses
 * XLAT12_TDESC_PROVENANCE naming its surface exactly as a tiled T# does. Counted in dcc_stripped / dcc_unproven /
 * dcc_refused. OFF, not one output dword, counter or return code changes: the strip is not reached. */
#define XLAT12_EXTRA_DCC_STRIP 0x80000u
/* build 0.0.500 (an internal design note Q4, ; switch 66 in the kext) — XLAT12_EXTRA_DRAW_ELIDE,
 * DEFAULT OFF; valid only together with XLAT12_EXTRA_TABLE_DESC and with `ex->draw_elide_rows` a non-empty subset of
 * XLAT12_DE_CLASS_UY | XLAT12_DE_CLASS_AO | XLAT12_DE_CLASS_GLASS (XLAT12_ERR_ARG otherwise; build 0.0.552: or of those
 * and ONE of XLAT12_DE_CLASS_AN / XLAT12_DE_CLASS_AN_SHADOW, below). Nothing in the kext sets it unless
 * `accel gfxneuter 66` is ON (M 1 = 322: U/Y; M 3 = 834: U/Y and AO; build 0.0.512: M 7 = 1858: U/Y, AO and the glass rows).
 *
 * WHAT IT CHANGES. A table draw the table step refused XLAT12_TDESC_PROVENANCE is replaced by a PACKET3 NOP of the draw's own
 * length (0xC0001000 | (len - 2) << 16, then zeros) - it runs no vertex or fragment program, writes no colour and reads no
 * memory - and the translation carries on with the next region, ONLY when ALL of these hold, decided at the table step's
 * return and nowhere else (d_table_desc refuses PROVENANCE inside its texture loop, before its sampler loop and before any
 * placement, redirect or write to `out`; so nothing of this draw's table exists yet):
 *   1  the flag is set, the step returned XLAT12_IB_ERR_DESC with err_op XLAT12_TDESC_PROVENANCE;
 *   2  on the row's LAST texture, every earlier one proven: in_n == ntex, !in_proven[ntex - 1], in_va[ntex - 1] == prov_va,
 *      in_proven[i] for every i < ntex - 1;
 *   3  the in-force fragment program's kDTableAbi row is in kDrawElideRows under an enabled class - U {192, 0x92c6ae13} and
 *      Y {372, 0x5276813b} (class UY, the refused texture must be a stripped DCC record: dcc_unproven rose in this step),
 *      AO {168, 0xb4fc3c24} (class AO) - programs whose gfx1201 objects hold no store or atomic (`export mrt0` only);
 *   4  the write set, from THIS translation's own packets before the draw: DB_Z_INFO FORMAT 0 and DB_STENCIL_INFO FORMAT 0
 *      (both written), VGT_STRMOUT_CONFIG 0 (written), and CB_COLORn_BASE (n = 1..7) written 0 or, unwritten, masked off
 *      by a written CB_TARGET_MASK; no register-destination WRITE_DATA / COPY_DATA / WAIT_REG_MEM and no LOAD_CONTEXT_REG
 *      before the draw (a register this scan cannot see);
 *   5  fewer than XLAT12_DRAW_ELIDE_MAX draws elided in this translation.
 * Then the exported inputs are emptied (d_in_clear: the draw reads nothing), err_op / err_in_dword / prov_* are cleared, the
 * NOP goes where the draw would have (a unit's through the unit's own draw step), and the draw's fill-colour step and
 * read-set export are skipped. A draw that fails any part refuses exactly as with the flag off, and is counted by the first
 * part that failed. BACKSTOP: a translation that elided anything must end with exactly `draws - draw_elided` draw packets
 * in its output (xlat12_ib_count_draws), else XLAT12_IB_ERR_VERIFY, err_op XLAT12_DE_BACKSTOP. OFF, not one output dword,
 * counter or return code changes: nothing here is reached. */
#define XLAT12_EXTRA_DRAW_ELIDE 0x100000u
#define XLAT12_DRAW_ELIDE_MAX   2u
#define XLAT12_DE_CLASS_UY      0x1u
#define XLAT12_DE_CLASS_AO      0x2u
/* build 0.0.512 (; switch 66 M 7 = 1858 in the kext) — XLAT12_DE_CLASS_GLASS: the clock's glass passes.
 *   BD {1271, 0x3858ea3a} ws_BD_glass_background_lph, require_dcc, the LAST-texture rule above (its texture 1 is the clock's
 *      frozen signed-distance surface: run10t 0x402380000, run10s 0x4015e0000 - named by the ROLE, never by a VA);
 *   BA {120, 0x8e1812e4} ws_BA_TdfgXh_Isrc, require_dcc, THE NOT-LAST CASE: part 2 is replaced, for this row only, by
 *      "the refusal is PROVENANCE on texture 0 (tex0, s10: the SDF slot), and EVERY other texture of the row (tex1, s12: the
 *      256x1 colour ramp) was read, translated and PROVEN by the same ask" - asked by the table step itself, right at the
 *      refusal (d_de_probe_rest: no byte of `out` written, no counter moved, exports untouched). Any other unproven, unread
 *      or refused texture: refused exactly as today (not-last).
 * Both programs are store-free (CONFIRMED on their gfx1201 objects, re/pc-26.6.2/xlat/windowserver-r2/work/{BD,BA}/<name>.pal.o,
 * `llvm-objdump -d --mcpu=gfx1201`: BD image_sample 2, image_sample_l 5, global_load_d16(_hi)_b16 9,
 * ds_param_load 4, export mrt0 2; BA image_sample 2, ds_param_load 2, export mrt0 1; no *_store*, *_atomic* or scratch op). An elided BA writes nothing: its target (the clock layer 0x401080000, written only by BA -)
 * is never ledgered and frame a's U, which reads it, stays refused (and elided). */
#define XLAT12_DE_CLASS_GLASS   0x4u
#define XLAT12_DE_ROW_U         1u
#define XLAT12_DE_ROW_Y         2u
#define XLAT12_DE_ROW_AO        3u
#define XLAT12_DE_ROW_BD        4u
#define XLAT12_DE_ROW_BA        5u
/* build 0.0.552 ( PLAN (2); switch 110 in the kext) — THE AN ROW, A DIAGNOSTIC: is AN (identity 62,
 * ws_AN_TmuaXh_Isrc_Isrc, kDTableAbi {122, 0x7b3a6dfe}) the writer of the black date / message boxes? Two class bits, never both
 * (XLAT12_ERR_ARG), each valid only beside the flag like every class:
 *   XLAT12_DE_CLASS_AN         ON: a draw whose in-force fragment program is exactly AN's identity is replaced by a same-length
 *                              NOP BEFORE its table step - whether or not its textures would prove (AN's draws COMMIT today,,
 *                              so the refusal-side rule above would never reach them) - when the write set (part 4) holds and
 *                              fewer than XLAT12_DRAW_ELIDE_MAX draws were elided (part 5). The table step is never entered, so the
 *                              state it leaves is the refusal-side elide's own (nothing placed, redirected or exported: d_in_clear).
 *                              Counted like every elision (draw_elided, de_row XLAT12_DE_ROW_AN, de_va8 0: no surface refused).
 *   XLAT12_DE_CLASS_AN_SHADOW  SHADOW: the same predicate is ASKED and a yes is only counted (de_an_would); the draw then goes to its
 *                              table step exactly as without the bit - not one output dword, return code or other counter changes.
 * The AN row is NOT in the refusal-side table (kDrawElideRows): no class bit here can elide any other program, or AN at a refusal.
 * Any other identity (AI {119, 0xae6c50d3}, AN's own function compiled a second time, included) is never matched. */
#define XLAT12_DE_CLASS_AN        0x8u
#define XLAT12_DE_CLASS_AN_SHADOW 0x10u
#define XLAT12_DE_ROW_AN          6u
#define XLAT12_DE_AN_NDW          122u
#define XLAT12_DE_AN_FNV          0x7b3a6dfeu
#define XLAT12_DE_BACKSTOP     0xDEu   /* err_op with XLAT12_IB_ERR_VERIFY: the output's draw count is not draws - draw_elided */
/* build 0.0.500: the draw packets (the translator's own draw opcodes) in out[0..n), walking packet lengths; a NOP body is
 * skipped whole. Pure. Used by the translator's backstop and by the kext's own. */
uint32_t xlat12_ib_count_draws(const uint32_t *out, uint32_t n);
/* build 0.0.500: the translator's backstop, exported for its tests: 1 (and err_op XLAT12_DE_BACKSTOP) when out[0..n)
 * does not carry exactly ds->draws - ds->draw_elided draw packets (or draw_elided > draws); 0 otherwise. */
uint32_t xlat12_ib_de_backstop(const uint32_t *out, uint32_t n, xlat12_draw_stats *ds);
/* build 0.0.535 ( fix 1; switch 91 in the kext) — XLAT12_EXTRA_NCLEAR, DEFAULT OFF; valid only together with
 * XLAT12_EXTRA_CS_ELIDE (XLAT12_ERR_ARG otherwise). Nothing in the kext sets it unless `accel gfxneuter 91 | 1 << 8` is ON.
 *
 * WHY. N (the compute DCC clear 57 elides) was Apple's fast clear to 0000: on gfx10 the target then READ as transparent black
 * wherever a pass did not overwrite it. The translator drops DCC (CB_COLOR0_DCC_BASE ABSENT, DCC_ENABLE cleared), so with the
 * dispatch elided the target is never cleared at all.
 *
 * WHAT IT CHANGES. Where 57 elides a PROVEN N dispatch (P1-P5, XLAT12_EXTRA_CS_ELIDE above), the NOP is replaced by a CP DMA_DATA
 * constant fill of ZERO over exactly [CB_COLOR0_BASE << 8, CB_COLOR0_DCC_BASE << 8) of the colour target N's DCC buffer belongs
 * to - only when ALL of these hold, else 57's NOP stands (counted in nc_ref_*):
 *   E1 the extent, from the window's own writes (the dispatch's end up to the next draw) at a packet boundary where P5's bind
 *      holds (CB_COLOR0_INFO DCC_ENABLE, CB_COLOR0_DCC_BASE == N's V# base >> 8): CB_COLOR0_BASE, CB_COLOR0_INFO,
 *      CB_COLOR0_ATTRIB, CB_COLOR0_VIEW, CB_COLOR0_ATTRIB2 and CB_COLOR0_ATTRIB3 all written in the window; CB_COLOR0_BASE_EXT
 *      and CB_COLOR0_DCC_BASE_EXT 0 or unwritten; ATTRIB 0 (one sample, one fragment), VIEW 0 (slice 0), ATTRIB2 MAX_MIP 0,
 *      ATTRIB3 MIP0_DEPTH 0, RESOURCE_TYPE 1 (2D) and COLOR_SW_MODE 27 (SW_64KB_R_X); FORMAT 10 (8_8_8_8, 4 bytes) or 12
 *      (16_16_16_16, 8 bytes); and the two derivations of the size AGREE: DCC_BASE - BASE == align(W, bw) * align(H, bh) * bpp
 *      with the 64 KiB block of that element size (128x128 at 4 bytes, 128x64 at 8). Every boundary that holds must name
 *      the same extent (else refused). All five targets satisfy it on run11v's captures (tests/gfx_nclear_test.cpp).
 *   E2 the room: the fill's packets replace the output of N's own contiguous compute-register run (the SET_SH_REG /
 *      SET_SH_REG_INDEX packets writing only [0xb800, 0xba00) that END at the dispatch, walked and re-checked in the output)
 *      plus the dispatch's own five dwords, and the fill's end never passes the dispatch's end in the INPUT (i + 5): the
 *      output never runs ahead of Apple's stream and the IB never grows. Measured on run11v: 73 dwords (10 packets).
 *   E3 the packets: ceil(size / XLAT12_NCLEAR_MAX_BYTES) DMA_DATA packets, 7 dwords each, XLAT12_NCLEAR_CTRL (ENGINE_SEL 0 =
 *      micro engine, DST_SEL 3 = dst_addr_using_l2, SRC_SEL 2 = data, CP_SYNC 1), data 0, src_hi 0, the destination, and
 *      COMMAND = BYTE_COUNT only (SAS/DAS memory, SAIC/DAIC increment, RAW_WAIT 0, DIS_WC 0). Encoding from AMD's own gfx12
 *      PM4 table (mesa src/amd/packets/cp_pm4_table_data_gfx12.json, DMA_DATA words 2-7); the per-packet cap from mesa's two
 *      drivers (radeonsi si_cp_dma.c and radv radv_cp_dma.c: `gfx_level >= GFX11 ? 32767`, aligned to SI_CPDMA_ALIGNMENT 32).
 *      DST_SEL 3 is what radeonsi's emitter writes on GFX12, where its comment says TC_L2 there means MALL - it does NOT make
 *      the write L2-coherent: mesa sets cp_sdma_ge_use_system_memory_scope and cp_dma_use_L2 = false for GFX12
 *      (ac_gpu_info.c), and radeonsi never CP-DMA-clears on gfx12 (0.0.535 review). CP_SYNC on every packet makes the micro
 *      engine wait for each fill to complete before the next packet; VISIBILITY to the colour pipe rests on APPLE's own
 *      barriers, so the fill is made only when both are in its input (E4 below).
 *   E4 (fix round (a)) the barriers: before N's compute run the end-of-pipe triplet (RELEASE_MEM 0x514 to a fence, a memory
 *      WAIT_REG_MEM equal on it), then only CS_PARTIAL_FLUSH, then an ACQUIRE_MEM with GCR_CNTL GL2_WB (bit 15) immediately
 *      before the run; after the dispatch, its next two packets CS_PARTIAL_FLUSH and an ACQUIRE_MEM with GL2_INV (bit 14),
 *      before the next draw (GCR_CNTL = ACQUIRE_MEM dword 7, mesa pkt3.json). Else 57's NOP (nc_ref_barrier).
 *   E5 (fix round (b)) at most XLAT12_NCLEAR_MAX fills per translation; a later proven N keeps 57's NOP (nc_ref_max).
 * Nothing else changes: every packet outside N's compute run and dispatch is emitted exactly as under 57 alone. The
 * verify (xlat12_ib_draw_verify) still refuses every DMA_DATA; the translation's own verify admits exactly this packet
 * form under this flag (xlat12_ib_nclear_pkt_ok). OFF, not one output dword, counter or return code changes. */
#define XLAT12_EXTRA_NCLEAR      0x200000u
#define XLAT12_NCLEAR_HDR        0xC0055000u   /* PACKET3(DMA_DATA 0x50, count 5): 7 dwords */
#define XLAT12_NCLEAR_CTRL       0xC0300000u   /* CP_SYNC [31] | SRC_SEL 2 (data) [30:29] | DST_SEL 3 (L2) [21:20] | ENGINE 0 */
#define XLAT12_NCLEAR_MAX_BYTES  32736u        /* mesa GFX11+: 32767 & ~(SI_CPDMA_ALIGNMENT 32 - 1) */
#define XLAT12_NCLEAR_MAX        2u            /* fills recorded per translation (nc_va8 / nc_len); more still fill, counted */
#define XLAT12_NCLEAR_BACKSTOP   0xDCu         /* err_op with XLAT12_IB_ERR_VERIFY: the kext's check of the fills (below) failed */
/* 1 when p[0..l) is exactly one NCLEAR fill packet (XLAT12_NCLEAR_HDR, _CTRL, data 0, src_hi 0, a 32-byte aligned destination
 * below 2^48, COMMAND a byte count in (0, XLAT12_NCLEAR_MAX_BYTES] and a multiple of 4, nothing else). Pure; static inline so
 * the kext's R1 classifier (gfx_memdst.h) shares it without a link dependency (fix round (c)). */
static inline int xlat12_ib_nclear_pkt_ok(const uint32_t *p, uint32_t l)
{
    if (!p || l != 7u || p[0] != XLAT12_NCLEAR_HDR || p[1] != XLAT12_NCLEAR_CTRL || p[2] || p[3]) return 0;
    if ((p[4] & 31u) || p[5] >= 0x10000u) return 0;
    return p[6] != 0u && p[6] <= XLAT12_NCLEAR_MAX_BYTES && !(p[6] & 3u);
}
/* build 0.0.536 ( defect 2; switch 92 in the kext) — XLAT12_EXTRA_RECT2D, DEFAULT OFF. Nothing in the kext sets it
 * unless `accel gfxneuter 92 | 1 << 8` is ON.
 *
 * WHY. Apple's rectangle draws (VGT_PRIMITIVE_TYPE 0x30908 = DI_PT_RECTLIST 17: WindowServer's Const_PS clears and Tex_PS LUT
 * draws, DRAW_INDEX_AUTO of 3 vertices) rasterize, on gfx11+ NGG, only with VGT_GS_OUT_PRIM_TYPE (uconfig 0x30998) = RECT_2D:
 * Mesa radeonsi si_pipe.h `sctx->gs_out_prim = sctx->gfx_level >= GFX11 ? V_030998_RECT_2D : V_028A6C_RECTLIST` and radv
 * radv_pipeline_graphics.h `case V_008958_DI_PT_RECTLIST: return is_ngg ? (gfx_level >= GFX11 ? V_030998_RECT_2D : ...`. The
 * translator writes the profile's TRISTRIP (2) for every Apple 0x28a6c write (d_reg), so the rectangle's third vertex is never
 * extrapolated and the clear never covers its target.
 *
 * WHAT IT CHANGES. Decided at each DRAW (Apple writes 0x28a6c BEFORE 0x30908 in a segment, so the register write cannot decide):
 * the draw's VGT_PRIMITIVE_TYPE in force is the last one THIS translation wrote (PRIM_TYPE [5:0]); VGT_GS_OUT_PRIM_TYPE in force
 * is the last value this translation wrote to 0x30998 (d_reg's override of Apple's 0x28a6c, or this rule's own write).
 *   - RECTLIST draw, 0x30998 not already RECT_2D here: one SET_UCONFIG_REG 0x30998 = XLAT12_R2D_OUTPRIM (3) placed LAST in the
 *     draw's region (after the re-emission and the extra block, before the pad), i.e. immediately before the draw.
 *   - any other draw with RECT_2D (this translation's own write) in force: the same packet with the profile's own
 *     gs_out_prim_type (2, TRISTRIP). A non-RECTLIST draw with no VGT_GS_OUT_PRIM_TYPE of this translation in force is left
 *     as today: in run11y and run11v every non-RECTLIST draw after a RECTLIST draw follows an Apple 0x28a6c write in the same
 *     IB, and every IB whose first draw has no 0x28a6c before it opens with a RECTLIST draw (d_r2d_draw has the census).
 *   The packet takes 3 dwords of the region's own pad (the in-place contract: the IB never grows). No room, or ANY refusal of
 *   the flagged translation after it wrote, and xlat12_ib_translate_draw_ex translates the segment again WITHOUT the flag -
 *   today's output byte for byte - and counts it (r2d_noroom / r2d_fallback). The translation's own backstop
 *   (xlat12_ib_rect2d_check) runs over the flagged output. Refused (XLAT12_ERR_ARG) with XLAT12_EXTRA_APPLE_OUTPRIM or
 *   XLAT12_EXTRA_SYNTH_IDXPRIM: one register, two policies. OFF, not one output dword, counter or return code changes.
 * Values: gfx12.json (re/graphics/src/mesa/src/amd/registers) enum VGT_GS_OUTPRIM_TYPE `{"name": "RECT_2D", "value": 3}`,
 * enum VGT_DI_PRIM_TYPE `{"name": "DI_PT_RECTLIST", "value": 17}`; VGT_GS_OUT_PRIM_TYPE at 199064 = 0x30998, field OUTPRIM_TYPE
 * [5:0]; VGT_PRIMITIVE_TYPE at 198920 = 0x30908, field PRIM_TYPE [5:0]. */
#define XLAT12_EXTRA_RECT2D        0x400000u
#define XLAT12_R2D_OUTPRIM         3u       /* VGT_GS_OUTPRIM_TYPE RECT_2D */
#define XLAT12_R2D_PRIM_RECTLIST   0x11u    /* VGT_DI_PRIM_TYPE DI_PT_RECTLIST (17) */
#define XLAT12_R2D_NO_ROOM         0xD2u    /* err_op with XLAT12_IB_ERR_TOO_LONG: the 0x30998 write found no room (then: fallback) */
#define XLAT12_R2D_BACKSTOP        0xD3u    /* err_op with XLAT12_IB_ERR_VERIFY: xlat12_ib_rect2d_check refused the output */
/* The backstop, exported for the kext and the tests: walks out[0..n) as the GPU would see it and returns 0 when every
 * VGT_GS_OUT_PRIM_TYPE write is either `keep` (the profile's value) or RECT_2D, and every draw that executes with RECT_2D in
 * force has a RECTLIST VGT_PRIMITIVE_TYPE in force, both written in out[0..n), and RECT_2D is NOT in force at the end of out[0..n)
 * (fix round: nothing after this output - another segment, IB or frame - can run under it); 1 otherwise (including an unwalkable
 * stream). Accepts RECT_2D ONLY for RECTLIST draws. The translator never restores at the end: a flagged output that would end under
 * RECT_2D is refused by this check and falls back to today's translation (0 such segments in run11y/run11v). Pure. */
uint32_t xlat12_ib_rect2d_check(const uint32_t *out, uint32_t n, uint32_t keep);
/* build 0.0.548 item B: the same check, also answering WHERE (*at) it refused: the OUTPUT dword of the refusing packet in out[],
 * or n for the end rule (RECT_2D still in force at the end); 0 when accepted. The flagged pass's XLAT12_R2D_BACKSTOP refusal stores it
 * in err_in_dword (so, for err_op 0xD3 ONLY, err_in_dword / r2d_on_dw is an OUTPUT dword of the candidate, not an input dword). */
uint32_t xlat12_ib_rect2d_check_at(const uint32_t *out, uint32_t n, uint32_t keep, uint32_t *at);
/* build 0.0.547 item 3 ( ranked fix (3); the kext's switch 105 "rectfb", gfx_rectfb105.h): HOW MANY DRAWS OF out[0..n)
 * EXECUTE WITH A RECTLIST VGT_PRIMITIVE_TYPE IN FORCE (written in out[0..n), the same reading d_r2d_draw and the backstop use).
 * Pure, read-only. `*last_is_rect` (optional) is 1 when the LAST draw of the output is one of them (the end rule's signature: a
 * flagged pass that ends under RECT_2D is refused by the backstop and falls back). An unwalkable stream answers
 * XLAT12_RL_UNWALKED, which a caller must treat as "may contain one" (fail closed). A fallback output (flag clear) that returns
 * > 0 carries a RECTLIST draw that the GPU rasterises under the profile's TRISTRIP: ONE triangle, half the rectangle. */
#define XLAT12_RL_UNWALKED         0xFFFFFFFFu
#define XLAT12_RFB_REFUSED         0xD5u    /* err_op with XLAT12_IB_ERR_VERIFY: the kext's switch 105 refused a RECTLIST fallback */
uint32_t xlat12_ib_rectlist_draws(const uint32_t *out, uint32_t n, uint32_t *last_is_rect);
/* build 0.0.537 (; switch 93 in the kext) — XLAT12_EXTRA_PWS, DEFAULT OFF. Nothing in the kext sets it unless
 * `accel gfxneuter 93 | 1 << 8` is ON.
 *
 * WHY. Apple's barrier between a colour-target write and a later read of it is `EVENT_WRITE 0x16` + this ACQUIRE_MEM:
 *   C0065800 86287FC3 FFFFFFFF 000000FF 00000000 00000000 0000000A 0001C3F1
 * Word 1 is gfx10's CP_COHER_CNTL (bit 31 ENGINE_SEL micro_engine; gfx103.json names only CB_ACTION_ENA [25] and DB_ACTION_ENA
 * [26] in the low 31 bits). The 0x7fc3 bits are RESOLVED by the older register files (xhigh review, ; gfx6/7/8.json
 * CP_COHER_CNTL): the classic surface-sync destination enables DEST_BASE_0_ENA [0], DEST_BASE_1_ENA [1], CB0..CB7_DEST_BASE_ENA
 * [6..13], DB_DEST_BASE_ENA [14], DEST_BASE_2_ENA [19], DEST_BASE_3_ENA [21], with CB_ACTION_ENA [25], DB_ACTION_ENA [26] (0x86287FC3 =
 * those thirteen enables + the two actions + bit 31, which the gfx12 PM4 table reads as the packet's ENGINE_SEL and gfx8.json names
 * SH_SD_ACTION_ENA) - "every CB and DB surface, full range": exactly the CB + DB flush the conversion's event 0x14 performs. Kept as Apple wrote them in the verbatim copy, dropped by the conversion. CP_COHER_CNTL is ABSENT from gfx11.json and gfx12.json, and word 6's PWS_ENA (bit 31) is clear,
 * so on gfx12 the packet is a cache operation with NO WAIT for the colour writes before it (: the capsule's squares).
 *
 * WHAT IT CHANGES. In d_region's pass-through branch, an 8-dword ACQUIRE_MEM whose words 1-7 are exactly the above becomes, in
 * place, the two packets radeonsi and radv emit for such a barrier on gfx11+ (si_barrier.c gfx10_emit_barrier / radv_cs.c
 * gfx10_cs_emit_cache_flush: ac_emit_cp_release_mem_pws(cb_db_event, gcr & C_587_GLI_INV) then
 * ac_emit_cp_acquire_mem_pws(cb_db_event, CP_ME, 0, gcr & ~C_587_GLI_INV)), 16 dwords for 8:
 *   RELEASE_MEM  C0064900 C1704514 00000000 00000000 00000000 00000000 00000000 00000000
 *   ACQUIRE_MEM  C0065800 00022800 FFFFFFFF 01FFFFFF 00000000 00000000 80000000 00000001
 * DERIVATION A (built; the reviewer's decision after the builder's stop): Mesa's emit functions applied to APPLE's barrier as
 * written - CB_ACTION_ENA and DB_ACTION_ENA both, so the event is CACHE_FLUSH_AND_INV_TS_EVENT (0x14; gfx12.json VGT_EVENT_TYPE 20;
 * si_barrier.c picks it when CB and DB are both flushed), and Apple's GCR_CNTL 0x1C3F1 (pkt3.json GCR_CNTL: GLI_INV 1 [1:0],
 * GLM_WB [4], GLM_INV [5], GLK_WB [6], GLK_INV [7], GLV_INV [8], GL1_INV [9], GL2_INV [14], GL2_WB [15], SEQ 1 forward [17:16]):
 *   RELEASE_MEM (cp_pm4_table_data_gfx12.json RELEASE_MEM word 2: event_type [5:0], event_index [11:8], gcr_cntl [24:12] laid out
 *   as pkt3.json RELEASE_MEM_OP_gfx11, glk_inv [30], pws_enable [31]): event 0x14 | index 5 (end_of_pipe: a TS event) << 8 |
 *   GLV_INV [14] | GL2_INV [20] | GL2_WB [21] | SEQ 1 [23:22] | GLK_WB [24] | GLK_INV [30] | PWS_ENABLE [31] = C1704514; GLM and
 *   GL1 are dropped exactly as ac_emit_cp_release_mem_pws drops them for gfx_level >= GFX12; words 2-7 zero (DST_SEL / INT_SEL /
 *   DATA_SEL 0: no memory is written, no interrupt).
 *   ACQUIRE_MEM (word 2 b: pws_stage_sel [13:11] cp_me 5, pws_counter_sel [15:14] ts_select 0, pws_ena2 [17], pws_count [23:18] 0
 *   = 00022800; gcr_size FFFFFFFF, gcr_size_hi 01FFFFFF, gcr_base 0 0 exactly as ac_emit_cp_acquire_mem_pws; word 7 b pws_ena
 *   [31] = 80000000; word 8 gcr_cntl = only Apple's GLI_INV = 00000001). Stage CP_ME because Apple's ENGINE_SEL is micro_engine
 *   (no PFP_SYNC_ME).
 * DERIVATION B (NOT built): Mesa's own flags for a pure CB -> texture barrier (si_texture_barrier -> si_make_CB_shader_coherent:
 * on gfx12 only SYNC_AND_INV_CB | INV_VMEM) would give event FLUSH_AND_INV_CB_DATA_TS (0x2d) with GLV_INV | SEQ forward only:
 *   RELEASE_MEM  C0064900 8040452D 0 0 0 0 0 0      ACQUIRE_MEM  C0065800 00022800 FFFFFFFF 01FFFFFF 0 0 80000000 00000000
 * A is a strict superset of B (a TS wait for CB and DB, plus Apple's own GL2 / GLK / GLI operations): it keeps everything Apple's
 * barrier asked for and adds only the missing wait. Both use the TS counter with count 0 at CP_ME.
 * The pair is placed where the barrier was, so the release follows every draw before it and the acquire precedes every draw
 * after it. ROOM: 8 more dwords than the input, taken from the region's own pad before its draw (the IB never grows). The
 * conversion is made only in a region that ENDS AT A DRAW with at least 16 dwords left at the barrier; the region after the last
 * draw is never used (its trailer carries Apple's buried fence828 RELEASE_MEM, whose offset R1's fence identity requires to be
 * Apple's), and (fix round item 3) neither is a barrier with that buried packet later in its own region (a per-draw trailer: the pair
 * would move the packet 8 dwords off Apple's offset; counted `pws_fence`). No room: the barrier is copied verbatim and counted (`pws_noroom`), never refused. If the flagged translation is
 * refused after it converted (a later packet of the region no longer fits, or anything else), the translation runs again without
 * the conversion - today's output, byte for byte - with every barrier counted as no-room and `pws_fallback` 1 (never a refusal
 * this flag caused). The compute barriers (words 1 a8c40000 / 80c40000) and 86007fc0 are never touched (`pws_other`). OFF, not
 * one output dword, counter or return code changes. */
#define XLAT12_EXTRA_PWS           0x800000u
#define XLAT12_PWS_APPLE_W1        0x86287FC3u   /* Apple's barrier, words 1..7 (CP_COHER_CNTL ... GCR_CNTL) */
#define XLAT12_PWS_APPLE_W7        0x0001C3F1u
#define XLAT12_PWS_REL_HDR         0xC0064900u   /* PACKET3(RELEASE_MEM, count 6) */
#define XLAT12_PWS_REL_W1          0xC1704514u
#define XLAT12_PWS_ACQ_HDR         0xC0065800u   /* PACKET3(ACQUIRE_MEM, count 6) */
#define XLAT12_PWS_ACQ_W1          0x00022800u
#define XLAT12_PWS_ACQ_W6          0x80000000u
#define XLAT12_PWS_ACQ_W7          0x00000001u
#define XLAT12_PWS_BACKSTOP        0xD4u         /* err_op with XLAT12_IB_ERR_VERIFY: the kext's pairing check refused the output */
/* build 0.0.539 ( build spec,) — THE SLOT REUSE, under XLAT12_EXTRA_PWS (switch 93) only. Every segment head
 * Apple encodes is `EVENT_WRITE 0x16` + this barrier + `EVENT_WRITE 0xE` + an 8-dword NOP `C0061000 C0051000 0 ...`: Apple's DISABLED
 * top-of-pipe wait slot (-[GFX10_MtlCmdBuffer amdMtl_HWL_AllocateTopOfPipeWait], AMDRadeonX6000MTLDriver 0x7ffb111dc182; enabled only
 * during encoding at EndEncoder, when it becomes `FFFF1000 C0053C00 ...`, a 1-dword NOP and a WAIT_REG_MEM; 0 enabled slots in
 * run11v/y/z/ac; nothing of ours reads its body by address,). Where d_pws has CONVERTED the barrier and the 4 dwords right after
 * it (in[i+8..i+11], inside the region) are exactly EW0 EW1 SLOT0 SLOT1 below, d_region marks the slot (input dword i+10), copies the
 * EVENT_WRITE as always, and its NOP branch then DROPS exactly that slot (emits nothing, counts `pws_slot`):
 *   input   [ACQUIRE_MEM 8][EVENT_WRITE 0xE 2][NOP slot 8]      = 18 dwords
 *   output  [RELEASE_MEM(PWS) 8][ACQUIRE_MEM(PWS) 8][EVENT_WRITE 0xE 2] = 18 dwords
 * so every output dword from the barrier's + 18 on is the one the same pass would have written with the barrier unconverted: the
 * capsule barrier (run11z F85 IB0 7378,) and every other slotted head then convert under the real recipe with ZERO growth.
 * THE ROOM CREDIT COMES ONLY AFTER THE MATCH: d_pws still needs its 16 dwords at the barrier (the pair is written there, as 0.0.537);
 * the slot's 8 are given back only when the NOP branch meets the marked dword and re-matches it (SLOT0 SLOT1, length 8) - never assumed
 * at the barrier. An ENABLED slot (FFFF1000 C0053C00 ...), a stale-bodied disabled slot, a head without the EVENT_WRITE 0xE between
 * the barrier and the NOP, a no-room / fence / tail / fallback-pass barrier: never marked (the first is kept, the second DROPPED - its
 * body is never executed, the header says so - and the rest are verbatim as 0.0.537). The mark is cleared at the end of every region.
 * OFF (the flag unset), in a fallback pass or at an unconverted barrier nothing here runs. */
#define XLAT12_PWS_SLOT_EW0        0xC0004600u   /* EVENT_WRITE, 2 dwords */
#define XLAT12_PWS_SLOT_EW1        0x0000000Eu   /* its event: 0xE */
#define XLAT12_PWS_SLOT0           0xC0061000u   /* PACKET3(NOP, count 6): 8 dwords */
#define XLAT12_PWS_SLOT1           0xC0051000u   /* the disabled slot's first body dword */
/* 1 when p[0..l) is Apple's DISABLED top-of-pipe wait slot: an 8-dword NOP C0061000 C0051000 (its body is never executed, so it is
 * not read). The one matcher of the slot, at the mark (xlat12_ib_pws_slot_at) and again at the drop (d_region's NOP branch). Pure. */
static inline int xlat12_ib_pws_slot_pkt(const uint32_t *p, uint32_t l)
{
    return p && l == 8u && p[0] == XLAT12_PWS_SLOT0 && p[1] == XLAT12_PWS_SLOT1;
}
/* 1 when the barrier at p[0] (8 dwords) is followed, within `avail` input dwords from p[0] (the region's end - the barrier), by
 * Apple's EVENT_WRITE 0xE and its disabled top-of-pipe wait slot: p[8..11] == EW0 EW1 SLOT0 SLOT1 and the slot's 8 dwords end at or
 * before the region's end (avail >= 18). Pure. */
static inline int xlat12_ib_pws_slot_at(const uint32_t *p, uint32_t avail)
{
    return p && avail >= 18u && p[8] == XLAT12_PWS_SLOT_EW0 && p[9] == XLAT12_PWS_SLOT_EW1 && xlat12_ib_pws_slot_pkt(&p[10], 8u);
}
/* 1 when p[0..l) is exactly Apple's CB/DB barrier ACQUIRE_MEM (the 8 dwords above). Pure. */
static inline int xlat12_ib_pws_apple_ok(const uint32_t *p, uint32_t l)
{
    return p && l == 8u && p[0] == XLAT12_PWS_ACQ_HDR && p[1] == XLAT12_PWS_APPLE_W1 && p[2] == 0xFFFFFFFFu && p[3] == 0x000000FFu &&
           p[4] == 0u && p[5] == 0u && p[6] == 0x0000000Au && p[7] == XLAT12_PWS_APPLE_W7;
}
/* 1 when p[0..l) is exactly OUR RELEASE_MEM(PWS) (all 8 dwords). Pure; static inline so the kext's R1 classifier
 * (gfx_memdst.h n48_md_classify) recognises exactly this packet - and nothing else - as not a memory destination. */
static inline int xlat12_ib_pws_release_ok(const uint32_t *p, uint32_t l)
{
    return p && l == 8u && p[0] == XLAT12_PWS_REL_HDR && p[1] == XLAT12_PWS_REL_W1 && p[2] == 0u && p[3] == 0u && p[4] == 0u &&
           p[5] == 0u && p[6] == 0u && p[7] == 0u;
}
/* 1 when p[0..l) is exactly OUR ACQUIRE_MEM(PWS) (all 8 dwords). Pure. */
static inline int xlat12_ib_pws_acquire_ok(const uint32_t *p, uint32_t l)
{
    return p && l == 8u && p[0] == XLAT12_PWS_ACQ_HDR && p[1] == XLAT12_PWS_ACQ_W1 && p[2] == 0xFFFFFFFFu && p[3] == 0x01FFFFFFu &&
           p[4] == 0u && p[5] == 0u && p[6] == XLAT12_PWS_ACQ_W6 && p[7] == XLAT12_PWS_ACQ_W7;
}
/* build 0.0.540 item 5 (the host Mac benchmark bench540: ~95% of the translator's CPU time was d_table_g12, the output verifier's
 * per-register linear walk of all 668 register-table rows, each through xlat12's lookup()) — THE VERIFIER'S TABLE CACHE, under the
 * kext's switch 97 (OFF by default). With this flag, d_verify asks d_table_g12's question of a SORTED CACHE of the table's gfx12
 * destinations (the same rows, the same class mask: IDENTICAL / MOVED / FIELD_REPACK / REUSED, override included, exactly the walk's
 * xlat12_table_entry answers) by binary search. The cache is built ONCE, the first time a flagged translation verifies, into
 * file-scope storage of XLAT12_TC_CAP entries: no allocation, no lock, no wait - one builder claims it by compare-and-swap, and any
 * translation that finds it unbuilt, being built, or unable to hold the table answers from the linear walk (today's answer). The
 * table is const, so a built cache never goes stale. OFF (the flag unset): d_verify calls d_table_g12 exactly as 0.0.539; not one
 * output dword, counter or return code changes, ON or OFF (the answers are the same function). */
#define XLAT12_EXTRA_TBLCACHE      0x1000000u
#define XLAT12_TC_CAP              1024u
/* For tests: the linear walk's and the cache's answers for gfx12 byte address `a` (*linear, *cached: 1 = allowed by the table),
 * building the cache if it is not built; returns the cache's state (2 = built and used; anything else = the cache answered from the
 * linear walk). Pure apart from the one-time build. */
uint32_t xlat12_ib_tblcache_answer(uint32_t a, int *linear, int *cached);
/* For tests: xlat12_ib_draw_verify with the translation's flags (only XLAT12_EXTRA_TBLCACHE and XLAT12_EXTRA_NCLEAR are read). */
uint32_t xlat12_ib_draw_verify_ex(const uint32_t *out, uint32_t n, uint32_t flags, uint32_t *bad_addr, uint32_t *bad_op);
/* The kext's pairing check, exported for its tests: walks out[0..n) as the GPU would see it (NOP bodies skipped) and returns 0
 * when every RELEASE_MEM with PWS_ENABLE (word 1 bit 31) is exactly ours and is followed by exactly our ACQUIRE_MEM(PWS) as the
 * NEXT non-NOP packet (so before the next draw), and every ACQUIRE_MEM with PWS_ENA (word 6 bit 31) is exactly ours and directly
 * preceded by that release; 1 otherwise (including an unwalkable stream). Pure. */
uint32_t xlat12_ib_pws_check(const uint32_t *out, uint32_t n);
/* The kext's backstop, exported for its tests: 0 when out[0..n) carries exactly ds->nc_pkts NCLEAR fill packets, every one of
 * them inside the contiguous run of a recorded fill q < nc_filled (<= XLAT12_NCLEAR_MAX) that covers exactly [nc_va8[q] << 8,
 * + nc_len[q]), and no other DMA_DATA. 1 otherwise. Pure. */
uint32_t xlat12_ib_nclear_check(const uint32_t *out, uint32_t n, const xlat12_draw_stats *ds);
/* build 0.0.488: the T# step with the strip (flags & XLAT12_EXTRA_DCC_STRIP; every other flag bit is ignored here). */
uint32_t xlat12_table_img_desc_ex(const uint32_t in[8], uint32_t out[8], uint32_t *dropped, uint32_t flags);
/* build 0.0.488: 1 when `in` carries a DCC metadata address (w7 != 0 or w6 [31:24] != 0) and COMPRESSION_EN (w6 bit
 * 21) - the records XLAT12_EXTRA_DCC_STRIP is about, whether or not they are of its accept shape. Pure. */
int xlat12_desc_has_dcc(const uint32_t in[8]);
/* build 0.0.447 (S5-COVERAGE-PART2.md Q1) — the class-11 sampler-index-TABLE entry step, exposed for tests the
 * SAME way as xlat12_table_img_desc above (this is the exact function d_table_desc's own class-11 path calls, not
 * a reimplementation): entry i (0-based) at samptbl_va + 8*i is TWO dwords {idx, w1}, read TWICE through
 * ex->desc_read exactly like every other class-19 record (a mismatch is XLAT12_TDESC_UNSTABLE, a failed read is
 * XLAT12_TDESC_READ) and refused with XLAT12_TDESC_SAMP_OVERRIDE when idx's bit 31 is set (the SAME generic
 * override rule the direct-SGPR sampler path already declines on). 0 and *idx = the low-31-bit sampler index on
 * success. `ex->desc_read` must be non-null (asserted by the caller, not here - this mirrors d_tbl_read, which is
 * private). */
uint32_t xlat12_class11_entry_read(const xlat12_draw_extra *ex, uint64_t samptbl_va, uint32_t i, uint32_t *idx);
/* build 0.0.448 (review addition item 8) — a class-11 row's shape check, exposed for tests the SAME way:
 * 1 = `nsamp` is nonzero and equals `ntex` (the ONLY shape d_table_desc's placement loop can place entries
 * for correctly), 0 = refused (XLAT12_TDESC_TOO_MANY) before any placement. Every shipped row today has
 * samptbl == 0xff (class11 == 0, this is never even asked); this is a build-time defect guard for a future row. */
int xlat12_class11_row_ok(uint32_t ntex, uint32_t nsamp);
/* build 0.0.449 item 2 (F2) — the PS/VS user-data dword-index range overlap test d_region's WRITE_DATA /
 * COPY_DATA / WAIT_REG_MEM refusal uses, exposed for tests: 1 = the INCLUSIVE dword range [first, last] (order
 * does not matter) intersects PS user data (0xb030-0xb06c) or VS user data (0xb130-0xb1ac), in the SAME
 * dword-index space xlat12_lookup/reg_identical already use (dword_index << 2 == the byte address). O(1): no
 * cap, whatever `last - first` is. */
int xlat12_ud_range_hits(uint32_t first, uint32_t last);
/* build 0.0.453 item 5 (XLAT12_EXTRA_DESC_INV_APPLE_HEAD's own check), exposed for tests: 1 when `in[0..7]`
 * (at least 8 dwords, `n` covers them) is exactly PACKET3(ACQUIRE_MEM, 6) - header op 0x58, the header's own count
 * FIELD (not body length) equal to 6 - and its GCR_CNTL body word (in[7]: header + CP_COHER_CNTL, SIZE, SIZE_HI,
 * BASE, BASE_HI, POLL_INTERVAL, GCR_CNTL) has every bit of `needMask` set. Pure: reads `in`, writes nothing. 0 on
 * too few dwords, a different opcode, a different count, or a GCR_CNTL missing any needed bit. */
int xlat12_ib_head_acquire_mem_covers(const uint32_t *in, uint32_t n, uint32_t needMask);
/* 1 when fragment identity `id` samples through a class-19 table this translator knows the ABI of, with its slots.
 * build 0.0.470: 0 for a CLASS-10 row (its three texture indices are in Apple's entry table, not in tex[2]'s slots).
 * A no-sampler row answers 1 with *samp = 0xff (no sampler slot). */
int xlat12_shader_id_desc_table(int id, uint32_t *table, uint32_t *ntex, uint32_t tex[2], uint32_t *samp);
/*: may a gfx10 image record's ITERATE_256 (word 6 bit 10) be dropped for gfx12? gfx12 has no such field (Mesa
 * gfx12-rsrc.json SQ_IMG_RSRC_WORD6: bit 10 unassigned) and both Mesa drivers set it only for a depth MSAA image with
 * TC-compatible HTILE, inside the metadata branch (ac_descriptors.c:738-762, radv_image.h:399-400, si_descriptors.c:252). So:
 * 1 only when the record carries NO metadata - COMPRESSION_EN (w6 bit 21), WRITE_COMPRESS_ENABLE (bit 20), META_PIPE_ALIGNED
 * (bit 19) and META_DATA_ADDRESS_LO (w6 [31:24]) all 0 and word 7 0 - and is not MSAA (TYPE, w3 [31:28], not 14 or 15). Any
 * other record keeps the bit and the translator refuses it. */
int xlat12_iterate256_droppable(const uint32_t rec[8]);
/*: 1 when identity `id` is a fragment row annotated INLINE (xlat12_shader_desc.h), with *tex / *samp its first user-data slot
 * (samp 0xff = no sampler); 0 otherwise, including an id out of range. The descriptor CLASS is not changed by the annotation. */
int xlat12_shader_id_desc_inline(int id, uint32_t *tex, uint32_t *samp);

/* 0.0.224 : exact shader identity (tools/gfx-shader-ids.py -> xlat12_shader_ids.h, included only by
 * xlat12_ib.c). A program is ours when code[0..ndw) of some identity of that stage (1 vertex, 0 fragment) matches its head,
 * ends in s_endpgm and hashes to its FNV-1a. xlat12_shader_id_match returns the identity index or -1. */
uint32_t    xlat12_ib_shader_fnv(const uint32_t *d, uint32_t n);
uint32_t    xlat12_shader_id_count(void);
uint32_t    xlat12_readset_count(void);     /* build 0.0.504: kXlat12Readset rows (xlat12_readset.h is private) */
uint32_t    xlat12_readset_ptr_max(void);   /* build 0.0.504: XLAT12_READSET_PTR_MAX (5: J's five pointers) */
uint32_t    xlat12_shader_id_max_ndw(void);
/* 0.0.307: the identities a caller reading at most `cap` dwords cannot match, so it can NAME them instead of
 * silently matching nothing. Returns the count; for i < count fills *name and *ndw with the i-th such entry. */
uint32_t    xlat12_shader_id_over_cap(uint32_t cap, uint32_t i, const char **name, uint32_t *ndw);
const char *xlat12_shader_id_name(int id);
int         xlat12_shader_id_match(uint32_t stage, const uint32_t *code, uint32_t n);
/* 0.0.434 (an internal design note Part 1): a BY-INDEX view of the private identity table
 * (kXlat12ShaderIds, xlat12_shader_ids.h, included only by xlat12_ib.c), for a caller like gfx_pgmid.h's
 * n48_pgm_need to build its own (stage, head, ndw) row array without this module exposing the table's layout or
 * its private header. i < xlat12_shader_id_count() fills stage, ndw and head[0..3] from the SAME row
 * xlat12_shader_id_match itself reads and returns 1; i out of range returns 0 and touches nothing. */
int         xlat12_shader_id_row(uint32_t i, uint32_t *stage, uint32_t *ndw, uint32_t head[4]);
/* The m2tri profile with an identified vertex/fragment pair's record words (RSRC1/2_GS; RSRC1/2_PS, SPI_PS_INPUT_ENA/ADDR) and
 * heads, and (0.0.226) SPI_SHADER_GS_OUT_CONFIG_PS from the pair's io words as radeonsi builds it for gfx12 NGG
 * (si_state_shaders.cpp:1199-1235, :1782): VS_EXPORT_COUNT [4:0] = max(1, params) - 1 | NO_PC_EXPORT [10] = (params == 0) |
 * NUM_INTERP [16:11] = the fragment stage's used interpolants. 0, or XLAT12_ERR_ARG for an index out of range or a stage mismatch. */
uint32_t    xlat12_ib_profile_for(int vs_id, int ps_id, xlat12_draw_profile *out);
/* 0.0.309: one stage of the profile from one identity, for xlat12_draw_extra.pgm_profile. `io` is the
 * caller's {vertex, fragment} cache, kept across calls and updated here, because SPI_SHADER_GS_OUT_CONFIG_PS depends on
 * both stages. Starting from the m2tri profile with io = {0, 0}, a vertex call then a fragment call give exactly
 * xlat12_ib_profile_for(vs, ps) - asserted over every pair. */
uint32_t    xlat12_ib_profile_stage(int id, xlat12_draw_profile *out, uint32_t io[2]);

/* 0.0.232 : where an identified fragment stage keeps its sampling resources, as user-data indices (its SGPR
 * numbers: fragment user data k is SGPR k). 1 when the stage samples, with *desc the class-19 table pointer pair, *tex the image
 * table index and *samp the sampler table index; 0 when it samples nothing. The class-19 table holds the image table pointer at
 * +0x00 and the sampler table at +0x10; the image record is 8 dwords at index x 32, the sampler record 4 dwords at index x 16
 * (G2, an earlier analysis). */
int         xlat12_shader_id_sampling(int id, uint32_t *desc, uint32_t *tex, uint32_t *samp);

/* 0.0.232 : the fragment user-data VALUES a segment sets, which the translator itself never records (it keeps only
 * the USER_SGPR count and passes 0xb030..0xb05c through). SPI_SHADER_USER_DATA_PS_k is gfx10 byte 0xb030 + 4k = SET_SH_REG dword
 * offset 0xc + k from the 0x2c00 base. vals[k] = the value written to PS_k, bit k of *mask = it was written, last write wins; only
 * well-formed graphics-typed SET_SH_REG / SET_SH_REG_INDEX packets are read. Returns how many distinct slots were seen. */
#define XLAT12_PS_USER_DATA_SLOTS  12u
#define XLAT12_PS_USER_DATA_OFF    0x0cu
uint32_t    xlat12_ib_ps_user_data(const uint32_t *in, uint32_t n, uint32_t *vals, uint32_t *mask);
#define XLAT12_IMG_DESC_DWORDS   8u
#define XLAT12_SAMP_DESC_DWORDS  4u
#define XLAT12_DESC_TABLE_IMG_OFF   0x00u
#define XLAT12_DESC_TABLE_SAMP_OFF  0x10u

/*: DOES AN IDENTIFIED PROGRAM READ A RESOURCE DESCRIPTOR OUT OF THE CLIENT'S DATA? Answered from its
 * gfx1201 machine code by tools/gfx-shader-desc.py into xlat12_shader_desc.h, keyed by (stage, ndw, fnv) exactly as
 * xlat12_shader_id_match identifies the bytes. Classes: 0 UNPROVEN (no row - refuses), 1 NONE (no image/buffer instruction),
 * 2 RING (a vertex stage whose only descriptor is the attribute-ring record it loads from s[0:1]+0xa0 - ours), 3 IMAGE (samples
 * or loads through a T#/S# the client wrote), 4 BUFFER (a V# that is not the ring's). NOTHING on the Apple-frame path translates
 * a client's descriptor (the gfx10 -> gfx12 translators are called only by the tri suite route), so only NONE and RING are
 * "descriptor-free": xlat12_shader_id_desc_free is 1 for those two and 0 for everything else, including an id out of range.
 * The identity table's sampling slots are NOT this answer: they say 0xff for Tex_PS, which samples through an inline T#. */
uint32_t    xlat12_shader_id_desc_class(int id);
int         xlat12_shader_id_desc_free(int id);
const char *xlat12_desc_class_name(uint32_t cls);

/* D4-PRIME-FIXES.md item 3(b)/item 4, ; generalised build 0.0.445 — 1 iff `abi1` (a
 * resolved xlat12_draw_profile.ps_table_abi1, or 0 = none) names one of a SMALL, NAMED LIST of kDTableAbi rows
 * (ws_P_TimgXh_Ialp, ws_U_TvcmXh_Isrc, ws_Y_TkfhBvcmXh_Isrc - xlat12_ib.c's kDTblAbiGated) this library GATES
 * rather than serving unconditionally. The kext calls this right after xlat12_ib_profile_stage/_for resolve a
 * fragment identity, to decide whether to keep `ps_table_abi1` or zero it back to 0 under its own switch (a
 * hardware-output change: these draws become translated only with it, independent of D4'). Every OTHER kDTableAbi
 * row is unconditional, exactly as before this item; `abi1 == 0` (no row) answers 0. P's own answer is unchanged
 * by the generalisation. */
int         xlat12_table_abi_is_gated(uint32_t abi1);
/* build 0.0.470 (an internal design note section 3) — 1 iff `abi1` names a kDTableAbi row of one of the
 * two shapes this build adds: NO SAMPLER (samp and samptbl both 0xff, no class-10 pointer) or CLASS 10 (a texture-INDEX
 * table pointer, textbl1 != 0). Derived from the row's own fields. The kext's gfxsrc_pgm_profile zeroes such a row's
 * ps_table_abi1 unless its switch 51 is ON (after switch 43's own gate: every such row is ALSO in the 43 list), so these
 * shapes are translated only with BOTH switches ON. 0 for abi1 0 or out of range. Pure. */
int         xlat12_table_abi_new_shape(uint32_t abi1);
/* build 0.0.470 — for tests: how many kDTableAbi rows exist (abi1 runs 1..count); whether row abi1's own shape
 * passes the check d_table_desc refuses TOO_MANY without (0 for an index out of range); and the dwords a fresh
 * placement of row abi1 needs before alignment and the optional invalidate (0 out of range) - the SAME functions
 * d_table_desc calls. */
uint32_t    xlat12_table_abi_count(void);
int         xlat12_table_abi_row_ok(uint32_t abi1);
uint32_t    xlat12_table_abi_need(uint32_t abi1);
/* build 0.0.484 — for tests: the 1-based kDTableAbi row whose (ndw, fnv) is this program identity's, 0 = none - the
 * SAME first-match-by-(ndw, fnv) rule the profile's own lookup applies, but keyed on the numbers rather than on an
 * xlat12_shader_ids.h row, so a row's own tests (the glass rows' translate and gate tests) do not depend on the generated
 * identity table having been regenerated first. Pure. */
uint32_t    xlat12_table_abi_find(uint32_t ndw, uint32_t fnv);
/* build 0.0.470 (NO-SAMPLER-CLASS10.md section 2) — the class-10 texture-INDEX-table entry step, exposed for tests
 * exactly like xlat12_class11_entry_read (d_table_desc's class-10 path calls this SAME function): ONE dword at
 * textbl_va + `off` (the row's own tent[i], a BYTE offset), read TWICE through ex->desc_read (XLAT12_TDESC_READ on a
 * failed read or a missing callback, XLAT12_TDESC_UNSTABLE when the two reads differ). No bit-31 rule (an image index
 * carries no override). 0 and *idx = the index on success. */
uint32_t    xlat12_class10_entry_read(const xlat12_draw_extra *ex, uint64_t textbl_va, uint32_t off, uint32_t *idx);

#ifdef __cplusplus
}
#endif
#endif /* XLAT12_IB_H */
