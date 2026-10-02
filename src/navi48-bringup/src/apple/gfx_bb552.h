/* gfx_bb552.h — build 0.0.552 ( PLAN items (2) and (3)): TWO DIAGNOSTIC SWITCHES FOR RUN BA's CLOCK AND BLACK BOXES.
 *
 * SWITCH 109 "rp109", THE RESIDENCY-PROVENANCE TABLE'S CAPACITY. `109 | M << 8`: M 1 ON (= 365), M 2 OFF (= 621, the default and
 * the boot value), M 3 SHADOW (= 877); bare `109` reads.: the clock froze at the 8:57 minute change, SUSPECTED because the
 * table (ws_resprov.h, 32 entries) was FULL from frame 1024 (BA `full 278`). ON: the capacity is N48_RP_CAP_ON (128), and a record
 * that finds it full first evicts ONE entry that can never answer again (its ledger epoch has moved: ws_resprov.h
 * n48_rp_evict_pick). OFF: 0.0.551's table exactly (capacity 32, no eviction). SHADOW: OFF's table, and each FULL refusal ON would
 * have recorded is counted (fullOnWould). EVERY admission rule of n48_rp_record is unchanged in every mode: capacity and
 * replacement only. EVERY mode prints one capped line per FULL refusal (N48_RP109_FULL_FMT, log only).
 *
 * SWITCH 110 "an110", THE AN ROW (xlat12_ib.h XLAT12_DE_CLASS_AN / _AN_SHADOW). `110 | M << 8`: M 1 ON (= 366), M 2 OFF (= 622,
 * the default and the boot value), M 3 SHADOW (= 878); bare `110` reads.: SUSPECTED writer of the black date / message boxes
 * is AN (identity 62, ws_AN_TmuaXh_Isrc_Isrc, kDTableAbi {122, 0x7b3a6dfe}), whose draws COMMIT. ON: xlat12 replaces AN's draw by a
 * same-length NOP before its table step (content only: the draw runs no program, writes no colour, reads nothing); SHADOW: the
 * same predicate is counted, nothing changes. It rides switch 66's flag (latched with it, ONLY while 66 is ON and 60 is ON: the
 * flag's own home), so a frame with an AN elision commits only through 66's DRAW-ELIDE-R1 rung (42 ENFORCE, R1 clean). With 66 OFF
 * the switch is INERT (its line says so).
 *
 * Both: mid-arm guarded (the continuous guard, and any standing arm: hw_cm_armed), setting M resets their counters, and they write
 * no register, no page table and nothing of Apple's. Pure: no lock, no clock, no log. Host-tested by tests/gfx_bb552_test.cpp. */
#ifndef N48_GFX_BB552_H
#define N48_GFX_BB552_H

#include <stdint.h>

enum { N48_BB_OFF = 0u, N48_BB_ON = 1u, N48_BB_SHADOW = 2u, N48_BB_MODES = 3u };
#define N48_RP109_SWITCH      109u
#define N48_AN110_SWITCH      110u
#define N48_RP109_FULL_LINES  48u    /* FULL-refusal lines per arm scope (pre-arm is its own scope); the count is not capped */

/* The verb's M -> the mode (both switches). 0 is a read; anything else not listed answers N48_BB_MODES (refused, unchanged). */
static inline uint32_t n48_bb_mode_of_m(uint32_t m)
{
    return m == 1u ? (uint32_t)N48_BB_ON : m == 2u ? (uint32_t)N48_BB_OFF : m == 3u ? (uint32_t)N48_BB_SHADOW : (uint32_t)N48_BB_MODES;
}
static inline const char *n48_bb_mode_name(uint32_t mode)
{
    return mode == N48_BB_ON ? "ON" : mode == N48_BB_SHADOW ? "SHADOW" : "OFF (default)";
}
/* Switch 109's mode -> n48_rp_record_x's `on` (1 ON, 2 SHADOW, 0 OFF - ws_resprov.h). */
static inline uint32_t n48_rp109_on_arg(uint32_t mode)
{
    return mode == N48_BB_ON ? 1u : mode == N48_BB_SHADOW ? 2u : 0u;
}
/* Switch 110's mode -> the AN class bit added to switch 66's latched rows (xlat12_ib.h: XLAT12_DE_CLASS_AN 0x8, _AN_SHADOW 0x10),
 * ONLY when 66's own rows are non-zero (66 ON). 0 otherwise: the latched rows are 0.0.551's exactly. */
static inline uint32_t n48_an110_rows(uint32_t mode, uint32_t rows66)
{
    if (!rows66) return 0u;
    return rows66 | (mode == N48_BB_ON ? 0x8u : mode == N48_BB_SHADOW ? 0x10u : 0u);
}

/* THE LINES, each bounded under N48_LOG_CAP_BODY (491) by the test at worst-case values. */
/* 109's report (every `gfxneuter 109` verb): mode, how; table n / capacity / storage; recorded; FULL refusals and lines; evicted;
 * SHADOW's would-record; the arm's FULL lines. */
#define N48_RP109_FMT "rp109: switch 109 %s (365 ON: capacity 128 + evict epoch-stale; 621 OFF: 32; 877 SHADOW)%s; table %u/%u " \
    "(storage %u), recorded %llu; FULL refused %llu (lines %u this scope), evicted epoch-stale %llu, SHADOW would record %llu."
/* One line per FULL refusal (every mode, capped N48_RP109_FULL_LINES per arm scope): the copy's VA, size, gfx12 mode and context;
 * the table: n / capacity, the epoch now, entries of another context, epoch-stale (never answer again) and arm-stale (answer nothing
 * at this arm level); 109's mode; the boot's FULL count; the line's number. */
#define N48_RP109_FULL_FMT "rp109: FULL - copy VA %#llx size %#llx mode %u ctx %llu NOT RECORDED; table %u/%u, epoch %u arm %u: " \
    "epoch-stale %u arm-stale %u other-ctx %u; switch 109 %s; full %llu (line %u of %u)."
/* 110's report (every `gfxneuter 110` verb): mode, inert note, how; AN draws seen / elided (ON) / would (SHADOW) / refused by the
 * write set and the cap; committed frames' AN elisions (the de515 row); segments that carried a bit. */
#define N48_AN110_FMT "an110: switch 110 %s (366 ON: NOP AN {122, 0x7b3a6dfe} before its table step; 622 OFF; 878 SHADOW)%s%s; " \
    "segs %llu; AN draws seen %llu, elided %llu, would elide %llu; refused write-set %llu cap %llu; committed AN elisions %llu."

#endif /* N48_GFX_BB552_H */
