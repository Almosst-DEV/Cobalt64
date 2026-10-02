// gfx_t0src_test.cpp — build 0.0.524 (notes/design/T0SRC.md items 1-7, ; src/apple/gfx_t0src.h): switch 79, who
// wrote S's texture 0. READ-ONLY instrumentation. What is proven here:
//   RINGS    the newest (not the oldest) covering map / unmap is chosen, before and after a wrap; wraps drop the oldest; a slot
//            claimed but unpublished, a slot whose seq names another slot, and a slot rewritten DURING the copy all read TORN; the
//            same memory object mapped by another context is found (never the same context; mem 0 never matches); `rc` keeps its
//            low byte only; the class table interns, caps and names; only the exact VidMemory class name qualifies for +0x40.
//   PROBES   (a) a system page is never a VRAM key (it reads as unresolved); (c) the ledger, (d) the hazard set and (e) the
//            residency table answer by range at both edges, size 0 included, and (d) moves no hazard counter; the line cap.
//   LINES    the three formats fit N48_LOG_CAP_BODY at maximal fields with a 64-character class name (C2).
//   GLUE     switch 79 is OFF at boot and joins the mid-arm guard; the map note comes AFTER Apple's mapVA returns (C3); the unmap
//            note is at entry; the probe runs only under 79 and under its cap before its MM reads (C1), after the B1 lines; it
//            asks n48_hz_probe, never n48_hz_hit; no new body holds a write primitive; item 6's watch index and item 7's counter.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_t0src_test.cpp -o /tmp/t0 && \
//         /tmp/t0 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/Navi48AccelPeer.cpp \
//         src/navi48-bringup/src/apple/gfx_t0src.h
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

// The torn-slot plant: a writer that moves a slot's seq between the reader's copy and its second read.
static uint64_t *gTornTarget = nullptr;
static void t0_torn_hook(const void *slot)
{
    if (gTornTarget && (const void *)gTornTarget == slot) { *gTornTarget += 1024ull; gTornTarget = nullptr; }
}
#define N48_T0_TORN_HOOK(slot) t0_torn_hook((const void *)(slot))
#include "gfx_t0src.h"
#include "gfx_clock88.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::string body(const std::string &src, const char *sig)
{
    const size_t a = src.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}
static uint32_t count(const std::string &h, const char *n)
{
    uint32_t c = 0; for (size_t p = h.find(n); p != std::string::npos; p = h.find(n, p + 1)) c++; return c;
}
// the write primitives no read-only body may hold (gfx_clock88_test.cpp's list, plus the ledger / resprov / hazard writers)
static uint32_t writes_in(const std::string &b)
{
    static const char *const kW[] = { "write_mm(", "writeBytes", "WVRAM", "navi48_fc_chunk", "IOMalloc", "memcpy(", "memset(",
                                      "WREG", "wreg(", "navi48_cg_open", "->write", "hw_", "gXdNew[", "gXdOut[", "n48_hz_hit(",
                                      "n48_hz_add(", "n48_dl_set(", "n48_dl_clear(", "n48_rp_record(", "n48_rp_unmap" };
    uint32_t c = 0; for (const char *w : kW) c += count(b, w); return c;
}

static n48_t0_mapring gM;
static n48_t0_unmapring gU;

static void map_put(uint64_t va, uint64_t size, uint32_t ctx, uint64_t mem, uint64_t tag)
{
    (void)n48_t0_map_note(&gM, tag, tag, ctx, (int32_t)ctx, va, 0ull, size, mem, 0ull, 0u, 0u, 1ull, 0u);
}

static void ring_checks()
{
    std::printf("== RINGS ==\n");
    gM = n48_t0_mapring {};
    n48_t0_map o {};
    uint32_t torn = 0u;
    expect_u("an empty ring finds nothing and reads no torn slot", n48_t0_map_newest(&gM, 0x401000000ull, &o, &torn) * 0x10u + torn, 0u);
    // three maps cover 0x401000000 (seq 1, 3, 5); two do not
    map_put(0x401000000ull, 0x10000ull, 5u, 0xffffff8000001000ull, 1u);
    map_put(0x402000000ull, 0x10000ull, 5u, 0xffffff8000002000ull, 2u);
    map_put(0x400ff0000ull, 0x30000ull, 7u, 0xffffff8000003000ull, 3u);
    map_put(0x403000000ull, 0x10000ull, 5u, 0xffffff8000004000ull, 4u);
    map_put(0x401000000ull, 0x1000ull, 9u, 0xffffff8000005000ull, 5u);
    torn = 0u;
    expect_u("NEWEST-NOT-OLDEST: the newest of three covering maps (seq 5), not seq 1 or 3",
             n48_t0_map_newest(&gM, 0x401000000ull, &o, &torn) ? o.seq : 0u, 5u);
    expect_u("NEWEST-NOT-OLDEST: its fields (ctx 9, up 5)", o.ctxSeq * 0x100u + o.up_ms, 0x905u);
    expect_u("a VA inside only the seq-3 map's range (0x401010000) finds seq 3",
             n48_t0_map_newest(&gM, 0x401010000ull, &o, &torn) ? o.seq : 0u, 3u);
    expect_u("an end-exclusive edge: 0x401010000 is past seq 1's [0x401000000,+0x10000) and seq 5's 4 KiB", o.seq == 3u, 1u);
    expect_u("an uncovered VA finds nothing", n48_t0_map_newest(&gM, 0x405000000ull, &o, &torn), 0u);
    expect_u("no torn slot in a quiet ring", torn, 0u);
    // SIZE 0: a record still names its own first page
    map_put(0x406000000ull, 0ull, 3u, 0ull, 6u);
    expect_u("a size-0 map covers its first page", n48_t0_map_newest(&gM, 0x406000fffull, &o, &torn) ? o.seq : 0u, 6u);
    expect_u("a size-0 map does not cover the next page", n48_t0_map_newest(&gM, 0x406001000ull, &o, &torn), 0u);
    // rc: the LOW BYTE only (0xbe121be sete %r12b / movl %r12d,%eax: the upper bits are flag residue)
    (void)n48_t0_map_note(&gM, 7u, 7u, 1u, 1, 0x407000000ull, 0x40ull, 0x2000ull, 0xffffff8000007000ull, 0x2000ull, 0x10004u, 0x7u,
                          0xdeadbeef00000101ull, 2u);
    expect_u("RC: 0xdeadbeef00000101 records rc 1 (the low byte), never 0x101",
             n48_t0_map_newest(&gM, 0x407000000ull, &o, &torn) ? o.rc8 : 0xbadu, 1u);
    expect_u("RC: rc 0xff00 records 0", (n48_t0_map_note(&gM, 8u, 8u, 1u, 1, 0x408000000ull, 0, 0x1000, 0, 0, 0, 0, 0xff00ull, 0u),
                                          n48_t0_map_newest(&gM, 0x408000000ull, &o, &torn) ? o.rc8 : 0xbadu), 0u);
    expect_u("the map's off/size/memLen/mem0c/flags/cls are kept", (o.seq == 8u) &&
             (n48_t0_map_newest(&gM, 0x407000000ull, &o, &torn), o.off == 0x40u && o.size == 0x2000u && o.memLen == 0x2000u &&
              o.mem0c == 0x10004u && o.flags == 7u && o.cls == 2u), 1u);

    // CROSS-CONTEXT: another context mapping the same memory object
    gM = n48_t0_mapring {};
    map_put(0x401000000ull, 0x10000ull, 5u, 0xffffff80000aa000ull, 1u);    // WindowServer-like ctx 5
    map_put(0x500000000ull, 0x10000ull, 11u, 0xffffff80000aa000ull, 2u);   // ctx 11 maps the same mem elsewhere
    map_put(0x501000000ull, 0x10000ull, 12u, 0xffffff80000bb000ull, 3u);   // another mem
    map_put(0x502000000ull, 0x10000ull, 5u, 0xffffff80000aa000ull, 4u);    // ctx 5 again: never "other"
    map_put(0x503000000ull, 0x10000ull, 13u, 0xffffff80000aa000ull, 5u);   // ctx 13, the newest other
    torn = 0u;
    expect_u("CROSS-CTX: the newest OTHER context mapping the same mem (ctx 13, seq 5)",
             n48_t0_map_other(&gM, 0xffffff80000aa000ull, 5u, &o, &torn) ? o.ctxSeq * 0x100u + o.seq : 0u, 0xd05u);
    expect_u("CROSS-CTX: its VA", o.va, 0x503000000ull);
    expect_u("CROSS-CTX: asked for ctx 13, the other is ctx 11 or 5's newest (seq 4, ctx 5)",
             n48_t0_map_other(&gM, 0xffffff80000aa000ull, 13u, &o, &torn) ? o.ctxSeq * 0x100u + o.seq : 0u, 0x504u);
    expect_u("CROSS-CTX: mem 0 matches nothing", n48_t0_map_other(&gM, 0ull, 5u, &o, &torn), 0u);
    expect_u("CROSS-CTX: a mem only its own context mapped has no other", n48_t0_map_other(&gM, 0xffffff80000bb000ull, 12u, &o, &torn), 0u);

    // WRAP: 1024 + 10 maps; the first 10 are gone; the newest after the wrap sits at a LOW slot
    gM = n48_t0_mapring {};
    for (uint64_t i = 1; i <= N48_T0_MAP_N + 10u; i++) map_put(0x600000000ull + (i % 7u) * 0x1000ull, 0x1000ull, 1u, i, i);
    expect_u("WRAP: head 1034, one full lap", n48_t0_wraps(gM.head, N48_T0_MAP_N) * 0x10000u + gM.head, 0x1040au);
    expect_u("WRAP: slot 3 now holds seq 1028 (the older seq 4 is gone)", n48_t0_map_get(&gM, 3u, &o) == N48_T0_SLOT_OK ? o.seq : 0u, 1028u);
    torn = 0u;
    // VA 0x600000000 is mapped by every i % 7 == 0: the newest is i = 1029 (slot 4), the oldest survivor i = 14 (slot 13).
    expect_u("WRAP NEWEST: the newest covering map after the wrap (seq 1029), not the oldest survivor (seq 14)",
             n48_t0_map_newest(&gM, 0x600000000ull, &o, &torn) ? o.seq : 0u, 1029u);
    expect_u("WRAP OTHER: no torn slot", torn, 0u);
    expect_u("WRAP: a mem only in the lost first lap (mem 3) is gone", n48_t0_map_other(&gM, 3ull, 99u, &o, &torn), 0u);

    // TORN: (1) a slot claimed but unpublished (seq 0 below head); (2) a seq naming another slot; (3) a rewrite during the copy
    gM = n48_t0_mapring {};
    for (uint64_t i = 1; i <= 6u; i++) map_put(0x700000000ull, 0x1000ull, 1u, i, i);
    gM.s[5].seq = 0ull;                         // seq 6 being written
    torn = 0u;
    expect_u("TORN 1: a claimed, unpublished slot is torn and skipped: the newest is seq 5",
             n48_t0_map_newest(&gM, 0x700000000ull, &o, &torn) ? o.seq : 0u, 5u);
    expect_u("TORN 1: counted once", torn, 1u);
    expect_u("TORN 1: an unclaimed slot (7, above head) is empty, not torn", n48_t0_map_get(&gM, 7u, &o), (uint64_t)N48_T0_SLOT_EMPTY);
    gM.s[4].seq = 3ull + N48_T0_MAP_N;          // slot 4 claims to be seq 1027 (slot 2's position)
    torn = 0u;
    expect_u("TORN 2: a seq naming another slot is torn (slot 4)", n48_t0_map_get(&gM, 4u, &o), (uint64_t)N48_T0_SLOT_TORN);
    expect_u("TORN 2: the newest is now seq 4", n48_t0_map_newest(&gM, 0x700000000ull, &o, &torn) ? o.seq : 0u, 4u);
    expect_u("TORN 2: two torn slots counted", torn, 2u);
    gTornTarget = &gM.s[3].seq;                 // a writer laps slot 3 while it is copied
    expect_u("TORN 3: a seq that changes during the copy is torn", n48_t0_map_get(&gM, 3u, &o), (uint64_t)N48_T0_SLOT_TORN);
    expect_u("TORN 3: the hook fired", gTornTarget == nullptr, 1u);

    // THE UNMAP RING: newest covering, size 0, wrap, torn
    gU = n48_t0_unmapring {};
    n48_t0_unmap u {};
    (void)n48_t0_unmap_note(&gU, 10u, 5u, 0x401000000ull, 0x20000ull);
    (void)n48_t0_unmap_note(&gU, 20u, 5u, 0x401000000ull, 0x10000ull);
    (void)n48_t0_unmap_note(&gU, 30u, 5u, 0x409000000ull, 0x10000ull);
    torn = 0u;
    expect_u("UNMAP NEWEST: seq 2 (up 20), not seq 1", n48_t0_unmap_newest(&gU, 0x401004000ull, &u, &torn) ? u.seq * 0x100u + u.up_ms : 0u, 0x214u);
    expect_u("UNMAP: 0x401010000 is covered only by seq 1", n48_t0_unmap_newest(&gU, 0x401010000ull, &u, &torn) ? u.seq : 0u, 1u);
    (void)n48_t0_unmap_note(&gU, 40u, 5u, 0x40a000000ull, 0ull);
    expect_u("UNMAP size 0 covers its first page only", n48_t0_unmap_newest(&gU, 0x40a000800ull, &u, &torn) * 0x10u +
             n48_t0_unmap_newest(&gU, 0x40a001000ull, &u, &torn), 0x10u);
    for (uint32_t i = 0; i < N48_T0_UNMAP_N; i++) (void)n48_t0_unmap_note(&gU, 100u + i, 6u, 0x800000000ull, 0x1000ull);
    expect_u("UNMAP WRAP: 260 notes, one lap; the seq-2 unmap is gone",
             n48_t0_wraps(gU.head, N48_T0_UNMAP_N) * 0x1000u + n48_t0_unmap_newest(&gU, 0x401004000ull, &u, &torn), 0x1000u);
    expect_u("UNMAP WRAP: the newest at 0x800000000 is seq 260 (up 355)",
             n48_t0_unmap_newest(&gU, 0x800000000ull, &u, &torn) ? u.seq * 0x1000u + u.up_ms : 0u, 260u * 0x1000u + 355u);
    gU.s[10].seq = 0ull;
    torn = 0u;
    (void)n48_t0_unmap_newest(&gU, 0x800000000ull, &u, &torn);
    expect_u("UNMAP TORN: an unpublished slot below head is counted torn", torn, 1u);
    gTornTarget = &gU.s[11].seq;
    expect_u("UNMAP TORN: a rewrite during the copy is torn", n48_t0_unmap_get(&gU, 11u, &u), (uint64_t)N48_T0_SLOT_TORN);
}

static void cls_checks()
{
    std::printf("== CLASS TABLE ==\n");
    static n48_t0_cls t;
    t = n48_t0_cls {};
    const char *l64 = "AMDRadeonX6000_AMDAccelVidMemoryABCDEFGHIJKLMNOPQRSTUVWXYZ012345";   // 64 characters
    const char *l70 = "AMDRadeonX6000_AMDAccelVidMemoryABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
    expect_u("a 64-character name", std::strlen(l64), 64u);
    expect_u("VIDMEM: the exact class name qualifies", n48_t0_is_vidmem("AMDRadeonX6000_AMDAccelVidMemory"), 1u);
    expect_u("VIDMEM: a longer name, a prefix, another class and null do not",
             n48_t0_is_vidmem(l64) + n48_t0_is_vidmem("AMDRadeonX6000_AMDAccelVidMem") + n48_t0_is_vidmem("AMDRadeonX6000_AMDAccelSysMemory") +
             n48_t0_is_vidmem(nullptr), 0u);
    expect_u("INTERN: first name -> 0, second -> 1, first again -> 0",
             n48_t0_intern(&t, "IOAccelMemory") * 0x100u + n48_t0_intern(&t, N48_T0_VIDMEM) * 0x10u + n48_t0_intern(&t, "IOAccelMemory"), 0x010u);
    expect_u("INTERN: a 64-character name is kept whole", n48_t0_intern(&t, l64), 2u);
    expect_u("INTERN: its stored name is 64 characters", std::strlen(n48_t0_cls_name(&t, 2u)), 64u);
    expect_u("INTERN: a 70-character name sharing those 64 is the same entry (64 compared)", n48_t0_intern(&t, l70), 2u);
    for (uint32_t i = 3; i < N48_T0_CLS_N; i++) { char nm[16]; std::snprintf(nm, sizeof nm, "Class%u", i); (void)n48_t0_intern(&t, nm); }
    expect_u("INTERN: the table full, a new name -> FULL", n48_t0_intern(&t, "OneMore"), (uint64_t)N48_T0_CLS_FULL);
    expect_u("INTERN: a known name still found when full", n48_t0_intern(&t, "Class5"), 5u);
    expect_u("INTERN: no name -> NONE", n48_t0_intern(&t, nullptr), (uint64_t)N48_T0_CLS_NONE);
    expect_u("NAME: NONE and FULL print as labels", std::strcmp(n48_t0_cls_name(&t, N48_T0_CLS_NONE), "(no class)") == 0 &&
             std::strcmp(n48_t0_cls_name(&t, N48_T0_CLS_FULL), "(class table full)") == 0, 1u);
}

static void probe_checks()
{
    std::printf("== PROBES ==\n");
    // (a) the page class: a SYSTEM page is never a VRAM key
    expect_u("SYS: walked + sys (even with an offset the converter accepted) is SYS", n48_t0_pg_class(1u, 1u, 1u), (uint64_t)N48_T0_PG_SYS);
    expect_u("SYS: a sys page reads as unresolved (not usable as a VRAM key)", n48_t0_pg_usable(n48_t0_pg_class(1u, 1u, 1u)), 0u);
    expect_u("SYS: walked, not sys, offset ok is VRAM and usable", n48_t0_pg_usable(n48_t0_pg_class(1u, 0u, 1u)), 1u);
    expect_u("UNRES: not walked, or no VRAM offset", n48_t0_pg_class(0u, 0u, 1u) * 0x10u + n48_t0_pg_class(1u, 0u, 0u), 0x22u);
    expect_u("PREFIX: '', 'sys:', 'unres:'", std::strcmp(n48_t0_pg_prefix(N48_T0_PG_VRAM), "") == 0 && std::strcmp(n48_t0_pg_prefix(N48_T0_PG_SYS), "sys:") == 0 &&
             std::strcmp(n48_t0_pg_prefix(N48_T0_PG_UNRES), "unres:") == 0, 1u);
    // the T# extent (clock88's field positions): 26x78 x 4 bytes = 8112 -> the last page is va + 0x1000
    uint32_t rec[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    rec[1] = (25u & 3u) << 30; rec[2] = (25u >> 2) | (77u << 14);
    expect_u("TEX: 26x78 x 4 B = 8112 bytes", n48_t0_tex_bytes(rec, 4u), 8112u);
    expect_u("TEX: a 1x1 texture is one page", (rec[1] = 0u, rec[2] = 0u, n48_t0_tex_bytes(rec, 4u)), 0x1000u);
    uint32_t w[64]; for (uint32_t i = 0; i < 64u; i++) w[i] = (i % 3u) ? 0u : i + 1u;
    expect_u("ZERO: 42 of 64", n48_t0_zeros(w, 64u), 42u);

    // (c) the ledger: page <= P < page + max(size, 4 KiB); page-0 entries never match
    static n48_dl l;
    l = n48_dl {};
    l.n = 4u;
    l.e[0] = n48_dl_ent { 0x11ull, 0x401000000ull, 0x8123db000ull, 0x3000ull, 0u, 0u, 7u, 1u };   // 3 pages
    l.e[1] = n48_dl_ent { 0x12ull, 0x402000000ull, 0x8123dd000ull, 0ull, 0u, 0u, 8u, 1u };       // size 0: one page
    l.e[2] = n48_dl_ent { 0x13ull, 0x403000000ull, 0ull, 0x100000ull, 0u, 0u, 9u, 1u };          // no page: no key
    l.e[3] = n48_dl_ent { 0x14ull, 0x404000000ull, 0x8123e0000ull, 0x1000ull, 0u, 0u, 10u, 1u };
    uint32_t tok = 0u; uint64_t lva = 0ull, lctx = 0ull;
    expect_u("LED: the base page: one entry (tok 7)", n48_t0_led_probe(&l, 0x8123db000ull, &tok, &lva, &lctx) * 0x100u + tok, 0x107u);
    expect_u("LED: its va and ctx", lva == 0x401000000ull && lctx == 0x11ull, 1u);
    expect_u("LED: the third page is inside (0x8123dd000: entry 0's last page AND entry 1's size-0 page)",
             n48_t0_led_probe(&l, 0x8123dd000ull, &tok, &lva, &lctx) * 0x100u + tok, 0x207u);
    expect_u("LED: size 0 covers its own page only (0x8123de000: nothing)", n48_t0_led_probe(&l, 0x8123de000ull, &tok, &lva, &lctx), 0u);
    expect_u("LED: the end edge is exclusive (0x8123e1000 past entry 3)", n48_t0_led_probe(&l, 0x8123e1000ull, &tok, &lva, &lctx), 0u);
    expect_u("LED: page 0 matches nothing (a key-less entry is no proof)", n48_t0_led_probe(&l, 0ull, &tok, &lva, &lctx), 0u);
    expect_u("LED: the ledger is unchanged (n, entry 1)", l.n == 4u && l.e[1].tok == 8u && l.asked == 0u, 1u);

    // (d) the hazard set: READ-ONLY - no counter moves; the exact row's namer and va
    static n48_hazard hz;
    hz = n48_hazard {};
    n48_hz_add(&hz, 0x8141af000ull, 0x4017f0000ull, 404u);
    n48_hz_add(&hz, 0x8141af000ull, 0x4017f0000ull, 405u);
    const uint64_t q0 = hz.queries, h0 = hz.hits, a0 = hz.added;
    uint64_t hits = 0ull, namer = 0ull, hva = 0ull; uint32_t ex = 0u;
    const uint32_t f1 = n48_t0_hz_probe(&hz, 0x8141af000ull, &hits, &ex, &namer, &hva);
    expect_u("HZ: a named page: filter 1, exact, hits 2, namer 404, va", f1 == 1u && ex == 1u && hits == 2u && namer == 404u && hva == 0x4017f0000ull, 1u);
    const uint32_t f2 = n48_t0_hz_probe(&hz, 0x8141b0000ull, &hits, &ex, &namer, &hva);
    expect_u("HZ: another page: not exact, hits 0, namer 0", ex == 0u && hits == 0u && namer == 0u && hva == 0u, 1u);
    (void)f2;
    expect_u("HZ READ-ONLY: queries, hits and added did not move (n48_hz_probe, never n48_hz_hit)",
             hz.queries == q0 && hz.hits == h0 && hz.added == a0 && q0 == 0u, 1u);

    // (e) resprov: vram <= P < vram + bytes; a 0-byte entry covers nothing
    static n48_rp rp;
    rp = n48_rp {};
    rp.n = 3u;
    rp.e[0].vram = 0x13e8b000ull; rp.e[0].bytes = 0x2000ull; rp.e[0].va = 0x400470000ull;
    rp.e[1].vram = 0x123db000ull; rp.e[1].bytes = 0ull; rp.e[1].va = 0x401710000ull;
    rp.e[2].vram = 0x13e8c000ull; rp.e[2].bytes = 0x1000ull; rp.e[2].va = 0x400480000ull;
    uint64_t rva = 0ull;
    expect_u("RP: the first byte of entry 0", n48_t0_rp_probe(&rp, 0x13e8b000ull, &rva) * 0x10u + (rva == 0x400470000ull), 0x11u);
    expect_u("RP: its last page, also entry 2's: two, the first named", n48_t0_rp_probe(&rp, 0x13e8c000ull, &rva) * 0x10u + (rva == 0x400470000ull), 0x21u);
    expect_u("RP: the end edge is exclusive (0x13e8d000)", n48_t0_rp_probe(&rp, 0x13e8d000ull, &rva), 0u);
    expect_u("RP: a 0-byte entry covers nothing, not even its own first byte", n48_t0_rp_probe(&rp, 0x123db000ull, &rva), 0u);

    // the line cap
    uint32_t lines = 0u, last = 0u, got = 0u;
    for (uint32_t i = 0; i < 40u; i++) { const uint32_t x = n48_t0_take_line(&lines); if (x) { got++; last = x; } }
    expect_u("CAP: 32 of 40, numbered 1..32", got * 0x100u + last, 0x2020u);

    // item 6: a FULL watch table leaves the caller's index untouched - which is why the kext initialises it to N48_C88_WATCH
    n48_c88_watch wt {};
    for (uint32_t i = 0; i < N48_C88_WATCH; i++) { uint32_t k = 0u; (void)n48_c88_watch_add(&wt, 0x400000000ull + i * 0x100000ull, 0x10000ull, 0, 0, &k); }
    uint32_t idx = N48_C88_WATCH;
    expect_u("ITEM 6: a full table does not add and does not set idx", n48_c88_watch_add(&wt, 0x402070000ull, 0x10000ull, 0, 0, &idx) * 0x10u + idx,
             (uint64_t)N48_C88_WATCH);
}

static void line_checks()
{
    std::printf("== LINES: under N48_LOG_CAP_BODY (%u) at maximal fields ==\n", N48_LOG_CAP_BODY);
    char bf[2048]; const unsigned long long M = 0xffffffffffffffffull; const uint32_t U = 0xffffffffu;
    const char *c64 = "AMDRadeonX6000_AMDAccelVidMemoryABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    int n = std::snprintf(bf, sizeof bf, N48_T0_FMT, M, M, "unres:", M, "unres:", M, U, U, U, U, U, U, U, U, M, U, M, U, M, M, U, M, U);
    expect_u("LINE t0src", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    expect_u("LINE t0src: the hazard fields are labelled `hz %u hits %llu exact %u namer`", count(std::string(N48_T0_FMT), "hz %u hits %llu exact %u namer %llu"), 1u);
    n = std::snprintf(bf, sizeof bf, N48_T0_MAP_FMT, M, M, U, -2147483647, M, (void *)0xffffffffffffffffull, c64, M, M, M, U, U, U,
                      M, M, U, -2147483647, M, U, U);
    expect_u("LINE t0map with a 64-character class name (C2)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    expect_u("LINE t0map: the 64-character class name is cut to its first 40 characters",
             (std::strstr(bf, c64) == nullptr && std::strstr(bf, " AMDRadeonX6000_AMDAccelVidMemoryABCDEFGH len ") != nullptr) ? 1u : 0u, 1u);
    n = std::snprintf(bf, sizeof bf, N48_T0_REPORT_FMT, "OFF (default)", " - `gfxneuter 79` REFUSED: a continuous arm stands, unchanged",
                      M, M, M, M, M, U, U, M, M, M, M, M);
    expect_u("LINE t0src report (with the longest verb suffix)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
}

static void glue_checks(const char *ahh, const char *peer, const char *hdr)
{
    std::printf("== GLUE ==\n");
    const std::string H = slurp(ahh), P = slurp(peer), X = slurp(hdr);
    if (H.empty() || P.empty() || X.empty()) { expect_u("GLUE: the three sources were read", 0u, 1u); return; }
    // SWITCH OFF AT BOOT
    expect_u("BOOT: N48_T0_BOOT_ON is 0 (OFF)", N48_T0_BOOT_ON, 0u);
    expect_u("BOOT: gT0On is declared once, at N48_T0_BOOT_ON", count(H, "static volatile uint32_t gT0On { N48_T0_BOOT_ON };"), 1u);
    expect_u("BOOT: the header's boot value is 0u", count(X, "#define N48_T0_BOOT_ON   0u"), 1u);
    expect_u("BOOT: gT0On's only assignment is the verb's", count(H, "gT0On = "), 1u);
    expect_u("BOOT: ... and it is `gT0On = f79;`", count(H, "if (changed79) gT0On = f79;"), 1u);
    // THE VERB (switch-76's pattern), and the mid-arm guard
    const size_t v0 = H.find("} else if ((arg & 0xffull) == 79ull) {");
    const size_t v1 = v0 == std::string::npos ? v0 : H.find("} else if ((arg & 0xffull) ==", v0 + 10);
    const std::string vb = (v0 == std::string::npos || v1 == std::string::npos) ? std::string() : H.substr(v0, v1 - v0);
    expect_u("VERB: one `79ull` selector", count(H, "(arg & 0xffull) == 79ull"), 1u);
    expect_u("VERB: the guard, n48_ra_set (M 1 ON, M 2 OFF), st 5 / st 11, the report", !vb.empty() &&
             count(vb, "n48_cm_cont_switch_refused(79u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             count(vb, "changed79 = n48_ra_set(m, &f79);") == 1u && count(vb, "if (contRefused79) st = 5;") == 1u &&
             count(vb, "if (!contRefused79 && !changed79 && m != 0u) st = 11;") == 1u && count(vb, "t0src_report_line(") == 1u, 1u);
    expect_u("VERB: 335 = 79 | 1 << 8 (ON), 591 = 79 | 2 << 8 (OFF)", (79u | (1u << 8)) * 0x1000u + (79u | (2u << 8)), 335u * 0x1000u + 591u);
    expect_u("VERB READ-ONLY: the verb's body holds no write primitive", writes_in(vb), 0u);
    // C3: the map note AFTER Apple's mapVA returned, once, switched
    const std::string mv = body(H, "static uint64_t hook_mapVA(void *self, uint64_t va, void *mem, uint64_t a, uint64_t b,");
    const size_t m1 = mv.find("const uint64_t rc = reinterpret_cast<Fn>(gOrigMapVA)(self, va, mem, a, b, flags);");
    const size_t m2 = mv.find("if (gT0On) t0src_map_note(e->seq, e->pid, va, mem, a, b, flags, rc);");
    const size_t m0 = mv.find("if (!e || e->state != 1)\n        return reinterpret_cast<Fn>(gOrigMapVA)(self, va, mem, a, b, flags);");
    expect_u("C3 ORDER: hook_mapVA: scope check -> Apple's mapVA -> the ring note", !mv.empty() && m0 != std::string::npos &&
             m1 != std::string::npos && m2 != std::string::npos && m0 < m1 && m1 < m2, 1u);
    expect_u("C3 ORDER: the note follows the LAST call of gOrigMapVA (whatever its spelling)",
             mv.rfind("gOrigMapVA)(") != std::string::npos && mv.find("t0src_map_note(") != std::string::npos &&
             mv.rfind("gOrigMapVA)(") < mv.find("t0src_map_note("), 1u);
    expect_u("C3: exactly one map note, and gOrigMapVA called exactly twice (scope return, delegate)",
             count(mv, "t0src_map_note(") * 0x10u + count(mv, "gOrigMapVA)("), 0x12u);
    // the unmap note at ENTRY (right after the scope check), once, switched
    const std::string uv = body(H, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    const size_t us = uv.find("if (!e || e->state != 1)\n        return reinterpret_cast<Fn>(gOrigUnmapVA)(self, va, size);\n");
    const size_t u0 = uv.find("    __atomic_fetch_add(&e->ksWithdrawing, 1u, __ATOMIC_SEQ_CST);\n"
                              "    if (gT0On) t0src_unmap_note(e->seq, va, size);");
    const size_t u1 = uv.find("const uint64_t rc = reinterpret_cast<Fn>(gOrigUnmapVA)(self, va, size);");
    expect_u("ENTRY: hook_unmapVA records right AFTER the marker is set (scope check -> marker -> note -> Apple's unmapVA)",
             !uv.empty() && us != std::string::npos && u0 != std::string::npos && u1 != std::string::npos && us < u0 && u0 < u1 &&
             count(uv, "t0src_unmap_note(") == 1u, 1u);
    // the note bodies: read-only, the class behind kernel-pointer checks, +0x40 only for the exact VidMemory class
    const std::string mn = body(H, "static __attribute__((noinline)) void t0src_map_note("), un = body(H, "static __attribute__((noinline)) void t0src_unmap_note(");
    expect_u("NOTES READ-ONLY: no write primitive in either note", !mn.empty() && !un.empty() && writes_in(mn) + writes_in(un) == 0u, 1u);
    expect_u("NOTES: the class read is behind kernel-pointer checks on the object and its vtable",
             count(mn, "if (mp >= kKernelHalfBase && reinterpret_cast<uintptr_t>(*reinterpret_cast<void *const *>(mem)) >= kKernelHalfBase) {"), 1u);
    expect_u("NOTES: +0x40 is read only for the exact VidMemory class", count(mn, "if (n48_t0_is_vidmem(name)) mlen ="), 1u);
    expect_u("NOTES: the one Apple call is getMetaClass()->getClassName()", count(mn, "->getMetaClass()") * 0x10u + count(mn, "->getClassName()") +
             count(mn, "->release(") + count(mn, "->retain("), 0x11u);
    // C1: the probe runs only under 79 and under its cap, BEFORE any MM read, after the B1 lines, and asks the hazard set read-only
    const std::string pb = body(H, "static __attribute__((noinline)) void gfxsrc_t0src_probe(const GfxcVm &vm) {");
    const size_t p0 = pb.find("if (!gT0On) return;"), p1 = pb.find("const uint32_t ln = n48_t0_take_line(&gT0Lines);"),
                 p2 = pb.find("if (!ln) return;"), p3 = pb.find("navi48_vram_read_mm(");
    expect_u("C1 ORDER: the probe: switch -> cap -> MM reads", !pb.empty() && p0 != std::string::npos && p1 != std::string::npos &&
             p2 != std::string::npos && p3 != std::string::npos && p0 < p1 && p1 < p2 && p2 < p3, 1u);
    expect_u("C1: exactly 16 reads of 64 dwords at P + 64i*4, timed", count(pb, "for (; i < 16u; i++) {") == 1u &&
             count(pb, "navi48_vram_read_mm(off0 + (uint64_t)i * 64u * 4u, gT0Buf, 64u)") == 1u && count(pb, "gT0S.us += ns / 1000ull;") == 1u, 1u);
    expect_u("C1: the MM reads only for a usable (VRAM) page", count(pb, "if (use0) {\n        uint64_t t0 = 0ull"), 1u);
    expect_u("STACK: the 256-byte read buffer is a file-scope static", count(H, "static uint32_t gT0Buf[64];"), 1u);
    expect_u("PROBE READ-ONLY: no write primitive in the probe", writes_in(pb), 0u);
    expect_u("PROBE: the hazard set is asked through n48_hz_probe, never n48_hz_hit (the probe, t0src_hz and the header)",
             count(pb, "n48_hz_hit") + count(body(H, "static void t0src_hz(uint64_t page,"), "n48_hz_hit") +
             count(body(X, "static inline uint32_t n48_t0_hz_probe("), "n48_hz_hit"), 0u);
    expect_u("PROBE: the header's hazard ask is n48_hz_probe + n48_hz_count on a const set",
             count(X, "static inline uint32_t n48_t0_hz_probe(const n48_hazard *hz,") * 0x100u +
             count(body(X, "static inline uint32_t n48_t0_hz_probe("), "n48_hz_probe(hz, page)") * 0x10u +
             count(body(X, "static inline uint32_t n48_t0_hz_probe("), "n48_hz_count(hz, page, &ex)"), 0x111u);
    expect_u("PROBE: the t0src line carries the filter, the exact row's hits AND its exact flag",
             count(pb, "hzF, (unsigned long long)hzHits, hzEx, (unsigned long long)hzNamer,"), 1u);
    expect_u("PROBE: t0src_hz passes gR5Ring.hz", count(H, "*filt = n48_t0_hz_probe(&gR5Ring.hz, page, hits, exact, namer, va);"), 1u);
    expect_u("PROBE: the ledger and resprov asks are the header's read-only probes on gXdLed / gXdRp",
             count(pb, "n48_t0_led_probe(&gXdLed, pg0,") * 0x10u + count(pb, "n48_t0_rp_probe(&gXdRp, off0,"), 0x11u);
    expect_u("PROBE: the page class comes from n48_t0_pg_class and gates every key", count(pb, "const uint32_t use0 = n48_t0_pg_usable(c0);"), 1u);
    const std::string fe = body(H, "static __attribute__((noinline)) void gfxsrc_c88_frame_end(");
    const size_t f1 = fe.find("HWLOG(N48_C88_FMT"), f2 = fe.find("gfxsrc_t0src_probe(vm);");
    expect_u("REACH: the frame end calls the probe once, after the B1 lines (so only on B1 frames)", f1 != std::string::npos &&
             f2 != std::string::npos && f1 < f2 && count(fe, "gfxsrc_t0src_probe(") == 1u && count(H, "gfxsrc_t0src_probe(vm);") == 1u, 1u);
    expect_u("REACH: the report line on the drawelide66 report", count(body(H, "static void drawelide_report_line(const char *how)"), "t0src_report_line(\"\");"), 1u);
    // ITEM 6
    expect_u("ITEM 6: the watch index starts at N48_C88_WATCH (a full table prints -1), not 0",
             count(fe, "uint32_t idx = N48_C88_WATCH;") * 0x10u + count(fe, "uint32_t idx = 0u;"), 0x10u);
    // ITEM 7
    const std::string nr = body(P, "static void *hook_new_resource(void *accel) {"), rn = body(P, "static __attribute__((noinline)) void res2_note(void *res, void **vt) {");
    expect_u("ITEM 7: the second-class path counts and names, then returns res unchanged",
             count(nr, "    if (gResOrigVt) {\n        // A second, different resource class. Leave it alone rather than guess.\n"
                       "        res2_note(res, vt);   // build 0.0.524 item 7: counted, and its class named once per boot (read-only)\n"
                       "        return res;\n    }"), 1u);
    expect_u("ITEM 7: one atomic counter, one once-per-boot compare-exchange, one line", !rn.empty() &&
             count(rn, "__atomic_add_fetch(&gRes2Count, 1ull, __ATOMIC_RELAXED)") == 1u &&
             count(rn, "if (!__atomic_compare_exchange_n(&gRes2Logged, &zero, 1u,") == 1u && count(rn, "PEERLOG(") == 1u, 1u);
    expect_u("ITEM 7 READ-ONLY: res2_note holds no write primitive and patches no vtable", writes_in(rn) + count(rn, "*slot") + count(rn, "gResCopyVt ="), 0u);
    // THE HEADER: no write primitive (it writes only its caller's ring / table / out-parameters)
    const std::string xs = X.substr(X.find("#ifndef N48_GFX_T0SRC_H"));
    expect_u("HEADER: no write primitive and no kext call", writes_in(xs) + count(xs, "navi48_") + count(xs, "gfxc_page"), 0u);
    expect_u("HEADER: the map note masks rc to its low byte", count(X, "s->rc8 = (uint32_t)(rc & 0xffull);"), 1u);
    expect_u("HEADER: the class name prints at most 40 characters (%.40s)", count(X, "mem %p %.40s len"), 1u);
}

int main(int argc, char **argv)
{
    std::printf("gfx_t0src (0.0.524, switch 79): S's texture-0 source, read-only\n");
    ring_checks(); cls_checks(); probe_checks(); line_checks();
    if (argc >= 4) glue_checks(argv[1], argv[2], argv[3]);
    else expect_u("GLUE: pass AppleHardwareHook.cpp Navi48AccelPeer.cpp gfx_t0src.h", 0u, 1u);
    std::printf("gfx_t0src: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
