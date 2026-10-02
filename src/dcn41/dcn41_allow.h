/*
 * dcn41_allow.h - the DCN 4.1 DISPLAY WRITE ALLOWLIST.
 *
 * WHAT THIS IS, AND THE PROMISE IT KEEPS
 *
 * Every register write the display code originates goes through dcn41_allow_write(). A write is
 * REFUSED unless its absolute BAR5 dword address falls inside one of the explicit DCN 4.1 display
 * ranges in the generated dcn41_allow_ranges.h. There is no "allow anything else" branch: the
 * default is refusal, and a refusal is counted and sampled rather than silently dropped.
 *
 * The user was promised that this project never touches the card's power or voltage tables, the
 * SMU, or its firmware ROM. This file is that promise expressed as code rather than as care:
 *
 *   1. DENY BY DEFAULT. dcn41_allow_classify() returns a refusal reason unless a range matches.
 *   2. The guard is INERT until dcn41_allow_init() succeeds, and init refuses unless the caller's
 *      five DMU segment bases are exactly the ones this card's discovery table reports and the ones
 *      the range table was generated for. An allowlist of absolute addresses is meaningless if the
 *      bases moved, so a mismatch disables every write instead of trusting stale ranges.
 *   3. INDIRECT WINDOWS ARE HARD-DENIED, BEFORE the allow table is consulted. The low BAR5 page
 *      holds MM_INDEX/MM_DATA and NBIO's PCIE_INDEX2/RSMU_INDEX pairs; a write there selects a
 *      target anywhere in SMN - SMU, SMUIO, THM, the SPI ROM - and the next write goes to it. That
 *      is the one way a write inside a small address range could reach the things we promised not
 *      to touch, so it is refused even if a mis-generated range were to cover it.
 *   4. SMU/PSP/SMUIO BLOCKS ARE NAMED AND DENIED, also before the allow table. None of them
 *      overlaps a display range (tools/dcn41/gen_allow.py asserts it at generation time), so this
 *      layer is belt and braces; its value is that a refusal says WHICH block was aimed at, which
 *      turns "an unexpected refusal" into an immediate abort signal rather than a puzzle.
 *   5. COUNTERS AND SAMPLES. Counting is not witnessing: the state keeps the first
 *      DCN41_ALLOW_SAMPLES refusals AND the first DCN41_ALLOW_SAMPLES allowed writes, each with the
 *      address, the value, the caller tag and a sequence number, so a run can be audited afterwards
 *      rather than trusted.
 *
 * What this is NOT: it does not gate the bring-up ladder's own GFX, SDMA, MES, GART, PSP or IH
 * writes, which have their own guards and predate it. It gates the display path - everything the
 * src/dcn41 layer does - which is the only code allowed to write DCN registers.
 *
 * Freestanding: no heap, no libc, no floating point. Compiles as C11 and C++17 and as x86_64 kernel
 * code (tools/dcn41/build.py kextcheck).
 */
#ifndef N48_DCN41_ALLOW_H
#define N48_DCN41_ALLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dcn41_allow_reason {
    DCN41_ALLOW_OK = 0,          /* inside an explicit display range */
    DCN41_ALLOW_UNARMED,         /* dcn41_allow_init() has not succeeded on this state */
    DCN41_ALLOW_WINDOW,          /* beyond the BAR5 window the caller mapped */
    DCN41_ALLOW_INDIRECT,        /* an SMN/VRAM indirect-access window (hard deny, see the header) */
    DCN41_ALLOW_BLOCK,           /* inside a named SMU/PSP/SMUIO block (hard deny) */
    DCN41_ALLOW_RANGE,           /* outside every display range - the DEFAULT */
    DCN41_ALLOW_REASON_COUNT
};

#define DCN41_ALLOW_SAMPLES 16u
#define DCN41_ALLOW_TAG_LEN 20u
#define DCN41_ALLOW_STATE_MAGIC 0x314c4144u   /* "DAL1" */

struct dcn41_allow_sample {
    uint32_t abs_dword;
    uint32_t value;
    uint32_t seq;                             /* which write this was, counting from 1 */
    uint8_t reason;                           /* enum dcn41_allow_reason */
    char tag[DCN41_ALLOW_TAG_LEN];            /* caller tag, truncated, always NUL-terminated */
};

struct dcn41_allow_state {
    uint32_t magic;                           /* DCN41_ALLOW_STATE_MAGIC once init succeeded */
    uint32_t armed;                           /* 1 only if init validated the bases and the window */
    uint32_t mmio_dwords;                     /* the BAR5 window init was given */
    uint64_t writes_seen;                     /* every call to dcn41_allow_write */
    uint64_t allowed;
    uint64_t refused;
    uint64_t reads_seen;                      /* reads are not gated, only counted */
    uint64_t by_reason[DCN41_ALLOW_REASON_COUNT];
    uint32_t n_refuse_samples;                /* how many of refuse[] are filled */
    uint32_t n_allow_samples;
    uint32_t refuse_dropped;                  /* refusals past the sample capacity */
    uint32_t last_refused_abs;                /* the most recent refused address, for the abort rule */
    uint8_t  last_refused_reason;
    struct dcn41_allow_sample refuse[DCN41_ALLOW_SAMPLES];
    struct dcn41_allow_sample allow[DCN41_ALLOW_SAMPLES];
};

/* Arm the guard. Returns DCN41_ALLOW_OK, or a reason (never OK) if it refuses:
 *  - seg must equal the five bases the range table was generated for, element for element;
 *  - mmio_dwords must be non-zero and must cover the highest allowed range.
 * On refusal the state is zeroed and left UNARMED, so every subsequent write is refused. */
int dcn41_allow_init(struct dcn41_allow_state *st, const uint32_t seg[5], uint32_t mmio_dwords);

/* Pure classification, no state. *range_name and *why are optional out-parameters: on OK
 * *range_name names the display block, on a refusal *why is the documented reason text (or NULL). */
int dcn41_allow_classify(uint32_t abs_dword, uint32_t mmio_dwords,
                         const char **range_name, const char **why);

/* The gate. Returns true only if the write may proceed; the caller MUST NOT touch hardware on
 * false. tag is a short caller identifier ("irqset", "flip", "otglock", ...) kept in the samples. */
bool dcn41_allow_write(struct dcn41_allow_state *st, uint32_t abs_dword, uint32_t value,
                       const char *tag);

/* Reads are never gated (a read cannot change the card) but are counted so a run can say how much
 * of its traffic was read-only. */
void dcn41_allow_note_read(struct dcn41_allow_state *st);

const char *dcn41_allow_reason_name(int reason);

/* The generated table, for logging and for the host test; no other translation unit includes the
 * generated header, so the arrays exist exactly once. */
uint32_t dcn41_allow_range_count(void);
int dcn41_allow_range_at(uint32_t index, uint32_t *abs_lo, uint32_t *abs_hi, uint32_t *base_idx,
                         const char **name);
uint32_t dcn41_allow_total_dwords(void);

#ifdef __cplusplus
}
#endif

#endif /* N48_DCN41_ALLOW_H */
