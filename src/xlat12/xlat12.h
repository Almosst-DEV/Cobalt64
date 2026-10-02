/* xlat12 - gfx10.3 (Navi21) -> gfx12 (Navi48 / RDNA4) render-state PM4 translator.
 *
 * Milestone 2, route K (notes/MILESTONE2-DESIGN.md). Written to drop into the
 * bring-up kext beside AppleHardwareHook's `xlatregs`: plain C that also compiles
 * as C++; in the translation path there are no exceptions, RTTI, STL, heap
 * allocation or floating point. One function over caller-owned dword buffers.
 *
 * The input is always Apple-authored gfx10.3 PM4, so every register address is
 * read as gfx10.3 and re-emitted at its gfx12 address; the 92 aliasing addresses
 * are never ambiguous (design, Decision). Classes come from the generated table
 * xlat12_tables.h (Mesa gfx103.json / gfx12.json, cross-checked against Linux):
 *   identical     address and fields equal: value re-emitted unchanged
 *   moved         address changed, fields equal: value re-emitted at gfx12 address
 *   field_repack  fields changed: value remapped by the generated per-register function
 *   absent        no gfx12 register: dropped (reason string in the table)
 *   reused        no gfx12 register of that NAME, but a DIFFERENT gfx12 register sits at the same
 *                 address: the value is mapped by the row's function and emitted there (xlat12_reused.h)
 *   legacy_vs     legacy VS slot: dropped under the NGG rule below, else refused
 *   unknown       neither gfx10.3 source names it: the whole call is refused
 *
 * NGG rule. gfx12 has only the NGG vertex path. A VGT_SHADER_STAGES_EN write whose
 * gfx10.3 PRIMGEN_EN bit is 0 describes a legacy-VS pipeline and is refused
 * (XLAT12_ERR_LEGACY_VS); PRIMGEN_EN=1 records NGG in the context. A legacy-VS
 * register write is dropped when the context is NGG or the value is 0, and is
 * refused (XLAT12_ERR_VS_MODE_UNKNOWN) otherwise.
 *
 * Memory-loaded state (LOAD_CONTEXT_REG, LOAD_SH_REG, LOAD_UCONFIG_REG, their
 * _INDEX forms, CLEAR_STATE, CONTEXT_CONTROL) is detected and refused with
 * XLAT12_ERR_MEMLOADED: a documented stub, no translation yet.
 *
 * SET_CONTEXT_REG / SET_SH_REG / SET_UCONFIG_REG runs are split wherever the gfx12
 * addresses stop being contiguous or change register block. SET_SH_REG_INDEX and
 * SET_UCONFIG_REG_INDEX are re-emitted one register per packet with the index kept.
 * The input header's flag byte (predicate, shader type, reset-filter-cam) is kept.
 * Every other PM4 type-3 packet is copied verbatim.
 */
#ifndef XLAT12_H
#define XLAT12_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XLAT12_OK                  = 0,
    XLAT12_ERR_ARG             = 1,  /* NULL buffer with a non-zero size */
    XLAT12_ERR_BAD_PACKET      = 2,  /* not a PM4 type-3 packet */
    XLAT12_ERR_TRUNCATED       = 3,  /* packet runs past the end of the input */
    XLAT12_ERR_RANGE           = 4,  /* register offset/count outside its block */
    XLAT12_ERR_BAD_INDEX       = 5,  /* _INDEX form: index or offset bits not allowed */
    XLAT12_ERR_UNKNOWN_REG     = 6,  /* register with no authoritative mapping */
    XLAT12_ERR_LEGACY_VS       = 7,  /* VGT_SHADER_STAGES_EN describes a non-NGG pipeline */
    XLAT12_ERR_VS_MODE_UNKNOWN = 8,  /* non-zero legacy-VS write before NGG is known */
    XLAT12_ERR_MEMLOADED       = 9,  /* memory-loaded state packet: stub, not translated */
    XLAT12_ERR_CAPACITY        = 10, /* output buffer too small */
    XLAT12_ERR_SWMODE          = 11, /* build 0.0.452 item 3 (F5, CORRECTED after this item's own golden-vector
                                      * run): a CB_COLORn_ATTRIB3/DB_Z_INFO/DB_STENCIL_INFO write's gfx10
                                      * COLOR_SW_MODE/SW_MODE has NO gfx12 counterpart in kXlat12SwModeG10ToG12
                                      * (xlat12_desc.h - modes 28/31/32 and every other reserved value; see that
                                      * table's own comments). Every OTHER mode is remapped BY NAME through that
                                      * same table (xlat12.c's decide(), XLAT12_CLS_FIELD_REPACK), exactly as
                                      * xlat12_ib.c's draw-policy d_reg/d_swmode_repack already does (0.0.451 item
                                      * 8) - a first attempt refused every mode but {0, 27} instead, and `make test`
                                      * (src/xlat12) immediately falsified it: golden_vectors.h's encoder_named_nav2
                                      * and encoder_named_4vp are real Apple RenderEncoder captures that write
                                      * CB_COLOR0_ATTRIB3 mode 9 (ADDR_SW_64KB_S) through this exact path and expect
                                      * XLAT12_OK - refusing real, currently-working traffic would have been worse
                                      * than the mistranslation bug this item fixes. */
    XLAT12_ERR_SCISSOR         = 12  /* build 0.0.499: a PA_SC_{SCREEN,WINDOW,GENERIC,VPORT_n}_SCISSOR_BR
                                      * write with BR_X or BR_Y == 0. gfx12's BR is INCLUSIVE (xlat12_scissor.h, with
                                      * the mesa quotes), so every other BR is emitted -1; a 0 cannot be, without also
                                      * rewriting its TL (mesa's empty encoding TL (1,1) BR (0,0)) - a different
                                      * register write - so the frame is REFUSED, never admitted. Both paths: xlat12.c's
                                      * decide() and xlat12_ib.c's d_reg. */
} xlat12_status;

enum { XLAT12_VS_UNKNOWN = 0, XLAT12_VS_NGG = 1 };

/* Register classes; the generated table (xlat12_tables.h) uses these values. A
 * field-changed register whose gfx10.3 and gfx12 field lists share no field name
 * has no established value mapping and is generated as UNKNOWN, not as a repack. */
typedef enum {
    XLAT12_CLS_IDENTICAL = 0, XLAT12_CLS_MOVED = 1, XLAT12_CLS_FIELD_REPACK = 2,
    XLAT12_CLS_ABSENT = 3, XLAT12_CLS_LEGACY_VS = 4, XLAT12_CLS_UNKNOWN = 5,
    /* 0.0.389 (notes 880/881) - REUSED: the gfx10.3 register has NO gfx12 namesake, but gfx12 puts a DIFFERENT
     * register at the SAME MMIO address. "absent" would drop the write and leave that gfx12 register holding
     * whatever its context slot last held - and gfx12 has no CLEAR_STATE to reset it. The row's `g12` is that
     * same address, its `repack` maps the gfx10.3 value to a gfx12 value for the register that really lives
     * there, and its `name` names BOTH. The rows live in xlat12_reused.h (hand-written, NOT generated). */
    XLAT12_CLS_REUSED = 6
} Xlat12Class;
#define XLAT12_CLS_MAX XLAT12_CLS_REUSED

/* State carried across calls (e.g. from the pipeline IB to a later draw IB).
 * Zero-initialise; the kext may seed vs_mode = XLAT12_VS_NGG once G2 confirms. */
typedef struct {
    uint32_t vs_mode;
} xlat12_ctx;

typedef struct {
    uint32_t packets_in;          /* type-3 packets consumed */
    uint32_t packets_out;         /* type-3 packets emitted */
    uint32_t packets_verbatim;    /* copied unchanged */
    uint32_t set_packets_in;      /* SET_* and SET_*_INDEX packets consumed */
    uint32_t set_packets_out;     /* SET_* and SET_*_INDEX packets emitted */
    uint32_t set_packets_empty;   /* SET_* packets that emitted nothing */
    uint32_t set_runs_split;      /* extra output SET packets created by splitting */
    uint32_t regs_in;
    uint32_t regs_identical;
    uint32_t regs_moved;
    uint32_t regs_repacked;
    uint32_t regs_dropped_absent;
    uint32_t regs_reused;         /* 0.0.389: XLAT12_CLS_REUSED - emitted at the same address as a DIFFERENT gfx12 register */
    uint32_t regs_dropped_legacy_vs;
    uint32_t regs_scissor_adjusted; /* 0.0.499: scissor BR writes emitted -1 (xlat12_scissor.h) */
    uint32_t dwords_out;          /* == *out_dwords on success */
    /* on error */
    uint32_t err_in_dword;        /* input dword index of the failing packet */
    uint32_t err_reg_addr;        /* gfx10.3 address involved, if any */
    uint32_t err_opcode;          /* PKT3 opcode (or the raw header for BAD_PACKET) */
    const char *err_name;         /* register or opcode name, if any */
} xlat12_stats;

/* Translate `in_dwords` dwords of gfx10.3 PM4 into `out` (capacity `out_cap`
 * dwords). On XLAT12_OK *out_dwords is the output length. `ctx` and `stats` may
 * be NULL (a NULL ctx means the NGG mode is unknown and nothing is recorded).
 * Nothing is written past out[out_cap-1]. On error the output is incomplete and
 * must be discarded; `stats` names the failing packet, register or opcode. */
xlat12_status xlat12_translate(xlat12_ctx *ctx, const uint32_t *in, uint32_t in_dwords,
                               uint32_t *out, uint32_t out_cap,
                               uint32_t *out_dwords, xlat12_stats *stats);

/* One gfx10.3 address: returns 1 and fills gfx12 address / class if tabled, else 0. */
int xlat12_lookup(uint32_t g10_addr, uint32_t *out_g12, uint32_t *out_cls);

/* The field repack for one register value (identity for non-repack classes). */
uint32_t xlat12_repack_value(uint32_t g10_addr, uint32_t value);

/* Table introspection for tests and integration probes. */
uint32_t xlat12_table_len(void);
int xlat12_table_entry(uint32_t i, uint32_t *g10, uint32_t *g12, uint32_t *cls, const char **name);

const char *xlat12_status_name(xlat12_status s);

#ifdef __cplusplus
}
#endif
#endif /* XLAT12_H */
