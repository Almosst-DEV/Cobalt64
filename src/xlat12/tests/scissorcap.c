/* scissorcap.c - build 0.0.499: THE CAPTURE TEST for the gfx12 inclusive scissor rule.
 *
 *   scissorcap < FRAMES          FRAMES: one line per frame, "<label> <IB0.bin> [<IB1.bin> ...]" (tests/scissorcap.py
 *                                builds it from a run's capdec/ directory and its driver log)
 *
 * For every draw in every listed frame: the gfx10.3 context state in force (SET_CONTEXT_REG, tracked across the frame's
 * IBs in submission order, as the hardware keeps it) is put through THE TRANSLATOR'S OWN DRAW POLICY - a synthetic
 * segment of one SET_CONTEXT_REG per register plus DRAW_INDEX_AUTO, translated by xlat12_ib_translate_draw (d_reg, the
 * path the kext commits) - and the gfx12 values it emits are read back. Then, in gfx12's INCLUSIVE bottom-right
 * semantics (xlat12_scissor.h), the coverage
 *     X = [max TL_X, min(BR_X of screen, window, generic, viewport-scissor-0 if enabled, viewport right edge)]
 * (and Y the same) must lie inside EVERY bound colour target's gfx12 ATTRIB2 size (MIP0_WIDTH/HEIGHT are size - 1 on
 * gfx12: ac_descriptors.c `S_028C78_MIP0_WIDTH(width - 1)`). A draw whose segment the policy REFUSES (XLAT12_ERR_SCISSOR)
 * is never emitted and counts as safe (and is counted). `scissor-only` repeats the check without the viewport bound
 * (stricter: the viewport clips geometry at the guard band, not at a pixel edge).
 *
 * The same coverage is computed from APPLE'S OWN gfx10.3 values in gfx10's EXCLUSIVE semantics (what Apple's driver meant).
 * FAILURES (exit 1): a draw whose translated coverage GROWS Apple's (any pixel Apple's state excludes - the bug), or
 * one past its target only because of the translation. REPORTED, not failed: draws whose Apple-authored gfx10 state
 * already reaches past ATTRIB2 (the translated coverage is then exactly Apple's - e.g. RUN I's screen scissor 256 over a
 * 192-wide 64KB_R_X target, whose allocation is padded to the 64 KiB block). Exit 2 on a usage or read error.
 * Host only; linked with the SAME xlat12.c / xlat12_ib.c the kext compiles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat12.h"
#include "xlat12_ib.h"

#define MAXDW (1u << 20)
static uint32_t gIb[MAXDW];
static uint32_t gCtx[0x400];      /* context registers 0x28000 + 4k, k < 0x400 */
static uint8_t  gSeen[0x400];
static uint32_t gOut[256];

static int is_draw(uint32_t op)
{
    return op == 0x24u || op == 0x25u || op == 0x26u || op == 0x27u || op == 0x2cu || op == 0x2du || op == 0x35u ||
           op == 0x38u || op == 0x39u || op == 0x3au;
}

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

typedef struct { uint64_t frames, draws, unseen, refused, grow, exceedXlat, exceedApple, exceedSc, maxOverX, maxOverY, shown; } Tot;
static Tot T;

/* the translated gfx12 value of each register, via the draw policy. Returns the policy's status. */
static uint32_t xlat_state(const uint32_t *g10, uint32_t nreg, uint32_t *g12out, uint32_t *v12out, uint32_t *errReg)
{
    static uint32_t in[256];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nreg; i++) {
        in[n++] = 0xC0016900u; in[n++] = (g10[i] - 0x28000u) >> 2; in[n++] = gCtx[(g10[i] - 0x28000u) >> 2];
    }
    in[n++] = 0xC0012D00u; in[n++] = 3u; in[n++] = 2u;
    xlat12_draw_stats ds; uint32_t len = 0;
    memset(&ds, 0, sizeof ds);
    const uint32_t st = xlat12_ib_translate_draw(xlat12_ib_m2tri_profile(), in, n, gOut, &len, &ds);
    *errReg = ds.err_reg;
    if (st) return st;
    for (uint32_t i = 0; i < nreg; i++) {
        uint32_t v = 0;
        if (!xlat12_ib_find_set(gOut, len, g12out[i], &v)) return 0xFFFFu;   /* the policy did not emit it: a test failure */
        v12out[i] = v;
    }
    return 0;
}

static void one_draw(const char *label, uint32_t ibi, uint32_t dw, uint32_t op)
{
    T.draws++;
    /* the scissor registers, gfx10 -> gfx12 address (xlat12_tables.h rows) */
    static const uint32_t kG10[] = { 0x28030u, 0x28034u, 0x28204u, 0x28208u, 0x28240u, 0x28244u, 0x28250u, 0x28254u, 0x28a48u };
    static const uint32_t kG12[] = { 0x28180u, 0x28184u, 0x28204u, 0x28208u, 0x28240u, 0x28244u, 0x28250u, 0x28254u, 0x28a48u };
    uint32_t g10[32], g12[32], v12[32], nreg = 0, tgt[8], ntgt = 0;
    int unseen = 0;
    for (uint32_t i = 0; i < sizeof kG10 / sizeof kG10[0]; i++) {
        if (!gSeen[(kG10[i] - 0x28000u) >> 2]) { unseen = 1; continue; }
        g10[nreg] = kG10[i]; g12[nreg] = kG12[i]; nreg++;
    }
    /* bound colour targets: CB_TARGET_MASK nibble != 0 and CB_COLORn_INFO FORMAT (gfx10 [6:2]) != 0 (COLOR_INVALID) */
    const uint32_t mask = gSeen[(0x28238u - 0x28000u) >> 2] ? gCtx[(0x28238u - 0x28000u) >> 2] : 0xFFFFFFFFu;
    for (uint32_t c = 0; c < 8u; c++) {
        const uint32_t info = 0x28c70u + 0x3cu * c, att2 = 0x28ec0u + 4u * c;
        if (!((mask >> (4u * c)) & 0xFu) || !gSeen[(info - 0x28000u) >> 2]) continue;
        if (((gCtx[(info - 0x28000u) >> 2] >> 2) & 0x1Fu) == 0u) continue;
        if (!gSeen[(att2 - 0x28000u) >> 2]) { unseen = 1; continue; }
        tgt[ntgt++] = nreg;
        g10[nreg] = att2; g12[nreg] = 0x28c78u + 0x24u * c; nreg++;
    }
    const uint32_t vpk[4] = { 0x2843cu, 0x28440u, 0x28444u, 0x28448u };
    for (unsigned k = 0; k < 4; k++) if (!gSeen[(vpk[k] - 0x28000u) >> 2]) unseen = 1;
    if (unseen || !ntgt) { T.unseen++; return; }
    uint32_t errReg = 0;
    const uint32_t st = xlat_state(g10, nreg, g12, v12, &errReg);
    if (st == (uint32_t)XLAT12_ERR_SCISSOR) { T.refused++; return; }
    if (st) {
        printf("%s IB%u dw %u op %#x: the draw policy answered %s (reg %#x) on the mini segment - TEST FAILURE\n", label, ibi, dw,
               op, st == 0xFFFFu ? "NOT-EMITTED" : xlat12_ib_status_name(st), errReg);
        T.grow++; T.exceedXlat++;
        return;
    }
    /* gfx12 inclusive (the TRANSLATED values): coverage = [max TL, min BR]; gfx10 exclusive (APPLE'S OWN values, what
     * Apple meant): [max TL, min BR - 1]. The viewport bound is the same for both (PA_CL_VPORT_* are IDENTICAL rows). */
    int64_t x0 = 0, y0 = 0, x1 = 0x7fffffff, y1 = 0x7fffffff, ax0 = 0, ay0 = 0, ax1 = 0x7fffffff, ay1 = 0x7fffffff;
    const uint32_t modeCntl = v12[8];   /* kG10[8] PA_SC_MODE_CNTL_0: every kG10 row is present here (else UNSEEN above) */
    const uint32_t modeCntl10 = gCtx[(0x28a48u - 0x28000u) >> 2];
    for (uint32_t i = 0; i < nreg; i++) {
        const uint32_t a = g12[i], v = v12[i], r = gCtx[(g10[i] - 0x28000u) >> 2];
        const int isTl = (a == 0x28180u || a == 0x28204u || a == 0x28240u || a == 0x28250u);
        const int isBr = (a == 0x28184u || a == 0x28208u || a == 0x28244u || a == 0x28254u);
        const uint32_t m10 = (g10[i] == 0x28030u || g10[i] == 0x28034u) ? 0xFFFFu : 0x7FFFu;   /* gfx10 field widths */
        const int vp = (a == 0x28250u || a == 0x28254u);
        if (!(vp && !(modeCntl & ((1u << 1) | (1u << 7))))) {   /* gfx12: viewport scissor on (VPORT_ or IMPLICIT_VPORT_) */
            if (isTl) { if ((int64_t)(v & 0xFFFFu) > x0) x0 = v & 0xFFFFu; if ((int64_t)(v >> 16) > y0) y0 = v >> 16; }
            if (isBr) { if ((int64_t)(v & 0xFFFFu) < x1) x1 = v & 0xFFFFu; if ((int64_t)(v >> 16) < y1) y1 = v >> 16; }
        }
        if (!(vp && !(modeCntl10 & (1u << 1)))) {
            if (isTl) { if ((int64_t)(r & m10) > ax0) ax0 = r & m10; if ((int64_t)((r >> 16) & m10) > ay0) ay0 = (r >> 16) & m10; }
            if (isBr) { if ((int64_t)(r & m10) - 1 < ax1) ax1 = (int64_t)(r & m10) - 1; if ((int64_t)((r >> 16) & m10) - 1 < ay1) ay1 = (int64_t)((r >> 16) & m10) - 1; }
        }
    }
    const int64_t sx1 = x1, sy1 = y1;
    const float xs = f32(gCtx[(0x2843cu - 0x28000u) >> 2]), xo = f32(gCtx[(0x28440u - 0x28000u) >> 2]);
    const float ys = f32(gCtx[(0x28444u - 0x28000u) >> 2]), yo = f32(gCtx[(0x28448u - 0x28000u) >> 2]);
    const float vxr = xo + (xs < 0 ? -xs : xs), vyr = yo + (ys < 0 ? -ys : ys);
    const int64_t vx1 = (int64_t)vxr - (((float)(int64_t)vxr == vxr) ? 1 : 0), vy1 = (int64_t)vyr - (((float)(int64_t)vyr == vyr) ? 1 : 0);
    if (vx1 < x1) x1 = vx1;
    if (vy1 < y1) y1 = vy1;
    if (vx1 < ax1) ax1 = vx1;
    if (vy1 < ay1) ay1 = vy1;
    const int empty = (x1 < x0 || y1 < y0), aempty = (ax1 < ax0 || ay1 < ay0);
    /* GROW: the translation covers a pixel Apple's own gfx10 state does not. Always a failure. */
    const int grow = !empty && (aempty || x0 < ax0 || y0 < ay0 || x1 > ax1 || y1 > ay1);
    if (grow) {
        T.grow++;
        if (T.grow <= 12u)
            printf("%s IB%u dw %u op %#x: GROWS Apple's coverage: translated x %lld..%lld y %lld..%lld, Apple's gfx10 x %lld..%lld y %lld..%lld\n",
                   label, ibi, dw, op, (long long)x0, (long long)x1, (long long)y0, (long long)y1, (long long)ax0, (long long)ax1,
                   (long long)ay0, (long long)ay1);
    }
    for (uint32_t t = 0; t < ntgt; t++) {
        const uint32_t a2 = v12[tgt[t]];
        const int64_t wmax = a2 >> 16, hmax = a2 & 0xFFFFu;   /* inclusive maxima: MIP0_WIDTH/HEIGHT = size - 1 */
        const int emptySc = (sx1 < x0 || sy1 < y0);
        const int bad = !empty && (x1 > wmax || y1 > hmax), badSc = !emptySc && (sx1 > wmax || sy1 > hmax);
        const int abad = !aempty && (ax1 > wmax || ay1 > hmax);
        if (bad && !abad) T.exceedXlat++;             /* past the target ONLY because of the translation: a failure */
        if (bad && abad) {                            /* Apple's own gfx10 state reaches past ATTRIB2 too: reported */
            T.exceedApple++;
            if (x1 > wmax && (uint64_t)(x1 - wmax) > T.maxOverX) T.maxOverX = (uint64_t)(x1 - wmax);
            if (y1 > hmax && (uint64_t)(y1 - hmax) > T.maxOverY) T.maxOverY = (uint64_t)(y1 - hmax);
        }
        if (badSc) T.exceedSc++;
        const uint32_t c = (g12[tgt[t]] - 0x28c78u) / 0x24u, att3 = gCtx[(0x28ee0u + 4u * c - 0x28000u) >> 2];
        const uint32_t fmt = (gCtx[(0x28c70u + 0x3cu * c - 0x28000u) >> 2] >> 2) & 0x1Fu;
        if (bad && T.shown++ < 40u)
            printf("%s IB%u dw %u op %#x: CB%u %ux%u (gfx10 FORMAT %u SW_MODE %u) translated x %lld..%lld y %lld..%lld; Apple's gfx10 x ..%lld y ..%lld -> %s\n",
                   label, ibi, dw, op, c, (unsigned)(wmax + 1), (unsigned)(hmax + 1), fmt, (att3 >> 14) & 0x1Fu, (long long)x0,
                   (long long)x1, (long long)y0, (long long)y1, (long long)ax1, (long long)ay1,
                   abad ? "APPLE'S OWN STATE EXCEEDS ATTRIB2 (same coverage as gfx10)" : "EXCEEDS BECAUSE OF THE TRANSLATION");
        if (getenv("SCISSORCAP_SHOW") && strcmp(getenv("SCISSORCAP_SHOW"), label) == 0 && dw == (uint32_t)atoi(getenv("SCISSORCAP_DW") ? getenv("SCISSORCAP_DW") : "0"))
            printf("SHOW %s IB%u dw %u op %#x: CB%u %ux%u, viewport x ..%.1f y ..%.1f, translated coverage x %lld..%lld y %lld..%lld (scissor-only x ..%lld y ..%lld); Apple's gfx10 x %lld..%lld y %lld..%lld\n",
                   label, ibi, dw, op, c, (unsigned)(wmax + 1), (unsigned)(hmax + 1), vxr, vyr, (long long)x0, (long long)x1,
                   (long long)y0, (long long)y1, (long long)sx1, (long long)sy1, (long long)ax0, (long long)ax1, (long long)ay0, (long long)ay1);
    }
}

static int one_ib(const char *label, uint32_t ibi, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    const size_t n = fread(gIb, 4, MAXDW, f);
    fclose(f);
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = gIb[i];
        if (h == XLAT12_IB_NOP || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) { fprintf(stderr, "%s: not type 3 at dw %u\n", path, i); return 2; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u, op = (h >> 8) & 0xFFu;
        if (i + l > n) { fprintf(stderr, "%s: truncated packet at dw %u\n", path, i); return 2; }
        if (op == 0x69u) {
            const uint32_t off = gIb[i + 1] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++)
                if (off + k < 0x400u) { gCtx[off + k] = gIb[i + 2u + k]; gSeen[off + k] = 1; }
        }
        if (is_draw(op)) one_draw(label, ibi, i, op);
        i += l;
    }
    return 0;
}

int main(void)
{
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *save = 0, *label = strtok_r(line, " \t\r\n", &save);
        if (!label) continue;
        memset(gSeen, 0, sizeof gSeen);   /* nothing is inherited from outside the frame: such a draw is UNSEEN */
        T.frames++;
        uint32_t ibi = 0;
        for (char *p = strtok_r(0, " \t\r\n", &save); p; p = strtok_r(0, " \t\r\n", &save))
            if (one_ib(label, ibi++, p)) return 2;
    }
    printf("scissorcap: frames %llu, draws %llu, evaluated %llu, unseen state %llu, refused by the policy %llu; "
           "GROWING Apple's own coverage %llu; past the target BECAUSE OF THE TRANSLATION %llu; past the target with "
           "Apple's own gfx10 state past it too %llu (max over x %llu y %llu); scissors-only (no viewport) past the target %llu\n",
           (unsigned long long)T.frames, (unsigned long long)T.draws,
           (unsigned long long)(T.draws - T.unseen - T.refused), (unsigned long long)T.unseen, (unsigned long long)T.refused,
           (unsigned long long)T.grow, (unsigned long long)T.exceedXlat, (unsigned long long)T.exceedApple,
           (unsigned long long)T.maxOverX, (unsigned long long)T.maxOverY, (unsigned long long)T.exceedSc);
    /* A refusal is SAFE but not expected on these runs: a census of every IB in run10o/run10l/run10j found no scissor BR
     * component of 0, so a refusal here means the rule refuses real traffic (a liveness regression). SCISSORCAP_ALLOW_REFUSED=1
     * for a capture that really carries empty scissors. */
    const int refusedBad = T.refused && !(getenv("SCISSORCAP_ALLOW_REFUSED") && getenv("SCISSORCAP_ALLOW_REFUSED")[0] == '1');
    if (refusedBad) printf("scissorcap: %llu draw(s) REFUSED - none expected (set SCISSORCAP_ALLOW_REFUSED=1 if the capture has empty scissors)\n",
                           (unsigned long long)T.refused);
    return (T.grow || T.exceedXlat || refusedBad) ? 1 : 0;
}
