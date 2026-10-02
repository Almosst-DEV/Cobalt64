/* test_headless.c - the headless-pass recogniser (xlat12_headless.h), its negative controls and its PLANTED-DEFECT suite
 *. OFFLINE ONLY, like the recogniser: nothing here is in the kext.
 *
 * Positive controls: wsgc1 F5 and F10 (the compositor's two setup halves) are accepted as exactly three passes each, at the
 * offsets's table gives, and every pass then translates under the kext's draw policy; the encoder segmenter's answer for
 * the same streams is UNCHANGED (0 for F5, 1 for the render half F6).
 * Negative controls: the render half, tri's frame, a SecurityAgent frame, truncations (at a packet boundary, mid-packet, after
 * a draw), foreign packets spliced in, a second draw, a missing trailer, state leaking past the draw, incomplete state, an
 * encoder head spliced in, junk after the last pass, a pass that lost its CONTEXT_CONTROL.
 * Planted defects: each of H1..H8 switched off in turn (xlat12_ib_headless_passes_m). Each has a control that ONLY that check
 * refuses, so switching it off must turn that control into an ACCEPT - a check no mutation can break is not a check.
 * Property: over 20000 random single-dword corruptions of F5, every ACCEPTED stream satisfies the output contract (passes tile
 * [0, n), each opens with the proven CONTEXT_CONTROL, holds exactly one draw, and head == start). */
#include <stdio.h>
#include <string.h>
#include "xlat12_ib.h"
#include "xlat12_headless.h"
#include "tests/fixture_wsgc1_headless.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXN 4096u
#define MAXS 32u
static uint32_t gBuf[MAXN], gOut[MAXN];
static xlat12_ib_segment gSeg[MAXS];

static uint32_t fnv32(const uint32_t *d, uint32_t n)   /* gcap_fnv32, AppleHardwareHook.cpp:14735 */
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) for (unsigned b = 0; b < 4; b++) { h ^= (d[i] >> (8u * b)) & 0xffu; h *= 0x01000193u; }
    return h;
}
static uint32_t plen(const uint32_t *d, uint32_t i, uint32_t n)
{
    const uint32_t h = d[i];
    if (h == XLAT12_IB_NOP || (h >> 30) == 2u) return 1u;
    if ((h >> 30) != 3u) return 0u;
    const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
    return i + l <= n ? l : 0u;
}
/* the packet-aligned offset of the first packet with opcode `op` at or after `from` (and, when reg != 0, the SET packet
 * whose run covers gfx10 byte address `reg`) */
static uint32_t find_pkt(const uint32_t *d, uint32_t n, uint32_t from, uint32_t op, uint32_t reg)
{
    uint32_t i = 0;
    while (i < n) {
        const uint32_t l = plen(d, i, n);
        if (!l) break;
        if (i >= from && d[i] != XLAT12_IB_NOP && (d[i] >> 30) == 3u && ((d[i] >> 8) & 0xFFu) == op) {
            if (!reg) return i;
            const uint32_t base = op == 0x69u ? 0x28000u : op == 0x76u ? 0xb000u : op == 0x79u ? 0x30000u : 0u;
            const uint32_t first = base + ((d[i + 1] & 0xFFFFu) << 2);
            if (base && reg >= first && reg < first + 4u * (l - 2u)) return i;
        }
        i += l;
    }
    return 0xFFFFFFFFu;
}
static void nop_packet(uint32_t *d, uint32_t at, uint32_t n)
{
    const uint32_t l = plen(d, at, n);
    d[at] = 0xC0001000u | ((l - 2u) << 16);
    for (uint32_t k = 1; k < l; k++) d[at + k] = 0u;
}
/* insert `k` dwords at `at`, shifting the rest; returns the new length */
static uint32_t insert(uint32_t *d, uint32_t n, uint32_t at, const uint32_t *w, uint32_t k)
{
    memmove(&d[at + k], &d[at], (n - at) * 4u);
    memcpy(&d[at], w, k * 4u);
    return n + k;
}
static uint32_t load(const uint32_t *src, uint32_t n) { memcpy(gBuf, src, n * 4u); return n; }
static uint32_t run(const uint32_t *d, uint32_t n, uint32_t skip, xlat12_hl_report *r, uint32_t *total)
{
    return xlat12_ib_headless_passes_m(d, n, gSeg, MAXS, total, r, skip);
}

/* ---- positive controls ---- */
static void test_positive(void)
{
    CHECK(fnv32(kWsgc1F5Setup, KWSGC1F5SETUP_N) == 0x3351f196u && fnv32(kWsgc1F10Setup, KWSGC1F10SETUP_N) == 0xdcc7a193u &&
          fnv32(kWsgc1F6Render, KWSGC1F6RENDER_N) == 0x4bfff10du && fnv32(kWsgc1F1Tri, KWSGC1F1TRI_N) == 0xf34bb9e5u &&
          fnv32(kGfxcap1F5SecurityAgent, KGFXCAP1F5SECURITYAGENT_N) == KGFXCAP1F5SECURITYAGENT_FNV,
          "a fixture no longer hashes to what the capture (and) recorded");
    const struct { const uint32_t *d; uint32_t n; const char *name; } setup[] = {
        { kWsgc1F5Setup, KWSGC1F5SETUP_N, "F5" }, { kWsgc1F10Setup, KWSGC1F10SETUP_N, "F10" } };
    for (uint32_t s = 0; s < 2u; s++) {
        xlat12_hl_report r; uint32_t total = 0;
        const uint32_t np = xlat12_ib_headless_passes(setup[s].d, setup[s].n, gSeg, MAXS, &total, &r);
        CHECK(np == 3u && total == 3u && r.why == XLAT12_HL_OK, "%s: %u pass(es), %s at %#x", setup[s].name, np,
              xlat12_hl_reason_name(r.why), r.at);
        const uint32_t want[4] = { 0x000u, 0x1e5u, 0x3cau, 0x5b0u };   /*: 3 x 485 + the one NOP */
        for (uint32_t k = 0; k < 3u && np == 3u; k++) {
            CHECK(gSeg[k].head == want[k] && gSeg[k].start == want[k] && gSeg[k].end == want[k + 1] && gSeg[k].draws == 1u &&
                  gSeg[k].draw_at == want[k] + 0x1dau, "%s pass %u = [%#x,%#x) head %#x draws %u at %#x", setup[s].name, k,
                  gSeg[k].start, gSeg[k].end, gSeg[k].head, gSeg[k].draws, gSeg[k].draw_at);
            /* each pass is well-formed input to the kext's draw policy (no resolver: the D-run of) */
            uint32_t olen = 0; xlat12_draw_stats ds;
            const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), NULL, &setup[s].d[gSeg[k].start],
                                                           gSeg[k].end - gSeg[k].start, gOut, &olen, &ds);
            CHECK(st == 0u && olen == gSeg[k].end - gSeg[k].start, "%s pass %u does not translate: %s at %#x",
                  setup[s].name, k, xlat12_ib_status_name(st), ds.err_in_dword);
        }
    }
    /* the recogniser changes nothing about the encoder segmenter */
    uint32_t tot = 0;
    CHECK(xlat12_ib_segments(kWsgc1F5Setup, KWSGC1F5SETUP_N, gSeg, MAXS, &tot) == 0u && tot == 0u, "segmenter now accepts F5");
    CHECK(xlat12_ib_segments(kWsgc1F6Render, KWSGC1F6RENDER_N, gSeg, MAXS, &tot) == 1u, "segmenter no longer accepts F6");
    /* a CONTEXT_CONTROL pattern that is NOT packet-aligned (inside a SET body) must not open a pass */
    {   uint32_t n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N);
        const uint32_t at = find_pkt(gBuf, n, 0x1e5u, 0x69u, 0x28c38u);   /* PA_SC_AA_MASK pair, 2 values */
        const uint32_t w[5] = { 0xC0036900u, (0x28c38u - 0x28000u) >> 2, XLAT12_IB_CTXCTL_HDR, XLAT12_IB_CTXCTL_DW1, XLAT12_IB_CTXCTL_DW2 };
        CHECK(at != 0xFFFFFFFFu, "no PA_SC_AA_MASK packet in pass 2");
        n = insert(gBuf, n, at, w, 5u);
        xlat12_hl_report r; uint32_t total = 0;
        CHECK(run(gBuf, n, 0u, &r, &total) == 3u, "a ctxctl pattern inside a SET body split a pass: %s", xlat12_hl_reason_name(r.why)); }
}

/* ---- negative controls: each must be REFUSED, and with the named reason ---- */
typedef struct { const char *name; uint32_t why; uint32_t only; } Neg;   /* only: the one check that must catch it (0 = any) */
static uint32_t build_neg(uint32_t k, uint32_t *n)
{
    static const uint32_t ib[4] = { 0xC0023F00u, 0x00100000u, 0x00000004u, 0x00000010u };        /* INDIRECT_BUFFER */
    static const uint32_t setreg[3] = { 0xC0016900u, (0x28238u - 0x28000u) >> 2, 0xfu };       /* SET_CONTEXT_REG CB_TARGET_MASK */
    static const uint32_t setcfg[3] = { 0xC0016800u, 0x0u, 0x0u };                             /* SET_CONFIG_REG */
    static const uint32_t dma[7] = { 0xC0055000u, 0x40000000u, 0u, 0u, 0u, 0u, 0x40u };        /* DMA_DATA (not a prefetch) */
    static const uint32_t ldctx[4] = { 0xC0026100u, 0x1000u, 0u, 0x10u };                      /* LOAD_CONTEXT_REG */
    static const uint32_t condx[5] = { 0xC0032200u, 0x1000u, 4u, 0u, 3u };                     /* COND_EXEC */
    static const uint32_t ctx2[3] = { XLAT12_IB_CTXCTL_HDR, 0x80000002u, 0x80000000u };        /* a non-proven CONTEXT_CONTROL */
    static const uint32_t junk[1] = { 0x00000000u };                                           /* a type-0 header: stops the walk */
    const uint32_t p2 = 0x1e5u, d2 = 0x1e5u + 0x1dau;
    switch (k) {
    case 0: *n = load(kWsgc1F6Render, KWSGC1F6RENDER_N); return XLAT12_HL_NOT_CTXCTL;            /* the render half */
    case 1: *n = load(kWsgc1F1Tri, KWSGC1F1TRI_N); return XLAT12_HL_NOT_CTXCTL;                  /* tri */
    case 2: *n = load(kGfxcap1F5SecurityAgent, KGFXCAP1F5SECURITYAGENT_N); return XLAT12_HL_NOT_CTXCTL;   /* SecurityAgent */
    case 3: *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); *n = d2; return XLAT12_HL_DRAWS;          /* cut at pass 2's draw (packet-aligned) */
    case 4: *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); *n = p2 + 101u; return XLAT12_HL_WALK;    /* cut mid-packet */
    case 5: *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); *n = d2 + 3u; return XLAT12_HL_TRAILER;   /* cut after pass 2's draw */
    case 6: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, ib, 4u); return XLAT12_HL_FOREIGN;
    case 7: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, setcfg, 3u); return XLAT12_HL_FOREIGN;
    case 8: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, dma, 7u); return XLAT12_HL_FOREIGN;
    case 9: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, ldctx, 4u); return XLAT12_HL_FOREIGN;
    case 10: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, condx, 5u); return XLAT12_HL_FOREIGN;
    case 11: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, ctx2, 3u); return XLAT12_HL_FOREIGN;
    case 12: {  /* a second draw in pass 2 */
        const uint32_t dr[3] = { kWsgc1F5Setup[d2], kWsgc1F5Setup[d2 + 1u], kWsgc1F5Setup[d2 + 2u] };
        *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, dr, 3u); return XLAT12_HL_DRAWS; }
    case 13: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2 + 3u, setreg, 3u); return XLAT12_HL_TAIL;   /* state after the draw */
    case 14: { *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); nop_packet(gBuf, find_pkt(gBuf, *n, p2, 0x69u, 0x28b54u), *n); return XLAT12_HL_STATE; }
    case 15: { *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); nop_packet(gBuf, find_pkt(gBuf, *n, 0x3cau, 0x76u, 0xb020u), *n); return XLAT12_HL_STATE; }
    case 16: { *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); nop_packet(gBuf, find_pkt(gBuf, *n, d2, 0x58u, 0u), *n); return XLAT12_HL_TRAILER; }
    case 17: *n = insert(gBuf, load(kWsgc1F5Setup, KWSGC1F5SETUP_N), d2, kWsgc1F6Render, 12u); return XLAT12_HL_HAS_HEAD;   /* F6's own encoder head spliced in */
    case 18: { *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); gBuf[(*n)++] = junk[0]; return XLAT12_HL_WALK; }           /* junk after the last pass */
    case 19: { *n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N); nop_packet(gBuf, 0u, *n); return XLAT12_HL_NOT_CTXCTL; }  /* pass 1 lost its ctxctl */
    case 20: {  /* the render half, disguised: its 2-dword EVENT_WRITE head replaced by the proven CONTEXT_CONTROL */
        gBuf[0] = XLAT12_IB_CTXCTL_HDR; gBuf[1] = XLAT12_IB_CTXCTL_DW1; gBuf[2] = XLAT12_IB_CTXCTL_DW2;
        memcpy(&gBuf[3], &kWsgc1F6Render[2], (KWSGC1F6RENDER_N - 2u) * 4u);
        *n = KWSGC1F6RENDER_N + 1u;
        return 0u; }   /* any refusal will do; the reason is printed */
    case 21: *n = 0u; load(kWsgc1F5Setup, 1u); return XLAT12_HL_ARG;
    case 22: { gBuf[0] = XLAT12_IB_NOP; *n = 1u; return XLAT12_HL_NOT_CTXCTL; }   /* only the trailing NOP */
    default: return 0xFFFFu;
    }
}
static const char *kNegName[] = {
    "the render half F6", "tri's frame F1", "a SecurityAgent frame (gfxcap1 F5)", "truncated at pass 2's draw",
    "truncated mid-packet", "truncated after pass 2's draw (no trailer)", "INDIRECT_BUFFER spliced into pass 2",
    "SET_CONFIG_REG spliced into pass 2", "DMA_DATA spliced into pass 2", "LOAD_CONTEXT_REG spliced into pass 2",
    "COND_EXEC spliced into pass 2", "a non-proven CONTEXT_CONTROL mid-pass", "a second draw in pass 2",
    "a register write after pass 2's draw", "pass 2 without VGT_SHADER_STAGES_EN", "pass 3 without SPI_SHADER_PGM_LO/HI_PS",
    "pass 2 without its ACQUIRE_MEM trailer", "an encoder head spliced into pass 2", "junk after the last pass",
    "pass 1 lost its CONTEXT_CONTROL", "the render half with a CONTEXT_CONTROL for a head", "an empty stream",
    "the trailing NOP alone" };
#define NNEG 23u

static void test_negative(void)
{
    for (uint32_t k = 0; k < NNEG; k++) {
        uint32_t n = 0; const uint32_t want = build_neg(k, &n);
        xlat12_hl_report r; uint32_t total = 0;
        const uint32_t np = run(gBuf, n, 0u, &r, &total);
        CHECK(np == 0u && (want == 0u || r.why == want), "negative control %u (%s) %s: got %u pass(es), reason %s, want %s",
              k, kNegName[k], np ? "ACCEPTED" : "refused for the wrong reason", np, xlat12_hl_reason_name(r.why),
              want ? xlat12_hl_reason_name(want) : "any");
        printf("  negative %-2u %-48s -> %s at %#x\n", k, kNegName[k], np ? "ACCEPTED" : xlat12_hl_reason_name(r.why), r.at);
    }
}

/* ---- planted defects: each check switched off must be caught by its own isolating control ---- */
static void test_planted(void)
{
    /* the control that ONLY this check refuses (build_neg index) */
    static const struct { uint32_t skip; uint32_t ctl; const char *name; } P[] = {
        { XLAT12_HL_SKIP_H1, 19u, "H1 dword 0 is the proven CONTEXT_CONTROL" },
        { XLAT12_HL_SKIP_H2, 17u, "H2 no encoder head anywhere" },
        { XLAT12_HL_SKIP_H3, 18u, "H3 the walk covers exactly n" },
        { XLAT12_HL_SKIP_H4,  6u, "H4 the pass alphabet" },
        { XLAT12_HL_SKIP_H5, 12u, "H5 exactly one draw per pass" },
        { XLAT12_HL_SKIP_H6, 13u, "H6 nothing but the trailer after the draw" },
        { XLAT12_HL_SKIP_H7, 14u, "H7 the pass-completeness set" },
        { XLAT12_HL_SKIP_H8, 16u, "H8 every pass ends in its trailer" },
    };
    uint32_t caught = 0;
    for (uint32_t m = 0; m < sizeof P / sizeof P[0]; m++) {
        uint32_t n = 0, total = 0; xlat12_hl_report r;
        build_neg(P[m].ctl, &n);
        const uint32_t full = run(gBuf, n, 0u, &r, &total);
        const uint32_t whyFull = r.why;
        const uint32_t mut = run(gBuf, n, P[m].skip, &r, &total);
        /* the mutant must still accept the real setup halves (a mutant that refuses everything catches nothing honestly) */
        const uint32_t pos = xlat12_ib_headless_passes_m(kWsgc1F5Setup, KWSGC1F5SETUP_N, gSeg, MAXS, &total, &r, P[m].skip);
        const int ok = full == 0u && mut > 0u && pos == 3u;
        caught += ok ? 1u : 0u;
        CHECK(ok, "planted defect %s: full recogniser %s (%s), mutant %s, mutant on F5 %u pass(es)", P[m].name,
              full ? "ACCEPTS" : "refuses", xlat12_hl_reason_name(whyFull), mut ? "accepts" : "still refuses", pos);
        printf("  planted %-44s control '%s': full -> %s, with the check off -> %s   %s\n", P[m].name, kNegName[P[m].ctl],
               xlat12_hl_reason_name(whyFull), mut ? "ACCEPTED" : "refused", ok ? "CAUGHT" : "NOT CAUGHT");
    }
    printf("  planted defects caught %u/%u\n", caught, (uint32_t)(sizeof P / sizeof P[0]));
}

/* ---- property: whatever it accepts has the promised shape ---- */
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static void test_property(void)
{
    uint32_t accepted = 0, bad = 0;
    for (uint32_t it = 0; it < 20000u; it++) {
        uint32_t n = load(kWsgc1F5Setup, KWSGC1F5SETUP_N);
        const uint32_t at = rnd() % n;
        gBuf[at] = (rnd() & 1u) ? rnd() : gBuf[rnd() % n];
        if (rnd() % 8u == 0u) n = 1u + rnd() % n;          /* sometimes also truncate */
        xlat12_hl_report r; uint32_t total = 0;
        const uint32_t np = run(gBuf, n, 0u, &r, &total);
        if (!np) continue;
        accepted++;
        uint32_t cur = 0;
        for (uint32_t k = 0; k < np; k++) {
            const xlat12_ib_segment *s = &gSeg[k];
            if (s->start != cur || s->head != s->start || s->end <= s->start || s->draws != 1u ||
                gBuf[s->start] != XLAT12_IB_CTXCTL_HDR || gBuf[s->start + 1u] != XLAT12_IB_CTXCTL_DW1 ||
                gBuf[s->start + 2u] != XLAT12_IB_CTXCTL_DW2 || ((gBuf[s->draw_at] >> 8) & 0xFFu) != 0x2Du) { bad++; break; }
            cur = s->end;
        }
        if (cur != n) bad++;
    }
    CHECK(bad == 0u, "property: %u of %u accepted corruptions break the output contract", bad, accepted);
    printf("  property: 20000 corruptions of F5, %u accepted (a corrupted VALUE is still a complete pass), %u broke the contract\n",
           accepted, bad);
}

int main(void)
{
    test_positive();
    test_negative();
    test_planted();
    test_property();
    printf("headless tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
