// gfx_cycle80_test.cpp — build 0.0.525 (an internal design note C1-C7, X1-X4; ): switch 80, cycle
// completeness, gfx_cycle80.h + gfx_present73.h's C6 over run11c's REAL sequence, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_cycle80_test.cpp -o /tmp/c80 && \
//         /tmp/c80 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp
// Covers (the contract's T1-T10, then the build's own):
//   T1  run11c F1..F165 (tests/fixture_cycle80_run11c.h: the gate's answers, the kext's own R5′ records, P inputs BY CONTENT): the
//       committed P judged INCOMPLETE are EXACTLY {17,24,29,35,39,43,47,51,69,74,86,94,138,142,146}; the other captured committed
//       P are COMPLETE; F1/F15 (fills before either layer is known) are UNDETERMINED; BOUNDED 5, BLIND 0, lost 1 (F65), and no P
//       ever read an unknown;
//   T2  the present replay (73 ON, Part E): ON never puts an INCOMPLETE P on the glass, freezes on P134 through P146 and resumes at
//       P150; SHADOW's presents are byte-identical to OFF's while its cycle state and counters equal ON's (X1);
//   T3  BLIND / truncated / no record: every layer; BOUNDED: none;
//   T4  a memory destination inside / outside the extent (end exclusive), another ctx's VA, a page match across ctx;
//   T5  a post-gate loss (queued, drained) demotes a P slot on that layer with a newer seq; a seq not in the ring: every layer;
//   T6  sticky: a P with no writers after a dirty cycle stays INCOMPLETE (and after a clean one stays COMPLETE);
//   T7  discovery starts UNKNOWN; recovery at the first cycle with a committed writer and nothing held;
//   T8  OFF IDENTITY: run10u's and run11c's switch-73 replays through the current header with 80 OFF are the frozen 0.0.524
//       header's (tests/frozen/gfx_present73_c4cbc423.h): every present's answer and reason, every counter, every slot; 73's
//       format strings are the frozen ones (a differing macro redefinition fails this compile under -Werror);
//   T9  seq wrap (ring, demotion, the slot's verdict);
//   T10 swapped descriptor slots (the boot-to-boot swap,) give the same verdicts: the key never reads the slot;
//   T11 C6's order: INCOMPLETE before Part E's newer-than, nothing of Part E set by a hold; UNDET holds;
//   T12 C5's cross clause; the verb's M; the queue's overflow and reset;
//   T13 X4: every new line at maximal fields fits the 491-byte body; the per-P line cap; the copy summary cap;
//   T14 the kext glue (source pins, with ORDER): OFF at boot, the verb, each hook gated and placed, the dpled extension.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include "gfx_cycle80.h"
#include "fixture_cycle80_run11c.h"
#include "fixture_cycle515_run10u.h"
namespace p524 {
#include "frozen/gfx_present73_c4cbc423.h"
}

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p)
{
    std::string s;
    if (!p) return s;
    FILE *f = std::fopen(p, "rb");
    if (!f) return s;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const char *n)
{
    uint32_t c = 0; size_t at = 0; const size_t l = std::strlen(n);
    while ((at = s.find(n, at)) != std::string::npos) { c++; at += l; }
    return c;
}

// ------------------------------------------------------------------------------------------------ the run11c replay engine
static const uint64_t kCtx = 0x5ull;
static const uint64_t kX = 0x400800000ull, kXp = 0x404800000ull;
static const uint32_t kExpInc[] = { 17, 24, 29, 35, 39, 43, 47, 51, 69, 74, 86, 94, 138, 142, 146 };
static const uint32_t kExpComplete[] = { 55, 60, 65, 78, 82, 90, 98, 102, 106, 110, 114, 118, 126, 130, 134, 150, 154, 158, 162 };

static void r5_of(const c80_fx &x, n48_r5_frame *r)
{
    std::memset(r, 0, sizeof *r);
    if (x.bucket == 0u) { r->out_of_scope = 1u; return; }
    r->ib_ok = r->walk_ok = r->targets_ok = r->memw_ok = r->tgts_resolved = r->memw_resolved = 1u;
    r->ntgt = x.ntgt; r->nmemw = x.nmemw;
    for (uint32_t k = 0; k < x.ntgt && k < N48_CP_TGT_MAX; k++) { r->tgt[k].va = x.tgt[k].va; r->tgt[k].page = x.tgt[k].page; }
    for (uint32_t k = 0; k < x.nmemw && k < N48_CP_MEMW_MAX; k++) {
        r->memw[k].va = kC80Memw[x.memw_at + k].va; r->memw[k].page = kC80Memw[x.memw_at + k].page;
    }
}
static uint64_t key11(uint64_t va, uint64_t page) { return page ? page + (va & 0xfffull) : 0ull; }
static bool is_shape(const c80_fx &x) { return x.captured && x.nib == 1u && x.len[0] == 1040u; }
static bool is_plane11(uint64_t va) { return va == 0x401800000ull || va == 0x402800000ull || va == 0x403800000ull; }

struct Present { uint32_t f, copy, why; uint32_t glass; };
struct Rep11 {
    n48_c80 c; n48_c80_q q; n48_p73 t;
    std::map<uint32_t, n48_c80_pout> pout;     // frame -> the P's judgement
    std::map<uint32_t, uint32_t> seqOf, fOfSeq;
    std::vector<Present> pres;
    std::set<uint32_t> copiedIncomplete;      // frames whose INCOMPLETE P reached the glass
    uint64_t sumU;
};
struct Opt {
    uint32_t mode;          /* gP73.c80on */
    bool hooks;             /* the c80 hooks run (the kext: mode != OFF) */
    uint64_t extent;        /* the ledger's size handed to every layer (0 = absent) */
    uint32_t swapFrom;      /* from this frame on the descriptor slots 1 and 7 are swapped (T10); 0 = never */
    bool readUnknown;       /* every committed writer's reads are unknown (T12) */
    bool r5Only;            /* hand no cyc515 write set (the contract's C3 as written: the R5' record alone) */
};
static void replay11(Rep11 &R, const Opt &o)
{
    std::memset(&R.c, 0, sizeof R.c); std::memset(&R.q, 0, sizeof R.q); std::memset(&R.t, 0, sizeof R.t);
    n48_p73_reset(&R.t); n48_c80_reset(&R.c);
    R.t.c80on = o.mode; R.sumU = 0u;
    uint32_t gseq = 0u;
    for (uint32_t r = 0; r < kC80Run11cN; r++) {
        const c80_fx &x = kC80Run11c[r];
        const uint32_t seq = x.committed ? ++gseq : 0u;
        if (seq) { R.seqOf[x.f] = seq; R.fOfSeq[seq] = x.f; }
        n48_p73_call_begin(&R.t);
        const bool shape = is_shape(x);
        if (shape && x.ntgt == 1u) (void)n48_p73_gate_p(&R.t, key11(x.tgt[0].va, x.tgt[0].page), x.tgt[0].va, x.committed, seq);
        else {
            uint64_t keys[8] = {};
            for (uint32_t k = 0; k < x.ntgt && k < 8u; k++) keys[k] = key11(x.tgt[k].va, x.tgt[k].page);
            (void)n48_p73_gate_nonp(&R.t, keys, x.ntgt < 8u ? x.ntgt : 8u);
        }
        if (o.hooks) {
            (void)n48_c80_drain(&R.c, &R.q, &R.t);
            n48_c80_scope(&R.c, 1u);
            n48_c80_frame_begin(&R.c);
            if (o.readUnknown) n48_c80_note_reads(&R.c, nullptr, nullptr, 0u, 1u);
            if (x.in_ok) {   // the D draw's input pair as the translator hands it (the tiled layer + the linear LUT) - MF-3 counts it
                const uint64_t pv[2] = { x.in_va, 0x400240000ull }; const uint32_t pm[2] = { 3u, 0u };
                n48_c80_note_reads(&R.c, pv, pm, 2u, 0u);
            }
            uint64_t va[8] = {}, pg[8] = {}; uint32_t res[8] = {};
            for (uint32_t k = 0; k < x.ntgt && k < 8u; k++) { va[k] = x.tgt[k].va; pg[k] = x.tgt[k].page; res[k] = 1u; }
            if (n48_c80_is_p(&R.c, shape ? 1u : 0u, x.ntgt, kCtx, x.ntgt ? pg[0] : 0ull, x.ntgt ? va[0] : 0ull)) {
                n48_c80_pin in {};
                in.ctx = kCtx; in.frame = x.f; in.plane = x.plane; in.in_page = x.in_page;
                in.in_ok = n48_c80_p_input(&R.c, &in.in_va); in.page_ok = x.in_page ? 1u : 0u; in.extent = o.extent; in.seq = seq;
                in.dslot = (o.swapFrom && x.f >= o.swapFrom) ? (x.dslot == 1u ? 7u : x.dslot == 7u ? 1u : x.dslot) : x.dslot;
                const uint32_t slot = n48_p73_find(&R.t, key11(x.tgt[0].va, x.tgt[0].page));
                n48_c80_pout po {};
                (void)n48_c80_p(&R.c, &in, &R.t, slot, &po);
                R.pout[x.f] = po; R.sumU += po.u;
            } else (void)n48_c80_writer(&R.c, kCtx, va, pg, res, x.ntgt < 8u ? x.ntgt : 8u, o.r5Only ? nullptr : x.ws,
                                        o.r5Only ? 0u : x.nws, x.committed, seq);
            if (!x.committed) {
                static n48_r5_frame rf; r5_of(x, &rf);
                (void)n48_c80_held(&R.c, &rf, 0u, o.r5Only ? nullptr : x.ws, o.r5Only ? 0u : x.nws, o.r5Only ? 0u : x.wsunk, kCtx, x.f);
            }
        }
        (void)n48_p73_after_decide(&R.t);
        if (x.committed) {
            if (x.lost) { (void)n48_p73_final(&R.t, seq, 0u); if (o.hooks) n48_c80_q_lost(&R.q, seq); }
            else { (void)n48_p73_final(&R.t, seq, 1u); (void)n48_p73_retired(&R.t, seq); }
        }
        if (shape && x.ntgt == 1u && is_plane11(x.tgt[0].va)) {
            uint32_t why = 0u, slot = 0u;
            const uint32_t c = n48_p73_should_copy(1u, &R.t, key11(x.tgt[0].va, x.tgt[0].page), &why, &slot);
            if (c) {
                n48_p73_delivered(&R.t);
                const uint32_t f = R.fOfSeq[R.t.lastDelivered];
                auto it = R.pout.find(f);
                if (it == R.pout.end() || it->second.verdict != N48_P73_C80V_COMPLETE) R.copiedIncomplete.insert(f);
            }
            R.pres.push_back({ x.f, c, why, R.t.deliveredOk ? R.fOfSeq[R.t.lastDelivered] : 0u });
        }
    }
}
static Rep11 gOn, gShadow, gOff, gSwap, gExt0;
static const uint64_t kExtent = 0x800000ull;   // 1920x1080x4 rounded to the 8 MiB the tiled layer occupies (SUSPECTED)

// ---------------------------------------------------------------------------------------------------------------- T1
static void t1_run11c()
{
    replay11(gOn, { N48_P73_C80_ON, true, kExtent, 0u, false, false });
    std::set<uint32_t> inc, comp, undet;
    uint32_t committedP = 0u;
    for (const auto &kv : gOn.pout) {
        const c80_fx *x = nullptr;
        for (uint32_t r = 0; r < kC80Run11cN; r++) if (kC80Run11c[r].f == kv.first) x = &kC80Run11c[r];
        if (!x || !x->committed) continue;
        committedP++;
        if (kv.second.verdict == N48_P73_C80V_INCOMPLETE) inc.insert(kv.first);
        else if (kv.second.verdict == N48_P73_C80V_COMPLETE) comp.insert(kv.first);
        else undet.insert(kv.first);
    }
    const std::set<uint32_t> expInc(std::begin(kExpInc), std::end(kExpInc)), expComp(std::begin(kExpComplete), std::end(kExpComplete));
    std::string got;
    for (uint32_t f : inc) got += " " + std::to_string(f);
    std::printf("      run11c committed P judged INCOMPLETE:%s\n", got.c_str());
    got.clear();
    for (uint32_t f : comp) got += " " + std::to_string(f);
    std::printf("      run11c committed P judged COMPLETE:%s\n", got.c_str());
    expect_u("T1 the INCOMPLETE committed P are EXACTLY {17,24,29,35,39,43,47,51,69,74,86,94,138,142,146}", inc == expInc, 1u);
    expect_u("T1 the other captured committed P (19; F166 is outside the capture) are COMPLETE", comp == expComp, 1u);
    expect_u("T1 F1 and F15 (P-shaped fills before either layer is known, no D input) are UNDETERMINED",
             undet == std::set<uint32_t>({ 1u, 15u }), 1u);
    expect_u("T1 committed P judged: 15 + 19 + 2", committedP, 36u);
    expect_u("T1 F25/F31 (fills of X'/X after discovery) are WRITERS, not P",
             gOn.pout.count(25u) + gOn.pout.count(31u), 0u);
    expect_u("T1 two layers, found by content at F13 (X, page 0x10030000) and F17 (X', page 0x12560000)",
             gOn.c.L[0].used && gOn.c.L[0].va == kX && gOn.c.L[0].page == 0x10030000ull && gOn.c.L[0].found == 13u &&
             gOn.c.L[1].used && gOn.c.L[1].va == kXp && gOn.c.L[1].page == 0x12560000ull && gOn.c.L[1].found == 17u &&
             !gOn.c.L[2].used && gOn.c.pFirstSight == 2u, 1u);
    expect_u("T1 held-back: BOUNDED 5 (F11/F14/F18/F19/F30), BLIND 0, truncated 0, no record 0",
             gOn.c.heldBounded * 0x1000u + gOn.c.heldBlind * 0x100u + gOn.c.heldTrunc * 0x10u + gOn.c.heldNoRecord, 0x5000u);
    expect_u("T1 lost after the gate: 1 (F65, token seq 35's walk NOP), found in the ring (a P: no layer)",
             gOn.c.lost * 0x10u + gOn.c.lostUnknown, 0x10u);
    expect_u("T1 no P judgement read an unknown (sum of u over every P = 0)", gOn.sumU, 0u);
    // every INCOMPLETE P after discovery has a held-back write in its cycle (the reason is the refused writer, not an unknown)
    uint32_t heldWhy = 0u, disc = 0u;
    for (uint32_t f : inc) {
        const n48_c80_pout &p = gOn.pout[f];
        if (p.h > 0u && p.hFirst > 0u && p.hFirst < f) heldWhy++;
        else if (p.prev == N48_C80_ST_UNKNOWN && p.c == 0u && p.h == 0u) disc++;
    }
    expect_u("T1 14 INCOMPLETE P carry a held-back write in their cycle; P17 is the discovery (UNKNOWN, no writer counted)",
             heldWhy * 0x100u + disc, 14u * 0x100u + 1u);
    expect_u("T1 P55 (the first complete cycle of X') and P60 (X) are COMPLETE with committed writers and nothing held",
             gOn.pout[55u].c > 0u && gOn.pout[55u].h == 0u && gOn.pout[60u].c > 0u && gOn.pout[60u].h == 0u, 1u);
    expect_u("T1 P134 COMPLETE, P138 (f137 'a' refused) INCOMPLETE, the missing-avatar cycle",
             gOn.pout[134u].verdict * 0x10u + gOn.pout[138u].verdict, N48_P73_C80V_COMPLETE * 0x10u + N48_P73_C80V_INCOMPLETE);
    expect_u("T1 C3's 0.0.525 finding: cyc515's write set named a layer the R5' record did not, in held-back frames (F67, F70, ...)",
             gOn.c.heldWsOnly > 0u && gOn.c.heldWsUnknown == 0u, 1u);
    {   // the contract's C3 as written (the R5' record alone) is fail-OPEN on run11c: P69 and P74 read COMPLETE
        static Rep11 r5o;
        replay11(r5o, { N48_P73_C80_ON, true, kExtent, 0u, false, true });
        std::set<uint32_t> inc5;
        for (const auto &kv : r5o.pout) if (kv.second.verdict == N48_P73_C80V_INCOMPLETE && r5o.seqOf.count(kv.first)) inc5.insert(kv.first);
        std::set<uint32_t> miss;
        for (uint32_t f : expInc) if (!inc5.count(f)) miss.insert(f);
        expect_u("T1 (the contract's C3 alone, R5' records only) misses exactly P69 and P74: F67 'b' and F70 drew X / X' then retargeted",
                 miss == std::set<uint32_t>({ 69u, 74u }) && inc5.size() == expInc.size() - 2u, 1u);
    }
    // F166 is a COMMIT the capture did not hold: say so, never guess it
    expect_u("T1 the fixture stops at F165 (F166 committed but uncaptured: not judged here)", kC80Run11c[kC80Run11cN - 1u].f, 165u);
    // the extent: the 16 MiB fallback (no ledger size) gives the same verdicts on run11c
    replay11(gExt0, { N48_P73_C80_ON, true, 0ull, 0u, false, false });
    uint32_t same = 1u;
    std::string diff;
    for (const auto &kv : gOn.pout)
        if (gExt0.pout[kv.first].verdict != kv.second.verdict) { same = 0u; diff += " P" + std::to_string(kv.first); }
    std::printf("      the 16 MiB fallback extent changes:%s (held %llu readable, named only by cyc515 %llu)\n", diff.c_str(),
                (unsigned long long)gExt0.c.wrHeld, (unsigned long long)gExt0.c.heldWsOnly);
    // CYCLE80.md open risk 4, measured: with NO ledger size the 16 MiB fallback reaches 0x4010e0000 ('b' frames' second target,
    // X + 0x8e0000), so P78 and P150 read INCOMPLETE (fail-closed, but more freezing). The kext asks the ledger (c80_led_extent).
    expect_u("T1 the 16 MiB fallback (no ledger size; counted) turns exactly P78 and P150 INCOMPLETE (a 'b' target at X + 0x8e0000)",
             same == 0u && diff == " P78 P150" && gExt0.c.extentAssumed == 2u, 1u);
    {
        static Rep11 e7;
        replay11(e7, { N48_P73_C80_ON, true, 0x7E9000ull, 0u, false, false });
        uint32_t s7 = 1u;
        for (const auto &kv : gOn.pout) if (e7.pout[kv.first].verdict != kv.second.verdict) s7 = 0u;
        expect_u("T1 the exact 1920x1080x4 extent (0x7E9000) gives the same verdicts as 8 MiB", s7, 1u);
    }
    {   // a size the ledger brings at a LATER P replaces the fallback
        static n48_c80 c; std::memset(&c, 0, sizeof c); n48_c80_reset(&c);
        n48_c80_pin a {}; a.ctx = kCtx; a.in_ok = 1u; a.page_ok = 1u; a.in_va = kX; a.in_page = 0x10030000ull; a.extent = 0ull;
        (void)n48_c80_p(&c, &a, nullptr, N48_P73_SLOTS, nullptr);
        const uint64_t e0 = c.L[0].extent;
        a.extent = 0x7E9000ull;
        (void)n48_c80_p(&c, &a, nullptr, N48_P73_SLOTS, nullptr);
        expect_u("T1 the fallback extent is replaced by the ledger's size at a later P", e0 == N48_C80_EXTENT_DEF && c.L[0].extent == 0x7E9000ull &&
                 !c.L[0].extentAssumed, 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------- T2
static void t2_present()
{
    replay11(gShadow, { N48_P73_C80_SHADOW, true, kExtent, 0u, false, false });
    replay11(gOff, { N48_P73_C80_OFF, false, kExtent, 0u, false, false });
    expect_u("T2 ON: no INCOMPLETE or UNDETERMINED P ever reached the glass", gOn.copiedIncomplete.size(), 0u);
    expect_u("T2 OFF (80 OFF, 73 ON): INCOMPLETE P do reach the glass", gOff.copiedIncomplete.count(138u), 1u);
    uint32_t froze = 1u, resumed = 0u, glass134 = 0u;
    for (const Present &p : gOn.pres) {
        if (p.f == 134u) glass134 = p.glass;
        if (p.f >= 138u && p.f <= 146u && p.glass != 134u) froze = 0u;
        if (p.f == 150u) resumed = p.glass;
    }
    expect_u("T2 ON: the glass is P134 after its present", glass134, 134u);
    expect_u("T2 ON: frozen on P134 through P138, P142 and P146 (held INCOMPLETE)", froze, 1u);
    expect_u("T2 ON: resumes at P150", resumed, 150u);
    uint32_t incHolds = 0u;
    for (const Present &p : gOn.pres) if (!p.copy && p.why == N48_P73_HOLD_INCOMPLETE) incHolds++;
    expect_u("T2 ON: the INCOMPLETE holds are counted in held[INCOMPLETE]", gOn.t.held[N48_P73_HOLD_INCOMPLETE], incHolds);
    // X1: SHADOW = OFF on the glass, = ON in everything computed
    uint32_t sameAsOff = gShadow.pres.size() == gOff.pres.size() ? 1u : 0u;
    for (size_t i = 0; sameAsOff && i < gOff.pres.size(); i++)
        if (gShadow.pres[i].f != gOff.pres[i].f || gShadow.pres[i].copy != gOff.pres[i].copy ||
            gShadow.pres[i].why != gOff.pres[i].why || gShadow.pres[i].glass != gOff.pres[i].glass) sameAsOff = 0u;
    expect_u("X1 SHADOW: every present's answer, reason and glass P are byte-identical to 80 OFF's", sameAsOff, 1u);
    expect_u("X1 SHADOW: 73's copy counters equal OFF's (copied, delivered, held OLDER) and it never holds INCOMPLETE",
             gShadow.t.copied == gOff.t.copied && gShadow.t.delivered == gOff.t.delivered &&
             gShadow.t.held[N48_P73_HOLD_OLDER] == gOff.t.held[N48_P73_HOLD_OLDER] &&
             gShadow.t.held[N48_P73_HOLD_INCOMPLETE] == 0u, 1u);
    expect_u("X1 SHADOW: the cycle state and every counter equal ON's (the whole n48_c80, byte for byte)",
             std::memcmp(&gShadow.c, &gOn.c, sizeof gOn.c) == 0, 1u);
    expect_u("X1 SHADOW: every P verdict equals ON's", gShadow.pout.size() == gOn.pout.size(), 1u);
    uint32_t pv = 1u;
    for (const auto &kv : gOn.pout) if (gShadow.pout[kv.first].verdict != kv.second.verdict) pv = 0u;
    expect_u("X1 ... verdict by verdict", pv, 1u);
    expect_u("X1 SHADOW's 'would hold' == ON's INCOMPLETE holds (> 0)",
             gShadow.t.c80WouldHold == gOn.t.held[N48_P73_HOLD_INCOMPLETE] && gOn.t.held[N48_P73_HOLD_INCOMPLETE] > 0u, 1u);
    expect_u("X1 OFF: nothing of 80 counted by the present", gOff.t.c80WouldHold + gOff.t.held[N48_P73_HOLD_INCOMPLETE], 0u);
}

// ---------------------------------------------------------------------------------------------------------------- helpers
static void two_layers(n48_c80 *c, n48_p73 *t)
{
    std::memset(c, 0, sizeof *c); n48_c80_reset(c);
    if (t) { std::memset(t, 0, sizeof *t); n48_p73_reset(t); t->c80on = N48_P73_C80_ON; }
    n48_c80_pin a {}; a.ctx = kCtx; a.in_ok = 1u; a.page_ok = 1u; a.in_va = kX; a.in_page = 0x10030000ull; a.extent = kExtent; a.frame = 1u;
    (void)n48_c80_p(c, &a, nullptr, N48_P73_SLOTS, nullptr);
    a.in_va = kXp; a.in_page = 0x12560000ull; a.frame = 2u;
    (void)n48_c80_p(c, &a, nullptr, N48_P73_SLOTS, nullptr);
}
static uint32_t judge(n48_c80 *c, uint64_t va, uint64_t page, uint32_t seq, n48_p73 *t = nullptr, uint32_t slot = N48_P73_SLOTS)
{
    n48_c80_pin a {}; a.ctx = kCtx; a.in_ok = 1u; a.page_ok = 1u; a.in_va = va; a.in_page = page; a.extent = kExtent; a.seq = seq;
    a.dslot = N48_C80_NOSLOT;
    return n48_c80_p(c, &a, t, slot, nullptr);
}
static void commit_writer(n48_c80 *c, uint64_t va, uint64_t page, uint32_t seq)
{
    const uint64_t v[1] = { va }, p[1] = { page }; const uint32_t r[1] = { 1u };
    (void)n48_c80_writer(c, kCtx, v, p, r, 1u, nullptr, 0u, 1u, seq);
}
static void held_readable(n48_c80 *c, uint64_t tva, uint64_t tpg, uint64_t mva)
{
    static n48_r5_frame r; std::memset(&r, 0, sizeof r);
    r.ib_ok = r.walk_ok = r.targets_ok = r.memw_ok = r.tgts_resolved = r.memw_resolved = 1u;
    if (tva) { r.ntgt = 1u; r.tgt[0].va = tva; r.tgt[0].page = tpg; }
    if (mva) { r.nmemw = 1u; r.memw[0].va = mva; r.memw[0].page = (mva & ~0xfffull) | (1ull << 62); }
    if (!tva && !mva) { r.ntgt = 1u; r.tgt[0].va = 0x4dead0000ull; r.tgt[0].page = 0x77770000ull; }
    (void)n48_c80_held(c, &r, 0u, nullptr, 0u, 0u, kCtx, 9u);
}

// ---------------------------------------------------------------------------------------------------------------- T3 / T4
static void t3_t4()
{
    static n48_c80 c;
    two_layers(&c, nullptr);
    static n48_r5_frame r; std::memset(&r, 0, sizeof r);   // all flags 0: BLIND
    expect_u("T3 BLIND gives every layer an unknown", n48_c80_held(&c, &r, 0u, nullptr, 0u, 0u, kCtx, 5u) == 3u && c.L[0].u == 1u && c.L[1].u == 1u, 1u);
    expect_u("T3 ... and the next P of each is INCOMPLETE", judge(&c, kX, 0x10030000ull, 0u) * 0x10u + judge(&c, kXp, 0x12560000ull, 0u),
             N48_P73_C80V_INCOMPLETE * 0x11u);
    two_layers(&c, nullptr);
    r.out_of_scope = 1u;
    expect_u("T3 BOUNDED touches nothing", n48_c80_held(&c, &r, 0u, nullptr, 0u, 0u, kCtx, 5u) * 0x10u + c.L[0].u + c.L[1].u + c.L[0].h, 0u);
    expect_u("T3 no R5' record (28 OFF): every layer", n48_c80_held(&c, nullptr, 0u, nullptr, 0u, 0u, kCtx, 5u) == 3u && c.heldNoRecord == 1u, 1u);
    two_layers(&c, nullptr);
    held_readable(&c, 0u, 0u, 0u);
    std::memset(&r, 0, sizeof r); r.ib_ok = r.walk_ok = r.targets_ok = r.memw_ok = r.tgts_resolved = r.memw_resolved = 1u;
    r.ntgt = 1u; r.tgt[0].va = 0x4dead0000ull; r.tgt[0].page = 0x77770000ull;
    expect_u("T3 a READABLE frame naming no layer touches nothing; TRUNCATED targets: every layer",
             c.L[0].u + c.L[0].h + c.L[1].u + c.L[1].h == 0u && n48_c80_held(&c, &r, 1u, nullptr, 0u, 0u, kCtx, 6u) == 3u && c.heldTrunc == 1u, 1u);
    // C3's 0.0.525 finding: cyc515's write set (by VA) joins the R5' record's; not known -> every layer
    two_layers(&c, nullptr);
    std::memset(&r, 0, sizeof r); r.ib_ok = r.walk_ok = r.targets_ok = r.memw_ok = r.tgts_resolved = r.memw_resolved = 1u;
    r.ntgt = 1u; r.tgt[0].va = 0x4007e0000ull; r.tgt[0].page = 0x13aba000ull;
    const uint64_t ws[2] = { 0x4007e0000ull, kX };
    expect_u("C3+ a held-back frame whose R5' record misses X but whose cyc515 set names X: X held (counted as named only by cyc515)",
             n48_c80_held(&c, &r, 0u, ws, 2u, 0u, kCtx, 7u) * 0x100u + c.L[0].h * 0x10u + (uint32_t)c.heldWsOnly, 0x111u);
    two_layers(&c, nullptr);
    expect_u("C3+ a held-back frame whose cyc515 set is NOT known: every layer an unknown",
             n48_c80_held(&c, &r, 0u, ws, 2u, 1u, kCtx, 7u) * 0x100u + c.L[0].u * 0x10u + c.L[1].u, 0x311u);
    // T4 the range: [va, va + extent), end exclusive; ctx must match for the VA; a page matches across ctx
    two_layers(&c, nullptr);
    held_readable(&c, 0u, 0u, kX + kExtent - 4u);
    expect_u("T4 a memory destination at extent - 4 is inside (X held 1)", c.L[0].h * 0x10u + c.L[1].h, 0x10u);
    two_layers(&c, nullptr);
    held_readable(&c, 0u, 0u, kX + kExtent);
    expect_u("T4 ... at exactly extent is outside (end exclusive)", c.L[0].h + c.L[1].h, 0u);
    two_layers(&c, nullptr);
    held_readable(&c, 0u, 0u, kX - 4u);
    expect_u("T4 ... below the base is outside", c.L[0].h + c.L[1].h, 0u);
    two_layers(&c, nullptr);
    held_readable(&c, kXp + 0x1000u, 0x12561000ull, 0u);
    expect_u("T4 a colour target inside X' by VA (other page) is X''s", c.L[1].h, 1u);
    two_layers(&c, nullptr);
    expect_u("T4 another ctx's VA inside X's range is NOT X; the same physical page IS, whatever the ctx",
             n48_c80_layers_of(&c, kCtx + 1u, 0ull, kX + 0x100u) * 0x10u + n48_c80_layers_of(&c, kCtx + 1u, 0x10030000ull, 0x999000000ull), 0x01u);
}

// ---------------------------------------------------------------------------------------------------------------- T5
static void t5_lost()
{
    static n48_c80 c; static n48_p73 t; static n48_c80_q q;
    two_layers(&c, &t); std::memset(&q, 0, sizeof q);
    // a cycle of X: writer seq 10 committed, then P seq 11 judged COMPLETE into slot 0 (a plane)
    const uint64_t plane = 0x10930000ull;
    const uint32_t s0 = n48_p73_gate_p(&t, plane, 0x401800000ull, 1u, 11u);
    commit_writer(&c, kX, 0x10030000ull, 10u);
    expect_u("T5 setup: P 11 judged COMPLETE into its slot", judge(&c, kX, 0x10030000ull, 11u, &t, s0), N48_P73_C80V_COMPLETE);
    (void)n48_p73_final(&t, 11u, 1u); (void)n48_p73_retired(&t, 11u);
    // writer 10 is lost after the gate (queued on the hook's thread), drained at the next judged frame
    n48_c80_q_lost(&q, 10u);
    expect_u("T5 before the drain the slot still says COMPLETE (the queue is lock-free)", t.s[s0].c80verdict, N48_P73_C80V_COMPLETE);
    expect_u("T5 the drain applies one loss", n48_c80_drain(&c, &q, &t), 1u);
    expect_u("T5 the P slot on that layer with a NEWER seq (11 > 10) is demoted to INCOMPLETE", t.s[s0].c80verdict, N48_P73_C80V_INCOMPLETE);
    expect_u("T5 ... counted, and as LATE (the slot was already COMMITTED: a copy may have been made)", c.demoted * 0x10u + c.demotedLate, 0x11u);
    uint32_t why = 0u, sl = 0u;
    expect_u("T5 ON: the demoted P's present is HELD, reason INCOMPLETE", n48_p73_should_copy(1u, &t, plane, &why, &sl) * 0x10u + why,
             N48_P73_HOLD_INCOMPLETE);
    expect_u("T5 X got the unknown, X' did not", c.L[0].u * 0x10u + c.L[1].u, 0x10u);
    // an OLDER P on the layer is not demoted
    two_layers(&c, &t); std::memset(&q, 0, sizeof q);
    const uint32_t s1 = n48_p73_gate_p(&t, plane, 0x401800000ull, 1u, 20u);
    commit_writer(&c, kX, 0x10030000ull, 19u);
    (void)judge(&c, kX, 0x10030000ull, 20u, &t, s1);
    commit_writer(&c, kX, 0x10030000ull, 21u);
    n48_c80_q_lost(&q, 21u); (void)n48_c80_drain(&c, &q, &t);
    expect_u("T5 a loss NEWER than the P (21 > 20) does not demote it; it dirties the next cycle", t.s[s1].c80verdict * 0x10u + c.L[0].u,
             N48_P73_C80V_COMPLETE * 0x10u + 1u);
    // a seq not in the ring: every layer
    n48_c80_q_lost(&q, 999u); (void)n48_c80_drain(&c, &q, &t);
    expect_u("T5 a seq not in the ring gives EVERY layer an unknown (counted)", c.L[0].u * 0x10u + c.L[1].u + c.lostUnknown * 0x100u, 0x121u);
    // the queue's overflow
    for (uint32_t k = 0; k < N48_C80_LOSTQ + 3u; k++) n48_c80_q_lost(&q, 500u + k);
    const uint64_t lostBefore = c.lost;
    (void)n48_c80_drain(&c, &q, &t);
    expect_u("T5 the queue's overflow: every loss counted, the overflow as not-in-the-ring", c.lost - lostBefore, N48_C80_LOSTQ + 3u);
    // unjudged frames, queued
    two_layers(&c, &t); std::memset(&q, 0, sizeof q);
    n48_c80_q_unjudged(&q); n48_c80_q_unjudged(&q);
    (void)n48_c80_drain(&c, &q, &t);
    expect_u("T5 two frames the judge never saw: every layer 2 unknowns", c.L[0].u * 0x10u + c.L[1].u + c.unjudged * 0x100u, 0x222u);
}

// ---------------------------------------------------------------------------------------------------------------- T6 / T7
static void t6_t7()
{
    static n48_c80 c;
    std::memset(&c, 0, sizeof c); n48_c80_reset(&c);
    expect_u("T7 discovery: the first P of a layer is INCOMPLETE (UNKNOWN)", judge(&c, kX, 0x10030000ull, 1u), N48_P73_C80V_INCOMPLETE);
    expect_u("T7 ... the layer's state is UNKNOWN, first sight counted", c.L[0].state * 0x10u + (uint32_t)c.pFirstSight, N48_C80_ST_UNKNOWN * 0x10u + 1u);
    expect_u("T7 a P with no writer since stays UNKNOWN -> INCOMPLETE (nothing proven)", judge(&c, kX, 0x10030000ull, 2u), N48_P73_C80V_INCOMPLETE);
    commit_writer(&c, kX, 0x10030000ull, 3u);
    expect_u("T7 recovery: a cycle with a committed writer and nothing held -> COMPLETE", judge(&c, kX, 0x10030000ull, 4u), N48_P73_C80V_COMPLETE);
    // T6 sticky
    commit_writer(&c, kX, 0x10030000ull, 5u);
    held_readable(&c, kX, 0x10030000ull, 0u);
    expect_u("T6 a cycle with a held-back writer -> INCOMPLETE", judge(&c, kX, 0x10030000ull, 6u), N48_P73_C80V_INCOMPLETE);
    expect_u("T6 STICKY: the next P with NO writers stays INCOMPLETE (rule B would have cleared it)", judge(&c, kX, 0x10030000ull, 7u),
             N48_P73_C80V_INCOMPLETE);
    expect_u("T6 ... and again", judge(&c, kX, 0x10030000ull, 8u), N48_P73_C80V_INCOMPLETE);
    commit_writer(&c, kX, 0x10030000ull, 9u);
    expect_u("T6 a clean cycle recovers", judge(&c, kX, 0x10030000ull, 10u), N48_P73_C80V_COMPLETE);
    expect_u("T6 STICKY: a P with no writers after a clean cycle stays COMPLETE", judge(&c, kX, 0x10030000ull, 11u), N48_P73_C80V_COMPLETE);
    // ORDER: the judgement reads the counts BEFORE they are zeroed
    commit_writer(&c, kX, 0x10030000ull, 12u);
    held_readable(&c, kX, 0x10030000ull, 0u);
    n48_c80_pin a {}; a.ctx = kCtx; a.in_ok = 1u; a.page_ok = 1u; a.in_va = kX; a.in_page = 0x10030000ull; a.seq = 13u;
    n48_c80_pout o {};
    (void)n48_c80_p(&c, &a, nullptr, N48_P73_SLOTS, &o);
    expect_u("T6 ORDER: the P read writers 2, committed 1, held 1 and THEN the counts were zeroed",
             o.w * 0x1000u + o.c * 0x100u + o.h * 0x10u + (c.L[0].w + c.L[0].c + c.L[0].h + c.L[0].u), 0x2110u);
    // UNDET
    n48_c80_pin u {}; u.ctx = kCtx; u.in_ok = 0u;
    expect_u("C2 no D input: UNDET", n48_c80_p(&c, &u, nullptr, N48_P73_SLOTS, nullptr), N48_P73_C80V_UNDET);
    u.in_ok = 1u; u.in_va = kX; u.page_ok = 0u;
    expect_u("C2 an input that does not resolve: UNDET", n48_c80_p(&c, &u, nullptr, N48_P73_SLOTS, nullptr), N48_P73_C80V_UNDET);
    std::memset(&c, 0, sizeof c); n48_c80_reset(&c);
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) (void)judge(&c, 0x500000000ull + i * 0x1000000ull, 0x20000000ull + i * 0x1000000ull, 0u);
    expect_u("C2 a full table: the fifth layer's P is UNDET (counted), no eviction",
             judge(&c, 0x600000000ull, 0x30000000ull, 0u) * 0x10u + (uint32_t)c.pUndetFull + (c.L[0].va == 0x500000000ull ? 0x100u : 0u),
             0x100u + N48_P73_C80V_UNDET * 0x10u + 1u);
    // the P classification
    two_layers(&c, nullptr);
    expect_u("C2 a 1040-dword frame whose one target is a layer is a WRITER, not a P",
             n48_c80_is_p(&c, 1u, 1u, kCtx, 0x10030000ull, kX) * 0x10u + n48_c80_is_p(&c, 1u, 1u, kCtx, 0x10930000ull, 0x401800000ull), 0x01u);
    expect_u("C2 two targets or another shape is not a P", n48_c80_is_p(&c, 1u, 2u, kCtx, 0x10930000ull, 0x401800000ull) +
             n48_c80_is_p(&c, 0u, 1u, kCtx, 0x10930000ull, 0x401800000ull), 0u);
}

// ---------------------------------------------------------------------------------------------------------------- T8
template <class T> static void p73_run10u(T &t, std::vector<uint32_t> &ans)
{
    // run10u's own map (tests/gfx_present73_test.cpp kMap): the plane / layer VAs -> VRAM
    static const uint64_t va[6] = { 0x401800000ull, 0x402800000ull, 0x403800000ull, 0x400800000ull, 0x404800000ull, 0x400240000ull };
    static const uint64_t vo[6] = { 0x10930000ull, 0x11200000ull, 0x11a70000ull, 0x10030000ull, 0x125e0000ull, 0x111ba000ull };
    auto key = [&](uint64_t v) -> uint64_t { for (int i = 0; i < 6; i++) if (va[i] == (v & ~0xfffull)) return vo[i] + (v & 0xfffull); return 0ull; };
    for (const cy515_fx &x : kCy515Run10u) {
        n48_p73_call_begin(&t);
        const bool isP = x.captured && x.nib == 1u && x.len[0] == 1040u && x.nws == 1u;
        if (!x.captured) (void)n48_p73_after_decide(&t);
        else if (isP) {
            (void)n48_p73_gate_p(&t, key(x.ws[0]), x.ws[0], x.committed, x.committed ? x.f : 0u);
            (void)n48_p73_after_decide(&t);
            if (x.committed) { (void)n48_p73_final(&t, x.f, 1u); (void)n48_p73_retired(&t, x.f); }
        } else {
            uint64_t k[24] = {};
            for (uint32_t q = 0; q < x.nws && q < 24u; q++) k[q] = key(x.ws[q]);
            (void)n48_p73_gate_nonp(&t, k, x.nws < 8u ? x.nws : 8u);
            (void)n48_p73_after_decide(&t);
        }
        for (int p = 0; p < 3; p++) {   // a present of every plane after every frame: the widest probe of the decision
            uint32_t why = 0u, slot = 0u;
            const uint32_t c = n48_p73_should_copy(1u, &t, vo[p], &why, &slot);
            if (c && (x.f % 3u)) n48_p73_delivered(&t);   // some copies "fail" (not delivered), exercising Part E both ways
            ans.push_back(c << 16 | (c ? 0xFFu : why) << 8 | slot);
        }
    }
}
template <class T> static void p73_run11c(T &t, std::vector<uint32_t> &ans, bool withVerdicts)
{
    static n48_c80 c;
    std::memset(&c, 0, sizeof c); n48_c80_reset(&c);
    uint32_t gseq = 0u;
    for (uint32_t r = 0; r < kC80Run11cN; r++) {
        const c80_fx &x = kC80Run11c[r];
        const uint32_t seq = x.committed ? ++gseq : 0u;
        n48_p73_call_begin(&t);
        const bool shape = is_shape(x);
        if (shape && x.ntgt == 1u) (void)n48_p73_gate_p(&t, key11(x.tgt[0].va, x.tgt[0].page), x.tgt[0].va, x.committed, seq);
        else {
            uint64_t k[8] = {};
            for (uint32_t q = 0; q < x.ntgt && q < 8u; q++) k[q] = key11(x.tgt[q].va, x.tgt[q].page);
            (void)n48_p73_gate_nonp(&t, k, x.ntgt < 8u ? x.ntgt : 8u);
        }
        (void)n48_p73_after_decide(&t);
        if (x.committed) {
            if (x.lost) (void)n48_p73_final(&t, seq, 0u);
            else { (void)n48_p73_final(&t, seq, 1u); (void)n48_p73_retired(&t, seq); }
        }
        (void)withVerdicts;
        const uint64_t planes[2] = { 0x10930000ull, 0x111f0000ull };
        for (int p = 0; p < 2; p++) {
            uint32_t why = 0u, slot = 0u;
            const uint32_t cp = n48_p73_should_copy(1u, &t, planes[p], &why, &slot);
            if (cp && (x.f % 4u)) n48_p73_delivered(&t);
            ans.push_back(cp << 16 | (cp ? 0xFFu : why) << 8 | slot);
        }
    }
}
// the same run11c replay, with switch 80's verdicts WRITTEN into the slots (as if hooks had run) but c80on OFF
static void p73_run11c_verdicts(n48_p73 &t, std::vector<uint32_t> &ans)
{
    static n48_c80 c;
    std::memset(&c, 0, sizeof c); n48_c80_reset(&c);
    uint32_t gseq = 0u;
    for (uint32_t r = 0; r < kC80Run11cN; r++) {
        const c80_fx &x = kC80Run11c[r];
        const uint32_t seq = x.committed ? ++gseq : 0u;
        n48_p73_call_begin(&t);
        const bool shape = is_shape(x);
        if (shape && x.ntgt == 1u) {
            const uint32_t s = n48_p73_gate_p(&t, key11(x.tgt[0].va, x.tgt[0].page), x.tgt[0].va, x.committed, seq);
            n48_c80_pin in {}; in.ctx = kCtx; in.in_ok = x.in_ok; in.in_va = x.in_va; in.in_page = x.in_page; in.page_ok = x.in_page ? 1u : 0u;
            in.seq = seq; in.extent = kExtent;
            (void)n48_c80_p(&c, &in, &t, s, nullptr);
        } else {
            uint64_t k[8] = {};
            for (uint32_t q = 0; q < x.ntgt && q < 8u; q++) k[q] = key11(x.tgt[q].va, x.tgt[q].page);
            (void)n48_p73_gate_nonp(&t, k, x.ntgt < 8u ? x.ntgt : 8u);
        }
        (void)n48_p73_after_decide(&t);
        if (x.committed) {
            if (x.lost) (void)n48_p73_final(&t, seq, 0u);
            else { (void)n48_p73_final(&t, seq, 1u); (void)n48_p73_retired(&t, seq); }
        }
        const uint64_t planes[2] = { 0x10930000ull, 0x111f0000ull };
        for (int p = 0; p < 2; p++) {
            uint32_t why = 0u, slot = 0u;
            const uint32_t cp = n48_p73_should_copy(1u, &t, planes[p], &why, &slot);
            if (cp && (x.f % 4u)) n48_p73_delivered(&t);
            ans.push_back(cp << 16 | (cp ? 0xFFu : why) << 8 | slot);
        }
    }
}
template <class A, class B> static bool same_counters(const A &a, const B &b)
{
    bool ok = a.copied == b.copied && a.pRecorded == b.pRecorded && a.pCommitted == b.pCommitted && a.pRefused == b.pRefused &&
              a.pWithdrawn == b.pWithdrawn && a.pUnresolved == b.pUnresolved && a.overflow == b.overflow && a.nonP == b.nonP &&
              a.unjudged == b.unjudged && a.finalUnmatched == b.finalUnmatched && a.flushHeld == b.flushHeld &&
              a.pSpared == b.pSpared && a.pRetiredFirst == b.pRetiredFirst && a.retireNoSlot == b.retireNoSlot &&
              a.promoteReverted == b.promoteReverted && a.lastDelivered == b.lastDelivered && a.deliveredOk == b.deliveredOk &&
              a.decided == b.decided && a.decidedOk == b.decidedOk && a.delivered == b.delivered && a.hold_lines == b.hold_lines &&
              a.copy_lines == b.copy_lines && a.pend_seq == b.pend_seq && a.judged == b.judged;
    for (int h = 0; h < 5; h++) ok = ok && a.held[h] == b.held[h];
    for (int i = 0; i < 8; i++)
        ok = ok && a.s[i].key == b.s[i].key && a.s[i].va == b.s[i].va && a.s[i].state == b.s[i].state && a.s[i].seq == b.s[i].seq &&
             a.s[i].spared == b.s[i].spared && a.s[i].retired == b.s[i].retired;
    return ok;
}
static void t8_off_identity()
{
    static n48_p73 cur; static p524::n48_p73 old;
    std::vector<uint32_t> a, b;
    std::memset(&cur, 0, sizeof cur); n48_p73_reset(&cur);
    std::memset(&old, 0, sizeof old); p524::n48_p73_reset(&old);
    p73_run10u(cur, a); p73_run10u(old, b);
    expect_u("T8 run10u: 80 OFF gives every present the frozen 0.0.524 answer, reason and slot", a == b && a.size() == 171u * 3u, 1u);
    expect_u("T8 run10u: every switch-73 counter and slot equals 0.0.524's; nothing of 80 moved",
             same_counters(cur, old) && cur.held[N48_P73_HOLD_INCOMPLETE] == 0u && cur.c80WouldHold == 0u, 1u);
    a.clear(); b.clear();
    std::memset(&cur, 0, sizeof cur); n48_p73_reset(&cur);
    std::memset(&old, 0, sizeof old); p524::n48_p73_reset(&old);
    p73_run11c(cur, a, false); p73_run11c(old, b, false);
    expect_u("T8 run11c: 80 OFF gives every present the frozen 0.0.524 answer, reason and slot", a == b && !a.empty(), 1u);
    expect_u("T8 run11c: every switch-73 counter and slot equals 0.0.524's", same_counters(cur, old) && cur.c80WouldHold == 0u, 1u);
    uint32_t copies = 0u;
    for (uint32_t v : a) copies += v >> 16;
    expect_u("T8 run11c: the replay copies (not vacuous)", copies > 20u, 1u);
    // verdicts present in the slots (an earlier ON window) but the mode OFF: still 0.0.524, byte for byte
    a.clear();
    std::memset(&cur, 0, sizeof cur); n48_p73_reset(&cur);
    p73_run11c_verdicts(cur, a);
    expect_u("T8 run11c with switch 80's verdicts in the slots and c80on OFF: still 0.0.524's answers", a == b, 1u);
    expect_u("T8 ... and counters", same_counters(cur, old) && cur.held[N48_P73_HOLD_INCOMPLETE] == 0u, 1u);
    expect_u("T8 the boot value: a zeroed n48_p73 is 80 OFF (N48_P73_C80_OFF == 0)", N48_P73_C80_OFF, 0u);
    expect_u("T8 the appended hold reason keeps 0..4 (REFUSED WITHDRAWN UNKNOWN NO-MATCH OLDER) and the names",
             N48_P73_HOLD_OLDER == p524::N48_P73_HOLD_OLDER && N48_P73_HOLD_NOMATCH == p524::N48_P73_HOLD_NOMATCH &&
             std::strcmp(n48_p73_hold_name(N48_P73_HOLD_OLDER), p524::n48_p73_hold_name(p524::N48_P73_HOLD_OLDER)) == 0 &&
             std::strcmp(n48_p73_hold_name(N48_P73_HOLD_INCOMPLETE), "INCOMPLETE") == 0 &&
             std::strcmp(n48_p73_hold_name(N48_P73_HOLDS), "COPY") == 0, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T9 / T10
static void t9_wrap()
{
    static n48_c80 c; static n48_p73 t; static n48_c80_q q;
    two_layers(&c, &t); std::memset(&q, 0, sizeof q);
    commit_writer(&c, kX, 0x10030000ull, 0xFFFFFFFFu);
    uint32_t m = 0u;
    expect_u("T9 the ring finds 0xFFFFFFFF", n48_c80_ring_find(&c, 0xFFFFFFFFu, &m) * 0x10u + m, 0x11u);
    const uint32_t s0 = n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 1u);
    expect_u("T9 P seq 1 (after the wrap) judged COMPLETE", judge(&c, kX, 0x10030000ull, 1u, &t, s0), N48_P73_C80V_COMPLETE);
    n48_c80_q_lost(&q, 0xFFFFFFFFu); (void)n48_c80_drain(&c, &q, &t);
    expect_u("T9 losing writer 0xFFFFFFFF demotes P 1 (newer across the wrap)", t.s[s0].c80verdict, N48_P73_C80V_INCOMPLETE);
    two_layers(&c, &t);
    const uint32_t s1 = n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 0xFFFFFFFEu);
    commit_writer(&c, kX, 0x10030000ull, 0xFFFFFFFDu);
    (void)judge(&c, kX, 0x10030000ull, 0xFFFFFFFEu, &t, s1);
    commit_writer(&c, kX, 0x10030000ull, 2u);
    n48_c80_q_lost(&q, 2u); (void)n48_c80_drain(&c, &q, &t);
    expect_u("T9 losing writer 2 (after the wrap) does NOT demote P 0xFFFFFFFE (older)", t.s[s1].c80verdict, N48_P73_C80V_COMPLETE);
    // the present's equality across the wrap
    (void)n48_p73_final(&t, 0xFFFFFFFEu, 1u); (void)n48_p73_retired(&t, 0xFFFFFFFEu);
    uint32_t why = 0u, sl = 0u;
    expect_u("T9 the present copies P 0xFFFFFFFE judged COMPLETE (c80seq == seq)", n48_p73_should_copy(1u, &t, 0x10930000ull, &why, &sl), 1u);
    two_layers(&c, &t);
    n48_c80_ring_push(&c, 0u, 1u);
    expect_u("T9 seq 0 is never recorded or found", c.ringN * 0x10u + n48_c80_ring_find(&c, 0u, &m), 0u);
}
static void t10_slots()
{
    replay11(gSwap, { N48_P73_C80_ON, true, kExtent, 70u, false, false });
    uint32_t same = gSwap.pout.size() == gOn.pout.size() ? 1u : 0u, swapped = 0u;
    for (const auto &kv : gOn.pout) {
        const n48_c80_pout &s = gSwap.pout[kv.first];
        if (s.verdict != kv.second.verdict || s.layer != kv.second.layer) same = 0u;
    }
    for (uint32_t r = 0; r < kC80Run11cN; r++) if (kC80Run11c[r].f >= 70u && kC80Run11c[r].in_ok) swapped++;
    expect_u("T10 descriptor slots 1 and 7 swapped from F70 on (the boot-to-boot swap): the same verdicts and layers",
             same * 0x100u + (swapped > 10u ? 1u : 0u), 0x101u);
    expect_u("T10 the fixture's P really do use slot 1 for X and 7 for X' (the swap is not vacuous)",
             kC80Run11c[23].f == 24u && kC80Run11c[23].dslot == 1u && kC80Run11c[23].in_va == kX &&
             kC80Run11c[16].f == 17u && kC80Run11c[16].dslot == 7u && kC80Run11c[16].in_va == kXp, 1u);
    expect_u("T10 ON and SHADOW with the swap: the whole n48_c80 equal to the unswapped run", std::memcmp(&gSwap.c, &gOn.c, sizeof gOn.c) == 0, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T11 / T12
static void t11_order()
{
    static n48_p73 t;
    std::memset(&t, 0, sizeof t); n48_p73_reset(&t); t.c80on = N48_P73_C80_ON;
    const uint64_t A = 0x10930000ull, B = 0x111f0000ull;
    const uint32_t sa = n48_p73_gate_p(&t, A, 0x401800000ull, 1u, 10u);
    n48_p73_st32(&t.s[sa].c80seq, 10u); n48_p73_st32(&t.s[sa].c80verdict, N48_P73_C80V_COMPLETE);
    (void)n48_p73_final(&t, 10u, 1u); (void)n48_p73_retired(&t, 10u);
    uint32_t why = 0u, sl = 0u;
    expect_u("T11 a COMPLETE committed P copies", n48_p73_should_copy(1u, &t, A, &why, &sl), 1u);
    n48_p73_delivered(&t);
    // B: committed, OLDER than the glass (5 < 10) AND INCOMPLETE -> the reason is INCOMPLETE (C6 comes before Part E)
    const uint32_t sb = n48_p73_gate_p(&t, B, 0x402800000ull, 1u, 5u);
    n48_p73_st32(&t.s[sb].c80seq, 5u); n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_INCOMPLETE);
    (void)n48_p73_final(&t, 5u, 1u); (void)n48_p73_retired(&t, 5u);
    const uint64_t copied0 = t.copied; const uint32_t dec0 = t.decided, decOk0 = t.decidedOk;
    expect_u("T11 ORDER: an INCOMPLETE P that is also OLDER is held as INCOMPLETE (before Part E's newer-than)",
             n48_p73_should_copy(1u, &t, B, &why, &sl) * 0x10u + why, N48_P73_HOLD_INCOMPLETE);
    expect_u("T11 ... held[INCOMPLETE] 1, held[OLDER] 0", t.held[N48_P73_HOLD_INCOMPLETE] * 0x10u + t.held[N48_P73_HOLD_OLDER], 0x10u);
    // B newer and INCOMPLETE: held; nothing of Part E moved
    (void)n48_p73_gate_p(&t, B, 0x402800000ull, 1u, 11u);
    n48_p73_st32(&t.s[sb].c80seq, 11u); n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_INCOMPLETE);
    (void)n48_p73_final(&t, 11u, 1u); (void)n48_p73_retired(&t, 11u);
    expect_u("T11 a NEWER INCOMPLETE P is held INCOMPLETE", n48_p73_should_copy(1u, &t, B, &why, &sl) * 0x10u + why, N48_P73_HOLD_INCOMPLETE);
    expect_u("T11 a hold sets nothing of Part E: copied, decided, decidedOk unchanged; the glass stays at 10",
             t.copied == copied0 && t.decided == dec0 && t.decidedOk == decOk0 && t.lastDelivered == 10u, 1u);
    n48_p73_delivered(&t);
    expect_u("T11 a delivered() after an INCOMPLETE hold moves nothing (no decision stands)", t.lastDelivered * 0x10u + (uint32_t)t.delivered, 10u * 0x10u + 1u);
    // a verdict for ANOTHER P (c80seq != seq) is not this P's: held
    n48_p73_st32(&t.s[sb].c80seq, 10u); n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_COMPLETE);
    expect_u("T11 a COMPLETE verdict for another seq is not this P's: held INCOMPLETE", n48_p73_should_copy(1u, &t, B, &why, &sl) * 0x10u + why,
             N48_P73_HOLD_INCOMPLETE);
    n48_p73_st32(&t.s[sb].c80seq, 11u); n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_UNDET);
    expect_u("T11 UNDET is not COMPLETE: held", n48_p73_should_copy(1u, &t, B, &why, &sl) * 0x10u + why, N48_P73_HOLD_INCOMPLETE);
    n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_NONE);
    expect_u("T11 a P never judged (verdict none) is held", n48_p73_should_copy(1u, &t, B, &why, &sl), 0u);
    n48_p73_st32(&t.s[sb].c80verdict, N48_P73_C80V_COMPLETE);
    expect_u("T11 its own COMPLETE verdict copies (newer than 10)", n48_p73_should_copy(1u, &t, B, &why, &sl), 1u);
    // SHADOW: the same slot states, never held INCOMPLETE, counted
    static n48_p73 s; std::memcpy(&s, &t, sizeof s); s.c80on = N48_P73_C80_SHADOW;
    n48_p73_st32(&s.s[sb].c80verdict, N48_P73_C80V_INCOMPLETE);
    s.deliveredOk = 0u;
    const uint64_t wh = s.c80WouldHold;
    expect_u("T11 SHADOW: an INCOMPLETE P copies and counts 'would hold'", n48_p73_should_copy(1u, &s, B, &why, &sl) * 0x10u + (uint32_t)(s.c80WouldHold - wh), 0x11u);
    s.c80on = 7u;
    expect_u("T11 an unknown mode value holds (fail-closed)", n48_p73_should_copy(1u, &s, B, &why, &sl) * 0x10u + why, N48_P73_HOLD_INCOMPLETE);
}
static void t12_cross_and_verb()
{
    static n48_c80 c;
    two_layers(&c, nullptr);
    commit_writer(&c, kXp, 0x12560000ull, 1u); (void)judge(&c, kXp, 0x12560000ull, 2u);   // X' CLEAN
    held_readable(&c, kX, 0x10030000ull, 0u); (void)judge(&c, kX, 0x10030000ull, 3u);      // X DIRTY
    // a committed writer of X' that reads X (not CLEAN)
    n48_c80_frame_begin(&c);
    const uint64_t rd[2] = { kX + 0x40u, 0x400240000ull };
    n48_c80_note_reads(&c, rd, nullptr, 2u, 0u);
    commit_writer(&c, kXp, 0x12560000ull, 4u);
    expect_u("C5 cross: a writer of X' reading X (DIRTY) gives X' an unknown -> its P INCOMPLETE",
             (uint32_t)c.crossDirty * 0x10u + judge(&c, kXp, 0x12560000ull, 5u), 0x10u + N48_P73_C80V_INCOMPLETE);
    // reading its OWN layer is fine
    two_layers(&c, nullptr);
    n48_c80_frame_begin(&c);
    const uint64_t own[1] = { kXp + 0x80u };
    n48_c80_note_reads(&c, own, nullptr, 1u, 0u);
    commit_writer(&c, kXp, 0x12560000ull, 6u);
    expect_u("C5 cross: reading its own layer is not a cross read", (uint32_t)c.crossDirty * 0x10u + judge(&c, kXp, 0x12560000ull, 7u),
             N48_P73_C80V_COMPLETE);
    // reads unknown (the translator's list overflowed)
    two_layers(&c, nullptr);
    n48_c80_frame_begin(&c);
    n48_c80_note_reads(&c, own, nullptr, 1u, 1u);
    commit_writer(&c, kXp, 0x12560000ull, 8u);
    expect_u("C5 cross: a writer whose reads are not known gives its layer an unknown (counted)",
             (uint32_t)c.crossUnknown * 0x10u + judge(&c, kXp, 0x12560000ull, 9u), 0x10u + N48_P73_C80V_INCOMPLETE);
    // FIX PASS MF-1: 20 distinct NON-layer inputs are not "reads unknown" (no capped list): the writer's cycle can be CLEAN
    two_layers(&c, nullptr);
    n48_c80_frame_begin(&c);
    uint64_t many[20];
    for (uint32_t k = 0; k < 20u; k++) many[k] = 0x700000000ull + k * 0x1000u;
    n48_c80_note_reads(&c, many, nullptr, 20u, 0u);
    expect_u("MF-1 20 distinct non-layer inputs: no unknown reads, no layer read", c.rdOver * 0x10u + c.rdMask, 0u);
    commit_writer(&c, kXp, 0x12560000ull, 10u);
    expect_u("MF-1 ... and that writer's layer reaches COMPLETE", (uint32_t)c.crossUnknown * 0x10u + judge(&c, kXp, 0x12560000ull, 11u),
             N48_P73_C80V_COMPLETE);
    // FIX PASS MF-1: two layers that start UNKNOWN and read each other reach CLEAN after a complete cycle (no deadlock)
    two_layers(&c, nullptr);
    expect_u("MF-1 setup: both layers UNKNOWN", c.L[0].state * 0x10u + c.L[1].state, 0u);
    n48_c80_frame_begin(&c);
    const uint64_t rXp[1] = { kXp + 0x40u };
    n48_c80_note_reads(&c, rXp, nullptr, 1u, 0u);
    commit_writer(&c, kX, 0x10030000ull, 12u);          // a writer of X reading X' (UNKNOWN)
    n48_c80_frame_begin(&c);
    const uint64_t rX[1] = { kX + 0x40u };
    n48_c80_note_reads(&c, rX, nullptr, 1u, 0u);
    commit_writer(&c, kXp, 0x12560000ull, 13u);         // a writer of X' reading X (UNKNOWN)
    expect_u("MF-1 two UNKNOWN layers reading each other: both P COMPLETE after a complete cycle",
             judge(&c, kX, 0x10030000ull, 14u) * 0x10u + judge(&c, kXp, 0x12560000ull, 15u), N48_P73_C80V_COMPLETE * 0x11u);
    expect_u("MF-1 ... and nothing counted as a cross read", c.crossDirty + c.crossUnknown, 0u);
    n48_c80_frame_begin(&c);
    expect_u("C5 cross: the frame top clears the reads", c.rdOver + c.rdMask + c.nPair, 0u);
    // FIX PASS MF-3: the P's input is its OWN count of plane-shaped pairs: exactly one, else UNDET
    {
        n48_c80_frame_begin(&c);
        const uint64_t p1[2] = { kX, 0x400240000ull }, p2[2] = { kXp, 0x400240000ull }; const uint32_t pm[2] = { 3u, 0u };
        n48_c80_note_reads(&c, p1, pm, 2u, 0u);
        uint64_t iv = 0ull;
        expect_u("MF-3 one plane-shaped pair: the P's input is its tiled VA", n48_c80_p_input(&c, &iv) * 0x10u + (iv == kX ? 1u : 0u), 0x11u);
        n48_c80_note_reads(&c, p2, pm, 2u, 0u);
        expect_u("MF-3 TWO plane-shaped pairs in the frame: no input (UNDET)", n48_c80_p_input(&c, &iv) * 0x10u + (iv ? 1u : 0u), 0u);
        n48_c80_pin a {}; a.ctx = kCtx; a.in_ok = n48_c80_p_input(&c, &a.in_va); a.page_ok = 1u; a.in_page = 0x10030000ull;
        expect_u("MF-3 ... so the P is UNDETERMINED (held)", n48_c80_p(&c, &a, nullptr, N48_P73_SLOTS, nullptr), N48_P73_C80V_UNDET);
        n48_c80_frame_begin(&c);
        const uint32_t pmBoth[2] = { 3u, 3u };
        n48_c80_note_reads(&c, p1, pmBoth, 2u, 0u);
        n48_c80_note_reads(&c, p1, pm, 2u, 1u);
        expect_u("MF-3 a pair of two tiled, or an overflowing list, is not plane-shaped", c.nPair, 0u);
    }
    // FIX PASS MF-2: the verdict counts only when the seq read on both sides of it is this P's
    expect_u("MF-2 (s1, v, s2) = (seq, COMPLETE, seq) is complete", n48_p73_c80_complete(9u, N48_P73_C80V_COMPLETE, 9u, 9u), 1u);
    expect_u("MF-2 a torn read (seq, COMPLETE, 0: the writer began a new P between the loads) is NOT",
             n48_p73_c80_complete(9u, N48_P73_C80V_COMPLETE, 0u, 9u) + n48_p73_c80_complete(0u, N48_P73_C80V_COMPLETE, 9u, 9u) +
             n48_p73_c80_complete(9u, N48_P73_C80V_COMPLETE, 10u, 9u) + n48_p73_c80_complete(0u, N48_P73_C80V_COMPLETE, 0u, 0u) +
             n48_p73_c80_complete(9u, N48_P73_C80V_INCOMPLETE, 9u, 9u), 0u);
    // FIX PASS (SHOULD): eviction after 16 P without a sample
    {
        two_layers(&c, nullptr);
        for (uint32_t k = 0; k < N48_C80_EVICT_P - 1u; k++) (void)judge(&c, kX, 0x10030000ull, 0u);
        expect_u("EVICT X' survives 15 P without a sample", c.L[1].used * 0x10u + (uint32_t)c.evicted, 0x10u);
        (void)judge(&c, kX, 0x10030000ull, 0u);
        expect_u("EVICT ... and is evicted at the 16th (counted); X stays", c.L[1].used * 0x100u + (uint32_t)c.evicted * 0x10u + c.L[0].used, 0x11u);
        expect_u("EVICT a rediscovered X' starts UNKNOWN (INCOMPLETE)", judge(&c, kXp, 0x12560000ull, 0u), N48_P73_C80V_INCOMPLETE);
    }
    // FIX PASS (SHOULD): committed writers are credited from the union (cyc515's set too)
    {
        two_layers(&c, nullptr);
        const uint64_t v[1] = { 0x401800000ull }, pg[1] = { 0x10930000ull }; const uint32_t rs[1] = { 1u };
        const uint64_t w1[1] = { kXp };
        expect_u("UNION a committed frame is credited from cyc515's set too", n48_c80_writer(&c, kCtx, v, pg, rs, 1u, w1, 1u, 1u, 40u) * 0x10u + c.L[1].c, 0x21u);
    }
    // the verb's M
    expect_u("VERB M 1 ON, 2 OFF, 3 SHADOW, 0 and 4 refused (0xFF)",
             n48_c80_mode_of_m(1u) == N48_P73_C80_ON && n48_c80_mode_of_m(2u) == N48_P73_C80_OFF &&
             n48_c80_mode_of_m(3u) == N48_P73_C80_SHADOW && n48_c80_mode_of_m(0u) == 0xFFu && n48_c80_mode_of_m(4u) == 0xFFu, 1u);
    expect_u("VERB the values: 336 ON, 592 OFF, 848 SHADOW", (80u | 1u << 8) == 336u && (80u | 2u << 8) == 592u && (80u | 3u << 8) == 848u, 1u);
    // the verb's reset is queued and drained BEFORE anything is judged
    static n48_c80_q q; std::memset(&q, 0, sizeof q);
    two_layers(&c, nullptr);
    n48_c80_q_reset(&q);
    (void)n48_c80_drain(&c, &q, nullptr);
    expect_u("RESET the queued reset clears the table at the drain", n48_c80_all(&c), 0u);
    // T12 read-unknown over run11c: every committed writer's reads unknown -> a freeze (for the review: the cost of in_over)
    static Rep11 ru;
    replay11(ru, { N48_P73_C80_ON, true, kExtent, 0u, true, false });
    uint32_t comp = 0u;
    for (const auto &kv : ru.pout) if (kv.second.verdict == N48_P73_C80V_COMPLETE) comp++;
    expect_u("C5 cross (read-unknown everywhere, a stress): run11c has no COMPLETE P left", comp, 0u);
}

// ---------------------------------------------------------------------------------------------------------------- T13 (X4)
static void t13_lines()
{
    const uint64_t M = ~0ull; const uint32_t U = ~0u;
    char b[1024];
    int n;
    n = std::snprintf(b, sizeof b, N48_C80_FMT, "SHADOW", " - `gfxneuter 80` REFUSED: a continuous arm stands, unchanged",
                      "; INERT: switch 73 is OFF (no present is held)", "; 28 is OFF: every held-back frame gives every layer an unknown",
                      M, M, M, M, M, M);
    std::printf("      N48_C80_FMT max %d\n", n);
    expect_u("X4 the report line at maximal fields fits the 491-byte body", n <= 491, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_FMT2, M, M, M, M, U, M, M, M, M, M, M, M, M);
    std::printf("      N48_C80_FMT2 max %d\n", n);
    expect_u("X4 the second report line at maximal fields fits", n <= 491, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_FMT3, M, M, M, M, M, M, M, M, M, M, M, M, M);
    std::printf("      N48_C80_FMT3 max %d\n", n);
    expect_u("X4 the third report line at maximal fields fits", n <= 491, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_EVICT_FMT, M, M, U, M, M);
    std::printf("      N48_C80_EVICT_FMT max %d\n", n);
    expect_u("X4 the eviction line at maximal fields fits", n <= 491, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_LAYER_FMT, U, M, M, M, M, "UNKNOWN", U, U, U, U, M, M, M, M, M);
    std::printf("      N48_C80_LAYER_FMT max %d\n", n);
    expect_u("X4 the layer line at maximal fields fits", n <= 491, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_LINE_FMT, "SHADOW", M, U, M, U, M, M, "INCOMPLETE", U, U, U, M, U, "UNKNOWN");
    std::printf("      N48_C80_LINE_FMT max %d\n", n);
    expect_u("X4 the per-P line at maximal fields fits", n <= 491, 1u);
    char ent[480]; size_t used = 0u;
    for (int k = 0; k < 8; k++) used += (size_t)std::snprintf(ent + used, sizeof ent - used, N48_C80_COPY_ENT, M, M, U, 'B', 'I');
    n = std::snprintf(b, sizeof b, N48_C80_COPY_FMT, M, M, ent);
    std::printf("      N48_C80_COPY_FMT max %d (8 entries %zu)\n", n, used);
    expect_u("X4 the copy dump line with 8 maximal entries fits (and the 480-byte buffer holds 8)", n <= 491 && used < 480u, 1u);
    n = std::snprintf(b, sizeof b, N48_C80_SUM_FMT, "SHADOW", M, M, M, M, U, '?', "INCOMPLETE", M, M);
    std::printf("      N48_C80_SUM_FMT max %d\n", n);
    expect_u("X4 the copy summary line at maximal fields fits", n <= 491, 1u);
    // the per-P line cap: 16 first, then INCOMPLETE / changes only, 1 per second, 64 in all
    static n48_c80 c; std::memset(&c, 0, sizeof c);
    uint32_t got = 0u;
    for (int k = 0; k < 16; k++) got += n48_c80_line_due(&c, N48_P73_C80V_COMPLETE, 0u, 1000u);
    expect_u("X4 the first 16 per-P lines are unconditional (even COMPLETE, same instant)", got, 16u);
    expect_u("X4 then a COMPLETE with no state change is not logged", n48_c80_line_due(&c, N48_P73_C80V_COMPLETE, 0u, 5000000u), 0u);
    expect_u("X4 an INCOMPLETE is, once per second", n48_c80_line_due(&c, N48_P73_C80V_INCOMPLETE, 0u, 5000000u) * 0x10u +
             n48_c80_line_due(&c, N48_P73_C80V_INCOMPLETE, 0u, 5500000u), 0x10u);
    expect_u("X4 a state change after a second is", n48_c80_line_due(&c, N48_P73_C80V_COMPLETE, 1u, 6000001u), 1u);
    expect_u("X4 a clock of 0 refuses past the first 16", n48_c80_line_due(&c, N48_P73_C80V_INCOMPLETE, 1u, 0u), 0u);
    uint64_t now = 10000000u;
    for (int k = 0; k < 200; k++) { now += 1000001u; (void)n48_c80_line_due(&c, N48_P73_C80V_INCOMPLETE, 1u, now); }
    expect_u("X4 at most 64 per-P lines in all", c.lines, 64u);
    // the copy summary: past the first 8 copies, one per 2 s
    static n48_c80_copies cp; std::memset(&cp, 0, sizeof cp);
    uint32_t sums = 0u;
    for (uint32_t k = 0; k < 8u; k++) { n48_c80_copy_note(&cp, k, 0x10930000u, k, 'A', N48_P73_C80V_COMPLETE); sums += n48_c80_copy_sum_due(&cp, 1000000u + k); }
    expect_u("X4 no summary within the first 8 copies", sums, 0u);
    n48_c80_copy_note(&cp, 8u, 0x10930000u, 8u, 'B', N48_P73_C80V_COMPLETE);
    expect_u("X4 the 9th copy may summarise; again only 2 s later", n48_c80_copy_sum_due(&cp, 3000000u) * 0x100u +
             n48_c80_copy_sum_due(&cp, 4999999u) * 0x10u + n48_c80_copy_sum_due(&cp, 5000000u), 0x101u);
    for (uint32_t k = 9u; k < 70u; k++) n48_c80_copy_note(&cp, k, 0x10930000u, k, 'A', N48_P73_C80V_INCOMPLETE);
    expect_u("X4 the copy ring keeps the last 64 (entry 69 at slot 69 % 64)", cp.n * 0x100u + cp.r[69u % N48_C80_COPIES].present, 70u * 0x100u + 69u);
}

// ---------------------------------------------------------------------------------------------------------------- T14 glue
static void t14_glue(const char *ahhPath, const char *bringupPath)
{
    const std::string s = slurp(ahhPath), br = slurp(bringupPath);
    std::string hp = ahhPath ? ahhPath : "";
    const size_t sl = hp.rfind('/');
    const std::string dir = sl == std::string::npos ? std::string() : hp.substr(0, sl + 1);
    const std::string h = slurp((dir + "gfx_present73.h").c_str()), hc = slurp((dir + "gfx_cycle80.h").c_str());
    expect_u("GLUE the sources read (AppleHardwareHook.cpp, Navi48Bringup.cpp, gfx_present73.h, gfx_cycle80.h)",
             !s.empty() && !br.empty() && !h.empty() && !hc.empty(), 1u);
    if (s.empty() || h.empty()) return;
    // the mode: one writer (the verb), the boot value 0 = OFF
    expect_u("GLUE the mode (gP73.c80on) is written in exactly one place, the verb, and never assigned directly",
             count(s, "n48_p73_st32(&gP73.c80on, want80);") == 1u && count(s, "gP73.c80on =") == 0u &&
             count(s, "&gP73.c80on") == 2u && count(s, "static inline uint32_t c80_mode() { return __atomic_load_n(&gP73.c80on, __ATOMIC_ACQUIRE); }") == 1u, 1u);
    expect_u("GLUE gP73 is zero-initialised at file scope (80 OFF at boot)", count(s, "static n48_p73 gP73 {};"), 1u);
    // the verb
    const size_t v = s.find("    } else if ((arg & 0xffull) == 80ull) {");
    const size_t vEnd = s.find("    } else if ((arg & 0xffull) == 59ull) {", v);
    const std::string vb = (v != std::string::npos && vEnd != std::string::npos) ? s.substr(v, vEnd - v) : std::string();
    expect_u("GLUE the verb: one selector `== 80ull`, guarded, M through n48_c80_mode_of_m, OFF->ON/SHADOW clears the slots and queues the reset BEFORE the mode is published",
             count(s, "(arg & 0xffull) == 80ull") == 1u && !vb.empty() &&
             vb.find("n48_cm_cont_switch_refused(80u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") != std::string::npos &&
             vb.find("const uint32_t want80 = n48_c80_mode_of_m(m);") != std::string::npos &&
             vb.find("if (want80 != N48_P73_C80_OFF && c80_mode() == N48_P73_C80_OFF) {") < vb.find("n48_c80_q_reset(&gC80Q);") &&
             vb.find("n48_c80_q_reset(&gC80Q);") < vb.find("n48_p73_st32(&gP73.c80on, want80);") &&
             vb.find("n48_p73_st32(&gP73.s[i].c80verdict, N48_P73_C80V_NONE);") < vb.find("n48_p73_st32(&gP73.c80on, want80);") &&
             vb.find("if (!contRefused80 && !changed80 && m != 0u) st = 11;") != std::string::npos &&
             vb.find("c80_report_line(") != std::string::npos, 1u);
    // C6 in the header: before Part E, one load while OFF
    const size_t pp = h.find("static inline uint32_t n48_p73_present(n48_p73 *t, uint64_t key, uint32_t *reason, uint32_t *slot)");
    const size_t c6 = h.find("            if (n48_p73_c80_hold(t, &t->s[i], seq)) why = N48_P73_HOLD_INCOMPLETE;", pp);
    const size_t pe = h.find("            else if (!t->deliveredOk || n48_p73_seq_newer(seq, t->lastDelivered)) {", pp);
    expect_u("GLUE C6: n48_p73_present asks n48_p73_c80_hold FIRST for a COMMITTED slot, then Part E's newer-than (ORDER), once",
             pp != std::string::npos && c6 != std::string::npos && pe != std::string::npos && c6 < pe &&
             count(h, "n48_p73_c80_hold(t, &t->s[i], seq)") == 1u, 1u);
    expect_u("GLUE C6: OFF is one load of c80on and a return",
             count(h, "    const uint32_t on = n48_p73_ld32(&t->c80on);\n    if (on == N48_P73_C80_OFF) return 0u;") == 1u &&
             count(h, "t->c80on") == 1u, 1u);
    // the judge's hooks, each gated on the mode, in decide_frame, in order
    const size_t dec = s.find("static uint32_t gfxsrc_decide_frame(");
    const size_t decEnd = s.find("\n}\n", dec);
    auto in_dec = [&](const char *n) { const size_t a = s.find(n, dec); return a != std::string::npos && a < decEnd ? a : std::string::npos; };
    const size_t fb = in_dec("    if (c80_mode()) c80_frame_begin();");
    const size_t cy = in_dec("    cy515_begin(0u);");
    const size_t act = in_dec("    const uint32_t action = n48_sd_action(arm, shapeOk, verdict, commitOk);\n");
    const size_t pj = in_dec("        p73_judge(hVa, hPage, hRes, hSys);\n");
    const size_t cj = in_dec("        c80_judge(hVa, hPage, hRes, vm.ok ? &vm : nullptr);\n");
    const size_t cjIf = in_dec("    if (c80_mode()) {\n        gC80J.isShape = ");
    expect_u("GLUE the frame top clears the reads (gated), right after cy515_begin", fb != std::string::npos && cy != std::string::npos && fb > cy &&
             count(s, " c80_frame_begin(") == 2u, 1u);
    expect_u("GLUE c80_judge: once, gated, inside gfxsrc_decide_frame AFTER the gate's action and AFTER switch 73's p73_judge",
             cj != std::string::npos && cjIf != std::string::npos && act < pj && pj < cjIf && cjIf < cj && count(s, "c80_judge(") == 2u, 1u);
    expect_u("GLUE c80_judge drains the queue FIRST, then judges (the loss / unjudged / reset before any P)",
             count(s, "    __atomic_store_n(&gC80Judged, 1u, __ATOMIC_RELAXED);\n    (void)n48_c80_drain(&gC80, &gC80Q, &gP73);\n") == 1u, 1u);
    expect_u("GLUE C2 + MF-3: the P's input is 80's OWN count of plane-shaped pairs (exactly one), resolved through the P's own vm; never a slot",
             count(s, "    in.in_ok = n48_c80_p_input(&gC80, &in.in_va);") == 1u && count(s, "gCy515.in_layer") == 0u && count(s, "gCy515.in_ok") == 0u &&
             count(s, "        if (c80_mode() && ds.in_abi && !cgRefused) c80_inputs(ds.in_va, ds.in_mode, ds.in_n, ds.in_over);") == 1u &&
             count(s, "gfxc_page_sub(N48_MMT_D_TGT, *vm, in.in_va & ~0xfffull, pg, sys);") == 1u && count(s, "in.dslot = N48_C80_NOSLOT;") == 1u, 1u);
    const size_t rr = in_dec("                n48_r5_resolve(&r5f, r5_resolve_cb, &r5c);\n                if (r5ShortRead) r5f.ib_ok = 0u;\n");
    const size_t hb = in_dec("                if (c80_mode()) c80_heldback(&r5f, tgtN > hHeld ? 1u : 0u, fctx);");
    const size_t nc = in_dec("                n48_r5_note_counted(&gR5Ring, gXpScopeSeq, &r5f, gXdC.judged + 1u,");
    const size_t hbNo = in_dec("        if (c80_mode() && !gXpOn) c80_heldback(nullptr, 1u, 0ull);");
    const size_t notTr = in_dec("    if (action != N48_SD_ACT_TRANSLATE) {\n        for (uint32_t q = 0; q < hHeld; q++) n48_dep_note(");
    const size_t wsl = in_dec("    // 0.0.363: THE PER-FRAME KEY AND TIME,");
    expect_u("GLUE C3 held-back: AFTER n48_r5_resolve (and the short-read mark), BEFORE the note; truncated = tgtN > hHeld",
             rr != std::string::npos && hb != std::string::npos && nc != std::string::npos && rr < hb && hb < nc, 1u);
    expect_u("GLUE C3 28 OFF: a held-back frame with no R5' record gives every layer an unknown, inside the held-back block",
             notTr != std::string::npos && hbNo != std::string::npos && wsl != std::string::npos && notTr < hbNo && hbNo < wsl &&
             count(s, "c80_heldback(") == 3u, 1u);
    expect_u("GLUE C5 reads: the policy's input list, gated, beside cyc515's",
             count(s, "        if (ds.in_abi && !cgRefused) cy515_inputs(ds.in_va, ds.in_mode, ds.in_n, ds.in_over);   // build 0.0.515 (read-only)\n"
                      "        if (c80_mode() && ds.in_abi && !cgRefused) c80_inputs(ds.in_va, ds.in_mode, ds.in_n, ds.in_over);") == 1u, 1u);
    // the hook: begin, unjudged x2, lost x2 - each gated, each beside switch 73's
    const char *hB = "    if (gP73On && self == gPm4GfxChan) n48_p73_call_begin(&gP73);   // build 0.0.516 (switch 73): nothing judged yet\n"
                     "    if (c80_mode() && self == gPm4GfxChan) c80_call_begin();";
    const char *hU1 = "        if (gP73On && self == gPm4GfxChan) p73_after_decide();   // build 0.0.516: an unjudged GFX frame: every plane UNKNOWN\n"
                      "        if (c80_mode() && self == gPm4GfxChan) c80_after_decide();";
    const char *hU2 = "    if (gP73On && route == N48_ROUTE_DECIDE) p73_after_decide();\n    if (c80_mode() && route == N48_ROUTE_DECIDE) c80_after_decide();";
    const char *hL1 = "        if (gP73On && !(tokMatch && ksOk)) p73_final(here.seq, 0u);   // build 0.0.516: token/guard/keystone -> WITHDRAWN\n"
                      "        if (c80_mode() && !(tokMatch && ksOk)) c80_writer_lost(here.seq);";
    const char *hL2 = "            if (gP73On) p73_final(seq, exWhy == N48_GFXN_EX_SPARED ? 1u : 0u);\n";
    const char *hL2b = "            if (c80_mode() && exWhy != N48_GFXN_EX_SPARED) c80_writer_lost(seq);\n";
    const size_t orig = s.find("            const uint64_t rvT = orig(self, info);");
    const size_t l2 = s.find(hL2), l2b = s.find(hL2b), wn = s.find("            const n48_wn75_plan_t wnPlan = n48_wn75_plan(");
    expect_u("GLUE the hook's top clears `judged` beside 73's (gated, GFX channel)", count(s, hB), 1u);
    expect_u("GLUE a frame the judge never saw is queued as unjudged at BOTH of 73's sites", count(s, hU1) * 0x10u + count(s, hU2), 0x11u);
    expect_u("GLUE C4 the token / guard / keystone withdrawal queues the loss, right after 73's WITHDRAWN", count(s, hL1), 1u);
    expect_u("GLUE C4 the ring walk that did not spare it (the walk-NOP 75 retires, 77 forgives) queues the loss, AFTER Apple's original, before switch 75's plan",
             orig != std::string::npos && l2 != std::string::npos && l2b != std::string::npos && wn != std::string::npos &&
             orig < l2 && l2 < l2b && l2b < wn && count(s, hL2b) == 1u && count(s, "c80_writer_lost(") == 3u, 1u);
    expect_u("GLUE the hook's unjudged answer reads the flag c80_judge sets", count(s, "    if (!__atomic_load_n(&gC80Judged, __ATOMIC_RELAXED)) n48_c80_q_unjudged(&gC80Q);"), 1u);
    // the present side: the copy ring (gated), placed before 73's COPIED line (whose own pin wants `return copy;` right after it)
    expect_u("GLUE hw_p73_present records a copy in the ring only while 80 is ON or SHADOW, before 73's copy line",
             count(s, "    if (copy && c80_mode() != N48_P73_C80_OFF) c80_copy_note(presentNo, phys, i);") == 1u &&
             s.find("    if (copy && c80_mode() != N48_P73_C80_OFF) c80_copy_note(presentNo, phys, i);") <
             s.find("    if (n48_p73_copy_line_due(&gP73, gP73On ? 1u : 0u, copy, i))"), 1u);
    expect_u("GLUE flip mode's copy buffer: read-only, lock-free", count(br, "uint32_t navi48_fm_copy_buf(void) {") == 1u &&
             br.find("IOLockLock", br.find("uint32_t navi48_fm_copy_buf(void) {")) > br.find("\n}\n", br.find("uint32_t navi48_fm_copy_buf(void) {")), 1u);
    // X4: the dpled line unchanged, its cap extended only while armed and 80 is ON or SHADOW
    expect_u("X4 the dpled cap: 256, or 1024 only while armed (hw_cm_armed) and 80 is ON or SHADOW",
             count(s, "        static constexpr uint32_t kXdDpLines = 256u;\n        static constexpr uint32_t kXdDpLines80 = 1024u;") == 1u &&
             count(s, "        if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {") == 1u &&
             count(s, "HWLOG(N48_DL_LINE_FMT,") == 1u, 1u);
    // switch 73's own strings are untouched (the frozen header's macros are redefined identically or this file would not compile)
    expect_u("GLUE switch 73's report is still switch 73's (N48_P73_FMT printed once, its fields unchanged)",
             count(s, "HWLOG(N48_P73_FMT, gP73On ? \"ON (a present copies only a plane whose last P COMMITTED)\" : \"OFF (default)\", how,") == 1u, 1u);
    expect_u("GLUE nothing of switch 80 reads the descriptor slot: the only `.dslot` in the kext is the NOSLOT fill", count(s, ".dslot"), 1u);
    expect_u("GLUE C3+: the held-back site and (FIX PASS SHOULD) the writer credit hand cyc515's write set",
             count(s, "    (void)n48_c80_held(&gC80, r, trunc, gCy515.ws, gCy515.nws < N48_CY_WS_MAX ? gCy515.nws : N48_CY_WS_MAX,\n"
                      "                       (gCy515.ws_unknown || gCy515.ws_over) ? 1u : 0u, ctx, gXdC.judged + 1u);") == 1u &&
             count(s, "gCy515.ws,") == 2u && count(s, "(gCy515.ws_unknown || gCy515.ws_over)") == 1u &&
             count(s, "(void)n48_c80_writer(&gC80, gC80J.ctx, hVa, hPage, hRes, nh, gCy515.ws, gCy515.nws < N48_CY_WS_MAX ? gCy515.nws : N48_CY_WS_MAX,") == 1u, 1u);
    // FIX PASS MF-2: the order of the three reads in the present and of the four stores in the judge
    expect_u("GLUE MF-2: n48_p73_c80_hold reads seq, verdict, seq and hands all three to n48_p73_c80_complete",
             count(h, "    const uint32_t s1 = n48_p73_ld32(&s->c80seq);\n    const uint32_t v = n48_p73_ld32(&s->c80verdict);\n"
                      "    const uint32_t s2 = n48_p73_ld32(&s->c80seq);\n    if (n48_p73_c80_complete(s1, v, s2, seq)) return 0u;") == 1u, 1u);
    expect_u("GLUE MF-2: n48_c80_p stores c80seq 0, the verdict, the layer, then the seq",
             count(hc, "        n48_p73_st32(&t->s[slot].c80seq, 0u);\n        n48_p73_st32(&t->s[slot].c80verdict, z.verdict);\n"
                       "        n48_p73_st32(&t->s[slot].c80layer, li < N48_C80_LAYERS ? li + 1u : 0u);\n"
                       "        n48_p73_st32(&t->s[slot].c80seq, in->seq);") == 1u, 1u);
    expect_u("GLUE (SHOULD) the drain claims the loss count with one exchange at its top",
             count(hc, "    const uint32_t n = __atomic_exchange_n(&q->n, 0u, __ATOMIC_ACQ_REL);") == 1u && count(hc, "__atomic_store_n(&q->n,") == 0u, 1u);
    expect_u("GLUE (SHOULD) the eviction line is printed once",
             count(s, "    if (gC80.evicted != ev0 && !gC80.evictLogged) {") == 1u && count(s, "        gC80.evictLogged = 1u;") == 1u, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== T1 run11c replayed ==\n"); t1_run11c();
    std::printf("== T2 presents (ON / SHADOW / OFF) ==\n"); t2_present();
    std::printf("== T3/T4 held-back buckets and the range ==\n"); t3_t4();
    std::printf("== T5 lost after the gate ==\n"); t5_lost();
    std::printf("== T6/T7 sticky, discovery, C2 ==\n"); t6_t7();
    std::printf("== T8 OFF identity vs 0.0.524 ==\n"); t8_off_identity();
    std::printf("== T9 wrap ==\n"); t9_wrap();
    std::printf("== T10 swapped slots ==\n"); t10_slots();
    std::printf("== T11 C6 order ==\n"); t11_order();
    std::printf("== T12 cross clause, verb, reset ==\n"); t12_cross_and_verb();
    std::printf("== T13 X4 lines and caps ==\n"); t13_lines();
    std::printf("== T14 kext glue ==\n"); t14_glue(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
    std::printf("gfx_cycle80: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
