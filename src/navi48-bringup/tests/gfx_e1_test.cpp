// gfx_e1_test.cpp — RULE E1's proof, offline. The property under test:
//
//     "EARLY may go live on a ring that held an INDIRECT_BUFFER ONLY when that IB was our own clear-state NOP page,
//      proven ours in hardware, alone in a ring of packets we allow, on a ring generation nothing has reset."
//
// Every clause must refuse ON ITS OWN, at a departure of ONE, from an input that otherwise passes. And an input nobody
// filled must refuse rather than read as a clean ring.
//
// THE FIXTURE THAT MATTERS: hp2. printed hp2's arm-time IB and it satisfied every clause's E1 had — VMID 0,
// our page, within 64 bytes, every dword a NOP — while the ring it sat in was Apple's ring AFTER A GPU RESET
// (`ARM-TIME WALK ... [0..512)`; every other logged run 128). hp2 is built here from its own logged numbers and must
// REFUSE, and it must refuse on the ring-generation clause specifically, not by accident on some other one.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). Each mutant below is a plausible
// way to write E1 wrong — several of them are exactly the way it WAS written in — and each must be CAUGHT by at
// least one named check.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_e1_test.cpp -o /tmp/e1test && /tmp/e1test
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include "gfx_e1.h"
#include "gfx_dep.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-70s got %llu want %llu\n", what, (unsigned long long)got,
                                 (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-70s %llu\n", what, (unsigned long long)got);
    }
}

static void expect_clause(const char *what, uint32_t got, uint32_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-70s got %u %s want %u %s\n", what, got, n48_e1_clause_name(got), want,
                                 n48_e1_clause_name(want));
    } else if (!gQuiet) {
        std::printf("ok    %-70s %u %s\n", what, got, n48_e1_clause_name(got));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// THE ONE INPUT THAT MAY PASS: hp1/hp3/hp5/hp8's arm-time ring, from what those runs logged.
//   `ARM-TIME WALK ... [0..128): walked, 2 type-3 packet(s), 1 INDIRECT_BUFFER(s) ... walk reached the end`
//   `ARM-TIME IB 1 of 1 at ring dword 0x6: header 0xc0023f00, VA 0x840fc39000, len 16 dw, VMID 0 (control 0x80000010);
//    read 16 of 16 dw ... 16 of 16 read dword(s) are Apple's 1-dword NOP 0xffff1000`
//   `csb: published a 16-dword NOP IB at GART MC 0x840fc39000`
//   pre-arm, in those runs: 0 resetEngineQueue calls, 0 reset-hardware-and-replay keys, 0 bind_at overlap refusals,
//   1 MAP_QUEUES(engine_sel 4), 0 UNMAP_QUEUES(GFX).
// ---------------------------------------------------------------------------------------------------------------------
static constexpr uint64_t kCsbMc   = 0x840fc39000ull;   // the `csb:` publish of hp1/hp2/hp3/hp5/hp8, all the same
// The bus address and PTE below are SYNTHETIC: no logged run prints the clear-state page's DMA bus address or its GART
// PTE, because 0.0.368 is the first build that reads either. They are here to exercise the comparison, not to assert a
// measured value. What IS measured is the shape: gmc_bind_existing writes (bus & ~0xfff) | PTEFlags::SYSMEM_RW
// (gmc_v12_0.cpp:1753, CONFIRMED), which is what navi48_csb_pte_read reconstructs.
static constexpr uint64_t kCsbBus  = 0x0000000112a34000ull;
static constexpr uint64_t kPteWant = (kCsbBus & ~0xFFFull) | 0x8000000000000077ull;  // bus | the flags gmc writes

static n48_e1_in clean_in()
{
    n48_e1_in e {};
    e.gathered = 1u;
    e.walked = 1u; e.wrapped = 0u; e.stopped = 0u;
    e.ibs = 1u;
    e.packets = 2u; e.pkt_recorded = 2u;
    e.pkt_op[0] = N48_E1_OP_IB;     e.pkt_pos[0] = 0x6u;
    e.pkt_op[1] = N48_E1_OP_RELMEM; e.pkt_pos[1] = 0xau;
    e.ib_recorded = 1u;
    e.ib_pos = 0x6u;
    e.ib_ctl = 0x80000010u;
    e.ib_dw1 = (uint32_t)(kCsbMc & 0xFFFFFFFFu);
    e.ib_va  = kCsbMc;
    e.ib_read = 16u; e.ib_nops = 16u;
    e.csb_published = 1u; e.csb_mc = kCsbMc; e.csb_bytes = 64u;
    e.pte_read_ok = 1u; e.pte = kPteWant; e.pte_want = kPteWant;
    e.bind_refusals = 0u;
    e.gfx_queue_maps = 1u; e.gfx_queue_unmaps = 0u;
    e.engine_resets = 0u; e.hw_reset_keys = 0u;
    return e;
}

// hp2's arm-time ring, from hp2/driverlog-stream.txt. Every clause held on it. Counted by me in that log against
// the first `ARM-TIME WALK` line: 2 `[39] resetEngineQueue` calls and 4 `type=0x17 (reset-hardware-and-replay)` keys
// before arming; hp1/hp3/hp4/hp5/hp7/hp8 each have 0 and 0.
static n48_e1_in hp2_in()
{
    n48_e1_in e = clean_in();
    e.engine_resets = 2u;
    e.hw_reset_keys = 4u;
    return e;
}

// The rule under test, indirected so the SAME clause checks can be run against each planted defect below.
typedef uint32_t (*eval_fn)(const n48_e1_in *);
static eval_fn gEvalFn = &n48_e1_eval;
static uint32_t gEval(const n48_e1_in *e) { return gEvalFn(e); }

// ---------------------------------------------------------------------------------------------------------------------
// THE CLAUSES, each on its own, each at a departure of ONE.
// ---------------------------------------------------------------------------------------------------------------------
struct Case { const char *name; std::function<void(n48_e1_in &)> mutate; uint32_t want; };

static const Case kCases[] = {
    { "gathered 0 - an input nobody filled",        [](n48_e1_in &e) { e.gathered = 0u; },        N48_E1_NOT_GATHERED },
    { "walk never ran",                             [](n48_e1_in &e) { e.walked = 0u; },          N48_E1_NOT_WALKED },
    { "ring had wrapped",                           [](n48_e1_in &e) { e.wrapped = 1u; },         N48_E1_NOT_WALKED },
    { "walk stopped short of wptr",                 [](n48_e1_in &e) { e.stopped = 1u; },         N48_E1_NOT_WALKED },
    { "0 IBs - E1 must not be the reason",          [](n48_e1_in &e) { e.ibs = 0u; },             N48_E1_NO_IBS },
    { "2 IBs",                                      [](n48_e1_in &e) { e.ibs = 2u; },             N48_E1_TOO_MANY_IBS },
    { "IB found but never recorded",                [](n48_e1_in &e) { e.ib_recorded = 0u; },     N48_E1_NO_IB_RECORD },
    { "clear-state page never published",           [](n48_e1_in &e) { e.csb_published = 0u; },   N48_E1_CSB_UNPUBLISHED },
    { "csb MC zero",                                [](n48_e1_in &e) { e.csb_mc = 0u; },          N48_E1_CSB_UNPUBLISHED },
    { "control 0x8000000f - 15 dwords, not 16",     [](n48_e1_in &e) { e.ib_ctl = 0x8000000fu; }, N48_E1_IB_CONTROL },
    { "control 0x80000011 - 17 dwords",             [](n48_e1_in &e) { e.ib_ctl = 0x80000011u; }, N48_E1_IB_CONTROL },
    { "control VMID 2 not 0",                       [](n48_e1_in &e) { e.ib_ctl = 0x82000010u; }, N48_E1_IB_CONTROL },
    { "control bit 31 clear",                       [](n48_e1_in &e) { e.ib_ctl = 0x00000010u; }, N48_E1_IB_CONTROL },
    { "dw1 low bits set",                           [](n48_e1_in &e) { e.ib_dw1 |= 2u; },         N48_E1_IB_DW1_LOW },
    { "VA is another page",                         [](n48_e1_in &e) { e.ib_va = kCsbMc + 0x1000ull; }, N48_E1_IB_VA },
    { "VA is last boot's CSB address",              [](n48_e1_in &e) { e.csb_mc = kCsbMc + 0x2000ull; }, N48_E1_IB_VA },
    { "reported size 32 bytes not 64",              [](n48_e1_in &e) { e.csb_bytes = 32u; },      N48_E1_IB_SIZE },
    { "only 15 dwords readable",                    [](n48_e1_in &e) { e.ib_read = 15u; e.ib_nops = 15u; }, N48_E1_IB_READ },
    { "17 dwords read - more than we published",    [](n48_e1_in &e) { e.ib_read = 17u; e.ib_nops = 17u; }, N48_E1_IB_READ },
    { "15 of 16 are NOPs",                          [](n48_e1_in &e) { e.ib_nops = 15u; },        N48_E1_IB_WORDS },
    { "0 of 16 are NOPs",                           [](n48_e1_in &e) { e.ib_nops = 0u; },         N48_E1_IB_WORDS },
    { "PTE unreadable",                             [](n48_e1_in &e) { e.pte_read_ok = 0u; },     N48_E1_PTE_UNREAD },
    { "PTE names somebody else's page",             [](n48_e1_in &e) { e.pte ^= 0x1000ull; },     N48_E1_PTE_MISMATCH },
    { "PTE flags differ by one bit",                [](n48_e1_in &e) { e.pte ^= 1ull; },          N48_E1_PTE_MISMATCH },
    { "PTE and want both zero",                     [](n48_e1_in &e) { e.pte = 0u; e.pte_want = 0u; }, N48_E1_PTE_MISMATCH },
    { "one bind_at overlap refusal",                [](n48_e1_in &e) { e.bind_refusals = 1u; },   N48_E1_BIND_REFUSALS },
    { "more packets than recorded",                 [](n48_e1_in &e) { e.packets = 3u; },         N48_E1_PACKET_OVERFLOW },
    { "17 packets - past the recorder's cap",       [](n48_e1_in &e) { e.packets = 17u; e.pkt_recorded = 16u; },
                                                                                                 N48_E1_PACKET_OVERFLOW },
    { "a WRITE_DATA sits in the ring",              [](n48_e1_in &e) { e.pkt_op[1] = 0x37u; },    N48_E1_PACKET_OTHER },
    { "a DMA_DATA sits in the ring",                [](n48_e1_in &e) { e.pkt_op[1] = 0x50u; },    N48_E1_PACKET_OTHER },
    { "a COPY_DATA sits in the ring",               [](n48_e1_in &e) { e.pkt_op[1] = 0x40u; },    N48_E1_PACKET_OTHER },
    { "a second IB packet at another position",     [](n48_e1_in &e) { e.pkt_op[1] = N48_E1_OP_IB; }, N48_E1_PACKET_IB_ALIAS },
    { "the IB packet is at a position we did not read",
                                                    [](n48_e1_in &e) { e.pkt_pos[0] = 0x20u; },   N48_E1_PACKET_IB_ALIAS },
    { "no IB packet in the allowlist at all",       [](n48_e1_in &e) { e.pkt_op[0] = N48_E1_OP_RELMEM; }, N48_E1_PACKET_IB_ALIAS },
    { "GFX queue mapped twice",                     [](n48_e1_in &e) { e.gfx_queue_maps = 2u; },  N48_E1_QUEUE_MAPS },
    { "GFX queue never mapped",                     [](n48_e1_in &e) { e.gfx_queue_maps = 0u; },  N48_E1_QUEUE_MAPS },
    { "GFX queue unmapped once",                    [](n48_e1_in &e) { e.gfx_queue_unmaps = 1u; }, N48_E1_QUEUE_UNMAPPED },
    { "one resetEngineQueue before arming",         [](n48_e1_in &e) { e.engine_resets = 1u; },   N48_E1_RING_RESET },
    { "one reset-and-replay bracket before arming", [](n48_e1_in &e) { e.hw_reset_keys = 1u; },   N48_E1_RING_RESET },
    { "hp2: both, at hp2's own counts",             [](n48_e1_in &e) { e.engine_resets = 2u; e.hw_reset_keys = 4u; },
                                                                                                 N48_E1_RING_RESET },
};

static void run_clauses()
{
    { n48_e1_in e = clean_in(); expect_clause("the clean arm-time ring PASSES", gEval(&e), N48_E1_PASS); }
    for (const Case &c : kCases) {
        n48_e1_in e = clean_in();
        c.mutate(e);
        expect_clause(c.name, gEval(&e), c.want);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// THE MUTANTS. Each is E1 written a plausible wrong way; each must be caught by at least one case above.
// ---------------------------------------------------------------------------------------------------------------------
// M1 —'s own rule, verbatim: VMID 0, our page, "<= 64 bytes", every dword read a NOP. Nothing else.
static uint32_t mut_773(const n48_e1_in *e)
{
    if (!e) return N48_E1_NOT_GATHERED;
    const uint32_t len = e->ib_ctl & 0xFFFFFu, vmid = (e->ib_ctl >> 24) & 0xFu;
    const bool ok = vmid == 0u && e->csb_published == 1u && e->ib_va == e->csb_mc && len != 0u &&
                    len * 4u <= e->csb_bytes && e->ib_read == len && e->ib_nops == len;
    return ok ? N48_E1_PASS : N48_E1_IB_CONTROL;
}
// M2 — exact match, but no hardware proof: the PTE and the bind refusals are never looked at.
static uint32_t mut_no_pte(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e; c.pte_read_ok = 1u; c.pte = 1u; c.pte_want = 1u; c.bind_refusals = 0u;
    return n48_e1_eval(&c);
}
// M3 — a CONSTANT clear-state address instead of this boot's publish. NB: the address has in fact been 0x840fc39000 in
// every logged boot (hp1/hp2/hp3/hp5/hp8 all print it), so this mutant is NOT justified by an observed variation - it is
// justified by the fact that the address is whatever the GART bump allocator hands out at run time and nothing pins it.
// A rule that reads "our page" out of a literal stops being a statement about this boot the first time that changes.
static uint32_t mut_const_va(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e; c.csb_mc = c.ib_va; c.csb_published = 1u;
    return n48_e1_eval(&c);
}
// M4 — the bind_at refusals counted but ignored ("they were 0 in hp1 and hp2 anyway").
static uint32_t mut_ignore_binds(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e; c.bind_refusals = 0u;
    return n48_e1_eval(&c);
}
// M5 — no packet allowlist: only the IBs are judged, so anything else in the ring passes.
static uint32_t mut_no_allowlist(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e;
    c.packets = c.pkt_recorded = 1u;
    c.pkt_op[0] = N48_E1_OP_IB; c.pkt_pos[0] = c.ib_pos;
    return n48_e1_eval(&c);
}
// M6 — the ring-generation clause dropped (E1 as the safety review found it: every clause held on hp2).
static uint32_t mut_no_generation(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e; c.gfx_queue_maps = 1u; c.gfx_queue_unmaps = 0u; c.engine_resets = 0u; c.hw_reset_keys = 0u;
    return n48_e1_eval(&c);
}
// M7 — "<= 16 dwords" instead of exactly 16, the review's first named defect.
static uint32_t mut_le16(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    n48_e1_in c = *e;
    const uint32_t len = c.ib_ctl & 0xFFFFFu;
    if (len != 0u && len <= N48_E1_IB_DWORDS && c.ib_read == len && c.ib_nops == len) {
        c.ib_ctl = N48_E1_IB_CTL; c.ib_read = N48_E1_IB_DWORDS; c.ib_nops = N48_E1_IB_DWORDS;
    }
    return n48_e1_eval(&c);
}
// M8 — a zero input reads as a clean ring (the fail-open inversion, transplanted from `target_vram`).
static uint32_t mut_fail_open(const n48_e1_in *e)
{
    if (!e) return N48_E1_PASS;
    if (e->gathered != 1u) return N48_E1_PASS;
    return n48_e1_eval(e);
}
// M9 — "0 IBs also passes E1", which would let E1 rather than EARLY's own rung be the reason EARLY is live.
static uint32_t mut_zero_ibs_pass(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;
    if (e->walked == 1u && !e->wrapped && !e->stopped && e->ibs == 0u) return N48_E1_PASS;
    return n48_e1_eval(e);
}

struct Mutant { const char *name; eval_fn fn; };
static const Mutant kMutants[] = {
    { "M1's rule verbatim",                &mut_773 },
    { "M2 no PTE / no bind-refusal proof",      &mut_no_pte },
    { "M3 a constant VA, not this boot's",      &mut_const_va },
    { "M4 bind_at refusals ignored",            &mut_ignore_binds },
    { "M5 no packet allowlist",                 &mut_no_allowlist },
    { "M6 no ring-generation clause (hp2)",     &mut_no_generation },
    { "M7 \"<= 16 dwords\" instead of exact",   &mut_le16 },
    { "M8 a zero input reads as clean",         &mut_fail_open },
    { "M9 0 IBs passes E1 too",                 &mut_zero_ibs_pass },
};

// ---------------------------------------------------------------------------------------------------------------------
// EARLY'S OWN RUNG: E1 must widen EARLY and nothing else, and only when it PASSES.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t early_of(uint32_t walked, uint32_t wrapped, uint32_t stopped, uint32_t ibs, uint32_t e1)
{
    n48_dep_src s {};
    s.gathered = 1u;
    s.pre_walked = walked; s.pre_wrapped = wrapped; s.pre_stopped = stopped; s.pre_ibs = ibs; s.pre_e1 = e1;
    n48_dep_mono m {}; n48_dep_world w {};
    n48_dep_fill(&s, &m, &w);
    return (w.observers & N48_DEP_OBS_EARLY) ? 1u : 0u;
}

static void run_early()
{
    expect_u("EARLY live: walked, 0 IBs (0.0.358's rule, unchanged)", early_of(1, 0, 0, 0, 0), 1u);
    expect_u("EARLY live: walked, 1 IB, E1 PASSED",                   early_of(1, 0, 0, 1, 1), 1u);
    expect_u("EARLY NOT live: 1 IB, E1 refused",                      early_of(1, 0, 0, 1, 0), 0u);
    expect_u("EARLY NOT live: 2 IBs, E1 somehow 1",                   early_of(1, 0, 0, 2, 1), 0u);
    expect_u("EARLY NOT live: wrapped, even with E1",                 early_of(1, 1, 0, 1, 1), 0u);
    expect_u("EARLY NOT live: walk stopped, even with E1",            early_of(1, 0, 1, 1, 1), 0u);
    expect_u("EARLY NOT live: never walked, even with E1",            early_of(0, 0, 0, 1, 1), 0u);
    expect_u("EARLY NOT live: 1 IB, nothing set (the zero world)",    early_of(1, 0, 0, 1, 0), 0u);

    // E1 widens EARLY and NOTHING else: with E1 passing, every other observer bit is exactly what it was without it.
    n48_dep_src a {}, b {};
    a.gathered = b.gathered = 1u;
    a.src_install = b.src_install = 1u;
    a.ring_state = b.ring_state = 1u; a.ring_hooked = b.ring_hooked = 1u;
    a.sdma_state = b.sdma_state = 2u;
    a.stall_armed = b.stall_armed = 1u; a.stall_read_ok = b.stall_read_ok = 1u;
    a.q_hook_live = b.q_hook_live = 1u; a.q_tally_ok = b.q_tally_ok = 1u;
    a.fault_read_ok = b.fault_read_ok = 1u;
    a.pre_walked = b.pre_walked = 1u; a.pre_ibs = b.pre_ibs = 1u;
    b.pre_e1 = 1u;
    n48_dep_mono ma {}, mb {}; n48_dep_world wa {}, wb {};
    n48_dep_fill(&a, &ma, &wa);
    n48_dep_fill(&b, &mb, &wb);
    expect_u("without E1: observers are the six, EARLY missing", wa.observers, (uint64_t)(N48_DEP_OBS_REQUIRED & ~N48_DEP_OBS_EARLY));
    expect_u("with E1:    observers are all seven",              wb.observers, (uint64_t)N48_DEP_OBS_REQUIRED);
    expect_u("E1 changed EXACTLY the EARLY bit",                 wa.observers ^ wb.observers, (uint64_t)N48_DEP_OBS_EARLY);
    // ... and nothing outside `observers` moved at all.
    n48_dep_world za = wa, zb = wb;
    za.observers = zb.observers = 0u;
    expect_u("E1 moved no other field of the world", std::memcmp(&za, &zb, sizeof(za)) == 0 ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// INERT: with E1 REFUSING, n48_dep_fill's observers are 0.0.367's, over a large generated set.
// ---------------------------------------------------------------------------------------------------------------------
// 0.0.367's WHOLE observers word, hand-copied from the fill as it stood before RULE E1 (gfx_dep.h, 0.0.367). Nothing here
// calls n48_dep_fill, so the comparison below is not the new code checking itself.
static uint32_t frozen0367_observers(const n48_dep_src *s)
{
    uint32_t obs = 0u;
    if (s->src_install == 1u) obs |= N48_DEP_OBS_SRC;
    if ((s->ring_state == 1u || s->ring_state == 2u) && s->ring_hooked == 1u) obs |= N48_DEP_OBS_RING;
    if (s->sdma_state == 2u) obs |= N48_DEP_OBS_SDMA;
    if (s->stall_armed == 1u && s->stall_read_ok == 1u) obs |= N48_DEP_OBS_STALL;
    if (s->pre_walked == 1u && !s->pre_wrapped && !s->pre_stopped && s->pre_ibs == 0u) obs |= N48_DEP_OBS_EARLY;
    if (s->q_hook_live == 1u && s->q_tally_ok == 1u) obs |= N48_DEP_OBS_QUEUE;
    if (s->fault_read_ok == 1u) obs |= N48_DEP_OBS_FAULT;
    return obs;
}

static void run_inert()
{
    uint64_t x = 0x243F6A8885A308D3ull, states = 0, same = 0, sameReason = 0;
    for (uint32_t i = 0; i < 300000u; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        n48_dep_src s {};
        s.gathered = 1u;
        s.src_install   = (uint32_t)((x >> 1) & 3u);
        s.ring_state    = (uint32_t)((x >> 3) & 3u);
        s.ring_hooked   = (uint32_t)((x >> 5) & 1u);
        s.sdma_state    = (uint32_t)((x >> 6) & 3u);
        s.stall_armed   = (uint32_t)((x >> 8) & 1u);
        s.stall_read_ok = (uint32_t)((x >> 9) & 1u);
        s.q_hook_live   = (uint32_t)((x >> 10) & 1u);
        s.q_tally_ok    = (uint32_t)((x >> 11) & 1u);
        s.fault_read_ok = (uint32_t)((x >> 12) & 1u);
        s.pre_walked    = (uint32_t)((x >> 13) & 1u);
        s.pre_wrapped   = (uint32_t)((x >> 14) & 1u);
        s.pre_stopped   = (uint32_t)((x >> 15) & 1u);
        s.pre_ibs       = (uint32_t)((x >> 16) & 7u);
        /* E1 REFUSED on every one of these: pre_e1 stays 0, pre_e1_clause ranges over every clause it can return. */
        s.pre_e1        = 0u;
        s.pre_e1_clause = (uint32_t)((x >> 19) % N48_E1_CLAUSES);
        if (s.pre_e1_clause == N48_E1_PASS) s.pre_e1_clause = N48_E1_RING_RESET;   /* a refusal is never clause 0 */
        /* Every v2 counter too, so "the world" being identical means the whole world, not just the observers. */
        for (uint32_t k = 0; k < N48_DEPC_COUNT; k++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; s.v[k] = x & 3u; }
        n48_dep_mono m {}; n48_dep_world w {};
        n48_dep_fill(&s, &m, &w);
        states++;
        /* (a) the observers word is EXACTLY 0.0.367's, computed by hand above; */
        const bool obsSame = (w.observers == frozen0367_observers(&s));
        /* (b) ... and nothing outside it can have moved, because the fill's only change is inside it: the world with the
         * observers stripped must be byte-identical to a world built from the SAME src with pre_e1/pre_e1_clause cleared. */
        n48_dep_src s0 = s; s0.pre_e1 = 0u; s0.pre_e1_clause = 0u;
        n48_dep_mono m0 {}; n48_dep_world w0 {};
        n48_dep_fill(&s0, &m0, &w0);
        n48_dep_world a = w, b = w0; a.observers = b.observers = 0u;
        if (obsSame && std::memcmp(&a, &b, sizeof(a)) == 0) same++;
        /* (c) and X9's own answer - reason and detail - is the same on both. */
        uint64_t d1 = 0u, d0 = 0u;
        if (n48_dep_check(&w, &d1) == n48_dep_check(&w0, &d0) && d1 == d0) sameReason++;
    }
    expect_u("inert: E1 refusing, the world is 0.0.367's over 300000 generated worlds", same, states);
    expect_u("inert: ... and n48_dep_check's reason and detail are identical",          sameReason, states);
}

int main(int argc, char **argv)
{
    const bool quiet = argc > 1 && std::strcmp(argv[1], "-q") == 0;
    gQuiet = quiet ? 1 : 0;

    std::printf("=== RULE E1 (gfx_e1.h, ) ===\n");
    run_clauses();
    run_early();
    run_inert();

    // hp2, named: it must refuse, and it must refuse on the generation clause.
    { n48_e1_in e = hp2_in(); expect_clause("hp2's own arm-time ring REFUSES on ring-generation", n48_e1_eval(&e), N48_E1_RING_RESET); }
    { n48_e1_in e = hp2_in(); e.engine_resets = 0u; e.hw_reset_keys = 0u;
      expect_clause("... and with hp2's resets removed it is hp8's ring, which passes", n48_e1_eval(&e), N48_E1_PASS); }

    // Planted-defect control.
    std::printf("\n=== planted defects (each must be caught by a named check) ===\n");
    const int clauseChecks = gRun;
    int caught = 0;
    for (const Mutant &mu : kMutants) {
        const int f0 = gFail, r0 = gRun;
        gEvalFn = mu.fn;
        gQuiet = 1;
        run_clauses();
        gQuiet = quiet ? 1 : 0;
        gEvalFn = &n48_e1_eval;
        const int broke = gFail - f0;
        gFail = f0; gRun = r0;   // the mutant's failures are the POINT; they are not this suite's failures
        if (broke > 0) caught++;
        std::printf("%-42s %s (%d check(s) refused it)\n", mu.name, broke > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", broke);
    }
    (void)clauseChecks;
    const int nMut = (int)(sizeof(kMutants) / sizeof(kMutants[0]));
    if (caught != nMut) gFail += (nMut - caught);

    std::printf("\n%d checks, %d failures, %d of %d planted defects caught\n", gRun, gFail, caught, nMut);
    return gFail ? 1 : 0;
}
