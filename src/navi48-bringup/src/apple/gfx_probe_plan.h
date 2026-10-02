// gfx_probe_plan.h — which colour target a GFX IB's LAST draw renders into (0.0.283). Pure C, host-tested by
// tests/gfx_probe_plan_test.cpp; the kext compiles the SAME header.
//
// gfxcap1 showed SecurityAgent's frames are clock-glyph layers: each IB ends in a pass whose colour target 0 is a small
// BGR10A2 drawable (CB_COLOR0_INFO 0x8824: FORMAT 9 COLOR_2_10_10_10, NUMBER_TYPE 0 UNORM, COMP_SWAP 1) sized by
// PA_SC_SCREEN_SCISSOR_BR (366x113, 26x78, 51x110 ...), in VRAM. The copy-back probe fills that drawable with a known colour
// before Apple commits the (neutered) frame, to learn whether WindowServer's CPU compositor ever sees what lands in VRAM.
//
// n48_probe_last_target walks the IB tracking the last value of CB_COLOR0_BASE (context dword 0xa318, VA >> 8: Mesa
// ac_descriptors.c:1477 `cb_color_base = va >> 8`), CB_COLOR0_BASE_EXT (0xa390, VA >> 40), CB_COLOR0_INFO (0xa31c),
// PA_SC_SCREEN_SCISSOR_TL/BR (0xa00c/0xa00d) and CB_TARGET_MASK (0xa08e) through SET_CONTEXT_REG, and at every draw
// (DRAW_INDEX_2 0x27, DRAW_INDEX_AUTO 0x2d) snapshots them. It returns 1 with the LAST draw's snapshot when that draw's target 0
// is an 8-8-8-8 or 2-10-10-10 UNORM/SRGB colour buffer that the target mask enables, with a non-empty scissor starting at (0,0) and at
// most 4096 x 4096; else 0 (and *why names the first failing condition).
#ifndef N48_GFX_PROBE_PLAN_H
#define N48_GFX_PROBE_PLAN_H

#include <stdint.h>

enum { N48_PROBE_OK = 0, N48_PROBE_NO_DRAW = 1, N48_PROBE_NO_TARGET = 2, N48_PROBE_FORMAT = 3, N48_PROBE_MASK = 4,
       N48_PROBE_SCISSOR = 5 };

typedef struct { uint64_t va; uint32_t info, width, height, draw_at, draws, mask; } n48_probe_target;

static inline uint32_t n48_probe_last_target(const uint32_t *d, uint32_t n, n48_probe_target *t, uint32_t *why)
{
    uint32_t base = 0, ext = 0, info = 0, stl = 0, sbr = 0, mask = 0, seen = 0;
    n48_probe_target last = { 0, 0, 0, 0, 0, 0, 0 };
    uint32_t draws = 0;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t h = d[i];
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) break;
        const uint32_t op = (h >> 8) & 0xFFu, cnt = (h >> 16) & 0x3FFFu, len = cnt + 2u;
        if (len > n - i) break;
        if (op == 0x69u && cnt >= 1u) {
            const uint32_t r0 = 0xa000u + (d[i + 1] & 0xFFFFu);
            for (uint32_t k = 0; k < cnt; k++) {
                const uint32_t r = r0 + k, v = d[i + 2 + k];
                if (r == 0xa318u) { base = v; seen = 1; }
                else if (r == 0xa390u) ext = v;
                else if (r == 0xa31cu) info = v;
                else if (r == 0xa00cu) stl = v;
                else if (r == 0xa00du) sbr = v;
                else if (r == 0xa08eu) mask = v;
            }
        } else if (op == 0x27u || op == 0x2Du) {
            draws++;
            last.va = seen ? ((((uint64_t)(ext & 0xFFu)) << 40) | (((uint64_t)base) << 8)) : 0u;
            last.info = info;
            last.width = (stl == 0u) ? (sbr & 0x7FFFu) : 0u;
            last.height = (stl == 0u) ? ((sbr >> 16) & 0x7FFFu) : 0u;
            last.draw_at = i;
            last.mask = mask;
        }
        i += len;
    }
    last.draws = draws;
    *t = last;
    if (!draws) { *why = N48_PROBE_NO_DRAW; return 0; }
    if (!last.va) { *why = N48_PROBE_NO_TARGET; return 0; }
    const uint32_t fmt = (last.info >> 2) & 0x1Fu, num = (last.info >> 8) & 7u;
    if (!((fmt == 9u || fmt == 10u) && (num == 0u || num == 6u))) { *why = N48_PROBE_FORMAT; return 0; }
    if (!(last.mask & 0xFu)) { *why = N48_PROBE_MASK; return 0; }
    if (!last.width || !last.height || last.width > 4096u || last.height > 4096u) { *why = N48_PROBE_SCISSOR; return 0; }
    *why = N48_PROBE_OK;
    return 1;
}

/* The fill value for magenta, opaque, in both arrangements the target can have: FORMAT 9 2-10-10-10 (A in bits 30-31, the two
 * outer 10-bit fields full, the middle one zero) and FORMAT 10 8-8-8-8 (bytes 0 and 2 full, byte 1 zero, byte 3 alpha). Magenta is
 * symmetric under the R/B swap, so COMP_SWAP does not change it. */
static inline uint32_t n48_probe_magenta(uint32_t info)
{
    return (((info >> 2) & 0x1Fu) == 9u) ? 0xFFF003FFu : 0xFFFF00FFu;
}

#endif
