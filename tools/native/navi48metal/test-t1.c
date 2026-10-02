// test-t1.c: host test of n48_t1.h.  cc -O1 -Wall -Wextra -Werror -o /tmp/test-t1 test-t1.c && /tmp/test-t1
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "n48_t1.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static n48t_hist H;
static void *worker(void *a) { (void)a; for (int i = 0; i < 100000; i++) n48t_add(&H, 1000 + (uint64_t)(i % 7)); return NULL; }
int main(void) {
    CHECK("bucket edges", n48t_bucket(0) == 0 && n48t_bucket(99999) == 0 && n48t_bucket(100000) == 1 && n48t_bucket(399999) == 1 && n48t_bucket(400000) == 2 &&
          n48t_bucket(409599999ULL) == 6 && n48t_bucket(409600000ULL) == 7 && n48t_bucket(~0ULL) == 7, "0.1ms x4 steps, last bucket open");
    n48t_snap s; n48t_take(&H, &s); CHECK("empty", s.n == 0 && s.min == 0 && s.max == 0, "n=%llu", (unsigned long long)s.n);
    char buf[512]; CHECK("empty fmt", n48t_fmt("x", &s, buf, sizeof buf) == 0, "no line for an empty window");
    n48t_add(&H, 2000000); n48t_add(&H, 500000); n48t_add(&H, 30000000);
    n48t_take(&H, &s);
    CHECK("three samples", s.n == 3 && s.sum == 32500000 && s.min == 500000 && s.max == 30000000 && s.b[2] == 1 && s.b[3] == 1 && s.b[5] == 1 && s.b[0] + s.b[1] + s.b[4] + s.b[6] + s.b[7] == 0,
          "n=%llu sum=%llu min=%llu max=%llu (0.5ms->b2, 2ms->b3, 30ms->b5)", (unsigned long long)s.n, (unsigned long long)s.sum, (unsigned long long)s.min, (unsigned long long)s.max);
    n48t_fmt("pipe", &s, buf, sizeof buf); printf("     %s\n", buf);
    CHECK("fmt content", strstr(buf, "T1 pipe n=3 avg=10.833ms min=0.500ms max=30.000ms") != NULL, "avg 10.833");
    n48t_take(&H, &s); CHECK("drained", s.n == 0, "second take empty");
    n48t_add(&H, 0); n48t_take(&H, &s); CHECK("zero-ns sample keeps min 0", s.n == 1 && s.min == 0 && s.max == 0, "min=%llu", (unsigned long long)s.min);
    pthread_t t[4]; for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    n48t_take(&H, &s); CHECK("4 threads", s.n == 400000 && s.min == 1000 && s.max == 1006 && s.b[0] == 400000, "n=%llu min=%llu max=%llu", (unsigned long long)s.n, (unsigned long long)s.min, (unsigned long long)s.max);

    // ---- pipeline cache file ----
    uint8_t key[32], uuid[16], key2[32], uuid2[16]; for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; key2[i] = (uint8_t)(i ^ 0x55); } for (int i = 0; i < 16; i++) { uuid[i] = (uint8_t)(0xA0 + i); uuid2[i] = (uint8_t)i; }
    size_t plen = 1000; uint8_t *pay = malloc(plen); for (size_t i = 0; i < plen; i++) pay[i] = (uint8_t)(i * 7);
    uint32_t vk[4] = { 32, 1, 0x1002, 0x7550 }; memcpy(pay, vk, 16); memcpy(pay + 16, uuid, 16);
    size_t flen = sizeof(n48pc_hdr) + plen; uint8_t *f = malloc(flen);
    n48pc_hdr h; n48pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    const void *pp = NULL; size_t pl = 0;
    CHECK("header size is 88", sizeof(n48pc_hdr) == 88, "%zu", sizeof(n48pc_hdr));
    int c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("valid file", c == N48PC_OK && pl == plen && pp == f + sizeof h, "%s", n48pc_why(c));
    c = n48pc_check(f, flen - 1, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("truncated by 1", c == N48PC_LEN, "%s", n48pc_why(c));
    c = n48pc_check(f, sizeof h - 1, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("shorter than header", c == N48PC_SHORT, "%s", n48pc_why(c));
    c = n48pc_check(f, 0, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("empty file", c == N48PC_SHORT, "%s", n48pc_why(c));
    c = n48pc_check(f, flen, key2, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("other build key", c == N48PC_KEY, "%s", n48pc_why(c));
    c = n48pc_check(f, flen, key, uuid2, 0x1002, 0x7550, &pp, &pl); CHECK("other UUID", c == N48PC_UUID, "%s", n48pc_why(c));
    c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7551, &pp, &pl); CHECK("other device id", c == N48PC_UUID, "%s", n48pc_why(c));
    f[sizeof h + 500] ^= 1; c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("flipped payload bit", c == N48PC_HASH, "%s", n48pc_why(c)); f[sizeof h + 500] ^= 1;
    f[0] = 'X'; c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("bad magic", c == N48PC_MAGIC, "%s", n48pc_why(c)); f[0] = 'N';
    f[8] = 2; c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("version 2", c == N48PC_VERSION_BAD, "%s", n48pc_why(c)); f[8] = 1;
    // an inner Vulkan header that disagrees (self-consistent file, so only the inner check can catch it)
    pay[8] = 9; n48pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("inner vk vendor wrong", c == N48PC_VKHDR, "%s", n48pc_why(c));
    pay[8] = 0x02; pay[9] = 0x10; pay[10] = 0; pay[11] = 0; memcpy(pay + 16, uuid2, 16); n48pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    c = n48pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("inner vk uuid wrong", c == N48PC_VKHDR, "%s", n48pc_why(c));
    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
