// gfx_mmprio_test.cpp — 0.0.433 (an internal design note). MM-WINDOW PRIORITY'S HOST PROOF.
//
// T1-T4 drive the REAL pure functions (gfx_mmprio.h, the same header the kext compiles) against MUTANT copies with
// each brief-named defect, and prove every mutant is CAUGHT on a planted input. T5/T6 are source pins: T5 checks (by
// text) that the wait comes before IOLockLock in both navi48_vram_read_mm/write_mm and that the RAII scope is
// gfxsrc_policy's first statement; T6 hashes the six untouched reader bodies (gfxc_read, gfxc_page,
// gfxsrc_desc_read, n48_dp_read, gfxsrc_pgm_profile, xlat12_ib_translate_draw_ex) and compares them against
// 0.0.431's FNV-1a32, embedded from `git show 5f57ad6:` of each file (verified offline to be byte-identical at
// HEAD before this brief's edit). T7 measures the two report lines at their numeric widest.
//
// Every file argument is OPTIONAL and its checks are SKIPPED, not failed, when absent (gfx_mib_test.cpp's
// convention) — this suite still proves T1-T4/T7 with no arguments at all.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_mmprio_test.cpp -o /tmp/mmpriotest && \
//         /tmp/mmpriotest [src/navi48-bringup/src/Navi48Bringup.cpp] [src/navi48-bringup/src/apple/AppleHardwareHook.cpp] \
//                         [src/navi48-bringup/src/apple/gfx_desc_port.h] [src/xlat12/xlat12_ib.c]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <fstream>
#include <sstream>
#include "gfx_mmprio.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect(const char *what, bool ok)
{
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL  %s\n", what); }
    else if (!gQuiet) std::printf("ok    %s\n", what);
}

// =============================================================================================================
// T1 — switch off, it never waits. Break: ignore the switch.
// =============================================================================================================
static int mut_should_yield_ignore_switch(uint32_t /*on*/, uint32_t nesting, int isOwner)
{
    return (nesting > 0u) && !isOwner;
}

static void test_T1(void)
{
    // Positive control: with the switch ON, a pass active and a non-owner caller, the real function DOES say yield —
    // so T1's negative result below is not simply "always 0".
    expect("T1 positive control: on, active, non-owner -> yield", n48_mmprio_should_yield(1u, 3u, 0) != 0);
    // The real property: switch OFF -> never yields, whatever nesting/ownership is.
    expect("T1 real: switch off, active, non-owner -> no yield", n48_mmprio_should_yield(0u, 3u, 0) == 0);
    expect("T1 real: switch off, active, owner -> no yield", n48_mmprio_should_yield(0u, 3u, 1) == 0);
    // MUTANT (ignores `on`): on the same OFF input it says yield anyway — CAUGHT.
    const int real = n48_mmprio_should_yield(0u, 3u, 0);
    const int mut  = mut_should_yield_ignore_switch(0u, 3u, 0);
    expect("T1 mutant (ignore switch) CAUGHT: diverges from real when off", real != mut);
    // Also gate the spin loop itself: keep_spinning must be false immediately when off.
    expect("T1 real: keep_spinning is false when off", n48_mmprio_keep_spinning(0u, 3u, 0ull) == 0);
}

// =============================================================================================================
// T2 — the owner never waits. Break: drop the owner test.
// =============================================================================================================
static int mut_should_yield_drop_owner(uint32_t on, uint32_t nesting, int /*isOwner*/)
{
    return on && (nesting > 0u);
}

static void test_T2(void)
{
    expect("T2 real: owner, switch on, active -> no yield", n48_mmprio_should_yield(1u, 5u, 1) == 0);
    expect("T2 real: non-owner, switch on, active -> yield", n48_mmprio_should_yield(1u, 5u, 0) != 0);
    const int real = n48_mmprio_should_yield(1u, 5u, 1);
    const int mut  = mut_should_yield_drop_owner(1u, 5u, 1);
    expect("T2 mutant (drop owner test) CAUGHT: diverges from real for the owner", real != mut);
    // is_owner itself: only true while nesting > 0 and the identities match.
    expect("T2 real: is_owner true (nesting>0, same id)", n48_mmprio_is_owner(1u, 0x1000ull, 0x1000ull) != 0);
    expect("T2 real: is_owner false (different id)", n48_mmprio_is_owner(1u, 0x1000ull, 0x2000ull) == 0);
    expect("T2 real: is_owner false (no pass active, same id)", n48_mmprio_is_owner(0u, 0x1000ull, 0x1000ull) == 0);
}

// =============================================================================================================
// T3 — the bound ends the wait. Break: remove it; the simulated loop hits its cap.
// =============================================================================================================
static int mut_keep_spinning_no_bound(uint32_t on, uint32_t nesting, uint64_t /*elapsedUs*/)
{
    return on && (nesting > 0u);   // no bound test at all
}

static void test_T3(void)
{
    expect("T3 bound_hit: exactly at the cap is a hit", n48_mmprio_bound_hit(N48_MMPRIO_BOUND_US) != 0);
    expect("T3 bound_hit: one below the cap is not a hit", n48_mmprio_bound_hit(N48_MMPRIO_BOUND_US - 1ull) == 0);
    expect("T3 bound_hit: well past the cap is a hit", n48_mmprio_bound_hit(N48_MMPRIO_BOUND_US * 4ull) != 0);

    // Simulate the real spin loop: elapsed advances 50us per "IODelay", exactly as navi48_vram_read_mm/write_mm do.
    // The real predicate must stop the loop AT the bound, never past a 4050us elapsed (80 iterations of 50us).
    {
        uint64_t elapsed = 0ull;
        uint32_t iters = 0u;
        while (n48_mmprio_keep_spinning(1u, 1u, elapsed) && iters < 100000u) { elapsed += 50ull; iters++; }
        expect("T3 real loop: stops at/just past the 4ms bound", elapsed >= N48_MMPRIO_BOUND_US && elapsed < N48_MMPRIO_BOUND_US + 100ull);
        expect("T3 real loop: iteration count matches bound/step", iters == (uint32_t)(N48_MMPRIO_BOUND_US / 50ull));
        expect("T3 real loop: the elapsed it stopped on IS a bound hit", n48_mmprio_bound_hit(elapsed) != 0);
    }
    // MUTANT (no bound): the SAME simulated loop, with an iteration cap standing in for "runs forever" — it must
    // ride the cap out, proving the real loop's stop was the bound's doing, not something else.
    {
        uint64_t elapsed = 0ull;
        uint32_t iters = 0u;
        const uint32_t ITER_CAP = 100000u;
        while (mut_keep_spinning_no_bound(1u, 1u, elapsed) && iters < ITER_CAP) { elapsed += 50ull; iters++; }
        expect("T3 mutant (no bound) CAUGHT: the simulated loop hits its cap, not the 4ms bound",
               iters == ITER_CAP && elapsed > N48_MMPRIO_BOUND_US * 10ull);
    }
    // A pass ending mid-spin (nesting -> 0) or the switch flipping off must ALSO stop the loop, independent of time.
    expect("T3 real: pass ended mid-spin stops immediately", n48_mmprio_keep_spinning(1u, 0u, 0ull) == 0);
    expect("T3 real: switch off mid-spin stops immediately", n48_mmprio_keep_spinning(0u, 1u, 0ull) == 0);
}

// =============================================================================================================
// T4 — an exit without an enter does not underflow. Break: plain decrement.
// =============================================================================================================
static uint32_t mut_exit_nesting_plain(uint32_t nesting)
{
    return nesting - 1u;   // wraps to 0xFFFFFFFF at nesting == 0
}

static void test_T4(void)
{
    expect("T4 real: enter from 0 -> 1", n48_mmprio_enter_nesting(0u) == 1u);
    expect("T4 real: enter from 1 -> 2 (re-entrant)", n48_mmprio_enter_nesting(1u) == 2u);
    expect("T4 real: exit from 1 -> 0", n48_mmprio_exit_nesting(1u) == 0u);
    expect("T4 real: exit from 2 -> 1", n48_mmprio_exit_nesting(2u) == 1u);
    // THE PLANTED PROPERTY: an exit with no matching enter (nesting already 0) must stay 0, never underflow.
    expect("T4 real: exit from 0 stays 0 (no underflow)", n48_mmprio_exit_nesting(0u) == 0u);
    const uint32_t real = n48_mmprio_exit_nesting(0u);
    const uint32_t mut  = mut_exit_nesting_plain(0u);
    expect("T4 mutant (plain decrement) CAUGHT: wraps to 0xFFFFFFFF where real stays 0", real != mut && mut == 0xFFFFFFFFu);
}

// =============================================================================================================
// T5 — source pins: the wait comes before IOLockLock in both MM functions; the RAII scope is gfxsrc_policy's
// first statement. Text-based, like gfx_desc_port_test.cpp's group C. Proven non-vacuous against embedded mutant
// snippets before being run against the real files.
// =============================================================================================================
static bool read_file(const char *path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Mirrors gfx_desc_port_test.cpp/gfx_mib_test.cpp's brace-matched function-body extractor: finds the DEFINITION
// (the first occurrence whose parameter list is followed by '{', not ';') and returns [start, end) of the body,
// braces included.
static bool extract_body(const std::string &text, const std::string &name, std::string &out)
{
    size_t idx = 0;
    const std::string needle = name + "(";
    const size_t n = text.size();
    for (;;) {
        size_t pos = text.find(needle, idx);
        if (pos == std::string::npos) return false;
        if (pos > 0 && (std::isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '_')) { idx = pos + 1; continue; }
        size_t i = pos + needle.size() - 1;
        int depth = 0;
        while (i < n) {
            if (text[i] == '(') depth++;
            else if (text[i] == ')') { depth--; if (depth == 0) { i++; break; } }
            i++;
        }
        size_t j = i;
        while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) j++;
        if (j < n && text[j] == '{') {
            int d = 0; size_t k = j, start = j;
            while (k < n) {
                if (text[k] == '{') d++;
                else if (text[k] == '}') { d--; if (d == 0) { k++; out = text.substr(start, k - start); return true; } }
                k++;
            }
            return false;
        }
        idx = pos + 1;
    }
}

// Strips // and /* */ comments (not inside string/char literals, which none of these bodies' relevant prefixes are).
static std::string strip_comments(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') i++;
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
        } else {
            out.push_back(s[i]);
            i++;
        }
    }
    return out;
}

// The property: within a function body, "IODelay" occurs (the yield spin) strictly before "IOLockLock(gVramMmLock)".
static bool pin_wait_before_lock(const std::string &body)
{
    const size_t d = body.find("IODelay");
    const size_t l = body.find("IOLockLock(gVramMmLock)");
    return d != std::string::npos && l != std::string::npos && d < l;
}

// The property: the first non-whitespace, non-comment token run in the body is "Navi48MmPrioScope".
static bool pin_scope_is_first_statement(const std::string &body)
{
    std::string stripped = strip_comments(body);
    // body includes the leading '{': skip it, then leading whitespace.
    size_t i = 0;
    if (i < stripped.size() && stripped[i] == '{') i++;
    while (i < stripped.size() && std::isspace((unsigned char)stripped[i])) i++;
    const std::string want = "Navi48MmPrioScope";
    return stripped.compare(i, want.size(), want) == 0;
}

static void test_T5(const char *bringupPath, const char *ahhPath)
{
    // Non-vacuity FIRST, against embedded mutant snippets mimicking the brief's named break ("move either one" /
    // the scope declared after other statements) — proves these predicates have teeth before trusting them on the
    // real files.
    {
        const std::string mutOrderSwapped =
            "{\n    if (gVramMmLock) {\n        IOLockLock(gVramMmLock);\n        /* yield moved AFTER the lock, wrongly */\n        IODelay(50);\n    }\n}\n";
        expect("T5 mutant (wait AFTER IOLockLock) CAUGHT", !pin_wait_before_lock(mutOrderSwapped));
        const std::string realShape =
            "{\n    if (gVramMmLock) {\n        IODelay(50);\n        IOLockLock(gVramMmLock);\n    }\n}\n";
        expect("T5 positive control: wait before lock passes on a correctly-shaped body", pin_wait_before_lock(realShape));
    }
    {
        const std::string mutScopeNotFirst =
            "{\n    static xlat12_ib_segment segs[8];\n    Navi48MmPrioScope mmPrioScope;\n}\n";
        expect("T5 mutant (scope not first statement) CAUGHT", !pin_scope_is_first_statement(mutScopeNotFirst));
        const std::string realShape2 =
            "{\n    // a comment before the scope must not fool the check\n    Navi48MmPrioScope mmPrioScope;\n    static xlat12_ib_segment segs[8];\n}\n";
        expect("T5 positive control: scope-first passes on a correctly-shaped body", pin_scope_is_first_statement(realShape2));
    }

    // Now the real files, each optional.
    if (bringupPath) {
        std::string text;
        if (!read_file(bringupPath, text)) {
            std::printf("SKIP  T5: could not read %s\n", bringupPath);
        } else {
            std::string readBody, writeBody;
            if (extract_body(text, "navi48_vram_read_mm", readBody))
                expect("T5 navi48_vram_read_mm: wait comes before IOLockLock", pin_wait_before_lock(readBody));
            else
                std::printf("SKIP  T5: navi48_vram_read_mm not found in %s\n", bringupPath);
            if (extract_body(text, "navi48_vram_write_mm", writeBody))
                expect("T5 navi48_vram_write_mm: wait comes before IOLockLock", pin_wait_before_lock(writeBody));
            else
                std::printf("SKIP  T5: navi48_vram_write_mm not found in %s\n", bringupPath);
        }
    } else {
        std::printf("SKIP  T5: no Navi48Bringup.cpp path given\n");
    }

    if (ahhPath) {
        std::string text;
        if (!read_file(ahhPath, text)) {
            std::printf("SKIP  T5: could not read %s\n", ahhPath);
        } else {
            std::string body;
            if (extract_body(text, "gfxsrc_policy", body))
                expect("T5 gfxsrc_policy: Navi48MmPrioScope is the first statement", pin_scope_is_first_statement(body));
            else
                std::printf("SKIP  T5: gfxsrc_policy not found in %s\n", ahhPath);
        }
    } else {
        std::printf("SKIP  T5: no AppleHardwareHook.cpp path given\n");
    }
}

// =============================================================================================================
// T6 — byte identity of the six untouched reader bodies against 0.0.431's FNV-1a32, embedded from
// `git show 5f57ad6:` of each file (offline-verified byte-identical to this branch's HEAD before this brief).
// =============================================================================================================
static uint32_t fnv1a32(const std::string &s)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) { h ^= c; h *= 0x01000193u; }
    return h;
}

// Expected 0.0.431 hashes, computed offline by the SAME extractor over `git show 5f57ad6:<file>` and re-verified
// identical against this branch's HEAD (aade29e, on top of 19c7881) before any 0.0.433 edit.
//
// 0.0.434 (an internal design note Part 1) — DELIBERATE RE-BASELINE OF TWO OF THE SIX:
// `gfxsrc_pgm_profile` is the body switch 38 edits (the head-first read replaces its
// `gfxc_read(...kXdIdDwords...)` + `xlat12_shader_id_match` pair; "everything from `int id =` down" is otherwise
// untouched). `gfxsrc_desc_read` is touched too, but for a DIFFERENT reason: the nib >= 2 phase-timer extension
// (also this build,'s binding for a second `n48_mib_pol`) turned the single-IB-only `gPolDescTiming` flag it
// reads into `gPolDescTarget`, a pointer at whichever accumulator (gMibPol or the new gMibPol2) the active pass
// feeds - the SAME two guarded uptime reads, now routed through a pointer instead of a flag. Both expected hashes
// below are the NEW bodies, computed the same way over this branch AFTER the edits; the other four (gfxc_read,
// gfxc_page, n48_dp_read, xlat12_ib_translate_draw_ex) are still 0.0.431's - neither this brief nor the nib >= 2
// extension touches them - and stay the review's own diff of old against new for the two that changed.
// 0.0.435 (an internal design note Part 2) — DELIBERATE RE-BASELINE OF A FIFTH: `gfxc_read`
// is now a thin wrapper (`return gfxc_read_core(vm, va, dst, n, sysPages, nullptr);`) over the new gfxc_read_core,
// so gfxc_read_rs (the recording twin gfxsrc_desc_gfxc/gfxsrc_pgm_profile_headfirst/id_read now call, so the
// copy-overlap check sees every VRAM page they touch) can share the one page-walk implementation rather than
// duplicate it. gfxc_read's OWN behaviour is unchanged - it always passes a null recorder, byte for byte - only
// its literal body text moved. The other four below (gfxc_page, n48_dp_read, xlat12_ib_translate_draw_ex, and
// gfxsrc_desc_read/gfxsrc_pgm_profile from 0.0.434's own re-baseline) are untouched by this brief.
static const uint32_t kExpectGfxcRead          = 0xe9955557u;   // 0.0.435 re-baseline (was 0xb23acf16u at 0.0.431..0.0.434)
// R1 (an internal design note Q2 "UNRESOLVED") re-baseline (was 0xcb3d45dfu at 0.0.435-0.0.439): gfxc_page grew
// the DEFAULTED `leafOut` parameter and its two writes (0 on entry, the raw leaf on a successful walk) - the
// reader's OWN behaviour, and every one of the 21 pre-existing call sites, are unchanged; only the body's SOURCE
// TEXT this hash covers moved.
// build 0.0.540 (switch 96, perf540 T6/T11) DELIBERATE re-baseline (was 0xd477e0d4u at 0.0.439-0.0.539, reproduced by this
// extractor over 71f805d8): the walk moved VERBATIM into gfxc_page_walk (+ an `lv` level counter that is null OFF) and gfxc_page is now
// `if (pf_on()) return gfxc_page_pf(...); return gfxc_page_walk(..., nullptr);` - one load OFF, the same walk and answer either way
// (tests/gfx_perf540_test.cpp pins both lines). Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.541 (switch 98, gfx_wc98.h) DELIBERATE re-baseline (was 0xa96244f6u at 0.0.540, reproduced by this extractor over
// 2c91d368): gfxc_page gains ONE first statement, `if (<gWc.open> && n48_wc_mine(...)) return gfxc_page_wc(...);` - the walk cache,
// reached only inside the calling thread's own open provenance-ask scope (never opened while 98 is OFF: one load). The two 0.0.540
// lines after it are unchanged (tests/gfx_wc98_test.cpp pins all three). Only the body's SOURCE TEXT this hash covers moved.
static const uint32_t kExpectGfxcPage          = 0x1c95c838u;
// build 0.0.540 (switch 96, perf540 T4/T5) DELIBERATE re-baseline (was 0xbef75431u at 0.0.434-0.0.539): gfxsrc_desc_read and
// gfxsrc_pgm_profile each gain ONE first statement, `Pf540Scope pfs(<timer>, <JUDGE sub-tag>);` (OFF: one load; ON: one clock pair and
// the MM sub-tag J_DESC / J_PGMID over the same reads). Nothing else in either body moved; only the SOURCE TEXT these hashes cover.
static const uint32_t kExpectGfxsrcDescRead    = 0x84cc6ad7u;   // 0.0.540 re-baseline (0.0.434's 0xbef75431u; 0xfff25642u at 0.0.431..0.0.433)
// D4-PRIME-FIXES.md item 3(b)/item 4 re-baseline at 0.0.442 (was 0x3243ab56u at 0.0.438-0.0.441):
// right after xlat12_ib_profile_stage resolves a fragment identity, a gate checks whether its ps_table_abi1 names
// P's row (xlat12_table_abi_is_gated) and, if switch 43 is OFF, zeroes it back to 0 - so P is untranslated exactly
// as before that row existed. The reader's OWN behaviour (everything this hash is meant to prove untouched: the
// program-identity read itself) is unchanged; only the body's SOURCE TEXT this hash covers moved.
// build 0.0.470 (an internal design note section 3) re-baseline (was 0xccebeb00u at 0.0.444-0.0.455):
// right after switch 43's gate, a SECOND gate of the same shape - switch 51 zeroes ps_table_abi1 for a row
// xlat12_table_abi_new_shape names (no-sampler / class-10) unless gT51On. The program-identity read itself (what this
// hash is meant to prove untouched) is unchanged; only the body's SOURCE TEXT moved.
// build 0.0.550 (switch 107) DELIBERATE re-baseline (was 0xfdbbdcc5u at 0.0.540-0.0.549, reproduced by this extractor over
// b22035dc): right after switch 35's in-force plane identity line, ONE statement `if (stage == 0u) lr107_note_ps((int)out->ps_id);`
// stamps THIS frame's fragment identity (a frame-tagged store, every mode; tests/gfx_commit_test.cpp 107 R7 pins it). The
// program-identity read itself is unchanged; only the body's SOURCE TEXT this hash covers moved.
static const uint32_t kExpectGfxsrcPgmProfile = 0x3228b271u;   // build 0.0.550 re-baseline (0.0.540's 0xfdbbdcc5u). build 0.0.540 re-baseline (see gfxsrc_desc_read note; was 0x135c597eu). 0.0.444 re-baseline (reviewer): item J changed only the
                                                                  // descriptor-rung evidence block to read the gated ps_table_abi1 for a fragment program.
                                                                  // 0.0.438 re-baseline (was 0x6f264b6au; before that 0x0e09e38bu at 0.0.436-0.0.437):
                                                                  //  - gfxsrc_pgmid_rows_build() is now called
                                                                  // right after `mode` is captured (every mode used to reach
                                                                  // it only through gfxsrc_pgm_profile_id_read, which T+M's
                                                                  // own miss path bypasses entirely);  - the
                                                                  // 1,032-byte local `n48_cg_pagerec localRec` is gone,
                                                                  // replaced by the file-scope gPgmProfileScratchTM (see
                                                                  // AppleHardwareHook.cpp's own comment on it).
                                                                  // (0.0.436 re-baseline was 0x9fc573edu at 0.0.434-0.0.435,
                                                                  // 0x0fdf8ee0u for one build within 0.0.436 itself): mode
                                                                  // T+M (the per-pass memo, an internal design note
                                                                  // design "2. M") is tried before the read, the relocation
                                                                  // scan is skipped on a hit, a miss fills the memo row, and
                                                                  // (reviewer defect fix, post-0.0.436) the fill's pages now
                                                                  // come from a LOCAL n48_cg_pagerec over the resolving
                                                                  // read's own VRAM offsets, never from `va` directly.
static const uint32_t kExpectN48DpRead         = 0x26fac2e5u;
// D4' (an internal design note item 3) re-baseline (0xf9d8a113u at 0.0.431-0.0.438, then 0x6d5e3011u at 0.0.439-
// 0.0.440): the per-draw loop gained the XLAT12_EXTRA_READSET valid-flags bit and one call to d_readset_accum
// (read-only over `out[]`, gated by the flag; the mmprio reader itself is byte-identical either way - only the
// body's SOURCE TEXT this hash covers moved).
// D6 (D4-PRIME-FIXES.md item 6,  (B)) re-baseline at 0.0.441: `xlat12_draw_stats local; if (!ds) ds =
// &local;` is now `if (!ds) return XLAT12_ERR_ARG;` (a grep of every real caller - kext, tools/, tests - found none
// that pass NULL, so this is free; a genuinely NULL caller would now be refused instead of silently using a stack
// local). The mmprio reader's own behaviour is unchanged either way - only the body's SOURCE TEXT this hash covers
// moved, same as every prior re-baseline above.
// D4-PRIME-FIXES.md items 1 and 4 (D4-1, D4-4) re-baseline at 0.0.442 (was 0xab5ab310u at 0.0.441):
// the per-draw loop's READSET accumulation is now split between a table-bound draw and a non-table one
// (d_readset_accum / d_readset_from_table), gained the per-draw DRAW_INDEX_2/DRAW_INDEX_OFFSET_2 index-range
// enumeration, and d_region gained the INDEX_BASE/INDEX_BUFFER_SIZE/INDEX_TYPE latches and the SET_UCONFIG_REG(_INDEX)
// offset-0x243 capture - all gated by XLAT12_EXTRA_READSET, all read-only over `out[]`/`in[]`. The mmprio reader's
// own behaviour is unchanged either way - only the body's SOURCE TEXT this hash covers moved.
// build 0.0.449 re-baseline (was 0x370eef6cu at end of 0.0.448; 0x785c4fe8u at 0.0.442-0.0.447): F1 removed
// the file-scope gUdCarry entirely - the carry is now caller-owned (`ex->ud_carry`, a NEW xlat12_draw_extra
// field), zeroed via a pointer dereference instead of a fixed global's address, and every re-emission read/write
// goes through `ex->ud_carry->ps_val[...]`/`ps_ok` instead of `gUdCarry.val[...]`. F2 replaced
// d_ud_clear_range's ud_seen-clearing with the (unrelated, in d_region, not this function) refusal logic. Item 3
// (F4) extended the re-emission block itself to ALSO walk this program's declared PS and VS ABI pointer pairs
// (xlat12_abi_ptrs.h), not only the table/texture/sampler slots, and added the non-proven CONTEXT_CONTROL
// carry-clear inside d_region's own CONTEXT_CONTROL branch (also not this function). The translator's OWN
// behaviour with the new flag OFF is unchanged (proven by src/xlat12's own OFF-identity tests, T11 among them) -
// only the body's SOURCE TEXT this hash covers moved.
// build 0.0.453 item 5 (inv-f84/REPORT.txt) re-baseline (was 0x85b5acf8u at 0.0.449-0.0.452): right after
// `tbl` is zeroed, a new unconditional-but-cheap check (`if (ex && (ex->flags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD))
// tbl.head_inv_ok = ...`) reads THIS translation's own dword 0 through xlat12_ib_head_acquire_mem_covers, and the
// flags validation gate gained one more bit (XLAT12_EXTRA_DESC_INV_APPLE_HEAD) plus its own "requires
// XLAT12_EXTRA_DESC_INV" ERR_ARG check. Every existing caller (this reader among them) that never sets the new
// flag is byte-identical: the new field defaults to 0 and is read nowhere outside d_table_desc's own `inv`
// computation. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.454 items 1-2 (switch 48, XLAT12_EXTRA_TABLE_REUSE) re-baseline (was 0x0605556fu at 0.0.453): the
// flags gate gained XLAT12_EXTRA_TABLE_REUSE and its own "requires XLAT12_EXTRA_TABLE_DESC" ERR_ARG check; the
// re-emission loop and the REDIRECTED rung each gained a reuse test (d_sh_reuse_ok) that is 0 without the flag. With
// the flag OFF every caller is byte-identical (src/xlat12's own tests; the F84 suite's 0.0.453 output pins, gfx_f84):
// only the body's SOURCE TEXT this hash covers moved.
// build 0.0.455 item 1 (switch 52, 's known-slot rule) re-baseline (was 0x8a168fe1u at 0.0.454): the
// flags validation gate gained one more bit (XLAT12_EXTRA_VS_KNOWN) and its own "requires XLAT12_EXTRA_UD_REEMIT"
// ERR_ARG check (the same "requires its own mechanism" shape every prior flag addition here used); d_region's TWO
// call sites now pass `&tbl` as a new (always-non-NULL) trailing argument, so item 2 (F4)'s carry-clear branch can
// also invalidate switch 48's reusable shadow (tb->sh.valid = 0) - `tbl` is unconditionally constructed whether or
// not XLAT12_EXTRA_TABLE_DESC is set, so this is a plain pointer pass-through, not a new conditional. With the new
// flag OFF, this reader (and every existing caller) is byte-identical: src/xlat12's own OFF-identity tests and the
// gfx_f84 suite's pinned hashes prove it. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.470 (NO-SAMPLER-CLASS10.md sections 1-2) re-baseline (was 0xe12402c1u at 0.0.455): the re-emission
// block's slot list follows d_table_desc's per shape (a class-10 row's textbl pair in place of texture slots, no sampler
// slot for a no-sampler row) and its ABI-pointer walks take their count through xlat12_abi_ptr_n (no silent truncation),
// walking each row's pairs in place instead of copying them into a local array first (the stack: translate_draw_ex's
// frame 0x4e8 -> 0x4c8 with XLAT12_ABI_PTR_MAX 6).
// For every row that existed before 0.0.470 the list is the same slots in the same order (a no-sampler/class-10 row
// exists only from 0.0.470, gated by switches 43 AND 51): src/xlat12's own tests and gfx_f84's pinned hashes prove it.
// Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.480 (an internal design note Q11, XLAT12_EXTRA_UNIT) DELIBERATE re-baseline (was 0x276b9797u at
// 0.0.470-0.0.473): the unit path's sites in the body (the flag's argument check, `U`, the region's output start `oreg`
// at the five "written in this region" comparisons, the block range, P4's call, the pad skipped for a unit, the inline
// invalidate, the draw and tail placement, d_unit_finish, the pool undo on a verify refusal). Every one tests U first;
// with the flag OFF the output is byte-identical (the 160-frame decide44 OFF identity, the builder's report) and
// src/xlat12's own tests and gfx_f84's pinned hashes are unchanged. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.488 (switch 60, XLAT12_EXTRA_DCC_STRIP) DELIBERATE re-baseline (was 0xba15f48fu at 0.0.480-0.0.485, re-verified
// against `git archive 34019fb`): the body gains XLAT12_EXTRA_DCC_STRIP in the accepted-flags mask and one ERR_ARG line (the
// flag without XLAT12_EXTRA_TABLE_DESC). With the flag OFF nothing else in the body runs differently; src/xlat12's own tests
// (T2: every captured T# row byte-identical OFF) and gfx_f84's pinned hashes are unchanged. Only the SOURCE TEXT moved.
// build 0.0.487 (an internal design note, XLAT12_EXTRA_CS_ELIDE, switch 57) DELIBERATE re-baseline (was 0xba15f48fu
// at 0.0.480-0.0.485): the flags validation gate gained one more bit (XLAT12_EXTRA_CS_ELIDE, 0x40000) and its own
// "requires ex->cs_is_n" ERR_ARG check - the same "requires its own mechanism" shape as every prior flag. The elision
// itself lives in d_region (not this body). With the flag OFF the output is byte-identical: 2,449 captured ranges of
// decide49 + run10c (2,413 encoder segments, 36 headless passes), two modes each, against 34019fb's translator (the
// builder's report). Only the body's SOURCE TEXT this hash covers moved.
// MERGE 0.0.489 (0.0.488 + 0.0.487 on 34019fb, then 0.0.486) - the ONE re-baseline that replaces both values above
// (0.0.488's 0x208aec9au and 0.0.487's 0xa0f52b84u each covered only its own edit): the merged body carries BOTH flags in
// the accepted-flags mask (XLAT12_EXTRA_CS_ELIDE 0x40000 and XLAT12_EXTRA_DCC_STRIP 0x80000, the mask comment naming both)
// and BOTH ERR_ARG lines (DCC_STRIP without TABLE_DESC; CS_ELIDE without cs_is_n). 0.0.486 does not touch this body
// (its commit's own body hashes 0xba15f48fu, 34019fb's value). Computed by this file's extractor over the merged tree.
// build 0.0.500 (an internal design note Q4, XLAT12_EXTRA_DRAW_ELIDE, switch 66) DELIBERATE re-baseline (was 0x64ccb872u
// at 0.0.489-0.0.499): the body gains XLAT12_EXTRA_DRAW_ELIDE in the accepted-flags mask (and its comment), one ERR_ARG line
// (the flag without TABLE_DESC or with no/unknown row class), one store of dcc_unproven before the table step, the elide's
// branch at the table step's refusal (d_draw_elide / d_draw_elide_emit, both outside this body) and the backstop after the
// verify (xlat12_ib_de_backstop, outside it). With the flag OFF none of them runs: the OFF identity over every segment of run10g's
// captured frames (626 translations, fu_head vs fu_new, output hashes identical) and src/xlat12's own tests are unchanged.
// Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.502 (the VRS-centre write) DELIBERATE re-baseline (was 0xdf565b25u at 0.0.500-0.0.501,
// re-verified by this extractor over c7d8ba3's xlat12_ib.c): the body's one d_rings call passes a new trailing argument
// (`pair.vrs_state == XLAT12_VRS_PREFOLLOW`) and region 0's close sets `pair.vrs_state = XLAT12_VRS_CLOSED`, so that gfx12
// DB_SPI_VRS_CENTER_LOCATION (0x28068) = 0 - mesa's gfx12 preamble value, ac_cmdbuf.c:702 - is written exactly once per
// translation with a ring, before its first draw. The mmprio reader's own behaviour is unchanged; only the body's SOURCE
// TEXT this hash covers moved (the output change itself is pinned, with its restore check, in src/xlat12's own tests and
// gfx_f84).
// build 0.0.512 (switch 66 M 7, XLAT12_DE_CLASS_GLASS) DELIBERATE re-baseline (was 0x6ead4231u at 0.0.502-0.0.511, reproduced
// by a copy of this extractor over dc79e1a4's xlat12_ib.c): the table step's pre-call store under the flag gained `ds->de_nl = 0u`
// and the flag check accepts the GLASS class bit. The mmprio reader is untouched; with the flag OFF (or at M 3) nothing new runs
// (harness OFF identity over run10t/run10s, per-unit output hashes identical). Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.522 (xlat12_unit.spill, switch 76) DELIBERATE re-baseline (was 0x859912b4u at 0.0.512-0.0.521, reproduced
// by this extractor over 0f746f3c's xlat12_ib.c): the body's two verify refusals (draw_verify, the elide's backstop) also undo the
// unit's spill tier (`if (U && U->spill) (void)xlat12_pool_undo(U->spill);`). With U->spill NULL (every unit while 76 is OFF) the added
// test is false and nothing else in the body moved: the harness OFF identity over run10t/u/v (whole outputs identical, 6 runs) and
// src/xlat12's own tests are unchanged. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.535 ( fix 1, XLAT12_EXTRA_NCLEAR, switch 91; item 4, xlat12_tex_state) DELIBERATE re-baseline (was
// 0xd4999147u at 0.0.522-0.0.534, reproduced by this extractor over 48740edf's xlat12_ib.c): the body gains XLAT12_EXTRA_NCLEAR in the
// accepted-flags mask, one ERR_ARG line (the flag without CS_ELIDE), the tex_state zeroing (only with a non-NULL pointer), and its
// verify call becomes d_verify(..., fill) (fill 0 without the flag = xlat12_ib_draw_verify's own walk). With the flag OFF and no
// tex_state nothing new runs: tests/gfx_nclear_test.cpp T3 compares today's outputs with the FROZEN 48740edf translator over run11v's
// real N segments (no extra block and 57 alone, whole-output FNV identical) and src/xlat12's own tests are unchanged. Only the body's
// SOURCE TEXT this hash covers moved.
// build 0.0.536 (XLAT12_EXTRA_RECT2D, switch 92; fix round item 1, the stack) DELIBERATE re-baseline (was 0x04f5e67cu
// at 0.0.535, reproduced by this extractor over 00ff5906's xlat12_ib.c): the body now sits in ONE block that can run twice - every
// `return v;` becomes D_R2D_RET(v) (store, leave the block), and after the block, only when a flagged pass was refused after it
// wrote or found no room, the block runs again with the flag's pass variable 0 (today's translation) - so the fallback costs no
// second frame (its state: two locals, r2dCtl and r2dSt). Besides that respelling the body gains: XLAT12_EXTRA_RECT2D in the accepted-flags mask, one ERR_ARG line (the flag
// with APPLE_OUTPRIM / SYNTH_IDXPRIM), one d_r2d_draw call per draw under `if (r2d)`, one backstop under `if (r2d)`. With the flag
// OFF nothing new runs: tests/gfx_rect92_test.cpp T3 compares today's outputs with the FROZEN 00ff5906 translator over real
// run11y/run11v segments (three modes, whole-output FNV identical). Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.537 (XLAT12_EXTRA_PWS, switch 93) DELIBERATE re-baseline (was 0x0929bb91u at 0.0.536): the body gains
// one local (pwsCtl: bit 0 the pass converts, then the fallback bit and the flagged pass's status), XLAT12_EXTRA_PWS in the
// accepted-flags mask, one line setting pair.pws after the pair's zeroing, and at d_r2d_done - BEFORE 92's own fallback - the PWS
// fallback (a flagged pass refused after it converted runs the same block again with the conversion off) and the two stats it sets.
// With the flag OFF nothing new runs: tests/gfx_pws93_test.cpp T3 and tools/test_pws93_corpus.py C1 compare today's outputs with the
// FROZEN 12e011c7 translator (whole-output FNV). Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.540 item 5 (XLAT12_EXTRA_TBLCACHE, switch 97) DELIBERATE re-baseline (was 0x0d1c7b17u at 0.0.537-0.0.539, reproduced
// by this extractor over 71f805d8's xlat12_ib.c): the accepted-flags mask gains XLAT12_EXTRA_TBLCACHE and the d_verify call passes the
// flag's bit (d_verify then asks the sorted cache instead of d_table_g12's linear walk - the same answers). With the flag OFF nothing
// new runs: tools/test_tc97_corpus.py compares ON with OFF over the whole captured corpus (whole-output FNV, 7 modes, the real stages)
// and tests/gfx_tc97_test.cpp checks the cache against the walk exhaustively. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.547 item 3 DELIBERATE re-baseline (was 0x8c0d4f05u at 0.0.540-0.0.546, reproduced by a copy of
// this extractor over 42e66725's xlat12_ib.c): the body gains two locals (r2dOnA/r2dOnB), one line on 92's fallback branch storing the
// flagged pass's err_op/err_reg/err_in_dword before its goto, and one line restoring them into the new stats r2d_on_op/_reg/_dw beside
// r2d_fallback. Nothing runs without the flag, and the fallback's output is unchanged (tests/gfx_rect92_test.cpp T3/T4b/E2 against the
// FROZEN translator). Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.548 item B (the 0.0.547 review's SHOULD-FIX: the 0xD3 backstop left err_in_dword 0) DELIBERATE re-baseline (was
// 0x56d5fc9bu at 0.0.547, reproduced by a copy of this extractor over 01131813's xlat12_ib.c): the body gains one local (r2dAt) and
// the flagged pass's backstop calls xlat12_ib_rect2d_check_at (the same walk, also answering WHERE) and stores that position in
// err_in_dword. Only a refused flagged pass reaches that line; its output is refused either way. tests/gfx_rect92_test.cpp T4/E3/F6
// check the position. Only the body's SOURCE TEXT this hash covers moved.
// build 0.0.552 ( PLAN (2), XLAT12_DE_CLASS_AN / _AN_SHADOW, switch 110) DELIBERATE re-baseline (was 0x306dd8a8u at
// 0.0.548-0.0.551, reproduced by a copy of this extractor over 72095986's xlat12_ib.c): the flag's row-class check accepts the two AN
// bits (never both), and the table step asks d_de_an (outside this body) before d_table_desc under the flag, emitting the draw's NOP
// through d_draw_elide_emit exactly as the refusal-side elide does. Without an AN bit d_de_an answers 0 at its first line; with the
// flag OFF nothing new runs (tests/gfx_f84_test.cpp AN552: 110 OFF and SHADOW outputs identical). Only the body's SOURCE TEXT moved.
static const uint32_t kExpectXlat12Translate   = 0xa216d989u;

static void check_hash(const char *path, const char *name, uint32_t expect_val)
{
    std::string text;
    if (!read_file(path, text)) { std::printf("SKIP  T6: could not read %s\n", path); return; }
    std::string body;
    if (!extract_body(text, name, body)) { std::printf("SKIP  T6: %s not found in %s\n", name, path); return; }
    const uint32_t got = fnv1a32(body);
    char what[256];
    std::snprintf(what, sizeof what, "T6 %s: FNV32 matches 0.0.431 (%#010x)", name, expect_val);
    expect(what, got == expect_val);
}

static void test_T6(const char *ahhPath, const char *dpPath, const char *xlatPath)
{
    // Non-vacuity FIRST: mutate one token of a real extracted body and prove the hash moves.
    {
        std::string mutant = "{ int x = 1; return x; }";
        std::string mutant2 = mutant;
        mutant2[7] = '2';   // "change one token", brief's own named break
        expect("T6 mutant (one token changed) CAUGHT: hash differs", fnv1a32(mutant) != fnv1a32(mutant2));
    }
    if (ahhPath) {
        check_hash(ahhPath, "gfxc_read", kExpectGfxcRead);
        check_hash(ahhPath, "gfxc_page", kExpectGfxcPage);
        check_hash(ahhPath, "gfxsrc_desc_read", kExpectGfxsrcDescRead);
        check_hash(ahhPath, "gfxsrc_pgm_profile", kExpectGfxsrcPgmProfile);
    } else {
        std::printf("SKIP  T6: no AppleHardwareHook.cpp path given (4 checks skipped)\n");
    }
    if (dpPath) check_hash(dpPath, "n48_dp_read", kExpectN48DpRead);
    else std::printf("SKIP  T6: no gfx_desc_port.h path given\n");
    if (xlatPath) check_hash(xlatPath, "xlat12_ib_translate_draw_ex", kExpectXlat12Translate);
    else std::printf("SKIP  T6: no xlat12_ib.c path given\n");
}

// =============================================================================================================
// T7 — worst-case line length. Break: widen a field.
// =============================================================================================================
static void test_T7(void)
{
    char buf[1024];
    n48_mmprio_stats worst {};
    worst.owner_wait_us = UINT64_MAX; worst.owner_acquires = UINT64_MAX; worst.owner_max_us = UINT64_MAX;
    worst.yield_count = UINT64_MAX; worst.yield_us = UINT64_MAX; worst.yield_max_us = UINT64_MAX; worst.bound_hits = UINT64_MAX;
    int n = std::snprintf(buf, sizeof buf, N48_MMPRIO_FMT, N48_MMPRIO_ARGS(&worst));
    std::printf("T7 mmprio: line at widest numerics: %d bytes\n", n);
    expect("T7 mmprio: line stays under n48log's 512-byte body cap", n > 0 && (size_t)n < 512u);

    const char *longestHow = " - REFUSED (unknown M), unchanged";
    n = std::snprintf(buf, sizeof buf, N48_MMPRIO_SWITCH_FMT, "OFF (default)", longestHow);
    std::printf("T7 mmprio-sw: line at widest text: %d bytes\n", n);
    expect("T7 mmprio-sw: line stays under n48log's 512-byte body cap", n > 0 && (size_t)n < 512u);

    // MUTANT: widen a field (append an extra, needless clause) — must be CAUGHT by the same 512-byte assertion,
    // proving the check is not vacuously true for any format string.
    {
        std::string widened = std::string(N48_MMPRIO_SWITCH_FMT) +
            " Also, for good measure, here is a much longer sentence appended only to make this line far too wide, "
            "repeated: for good measure, here is a much longer sentence appended only to make this line far too "
            "wide, repeated again for good measure to push it well past five hundred and twelve bytes of body.";
        n = std::snprintf(buf, sizeof buf, widened.c_str(), "OFF (default)", longestHow);
        expect("T7 mutant (widened field) CAUGHT: exceeds the 512-byte cap", n > 0 && (size_t)n >= 512u);
    }
}

int main(int argc, char **argv)
{
    const char *bringupPath = argc > 1 ? argv[1] : nullptr;
    const char *ahhPath     = argc > 2 ? argv[2] : nullptr;
    const char *dpPath      = argc > 3 ? argv[3] : nullptr;
    const char *xlatPath    = argc > 4 ? argv[4] : nullptr;

    test_T1();
    test_T2();
    test_T3();
    test_T4();
    test_T5(bringupPath, ahhPath);
    test_T6(ahhPath, dpPath, xlatPath);
    test_T7();

    std::printf("\ngfx_mmprio: %d check(s), %d failed\n", gRun, gFail);
    if (gFail) { std::printf("gfx_mmprio: FAIL\n"); return 1; }
    std::printf("gfx_mmprio: N48-MMPRIO-TEST-PASS\n");
    return 0;
}
