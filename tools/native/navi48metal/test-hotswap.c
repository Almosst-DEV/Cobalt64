// Host test for n48_hotswap.h:  cc -O1 -Wall -Wextra -Werror -pthread -o /tmp/test-hotswap test-hotswap.c && /tmp/test-hotswap
#include <stdio.h>
#include <pthread.h>
#include "n48_hotswap.h"
static int fails, checks;
#define CK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define S 1000000000LL
static n48hs_key_obs mk(int spv, uint64_t sz, int64_t mt, int meta) {
    n48hs_key_obs k = { { spv, sz, mt }, { meta, meta ? 100 : 0, meta ? mt : 0 } }; return k; }

static void t_basic(void) {
    n48hs_entry e; n48hs_init(&e, 2, 1);
    n48hs_key_obs none[2] = { mk(0, 0, 0, 0), mk(0, 0, 0, 0) };
    CK(n48hs_tick(&e, 10 * S, none) == N48HS_NONE);
    n48hs_key_obs one[2] = { mk(1, 400, 10 * S, 1), mk(0, 0, 0, 0) };      // only the vertex file is there
    CK(n48hs_tick(&e, 11 * S, one) == N48HS_NONE); CK(n48hs_tick(&e, 12 * S, one) == N48HS_NONE);
    n48hs_key_obs both[2] = { mk(1, 400, 10 * S, 1), mk(1, 800, 13 * S, 1) };
    CK(n48hs_tick(&e, 13 * S + 100, both) == N48HS_NONE);                   // first sighting: not yet stable
    CK(n48hs_tick(&e, 13 * S + 200000000, both) == N48HS_NONE);             // stable but newest file younger than MIN_AGE
    CK(n48hs_tick(&e, 14 * S, both) == N48HS_BUILD); CK(e.st == N48HS_BUILDING); CK(e.attempts == 1);
    CK(n48hs_tick(&e, 15 * S, both) == N48HS_NONE);                          // one BUILD per attempt
    CK(n48hs_build_done(&e, 1, 15 * S) == 1); CK(e.st == N48HS_SWAPPED);
    CK(n48hs_tick(&e, 16 * S, both) == N48HS_NONE);
    CK(n48hs_build_done(&e, 1, 16 * S) == 0);                                // a second completion is ignored
}
static void t_partial_file(void) {
    n48hs_entry e; n48hs_init(&e, 1, 1);
    n48hs_key_obs a = mk(1, 400, 10 * S, 1), b = mk(1, 800, 11 * S, 1), odd = mk(1, 402, 10 * S, 1);
    CK(n48hs_tick(&e, 20 * S, &a) == N48HS_NONE);
    CK(n48hs_tick(&e, 21 * S, &b) == N48HS_NONE);                            // grew between ticks: never stable on the first sighting
    CK(n48hs_tick(&e, 22 * S, &b) == N48HS_BUILD);
    n48hs_entry f; n48hs_init(&f, 1, 1);
    CK(n48hs_tick(&f, 20 * S, &odd) == N48HS_NONE); CK(n48hs_tick(&f, 21 * S, &odd) == N48HS_NONE); CK(f.st == N48HS_WAIT);   // size % 4 != 0
    n48hs_key_obs tiny = mk(1, 16, 10 * S, 1);
    CK(n48hs_tick(&f, 22 * S, &tiny) == N48HS_NONE); CK(n48hs_tick(&f, 23 * S, &tiny) == N48HS_NONE);                         // < 20 bytes
}
static void t_meta(void) {
    n48hs_entry c; n48hs_init(&c, 1, 1);                                     // compute: meta required forever
    n48hs_key_obs nm = mk(1, 400, 10 * S, 0);
    for (int i = 0; i < 20; i++) CK(n48hs_tick(&c, (20 + i) * S, &nm) == N48HS_NONE);
    n48hs_key_obs wm = mk(1, 400, 10 * S, 1); wm.meta.mtime_ns = 10 * S;
    CK(n48hs_tick(&c, 50 * S, &wm) == N48HS_NONE); CK(n48hs_tick(&c, 51 * S, &wm) == N48HS_BUILD);
    n48hs_entry r; n48hs_init(&r, 1, 0);                                     // render: proceeds without meta only after the grace period
    CK(n48hs_tick(&r, 11 * S, &nm) == N48HS_NONE); CK(n48hs_tick(&r, 12 * S, &nm) == N48HS_NONE);
    CK(n48hs_tick(&r, 14 * S, &nm) == N48HS_NONE);                           // 4 s old < 5 s grace
    CK(n48hs_tick(&r, 16 * S, &nm) == N48HS_BUILD);
}
static void t_failure_backoff(void) {
    n48hs_entry e; n48hs_init(&e, 1, 1); n48hs_key_obs a = mk(1, 400, 10 * S, 1);
    int64_t t = 20 * S; n48hs_tick(&e, t, &a);
    CK(n48hs_tick(&e, t += S, &a) == N48HS_BUILD);
    CK(n48hs_build_done(&e, 0, t) == 0); CK(e.st == N48HS_WAIT); CK(e.attempts == 1); CK(e.next_try_ns == t + 2 * S);
    CK(n48hs_tick(&e, t += S, &a) == N48HS_NONE);                            // backoff (2 s) not over yet
    CK(n48hs_tick(&e, t += S, &a) == N48HS_BUILD);                           // backoff elapsed (>= 2 s)
    CK(n48hs_build_done(&e, 0, t) == 0);
    for (int i = 0; i < 20 && e.st == N48HS_WAIT; i++) { if (n48hs_tick(&e, t += S, &a) == N48HS_BUILD) n48hs_build_done(&e, 0, t); }
    CK(e.st == N48HS_GAVEUP); CK(e.attempts == N48HS_MAX_ATTEMPTS);
    CK(n48hs_tick(&e, t += 100 * S, &a) == N48HS_NONE);                      // terminal
    // changed files after a failure reset the budget
    n48hs_entry g; n48hs_init(&g, 1, 1); int64_t u = 20 * S; n48hs_tick(&g, u, &a);
    CK(n48hs_tick(&g, u += S, &a) == N48HS_BUILD); n48hs_build_done(&g, 0, u); CK(g.attempts == 1);
    n48hs_key_obs b = mk(1, 800, 30 * S, 1);
    n48hs_tick(&g, u += 5 * S, &b); CK(g.attempts == 0 && g.next_try_ns == 0);
}
static void t_deferred_destroy(void) {
    n48hs_entry e; n48hs_init(&e, 1, 1); n48hs_key_obs a = mk(1, 400, 10 * S, 1);
    int g1 = n48hs_acquire(&e), g2 = n48hs_acquire(&e); CK(g1 == 0 && g2 == 0 && e.users[0] == 2);   // two command buffers encode with the fallback
    CK(n48hs_destroy_ok(&e) == 0);                                                                   // not swapped: never destroy
    n48hs_tick(&e, 20 * S, &a); CK(n48hs_tick(&e, 21 * S, &a) == N48HS_BUILD); CK(n48hs_build_done(&e, 1, 21 * S) == 1);
    CK(n48hs_destroy_ok(&e) == 0);                                                                   // swapped, but two cbs in flight
    int g3 = n48hs_acquire(&e); CK(g3 == 1 && e.users[0] == 2 && e.users[1] == 1);                   // new work gets the real generation
    n48hs_release(&e, g1); CK(n48hs_destroy_ok(&e) == 0);
    n48hs_release(&e, g2); CK(e.users[0] == 0);
    CK(n48hs_destroy_ok(&e) == 1); CK(n48hs_destroy_ok(&e) == 0);                                    // exactly once
    n48hs_release(&e, g3); n48hs_release(&e, g3); CK(e.users[1] == 0);                               // over-release is clamped
    n48hs_release(&e, 7); n48hs_release(&e, -1);
    // swapped with no users at all: destroy at once
    n48hs_entry f; n48hs_init(&f, 1, 1); n48hs_tick(&f, 20 * S, &a); n48hs_tick(&f, 21 * S, &a); n48hs_build_done(&f, 1, 21 * S); CK(n48hs_destroy_ok(&f) == 1);
    // a failed build never retires the fallback
    n48hs_entry h; n48hs_init(&h, 1, 1); n48hs_tick(&h, 20 * S, &a); n48hs_tick(&h, 21 * S, &a); n48hs_build_done(&h, 0, 21 * S); CK(n48hs_destroy_ok(&h) == 0);
}
static _Atomic uintptr_t slot; static _Atomic int winners, bad;
static void *racer(void *arg) { uintptr_t v = (uintptr_t)arg; if (n48hs_install(&slot, v)) atomic_fetch_add(&winners, 1); return NULL; }
static void *reader(void *arg) {
    (void)arg; uintptr_t seen = 0;
    for (int i = 0; i < 2000000; i++) { uintptr_t v = n48hs_load(&slot); if (v && !seen) seen = v; if (seen && v != seen) atomic_fetch_add(&bad, 1); if (!v && seen) atomic_fetch_add(&bad, 1); }
    return NULL;
}
static void t_publish_race(void) {
    for (int round = 0; round < 50; round++) {
        atomic_store(&slot, 0); atomic_store(&winners, 0); atomic_store(&bad, 0);
        pthread_t r[4], w[4]; for (int i = 0; i < 4; i++) pthread_create(&r[i], NULL, reader, NULL);
        for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, racer, (void *)(uintptr_t)(0x1000 + 0x10 * (i + 1)));
        for (int i = 0; i < 4; i++) { pthread_join(w[i], NULL); pthread_join(r[i], NULL); }
        CK(atomic_load(&winners) == 1); CK(atomic_load(&bad) == 0); CK(n48hs_load(&slot) >= 0x1010 && n48hs_load(&slot) <= 0x1040);
    }
}
int main(void) {
    t_basic(); t_partial_file(); t_meta(); t_failure_backoff(); t_deferred_destroy(); t_publish_race();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
