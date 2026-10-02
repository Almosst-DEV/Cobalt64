// f3_reader_test.cpp — F3's full-coverage writer reader and the per-frame key/time line (f3_reader.h, 0.0.363), offline.
// The properties under test:
//
//     "the reader is OFF unless its switch is on AND its buffers exist, and its default is OFF; every NOT-WindowServer frame
//      is read whenever both hold (never skipped because another path is busy, never
//      sampled); every colour target bound at every draw is compared, not only the last per slot and not only the first 64 scan
//      items; a cap on targets is COUNTED and turns the frame UNSETTLED, never NO; NO needs full coverage and a resolved
//      reference; HOST and VRAM pages never compare equal; a frame seen before the reference resolved is compared
//      retrospectively; SDMA COPY / WRITE / CONST_FILL destinations are found; the short lines fit under the log cap."
//
// Every scenario runs against an OPS TABLE, so the same assertions judge the real header and each planted defect: the real table
// must pass every check, and each mutant must be CAUGHT by at least one check.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/f3_reader_test.cpp -o /tmp/f3test && /tmp/f3test
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "f3_reader.h"

static int gFail = 0, gRun = 0, gQuiet = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-92s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-92s %#llx\n", what, (unsigned long long)got);
    }
}

struct Ops {
    const char *name;
    uint32_t (*scan)(const uint32_t *, uint32_t, n48_f3_tgt *, uint32_t, n48_f3_scan *);
    uint32_t (*verdict)(const n48_f3_frame *);
    uint32_t (*cmp)(const n48_f3_ref *, uint32_t, uint32_t, uint64_t);
    /* 0.0.370 (notes 807): (enabled, allocated, otherPathBusy, frameIndex) */
    uint32_t (*should_read)(uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*sdma)(const uint32_t *, uint32_t, n48_f3_sdst *, uint32_t, n48_f3_sscan *);
    int (*ref_changed)(const n48_f3_ref *, const n48_f3_ref *);
    uint64_t (*key)(const n48_xv_frame *, uint32_t, uint64_t, uint32_t *);
    /* 0.0.370: the SHIPPED boot default of the reader's switch. It is a value, not a function, because "what does an ARMED
     * run pay by default" is the question 807 exists to answer and a default is exactly the kind of thing that gets flipped
     * by accident. */
    uint32_t read_default;
};

// ---- planted defects ----
// D1: the 64-item cap of the capture scan, SILENT (targets past it dropped, nothing counted)
static uint32_t scan_silent_cap(const uint32_t *d, uint32_t n, n48_f3_tgt *t, uint32_t max, n48_f3_scan *s)
{
    const uint32_t w = n48_f3_cb_scan(d, n, t, max, s);
    s->capped = 0;
    return w;
}
// D2: only the LAST base per slot (n48_gcap_scan's end-of-walk semantics): the draws are ignored
static uint32_t scan_last_only(const uint32_t *d, uint32_t n, n48_f3_tgt *t, uint32_t max, n48_f3_scan *s)
{
    std::vector<uint32_t> c(d, d + n);
    for (uint32_t i = 0; i < n;) {        // turn every draw packet into a NOP-like unknown opcode (0x10) of the same length
        const uint32_t h = c[i];
        if ((h >> 30) != 3u) { i++; continue; }
        const uint32_t op = (h >> 8) & 0xffu, nb = ((h >> 16) & 0x3fffu) + 1u;
        if (n48_f3_is_draw(op)) c[i] = (h & ~0xff00u) | (0x10u << 8);
        i += nb + 1u;
    }
    return n48_f3_cb_scan(c.data(), n, t, max, s);
}
// D3: NO on partial coverage (the hp4 mistake: "0 matches" read as an answer)
static uint32_t verdict_no_on_partial(const n48_f3_frame *f)
{
    if (!f->head_ok) return N48_F3_V_UNREAD;
    if (f->matches) return N48_F3_V_YES;
    if (!f->ref_ok) return N48_F3_V_NOREF;
    return N48_F3_V_NO;
}
// D4: the comparison ignores HOST vs VRAM
static uint32_t cmp_no_sys(const n48_f3_ref *r, uint32_t ok, uint32_t, uint64_t page)
{
    if (!ok || !r->ok) return N48_F3_CMP_UNRES;
    return page == r->page ? N48_F3_CMP_YES : N48_F3_CMP_NO;
}
// D5: the retired read policy: the first 16 in full, then 1 in 8, and skipped when the decision's buffer is busy
static uint32_t read_old_policy(uint32_t enabled, uint32_t allocated, uint32_t busy, uint32_t idx)
{
    if (!enabled || !allocated || busy) return 0u;
    return (idx < 16u || idx % 8u == 0u) ? 1u : 0u;
}
// D9 (0.0.370, notes 807): THE 0.0.363-0.0.369 GATE - `allocated` is the only input, so the switch is ignored and
// `gfxneuter 3` arms the reader by allocating its buffer. This is the defect 807 fixes.
static uint32_t read_ignores_switch(uint32_t enabled, uint32_t allocated, uint32_t busy, uint32_t idx)
{
    (void)enabled; (void)busy; (void)idx;
    return allocated ? 1u : 0u;
}
// D10 (0.0.370): the switch alone arms the reader - the buffer check is dropped, so `gfxneuter 14 | 1 << 8` before the
// allocation succeeded would walk an IB into a null buffer.
static uint32_t read_no_buffer(uint32_t enabled, uint32_t allocated, uint32_t busy, uint32_t idx)
{
    (void)allocated; (void)busy; (void)idx;
    return enabled ? 1u : 0u;
}
// D6: the SDMA scan misses CONST_FILL and takes the tiled side of a detiling copy
static uint32_t sdma_no_fill(const uint32_t *d, uint32_t n, n48_f3_sdst *o, uint32_t max, n48_f3_sscan *s)
{
    std::vector<uint32_t> c(d, d + n);
    for (uint32_t k = 0; k < n; k++) if ((c[k] & 0xffu) == 11u) { c[k] = (c[k] & ~0xffu) | 0x1fu; }   // an unknown op: stops the walk
    return n48_f3_sdma_scan(c.data(), n, o, max, s);
}
// D7: no retrospective pass on the FIRST resolution (only on a change of an already-resolved page)
static int ref_changed_late(const n48_f3_ref *was, const n48_f3_ref *now)
{
    return now->ok && was->ok && was->page != now->page;
}
// D8: the key line prints only the verdict's key (0 for every non-program rung - the pre-0.0.363 shape)
static uint64_t key_verdict_only(const n48_xv_frame *, uint32_t, uint64_t vkey, uint32_t *cls)
{
    *cls = vkey ? N48_F3_K_UNKNOWN : N48_F3_K_NONE;
    return vkey;
}

static const Ops kReal = { "real", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan,
                           n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT };

// ---- IB builders (gfx10 PM4) ----
static uint32_t pkt3(uint32_t op, uint32_t nb) { return (3u << 30) | ((nb - 1u) << 16) | (op << 8); }
static void set_cb(std::vector<uint32_t> &v, uint32_t slot, uint64_t va)
{
    v.push_back(pkt3(0x69u, 2u)); v.push_back(0x318u + 15u * slot); v.push_back((uint32_t)(va >> 8));
    v.push_back(pkt3(0x69u, 2u)); v.push_back(0x390u + slot); v.push_back((uint32_t)(va >> 40) & 0xffu);
}
static void draw(std::vector<uint32_t> &v) { v.push_back(pkt3(0x2du, 2u)); v.push_back(3u); v.push_back(2u); }   // DRAW_INDEX_AUTO
static void filler_sh(std::vector<uint32_t> &v, uint32_t pairs)   // user-data SGPR pairs: 64+ capture-scan items before the targets
{
    v.push_back(pkt3(0x76u, 1u + 2u * pairs)); v.push_back(0x0c);
    for (uint32_t k = 0; k < pairs; k++) { v.push_back(0x00100000u + k * 0x100u); v.push_back(0x4u); }
}
static bool has_va(const n48_f3_tgt *t, uint32_t n, uint64_t va)
{
    for (uint32_t i = 0; i < n; i++) if (t[i].va == va) return true;
    return false;
}

static void run_all(const Ops &o)
{
    // S1: two render passes on CB0 in one IB, the F3 page is the FIRST pass's target (the last-per-slot scan cannot see it).
    {
        std::vector<uint32_t> v;
        filler_sh(v, 80);                              // 80 capture items ahead of any target: the old 64-cap overflowed here
        set_cb(v, 0, 0x400006000ull); draw(v);
        set_cb(v, 0, 0x400440000ull); draw(v);
        static n48_f3_tgt t[N48_F3_TGT_CAP]; n48_f3_scan s {};
        const uint32_t w = o.scan(v.data(), (uint32_t)v.size(), t, N48_F3_TGT_CAP, &s);
        expect_u("S1 walked the whole IB", w, v.size());
        expect_u("S1 the first pass's target (0x400006000) is seen", has_va(t, s.stored, 0x400006000ull), 1);
        expect_u("S1 the second pass's target is seen", has_va(t, s.stored, 0x400440000ull), 1);
        expect_u("S1 two draws", s.draws, 2);
        expect_u("S1 no cap hit", s.capped, 0);
    }
    // S2: the safety cap: more distinct targets than the cap -> counted, and the frame is UNSETTLED
    {
        std::vector<uint32_t> v;
        for (uint32_t k = 0; k < N48_F3_TGT_CAP + 5u; k++) { set_cb(v, k & 7u, 0x500000000ull + (uint64_t)k * 0x10000ull); draw(v); }
        static n48_f3_tgt t[N48_F3_TGT_CAP]; n48_f3_scan s {};
        (void)o.scan(v.data(), (uint32_t)v.size(), t, N48_F3_TGT_CAP, &s);
        expect_u("S2 stored == the cap", s.stored, N48_F3_TGT_CAP);
        expect_u("S2 cap hit COUNTED (at least the 5 distinct not stored)", s.capped >= 5u, 1);
        n48_f3_frame f {}; f.head_ok = 1; f.ref_ok = 1; f.ibs = 1; f.ibs_full = 1; f.capped = s.capped; f.targets = s.stored;
        expect_u("S2 a capped frame is UNSETTLED, never NO", o.verdict(&f), N48_F3_V_UNSETTLED);
    }
    // S3: verdicts
    {
        n48_f3_frame f {}; f.head_ok = 1; f.ref_ok = 1; f.ibs = 2; f.ibs_full = 2; f.targets = 3;
        expect_u("S3 full coverage, no match -> NO", o.verdict(&f), N48_F3_V_NO);
        f.matches = 1;
        expect_u("S3 a match -> YES", o.verdict(&f), N48_F3_V_YES);
        f.matches = 0; f.ibs_full = 1;
        expect_u("S3 an IB not in full -> UNSETTLED", o.verdict(&f), N48_F3_V_UNSETTLED);
        f.ibs_full = 2; f.nested = 1;
        expect_u("S3 a nested IB not followed -> UNSETTLED", o.verdict(&f), N48_F3_V_UNSETTLED);
        f.nested = 0; f.unresolved = 1;
        expect_u("S3 an unresolved target -> UNSETTLED", o.verdict(&f), N48_F3_V_UNSETTLED);
        f.unresolved = 0; f.ref_ok = 0;
        expect_u("S3 reference unresolved -> NO-REFERENCE", o.verdict(&f), N48_F3_V_NOREF);
        f.ref_ok = 1; f.head_ok = 0;
        expect_u("S3 IB 0 not read -> UNREAD", o.verdict(&f), N48_F3_V_UNREAD);
        f.head_ok = 1; f.ibs = 0; f.ibs_full = 0;
        expect_u("S3 no IB -> UNSETTLED", o.verdict(&f), N48_F3_V_UNSETTLED);
        f.ibs = 1; f.ibs_full = 1; f.ibs_full = 0; f.matches = 1;
        expect_u("S3 a match with partial coverage is still YES", o.verdict(&f), N48_F3_V_YES);
    }
    // S4: the comparison
    {
        n48_f3_ref r {}; r.ok = 1; r.sys = 0; r.page = 0x10079000ull;
        expect_u("S4 same VRAM page -> YES", o.cmp(&r, 1, 0, 0x10079000ull), N48_F3_CMP_YES);
        expect_u("S4 other page -> NO", o.cmp(&r, 1, 0, 0x12ec4000ull), N48_F3_CMP_NO);
        expect_u("S4 same number but HOST -> NO", o.cmp(&r, 1, 1, 0x10079000ull), N48_F3_CMP_NO);
        expect_u("S4 target unresolved -> UNRESOLVED", o.cmp(&r, 0, 0, 0x10079000ull), N48_F3_CMP_UNRES);
        r.ok = 0;
        expect_u("S4 reference unresolved -> UNRESOLVED", o.cmp(&r, 1, 0, 0x10079000ull), N48_F3_CMP_UNRES);
    }
    // S5: skipped-never - 1000 frames with the other path busy on every third: every one is read while ON and allocated,
    // none when the switch is off. The busy flag and the frame index are still passed and must still change nothing.
    {
        uint32_t read = 0, readOff = 0;
        for (uint32_t i = 0; i < 1000u; i++) {
            read += o.should_read(1u, 1u, (i % 3u) == 0u, i);
            readOff += o.should_read(0u, 1u, 0u, i);
        }
        expect_u("S5 on: 1000 of 1000 frames read (busy/sample never skip)", read, 1000);
        expect_u("S5 switch off: none read", readOff, 0);
    }
    // S9 (0.0.370, notes 807): THE SWITCH IS THE SWITCH, AND THE DEFAULT IS OFF.
    //   - both inputs are required: an allocated buffer alone must NOT read (that was the 0.0.363-0.0.369 defect, D9), and a
    //     switch alone must NOT read either (D10: there is no buffer to read into);
    //   - the shipped default is OFF, so an ARMED run that never names the reader never pays for it.
    {
        expect_u("S9 allocated but switch OFF -> not read", o.should_read(0u, 1u, 0u, 0u), 0);
        expect_u("S9 switch ON but not allocated -> not read", o.should_read(1u, 0u, 0u, 0u), 0);
        expect_u("S9 neither -> not read", o.should_read(0u, 0u, 0u, 0u), 0);
        expect_u("S9 both -> read", o.should_read(1u, 1u, 0u, 0u), 1);
        expect_u("S9 the shipped default is OFF", o.read_default, 0);
        // And a default that is off has to STAY off across the whole busy/index space the retired policy used as inputs.
        uint32_t readAtDefault = 0;
        for (uint32_t i = 0; i < 64u; i++) readAtDefault += o.should_read(o.read_default, 1u, (i % 3u) == 0u, i);
        expect_u("S9 at the default, 64 frames, 0 read", readAtDefault, 0);
    }
    // S6: the SDMA scan
    {
        std::vector<uint32_t> v = {
            0x00000000u,                                                   // NOP
            0x00000001u, 0x3fffu, 0u, 0x1000u, 0x84u, 0x00079000u, 0x80u,  // COPY_LINEAR dst 0x8000079000
            0x0000000bu, 0x0007a000u, 0x80u, 0xdeadbeefu, 0x100u,          // CONST_FILL dst 0x800007a000
            0x80000101u, 0x7b000u, 0x80u, 0, 0, 0, 0, 0, 0, 0x7c000u, 0x80u, 0, 0, 0,   // COPY_TILED detile: dst = linear 0x800007c000
            0x00000002u, 0x0007d000u, 0x80u, 1u, 0x11u, 0x22u,             // WRITE_UNTILED 2 dwords, dst 0x800007d000
            0x00000005u, 0, 0, 0,                                          // FENCE
        };
        n48_f3_sdst d[N48_F3_SDMA_CAP]; n48_f3_sscan s {};
        const uint32_t n = o.sdma(v.data(), (uint32_t)v.size(), d, N48_F3_SDMA_CAP, &s);
        expect_u("S6 four destinations", n, 4);
        expect_u("S6 walked to the end", s.stopped, 0);
        bool fill = false, lin = false, tiled = false, wr = false;
        for (uint32_t i = 0; i < s.stored; i++) {
            fill |= d[i].va == 0x800007a000ull; lin |= d[i].va == 0x8000079000ull; tiled |= d[i].va == 0x800007c000ull;
            wr |= d[i].va == 0x800007d000ull;
        }
        expect_u("S6 COPY_LINEAR dst", lin, 1);
        expect_u("S6 CONST_FILL dst", fill, 1);
        expect_u("S6 detiling COPY_TILED writes the LINEAR side", tiled, 1);
        expect_u("S6 WRITE_UNTILED dst", wr, 1);
        std::vector<uint32_t> u = { 0x00000001u, 0x3fffu, 0u, 0u, 0u, 0x1000u, 0x80u, 0x000000ffu, 0u, 0u };
        (void)o.sdma(u.data(), (uint32_t)u.size(), d, N48_F3_SDMA_CAP, &s);
        expect_u("S6 an unknown opcode stops the walk (reported)", s.stopped, 1);
    }
    // S7: retrospective comparison
    {
        static n48_f3_pages t; std::memset(&t, 0, sizeof(t));
        n48_f3_pages_note(&t, 0x12ec4000ull, 0, N48_F3_T_CB, 0x400440000ull, 1, 4, 1277);
        n48_f3_pages_note(&t, 0x10079000ull, 0, N48_F3_T_CB, 0x400024000ull, 2, 4, 1277);
        expect_u("S7 a repeated page is not new", n48_f3_pages_note(&t, 0x10079000ull, 0, N48_F3_T_CB, 0x400024000ull, 3, 4, 1277), 0);
        n48_f3_ref none {}, now {}; now.ok = 1; now.page = 0x10079000ull;
        expect_u("S7 the FIRST resolution triggers a retro pass", (uint64_t)o.ref_changed(&none, &now), 1);
        expect_u("S7 the same reference does not", (uint64_t)o.ref_changed(&now, &now), 0);
        uint32_t idx[4] = { 0 };
        expect_u("S7 retro finds the earlier frame's page", n48_f3_pages_retro(&t, &now, idx, 4), 1);
        expect_u("S7 ... it is frame #2's", t.p[idx[0]].frame, 2);
        for (uint32_t k = 0; k < N48_F3_PAGES_CAP + 3u; k++) n48_f3_pages_note(&t, 0x20000000ull + k * 0x1000ull, 0, N48_F3_T_CB, 0, 9, 4, 1);
        expect_u("S7 table overflow is COUNTED", t.over, 5);
    }
    // S8: the per-frame key
    {
        n48_xv_frame f {}; uint32_t cls = 9;
        f.npgm = 2; f.pgm[0].key = 0x1111ull; f.pgm[0].key_class = N48_XV_PGM_KEY_XLAT; f.pgm[0].bytes_are_ours = 1;
        f.pgm[1].key = 0xfcc6342ea8db283full; f.pgm[1].key_class = N48_XV_PGM_KEY_UNKNOWN;
        expect_u("S8 an ib-short frame names its first UNKNOWN program's key", o.key(&f, N48_XV_IB_SHORT, 0, &cls), 0xfcc6342ea8db283full);
        expect_u("S8 ... class unknown", cls, N48_F3_K_UNKNOWN);
        expect_u("S8 the verdict's own key wins", o.key(&f, N48_XV_PGM_UNKNOWN, 0xabcull, &cls), 0xabc);
        f.pgm[1].key_class = N48_XV_PGM_KEY_XLAT; f.pgm[1].relocated = 1;
        expect_u("S8 all programs ours -> key 0", o.key(&f, N48_XV_SEG_POLICY, 0, &cls), 0);
        expect_u("S8 ... class all-programs-ours", cls, N48_F3_K_NONE);
    }
}

// The short lines' worst case under the kext's 512-byte cap (HWLOG adds 20 bytes).
static void line_bounds()
{
    char b[1024];
    int n = std::snprintf(b, sizeof(b), N48_F3_LINE_FMT, 0xffffffffu, -2147483647 - 1, "0123456789abcdefghi", 15u, 0xffffffffu,
                          "PARTIAL", "MATCH DMA4294967295", 0xffffffffffffffffull, "UNRESOLVED", 0xffffffffffffffffull,
                          0xffffffffffffffffull, "UNRESOLVED", 0xffffffffffffffffull, "NO-REFERENCE", " - ",
                          "WindowServer's VMID not yet recorded (no judged frame yet)");
    std::printf("      f3-writer line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect_u("L1 the f3-writer verdict line fits under the log cap", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1);
    n = std::snprintf(b, sizeof(b), N48_WSF_LINE_FMT, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, 0xffffffffffffffffull, 999999ull,
                      -2147483647 - 1, 0xffffffffu, 0xffffffffu, "program-not-substituted", 0xffffffffffffffffull, "no-program-read",
                      "HEADLESS",   /* 0.0.370 (notes 807): the longest of ENCODER / HEADLESS / UNSET */
                      "neuter (verdict computed; COMMIT not armed)",
                      n48_wsf_nseg_cause_name(N48_WSF_NSEG_NOT_ELIGIBLE),   /* build 0.0.474 item 1: the longest cause name */
                      "NOP-HEAD",    /* gfx_mib.h's n48_mib_start_name's longest string (NOT included here - see
                                      * N48_WSF_LINE_FMT's own comment; gfx_mib_test.cpp's own suite bounds the
                                      * real N48_MIB0_SEG_FMT/etc. lines that DO call it) */
                      N48_UNITS_MARK);   /* build 0.0.481: the switch-55 units marker, its only non-empty value */
    std::printf("      gfx-xlat key/time line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect_u("L2 the gfx-xlat key/time line fits under the log cap", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1);
}

// build 0.0.474 item 1 (10B-COVERAGE.md Q4 item 4 contract (1)) — the WSF line's nseg-0 cause classifier, pure
// and REACHABILITY-driven in the real order a judged frame takes: segment stage (n48_mib_segment, real) -> the
// stage's own answer (nseg) -> n48_xv_decide (real, unchanged) -> n48_wsf_nseg_cause (this build's new classifier).
static void nseg0_cause()
{
    expect_u("N1 divisor-skipped frame -> SAMPLED (sampled wins even if ran_policy were also set)",
             n48_wsf_nseg_cause(1u, 1u), N48_WSF_NSEG_SAMPLED);
    expect_u("N2 divisor-skipped frame -> SAMPLED", n48_wsf_nseg_cause(1u, 0u), N48_WSF_NSEG_SAMPLED);
    expect_u("N3 policy ran and still found nothing -> ZERO-SEG", n48_wsf_nseg_cause(0u, 1u), N48_WSF_NSEG_ZERO_SEG);
    expect_u("N4 policy never attempted -> NOT-ELIGIBLE", n48_wsf_nseg_cause(0u, 0u), N48_WSF_NSEG_NOT_ELIGIBLE);
    expect_u("N5 name(SAMPLED)", std::strcmp(n48_wsf_nseg_cause_name(N48_WSF_NSEG_SAMPLED), "SAMPLED") == 0, 1);
    expect_u("N6 name(ZERO-SEG)", std::strcmp(n48_wsf_nseg_cause_name(N48_WSF_NSEG_ZERO_SEG), "ZERO-SEG") == 0, 1);
    expect_u("N7 name(NOT-ELIGIBLE)", std::strcmp(n48_wsf_nseg_cause_name(N48_WSF_NSEG_NOT_ELIGIBLE), "NOT-ELIGIBLE") == 0, 1);

    // N8-N9: the classifier composed with the REAL, unmodified n48_xv_decide (no xlat12 link needed here - the full
    // segment-stage reachability chain, which DOES need xlat12_ib.c linked, is tests/gfx_mib_test.cpp's own suite;
    // this still proves "verdict lands on segment-policy with nseg 0" -> "the classifier reads that same nseg 0").
    n48_xv_frame f {};
    f.armed = 1; f.shape_ok = 1; f.reader_ok = 1; f.nib = 1;
    f.ib[0].len = f.ib[0].got = f.ib[0].walk = 16u;
    f.nseg = 0u;   // exactly what gfxsrc_policy's `f->nseg = ns;` stores when the real stage answers 0
    uint64_t key = 0; uint32_t detail = 0;
    const uint32_t verdict = n48_xv_decide(&f, &key, &detail);
    expect_u("N8 the real n48_xv_decide answers segment-policy on nseg 0", verdict, (uint32_t)N48_XV_SEG_POLICY);
    expect_u("N9 the classifier, fed ran_policy=1 for that same frame, answers ZERO-SEG",
             n48_wsf_nseg_cause(0u, 1u), N48_WSF_NSEG_ZERO_SEG);
}

int main()
{
    run_all(kReal);
    line_bounds();
    nseg0_cause();
    const int realFail = gFail, realRun = gRun;
    struct { const char *what; Ops o; } mut[] = {
        { "D1 silent 64-style cap (not counted)", { "D1", scan_silent_cap, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D2 last CB per slot only (gcap semantics)", { "D2", scan_last_only, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D3 NO on partial coverage", { "D3", n48_f3_cb_scan, verdict_no_on_partial, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D4 HOST == VRAM in the comparison", { "D4", n48_f3_cb_scan, n48_f3_frame_verdict, cmp_no_sys, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D5 the retired read policy (busy skip + 1-in-8)", { "D5", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, read_old_policy, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D6 SDMA scan misses CONST_FILL", { "D6", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, sdma_no_fill, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D7 no retro pass on the first resolution", { "D7", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, ref_changed_late, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D8 key only from the verdict", { "D8", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, key_verdict_only, N48_F3_READ_DEFAULT } },
        { "D9 the switch is IGNORED (allocated == armed, the 0.0.369 gate)", { "D9", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, read_ignores_switch, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D10 the switch alone reads (no buffer required)", { "D10", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, read_no_buffer, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, N48_F3_READ_DEFAULT } },
        { "D11 the reader is ON by default", { "D11", n48_f3_cb_scan, n48_f3_frame_verdict, n48_f3_cmp, n48_f3_should_read, n48_f3_sdma_scan, n48_f3_ref_changed, n48_f3_frame_key, 1u } },
    };
    int caught = 0;
    const int nm = (int)(sizeof(mut) / sizeof(mut[0]));
    gQuiet = 1;
    for (int i = 0; i < nm; i++) {
        const int before = gFail;
        run_all(mut[i].o);
        const int f = gFail - before;
        std::printf("  planted %-60s %s (%d check(s) fail)\n", mut[i].what, f ? "CAUGHT" : "MISSED", f);
        if (f) caught++;
    }
    std::printf("f3_reader: %d check(s) on the real header, %d failed; %d of %d planted defects caught.%s\n", realRun, realFail, caught,
                nm, (realFail == 0 && caught == nm) ? " N48-F3-TEST-PASS" : " FAIL");
    return (realFail == 0 && caught == nm) ? 0 : 1;
}
