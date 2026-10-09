// native_s1c_test.cpp - build 0.0.601 (native-stack step S1c, kernel half): the pure half of the native user client, plus source pins.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_s1c_test.cpp -o /tmp/native_s1c && /tmp/native_s1c .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_s1c_plant.sh plants breaks and
//    shows every check that catches them)
// Covers (numbers are the checks' groups):
//   U1  the ABI header: every struct's size and offsets, the selector numbers 0..8, the return codes vs the contract's table;
//   U2  BoCreate placement: sizes, alignment, domains, flags (ignored / refused / unknown), hi-pool eligibility;
//   U3  GemVa validation: canonical and stripped VAs, the 0x10000 floor, the sign-extension seam, alignment, timeline fields, PRT,
//       CLEAR / REPLACE, handle 0; flag -> PTE bits for the three flag words the S1d recording carries;
//   U4  handle / ctx tables: bounds, lowest-free, double free, never 0;
//   U5  the VA map: overlap, exact match, full table, EXEC coverage of an IB (adjacent mappings, a non-EXEC gap);
//   U6  the page-table mapper WALKED like the hardware over a fake memory: low and high half, 2 MiB and 1 GiB seams, atomic failure at
//       the page-table cap, unmap, and a map after an unmap;
//   U7  Submit chunk parsing: exact size, every field, unsupported vs bad flags;
//   U8  ring arithmetic: free space, the +16 slack, wrap, and a 2000-submit lap simulation that never overruns the reader;
//   U9  the HUNG latch: no false positive when idle, the clock restarts on progress and on the kick, expiry at exactly 2 s, latches once,
//       Timeout on the detecting call then Aborted;
//   U10 WaitSeq resolution and the 2 s cap; the retired clamp;
//   U11 the Submit packet builder: the contract's order (CONTEXT_CONTROL, IBs, USER FENCE, then the kernel SEQNO), sizes 3+4n+8+8,
//       control words, canonical VAs untouched, refusals; the fence-window picker and addresses;
//   U12 source pins on the kext: the gate order, the ring path (space check before the write, emitted before the doorbell, one helper),
//       exclusivity, the legacy refusals, the client's exact-shape checks, no native_s1b_refuse, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "native_s1c_pure.h"

using namespace n48native;
using namespace n48native::s1c;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static size_t count_of(const std::string &s, const std::string &needle) {
    size_t n = 0, p = 0;
    while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
    return n;
}
// 0.0.627: the body of the function whose signature starts with `head` (up to the first line that is exactly "}"), and an in-order search.
static std::string fn_body(const std::string &s, const char *head) {
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}
static bool in_order(const std::string &s, std::initializer_list<const char *> parts) {
    size_t at = 0;
    for (const char *p : parts) { const size_t f = s.find(p, at); if (f == std::string::npos) return false; at = f + 1; }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
static void u1_abi() {
    expect_u("N48N_UC_TYPE is 'N48N'", N48N_UC_TYPE, 0x4E34384Eu);
    expect_u("ABI version", N48N_ABI_VERSION, 1);
    expect_u("selectors: Hello", N48N_SEL_HELLO, 0);       expect_u("selectors: QueryInfo", N48N_SEL_QUERYINFO, 1);
    expect_u("selectors: ReadRegs", N48N_SEL_READREGS, 2); expect_u("selectors: BoCreate", N48N_SEL_BOCREATE, 3);
    expect_u("selectors: BoFree", N48N_SEL_BOFREE, 4);     expect_u("selectors: GemVa", N48N_SEL_GEMVA, 5);
    expect_u("selectors: Ctx", N48N_SEL_CTX, 6);           expect_u("selectors: Submit", N48N_SEL_SUBMIT, 7);
    expect_u("selectors: WaitSeq", N48N_SEL_WAITSEQ, 8);   expect_u("selector count", N48N_SEL_COUNT, 9);
    expect_u("struct sizes: gem_create_in", sizeof(n48n_gem_create_in), 32); expect_u("gem_va", sizeof(n48n_gem_va), 64);
    expect_u("ctx", sizeof(n48n_ctx), 16); expect_u("cs_ib", sizeof(n48n_cs_ib), 32); expect_u("cs_in", sizeof(n48n_cs_in), 32);
    expect_u("info", sizeof(n48n_info), 192);
    // The return codes of the contract's table (section 1.1), as raw numbers.
    expect_u("BadArgument", kBadArg, 0xe00002c2u); expect_u("NotFound", kNotFound, 0xe00002f0u); expect_u("Unsupported", kUnsupported, 0xe00002c7u);
    expect_u("NoMemory", kNoMemory, 0xe00002bdu); expect_u("NoResources", kNoResources, 0xe00002beu); expect_u("NotReady", kNotReady, 0xe00002d8u);
    expect_u("ExclusiveAccess", kExclusive, 0xe00002c5u); expect_u("NotPrivileged", kNotPrivileged, 0xe00002c1u);
    expect_u("NotPermitted", kNotPermitted, 0xe00002e2u); expect_u("Timeout", kTimeout, 0xe00002d6u); expect_u("Aborted", kAborted, 0xe00002ebu);
    expect_u("limits: max IBs", N48N_MAX_IBS, 64); expect_u("max BOs", N48N_MAX_BOS, 4096); expect_u("max ctx", N48N_MAX_CTX, 64);
    expect_u("fence slots", N48N_FENCE_SLOTS, 16); expect_u("wait cap", N48N_WAIT_CAP_NS, 2000000000ull);
    expect_u("seq last", N48N_SEQ_LAST, ~0ull);
    expect_u("info flags", N48N_INFO_HUNG | N48N_INFO_S1B_POSITIVE | N48N_INFO_NATIVE_BOOT, 7);
    expect_u("placement bits", N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED | N48N_PLACED_HI_POOL, 7);
}

static void u2_place() {
    const uint64_t V = N48N_GEM_DOMAIN_VRAM, G = N48N_GEM_DOMAIN_GTT;
    { Place p = place_decide(0, 0, V, 0); expect_u("size 0 -> BadArg", p.rc, kBadArg); }
    { Place p = place_decide(1, 3, V, 0); expect_u("alignment 3 (not a power of two) -> BadArg", p.rc, kBadArg); }
    { Place p = place_decide(1, 4ull << 20, V, 0); expect_u("alignment 4 MiB > 2 MiB -> BadArg", p.rc, kBadArg); }
    { Place p = place_decide(1, 2ull << 20, V, 0); expect_u("alignment 2 MiB ok", p.rc, kOk); expect_u("align kept", p.align, 2ull << 20); }
    { Place p = place_decide(1312, 0, V | G, 0); expect_u("1312 B rounds to 4 KiB", p.size, 4096); expect_u("align 0 -> 4 KiB", p.align, 4096); expect_u("VRAM|GTT -> visible pool", p.kind, kPlaceVis); }
    { Place p = place_decide(1, 512, G, 0); expect_u("alignment below 4 KiB is raised", p.align, 4096); expect_u("exactly GTT -> GTT", p.kind, kPlaceGtt); }
    { Place p = place_decide(4096, 0, V, 0); expect_u("VRAM alone -> visible pool", p.kind, kPlaceVis); }
    { Place p = place_decide(11010048, 0, V | G, N48N_GEM_NO_CPU_ACCESS | N48N_GEM_DISCARDABLE | N48N_GEM_EXPLICIT_SYNC);
      expect_u("BO9 of the recording (NO_CPU_ACCESS) is accepted", p.rc, kOk); expect(p.hiOk, "and may fall back to the hi pool"); expect_u("size", p.size, 11010048); }
    { Place p = place_decide(4096, 0, V, N48N_GEM_NO_CPU_ACCESS | N48N_GEM_CPU_ACCESS_REQUIRED); expect(!p.hiOk, "CPU_ACCESS_REQUIRED forbids the hi pool"); }
    { Place p = place_decide(4096, 0, V, 0); expect(!p.hiOk, "no NO_CPU_ACCESS: no hi pool"); }
    for (uint64_t d : { 0ull, 1ull, 8ull, 0x10ull, 0x20ull, 0x40ull, V | 1, G | 8, 0x80ull, 0x100ull }) {
        Place p = place_decide(4096, 0, d, 0);
        expect_u("CPU / GDS / GWS / OA / DOORBELL / none / unknown domain -> Unsupported", p.rc, kUnsupported);
    }
    for (uint64_t f : { N48N_GEM_ENCRYPTED, N48N_GEM_CP_MQD_GFX9, N48N_GEM_PREEMPTIBLE, 1ull << 4, 1ull << 9, 1ull << 13, 1ull << 15, 1ull << 17, 1ull << 40 }) {
        Place p = place_decide(4096, 0, V, f);
        expect_u("refused / unlisted GEM flag -> Unsupported", p.rc, kUnsupported);
    }
    for (uint64_t f : { N48N_GEM_CPU_GTT_USWC, N48N_GEM_VRAM_CLEARED, N48N_GEM_VRAM_CONTIGUOUS, N48N_GEM_VM_ALWAYS_VALID, N48N_GEM_EXPLICIT_SYNC,
                        N48N_GEM_DISCARDABLE, N48N_GEM_GFX12_DCC, N48N_GEM_VIRTIO_SHARED }) {
        Place p = place_decide(4096, 0, V, f);
        expect_u("ignored GEM flag is accepted", p.rc, kOk); expect(!p.uc, "and does not force UC");
    }
    { Place p = place_decide(4096, 0, G, N48N_GEM_UNCACHED); expect(p.rc == kOk && p.uc, "UNCACHED is accepted and forces UC"); }
    { Place p = place_decide(kMaxBoBytes + 1, 0, V, 0); expect_u("absurd size -> BadArg (no rounding overflow)", p.rc, kBadArg); }
    { Place p = place_decide(~0ull, 0, V, 0); expect_u("size ~0 -> BadArg", p.rc, kBadArg); }
}

static void u3_gemva() {
    auto chk = [](uint32_t op, uint32_t handle, uint32_t flags, uint64_t va, uint64_t off, uint64_t size) {
        return gemva_check(op, handle, 0, flags, va, off, size, 0, 0, 0, 0);
    };
    const uint64_t HI = 0xffff800100000000ull;
    { VaReq r = chk(N48N_VA_OP_MAP, 1, 0xe, HI, 0, 0x10000); expect_u("a high-half map is fine", r.rc, kOk); expect_u("stripped", r.stripped, 0x800100000000ull); }
    { VaReq r = chk(N48N_VA_OP_MAP, 1, 0xe, 0x100000, 0, 0x1000); expect_u("a low-half map is fine", r.rc, kOk); expect_u("low stripped = itself", r.stripped, 0x100000); }
    expect_u("VA below 0x10000 -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, 0xF000, 0, 0x1000).rc, kBadArg);
    expect_u("VA 0x10000 exactly is the floor (ok)", chk(N48N_VA_OP_MAP, 1, 0xe, 0x10000, 0, 0x1000).rc, kOk);
    expect_u("VA 0 -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, 0, 0, 0x1000).rc, kBadArg);
    expect_u("non-canonical VA 0x0000800000000000 -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, 0x0000800000000000ull, 0, 0x1000).rc, kBadArg);
    expect_u("non-canonical VA 0x7fff... with bit 63 clear and 47 set", chk(N48N_VA_OP_MAP, 1, 0xe, 0x0001000000001000ull, 0, 0x1000).rc, kBadArg);
    expect_u("unaligned VA -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, HI + 0x10, 0, 0x1000).rc, kBadArg);
    expect_u("unaligned offset -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, HI, 0x10, 0x1000).rc, kBadArg);
    expect_u("unaligned size -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, HI, 0, 0x1800).rc, kBadArg);
    expect_u("size 0 -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, HI, 0, 0).rc, kBadArg);
    expect_u("a range straddling the sign-extension seam -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, 0x00007FFFFFFFF000ull, 0, 0x2000).rc, kBadArg);
    expect_u("a range running past 2^48 -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe, 0xFFFFFFFFFFFFF000ull, 0, 0x2000).rc, kBadArg);
    expect_u("the very last page of the space is fine", chk(N48N_VA_OP_MAP, 1, 0xe, 0xFFFFFFFFFFFFF000ull, 0, 0x1000).rc, kOk);
    expect_u("PRT -> Unsupported", chk(N48N_VA_OP_MAP, 1, 0xe | N48N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kUnsupported);
    expect_u("PRT on UNMAP too -> Unsupported", chk(N48N_VA_OP_UNMAP, 1, N48N_VM_PAGE_PRT, HI, 0, 0x1000).rc, kUnsupported);
    expect_u("handle 0 -> Unsupported", chk(N48N_VA_OP_MAP, 0, 0xe, HI, 0, 0x1000).rc, kUnsupported);
    expect_u("CLEAR -> Unsupported", chk(N48N_VA_OP_CLEAR, 1, 0xe, HI, 0, 0x1000).rc, kUnsupported);
    expect_u("REPLACE -> Unsupported", chk(N48N_VA_OP_REPLACE, 1, 0xe, HI, 0, 0x1000).rc, kUnsupported);
    expect_u("op 0 -> BadArg", chk(0, 1, 0xe, HI, 0, 0x1000).rc, kBadArg);
    expect_u("op 5 -> BadArg", chk(5, 1, 0xe, HI, 0, 0x1000).rc, kBadArg);
    expect_u("MTYPE 6 (unknown) on MAP -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe | (6u << 5), HI, 0, 0x1000).rc, kBadArg);
    expect_u("an unknown flag bit on MAP -> BadArg", chk(N48N_VA_OP_MAP, 1, 0xe | (1u << 20), HI, 0, 0x1000).rc, kBadArg);
    expect_u("UNMAP ignores the flags (the recording unmaps with 0xe)", chk(N48N_VA_OP_UNMAP, 1, 0xe, HI, 0, 0x1000).rc, kOk);
    expect_u("pad != 0 -> BadArg", gemva_check(1, 1, 1, 0xe, HI, 0, 0x1000, 0, 0, 0, 0).rc, kBadArg);
    expect_u("timeline point != 0 -> BadArg", gemva_check(1, 1, 0, 0xe, HI, 0, 0x1000, 5, 0, 0, 0).rc, kBadArg);
    expect_u("syncobj out != 0 -> BadArg", gemva_check(1, 1, 0, 0xe, HI, 0, 0x1000, 0, 7, 0, 0).rc, kBadArg);
    expect_u("num syncobj != 0 -> BadArg", gemva_check(1, 1, 0, 0xe, HI, 0, 0x1000, 0, 0, 1, 0).rc, kBadArg);
    expect_u("input fence != 0 -> BadArg", gemva_check(1, 1, 0, 0xe, HI, 0, 0x1000, 0, 0, 0, 9).rc, kBadArg);
    expect(range_in_bo(0, 4096, 4096) && range_in_bo(4096, 4096, 8192) && !range_in_bo(4096, 4096, 4096) && !range_in_bo(~0ull, 4096, 8192) &&
           !range_in_bo(0, 8192, 4096) && !range_in_bo(4096, ~0ull, 8192), "range_in_bo, overflow-safe");
    // flag -> PTE bits: the three flag words of the recording, as complete words through pte_encode.
    const uint64_t pa = 0x0000008002728000ull;
    expect_u("0xe leaf = pa | IS_PTE | EXEC | READ | WRITE | VALID (NC)", pte_encode(pa, leaf_flags(0xe, false, false)), pa | kPteIsPte | kPteExec | kPteRead | kPteWrite | kPteValid);
    expect_u("0xa leaf = R|X", pte_encode(pa, leaf_flags(0xa, false, false)), pa | kPteIsPte | kPteExec | kPteRead | kPteValid);
    expect_u("0x8a leaf = R|X|UC", pte_encode(pa, leaf_flags(0x8a, false, false)), pa | kPteIsPte | kPteExec | kPteRead | kPteValid | kPteMtypeUC);
    expect_u("the numeric word of a 0xe VRAM leaf (S1b's 0x8080..71 without the UC bit)", pte_encode(0x8002728000ull, leaf_flags(0xe, false, false)), 0x8000008002728071ull);
    expect_u("the numeric word of a 0x8a UC leaf", pte_encode(0x8002728000ull, leaf_flags(0x8a, false, false)), 0x8080008002728031ull);
    expect_u("MTYPE_DEFAULT/NC/WC/CC/RW all map to NC (no MTYPE bits)", leaf_flags(0x2 | N48N_VM_MTYPE_NC, false, false) | leaf_flags(0x2 | N48N_VM_MTYPE_WC, false, false) |
             leaf_flags(0x2 | N48N_VM_MTYPE_CC, false, false) | leaf_flags(0x2 | N48N_VM_MTYPE_RW, false, false) | leaf_flags(0x2, false, false), kPteValid | kPteRead);
    expect_u("NOALLOC / DELAY_UPDATE are ignored", leaf_flags(0xe | N48N_VM_PAGE_NOALLOC | N48N_VM_DELAY_UPDATE, false, false), leaf_flags(0xe, false, false));
    expect_u("a GTT page is SYSTEM|SNOOPED", leaf_flags(0xe, false, true) & (kPteSystem | kPteSnooped), kPteSystem | kPteSnooped);
    expect_u("a VRAM page is neither", leaf_flags(0xe, false, false) & (kPteSystem | kPteSnooped), 0);
    expect_u("no vm access flags -> valid but no R/W/X", leaf_flags(0, false, false), kPteValid);
    expect_u("an unencodable pa gives 0 (an invalid entry)", pte_encode(1ull << 52, leaf_flags(0xe, false, false)), 0);
}

static void u4_tables() {
    uint8_t used[N48N_MAX_BOS] = { 0 };
    expect_u("empty table: first handle is 1 (never 0)", lowest_free(used, 1, N48N_MAX_BOS - 1), 1);
    used[1] = used[2] = used[3] = 1;
    expect_u("lowest free after 1,2,3", lowest_free(used, 1, N48N_MAX_BOS - 1), 4);
    used[2] = 0;
    expect_u("a freed handle is the next one handed out", lowest_free(used, 1, N48N_MAX_BOS - 1), 2);
    expect(id_live(used, N48N_MAX_BOS - 1, 1) && !id_live(used, N48N_MAX_BOS - 1, 2), "live / freed");
    expect(!id_live(used, N48N_MAX_BOS - 1, 0), "handle 0 is never live");
    expect(!id_live(used, N48N_MAX_BOS - 1, N48N_MAX_BOS) && !id_live(used, N48N_MAX_BOS - 1, 0xFFFFFFFFu), "out-of-range handles are not live");
    for (uint32_t i = 1; i < N48N_MAX_BOS; i++) used[i] = 1;
    expect_u("full table (4095 handles) -> 0", lowest_free(used, 1, N48N_MAX_BOS - 1), 0);
    used[N48N_MAX_BOS - 1] = 0;
    expect_u("the last handle, 4095, is usable", lowest_free(used, 1, N48N_MAX_BOS - 1), N48N_MAX_BOS - 1);
    // Double free: the second free finds the handle not live.
    uint8_t u2[8] = { 0 }; u2[3] = 1;
    expect(id_live(u2, 7, 3), "live before the free"); u2[3] = 0; expect(!id_live(u2, 7, 3), "a double free sees NotFound");
    // ctx ids 1..64
    uint8_t cu[N48N_MAX_CTX + 1] = { 0 };
    for (uint32_t i = 1; i <= N48N_MAX_CTX; i++) { expect_u("ctx id = lowest free", lowest_free(cu, 1, N48N_MAX_CTX), i); cu[i] = 1; }
    expect_u("65th ctx -> 0 (NoResources)", lowest_free(cu, 1, N48N_MAX_CTX), 0);
    expect(!id_live(cu, N48N_MAX_CTX, 0) && !id_live(cu, N48N_MAX_CTX, 65), "ctx 0 and 65 are never live");
    // caps
    expect(!would_exceed(0, kGttCap, kGttCap) && would_exceed(1, kGttCap, kGttCap) && would_exceed(0, kGttCap + 1, kGttCap) &&
           would_exceed(~0ull, 2, kGttCap) && !would_exceed(kGttCap - 4096, 4096, kGttCap), "would_exceed is exact and overflow-safe");
}

static void u5_vamap() {
    VaEnt t[8] = {};
    auto ins = [&](uint64_t s, uint64_t sz, uint32_t h, uint32_t fl) { return va_insert(t, 8, VaEnt{ s, sz, 0, h, fl }); };
    expect(ins(0x800100000000ull, 0x10000, 1, 0xe) == 0, "insert");
    expect(va_overlap(t, 8, 0x800100000000ull, 0x1000) == 0, "overlap: same start");
    expect(va_overlap(t, 8, 0x8000FFFFF000ull, 0x2000) == 0, "overlap: straddling the start");
    expect(va_overlap(t, 8, 0x80010000F000ull, 0x2000) == 0, "overlap: straddling the end");
    expect(va_overlap(t, 8, 0x800100010000ull, 0x1000) < 0, "no overlap: directly after");
    expect(va_overlap(t, 8, 0x8000FFFFF000ull, 0x1000) < 0, "no overlap: directly before");
    expect(va_overlap(t, 8, 0x800000000000ull, 0x1000000000ull) == 0, "overlap: a huge range covering it");
    expect(va_find_exact(t, 8, 1, 0x800100000000ull, 0x10000) == 0, "exact match");
    expect(va_find_exact(t, 8, 1, 0x800100000000ull, 0x8000) < 0, "exact: a different size does not match");
    expect(va_find_exact(t, 8, 2, 0x800100000000ull, 0x10000) < 0, "exact: a different handle does not match");
    expect(va_find_exact(t, 8, 1, 0x800100001000ull, 0x10000) < 0, "exact: a different start does not match");
    for (uint32_t i = 1; i < 8; i++) expect(ins(0x100000ull + 0x10000ull * i, 0x1000, 10 + i, 0xe) == (int)i, "fill");
    expect(ins(0x900000, 0x1000, 99, 0xe) == -1, "a full table refuses");
    expect_u("count of a handle", va_count_handle(t, 8, 12), 1);
    expect_u("remove by handle", va_remove_handle(t, 8, 12), 1);
    expect(va_overlap(t, 8, 0x100000ull + 0x20000ull, 0x1000) < 0, "the removed range is free");
    // EXEC coverage
    VaEnt m[4] = {};
    va_insert(m, 4, VaEnt{ 0x800100001000ull, 0x14000, 0, 7, 0x8a });      // R|X|UC, 80 KiB
    va_insert(m, 4, VaEnt{ 0x800100015000ull, 0x1000, 0, 3, 0x8a });       // adjacent, R|X
    va_insert(m, 4, VaEnt{ 0x800100016000ull, 0x1000, 0, 10, 0xa });       // adjacent, R|X
    va_insert(m, 4, VaEnt{ 0x800100040000ull, 0x40000, 0, 4, 0xe });       // R|W|X but not adjacent
    expect(va_exec_covered(m, 4, 0x800100001000ull, 1984), "IB1 (1984 B) inside its EXEC mapping");
    expect(va_exec_covered(m, 4, 0x800100015000ull, 328 * 4), "the nested preamble page");
    expect(va_exec_covered(m, 4, 0x800100014000ull, 0x3000), "a range spanning three adjacent EXEC mappings");
    expect(!va_exec_covered(m, 4, 0x800100017000ull, 4), "unmapped -> not covered");
    expect(!va_exec_covered(m, 4, 0x800100016F00ull, 0x200), "runs off the end of the last adjacent mapping into a hole");
    expect(!va_exec_covered(m, 4, 0x800100001000ull, 0), "zero bytes is never 'covered'");
    VaEnt n2[2] = {};
    va_insert(n2, 2, VaEnt{ 0x800100001000ull, 0x1000, 0, 1, 0xa });       // R|X
    va_insert(n2, 2, VaEnt{ 0x800100002000ull, 0x1000, 0, 2, 0x6 });       // R|W, no EXEC
    expect(va_exec_covered(n2, 2, 0x800100001000ull, 0x1000), "an EXEC page");
    expect(!va_exec_covered(n2, 2, 0x800100001000ull, 0x2000), "a non-EXEC neighbour breaks the coverage");
    expect(!va_exec_covered(n2, 2, 0x800100002000ull, 4), "a non-EXEC page alone");
    expect(!va_exec_covered(n2, 2, ~0ull - 3, 0x100), "range overflow is refused");
    { VaEnt wx[1] = {}; va_insert(wx, 1, VaEnt{ 0x800100001000ull, 0x1000, 0, 1, N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE });
      expect(!va_exec_covered(wx, 1, 0x800100001000ull, 4), "EXEC without READ does not cover an IB (an IB fetch needs both)");
      VaEnt rx[1] = {}; va_insert(rx, 1, VaEnt{ 0x800100001000ull, 0x1000, 0, 1, N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE });
      expect(!va_exec_covered(rx, 1, 0x800100001000ull, 4), "READ without EXEC does not cover an IB");
      VaEnt ok[1] = {}; va_insert(ok, 1, VaEnt{ 0x800100001000ull, 0x1000, 0, 1, N48N_VM_PAGE_READABLE | N48N_VM_PAGE_EXECUTABLE });
      expect(va_exec_covered(ok, 1, 0x800100001000ull, 4), "READ|EXEC covers an IB"); }
}

// A fake memory of 4 KiB table pages keyed by pa, with a page budget.
struct FakeMem {
    std::map<uint64_t, std::array<uint64_t, 512>> pages;
    uint64_t next { 0x40000000ull };
    uint32_t budget;
    uint32_t allocs { 0 };
    explicit FakeMem(uint32_t b) : budget(b) {}
    uint64_t alloc_page() {
        if (allocs >= budget) return 0;
        allocs++;
        const uint64_t pa = next; next += 0x1000;
        pages[pa].fill(0);
        return pa;
    }
    uint64_t rd(uint64_t pa, uint32_t idx) { auto it = pages.find(pa); if (it == pages.end()) { std::printf("FAIL: read of an unknown table pa %#llx\n", (unsigned long long)pa); gFail++; return 0; } return it->second[idx]; }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { auto it = pages.find(pa); if (it == pages.end()) { std::printf("FAIL: write to an unknown table pa %#llx\n", (unsigned long long)pa); gFail++; return; } it->second[idx] = v; }
};
static void u6_pt() {
    { // high half, 10.5 MiB, crossing PTB seams (BO9 of the recording at 0xffff800000200000)
        FakeMem m(64);
        const uint64_t root = m.alloc_page();
        const uint64_t va = 0xffff800000200000ull, pages = 11010048 / 4096, pa0 = 0x8010000000ull;
        expect_u("map 10.5 MiB", pt_map(root, va, pages, pa0, leaf_flags(0xe, false, false), m), kOk);
        bool ok = true;
        for (uint64_t p = 0; p < pages && ok; p++) {
            uint64_t leaf = 0;
            const uint64_t got = pt_walk(root, va + p * 4096 + 0x123, m, &leaf);
            ok = got == pa0 + p * 4096 + 0x123 && (leaf & kPteIsPte) && (leaf & kPteValid);
        }
        expect(ok, "every page of the 10.5 MiB mapping walks to its physical page (offset kept)");
        expect_u("just below the range is unmapped", pt_walk(root, va - 4096, m, nullptr), 0);
        expect_u("just above the range is unmapped", pt_walk(root, va + pages * 4096, m, nullptr), 0);
        expect_u("root index 256 (high half) was used, root[0] untouched", (m.rd(root, 256) & kPteValid) && !(m.rd(root, 0) & kPteValid), 1);
        expect_u("root[256] is a pointer: VALID and no IS_PTE", m.rd(root, 256) & (kPteIsPte | kPteRead | kPteWrite | kPteExec), 0);
        pt_unmap(root, va, pages, m);
        ok = true;
        for (uint64_t p = 0; p < pages && ok; p++) { uint64_t leaf = ~0ull; ok = pt_walk(root, va + p * 4096, m, &leaf) == 0 && leaf == 0ull; }
        expect(ok, "unmap clears every leaf to exactly 0 (not merely 'walks to nothing')");
        expect_u("map again after the unmap", pt_map(root, va, pages, pa0 + 0x100000, leaf_flags(0xa, false, false), m), kOk);
        expect_u("the new mapping walks to the new pa", pt_walk(root, va, m, nullptr), pa0 + 0x100000);
    }
    { // low half + a mapping across a 1 GiB seam (PDB1 index changes) and a 2 MiB seam
        FakeMem m(64);
        const uint64_t root = m.alloc_page();
        const uint64_t va = 0x000000003FFFF000ull;      // last page before 1 GiB
        expect_u("map across the 1 GiB seam", pt_map(root, va, 3, 0x9000000000ull, leaf_flags(0xe, false, true), m), kOk);
        expect_u("page 0", pt_walk(root, va, m, nullptr), 0x9000000000ull);
        expect_u("page 1 (next PDB1 entry)", pt_walk(root, va + 0x1000, m, nullptr), 0x9000001000ull);
        expect_u("page 2", pt_walk(root, va + 0x2000, m, nullptr), 0x9000002000ull);
        uint64_t leaf = 0; pt_walk(root, va, m, &leaf);
        expect_u("GTT leaf carries SYSTEM|SNOOPED", leaf & (kPteSystem | kPteSnooped), kPteSystem | kPteSnooped);
        expect_u("low half uses root[0] only", (m.rd(root, 0) & kPteValid) != 0 && (m.rd(root, 256) & kPteValid) == 0, 1);
        const uint64_t v2 = 0x00000000001FF000ull;      // 2 MiB seam
        expect_u("map across the 2 MiB seam", pt_map(root, v2, 2, 0x9100000000ull, leaf_flags(0xe, false, false), m), kOk);
        expect_u("before the seam", pt_walk(root, v2, m, nullptr), 0x9100000000ull);
        expect_u("after the seam", pt_walk(root, v2 + 0x1000, m, nullptr), 0x9100001000ull);
    }
    { // exhaustion is atomic: a failed map leaves no leaf behind
        FakeMem m(5);   // root + PDB1 + PDB0 + PTB = 4 pages for the first map; the second needs 3 more (a new root entry)
        const uint64_t root = m.alloc_page();
        expect_u("first map fits", pt_map(root, 0x100000ull, 4, 0x9000000000ull, leaf_flags(0xe, false, false), m), kOk);
        const uint32_t before = m.allocs;
        expect_u("the second map (a new half) exceeds the page-table budget -> NoMemory", pt_map(root, 0xffff800000100000ull, 4, 0x9200000000ull, leaf_flags(0xe, false, false), m), kNoMemory);
        expect(before <= m.allocs, "pages may have been taken");
        bool none = true;
        for (uint64_t p = 0; p < 4; p++) none = none && pt_walk(root, 0xffff800000100000ull + p * 4096, m, nullptr) == 0;
        expect(none, "and not one leaf of the failed map exists");
        expect_u("the first mapping is intact", pt_walk(root, 0x100000ull, m, nullptr), 0x9000000000ull);
        expect_u("a map of zero pages is refused", pt_map(root, 0x104000ull, 0, 0x9000000000ull, 0, m), kBadArg);
        expect_u("an unencodable physical address is refused", pt_map(root, 0x104000ull, 1, 1ull << 52, leaf_flags(0xe, false, false), m), kBadArg);
    }
    { // unmap of a range with no tables is a no-op
        FakeMem m(8);
        const uint64_t root = m.alloc_page();
        pt_unmap(root, 0x500000ull, 100, m);
        expect(m.allocs == 1, "unmap never allocates");
    }
}

static std::vector<uint8_t> mk_cs(uint32_t nIbs, uint32_t flags = 0, uint32_t fh = 0, uint32_t fo = 0) {
    std::vector<uint8_t> b(32 + 32 * nIbs, 0);
    auto p32 = [&](size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); };
    auto p64 = [&](size_t o, uint64_t v) { std::memcpy(&b[o], &v, 8); };
    p32(0, 1); p32(4, 1); p32(8, nIbs); p32(12, flags); p32(16, fh); p32(20, fo);
    for (uint32_t i = 0; i < nIbs; i++) { p64(32 + 32 * i + 8, 0xffff800100001000ull + 0x1000ull * i); p32(32 + 32 * i + 16, 1984); }
    return b;
}
static void set32(std::vector<uint8_t> &b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
static void set64(std::vector<uint8_t> &b, size_t o, uint64_t v) { std::memcpy(&b[o], &v, 8); }
static void u7_cs() {
    CsView v;
    { auto b = mk_cs(2, 1, 1, 0); expect_u("a good 2-IB CS with a fence", cs_parse(b.data(), (uint32_t)b.size(), &v), kOk);
      expect(v.nIbs == 2 && v.ctx == 1 && v.fenceHandle == 1 && v.fenceOffset == 0 && (v.flags & N48N_CS_HAS_FENCE), "fields read back");
      uint64_t va; uint32_t by; cs_ib(b.data(), 1, &va, &by); expect(va == 0xffff800100002000ull && by == 1984, "cs_ib reads the second record"); }
    { auto b = mk_cs(1); expect_u("no fence: fence_handle/offset are ignored", cs_parse(b.data(), (uint32_t)b.size(), &v), kOk); }
    { auto b = mk_cs(1); expect_u("size 0 -> BadArg", cs_parse(b.data(), 0, &v), kBadArg); expect_u("size 31 -> BadArg", cs_parse(b.data(), 31, &v), kBadArg);
      expect_u("size 32 with 1 IB claimed -> BadArg", cs_parse(b.data(), 32, &v), kBadArg); expect_u("size 63 -> BadArg", cs_parse(b.data(), 63, &v), kBadArg);
      expect_u("size 65 -> BadArg", cs_parse(b.data(), 65, &v), kBadArg); expect_u("null -> BadArg", cs_parse(nullptr, 64, &v), kBadArg); }
    { auto b = mk_cs(1); set32(b, 0, 2); expect_u("abi 2 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); set32(b, 0, 0); expect_u("abi 0 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1); set32(b, 8, 0); expect_u("num_ibs 0 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(65); expect_u("num_ibs 65 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(64); expect_u("num_ibs 64 is the maximum (ok)", cs_parse(b.data(), (uint32_t)b.size(), &v), kOk); }
    { auto b = mk_cs(2); set32(b, 8, 3); expect_u("num_ibs says 3 but the buffer holds 2 -> BadArg (never over-read)", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(3); set32(b, 8, 2); expect_u("num_ibs says 2 but the buffer holds 3 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1); set64(b, 24, 1); expect_u("reserved != 0 -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1, 2); expect_u("an unknown CS flag -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1, 1, 1, 4); expect_u("fence offset not 8-aligned -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    struct Field { size_t off; uint32_t val; uint32_t rc; const char *what; };
    for (const Field &f : { Field{ 0, 1, kBadArg, "IB _pad != 0" }, Field{ 20, 1, kBadArg, "ip_type != GFX" }, Field{ 24, 1, kBadArg, "ip_instance != 0" },
                            Field{ 28, 1, kBadArg, "ring != 0" }, Field{ 16, 0, kBadArg, "ib_bytes 0" }, Field{ 16, 1982, kBadArg, "ib_bytes not a multiple of 4" },
                            Field{ 16, 0xFFFFFu * 4u + 4u, kBadArg, "ib_bytes above 0xFFFFF dwords" }, Field{ 4, 1u << 0, kUnsupported, "IB flag CE" },
                            Field{ 4, 1u << 4, kUnsupported, "IB flag RESET_GDS_MAX_WAVE_ID" }, Field{ 4, 1u << 5, kUnsupported, "IB flag SECURE" },
                            Field{ 4, 1u << 6, kUnsupported, "IB flag EMIT_MEM_SYNC" }, Field{ 4, 1u << 7, kBadArg, "an unknown IB flag" } }) {
        auto b = mk_cs(2); set32(b, 32 + 32 + f.off, f.val);       // the SECOND record: proves every record is checked
        expect_u(f.what, cs_parse(b.data(), (uint32_t)b.size(), &v), f.rc);
        expect_u("... and names the record", v.badIb, 1);
    }
    for (uint32_t fl : { N48N_IB_FLAG_PREAMBLE, N48N_IB_FLAG_PREEMPT, N48N_IB_FLAG_TC_WB_NOT_INVALIDATE, 0xEu }) {
        auto b = mk_cs(1); set32(b, 32 + 4, fl); expect_u("PREAMBLE / PREEMPT / TC_WB_NOT_INVALIDATE are accepted", cs_parse(b.data(), (uint32_t)b.size(), &v), kOk);
    }
    { auto b = mk_cs(1); set64(b, 32 + 8, 0x0000800000000000ull); expect_u("a non-canonical IB VA -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1); set64(b, 32 + 8, 0xffff800100001002ull); expect_u("an IB VA not 4-aligned -> BadArg", cs_parse(b.data(), (uint32_t)b.size(), &v), kBadArg); }
    { auto b = mk_cs(1); set32(b, 32 + 16, 0xFFFFFu * 4u); expect_u("ib_bytes = 0xFFFFF dwords is the maximum (ok)", cs_parse(b.data(), (uint32_t)b.size(), &v), kOk); }
    expect(cs_fence_in_bo(0, 8) && cs_fence_in_bo(4088, 4096) && !cs_fence_in_bo(4096, 4096) && !cs_fence_in_bo(4090, 4096) && !cs_fence_in_bo(0, 4) &&
           !cs_fence_in_bo(8, 8) && !cs_fence_in_bo(0xFFFFFFF8u, 4096), "fence offset inside the BO (8 B, 8-aligned)");
}

static void u8_ring() {
    const uint32_t mask = 4095;
    expect_u("empty ring (rptr == wptr): 4095 free", ring_free(100, 100, mask), 4095);
    expect_u("one dword in flight", ring_free(100, 101, mask), 4094);
    expect_u("full: wptr one behind rptr -> 0 free", ring_free(100, 99, mask), 0);
    expect_u("wrap: wptr 4090, rptr 10", ring_free(10, 4090, mask), 15);
    expect_u("wrap: wptr 10, rptr 4090", ring_free(4090, 10, mask), 4079);
    expect_u("rptr 0 wptr 4095", ring_free(0, 4095, mask), 0);
    expect_u("rptr 0 wptr 0", ring_free(0, 0, mask), 4095);
    expect(ring_has_space(291, 275) && !ring_has_space(290, 275), "need + 16 exactly");
    expect(!ring_has_space(4095, 0xFFFFFFF0u) && !ring_has_space(0, 0), "an absurd need never has space; free 0 has none");
    expect_u("advance wraps", ring_advance(4090, 10, mask), 4);
    expect_u("advance to exactly the end wraps to 0", ring_advance(4090, 6, mask), 0);
    expect_u("submit_dwords(1, no fence)", submit_dwords(1, false), 3 + 4 + 8);
    expect_u("submit_dwords(1, fence)", submit_dwords(1, true), 3 + 4 + 8 + 8);
    expect_u("submit_dwords(64, fence) = 275 (< the 2048 half-ring)", submit_dwords(64, true), 275);
    // Lap simulation: 2000 submits of 1 IB with a fence, a GPU that retires in random chunks; the writer must NEVER write into unread space.
    uint32_t wptr = 4000, rptr = 4000, gpuInFlight = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull, laps = 0, waits = 0;
    bool overrun = false;
    uint32_t written = 0;
    for (int s = 0; s < 2000; s++) {
        const uint32_t n = submit_dwords(1 + (s % 3), (s & 1) != 0);
        while (!ring_has_space(ring_free(rptr, wptr, mask), n)) {          // the writer waits; the GPU makes progress
            waits++;
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            uint32_t adv = 1 + (uint32_t)((rng >> 33) % 200);
            if (adv > gpuInFlight) adv = gpuInFlight;
            if (adv == 0) { overrun = true; break; }                        // nothing left to wait for and still no room: would deadlock
            rptr = (rptr + adv) & mask; gpuInFlight -= adv;
        }
        if (overrun) break;
        if (n > ring_free(rptr, wptr, mask)) overrun = true;
        const uint32_t nw = ring_advance(wptr, n, mask);
        if (nw < wptr) laps++;
        wptr = nw; gpuInFlight += n; written += n;
        if (((wptr - rptr) & mask) != gpuInFlight) overrun = true;         // the model's own consistency: in-flight == wptr - rptr
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        if (((rng >> 40) & 7) == 0) { const uint32_t adv = gpuInFlight / 32; rptr = (rptr + adv) & mask; gpuInFlight -= adv; }
    }
    expect(!overrun, "2000 submits: the writer never overwrites an unread dword and never deadlocks");
    expect(laps >= 8, "the simulation lapped the 4096-dword ring at least 8 times");
    expect(waits > 0, "and the writer really had to wait for the reader");
    expect(written > 4096u * 8u, "more than 8 rings of dwords were written");
}

static void u9_hang() {
    const uint64_t S = 1000000000ull;
    Hang h = { 0, 0, false };
    // idle ring, however long: never hung
    for (uint64_t t = 0; t < 100 * S; t += 7 * S) expect(!hang_detect(h, 5, 5, t), "an idle ring is never hung (retired == emitted)");
    expect(!h.hung, "still not latched");
    // a kick at t = 200 s: the clock starts THEN, not at the last idle observation
    h.tProgress = 100 * S;                       // a stale progress time from long ago
    hang_kick(h, 5, 5, 200 * S);                 // idle before the kick -> the clock restarts now
    expect_u("the kick restarts the clock on an idle ring", h.tProgress, 200 * S);
    expect(!hang_detect(h, 5, 6, 200 * S + 1999999999ull), "busy 1.999999999 s: not yet");
    expect(!h.hung, "not latched at 1.999999999 s");
    expect(hang_detect(h, 5, 6, 200 * S + 2 * S), "busy exactly 2 s with no progress: THIS call detects it");
    expect(h.hung, "latched");
    expect(!hang_detect(h, 5, 6, 300 * S), "an already-latched state is not detected again");
    // a kick while busy keeps the old clock
    Hang b = { 5, 10 * S, false };
    hang_kick(b, 5, 7, 11 * S);
    expect_u("a kick on a busy ring keeps the clock (no progress since t=10)", b.tProgress, 10 * S);
    // progress resets the clock
    Hang p = { 5, 10 * S, false };
    expect(!hang_detect(p, 6, 9, 11 * S), "retired advanced: the clock restarts");
    expect_u("tProgress moved to the observation", p.tProgress, 11 * S);
    expect(!hang_detect(p, 6, 9, 12 * S + 999999999ull), "1.999999999 s after the advance: not hung");
    expect(hang_detect(p, 6, 9, 13 * S), "2 s after the last advance: hung");
    // a slow but steady GPU (progress every 1.5 s) is never declared hung
    Hang s = { 0, 0, false };
    hang_kick(s, 0, 0, 0);
    bool any = false;
    for (uint64_t k = 1; k <= 40; k++) any = any || hang_detect(s, k, 100, k * 1500000000ull);
    expect(!any, "a GPU that advances every 1.5 s is never hung");
    // (a) an internal wait that reaches its bound latches once
    Hang w = { 0, 0, false };
    expect(hang_latch_wait(w) && w.hung, "a bounded wait at 2 s latches");
    expect(!hang_latch_wait(w), "and only the first one is the detecting call");
    expect_u("the detecting call returns Timeout", hang_rc(false, true), kTimeout);
    expect_u("every later call returns Aborted", hang_rc(true, false), kAborted);
    expect_u("even a call that would detect again returns Aborted", hang_rc(true, true), kAborted);
    expect_u("a healthy call is Ok", hang_rc(false, false), kOk);
    expect(hang_expired(Hang{ 0, 5, false }, 1, 2, 5 + kHangNs) && !hang_expired(Hang{ 0, 5, false }, 1, 2, 4 + kHangNs) && !hang_expired(Hang{ 0, 5, false }, 2, 2, 100 * kHangNs),
           "hang_expired: the boundary, and never when retired >= emitted");
    expect(!hang_expired(Hang{ 0, 50, false }, 1, 2, 10), "a clock that reads before tProgress does not underflow into a hang");
}

static void u10_wait() {
    uint64_t r = 99;
    expect_u("target 0 -> ok, idle at once", wait_resolve(0, 10, &r), kOk); expect_u("resolved 0", r, 0);
    expect_u("target 7 of 10", wait_resolve(7, 10, &r), kOk); expect_u("resolved 7", r, 7);
    expect_u("target == emitted", wait_resolve(10, 10, &r), kOk); expect_u("resolved 10", r, 10);
    expect_u("a future seqno -> BadArg", wait_resolve(11, 10, &r), kBadArg);
    expect_u("LAST -> ok", wait_resolve(N48N_SEQ_LAST, 10, &r), kOk); expect_u("LAST resolves to emitted at entry", r, 10);
    expect_u("LAST with nothing emitted -> 0 (idle)", (wait_resolve(N48N_SEQ_LAST, 0, &r), r), 0);
    expect_u("timeout below the cap", wait_timeout_ns(1000), 1000);
    expect_u("timeout at the cap", wait_timeout_ns(2000000000ull), 2000000000ull);
    expect_u("timeout above the cap is clamped to 2 s", wait_timeout_ns(2000000001ull), 2000000000ull);
    expect_u("infinite timeout is clamped", wait_timeout_ns(~0ull), 2000000000ull);
    expect_u("retired below emitted", retired_clamped(5, 9), 5);
    expect_u("a corrupt slot above emitted is clamped", retired_clamped(0x1000, 9), 9);
    expect_u("all retired", retired_clamped(9, 9), 9);
}

// Decode a built Submit back into its packets.
struct Pkt { uint32_t op; uint32_t n; size_t at; };
static std::vector<Pkt> decode(const uint32_t *o, uint32_t n) {
    std::vector<Pkt> v;
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = o[i];
        const uint32_t type = h >> 30, cnt = (h >> 16) & 0x3FFF, op = (h >> 8) & 0xFF;
        if (type != 3) { gFail++; std::printf("FAIL: a non-PACKET3 header %#x at %u\n", h, i); break; }
        v.push_back(Pkt{ op, cnt + 2, i });
        i += cnt + 2;
    }
    return v;
}
static void u11_submit() {
    const uint64_t va[3] = { 0xffff80010002b000ull, 0xffff800100001000ull, 0x0000000000123000ull };
    const uint32_t by[3] = { 256, 1984, 16 };
    const uint64_t seqAddr = 0x8400000000ull + 0xC0, fenceAddr = 0x8400400000ull + 8;
    uint32_t o[300];
    { // two IBs with a fence
        const uint32_t n = build_submit(o, 300, va, by, 2, 8, true, fenceAddr, seqAddr, 0x1122334455667788ull);
        expect_u("2 IBs + fence = 3 + 8 + 8 + 8 = 27 dwords", n, 27);
        auto pk = decode(o, n);
        expect(pk.size() == 5, "five packets: CONTEXT_CONTROL, IB, IB, RELEASE_MEM (user), RELEASE_MEM (seqno)");
        if (pk.size() == 5) {
            expect(pk[0].op == 0x28 && pk[1].op == 0x3F && pk[2].op == 0x3F && pk[3].op == 0x49 && pk[4].op == 0x49, "ORDER: ctxctl, ib, ib, USER FENCE, then the kernel SEQNO");
            expect(o[0] == 0xC0012800u && o[1] == 0x80000000u && o[2] == 0, "CONTEXT_CONTROL with load_enable only");
            expect(o[3] == 0xC0023F00u && o[4] == 0x0002b000u && o[5] == 0xffff8001u && o[6] == (64u | (8u << 24)), "IB0: canonical VA unstripped, dwords | vmid 8 << 24, no VALID bit");
            expect(o[7] == 0xC0023F00u && o[8] == 0x00001000u && o[9] == 0xffff8001u && o[10] == (496u | (8u << 24)), "IB1");
            const size_t f = pk[3].at, k = pk[4].at;
            expect(o[f + 3] == (uint32_t)(fenceAddr & 0xFFFFFFF8u) && o[f + 4] == (uint32_t)(fenceAddr >> 32), "the user fence goes to the fence address");
            expect(o[k + 3] == (uint32_t)(seqAddr & 0xFFFFFFF8u) && o[k + 4] == (uint32_t)(seqAddr >> 32), "the seqno goes to the native slot");
            expect(o[f + 5] == 0x55667788u && o[f + 6] == 0x11223344u && o[k + 5] == 0x55667788u && o[k + 6] == 0x11223344u, "both write the same 64-bit seqno");
            expect(o[f + 1] == amdgpu::pm4_release_mem_dw1() && o[k + 1] == amdgpu::pm4_release_mem_dw1(), "upstream emit_fence GCR/event word");
            expect(((o[f + 2] >> 29) & 7) == 2 && ((o[f + 2] >> 24) & 3) == 0 && ((o[k + 2] >> 29) & 7) == 2 && ((o[k + 2] >> 24) & 3) == 0, "DATA_SEL 2 (64-bit), INT_SEL 0");
            expect(f < k, "the user fence PRECEDES the kernel seqno, so a retired seqno implies the fence landed");
        }
    }
    { const uint32_t n = build_submit(o, 300, va, by, 1, 8, false, 0, seqAddr, 5); expect_u("1 IB, no fence = 15 dwords", n, 15);
      auto pk = decode(o, n); expect(pk.size() == 3 && pk[2].op == 0x49, "ctxctl, ib, seqno only"); }
    { const uint32_t n = build_submit(o, 300, va, by, 3, 8, true, fenceAddr, seqAddr, 5);
      expect_u("3 IBs + fence = 31 dwords", n, 31);
      expect(o[11] == 0xC0023F00u && o[12] == 0x00123000u && o[13] == 0u && o[14] == (4u | (8u << 24)), "a low-half IB VA is passed as is, control = 4 dwords | vmid 8 << 24"); }
    { std::vector<uint64_t> bigVa(64, 0xffff800100001000ull); std::vector<uint32_t> bigBy(64, 64);
      uint32_t big[300]; const uint32_t n = build_submit(big, 300, bigVa.data(), bigBy.data(), 64, 8, true, fenceAddr, seqAddr, 9);
      expect_u("64 IBs + fence = 275 dwords", n, 275); auto pk = decode(big, n); expect(pk.size() == 1 + 64 + 2, "one packet per IB plus 3"); }
    expect_u("n = 0 refused", build_submit(o, 300, va, by, 0, 8, true, fenceAddr, seqAddr, 1), 0);
    expect_u("n = 65 refused", build_submit(o, 300, va, by, 65, 8, true, fenceAddr, seqAddr, 1), 0);
    expect_u("a misaligned seqno slot refused", build_submit(o, 300, va, by, 1, 8, true, fenceAddr, seqAddr + 4, 1), 0);
    expect_u("a misaligned fence address refused", build_submit(o, 300, va, by, 1, 8, true, fenceAddr + 4, seqAddr, 1), 0);
    expect_u("a misaligned fence address is ignored when there is no fence", build_submit(o, 300, va, by, 1, 8, false, fenceAddr + 4, seqAddr, 1), 15);
    expect_u("a too-small output buffer refused", build_submit(o, 26, va, by, 2, 8, true, fenceAddr, seqAddr, 1), 0);
    expect_u("vmid 16 refused", build_submit(o, 300, va, by, 1, 16, true, fenceAddr, seqAddr, 1), 0);
    // the fence window
    FenceSlot fs[4] = {};
    bool isNew = false;
    expect(fence_slot_pick(fs, 4, 7, 0, &isNew) == 0 && isNew, "first fence takes slot 0");
    fs[0] = FenceSlot{ 7, 0, 1 };
    expect(fence_slot_pick(fs, 4, 7, 0, &isNew) == 0 && !isNew, "the same (handle, page) reuses its slot");
    expect(fence_slot_pick(fs, 4, 7, 1, &isNew) == 1 && isNew, "another page of the same BO takes the next slot");
    expect(fence_slot_pick(fs, 4, 8, 0, &isNew) == 1 && isNew, "another BO takes the next free slot");
    fs[1] = FenceSlot{ 8, 0, 1 }; fs[2] = FenceSlot{ 9, 0, 1 }; fs[3] = FenceSlot{ 10, 0, 1 };
    expect(fence_slot_pick(fs, 4, 11, 0, &isNew) == -1, "a full window -> -1 (NoResources)");
    expect(fence_slot_pick(fs, 4, 9, 0, &isNew) == 2 && !isNew, "a full window still finds an existing binding");
    fs[2].used = 0;
    expect(fence_slot_pick(fs, 4, 11, 3, &isNew) == 2 && isNew, "a released slot is reused");
    expect_u("VRAM fence address = mc + offset", fence_addr_vram(0x8000100000ull, 8), 0x8000100008ull);
    expect_u("GTT fence address = window + slot * 4 KiB + (offset in page)", fence_addr_gtt(0x8400010000ull, 3, 0x1238), 0x8400010000ull + 3 * 4096 + 0x238);
}

static void u13_review() {
    // ReadRegs allowlist 0x263e..0x2641
    expect(regs_allowed(0x263e, 1) && regs_allowed(0x263e, 4) && regs_allowed(0x2641, 1) && regs_allowed(0x2640, 2), "allowlist: 0x263e..0x2641 accepted");
    expect(!regs_allowed(0x263d, 1) && !regs_allowed(0x2642, 1) && !regs_allowed(0x263e, 5) && !regs_allowed(0x2641, 2) && !regs_allowed(0x2830, 4) &&
           !regs_allowed(0, 1) && !regs_allowed(0x263e, 0) && !regs_allowed(0x263e, 17) && !regs_allowed(~0ull, 1) && !regs_allowed(0x2641, ~0ull),
           "allowlist: everything else refused (below, above, straddling, zero/huge counts, overflow)");
    // The 64-bit write counter across the 4096 wrap: monotonic, the doorbell value never decreases, the index wraps, space checks stay right.
    const uint32_t mask = 4095;
    uint64_t wc = 138, lastDoorbell = 0, rptrAbs = 138;   // the ring's used wptr at boot was 138 dwords
    bool mono = true, idxOk = true, spaceOk = true; uint64_t laps = 0, maxIdx = 0;
    for (int s = 0; s < 2000; s++) {
        const uint32_t n = submit_dwords(2, true);
        while (!ring_has_space(ring_free64((uint32_t)rptrAbs, wc, mask), n)) rptrAbs += 1 + (wc - rptrAbs) / 2;   // the GPU catches up
        const uint32_t before = ring_idx(wc, mask);
        const uint64_t nwc = wc + n;
        if (ring_idx(nwc, mask) != ((before + n) & mask)) idxOk = false;
        if (ring_idx(nwc, mask) < before) laps++;
        if (n > ring_free64((uint32_t)rptrAbs, wc, mask)) spaceOk = false;
        wc = nwc;
        if (wc <= lastDoorbell) mono = false;             // the value written to the doorbell and the wptr shadow is the FULL counter
        lastDoorbell = wc;
        if (ring_idx(wc, mask) > maxIdx) maxIdx = ring_idx(wc, mask);
    }
    expect(mono, "the doorbell / wptr-shadow value strictly increases across every wrap");
    expect(idxOk && spaceOk, "the ring index and the space check stay correct across the wrap");
    expect(laps >= 13 && wc > 4096ull * 13, "2000 submits: more than 13 laps, and the counter is well past 4096 (no masking of the doorbell value)");
    expect_u("counter arithmetic", ring_idx(4096 + 5, mask), 5);
    expect_u("free space uses only the masked positions: rptr 10, wc 4090+4096", ring_free64(10, 4090 + 4096, mask), 15);
    expect_u("free space with the counter one lap ahead of a wrapped rptr", ring_free64(4090, 10 + 4096ull * 3, mask), 4079);
}

// ---- source pins ---------------------------------------------------------------------------------------------------------------
static void u12_pins(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string eng = slurp(src + "amd/native_s1c.cpp"), cli = slurp(src + "Navi48NativeClient.cpp"), boot = slurp(src + "Navi48Bringup.cpp"),
                      uc = slurp(src + "Navi48UserClient.cpp"), mk = slurp(root + "/src/navi48-bringup/Makefile"), plist = slurp(root + "/src/navi48-bringup/Info.plist"),
                      pure = slurp(src + "amd/native_s1c_pure.h"), abi = slurp(src + "Navi48NativeABI.h");
    expect(!eng.empty() && !cli.empty() && !boot.empty() && !uc.empty() && !mk.empty() && !plist.empty() && !pure.empty() && !abi.empty(), "the sources are readable");
    // gate order in n1c_open: the ladder, the S1b gate, HUNG, then exclusivity
    const size_t g1 = eng.find("ctx.reached != BringupStage::ComputeDispatch"), g2 = eng.find("s1b.gate != kGateOn || !s1b.ran || !s1b.positivePass || s1b.stopped"),
                 g3 = eng.find("if (gHang.hung) {\n        N1C_LOG(\"open refused (NotReady): the GPU was declared HUNG"), g4 = eng.find("OSCompareAndSwap(0, 1, &gOpenFlag)");
    expect(g1 != std::string::npos && g2 != std::string::npos && g3 != std::string::npos && g4 != std::string::npos, "the four gates are present");
    expect(g1 < g2 && g2 < g3 && g3 < g4, "ORDER: ladder, S1b POSITIVE PASS, not HUNG, exclusivity");
    expect(eng.find("return kIOReturnExclusiveAccess") != std::string::npos, "a second open returns ExclusiveAccess");
    expect_u("exactly one path opens a native session (one OSCompareAndSwap(0, 1", count_of(eng, "OSCompareAndSwap(0, 1, &gOpenFlag)"), 1);
    // ring path: one helper, space check before the write, the owner and emitted before the doorbell. 0.0.627 (G2): ring_put_locked (the caller holds gRingLock);
    // ring_submit assigns the seqno, builds and puts under ONE hold of gRingLock (two sessions can never take the same seqno).
    const size_t re = eng.find("static uint32_t ring_put_locked("), sp = eng.find("ring_has_space(ring_free64(r, gWc, mask), n)", re), wr = eng.find("ring[ring_idx(wc, mask)] = dw[i]", re),
                 on = eng.find("n48native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc);", re),
                 em = eng.find("__atomic_store_n(&gEmitted, seq, __ATOMIC_RELEASE);", re), hf = eng.find("amdgpu_hdp_flush(dev)", em), db = eng.find("WDOORBELL64(dev, off, wptr64)", re);
    expect(re != std::string::npos && sp != std::string::npos && wr != std::string::npos && on != std::string::npos && em != std::string::npos && hf != std::string::npos && db != std::string::npos, "ring_put_locked's steps are present");
    expect(re < sp && sp < wr && wr < on && on < em && em < hf && hf < db, "ORDER in ring_put_locked: space check, ring write, owner record, emitted++, HDP flush, doorbell");
    expect_u("the ring is written in exactly one place", count_of(eng, "ring[ring_idx(wc, mask)] = dw[i]"), 1);
    expect_u("0.0.623: the only other ring stores are G1's NOP fills (the scrub outside the live range, the remap clear) and 0.0.627's recovery scrub", count_of(eng, "ring[i] = cp_p3_nop1();"), 3);
    { const size_t g1b = eng.find("// ==== 0.0.623: GPU-apps stage G1"), sc = eng.find("static void g2_scrub_locked("), n1 = eng.find("ring[i] = cp_p3_nop1();"), n2 = eng.find("ring[i] = cp_p3_nop1();", n1 + 1);
      expect(sc != std::string::npos && n1 > sc && n1 < eng.find("\n}\n", sc) && n2 > g1b, "the NOP fills: one in g2_scrub_locked (the recovery), the other two inside the G1 block"); }
    expect_u("the doorbell is rung in exactly one place", count_of(eng, "WDOORBELL64("), 1);
    { const std::string rs = fn_body(eng, "static uint32_t ring_submit(");
      expect(!rs.empty() && in_order(rs, { "IOLockLock(gRingLock);", "const uint64_t seq = emitted_now() + 1ull;", "build_submit(gSubmitBuf,", "ring_put_locked(gSubmitBuf, n, seq,", "IOLockUnlock(gRingLock);", "if (er == kTimeout) { (void)stall_from_wait(); return kTimeout; }" }),
             "ring_submit: the ring lock is held over the seqno, the build and the put (incl. the space check); a 2 s space wait goes to the stall path AFTER the lock is dropped"); }
    expect(eng.find("return kTimeout;", sp) < wr, "a space wait that reaches 2 s returns before anything is written");
    expect_u("ring_put_locked is called by ring_submit, the recovery probe and (0.0.650, G5, ON only) ring_submit_app_g5 only", count_of(eng, "ring_put_locked("), 4);
    expect_u("ONE builder call and ONE put of a built Submit in each of the two submit paths: ring_submit and (0.0.650, G5) ring_submit_app_g5 (0.0.627: G1's hang IB and probe IB go through ring_submit too)", count_of(eng, "build_submit(gSubmitBuf") + count_of(eng, "ring_put_locked(gSubmitBuf, n, seq,"), 4);
    expect_u("ring_submit callers: Submit (0.0.650: the OFF branch and WindowServer's branch) + G1's hang IB + G1's probe IB", count_of(eng, "ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut)") + count_of(eng, "ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq)"), 4);
    expect(eng.find("resolve_fence(s, v.fenceHandle") < eng.find("ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut)"), "the fence target is resolved BEFORE the ring is touched");
    expect(eng.find("va_exec_covered(s->maps") < eng.find("ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut)"), "the IB coverage check is BEFORE the ring is touched");
    expect(eng.find("const uint32_t gate = gpu_gate(s);", eng.find("IOReturn n1c_submit(")) < eng.find("ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut)"), "Submit takes the HUNG gate first");
    expect(eng.find("cs_parse(in, size, &v)") < eng.find("IOLockLock(gCliLock);", eng.find("IOReturn n1c_submit(")), "the chunk is parsed before the lock");
    expect(eng.find("wb_bus + kWbOffsetNativeSeq") != std::string::npos, "the seqno slot address is the WB page + the slot offset");
    // WaitSeq takes no lock; QueryInfo / ReadRegs neither
    for (const char *fn : { "IOReturn n1c_wait(", "IOReturn n1c_query_info(", "IOReturn n1c_read_regs(" }) {
        const size_t a = eng.find(fn), b = eng.find("\n}\n", a);
        expect(a != std::string::npos && b != std::string::npos && eng.substr(a, b - a).find("IOLockLock(gCliLock)") == std::string::npos, "this selector takes no client lock");
    }
    for (const char *fn : { "IOReturn n1c_hello(", "IOReturn n1c_bo_create(", "IOReturn n1c_bo_free(", "IOReturn n1c_gem_va(", "IOReturn n1c_ctx(", "IOReturn n1c_submit(" }) {
        const size_t a = eng.find(fn), b = eng.find("\n}\n", a);
        expect(a != std::string::npos && b != std::string::npos && eng.substr(a, b - a).find("Session *s = sess_lock(ref, ") != std::string::npos && eng.substr(a, b - a).find("sess_unlock(s);") != std::string::npos,
               "this selector takes the caller's SESSION lock (0.0.627: the per-session successor of the one client lock)");
    }
    expect_u("every selector body checks Hello (9 in ABI 1.0 + 6 scanout selectors of ABI 1.1, 0.0.603 + the DAL step of ABI 1.2, 0.0.604 + the mode trial of ABI 1.3, 0.0.605 + the HELD mode's hold / release of ABI 1.7, 0.0.609 + BoImportHost of ABI 1.9, 0.0.612 + the five instance-2 scanout selectors of ABI 1.11, 0.0.661)", count_of(eng, "if (!sess_hello(ref)) return kIOReturnNotReady;"), 25);
    expect_u("every locked selector re-checks the session under the lock (6 + ScanoutAcquire / Register / Release, 0.0.603 + BoImportHost, 0.0.612)", count_of(eng, "closed while we waited for the lock"), 10);
    expect(eng.find("static Session gSess;") != std::string::npos && eng.find("IOFree(ns") == std::string::npos && count_of(eng, "IOMalloc(sizeof(Session))") == 1 && eng.find("IOMallocAligned") == std::string::npos && eng.find("IOFreeAligned") == std::string::npos,
           "SESSION LIFETIME: slot 0 is a static, slots 1..3 (0.0.627) are allocated once and never freed");
    { const size_t a = eng.find("IOReturn n1c_query_info("), b = eng.find("\n}\n", a); expect(eng.substr(a, b - a).find("sess_lock(") == std::string::npos && eng.substr(a, b - a).find("Session *s = sess_peek(ref);") != std::string::npos, "the lock-free QueryInfo reads the caller's never-freed session record without a lock"); }
    expect(eng.find("vram_alloc.free(s->blk") == std::string::npos && eng.find("gPtFree[gPtFreeN++] = s->blk[i]") != std::string::npos &&
           count_of(eng, "vram_alloc.alloc(kArenaBlockBytes") == 2, "PAGE TABLES: blocks return to the per-boot reserve, never to vram_alloc (the two allocs are the reserve's growth and the park root)");
    expect(eng.find("if (!regs_allowed(off, count)") != std::string::npos, "ReadRegs is allowlisted");
    expect(eng.find("const uint64_t wptr64 = wc;") != std::string::npos && eng.find("if (!gWcInit) { gWc = cp.wptr; gWcInit = true; }") != std::string::npos &&
           count_of(eng, "cp.wptr = ring_idx(wc, mask)") == 1, "the doorbell / wptr shadow carry the full 64-bit counter, initialised once from cp.wptr");
    expect_u("the native path never calls native_s1b_refuse", count_of(eng, "native_s1b_refuse("), 0);
    expect_u("... and does not touch CP_DEBUG", count_of(eng, "0x1e1f"), 0);
    expect_u("close is idempotent and single-owner: the session's open flag is cleared once, under its lock (0.0.627)", count_of(eng, "    __atomic_store_n(&s->open, false, __ATOMIC_RELEASE);          // the Session itself is static (slot 0) or never freed (slots 1..3)\n"), 1);
    expect(eng.find("LEAKED (HUNG)") != std::string::npos && eng.find("seqs, freed\"") != std::string::npos, "both close log lines exist");
    expect(eng.find("if (!leak && !own_wait(s)) leak = true;") != std::string::npos, "close waits for ITS OWN work (0.0.627) and leaks when it cannot");
    expect(eng.find("program_root(s->vmid, gPark.pa)") != std::string::npos && eng.find("program_root(s->vmid, gPark.pa)") < eng.find("bo_release(s, h, kRelClosing)"), "the session's context is parked BEFORE any memory is freed");
    expect(eng.find("if (hung_now()) m = kRelLeak;") != std::string::npos && fn_body(eng, "IOReturn n1c_bo_free(").find("bo_release(s, h, m);") != std::string::npos, "BoFree under HUNG leaks and succeeds");
    // client: exact shapes, nothing inline-refusable slips through
    expect(cli.find("args->structureInputDescriptor != nullptr || args->structureOutputDescriptor != nullptr") != std::string::npos, "out-of-line structs are refused");
    expect_u("the client dispatches exactly the 21 selectors (9 + the 6 scanout selectors, 0.0.603 + the DAL step, 0.0.604 + the mode trial, 0.0.605 + the HELD mode's hold / release, 0.0.609 + the Metal nub's publish / withdraw, 0.0.610 + BoImportHost, 0.0.612 + the five instance-2 scanout selectors, 0.0.661)", count_of(cli, "case N48N_SEL_"), 27);
    expect(cli.find("return kIOReturnBadArgument;   // an unknown selector") != std::string::npos && cli.find("super::externalMethod") == std::string::npos, "an unknown selector never falls through to IOUserClient");
    expect(cli.find("kIOClientPrivilegeAdministrator") != std::string::npos, "root only");
    expect(cli.find("if (opened) { n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"clientClose\")") != std::string::npos && cli.find("amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"stop\")") != std::string::npos, "close runs from clientClose and stop, with this client's session");
    // the bringup switch, and the default path unchanged
    expect(boot.find("if (type == N48N_UC_TYPE) return IOAccelNavi48NativeClient::create(") != std::string::npos, "newUserClient switches on the type");
    expect(boot.find("if (type == N48N_UC_TYPE)") < boot.find("auto *uc = OSTypeAlloc(Navi48UserClient);"), "the switch precedes the legacy allocation");
    expect_u("the legacy client is still created in exactly one place", count_of(boot, "OSTypeAlloc(Navi48UserClient)"), 1);
    // legacy refusals
    for (const char *fn : { "doAllocVRAM", "doFreeVRAM", "doWriteVRAM", "doSelfTest" }) {
        const std::string sig = std::string("IOReturn Navi48UserClient::") + fn + "(IOExternalMethodArguments *args) {\n\tif (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSite";
        expect(uc.find(sig) != std::string::npos, fn);
    }
    expect(uc.find("IOReturn Navi48UserClient::doSubmitIB(IOExternalMethodArguments *args) {\n\tif (amdgpu::native_s1b_refuse(1)) return kIOReturnNotPermitted;") != std::string::npos, "SubmitIB still refuses through the S1b latch");
    expect(eng.find("bool n1c_refuse_legacy(uint32_t site) {\n    if (!native_s1b_latched()) return false;") != std::string::npos, "the legacy refusal is false until the S1b latch is set (every non-native boot)");
    expect_u("the native engine never calls cp_submit_ib / cp_ring_write / cp_kick_doorbell", count_of(eng, "cp_submit_ib(") + count_of(eng, "cp_ring_write(") + count_of(eng, "cp_kick_doorbell("), 0);
    // 0.0.602: one CPU-map descriptor per VRAM BO (IOConnectUnmapMemory64 matches the mapping by descriptor identity)
    expect_u("IODeviceMemory::withRange is called in exactly one place (the lazy creation in n1c_memory_for_handle)", count_of(eng, "IODeviceMemory::withRange("), 1);
    {
        const size_t fn = eng.find("IOReturn n1c_memory_for_handle("), cr = eng.find("if (b.vmd == nullptr) {", fn), wr = eng.find("IODeviceMemory::withRange(", fn), ret = eng.find("md->retain(); *memory = md;", fn);
        expect(fn != std::string::npos && cr != std::string::npos && wr > cr && ret > wr, "the descriptor is created only when the BO has none, then retained for the caller");
        expect_u("the descriptor is created only under the b.vmd == nullptr guard (one creation site, guarded)", count_of(eng, "if (b.vmd == nullptr) {"), 1);
        expect(eng.find("*memory = md;", fn) == ret + 15 || eng.find("md->retain(); *memory = md;", fn) == ret, "the returned descriptor is the stored one");
    }
    expect_u("the descriptor is released in exactly two places (BoFree path, close path), each nulling the pointer", count_of(eng, "if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }"), 2);
    expect_u("no other release of the descriptor (0.0.641: plus the close-time unmap sweep's release of its OWN reference, n1c_unmap_for_close, which retains first)", count_of(eng, "vmd->release()"), 3);
    {
        const size_t bo = eng.find("static void bo_release(Session *s, uint32_t h, RelMode mode) {"), cl = eng.find("if (mode == kRelClosing) {", bo), nm = eng.find("mode == kRelNormal", bo), lk = eng.find("} else {\n        // Leak:", bo);
        const size_t r1 = eng.find("b.vmd->release()", bo), r2 = eng.find("b.vmd->release()", r1 + 1);
        expect(cl < r1 && r1 < nm && nm < r2 && r2 < lk, "one release in the closing branch, one in the normal branch, none in the leak branch (leaked with the BO)");
        expect(eng.find("b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed\n        if (b.kind == kBoGtt) { sysmem_free(b.sm); sys_release(b); }", bo) != std::string::npos, "released before the memory it names is freed");
    }
    // build plumbing
    expect(mk.find("src/Navi48NativeClient.cpp") != std::string::npos, "the Makefile builds the client");
    expect(plist.find("<string>0.0.664</string>") != std::string::npos && count_of(plist, "0.0.664") == 2 && plist.find("0.0.605") == std::string::npos, "Info.plist is 0.0.664");
    expect(pure.find("kWbOffsetNativeSeq = 0x0C0u") != std::string::npos, "the native seqno slot offset");
    // banned strings in the new files
    const std::string bad1 = std::string("pipe+0x2") + "80", bad2 = std::string("+0x2") + "82", bad3 = std::string("+0x2") + "99";   // built at run time: this file must not contain the tokens
    for (const std::string *f : { &eng, &cli, &pure, &abi }) expect(f->find(bad1) == std::string::npos && f->find(bad2) == std::string::npos && f->find(bad3) == std::string::npos, "no banned pipe offsets in the new files");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    u1_abi(); u2_place(); u3_gemva(); u4_tables(); u5_vamap(); u6_pt(); u7_cs(); u8_ring(); u9_hang(); u10_wait(); u11_submit(); u13_review(); u12_pins(root);
    std::printf("native_s1c_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
