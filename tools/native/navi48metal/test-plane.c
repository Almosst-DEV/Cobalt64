// test-plane.c: n48_plane.h (P2 multi-plane IOSurface geometry) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-plane test-plane.c && /tmp/test-plane
#include <stdio.h>
#include <string.h>
#include "n48_plane.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
// run both stages as the .m does
static int run(const n48pl_in *in, n48pl_out *o) { return n48pl_geom(in, o) && n48pl_bound(in, o); }

int main(void) {
    n48pl_out o;
    // 420v 1920x1080: plane 0 1920x1080 bpe1 bpr 1920 (aligned to 64 -> 1920), 2073600 bytes; plane 1 960x540 bpe 2 bpr 1920 at offset 2073600 (+ page padding below)
    const uint64_t P0 = 2073600, ALLOC = 2073600 + 1920 * 540;
    n48pl_in p0 = { .pc = 2, .plane = 0, .pw = 1920, .ph = 1080, .pbpe = 1, .pbpr = 1920, .poff = 0, .alloc = ALLOC, .dw = 1920, .dh = 1080, .dbpp = 1 };
    n48pl_in p1 = { .pc = 2, .plane = 1, .pw = 960, .ph = 540, .pbpe = 2, .pbpr = 1920, .poff = P0, .alloc = ALLOC, .dw = 960, .dh = 540, .dbpp = 2 };
    CHECK("420v plane 0 accepted", run(&p0, &o) && o.off == 0 && o.rowlen == 1920, "off %llu rowlen %zu", (unsigned long long)o.off, o.rowlen);
    CHECK("420v plane 1 accepted at its offset, rowlen = bpr/bpe", run(&p1, &o) && o.off == P0 && o.rowlen == 960, "off %llu rowlen %zu", (unsigned long long)o.off, o.rowlen);
    n48pl_in q = p1; q.plane = 2;
    CHECK("plane == count refused (74)", !run(&q, &o) && o.code == 74, "code %d: %s", o.code, o.why);
    q = p1; q.plane = 7;
    CHECK("plane > count refused (74)", !run(&q, &o) && o.code == 74, "code %d: %s", o.code, o.why);
    q = p1; q.pc = 3; q.plane = 2;
    CHECK("3-plane surface: plane 2 accepted", run(&q, &o), "code %d", o.code);
    q = p1; q.poff = ALLOC - 1920 * 540 + 2;
    CHECK("offset + size overflowing alloc refused (81)", !run(&q, &o) && o.code == 81, "code %d: %s", o.code, o.why);
    q = p1; q.poff = UINT64_MAX;
    CHECK("plane below the base (poff = UINT64_MAX) refused (81)", !run(&q, &o) && o.code == 81, "code %d: %s", o.code, o.why);
    q = p1; q.poff = P0 + 1;
    CHECK("offset not a multiple of the element size refused (81 or 93)", !run(&q, &o) && (o.code == 81 || o.code == 93), "code %d: %s", o.code, o.why);
    q = p1; q.alloc = ALLOC + 4096; q.poff = P0 + 1;
    CHECK("odd offset inside alloc refused (93)", !run(&q, &o) && o.code == 93, "code %d: %s", o.code, o.why);
    q = p1; q.dw = 1920; q.dh = 1080;
    CHECK("descriptor 1920x1080 on plane 1 refused (75)", !run(&q, &o) && o.code == 75, "code %d: %s", o.code, o.why);
    q = p1; q.dbpp = 1;
    CHECK("R8 descriptor on the RG8 plane refused (76)", !run(&q, &o) && o.code == 76, "code %d: %s", o.code, o.why);
    q = p1; q.pbpr = 1000;
    CHECK("bytesPerRow below width*bpe refused (77)", !run(&q, &o) && o.code == 77, "code %d: %s", o.code, o.why);
    CHECK("exact fit (poff + bpr*h == alloc) accepted", run(&p1, &o) && P0 + 1920ull * 540 == ALLOC, "ok");
    q = p1; q.alloc = ALLOC - 1;
    CHECK("one byte short refused (81)", !run(&q, &o) && o.code == 81, "code %d", o.code);
    q = p1; q.pbpr = 0xFFFFFFFFFFFF0000ull; q.ph = 0x100000;
    CHECK("bpr*height overflow refused", !n48pl_bound(&q, &o) && o.code == 81, "code %d: %s", o.code, o.why);
    // a plane 1 that would pass with plane 0's geometry must not: using plane 0's bpr (1920 for 960x2) is the same here, so check bpr 960 (plane 0 of a half-width layout)
    q = p1; q.pbpr = 960;
    CHECK("RG8 plane with a 960-byte row (< 960*2) refused (77)", !run(&q, &o) && o.code == 77, "code %d", o.code);
    q = p1; q.pbpr = 2048; q.alloc = P0 + 2048ull * 540;
    CHECK("padded plane 1 row (2048) -> bufferRowLength 1024 texels, not the width", run(&q, &o) && o.rowlen == 1024, "rowlen %zu", o.rowlen);
    // build 7: a descriptor inside the plane (Core Image intermediates), RGBA16Float bpe 8
    n48pl_in ci = { .pc = 1, .plane = 0, .pw = 320, .ph = 576, .pbpe = 8, .pbpr = 2560, .poff = 0, .alloc = 2560ull * 576, .dw = 308, .dh = 514, .dbpp = 8 };
    CHECK("308x514 on 320x576 (bpe 8, bpr 2560) accepted, rowlen 320", run(&ci, &o) && o.rowlen == 320, "ok %d code %d rowlen %zu", o.ok, o.code, o.rowlen);
    q = ci; q.dw = 330;
    CHECK("330x514 on 320x576 refused (75)", !run(&q, &o) && o.code == 75, "code %d: %s", o.code, o.why);
    q = ci; q.dh = 577;
    CHECK("308x577 on 320x576 refused (75)", !run(&q, &o) && o.code == 75, "code %d: %s", o.code, o.why);
    q = ci; q.dw = 320; q.dh = 576;
    CHECK("320x576 exact unchanged", run(&q, &o) && o.rowlen == 320, "rowlen %zu", o.rowlen);
    // single plane surfaces: exactly today's answers
    n48pl_in s1 = { .pc = 1, .plane = 0, .pw = 64, .ph = 64, .pbpe = 4, .pbpr = 256, .poff = 0, .alloc = 16384, .dw = 64, .dh = 64, .dbpp = 4 };
    CHECK("pc 1 plane 0 accepted as today", run(&s1, &o) && o.off == 0 && o.rowlen == 64, "rowlen %zu", o.rowlen);
    n48pl_in s0 = s1; s0.pc = 0;
    CHECK("pc 0 plane 0 accepted as today", run(&s0, &o) && o.off == 0 && o.rowlen == 64, "rowlen %zu", o.rowlen);
    q = s1; q.plane = 1;
    CHECK("pc 1 plane 1 refused 74 'plane 1 of a single-plane surface'", !run(&q, &o) && o.code == 74 && !strcmp(o.why, "plane 1 of a single-plane surface"), "%s", o.why);
    q = s0; q.plane = 1;
    CHECK("pc 0 plane 1 refused 74", !run(&q, &o) && o.code == 74, "code %d", o.code);
    q = s1; q.alloc = 256 * 64 - 1;
    CHECK("pc 1: bpr*height > alloc refused 81 as today", !run(&q, &o) && o.code == 81, "code %d: %s", o.code, o.why);
    // path (a) bind rule
    CHECK("bind: offset multiple of alignment and image fits", n48pl_bind_ok(P0, 1920 * 540, 256, ALLOC + 1000), "");
    CHECK("bind: offset not a multiple of the alignment -> path (b)", !n48pl_bind_ok(P0, 1920 * 540, 4096, ALLOC + 1000), "2073600 %% 4096 = %llu", (unsigned long long)(P0 % 4096));
    CHECK("bind: image does not fit behind the offset -> path (b)", !n48pl_bind_ok(P0, 1920 * 540 + 1, 256, ALLOC), "");
    CHECK("bind: offset beyond the import -> path (b)", !n48pl_bind_ok(ALLOC + 1, 0, 256, ALLOC), "");
    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails != 0;
}
