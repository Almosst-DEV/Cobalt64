// sdma_drain_xlat_test.cpp — drive the drain's IB pass and ring walk (0.0.269) through every
// transform, every refusal and the idempotence the drain relies on, on packets built the way Apple's own
// emitters build them (addresses in the header's stride table).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -O1 \
//         -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/sdma_drain_xlat_test.cpp -o /tmp/dxtest && /tmp/dxtest
//
// It compiles the SAME header the kext compiles (and that xlatregs now shares), so there is no second
// implementation that could drift from the one that runs.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sdma_drain_xlat.h"

static int gFail = 0, gRun = 0;
#define CHECK(cond, ...) do { gRun++; if (!(cond)) { gFail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static constexpr uint32_t kGc = 0x1260;                       // GC base[0] (the project notes)
static constexpr uint64_t kBot = 0x3d6c00000ull, kTop = 0x3db000000ull;   // Apple's 68 MiB arena
static constexpr uint32_t kCtx2BaseAppleIp = 0x1693 - 0x28;   // what Apple emits for CONTEXT2 PT base lo
static constexpr uint32_t kEng6AckAppleIp  = 0x1650 - 0x28;   // a gfx12 GCVM invalidate register, Apple-numbered

// AMDGFX10SDMAChannel::initializeVMInvalidateFrame @0xbe22e5a: REG_WRITEs 0xf000000e (be22efd) and a register-form
// POLL_REGMEM 0x30000008 with DW5 0x0fff0004 (be230b8 / be230ea).
static std::vector<uint32_t> vm_program_frame()
{
    std::vector<uint32_t> f;
    auto regw = [&](uint32_t ip, uint32_t v) { f.push_back(0xf000000eu); f.push_back(kGc + ip); f.push_back(v); };
    regw(kCtx2BaseAppleIp, 0xd6c00000u);          // base lo, VALID bit clear as Apple writes it
    regw(kCtx2BaseAppleIp + 1, 0x00000003u);      // base hi
    regw(0x1614, 0x12345678u);                    // an MC aperture SOURCE: must be left alone (0.0.52)
    regw(0x0100, 0xdeadbeefu);                    // not a GCVM register: left alone
    f.push_back(0x30000008u); f.push_back((kGc + kEng6AckAppleIp) << 2); f.push_back(0);
    f.push_back(0x4u); f.push_back(0x4u); f.push_back(0x0fff0004u);
    f.push_back(0x00000000u); f.push_back(0x00000000u);   // NOP padding
    return f;
}

// writeWritePTEPDECommand @0xbe23630: PTEPDE (10) + memory POLL_REGMEM 0x80000008 with DW5 0x10004 (6).
static void ptepde_pair(std::vector<uint32_t> &f, uint64_t dst, uint32_t lo, uint32_t hi, uint64_t val,
                        uint32_t incr, uint32_t countMinus1)
{
    f.push_back(0x0000000cu); f.push_back((uint32_t)dst); f.push_back((uint32_t)(dst >> 32));
    f.push_back(lo); f.push_back(hi); f.push_back((uint32_t)val); f.push_back((uint32_t)(val >> 32));
    f.push_back(incr); f.push_back(0); f.push_back(countMinus1);
    f.push_back(0x80000008u); f.push_back((uint32_t)dst & ~7u); f.push_back((uint32_t)(dst >> 32));
    f.push_back(0); f.push_back(0); f.push_back(0x00010004u);
}


// ---------------------------------------------------------------------------------------------------------------------
// 0.0.358: THE ACK WAIT MADE REAL. The frozen copy below is sdma_drain_translate_ib as committed at 0.0.357,
// extracted from git (`git show HEAD:.../sdma_drain_xlat.h`), renamed only.
// ---------------------------------------------------------------------------------------------------------------------
static int old_translate_ib_0357(uint32_t *buf, uint32_t n, uint32_t gcBase,
                                          uint64_t arenaBot, uint64_t arenaTop, SdmaDrainIbStats *st)
{
    if (!buf || !st || n == 0 || n > kDxMaxDwords || gcBase == 0) { if (st) st->refused = 3; return -1; }
    // pass 1: every packet known, and the walk lands exactly on the end
    for (uint32_t k = 0; k < n; ) {
        const uint32_t h = buf[k];
        const uint32_t s = sdma_drain_ib_stride(h);
        if (s == 0) { st->refused = 1; st->refusedOp = h & 0xFFu; st->refusedAt = k; st->refusedHeader = h; return -1; }
        if (k + s > n) { st->refused = 2; st->refusedOp = h & 0xFFu; st->refusedAt = k; st->refusedHeader = h; return -1; }
        st->packets++;
        switch (h & 0xFFu) {
        case 0:  st->nops++; break;
        case 5:  st->fences++; break;
        case 6:  st->traps++; break;
        case 8:  if ((h >> 31) & 1u) st->memPolls++; else st->regPolls++; break;
        case 11: st->fills++; break;
        case 12: st->ptepdes++; break;
        case 13: st->timestamps++; break;
        case 14: st->regWrites++; break;
        case 16: st->gpuvmInv++; break;
        default: break;
        }
        k += s;
    }
    // pass 2: the three transforms xlatregs + neuterpoll apply, in one pass
    bool changed = false;
    for (uint32_t k = 0; k < n; ) {
        const uint32_t h = buf[k];
        const uint32_t s = sdma_drain_ib_stride(h);
        const uint32_t op = h & 0xFFu;
        if (op == 14) {
            if (h == 0x0000000eu) {
                st->regWritesAlreadyV7++;
            } else {
                const uint32_t abs = buf[k + 1];
                bool done = false;
                if (abs > gcBase) {
                    const uint32_t ip = abs - gcBase;
                    if (gcvm_translatable(ip)) {
                        buf[k]     = 0x0000000eu;
                        buf[k + 1] = (abs + kGcvmShift) << 2;
                        if (ip + kGcvmShift == kGcvmCtx2PtBaseLo && (buf[k + 2] & 1u) == 0) {
                            buf[k + 2] |= 1u;
                            st->ctx2BasesValidated++;
                        }
                        st->regWritesConverted++;
                        changed = done = true;
                    } else if (ip >= kGcmcXLo && ip <= kGcmcXHi) {
                        st->regWritesMcExcluded++;
                        done = true;
                    }
                }
                if (!done) st->regWritesLeft++;
            }
        } else if (op == 12) {
            if (ptepde_to_gfx12(&buf[k], arenaBot, arenaTop, st->pte)) changed = true;
        } else if (op == 8 && !((h >> 31) & 1u)) {
            if (buf[k + 4] == 0) {
                st->pollsAlreadyNeutered++;
            } else {
                const uint32_t abs = buf[k + 1] >> 2;
                if (abs > gcBase && gcvm_translatable(abs - gcBase)) {
                    buf[k + 1] = (abs + kGcvmShift) << 2;
                    st->pollsShifted++;
                }
                buf[k + 3] = 0;     // ref
                buf[k + 4] = 0;     // mask
                st->pollsNeutered++;
                changed = true;
            }
        }
        k += s;
    }
    return changed ? 1 : 0;
}

static constexpr uint32_t kEng6ReqApple = 0x164d - 0x28, kEng6AckApple = 0x165f - 0x28;   // 0x1625, 0x1637

// Apple's VM-program IB as dumped it after translation, rebuilt in Apple's own form: nine SRBM-form REG_WRITEs
// (CONTEXT2 base/start/end lo+hi, ENG6 range lo/hi, ENG6_REQ <- 0x00990004), NOP padding, the register POLL at dword 0x21
// (`30000008 0000a25c 00000000 00000004 00000004 0fff0004`), NOP padding to IB_SIZE 0x32.
static std::vector<uint32_t> apple_vm_ib(uint32_t reqVal = 0x00990004u, uint32_t pollRef = 4u, uint32_t pollMask = 4u,
                                         uint32_t pollHdr = 0x30000008u, uint32_t reqIp = kEng6ReqApple,
                                         uint32_t ackIp = kEng6AckApple)
{
    std::vector<uint32_t> f;
    auto regw = [&](uint32_t abs, uint32_t v) { f.push_back(0xf000000eu); f.push_back(abs); f.push_back(v); };
    regw(0x28cb, 0xd6c00000u); regw(0x28cc, 0x3u); regw(0x28eb, 0x00400000u); regw(0x28ec, 0u);
    regw(0x290b, 0x023fffffu); regw(0x290c, 0u); regw(0x28af, 0xffffffffu); regw(0x28b0, 0x1fu);
    regw(kGc + reqIp, reqVal);
    while (f.size() < 0x21) f.push_back(0u);
    f.push_back(pollHdr); f.push_back((kGc + ackIp) << 2); f.push_back(0u); f.push_back(pollRef); f.push_back(pollMask);
    f.push_back(0x0fff0004u);
    while (f.size() < 0x32) f.push_back(0u);
    return f;
}

static int ack_checks(int mutant)
{
    const int before = gFail;
    // mutant 1: the wait kept WITHOUT the same-IB request clause (a wait on an ACK nobody asked for)
    // mutant 2: the kept poll recognised only while the wait is on (a reused IB re-walked after the switch-off is shifted twice)
    auto xl = [&](std::vector<uint32_t> &f, SdmaDrainIbStats &st, uint32_t flags) {
        if (mutant == 1 && flags) {
            std::vector<uint32_t> g = f;
            const int rc = sdma_drain_translate_ib_ex(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st, flags);
            // the mutant: any renumbered ENGn_ACK poll with ref == mask is kept, whatever was requested
            for (uint32_t k = 0; k + 5 < f.size(); k++)
                if (f[k] == 0x30000008u && g[k + 4] && !f[k + 4] && ((f[k + 1] >> 2) - kGc) >= 0x1659 && ((f[k + 1] >> 2) - kGc) < 0x166b)
                    { f[k + 3] = g[k + 3]; f[k + 4] = g[k + 4]; }
            return rc;
        }
        if (mutant == 2 && !flags)   // recognition gated by the flag: with the wait switched off, the 0.0.357 rule runs
            return old_translate_ib_0357(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        return sdma_drain_translate_ib_ex(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st, flags);
    };
    // A. Apple's IB, wait kept: the poll is renumbered to ENG6_ACK 0x28bf and ref/mask stay 4/4; nothing else differs from
    //    the neutered translation.
    {
        auto a = apple_vm_ib(), b = apple_vm_ib();
        SdmaDrainIbStats sa {}, sb {};
        const int ra = xl(a, sa, kDxKeepInvAck), rb = sdma_drain_translate_ib(b.data(), (uint32_t)b.size(), kGc, kBot, kTop, &sb);
        CHECK(ra == 1 && rb == 1, "rc %d %d", ra, rb);
        CHECK(a[0x21] == 0x30000008u && a[0x22] == (0x28bfu << 2) && a[0x24] == 4u && a[0x25] == 4u && a[0x26] == 0x0fff0004u,
              "kept poll %08x %08x ref %08x mask %08x dw5 %08x", a[0x21], a[0x22], a[0x24], a[0x25], a[0x26]);
        CHECK(b[0x24] == 0u && b[0x25] == 0u, "flags 0 still neuters");
        uint32_t diff = 0; for (uint32_t k = 0; k < a.size(); k++) if (a[k] != b[k] && k != 0x24 && k != 0x25) diff++;
        CHECK(diff == 0, "%u dword(s) other than ref/mask differ from the neutered translation", diff);
        CHECK(a[24] == 0x0000000eu && a[25] == ((kGc + 0x164d) << 2) && a[26] == 0x00990004u, "REQ write %08x %08x %08x", a[24], a[25], a[26]);
        CHECK(sa.pollsKept == 1 && sa.pollsNeutered == 0 && sa.pollsShifted == 1, "kept counts %u %u %u", sa.pollsKept, sa.pollsNeutered, sa.pollsShifted);
        // B. Idempotence: a second pass over OUR output changes nothing - with the wait on, AND after it is switched off.
        auto once = a;
        for (uint32_t fl = 0; fl < 2; fl++) {
            SdmaDrainIbStats s2 {};
            const int r2 = xl(a, s2, fl ? kDxKeepInvAck : 0u);
            CHECK(r2 == 0 && a == once, "second pass (flags %u) changed our output (rc %d, reg %08x)", fl, r2, a[0x22]);
            CHECK(s2.pollsAlreadyKept == 1 && s2.regWritesAlreadyV7 == 9, "second-pass counts %u %u", s2.pollsAlreadyKept, s2.regWritesAlreadyV7);
        }
    }
    // C. NEGATIVE CONTROLS: each differs from Apple's shape in ONE way and must come out neutered, exactly as 0.0.357 did.
    struct Neg { const char *what; std::vector<uint32_t> ib; };
    const Neg negs[] = {
        { "no VMID bit requested for the polled one", apple_vm_ib(0x00990008u) },
        { "the REQ is on another engine",             apple_vm_ib(0x00990004u, 4u, 4u, 0x30000008u, kEng6ReqApple - 1u) },
        { "ref != mask",                              apple_vm_ib(0x00990004u, 0u, 4u) },
        { "func is not 'equal'",                      apple_vm_ib(0x00990004u, 4u, 4u, 0x40000008u) },
        { "mask outside the per-VMID bits",           apple_vm_ib(0x00990004u, 0x10004u, 0x10004u) },
        { "polls a register that is not an ACK",      apple_vm_ib(0x00990004u, 4u, 4u, 0x30000008u, kEng6ReqApple, 0x1650u - 0x28u) },
    };
    for (const Neg &n : negs) {
        auto a = n.ib, b = n.ib;
        SdmaDrainIbStats sa {}, sb {};
        xl(a, sa, kDxKeepInvAck);
        old_translate_ib_0357(b.data(), (uint32_t)b.size(), kGc, kBot, kTop, &sb);
        CHECK(a == b && sa.pollsNeutered == 1 && sa.pollsKept == 0, "negative control '%s' was not neutered as 0.0.357 did", n.what);
    }
    // D. The poll BEFORE its request (the request later in the IB) is not kept.
    {
        auto f = apple_vm_ib();
        std::vector<uint32_t> g(f.begin() + 0x21, f.begin() + 0x27);
        g.insert(g.end(), f.begin(), f.begin() + 0x21);
        auto h = g;
        SdmaDrainIbStats sa {}, sb {};
        xl(g, sa, kDxKeepInvAck);
        old_translate_ib_0357(h.data(), (uint32_t)h.size(), kGc, kBot, kTop, &sb);
        CHECK(g == h && sa.pollsKept == 0, "a poll before its request was kept");
    }
    return gFail - before;
}

// The property, against the frozen 0.0.357 function over generated IBs built from Apple's packet forms:
//   flags 0 -> byte-identical output, return code and refusal on every IB;
//   flags 1 -> identical except ref/mask of polls it KEPT, which equal the INPUT's, where 0.0.357 wrote 0/0.
static void ack_property()
{
    uint64_t x = 0xB7E151628AED2A6Bull, ibs = 0, kept = 0, bad0 = 0, bad1 = 0;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    const uint32_t ips[] = { 0x1625, 0x1637, 0x161f, 0x1620, 0x1630, 0x1631, 0x1642, 0x164f, 0x1650, 0x166b, 0x1614,
                             0x0100, 0x1693 - 0x28, 0x1647, 0x1659, 0x165f, 0x164d };
    for (uint32_t it = 0; it < 300000u; it++) {
        std::vector<uint32_t> f;
        const uint32_t np = 1u + (uint32_t)(rnd() % 12u);
        for (uint32_t p = 0; p < np; p++) {
            const uint64_t r = rnd();
            const uint32_t ip = ips[(r >> 8) % (sizeof(ips) / 4)];
            const uint32_t bits = (uint32_t)(1u << ((r >> 16) % 16u)) | ((r >> 40) & 1u ? (uint32_t)(1u << ((r >> 44) % 16u)) : 0u);
            switch (r % 7u) {
            case 0: f.push_back(0xf000000eu); f.push_back(kGc + ip); f.push_back(0x00990000u | bits); break;
            case 1: f.push_back(0x0000000eu); f.push_back((kGc + ip) << 2); f.push_back(0x00990000u | bits); break;
            case 2: case 3: {
                const uint32_t func = (r >> 30) & 1u ? 3u : (uint32_t)((r >> 50) % 8u);
                const uint32_t ref = (r >> 29) & 1u ? bits : (uint32_t)(r >> 20);
                const uint32_t mask = (r >> 28) & 1u ? bits : (uint32_t)((r >> 33) & 0xFFFFu);
                f.push_back((func << 28) | 8u); f.push_back((kGc + ip) << 2); f.push_back(0u); f.push_back(ref); f.push_back(mask);
                f.push_back(0x0fff0004u); break; }
            case 4: f.push_back(0u); break;
            case 5: f.push_back(0x00030005u); f.push_back(0x180u); f.push_back(0x84u); f.push_back(1u); break;
            default: f.push_back(0x00000010u); f.push_back(0x00890204u); f.push_back(0x00800006u); f.push_back(0u); break;
            }
        }
        ibs++;
        for (uint32_t fl = 0; fl < 2; fl++) {
            auto a = f, b = f;
            SdmaDrainIbStats sa {}, sb {};
            const int ra = sdma_drain_translate_ib_ex(a.data(), (uint32_t)a.size(), kGc, kBot, kTop, &sa, fl);
            const int rb = old_translate_ib_0357(b.data(), (uint32_t)b.size(), kGc, kBot, kTop, &sb);
            bool ok = ra == rb && sa.refused == sb.refused;
            // Our own kept-poll signature cannot occur in 0.0.357's world; skip generated IBs that happen to carry it.
            if (sa.pollsAlreadyKept) continue;
            for (uint32_t k = 0; ok && k < a.size(); k++) {
                if (a[k] == b[k]) continue;
                // only a kept poll's ref (k) / mask (k+1) may differ, and only as input-vs-zero
                const bool refPos = k >= 3 && a[k - 3] == b[k - 3] && (b[k - 3] & 0x800000FFu) == 8u && b[k] == 0u && b[k + 1] == 0u && a[k] == f[k];
                const bool maskPos = k >= 4 && (b[k - 4] & 0x800000FFu) == 8u && b[k] == 0u && b[k - 1] == 0u && a[k] == f[k];
                if (!fl || !(refPos || maskPos)) ok = false;
            }
            if (fl) kept += sa.pollsKept;
            if (!ok) (fl ? bad1 : bad0)++;
        }
    }
    std::printf("ack property: %llu generated IBs; flags 0: %llu differ from 0.0.357; flags 1: %llu differ beyond a kept "
                "wait; %llu waits kept\n", (unsigned long long)ibs, (unsigned long long)bad0, (unsigned long long)bad1,
                (unsigned long long)kept);
    CHECK(bad0 == 0, "flags 0 differs from the frozen 0.0.357 function on %llu IB(s)", (unsigned long long)bad0);
    CHECK(bad1 == 0, "flags 1 differs beyond a kept wait on %llu IB(s)", (unsigned long long)bad1);
    CHECK(kept > 0, "the property is vacuous: no wait was ever kept");
}

int main()
{
    // 1. The VM-program frame: register writes converted, the poll renumbered AND neutered.
    {
        auto f = vm_program_frame();
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == 1, "vm frame rc %d", rc);
        CHECK(f[0] == 0x0000000eu && f[1] == ((kGc + kCtx2BaseAppleIp + 0x28) << 2), "base lo write %08x %08x", f[0], f[1]);
        CHECK(f[2] == 0xd6c00001u, "CONTEXT2 base not validated: %08x", f[2]);
        CHECK(f[3] == 0x0000000eu && f[4] == ((kGc + kCtx2BaseAppleIp + 1 + 0x28) << 2) && f[5] == 3u, "base hi");
        CHECK(f[6] == 0xf000000eu && f[7] == kGc + 0x1614 && f[8] == 0x12345678u, "MC aperture source was touched");
        CHECK(f[9] == 0xf000000eu && f[10] == kGc + 0x0100, "non-GCVM write was touched");
        CHECK(f[12] == 0x30000008u && f[13] == ((kGc + kEng6AckAppleIp + 0x28) << 2), "poll not renumbered %08x", f[13]);
        CHECK(f[15] == 0 && f[16] == 0 && f[17] == 0x0fff0004u, "poll not neutered ref %08x mask %08x", f[15], f[16]);
        CHECK(st.regWrites == 4 && st.regWritesConverted == 2 && st.regWritesMcExcluded == 1 && st.regWritesLeft == 1,
              "regw counts %u %u %u %u", st.regWrites, st.regWritesConverted, st.regWritesMcExcluded, st.regWritesLeft);
        CHECK(st.ctx2BasesValidated == 1 && st.pollsNeutered == 1 && st.pollsShifted == 1 && st.nops == 2,
              "poll/ctx counts");
        // idempotence: a second pass changes nothing
        auto once = f;
        SdmaDrainIbStats st2 {};
        rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st2);
        CHECK(rc == 0 && f == once, "second pass changed the frame (rc %d)", rc);
        CHECK(st2.regWritesAlreadyV7 == 2 && st2.pollsAlreadyNeutered == 1, "second-pass counts");
        (void)orig;
    }
    // 2. PTEPDE root / L1 pointer / leaf, each with its memory poll: entries re-encoded, memory polls untouched.
    {
        std::vector<uint32_t> f;
        ptepde_pair(f, kBot, 1u, 0x20000000u, kBot + 0x1000, 0, 0);                  // root PDE (BFS 4)
        ptepde_pair(f, kBot + 0x1000, 1u, 0x01000000u, kBot + 0x2000, 0, 0);          // L1 -> sub-table (TF)
        ptepde_pair(f, kBot + 0x2000, 0x77u, 0x00010000u, 0x7131c5000ull, 0x1000, 15); // 16 sysmem leaves, MTYPE 1
        ptepde_pair(f, 0x100000000ull, 0x77u, 0x00010000u, 0x7131c5000ull, 0x1000, 0); // dst OUTSIDE the arena
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == 1, "ptepde rc %d", rc);
        CHECK(f[4] == 0x10000000u, "root hi %08x", f[4]);
        CHECK(f[16 + 4] == 0u, "L1 hi %08x", f[16 + 4]);
        CHECK(f[32 + 4] == 0x80400000u, "leaf hi %08x", f[32 + 4]);
        CHECK(f[48 + 4] == 0x00010000u, "foreign leaf touched %08x", f[48 + 4]);
        for (unsigned k = 10; k < f.size(); k += 16)
            CHECK(f[k] == orig[k] && f[k + 3] == orig[k + 3] && f[k + 4] == orig[k + 4], "memory poll at %u touched", k);
        CHECK(st.pte.roots == 1 && st.pte.l1ptrs == 1 && st.pte.leaves == 1 && st.pte.foreign == 1 && st.memPolls == 4,
              "pte counts %u %u %u %u mem %u", st.pte.roots, st.pte.l1ptrs, st.pte.leaves, st.pte.foreign, st.memPolls);
        auto once = f;
        SdmaDrainIbStats st2 {};
        rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st2);
        CHECK(rc == 0 && f == once, "ptepde second pass changed something");
    }
    // 2b. D3 (0.0.417): the SOURCE entry's bit 58 is counted (REPORT ONLY) on translated leaves.
    // Two leaves, one with bit 58 set (hi 0x04010000, gfx10 NOALLOC / gfx12 PTE DCC) and one without; the
    // translation is otherwise the same and the count must be exactly 1.
    {
        std::vector<uint32_t> f;
        ptepde_pair(f, kBot,          0x77u, 0x04010000u, kBot + 0x1000, 0, 0);   // leaf: bit 58 SET
        ptepde_pair(f, kBot + 0x2000, 0x77u, 0x00010000u, kBot + 0x3000, 0, 0);   // leaf: bit 58 clear
        auto orig = f;
        SdmaDrainIbStats st {};
        const int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == 1, "bit58 rc %d", rc);
        CHECK(st.pte.leaves == 2 && st.pteLeafBit58 == 1,
              "bit58 counts: leaves %u bit58 %u", st.pte.leaves, st.pteLeafBit58);
        // the flag is REPORT ONLY: both leaves are re-encoded exactly as without it (P set, MTYPE moved).
        CHECK(f[4] == 0x84400000u && f[20] == 0x80400000u, "bit58 leaf hi %08x %08x", f[4], f[20]);
        CHECK(orig[4] == 0x04010000u, "source untouched %08x", orig[4]);
    }
    // 3. GPUVM_INV (writeVMInvalidateCommand @0xbe234d8, 4 dwords), TIMESTAMP 0x20d (3), FENCE 0x30005 (4),
    //    CONST_FILL 0x8000000b (5), TRAP (1): known, passed through, counted - and an IB of 16 + k*4 dwords lands.
    {
        std::vector<uint32_t> f;
        ptepde_pair(f, kBot + 0x2000, 0x77u, 0x80400000u, 0x7131c5000ull, 0x1000, 0);   // already gfx12
        for (int i = 0; i < 5; i++) { f.push_back(0x10u); f.push_back(0x00010002u + i); f.push_back(0x1650u); f.push_back(0x1651u); }
        f.push_back(0x00030005u); f.push_back(0x180u); f.push_back(0x84u); f.push_back(0u);
        f.push_back(0x0000020du); f.push_back(0x200u); f.push_back(0x84u);
        f.push_back(0x8000000bu); f.push_back((uint32_t)kBot); f.push_back((uint32_t)(kBot >> 32)); f.push_back(0); f.push_back(0x3ffffc);
        f.push_back(0x00000006u);
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == 0 && f == orig, "pass-through IB changed (rc %d)", rc);
        CHECK(st.gpuvmInv == 5 && st.timestamps == 1 && st.fences == 1 && st.fills == 1 && st.traps == 1 &&
              st.pte.already == 1, "pass-through counts gpuvm %u ts %u", st.gpuvmInv, st.timestamps);
    }
    // 4. REFUSALS leave the buffer untouched: an unknown opcode AFTER a translatable packet, and an overrun.
    {
        auto f = vm_program_frame();
        f.push_back(0x00000002u);          // WRITE (op 2): not in the drain's table
        f.push_back(0x1u); f.push_back(0x2u); f.push_back(0x0u); f.push_back(0x5u);
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == -1 && st.refused == 1 && st.refusedOp == 2 && st.refusedAt == 20 && f == orig,
              "unknown op: rc %d refused %u op %u at %u changed %d", rc, st.refused, st.refusedOp, st.refusedAt, f != orig);
    }
    {
        auto f = vm_program_frame();
        f.push_back(0x0000000cu); f.push_back(0u); f.push_back(0u);   // PTEPDE header with 2 of its 9 operands
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == -1 && st.refused == 2 && f == orig, "overrun: rc %d refused %u", rc, st.refused);
    }
    {
        uint32_t one = 0x10u;
        SdmaDrainIbStats st {};
        CHECK(sdma_drain_translate_ib(&one, 1, kGc, kBot, kTop, &st) == -1 && st.refused == 2, "GPUVM_INV short");
        SdmaDrainIbStats st2 {};
        CHECK(sdma_drain_translate_ib(nullptr, 4, kGc, kBot, kTop, &st2) == -1 && st2.refused == 3, "null buffer");
        SdmaDrainIbStats st3 {};
        CHECK(sdma_drain_translate_ib(&one, 1, 0, kBot, kTop, &st3) == -1 && st3.refused == 3, "no GC base");
    }
    // 5. A NOP with a count skips its payload (a payload dword that LOOKS like a REG_WRITE is not touched).
    {
        std::vector<uint32_t> f = { 0x00030000u, 0xf000000eu, kGc + kCtx2BaseAppleIp, 0xd6c00000u };
        auto orig = f;
        SdmaDrainIbStats st {};
        int rc = sdma_drain_translate_ib(f.data(), (uint32_t)f.size(), kGc, kBot, kTop, &st);
        CHECK(rc == 0 && f == orig && st.nops == 1 && st.packets == 1, "NOP count: rc %d packets %u", rc, st.packets);
    }
    // 6. The ring walk, on the chan-14 frame `ringib` dumped (COND_EXE, NOPs, INDIRECT 0x80000004, FENCE, TRAP).
    {
        std::vector<uint32_t> r(128, 0u);
        const uint32_t head[] = { 0x00000009, 0x0000019c, 0x00000084, 0x00000001, 0x0000007b, 0, 0, 0, 0, 0,
                                  0x80000004, 0x00800000, 0x00000084, 0x00000055, 0, 0 };
        std::memcpy(r.data(), head, sizeof(head));
        r[0x2b] = 0x00030005; r[0x2c] = 0x180; r[0x2d] = 0x84; r[0x2e] = 1; r[0x2f] = 0x6;
        SdmaDrainRingIb ibs[4] {};
        uint32_t more = 9, stopAt = 0, stopHdr = 1;
        uint32_t n = sdma_drain_ring_walk(r.data(), 128, ibs, 4, &more, &stopAt, &stopHdr);
        CHECK(n == 1 && ibs[0].ib == 0x8400800000ull && ibs[0].dwords == 0x55 && ibs[0].at == 10, "walk IB");
        CHECK(stopAt == 128 && more == 0 && stopHdr == 0, "walk end %u more %u", stopAt, more);
        // two frames back to back, one with an unknown ring opcode in the second
        std::vector<uint32_t> r2 = r;
        r2.insert(r2.end(), r.begin(), r.end());
        r2[128 + 0x30] = 0x00000011u;      // op 17 (GCR_REQ) - not a ring packet the walk knows
        n = sdma_drain_ring_walk(r2.data(), 256, ibs, 4, &more, &stopAt, &stopHdr);
        CHECK(n == 2 && stopAt == 128 + 0x30 && stopHdr == 0x11u, "walk stop n %u at %u hdr %08x", n, stopAt, stopHdr);
        // cap: max 1 records one and counts the other
        n = sdma_drain_ring_walk(r2.data(), 128 + 0x30, ibs, 1, &more, &stopAt, &stopHdr);
        CHECK(n == 1 && more == 1 && stopAt == 128 + 0x30, "walk cap n %u more %u", n, more);
        // a packet running past the end stops the walk there
        n = sdma_drain_ring_walk(r.data(), 12, ibs, 4, &more, &stopAt, &stopHdr);
        CHECK(n == 0 && stopAt == 10 && stopHdr == 0x80000004u, "walk overrun n %u at %u", n, stopAt);
    }
    // 7. The moved guard still refuses what it always refused: every MC aperture source, and a target off-cluster.
    {
        int bad = 0;
        for (uint32_t ip = 0x1614; ip <= 0x161b; ip++) if (gcvm_translatable(ip)) bad++;
        CHECK(bad == 0, "%d MC aperture source(s) accepted", bad);
        CHECK(!gcvm_translatable(0x0100) && gcvm_translatable(kCtx2BaseAppleIp), "cluster guard");
    }
    // 8. 0.0.358: the ACK wait kept, its idempotence, its negative controls, the property, the planted defects.
    ack_checks(0);
    ack_property();
    const int realFail = gFail, realRun = gRun;
    int caught = 0;
    for (int m = 1; m <= 2; m++) {
        const int f0 = gFail;
        const int f = ack_checks(m);
        (void)f0;
        std::printf("  planted defect %d (%s): %s (%d check(s) failed)\n", m,
                    m == 1 ? "the wait kept with no same-IB request" : "kept poll recognised only while the flag is on",
                    f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
        if (f > 0) caught++;
    }
    std::printf("%d/%d PASS; 2 planted defects, %d caught\n", realRun - realFail, realRun, caught);
    return (realFail || caught != 2) ? 1 : 0;
}
