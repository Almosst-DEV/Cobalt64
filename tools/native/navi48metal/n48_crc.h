// n48_crc.h: the pure helpers of the opt-in per-frame CRC diagnostic for the D-copy path (an internal design note "Tearing diagnostic (CRC)").
// Row sampling + CRC-32 (IEEE, reflected, the zlib one) + the accounting of the three checks. No Vulkan, no ObjC, no locking (the bundle holds a mutex).
//   A1 = sampled CRC of the display surface's import memory right after the GPU finished the command buffer; B = same rows of the scanout slot (the GPU copy's
//   result); A2 = the surface again right after B (A1 != A2: someone is writing it WHILE we check = written outside our command buffers, H1); C = the surface once
//   more immediately before scanout_present (A1 != C: changed between completion and present).
#ifndef N48_CRC_H
#define N48_CRC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define N48CRC_MAXROWS 64
#define N48CRC_DEFAULT_STEP 64

static inline uint32_t n48crc32(uint32_t crc, const void *p, size_t n) {   // incremental: pass 0 first, feed the result back. Slicing-by-8 (little-endian hosts only: x86_64, arm64)
    static uint32_t tab[8][256]; static int init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[0][i] = c; }
        for (uint32_t i = 0; i < 256; i++) for (int t = 1; t < 8; t++) tab[t][i] = (tab[t - 1][i] >> 8) ^ tab[0][tab[t - 1][i] & 0xFF];
        init = 1;
    }
    const uint8_t *b = (const uint8_t *)p; crc = ~crc;
    while (n >= 8) {
        uint32_t lo, hi; memcpy(&lo, b, 4); memcpy(&hi, b + 4, 4); lo ^= crc;
        crc = tab[7][lo & 0xFF] ^ tab[6][(lo >> 8) & 0xFF] ^ tab[5][(lo >> 16) & 0xFF] ^ tab[4][lo >> 24] ^
              tab[3][hi & 0xFF] ^ tab[2][(hi >> 8) & 0xFF] ^ tab[1][(hi >> 16) & 0xFF] ^ tab[0][hi >> 24];
        b += 8; n -= 8;
    }
    while (n--) crc = tab[0][(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct { uint32_t rows, step; uint32_t crc[N48CRC_MAXROWS]; } n48crc_samp_t;   // crc[k] = CRC of row k*step (rowbytes bytes)

// Rows 0, step, 2*step ... below `height`, at most N48CRC_MAXROWS of them; each row is memcpy'd into `scratch` (>= rowbytes) first, so an uncached/BAR source is read
// with wide loads, not byte loads. step 0 means the default. Returns the number of rows sampled.
static inline uint32_t n48crc_sample(n48crc_samp_t *s, const uint8_t *base, size_t pitch, size_t rowbytes, uint32_t height, uint32_t step, uint8_t *scratch) {
    if (!step) step = N48CRC_DEFAULT_STEP;
    s->step = step; s->rows = 0;
    for (uint32_t y = 0; y < height && s->rows < N48CRC_MAXROWS; y += step) {
        memcpy(scratch, base + (size_t)y * pitch, rowbytes);
        s->crc[s->rows++] = n48crc32(0, scratch, rowbytes);
    }
    return s->rows;
}
// Whole-frame signature: CRC over the row CRCs.
static inline uint32_t n48crc_sig(const n48crc_samp_t *s) { return n48crc32(0, s->crc, (size_t)s->rows * sizeof s->crc[0]); }
// Image rows (y = k*step) whose CRC differs; fills out[0..max), returns the TOTAL number of differing rows (rows counts must match, else all rows differ).
static inline int n48crc_diff(const n48crc_samp_t *a, const n48crc_samp_t *b, uint32_t *out, int max) {
    int n = 0; uint32_t m = a->rows < b->rows ? a->rows : b->rows;
    for (uint32_t k = 0; k < m; k++) if (a->crc[k] != b->crc[k]) { if (n < max) out[n] = k * a->step; n++; }
    return n + (int)(a->rows > b->rows ? a->rows - m : b->rows - m);
}

typedef struct {
    uint64_t checked, src_unstable, slot_ne_src, present_checked, src_chg_present, cost_n, cost_sum, cost_max, logged;
    uint64_t slot_n, slot_sum, slot_max;   // the slot (BAR, write-combined) read alone
    uint64_t chg_later_cb, chg_outside;     // a source change classified: a LATER command buffer for the same surface was submitted (later seq) / none was (written outside our command buffers)
} n48crc_stats_t;
enum { N48CRC_F_UNSTABLE = 1, N48CRC_F_SLOT_NE = 2 };
static inline void n48crc_cost(n48crc_stats_t *s, uint64_t ns) { s->cost_n++; s->cost_sum += ns; if (ns > s->cost_max) s->cost_max = ns; }
static inline void n48crc_classify(n48crc_stats_t *s, int later_cb) { if (later_cb) s->chg_later_cb++; else s->chg_outside++; }
// One frame's completion-time check (b == NULL: the slot was not read, mode "noslot"). slot_ns = time spent reading the slot. later_cb: a later command buffer for this surface
// had been submitted by now. Returns N48CRC_F_* flags.
static inline int n48crc_account(n48crc_stats_t *s, const n48crc_samp_t *a1, const n48crc_samp_t *a2, const n48crc_samp_t *b, uint64_t cost_ns, uint64_t slot_ns, int later_cb) {
    int f = 0; uint32_t d[1];
    s->checked++; n48crc_cost(s, cost_ns);
    if (b) { s->slot_n++; s->slot_sum += slot_ns; if (slot_ns > s->slot_max) s->slot_max = slot_ns; }
    if (n48crc_diff(a1, a2, d, 0)) { s->src_unstable++; f |= N48CRC_F_UNSTABLE; n48crc_classify(s, later_cb); }
    if (b && n48crc_diff(a1, b, d, 0)) { s->slot_ne_src++; f |= N48CRC_F_SLOT_NE; }
    return f;
}
// The just-before-present check. Returns 1 when the surface changed since A1. already: the completion check already saw (and classified) a change for this frame.
static inline int n48crc_account_present(n48crc_stats_t *s, const n48crc_samp_t *a1, const n48crc_samp_t *c, uint64_t cost_ns, int already, int later_cb) {
    uint32_t d[1]; s->present_checked++; n48crc_cost(s, cost_ns);
    if (n48crc_diff(a1, c, d, 0)) { s->src_chg_present++; if (!already) n48crc_classify(s, later_cb); return 1; }
    return 0;
}
static inline int n48crc_summary(const n48crc_stats_t *s, char *buf, size_t n) {
    uint64_t avg = s->cost_n ? s->cost_sum / s->cost_n : 0, savg = s->slot_n ? s->slot_sum / s->slot_n : 0;
    return snprintf(buf, n, "frames_checked %llu src_unstable_during_check %llu slot_ne_src %llu present_checked %llu src_changed_before_present %llu changed_by_later_cb %llu changed_outside_cbs %llu crc_cost_avg_us %.1f crc_cost_max_us %.1f slot_read_avg_us %.1f slot_read_max_us %.1f",
                    (unsigned long long)s->checked, (unsigned long long)s->src_unstable, (unsigned long long)s->slot_ne_src, (unsigned long long)s->present_checked,
                    (unsigned long long)s->src_chg_present, (unsigned long long)s->chg_later_cb, (unsigned long long)s->chg_outside, avg / 1000.0, s->cost_max / 1000.0, savg / 1000.0, s->slot_max / 1000.0);
}
#endif
