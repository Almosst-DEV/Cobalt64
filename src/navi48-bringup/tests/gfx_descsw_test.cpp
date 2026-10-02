// gfx_descsw_test.cpp — build 0.0.454 item 6 (the reviewer's addition): THE KEXT'S OWN FLAG-SETTING SEQUENCE for
// the descriptor-port refinement switches 48 (XLAT12_EXTRA_TABLE_REUSE, this build) and 49
// (XLAT12_EXTRA_DESC_INV_APPLE_HEAD, 0.0.453), driven, not pinned.
//
// WHY: the 0.0.453 break round planted G1 - switch 49's guard `if (gDescInvAppleHeadOn) {` rewritten to `if (1) {`, i.e.
// the flag set on every segment whatever the switch says - and NO suite caught it (desc_port, mmprio and
// gfx_rasterarm_pd all passed). A default-OFF switch whose flag is set with the switch OFF silently changes hardware
// output on every boot. This suite reads AppleHardwareHook.cpp (argv[1]) with comments and string literals blanked,
// finds the ONE statement that ORs each flag into `ex.flags`, and then RUNS that statement's own guard: it evaluates
// the guard expression, as written in the kext, for the switch OFF and ON (a small evaluator over the guard's tokens:
// the switch's global, integer literals, `!`, `&&`, `||`, parentheses; anything else is unknown and fails). OFF must
// leave the flag clear and ON must set it. It also drives the verb's own mode rule (n48_ra_set, gfx_rasterarm.h - the
// function the kext calls) through every mode byte, checks the global is declared OFF and written by exactly that one
// verb, and checks the statement sits where the policy's real order of state needs it: inside gfxsrc_policy, after the
// descriptor port's own TABLE_DESC flags are set and before the segment's translate call. Each rule then re-runs
// against MUTATED copies of the source - the G1 break itself for 49 and 48, the switch declared ON, the statement
// moved outside the descriptor-port block - and must FAIL on every one.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 src/navi48-bringup/tests/gfx_descsw_test.cpp \
//         -o /tmp/descsw && /tmp/descsw src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_rasterarm.h"   // n48_ra_set: the verb's mode rule, the same function the kext's 48/49/50 handlers call

static int gFail = 0, gRun = 0;
static void expect(const std::string &what, bool ok)
{
    gRun++;
    if (!ok) { gFail++; printf("  FAIL %s\n", what.c_str()); }
    else printf("  ok   %s\n", what.c_str());
}

// comments and string/char literals replaced by spaces (newlines kept), so a phrase in a comment or a log string
// can never stand in for code
static std::string strip(const std::string &s)
{
    std::string o = s;
    size_t i = 0;
    while (i < o.size()) {
        if (o.compare(i, 2, "//") == 0) { while (i < o.size() && o[i] != '\n') o[i++] = ' '; continue; }
        if (o.compare(i, 2, "/*") == 0) {
            o[i] = o[i + 1] = ' '; i += 2;
            while (i < o.size() && o.compare(i, 2, "*/") != 0) { if (o[i] != '\n') o[i] = ' '; i++; }
            if (i < o.size()) { o[i] = o[i + 1] = ' '; i += 2; }
            continue;
        }
        if (o[i] == '"' || o[i] == '\'') {
            const char q = o[i++];
            while (i < o.size() && o[i] != q) { if (o[i] == '\\') { o[i++] = ' '; } if (i < o.size() && o[i] != '\n') o[i] = ' '; i++; }
            i++;
            continue;
        }
        i++;
    }
    return o;
}
static size_t count_of(const std::string &h, const std::string &n)
{
    size_t c = 0;
    for (size_t p = h.find(n); p != std::string::npos; p = h.find(n, p + n.size())) c++;
    return c;
}

// ---- the guard evaluator ------------------------------------------------------------------------------------------------
// build 0.0.456 item 2: switch 52's guard grew a second variable (`gVsKnownOn && gXdUdReemitOn`), so
// the evaluator takes a MAP of variable -> value rather than one pinned name, exactly as general as the guard syntax
// it already parses (`!`, `&&`, `||`, parentheses). A single-entry map is byte-for-byte the old one-variable shape,
// so switches 48/49 (and 52's own on/off rules, which pin the SECOND variable while flipping the first) are unchanged.
typedef std::vector<std::pair<std::string, int>> Vars;
struct Ev { std::vector<std::string> t; size_t i; const Vars *vars; bool bad; };
static std::vector<std::string> lex(const std::string &e)
{
    std::vector<std::string> t;
    for (size_t i = 0; i < e.size();) {
        const char c = e[i];
        if (c == ' ' || c == '\t' || c == '\n') { i++; continue; }
        if (isalnum((unsigned char)c) || c == '_') { size_t j = i; while (j < e.size() && (isalnum((unsigned char)e[j]) || e[j] == '_')) j++; t.push_back(e.substr(i, j - i)); i = j; continue; }
        if ((c == '&' || c == '|') && i + 1 < e.size() && e[i + 1] == c) { t.push_back(e.substr(i, 2)); i += 2; continue; }
        t.push_back(std::string(1, c)); i++;
    }
    return t;
}
static int ev_or(Ev &v);
static int ev_prim(Ev &v)
{
    if (v.i >= v.t.size()) { v.bad = true; return 0; }
    const std::string tok = v.t[v.i++];
    if (tok == "!") return !ev_prim(v);
    if (tok == "(") { const int r = ev_or(v); if (v.i >= v.t.size() || v.t[v.i++] != ")") v.bad = true; return r; }
    for (const auto &kv : *v.vars) if (tok == kv.first) return kv.second;
    if (tok == "true") return 1;
    if (tok == "false") return 0;
    char *end = nullptr; const unsigned long n = strtoul(tok.c_str(), &end, 0);
    if (end && (*end == 0 || !strcmp(end, "u") || !strcmp(end, "U"))) return n != 0;
    v.bad = true;   // any other identifier: the guard depends on something this suite cannot vouch for
    return 0;
}
static int ev_and(Ev &v) { int r = ev_prim(v); while (v.i < v.t.size() && v.t[v.i] == "&&") { v.i++; const int b = ev_prim(v); r = r && b; } return r; }
static int ev_or(Ev &v) { int r = ev_and(v); while (v.i < v.t.size() && v.t[v.i] == "||") { v.i++; const int b = ev_and(v); r = r || b; } return r; }
// value of guard `cond` under `vars`; -1 when it cannot be evaluated (an identifier not in `vars`, or a syntax error)
static int eval_guard(const std::string &cond, const Vars &vars)
{
    Ev v { lex(cond), 0u, &vars, false };
    const int r = ev_or(v);
    if (v.bad || v.i != v.t.size()) return -1;
    return r ? 1 : 0;
}
// convenience for the single-variable case (48/49, and 52's own on/off flip)
static int eval_guard(const std::string &cond, const std::string &var, int val)
{
    return eval_guard(cond, Vars { { var, val } });
}

// ---- one switch -------------------------------------------------------------------------------------------------------
// build 0.0.456 item 2: `global2`, DEFAULT nullptr - switch 52's own second precondition
// (gXdUdReemitOn, switch 44). nullptr for 48/49 (unchanged, single-variable guards).
struct Sw { int num; const char *global; const char *flag; bool inDp; const char *global2 = nullptr; };
// every rule for switch `w` over (stripped) source `s`; returns the number of rules that failed, printing each when loud
static int rules(const std::string &s, const Sw &w, bool loud)
{
    int bad = 0;
    auto chk = [&](const std::string &what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    const std::string pre = "switch " + std::to_string(w.num) + ": ";
    const std::string stmt = std::string("ex.flags |= ") + w.flag + ";";
    const size_t n = count_of(s, stmt);
    chk(pre + "exactly ONE statement ORs " + w.flag + " into ex.flags (found " + std::to_string(n) + ")", n == 1u);
    const size_t at = s.find(stmt);
    std::string cond;
    if (at != std::string::npos) {
        // the guard: the `if (` ... `) {` that opens the block holding the statement, on the same line
        const size_t ls = s.rfind('\n', at) + 1u;
        const size_t ifp = s.find("if (", ls);
        if (ifp != std::string::npos && ifp < at) {
            size_t j = ifp + 3u; int depth = 0; size_t open = j;
            for (; j < at; j++) { if (s[j] == '(') { if (!depth) open = j; depth++; } else if (s[j] == ')') { if (!--depth) break; } }
            if (j < at) cond = s.substr(open + 1u, j - open - 1u);
        }
    }
    chk(pre + "the statement has a guard on its own line: `if (" + cond + ")`", !cond.empty());
    // build 0.0.456 item 2: when the switch has a second precondition (52's gXdUdReemitOn), the
    // OWN-switch OFF/ON checks pin it ON, matching the ORIGINAL question ("with the other precondition satisfied,
    // does this switch's own flip work") - and a THIRD check below asks the NEW question the fix exists for.
    const Vars v0 = w.global2 ? Vars { { w.global, 0 }, { w.global2, 1 } } : Vars { { w.global, 0 } };
    const Vars v1 = w.global2 ? Vars { { w.global, 1 }, { w.global2, 1 } } : Vars { { w.global, 1 } };
    const int off = cond.empty() ? -1 : eval_guard(cond, v0), on = cond.empty() ? -1 : eval_guard(cond, v1);
    chk(pre + "the guard, EVALUATED with the switch OFF, leaves the flag clear (guard = " + std::to_string(off) + ")", off == 0);
    chk(pre + "the guard, EVALUATED with the switch ON, sets the flag (guard = " + std::to_string(on) + ")", on == 1);
    if (w.global2) {
        //, THE REGRESSION THIS BUILD FIXES: 52 ON with 44 (gXdUdReemitOn) OFF must be INERT (guard 0), not
        // fail-closed-by-ERR_ARG. Through 0.0.455 this `if` read `gVsKnownOn` alone, so this check would have read
        // guard = 1 here - the exact bug found.
        const int cross = cond.empty() ? -1 : eval_guard(cond, Vars { { w.global, 1 }, { w.global2, 0 } });
        chk(pre + "the guard, EVALUATED with the switch ON and " + w.global2 + " OFF, leaves the flag clear (INERT, guard = " +
            std::to_string(cross) + ")", cross == 0);
    }
    // the global: declared OFF, written by the one verb only, through n48_ra_set
    const std::string decl = std::string("static volatile uint32_t ") + w.global + " { 0u };";
    chk(pre + "the global is declared exactly once, OFF (" + decl + ")", count_of(s, decl) == 1u);
    size_t writers = 0;
    for (size_t p = s.find(w.global); p != std::string::npos; p = s.find(w.global, p + 1u)) {
        size_t q = p + strlen(w.global);
        while (q < s.size() && s[q] == ' ') q++;
        if (q < s.size() && s[q] == '=' && (q + 1u >= s.size() || s[q + 1u] != '=')) writers++;
    }
    chk(pre + "exactly one assignment writes the global (" + std::to_string(writers) + ")", writers == 1u);
    const std::string sel = "(arg & 0xffull) == " + std::to_string(w.num) + "ull)";
    const size_t hs = s.find(sel), hn = hs == std::string::npos ? hs : s.find("} else if (", hs);
    const size_t wr = s.find(std::string("if (changed) ") + w.global + " = ");
    const size_t rs = hs == std::string::npos ? hs : s.find("n48_ra_set(m, &", hs);
    chk(pre + "that assignment is `if (changed) G = ...` inside the verb's own selector, after its n48_ra_set",
        hs != std::string::npos && wr != std::string::npos && rs != std::string::npos && hs < rs && rs < wr && wr < hn &&
        count_of(s, sel) == 1u);
    // the real order of state inside gfxsrc_policy
    const size_t pol = s.find("gfxsrc_policy(const GfxcVm");
    const size_t dpf = s.find("ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;");
    const size_t xl = s.find("xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from]");
    if (w.inDp) {
        // the descriptor port's block: from its `if (dp) {` to the matching close brace
        size_t dpo = dpf == std::string::npos ? dpf : s.rfind("if (dp) {", dpf), dpc = std::string::npos;
        if (dpo != std::string::npos) { int d = 0; for (size_t j = s.find('{', dpo); j < s.size(); j++) { if (s[j] == '{') d++; else if (s[j] == '}' && !--d) { dpc = j; break; } } }
        chk(pre + "inside gfxsrc_policy's `if (dp)` block, after TABLE_DESC | DESC_INV are set, before the translate call",
            pol != std::string::npos && dpo != std::string::npos && dpc != std::string::npos && at != std::string::npos &&
            xl != std::string::npos && pol < dpo && dpf < at && at < dpc && dpc < xl);
    } else {
        chk(pre + "inside gfxsrc_policy, before the translate call",
            pol != std::string::npos && at != std::string::npos && xl != std::string::npos && pol < at && at < xl);
    }
    return bad;
}

// ---- build 0.0.486: switch 59's wiring of the T#-aware ask, driven the same way ---------------------------------------
// The statement `ex.desc_tiled_okt = &gfxsrc_desc_tiled_okt;` (the ONLY way the translator ever sees the T#-aware ask) must be
// guarded by switch 59 AND switch 11, evaluated OFF / ON / 59-without-11; the global is declared OFF and written by exactly one
// assignment, inside selector 59, after its n48_ra_set; the statement sits in gfxsrc_policy's `if (dp)` block, after the plain
// ask is wired and before the translate call. Returns the rules that failed.
static int rules_okt(const std::string &s, bool loud)
{
    int bad = 0;
    auto chk = [&](const std::string &what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    const std::string pre = "switch 59: ";
    const std::string stmt = "ex.desc_tiled_okt = &gfxsrc_desc_tiled_okt;";
    const size_t n = count_of(s, stmt);
    chk(pre + "exactly ONE statement wires desc_tiled_okt (found " + std::to_string(n) + ")", n == 1u);
    // build 0.0.544 DELIBERATE re-baseline: the one other assignment is switch 103's st103_wire, an identity-checked swap of
    // THIS callback for its wrapper (the line holds the substring twice: the `==` identity check and the assignment), only while 103
    // is ON / SHADOW (tests/gfx_stale103_test.cpp S14 pins it).
    const std::string swap103 = "if (e->desc_tiled_okt == static_cast<TiledTFn>(&gfxsrc_desc_tiled_okt)) e->desc_tiled_okt = &st103_tiled_okt;";
    chk(pre + "nothing else assigns desc_tiled_okt (but switch 103's identity-checked swap)",
        count_of(s, "desc_tiled_okt =") == 1u + 2u * count_of(s, swap103) && count_of(s, swap103) <= 1u);
    const size_t at = s.find(stmt);
    std::string cond;
    if (at != std::string::npos) {
        const size_t ls = s.rfind('\n', at) + 1u;
        const size_t ifp = s.find("if (", ls);
        if (ifp != std::string::npos && ifp < at) {
            size_t j = ifp + 3u; int depth = 0; size_t open = j;
            for (; j < at; j++) { if (s[j] == '(') { if (!depth) open = j; depth++; } else if (s[j] == ')') { if (!--depth) break; } }
            if (j < at) cond = s.substr(open + 1u, j - open - 1u);
        }
    }
    chk(pre + "the statement has a guard on its own line: `if (" + cond + ")`", !cond.empty());
    const int off = cond.empty() ? -1 : eval_guard(cond, Vars { { "gXdResProvLin", 0 }, { "gXdResProv", 1 } });
    const int on = cond.empty() ? -1 : eval_guard(cond, Vars { { "gXdResProvLin", 1 }, { "gXdResProv", 1 } });
    const int no11 = cond.empty() ? -1 : eval_guard(cond, Vars { { "gXdResProvLin", 1 }, { "gXdResProv", 0 } });
    chk(pre + "the guard, EVALUATED with 59 OFF (11 on), leaves the ask unwired (guard = " + std::to_string(off) + ")", off == 0);
    chk(pre + "the guard, EVALUATED with 59 ON and 11 ON, wires it (guard = " + std::to_string(on) + ")", on == 1);
    chk(pre + "the guard, EVALUATED with 59 ON and 11 OFF, leaves it unwired (guard = " + std::to_string(no11) + ")", no11 == 0);
    const std::string decl = "static volatile uint32_t gXdResProvLin { 0u };";
    chk(pre + "the global is declared exactly once, OFF", count_of(s, decl) == 1u);
    size_t writers = 0;
    for (size_t p = s.find("gXdResProvLin"); p != std::string::npos; p = s.find("gXdResProvLin", p + 1u)) {
        size_t q = p + strlen("gXdResProvLin");
        while (q < s.size() && s[q] == ' ') q++;
        if (q < s.size() && s[q] == '=' && (q + 1u >= s.size() || s[q + 1u] != '=')) writers++;
    }
    chk(pre + "exactly one assignment writes the global (" + std::to_string(writers) + ")", writers == 1u);
    const std::string sel = "(arg & 0xffull) == 59ull)";
    const size_t hs = s.find(sel), hn = hs == std::string::npos ? hs : s.find("} else if (", hs);
    const size_t wr = s.find("if (changed59) gXdResProvLin = flin;");
    const size_t rs = hs == std::string::npos ? hs : s.find("n48_ra_set(m, &flin)", hs);
    chk(pre + "that assignment is `if (changed59) gXdResProvLin = flin;` inside selector 59, after its n48_ra_set",
        hs != std::string::npos && wr != std::string::npos && rs != std::string::npos && hs < rs && rs < wr && wr < hn &&
        count_of(s, sel) == 1u);
    const size_t pol = s.find("gfxsrc_policy(const GfxcVm");
    const size_t plain = s.find("ex.desc_tiled_ok = &gfxsrc_desc_tiled_ok;");
    const size_t xl = s.find("xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from]");
    chk(pre + "inside gfxsrc_policy, after the plain ask is wired, before the translate call",
        pol != std::string::npos && plain != std::string::npos && at != std::string::npos && xl != std::string::npos &&
        pol < plain && plain < at && at < xl);
    return bad;
}

int main(int argc, char **argv)
{
    printf("gfx_descsw (0.0.454 item 6): switches 48 and 49's flag-setting sequence, driven\n");
    // the verb's mode rule itself: M 1 ON, M 2 OFF, anything else changes nothing
    { uint32_t f = 7u;
      expect("n48_ra_set: M 1 sets ON", n48_ra_set(1u, &f) == 1 && f == 1u);
      expect("n48_ra_set: M 2 sets OFF", n48_ra_set(2u, &f) == 1 && f == 0u);
      f = 5u; expect("n48_ra_set: a bare read (M 0) changes nothing", n48_ra_set(0u, &f) == 0 && f == 5u);
      expect("n48_ra_set: an unknown M (3, 0xFF) changes nothing", n48_ra_set(3u, &f) == 0 && n48_ra_set(0xFFu, &f) == 0 && f == 5u); }
    // the evaluator on its own
    expect("evaluator: `G` follows G", eval_guard("G", "G", 0) == 0 && eval_guard("G", "G", 1) == 1);
    expect("evaluator: `1` is always true (the G1 break's own guard)", eval_guard("1", "G", 0) == 1);
    expect("evaluator: an unknown identifier is refused", eval_guard("G && other", "G", 1) == -1);
    if (argc < 2) { printf("  FAIL: no AppleHardwareHook.cpp argument - THE CALL SITES ARE NOT CHECKED\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("  FAIL: cannot read %s\n", argv[1]); return 1; }
    std::string raw; { char b[65536]; size_t k; while ((k = fread(b, 1, sizeof b, fp)) > 0) raw.append(b, k); } fclose(fp);
    const std::string src = strip(raw);
    const Sw k48 { 48, "gTblReuseOn", "XLAT12_EXTRA_TABLE_REUSE", true };
    const Sw k49 { 49, "gDescInvAppleHeadOn", "XLAT12_EXTRA_DESC_INV_APPLE_HEAD", true };
    // build 0.0.455 item 1 ('s known-slot rule): switch 52. build 0.0.456 item 2:
    // the guard is now TWO variables (`gVsKnownOn && gXdUdReemitOn`), not the same single-global shape as 48/49.
    const Sw k52 { 52, "gVsKnownOn", "XLAT12_EXTRA_VS_KNOWN", true, "gXdUdReemitOn" };
    const Sw kSw[3] = { k48, k49, k52 };
    for (const Sw &w : kSw) rules(src, w, true);
    // PLANTED BREAKS, each on a copy of the REAL source text, each must make at least one rule fail
    struct Plant { const Sw *w; const char *what; std::string from, to; };
    const std::vector<Plant> plants = {
        { &k49, "G1 (the 0.0.453 round's NOT CAUGHT break): switch 49's guard -> `if (1)`",
          "if (gDescInvAppleHeadOn) { ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;", "if (1) { ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;" },
        { &k48, "the same break for switch 48: `if (1)`",
          "if (gTblReuseOn) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE;", "if (1) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE;" },
        { &k48, "switch 48's guard inverted: `if (!gTblReuseOn)`",
          "if (gTblReuseOn) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE;", "if (!gTblReuseOn) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE;" },
        { &k49, "switch 49 declared ON", "static volatile uint32_t gDescInvAppleHeadOn { 0u };", "static volatile uint32_t gDescInvAppleHeadOn { 1u };" },
        { &k48, "switch 48 declared ON", "static volatile uint32_t gTblReuseOn { 0u };", "static volatile uint32_t gTblReuseOn { 1u };" },
        { &k48, "switch 48's flag set a SECOND time, unguarded, after the block",
          "if (gTblReuseOn) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE; gTblReuseS.segsOn++; }",
          "if (gTblReuseOn) { ex.flags |= XLAT12_EXTRA_TABLE_REUSE; gTblReuseS.segsOn++; } ex.flags |= XLAT12_EXTRA_TABLE_REUSE;" },
        { &k48, "switch 48 written by a second assignment", "if (changed) gTblReuseOn = ftr;", "if (changed) gTblReuseOn = ftr; gTblReuseOn = 1u;" },
        { &k52, "the G1 break for switch 52: `if (1)`",
          "if (gVsKnownOn && gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;", "if (1) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;" },
        { &k52, "switch 52's guard inverted: `if (!gVsKnownOn && gXdUdReemitOn)`",
          "if (gVsKnownOn && gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;", "if (!gVsKnownOn && gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;" },
        { &k52, "switch 52 declared ON", "static volatile uint32_t gVsKnownOn { 0u };", "static volatile uint32_t gVsKnownOn { 1u };" },
        { &k52, "switch 52 written by a second assignment", "if (changed) gVsKnownOn = fvk;", "if (changed) gVsKnownOn = fvk; gVsKnownOn = 1u;" },
        // build 0.0.456 item 2 - THE REGRESSION THIS BUILD FIXES: the `&& gXdUdReemitOn` clause
        // deleted, exactly 0.0.455's own bug (52 ON with 44 OFF then sets VS_KNOWN alone, which xlat12_ib.c's own
        // ERR_ARG guard refuses for EVERY descriptor segment - fail-closed, but not INERT as the comment claimed).
        { &k52, "0.0.456's own fix REGRESSED: `&& gXdUdReemitOn` deleted (52 with 44 off is no longer inert)",
          "if (gVsKnownOn && gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;", "if (gVsKnownOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;" },
        // build 0.0.456 item 2: the AND weakened to OR - 52 alone (44 off) would set the flag, the same bug
        // by a different route.
        { &k52, "switch 52's AND weakened to OR: `if (gVsKnownOn || gXdUdReemitOn)`",
          "if (gVsKnownOn && gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;", "if (gVsKnownOn || gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_VS_KNOWN;" },
    };
    for (const Plant &p : plants) {
        std::string mut = raw;
        const size_t at = mut.find(p.from);
        if (at == std::string::npos || count_of(mut, p.from) != 1u) { expect(std::string("plant setup: found once: ") + p.from, false); continue; }
        mut.replace(at, p.from.size(), p.to);
        expect(std::string("BREAK-check: ") + p.what + " - caught", rules(strip(mut), *p.w, false) > 0);
    }
    // moving switch 49's statement OUT of the descriptor-port block (to just before `if (dp) {`) is caught by ordering
    { std::string mut = raw;
      const std::string st49 = "if (gDescInvAppleHeadOn) { ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD; gDescInvAppleHeadS.segsOn++; }";
      const size_t a = mut.find(st49), dpf = mut.find("ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;");
      const size_t dpo = dpf == std::string::npos ? dpf : mut.rfind("if (dp) {", dpf);
      if (a != std::string::npos && dpo != std::string::npos && dpo < a) {
          mut.erase(a, st49.size());
          mut.insert(dpo, st49 + "\n        ");
          expect("BREAK-check: switch 49's statement moved before `if (dp) {` - caught", rules(strip(mut), k49, false) > 0);
      } else expect("plant setup: switch 49's statement and the dp block found", false); }
    // build 0.0.486: switch 59, the rules and their planted breaks on copies of the REAL source
    rules_okt(src, true);
    { struct { const char *what, *from, *to; } okp[] = {
          { "switch 59's guard -> `if (1)`", "if (gXdResProvLin && gXdResProv) ex.desc_tiled_okt", "if (1) ex.desc_tiled_okt" },
          { "switch 59's guard drops switch 11: `if (gXdResProvLin)`", "if (gXdResProvLin && gXdResProv) ex.desc_tiled_okt",
            "if (gXdResProvLin) ex.desc_tiled_okt" },
          { "switch 59's guard inverted", "if (gXdResProvLin && gXdResProv) ex.desc_tiled_okt", "if (!gXdResProvLin && gXdResProv) ex.desc_tiled_okt" },
          { "switch 59 declared ON", "static volatile uint32_t gXdResProvLin { 0u };", "static volatile uint32_t gXdResProvLin { 1u };" },
          { "switch 59 written by a second assignment", "if (changed59) gXdResProvLin = flin;", "if (changed59) gXdResProvLin = flin; gXdResProvLin = 1u;" },
      };
      for (const auto &p : okp) {
          std::string mut = raw;
          const size_t a = mut.find(p.from);
          if (a == std::string::npos || count_of(mut, p.from) != 1u) { expect(std::string("plant setup: found once: ") + p.from, false); continue; }
          mut.replace(a, strlen(p.from), p.to);
          expect(std::string("BREAK-check: ") + p.what + " - caught", rules_okt(strip(mut), false) > 0);
      } }
    printf("\nchecks run %d, failures %d\ngfx_descsw: %s\n", gRun, gFail, gFail ? "N48-DESCSW-TEST-FAIL" : "N48-DESCSW-TEST-PASS");
    return gFail ? 1 : 0;
}
