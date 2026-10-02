// test-cblog.c: host test of n48_cblog.h.  cc -O1 -Wall -Wextra -Werror -o /tmp/test-cblog test-cblog.c && /tmp/test-cblog
#include <stdio.h>
#include <stdlib.h>
#include "n48_cblog.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static n48cb_t T;
static uint32_t sub(uint64_t id, uint64_t q, uint64_t now, uint32_t nr, const uint32_t *r, n48cb_inv_t *inv, uint32_t mx, uint64_t *sq) {
    return n48cb_submit(&T, id, q, now, NULL, 0, r, nr, inv, mx, sq);
}
int main(void) {
    n48cb_inv_t inv[N48CB_MAXINV]; uint64_t sq; uint32_t w1[] = { 5 }, r1[] = { 5 }, none[] = { 0 };
    n48cb_init(&T);
    // A (queue 1) writes 5, committed t=10; B (queue 2) reads 5, committed t=20. B submitted before A: 1 inversion.
    n48cb_commit(&T, 1, 0xA1, 10, w1, 1, none, 0); n48cb_commit(&T, 2, 0xB2, 20, none, 0, r1, 1);
    uint32_t n = sub(2, 0xB2, 30, 1, r1, inv, 32, &sq);
    CHECK("B before A", n == 1 && inv[0].sid == 5 && inv[0].writer_id == 1 && inv[0].reader_id == 2 && inv[0].writer_queue == 0xA1 && inv[0].reader_queue == 0xB2 && inv[0].reader_sub == 1 && sq == 1, "n=%u sub=%llu", n, (unsigned long long)sq);
    n = sub(1, 0xA1, 31, 0, none, inv, 32, &sq);
    CHECK("A afterwards: reads nothing", n == 0 && sq == 2 && T.inv_total == 1 && T.inv_cbs == 1, "no inversion");
    // In order: A submitted first, then B: none.
    n48cb_init(&T);
    n48cb_commit(&T, 1, 1, 10, w1, 1, none, 0); n48cb_commit(&T, 2, 2, 20, none, 0, r1, 1);
    sub(1, 1, 25, 0, none, inv, 32, &sq); n = sub(2, 2, 26, 1, r1, inv, 32, &sq);
    CHECK("in order", n == 0 && T.inv_total == 0, "0");
    // Writer committed LATER than the reader is not an inversion.
    n48cb_init(&T);
    n48cb_commit(&T, 2, 2, 20, none, 0, r1, 1); n48cb_commit(&T, 1, 1, 30, w1, 1, none, 0);
    n = sub(2, 2, 40, 1, r1, inv, 32, &sq);
    CHECK("later writer ignored", n == 0, "0");
    // Same-id and a cb that both writes and reads the sid (self-sample): never counted against itself.
    n48cb_init(&T);
    n48cb_commit(&T, 3, 1, 10, w1, 1, r1, 1);
    n = sub(3, 1, 11, 1, r1, inv, 32, &sq);
    CHECK("self read-write", n == 0, "0");
    // Two writers pending for the same sid, two sids read: 4 triples; detail list capped but count exact.
    n48cb_init(&T);
    uint32_t wa[] = { 7, 8 }, rr[] = { 7, 8 };
    n48cb_commit(&T, 1, 1, 1, wa, 2, none, 0); n48cb_commit(&T, 2, 2, 2, wa, 2, none, 0); n48cb_commit(&T, 9, 3, 5, none, 0, rr, 2);
    n = sub(9, 3, 6, 2, rr, inv, 3, &sq);
    CHECK("count exact, list capped", n == 4 && inv[2].sid != 0 && T.inv_total == 4 && T.iv_inv == 4, "n=%u", n);
    uint64_t s_, i_; n48cb_take_interval(&T, &s_, &i_);
    CHECK("interval", s_ == 1 && i_ == 4 && T.iv_submits == 0 && T.iv_inv == 0, "submits %llu inv %llu then reset", (unsigned long long)s_, (unsigned long long)i_);
    // Duplicates in the sid lists are removed; zero is ignored; cap respected.
    uint32_t a[4]; uint32_t c = 0;
    c = n48cb_addsid(a, c, 4, 3); c = n48cb_addsid(a, c, 4, 3); c = n48cb_addsid(a, c, 4, 0); c = n48cb_addsid(a, c, 4, 4);
    CHECK("addsid dedupe/zero", c == 2 && a[0] == 3 && a[1] == 4, "%u", c);
    c = n48cb_addsid(a, c, 4, 5); c = n48cb_addsid(a, c, 4, 6); c = n48cb_addsid(a, c, 4, 7);
    CHECK("addsid cap", c == 4, "%u", c);
    // Drop: a dropped writer is no longer pending.
    n48cb_init(&T);
    n48cb_commit(&T, 1, 1, 1, w1, 1, none, 0); n48cb_commit(&T, 2, 2, 2, none, 0, r1, 1); n48cb_drop(&T, 1);
    n = sub(2, 2, 3, 1, r1, inv, 32, &sq);
    CHECK("drop", n == 0 && T.drops == 1, "0");
    // Submit without a registered commit: counted, commit assumed = now, so earlier pending writers still count.
    n48cb_init(&T);
    n48cb_commit(&T, 1, 1, 1, w1, 1, none, 0);
    n = sub(2, 2, 3, 1, r1, inv, 32, &sq);
    CHECK("no commit registered", n == 1 && T.no_commit == 1, "n=%u", n);
    // Eviction: more than N48CB_TAB pending commits evicts the oldest, never crashes; the evicted writer is forgotten.
    n48cb_init(&T);
    n48cb_commit(&T, 1000, 1, 1, w1, 1, none, 0);
    for (int i = 0; i < N48CB_TAB; i++) n48cb_commit(&T, 1 + i, 1, 100 + i, none, 0, none, 0);
    CHECK("evicted oldest", T.evicted == 1 && n48cb_find(&T, 1000) == NULL, "evicted %llu", (unsigned long long)T.evicted);
    // Re-commit of the same id replaces the entry (no duplicate).
    n48cb_init(&T);
    n48cb_commit(&T, 1, 1, 1, w1, 1, none, 0); n48cb_commit(&T, 1, 1, 2, none, 0, none, 0); n48cb_commit(&T, 2, 2, 3, none, 0, r1, 1);
    n = sub(2, 2, 4, 1, r1, inv, 32, &sq);
    CHECK("re-commit replaces", n == 0, "0");
    // Randomised model check against a brute-force reference.
    srand(12345); int bad = 0;
    for (int round = 0; round < 2000 && !bad; round++) {
        n48cb_init(&T); struct { int live; uint64_t c; uint32_t w[3], r[3]; } m[24]; memset(m, 0, sizeof m);
        int cnt = 2 + rand() % 22;
        for (int i = 0; i < cnt; i++) {
            uint32_t wl[3], rl[3], nw = rand() % 3, nr = rand() % 3; for (uint32_t k = 0; k < nw; k++) wl[k] = 1 + rand() % 5; for (uint32_t k = 0; k < nr; k++) rl[k] = 1 + rand() % 5;
            uint32_t dw[3], dr[3], cw = 0, cr = 0; for (uint32_t k = 0; k < nw; k++) cw = n48cb_addsid(dw, cw, 3, wl[k]); for (uint32_t k = 0; k < nr; k++) cr = n48cb_addsid(dr, cr, 3, rl[k]);
            m[i].live = 1; m[i].c = 100 + i; memcpy(m[i].w, dw, sizeof dw); memcpy(m[i].r, dr, sizeof dr); (void)cw; (void)cr;
            m[i].w[2] = cw; m[i].r[2] = cr;   // slot 2 holds the count when n<=2: keep it simple, cap lists at 2
            if (cw > 2) cw = m[i].w[2] = 2; if (cr > 2) cr = m[i].r[2] = 2;
            n48cb_commit(&T, 1 + i, 1, 100 + i, dw, cw, dr, cr);
        }
        for (int step = 0; step < cnt; step++) {
            int k; do { k = rand() % cnt; } while (!m[k].live);
            uint32_t exp = 0; for (uint32_t j = 0; j < m[k].r[2]; j++) for (int e = 0; e < cnt; e++) if (e != k && m[e].live && m[e].c < m[k].c) for (uint32_t q = 0; q < m[e].w[2]; q++) if (m[e].w[q] == m[k].r[j]) exp++;
            uint32_t got = n48cb_submit(&T, 1 + k, 1, 1000 + step, NULL, 0, m[k].r, m[k].r[2], NULL, 0, &sq); m[k].live = 0;
            if (got != exp) { bad = 1; printf("FAIL model: round %d step %d got %u exp %u\n", round, step, got, exp); }
        }
    }
    CHECK("model check 2000 rounds", !bad, "matches brute force");
    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails != 0;
}
