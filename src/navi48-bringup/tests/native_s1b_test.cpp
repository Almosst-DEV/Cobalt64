// native_s1b_test.cpp - build 0.0.600 (native-stack step S1b): the pure half of the reserved-VMID VM self-test.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_s1b_test.cpp -o /tmp/native_s1b && /tmp/native_s1b .
//   (run from the repo root; the argument is the repo root the source pins read from)
// Covers:
//   U1 the VMID rules (8..15, outside the MES 0xFE mask) and the mask pinned to mes_v12_1.cpp;
//   U2 VA arithmetic: sign-extension strip / re-extend, canonical forms, the per-level indices (the high half is root 256..511);
//   U3 entry encoding in both polarities: address field, flags, rejection of an unusable address, leaf vs pointer;
//   U4 the context CNTL value (DEPTH 3 / BLOCK 0 / RETRY 0, every other bit kept);
//   U5 the gate: every conflicting boot-arg refuses, absent = OFF whatever else is set, accepted otherwise;
//   U6 the fault-control check (CRASH_ON_* refuse) and the fault-status match;
//   U7 the tree the kext builds (tree_build) WALKED like the hardware: every mapped page, low and high half, in both polarities; the
//      unmapped VA faults at PDB1; a bad address refuses the build;
//   U8 the leg sequence (seq_drive, the driver the kext runs) against a fake runner: ORDERING (fault last, only after every positive
//      leg ran), the latched stop, the polarity fallback once, a refused fault leg, the verdict;
//   U9 source pins: the kext calls the driver and the builder, the run sits after the ladder behind the gate, the file writes exactly
//      the registers it claims, nothing on MMHUB, the MES mask and the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "native_s1b_pure.h"

using namespace n48native;

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

// ---------------------------------------------------------------------------------------------------------------------------
static void u1_vmid() {
    for (uint32_t v = 0; v <= 15; v++) {
        const bool want = v >= 8;
        expect(vmid_ok(v) == want, "vmid_ok: 8..15 only");
    }
    expect(!vmid_ok(16) && !vmid_ok(0xFFFFFFFFu), "vmid_ok: out of range");
    expect_u("MES mask has bits for VMIDs 1..7 only", kMesVmidMaskGfxhub, 0xFEu);
    expect(((kMesVmidMaskGfxhub >> kNativeVmid) & 1u) == 0u, "the native VMID is outside the MES mask");
}

static void u2_va() {
    expect_u("strip high", va_strip(0xFFFF800000100000ull), 0x800000100000ull);
    expect_u("strip low is identity", va_strip(0x100000ull), 0x100000ull);
    expect_u("strip keeps 48 bits", va_strip(0xFFFFFFFFFFFFFFFFull), 0xFFFFFFFFFFFFull);
    expect(va_canonical(0xFFFF800000000000ull) && va_canonical(0x00007FFFFFFFFFFFull), "canonical edges");
    expect(!va_canonical(0x0000800000000000ull) && !va_canonical(0xFFFF7FFFFFFFFFFFull) && !va_canonical(0x8000000000000000ull),
           "non-canonical values are rejected");
    expect_u("re-extend high", va_canonicalize(0x800000100000ull), 0xFFFF800000100000ull);
    expect_u("re-extend low", va_canonicalize(0x7FFFFFFFFFFFull), 0x7FFFFFFFFFFFull);
    for (uint64_t v : { 0x0ull, 0x1000ull, 0x7FFFFFFFF000ull, 0x800000000000ull, 0xFFFFFFFFF000ull })
        expect_u("strip(canonicalize(x)) == x", va_strip(va_canonicalize(v)), v);
    expect(va_high_half(0xFFFF800000100000ull) && !va_high_half(0x100000ull), "half detection");
    // The root index of the high half is 256..511, the low half 0..255.
    expect_u("root idx high base", idx_pdb2(0xFFFF800000000000ull), 256);
    expect_u("root idx of the last high VA", idx_pdb2(0xFFFFFFFFFFFFF000ull), 511);
    expect_u("root idx of the last low VA", idx_pdb2(0x00007FFFFFFFF000ull), 255);
    expect_u("root idx does not depend on the sign extension", idx_pdb2(0x0000800000100000ull), idx_pdb2(0xFFFF800000100000ull));
    // Every level's index is 9 bits and they recompose.
    for (uint64_t v : { 0x100000ull, 0x800000100000ull, 0x7F1234567000ull, 0xFFFFFFFFF000ull }) {
        const uint64_t re = ((uint64_t)idx_pdb2(v) << 39) | ((uint64_t)idx_pdb1(v) << 30) | ((uint64_t)idx_pdb0(v) << 21) |
                            ((uint64_t)idx_ptb(v) << 12);
        expect_u("indices recompose the VA", re, va_strip(v) & ~0xFFFull);
    }
    expect_u("pfn of the high base", va_pfn(kVaHighBase), 0x800000100ull);
    expect(va_in_context(0xFFFFFFFFFFFFF000ull) && va_in_context(kVaUnmapped), "inside the END register's range");
    expect_u("max_pfn - 1", kMaxPfn - 1, 0xFFFFFFFFFull);
}

static void u3_entries() {
    const uint64_t pa = 0x0000000123456000ull;
    const uint64_t d = pde_encode(pa), t = pte_encode(pa, kPteFlagsRwx);
    expect(entry_valid(d) && entry_valid(t), "both entries are VALID");
    expect_u("pde address", entry_pa(d), pa);
    expect_u("pte address", entry_pa(t), pa);
    expect(entry_is_leaf(t) && !entry_is_leaf(d), "leaf vs pointer");
    expect_u("a pointer carries no P bit", d >> 63, 0);
    expect_u("a leaf carries the vmfrag-proven P bit", t >> 63, 1);
    expect_u("a pointer carries no READ/WRITE/EXEC", d & (kPteRead | kPteWrite | kPteExec), 0);
    expect_u("a pointer carries no flag but VALID", d & ~kPdeAddrMask, kPteValid);
    expect((t & (kPteExec | kPteRead | kPteWrite)) == (kPteExec | kPteRead | kPteWrite), "leaf is RWX like RADV's BOs");
    expect_u("leaf memory type is UC", (t >> 54) & 3u, 2u);
    expect((t & kPteSystem) == 0u, "local VRAM: SYSTEM clear");
    // The leaf is exactly vmfrag_leaf's word (proven on hardware): pa | IS_PTE | MTYPE_UC | EXEC | READ | WRITE | VALID.
    expect_u("leaf == vmfrag_leaf", t, pa | (1ull << 63) | (2ull << 54) | (1ull << 4) | (1ull << 5) | (1ull << 6) | 1ull);
    expect_u("pointer == address | VALID", d, pa | 1ull);
    // No pde over any address (even the widest legal one) sets an access bit.
    for (uint64_t a : { 0x1000ull, 0x0000FFFFFFFFF000ull, 0x0000000AB0007000ull })
        expect_u("pde never sets READ/WRITE/EXEC", pde_encode(a) & (kPteRead | kPteWrite | kPteExec), 0);
    // Unusable addresses encode to 0, an invalid entry: unaligned, above the 48-bit field, carrying flag bits.
    expect_u("unaligned pde", pde_encode(pa | 0x40), 0);
    expect_u("unaligned pte", pte_encode(pa | 0x800, kPteFlagsRwx), 0);
    expect_u("pde above 48 bits", pde_encode(1ull << 48), 0);
    expect_u("pte above 48 bits", pte_encode(1ull << 60, kPteFlagsRwx), 0);
    expect_u("pde with P in the address", pde_encode(pa | (1ull << 63)), 0);
}

static void u4_cntl() {
    expect_u("the upstream default bits with retry off", cntl_native(0x03fffd07u), 0x03fffc07u);
    expect_u("Apple's vmfrag value 0x03fffd73 becomes depth 3 / block 0 / retry 0", cntl_native(0x03fffd73u), 0x03fffc07u);
    expect_u("a zero CNTL", cntl_native(0), 0x7u);   // ENABLE | DEPTH 3 << 1
    expect_u("bits outside the four fields survive", cntl_native(0xFC000000u) & 0xFC000000u, 0xFC000000u);
    for (uint32_t cur : { 0u, 0xFFFFFFFFu, 0x03fffd07u, 0x12345678u }) {
        const uint32_t v = cntl_native(cur);
        expect((v & kCntlRetryMask) == 0u, "RETRY is clear");
        expect((v & kCntlEnableMask) == 1u, "ENABLE is set");
        expect(((v & kCntlDepthMask) >> kCntlDepthShift) == 3u, "DEPTH is 3");
        expect((v & kCntlBlockMask) == 0u, "BLOCK_SIZE is 0");
        expect((v & ~(kCntlEnableMask | kCntlDepthMask | kCntlBlockMask | kCntlRetryMask)) ==
               (cur & ~(kCntlEnableMask | kCntlDepthMask | kCntlBlockMask | kCntlRetryMask)), "every other bit is kept");
    }
}

static GateArgs on() { GateArgs a {}; a.native = 1; return a; }
static void u5_gate() {
    expect(gate_decide(on()) == kGateOn, "native alone is ON");
    GateArgs off {}; expect(gate_decide(off) == kGateOff, "nothing set is OFF");
    // Absent native: OFF whatever else is armed (an existing config must behave as before).
    for (int i = 0; i < 8; i++) {
        GateArgs a {}; uint32_t *f[8] = { &a.accelExperiment, &a.bootChain, &a.sdmaQ1Test, &a.sdmaQnTest, &a.srbmTest, &a.vmfragTest,
                                          &a.eopBridge, &a.shaderCache };
        *f[i] = 1;
        expect(gate_decide(a) == kGateOff, "native absent stays OFF with any other arg armed");
    }
    static const uint32_t bits[8] = { kConflictAccel, kConflictBootChain, kConflictSdmaQ1, kConflictSdmaQn, kConflictSrbm, kConflictVmfrag,
                                      kConflictEopBridge, kConflictShaderCache };
    for (int i = 0; i < 8; i++) {   // each conflicting arg, alone, refuses, and names itself
        GateArgs a = on(); uint32_t *f[8] = { &a.accelExperiment, &a.bootChain, &a.sdmaQ1Test, &a.sdmaQnTest, &a.srbmTest, &a.vmfragTest,
                                              &a.eopBridge, &a.shaderCache };
        *f[i] = 29;   // e.g. navi48-boot-chain=29
        expect(gate_decide(a) == kGateRefused, "each conflicting arg refuses navi48-native=1");
        expect_u("and names itself", gate_conflicts(a), bits[i]);
    }
    GateArgs all = on();
    all.accelExperiment = all.bootChain = all.sdmaQ1Test = all.sdmaQnTest = all.srbmTest = all.vmfragTest = all.eopBridge = all.shaderCache = 1;
    expect_u("the live -1440 config's args name all eight", gate_conflicts(all), 0xFFu);
    expect(gate_decide(all) == kGateRefused, "the live -1440 boot-args plus native are refused");
    expect_u("a conflicting arg set to 0 is not armed", gate_conflicts(GateArgs{ 1, 0, 0, 0, 0, 0, 0, 0, 0 }), 0);
}

static void u6_fault() {
    expect(fault_leg_allowed(0), "no CRASH_ON bit: allowed");
    expect(fault_leg_allowed(0x00001FFCu), "the ENABLE_DEFAULT bits are not crash bits");
    expect(!fault_leg_allowed(0x40000000u), "CRASH_ON_NO_RETRY_FAULT refuses");
    expect(!fault_leg_allowed(0x80000000u), "CRASH_ON_RETRY_FAULT refuses");
    expect(!fault_leg_allowed(0xC0001FFCu), "both refuse");
    expect(kRefuseFaultLegIfCpHalts, "kRefuseFaultLegIfCpHalts is ON (reviewer decision)");
    expect(!fault_leg_allowed2(0, 0), "CP_DEBUG halt-disable clear: the fault leg is refused");
    expect(fault_leg_allowed2(0, kCpDebugHaltDisableMask), "halt-disable set and no CRASH_ON: allowed");
    expect(!fault_leg_allowed2(0xC0000000u, kCpDebugHaltDisableMask), "CRASH_ON still refuses with halt-disable set");
    expect(!fault_leg_allowed2(0x40000000u, kCpDebugHaltDisableMask), "CRASH_ON refuses even with CP_DEBUG's bit set");
    // The status match: our vmid, some error bits, the unmapped page.
    const uint64_t va = kVaUnmapped;
    const uint32_t aLo = (uint32_t)(va_strip(va) >> 12), aHi = (uint32_t)(va_strip(va) >> 44);
    const uint32_t st = (8u << 20) | (1u << 8) | (2u << 1);   // vmid 8, mapping error, walker error 2
    expect(fault_matches(st, aLo, aHi, 8, va), "a matching fault");
    expect(!fault_matches(st, aLo, aHi, 9, va), "another VMID's fault does not match");
    expect(!fault_matches((8u << 20), aLo, aHi, 8, va), "no error bits: nothing latched");
    expect(!fault_matches(st, aLo + 1, aHi, 8, va), "another page does not match");
    expect_u("fault address decode", fault_addr_va(aLo, aHi), va_strip(va));
    // A high-half fault address is reported stripped (bit 47 set, 4 bits of HI32).
    expect_u("high fault address", fault_addr_va((uint32_t)(0x800000100000ull >> 12), (uint32_t)(0x800000100000ull >> 44)), 0x800000100000ull);
}

// A model of the hardware walk over the block tree_build fills. Levels: root (PDB2) -> PDB1 -> PDB0 -> PTB, 512 entries each.
struct Model {
    std::vector<uint8_t> tbl;     // kTblBytes
    uint64_t tblPa, dataPa;
    uint64_t rd(uint64_t pa, uint32_t idx) const {
        const uint64_t off = pa - tblPa + (uint64_t)idx * 8u;
        if (pa < tblPa || off + 8 > tbl.size()) return 0;
        uint64_t v; std::memcpy(&v, &tbl[off], 8); return v;
    }
};
// Returns the PA a VA translates to, or ~0 with *level = the level that refused (2 = PDB2 ... 0 = PTB).
static uint64_t walk(const Model &m, uint64_t va, int *level) {
    uint64_t pa = m.tblPa + kTblPdb2;
    const uint32_t idx[4] = { idx_pdb2(va), idx_pdb1(va), idx_pdb0(va), idx_ptb(va) };
    for (int l = 0; l < 4; l++) {
        const uint64_t e = m.rd(pa, idx[l]);
        if (!entry_valid(e)) { *level = 3 - l; return ~0ull; }
        const bool leaf = entry_is_leaf(e);
        if (l < 3 && leaf) { *level = 3 - l; return ~0ull; }    // a leaf above the PTB: not a shape we build
        if (l == 3 && !leaf) { *level = 0; return ~0ull; }
        pa = entry_pa(e);
        if (l == 3) return pa | (va & 0xFFFull);
    }
    return ~0ull;
}
static void u7_tree() {
    Model m; m.tbl.assign(kTblBytes, 0); m.tblPa = 0x0000000AB0000000ull; m.dataPa = 0x0000000AB0100000ull;
    const bool ok = tree_build(m.tblPa, m.dataPa, [&](uint32_t page, uint32_t idx, uint64_t val) {
        const uint64_t off = (uint64_t)page + (uint64_t)idx * 8u;
        expect(off + 8 <= m.tbl.size(), "the entry lands inside the table block");
        if (off + 8 <= m.tbl.size()) std::memcpy(&m.tbl[off], &val, 8);
    });
    expect(ok, "tree_build succeeds on aligned addresses");
    for (uint32_t pg = 0; pg < kPgCount; pg++) {
        const uint64_t va = va_of_page(pg);
        int lvl = -1;
        expect_u("a mapped page translates to its own data page", walk(m, va + 0x123, &lvl), m.dataPa + (uint64_t)pg * 0x1000u + 0x123);
        expect_u("stripped VA resolves the same", walk(m, va_strip(va) + 0x123, &lvl), walk(m, va + 0x123, &lvl));
    }
    int lvl = -1;
    expect_u("the unmapped VA does not translate", walk(m, kVaUnmapped, &lvl), ~0ull);
    expect_u("and refuses at the entry in the PDB1 table (level 2)", (uint64_t)lvl, 2);
    expect_u("a VA past the last mapped page of a mapped PTB refuses at the PTB", walk(m, va_of_page(kPgMixedIb) + 0x1000, &lvl), ~0ull);
    expect_u("... level 0", (uint64_t)lvl, 0);
    expect_u("root[254] (inside the low half) is empty", walk(m, 0x00007F0000000000ull, &lvl), ~0ull);
    expect_u("... at the root (level 3)", (uint64_t)lvl, 3);
    expect(entry_valid(m.rd(m.tblPa + kTblPdb2, 0)) && entry_valid(m.rd(m.tblPa + kTblPdb2, 256)), "root[0] and root[256] present");
    int nvalid = 0; for (uint32_t i = 0; i < 512; i++) nvalid += entry_valid(m.rd(m.tblPa + kTblPdb2, i)) ? 1 : 0;
    expect_u("exactly two root entries", nvalid, 2);
    // Every directory entry in the block is a pointer (no P, no access bits); every leaf has P and RWX.
    for (uint32_t pgOff : { (uint32_t)kTblPdb2, (uint32_t)kTblPdb1Lo, (uint32_t)kTblPdb0Lo, (uint32_t)kTblPdb1Hi, (uint32_t)kTblPdb0Hi })
        for (uint32_t i = 0; i < 512; i++) {
            const uint64_t e = m.rd(m.tblPa + pgOff, i);
            if (entry_valid(e)) expect(!entry_is_leaf(e) && (e & (kPteRead | kPteWrite | kPteExec)) == 0, "a directory entry has no P and no access bits");
        }
    // A bad table address refuses the build.
    expect(!tree_build(0x0000000AB0000040ull, 0x0000000AB0100000ull, [](uint32_t, uint32_t, uint64_t) {}), "unaligned table PA refuses");
    expect(!tree_build(0x0000000AB0000000ull, 0x0000000AB0100800ull, [](uint32_t, uint32_t, uint64_t) {}), "unaligned data PA refuses");
    // Layout sanity: table pages are distinct, inside the block; data pages inside the data block.
    static const uint32_t pages[7] = { kTblPdb2, kTblPdb1Lo, kTblPdb0Lo, kTblPtbLo, kTblPdb1Hi, kTblPdb0Hi, kTblPtbHi };
    for (int i = 0; i < 7; i++) { expect(pages[i] + 0x1000 <= kTblBytes && (pages[i] & 0xFFF) == 0, "table page in range, 4 KiB aligned");
                                  for (int j = i + 1; j < 7; j++) expect(pages[i] != pages[j], "table pages are distinct"); }
    expect(kPgCount * 0x1000u <= kDataBytes, "data pages fit their block");
    for (uint32_t a = 0; a < kPgCount; a++)
        for (uint32_t b = a + 1; b < kPgCount; b++) expect(va_of_page(a) != va_of_page(b), "page VAs are distinct");
    // The poison word has bit 0 clear, so it can never be read as a VALID entry (MUST-FIX 1) - pinned in the source below.
}

// ---- U8 ------------------------------------------------------------------------------------------------------------------
struct Fake {
    LegResult scripted[kLegCount] {};      // result of each leg
    bool timeout[kLegCount] {};            // a timeout
    std::vector<int> order;                // legs in the order they ran
    int refusals = 0;
    Fake() { for (auto &r : scripted) r = kLegPass; }
    LegResult run(Leg leg, bool &timedOut) {
        order.push_back((int)leg);
        if (timeout[leg]) { timedOut = true; return kLegFail; }
        return scripted[leg];
    }
    void refusedFault() { refusals++; }
};
static void u8_sequence() {
    {   // all pass: strict order, fault last, PASS
        Fake f; Seq s = seq_drive(seq_open(true), f);
        const std::vector<int> want = { 0, 1, 2, 3, 4 };
        expect(f.order == want, "ORDER: low, high, mixed, compute, fault");
        expect(seq_result_pass(s) && seq_positive_pass(s) && !s.stopped, "all pass: RESULT PASS and POSITIVE PASS");
        expect(std::string(seq_fault_word(s)) == "PASS", "fault word PASS");
    }
    {   // ORDER: the fault leg is admitted only after every positive leg has been RECORDED
        Seq s = seq_open(true);
        expect(!seq_may_run_fault(s), "fresh sequence: no fault leg");
        s = seq_record(s, kLegLowWrite, kLegPass, false);
        s = seq_record(s, kLegHighWrite, kLegPass, false);
        s = seq_record(s, kLegMixed, kLegPass, false);
        expect(!seq_may_run_fault(s), "compute not run yet: no fault leg");
        s = seq_record(s, kLegCompute, kLegFail, false);   // a FAILED (not timed-out) positive leg has still run
        expect(seq_may_run_fault(s), "every positive leg recorded: the fault leg is admitted");
        expect_u("and it is the next leg", seq_next(s), kLegFault);
        {   // each admission condition on its own: the next-leg index, and every positive leg recorded
            Seq early = seq_open(true); early.next = kLegCompute;
            for (uint32_t i = 0; i < 4; i++) early.res[i] = kLegPass;
            expect(!seq_may_run_fault(early), "all four recorded but the cursor is still on compute: no fault leg");
            for (uint32_t hole = 0; hole < 4; hole++) {
                Seq h = seq_open(true); h.next = kLegFault;
                for (uint32_t i = 0; i < 4; i++) h.res[i] = i == hole ? kLegPending : kLegPass;
                expect(!seq_may_run_fault(h), "cursor at the fault leg but a positive leg never ran: no fault leg");
            }
        }
        Seq refused = s; refused.faultAllowed = false;
        expect(!seq_may_run_fault(refused), "a refused fault control blocks it");
        Seq stopped = s; stopped.stopped = true;
        expect(!seq_may_run_fault(stopped) && seq_next(stopped) == (uint32_t)kLegEnd, "a stopped sequence runs nothing");
    }
    {   // a timeout on a positive leg latches: nothing after it runs, the fault leg never starts
        for (uint32_t leg = 0; leg < 4; leg++) {
            Fake f; f.timeout[leg] = true;
            Seq s = seq_drive(seq_open(true), f);
            expect(s.stopped, "timeout latches the stop");
            expect_u("no leg runs after the timeout", f.order.size(), leg + 1);
            expect(f.order.back() == (int)leg, "the timed-out leg was the last to run");
            bool faultRan = false; for (int l : f.order) faultRan |= (l == kLegFault);
            expect(!faultRan, "the fault leg never starts after a timeout");
            expect(!seq_result_pass(s), "a timeout is never a PASS");
        }
    }
    {   // a timeout on the fault leg itself (its own or the NEXT fence): recorded, stop latched, verdict FAIL, positives still PASS
        Fake f; f.timeout[kLegFault] = true;
        Seq s = seq_drive(seq_open(true), f);
        expect(s.stopped && !seq_result_pass(s) && f.order.size() == 5, "fault-leg timeout: stopped, FAIL, all five legs ran");
        expect(seq_positive_pass(s) && std::string(seq_fault_word(s)) == "FAIL", "POSITIVE PASS (fault leg FAIL)");
    }
    {   // NO polarity fallback: the first leg failing once latches the stop and runs once
        Fake f; f.scripted[kLegLowWrite] = kLegFail;
        Seq s = seq_drive(seq_open(true), f);
        expect(f.order.size() == 1 && f.order[0] == 0, "the first leg failing: it ran exactly once, nothing else ran (no fallback)");
        expect(s.stopped && !seq_result_pass(s) && !seq_positive_pass(s), "stopped, not a PASS");
    }
    {   // a later leg's failure does not stop the run
        Fake f; f.scripted[kLegHighWrite] = kLegFail;
        Seq s = seq_drive(seq_open(true), f);
        const std::vector<int> want = { 0, 1, 2, 3, 4 };
        expect(f.order == want && !seq_result_pass(s) && !seq_positive_pass(s) && !s.stopped, "recorded and the run goes on");
    }
    {   // the fault leg refused by the fault control: recorded REFUSED, never run; POSITIVE can still PASS
        Fake f; Seq s = seq_drive(seq_open(false), f);
        const std::vector<int> want = { 0, 1, 2, 3 };
        expect(f.order == want && f.refusals == 1, "fault control refuses: the four positive legs run, the fault leg is refused");
        expect(s.res[kLegFault] == kLegRefused && !seq_result_pass(s), "recorded REFUSED and RESULT is not PASS");
        expect(seq_positive_pass(s) && std::string(seq_fault_word(s)) == "REFUSED", "POSITIVE PASS (fault leg REFUSED)");
    }
    {   // a positive-leg failure still lets the fault leg run (its own question), but both verdicts are FAIL/RESULT FAIL
        Fake f; f.scripted[kLegCompute] = kLegFail;
        Seq s = seq_drive(seq_open(true), f);
        expect(f.order.size() == 5 && s.res[kLegFault] == kLegPass && !seq_result_pass(s) && !seq_positive_pass(s), "compute FAIL: fault leg still runs, POSITIVE FAIL");
        expect(std::string(seq_fault_word(s)) == "PASS", "fault word PASS");
    }
    {   // a skipped fault leg (stopped before it) reads FAIL, never PASS
        Fake f; f.timeout[kLegMixed] = true;
        Seq s = seq_drive(seq_open(true), f);
        expect(std::string(seq_fault_word(s)) == "FAIL" && !seq_positive_pass(s), "stopped early: fault word FAIL");
    }
    expect(kWaitBoundUs <= 2000000u, "every wait is bounded at 2 s");
}

// The logger truncates a line at 512 bytes (n48log.cpp: char line[512]); a truncated verdict line loses its tail silently. Every
// NAT_LOG / native-s1b N48LOG format, at every conversion's widest, plus the prefix, must fit.
static uint32_t worst_width(const std::string &f) {
    uint32_t tot = 0;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] == '\\' && i + 1 < f.size()) { i++; if (f[i] != 'n') tot++; continue; }
        if (f[i] != '%') { tot++; continue; }
        size_t j = i + 1; std::string flags; uint32_t width = 0;
        while (j < f.size() && (f[j] == '#' || f[j] == '0' || f[j] == '-')) flags += f[j++];
        while (j < f.size() && f[j] >= '0' && f[j] <= '9') width = width * 10 + (uint32_t)(f[j++] - '0');
        int l = 0; while (j < f.size() && (f[j] == 'l' || f[j] == 'z')) { l++; j++; }
        const char c = j < f.size() ? f[j] : '%';
        uint32_t w = 12;
        if (c == '%') w = 1;
        else if (c == 's') w = 40;                     // our longest %s argument is 26 characters
        else if (c == 'x') w = (l ? 16u : 8u) + (flags.find('#') != std::string::npos ? 2u : 0u);
        else if (c == 'u' || c == 'd') w = l ? 20u : 11u;
        tot += width > w ? width : w;
        i = j;
    }
    return tot;
}
static std::vector<std::string> log_formats(const std::string &src, const std::string &macro) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = src.find(macro + "(", p)) != std::string::npos) {
        size_t q = p + macro.size() + 1; std::string fmt; bool any = false;
        for (;;) {
            while (q < src.size() && (src[q] == ' ' || src[q] == '\n' || src[q] == '\t')) q++;
            if (q >= src.size() || src[q] != '"') break;
            q++; any = true;
            while (q < src.size() && src[q] != '"') { if (src[q] == '\\') fmt += src[q++]; fmt += src[q++]; }
            q++;
        }
        if (any) out.push_back(fmt);
        p = q;
    }
    return out;
}

// ---- U9 ------------------------------------------------------------------------------------------------------------------
static void u9_pins(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string nat = slurp(src + "amd/native_s1b.cpp"), boot = slurp(src + "Navi48Bringup.cpp"), mes = slurp(src + "amd/mes_v12_1.cpp");
    const std::string plist = slurp(root + "/src/navi48-bringup/Info.plist");
    expect(!nat.empty() && !boot.empty() && !mes.empty() && !plist.empty(), "the source files were read (run from the repo root)");
    expect(mes.find("in.vmid_mask_gfxhub = 0xFE;") != std::string::npos, "mes_v12_1.cpp still sets vmid_mask_gfxhub = 0xFE (kMesVmidMaskGfxhub)");
    expect((plist.find("<string>0.0.600</string>") != std::string::npos || plist.find("<string>0.0.601</string>") != std::string::npos || plist.find("<string>0.0.602</string>") != std::string::npos || plist.find("<string>0.0.603</string>") != std::string::npos || plist.find("<string>0.0.604</string>") != std::string::npos || plist.find("<string>0.0.605</string>") != std::string::npos || plist.find("<string>0.0.606</string>") != std::string::npos || plist.find("<string>0.0.607</string>") != std::string::npos || plist.find("<string>0.0.608</string>") != std::string::npos || plist.find("<string>0.0.609</string>") != std::string::npos || plist.find("<string>0.0.610</string>") != std::string::npos || plist.find("<string>0.0.611</string>") != std::string::npos || plist.find("<string>0.0.612</string>") != std::string::npos || plist.find("<string>0.0.613</string>") != std::string::npos || plist.find("<string>0.0.614</string>") != std::string::npos || plist.find("<string>0.0.615</string>") != std::string::npos || plist.find("<string>0.0.616</string>") != std::string::npos || plist.find("<string>0.0.618</string>") != std::string::npos || plist.find("<string>0.0.620</string>") != std::string::npos) && plist.find("0.0.555") == std::string::npos, "Info.plist is 0.0.600 or its S1c successors 0.0.601 .. 0.0.618");
    // U4b: no log line can be cut by the 512-byte logger.
    {
        const std::vector<std::string> nl = log_formats(nat, "NAT_LOG");
        expect(nl.size() >= 20, "found the NAT_LOG formats");
        uint32_t widest = 0;
        for (const std::string &f : nl) { const uint32_t w = 27 + worst_width(f) + 1; if (w > widest) widest = w; expect(w <= 500, f.substr(0, 60).c_str()); }
        std::printf("native_s1b_test: widest native-s1b log line at the widest fields: %u bytes (limit 511)\n", widest);
        for (const std::string &f : log_formats(boot, "N48LOG"))
            if (f.compare(0, 10, "native-s1b") == 0) expect(15 + worst_width(f) + 1 <= 500, "Navi48Bringup.cpp native-s1b line fits");
    }
    // The kext drives the same pure functions the tests drive.
    expect(nat.find("seq_drive(s, runner)") != std::string::npos, "native_s1b.cpp runs the legs through seq_drive");
    expect(nat.find("tree_build(E.tblPa, E.dataPa, [&]") != std::string::npos, "native_s1b.cpp builds the tree with tree_build");
    expect(nat.find("cntl_native(gN.cntlBefore)") != std::string::npos, "the context CNTL comes from cntl_native");
    expect(nat.find("gate_decide") == std::string::npos && boot.find("n48native::gate_decide(ga)") != std::string::npos, "the gate is decided in Navi48Bringup.cpp by gate_decide");
    // ORDER inside the file: the fault control is read (and decides) before any leg is driven, the context is programmed after the
    // tables are built, and the legs run after the context is programmed.
    const size_t rdFault = nat.find("gN.l2FaultCntl = RREG32"), allowed = nat.find("gN.faultAllowed = fault_leg_allowed2"),
                 build = nat.find("if (!build_tables())"), prog = nat.find("WREG32(*dev, E.cntlReg, cntl_native"),
                 drive = nat.find("s = seq_drive(s, runner)");
    expect(rdFault != std::string::npos && allowed != std::string::npos && build != std::string::npos && prog != std::string::npos && drive != std::string::npos, "all anchors present");
    expect(rdFault < allowed && allowed < build && build < prog && prog < drive, "ORDER: read fault control -> gate the fault leg -> tables -> context -> legs");
    // Registers written: exactly the two fault-clear pulses and the seven context registers of ONE VMID. Nothing on MMHUB; no other hub helper.
    expect_u("WREG32 sites in native_s1b.cpp", count_of(nat, "WREG32("), 9);
    expect(nat.find("mmhub") == std::string::npos, "native_s1b.cpp never names the MMHUB (the console scans out through it)");
    expect(nat.find("gmc_clear_vm_faults(") == std::string::npos && nat.find("gmc_set_fault_enable_default(") == std::string::npos, "no MMHUB-touching helper, no fault-control write");
    expect(nat.find("WREG32(*dev, SOC15_REG_OFFSET_BIDX(*dev, IPBlock::GC, 0, 0x1e1f)") == std::string::npos, "CP_DEBUG is read only");
    // MUST-FIX 1: the data block is zeroed before the tables are built; the poison has bit 0 clear.
    {
        const size_t zero = nat.find("bar0_memset_vram(*dev, E.dataOff, 0, kDataBytes);");
        expect(zero != std::string::npos && zero < build && nat.find("gLatched = true;") < zero, "the whole data block is zeroed before build_tables, right after the latch");
        expect(nat.find("kPoison   = 0xDEADBEEEu;") != std::string::npos && (0xDEADBEEEu & 1u) == 0u, "kPoison has bit 0 clear");
        expect(nat.find("0xDEADBEEFu") == std::string::npos, "no poison word with bit 0 set remains in native_s1b.cpp");
    }
    // SHOULD-FIX 1/2/4: no polarity fallback left; the POSITIVE line; the latch and its two refusal sites.
    expect(nat.find("inverted") == std::string::npos && nat.find("polarity") == std::string::npos, "the inverse-polarity fallback is gone from native_s1b.cpp");
    expect(nat.find("NAT_LOG(\"POSITIVE %s (fault leg %s)\"") != std::string::npos, "the POSITIVE verdict line exists");
    expect(nat.find("POSITIVE %s (fault leg %s); RESULT %s") != std::string::npos, "the property carries POSITIVE before RESULT");
    expect(nat.find("gLatched = true;") != std::string::npos && count_of(nat, "gLatched = true;") == 1, "the latch is set in one place");
    {
        const std::string uc = slurp(src + "Navi48UserClient.cpp");
        expect(uc.find("IOReturn Navi48UserClient::doSubmitIB(IOExternalMethodArguments *args) {\n\tif (amdgpu::native_s1b_refuse(1)) return kIOReturnNotPermitted;") != std::string::npos,
               "SubmitIB refuses first thing once the latch is set");
        expect(boot.find("if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;") != std::string::npos, "the accel verbs (all but read-only action 0) refuse once latched");
        expect(nat.find("bool native_s1b_refuse(uint32_t site) {\n    if (!gLatched) return false;") != std::string::npos, "refuse() is false until the latch is set");
    }
    expect(nat.find("kNativeVmid") != std::string::npos && nat.find("E.vmid = kNativeVmid") != std::string::npos, "the VMID is the pure constant");
    expect(nat.find("kWaitBoundUs") != std::string::npos && nat.find("cp_wait_fence(*E.cp, fence, kWaitBoundUs") != std::string::npos, "the fence wait is the 2 s bound");
    // The run in Navi48Bringup.cpp: after the ladder, behind the gate, once; the gate is read before the ladder.
    const size_t ladder = boot.find("kern_return_t r = amdgpu::bringup_to(ctx"), gateAt = boot.find("const uint32_t nativeGate = nativeS1bGate(this);"),
                 runAt = boot.find("(void)amdgpu::native_s1b_run(ctx);");
    expect(gateAt != std::string::npos && ladder != std::string::npos && runAt != std::string::npos, "call sites present");
    expect(gateAt < ladder && ladder < runAt, "ORDER: gate, then the ladder, then the native run");
    expect_u("native_s1b_run has one call site", count_of(boot, "native_s1b_run("), 1);
    expect(boot.substr(runAt > 80 ? runAt - 80 : 0, 80).find("if (nativeGate == n48native::kGateOn)") != std::string::npos, "the run sits directly under the gate test");
    // The gate reads every arg the review named, in the same spelling the kext parses elsewhere.
    for (const char *a : { "navi48-native", "navi48-accel-experiment", "navi48-boot-chain", "navi48-sdma-q1-test", "navi48-sdma-qn-test",
                           "navi48-srbm-test", "navi48-vmfrag-test", "navi48-eop-bridge", "navi48-shader-cache" }) {
        expect(count_of(boot, std::string("bootArgU32(\"") + a + "\")") == 1, a);
        expect(count_of(boot, std::string("\"") + a + "\"") >= 1, a);
    }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    u1_vmid(); u2_va(); u3_entries(); u4_cntl(); u5_gate(); u6_fault(); u7_tree(); u8_sequence(); u9_pins(root);
    std::printf("native_s1b_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
