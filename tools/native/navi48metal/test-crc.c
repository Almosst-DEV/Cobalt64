// test-crc.c: host test of n48_crc.h.  cc -O1 -Wall -Wextra -Werror -o /tmp/test-crc test-crc.c && /tmp/test-crc
#include <stdlib.h>
#include <time.h>
#include "n48_crc.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
int main(void) {
    CHECK("crc32 check value", n48crc32(0, "123456789", 9) == 0xCBF43926u, "0x%08x", n48crc32(0, "123456789", 9));
    CHECK("crc32 incremental", n48crc32(n48crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "split feed equals one shot");
    CHECK("crc32 empty", n48crc32(0, "", 0) == 0, "0");
    const uint32_t W = 2560, H = 1440; const size_t pitch = (size_t)W * 4, rb = pitch;
    uint8_t *a = malloc(pitch * H), *b = malloc(pitch * H), *scr = malloc(rb);
    for (size_t i = 0; i < pitch * H; i++) a[i] = (uint8_t)(i * 2654435761u >> 13);
    memcpy(b, a, pitch * H);
    n48crc_samp_t sa, sb; uint32_t out[8];
    CHECK("rows at step 64", n48crc_sample(&sa, a, pitch, rb, H, 64, scr) == 23 && sa.step == 64, "23 rows (0..1408)");
    CHECK("default step", n48crc_sample(&sb, a, pitch, rb, H, 0, scr) == 23 && sb.step == N48CRC_DEFAULT_STEP, "0 -> 64");
    CHECK("cap", n48crc_sample(&sb, a, pitch, rb, 100000, 1, scr) == N48CRC_MAXROWS, "capped at %d rows", N48CRC_MAXROWS);
    n48crc_sample(&sa, a, pitch, rb, H, 64, scr); n48crc_sample(&sb, b, pitch, rb, H, 64, scr);
    CHECK("identical -> no diff", n48crc_diff(&sa, &sb, out, 8) == 0 && n48crc_sig(&sa) == n48crc_sig(&sb), "equal");
    b[(size_t)128 * pitch + 4000] ^= 1; n48crc_sample(&sb, b, pitch, rb, H, 64, scr);
    int nd = n48crc_diff(&sa, &sb, out, 8);
    CHECK("1 bit in sampled row 128", nd == 1 && out[0] == 128 && n48crc_sig(&sa) != n48crc_sig(&sb), "diff rows %d first y %u", nd, out[0]);
    memcpy(b, a, pitch * H); b[(size_t)129 * pitch + 10] ^= 0xFF; n48crc_sample(&sb, b, pitch, rb, H, 64, scr);
    CHECK("unsampled row 129 not seen (documented limit)", n48crc_diff(&sa, &sb, out, 8) == 0, "no diff");
    memcpy(b, a, pitch * H); b[(size_t)0] ^= 1; b[(size_t)1408 * pitch + pitch - 1] ^= 1; n48crc_sample(&sb, b, pitch, rb, H, 64, scr);
    nd = n48crc_diff(&sa, &sb, out, 8);
    CHECK("first and last sampled rows", nd == 2 && out[0] == 0 && out[1] == 1408, "rows %u %u", out[0], out[1]);
    nd = n48crc_diff(&sa, &sb, out, 1);
    CHECK("diff total beyond max", nd == 2 && out[0] == 0, "returns the total (2) while filling only max");
    // pitch padding beyond rowbytes is ignored
    uint8_t *p = malloc((size_t)(pitch + 64) * H); for (size_t i = 0; i < (pitch + 64) * H; i++) p[i] = (uint8_t)i;
    n48crc_samp_t s1, s2; n48crc_sample(&s1, p, pitch + 64, rb, H, 64, scr); p[(size_t)64 * (pitch + 64) + pitch + 3] ^= 1; n48crc_sample(&s2, p, pitch + 64, rb, H, 64, scr);
    CHECK("padding ignored", n48crc_diff(&s1, &s2, out, 8) == 0, "bytes past rowbytes are not CRC'd");
    n48crc_samp_t s3 = s1; s3.rows = 22;
    CHECK("row-count mismatch counts", n48crc_diff(&s1, &s3, out, 8) == 1, "1 missing row");
    // accounting
    n48crc_stats_t st; memset(&st, 0, sizeof st);
    n48crc_samp_t a1 = sa, a2 = sa, bb = sa, c = sa;
    CHECK("account: clean", n48crc_account(&st, &a1, &a2, &bb, 500000, 400000, 0) == 0 && st.checked == 1 && st.cost_n == 1, "flags 0");
    a2 = sb; CHECK("account: unstable", n48crc_account(&st, &a1, &a2, &bb, 700000, 600000, 1) == N48CRC_F_UNSTABLE && st.src_unstable == 1, "A1!=A2");
    a2 = sa; bb = sb; CHECK("account: slot != src", n48crc_account(&st, &a1, &a2, &bb, 900000, 800000, 0) == N48CRC_F_SLOT_NE && st.slot_ne_src == 1, "A1!=B");
    a2 = sb; bb = sb; CHECK("account: both", n48crc_account(&st, &a1, &a2, &bb, 100, 50, 0) == (N48CRC_F_UNSTABLE | N48CRC_F_SLOT_NE) && st.checked == 4, "both flags");
    CHECK("account: present clean", n48crc_account_present(&st, &a1, &c, 1000, 0, 0) == 0 && st.present_checked == 1, "C == A1");
    c = sb; CHECK("account: present changed", n48crc_account_present(&st, &a1, &c, 1000, 0, 1) == 1 && st.src_chg_present == 1 && st.present_checked == 2, "C != A1");
    CHECK("cost avg/max", st.cost_max == 900000 && st.cost_n == 6 && st.cost_sum == 500000 + 700000 + 900000 + 100 + 2000, "max %llu n %llu", (unsigned long long)st.cost_max, (unsigned long long)st.cost_n);
    CHECK("classify: later-cb (unstable #2 + changed-at-present) = 2, outside (unstable #4) = 1", st.chg_later_cb == 2 && st.chg_outside == 1, "later %llu outside %llu", (unsigned long long)st.chg_later_cb, (unsigned long long)st.chg_outside);
    n48crc_stats_t st2; memset(&st2, 0, sizeof st2);
    CHECK("noslot: b NULL skips the slot compare", n48crc_account(&st2, &a1, &a1, NULL, 1000, 0, 0) == 0 && st2.slot_n == 0 && st2.checked == 1, "no slot read counted");
    CHECK("present-change already classified is not double counted", n48crc_account_present(&st2, &a1, &sb, 1, 1, 1) == 1 && st2.chg_later_cb == 0 && st2.chg_outside == 0, "no extra class");
    CHECK("present-change classified when new", n48crc_account_present(&st2, &a1, &sb, 1, 0, 0) == 1 && st2.chg_outside == 1, "outside");
    char line[500]; n48crc_summary(&st, line, sizeof line);
    CHECK("summary text", strstr(line, "frames_checked 4") && strstr(line, "slot_ne_src 2") && strstr(line, "src_changed_before_present 1") && strstr(line, "crc_cost_max_us 900.0") && strstr(line, "slot_read_max_us 800.0") && strstr(line, "changed_by_later_cb"), "%s", line);
    // cost of one full check (3 samples) on this host, cached memory
    uint64_t t0 = now_ns(); for (int i = 0; i < 100; i++) { n48crc_sample(&s1, a, pitch, rb, H, 64, scr); } uint64_t t1 = now_ns();
    printf("info: one 23-row sample (235 KB) on the host: %.1f us\n", (t1 - t0) / 100 / 1000.0);
    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails != 0;
}
