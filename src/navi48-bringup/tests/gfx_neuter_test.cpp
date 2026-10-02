// gfx_neuter_test.cpp — the GFX neuter's header rewrite over Apple's frame shape (0.0.278).
//     clang++ -std=c++17 -Wall -Wextra -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_neuter_test.cpp -o /tmp/gfxntest && /tmp/gfxntest
// The frame is wscap2's F1 shape: COND_EXEC over 0x79 dwords, COPY_DATA, WRITE_DATA, ACQUIRE_MEM,
// SET_UCONFIG_REG, WAIT_REG_MEM, padding, INDIRECT_BUFFER (VMID 2), RELEASE_MEM x2, SWITCH_BUFFER, padding to 128 dwords,
// placed across the ring's wrap point to exercise the modulo.
#include <cstdio>
#include <cstdint>
#include "gfx_neuter.h"
#include "gfx_src_decide.h"   /* 0.0.362: n48_sd_action, for the hook-outcome property */

static int gFail = 0, gRun = 0, gQuietN = 0, gMutFails = 0, gMutRuns = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (gQuietN) gMutRuns++;
    if (got != want) {
        gFail++;
        if (gQuietN) gMutFails++;
        else std::printf("FAIL  %-70s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    }
    else if (!gQuietN) std::printf("ok    %-70s %#llx\n", what, (unsigned long long)got);
}

struct Walk { uint32_t packets, ibs, stop, end, nops; uint32_t ibPos[4]; };

// The hook's walk: 0xFFFF1000 and type-2 are one dword, type 3 is count+2, anything else stops.
static Walk walk(const uint32_t *g, uint64_t from, uint64_t n, uint32_t size)
{
    Walk w{};
    uint64_t i = 0;
    for (; i < n; ) {
        const uint32_t h = g[(from + i) % size];
        if (h == 0xFFFF1000u) { i++; continue; }
        if ((h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) { w.stop = 1; break; }
        w.packets++;
        const uint32_t cnt = (h >> 16) & 0x3FFFu, op = (h >> 8) & 0xFFu;
        if (op == 0x10u) w.nops++;
        if (op == 0x3Fu && cnt == 2u && i + 3 < n && ((g[(from + i + 3) % size] >> 24) & 0xFu) == 2u) {
            if (w.ibs < 4) w.ibPos[w.ibs] = (uint32_t)((from + i) % size);
            w.ibs++;
        }
        i += (uint64_t)cnt + 2u;
    }
    w.end = (uint32_t)i;
    return w;
}


// ---------------------------------------------------------------------------------------------------------------------
// 0.0.358 — the VMID escape. FROZEN COPIES of the 0.0.357 logic, verbatim in effect, so the property "drops
// more, never passes more" is tested against what actually ran, not against a paraphrase.
// ---------------------------------------------------------------------------------------------------------------------
// hook_gfxWriteTail's loop as of 0.0.357 (AppleHardwareHook.cpp, `if (((ctl >> 24) & 0xFu) == 2u)`).
struct OldWalk { uint32_t pk, found, npos; uint32_t pos[16]; uint64_t ibVa; uint32_t ibLen; uint64_t stopAt; uint32_t stopHdr; };
static OldWalk old_walk(const uint32_t *g, uint64_t from, uint64_t n, uint32_t size)
{
    OldWalk o{}; o.stopAt = n;
    for (uint64_t i = 0; i < n; ) {
        const uint32_t h = g[(from + i) % size];
        if (h == 0xFFFF1000u) { i++; continue; }
        const uint32_t type = h >> 30;
        if (type == 2) { i++; continue; }
        if (type != 3) { o.stopAt = i; o.stopHdr = h; break; }
        o.pk++;
        const uint32_t cnt = (h >> 16) & 0x3FFFu, op = (h >> 8) & 0xFFu;
        if (op == 0x3F && cnt == 2 && i + 3 < n) {
            const uint32_t ctl = g[(from + i + 3) % size];
            if (((ctl >> 24) & 0xFu) == 2u) {
                if (o.npos < 16u) o.pos[o.npos++] = (uint32_t)((from + i) % size);
                o.found++;
                if (!o.ibVa) { o.ibVa = ((uint64_t)g[(from + i + 2) % size] << 32) | (g[(from + i + 1) % size] & ~3u); o.ibLen = ctl & 0xFFFFFu; }
            }
        }
        i += (uint64_t)cnt + 2u;
    }
    return o;
}
// ---------------------------------------------------------------------------------------------------------------------
// 0.0.362 — FROZEN COPIES of the 0.0.358-0.0.361 source check, route and ring exemption, verbatim in effect from
// gfx_neuter.h as 0.0.361 shipped them: the "VMID 2 is WindowServer" identity hp3 refuted. The owner route and the
// owner-keyed exemption are tested against them: where WindowServer really was on VMID 2 they must answer identically, and
// with COMMIT not armed the CP must receive exactly what 0.0.361 sent it, whoever owns which VMID.
// ---------------------------------------------------------------------------------------------------------------------
enum { kOld361NotVmid2 = 2, kOld361OtherVmid = 2 };
static uint32_t frozen0361_check(const uint8_t *info, const uint32_t *tmpl, uint32_t *nOut)
{
    *nOut = 0u;
    if (!info || !tmpl) return N48_SRC_ARG;
    if (n48_rd32(info, N48_SCB_FLAGS) & 0x12u) return N48_SRC_NOT_LIST;
    if ((n48_rd32(info, N48_SCB_VMID) & 0xFu) != 2u) return kOld361NotVmid2;
    const uint32_t n = n48_rd32(info, N48_SCB_COUNT);
    if (n == 0u || n > N48_SCB_MAX_IBS) return N48_SRC_COUNT;
    for (uint32_t k = 0; k < 4u * N48_SCB_MAX_IBS; k++)
        if (tmpl[N48_TMPL_IB0_DWORD + k] != N48_TMPL_NOP) return N48_SRC_TEMPLATE;
    *nOut = n;
    return N48_SRC_OK;
}
static uint32_t frozen0361_route(const uint8_t *info, const uint32_t *tmpl, uint32_t *nOut, uint32_t *why)
{
    uint32_t n = 0u;
    *nOut = 0u;
    *why = frozen0361_check(info, tmpl, &n);
    if (*why == N48_SRC_OK) { *nOut = n; return N48_ROUTE_DECIDE; }
    if (*why != kOld361NotVmid2) return N48_ROUTE_PASS;
    const uint32_t c = n48_rd32(info, N48_SCB_COUNT);
    if (c == 0u || c > N48_SCB_MAX_IBS) return N48_ROUTE_PASS;
    for (uint32_t k = 0; k < 4u * N48_SCB_MAX_IBS; k++)
        if (tmpl[N48_TMPL_IB0_DWORD + k] != N48_TMPL_NOP) return N48_ROUTE_PASS;
    *nOut = c;
    return kOld361OtherVmid;
}
static uint32_t frozen0361_exempt_why(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                      uint32_t arm, const n48_gfxn_exempt_t *ex)
{
    if (!ex || ex->inflight != 1u) return N48_GFXN_EX_NONE;
    if (arm != N48_GFXN_ARM_COMMIT) return N48_GFXN_EX_NOT_ARMED;
    if (ex->seq == 0u || ex->gate_seq != ex->seq) return N48_GFXN_EX_GATE;
    if (ex->used) return N48_GFXN_EX_USED;
    if (ex->nib != 1u) return N48_GFXN_EX_NIB;
    if (ex->same_thread != 1u) return N48_GFXN_EX_THREAD;
    if (!g || !size || !w || w->stop_at != n) return N48_GFXN_EX_WALK;
    if (w->ibs != 1u || w->ibs_vmid2 != 1u || w->npos2 != 1u || w->npos_other != 0u || w->ibs_other != 0u)
        return N48_GFXN_EX_FRAME;
    const uint32_t p = w->pos2[0];
    if (ex->pos_known != 1u || p != ex->expect_pos) return N48_GFXN_EX_POSITION;
    const uint32_t h = g[p % size], lo = g[(p + 1u) % size], hi = g[(p + 2u) % size], ctl = g[(p + 3u) % size];
    const uint64_t va = ((uint64_t)hi << 32) | (lo & ~3u);
    if (!n48_gfxn_is_ib(h) || ((ctl >> 24) & 0xFu) != 2u || (ctl & 0xFFFFFu) != ex->len || va != ex->va)
        return N48_GFXN_EX_IDENTITY;
    return N48_GFXN_EX_SPARED;
}
static uint32_t frozen0361_nop_list(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                    uint32_t arm, const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared, uint32_t *why)
{
    uint32_t np = 0u;
    for (uint32_t k = 0; k < w->npos2; k++) pos[np++] = w->pos2[k];
    for (uint32_t k = 0; k < w->npos_other; k++) pos[np++] = w->pos_other[k];
    *spared = 0xFFFFFFFFu;
    *why = frozen0361_exempt_why(g, size, n, w, arm, ex);
    if (*why != N48_GFXN_EX_SPARED || np != 1u || pos[0] != w->pos2[0]) {
        if (*why == N48_GFXN_EX_SPARED) *why = N48_GFXN_EX_FRAME;
        return np;
    }
    *spared = pos[0];
    return 0u;
}
// The source routing the kext runs from 0.0.362, with WindowServer's identity supplied as the ORACLE of a world: `wsVmid` is
// the VMID whose hardware page-table base is WindowServer's root (0 = WindowServer unidentified: nothing is WindowServer's).
static uint32_t owner_route(const uint8_t *info, const uint32_t *tmpl, uint32_t wsVmid, uint32_t *n, uint32_t *why)
{
    const uint32_t vm = info ? (n48_rd32(info, N48_SCB_VMID) & 0xFu) : 0u;
    return n48_gfxsrc_route_owner(info, tmpl, (wsVmid && vm == wsVmid) ? 1u : 0u, n, why);
}

// hook_gfxCommitIB's routing as of 0.0.357: the decision only for N48_SRC_OK, everything else to Apple.
static uint32_t old_route(const uint8_t *info, const uint32_t *tmpl)
{
    uint32_t n = 0;
    return frozen0361_check(info, tmpl, &n) == N48_SRC_OK ? (uint32_t)N48_ROUTE_DECIDE : (uint32_t)N48_ROUTE_PASS;
}

// The NOP set the kext writes: mutant 1 = the old walk's.
static uint32_t nop_set(int m, const uint32_t *g, uint64_t from, uint64_t n, uint32_t size, uint32_t *out)
{
    if (m == 1) { const OldWalk o = old_walk(g, from, n, size); for (uint32_t k = 0; k < o.npos; k++) out[k] = o.pos[k]; return o.npos; }
    n48_gfxn_walk_t w;
    n48_gfxn_walk(g, from, n, size, &w, nullptr, nullptr);
    uint32_t k = 0;
    for (uint32_t j = 0; j < w.npos2; j++) out[k++] = w.pos2[j];
    for (uint32_t j = 0; j < w.npos_other; j++) out[k++] = w.pos_other[j];
    return k;
}
static uint32_t route(int m, const uint8_t *info, const uint32_t *tmpl, uint32_t *n)
{
    if (m == 2) { *n = 0; return old_route(info, tmpl); }
    uint32_t why = 0;
    return owner_route(info, tmpl, 2u, n, &why);   // 0.0.362: hp1's world - WindowServer really was on VMID 2
}

static const uint32_t kF1[] = {
    0xC0032200u, 0x0000001cu, 0x00000084u, 0x00000000u, 0x00000079u,
    0xC0044000u, 0x00010209u, 0x00000000u, 0x00000000u, 0x000a0010u, 0x00000084u,
    0xC0033700u, 0x00000000u, 0x0000a2a4u, 0x00000000u, 0x00000016u,
    0xC0065800u, 0x86287fc3u, 0xffffffffu, 0x000000ffu, 0u, 0u, 0x00000010u, 0u,
    0xC0017900u, 0x00000074u, 0x00000000u,
    0xC0053C00u, 0x00000003u, 0x0000c07fu, 0u, 0u, 0x80000000u, 0x0000000au,
    0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
    0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
    0xC0023F00u, 0x00190000u, 0x00000004u, 0x03000e90u,                                     // SecurityAgent's: VMID 3
    0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
    0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
    0xC0064900u, 0x00000528u, 0x60000000u, 0x000a0018u, 0x00000084u, 0u, 0u, 0u,
    0xC0064900u, 0x0030c514u, 0x22000000u, 0u, 0x00000084u, 0x00000002u, 0u, 0u,
};

static void place(uint32_t *ring, uint32_t size, uint64_t from, uint32_t ctl)
{
    for (uint32_t k = 0; k < 128; k++) ring[(from + k) % size] = 0xFFFF1000u;
    for (uint32_t k = 0; k < sizeof(kF1) / 4; k++) ring[(from + k) % size] = kF1[k];
    ring[(from + 0x30) % size] = ctl;
    ring[(from + 0x7e) % size] = 0xC0008B00u; ring[(from + 0x7f) % size] = 0u;
}

static int vmid_checks(int m)
{
    const int before = gFail;
    enum { kSize = 256 };
    const uint64_t from = 200;
    // 1. SecurityAgent's frame (wsgc1 F12: `INDIRECT_BUFFER(0x3f) cnt 2: 00190000 00000004 03000e90`) - the IB is in the NOP set.
    const uint32_t vms[] = { 3u, 0u, 15u };
    for (uint32_t vm : vms) {
        uint32_t ring[kSize] = { 0 };
        place(ring, kSize, from, (vm << 24) | 0x0e90u);
        uint32_t pos[32]; const uint32_t np = nop_set(m, ring, from, 128, kSize, pos);
        char b[96]; std::snprintf(b, sizeof b, "VMID-%u IB is in the ring NOP set", vm);
        expect_u(b, np == 1 && pos[0] == (from + 0x2d) % kSize, 1);
        for (uint32_t k = 0; k < np; k++) ring[pos[k]] = n48_gfxn_nop_for(ring[pos[k]]);
        n48_gfxn_walk_t w; n48_gfxn_walk(ring, from, 128, kSize, &w, nullptr, nullptr);
        std::snprintf(b, sizeof b, "VMID-%u frame after the NOP: no IB left, walk reaches the end", vm);
        expect_u(b, w.ibs == 0 && w.stop_at == 128 && w.packets == 10, 1);
    }
    // 2. The VMID-2 frame is still NOPed, at the same place, and reported as before.
    {
        uint32_t ring[kSize] = { 0 };
        place(ring, kSize, from, 0x02000e90u);
        uint32_t pos[32]; const uint32_t np = nop_set(m, ring, from, 128, kSize, pos);
        expect_u("VMID-2 IB still in the NOP set", np == 1 && pos[0] == (from + 0x2d) % kSize, 1);
        n48_gfxn_walk_t w; n48_gfxn_walk(ring, from, 128, kSize, &w, nullptr, nullptr);
        expect_u("VMID-2 frame: first IB VA/len as the old walk reported", w.ib2_va == 0x400190000ull && w.ib2_len == 0xe90u, 1);
    }
    // 3. THE SOURCE: SecurityAgent's submission (flags 0x281, vmid 3, count 1, stamp 0xd) is neutered, never decided.
    {
        uint32_t tmpl[N48_TMPL_DWORDS];
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
        uint8_t info[0x200] = { 0 };
        n48_wr32(info, N48_SCB_FLAGS, 0x281u); n48_wr32(info, N48_SCB_VMID, 0x3u); n48_wr32(info, N48_SCB_COUNT, 1u);
        n48_wr32(info, N48_SCB_STAMP, 0xdu);
        n48_wr32(info, N48_SCB_ENTRY0, 3728u); n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, 0x00190000u);
        n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, 0x4u);
        uint32_t n = 0;
        expect_u("source: SecurityAgent's VMID-3 submission -> NEUTER_OTHER", route(m, info, tmpl, &n), N48_ROUTE_NOT_WS);
        expect_u("source:   with its IB count", n, 1u);
        uint32_t saved[N48_SCB_MAX_IBS] = { 0 }, blk[N48_TMPL_DWORDS];
        n48_gfxsrc_zero(info, n, saved);
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = tmpl[k];
        n48_gfxsrc_model_commit(info, blk);
        uint32_t ibs = 0; for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) if (blk[k] == 0xC0023F00u) ibs++;
        expect_u("source:   zeroed, Apple's model emits no IB packet", ibs, 0u);
        n48_gfxsrc_restore(info, n, saved);
        n48_wr32(info, N48_SCB_VMID, 0x0u);
        expect_u("source: a VMID-0 submission of that shape -> NEUTER_OTHER", route(m, info, tmpl, &n), N48_ROUTE_NOT_WS);
        n48_wr32(info, N48_SCB_VMID, 0x2u);
        expect_u("source: the VMID-2 submission still goes to the decision", route(m, info, tmpl, &n), N48_ROUTE_DECIDE);
        n48_wr32(info, N48_SCB_VMID, 0x3u); n48_wr32(info, N48_SCB_FLAGS, 0x10u);
        expect_u("source: VMID-3 single-IB frame -> PASS (the ring is the backstop)", route(m, info, tmpl, &n), N48_ROUTE_PASS);
        n48_wr32(info, N48_SCB_FLAGS, 0x281u); n48_wr32(info, N48_SCB_COUNT, 5u);
        expect_u("source: VMID-3, 5 IBs -> PASS", route(m, info, tmpl, &n), N48_ROUTE_PASS);
        n48_wr32(info, N48_SCB_COUNT, 1u); tmpl[0x31] = 0xC0064900u;
        expect_u("source: VMID-3, template slot not padding -> PASS", route(m, info, tmpl, &n), N48_ROUTE_PASS);
    }
    return gFail - before;
}

// The property, against the frozen copies: over generated rings and submissions, the new walk's VMID-2 outputs are the old
// walk's exactly and its NOP set contains the old one; the new route decides exactly what the old one decided and differs
// only by neutering what the old one passed.
static void vmid_property()
{
    uint64_t x = 0x243F6A8885A308D3ull, frames = 0, moreNops = 0, bad = 0;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    const uint32_t kinds[] = { 0xC0023F00u, 0xC0033F00u, 0xC0064900u, 0xC0017900u, 0xFFFF1000u, 0x80000000u, 0xC0021000u, 0x12345678u };
    for (uint32_t it = 0; it < 200000u; it++) {
        enum { kS = 512 };
        static uint32_t g[kS];
        for (uint32_t k = 0; k < kS; k++) {
            const uint64_t r = rnd();
            const uint32_t h = kinds[r % 8u];
            g[k] = (r & 0x100u) ? h : (uint32_t)(r >> 16);
            if (h == 0xC0023F00u && k + 3 < kS) { g[k + 3] = ((uint32_t)((r >> 20) & 0xFu) << 24) | (uint32_t)((r >> 32) & 0xFFFFFu); }
        }
        // Make many headers land on packet boundaries by planting IB packets at random offsets in a padding field.
        if (rnd() & 1u) for (uint32_t k = 0; k < kS; k++) g[k] = 0xFFFF1000u;
        const uint32_t nib = (uint32_t)(rnd() % 24u);
        for (uint32_t j = 0; j < nib; j++) {
            const uint32_t at = (uint32_t)(rnd() % (kS - 4u));
            g[at] = 0xC0023F00u; g[at + 1] = (uint32_t)rnd(); g[at + 2] = 0x4u;
            g[at + 3] = ((uint32_t)(rnd() % 16u) << 24) | 0x0e90u;
        }
        const uint64_t from = rnd() % kS, n = 1u + rnd() % kS;
        const OldWalk o = old_walk(g, from, n, kS);
        n48_gfxn_walk_t w; n48_gfxn_walk(g, from, n, kS, &w, nullptr, nullptr);
        frames++;
        bool same = o.pk == w.packets && o.found == w.ibs_vmid2 && o.npos == w.npos2 && o.ibVa == w.ib2_va && o.ibLen == w.ib2_len &&
                    o.stopAt == w.stop_at && o.stopHdr == w.stop_hdr;
        for (uint32_t k = 0; same && k < o.npos; k++) same = o.pos[k] == w.pos2[k];
        if (!same) bad++;
        if (w.npos_other) moreNops++;
    }
    std::printf("ring property: %llu generated frames, %llu where the new walk NOPs MORE, %llu where a VMID-2 output differs\n",
                (unsigned long long)frames, (unsigned long long)moreNops, (unsigned long long)bad);
    expect_u("ring: every VMID-2 output identical to the frozen 0.0.357 walk", bad, 0u);
    expect_u("ring: the property is not vacuous (other-VMID IBs were found)", moreNops > 0u, 1u);

    uint64_t subs = 0, newNeuter = 0, wrong = 0, same361Bad = 0;
    const uint32_t flagsSet[] = { 0u, 2u, 8u, 0x10u, 0x12u, 0x281u, 0x292u, 0xFFFFFFFFu };
    uint32_t tmpl[N48_TMPL_DWORDS];
    for (uint32_t fl : flagsSet)
        for (uint32_t vm = 0; vm < 34u; vm++)
            for (uint32_t c = 0; c < 7u; c++)
                for (int slot = -1; slot < (int)N48_TMPL_DWORDS; slot++) {
                    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
                    if (slot >= 0) tmpl[slot] = 0xC0064900u;
                    uint8_t info[0x200] = { 0 };
                    n48_wr32(info, N48_SCB_FLAGS, fl); n48_wr32(info, N48_SCB_VMID, vm < 32u ? vm : 0xFFFFFFF3u);
                    n48_wr32(info, N48_SCB_COUNT, c);
                    uint32_t n = 0, why = 0;
                    const uint32_t r = owner_route(info, tmpl, 2u, &n, &why), r0 = old_route(info, tmpl);
                    subs++;
                    // 0.0.362: in a world where WindowServer is on VMID 2 the owner route IS 0.0.361's route, route and count
                    uint32_t n61 = 0, why61 = 0;
                    const uint32_t r61 = frozen0361_route(info, tmpl, &n61, &why61);
                    if (r61 != r || n61 != n) same361Bad++;
                    if (r == r0) continue;
                    if (r0 == N48_ROUTE_PASS && r == N48_ROUTE_NOT_WS) { newNeuter++; continue; }
                    wrong++;
                }
    std::printf("source property: %llu generated submissions, %llu newly NEUTERED, %llu any other difference\n",
                (unsigned long long)subs, (unsigned long long)newNeuter, (unsigned long long)wrong);
    expect_u("source: differs from the frozen 0.0.357 route only by neutering what it passed", wrong, 0u);
    expect_u("source: the property is not vacuous", newNeuter > 0u, 1u);
    std::printf("source property (0.0.362): with WindowServer on VMID 2, %llu submission(s) route differently from the frozen 0.0.361\n",
                (unsigned long long)same361Bad);
    expect_u("source: WindowServer on VMID 2 -> the owner route is the frozen 0.0.361 route exactly", same361Bad, 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.359 — THE COMMITTED FRAME'S EXEMPTION FROM THE RING NOP. The NOP list 0.0.358 built inline in
// hook_gfxWriteTail is FROZEN here verbatim (VMID 2's positions, then every other VMID's); n48_gfxn_nop_list must return it
// position for position unless every one of its conditions holds, and COMMIT not armed must be byte-identical over a large
// generated set. The planted defects are an exemption WITHOUT the arm check and one that honours a STALE token.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t frozen_list_0358(const n48_gfxn_walk_t *w, uint32_t *ibPos)
{
    uint32_t nIbPos = 0;
    for (uint32_t k = 0; k < w->npos2; k++) ibPos[nIbPos++] = w->pos2[k];
    for (uint32_t k = 0; k < w->npos_other; k++) ibPos[nIbPos++] = w->pos_other[k];
    return nIbPos;
}

// 0.0.426 (MIB-COMMIT B8): the T2 planted defect is defined beside committed_frame_mib below; ex_list needs it first.
static uint32_t mib_ib0_alone_nop_list(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                       uint32_t arm, const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared,
                                       uint32_t *why);

enum { EX_REAL = 0, EX_NO_ARM, EX_STALE, EX_NO_GATE, EX_VMID2, EX_NO_BASE, EX_MIB_IB0, EX_MUTANTS };
static const char *ex_name(int m)
{
    static const char *const n[EX_MUTANTS] = { "(the real exemption)", "the exemption WITHOUT the arm check",
                                               "a STALE token honoured (record not in flight / used / seq 0)",
                                               "the gate's answer not required (any token seq)",
                                               "the exemption keyed on VMID 2 (the frozen 0.0.361 clause)",
                                               "the exemption WITHOUT the base-valid condition (M4-WS-VMID-VALID)",
                                               "the multi-IB exemption spares IB 0 ALONE (a SUBSET)" };
    return (m >= 0 && m < EX_MUTANTS) ? n[m] : "?";
}
// The implementation under test: the header's, or a mutant of its decision.
static uint32_t ex_list(int m, const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w, uint32_t arm,
                        const n48_gfxn_exempt_t *ex0, uint32_t *pos, uint32_t *spared, uint32_t *why)
{
    if (m == EX_REAL || !ex0) return n48_gfxn_nop_list(g, size, n, w, arm, ex0, pos, spared, why);
    if (m == EX_VMID2) return frozen0361_nop_list(g, size, n, w, arm, ex0, pos, spared, why);
    if (m == EX_MIB_IB0) return mib_ib0_alone_nop_list(g, size, n, w, arm, ex0, pos, spared, why);
    n48_gfxn_exempt_t ex = *ex0;
    uint32_t a = arm;
    if (m == EX_NO_ARM) a = N48_GFXN_ARM_COMMIT;                                   // the arm is never looked at
    if (m == EX_STALE) { ex.inflight = 1u; ex.used = 0u; if (!ex.seq) ex.seq = ex.gate_seq = 7u; }   // any leftover record counts
    if (m == EX_NO_GATE) ex.gate_seq = ex.seq ? ex.seq : 7u, ex.seq = ex.gate_seq;
    if (m == EX_NO_BASE) ex.base_valid = 1u;                                      // M4-WS-VMID-VALID: any base counts as valid
    return n48_gfxn_nop_list(g, size, n, w, a, &ex, pos, spared, why);
}

// A committed frame as Apple's original writes it: the IB-list template with ONE VMID-2 IB at 0x2d (the model of be20b91..),
// placed at `from` in a ring of `size`. Returns the record hook_gfxCommitIB's TRANSLATE branch would set for it.
static n48_gfxn_exempt_t committed_frame(uint32_t *ring, uint32_t size, uint64_t from, uint32_t len, uint64_t va,
                                         uint32_t vmid = 2u)
{
    uint32_t tmpl[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
    tmpl[0x3d] = 0xC0064900u; tmpl[0x3e] = 0x00000528u; tmpl[0x3f] = 0x60000000u; tmpl[0x40] = 0x000a0018u;   // RELEASE_MEM
    tmpl[0x41] = 0x00000084u; tmpl[0x42] = 0u; tmpl[0x43] = 0u; tmpl[0x44] = 0u;
    uint8_t info[0x200] = { 0 };
    n48_wr32(info, N48_SCB_FLAGS, 0u); n48_wr32(info, N48_SCB_VMID, vmid); n48_wr32(info, N48_SCB_COUNT, 1u);
    n48_wr32(info, N48_SCB_STAMP, 0x21u);
    n48_wr32(info, N48_SCB_ENTRY0, len); n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, (uint32_t)va);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, (uint32_t)(va >> 32));
    uint32_t blk[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = tmpl[k];
    n48_gfxsrc_model_commit(info, blk);
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) ring[(from + k) % size] = blk[k];
    n48_gfxn_exempt_t ex {};
    ex.inflight = 1u; ex.seq = 5u; ex.gate_seq = 5u; ex.used = 0u; ex.nib = 1u; ex.same_thread = 1u;
    ex.pos_known = 1u; ex.expect_pos = (uint32_t)((from + N48_TMPL_IB0_DWORD) % size);
    ex.va = va; ex.len = len & 0xFFFFFu; ex.stamp = 0x21u;
    ex.vmid = vmid;   // 0.0.362: the VMID the frame was judged WindowServer's on
    ex.base_valid = 1u;   // M4-WS-VMID-VALID: a committed frame's VMID base read VALID at the walk (condition 10)
    return ex;
}

// 0.0.426 (MIB-COMMIT B8, test T2) — A 2-IB COMMITTED FRAME as Apple's original writes it: the IB-list template with TWO
// IBs (count 2), at the template's consecutive slots 0x2d and 0x31. Returns the record hook_gfxCommitIB would set for it.
static n48_gfxn_exempt_t committed_frame_mib(uint32_t *ring, uint32_t size, uint64_t from,
                                             uint32_t len0, uint32_t len1, uint64_t va0, uint64_t va1, uint32_t vmid = 2u)
{
    uint32_t tmpl[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
    uint8_t info[0x200] = { 0 };
    n48_wr32(info, N48_SCB_FLAGS, 0u); n48_wr32(info, N48_SCB_VMID, vmid); n48_wr32(info, N48_SCB_COUNT, 2u);
    n48_wr32(info, N48_SCB_STAMP, 0x22u);
    n48_wr32(info, N48_SCB_ENTRY0, len0);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, (uint32_t)va0);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, (uint32_t)(va0 >> 32));
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE, len1);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE + N48_SCB_ENTRY_VA, (uint32_t)va1);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE + N48_SCB_ENTRY_VA + 4u, (uint32_t)(va1 >> 32));
    uint32_t blk[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = tmpl[k];
    n48_gfxsrc_model_commit(info, blk);
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) ring[(from + k) % size] = blk[k];
    n48_gfxn_exempt_t ex {};
    ex.inflight = 1u; ex.seq = 5u; ex.gate_seq = 5u; ex.used = 0u; ex.nib = 2u; ex.same_thread = 1u;
    ex.pos_known = 1u; ex.expect_pos = (uint32_t)((from + N48_TMPL_IB0_DWORD) % size);
    ex.va = va0; ex.len = len0 & 0xFFFFFu; ex.stamp = 0x22u;
    ex.vmid = vmid; ex.base_valid = 1u; ex.mib = 1u;
    ex.va_k[0] = va0; ex.va_k[1] = va1; ex.len_k[0] = len0 & 0xFFFFFu; ex.len_k[1] = len1 & 0xFFFFFu;
    return ex;
}

// The T2 PLANTED DEFECT: "spare IB 0 alone". For a valid multi-IB record the real list is EMPTY (all nib spared); this
// mutant leaves IB 1..nib-1 in the NOP list - a SUBSET, which is exactly what B8 forbids. It is otherwise the real
// function, so it is caught only by a check that pins "empty or full, never a subset".
static uint32_t mib_ib0_alone_nop_list(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                       uint32_t arm, const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared,
                                       uint32_t *why)
{
    const uint32_t np = n48_gfxn_nop_list(g, size, n, w, arm, ex, pos, spared, why);
    if (ex && ex->mib && *why == N48_GFXN_EX_SPARED) {
        uint32_t full[2u * N48_GFXN_MAX_IBS];
        const uint32_t nf = frozen_list_0358(w, full);
        uint32_t out = 0u;
        for (uint32_t j = 0; j < nf; j++) if (full[j] != ex->expect_pos) pos[out++] = full[j];
        *spared = ex->expect_pos;
        return out;
    }
    return np;
}

static int exempt_checks(int m)
{
    const int before = gFail;
    enum { kSize = 512 };
    char b[160];
    const uint64_t from = 448;                        // the frame wraps the ring's end: the modulo is exercised
    static uint32_t ring[kSize];
    for (uint32_t k = 0; k < kSize; k++) ring[k] = N48_TMPL_NOP;
    const n48_gfxn_exempt_t good = committed_frame(ring, kSize, from, 1040u, 0x4001a0000ull);
    n48_gfxn_walk_t w; n48_gfxn_walk(ring, from, 128, kSize, &w, nullptr, nullptr);
    std::snprintf(b, sizeof b, "exempt: the committed frame's one VMID-2 IB is at +0x2d          %s", ex_name(m));
    expect_u(b, w.ibs == 1u && w.npos2 == 1u && w.pos2[0] == (from + 0x2d) % kSize && w.stop_at == 128u, 1);
    uint32_t pos[2u * N48_GFXN_MAX_IBS], sp = 0, why = 0;
    // 1. THE POSITIVE CONTROL: every condition holds -> SPARED, nothing left to NOP. Without it a mutant that never spares
    //    would pass every refusal below.
    uint32_t np = ex_list(m, ring, kSize, 128, &w, N48_GFXN_ARM_COMMIT, &good, pos, &sp, &why);
    std::snprintf(b, sizeof b, "exempt: COMMIT + a live, gated, unused token -> SPARED              %s", ex_name(m));
    expect_u(b, why == N48_GFXN_EX_SPARED && np == 0u && sp == (from + 0x2d) % kSize, 1);
    // 2. EVERY OTHER CASE: the 0.0.358 list, position for position, and nothing spared.
    uint32_t frozen[2u * N48_GFXN_MAX_IBS];
    const uint32_t nf = frozen_list_0358(&w, frozen);
    struct Case { const char *name; void (*brk)(n48_gfxn_exempt_t &, uint32_t &); uint32_t want; };
    const Case k[] = {
        { "arm DECIDE",                      [](n48_gfxn_exempt_t &, uint32_t &a) { a = 1u; }, N48_GFXN_EX_NOT_ARMED },
        { "arm OFF",                         [](n48_gfxn_exempt_t &, uint32_t &a) { a = 0u; }, N48_GFXN_EX_NOT_ARMED },
        { "arm 3 (no such level)",           [](n48_gfxn_exempt_t &, uint32_t &a) { a = 3u; }, N48_GFXN_EX_NOT_ARMED },
        { "record not in flight (stale)",    [](n48_gfxn_exempt_t &e, uint32_t &) { e.inflight = 0u; }, N48_GFXN_EX_NONE },
        { "record already used (stale)",     [](n48_gfxn_exempt_t &e, uint32_t &) { e.used = 1u; }, N48_GFXN_EX_USED },
        { "token seq 0 (stale)",             [](n48_gfxn_exempt_t &e, uint32_t &) { e.seq = 0u; e.gate_seq = 0u; }, N48_GFXN_EX_GATE },
        { "gate answered for another seq",   [](n48_gfxn_exempt_t &e, uint32_t &) { e.gate_seq = 4u; }, N48_GFXN_EX_GATE },
        { "gate did not answer COMMIT (0)",  [](n48_gfxn_exempt_t &e, uint32_t &) { e.gate_seq = 0u; }, N48_GFXN_EX_GATE },
        { "two IBs in the submission",       [](n48_gfxn_exempt_t &e, uint32_t &) { e.nib = 2u; }, N48_GFXN_EX_NIB },
        { "another thread's walk",           [](n48_gfxn_exempt_t &e, uint32_t &) { e.same_thread = 0u; }, N48_GFXN_EX_THREAD },
        { "IB 0 not where the template puts it", [](n48_gfxn_exempt_t &e, uint32_t &) { e.expect_pos ^= 4u; }, N48_GFXN_EX_POSITION },
        { "Apple's wptr at the call unread", [](n48_gfxn_exempt_t &e, uint32_t &) { e.pos_known = 0u; }, N48_GFXN_EX_POSITION },
        { "token VA differs",                [](n48_gfxn_exempt_t &e, uint32_t &) { e.va += 0x1000u; }, N48_GFXN_EX_IDENTITY },
        { "token length differs",            [](n48_gfxn_exempt_t &e, uint32_t &) { e.len += 1u; }, N48_GFXN_EX_IDENTITY },
        { "VMID's base bit 0 clear (M4-WS-VMID-VALID)", [](n48_gfxn_exempt_t &e, uint32_t &) { e.base_valid = 0u; }, N48_GFXN_EX_BASE },
        // build 0.0.495 (switch 62): the kext's heap-generation answer refuses - 0.0.358's list, reason heap-gen.
        { "shader heap moved since the verdict", [](n48_gfxn_exempt_t &e, uint32_t &) { e.heap_refuse = 1u; }, N48_GFXN_EX_HEAPGEN },
    };
    for (const Case &c : k) {
        n48_gfxn_exempt_t e = good; uint32_t a = N48_GFXN_ARM_COMMIT;
        c.brk(e, a);
        np = ex_list(m, ring, kSize, 128, &w, a, &e, pos, &sp, &why);
        bool same = np == nf && sp == 0xFFFFFFFFu;
        for (uint32_t j = 0; same && j < np; j++) same = pos[j] == frozen[j];
        std::snprintf(b, sizeof b, "exempt: %-36s -> 0.0.358's list, nothing spared %s", c.name, ex_name(m));
        expect_u(b, same ? 1u : 0u, 1u);
        std::snprintf(b, sizeof b, "exempt: %-36s -> reason %-10s        %s", c.name, n48_gfxn_ex_name(c.want), ex_name(m));
        expect_u(b, why, c.want);
    }
    // 3. THE FRAME: a second IB in the walked range, another VMID, a stopped walk - each refuses.
    {
        static uint32_t r2[kSize];
        for (uint32_t j = 0; j < kSize; j++) r2[j] = ring[j];
        r2[(from + 0x31) % kSize] = 0xC0023F00u; r2[(from + 0x32) % kSize] = 0x00300000u; r2[(from + 0x33) % kSize] = 4u;
        r2[(from + 0x34) % kSize] = 0x02000010u;                          // a second VMID-2 IB right after the committed one
        n48_gfxn_walk_t w2; n48_gfxn_walk(r2, from, 128, kSize, &w2, nullptr, nullptr);
        uint32_t f2[2u * N48_GFXN_MAX_IBS]; const uint32_t nf2 = frozen_list_0358(&w2, f2);
        np = ex_list(m, r2, kSize, 128, &w2, N48_GFXN_ARM_COMMIT, &good, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: a second IB in the walked range -> both NOPed (frame)     %s", ex_name(m));
        expect_u(b, np == nf2 && nf2 == 2u && pos[0] == f2[0] && pos[1] == f2[1] && why == N48_GFXN_EX_FRAME, 1);
        for (uint32_t j = 0; j < kSize; j++) r2[j] = ring[j];
        r2[(from + 0x30) % kSize] = (r2[(from + 0x30) % kSize] & ~0x0F000000u) | 0x03000000u;   // the IB is VMID 3
        n48_gfxn_walk(r2, from, 128, kSize, &w2, nullptr, nullptr);
        np = ex_list(m, r2, kSize, 128, &w2, N48_GFXN_ARM_COMMIT, &good, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: the IB is VMID 3 -> NOPed (frame)                         %s", ex_name(m));
        expect_u(b, np == 1u && why == N48_GFXN_EX_FRAME && sp == 0xFFFFFFFFu, 1);
        for (uint32_t j = 0; j < kSize; j++) r2[j] = ring[j];
        r2[(from + 0x60) % kSize] = 0x12345678u;                          // a type-0 header after the IB stops the walk
        n48_gfxn_walk(r2, from, 128, kSize, &w2, nullptr, nullptr);
        np = ex_list(m, r2, kSize, 128, &w2, N48_GFXN_ARM_COMMIT, &good, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: the walk stopped -> NOPed (walk)                          %s", ex_name(m));
        expect_u(b, np == 1u && why == N48_GFXN_EX_WALK && sp == 0xFFFFFFFFu, 1);
    }
    // 3b. 0.0.362: THE OWNER'S VMID. hp3's world: WindowServer drew on VMID 3. Its committed frame (one VMID-3 IB)
    //     with a record naming VMID 3 is SPARED - the positive control that the frozen 0.0.361 clause (VMID 2 only) fails;
    //     a record naming VMID 3 over a VMID-2 IB (the draw client's), and a record naming no VMID, are refused.
    {
        static uint32_t r3[kSize];
        for (uint32_t j = 0; j < kSize; j++) r3[j] = N48_TMPL_NOP;
        const n48_gfxn_exempt_t ws3 = committed_frame(r3, kSize, from, 1040u, 0x4001a0000ull, 3u);
        n48_gfxn_walk_t w3; n48_gfxn_walk(r3, from, 128, kSize, &w3, nullptr, nullptr);
        np = ex_list(m, r3, kSize, 128, &w3, N48_GFXN_ARM_COMMIT, &ws3, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: WindowServer on VMID 3, its one IB, token VMID 3 -> SPARED    %s", ex_name(m));
        expect_u(b, why == N48_GFXN_EX_SPARED && np == 0u && sp == (from + 0x2d) % kSize, 1);
        n48_gfxn_exempt_t e = good; e.vmid = 3u;                          // token says VMID 3, the ring's IB is VMID 2
        np = ex_list(m, ring, kSize, 128, &w, N48_GFXN_ARM_COMMIT, &e, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: token VMID 3 over a VMID-2 IB -> NOPed (frame)               %s", ex_name(m));
        expect_u(b, np == nf && why == N48_GFXN_EX_FRAME && sp == 0xFFFFFFFFu, 1);
        e = good; e.vmid = 0u;
        np = ex_list(m, ring, kSize, 128, &w, N48_GFXN_ARM_COMMIT, &e, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: token with no VMID -> NOPed (frame)                           %s", ex_name(m));
        expect_u(b, np == nf && why == N48_GFXN_EX_FRAME && sp == 0xFFFFFFFFu, 1);
        n48_gfxn_exempt_t e3 = ws3; e3.vmid = 2u;                         // WindowServer's IB is VMID 3, token says 2
        np = ex_list(m, r3, kSize, 128, &w3, N48_GFXN_ARM_COMMIT, &e3, pos, &sp, &why);
        std::snprintf(b, sizeof b, "exempt: token VMID 2 over a VMID-3 IB -> NOPed (frame)               %s", ex_name(m));
        expect_u(b, np == 1u && why == N48_GFXN_EX_FRAME && sp == 0xFFFFFFFFu, 1);
    }
    // 4. No record at all (the ordinary case, every frame of every run so far): the 0.0.358 list, reason NONE.
    np = ex_list(m, ring, kSize, 128, &w, N48_GFXN_ARM_COMMIT, nullptr, pos, &sp, &why);
    std::snprintf(b, sizeof b, "exempt: no record -> 0.0.358's list, reason none                  %s", ex_name(m));
    expect_u(b, np == nf && pos[0] == frozen[0] && why == N48_GFXN_EX_NONE && sp == 0xFFFFFFFFu, 1);
    // ---- 5. T2 (0.0.426, MIB-COMMIT B8): A 2-IB FRAME IS SPARED ALL OR NONE. The positive control: a live, gated, unused
    //         2-IB record over a 2-IB walk SPARES BOTH (the NOP list is EMPTY). Then over EVERY single-field corruption the
    //         answer is either SPARED (empty list) or the FULL 0.0.358 list - NEVER A SUBSET. The planted defect
    //         `mib_ib0_alone_nop_list` spares IB 0 alone and is caught by the positive control's `np == 0`.
    {
        static uint32_t r2[kSize];
        for (uint32_t j = 0; j < kSize; j++) r2[j] = N48_TMPL_NOP;
        const n48_gfxn_exempt_t good2 = committed_frame_mib(r2, kSize, from, 15520u, 7616u, 0x4001a0000ull, 0x4005a0000ull);
        n48_gfxn_walk_t wm; n48_gfxn_walk(r2, from, 128, kSize, &wm, nullptr, nullptr);
        std::snprintf(b, sizeof b, "T2 exempt: the committed 2-IB frame walks two IBs at +0x2d/+0x31   %s", ex_name(m));
        expect_u(b, wm.ibs == 2u && wm.npos2 == 2u && wm.stop_at == 128u, 1);
        uint32_t f2[2u * N48_GFXN_MAX_IBS]; const uint32_t nf2 = frozen_list_0358(&wm, f2);
        np = ex_list(m, r2, kSize, 128, &wm, N48_GFXN_ARM_COMMIT, &good2, pos, &sp, &why);
        std::snprintf(b, sizeof b, "T2 exempt: a valid 2-IB record -> SPARED, EMPTY list (all or none)  %s", ex_name(m));
        expect_u(b, why == N48_GFXN_EX_SPARED && np == 0u, 1);
        // Every single-field corruption: empty or full, never a subset.
        struct { const char *what; void (*brk)(n48_gfxn_exempt_t &); } corrupt[] = {
            { "IB 1's VA",        [](n48_gfxn_exempt_t &e){ e.va_k[1] += 0x1000u; } },
            { "IB 1's length",    [](n48_gfxn_exempt_t &e){ e.len_k[1] += 1u; } },
            { "IB 0's VA",        [](n48_gfxn_exempt_t &e){ e.va_k[0] += 0x1000u; } },
            { "IB 0's length",    [](n48_gfxn_exempt_t &e){ e.len_k[0] += 1u; } },
            { "the mib bit",      [](n48_gfxn_exempt_t &e){ e.mib = 0u; } },
            { "nib -> 1",         [](n48_gfxn_exempt_t &e){ e.nib = 1u; } },
            { "nib -> 3",         [](n48_gfxn_exempt_t &e){ e.nib = 3u; } },
            { "the record used",  [](n48_gfxn_exempt_t &e){ e.used = 1u; } },
            { "the seq is 0",     [](n48_gfxn_exempt_t &e){ e.seq = 0u; e.gate_seq = 0u; } },
            { "the gate seq",     [](n48_gfxn_exempt_t &e){ e.gate_seq = 4u; } },
            { "not the same thread", [](n48_gfxn_exempt_t &e){ e.same_thread = 0u; } },
            { "expect_pos moved by one slot", [](n48_gfxn_exempt_t &e){ e.expect_pos ^= 4u; } },
            { "pos not known",    [](n48_gfxn_exempt_t &e){ e.pos_known = 0u; } },
            { "another VMID",     [](n48_gfxn_exempt_t &e){ e.vmid = 3u; } },
            { "base not valid",   [](n48_gfxn_exempt_t &e){ e.base_valid = 0u; } },
            { "not in flight",    [](n48_gfxn_exempt_t &e){ e.inflight = 0u; } },
        };
        int bad = 0;
        for (auto &c : corrupt) {
            n48_gfxn_exempt_t e = good2; c.brk(e);
            np = ex_list(m, r2, kSize, 128, &wm, N48_GFXN_ARM_COMMIT, &e, pos, &sp, &why);
            if (why == N48_GFXN_EX_SPARED) { if (np != 0u) bad++; }
            else { bool same = (np == nf2 && sp == 0xFFFFFFFFu); for (uint32_t j = 0; same && j < np; j++) same = pos[j] == f2[j]; if (!same) bad++; }
        }
        std::snprintf(b, sizeof b, "T2 exempt: every single-field corruption is empty or FULL, never a subset %s", ex_name(m));
        expect_u(b, bad, 0u);
        // A specific planted break: changing IB 1's VA must NOT be spared (the whole frame is refused).
        n48_gfxn_exempt_t e = good2; e.va_k[1] += 0x1000u;
        np = ex_list(m, r2, kSize, 128, &wm, N48_GFXN_ARM_COMMIT, &e, pos, &sp, &why);
        std::snprintf(b, sizeof b, "T2 exempt: IB 1's VA differs -> the whole frame is NOPed            %s", ex_name(m));
        expect_u(b, why != N48_GFXN_EX_SPARED && np == nf2 && sp == 0xFFFFFFFFu, 1);
    }
    return gFail - before;
}

// THE PROPERTY: over generated rings and records, with COMMIT NOT armed the list - and the ring bytes after the NOP is
// applied - is byte-identical to 0.0.358's; with COMMIT armed only a record valid by construction ever spares, and then only
// its own one position.
static int exempt_property(int m, bool print)
{
    const int before = gFail;
    uint64_t x = 0xD1B54A32D192ED03ull, frames = 0, notArmedDiff = 0, validSpared = 0, validFrames = 0, armedDiff = 0,
             armedBad = 0, vmid2Diff = 0, vmid2Same = 0;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    enum { kS = 512 };
    static uint32_t g[kS], g0[kS], g1[kS];
    const uint32_t kinds[] = { 0xC0023F00u, 0xC0064900u, 0xC0017900u, 0xFFFF1000u, 0x80000000u, 0xC0021000u, 0x12345678u };
    for (uint32_t it = 0; it < 150000u; it++) {
        const uint64_t from = rnd() % kS;
        const uint32_t shape = (uint32_t)(rnd() % 4u);
        n48_gfxn_exempt_t ex {};
        uint64_t fva = 0; uint32_t flen = 0, fvm = 0;
        if (shape == 0u) {                                        // noise
            for (uint32_t k = 0; k < kS; k++) { const uint64_t r = rnd(); g[k] = (r & 0x100u) ? kinds[r % 7u] : (uint32_t)(r >> 16); }
        } else {                                                  // a committed frame, sometimes with company
            for (uint32_t k = 0; k < kS; k++) g[k] = N48_TMPL_NOP;
            flen = 1u + (uint32_t)(rnd() % 0xFFFFFu); fva = 0x400000000ull + ((rnd() % 0x100000u) << 12);
            fvm = 1u + (uint32_t)(rnd() % 15u);                   // 0.0.362: WindowServer's VMID is whatever the world gave it
            ex = committed_frame(g, kS, from, flen, fva, fvm);
            if (rnd() % 6u == 0u) ex.vmid = (uint32_t)(rnd() % 17u);   // sometimes a record naming another (or no) VMID
            if (shape == 2u) {                                    // plant 1-3 more IBs of random VMIDs somewhere in range
                const uint32_t extra = 1u + (uint32_t)(rnd() % 3u);
                for (uint32_t j = 0; j < extra; j++) {
                    const uint32_t at = (uint32_t)((from + 0x46u + 4u * (rnd() % 14u)) % kS);
                    g[at] = 0xC0023F00u; g[(at + 1u) % kS] = (uint32_t)rnd() & ~3u; g[(at + 2u) % kS] = 4u;
                    g[(at + 3u) % kS] = ((uint32_t)(rnd() % 16u) << 24) | 0x40u;
                }
            }
        }
        // the record: sometimes the valid one, otherwise a random field corrupted, sometimes pure noise
        const uint32_t rec = (uint32_t)(rnd() % 8u);
        if (shape == 0u || rec == 7u) {
            ex.inflight = (uint32_t)(rnd() % 3u); ex.seq = (uint32_t)(rnd() % 4u); ex.gate_seq = (uint32_t)(rnd() % 4u);
            ex.used = (uint32_t)(rnd() % 2u); ex.nib = (uint32_t)(rnd() % 3u); ex.same_thread = (uint32_t)(rnd() % 2u);
            ex.pos_known = (uint32_t)(rnd() % 2u); ex.expect_pos = (uint32_t)((from + 0x2du) % kS);
            ex.vmid = (uint32_t)(rnd() % 17u);
        } else if (rec >= 4u) {
            switch (rnd() % 9u) {
            case 0: ex.inflight = 0u; break; case 1: ex.used = 1u; break; case 2: ex.gate_seq ^= 1u; break;
            case 3: ex.seq = 0u; break; case 4: ex.nib = 2u; break; case 5: ex.same_thread = 0u; break;
            case 6: ex.expect_pos = (ex.expect_pos + 4u) % kS; break; case 7: ex.va ^= 0x1000u; break; default: ex.len ^= 1u; break;
            }
        }
        const uint64_t n = 128u;
        n48_gfxn_walk_t w; n48_gfxn_walk(g, from, n, kS, &w, nullptr, nullptr);
        uint32_t frozen[2u * N48_GFXN_MAX_IBS]; const uint32_t nf = frozen_list_0358(&w, frozen);
        frames++;
        // (a) COMMIT NOT armed: every arm value but 2
        const uint32_t armNot = (uint32_t)(rnd() % 6u); const uint32_t a0 = armNot >= 2u ? armNot + 1u : armNot;
        uint32_t pos[2u * N48_GFXN_MAX_IBS], sp = 0, why = 0;
        uint32_t np = ex_list(m, g, kS, n, &w, a0, &ex, pos, &sp, &why);
        bool same = np == nf && sp == 0xFFFFFFFFu;
        for (uint32_t j = 0; same && j < np; j++) same = pos[j] == frozen[j];
        for (uint32_t k = 0; k < kS; k++) g0[k] = g1[k] = g[k];
        for (uint32_t j = 0; j < nf; j++) if (n48_gfxn_nop_for(g0[frozen[j]])) g0[frozen[j]] = n48_gfxn_nop_for(g0[frozen[j]]);
        for (uint32_t j = 0; j < np; j++) if (n48_gfxn_nop_for(g1[pos[j]])) g1[pos[j]] = n48_gfxn_nop_for(g1[pos[j]]);
        for (uint32_t k = 0; same && k < kS; k++) same = g0[k] == g1[k];
        if (!same) notArmedDiff++;
        // (b) COMMIT armed: spares only when the record is the valid one AND the frame is exactly the committed one
        np = ex_list(m, g, kS, n, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
        // The oracle, restated from the brief rather than from the code: a single-IB committed frame (no company) and a record
        // that is in flight, gated for its own seq, unused, single-IB, same thread, at the template's slot, naming that IB.
        const bool valid = (shape == 1u || shape == 3u) && ex.inflight == 1u && ex.seq != 0u && ex.gate_seq == ex.seq &&
                           ex.used == 0u && ex.nib == 1u && ex.same_thread == 1u && ex.pos_known == 1u &&
                           ex.expect_pos == (uint32_t)((from + 0x2du) % kS) && ex.va == fva && ex.len == flen &&
                           ex.vmid == fvm;                     // 0.0.362: on the VMID the token's frame was judged on
        if (valid) validFrames++;
        // 0.0.362: where the world put WindowServer on VMID 2 and the record names VMID 2, 0.0.361's answer exactly.
        if (m == EX_REAL && ex.vmid == 2u && (shape == 0u || fvm == 2u)) {
            uint32_t p61[2u * N48_GFXN_MAX_IBS], sp61 = 0, why61 = 0;
            const uint32_t np61 = frozen0361_nop_list(g, kS, n, &w, N48_GFXN_ARM_COMMIT, &ex, p61, &sp61, &why61);
            bool s61 = np61 == np && sp61 == sp && why61 == why;
            for (uint32_t j = 0; s61 && j < np; j++) s61 = p61[j] == pos[j];
            if (!s61) vmid2Diff++; else vmid2Same++;
        }
        if (why == N48_GFXN_EX_SPARED) {
            if (!valid || np != nf - 1u || sp != frozen[0]) armedBad++; else validSpared++;
        } else {
            bool s2 = np == nf && sp == 0xFFFFFFFFu;
            for (uint32_t j = 0; s2 && j < np; j++) s2 = pos[j] == frozen[j];
            if (!s2) armedDiff++;
            if (valid) armedBad++;                                 // a valid committed frame must be spared
        }
    }
    if (print)
        std::printf("exempt property: %llu generated frames; COMMIT NOT armed: %llu differ from 0.0.358 (list or ring bytes); "
                    "COMMIT armed: %llu of %llu valid committed frames spared, %llu wrong spares or valid frames not spared, "
                    "%llu refusals that altered the list\n", (unsigned long long)frames, (unsigned long long)notArmedDiff,
                    (unsigned long long)validSpared, (unsigned long long)validFrames, (unsigned long long)armedBad,
                    (unsigned long long)armedDiff);
    expect_u("exempt property: COMMIT not armed -> byte-identical to 0.0.358", notArmedDiff, 0u);
    expect_u("exempt property: COMMIT armed -> only valid committed frames spared, each by its one IB", armedBad, 0u);
    expect_u("exempt property: a refused exemption never alters the list", armedDiff, 0u);
    expect_u("exempt property: not vacuous (valid frames were spared)", validSpared > 1000u, 1u);
    if (m == EX_REAL) {
        if (print)
            std::printf("exempt property (0.0.362): WindowServer on VMID 2 and a VMID-2 record: %llu frame(s) compared with the frozen "
                        "0.0.361 exemption, %llu differ\n", (unsigned long long)vmid2Same + vmid2Diff, (unsigned long long)vmid2Diff);
        expect_u("exempt property: VMID 2 world -> the frozen 0.0.361 exemption exactly", vmid2Diff, 0u);
        expect_u("exempt property: ... not vacuous", vmid2Same > 1000u, 1u);
    }
    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.362 — THE ROUTE BY OWNER. hp3's world: the draw client on VMID 2, WindowServer 1142 on VMID 3,
// SecurityAgent 1263 on VMID 4. The route under test takes `is_ws` from that world's owner map; the planted defect is the
// frozen 0.0.361 route, which takes it from the VMID number.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t owner_route_m(int m, const uint8_t *info, const uint32_t *tmpl, uint32_t wsVmid, uint32_t *n, uint32_t *why)
{
    if (m == 1) return frozen0361_route(info, tmpl, n, why);            // the planted defect: VMID 2 is WindowServer's
    return owner_route(info, tmpl, wsVmid, n, why);
}
static int owner_checks(int m)
{
    const int before = gFail;
    uint32_t tmpl[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
    uint8_t info[0x200] = { 0 };
    n48_wr32(info, N48_SCB_FLAGS, 0x281u); n48_wr32(info, N48_SCB_COUNT, 1u); n48_wr32(info, N48_SCB_STAMP, 0x22cu);
    n48_wr32(info, N48_SCB_ENTRY0, 3728u); n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, 0x000d0000u);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, 0x4u);
    uint32_t n = 0, why = 0;
    n48_wr32(info, N48_SCB_VMID, 3u);
    expect_u("owner (hp3): WindowServer 1142's VMID-3 frame goes to the DECISION", owner_route_m(m, info, tmpl, 3u, &n, &why),
             N48_ROUTE_DECIDE);
    expect_u("owner (hp3):   with its IB count and reason OK", n == 1u && why == N48_SRC_OK, 1u);
    n48_wr32(info, N48_SCB_VMID, 2u);
    expect_u("owner (hp3): the draw client's VMID-2 frame is NEUTERED, never judged", owner_route_m(m, info, tmpl, 3u, &n, &why),
             N48_ROUTE_NOT_WS);
    expect_u("owner (hp3):   reason NOT_WS", why, N48_SRC_NOT_WS);
    n48_wr32(info, N48_SCB_VMID, 4u);
    expect_u("owner (hp3): SecurityAgent 1263's VMID-4 frame is NEUTERED", owner_route_m(m, info, tmpl, 3u, &n, &why),
             N48_ROUTE_NOT_WS);
    n48_wr32(info, N48_SCB_VMID, 3u);
    expect_u("owner: WindowServer unidentified -> its frame too is NEUTERED (fail closed)", owner_route_m(m, info, tmpl, 0u, &n, &why),
             N48_ROUTE_NOT_WS);
    n48_wr32(info, N48_SCB_VMID, 2u);
    expect_u("owner: WindowServer unidentified -> a VMID-2 frame is NEUTERED (fail closed)", owner_route_m(m, info, tmpl, 0u, &n, &why),
             N48_ROUTE_NOT_WS);
    n48_wr32(info, N48_SCB_FLAGS, 0x10u);
    expect_u("owner: WindowServer's single-IB frame -> PASS, exactly as 0.0.361 (the ring backstop)",
             owner_route_m(m, info, tmpl, 2u, &n, &why), N48_ROUTE_PASS);
    return gFail - before;
}

// THE HOOK'S OUTCOME, over every generated submission and every owner map: PASS (Apple, untouched), NEUTER (entries zeroed) or
// TRANSLATE. 0.0.361's outcome is its VMID-2 route; 0.0.362's is the owner route; DECIDE's action is n48_sd_action for the arm.
//   (1) the PASS set is 0.0.361's, whatever the owner map;
//   (2) with COMMIT NOT armed (OFF, DECIDE), every submission's outcome is 0.0.361's - the CP receives no more and no less;
//   (3) with COMMIT armed, a TRANSLATE is only ever WindowServer's frame by owner (and, as before, a TRANSLATE verdict with commit_ok).
static uint32_t outcome(uint32_t route, uint32_t arm, uint32_t verdict, uint32_t commitOk)
{
    if (route == N48_ROUTE_PASS) return N48_SD_ACT_PASS;
    if (route != N48_ROUTE_DECIDE) return N48_SD_ACT_NEUTER;
    return n48_sd_action(arm, 1u, verdict, commitOk);
}
static void owner_property()
{
    uint64_t subs = 0, passDiff = 0, notArmedDiff = 0, translateNotWs = 0, judgedWsOffVmid2 = 0, decideNotWs = 0;
    const uint32_t flagsSet[] = { 0u, 2u, 8u, 0x10u, 0x12u, 0x281u, 0xFFFFFFFFu };
    const uint32_t arms[] = { N48_SD_ARM_OFF, N48_SD_ARM_DECIDE, N48_SD_ARM_COMMIT };
    const uint32_t verdicts[] = { N48_XV_TRANSLATE, N48_XV_TARGET_VRAM, N48_XV_SHAPE };
    uint32_t tmpl[N48_TMPL_DWORDS];
    for (uint32_t fl : flagsSet)
        for (uint32_t vm = 0; vm < 18u; vm++)
            for (uint32_t c = 0; c < 6u; c++)
                for (int slot = -1; slot < (int)N48_TMPL_DWORDS; slot += 7) {
                    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
                    if (slot >= 0) tmpl[slot] = 0xC0064900u;
                    uint8_t info[0x200] = { 0 };
                    n48_wr32(info, N48_SCB_FLAGS, fl); n48_wr32(info, N48_SCB_VMID, vm < 16u ? vm : 0xFFFFFFF3u);
                    n48_wr32(info, N48_SCB_COUNT, c);
                    uint32_t n61 = 0, w61 = 0;
                    const uint32_t r61 = frozen0361_route(info, tmpl, &n61, &w61);
                    for (uint32_t wsVmid = 0; wsVmid < 16u; wsVmid++) {       // 0 = WindowServer unidentified
                        uint32_t n = 0, why = 0;
                        const uint32_t r = owner_route(info, tmpl, wsVmid, &n, &why);
                        const bool isWs = wsVmid && (n48_rd32(info, N48_SCB_VMID) & 0xFu) == wsVmid;
                        subs++;
                        if ((r == N48_ROUTE_PASS) != (r61 == N48_ROUTE_PASS)) passDiff++;
                        if (r == N48_ROUTE_DECIDE && !isWs) decideNotWs++;
                        if (r == N48_ROUTE_DECIDE && wsVmid != 2u) judgedWsOffVmid2++;
                        for (uint32_t a : arms)
                            for (uint32_t v : verdicts)
                                for (uint32_t co = 0; co < 2u; co++) {
                                    const uint32_t o = outcome(r, a, v, co), o61 = outcome(r61, a, v, co);
                                    if (a != N48_SD_ARM_COMMIT && o != o61) notArmedDiff++;
                                    if (o == N48_SD_ACT_TRANSLATE && !isWs) translateNotWs++;
                                }
                    }
                }
    std::printf("owner property: %llu (submission, owner map) pairs; PASS-set differences from 0.0.361 %llu; COMMIT-not-armed outcome "
                "differences %llu; TRANSLATE of a frame not WindowServer's %llu; DECIDE not WindowServer's %llu; WindowServer judged "
                "off VMID 2 %llu (the fix at work)\n", (unsigned long long)subs, (unsigned long long)passDiff,
                (unsigned long long)notArmedDiff, (unsigned long long)translateNotWs, (unsigned long long)decideNotWs,
                (unsigned long long)judgedWsOffVmid2);
    expect_u("owner property: the PASS set is 0.0.361's, whatever the owner map", passDiff, 0u);
    expect_u("owner property: COMMIT not armed -> every outcome is 0.0.361's (the CP gets nothing new)", notArmedDiff, 0u);
    expect_u("owner property: COMMIT armed -> TRANSLATE only ever WindowServer's frame by owner", translateNotWs, 0u);
    expect_u("owner property: DECIDE only ever WindowServer's frame by owner", decideNotWs, 0u);
    expect_u("owner property: not vacuous (WindowServer judged on VMIDs other than 2)", judgedWsOffVmid2 > 0u, 1u);
}

int main()
{
    expect_u("IB header -> NOP of the same count", n48_gfxn_nop_for(0xC0023F00u), 0xC0021000u);
    expect_u("low flag bits kept", n48_gfxn_nop_for(0xC0023F01u), 0xC0021001u);
    expect_u("count 3 IB refused", n48_gfxn_nop_for(0xC0033F00u), 0u);
    expect_u("RELEASE_MEM refused", n48_gfxn_nop_for(0xC0064900u), 0u);
    expect_u("type-2 refused", n48_gfxn_nop_for(0x80000000u), 0u);
    expect_u("Apple's one-dword NOP refused", n48_gfxn_nop_for(0xFFFF1000u), 0u);

    const uint32_t frame[] = {
        0xC0032200u, 0x0000001cu, 0x00000084u, 0x00000000u, 0x00000079u,                         // COND_EXEC
        0xC0044000u, 0x00010209u, 0x00000000u, 0x00000000u, 0x000a0010u, 0x00000084u,            // COPY_DATA
        0xC0033700u, 0x00000000u, 0x0000a2a4u, 0x00000000u, 0x00000016u,                         // WRITE_DATA
        0xC0065800u, 0x86287fc3u, 0xffffffffu, 0x000000ffu, 0u, 0u, 0x00000010u, 0u,             // ACQUIRE_MEM
        0xC0017900u, 0x00000074u, 0x00000000u,                                                   // SET_UCONFIG_REG
        0xC0053C00u, 0x00000003u, 0x0000c07fu, 0u, 0u, 0x80000000u, 0x0000000au,                 // WAIT_REG_MEM
        0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
        0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
        0xC0023F00u, 0x00190000u, 0x00000004u, 0x02000e90u,                                     // INDIRECT_BUFFER VMID 2
        0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
        0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u, 0xFFFF1000u,
        0xC0064900u, 0x00000528u, 0x60000000u, 0x000a0018u, 0x00000084u, 0u, 0u, 0u,             // RELEASE_MEM (timestamp) at +0x3d
        0xC0064900u, 0x0030c514u, 0x22000000u, 0u, 0x00000084u, 0x00000002u, 0u, 0u,             // RELEASE_MEM (stamp 2) at +0x45
    };
    const uint32_t nf = sizeof(frame) / sizeof(frame[0]);
    expect_u("the IB packet sits at frame dword 0x2d, as F1's did", nf > 0x2d ? frame[0x2d] : 0, 0xC0023F00u);
    enum { kSize = 256 };
    uint32_t ring[kSize] = { 0 };
    const uint64_t from = 200;   // [200..328) wraps at 256
    for (uint32_t k = 0; k < 128; k++) ring[(from + k) % kSize] = 0xFFFF1000u;
    for (uint32_t k = 0; k < nf; k++) ring[(from + k) % kSize] = frame[k];
    ring[(from + 0x7e) % kSize] = 0xC0008B00u; ring[(from + 0x7f) % kSize] = 0u;              // SWITCH_BUFFER at +0x7e
    Walk a = walk(ring, from, 128, kSize);
    expect_u("before: walk reaches the end", a.stop == 0 && a.end == 128, 1);
    expect_u("before: 10 type-3 packets (census count)", a.packets, 10);
    expect_u("before: 1 VMID-2 IB", a.ibs, 1);
    expect_u("before: IB position = (from + 0x2d) % size", a.ibPos[0], (from + 0x2d) % kSize);

    const uint32_t nop = n48_gfxn_nop_for(ring[a.ibPos[0]]);
    ring[a.ibPos[0]] = nop;
    Walk b = walk(ring, from, 128, kSize);
    expect_u("after: walk still reaches the end", b.stop == 0 && b.end == 128, 1);
    expect_u("after: the same 10 packets", b.packets, 10);
    expect_u("after: no VMID-2 IB left", b.ibs, 0);
    expect_u("after: one NOP where the IB was", b.nops, 1);
    expect_u("frame RELEASE_MEMs at +0x3d and +0x45, as F1's", ring[(from + 0x3d) % kSize] == 0xC0064900u && ring[(from + 0x45) % kSize] == 0xC0064900u, 1);
    expect_u("after: RELEASE_MEM stamp untouched", ring[(from + 0x45) % kSize], 0xC0064900u);
    expect_u("after: the body dwords are kept (VA low)", ring[(from + 0x2e) % kSize], 0x00190000u);
    // COND_EXEC's region: 0x79 dwords from +5 cover +5..+0x7d, which still ends on a packet boundary after the rewrite.
    expect_u("COND_EXEC region end +0x7e is the SWITCH_BUFFER header", ring[(from + 5 + 0x79) % kSize], 0xC0008B00u);

    // ---- 0.0.279: the neuter at the source ----
    {
        uint32_t tmpl[N48_TMPL_DWORDS];
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) tmpl[k] = N48_TMPL_NOP;
        uint8_t info[0x200] = { 0 };
        n48_wr32(info, N48_SCB_FLAGS, 0x0u);
        n48_wr32(info, N48_SCB_VMID, 0x2u);
        n48_wr32(info, N48_SCB_COUNT, 3u);
        n48_wr32(info, N48_SCB_STAMP, 0x16u);
        const uint32_t lens[3] = { 3744u, 13888u, 1552u };
        for (uint32_t i = 0; i < 3; i++) {
            const uint32_t off = N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE * i;
            n48_wr32(info, off, lens[i]);
            n48_wr32(info, off + N48_SCB_ENTRY_VA, 0x00650000u + 0x10000u * i);
            n48_wr32(info, off + N48_SCB_ENTRY_VA + 4u, 0x4u);
        }
        uint32_t blk[N48_TMPL_DWORDS];
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = tmpl[k];
        n48_gfxsrc_model_commit(info, blk);
        expect_u("model control: 3 IB packets written at 0x2d/0x31/0x35", blk[0x2d] == 0xC0023F00u && blk[0x31] == 0xC0023F00u && blk[0x35] == 0xC0023F00u, 1);
        expect_u("model control: first IB ctl = F21's 0x02000ea0", blk[0x30], 0x02000ea0u);
        uint32_t n = 99;
        expect_u("shape: IB-list VMID-2 frame accepted", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_OK);
        expect_u("check: count 3", n, 3u);
        uint32_t saved[N48_SCB_MAX_IBS] = { 0 };
        n48_gfxsrc_zero(info, n, saved);
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = tmpl[k];
        n48_gfxsrc_model_commit(info, blk);
        uint32_t ibs = 0, nops = 0;
        for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) { if (blk[k] == 0xC0023F00u) ibs++; if (blk[k] == N48_TMPL_NOP) nops++; }
        expect_u("zeroed: the model emits NO IB packet", ibs, 0u);
        expect_u("zeroed: the block is exactly the template", nops, N48_TMPL_DWORDS);
        expect_u("zeroed: the VA dwords are untouched", n48_rd32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA), 0x00650000u);
        expect_u("zeroed: stamp untouched", n48_rd32(info, N48_SCB_STAMP), 0x16u);
        n48_gfxsrc_restore(info, n, saved);
        expect_u("restore: entry 1 length back", n48_rd32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE), 13888u);
        expect_u("restore: entry 2 length back", n48_rd32(info, N48_SCB_ENTRY0 + 2u * N48_SCB_ENTRY_SIZE), 1552u);
        n48_wr32(info, N48_SCB_FLAGS, 0x10u);
        expect_u("refuse: single-IB frame (flag 0x10)", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_NOT_LIST);
        n48_wr32(info, N48_SCB_FLAGS, 0x2u);
        expect_u("refuse: timestamp-only frame (flag 0x2)", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_NOT_LIST);
        n48_wr32(info, N48_SCB_FLAGS, 0x8u);
        expect_u("accept: flag 0x8 (PFP_SYNC_ME) is still an IB-list frame", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_OK);
        n48_wr32(info, N48_SCB_FLAGS, 0u);
        n48_wr32(info, N48_SCB_VMID, 0x0u);
        expect_u("0.0.362: the shape no longer looks at the VMID (VMID 0 is of the shape)", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_OK);
        expect_u("0.0.362: ... the frozen 0.0.361 check refused it as not VMID 2", frozen0361_check(info, tmpl, &n), (uint32_t)kOld361NotVmid2);
        uint32_t wy = 0;
        expect_u("0.0.362: ... and the owner route NEUTERS it (not WindowServer's)", n48_gfxsrc_route_owner(info, tmpl, 0u, &n, &wy), N48_ROUTE_NOT_WS);
        expect_u("0.0.362: ... with reason NOT_WS", wy, N48_SRC_NOT_WS);
        n48_wr32(info, N48_SCB_VMID, 0x2u);
        n48_wr32(info, N48_SCB_COUNT, 5u);
        expect_u("refuse: 5 IBs (template room is 4)", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_COUNT);
        n48_wr32(info, N48_SCB_COUNT, 0u);
        expect_u("refuse: 0 IBs", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_COUNT);
        n48_wr32(info, N48_SCB_COUNT, 1u);
        tmpl[0x3c] = 0xC0064900u;
        expect_u("refuse: template slot not padding", n48_gfxsrc_shape(info, tmpl, &n), N48_SRC_TEMPLATE);
        expect_u("refuse: null info", n48_gfxsrc_shape(nullptr, tmpl, &n), N48_SRC_ARG);
    }

    // ---- 0.0.358: THE VMID ESCAPE, CLOSED AT BOTH LAYERS --------------------------------------------------
    const int beforeVmid = gFail;
    const int vmidReal = vmid_checks(0);
    vmid_property();
    std::printf("\n== planted defects (the old VMID-2-only filters): each must be CAUGHT ==\n");
    int caught = 0;
    for (int m = 1; m <= 2; m++) {
        gQuietN = 1;
        const int f = vmid_checks(m);
        gQuietN = 0;
        if (f > 0) caught++;
        std::printf("  %-52s %s (%d check(s) failed)\n", m == 1 ? "the old ring walk (records VMID 2 only)" :
                    "the old source hook (passes every VMID but 2)", f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    (void)vmidReal; (void)beforeVmid;

    // ---- 0.0.362: THE ROUTE BY OWNER -------------------------------------------------------------------------
    std::printf("\n== the route by owner (0.0.362): hp3's world, and the hook's outcome against the frozen 0.0.361 ==\n");
    (void)owner_checks(0);
    owner_property();
    int ownCaught = 0;
    {
        gQuietN = 1;
        const int f = owner_checks(1);
        gQuietN = 0;
        if (f > 0) ownCaught++;
        std::printf("  %-60s %s (%d check(s) failed)\n", "planted: the frozen 0.0.361 route (VMID 2 is WindowServer's)",
                    f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }

    // ---- 0.0.359: THE COMMITTED FRAME'S EXEMPTION ----------------------------------------------------------
    std::printf("\n== the committed frame's exemption from the ring NOP (0.0.359) ==\n");
    (void)exempt_checks(EX_REAL);
    (void)exempt_property(EX_REAL, true);
    std::printf("\n== planted defects in the exemption: each must be CAUGHT ==\n");
    int exCaught = 0;
    for (int m = EX_NO_ARM; m < EX_MUTANTS; m++) {
        gQuietN = 1;
        const int f = exempt_checks(m) + exempt_property(m, false);
        gQuietN = 0;
        if (f > 0) exCaught++;
        std::printf("  %-60s %s (%d check(s) failed)\n", ex_name(m), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    const int planted = 2 + 1 + (EX_MUTANTS - 1), caughtAll = caught + ownCaught + exCaught;
    const int realFails = gFail - gMutFails;
    std::printf("\n%d/%d passed; %d planted defects, %d caught\n", gRun - gMutRuns - realFails, gRun - gMutRuns, planted,
                caughtAll);
    return (realFails || caughtAll != planted) ? 1 : 0;
}
