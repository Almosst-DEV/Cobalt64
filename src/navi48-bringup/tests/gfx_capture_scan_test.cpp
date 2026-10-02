// gfx_capture_scan_test.cpp — what the source capture reads for a GFX IB (0.0.282).
//     clang++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_capture_scan_test.cpp -o /tmp/gcaptest && /tmp/gcaptest
// The real head is wsneuter1's F2 IB0 hex[000..01f] (notes/logs/runs/wsneuter1/driverlog-stream.txt, SecurityAgent, VA 0x400190000):
// EVENT_WRITE, ACQUIRE_MEM, EVENT_WRITE, NOP, CONTEXT_CONTROL 80000002/80000002, LOAD_CONTEXT_REG 000cf3b8 00000004 0 0, SET_CONTEXT_REG.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstddef>
#include "gfx_capture_scan.h"
#include "fixture_f84_decide44.h"   // 0.0.457: decide44 F84's two real IB bodies (15520 + 7616 dwords, a two-head 7|5 frame)

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-72s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
}

static const uint64_t kStart = 0x400000000ull;

// =====================================================================================================================
// build 0.0.457 — SWITCH 54 OVER REAL TWO-HEAD BYTES. decide44 F84 (fixture_f84_decide44.h) is the same
// 15520|7616 shape as mibA1 F50/F75 and decide46 F48/F92/F108/F124, whose target-in-vram verdicts the armed MIB-A1 hit.
// Every check drives the kext's OWN functions (n48_gcap_tv_ib, n48_gcap_cbt_ib, n48_gcap_tv_resolve) over the frame's IBs
// in submission order, exactly as gfxsrc_decide_frame does, with a fixture page map standing in for gfxc_page.
// =====================================================================================================================
struct TvMap { uint64_t vram[16]; uint32_t nv; uint64_t host[16]; uint32_t nh; uint64_t fail[16]; uint32_t nf; uint32_t asked; };
static uint32_t tv_map_cb(void *ud, uint64_t va, uint32_t *isSys)
{
    TvMap *m = static_cast<TvMap *>(ud);
    m->asked++;
    *isSys = 0u;
    for (uint32_t q = 0; q < m->nf; q++) if (m->fail[q] == va) return 0u;
    for (uint32_t q = 0; q < m->nh; q++) if (m->host[q] == va) { *isSys = 1u; return 1u; }
    for (uint32_t q = 0; q < m->nv; q++) if (m->vram[q] == va) return 1u;
    return 0u;   // an address the map does not know does not resolve: the fail-closed direction
}
// The eight colour targets F84 draws into (read off its own SET_CONTEXT_REG CB0 writes,  item 1).
static const uint64_t kF84Tgt[8] = { 0x400800000ull, 0x401348000ull, 0x400708000ull, 0x4012a0000ull,
                                     0x400788000ull, 0x400034000ull, 0x4000ac000ull, 0x400720000ull };
static n48_gcap_item gTvItems[N48_GCAP_TV_ITEMS];
static n48_gcap_cbt gTvS;
// One frame through the kext's order: reset once per frame, the scan step per IB, then the per-draw targets resolved.
// Returns target_vram as the kext would leave it (the item loop's own per-target clauses are exercised by the caller where
// it matters). *storedSum is the items the item loop would examine.
static uint32_t tv_frame(uint32_t on, const uint32_t *const *ib, const uint32_t *len, uint32_t nib, TvMap *m, uint32_t drop,
                         uint32_t *whyOut, uint32_t *storedSum, uint32_t *vramSeenOut = nullptr)
{
    uint32_t refuse = 0u, why = 0u, st = 0u;
    if (on) n48_gcap_cbt_reset(&gTvS);
    for (uint32_t k = 0; k < nib; k++) {
        uint32_t total = 0u;
        st += n48_gcap_tv_ib(on, ib[k], len[k], kStart, gTvItems, &total, &refuse, &why, &gTvS);
    }
    uint32_t tv = refuse ? 1u : 0u;
    // build 0.0.472 item 6: vramSeenOut is OPTIONAL (nullptr by default) and n48_gcap_tv_resolve itself treats
    // a null vram_seen as "not interested" - most callers here do not need it.
    if (on && n48_gcap_tv_resolve(&gTvS, tv_map_cb, m, drop, 1u, &why, vramSeenOut)) tv = 1u;
    if (whyOut) *whyOut = why;
    if (storedSum) *storedSum = st;
    return tv;
}
static void tv_map_all(TvMap &m, int host)
{
    m = TvMap {};
    for (uint32_t q = 0; q < 8u; q++) { if (host) m.host[m.nh++] = kF84Tgt[q]; else m.vram[m.nv++] = kF84Tgt[q]; }
}
// The 0.0.456 clause, verbatim from 7b76f13's gfxsrc_decide_frame: n48_gcap_scan with a 64-item cap and `total > 64u`.
static uint32_t tv456(const uint32_t *d, uint32_t n)
{
    uint32_t total = 0u;
    (void)n48_gcap_scan(d, n, kStart, gTvItems, 64u, &total);
    return total > 64u ? 1u : 0u;
}

static void tv457_checks()
{
    std::printf("== 0.0.457 switch 54: the colour-target scan over every item and every draw (decide44 F84) ==\n");
    const uint32_t *ib[2] = { kF84Ib0, kF84Ib1 };
    const uint32_t len[2] = { 15520u, 7616u };
    TvMap m; uint32_t why = 0u, st = 0u;

    // 1. OFF IS 0.0.456: both IBs overflow the 64-item cap, so the frame is target-in-vram whatever its targets are
    //   , and exactly 64 items of each are examined.
    tv_map_all(m, 1);
    expect_u("F84 OFF: 0.0.456's clause refuses IB 0 (164 items > 64)", tv456(kF84Ib0, 15520u), 1u);
    expect_u("F84 OFF: 0.0.456's clause refuses IB 1 (108 items > 64)", tv456(kF84Ib1, 7616u), 1u);
    expect_u("F84 OFF: target_vram 1 even with EVERY target in host memory", tv_frame(0u, ib, len, 2u, &m, 1u, &why, &st), 1u);
    expect_u("F84 OFF: 64 + 64 items examined", st, 128u);
    expect_u("F84 OFF: `why` untouched", why, 0u);
    expect_u("F84 OFF: no page walked", m.asked, 0u);
    { uint32_t t = 0u, r = 0u, w = 0u;
      const uint32_t s0 = n48_gcap_tv_ib(0u, kF84Ib0, 15520u, kStart, gTvItems, &t, &r, &w, nullptr);
      expect_u("F84 OFF: the CB item (#152) is past what OFF examines", (s0 <= 152u) ? 1u : 0u, 1u); }

    // 2. ON, the targets as the runs had them (every one in VRAM, N1 ON): every item examined, every draw's target named
    //    and resolved, NOTHING refuses - the frame reaches the next rung.
    tv_map_all(m, 0);
    expect_u("F84 ON, VRAM targets, N1 ON: target_vram 0", tv_frame(1u, ib, len, 2u, &m, 1u, &why, &st), 0u);
    expect_u("F84 ON: why 0", why, 0u);
    expect_u("F84 ON: all 164 + 108 items examined", st, 272u);
    expect_u("F84 ON: 40 draws seen", gTvS.draws, 40u);
    expect_u("F84 ON: nothing unseen", gTvS.unseen, 0u);
    expect_u("F84 ON: 8 distinct colour targets", gTvS.n, 8u);
    { uint32_t hit = 0u; for (uint32_t q = 0; q < 8u; q++) for (uint32_t z = 0; z < gTvS.n; z++) if (gTvS.va[z] == kF84Tgt[q]) hit++;
      expect_u("F84 ON: they are exactly F84's eight CB0 targets", hit, 8u); }
    expect_u("F84 ON: each walked once", m.asked, 8u);
    // the older gap: 0x401348000 is drawn to mid-IB and 0.0.456's end-of-walk item never names it
    { uint32_t t = 0u; (void)n48_gcap_scan(kF84Ib0, 15520u, kStart, gTvItems, N48_GCAP_TV_ITEMS, &t);
      uint32_t named = 0u; for (uint32_t z = 0; z < t; z++) if (gTvItems[z].kind == N48_GCAP_CB && gTvItems[z].va == 0x401348000ull) named = 1u;
      expect_u("the end-of-walk scan never names mid-IB target 0x401348000 (why the per-draw pass exists)", named, 0u); }

    // 3. THE RULE IS KEPT. A genuine VRAM target refuses with N1 OFF; so does a planted one among host targets; so does a
    //    target that does not resolve.
    tv_map_all(m, 0);
    expect_u("F84 ON, VRAM targets, N1 OFF: target_vram 1", tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st), 1u);
    expect_u("... why VRAM", (why & N48_TV_WHY_VRAM) ? 1u : 0u, 1u);
    tv_map_all(m, 1);
    expect_u("F84 ON, every target host memory, N1 OFF: target_vram 0", tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st), 0u);
    tv_map_all(m, 1); m.host[3] = 0x1ull; m.vram[m.nv++] = 0x4012a0000ull;   // PLANTED: one mid-IB target in VRAM
    expect_u("F84 ON, one planted VRAM target (0x4012a0000), N1 OFF: target_vram 1", tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st), 1u);
    expect_u("... why VRAM", (why & N48_TV_WHY_VRAM) ? 1u : 0u, 1u);
    tv_map_all(m, 0); m.fail[m.nf++] = 0x400708000ull; m.vram[2] = 0x1ull;   // PLANTED: one target does not resolve
    expect_u("F84 ON, one unresolvable target, N1 ON: target_vram 1", tv_frame(1u, ib, len, 2u, &m, 1u, &why, &st), 1u);
    expect_u("... why UNRES", (why & N48_TV_WHY_UNRES) ? 1u : 0u, 1u);

    // 4. AN ITEM BEYOND THE NEW BOUND still refuses: F84's IB 0 four times over (656 items > 512).
    {
        std::vector<uint32_t> big;
        for (int r = 0; r < 4; r++) big.insert(big.end(), kF84Ib0, kF84Ib0 + 15520u);
        uint32_t t = 0u; (void)n48_gcap_scan(big.data(), (uint32_t)big.size(), kStart, gTvItems, N48_GCAP_TV_ITEMS, &t);
        expect_u("IB 0 x4: more than 512 items", t > N48_GCAP_TV_ITEMS ? 1u : 0u, 1u);
        const uint32_t *bi[1] = { big.data() }; const uint32_t bl[1] = { (uint32_t)big.size() };
        tv_map_all(m, 1);
        expect_u("IB 0 x4 ON, every target host: target_vram 1 (an item past the 512th was never examined)",
                 tv_frame(1u, bi, bl, 1u, &m, 0u, &why, &st), 1u);
        expect_u("... why OVER", (why & N48_TV_WHY_OVER) ? 1u : 0u, 1u);
        expect_u("... and exactly 512 items examined", st, N48_GCAP_TV_ITEMS);
    }
    // 5. A SCAN THAT STOPS SHORT refuses: IB 1 cut one dword short (its last packet runs past the end).
    {
        const uint32_t *bi[2] = { kF84Ib0, kF84Ib1 }; const uint32_t bl[2] = { 15520u, 7615u };
        tv_map_all(m, 1);
        expect_u("F84 ON, IB 1 cut short: target_vram 1", tv_frame(1u, bi, bl, 2u, &m, 1u, &why, &st), 1u);
        expect_u("... why WALK", (why & N48_TV_WHY_WALK) ? 1u : 0u, 1u);
    }
    // 6. WHAT THE PER-DRAW PASS CANNOT SEE refuses: a nested IB, a LOAD_CONTEXT_REG over CB0's base, a draw whose slot this
    //    frame never wrote. Each is appended to (or run instead of) the real bytes.
    {
        const uint32_t nest[4] = { 0xc0023f00u, 0x00001000u, 0x00000004u, 0x00000010u };        // INDIRECT_BUFFER
        const uint32_t load[6] = { 0xc0046100u, 0x00001000u, 0x00000004u, 0x00000318u, 0x00000001u, 0x0u };  // LOAD_CONTEXT_REG CB0_BASE
        const uint32_t loadOther[6] = { 0xc0046100u, 0x00001000u, 0x00000004u, 0x00000200u, 0x00000004u, 0x0u }; // regs 0x200..0x203
        const uint32_t drawOnly[4] = { 0xc0022d00u, 0x00000003u, 0x00000002u, 0x0u };           // DRAW_INDEX_AUTO, nothing set
        // build 0.0.472 item 5 (F1) — WRITE_DATA/COPY_DATA, register-destination form, naming CB_COLOR0_BASE
        // (0xa318). Same decode as xlat12_ib.c's own operand_ok/reg_identical rung: DST_SEL (control bits [11:8]).
        const uint32_t wdReg[5]  = { 0xc0033700u, 0x00000000u, 0x0000a318u, 0x00000000u, 0xdeadbeefu };  // WRITE_DATA -> CB0_BASE (reg dest)
        const uint32_t wdMem[5]  = { 0xc0033700u, 0x00000100u, 0x00001000u, 0x00000004u, 0xdeadbeefu };  // WRITE_DATA -> memory (DST_SEL 1)
        const uint32_t cdReg[5]  = { 0xc0034000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0000a318u };  // COPY_DATA -> CB0_BASE (reg dest)
        struct { const uint32_t *p; uint32_t n; uint32_t wantUnseen; const char *what; } tail[7] = {
            { nest, 4u, 1u, "a nested INDIRECT_BUFFER after IB 1" },
            { load, 6u, 1u, "a LOAD_CONTEXT_REG over CB0_BASE after IB 1" },
            { loadOther, 6u, 0u, "a LOAD_CONTEXT_REG over non-CB registers (control: NOT unseen)" },
            { drawOnly, 4u, 0u, "a draw after IB 1 with the frame's own CB state (control: NOT unseen)" },
            { wdReg, 5u, 1u, "a register-destination WRITE_DATA to CB0_BASE after IB 1" },
            { wdMem, 5u, 0u, "a memory-destination WRITE_DATA after IB 1 (control: NOT unseen)" },
            { cdReg, 5u, 1u, "a register-destination COPY_DATA to CB0_BASE after IB 1" } };
        for (int t = 0; t < 7; t++) {
            std::vector<uint32_t> ib1(kF84Ib1, kF84Ib1 + 7616u);
            ib1.insert(ib1.end(), tail[t].p, tail[t].p + tail[t].n);
            const uint32_t *bi[2] = { kF84Ib0, ib1.data() }; const uint32_t bl[2] = { 15520u, (uint32_t)ib1.size() };
            tv_map_all(m, 0);
            const uint32_t tv = tv_frame(1u, bi, bl, 2u, &m, 1u, &why, &st);
            char lbl[160]; std::snprintf(lbl, sizeof(lbl), "F84 ON + %s: target_vram %u", tail[t].what, tail[t].wantUnseen);
            expect_u(lbl, tv, tail[t].wantUnseen);
            std::snprintf(lbl, sizeof(lbl), "... why UNSEEN %u", tail[t].wantUnseen);
            expect_u(lbl, (why & N48_TV_WHY_UNSEEN) ? 1u : 0u, tail[t].wantUnseen);
            if (tail[t].p == wdReg || tail[t].p == cdReg) {
                std::snprintf(lbl, sizeof(lbl), "... unseen_why WRITE (%s)", tail[t].what);
                expect_u(lbl, (gTvS.unseen_why & N48_CBT_U_WRITE) ? 1u : 0u, 1u);
            }
        }
        const uint32_t *bi[1] = { drawOnly }; const uint32_t bl[1] = { 4u };
        tv_map_all(m, 0);
        expect_u("a frame whose first packet is a draw (its targets inherited from before the frame): target_vram 1",
                 tv_frame(1u, bi, bl, 1u, &m, 1u, &why, &st), 1u);
        expect_u("... why UNSEEN (inherited)", (why & N48_TV_WHY_UNSEEN) ? 1u : 0u, 1u);
        expect_u("... unseen_why INHERIT", (gTvS.unseen_why & N48_CBT_U_INHERIT) ? 1u : 0u, 1u);
    }
    // 7. THE TABLE: more distinct targets than N48_GCAP_CBT_MAX refuses (33 draws, each into its own CB0).
    {
        std::vector<uint32_t> d;
        const uint32_t pre[6] = { 0xc0016900u, 0x0000008eu, 0x0000000fu, 0xc0016900u, 0x00000390u, 0x0u };   // mask 0xf, CB0_EXT 0
        d.insert(d.end(), pre, pre + 6);
        for (uint32_t q = 0; q < N48_GCAP_CBT_MAX + 1u; q++) {
            const uint32_t w[7] = { 0xc0016900u, 0x00000318u, 0x04100000u + q * 0x10u, 0xc0022d00u, 3u, 2u, 0u };
            d.insert(d.end(), w, w + 7);
        }
        const uint32_t *bi[1] = { d.data() }; const uint32_t bl[1] = { (uint32_t)d.size() };
        m = TvMap {};
        for (uint32_t q = 0; q < 16u; q++) m.host[m.nh++] = 0x410000000ull + (uint64_t)q * 0x1000ull;
        expect_u("33 distinct targets ON: target_vram 1", tv_frame(1u, bi, bl, 1u, &m, 1u, &why, &st), 1u);
        expect_u("... why CBOVER", (why & N48_TV_WHY_CBOVER) ? 1u : 0u, 1u);
    }
}

// =====================================================================================================================
// build 0.0.472 item 6 (F4, review of 0.0.457) — n48_gcap_tv_resolve's `vram_seen` OUT-PARAM COUNTS EVERY
// PER-DRAW TARGET THAT RESOLVES INTO VRAM, WHETHER OR NOT N1 (`drop`) EXCUSES IT. Through this build task the ONLY
// instrument was `*why`'s N48_TV_WHY_VRAM bit, which `drop` gates (`!drop` in the source) - so with N1 ON the
// tvscan457 report line read "VRAM 0" even on a boot where every one of F84's eight targets really was in VRAM,
// indistinguishable from a boot with no VRAM targets at all. These checks prove the NEW counter is unconditional
// while the REFUSAL (the return value / N48_TV_WHY_VRAM bit) is UNCHANGED - still `!drop` only.
// =====================================================================================================================
static void vram_seen_checks()
{
    std::printf("== 0.0.472 item 6: VRAM targets seen, counted whether or not N1 excuses them ==\n");
    const uint32_t *ib[2] = { kF84Ib0, kF84Ib1 }; const uint32_t len[2] = { 15520u, 7616u };
    TvMap m; tv_map_all(m, 0);   // every one of F84's 8 targets is in VRAM
    uint32_t why = 0u, st = 0u, vramSeen = 0u;

    // N1 OFF (drop 0): refuses, AND counts 8.
    why = 0u; st = 0u; vramSeen = 0u;
    expect_u("N1 OFF: target_vram 1 (the refusal - UNCHANGED)", tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st, &vramSeen), 1u);
    expect_u("N1 OFF: why has VRAM", (why & N48_TV_WHY_VRAM) ? 1u : 0u, 1u);
    expect_u("N1 OFF: vram_seen counts all 8", vramSeen, 8u);

    // N1 ON (drop 1): does NOT refuse (the decision is unchanged), but STILL counts 8 - the whole point of item 6.
    why = 0u; st = 0u; vramSeen = 0u;
    expect_u("N1 ON: target_vram 0 (excused - the refusal decision is UNCHANGED)",
             tv_frame(1u, ib, len, 2u, &m, 1u, &why, &st, &vramSeen), 0u);
    expect_u("N1 ON: why does NOT have VRAM (the refusal bit is UNCHANGED - still !drop only)",
             (why & N48_TV_WHY_VRAM) ? 1u : 0u, 0u);
    expect_u("N1 ON: vram_seen STILL counts all 8 (build 0.0.472 item 6 - this is the fix)", vramSeen, 8u);

    // NON-VACUITY: a caller that passes nullptr for vram_seen (every OTHER call site in this file) does not crash.
    why = 0u; st = 0u;
    expect_u("nullptr vram_seen: does not crash, and the refusal is unaffected",
             tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st), 1u);

    // Every target host memory (none in VRAM): vram_seen is 0 under EITHER N1 setting.
    tv_map_all(m, 1);
    why = 0u; st = 0u; vramSeen = 0u;
    expect_u("every target host, N1 OFF: not refused", tv_frame(1u, ib, len, 2u, &m, 0u, &why, &st, &vramSeen), 0u);
    expect_u("every target host, N1 OFF: vram_seen 0", vramSeen, 0u);
}

// =====================================================================================================================
// build 0.0.472 item 3 — n48_gcap_cbt_reset ZEROES THE WHOLE PER-DRAW ACCUMULATOR. Switch 54's per-frame scan
// (gfxsrc_decide_frame, AppleHardwareHook.cpp) calls this on the file-scope static `gTvCbt` ONCE per judged frame,
// BEFORE that frame's own IB loop fills it (CONFIRMED: `if (tvOn) n48_gcap_cbt_reset(&gTvCbt);` precedes the IB loop
// that calls n48_gcap_tv_ib - R5's own reachability pin in gfx_commit_test.cpp already proves the ORDER; this proves
// what the call itself DOES). Without this, a continuous arm's second and later judged frames could inherit the
// PREVIOUS frame's colour targets (base/ext/mask/draws/unseen/va[]) - a stale target surviving into a frame that never
// named it would be a FAIL-OPEN (a frame judged clean because an old refusal-worthy target was silently dropped, or
// judged on a target it never drew into). `gTvFrame`/`gTvS`, the other two statics switch 54 owns, are NOT judgment
// inputs: `gTvFrame` is overwritten every frame 54 runs and every read of it is gated on a frame-number match
// (`tvFresh`, AppleHardwareHook.cpp) before being used in a report line; `gTvS` is a cumulative per-boot census
// (frames/refused/why[] totals) read only by the tvscan457 report line, never fed back into a verdict. Neither can
// leak staleness into a later frame's judgement, so only gTvCbt's reset matters here.
// =====================================================================================================================
static void cbt_reset_checks()
{
    std::printf("== 0.0.472 item 3: n48_gcap_cbt_reset clears every field ==\n");
    n48_gcap_cbt s {};
    // Poison every field the struct owns, exactly as a frame that named 8 slots and N48_GCAP_CBT_MAX distinct VAs
    // would leave it.
    for (uint32_t c = 0; c < 8u; c++) { s.base[c] = 0x11110000u + c; s.ext[c] = 0x22220000u + c; }
    s.written = 0xffu; s.ext_written = 0xffu; s.mask = 0xdeadu; s.mask_written = 1u;
    s.draws = 77u; s.unseen = 9u; s.unseen_why = N48_CBT_U_NESTED | N48_CBT_U_LOAD | N48_CBT_U_INHERIT | N48_CBT_U_WRITE;
    s.n = N48_GCAP_CBT_MAX;
    for (uint32_t q = 0; q < N48_GCAP_CBT_MAX; q++) s.va[q] = 0x400000000ull + (uint64_t)q * 0x1000ull;

    n48_gcap_cbt_reset(&s);

    uint64_t badBase = 0, badExt = 0, badVa = 0;
    for (uint32_t c = 0; c < 8u; c++) { if (s.base[c]) badBase++; if (s.ext[c]) badExt++; }
    for (uint32_t q = 0; q < N48_GCAP_CBT_MAX; q++) if (s.va[q]) badVa++;
    expect_u("reset: every base[] dword is 0", badBase, 0u);
    expect_u("reset: every ext[] dword is 0", badExt, 0u);
    expect_u("reset: every va[] entry is 0", badVa, 0u);
    expect_u("reset: written == 0", s.written, 0u);
    expect_u("reset: ext_written == 0", s.ext_written, 0u);
    expect_u("reset: mask == 0", s.mask, 0u);
    expect_u("reset: mask_written == 0", s.mask_written, 0u);
    expect_u("reset: draws == 0", s.draws, 0u);
    expect_u("reset: unseen == 0", s.unseen, 0u);
    expect_u("reset: unseen_why == 0", s.unseen_why, 0u);
    expect_u("reset: n == 0", s.n, 0u);
    // NON-VACUITY: reset(nullptr) must not crash (the caller's `if (tvOn) n48_gcap_cbt_reset(&gTvCbt);` never passes
    // null, but the function's own guard is part of its contract).
    n48_gcap_cbt_reset(nullptr);
    expect_u("reset: null pointer is a no-op, not a crash (reached this line)", 1u, 1u);

    // NON-VACUITY THE OTHER WAY: two consecutive tv_frame() calls over the SAME static gTvS (as gfxsrc_decide_frame's
    // gTvCbt is, across frames) do not let frame 2 inherit frame 1's targets. Frame 1 draws into CB0_BASE = 0xa318
    // (via kF84Ib0/kF84Ib1's own SET_CONTEXT_REG writes, already proven to name 0x400800000 first); frame 2 is a
    // single SET_CONTEXT_REG + draw into a DIFFERENT, deliberately-planted target. If reset did not run at the top of
    // frame 2's tv_frame() call, frame 2's va[] would still carry frame 1's 15+ targets alongside its own one.
    uint32_t why2 = 0u, st2 = 0u;
    const uint32_t *ib1[2] = { kF84Ib0, kF84Ib1 }; const uint32_t bl1[2] = { 15520u, 7616u };
    TvMap m1; tv_map_all(m1, 1);
    (void)tv_frame(1u, ib1, bl1, 2u, &m1, 1u, &why2, &st2);
    expect_u("cross-frame: frame 1 names its own 8 targets", gTvS.n, 8u);
    // Frame 2: CB0_BASE_EXT = 0 (so the draw's slot 0 counts as "seen" - EXT must be written too, exactly as section
    // 7's `pre[]` sets it once before its own per-draw loop), then CB0_BASE = 0x04999000 (a VA no F84 target uses),
    // then one draw. n48_gcap_cbt_va(ext=0, base=0x04999000) = (0x04999000 << 8) = 0x0499900000.
    const uint32_t plantedFrame2[10] = {
        0xc0016900u, 0x00000390u, 0x00000000u,             // SET_CONTEXT_REG CB0_BASE_EXT = 0
        0xc0016900u, 0x00000318u, 0x04999000u,             // SET_CONTEXT_REG CB0_BASE = 0x04999000
        0xc0022d00u, 0x00000003u, 0x00000002u, 0x0u };     // DRAW_INDEX_AUTO (drawOnly's own 4-dword shape)
    const uint32_t *ib2[1] = { plantedFrame2 }; const uint32_t bl2[1] = { 10u };
    TvMap m2 {}; m2.host[m2.nh++] = 0x0499900000ull;   // zero-initialised: TvMap has no default member initialisers
    (void)tv_frame(1u, ib2, bl2, 1u, &m2, 1u, &why2, &st2);
    expect_u("cross-frame: frame 2's OWN target replaces frame 1's, not added to them (n == 1)", gTvS.n, 1u);
    expect_u("cross-frame: frame 2's one target is its own VA, not one of frame 1's",
             gTvS.va[0], 0x0499900000ull);
}

int main()
{
    // 1. The real F2 head (32 dwords): the walk reaches the SET_CONTEXT_REG at +0x1c and stops only at the cut; one LOAD item.
    static const uint32_t f2[32] = {
        0xc0004600, 0x00000016, 0xc0065800, 0x86287fc3, 0xffffffff, 0x000000ff, 0x00000000, 0x00000000,
        0x0000000a, 0x0001c3f1, 0xc0004600, 0x0000000e, 0xc0061000, 0xc0051000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xc0012800, 0x80000002, 0x80000002, 0xc0036100,
        0x000cf3b8, 0x00000004, 0x00000000, 0x00000000, 0xc0026900, 0x00000323, 0x00000000, 0x00000000 };
    {
        n48_gcap_item it[8]; uint32_t total = 99;
        const uint32_t walked = n48_gcap_scan(f2, 32, kStart, it, 8, &total);
        expect_u("F2 head: walked through the SET_CONTEXT_REG at +0x1c (32)", walked, 32);
        expect_u("F2 head: one item", total, 1);
        expect_u("F2 head: it is the LOAD_CONTEXT_REG image", it[0].kind, N48_GCAP_LOAD);
        expect_u("F2 head: at VA 0x4000cf3b8", it[0].va, 0x4000cf3b8ull);
        expect_u("F2 head: count 0 -> 256 dwords", it[0].want, 256);
        expect_u("F2 head: packet at dword 0x17", it[0].dword, 0x17);
        expect_u("F2 head: opcode carried in index", it[0].index, 0x61);
    }
    // 2. Programs, user data, index buffer, render target, a second copy of a program, LOAD with explicit ranges.
    std::vector<uint32_t> ib = {
        0xC0027600, 0x00000008, 0x04000207, 0x00000000,                 // PS LO/HI -> VA 0x400020700
        0xC0027600, 0x00000048, 0x04000289, 0x00000000,                 // VS LO/HI -> VA 0x400028900
        0xC0037600, 0x0000000c, 0x00240000, 0x00000004, 0x00000007,     // PS user data 0:1 = VA 0x400240000, then 7
        0xC0027600, 0x00000008, 0x04000207, 0x00000000,                 // PS program again: no second item
        0xC0047600, 0x0000004c, 0x00001000, 0x00000004, 0x00000004, 0x00000000,  // VS_0..3: (0x1000,4) pair consumed, (4,0) no
        0xC0027600, 0x00000050, 0x00001003, 0x00000004,                 // lo not 4-aligned: no
        0xC0036100, 0x000cf3b8, 0x00000004, 0x00000010, 0x00000020,     // LOAD_CONTEXT_REG offset 0x10 count 0x20 -> 0x30 dwords
        0x80000000,                                                     // type-2 filler
        0xC0026900, 0x00000318, 0x04003400, 0x00000000,                 // CB_COLOR0_BASE (0xa318) = VA >> 8, then 0xa319
        0xC0016900, 0x00000390, 0x00000000,                             // CB_COLOR0_BASE_EXT = VA >> 40 = 0
        0xC0016900, 0x00000013, 0x00005000,                             // DB_STENCIL_READ_BASE (0xa013) = 0x00005000 (VA >> 8)
        0xC0042700, 0x00000100, 0x00100000, 0x00000004, 0x00000006, 0x00000000,   // DRAW_INDEX_2 (0x27) -> index buffer 0x400100000
        0xC0022600, 0x00200001, 0x00000004, 0x00000000,                 // INDEX_BASE (low bit masked) -> 0x400200000
        0xC0055000, 0x60200001, 0x00008e00, 0x00000004, 0x00000000, 0x00000000, 0x00000040,   // DMA_DATA src 0x400008e00
        0xC0023F00, 0x00300000, 0x00000004, 0x02000080,                 // nested IB 0x400300000 len 0x80
        0xC0029B00, 0x00000008, 0x04000300, 0x00000000,                 // SET_SH_REG_INDEX PS program 0x400030000
    };
    {
        n48_gcap_item it[32]; uint32_t total = 0;
        const uint32_t walked = n48_gcap_scan(ib.data(), (uint32_t)ib.size(), kStart, it, 32, &total);
        expect_u("synthetic: walked the whole IB", walked, ib.size());
        expect_u("synthetic: 12 items", total, 12);
        struct W { uint32_t kind, index; uint64_t va; uint32_t want; } want[12] = {
            { N48_GCAP_PGM + 0, 0, 0x400020700ull, 1024 }, { N48_GCAP_PGM + 1, 1, 0x400028900ull, 1024 },
            { N48_GCAP_USER, 0x2c0c, 0x400240000ull, 256 }, { N48_GCAP_USER, 0x2c4c, 0x400001000ull, 256 },
            { N48_GCAP_LOAD, 0x61, 0x4000cf3b8ull, 0x30 }, { N48_GCAP_INDEX, 0x27, 0x400100000ull, 64 },
            { N48_GCAP_INDEX, 0x26, 0x400200000ull, 64 }, { N48_GCAP_DMA, 0x50, 0x400008e00ull, 64 },
            { N48_GCAP_NESTED, 0x3f, 0x400300000ull, 0x80 }, { N48_GCAP_PGM + 0, 0, 0x400030000ull, 1024 },
            { N48_GCAP_CB, 0, 0x400340000ull, 16 }, { N48_GCAP_DB, 1, 0x500000ull, 16 } };
        for (int k = 0; k < 12; k++) {
            char w[96];
            std::snprintf(w, sizeof w, "synthetic item %d kind", k);   expect_u(w, it[k].kind, want[k].kind);
            std::snprintf(w, sizeof w, "synthetic item %d va", k);     expect_u(w, it[k].va, want[k].va);
            std::snprintf(w, sizeof w, "synthetic item %d index", k);  expect_u(w, it[k].index, want[k].index);
            std::snprintf(w, sizeof w, "synthetic item %d dwords", k); expect_u(w, it[k].want, want[k].want);
        }
        expect_u("synthetic: PS program dword position (LO value at 2)", it[0].dword, 2);
        expect_u("synthetic: user data pair position (lo at 10)", it[2].dword, 10);
        // DB_STENCIL_READ_BASE with no _HI gives VA 0x500000, below startVa: the scan keeps a target, the kext's reader refuses it.
    }
    // 3. max truncation still counts; a stop header ends the walk; a packet running past n ends the walk.
    {
        n48_gcap_item it[2]; uint32_t total = 0;
        (void)n48_gcap_scan(ib.data(), (uint32_t)ib.size(), kStart, it, 2, &total);
        expect_u("max 2: total still 12", total, 12);
        expect_u("max 2: stored item 1 is the VS program", it[1].va, 0x400028900ull);
        static const uint32_t stop[] = { 0xC0027600, 0x00000008, 0x04000207, 0x00000000, 0x12345678, 0xC0027600, 0x00000048, 0x04000289, 0 };
        uint32_t t2 = 0;
        expect_u("stop header: walked 4", n48_gcap_scan(stop, 9, kStart, it, 2, &t2), 4);
        expect_u("stop header: 1 item (nothing after the stop)", t2, 1);
        static const uint32_t runs[] = { 0xC0027600, 0x00000008, 0x04000207, 0x00000000, 0xC0107600, 0x00000048 };
        uint32_t t3 = 0;
        expect_u("overlong packet: walked 4", n48_gcap_scan(runs, 6, kStart, it, 2, &t3), 4);
    }
    // 3a. The retracted opcode: 0x36 is DRAW_INDEX_MULTI_ELEMENT, not DRAW_INDEX_2 (Mesa sid.h:63 PKT3_DRAW_INDEX_2 0x27); no item.
    {
        static const uint32_t m36[] = { 0xC0033600, 0x00000100, 0x00100000, 0x00000004, 0x00000006 };
        n48_gcap_item it[2]; uint32_t t = 0;
        (void)n48_gcap_scan(m36, 5, kStart, it, 2, &t);
        expect_u("opcode 0x36 names no index buffer", t, 0);
    }
    // 3b. PGM_HI carries VA bits 40+ (LO = VA >> 8 covers bits 8..39); a lo with only bit 1 set is not a VA.
    {
        static const uint32_t hi[] = { 0xC0027600, 0x00000048, 0x00000123, 0x00000001, 0xC0027600, 0x0000000c, 0x00001002, 0x00000004 };
        n48_gcap_item it[4]; uint32_t t = 0;
        (void)n48_gcap_scan(hi, 8, kStart, it, 4, &t);
        expect_u("PGM_HI 1: one item (the program; lo 0x1002 refused)", t, 1);
        expect_u("PGM_HI 1: VA = 1 << 40 | 0x123 << 8", it[0].va, (1ull << 40) | (0x123ull << 8));
    }
    // 4. Pairs one level down: a class-19 table with the image table at +0 and the sampler table at +0x10 (dword 4).
    {
        static const uint32_t table[8] = { 0x00410000, 0x00000004, 0x00000000, 0x00000000, 0x00420000, 0x00000004, 0x0000000f, 0x00000000 };
        uint64_t va[4]; uint32_t pos[4];
        const uint32_t n = n48_gcap_pairs(table, 8, kStart, va, pos, 4);
        expect_u("pairs: 2 found", n, 2);
        expect_u("pairs: image table VA", va[0], 0x400410000ull);
        expect_u("pairs: image table at dword 0", pos[0], 0);
        expect_u("pairs: sampler table VA", va[1], 0x400420000ull);
        expect_u("pairs: sampler table at dword 4 (+0x10)", pos[1], 4);
        static const uint32_t negative[6] = { 0x00001000, 0x00000000, 0x00000003, 0x00000004, 0x00001000, 0x00000100 };
        expect_u("pairs negative control: hi 0, lo misaligned, hi > 0xff -> 0", n48_gcap_pairs(negative, 6, kStart, va, pos, 4), 0);
    }
    // 4a. build 0.0.447 (MIB-A1-PATH.md Q3 / ) — n48_gcap_user_indices over a SYNTHETIC
    // draw shaped exactly like U/Y (table s0:1, a raw buffer pointer at s2:3, texture indices at s8/s10, sampler
    // at s12), carrying the TWO indices found uncaptured by the fixed-64-dword window: U's second texture
    // (46, byte 1472) and Y's (21, byte 672).
    {
        static const uint32_t u[16] = {
            0x00000000, 0x00000004,          /* PS_0:1 the table pointer itself (skipped: this scan starts at +2) */
            0x00000110, 0x0000000c,          /* PS_2:3 a raw buffer pointer (VA-shaped: hi<=0xff, lo 4-aligned, >= startVa) - NOT an index */
            0x00000000, 0x00000000,          /* PS_4:5 unused */
            0x00000000, 0x00000000,          /* PS_6:7 unused */
            46u, 0x00000000,                 /* PS_8:9 texture 0 index = 46 (U's own second texture,) */
            21u, 0x00000000,                 /* PS_10:11 texture 1 index = 21 (Y's own second texture,) */
            8u, 0x00000000,                  /* PS_12:13 sampler index = 8 (outside n48_gcap_user_indices' job - it names IMAGE indices only, but is harmless to scan over) */
            0x00000000, 0x00000000 };        /* PS_14:15 padding, past the 16-dword window from tableDword=0 */
        uint32_t idx[8];
        const uint32_t ni = n48_gcap_user_indices(u, 16, kStart, 0u, idx, 8u);
        expect_u("user_indices: three nonzero words found (46, 21, 8)", ni, 3);
        int found46 = 0, found21 = 0; for (uint32_t z = 0; z < ni; z++) { if (idx[z] == 46u) found46 = 1; if (idx[z] == 21u) found21 = 1; }
        expect_u("user_indices: 46 (U's second texture) is among them", (uint64_t)found46, 1);
        expect_u("user_indices: 21 (Y's second texture) is among them", (uint64_t)found21, 1);
        // PLANTED BREAK: the buffer pointer at PS_2:3 must be SKIPPED as a pair, not misread as index 0x110.
        int foundBufPtr = 0; for (uint32_t z = 0; z < ni; z++) if (idx[z] == 0x00000110u) foundBufPtr = 1;
        expect_u("BREAK-check: the VA-shaped buffer pointer's low word is NOT read as an index", (uint64_t)foundBufPtr, 0);
        // PLANTED BREAK: the cap. Only N48_GCAP_USER_IDX_MAX (4) are ever STORED, even when more are found.
        uint32_t idxSmall[2];
        const uint32_t niCapped = n48_gcap_user_indices(u, 16, kStart, 0u, idxSmall, 2u);
        expect_u("BREAK-check: found count is uncapped (3) even when storage is capped at 2", niCapped, 3);
        // PLANTED BREAK: the window. A fourth index placed WELL past N48_GCAP_USER_IDX_WINDOW from the table must
        // NOT be found - a caller that forgot to bound the scan would read arbitrary later draws' own data.
        static uint32_t wide[40] = { 0 };
        wide[0] = 0x00000000; wide[1] = 0x00000004;   /* the table pointer */
        wide[30] = 99u;                                /* far past tableDword(0) + 2 + 16 */
        uint32_t idxW[8];
        const uint32_t niW = n48_gcap_user_indices(wide, 40, kStart, 0u, idxW, 8u);
        expect_u("BREAK-check: an index far past the window is NOT found (window bound)", niW, 0);
        // negative control: an all-zero body names nothing (0 is padding, never a valid heap index).
        static const uint32_t zeros[16] = { 0 };
        expect_u("user_indices negative control: an all-zero body finds nothing", n48_gcap_user_indices(zeros, 16, kStart, 0u, idx, 8u), 0);
    }
    // 5. The record layouts tools/m4-xlat/capdecode.py assumes.
    expect_u("sizeof arm_hdr 16", sizeof(n48_gcap_arm_hdr), 16);
    expect_u("sizeof arm_row 56", sizeof(n48_gcap_arm_row), 56);
    expect_u("arm_row.root at 48", offsetof(n48_gcap_arm_row, root), 48);
    expect_u("sizeof frame_hdr 128", sizeof(n48_gcap_frame_hdr), 128);
    expect_u("frame_hdr.ctx2Root at 24", offsetof(n48_gcap_frame_hdr, ctx2Root), 24);
    expect_u("frame_hdr.ownerPid at 48", offsetof(n48_gcap_frame_hdr, ownerPid), 48);
    expect_u("frame_hdr.ownerName at 56", offsetof(n48_gcap_frame_hdr, ownerName), 56);
    expect_u("frame_hdr.threadPid at 76", offsetof(n48_gcap_frame_hdr, threadPid), 76);
    expect_u("frame_hdr.threadName at 80", offsetof(n48_gcap_frame_hdr, threadName), 80);
    expect_u("frame_hdr.rptr at 100", offsetof(n48_gcap_frame_hdr, rptr), 100);
    expect_u("frame_hdr.readerWhy at 120", offsetof(n48_gcap_frame_hdr, readerWhy), 120);
    expect_u("sizeof probe 32", sizeof(n48_gcap_probe), 32);
    expect_u("sizeof ib_hdr 48", sizeof(n48_gcap_ib_hdr), 48);
    expect_u("ib_hdr.root at 40", offsetof(n48_gcap_ib_hdr, root), 40);
    expect_u("sizeof region_hdr 72", sizeof(n48_gcap_region_hdr), 72);
    expect_u("region_hdr.firstPage at 48", offsetof(n48_gcap_region_hdr, firstPage), 48);
    expect_u("region_hdr.key at 64", offsetof(n48_gcap_region_hdr, key), 64);
    // 6. build 0.0.450 item 4 (THE CENSUS CAPTURE): n48_gcap_census_ib, PS user data tracked per slot ACROSS
    // two IBs of one synthetic frame - IB1's table draw sets s0:1 and s2 then draws; IB2, with no table write of
    // its own, still finds s0:1/s2 (carried) plus its own new s4, at ITS OWN draw. `sh` builds a SET_SH_REG /
    // SET_SH_REG_INDEX body writing exactly one slot at PS user-data offset `off` (0x0c + 4*slot) to `val`.
    {
        auto sh = [](uint32_t off, uint32_t val) -> std::vector<uint32_t> {
            return { 0xC0017600u, off, val };   // op 0x76, cnt3=1 (nb=2: offset, value)
        };
        auto draw = [](uint32_t op) -> std::vector<uint32_t> { return { 0xC0000000u | (op << 8), 0u }; };   // cnt3=0 (nb=1)
        std::vector<uint32_t> ib1;
        ib1.push_back(0xFFFF1000u);                                    // the one-dword filler, BEFORE anything else
        { auto p = sh(0x0c, 0x00300000u); ib1.insert(ib1.end(), p.begin(), p.end()); }   // PS_0 = 0x300000 (table lo)
        { auto p = sh(0x0d, 0x00000004u); ib1.insert(ib1.end(), p.begin(), p.end()); }   // PS_1 = 4          (table hi)
        { auto p = sh(0x0e, 10u);         ib1.insert(ib1.end(), p.begin(), p.end()); }   // PS_2 = 10 (SGPR2, in-bound)
        const uint32_t ib1DrawAt = (uint32_t)ib1.size();
        { auto p = draw(0x2Du);           ib1.insert(ib1.end(), p.begin(), p.end()); }   // DRAW_INDEX_AUTO
        uint32_t ps[16] = { 0 };
        n48_gcap_census_row rows1[8]; uint32_t t1 = 0;
        const uint32_t walked1 = n48_gcap_census_ib(ib1.data(), (uint32_t)ib1.size(), kStart, ps, rows1, 8, &t1);
        expect_u("census IB1: walked the whole IB (filler consumed as one dword)", walked1, (uint32_t)ib1.size());
        expect_u("census IB1: s0:1 is VA-shaped -> table VA 0x400300000", ps[1] == 4 && ps[0] == 0x300000u, 1);
        expect_u("census IB1: exactly one row (only slot 2 is nonzero/in-bound)", t1, 1);
        expect_u("census IB1: row[0] SGPR is slot 2", rows1[0].sgpr, 2);
        expect_u("census IB1: row[0] idx is 10", rows1[0].idx, 10);
        expect_u("census IB1: row[0] va is the table VA", rows1[0].va, 0x400300000ull);
        expect_u("census IB1: row[0] dword is the draw's own position", rows1[0].dword, ib1DrawAt);
        // IB2: NO table write of its own - s0:1/s2 must be CARRIED from IB1 (the SAME `ps[]` array, unmodified by the
        // caller between calls, exactly as AppleHardwareHook.cpp's per-frame state persists across its IB loop).
        std::vector<uint32_t> ib2;
        { auto p = sh(0x10, 25u); ib2.insert(ib2.end(), p.begin(), p.end()); }           // PS_4 = 25 (SGPR4, in-bound)
        const uint32_t ib2DrawAt = (uint32_t)ib2.size();
        { auto p = draw(0x27u);   ib2.insert(ib2.end(), p.begin(), p.end()); }           // DRAW_INDEX_2
        n48_gcap_census_row rows2[8]; uint32_t t2 = 0;
        const uint32_t walked2 = n48_gcap_census_ib(ib2.data(), (uint32_t)ib2.size(), kStart, ps, rows2, 8, &t2);
        expect_u("census IB2: walked the whole IB", walked2, (uint32_t)ib2.size());
        expect_u("census IB2: two rows (slot 2 CARRIED from IB1, slot 4 new)", t2, 2);
        int found2 = 0, found4 = 0;
        for (uint32_t z = 0; z < t2; z++) {
            if (rows2[z].sgpr == 2 && rows2[z].idx == 10 && rows2[z].va == 0x400300000ull) found2 = 1;
            if (rows2[z].sgpr == 4 && rows2[z].idx == 25 && rows2[z].va == 0x400300000ull) found4 = 1;
            expect_u("census IB2: row dword is the draw's own position (within IB2)", rows2[z].dword, ib2DrawAt);
        }
        expect_u("census IB2: slot 2 carried across the IB boundary", (uint64_t)found2, 1);
        expect_u("census IB2: slot 4, set only in IB2, also found", (uint64_t)found4, 1);
        // negative: an odd slot (3) is never read, however qualifying its value.
        {
            uint32_t ps3[16] = { 0 };
            std::vector<uint32_t> ib3;
            { auto p = sh(0x0c, 0x00300000u); ib3.insert(ib3.end(), p.begin(), p.end()); }
            { auto p = sh(0x0d, 0x00000004u); ib3.insert(ib3.end(), p.begin(), p.end()); }
            { auto p = sh(0x0fu, 77u);        ib3.insert(ib3.end(), p.begin(), p.end()); }   // offset 0x0f -> slot 3 (odd)
            { auto p = draw(0x24u);           ib3.insert(ib3.end(), p.begin(), p.end()); }   // DRAW_INDIRECT
            n48_gcap_census_row r3[4]; uint32_t t3 = 0;
            n48_gcap_census_ib(ib3.data(), (uint32_t)ib3.size(), kStart, ps3, r3, 4, &t3);
            expect_u("census: an odd slot (3) is never a row, however qualifying its value", t3, 0);
        }
        // negative: s0:1 NOT VA-shaped (hi 0) -> no rows even with in-bound values at other slots.
        {
            uint32_t ps4[16] = { 0 };
            std::vector<uint32_t> ib4;
            { auto p = sh(0x0c, 0x00300000u); ib4.insert(ib4.end(), p.begin(), p.end()); }   // PS_0 set, PS_1 (hi) left 0
            { auto p = sh(0x0e, 10u);         ib4.insert(ib4.end(), p.begin(), p.end()); }
            { auto p = draw(0x35u);           ib4.insert(ib4.end(), p.begin(), p.end()); }   // DRAW_INDEX_OFFSET_2
            n48_gcap_census_row r4[4]; uint32_t t4 = 0;
            n48_gcap_census_ib(ib4.data(), (uint32_t)ib4.size(), kStart, ps4, r4, 4, &t4);
            expect_u("census: s0:1 not VA-shaped (hi 0) -> no rows", t4, 0);
        }
        // negative: a value at or above N48_GCAP_CENSUS_BOUND is not a "small integer".
        {
            uint32_t ps5[16] = { 0 };
            std::vector<uint32_t> ib5;
            { auto p = sh(0x0c, 0x00300000u); ib5.insert(ib5.end(), p.begin(), p.end()); }
            { auto p = sh(0x0d, 0x00000004u); ib5.insert(ib5.end(), p.begin(), p.end()); }
            { auto p = sh(0x0e, N48_GCAP_CENSUS_BOUND); ib5.insert(ib5.end(), p.begin(), p.end()); }
            { auto p = draw(0x25u);           ib5.insert(ib5.end(), p.begin(), p.end()); }   // DRAW_INDEX_INDIRECT
            n48_gcap_census_row r5[4]; uint32_t t5 = 0;
            n48_gcap_census_ib(ib5.data(), (uint32_t)ib5.size(), kStart, ps5, r5, 4, &t5);
            expect_u("census: a value == N48_GCAP_CENSUS_BOUND is not a small integer", t5, 0);
        }
        // max truncation still counts, exactly like n48_gcap_scan.
        {
            uint32_t ps6[16] = { 0 };
            std::vector<uint32_t> ib6;
            { auto p = sh(0x0c, 0x00300000u); ib6.insert(ib6.end(), p.begin(), p.end()); }
            { auto p = sh(0x0d, 0x00000004u); ib6.insert(ib6.end(), p.begin(), p.end()); }
            for (uint32_t s = N48_GCAP_CENSUS_SLOT_LO; s <= N48_GCAP_CENSUS_SLOT_HI; s += 2u) {
                auto p = sh(0x0c + s, s + 1u); ib6.insert(ib6.end(), p.begin(), p.end());
            }
            { auto p = draw(0x2Du); ib6.insert(ib6.end(), p.begin(), p.end()); }
            n48_gcap_census_row r6[2]; uint32_t t6 = 0;
            n48_gcap_census_ib(ib6.data(), (uint32_t)ib6.size(), kStart, ps6, r6, 2, &t6);
            expect_u("census: max truncation still counts all 7 slots", t6, 7);
        }
        // PLANTED BREAK: "no filler step" - a walk that omits the N48_GCAP_F_FILLER handling misreads the filler
        // dword's own count field (0x3fff -> 16385 dwords) and never reaches ib1's draw at all.
        {
            auto census_no_filler = [](const uint32_t *d, uint32_t n, uint64_t sv, uint32_t *psl,
                                       n48_gcap_census_row *rw, uint32_t mx, uint32_t *tot) -> uint32_t {
                uint32_t cnt = 0, i = 0;
                while (i < n) {
                    const uint32_t h = d[i];
                    const uint32_t type = h >> 30;
                    if (type == 2u) { i++; continue; }
                    if (type != 3u) break;
                    const uint32_t op = (h >> 8) & 0xffu, cnt3 = (h >> 16) & 0x3fffu, plen = cnt3 + 2u;
                    if (plen > n - i) break;
                    const uint32_t *b = &d[i + 1u];
                    const uint32_t nb = cnt3 + 1u;
                    if (op == 0x76u || op == 0x9bu) {
                        const uint32_t reg0 = 0x2c00u + (b[0] & 0xffffu);
                        for (uint32_t k = 1; k < nb; k++) {
                            const uint32_t reg = reg0 + (k - 1u);
                            if (reg >= N48_GCAP_PS_UD0 && reg < N48_GCAP_PS_UD0 + 16u) psl[reg - N48_GCAP_PS_UD0] = b[k];
                        }
                    } else if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) {
                        if (n48_gcap_is_va(sv, psl[0], psl[1])) {
                            for (uint32_t s = N48_GCAP_CENSUS_SLOT_LO; s <= N48_GCAP_CENSUS_SLOT_HI; s += 2u) {
                                const uint32_t v = psl[s];
                                if (v && v < N48_GCAP_CENSUS_BOUND) { if (cnt < mx) { rw[cnt].sgpr = s; } cnt++; }
                            }
                        }
                    }
                    i += plen;
                }
                if (tot) *tot = cnt;
                return i;
            };
            uint32_t psNf[16] = { 0 };
            n48_gcap_census_row rNf[8]; uint32_t tNf = 0;
            census_no_filler(ib1.data(), (uint32_t)ib1.size(), kStart, psNf, rNf, 8, &tNf);
            const int caught = (tNf != t1) ? 1 : 0;
            std::printf("  planted %-66s %s (no-filler finds %u row(s), real finds %u)\n",
                        "item 4: no filler step misses IB1's draw entirely", caught ? "CAUGHT" : "MISSED", tNf, t1);
            expect_u("BREAK-check: the no-filler walk misses IB1's row (planted break caught)", (uint64_t)caught, 1);
        }
        // PLANTED BREAK: "the stride-2 window instead of per-slot tracking" - n48_gcap_user_indices' window is a
        // FIXED span of RAW DWORDS from the table pointer's OWN position (N48_GCAP_USER_IDX_WINDOW = 16), which
        // "crosses packet boundaries" (its own header comment) and has no notion of the SGPR the value came from.
        // Build one IB: the table pointer packet (its lo word - the anchor n48_gcap_scan itself reports as
        // it.dword - lands at raw position 2), 20 dwords of ordinary type-2 filler (a real, if unusual, IB shape:
        // NOP-3F packets are type 3, but PACKET2 NOPs are legitimately type 2 and exactly one dword each), THEN a
        // packet setting slot 2 = 10, then a draw. The value's raw position (26) is well past the window
        // [tableDword+2, tableDword+2+16) = [4, 20) - exactly the "crosses packet boundaries" failure the window
        // is documented to have - while n48_gcap_census_ib tracks the SGPR itself and is immune to it.
        {
            std::vector<uint32_t> ibw;
            ibw.push_back(0xC0027600u); ibw.push_back(0x0cu); ibw.push_back(0x00300000u); ibw.push_back(4u);   // table s0:1
            const uint32_t tableDword = 2u;   // the lo word's own position, as n48_gcap_scan would report it (it.dword)
            for (int z = 0; z < 20; z++) ibw.push_back(0x80000000u);          // 20 dwords of type-2 filler
            { auto p = sh(0x0e, 10u); ibw.insert(ibw.end(), p.begin(), p.end()); }   // slot 2 = 10, far past the window
            { auto p = draw(0x2Du);   ibw.insert(ibw.end(), p.begin(), p.end()); }
            uint32_t idxWin[4] = { 0 };
            const uint32_t niWin = n48_gcap_user_indices(ibw.data(), (uint32_t)ibw.size(), kStart, tableDword, idxWin, 4u);
            int windowFound = 0; for (uint32_t z = 0; z < niWin; z++) if (idxWin[z] == 10u) windowFound = 1;
            uint32_t psw[16] = { 0 }; n48_gcap_census_row rw[4]; uint32_t tw = 0;
            n48_gcap_census_ib(ibw.data(), (uint32_t)ibw.size(), kStart, psw, rw, 4, &tw);
            int censusFound = 0; for (uint32_t z = 0; z < tw; z++) if (rw[z].sgpr == 2 && rw[z].idx == 10) censusFound = 1;
            const int caught = (!windowFound && censusFound) ? 1 : 0;
            std::printf("  planted %-66s %s (window found=%d, per-slot census found=%d)\n",
                        "item 4: the stride-2 window misses a slot set past it; per-slot tracking does not",
                        caught ? "CAUGHT" : "MISSED", windowFound, censusFound);
            expect_u("BREAK-check: the stride-2 window (crosses packet boundaries) is caught by a per-slot census", (uint64_t)caught, 1);
        }
    }
    tv457_checks();
    vram_seen_checks();
    cbt_reset_checks();
    std::printf("%d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
