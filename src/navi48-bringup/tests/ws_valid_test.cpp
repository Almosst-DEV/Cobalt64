// ws_valid_test.cpp — the VALID bit of WindowServer's page-table base, set only at COMMIT (ws_valid.h, the drain's keep-rule in
// sdma_drain_xlat.h, the exemption's condition (10) in gfx_neuter.h; notes/M4-WS-VMID-VALID.md), offline.
// The properties under test, restated from the brief rather than from the code:
//
//   W  "bit 0 is written only when COMMIT is armed, WindowServer is BOUND, the frame is judged on the RECORDED VMID, that VMID's
//       CNTL is 0x03fffd73, HI:LO is exactly the bound root, and the SDMA is proven idle - never for another VMID, never on a
//       root mismatch; the write stands only if LO reads back as written and HI is untouched."
//   K  "the drain keeps bit 0 only on a re-program of THAT VMID's base to THE bound root, only while armed; no other VMID's base
//       ever gains bit 0 from it; with the keep-rule not armed the drain's output is aed4ff2's, byte for byte."
//   E  "a committed frame whose VMID's base is not valid is never spared; with COMMIT not armed the exemption is aed4ff2's."
//
// Each property runs against an OPS TABLE, so the same assertions judge the real code and each planted defect. The real table
// must pass every check; each mutant must be CAUGHT by at least one named check. The mutants are the ways the brief names to get
// this wrong: acting without COMMIT; acting on another VMID; a root mismatch accepted; the drain rule applied to every VMID; and
// the exemption not requiring the bit.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/ws_valid_test.cpp -o /tmp/wsvtest && /tmp/wsvtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdint.h>
#include "ws_valid.h"
#include "sdma_drain_xlat.h"
#include "gfx_neuter.h"
namespace head {                         // aed4ff2's drain, frozen (tests/frozen/, only the include guard renamed)
#include "../tests/frozen/sdma_drain_xlat_aed4ff2.h"
}

static int gFail = 0, gRun = 0, gQuiet = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-96s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
        else if (gQuiet == 2) std::printf("        fails: %s (got %#llx)\n", what, (unsigned long long)got);   // which check caught it
    } else if (!gQuiet) {
        std::printf("ok    %-96s %#llx\n", what, (unsigned long long)got);
    }
}

// ---- the ops table -------------------------------------------------------------------------------------------------------
enum { M_REAL = 0, M_NO_COMMIT, M_OTHER_VMID, M_ROOT_MISMATCH, M_EVERY_VMID, M_EXEMPT_NO_BASE, M_COUNT };
static const char *mname(int m)
{
    static const char *const n[M_COUNT] = { "(the real code)", "acting WITHOUT COMMIT (write and keep-rule ignore the arm)",
                                            "acting on ANOTHER VMID (the recorded-VMID match not required)",
                                            "a ROOT MISMATCH accepted (HI:LO not compared with the bound root)",
                                            "the drain rule applied to EVERY VMID (bit 0 on every converted base write)",
                                            "the exemption does NOT require the base bit (condition 10 dropped)" };
    return (m >= 0 && m < M_COUNT) ? n[m] : "?";
}

static uint32_t op_decide(int m, n48_wsv_in in, uint32_t *nl)
{
    if (m == M_NO_COMMIT) in.arm = N48_WSV_ARM_COMMIT;
    if (m == M_OTHER_VMID && in.frame_vmid >= 1u && in.frame_vmid <= 15u) in.ws_vmid = in.frame_vmid;
    if (m == M_ROOT_MISMATCH) in.ws_root = n48_wsv_raw(in.lo, in.hi) & N48_WS_ROOT_MASK;
    return n48_wsv_decide(&in, nl);
}
static uint32_t op_keep(int m, n48_wsv_keep k, uint32_t ctx, uint32_t lo, uint32_t hf, uint32_t hi)
{
    if (m == M_NO_COMMIT && k.vmid) k.armed = 1u;
    if (m == M_OTHER_VMID || m == M_EVERY_VMID) k.vmid = ctx;
    if (m == M_ROOT_MISMATCH && hf) k.root = n48_wsv_raw(lo, hi) & N48_WS_ROOT_MASK;
    return n48_wsv_keep_applies(&k, ctx, lo, hf, hi);
}

// The drain under test. The keep-rule lives inside the header's pass, so the mutants are built the only way they could be written:
// as the same pass with the rule's input (or its scope) wrong.
static const uint32_t kGc = 0x1260u;   // GC BASE_IDX 0 on this board: 0x2830 (FAULT_STATUS,) - 0x15d0
static const uint64_t kArenaBot = 0x3d6c00000ull, kArenaTop = 0x3e6c00000ull;
static int op_drain(int m, uint32_t *buf, uint32_t n, SdmaDrainIbStats *st, uint32_t flags, const n48_wsv_keep *keep)
{
    if (m == M_REAL || m == M_EXEMPT_NO_BASE || !keep) return sdma_drain_translate_ib_keep(buf, n, kGc, kArenaBot, kArenaTop, st, flags, keep);
    n48_wsv_keep k = *keep;
    if (m == M_NO_COMMIT) k.armed = 1u;
    std::vector<uint32_t> orig(buf, buf + n);
    const int rc = sdma_drain_translate_ib_keep(buf, n, kGc, kArenaBot, kArenaTop, st, flags, &k);
    if (rc < 0 || (m != M_EVERY_VMID && m != M_ROOT_MISMATCH && m != M_OTHER_VMID)) return rc;
    // the scope/root mutants: OR bit 0 into converted base-LO writes the real rule would not touch
    for (uint32_t j = 0; j < n; ) {
        const uint32_t h = orig[j], s = sdma_drain_ib_stride(h);
        if (!s || j + s > n) break;
        if ((h & 0xFFu) == 14u && h != 0x0000000eu && orig[j + 1] > kGc) {
            const uint32_t t = orig[j + 1] - kGc + kGcvmShift;
            if (gcvm_translatable(orig[j + 1] - kGc) && t >= kGcvmCtxPtBaseLo0 && t < kGcvmCtxPtBaseLo0 + 32u && !((t - kGcvmCtxPtBaseLo0) & 1u)) {
                const uint32_t ctx = (t - kGcvmCtxPtBaseLo0) >> 1;
                const bool apply = (m == M_EVERY_VMID) ? (k.armed == 1u)
                                 : (m == M_OTHER_VMID) ? (k.armed == 1u && ctx != 0u)
                                 : (k.armed == 1u && ctx == k.vmid);   // ROOT_MISMATCH: that VMID, any root
                if (apply) buf[j + 2] |= 1u;
            }
        }
        j += s;
    }
    return rc;
}

// The exemption under test, and aed4ff2's, frozen verbatim (gfx_neuter.h at aed4ff2: n48_gfxn_exempt_why / n48_gfxn_nop_list).
static uint32_t frozen_exempt_why(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w, uint32_t arm,
                                  const n48_gfxn_exempt_t *ex)
{
    if (!ex || ex->inflight != 1u) return N48_GFXN_EX_NONE;
    if (arm != N48_GFXN_ARM_COMMIT) return N48_GFXN_EX_NOT_ARMED;
    if (ex->seq == 0u || ex->gate_seq != ex->seq) return N48_GFXN_EX_GATE;
    if (ex->used) return N48_GFXN_EX_USED;
    if (ex->nib != 1u) return N48_GFXN_EX_NIB;
    if (ex->same_thread != 1u) return N48_GFXN_EX_THREAD;
    if (!g || !size || !w || w->stop_at != n) return N48_GFXN_EX_WALK;
    if (w->ibs != 1u || w->npos2 + w->npos_other != 1u || w->ibs_vmid2 + w->ibs_other != 1u) return N48_GFXN_EX_FRAME;
    const uint32_t oneVmid = w->npos2 ? 2u : w->vmid_other[0];
    if (ex->vmid == 0u || ex->vmid > 15u || oneVmid != ex->vmid) return N48_GFXN_EX_FRAME;
    const uint32_t p = w->npos2 ? w->pos2[0] : w->pos_other[0];
    if (ex->pos_known != 1u || p != ex->expect_pos) return N48_GFXN_EX_POSITION;
    const uint32_t h = g[p % size], lo = g[(p + 1u) % size], hi = g[(p + 2u) % size], ctl = g[(p + 3u) % size];
    const uint64_t va = ((uint64_t)hi << 32) | (lo & ~3u);
    if (!n48_gfxn_is_ib(h) || ((ctl >> 24) & 0xFu) != ex->vmid || (ctl & 0xFFFFFu) != ex->len || va != ex->va)
        return N48_GFXN_EX_IDENTITY;
    return N48_GFXN_EX_SPARED;
}
static uint32_t frozen_nop_list(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w, uint32_t arm,
                                const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared, uint32_t *why)
{
    uint32_t np = 0u;
    for (uint32_t k = 0; k < w->npos2; k++) pos[np++] = w->pos2[k];
    for (uint32_t k = 0; k < w->npos_other; k++) pos[np++] = w->pos_other[k];
    *spared = 0xFFFFFFFFu;
    *why = frozen_exempt_why(g, size, n, w, arm, ex);
    const uint32_t one = w->npos2 ? w->pos2[0] : (w->npos_other ? w->pos_other[0] : 0xFFFFFFFFu);
    if (*why != N48_GFXN_EX_SPARED || np != 1u || pos[0] != one) {
        if (*why == N48_GFXN_EX_SPARED) *why = N48_GFXN_EX_FRAME;
        return np;
    }
    *spared = pos[0];
    return 0u;
}
static uint32_t op_nop_list(int m, const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w, uint32_t arm,
                            const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared, uint32_t *why)
{
    if (m == M_EXEMPT_NO_BASE) return frozen_nop_list(g, size, n, w, arm, ex, pos, spared, why);   // aed4ff2's: no condition (10)
    if (m == M_NO_COMMIT) arm = N48_GFXN_ARM_COMMIT;
    return n48_gfxn_nop_list(g, size, n, w, arm, ex, pos, spared, why);
}

// ---- W: the direct write's decision ---------------------------------------------------------------------------------------
static const uint64_t kRoot = 0x3d6c0a000ull;              // hp4: WindowServer's root on VMID 3, base raw 0x3:0xd6c0a000
static n48_wsv_in good_in()
{
    n48_wsv_in in {};
    in.arm = N48_WSV_ARM_COMMIT; in.ws_state = N48_WS_BOUND; in.ws_vmid = 3u; in.ws_root = kRoot;
    in.frame_vmid = 3u; in.verdict = N48_WSF_JUDGE; in.cntl = N48_WSV_CNTL; in.lo = 0xd6c0a000u; in.hi = 0x3u; in.sdma_idle = 1u;
    return in;
}
static int write_checks(int m)
{
    const int before = gFail;
    char b[200];
    uint32_t nl = 0;
    n48_wsv_in in = good_in();
    expect_u("W1 hp4's case (VMID 3, 0x3:0xd6c0a000, bound root, armed, idle) -> WRITE", op_decide(m, in, &nl), N48_WSV_WRITE);
    expect_u("W1 ... and the new LO is 0xd6c0a001 (bit 0 only)", nl, 0xd6c0a001u);
    const uint32_t notArmed[] = { 0u, 1u, 3u, 0xFFFFFFFFu };
    for (uint32_t a : notArmed) {
        in = good_in(); in.arm = a; nl = 0x55u;
        std::snprintf(b, sizeof(b), "W2 arm %#x (not COMMIT) -> INERT, and nothing proposed", a);
        const uint32_t d = op_decide(m, in, &nl);
        expect_u(b, d == N48_WSV_INERT && nl == in.lo, 1u);
    }
    struct Case { const char *name; void (*brk)(n48_wsv_in &); uint32_t want; };
    static const Case cs[] = {
        { "W3 frame on VMID 4, WindowServer recorded on 3 (another client)", [](n48_wsv_in &i) { i.frame_vmid = 4u; }, N48_WSV_VMID },
        { "W3 frame on VMID 2 (CONTEXT2's client), recorded 3",            [](n48_wsv_in &i) { i.frame_vmid = 2u; }, N48_WSV_VMID },
        { "W3 no VMID recorded yet for the binding",                        [](n48_wsv_in &i) { i.ws_vmid = 0u; }, N48_WSV_VMID },
        { "W3 frame VMID 0 (our GART)",                                     [](n48_wsv_in &i) { i.frame_vmid = 0u; }, N48_WSV_VMID },
        { "W4 base names another root (0x3:0xd6c1f000, hp4's other base)",  [](n48_wsv_in &i) { i.lo = 0xd6c1f000u; }, N48_WSV_ROOT },
        { "W4 base HI differs (0x4:0xd6c0a000)",                            [](n48_wsv_in &i) { i.hi = 0x4u; }, N48_WSV_ROOT },
        { "W4 bound root 0",                                                [](n48_wsv_in &i) { i.ws_root = 0u; }, N48_WSV_ROOT },
        { "W5 CNTL is ours (0x03fffd07), not Apple's geometry",             [](n48_wsv_in &i) { i.cntl = 0x03fffd07u; }, N48_WSV_CNTL_BAD },
        { "W5 context disabled (CNTL 0)",                                   [](n48_wsv_in &i) { i.cntl = 0u; }, N48_WSV_CNTL_BAD },
        { "W6 WindowServer NONE",                                           [](n48_wsv_in &i) { i.ws_state = N48_WS_NONE; }, N48_WSV_NOT_BOUND },
        { "W6 WindowServer AMBIGUOUS",                                      [](n48_wsv_in &i) { i.ws_state = N48_WS_AMBIGUOUS; }, N48_WSV_NOT_BOUND },
        { "W6 frame dropped as another client's",                           [](n48_wsv_in &i) { i.verdict = N48_WSF_OTHER; }, N48_WSV_NOT_JUDGED },
        { "W6 frame on a moved VMID",                                       [](n48_wsv_in &i) { i.verdict = N48_WSF_MOVED; }, N48_WSV_NOT_JUDGED },
        { "W7 bit 0 already set",                                           [](n48_wsv_in &i) { i.lo |= 1u; }, N48_WSV_ALREADY },
        { "W7 base carries bit 1 as well (not a base Apple wrote)",         [](n48_wsv_in &i) { i.lo |= 2u; }, N48_WSV_SHAPE },
        { "W7 base carries HI bit 28 (outside the root mask)",              [](n48_wsv_in &i) { i.hi |= 0x10000000u; }, N48_WSV_SHAPE },
        { "W8 SDMA not proven idle",                                        [](n48_wsv_in &i) { i.sdma_idle = 0u; }, N48_WSV_BUSY },
    };
    for (const Case &c : cs) {
        in = good_in(); c.brk(in); nl = 0;
        const uint32_t d = op_decide(m, in, &nl);
        std::snprintf(b, sizeof(b), "%s -> %s", c.name, n48_wsv_name(c.want));
        expect_u(b, d, c.want);
        if (c.want != N48_WSV_WRITE) {
            std::snprintf(b, sizeof(b), "%s: no new LO proposed", c.name);
            expect_u(b, nl, in.lo);
        }
    }
    expect_u("W9 read-back: LO as written, HI untouched -> stands", n48_wsv_readback(0xd6c0a001u, 3u, 0xd6c0a001u, 3u), 1u);
    expect_u("W9 read-back: LO did not take -> refused", n48_wsv_readback(0xd6c0a001u, 3u, 0xd6c0a000u, 3u), 0u);
    expect_u("W9 read-back: HI moved under us -> refused", n48_wsv_readback(0xd6c0a001u, 3u, 0xd6c0a001u, 4u), 0u);
    return gFail - before;
}

// ---- K: the drain's keep-rule, on Apple's own VM-program shape ------------------------------------------------------------
// Apple's form: f000000e <GC + gfx10 dword> <value>; gfx10 = gfx12 - 0x28. CONTEXTn PT base LO/HI = 0x168f/0x1690 + 2n.
static void apple_wr(std::vector<uint32_t> &v, uint32_t gfx12Reg, uint32_t val) { v.push_back(0xf000000eu); v.push_back(kGc + gfx12Reg - 0x28u); v.push_back(val); }
static std::vector<uint32_t> vm_program(uint32_t vmid, uint64_t root, bool withHi = true, bool hiFirst = false)
{
    std::vector<uint32_t> v;
    const uint32_t lo = kGcvmCtxPtBaseLo0 + 2u * vmid;
    if (hiFirst && withHi) apple_wr(v, lo + 1u, (uint32_t)(root >> 32));
    apple_wr(v, lo, (uint32_t)root);
    if (!hiFirst && withHi) apple_wr(v, lo + 1u, (uint32_t)(root >> 32));
    apple_wr(v, 0x16af + 2u * vmid, 0x00400000u); apple_wr(v, 0x16b0 + 2u * vmid, 0u);   // START
    apple_wr(v, 0x16cf + 2u * vmid, 0x023fffffu); apple_wr(v, 0x16d0 + 2u * vmid, 0u);   // END
    apple_wr(v, 0x1647u + 6u, 0x00990000u | (1u << vmid));                             // INVALIDATE_ENG6_REQ
    v.push_back(0x30000008u); v.push_back((kGc + 0x1659u + 6u - 0x28u) << 2); v.push_back(0u);    // POLL ENG6_ACK
    v.push_back(1u << vmid); v.push_back(1u << vmid); v.push_back(0x0fff0004u);
    v.push_back(0x00030005u); v.push_back(0x180u); v.push_back(0x84u); v.push_back(1u);        // FENCE
    return v;
}
static uint32_t lo_value_at(const std::vector<uint32_t> &v, uint32_t vmid)
{
    for (uint32_t j = 0; j + 2 < v.size(); j += sdma_drain_ib_stride(v[j]) ? sdma_drain_ib_stride(v[j]) : 1u)
        if ((v[j] & 0xFFu) == 14u && (v[j] == 0x0000000eu ? (v[j + 1] >> 2) - kGc : v[j + 1] - kGc + 0x28u) == kGcvmCtxPtBaseLo0 + 2u * vmid)
            return v[j + 2];
    return 0xFFFFFFFFu;
}
static int keep_checks(int m)
{
    const int before = gFail;
    const n48_wsv_keep armed3 { 1u, 3u, kRoot }, off3 { 0u, 3u, kRoot };
    auto run = [&](std::vector<uint32_t> v, const n48_wsv_keep *k, uint32_t vmid, SdmaDrainIbStats *pst = nullptr) {
        SdmaDrainIbStats st {};
        (void)op_drain(m, v.data(), (uint32_t)v.size(), &st, kDxKeepInvAck, k);
        if (pst) *pst = st;
        return lo_value_at(v, vmid);
    };
    SdmaDrainIbStats st {};
    expect_u("K1 armed, VMID 3 re-programmed to the bound root -> LO keeps bit 0 (0xd6c0a001)", run(vm_program(3, kRoot), &armed3, 3, &st), 0xd6c0a001u);
    expect_u("K1 ... counted as kept", st.wsBasesKept, 1u);
    expect_u("K2 NOT armed (keep.armed 0), same IB -> bare, as aed4ff2 (0xd6c0a000)", run(vm_program(3, kRoot), &off3, 3), 0xd6c0a000u);
    expect_u("K2 no keep at all (nullptr) -> bare", run(vm_program(3, kRoot), nullptr, 3), 0xd6c0a000u);
    expect_u("K3 armed for VMID 3, IB programs VMID 4 with the SAME root -> VMID 4 stays bare", run(vm_program(4, kRoot), &armed3, 4), 0xd6c0a000u);
    expect_u("K3 armed for VMID 3, IB programs VMID 5 (another client) -> bare", run(vm_program(5, 0x3d6c1f000ull), &armed3, 5), 0xd6c1f000u);
    expect_u("K4 armed, VMID 3 re-programmed to ANOTHER root -> bare", run(vm_program(3, 0x3d6c1f000ull), &armed3, 3, &st), 0xd6c1f000u);
    expect_u("K4 ... counted as refused", st.wsKeepRefused, 1u);
    expect_u("K4 armed, VMID 3, same LO but HI 4 (0x4d6c0a000) -> bare", run(vm_program(3, 0x4d6c0a000ull), &armed3, 3), 0xd6c0a000u);
    expect_u("K5 armed, VMID 3, NO HI write in the IB -> bare (refused)", run(vm_program(3, kRoot, false), &armed3, 3), 0xd6c0a000u);
    expect_u("K5 armed, VMID 3, HI written BEFORE LO -> bare (the rule looks forward only)", run(vm_program(3, kRoot, true, true), &armed3, 3), 0xd6c0a000u);
    expect_u("K6 VMID 2 is aed4ff2's CONTEXT2 rule whatever the keep says (bit 0 set)", run(vm_program(2, 0x3d6c00000ull), &off3, 2), 0xd6c00001u);
    // idempotence: a second pass over the kept IB changes nothing
    std::vector<uint32_t> v = vm_program(3, kRoot);
    SdmaDrainIbStats s1 {}, s2 {};
    (void)op_drain(m, v.data(), (uint32_t)v.size(), &s1, kDxKeepInvAck, &armed3);
    const std::vector<uint32_t> once = v;
    const int rc2 = op_drain(m, v.data(), (uint32_t)v.size(), &s2, kDxKeepInvAck, &armed3);
    expect_u("K7 idempotent: a second pass leaves the kept IB unchanged", rc2 == 0 && v == once, 1u);
    // the pure rule
    expect_u("K8 keep_applies: VMID 3, bound root, HI found -> 1", op_keep(m, armed3, 3u, 0xd6c0a000u, 1u, 3u), 1u);
    expect_u("K8 keep_applies: ctx 4 -> 0", op_keep(m, armed3, 4u, 0xd6c0a000u, 1u, 3u), 0u);
    expect_u("K8 keep_applies: not armed -> 0", op_keep(m, off3, 3u, 0xd6c0a000u, 1u, 3u), 0u);
    expect_u("K8 keep_applies: other root -> 0", op_keep(m, armed3, 3u, 0xd6c1f000u, 1u, 3u), 0u);
    expect_u("K8 keep_applies: no HI -> 0", op_keep(m, armed3, 3u, 0xd6c0a000u, 0u, 0u), 0u);
    return gFail - before;
}

// ---- E: the exemption's condition (10), on a committed frame ----------------------------------------------------------------
static n48_gfxn_exempt_t committed_frame(uint32_t *ring, uint32_t size, uint64_t from, uint32_t len, uint64_t va, uint32_t vmid)
{
    uint32_t blk[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = N48_TMPL_NOP;
    uint8_t info[0x200] = { 0 };
    n48_wr32(info, N48_SCB_FLAGS, 0u); n48_wr32(info, N48_SCB_VMID, vmid); n48_wr32(info, N48_SCB_COUNT, 1u);
    n48_wr32(info, N48_SCB_STAMP, 0x21u);
    n48_wr32(info, N48_SCB_ENTRY0, len); n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, (uint32_t)va);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, (uint32_t)(va >> 32));
    n48_gfxsrc_model_commit(info, blk);
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) ring[(from + k) % size] = blk[k];
    n48_gfxn_exempt_t ex {};
    ex.inflight = 1u; ex.seq = 5u; ex.gate_seq = 5u; ex.nib = 1u; ex.same_thread = 1u;
    ex.pos_known = 1u; ex.expect_pos = (uint32_t)((from + N48_TMPL_IB0_DWORD) % size);
    ex.va = va; ex.len = len & 0xFFFFFu; ex.stamp = 0x21u; ex.vmid = vmid; ex.base_valid = 1u;
    return ex;
}
static int exempt_checks(int m)
{
    const int before = gFail;
    enum { kS = 512 };
    static uint32_t ring[kS];
    for (uint32_t k = 0; k < kS; k++) ring[k] = N48_TMPL_NOP;
    const uint64_t from = 448;
    n48_gfxn_exempt_t ex = committed_frame(ring, kS, from, 1040u, 0x400190000ull, 3u);
    n48_gfxn_walk_t w; n48_gfxn_walk(ring, from, 128, kS, &w, nullptr, nullptr);
    uint32_t pos[2u * N48_GFXN_MAX_IBS], sp = 0, why = 0;
    uint32_t np = op_nop_list(m, ring, kS, 128, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
    expect_u("E1 WindowServer's committed frame on VMID 3, base VALID -> SPARED", why == N48_GFXN_EX_SPARED && np == 0u, 1u);
    ex.base_valid = 0u;
    np = op_nop_list(m, ring, kS, 128, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
    expect_u("E2 the same frame, base bit 0 CLEAR -> refused (base-invalid) and its IB NOPed", why == N48_GFXN_EX_BASE && np == 1u && sp == 0xFFFFFFFFu, 1u);
    np = op_nop_list(m, ring, kS, 128, &w, 1u, &ex, pos, &sp, &why);
    expect_u("E3 COMMIT not armed (DECIDE): not-armed, NOPed, whatever the base", why == N48_GFXN_EX_NOT_ARMED && np == 1u, 1u);
    // n48_wsv_base_valid, the kext's input to (10)
    expect_u("E4 base_valid: 0x3:0xd6c0a001, bound root, acked -> 1", n48_wsv_base_valid(0xd6c0a001u, 3u, kRoot, 0u), 1u);
    expect_u("E4 base_valid: bit 0 clear (hp4's 0x3:0xd6c0a000) -> 0", n48_wsv_base_valid(0xd6c0a000u, 3u, kRoot, 0u), 0u);
    expect_u("E4 base_valid: valid but another root -> 0", n48_wsv_base_valid(0xd6c1f001u, 3u, kRoot, 0u), 0u);
    expect_u("E4 base_valid: valid, but its invalidate was not ACKed -> 0", n48_wsv_base_valid(0xd6c0a001u, 3u, kRoot, 1u), 0u);
    expect_u("E4 base_valid: extra bits -> 0", n48_wsv_base_valid(0xd6c0a003u, 3u, kRoot, 0u), 0u);
    return gFail - before;
}

// ---- generated properties -------------------------------------------------------------------------------------------------
static uint64_t gX = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() { gX ^= gX << 13; gX ^= gX >> 7; gX ^= gX << 17; return gX; }
static const uint64_t kRoots[] = { 0x3d6c0a000ull, 0x3d6c1f000ull, 0x3d6c00000ull, 0x4d6c0a000ull, 0x3d6c0b000ull };

// One generated IB: Apple's packet alphabet, with VM-program writes for random VMIDs and roots, occasionally a bad opcode.
static std::vector<uint32_t> gen_ib()
{
    std::vector<uint32_t> v;
    const uint32_t npk = 1u + (uint32_t)(rnd() % 14u);
    for (uint32_t p = 0; p < npk; p++) {
        switch (rnd() % 12u) {
        case 0: v.push_back(0u); break;                                                     // NOP
        case 1: case 2: {                                                                   // a (partial) VM program
            const uint32_t vm = (uint32_t)(rnd() % 16u);
            const uint64_t root = kRoots[rnd() % 5u] | ((rnd() % 5u == 0u) ? (rnd() & 0xFFFu) : 0u);
            const std::vector<uint32_t> pr = vm_program(vm, root, rnd() % 6u != 0u, rnd() % 7u == 0u);
            v.insert(v.end(), pr.begin(), pr.begin() + (long)(rnd() % 3u == 0u ? 6u : pr.size()));
            break; }
        case 3: {                                                                           // a register write, Apple or v7 form
            const uint32_t t = 0x1624u + (uint32_t)(rnd() % 0xd0u);
            if (rnd() % 3u) apple_wr(v, t, (uint32_t)rnd());
            else { v.push_back(0x0000000eu); v.push_back((kGc + t) << 2); v.push_back((uint32_t)rnd()); }
            break; }
        case 4: { const uint32_t vm = (uint32_t)(rnd() % 16u);                               // an already-v7 base write
                  v.push_back(0x0000000eu); v.push_back((kGc + kGcvmCtxPtBaseLo0 + 2u * vm) << 2); v.push_back((uint32_t)kRoots[rnd() % 5u]); break; }
        case 5: v.push_back(0x00030005u); v.push_back((uint32_t)rnd()); v.push_back(0x84u); v.push_back((uint32_t)rnd() % 9u); break;
        case 6: v.push_back(6u); break;
        case 7: {                                                                           // PTEPDE
            v.push_back(0xcu); const uint64_t dst = kArenaBot + ((rnd() % 0x2000u) << 3) + ((rnd() % 9u == 0u) ? 0x20000000ull : 0u);
            v.push_back((uint32_t)dst); v.push_back((uint32_t)(dst >> 32));
            const uint32_t f = (uint32_t)(rnd() % 4u);
            v.push_back(f == 0 ? 1u : f == 1 ? 0x77u : f == 2 ? 1u : 0u); v.push_back(f == 0 ? 0x20000000u : f == 1 ? 0x30000u : f == 2 ? 0x01000000u : 0u);
            const uint64_t val = kArenaBot + ((rnd() % 0x2000u) << 12);
            v.push_back((uint32_t)val); v.push_back((uint32_t)(val >> 32)); v.push_back(rnd() % 2u ? 0x1000u : 0u); v.push_back(0u);
            v.push_back((uint32_t)(rnd() % 4u)); break; }
        case 8: v.push_back(0x20du); v.push_back((uint32_t)rnd()); v.push_back(0x84u); break;
        case 9: v.push_back(0x10u); v.push_back((uint32_t)rnd()); v.push_back((uint32_t)rnd()); v.push_back((uint32_t)rnd()); break;
        case 10: {                                                                          // a register poll, random shape
            const uint32_t r = 0x1659u + (uint32_t)(rnd() % 18u) - 0x28u, msk = (rnd() % 2u) ? (1u << (rnd() % 16u)) : (uint32_t)rnd();
            v.push_back(0x30000008u); v.push_back((kGc + r) << 2); v.push_back(0u); v.push_back(msk); v.push_back(rnd() % 2u ? msk : 0u);
            v.push_back(0x0fff0004u); break; }
        default: if (rnd() % 40u == 0u) v.push_back(0x000000ffu); else v.push_back(0u); break;   // rarely an unknown opcode: refused
        }
    }
    return v;
}

// The keep-rule's oracle, restated from the brief: starting from aed4ff2's output, bit 0 is added to the value of a CONVERTED
// (Apple-form, translatable) REG_WRITE to CONTEXT<k.vmid>'s PT base LO iff a LATER REG_WRITE in the same ORIGINAL IB to that
// context's HI makes HI:LO exactly the bound root (no other bits) - and to nothing else.
static std::vector<uint32_t> keep_oracle(const std::vector<uint32_t> &orig, std::vector<uint32_t> headOut, const n48_wsv_keep &k)
{
    if (k.armed != 1u || k.vmid == 0u || k.vmid > 15u) return headOut;
    const uint32_t n = (uint32_t)orig.size();
    for (uint32_t j = 0; j < n; ) {
        const uint32_t h = orig[j], s = sdma_drain_ib_stride(h);
        if (!s || j + s > n) return headOut;
        if ((h & 0xFFu) == 14u && h != 0x0000000eu && orig[j + 1] > kGc && gcvm_translatable(orig[j + 1] - kGc) &&
            orig[j + 1] - kGc + 0x28u == kGcvmCtxPtBaseLo0 + 2u * k.vmid) {
            bool found = false; uint32_t hi = 0;
            for (uint32_t q = j + s; q < n && !found; ) {
                const uint32_t hq = orig[q], sq = sdma_drain_ib_stride(hq);
                if (!sq || q + sq > n) break;
                if ((hq & 0xFFu) == 14u) {
                    const uint32_t reg = hq == 0x0000000eu ? (orig[q + 1] >> 2) : orig[q + 1] + 0x28u;
                    if (reg == kGc + kGcvmCtxPtBaseLo0 + 2u * k.vmid + 1u) { found = true; hi = orig[q + 2]; }
                }
                q += sq;
            }
            const uint64_t raw = ((uint64_t)hi << 32) | (orig[j + 2] & ~1u);
            if (found && raw == k.root) headOut[j + 2] |= 1u;
        }
        j += s;
    }
    return headOut;
}

static int drain_property(int m, bool print)
{
    const int before = gFail;
    gX = 0x9E3779B97F4A7C15ull;
    uint64_t ibs = 0, refused = 0, offDiff = 0, offStatDiff = 0, armedIbs = 0, armedOracleDiff = 0, otherVmidBit = 0, kept = 0,
             keptIbs = 0, baseWrites = 0;
    for (uint32_t it = 0; it < 200000u; it++) {
        const std::vector<uint32_t> orig = gen_ib();
        const uint32_t flags = (rnd() % 4u) ? kDxKeepInvAck : 0u;
        n48_wsv_keep k {};
        k.vmid = (uint32_t)(rnd() % 17u); k.root = kRoots[rnd() % 5u];
        // (a) NOT ARMED (armed 0 with any vmid/root, and no keep at all): aed4ff2's output and counters, byte for byte
        std::vector<uint32_t> h = orig, a = orig, a0 = orig;
        head::SdmaDrainIbStats hs; std::memset(&hs, 0, sizeof(hs));
        SdmaDrainIbStats as, a0s; std::memset(&as, 0, sizeof(as)); std::memset(&a0s, 0, sizeof(a0s));
        const int hrc = head::sdma_drain_translate_ib_ex(h.data(), (uint32_t)h.size(), kGc, kArenaBot, kArenaTop, &hs, flags);
        const int arc = op_drain(m, a.data(), (uint32_t)a.size(), &as, flags, &k);
        const int a0rc = op_drain(m, a0.data(), (uint32_t)a0.size(), &a0s, flags, nullptr);
        ibs++; if (hrc < 0) refused++;
        if (hrc != arc || h != a || hrc != a0rc || h != a0) offDiff++;
        if (std::memcmp(&hs, &as, sizeof(hs)) != 0 || std::memcmp(&hs, &a0s, sizeof(hs)) != 0) offStatDiff++;
        if (as.ptBaseVmidMask) baseWrites++;
        // (b) ARMED: exactly the oracle; and no dword that differs from aed4ff2 is anything but CONTEXT<k.vmid>'s LO gaining bit 0
        n48_wsv_keep ka = k; ka.armed = 1u;
        std::vector<uint32_t> b = orig;
        SdmaDrainIbStats bs; std::memset(&bs, 0, sizeof(bs));
        const int brc = op_drain(m, b.data(), (uint32_t)b.size(), &bs, flags, &ka);
        armedIbs++;
        const std::vector<uint32_t> want = hrc < 0 ? orig : keep_oracle(orig, h, ka);
        if (brc != hrc || b != want) armedOracleDiff++;   // a kept bit only ever rides on a write aed4ff2 already converted
        bool any = false;
        for (uint32_t j = 0; j < b.size(); ) {
            const uint32_t s = sdma_drain_ib_stride(orig[j]);
            if (!s || j + s > b.size()) break;
            for (uint32_t q = j; q < j + s; q++) {
                if (b[q] == h[q]) continue;
                const bool isKeepLo = q == j + 2u && (orig[j] & 0xFFu) == 14u && orig[j] != 0x0000000eu &&
                                      orig[j + 1] - kGc + 0x28u == kGcvmCtxPtBaseLo0 + 2u * ka.vmid && b[q] == (h[q] | 1u);
                if (!isKeepLo) otherVmidBit++; else any = true;
            }
            j += s;
        }
        if (any) { keptIbs++; kept += bs.wsBasesKept; }
    }
    if (print)
        std::printf("drain property: %llu generated IBs (%llu refused by pass 1, %llu with a PT-base write); NOT ARMED: %llu differ from "
                    "aed4ff2 in bytes or rc, %llu in counters; ARMED: %llu IBs, %llu differ from the oracle, %llu dword(s) changed "
                    "outside WindowServer's recorded VMID's base LO, %llu IB(s) kept bit 0 (%llu base(s))\n",
                    (unsigned long long)ibs, (unsigned long long)refused, (unsigned long long)baseWrites,
                    (unsigned long long)offDiff, (unsigned long long)offStatDiff, (unsigned long long)armedIbs,
                    (unsigned long long)armedOracleDiff, (unsigned long long)otherVmidBit, (unsigned long long)keptIbs,
                    (unsigned long long)kept);
    expect_u("K-P1 not armed: drain output byte-identical to aed4ff2 (200000 IBs)", offDiff, 0u);
    expect_u("K-P1 not armed: every aed4ff2 counter identical", offStatDiff, 0u);
    expect_u("K-P2 armed: output equals the oracle restated from the brief", armedOracleDiff, 0u);
    expect_u("K-P3 armed: no dword changed except the recorded VMID's base LO gaining bit 0", otherVmidBit, 0u);
    expect_u("K-P4 armed: the rule did fire (the property is not vacuous)", keptIbs > 1000u, 1u);
    return gFail - before;
}

static int exempt_property(int m, bool print)
{
    const int before = gFail;
    gX = 0xD1B54A32D192ED03ull;
    enum { kS = 512 };
    static uint32_t g[kS];
    uint64_t frames = 0, notArmedDiff = 0, armedValidDiff = 0, armedInvalidSpared = 0, armedInvalidOtherDiff = 0, spared = 0,
             baseRefused = 0;
    const uint32_t kinds[] = { 0xC0023F00u, 0xC0064900u, 0xC0017900u, 0xFFFF1000u, 0x80000000u, 0xC0021000u, 0x12345678u };
    for (uint32_t it = 0; it < 150000u; it++) {
        const uint64_t from = rnd() % kS;
        const uint32_t shape = (uint32_t)(rnd() % 4u);
        n48_gfxn_exempt_t ex {};
        if (shape == 0u) {
            for (uint32_t k = 0; k < kS; k++) { const uint64_t r = rnd(); g[k] = (r & 0x100u) ? kinds[r % 7u] : (uint32_t)(r >> 16); }
            ex.inflight = (uint32_t)(rnd() % 3u); ex.seq = (uint32_t)(rnd() % 4u); ex.gate_seq = (uint32_t)(rnd() % 4u);
            ex.nib = (uint32_t)(rnd() % 3u); ex.same_thread = (uint32_t)(rnd() % 2u); ex.pos_known = (uint32_t)(rnd() % 2u);
            ex.expect_pos = (uint32_t)((from + 0x2du) % kS); ex.vmid = (uint32_t)(rnd() % 17u);
        } else {
            for (uint32_t k = 0; k < kS; k++) g[k] = N48_TMPL_NOP;
            const uint32_t len = 1u + (uint32_t)(rnd() % 0xFFFFFu);
            const uint64_t va = 0x400000000ull + ((rnd() % 0x100000u) << 12);
            ex = committed_frame(g, kS, from, len, va, 1u + (uint32_t)(rnd() % 15u));
            if (shape == 2u) { const uint32_t at = (uint32_t)((from + 0x46u) % kS); g[at] = 0xC0023F00u; g[(at + 3u) % kS] = (5u << 24) | 0x40u; }
            if (rnd() % 3u == 0u) {
                switch (rnd() % 8u) {
                case 0: ex.inflight = 0u; break; case 1: ex.used = 1u; break; case 2: ex.gate_seq ^= 1u; break;
                case 3: ex.nib = 2u; break; case 4: ex.same_thread = 0u; break; case 5: ex.expect_pos ^= 4u; break;
                case 6: ex.va ^= 0x1000u; break; default: ex.vmid = (uint32_t)(rnd() % 17u); break;
                }
            }
        }
        ex.base_valid = (uint32_t)(rnd() % 3u);   // 0 invalid, 1 valid, 2 garbage (anything but 1 is not valid)
        n48_gfxn_walk_t w; n48_gfxn_walk(g, from, 128, kS, &w, nullptr, nullptr);
        uint32_t fp[2u * N48_GFXN_MAX_IBS], np_[2u * N48_GFXN_MAX_IBS], fsp = 0, nsp = 0, fwhy = 0, nwhy = 0;
        frames++;
        // not armed: aed4ff2's answer exactly, whatever base_valid says
        const uint32_t a0 = (uint32_t)(rnd() % 6u), arm = a0 >= 2u ? a0 + 1u : a0;
        uint32_t fn = frozen_nop_list(g, kS, 128, &w, arm, &ex, fp, &fsp, &fwhy);
        uint32_t nn = op_nop_list(m, g, kS, 128, &w, arm, &ex, np_, &nsp, &nwhy);
        bool same = fn == nn && fsp == nsp && fwhy == nwhy;
        for (uint32_t j = 0; same && j < fn; j++) same = fp[j] == np_[j];
        if (!same) notArmedDiff++;
        // armed
        fn = frozen_nop_list(g, kS, 128, &w, N48_GFXN_ARM_COMMIT, &ex, fp, &fsp, &fwhy);
        nn = op_nop_list(m, g, kS, 128, &w, N48_GFXN_ARM_COMMIT, &ex, np_, &nsp, &nwhy);
        same = fn == nn && fsp == nsp && fwhy == nwhy;
        for (uint32_t j = 0; same && j < fn; j++) same = fp[j] == np_[j];
        if (nwhy == N48_GFXN_EX_SPARED) spared++;
        if (nwhy == N48_GFXN_EX_BASE) baseRefused++;
        if (ex.base_valid == 1u) { if (!same) armedValidDiff++; }
        else {
            if (nwhy == N48_GFXN_EX_SPARED) armedInvalidSpared++;
            // the only allowed difference: aed4ff2 SPARED -> now BASE, with the full NOP list (the one IB NOPed)
            const bool okDiff = fwhy == N48_GFXN_EX_SPARED && nwhy == N48_GFXN_EX_BASE && nsp == 0xFFFFFFFFu && nn == 1u;
            if (!same && !okDiff) armedInvalidOtherDiff++;
        }
    }
    if (print)
        std::printf("exempt property: %llu generated frames; NOT ARMED: %llu differ from aed4ff2; ARMED: base valid - %llu differ "
                    "from aed4ff2; base not valid - %llu SPARED, %llu differ other than SPARED->base-invalid; spared %llu, "
                    "base-invalid refusals %llu\n", (unsigned long long)frames, (unsigned long long)notArmedDiff,
                    (unsigned long long)armedValidDiff, (unsigned long long)armedInvalidSpared,
                    (unsigned long long)armedInvalidOtherDiff, (unsigned long long)spared, (unsigned long long)baseRefused);
    expect_u("E-P1 not armed: the exemption's list/why/spared identical to aed4ff2 (150000 frames)", notArmedDiff, 0u);
    expect_u("E-P2 armed, base valid: identical to aed4ff2", armedValidDiff, 0u);
    expect_u("E-P3 armed, base NOT valid: never spared", armedInvalidSpared, 0u);
    expect_u("E-P4 armed, base NOT valid: the only change is SPARED -> base-invalid (IB NOPed)", armedInvalidOtherDiff, 0u);
    expect_u("E-P5 the property is not vacuous (frames spared and refused on the base)", spared > 1000u && baseRefused > 1000u, 1u);
    return gFail - before;
}

// W as a generated property: over random inputs the decision writes only in the one shape the brief allows.
static int write_property(int m, bool print)
{
    const int before = gFail;
    gX = 0x2545F4914F6CDD1Dull;
    uint64_t n = 0, writes = 0, bad = 0;
    for (uint32_t it = 0; it < 300000u; it++) {
        n48_wsv_in in {};
        in.arm = (uint32_t)(rnd() % 4u); in.ws_state = (uint32_t)(rnd() % 3u); in.ws_vmid = (uint32_t)(rnd() % 6u);
        in.ws_root = kRoots[rnd() % 5u];
        in.frame_vmid = (rnd() % 2u) ? in.ws_vmid : (uint32_t)(rnd() % 6u);   // half the frames on the recorded VMID in.verdict = (uint32_t)(rnd() % 3u ? 0u : rnd() % 7u);
        in.cntl = rnd() % 4u ? N48_WSV_CNTL : 0x03fffd07u;
        const uint64_t raw = (rnd() % 3u ? in.ws_root : kRoots[rnd() % 5u]) | (rnd() % 2u) | ((rnd() % 9u == 0u) ? 0x10000000000000ull : 0u);
        in.lo = (uint32_t)raw; in.hi = (uint32_t)(raw >> 32); in.sdma_idle = (uint32_t)(rnd() % 4u != 0u);
        uint32_t nl = 0;
        const uint32_t d = op_decide(m, in, &nl);
        n++;
        const bool allowed = in.arm == 2u && in.ws_state == N48_WS_BOUND && in.verdict == N48_WSF_JUDGE && in.ws_vmid != 0u &&
                             in.frame_vmid == in.ws_vmid && in.cntl == N48_WSV_CNTL &&
                             n48_wsv_raw(in.lo, in.hi) == in.ws_root && in.sdma_idle == 1u;   // exactly the bare root
        if (d == N48_WSV_WRITE) { writes++; if (!allowed || nl != (in.lo | 1u)) bad++; }
        else if (allowed) bad++;
        if (d != N48_WSV_WRITE && nl != in.lo) bad++;
    }
    if (print)
        std::printf("write property: %llu generated inputs, %llu WRITE decisions, %llu outside the one allowed shape or missed\n",
                    (unsigned long long)n, (unsigned long long)writes, (unsigned long long)bad);
    expect_u("W-P1 the decision writes exactly when every condition holds, and only bit 0", bad, 0u);
    expect_u("W-P2 not vacuous", writes > 1000u, 1u);
    return gFail - before;
}

static int all_checks(int m, bool print)
{
    int f = 0;
    f += write_checks(m);
    f += keep_checks(m);
    f += exempt_checks(m);
    f += write_property(m, print);
    f += drain_property(m, print);
    f += exempt_property(m, print);
    return f;
}

int main()
{
    const int f0 = all_checks(M_REAL, true);
    const int realChecks = gRun;
    std::printf("ws_valid: %d check(s) on the real code, %d failed\n", realChecks, f0);
    int caught = 0;
    gQuiet = 2;
    for (int m = 1; m < M_COUNT; m++) {
        const int f = all_checks(m, false);
        std::printf("  planted %-84s %s (%d check(s) fail)\n", mname(m), f ? "CAUGHT" : "NOT CAUGHT", f);
        if (f) caught++;
    }
    gFail = f0;
    std::printf("ws_valid: %d check(s) on the real code, %d failed; %d of %d planted defects caught.%s\n", realChecks, f0, caught,
                M_COUNT - 1, (f0 == 0 && caught == M_COUNT - 1) ? " N48-WSVALID-TEST-PASS" : " FAIL");
    return (f0 == 0 && caught == M_COUNT - 1) ? 0 : 1;
}
