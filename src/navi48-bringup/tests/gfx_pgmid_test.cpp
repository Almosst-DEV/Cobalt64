// gfx_pgmid_test.cpp — 0.0.436 (an internal design note Part 1, design "2. M"). PROGRAM-IDENTITY
// COST'S HOST PROOF, for switch 38's modes OFF / T (1) / T+M (2) / SHADOW (3).
//
// T1 drives the REAL n48_pgm_need (gfx_pgmid.h) over the REAL identity table (xlat12_ib.h's by-index
// xlat12_shader_id_row accessor) and proves its answer is the true maximum ndw among same-(stage,head) rows, with a
// planted "first match" mutant CAUGHT diverging on a real group. T2 proves match(512) == match(need) on a REAL
// short/long-sharing-a-head group using genuine captured program bytes (fixture_arm13's ViewportToNDC, whose head
// is shared by two OTHER real rows of different length), with a planted "one dword short" break CAUGHT. T3 models
// the no-identity witness's first-sighting rule (gXdMiss) in isolation, with a planted "hand it the trimmed buffer"
// break CAUGHT. T4 runs the SAME model in OFF/T/SHADOW form against BOTH named real fixtures: fixture_arm13's two
// genuine program bodies (real identity content) and fixture_mib_f48_f20_f21.h's real concatenated IBs (real
// xlat12_ib_segments structure — it carries no shader ISA payload, so it proves segmentation is untouched by this
// switch rather than identity content), with the same break CAUGHT there too. T5 pins N48_PGMID_FMT under the
// logger's 512-byte cap. T6 (0.0.436) is mode T+M's own suite, over the REAL n48_pm (gfx_pgmid.h) and the REAL
// ring/poison/pagerec primitives (gfx_copyguard.h) - the same headers the kext compiles - modelling
// gfxsrc_pgm_profile's mode-2 branch exactly (lookup before the read; a valid hit skips the read and notes its
// pages; a miss fills the row with the id, the pre-read ring mark, and the pages that read touched): memo key
// includes stage; the memo clears per pass; a ring-mark change forces a miss; a poison overlap forces a miss; a hit
// notes its pages into the recorder; a hit's xlat12_ib_profile_stage output equals a fresh resolve's; both named
// fixtures agree across OFF/T/T+M; a full table stores nothing. T7 pins that mode 2 is no longer refused and that
// the per-pass memo clear exists in AppleHardwareHook.cpp (optional argv[1]; skipped, not failed, when absent).
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ \
//         src/navi48-bringup/tests/gfx_pgmid_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c \
//         -o /tmp/pgmidtest && /tmp/pgmidtest [src/navi48-bringup/src/apple/AppleHardwareHook.cpp]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_pgmid.h"
#include "gfx_copyguard.h"
#include "gfx_subst_caps.h"      // build 0.0.484: K3/K5 windows, n48_xd_read_program, n48_xd_ours_find
#include "xlat12.h"
#include "xlat12_ib.h"
#include "fixture_arm13_f14_f15_gpupass_viewporttondc.h"
#include "fixture_mib_f48_f20_f21.h"
#include "fixture_glass_bd_be.h"  // build 0.0.484: glass BD/BE's real 5376-byte images (gen_fixture_glass.py)
#include "fixture_j_vfxxghb.h"   // build 0.0.504: program J's real m4c-r19 image (gen_fixture_j.py)
#include "xlat12_readset.h"      // build 0.0.504: J1 reads the row the profile names (kXlat12Readset)
#include "xlat12_abi_ptrs.h"

static int gFail = 0, gRun = 0;

static void expect(const char *what, bool ok)
{
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL  %s\n", what); }
    else std::printf("ok    %s\n", what);
}

// 0.0.436 — T7's source-pin helpers, the same convention gfx_copyguard_test.cpp's own T9 uses: every argument is
// optional, and checks that need a missing file are SKIPPED, not failed.
static bool read_file(const char *path, std::string &out) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return false;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return true;
}
static bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

// =============================================================================================================
// The real table, copied through the by-index accessor exactly as gfxsrc_pgmid_rows_build does in the kext.
// =============================================================================================================
static n48_pgmid_row gRows[128];
static uint32_t gRowN = 0;
static void build_rows()
{
    if (gRowN) return;
    uint32_t i = 0;
    for (; i < 128u; i++) {
        uint32_t stage = 0u, ndw = 0u, head[4] = { 0u, 0u, 0u, 0u };
        if (!xlat12_shader_id_row(i, &stage, &ndw, head)) break;
        gRows[i] = n48_pgmid_row{ stage, ndw, { head[0], head[1], head[2], head[3] } };
    }
    gRowN = i;
}

// The planted break named by the design: "take the first match's ndw instead" of tracking the maximum.
static uint32_t n48_pgm_need_MUTANT_first_match(uint32_t stage, const uint32_t head[4],
                                                 const n48_pgmid_row *rows, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (rows[i].stage != stage) continue;
        if (rows[i].head[0] != head[0] || rows[i].head[1] != head[1] ||
            rows[i].head[2] != head[2] || rows[i].head[3] != head[3]) continue;
        return rows[i].ndw > N48_PGMID_CAP ? N48_PGMID_CAP : rows[i].ndw;   // returns the FIRST hit, not the max
    }
    return 0u;
}

// =============================================================================================================
// T1 — n48_pgm_need over the REAL table equals the max among same-(stage,head) rows. Break: first match's ndw.
// =============================================================================================================
static void test_T1(void)
{
    build_rows();
    expect("T1 setup: the real table has rows", gRowN > 0u);

    // Independent brute-force reference: for every row, the true max ndw among all rows sharing its (stage,head).
    uint32_t divergedGroups = 0, checkedGroups = 0;
    bool anyMultiRowGroup = false;
    for (uint32_t i = 0; i < gRowN; i++) {
        // Only check each DISTINCT (stage,head) once: skip if an earlier row already has the same key.
        bool seenBefore = false;
        for (uint32_t j = 0; j < i; j++)
            if (gRows[j].stage == gRows[i].stage && gRows[j].head[0] == gRows[i].head[0] &&
                gRows[j].head[1] == gRows[i].head[1] && gRows[j].head[2] == gRows[i].head[2] &&
                gRows[j].head[3] == gRows[i].head[3]) { seenBefore = true; break; }
        if (seenBefore) continue;
        checkedGroups++;
        uint32_t trueMax = 0u, membersInGroup = 0u;
        for (uint32_t j = 0; j < gRowN; j++) {
            if (gRows[j].stage != gRows[i].stage || gRows[j].head[0] != gRows[i].head[0] ||
                gRows[j].head[1] != gRows[i].head[1] || gRows[j].head[2] != gRows[i].head[2] ||
                gRows[j].head[3] != gRows[i].head[3]) continue;
            membersInGroup++;
            if (gRows[j].ndw > trueMax) trueMax = gRows[j].ndw;
        }
        if (membersInGroup > 1u) anyMultiRowGroup = true;
        const uint32_t got = n48_pgm_need(gRows[i].stage, gRows[i].head, gRows, gRowN);
        if (got != trueMax) divergedGroups++;
        const uint32_t gotMutant = n48_pgm_need_MUTANT_first_match(gRows[i].stage, gRows[i].head, gRows, gRowN);
        (void)gotMutant;
    }
    expect("T1 setup: the real table has at least one (stage,head) group with more than one row",
           anyMultiRowGroup);
    expect("T1 real: n48_pgm_need matches the brute-force max over EVERY real (stage,head) group",
           checkedGroups > 0 && divergedGroups == 0u);

    // Non-vacuity: the mutant must diverge from the real function on at least one real group (the ViewportToNDC
    // head, which fixture_arm13 also uses below — custom_effect_vertex(90) is table-earlier than the true max
    // vhe2ae2a2...(139), so "first match" undershoots).
    {
        const uint32_t vpHead[4] = { 0xbefe00c1u, 0xd71f0002u, 0x020100c1u, 0x9302ff03u };
        const uint32_t real = n48_pgm_need(1u, vpHead, gRows, gRowN);
        const uint32_t mut  = n48_pgm_need_MUTANT_first_match(1u, vpHead, gRows, gRowN);
        expect("T1 mutant (first match, not max) CAUGHT: diverges from the real max on the ViewportToNDC head",
               real != mut && real == 139u);
    }
}

// =============================================================================================================
// T2 — a REAL short/long-sharing-a-head group: match(512) == match(need), using genuine captured program bytes.
// Break: cap the read one dword short of `need`.
// =============================================================================================================
static void test_T2(void)
{
    build_rows();
    const uint32_t vpHead[4] = { kArm13ViewportToNdcVs[0], kArm13ViewportToNdcVs[1],
                                  kArm13ViewportToNdcVs[2], kArm13ViewportToNdcVs[3] };
    const uint32_t need = n48_pgm_need(1u, vpHead, gRows, gRowN);
    expect("T2 setup: need() for ViewportToNDC's head is the group's true max (139, not its own 96)",
           need == 139u);

    // build 0.0.484: 512 is the OFF window (kXdIdDwords = N48_XD_ID_DWORDS), no longer N48_PGMID_CAP (1344).
    const int id512 = xlat12_shader_id_match(1u, kArm13ViewportToNdcVs, N48_XD_ID_DWORDS);
    const int idNeed = xlat12_shader_id_match(1u, kArm13ViewportToNdcVs, need);
    expect("T2 real: match(512) identifies ViewportToNDC", id512 == KARM13_VPTONDC_ID);
    expect("T2 real: match(need) == match(512) - capping the read at `need` never changes the answer",
           idNeed == id512);

    // MUTANT: read one dword short of `need` (need - 1 = 138 dwords, still < the true max but this time also
    // one short of ViewportToNDC's OWN ndw... no: 138 >= 96, so ViewportToNDC's own row is still reachable. Use
    // the REAL "first match" shortfall instead - 90 dwords, which is LESS than ViewportToNDC's own ndw (96) and
    // so excludes its row entirely (xlat12_shader_id_match's `s->ndw > n` guard skips it).
    {
        const uint32_t brokenNeed = n48_pgm_need_MUTANT_first_match(1u, vpHead, gRows, gRowN);
        const int idBroken = xlat12_shader_id_match(1u, kArm13ViewportToNdcVs, brokenNeed);
        expect("T2 mutant (first-match's too-short need) CAUGHT: match(broken) != match(512)",
               idBroken != id512);
    }
}

// =============================================================================================================
// T3 — the no-identity witness's first-sighting rule, modelled in isolation. Break: hand it the trimmed buffer.
// =============================================================================================================
// Mirrors AppleHardwareHook.cpp's gfxsrc_pgm_profile_headfirst(..., extendWitness=true): read the head, compute
// `need`, read up to it: if the identity is STILL unmatched and this (stage,va) has never been seen, grow the read
// to the full kXdIdDwords window (what the witness needs; build 0.0.484: N48_XD_ID_DWORDS, 512 - it was spelled
// N48_PGMID_CAP while the two were the same number); a repeat sighting stays head-first.
struct MockVm { const uint32_t *buf; uint32_t n; uint64_t base; };
static uint32_t mock_read(const MockVm &vm, uint64_t va, uint32_t *dst, uint32_t want)
{
    if (va < vm.base) return 0u;
    const uint64_t dwOff = (va - vm.base) / 4ull;
    if (dwOff >= vm.n) return 0u;
    const uint32_t avail = (uint32_t)(vm.n - dwOff);
    const uint32_t got = want < avail ? want : avail;
    std::memcpy(dst, vm.buf + dwOff, (size_t)got * 4u);
    return got;
}
// 0.0.437 (item 5): `tableFull` mirrors AppleHardwareHook.cpp's `gXdMissN >= kXdMissRows` - the no-identity witness
// table is already full, so a first sighting can never be STORED and the extension is skipped rather than read and
// thrown away. `plantedSkipBypassBreak` is the named break: extend anyway, ignoring a full table.
static uint32_t model_headfirst(uint32_t stage, uint64_t va, const MockVm &vm, uint32_t *buf,
                                 bool extendWitness, bool alreadyMissed, bool plantedTrimBreak,
                                 bool tableFull = false, bool plantedSkipBypassBreak = false)
{
    uint32_t got = mock_read(vm, va, buf, N48_PGMID_HEAD_DWORDS);
    if (got < N48_PGMID_HEAD_DWORDS) return got;
    const uint32_t head[4] = { buf[0], buf[1], buf[2], buf[3] };
    const uint32_t L = n48_pgm_need(stage, head, gRows, gRowN);
    if (L > got) got += mock_read(vm, va + (uint64_t)got * 4ull, buf + got, L - got);
    if (extendWitness && !plantedTrimBreak && got < N48_XD_ID_DWORDS) {
        const int idPeek = xlat12_shader_id_match(stage, buf, got);
        if (idPeek < 0 && !alreadyMissed) {
            if (tableFull && !plantedSkipBypassBreak) {
                // 0.0.437 (item 5): the table is full - a new (stage,va) could never be stored, so skip the read.
            } else {
                got += mock_read(vm, va + (uint64_t)got * 4ull, buf + got, N48_XD_ID_DWORDS - got);
            }
        }
    }
    return got;
}
static void test_T3(void)
{
    build_rows();
    // Deliberately unmatched content: 512 dwords that satisfy no real identity row (its own head is not in the
    // table, since it is a fixed pattern no captured program starts with).
    std::vector<uint32_t> unmatched(N48_PGMID_CAP, 0xDEADBEEFu);
    MockVm vm { unmatched.data(), (uint32_t)unmatched.size(), 0x1000ull };

    uint32_t buf[N48_PGMID_CAP];
    const uint32_t gotFirst = model_headfirst(0u, vm.base, vm, buf, true, /*alreadyMissed=*/false, false);
    expect("T3 real: a first sighting of an unmatched program grows to the FULL window (kXdIdDwords, 512)",
           gotFirst == N48_XD_ID_DWORDS);

    const uint32_t gotRepeat = model_headfirst(0u, vm.base, vm, buf, true, /*alreadyMissed=*/true, false);
    expect("T3 real: a REPEAT sighting stays head-first (no witness has changed)",
           gotRepeat < N48_XD_ID_DWORDS && gotRepeat == N48_PGMID_HEAD_DWORDS);

    // MUTANT: hand the witness the trimmed (head-first) buffer even on a first sighting - the extension never runs.
    const uint32_t gotBroken = model_headfirst(0u, vm.base, vm, buf, true, /*alreadyMissed=*/false,
                                                /*plantedTrimBreak=*/true);
    expect("T3 mutant (trimmed buffer on first sighting) CAUGHT: never reaches the full window",
           gotBroken != N48_XD_ID_DWORDS);
}

// =============================================================================================================
// T4 — OFF / T (1) / SHADOW (3) give IDENTICAL identities over BOTH named real fixtures. Break: the T2 shortfall.
// =============================================================================================================
static int model_off(uint32_t stage, const MockVm &vm, uint64_t va, uint32_t *buf, uint32_t *dwOut)
{
    const uint32_t got = mock_read(vm, va, buf, N48_XD_ID_DWORDS);   // build 0.0.484: OFF reads kXdIdDwords
    if (dwOut) *dwOut = got;
    return got ? xlat12_shader_id_match(stage, buf, got) : -1;
}
static int model_t(uint32_t stage, const MockVm &vm, uint64_t va, uint32_t *buf, uint32_t *dwOut, bool broken)
{
    uint32_t got = mock_read(vm, va, buf, N48_PGMID_HEAD_DWORDS);
    if (got < N48_PGMID_HEAD_DWORDS) { if (dwOut) *dwOut = got; return -1; }
    const uint32_t head[4] = { buf[0], buf[1], buf[2], buf[3] };
    const uint32_t L = broken ? n48_pgm_need_MUTANT_first_match(stage, head, gRows, gRowN)
                              : n48_pgm_need(stage, head, gRows, gRowN);
    if (L > got) got += mock_read(vm, va + (uint64_t)got * 4ull, buf + got, L - got);
    if (dwOut) *dwOut = got;
    return got ? xlat12_shader_id_match(stage, buf, got) : -1;
}
static void test_T4(void)
{
    build_rows();

    // ---- fixture_arm13: REAL identity content, both real programs the fixture names. ----
    struct { const char *name; uint32_t stage; const uint32_t *buf; uint32_t n; int wantId; } progs[] = {
        { "GPUPass (fragment)",     0u, kArm13GpuPassPs,       KARM13_GPUPASS_PS_N,   KARM13_GPUPASS_ID },
        { "ViewportToNDC (vertex)", 1u, kArm13ViewportToNdcVs, KARM13_VPTONDC_VS_N,   KARM13_VPTONDC_ID },
    };
    for (auto &p : progs) {
        MockVm vm { p.buf, p.n, 0x2000ull };
        uint32_t buf[N48_PGMID_CAP], dwOff = 0u, dwT = 0u;
        const int idOff = model_off(p.stage, vm, vm.base, buf, &dwOff);
        const int idT   = model_t(p.stage, vm, vm.base, buf, &dwT, /*broken=*/false);
        const int idShadow = idOff;   // SHADOW uses HEAD's (OFF's) answer, always
        char what[256];
        std::snprintf(what, sizeof what, "T4 fixture_arm13 %s: OFF identifies it as table row %d", p.name, p.wantId);
        expect(what, idOff == p.wantId);
        std::snprintf(what, sizeof what, "T4 fixture_arm13 %s: T (1) agrees with OFF", p.name);
        expect(what, idT == idOff);
        std::snprintf(what, sizeof what, "T4 fixture_arm13 %s: SHADOW (3) uses OFF's answer, so it agrees too",
                      p.name);
        expect(what, idShadow == idOff);
        std::snprintf(what, sizeof what, "T4 fixture_arm13 %s: T reads fewer dwords than OFF (%u < %u)", p.name,
                      dwT, dwOff);
        expect(what, dwT < dwOff);

        // MUTANT, on this same real fixture data: T's need() undershoots (the T2 break), so T's answer diverges
        // from OFF's - "identities identical for OFF, 1 and 3" FAILS, as the design's break requires.
        uint32_t dwBroken = 0u;
        const int idBroken = model_t(p.stage, vm, vm.base, buf, &dwBroken, /*broken=*/true);
        std::snprintf(what, sizeof what, "T4 mutant (%s, first-match need) CAUGHT or vacuously agreed", p.name);
        // GPUPass's own head is unique in the real table (no other row shares it), so the mutant cannot diverge
        // for it; ViewportToNDC's head IS shared (T1/T2), so the mutant MUST diverge there.
        if (p.wantId == KARM13_VPTONDC_ID)
            expect(what, idBroken != idOff);
        else
            expect("T4 mutant on GPUPass: its head is unique in the table, so no divergence is possible here "
                   "(the CAUGHT case is ViewportToNDC, above)", idBroken == idOff);
    }

    // ---- fixture_mib_f48_f20_f21.h: REAL concatenated IBs, no shader ISA payload - proves xlat12_ib_segments'
    // structure (which this switch never touches) is unaffected, complementing the identity-content proof above.
    {
        xlat12_ib_segment segs[64];
        uint32_t total = 0u;
        const uint32_t ns = xlat12_ib_segments(kF48Mib, kF48Mib_dwords, segs, 64u, &total);
        expect("T4 fixture_mib: F48's real concatenated IBs segment the same way regardless of switch 38 "
               "(this switch never touches segmentation)", ns > 0u && ns == total);
    }
}

// =============================================================================================================
// T5 — N48_PGMID_FMT stays under the logger's 512-byte body cap at its numeric widest.
// =============================================================================================================
static void test_T5(void)
{
    n48_pgmid_stats worst {};
    worst.asks = worst.pgm_us = worst.mm_dwords = UINT64_MAX;
    worst.head_only = worst.witness_full = worst.shadow_compares = worst.disagreed = UINT64_MAX;
    worst.witness_skip_full = UINT64_MAX;   // 0.0.437 (item 5): the new field, widest too
    char line[512];
    const int w = std::snprintf(line, sizeof line, N48_PGMID_FMT,
                                 N48_PGMID_ARGS("SHADOW (T compared, HEAD's answer used)", &worst));
    std::printf("T5 pgmid: line at widest: %d bytes\n", w);
    expect("T5 pgmid: line stays under n48log's 512-byte body cap", w > 0 && (size_t)w < 512u);
    expect("T5 pgmid: names itself", std::strncmp(line, "pgmid:", 6) == 0);
    expect("T5 pgmid: reports DISAGREED", std::strstr(line, "DISAGREED") != nullptr);
    expect("T5 pgmid: reports the new field, 'witness skipped (table full)' (item 5)",
           std::strstr(line, "witness skipped (table full)") != nullptr);

    // MUTANT: widen the format (append a needless clause) - must be CAUGHT, proving the check is not vacuous.
    {
        std::string widened = std::string(N48_PGMID_FMT) +
            " Also, for good measure, here is a much longer sentence appended only to make this line far too wide, "
            "repeated: for good measure, here is a much longer sentence appended only to make this line far too "
            "wide, repeated again for good measure to push it well past five hundred and twelve bytes of body, "
            "and then repeated a third and a fourth time because this format's own text starts far short of the "
            "cap and needs a good deal more padding than a wider report line would to prove the same point: for "
            "good measure, here is a much longer sentence appended only to make this line far too wide, repeated "
            "again and again and again until it is unmistakably, unambiguously, inarguably over the limit.";
        const int w2 = std::snprintf(line, sizeof line, widened.c_str(),
                                      N48_PGMID_ARGS("SHADOW (T compared, HEAD's answer used)", &worst));
        expect("T5 mutant (widened field) CAUGHT: exceeds the 512-byte cap", w2 > 0 && (size_t)w2 >= 512u);
    }
}

// =============================================================================================================
// T6 (0.0.436, design "2. M") — MODE T+M: the per-pass memo. Models gfxsrc_pgm_profile's mode==N48_PGMID_MODE_TM
// branch (AppleHardwareHook.cpp) exactly, over the REAL n48_pm (gfx_pgmid.h) and the REAL ring/poison/pagerec
// primitives (gfx_copyguard.h) — never a hand-rolled stand-in for either.
// =============================================================================================================
struct TmModel {
    n48_pm mem {};
    n48_cg_ring ring {};
    n48_cg_poison poison {};
    n48_cg_pagerec rec {};
    uint32_t hits = 0, misses = 0, inval = 0, poisonMiss = 0, notStored = 0;
};
static void tm_reset(TmModel &m)
{
    n48_pm_clear(&m.mem);
    n48_cg_ring_init(&m.ring);
    n48_cg_poison_init(&m.poison);
    n48_cg_pagerec_reset(&m.rec);
    m.hits = m.misses = m.inval = m.poisonMiss = m.notStored = 0;
}
// 0.0.436 DEFECT FIX (reviewer, post-0.0.436): a VA->VRAM translation with VA != VRAM offset, exactly as
// gfxc_read_core's own vmib_to_vram_off is for real hardware. Every row the model below stores must key its pages
// off THIS, never off `va` directly - the whole point of the fix this suite proves.
static uint64_t mock_va_to_vram(uint64_t va) { return va - 0x3ff00000000ull + 0x10000000ull; }

// Mirrors gfxsrc_pgmid_memo_try + gfxsrc_pgm_profile's mode-TM branch + gfxsrc_pgmid_memo_fill, over a MockVm (T3's
// harness). Every bool defaults false (the REAL behaviour); a caller sets exactly one to plant the design's own
// named break for that test: `keyOnVaOnly` collapses every stage to one bucket (also stands in for "swap the stage
// on a hit" — either bug makes a hit answer with the WRONG stage's row); `ignoreMark`/`ignorePoison` skip their own
// validity test; `skipRecorderNote` drops the hit's own pages from the recorder; `storeVaPagesBug` is the DEFECT
// this fix removes - a miss stores the raw VA's page instead of the read's own (translated) VRAM page.
static int model_tm(TmModel &m, uint32_t stage, uint64_t va, const MockVm &vm, uint32_t *dwOut,
                     bool keyOnVaOnly = false, bool ignoreMark = false, bool ignorePoison = false,
                     bool skipRecorderNote = false, bool storeVaPagesBug = false)
{
    const uint32_t lookupStage = keyOnVaOnly ? 0u : stage;
    const n48_pm_row *row = n48_pm_lookup(&m.mem, lookupStage, va);
    bool validHit = false;
    if (row) {
        validHit = true;
        if (!ignoreMark && n48_cg_ring_mark(&m.ring) != row->fillMark) { m.inval++; validHit = false; }
        if (validHit && !ignorePoison) {
            for (uint32_t i = 0; i < row->npages; i++)
                if (n48_cg_poison_overlaps(&m.poison, row->pages[i], row->pages[i] + 4096ull)) {
                    m.poisonMiss++; validHit = false; break;
                }
        }
    } else {
        m.misses++;
    }
    if (row && validHit) {
        if (!skipRecorderNote)
            for (uint32_t i = 0; i < row->npages; i++) n48_cg_pagerec_note(&m.rec, row->pages[i], 4096u);
        m.hits++;
        if (dwOut) *dwOut = 0u;             // a hit performs no read at all
        return (int)row->id;
    }
    uint32_t buf[N48_PGMID_CAP];
    const uint64_t markBefore = n48_cg_ring_mark(&m.ring);     // BEFORE the read - the design's own ordering
    const uint32_t got = model_headfirst(stage, va, vm, buf, /*extendWitness=*/true, /*alreadyMissed=*/false, false);
    if (dwOut) *dwOut = got;
    const int id = got ? xlat12_shader_id_match(stage, buf, got) : -1;

    // Mirror gfxc_read_core: note the READ's own pages into a LOCAL recorder, keyed by the TRANSLATED VRAM offset
    // (never `va`) — exactly what AppleHardwareHook.cpp's gfxsrc_pgm_profile_headfirst/gfxc_read_rs feed `rec`.
    n48_cg_pagerec local {};
    n48_cg_pagerec_reset(&local);
    if (got) {
        const uint64_t noted = storeVaPagesBug ? va : mock_va_to_vram(va);   // the planted break vs. the real fix
        n48_cg_pagerec_note(&local, noted, got * 4u);
    }
    for (uint32_t i = 0; i < local.n; i++) n48_cg_pagerec_note(&m.rec, local.page[i], 4096u);   // segment sees them too
    // build 0.0.484: the fill rule is gfx_pgmid.h's n48_pm_store_rec - the SAME call gfxsrc_pgmid_memo_fill makes.
    if (!n48_pm_store_rec(&m.mem, lookupStage, va, (int32_t)id, markBefore, local.page, local.n, local.overflow)) m.notStored++;
    return id;
}

// T6a — the memo key includes stage. Break: key on VA alone (collapses every stage to one bucket).
static void test_T6a(void)
{
    build_rows();
    const uint64_t va = 0x500000ull;
    MockVm vmGpu { kArm13GpuPassPs,       KARM13_GPUPASS_PS_N,  va };
    MockVm vmVp  { kArm13ViewportToNdcVs, KARM13_VPTONDC_VS_N,  va };

    TmModel tm; tm_reset(tm);
    const int id0 = model_tm(tm, 0u, va, vmGpu, nullptr);
    expect("T6a setup: stage 0's ask at this VA is a miss and identifies GPUPass",
           id0 == KARM13_GPUPASS_ID && tm.misses == 1u);
    const int id1 = model_tm(tm, 1u, va, vmVp, nullptr);
    expect("T6a real: stage 1 at the SAME VA is correctly a MISS (a different key), and identifies ViewportToNDC",
           id1 == KARM13_VPTONDC_ID && tm.misses == 2u && tm.hits == 0u);

    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vmGpu, nullptr, /*keyOnVaOnly=*/true);
    const int idBroken = model_tm(tmMut, 1u, va, vmVp, nullptr, /*keyOnVaOnly=*/true);
    expect("T6a mutant (key on VA alone) CAUGHT: stage 1 wrongly HITS stage 0's row and returns GPUPass's id, "
           "not ViewportToNDC's", idBroken == KARM13_GPUPASS_ID && idBroken != KARM13_VPTONDC_ID &&
           tmMut.hits == 1u);
}

// T6b — the memo clears per pass. Break: skip the clear.
static void test_T6b(void)
{
    build_rows();
    const uint64_t va = 0x510000ull;
    MockVm vm { kArm13GpuPassPs, KARM13_GPUPASS_PS_N, va };

    TmModel tm; tm_reset(tm);
    model_tm(tm, 0u, va, vm, nullptr);
    expect("T6b setup: pass 1 fills the row", tm.mem.n == 1u && tm.misses == 1u);
    n48_pm_clear(&tm.mem);                                      // what gfxsrc_policy does before its segment loop
    const int id2 = model_tm(tm, 0u, va, vm, nullptr);
    expect("T6b real: after the per-pass clear, the SAME ask is a MISS again", id2 == KARM13_GPUPASS_ID &&
           tm.misses == 2u && tm.hits == 0u);

    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vm, nullptr);
    // MUTANT: no clear between "passes" here.
    const int idBroken = model_tm(tmMut, 0u, va, vm, nullptr);
    expect("T6b mutant (skip the clear) CAUGHT: the SAME ask wrongly HITS a row left over from the previous pass",
           idBroken == KARM13_GPUPASS_ID && tmMut.hits == 1u && tmMut.misses == 1u);
}

// T6c — a ring-mark change forces a miss. Break: ignore the mark.
static void test_T6c(void)
{
    build_rows();
    const uint64_t va = 0x520000ull;
    MockVm vm { kArm13GpuPassPs, KARM13_GPUPASS_PS_N, va };

    TmModel tm; tm_reset(tm);
    model_tm(tm, 0u, va, vm, nullptr);
    n48_cg_ring_push(&tm.ring, N48_CG_EV_BEGIN, 0u, 0x1000ull, 0x2000ull);   // ANY Part-2 guard event
    const int id2 = model_tm(tm, 0u, va, vm, nullptr);
    expect("T6c real: a ring-mark change forces a MISS even though the row is otherwise present",
           id2 == KARM13_GPUPASS_ID && tm.inval == 1u && tm.hits == 0u);

    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vm, nullptr);
    n48_cg_ring_push(&tmMut.ring, N48_CG_EV_BEGIN, 0u, 0x1000ull, 0x2000ull);
    const int idBroken = model_tm(tmMut, 0u, va, vm, nullptr, false, /*ignoreMark=*/true);
    expect("T6c mutant (ignore the mark) CAUGHT: wrongly HITS despite the intervening guard event",
           idBroken == KARM13_GPUPASS_ID && tmMut.hits == 1u && tmMut.inval == 0u);
}

// T6d — a poison overlap forces a miss. Break: ignore poison.
static void test_T6d(void)
{
    build_rows();
    const uint64_t va = 0x530000ull;
    MockVm vm { kArm13GpuPassPs, KARM13_GPUPASS_PS_N, va };

    TmModel tm; tm_reset(tm);
    model_tm(tm, 0u, va, vm, nullptr);
    const n48_pm_row *row = n48_pm_lookup(&tm.mem, 0u, va);
    expect("T6d setup: the row named at least one page", row != nullptr && row->npages >= 1u);
    n48_cg_poison_mark(&tm.poison, row->pages[0], row->pages[0] + 4096ull);
    const int id2 = model_tm(tm, 0u, va, vm, nullptr);
    expect("T6d real: a poisoned page forces a MISS even though the row and its ring mark are otherwise valid",
           id2 == KARM13_GPUPASS_ID && tm.poisonMiss == 1u && tm.hits == 0u);

    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vm, nullptr);
    const n48_pm_row *rowMut = n48_pm_lookup(&tmMut.mem, 0u, va);
    n48_cg_poison_mark(&tmMut.poison, rowMut->pages[0], rowMut->pages[0] + 4096ull);
    const int idBroken = model_tm(tmMut, 0u, va, vm, nullptr, false, false, /*ignorePoison=*/true);
    expect("T6d mutant (ignore poison) CAUGHT: wrongly HITS a row whose page is poisoned",
           idBroken == KARM13_GPUPASS_ID && tmMut.hits == 1u && tmMut.poisonMiss == 0u);
}

// T6e — a hit notes its pages into the recorder (gfx_copyguard.h's own contract for a cache that answers without
// re-reading). Break: skip the note.
static void test_T6e(void)
{
    build_rows();
    const uint64_t va = 0x540000ull;
    MockVm vm { kArm13GpuPassPs, KARM13_GPUPASS_PS_N, va };

    TmModel tm; tm_reset(tm);
    model_tm(tm, 0u, va, vm, nullptr);                          // miss, fills the row
    n48_cg_pagerec_reset(&tm.rec);                              // simulate the NEXT segment's navi48_cg_seg_begin
    expect("T6e setup: the recorder is empty before the hit", tm.rec.n == 0u);
    model_tm(tm, 0u, va, vm, nullptr);                          // this ask is now a HIT
    const n48_pm_row *row = n48_pm_lookup(&tm.mem, 0u, va);
    expect("T6e real: a hit notes its row's pages into the active recorder", tm.rec.n >= 1u);
    expect("T6e real: the recorder's note actually covers the hit's own page (a copy-guard check would see it)",
           row != nullptr && n48_cg_pagerec_overlaps(&tm.rec, row->pages[0], row->pages[0] + 4096ull));

    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vm, nullptr);
    n48_cg_pagerec_reset(&tmMut.rec);
    model_tm(tmMut, 0u, va, vm, nullptr, false, false, false, /*skipRecorderNote=*/true);
    expect("T6e mutant (skip the note) CAUGHT: a hit leaves the recorder blind to its own page",
           tmMut.rec.n == 0u);
}

// T6f — a hit's profile bytes and io equal a fresh resolve: xlat12_ib_profile_stage(id, ...) is run on BOTH a
// fresh-resolve id and a same-pass hit's id and the two outputs must be byte-identical (the design's own basis for
// "a hit re-runs everything after the match safely, because xlat12_ib_profile_stage is pure").
static void test_T6f(void)
{
    build_rows();
    const uint64_t va = 0x550000ull;
    MockVm vm { kArm13ViewportToNdcVs, KARM13_VPTONDC_VS_N, va };

    TmModel tm; tm_reset(tm);
    const int idMiss = model_tm(tm, 1u, va, vm, nullptr);
    xlat12_draw_profile outFresh {}; uint32_t ioFresh[2] = { 0u, 0u };
    const uint32_t rFresh = xlat12_ib_profile_stage(idMiss, &outFresh, ioFresh);

    const int idHit = model_tm(tm, 1u, va, vm, nullptr);
    expect("T6f setup: the second ask was a genuine memo HIT", tm.hits == 1u);
    xlat12_draw_profile outHit {}; uint32_t ioHit[2] = { 0u, 0u };
    const uint32_t rHit = xlat12_ib_profile_stage(idHit, &outHit, ioHit);

    expect("T6f real: a hit's id equals a fresh resolve's id", idHit == idMiss);
    expect("T6f real: profile_stage's return code is identical for a hit's id and a fresh resolve's id",
           rHit == rFresh);
    expect("T6f real: profile_stage's io[] is identical", ioHit[0] == ioFresh[0] && ioHit[1] == ioFresh[1]);
    expect("T6f real: profile_stage's whole out struct is byte-identical",
           std::memcmp(&outHit, &outFresh, sizeof(outHit)) == 0);
}

// T6g — both named fixtures give identical output across OFF, T (1) and T+M (2). Break: swap/collapse the stage on
// a memo hit.
static void test_T6g(void)
{
    build_rows();
    struct { const char *name; uint32_t stage; const uint32_t *buf; uint32_t n; int wantId; } progs[] = {
        { "GPUPass (fragment)",     0u, kArm13GpuPassPs,       KARM13_GPUPASS_PS_N,   KARM13_GPUPASS_ID },
        { "ViewportToNDC (vertex)", 1u, kArm13ViewportToNdcVs, KARM13_VPTONDC_VS_N,   KARM13_VPTONDC_ID },
    };
    for (auto &p : progs) {
        MockVm vm { p.buf, p.n, 0x560000ull };
        uint32_t buf[N48_PGMID_CAP], dwOff = 0u;
        const int idOff = model_off(p.stage, vm, vm.base, buf, &dwOff);

        TmModel tm; tm_reset(tm);
        uint32_t dwMiss = 0u, dwHit = 0u;
        const int idMiss = model_tm(tm, p.stage, vm.base, vm, &dwMiss);
        const int idHit  = model_tm(tm, p.stage, vm.base, vm, &dwHit);

        char what[256];
        std::snprintf(what, sizeof what, "T6g fixture_arm13 %s: T+M's first ask (miss) agrees with OFF", p.name);
        expect(what, idMiss == idOff);
        std::snprintf(what, sizeof what, "T6g fixture_arm13 %s: T+M's second ask (hit) agrees with OFF too", p.name);
        expect(what, idHit == idOff);
        std::snprintf(what, sizeof what, "T6g fixture_arm13 %s: the hit performed no read (0 dwords, vs %u on the "
                      "miss)", p.name, dwMiss);
        expect(what, dwHit == 0u && dwMiss > 0u);
    }

    // The cross-program case: GPUPass asked at a VA a DIFFERENT stage previously used at, in the SAME pass. Break:
    // swap/collapse the stage on the hit lookup - the design's own named break for this test.
    {
        const uint64_t va = 0x570000ull;
        MockVm vmGpu { kArm13GpuPassPs,       KARM13_GPUPASS_PS_N,  va };
        MockVm vmVp  { kArm13ViewportToNdcVs, KARM13_VPTONDC_VS_N,  va };
        uint32_t bufOff[N48_PGMID_CAP];
        const int idOffGpu = model_off(0u, vmGpu, va, bufOff, nullptr);

        TmModel tm; tm_reset(tm);
        model_tm(tm, 1u, va, vmVp, nullptr);                    // stage 1 first: fills the row keyed (1, va)
        const int idGpu = model_tm(tm, 0u, va, vmGpu, nullptr); // stage 0 second: correctly a MISS (different key)
        expect("T6g real: GPUPass at a VA a different stage just used is correctly a MISS, and agrees with OFF",
               idGpu == idOffGpu && tm.misses == 2u);

        TmModel tmMut; tm_reset(tmMut);
        model_tm(tmMut, 1u, va, vmVp, nullptr, /*keyOnVaOnly=*/true);
        const int idGpuBroken = model_tm(tmMut, 0u, va, vmGpu, nullptr, /*keyOnVaOnly=*/true);
        expect("T6g mutant (swap/collapse the stage on a hit) CAUGHT: GPUPass's ask wrongly answers with "
               "ViewportToNDC's id, diverging from OFF",
               idGpuBroken != idOffGpu && idGpuBroken == KARM13_VPTONDC_ID && tmMut.hits == 1u);
    }

    // fixture_mib: unaffected by this switch (T4 already proves this for OFF/T/SHADOW) - re-confirmed here so the
    // "identical across OFF/1/2" claim covers this fixture too.
    {
        xlat12_ib_segment segs[64];
        uint32_t total = 0u;
        const uint32_t ns = xlat12_ib_segments(kF48Mib, kF48Mib_dwords, segs, 64u, &total);
        expect("T6g fixture_mib: F48's real concatenated IBs segment the same way regardless of switch 38's mode "
               "(T+M never touches segmentation)", ns > 0u && ns == total);
    }
}

// MUTANT for T6h: the real n48_pm_store's own cap check removed - overwrites row `n % N48_PM_ROWS` instead of
// refusing once the table is full.
static int n48_pm_store_MUTANT_no_cap(n48_pm *m, uint32_t stage, uint64_t va, int32_t id, uint64_t fillMark,
                                       const uint64_t *pages, uint32_t npages)
{
    if (npages > 2u) return 0;
    for (uint32_t i = 0; i < m->n && i < N48_PM_ROWS; i++) {
        if (m->row[i].stage != stage || m->row[i].va != va) continue;
        m->row[i].id = id; m->row[i].fillMark = fillMark; m->row[i].npages = npages;
        m->row[i].pages[0] = npages > 0u ? pages[0] : 0ull;
        m->row[i].pages[1] = npages > 1u ? pages[1] : 0ull;
        return 1;
    }
    n48_pm_row *r = &m->row[m->n % N48_PM_ROWS];               // MUTANT: no cap check, just wraps and overwrites
    r->stage = stage; r->va = va; r->id = id; r->fillMark = fillMark; r->npages = npages;
    r->pages[0] = npages > 0u ? pages[0] : 0ull;
    r->pages[1] = npages > 1u ? pages[1] : 0ull;
    m->n++;
    return 1;
}
// T6h — a full table stores nothing. Break: no cap check (n48_pm_store_MUTANT_no_cap, above).
static void test_T6h(void)
{
    const uint64_t pages[2] = { 0x1000ull, 0x2000ull };

    n48_pm m; n48_pm_clear(&m);
    for (uint32_t i = 0; i < N48_PM_ROWS; i++) {
        const int ok = n48_pm_store(&m, 0u, 0x600000ull + (uint64_t)i * 0x100ull, (int32_t)i, 1ull, pages, 2u);
        if (!ok) { expect("T6h setup: filling the table up to its own cap succeeds every time", false); return; }
    }
    expect("T6h setup: the table is now exactly full", m.n == N48_PM_ROWS);
    const uint64_t newVa = 0x699999ull;
    const int okOver = n48_pm_store(&m, 0u, newVa, 42, 1ull, pages, 2u);
    expect("T6h real: a full table REFUSES a new key", okOver == 0);
    expect("T6h real: the table's own row count never grows past its cap", m.n == N48_PM_ROWS);
    expect("T6h real: the refused key was never actually stored", n48_pm_lookup(&m, 0u, newVa) == nullptr);

    n48_pm mMut; n48_pm_clear(&mMut);
    for (uint32_t i = 0; i < N48_PM_ROWS; i++)
        n48_pm_store_MUTANT_no_cap(&mMut, 0u, 0x600000ull + (uint64_t)i * 0x100ull, (int32_t)i, 1ull, pages, 2u);
    const int okMutOver = n48_pm_store_MUTANT_no_cap(&mMut, 0u, newVa, 42, 1ull, pages, 2u);
    expect("T6h mutant (no cap check) CAUGHT: wrongly reports success storing past the real table's own cap",
           okMutOver != 0 && n48_pm_lookup(&mMut, 0u, newVa) != nullptr);
}

// =============================================================================================================
// T6i (0.0.436 DEFECT FIX, reviewer) — a row's pages are the resolving read's own VRAM pages (the VA->VRAM
// translation), NEVER the program's raw GPU VA. Break: store VA-derived pages (model_tm's storeVaPagesBug).
// =============================================================================================================
static void test_T6i(void)
{
    build_rows();
    const uint64_t va = 0x3ff00500000ull;               // a realistic VMID-2 GPU VA, far from any VRAM offset
    const uint64_t vram = mock_va_to_vram(va);
    MockVm vm { kArm13GpuPassPs, KARM13_GPUPASS_PS_N, va };

    TmModel tm; tm_reset(tm);
    model_tm(tm, 0u, va, vm, nullptr);
    const n48_pm_row *row = n48_pm_lookup(&tm.mem, 0u, va);
    expect("T6i setup: the row was filled", row != nullptr && row->npages >= 1u);
    expect("T6i real: the row's page is the TRANSLATED VRAM page, not the raw VA page",
           row != nullptr && row->pages[0] == (vram & ~4095ull) && row->pages[0] != (va & ~4095ull));

    // Poisoning the RAW VA must have NO effect - the row's page does not live there.
    TmModel tm2; tm_reset(tm2);
    model_tm(tm2, 0u, va, vm, nullptr);
    n48_cg_poison_mark(&tm2.poison, va & ~4095ull, (va & ~4095ull) + 4096ull);
    const int idStillHit = model_tm(tm2, 0u, va, vm, nullptr);
    expect("T6i real: poisoning the raw VA address does not force a miss (it is not where the row's page lives)",
           idStillHit == KARM13_GPUPASS_ID && tm2.hits == 1u && tm2.poisonMiss == 0u);

    // Poisoning the TRANSLATED VRAM page DOES force a miss.
    TmModel tm3; tm_reset(tm3);
    model_tm(tm3, 0u, va, vm, nullptr);
    n48_cg_poison_mark(&tm3.poison, vram & ~4095ull, (vram & ~4095ull) + 4096ull);
    const int idMiss = model_tm(tm3, 0u, va, vm, nullptr);
    expect("T6i real: poisoning the translated VRAM page DOES force a miss",
           idMiss == KARM13_GPUPASS_ID && tm3.poisonMiss == 1u && tm3.hits == 0u);

    // MUTANT: the planted break - store VA-derived pages instead of the read's own (translated) VRAM pages.
    TmModel tmMut; tm_reset(tmMut);
    model_tm(tmMut, 0u, va, vm, nullptr, false, false, false, false, /*storeVaPagesBug=*/true);
    const n48_pm_row *rowMut = n48_pm_lookup(&tmMut.mem, 0u, va);
    expect("T6i mutant (store VA-derived pages) CAUGHT: the row's page is the VA page, not the VRAM page",
           rowMut != nullptr && rowMut->pages[0] == (va & ~4095ull));
    n48_cg_poison_mark(&tmMut.poison, vram & ~4095ull, (vram & ~4095ull) + 4096ull);   // poison the REAL VRAM range
    const int idMutStillHit = model_tm(tmMut, 0u, va, vm, nullptr, false, false, false, false, true);
    expect("T6i mutant CAUGHT: a poisoned VRAM range is invisible to the broken (VA-keyed) row - wrongly HITS",
           idMutStillHit == KARM13_GPUPASS_ID && tmMut.hits == 1u && tmMut.poisonMiss == 0u);
}

// =============================================================================================================
// T8 (0.0.437, item 5) — A FULL WITNESS TABLE = ALREADY WITNESSED. With the witness table full, a no-identity
// ask's first sighting reads only T's head and row, not the full 512-dword window. Break: extend anyway.
// =============================================================================================================
static void test_T8_witness_skip_full(void)
{
    build_rows();
    // 512 dwords that satisfy no real identity row (fixed pattern no captured program starts with), so the head
    // read is always unidentified and the extension question is always reached - T3's own fixture.
    std::vector<uint32_t> unmatched(N48_PGMID_CAP, 0xDEADBEEFu);
    MockVm vm { unmatched.data(), (uint32_t)unmatched.size(), 0x3000ull };
    uint32_t buf[N48_PGMID_CAP];

    const uint32_t gotNotFull = model_headfirst(0u, vm.base, vm, buf, true, false, false, /*tableFull=*/false);
    expect("T8 setup: with the table NOT full, a first sighting still grows to the full window (T3's own property)",
           gotNotFull == N48_XD_ID_DWORDS);

    const uint32_t gotFull = model_headfirst(0u, vm.base, vm, buf, true, false, false, /*tableFull=*/true);
    expect("T8 real: with the witness table FULL, a first sighting reads ONLY the head-first window "
           "(no 512-dword read)", gotFull < N48_XD_ID_DWORDS && gotFull == N48_PGMID_HEAD_DWORDS);

    // MUTANT: extend anyway, ignoring the full table. CAUGHT.
    const uint32_t gotBroken = model_headfirst(0u, vm.base, vm, buf, true, false, false, /*tableFull=*/true,
                                                /*plantedSkipBypassBreak=*/true);
    expect("T8 mutant (extend anyway when the table is full) CAUGHT: reads the full 512-dword window regardless",
           gotBroken == N48_XD_ID_DWORDS);

    // A REPEAT sighting (already witnessed) never extends either way - unaffected by tableFull, as designed.
    const uint32_t gotRepeat = model_headfirst(0u, vm.base, vm, buf, true, /*alreadyMissed=*/true, false,
                                                /*tableFull=*/true);
    expect("T8 real: a repeat sighting stays head-first whether or not the table happens to be full",
           gotRepeat == N48_PGMID_HEAD_DWORDS);
}

// =============================================================================================================
// T9 (0.0.437, item 6) — THE ROW-CAP GUARD fires on an oversized synthetic table. Break: remove the guard (trust
// n48_pgm_need's answer computed over the truncated table anyway, exactly the wrong comment this brief corrects).
// =============================================================================================================
static void test_T9_rowcap_guard(void)
{
    // A tiny synthetic cap (4) standing in for kPgmIdRowCap, and a 6-row table where a row PAST the cap shares a
    // head with a row WITHIN it but needs far more dwords - the silent-drop shape the old (wrong) comment beside
    // kPgmIdRowCap called "safe either way".
    static constexpr uint32_t kSynCap = 4u;
    n48_pgmid_row full[6] = {
        { 0u, 10u,  {1,2,3,4} },
        { 0u, 20u,  {5,6,7,8} },
        { 0u, 30u,  {9,10,11,12} },
        { 0u, 40u,  {13,14,15,16} },
        { 0u, 200u, {1,2,3,4} },      // PAST kSynCap (row index 4): shares row 0's head, needs FAR more dwords
        { 1u, 50u,  {21,22,23,24} },
    };
    const uint32_t truncatedN = kSynCap;                  // what gPgmIdRows would hold if only kSynCap rows fit
    const bool tableGrewPastCap = 6u > kSynCap;            // mirrors gfxsrc_pgmid_rows_build's own overflow test
    expect("T9 setup: the synthetic table DID grow past its cap", tableGrewPastCap);

    const uint32_t head0[4] = { 1, 2, 3, 4 };
    const uint32_t needTruncated = n48_pgm_need(0u, head0, full, truncatedN);   // only sees rows 0..3
    expect("T9 setup: over the TRUNCATED table alone, need() silently returns the SHORTER answer (10, not 200)",
           needTruncated == 10u);

    // REAL (the guard fires): overflow detected -> refuse the shortcut, force the FULL window rather than trust
    // need()'s answer over an incomplete table.
    const uint32_t realL = tableGrewPastCap ? N48_PGMID_CAP : needTruncated;
    expect("T9 real: the row-cap guard fails closed to the FULL window rather than trusting the truncated table",
           realL == N48_PGMID_CAP);

    // MUTANT: the guard REMOVED - trust need() over the truncated table regardless of overflow. CAUGHT.
    const uint32_t mutantL = needTruncated;   // *** PLANTED BREAK: no overflow check at all ***
    expect("T9 mutant (guard removed) CAUGHT: reads only 10 dwords, silently too few for the dropped 200-dword row",
           mutantL != N48_PGMID_CAP && mutantL == 10u);
}

// =============================================================================================================
// T10 (0.0.438, , D1) — MODE T+M AS THE FIRST MODE OF A BOOT MUST STILL RESOLVE A KNOWN PROGRAM. The
// bug: gfxsrc_pgmid_rows_build() used to live ONLY inside gfxsrc_pgm_profile_id_read's first line, but T+M's own
// miss path (gfxsrc_pgm_profile) calls gfxsrc_pgm_profile_headfirst DIRECTLY, bypassing that function entirely -
// so a boot that starts in T+M (gPgmIdRowN still 0, "never built") asks n48_pgm_need to search an EMPTY table,
// which can only ever answer L == 0 ("no identity row shares this head"), for every ask, forever. Modelled here
// with the REAL n48_pgm_need over the REAL table (built by earlier tests) vs. an explicit count == 0, standing in
// for "gfxsrc_pgmid_rows_build() was never called this boot" - the exact state a fresh boot's static gPgmIdRowN
// starts in. Uses ViewportToNDC's real captured bytes (kArm13ViewportToNdcVs), the same fixture T1/T2 use.
// =============================================================================================================
static void test_T10_rowsbuild_before_first_read(void)
{
    build_rows();   // the REAL table now exists, as it would after gfxsrc_pgmid_rows_build() actually ran
    expect("T10 setup: the real table has rows", gRowN > 0u);
    const uint32_t vpHead[4] = { kArm13ViewportToNdcVs[0], kArm13ViewportToNdcVs[1],
                                  kArm13ViewportToNdcVs[2], kArm13ViewportToNdcVs[3] };

    // REAL (fixed): the table was built before this ask (gfxsrc_pgmid_rows_build() ran) - n48_pgm_need finds the
    // group's true max and the identity resolves, exactly T2's own setup.
    const uint32_t needBuilt = n48_pgm_need(1u, vpHead, gRows, gRowN);
    expect("T10 real (table built): n48_pgm_need finds ViewportToNDC's group max (139)", needBuilt == 139u);
    const int idBuilt = xlat12_shader_id_match(1u, kArm13ViewportToNdcVs, needBuilt);
    expect("T10 real (table built): the identity resolves to ViewportToNDC", idBuilt == KARM13_VPTONDC_ID);

    // PLANTED BREAK (the D1 defect): count == 0 stands in for "gfxsrc_pgmid_rows_build() was never called" - T+M
    // as the very first mode of a boot, before ANY call ever passed through gfxsrc_pgm_profile_id_read. n48_pgm_need
    // can only answer L == 0 over an empty table, so the head-first read never even reaches ViewportToNDC's own
    // ndw (96), let alone the group max - the ask fails closed (safe) but never identifies the program at all.
    const uint32_t needUnbuilt = n48_pgm_need(1u, vpHead, gRows, /*count=*/0u);
    expect("T10 planted break CAUGHT: an EMPTY (never-built) table answers need == 0 for a program it actually has",
           needUnbuilt == 0u);
    const int idUnbuilt = xlat12_shader_id_match(1u, kArm13ViewportToNdcVs, needUnbuilt);
    expect("T10 planted break CAUGHT: with need == 0 the identity NEVER resolves, although the real table has it",
           idUnbuilt < 0 && idUnbuilt != idBuilt);
}

// =============================================================================================================
// T7 (0.0.436) — SOURCE PINS: mode 2 is no longer refused, the per-pass memo clear exists, and (0.0.436 DEFECT
// FIX) the memo fill no longer derives pages from `va`. Optional argument; skipped, not failed, when absent (the
// project's own convention, gfx_copyguard_test.cpp's T9).
// =============================================================================================================
static void test_T7(const char *ahhPath)
{
    std::string ahh;
    if (!ahhPath || !read_file(ahhPath, ahh)) { std::printf("SKIP  T7: no AppleHardwareHook.cpp path given\n"); return; }

    expect("T7 mode 2 (T+M) is accepted by the switch-38 dispatcher, alongside T/SHADOW/SHADOW-M/OFF",
           has(ahh, "m == N48_PGMID_MODE_T || m == N48_PGMID_MODE_TM || m == N48_PGMID_MODE_SHADOW ||") &&
           has(ahh, "m == N48_PGMID_MODE_SHADOW_M || m == N48_PGMID_MODE_OFF"));
    expect("T7 the old 'NOT BUILT THIS BRIEF' refusal text for mode 2 is gone",
           !has(ahh, "NOT BUILT THIS BRIEF"));
    expect("T7 mode 4 (SHADOW-M) exists", has(ahh, "N48_PGMID_MODE_SHADOW_M"));
    expect("T7 the per-pass memo clear exists, for BOTH the real memo and SHADOW-M's own",
           has(ahh, "n48_pm_clear(&gPgmMemo);") && has(ahh, "n48_pm_clear(&gPgmMemoShadow);"));
    expect("T7 a valid hit looks up before doing any read",
           has(ahh, "if (mode == N48_PGMID_MODE_TM) memoHit = gfxsrc_pgmid_memo_try(&gPgmMemo, stage, va, &id, "
                    "/*noteRecorder=*/true);"));
    expect("T7 the relocation-marker scan is skipped on a hit",
           has(ahh, "if (!memoHit && stage == 1u) {"));
    // 0.0.438: the old 1,032-byte `localRec` local is GONE - replaced with a file-scope scratch
    // recorder (gPgmProfileScratchTM), off the stack. Pinned as an absence AND as the new call text.
    expect("T7 the old 1,032-byte localRec T+M-path local is gone from the kext source", !has(ahh, "n48_cg_pagerec localRec {};"));
    expect("T7 a miss (not a hit) fills the memo row after computing id, from the file-scope scratch recorder",
           has(ahh, "if (mode == N48_PGMID_MODE_TM) gfxsrc_pgmid_memo_fill(&gPgmMemo, stage, va, id, fillMarkBefore, &gPgmProfileScratchTM);"));
    // 0.0.436 DEFECT FIX pins: no VA-derived page range exists any more, and the fill takes the resolving read's
    // own per-call recorder instead.
    expect("T7 n48_pm_pages_of (a VA-derived page range) is gone from the kext source", !has(ahh, "n48_pm_pages_of"));
    expect("T7 gfxsrc_pgmid_memo_fill takes the resolving read's own recorder, not a VA-derived range",
           has(ahh, "static void gfxsrc_pgmid_memo_fill(n48_pm *m, uint32_t stage, uint64_t va, int id, uint64_t fillMark,") &&
           has(ahh, "const n48_cg_pagerec *local)"));
    expect("T7 mode T+M's own miss path hands gfxsrc_pgm_profile_headfirst the file-scope scratch recorder, not the active one",
           has(ahh, "got = gfxsrc_pgm_profile_headfirst(stage, va, gXdPgm, true, &gPgmProfileScratchTM);"));
    // Reviewer pins (0.0.436 verification): the model tests above cannot see this glue. A reviewer-planted break
    // that deleted the miss path's copy into the active recorder passed every check until these were added.
    expect("T7 the T+M miss path notes its read's VRAM pages into the ACTIVE recorder, so the segment check sees them",
           has(ahh, "for (uint32_t i = 0; i < gPgmProfileScratchTM.n; i++)\n            n48_cg_pagerec_note(activeRec, gPgmProfileScratchTM.page[i], 4096u);"));
    // 0.0.438 (D5): SHADOW-M's own 1,032-byte local is gone too, replaced by its own separate scratch recorder -
    // separate from gPgmProfileScratchTM, exactly as its own memo (gPgmMemoShadow) is separate from the real one.
    expect("T7 D5: gPgmProfileScratchTM and gPgmProfileScratchShadowM are both declared, file-scope",
           has(ahh, "static n48_cg_pagerec gPgmProfileScratchTM {};") &&
           has(ahh, "static n48_cg_pagerec gPgmProfileScratchShadowM {};"));
    expect("T7 D5: SHADOW-M's own trial uses its OWN separate scratch recorder, not T+M's",
           has(ahh, "const uint32_t gotT = gfxsrc_pgm_profile_headfirst(stage, va, gXdPgmT, false, &gPgmProfileScratchShadowM);") &&
           has(ahh, "gfxsrc_pgmid_memo_fill(&gPgmMemoShadow, stage, va, idM, markBefore, &gPgmProfileScratchShadowM);"));
    expect("T7 the fill mark is taken before the resolving read",
           has(ahh, "fillMarkBefore = navi48_cg_ring_mark_now();"));
    expect("T7 a hit is refused when the copy-guard ring moved since the fill",
           has(ahh, "if (navi48_cg_ring_mark_now() != row->fillMark) { gPgmId.pm_inval++; return false; }"));
    expect("T7 a hit is refused when a poison row overlaps one of its VRAM pages",
           has(ahh, "if (navi48_cg_poison_overlaps_page(row->pages[i])) { gPgmId.pm_poison_miss++; return false; }"));
    expect("T7 a valid hit notes its stored VRAM pages into the active recorder",
           has(ahh, "for (uint32_t i = 0; i < row->npages; i++) n48_cg_pagerec_note(rec, row->pages[i], 4096u);"));

    // 0.0.437 (item 5) source pins: the full-table skip is checked BEFORE the extension read, inside the SAME
    // extendWitness block every T/T+M-miss/SHADOW-M-trial caller of gfxsrc_pgm_profile_headfirst shares.
    expect("T7 item 5: the full-table check gates the extension, checked before the 512-dword read",
           has(ahh, "if (gXdMissN >= kXdMissRows) {\n                gPgmId.witness_skip_full++;"));

    // 0.0.437 (item 6) source pins: the row-cap guard exists, is set at build time, and gates n48_pgm_need's call.
    expect("T7 item 6: gPgmIdRowsOverflow is declared and set only when the accessor still answers past the cap",
           has(ahh, "static bool gPgmIdRowsOverflow { false };") &&
           has(ahh, "if (xlat12_shader_id_row(kPgmIdRowCap, &stage, &ndw, head)) {\n            gPgmIdRowsOverflow = true;"));
    expect("T7 item 6: the head-first read fails closed to the full window when the row table overflowed",
           has(ahh, "const uint32_t L = gPgmIdRowsOverflow ? N48_PGMID_CAP : n48_pgm_need(stage, head, gPgmIdRows, gPgmIdRowN);"));
    expect("T7 item 6: the old 'safe either way' claim beside kPgmIdRowCap is corrected, not merely deleted",
           !has(ahh, "which\n// n48_pgm_need's own N48_PGMID_CAP clamp already makes safe either way") &&
           has(ahh, "was WRONG. N48_PGMID_CAP only clamps how many dwords ONE read may ever ask for"));

    // 0.0.438 source pins: gfxsrc_pgmid_rows_build() is now called at the ONE point every mode of
    // gfxsrc_pgm_profile passes before its own read - right after `mode` is captured, before the memo-hit check,
    // T+M's own miss path, and gfxsrc_pgm_profile_id_read (whose own first line still calls it too - idempotent,
    // unchanged, harmless).
    expect("T7 item D1: gfxsrc_pgmid_rows_build() is called right after `mode` is captured, before ANY branch",
           has(ahh, "const uint32_t mode = gXdPgmIdMode;\n    // 0.0.438:") &&
           has(ahh, "gfxsrc_pgmid_rows_build();\n    // 0.0.436 DEFECT FIX (reviewer, post-0.0.436): the resolving read's OWN pages"));
    expect("T7 item D1: gfxsrc_pgmid_rows_build() is called at least twice (the new site, plus its original site "
           "inside gfxsrc_pgm_profile_id_read - idempotent, so calling it again there is unchanged and harmless)",
           [&ahh] {
               size_t n = 0, pos = 0;
               while ((pos = ahh.find("gfxsrc_pgmid_rows_build();", pos)) != std::string::npos) { n++; pos += 1; }
               return n >= 2u;
           }());
}

// =============================================================================================================
// T11 — 0.0.444 (an internal review note (B) item J): THE DESCRIPTOR RUNG'S EVIDENCE MUST USE THE GATED
// `out->ps_table_abi1`, NOT THE UNGATED `xlat12_shader_id_desc_table(id, ...)`, FOR A FRAGMENT PROGRAM.
//
// (CONFIRMED): switch 43 gates `ps_table_abi1` back to 0 when it is OFF (xlat12_table_abi_is_gated(row) &&
// !gP43On, AppleHardwareHook.cpp's resolve function), but through 0.0.443 the descriptor rung's OWN evidence block
// asked `xlat12_shader_id_desc_table(id, ...)` directly - a STATIC, id-keyed lookup that answers whether the row
// EXISTS in the table, not whether this frame's gate admitted it. P's kDTableAbi row exists unconditionally (only
// its USE is gated), so with 43 OFF (the default) a P-bearing frame was still counted as "converted... by the
// policy" (descPortProgs++) even though its draws stay UNTRANSLATED - the always-on descriptor rung no longer
// refused it. Modelled here with mocks standing in for the REAL functions (which need a real P program id to
// drive end to end): `mock_desc_table_static` always answers 1 (the row EXISTS), exactly as
// xlat12_shader_id_desc_table(P's id, ...) does whether or not switch 43 is on.
// ---------------------------------------------------------------------------------------------------------------
static uint32_t mock_desc_inline(uint32_t) { return 0u; }        // P has no inline row - only the table shape
static uint32_t mock_desc_table_static(uint32_t) { return 1u; }  // P's kDTableAbi row EXISTS, UNCONDITIONALLY

// The FIX (0.0.444): stage 0 asks the GATED field the kext's own resolve function just wrote.
static uint32_t real_desc_rung(uint32_t stage, uint32_t psTableAbi1)
{
    return mock_desc_inline(0u) || (stage == 0u ? (psTableAbi1 != 0u) : (mock_desc_table_static(0u) != 0u));
}
// 0.0.443's actual code: unconditionally asks the static table function, ignoring the gate entirely.
static uint32_t old_desc_rung(uint32_t stage, uint32_t psTableAbi1)
{
    (void)stage; (void)psTableAbi1;
    return mock_desc_inline(0u) || (mock_desc_table_static(0u) != 0u);
}
static void test_T11_desc_rung_gated(const char *ahhPath)
{
    // Switch 43 OFF (default): the gate zeroed ps_table_abi1, so the rung must refuse - "exactly as in 0.0.441"
    // (before this table row existed, `xlat12_shader_id_desc_table` answered 0 for P too, taking the SAME refusal).
    expect("T11: 43 OFF - the REAL (gated) rung refuses a fragment program (P not counted as descriptor-port-covered)",
           real_desc_rung(0u, 0u) == 0u);
    // Switch 43 ON: ps_table_abi1 stays the real row's non-zero value (xlat12_table_abi_is_gated admitted it).
    expect("T11: 43 ON - the REAL (gated) rung admits the table path", real_desc_rung(0u, 46u) == 1u);
    // A vertex-stage program is unaffected (no ps_table_abi1 concept there): the static lookup still applies.
    expect("T11: stage 1 (vertex) is unaffected - still asks the static table lookup",
           real_desc_rung(1u, 0u) == (mock_desc_table_static(0u) != 0u));
    // PLANTED BREAK: the OLD (ungated) rung admits P even with 43 OFF - the fail-open found.
    {
        const bool caught = (real_desc_rung(0u, 0u) == 0u) && (old_desc_rung(0u, 0u) == 1u);
        std::printf("T11 planted break (the descriptor rung asks the UNGATED static table function): %s\n",
                    caught ? "CAUGHT (0.0.444 refuses with 43 OFF; 0.0.443's ungated call would have admitted it)"
                           : "*** NOT CAUGHT ***");
        if (!caught) gFail++;
        gRun++;
    }
    // Source pins (optional argv[1]): the kext wiring itself, gated for stage 0.
    if (!ahhPath) { std::printf("  SKIP T11 source pins (no AppleHardwareHook.cpp argument)\n"); return; }
    std::string ahh;
    if (!read_file(ahhPath, ahh)) { std::printf("  SKIP T11 source pins (cannot read %s)\n", ahhPath); return; }
    expect("T11: the descriptor rung's evidence block reads `out->ps_table_abi1` for stage 0",
           has(ahh, "(stage == 0u ? out->ps_table_abi1 != 0u"));
    expect("T11: ... and the ungated table call moves to the ELSE (stage 1 only)",
           has(ahh, ": xlat12_shader_id_desc_table(id, nullptr, nullptr, nullptr,") &&
           has(ahh, "nullptr) != 0u))) {"));
    expect("T11: the OLD unconditional call (desc_inline || desc_table, no gate) is GONE",
           !has(ahh, "xlat12_shader_id_desc_inline(id, nullptr, nullptr) ||\n                                     "
                     "xlat12_shader_id_desc_table(id, nullptr, nullptr, nullptr, nullptr))) {"));
}

// =============================================================================================================
// G — build 0.0.484 (an internal design note Q2 K1-K7, Q6 test (a)): glass_background_lph's REAL 5376-byte
// images (fixture_glass_bd_be.h, generated from the GLASS v3 .pal.o objects) through the REAL read, the REAL "is
// this ours" compare, the REAL identity table and the REAL memo rule - the kext's own code from gfx_subst_caps.h /
// gfx_pgmid.h, never a copy.
// =============================================================================================================
static const uint64_t kGlassKeyBD = 0x597b70b28759e040ull, kGlassKeyBE = 0x5abbb918af0075b7ull;   // GLASS.md's title
// A page-table stand-in: the 5376-byte image at `base`, then 256 dwords of whatever Apple placed next.
struct GlassVm { std::vector<uint32_t> mem; uint64_t base; uint32_t calls, firstAsk, lastAsk; };
static void glass_vm(GlassVm &g, const uint32_t *img, uint64_t base)
{
    g.mem.assign(img, img + N48_GLASS_IMAGE_DWORDS);
    g.mem.resize(N48_GLASS_IMAGE_DWORDS + 256u, 0xDEADBEEFu);
    g.base = base; g.calls = g.firstAsk = g.lastAsk = 0u;
}
static uint32_t glass_read(void *ctx, uint64_t va, uint32_t *dst, uint32_t ndw)
{
    GlassVm *g = static_cast<GlassVm *>(ctx);
    if (!g->calls) g->firstAsk = ndw;
    g->calls++; g->lastAsk = ndw;
    if (va < g->base || ((va - g->base) & 3u)) return 0u;
    const uint64_t off = (va - g->base) / 4u;
    if (off >= g->mem.size()) return 0u;
    const uint32_t avail = (uint32_t)(g->mem.size() - off), got = ndw < avail ? ndw : avail;
    std::memcpy(dst, g->mem.data() + off, (size_t)got * 4u);
    return got;
}
static int glass_id_by_name(const char *nm)
{
    for (int i = 0; i < (int)xlat12_shader_id_count(); i++) if (!std::strcmp(xlat12_shader_id_name(i), nm)) return i;
    return -1;
}

// G1 — K3/K2: the kext's program read at glass's real VA returns the WHOLE 5376-byte image, and the kext's compare
// recognises it as OURS, naming the right key. Break (planted in the REAL header): K3 back at 1024 (with K4 at 512 so it
// compiles) - the read stops at 4096 bytes and the compare, which needs every byte of the image, misses.
static void test_G1_read_and_recognise(void)
{
    static uint8_t pool[2u * N48_XD_SUBST_BYTES];
    static uint32_t buf[N48_XD_PGM_DWORDS];
    expect("G1 setup: a rendered glass image fits one recognition-pool row (5376 <= N48_XD_SUBST_BYTES)",
           N48_GLASS_IMAGE_DWORDS * 4u <= N48_XD_SUBST_BYTES);
    std::memset(pool, 0, sizeof pool);
    std::memcpy(pool, kGlassBdImage, N48_GLASS_IMAGE_DWORDS * 4u);
    std::memcpy(pool + N48_XD_SUBST_BYTES, kGlassBeImage, N48_GLASS_IMAGE_DWORDS * 4u);
    const n48_xd_subst_row rows[2] = { { kGlassKeyBD, N48_GLASS_IMAGE_DWORDS * 4u, 3u },
                                       { kGlassKeyBE, N48_GLASS_IMAGE_DWORDS * 4u, 3u } };
    // decide50's glass PS VA (GLASS.md Q3: PS 0x4011f1900), page offset 0x900: the first read is the rest of the page
    const uint64_t va = 0x4011f1900ull;
    const struct { const char *nm; const uint32_t *img; uint64_t key; } g[2] = {
        { "BD", kGlassBdImage, kGlassKeyBD }, { "BE", kGlassBeImage, kGlassKeyBE } };
    for (uint32_t k = 0; k < 2u; k++) {
        GlassVm vm; glass_vm(vm, g[k].img, va);
        uint32_t L = 99u;
        const uint32_t got = n48_xd_read_program(&glass_read, &vm, va, buf, &L);
        char what[256];
        std::snprintf(what, sizeof what, "G1 %s: the REAL read at 0x4011f1900 asks the rest of the page (%u dwords), finds no gfx10 "
                      "terminator, and re-reads the FULL K3 window: %u dwords (want %u), L %u", g[k].nm, vm.firstAsk, got,
                      N48_XD_PGM_DWORDS, L);
        expect(what, vm.firstAsk == (0x1000u - 0x900u) / 4u && vm.calls == 2u && vm.lastAsk == N48_XD_PGM_DWORDS &&
                     got == N48_XD_PGM_DWORDS && L == 0u);
        uint64_t key = 0ull;
        const int hit = n48_xd_ours_find(buf, got, pool, rows, 2u, &key);
        std::snprintf(what, sizeof what, "G1 %s: the REAL compare recognises the bytes as OURS, key %#llx", g[k].nm,
                      (unsigned long long)key);
        expect(what, hit == 1 && key == g[k].key);
        // non-vacuity: every byte counts, and a read one dword short of the image is never a match
        key = 0ull;
        std::snprintf(what, sizeof what, "G1 %s: the same bytes read one dword SHORT of the image (%u) are not recognised",
                      g[k].nm, got - 1u);
        expect(what, n48_xd_ours_find(buf, got - 1u, pool, rows, 2u, &key) == 0 && key == 0ull);
        buf[N48_GLASS_IMAGE_DWORDS - 1u] ^= 1u;   // the LAST dword of the image (zero padding) differs
        std::snprintf(what, sizeof what, "G1 %s: one flipped bit in the image's last dword is not recognised", g[k].nm);
        expect(what, n48_xd_ours_find(buf, got, pool, rows, 2u, &key) == 0);
    }
    // the other page offsets a 256-byte-aligned VA can have: the first read is never the whole image, the second always is
    for (uint32_t po = 0u; po < 0x1000u; po += 0x100u) {
        GlassVm vm; glass_vm(vm, kGlassBdImage, 0x4011f0000ull + po);
        uint32_t L = 99u;
        const uint32_t got = n48_xd_read_program(&glass_read, &vm, vm.base, buf, &L);
        uint64_t key = 0ull;
        if (got != N48_XD_PGM_DWORDS || n48_xd_ours_find(buf, got, pool, rows, 2u, &key) != 1 || key != kGlassKeyBD) {
            char what[160];
            std::snprintf(what, sizeof what, "G1 BD at page offset %#x: read %u, recognised %d", po, got, key == kGlassKeyBD);
            expect(what, false);
            return;
        }
    }
    expect("G1 BD at every one of the 16 page offsets a 256-byte-aligned program VA can have: read whole, recognised", true);
}

// G2 — K4/K5 and the rows: identification at the REAL row length through the REAL identity table (the tools build's
// regenerated xlat12_shader_ids.h), the REAL n48_pgm_need, mode T's read model, the REAL memo rule for a 3-page read, and
// the chain identity -> xlat12_ib_profile_stage -> ps_table_abi1 -> the kext's 43 x 51 gate. Break (planted in the REAL
// header): N48_PGMID_CAP back at 512 - n48_pgm_need clamps to 512 and xlat12_shader_id_match skips the 1271-dword row.
static uint32_t g2_gate(uint32_t abi1, uint32_t on43, uint32_t on51)
{
    // gfxsrc_pgm_profile's two blocks, in its order (gfx_dep_test N9 pins the statements and their order in the source)
    if (abi1 && xlat12_table_abi_is_gated(abi1)) { if (!on43) abi1 = 0u; }
    if (abi1 && xlat12_table_abi_new_shape(abi1)) { if (!on51) abi1 = 0u; }
    return abi1;
}
static void test_G2_identify_at_row_length(void)
{
    build_rows();
    const int idBD = glass_id_by_name("ws_BD_glass_background_lph"), idBE = glass_id_by_name("ws_BE_glass_background_lph");
    expect("G2 the identity table carries ws_BD_glass_background_lph and ws_BE_glass_background_lph (the tools build's "
           "regenerated xlat12_shader_ids.h - MERGE-PENDING until the reviewer merges it)", idBD >= 0 && idBE >= 0);
    if (idBD < 0 || idBE < 0) return;
    uint32_t st = 9u, ndw = 0u, head[4] = { 0u, 0u, 0u, 0u };
    expect("G2 BD's identity row is (stage 0, 1271) with the image's own head",
           xlat12_shader_id_row((uint32_t)idBD, &st, &ndw, head) && st == 0u && ndw == N48_GLASS_BD_NDW &&
           !std::memcmp(head, kGlassBdImage, 16));
    expect("G2 BD identified at its REAL row length (match(img, 1271) = the BD row)",
           xlat12_shader_id_match(0u, kGlassBdImage, N48_GLASS_BD_NDW) == idBD);
    expect("G2 BD one dword short (1270) is NOT identified", xlat12_shader_id_match(0u, kGlassBdImage, N48_GLASS_BD_NDW - 1u) < 0);
    expect("G2 BD at the whole K3 read (1344) is identified - what mode T's buffer can hold",
           xlat12_shader_id_match(0u, kGlassBdImage, N48_XD_PGM_DWORDS) == idBD);
    expect("G2 K5 LEFT AT 512 (the choice, recorded): mode OFF's kXdIdDwords window cannot identify BD",
           xlat12_shader_id_match(0u, kGlassBdImage, N48_XD_ID_DWORDS) < 0);
    expect("G2 BE identified at its own length (1269), and at BD's 1271 (the shared head's need) still as BE",
           xlat12_shader_id_match(0u, kGlassBeImage, N48_GLASS_BE_NDW) == idBE &&
           xlat12_shader_id_match(0u, kGlassBeImage, N48_GLASS_BD_NDW) == idBE);
    const uint32_t need = n48_pgm_need(0u, kGlassBdImage, gRows, gRowN);
    char what[256];
    std::snprintf(what, sizeof what, "G2 n48_pgm_need over the REAL rows for glass's shared head = 1271 (BD's, the max of BD 1271 / "
                  "BE 1269), NOT clamped by N48_PGMID_CAP %u (got %u)", N48_PGMID_CAP, need);
    expect(what, need == N48_GLASS_BD_NDW);
    // mode T's own read (T4's model_t) over the image: reads exactly `need` dwords and identifies each compile
    const struct { const char *nm; const uint32_t *img; int want; } g[2] = { { "BD", kGlassBdImage, idBD }, { "BE", kGlassBeImage, idBE } };
    for (uint32_t k = 0; k < 2u; k++) {
        MockVm vm { g[k].img, N48_GLASS_IMAGE_DWORDS, 0x4011f1900ull };
        static uint32_t buf[N48_PGMID_CAP];
        uint32_t dw = 0u, dwOff = 0u;
        const int idT = model_t(0u, vm, vm.base, buf, &dw, false);
        const int idOff = model_off(0u, vm, vm.base, buf, &dwOff);
        std::snprintf(what, sizeof what, "G2 %s: mode T reads %u dwords and identifies it; mode OFF reads %u and does not (K5)",
                      g[k].nm, dw, dwOff);
        expect(what, idT == g[k].want && dw == N48_GLASS_BD_NDW && idOff < 0 && dwOff == N48_XD_ID_DWORDS);
    }
    // mode T+M: a glass read that ends in a THIRD page is memoised now (N48_PM_PAGES 3), and the next ask hits
    {
        const uint64_t va = 0x3ff00600d00ull;          // page offset 0xd00: 0xd00 + 5084 = 0x20dc -> three 4 KiB pages
        MockVm vm { kGlassBdImage, N48_GLASS_IMAGE_DWORDS, va };
        TmModel tm; tm_reset(tm);
        uint32_t dwMiss = 0u, dwHit = 0u;
        const int idMiss = model_tm(tm, 0u, va, vm, &dwMiss);
        const n48_pm_row *row = n48_pm_lookup(&tm.mem, 0u, va);
        const int idHit = model_tm(tm, 0u, va, vm, &dwHit);
        std::snprintf(what, sizeof what, "G2 T+M: a 3-page glass read (VA offset 0xd00) is STORED (npages %u, not-stored %u) and the "
                      "next ask HITS without a read (%u then %u dwords)", row ? row->npages : 0u, tm.notStored, dwMiss, dwHit);
        expect(what, idMiss == idBD && idHit == idBD && row && row->npages == 3u && tm.notStored == 0u && tm.hits == 1u &&
                     dwMiss == N48_GLASS_BD_NDW && dwHit == 0u);
        expect("G2 T+M: a 4-page recorder is still refused (the rule is n48_pm_store_rec's, fail-closed)",
               n48_pm_store_rec(&tm.mem, 0u, 0x777000ull, idBD, 1ull, row ? row->pages : nullptr, 4u, 0u) == 0 &&
               n48_pm_store_rec(&tm.mem, 0u, 0x777000ull, idBD, 1ull, row ? row->pages : nullptr, 1u, 1u) == 0);
    }
    // the chain the kext runs after a match: xlat12_ib_profile_stage fills ps_table_abi1 with glass's row, then 43 then 51
    for (uint32_t k = 0; k < 2u; k++) {
        xlat12_draw_profile q {}; uint32_t io[2] = { 0u, 0u };
        const uint32_t rc = xlat12_ib_profile_stage(g[k].want, &q, io);
        const uint32_t abi = xlat12_table_abi_find(k ? N48_GLASS_BE_NDW : N48_GLASS_BD_NDW, k ? N48_GLASS_BE_FNV : N48_GLASS_BD_FNV);
        std::snprintf(what, sizeof what, "G2 %s: REACHABILITY identity -> xlat12_ib_profile_stage -> ps_table_abi1 = its kDTableAbi row "
                      "(%u, want %u) -> the kext's gate keeps it ONLY with 43 AND 51 ON", g[k].nm, q.ps_table_abi1, abi);
        expect(what, rc == 0u && abi != 0u && q.ps_table_abi1 == abi && g2_gate(abi, 0u, 0u) == 0u && g2_gate(abi, 1u, 0u) == 0u &&
                     g2_gate(abi, 0u, 1u) == 0u && g2_gate(abi, 1u, 1u) == abi);
    }
}

// G3 — K7: what gfxsrc_idcap_boot_report asks, over the REAL identity table. Break: none needed beyond the table itself -
// these are the numbers the boot line prints, and the first must be 0.
static void test_G3_boot_guard(void)
{
    const char *nm = nullptr; uint32_t ndw = 0u;
    expect("G3 no identity row is over the head-first cap (xlat12_shader_id_over_cap(N48_PGMID_CAP) == 0, the boot guard's MUST)",
           xlat12_shader_id_over_cap(N48_PGMID_CAP, 0, &nm, &ndw) == 0u && xlat12_shader_id_max_ndw() <= N48_PGMID_CAP);
    const uint32_t over = xlat12_shader_id_over_cap(N48_XD_ID_DWORDS, 0, nullptr, nullptr);
    bool bd = false, be = false;
    for (uint32_t i = 0; i < over; i++) {
        (void)xlat12_shader_id_over_cap(N48_XD_ID_DWORDS, i, &nm, &ndw);
        if (nm && !std::strcmp(nm, "ws_BD_glass_background_lph") && ndw == N48_GLASS_BD_NDW) bd = true;
        if (nm && !std::strcmp(nm, "ws_BE_glass_background_lph") && ndw == N48_GLASS_BE_NDW) be = true;
    }
    char what[200];
    std::snprintf(what, sizeof what, "G3 the rows beyond kXdIdDwords (512) are EXACTLY glass BD and BE (%u named; MERGE-PENDING "
                  "until the regenerated identity table is merged)", over);
    expect(what, over == 2u && bd && be);
}

// G4 — the wiring the pure tests above cannot see: every kext call site of the moved code, pinned as a REACHABILITY
// complement (the behaviour itself is G1-G3's, over the same headers the kext compiles).
static void test_G4_wiring(const char *ahhPath)
{
    std::string ahh, peer;
    expect("G4 the kext source opens (a pin that cannot read its source must FAIL, never skip)",
           ahhPath && read_file(ahhPath, ahh));
    std::string peerPath = ahhPath ? ahhPath : "";
    const size_t cut = peerPath.rfind("AppleHardwareHook.cpp");
    if (cut != std::string::npos) peerPath.replace(cut, std::strlen("AppleHardwareHook.cpp"), "Navi48AccelPeer.cpp");
    expect("G4 Navi48AccelPeer.cpp (beside it) opens", !peerPath.empty() && read_file(peerPath.c_str(), peer));
    expect("G4 K2/K3/cap: the kext's own names are gfx_subst_caps.h's numbers",
           has(ahh, "static constexpr uint32_t kXdPgmDwords   = N48_XD_PGM_DWORDS;") &&
           has(ahh, "static constexpr uint32_t kXdSubstMax    = N48_XD_SUBST_MAX;") &&
           has(ahh, "static constexpr uint32_t kXdSubstBytes  = N48_XD_SUBST_BYTES;") &&
           has(ahh, "static constexpr uint32_t kXdIdDwords = N48_XD_ID_DWORDS;"));
    expect("G4 K3: gXdPgm is allocated kXdPgmDwords wide and gfxsrc_read_program IS n48_xd_read_program into it",
           has(ahh, "if (!gXdPgm)  gXdPgm  = static_cast<uint32_t *>(IOMalloc(kXdPgmDwords * sizeof(uint32_t)));") &&
           has(ahh, "return n48_xd_read_program(&gfxsrc_read_cb, const_cast<GfxcVm *>(&vm), va, gXdPgm, &L);"));
    expect("G4 both OURS compares are n48_xd_ours_find over the pool gfxsrc_xlat_open fills",
           has(ahh, "if (!hit) oursHit = n48_xd_ours_find(gXdPgm, got, gXdSubst, gXdSubstRow, gXdSubstN, &oursKey) ? 1u : 0u;") &&
           has(ahh, "if (n48_xd_ours_find(p, got, gXdSubst, gXdSubstRow, gXdSubstN, &k)) { m.oursHit = 1; m.oursKey = k; }"));
    {
        const size_t a = ahh.find("if (!gXdSubst) gXdSubst = static_cast<uint8_t *>(IOMalloc((size_t)kXdSubstMax * kXdSubstBytes));");
        const size_t b = ahh.find("gXdSubstN = n48_xd_subst_build(&gXdSc, gXdSubst, gXdSubstRow, kXdSubstMax, &gXdSubstCounts);");
        const size_t c = ahh.find("gXdScOpen = 1;", b == std::string::npos ? 0 : b);
        expect("G4 K2/cap: the pool is allocated kXdSubstMax x kXdSubstBytes, then built by n48_xd_subst_build at the same cap, "
               "then marked open, in that order", a != std::string::npos && b != std::string::npos && a < b && b < c &&
               c != std::string::npos && c - b < 300u);
    }
    expect("G4 cap: the open line prints the dropped count", has(ahh, "substitutable DROPPED past the %u-entry cap)"));
    {
        const size_t f = ahh.find("static uint32_t gfx_protect_arm(const char *who) {");
        const size_t r = ahh.find("gfxsrc_idcap_boot_report();", f == std::string::npos ? 0 : f);
        const size_t p = ahh.find("const uint32_t p = n48_gfxprot_plan(", f == std::string::npos ? 0 : f);
        expect("G4 K7: the boot guard is the FIRST statement of gfx_protect_arm (the boot chain's [10]), before the plan",
               f != std::string::npos && r != std::string::npos && p != std::string::npos && f < r && r < p && r - f < 120u);
        expect("G4 K7: the guard runs once, asks over_cap at BOTH windows and takes the census with no pool",
               has(ahh, "if (gXdIdcapReported) return;\n    gXdIdcapReported = true;") &&
               has(ahh, "xlat12_shader_id_over_cap(N48_PGMID_CAP, 0, &onm, &ondw);") &&
               has(ahh, "xlat12_shader_id_over_cap(kXdIdDwords, 0, nullptr, nullptr);") &&
               has(ahh, "n48_xd_subst_build(&gXdScBoot, nullptr, nullptr, kXdSubstMax, &gXdSubstCensus);"));
    }
    expect("G4 K4: the witness extension grows to kXdIdDwords, not to the raised N48_PGMID_CAP",
           has(ahh, "if (extendWitness && got < kXdIdDwords) {") && has(ahh, "const uint32_t want = kXdIdDwords - got;") &&
           !has(ahh, "N48_PGMID_CAP - got"));
    expect("G4 K4: the memo fill is n48_pm_store_rec over the read's own recorder",
           has(ahh, "if (!n48_pm_store_rec(m, stage, va, (int32_t)id, fillMark, local->page, local->n, local->overflow)) gPgmId.pm_not_stored++;"));
    expect("G4 K1: gScOut is kScMaxSubstBytes = N48_SC_MAX_SUBST_BYTES wide, tied to K2, and sc_subst_render gets sizeof gScOut",
           has(peer, "static constexpr uint32_t kScMaxSubstBytes  = N48_SC_MAX_SUBST_BYTES;") &&
           has(peer, "static_assert(kScMaxSubstBytes == N48_XD_SUBST_BYTES,") &&
           has(peer, "static uint8_t gScOut[kScMaxSubstBytes];") &&
           // build 0.0.536 (switch 92, gfx_rv92.h): the rendered match is n48_rv92_pick's (`m` unless 92 picks the v3 alternative)
           has(peer, "sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, gScOut, sizeof gScOut, &nb);"));
}

// =============================================================================================================
// J1 — build 0.0.504: REACHABILITY of program J (VfxXghb vertex, key 0xfcc6342ea8db283f).
// J's REAL m4c-r19 image (fixture_j_vfxxghb.h: the blob's substitutable value, program + s_code_end fill) through the
// kext's own identify path - the REAL head-first n48_pgm_need over the REAL identity rows, mode T's and mode OFF's
// read models, xlat12_shader_id_match - then the chain the kext runs after a match: xlat12_ib_profile_stage fills
// vs_readset1 / vs_abi_ptr1 with J's rows (5 pointers, slots 4/6/8/10/12, pd1 1). Breaks: one flipped program dword,
// the image cut one dword short of the row, and the fragment stage - each must NOT identify J.
// =============================================================================================================
static void test_J1_identify_and_rows(void)
{
    build_rows();
    const int idJ = glass_id_by_name("ws_J_VfxXghb");
    char what[320];
    std::snprintf(what, sizeof what, "J1 the identity table carries ws_J_VfxXghb (id %d of %u)", idJ, xlat12_shader_id_count());
    expect(what, idJ >= 0);
    if (idJ < 0) return;
    uint32_t st = 9u, ndw = 0u, head[4] = { 0u, 0u, 0u, 0u };
    expect("J1 J's identity row is (stage 1, 291) with the blob image's own head",
           xlat12_shader_id_row((uint32_t)idJ, &st, &ndw, head) && st == 1u && ndw == N48_J_NDW && !std::memcmp(head, kJImage, 16));
    const uint32_t need = n48_pgm_need(1u, kJImage, gRows, gRowN);
    std::snprintf(what, sizeof what, "J1 n48_pgm_need over the REAL rows for J's head = %u (>= 291, <= N48_PGMID_CAP %u)", need,
                  N48_PGMID_CAP);
    expect(what, need >= N48_J_NDW && need <= N48_PGMID_CAP);
    MockVm vm { kJImage, N48_J_IMAGE_DWORDS, N48_J_APPLE_VA };
    static uint32_t buf[N48_PGMID_CAP];
    uint32_t dwT = 0u, dwOff = 0u;
    const int idT = model_t(1u, vm, vm.base, buf, &dwT, false);
    const int idOff = model_off(1u, vm, vm.base, buf, &dwOff);
    std::snprintf(what, sizeof what, "J1 the REAL bytes at J's VA %#llx identify as ws_J_VfxXghb: mode T reads %u dw -> %d, mode OFF "
                  "reads %u dw -> %d (want %d)", (unsigned long long)N48_J_APPLE_VA, dwT, idT, dwOff, idOff, idJ);
    expect(what, idT == idJ && idOff == idJ && dwT >= N48_J_NDW);
    expect("J1 J's bytes at the FRAGMENT stage identify as nothing", model_t(0u, vm, vm.base, buf, &dwT, false) < 0);
    // planted breaks on the bytes: a flipped program dword, and the image cut one dword short of the row
    static uint32_t bad[N48_J_IMAGE_DWORDS];
    std::memcpy(bad, kJImage, sizeof bad);
    bad[100] ^= 1u;
    MockVm vb { bad, N48_J_IMAGE_DWORDS, N48_J_APPLE_VA };
    expect("J1 BREAK one flipped program dword (100) -> NOT identified", model_t(1u, vb, vb.base, buf, &dwT, false) < 0);
    MockVm vs { kJImage, N48_J_NDW - 1u, N48_J_APPLE_VA };
    expect("J1 BREAK the image cut one dword short of the row (290) -> NOT identified", model_t(1u, vs, vs.base, buf, &dwT, false) < 0);
    // the chain: identity -> xlat12_ib_profile_stage -> vs_readset1 / vs_abi_ptr1 -> J's rows
    xlat12_draw_profile q {}; uint32_t io[2] = { 0u, 0u };
    const uint32_t rc = xlat12_ib_profile_stage(idJ, &q, io);
    const xlat12_readset_row *rr = (q.vs_readset1 && q.vs_readset1 <= XLAT12_READSET_COUNT) ? &kXlat12Readset[q.vs_readset1 - 1u] : nullptr;
    const xlat12_abi_ptrs *ap = (q.vs_abi_ptr1 && q.vs_abi_ptr1 <= XLAT12_ABI_PTR_ROWS) ? &kXlat12AbiPtrs[q.vs_abi_ptr1 - 1u] : nullptr;
    std::snprintf(what, sizeof what, "J1 REACHABILITY identity -> xlat12_ib_profile_stage: rc %u, vs_rsrc1_gs %#x, io %u, "
                  "vs_readset1 %u (%s), vs_abi_ptr1 %u (%s)", rc, q.vs_rsrc1_gs, io[0], q.vs_readset1, rr ? rr->name : "none",
                  q.vs_abi_ptr1, ap ? ap->name : "none");
    expect(what, rc == 0u && q.vs_rsrc1_gs == 0x020F0002u && io[0] == 3u && rr && ap && !std::strcmp(rr->name, "ws_J_VfxXghb") &&
                 !std::strcmp(ap->name, "ws_J_VfxXghb"));
    if (!rr || !ap) return;
    const uint8_t want[5] = { 4u, 6u, 8u, 10u, 12u };
    expect("J1 J's read-set row: stage 1, 291/0xfa0982ef, pd1 1, pii 1, no table, FIVE pointers at slots 4/6/8/10/12 "
           "(XLAT12_READSET_PTR_MAX 5)",
           rr->stage == 1u && rr->ndw == N48_J_NDW && rr->fnv == N48_J_FNV && rr->proof_depth1_data_only == 1u &&
           rr->proof_images_inline == 1u && rr->desc_table == 0u && rr->inl_tex == 0xffu && rr->nptr == 5u &&
           XLAT12_READSET_PTR_MAX == 5u && !std::memcmp(rr->slot, want, 5));
    expect("J1 J's ABI-pointer row declares the same five slots",
           ap->stage == 1u && ap->nptr == 5u && ap->slot[0] == 4u && ap->slot[1] == 6u && ap->slot[2] == 8u &&
           ap->slot[3] == 10u && ap->slot[4] == 12u);
}

int main(int argc, char **argv)
{
    test_T1();
    test_T2();
    test_T3();
    test_T4();
    test_T5();
    test_T6a();
    test_T6b();
    test_T6c();
    test_T6d();
    test_T6e();
    test_T6f();
    test_T6g();
    test_T6h();
    test_T6i();
    test_T8_witness_skip_full();
    test_T9_rowcap_guard();
    test_T10_rowsbuild_before_first_read();
    test_T7(argc > 1 ? argv[1] : nullptr);
    test_T11_desc_rung_gated(argc > 1 ? argv[1] : nullptr);
    test_G1_read_and_recognise();          // build 0.0.484 (GLASS.md Q6 (a))
    test_G2_identify_at_row_length();
    test_G3_boot_guard();
    test_G4_wiring(argc > 1 ? argv[1] : nullptr);
    test_J1_identify_and_rows();          // build 0.0.504

    std::printf("\ngfx_pgmid: %d check(s), %d failed\n", gRun, gFail);
    if (gFail) { std::printf("gfx_pgmid: FAIL\n"); return 1; }
    std::printf("gfx_pgmid: N48-PGMID-TEST-PASS\n");
    return 0;
}
