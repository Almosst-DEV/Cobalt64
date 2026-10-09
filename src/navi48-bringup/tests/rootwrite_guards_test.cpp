// rootwrite_guards_test.cpp — prove every guard G1..G6 REFUSES.
//
// The brief for milestone 3 increment (iii) requires each guard "proven to REFUSE
// in a host test, not merely to pass", and notes/an internal review note.4 says
// why: G1, G2, G3 and the teardown's G7 all fail SILENTLY and OFF-TARGET. On
// hardware their failure looks identical to their success — nothing visibly
// happens, and another process's page table is quietly wrong. A passing hardware
// run is therefore not evidence that these guards work. Only driving each one
// through its refusal path is.
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -O1 \
//         -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/rootwrite_guards_test.cpp -o /tmp/rwtest && /tmp/rwtest
//
// It compiles the SAME header the kext compiles, so there is no second
// implementation that could drift from the one that actually runs.

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "rootwrite_guards.h"

static int gFail = 0;
static int gRun  = 0;

static const char *guard_name(uint32_t g)
{
    switch (g) {
    case kRootWriteOk: return "PERMIT";
    case kRootWriteG1: return "G1 identity";
    case kRootWriteG2: return "G2 arena range";
    case kRootWriteG3: return "G3 root[0] live";
    case kRootWriteG4: return "G4 slot 511 zero";
    case kRootWriteG5: return "G5 pending/clear/slot";
    case kRootWriteG6: return "G6 our L1 built";
    case kRootWriteG8: return "G8 gfx power";
    default:           return "??";
    }
}

static void check(const char *what, const RootWriteInputs &in, uint32_t want)
{
    gRun++;
    const uint32_t got = rootwrite_guard_check(&in);
    const bool ok = (got == want);
    if (!ok) gFail++;
    printf("  %-58s want %-22s got %-22s %s\n",
           what, guard_name(want), guard_name(got), ok ? "ok" : "*** FAILED ***");
}

// The measured-good baseline: r87's live context, r80/r82's arena and root, and
// a ringmap built and verified exactly as r82/r83 reported it. Every negative
// case below is this struct with ONE field spoiled, so each test isolates the
// guard it names.
static RootWriteInputs good()
{
    RootWriteInputs in;
    memset(&in, 0, sizeof(in));
    // G1 — a live context we created, identity intact, never written.
    in.ctxState      = 1;
    in.ctxIdentityOk = 1;
    in.ctxSeq        = 6;          // r87: blit2 was create #6
    in.alreadyWrote  = 0;
    // G2 — the arena as the kext derives it, and blit2's measured root.
    in.arenaBot      = 0x3d6000000ull;
    in.arenaTop      = 0x3da400000ull;   // arenaBot + 0x4400000 (68 MiB,)
    in.root          = 0x3d6c00000ull;   // r84/r87, from ctx+0x98+0x20
    // G3 — Apple's own root[0], verbatim from r80.
    in.rootEnt0      = 0x10000003d6c01001ull;
    // G4 — measured zero in every root table dumped (r80, r82, r87).
    in.rootEnt511    = 0;
    // G5 — nothing queued at this page.
    in.pendingPtepdeHits = 0;
    // G6 — r82/r83: 4096/4096 entries, 0 mismatches, 6/6 probes.
    in.ringMapBuilt      = 1;
    in.ringMapMismatched = 0;
    in.ringMapProbes     = 6;
    in.ringMapProbeBad   = 0;
    in.l1Off             = 0x3cba81000ull;
    // 0.0.356 G5's block-clear witness: the mapVA hook saw THIS root appear and
    // read root[0] as ZERO then (wsgc1's measured WindowServer transition, create #7:
    // "root[0] = 000000000000000000"); root[0] is live now, so the clear is behind us.
    // Every other 0.0.356 field is left at its neutral zero: no owner demanded, no
    // CONTEXT2 demand, no slot write seen, power not measured.
    in.clearWitnessKnown = 1;
    in.rootAtTransition  = in.root;
    in.ent0AtTransition  = 0;
    return in;
}

// ---------------------------------------------------------------------------
// 0.0.356: the predicate AS IT WAS before this change, frozen here verbatim
// (G1-G6 over the pre-0.0.356 fields only). The refuse-only property below compares
// the live header against it: nothing this copy refused may now be permitted.
static uint32_t old_guard_check(const RootWriteInputs *in)
{
    if (!in) return kRootWriteG1;
    if (in->ctxSeq == 0)        return kRootWriteG1;
    if (in->ctxState != 1)      return kRootWriteG1;
    if (!in->ctxIdentityOk)     return kRootWriteG1;
    if (in->alreadyWrote)       return kRootWriteG1;
    if (in->arenaTop <= in->arenaBot)                  return kRootWriteG2;
    if (in->root == 0)                                 return kRootWriteG2;
    if (in->root & 0xFFFull)                           return kRootWriteG2;
    if (in->root < in->arenaBot)                       return kRootWriteG2;
    if (in->root >= in->arenaTop)                      return kRootWriteG2;
    if (!(in->rootEnt0 & N48_PTE_VALID_BIT))           return kRootWriteG3;
    if (in->rootEnt0 & N48_PTE_P_BIT)                  return kRootWriteG3;
    if (in->rootEnt0 & N48_PTE_SYSTEM_BIT)             return kRootWriteG3;
    {
        const uint64_t t = in->rootEnt0 & N48_PTE_ADDR_MASK;
        if (t < in->arenaBot || t >= in->arenaTop)     return kRootWriteG3;
    }
    if (in->rootEnt511 != 0)                           return kRootWriteG4;
    if (in->pendingPtepdeHits != 0)                    return kRootWriteG5;
    if (!in->ringMapBuilt)                             return kRootWriteG6;
    if (in->ringMapMismatched != 0)                    return kRootWriteG6;
    if (in->ringMapProbes == 0)                        return kRootWriteG6;
    if (in->ringMapProbeBad != 0)                      return kRootWriteG6;
    if (in->l1Off == 0)                                return kRootWriteG6;
    if (in->l1Off & 0xFFFull)                          return kRootWriteG6;
    return kRootWriteOk;
}

// A PLANTED DEFECT for the property harness to catch: the 0.0.356 predicate with the
// stale-root[0] clause forgotten. It permits the r96 arm-at-the-transition case, so
// the harness below must report it - or the harness proves nothing.
static uint32_t planted_no_stale_clause(const RootWriteInputs *in)
{
    RootWriteInputs t = *in;
    t.ent0AtTransition = 0;
    return rootwrite_guard_check(&t);
}

static uint64_t gRng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return gRng; }
template <typename T, size_t N> static T pick(const T (&a)[N]) { return a[rnd() % N]; }

// One random input over the values that exercise every clause, anchored on good().
static RootWriteInputs random_inputs()
{
    RootWriteInputs in = good();
    static const uint32_t b01[] = { 0, 1 };
    static const uint32_t st[]  = { 0, 1, 1, 1, 2 };
    static const uint32_t seq[] = { 0, 6, 7 };
    static const uint64_t roots[] = { 0x3d6c00000ull, 0x3d6c0a000ull, 0x3d6c00800ull, 0,
                                      0x3d5fff000ull, 0x3da400000ull };
    static const uint64_t e0s[] = { 0x10000003d6c01001ull, 0, 0x10000003d6c01000ull,
                                    0x90000003d6c01001ull, 0x1000000010000001ull,
                                    0x10000003d6c0b001ull };
    static const uint64_t e511[] = { 0, 0, 0, 1, 0x10000003cba81001ull };
    static const uint32_t hits[] = { 0, 0, 0, 1, 2 };
    static const uint64_t l1s[] = { 0x3cba81000ull, 0, 0x3cba81800ull };
    in.ctxState = pick(st);          in.ctxIdentityOk = pick(b01) | pick(b01);
    in.ctxSeq = pick(seq);           in.alreadyWrote = pick(b01) & pick(b01);
    in.root = pick(roots);           in.rootEnt0 = pick(e0s);
    in.rootEnt511 = pick(e511);      in.pendingPtepdeHits = pick(hits);
    in.ringMapBuilt = pick(b01) | pick(b01);
    in.ringMapMismatched = pick(hits) & 1u;
    in.ringMapProbes = (pick(b01) | pick(b01)) ? 6u : 0u;
    in.ringMapProbeBad = pick(hits) & 1u;
    in.l1Off = pick(l1s);
    in.ownerRequired = pick(b01);    in.ownerOk = pick(b01) | pick(b01);
    in.ctx2Required = pick(b01);     in.ctx2Agrees = pick(b01) | pick(b01);
    in.clearWitnessKnown = pick(b01) | pick(b01);
    in.rootAtTransition = (rnd() & 3) ? in.root : pick(roots);
    in.ent0AtTransition = (rnd() & 3) ? 0 : pick(e0s);
    in.slotWriteHits = pick(hits);
    in.gfxPowerChecked = pick(b01);  in.gfxPowerOn = pick(b01) | pick(b01);
    return in;
}

// --- 0.0.253: G7 (withdraw) and the re-arm ---------------------------------
//
// Review.4 puts G7 in the SAME class as G1-G3: silent and off-target. If it
// permits wrongly we zero eight bytes of a page that has already changed owner,
// nothing faults, and another process's mapping is quietly destroyed. So, exactly
// as with G1-G6, a passing hardware run proves nothing here and only the refusal
// paths are evidence.

static const char *withdraw_name(uint32_t v)
{
    switch (v) {
    case kRootWithdrawOk:        return "CLEAR IT";
    case kRootWithdrawNotArmed:  return "not armed";
    case kRootWithdrawNoRead:    return "slot unread";
    case kRootWithdrawPageMoved: return "page moved";
    case kRootWithdrawNotOurs:   return "G7 not ours";
    default:                     return "??";
    }
}

static const char *rearm_name(uint32_t v)
{
    switch (v) {
    case kRootRearmOk:           return "RE-ARM";
    case kRootRearmNotWithdrawn: return "not withdrawn";
    case kRootRearmRootFreed:    return "root freed";
    case kRootRearmRootMoved:    return "root moved";
    case kRootRearmSlotTaken:    return "slot taken";
    case kRootRearmNoRead:       return "slot unread";
    default:                     return "??";
    }
}

static void wcheck(const char *what, const RootWithdrawInputs &in, uint32_t want)
{
    gRun++;
    const uint32_t got = rootwrite_withdraw_check(&in);
    const bool ok = (got == want);
    if (!ok) gFail++;
    printf("  %-58s want %-22s got %-22s %s\n",
           what, withdraw_name(want), withdraw_name(got), ok ? "ok" : "*** FAILED ***");
}

static void rcheck(const char *what, const RootRearmInputs &in, uint32_t want)
{
    gRun++;
    const uint32_t got = rootwrite_rearm_check(&in);
    const bool ok = (got == want);
    if (!ok) gFail++;
    printf("  %-58s want %-22s got %-22s %s\n",
           what, rearm_name(want), rearm_name(got), ok ? "ok" : "*** FAILED ***");
}

// The armed steady state: we wrote our entry into r89's measured root page, and the
// slot still reads exactly what we put there.
static RootWithdrawInputs wgood()
{
    RootWithdrawInputs in;
    memset(&in, 0, sizeof(in));
    in.wrote           = 1;
    in.withdrawn       = 0;
    in.slotRead        = 1;
    in.rootNow         = 0x3d6c00000ull;   // r89: blit2's root, create #6
    in.rootPageAtWrite = 0x3d6c00000ull;
    in.rootWritten     = 0x10000003cba81001ull;   // our L1 block as a directory PDE
    in.slot511         = 0x10000003cba81001ull;
    return in;
}

// Just after a withdrawal, inside the call: we zeroed the slot and Apple has not
// (yet) freed the page.
static RootRearmInputs rgood()
{
    RootRearmInputs in;
    memset(&in, 0, sizeof(in));
    in.withdrawn       = 1;
    in.slotRead        = 1;
    in.rootAtEntry     = 0x3d6c00000ull;
    in.rootAtReturn    = 0x3d6c00000ull;
    in.rootPageAtWrite = 0x3d6c00000ull;
    in.slot511         = 0;                // we left it zero on the way in
    return in;
}

// --- 0.0.257: the ARM TRIGGER on mapVA ------------------------------------
//
// Arming on the wrong call means writing eight bytes into a page that is not the
// context's root - review 7.4's silent, off-target class again, so the refusals are
// the only evidence that can be gathered anywhere but here.

static const char *trigger_name(uint32_t v)
{
    switch (v) {
    case kRootArmOk:           return "ARM NOW";
    case kRootArmNotOurs:      return "not ours";
    case kRootArmAlreadyArmed: return "already armed";
    case kRootArmNoTransition: return "no transition";
    default:                   return "??";
    }
}

static void tcheck(const char *what, const RootArmTriggerInputs &in, uint32_t want)
{
    gRun++;
    const uint32_t got = rootwrite_arm_trigger(&in);
    const bool ok = (got == want);
    if (!ok) gFail++;
    printf("  %-58s want %-22s got %-22s %s\n",
           what, trigger_name(want), trigger_name(got), ok ? "ok" : "*** FAILED ***");
}

// The moment the arm exists for: mapVA ran, and the root it lazily allocated is
// r90/r91's measured address.
static RootArmTriggerInputs tgood()
{
    RootArmTriggerInputs in;
    memset(&in, 0, sizeof(in));
    in.ctxLive      = 1;
    in.identityOk   = 1;
    in.alreadyWrote = 0;
    in.rootBefore   = 0;
    in.rootAfter    = 0x3d6c00000ull;
    return in;
}

// --- 0.0.259: RE-ADOPTION by region ----------------------------------------
//
// A wrong adoption writes eight bytes into a page we have misidentified, which is
// review 7.4's silent, off-target class - so, as with G7 and the arm trigger, the
// refusals are the only evidence obtainable anywhere but here.

static const char *adopt_name(uint32_t v)
{
    switch (v) {
    case kAdoptRefuse:    return "REFUSE";
    case kAdoptStaleOurs: return "ADOPT (ours)";
    default:              return "??";
    }
}

static void acheck(const char *what, const RootAdoptInputs &in, uint32_t want)
{
    gRun++;
    const uint32_t got = rootwrite_adopt_check(&in);
    const bool ok = (got == want);
    if (!ok) gFail++;
    printf("  %-58s want %-22s got %-22s %s\n",
           what, adopt_name(want), adopt_name(got), ok ? "ok" : "*** FAILED ***");
}

// r93's ACTUAL measured state: slot 511 held our own PDE from r92, naming our L1
// block at 0x3cba90000, inside the reservation [0x3cb000000, 0x3cba98000).
static RootAdoptInputs agood()
{
    RootAdoptInputs in;
    memset(&in, 0, sizeof(in));
    in.slot511       = 0x10000003cba90001ull;   // r93 driverlog, G4 slot511
    in.reservedLo    = 0x3cb000000ull;
    in.reservedHi    = 0x3cb000000ull + 0xa98000ull;
    in.ringMapBuilt  = 1;
    in.armTransition = 1;
    return in;
}

int main()
{
    printf("rootwrite guard predicate — every guard must REFUSE on its own bad input\n\n");

    printf("baseline (the only case that may permit the write):\n");
    check("measured-good inputs (r80/r82/r83/r87)", good(), kRootWriteOk);

    printf("\nG1 — identity. These are review 7.4's WORST row: silent, off-target,\n"
           "     another process's GPU memory. Apple recycles both ctx and task\n"
           "     pointers (notes 406, 407), so each of these has really happened.\n");
    { auto in = good(); in.ctxSeq = 0;         check("no record of this context at all", in, kRootWriteG1); }
    { auto in = good(); in.ctxState = 0;       check("record slot is free", in, kRootWriteG1); }
    { auto in = good(); in.ctxState = 2;       check("context ALREADY RELEASED (the r87 state)", in, kRootWriteG1); }
    { auto in = good(); in.ctxIdentityOk = 0;  check("vtable/task changed - object recycled (r86)", in, kRootWriteG1); }
    { auto in = good(); in.alreadyWrote = 1;   check("we already wrote for this context", in, kRootWriteG1); }

    printf("\nG2 — arena range. Failure here is review 7.4's unrecoverable row:\n"
           "     vram_alloc_hi or the PSP TMR tail.\n");
    { auto in = good(); in.root = 0;                    check("root reads zero (no mapVA yet)", in, kRootWriteG2); }
    { auto in = good(); in.root = 0x3d6c00800ull;       check("root not 4 KiB aligned", in, kRootWriteG2); }
    { auto in = good(); in.root = 0x3d5fff000ull;       check("root BELOW the arena", in, kRootWriteG2); }
    { auto in = good(); in.root = 0x3da400000ull;       check("root AT arenaTop (exclusive bound)", in, kRootWriteG2); }
    { auto in = good(); in.root = 0x100000000ull;       check("root in our hi pool, far outside", in, kRootWriteG2); }
    { auto in = good(); in.arenaBot = in.arenaTop;      check("degenerate arena bounds", in, kRootWriteG2); }

    printf("\nG3 — root[0] must be a LIVE root PDE. Notes 401: VRAM is not zeroed at\n"
           "     boot, so the VALID bit alone is worthless on stale contents and the\n"
           "     arena-range clause is what actually carries this guard.\n");
    { auto in = good(); in.rootEnt0 = 0;                          check("root[0] empty", in, kRootWriteG3); }
    { auto in = good(); in.rootEnt0 &= ~N48_PTE_VALID_BIT;        check("root[0] not VALID", in, kRootWriteG3); }
    { auto in = good(); in.rootEnt0 |= N48_PTE_P_BIT;             check("root[0] has P set - a LEAF, so a recycled PTE page", in, kRootWriteG3); }
    { auto in = good(); in.rootEnt0 |= N48_PTE_SYSTEM_BIT;        check("root[0] is sysmem, not VRAM", in, kRootWriteG3); }
    { auto in = good(); in.rootEnt0 = 0x1000000010000001ull;      check("VALID but target OUTSIDE the arena (stale VRAM)", in, kRootWriteG3); }
    { auto in = good(); in.rootEnt0 = 0x0000000000000001ull;      check("VALID with a zero target (stale VRAM)", in, kRootWriteG3); }

    printf("\nG4 — slot 511 must read EXACTLY zero, both dwords.\n");
    { auto in = good(); in.rootEnt511 = 1;                  check("slot 511 VALID - Apple claimed it", in, kRootWriteG4); }
    { auto in = good(); in.rootEnt511 = 0x100000000ull;      check("slot 511 high dword set only", in, kRootWriteG4); }
    { auto in = good(); in.rootEnt511 = 0x8000000000000000ull; check("slot 511 is a leaf (recycled PTE page, r87 read D)", in, kRootWriteG4); }

    printf("\nG5 — Apple must have nothing queued at this page.\n");
    { auto in = good(); in.pendingPtepdeHits = 1;   check("one pending PTEPDE targets this root page", in, kRootWriteG5); }

    printf("\nG5, 0.0.356 (, review change 3) — the BLOCK-CLEAR the old G5 never saw:\n"
           "     measured Apple's CONST_FILL of a fresh root page wiping slot 511.\n");
    { auto in = good(); in.clearWitnessKnown = 0;
      check("no transition witnessed for this context", in, kRootWriteG5); }
    { auto in = good(); in.rootAtTransition = 0x3d6c0a000ull;
      check("witness is for a DIFFERENT root page (re-allocated)", in, kRootWriteG5); }
    { auto in = good(); in.ent0AtTransition = in.rootEnt0;
      check("r96's case: root[0] already live AT the transition", in, kRootWriteG5); }
    { auto in = good(); in.ent0AtTransition = 0x5a4da921560da925ull;
      check("root[0] held stale VRAM at the transition", in, kRootWriteG5); }
    { auto in = good(); in.slotWriteHits = 1;
      check("a decoded WRITE/COPY/PTEPDE covers slot 511", in, kRootWriteG5); }
    { auto in = good(); in.rootEnt0 = 0;
      check("witnessed zero, root[0] STILL zero -> G3 first", in, kRootWriteG3); }

    printf("\nG1, 0.0.356 (, review change 1) — selected BY OWNER, bound to VMID 2:\n");
    { auto in = good(); in.ownerRequired = 1; in.ownerOk = 1;
      check("owner demanded and proven", in, kRootWriteOk); }
    { auto in = good(); in.ownerRequired = 1; in.ownerOk = 0;
      check("owner demanded, evidence FAILS (pid gone/renamed/ambiguous)", in, kRootWriteG1); }
    { auto in = good(); in.ownerRequired = 0; in.ownerOk = 0;
      check("owner not demanded (the mode-0/1 callers) -> unchanged", in, kRootWriteOk); }
    { auto in = good(); in.ctx2Required = 1; in.ctx2Agrees = 0;
      check("CONTEXT2 demanded and names ANOTHER root", in, kRootWriteG1); }
    { auto in = good(); in.ctx2Required = 1; in.ctx2Agrees = 1;
      check("CONTEXT2 demanded and agrees", in, kRootWriteOk); }

    printf("\nG8, 0.0.356 (, review change 4) — gfx power measured at the write:\n");
    { auto in = good(); in.gfxPowerChecked = 1; in.gfxPowerOn = 0;
      check("measured, and the RLC says gated off", in, kRootWriteG8); }
    { auto in = good(); in.gfxPowerChecked = 1; in.gfxPowerOn = 1;
      check("measured, and on", in, kRootWriteOk); }
    { auto in = good(); in.gfxPowerChecked = 0; in.gfxPowerOn = 0;
      check("not measured (hook callers) -> unchanged", in, kRootWriteOk); }
    {
        struct { uint32_t gpm, pg, want; const char *what; } pc[] = {
            { 0x00000006u, 0x00000001u, 1, "PG enabled, power+clock status set" },
            { 0x00000002u, 0x00000001u, 1, "PG enabled, power status set" },
            { 0x00000004u, 0x00000001u, 0, "PG enabled, power status CLEAR" },
            { 0x00000000u, 0x00000001u, 0, "PG enabled, all status clear" },
            { 0x00000000u, 0x00000000u, 1, "PG disabled: GFXOFF cannot engage via the RLC" },
            { 0xFFFFFFFFu, 0x00000000u, 0, "dead GPM_STAT read (all ones)" },
            { 0x00000006u, 0xFFFFFFFFu, 0, "dead PG_CNTL read (all ones)" },
        };
        for (auto &c : pc) {
            gRun++;
            const uint32_t got = rootwrite_gfx_power_on(c.gpm, c.pg);
            const bool ok = got == c.want;
            if (!ok) gFail++;
            printf("  %-58s want %-22s got %-22s %s\n", c.what, c.want ? "ON" : "OFF/unknown",
                   got ? "ON" : "OFF/unknown", ok ? "ok" : "*** FAILED ***");
        }
    }

    printf("\nREFUSE-ONLY PROPERTY (0.0.356): 2,000,000 random inputs over every clause.\n"
           "     (a) the live predicate never PERMITS what the pre-0.0.356 copy refused;\n"
           "     (b) with the new fields neutral it returns EXACTLY the old verdict;\n"
           "     (c) an arm AT the transition (root[0] now == root[0] then) never permits;\n"
           "     (d) the harness catches a planted defect that drops the stale clause.\n");
    {
        uint64_t widen = 0, neutralDiff = 0, transitionPermit = 0, planted = 0;
        uint64_t permitNew = 0, permitOld = 0;
        for (int i = 0; i < 2000000; i++) {
            RootWriteInputs in = random_inputs();
            const uint32_t n = rootwrite_guard_check(&in), o = old_guard_check(&in);
            if (n == kRootWriteOk) permitNew++;
            if (o == kRootWriteOk) permitOld++;
            if (n == kRootWriteOk && o != kRootWriteOk) widen++;
            RootWriteInputs z = in;
            z.ownerRequired = 0; z.ctx2Required = 0; z.slotWriteHits = 0; z.gfxPowerChecked = 0;
            z.clearWitnessKnown = 1; z.rootAtTransition = z.root; z.ent0AtTransition = 0;
            if (rootwrite_guard_check(&z) != old_guard_check(&z)) neutralDiff++;
            RootWriteInputs t = in;
            t.clearWitnessKnown = 1; t.rootAtTransition = t.root; t.ent0AtTransition = t.rootEnt0;
            if (rootwrite_guard_check(&t) == kRootWriteOk) transitionPermit++;
            if (planted_no_stale_clause(&t) == kRootWriteOk && old_guard_check(&t) == kRootWriteOk) planted++;
        }
        printf("  permitted: old %llu, new %llu of 2000000\n",
               (unsigned long long)permitOld, (unsigned long long)permitNew);
        struct { const char *what; uint64_t got; bool wantZero; } pr[] = {
            { "(a) new PERMITS where old REFUSED", widen, true },
            { "(b) neutral new fields, verdict differs from old", neutralDiff, true },
            { "(c) arm at the transition PERMITTED", transitionPermit, true },
            { "(d) planted defect caught (must be NON-zero)", planted, false },
        };
        for (auto &p : pr) {
            gRun++;
            const bool ok = p.wantZero ? p.got == 0 : p.got != 0;
            if (!ok) gFail++;
            printf("  %-58s %-10llu %s\n", p.what, (unsigned long long)p.got, ok ? "ok" : "*** FAILED ***");
        }
        gRun++;
        const bool sane = permitNew > 0 && permitOld > permitNew;
        if (!sane) gFail++;
        printf("  %-58s %s\n", "the random domain reaches PERMIT, and the new predicate is stricter",
               sane ? "ok" : "*** FAILED ***");
    }

    printf("\nG6 — our own mapping must be built and PROVEN this boot.\n");
    { auto in = good(); in.ringMapBuilt = 0;        check("ringmap never built this boot", in, kRootWriteG6); }
    { auto in = good(); in.ringMapMismatched = 1;   check("an L1 entry read back wrong", in, kRootWriteG6); }
    { auto in = good(); in.ringMapProbes = 0;       check("the walker never ran", in, kRootWriteG6); }
    { auto in = good(); in.ringMapProbeBad = 1;     check("a walker probe MISMATCHED", in, kRootWriteG6); }
    { auto in = good(); in.l1Off = 0;               check("no L1 block address", in, kRootWriteG6); }
    { auto in = good(); in.l1Off = 0x3cba81800ull;  check("L1 block not page aligned", in, kRootWriteG6); }

    printf("\nordering — the FIRST objection is the one reported, so a case that\n"
           "           violates several guards reports the earliest.\n");
    { auto in = good(); in.ctxState = 2; in.root = 0; in.rootEnt511 = 1;
      check("released AND no root AND slot taken -> G1", in, kRootWriteG1); }
    { auto in = good(); in.root = 0; in.rootEnt511 = 1;
      check("no root AND slot taken -> G2", in, kRootWriteG2); }
    { auto in = good(); in.rootEnt511 = 1; in.ringMapBuilt = 0;
      check("slot taken AND no mapping -> G4", in, kRootWriteG4); }

    printf("\nthe root PDE encoding (confirmed against our own gfx12 self-test and\n"
           "Apple's measured live root[0]):\n");
    {
        gRun++;
        // Apple's own root[0] rebuilt from its address must reproduce r80's value
        // byte for byte. This is the encoding check that matters: if it is wrong,
        // the write lands a malformed PDE in a live table.
        const uint64_t rebuilt = rootwrite_root_pde(0x3d6c01000ull);
        const bool ok = (rebuilt == 0x10000003d6c01001ull);
        if (!ok) gFail++;
        printf("  %-58s want 0x%016llx got 0x%016llx %s\n",
               "rebuild Apple's measured root[0] from its address",
               0x10000003d6c01001ull, (unsigned long long)rebuilt,
               ok ? "ok" : "*** FAILED ***");

        gRun++;
        const uint64_t ours = rootwrite_root_pde(0x3cba81000ull);
        // P (bit 63) MUST be clear: a P-set entry is a 64 KiB leaf, and a leaf here
        // would map the ring VA onto the L1 block itself instead of descending.
        const bool ok2 = (ours == 0x10000003cba81001ull) && !(ours & N48_PTE_P_BIT);
        if (!ok2) gFail++;
        printf("  %-58s want 0x%016llx got 0x%016llx %s\n",
               "our root[511] for the L1 block at 0x3cba81000",
               0x10000003cba81001ull, (unsigned long long)ours,
               ok2 ? "ok" : "*** FAILED ***");
    }

    printf("\nG7 / WITHDRAW — review 7.4 rates a wrong PERMIT here as silent and\n"
           "     off-target: another process's live entry zeroed, no fault. Every\n"
           "     refusal below leaves the page untouched, which is always safe.\n");
    wcheck("armed, same page, slot still exactly ours", wgood(), kRootWithdrawOk);
    { auto in = wgood(); in.wrote = 0;          wcheck("we never armed this context", in, kRootWithdrawNotArmed); }
    { auto in = wgood(); in.withdrawn = 1;      wcheck("already withdrawn - idempotent, no second clear", in, kRootWithdrawNotArmed); }
    { auto in = wgood(); in.rootWritten = 0;    wcheck("armed flag set but no recorded entry", in, kRootWithdrawNotArmed); }
    { auto in = wgood(); in.slotRead = 0;       wcheck("MM window refused the read - never guess", in, kRootWithdrawNoRead); }
    { auto in = wgood(); in.rootNow = 0;        wcheck("root already freed (field reads 0)", in, kRootWithdrawPageMoved); }
    { auto in = wgood(); in.rootNow = 0x3d6c0a000ull;
      wcheck("root is now a DIFFERENT page (r89 saw two roots)", in, kRootWithdrawPageMoved); }
    { auto in = wgood(); in.slot511 = 0;        wcheck("slot already zero - somebody cleared it", in, kRootWithdrawNotOurs); }
    { auto in = wgood(); in.slot511 = 0x10000003d6c01001ull;
      wcheck("slot holds APPLE's root[0]-style PDE, not ours", in, kRootWithdrawNotOurs); }
    { auto in = wgood(); in.slot511 = 0x5a4da921560da925ull;
      wcheck("slot holds r89's observed stale VRAM garbage", in, kRootWithdrawNotOurs); }
    { auto in = wgood(); in.slot511 = in.rootWritten ^ 1ull;
      wcheck("slot differs by ONE BIT (the VALID bit)", in, kRootWithdrawNotOurs); }

    printf("\nRE-ARM — r89 measured this as the COMMON case (11 fires, 1 freed, 10\n"
           "     survived), so it must be as hard to get wrong as the withdrawal.\n"
           "     A root that reads 0 at return is Apple saying it freed the page:\n"
           "     that is a REFUSAL, never a write.\n");
    rcheck("root survived and the slot is still the zero we left", rgood(), kRootRearmOk);
    { auto in = rgood(); in.withdrawn = 0;        rcheck("nothing of ours is down", in, kRootRearmNotWithdrawn); }
    { auto in = rgood(); in.rootAtReturn = 0;     rcheck("APPLE FREED THE ROOT in this call (r89's P2)", in, kRootRearmRootFreed); }
    { auto in = rgood(); in.rootAtReturn = 0x3d6c0a000ull;
      rcheck("root changed address across the call", in, kRootRearmRootMoved); }
    { auto in = rgood(); in.rootAtEntry = 0x3d6c0a000ull;
      rcheck("entry root disagrees with the page we wrote", in, kRootRearmRootMoved); }
    { auto in = rgood(); in.rootPageAtWrite = 0x3d6c0a000ull;
      rcheck("surviving root is not the page we armed", in, kRootRearmRootMoved); }
    { auto in = rgood(); in.slotRead = 0;         rcheck("MM window refused the read-back", in, kRootRearmNoRead); }
    { auto in = rgood(); in.slot511 = 0x10000003cba81001ull;
      rcheck("slot already re-taken (even by our own value)", in, kRootRearmSlotTaken); }
    { auto in = rgood(); in.slot511 = 0x10000003d6c01001ull;
      rcheck("Apple claimed slot 511 during its call", in, kRootRearmSlotTaken); }

    printf("\nthe withdraw/re-arm ROUND TRIP - the sequence r89 says will run on\n"
           "almost every unmap, walked end to end.\n");
    {
        // 1. armed and steady -> withdraw permitted
        auto w = wgood();
        wcheck("1. armed, unmap arrives -> clear", w, kRootWithdrawOk);
        // 2. having cleared, a second clear in the same call must be refused
        auto w2 = w; w2.withdrawn = 1; w2.slot511 = 0;
        wcheck("2. same call, already cleared -> no double clear", w2, kRootWithdrawNotArmed);
        // 3. Apple returns with the root alive -> re-arm
        auto r = rgood();
        rcheck("3. root survived -> put it back", r, kRootRearmOk);
        // 4. the OTHER branch: Apple freed it -> leave it alone
        auto r2 = rgood(); r2.rootAtReturn = 0;
        rcheck("4. root freed instead -> do nothing, entry died with page", r2, kRootRearmRootFreed);
    }

    printf("\nARM TRIGGER (mapVA, slot 36) - r91 proved the arm cannot be placed from a\n"
           "     step list, so it fires on the instant the root APPEARS. Only the\n"
           "     transition counts: a root that was already there is not ours to claim.\n");
    tcheck("mapVA allocated the root: arm now", tgood(), kRootArmOk);
    { auto in = tgood(); in.ctxLive = 0;      tcheck("no live record of this context", in, kRootArmNotOurs); }
    { auto in = tgood(); in.identityOk = 0;   tcheck("identity check failed at return", in, kRootArmNotOurs); }
    { auto in = tgood(); in.alreadyWrote = 1; tcheck("already armed - idempotent, no second write", in, kRootArmAlreadyArmed); }
    { auto in = tgood(); in.rootBefore = 0x3d6c00000ull;
      tcheck("root was ALREADY there - not ours to claim", in, kRootArmNoTransition); }
    { auto in = tgood(); in.rootAfter = 0;
      tcheck("mapVA did not allocate a root", in, kRootArmNoTransition); }
    { auto in = tgood(); in.rootBefore = 0; in.rootAfter = 0;
      tcheck("r91's actual state: zero before AND after", in, kRootArmNoTransition); }
    { auto in = tgood(); in.rootBefore = 0x3d6c00000ull; in.rootAfter = 0x3d6c00000ull;
      tcheck("root unchanged across the call", in, kRootArmNoTransition); }
    { auto in = tgood(); in.rootBefore = 0x3d6c00000ull; in.rootAfter = 0x3d6c0a000ull;
      tcheck("root CHANGED but did not appear - refuse", in, kRootArmNoTransition); }
    { auto in = tgood(); in.alreadyWrote = 1; in.rootBefore = 0; in.rootAfter = 0;
      tcheck("already armed AND no transition -> already armed", in, kRootArmAlreadyArmed); }
    { auto in = tgood(); in.ctxLive = 0; in.alreadyWrote = 1;
      tcheck("not ours outranks already-armed", in, kRootArmNotOurs); }

    printf("\nRE-ADOPTION by REGION (0.0.259) - the predicate must key on WHERE the entry\n"
           "     points, never on WHAT it equals. Our L1 address is stable across boots,\n"
           "     so value equality is predictable; region containment is not.\n");
    acheck("r93's measured stale entry, inside our carve", agood(), kAdoptStaleOurs);
    { auto in = agood(); in.armTransition = 0;
      acheck("VERB PATH - no mapVA transition, never adopt", in, kAdoptRefuse); }
    { auto in = agood(); in.ringMapBuilt = 0;
      acheck("our own mapping not built this boot", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0;
      acheck("slot is zero - the ordinary arm, not an adoption", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 &= ~N48_PTE_VALID_BIT;
      acheck("entry not VALID", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 |= N48_PTE_P_BIT;
      acheck("P set - a 64 KiB LEAF, not a directory", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 |= N48_PTE_SYSTEM_BIT;
      acheck("SYSTEM set - sysmem, not our VRAM tail", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = (in.slot511 & ~(0x1full << 58));
      acheck("BFS 0 instead of 4", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = (in.slot511 & ~(0x1full << 58)) | (2ull << 58);
      acheck("BFS 2 instead of 4", in, kAdoptRefuse); }
    printf("     the two cases that matter most - an APPLE page must never be adopted:\n");
    { auto in = agood(); in.slot511 = 0x10000003d6c01001ull;
      acheck("Apple's own root[0] target (arena page) - REFUSE", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0x10000003d6c00001ull;
      acheck("the arena root page itself - REFUSE", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0x1000000010000001ull;
      acheck("a vram_alloc_hi client page - REFUSE", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0x10000003caffF001ull;
      acheck("one page BELOW the reservation", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = (0x3cb000000ull + 0xa98000ull) | (4ull << 58) | 1ull;
      acheck("exactly AT reservedHi (exclusive bound)", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0x3cb000000ull | (4ull << 58) | 1ull;
      acheck("exactly AT reservedLo (inclusive bound)", in, kAdoptStaleOurs); }
    { auto in = agood(); in.reservedHi = in.reservedLo;
      acheck("degenerate reservation bounds", in, kAdoptRefuse); }
    { auto in = agood(); in.slot511 = 0x5a4da921560da925ull;
      acheck("r89's observed stale VRAM garbage", in, kAdoptRefuse); }

    printf("\n%d checks, %d failed\n", gRun, gFail);
    if (gFail) {
        printf("GUARD TEST FAILED - the write must not be built or run.\n");
        return 1;
    }
    printf("every guard REFUSES on its own bad input, and permits only the\n"
           "measured-good case. The predicate is safe to compile into the kext.\n");
    return 0;
}
