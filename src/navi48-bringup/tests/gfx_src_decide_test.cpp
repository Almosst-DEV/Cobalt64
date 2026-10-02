// gfx_src_decide_test.cpp — the translate-or-neuter decision point at the source hook: program identification, verdict-to-action,
// and the per-pid / per-key accounting. Includes a PLANTED-DEFECT CONTROL: seven mutants of the three rules,
// each of which must make at least one named check fail. A test that no mutation can break is not testing anything.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_src_decide_test.cpp -o /tmp/sdtest && /tmp/sdtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_src_decide.h"

static int gFail = 0, gRun = 0, gQuiet = 0;
static const char *gMutant = nullptr;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-74s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-74s %#llx\n", what, (unsigned long long)got);
    }
}

// ---- the three rules, behind function pointers so a mutant can replace exactly one -------------------------------------------
typedef void (*IdentifyFn)(n48_xv_program *, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t, uint64_t);
typedef uint32_t (*ActionFn)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*NoteFn)(n48_sd_stats *, int32_t, const char *, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t);
struct Ops { IdentifyFn identify; ActionFn action; NoteFn note; };

// Mutant 1: "ours" wins over the cache's verified hit on Apple's bytes.
static void m1_identify(n48_xv_program *p, uint32_t sc_hit, uint32_t sc_subst, uint64_t sc_key,
                        uint32_t ours_hit, uint64_t ours_key, uint64_t observed)
{
    p->key = 0; p->key_class = N48_XV_PGM_KEY_UNKNOWN; p->bytes_are_ours = 0;
    if (ours_hit) { p->key = ours_key; p->key_class = N48_XV_PGM_KEY_XLAT; p->bytes_are_ours = 1; return; }
    if (sc_hit) { p->key = sc_key; p->key_class = sc_subst ? N48_XV_PGM_KEY_XLAT : N48_XV_PGM_KEY_NO_XLAT; return; }
    p->key = observed;
}
// Mutant 2: an unknown program is called translatable.
static void m2_identify(n48_xv_program *p, uint32_t sc_hit, uint32_t sc_subst, uint64_t sc_key,
                        uint32_t ours_hit, uint64_t ours_key, uint64_t observed)
{
    n48_sd_identify(p, sc_hit, sc_subst, sc_key, ours_hit, ours_key, observed);
    if (p->key_class == N48_XV_PGM_KEY_UNKNOWN) p->key_class = N48_XV_PGM_KEY_XLAT;
}
// Mutant 3: Apple's own bytes are reported as ours (substitution believed without evidence).
static void m3_identify(n48_xv_program *p, uint32_t sc_hit, uint32_t sc_subst, uint64_t sc_key,
                        uint32_t ours_hit, uint64_t ours_key, uint64_t observed)
{
    n48_sd_identify(p, sc_hit, sc_subst, sc_key, ours_hit, ours_key, observed);
    if (sc_hit) p->bytes_are_ours = 1;
}
// Mutant 4: DECIDE translates — the liveness regression this arm level exists to prevent.
static uint32_t m4_action(uint32_t arm, uint32_t shape_ok, uint32_t verdict, uint32_t commit_ok)
{
    if (!shape_ok) return N48_SD_ACT_PASS;
    if (arm == N48_SD_ARM_OFF) return N48_SD_ACT_NEUTER;
    if (verdict != N48_XV_TRANSLATE) return N48_SD_ACT_NEUTER;
    return commit_ok ? N48_SD_ACT_TRANSLATE : N48_SD_ACT_NEUTER;
}
// Mutant 5: a failed rewrite still goes through — a half-written IB reaches the CP.
static uint32_t m5_action(uint32_t arm, uint32_t shape_ok, uint32_t verdict, uint32_t commit_ok)
{
    (void)commit_ok;
    if (!shape_ok) return N48_SD_ACT_PASS;
    if (arm != N48_SD_ARM_COMMIT) return N48_SD_ACT_NEUTER;
    return verdict == N48_XV_TRANSLATE ? N48_SD_ACT_TRANSLATE : N48_SD_ACT_NEUTER;
}
// Mutant 6: a shape-refused frame is counted as neutered.
static void m6_note(n48_sd_stats *s, int32_t pid, const char *name, uint32_t shape_ok, uint32_t verdict, uint64_t key,
                    uint32_t action, uint32_t nib)
{
    n48_sd_note(s, pid, name, shape_ok, verdict, key, action == N48_SD_ACT_PASS ? N48_SD_ACT_NEUTER : action, nib);
}
// Mutant 7: the (pid, verdict, key) tally is fed for frames that were never judged.
static void m7_note(n48_sd_stats *s, int32_t pid, const char *name, uint32_t shape_ok, uint32_t verdict, uint64_t key,
                    uint32_t action, uint32_t nib)
{
    n48_sd_note(s, pid, name, shape_ok, verdict, key, action, nib);
    if (!shape_ok) n48_xv_count(&s->keys, pid, verdict, key);
}

// ---- the checks ---------------------------------------------------------------------------------------------------------------
// Keys are real: they are the shader-cache keys of Apple's own compiled records in
// re/pc-26.6.2/metalcache/com.apple.SecurityAgent/16777236_6004/functions.data, as tools/m4-xlat/sa-programs.tsv prints them.
static const uint64_t kKeyPlaneVs   = 0x3f629a1c50bce538ull;   // plane__vertex       (vertex,   L 39,  Apple sizeB 512)
static const uint64_t kKeyFca15     = 0xa4b3621042a4a4abull;   // fca15jbebia3        (fragment, L 21,  Apple sizeB 512)
static const uint64_t kKeyPathInt   = 0xed70a542c08bbcf2ull;   // path_interior_fragment (no gfx1201 program: FAIL in)
static const uint64_t kKeyUnseen    = 0x9e3779b97f4a7c15ull;   // a key no table holds

static void checks(const Ops &o)
{
    n48_xv_program p {};

    // --- identification, the three answers ---
    o.identify(&p, 1, 1, kKeyPlaneVs, 0, 0, 0);
    expect_u("cache verifies Apple's bytes, entry substitutable -> key", p.key, kKeyPlaneVs);
    expect_u("  ... class XLAT", p.key_class, N48_XV_PGM_KEY_XLAT);
    expect_u("  ... and the bytes at the VA are still APPLE's", p.bytes_are_ours, 0);

    o.identify(&p, 1, 0, kKeyPathInt, 0, 0, 0);
    expect_u("cache verifies Apple's bytes, no gfx1201 program -> class NO_XLAT", p.key_class, N48_XV_PGM_KEY_NO_XLAT);
    expect_u("  ... key still named", p.key, kKeyPathInt);

    o.identify(&p, 0, 0, 0, 1, kKeyFca15, 0);
    expect_u("bytes equal our rendered substitution -> key of that entry", p.key, kKeyFca15);
    expect_u("  ... class XLAT", p.key_class, N48_XV_PGM_KEY_XLAT);
    expect_u("  ... and the bytes ARE ours", p.bytes_are_ours, 1);

    o.identify(&p, 0, 0, 0, 0, 0, kKeyUnseen);
    expect_u("neither: UNKNOWN", p.key_class, N48_XV_PGM_KEY_UNKNOWN);
    expect_u("  ... the OBSERVED key is reported so it can be named offline", p.key, kKeyUnseen);
    expect_u("  ... never claimed as ours", p.bytes_are_ours, 0);

    o.identify(&p, 0, 0, 0, 0, 0, 0);
    expect_u("bytes with no gfx10 s_endpgm: UNKNOWN with key 0", p.key_class, N48_XV_PGM_KEY_UNKNOWN);
    expect_u("  ... key 0", p.key, 0);

    // A verified cache hit must win: after substitution the bytes are gfx1201 and cannot key, so sc_hit and ours_hit are
    // mutually exclusive in the field; if a blob ever made both true, Apple's identity is the safe one (bytes NOT ours).
    o.identify(&p, 1, 1, kKeyPlaneVs, 1, kKeyFca15, 0);
    expect_u("both claim the bytes: the cache's verified compare wins", p.key, kKeyPlaneVs);
    expect_u("  ... and bytes_are_ours stays 0", p.bytes_are_ours, 0);

    // --- verdict -> action ---
    expect_u("OFF + shape ok -> neuter", o.action(N48_SD_ARM_OFF, 1, N48_XV_ARMED_OFF, 0), N48_SD_ACT_NEUTER);
    expect_u("OFF + shape refused -> pass", o.action(N48_SD_ARM_OFF, 0, N48_XV_SHAPE, 0), N48_SD_ACT_PASS);
    expect_u("DECIDE + TRANSLATE verdict STILL NEUTERS", o.action(N48_SD_ARM_DECIDE, 1, N48_XV_TRANSLATE, 1), N48_SD_ACT_NEUTER);
    expect_u("DECIDE + blocked verdict -> neuter", o.action(N48_SD_ARM_DECIDE, 1, N48_XV_PGM_UNKNOWN, 0), N48_SD_ACT_NEUTER);
    expect_u("DECIDE + shape refused -> pass", o.action(N48_SD_ARM_DECIDE, 0, N48_XV_SHAPE, 0), N48_SD_ACT_PASS);
    expect_u("COMMIT + TRANSLATE + rewrite verified -> TRANSLATE", o.action(N48_SD_ARM_COMMIT, 1, N48_XV_TRANSLATE, 1),
             N48_SD_ACT_TRANSLATE);
    expect_u("COMMIT + TRANSLATE + rewrite FAILED -> neuter", o.action(N48_SD_ARM_COMMIT, 1, N48_XV_TRANSLATE, 0),
             N48_SD_ACT_NEUTER);
    expect_u("COMMIT + blocked verdict -> neuter", o.action(N48_SD_ARM_COMMIT, 1, N48_XV_SEG_POLICY, 1), N48_SD_ACT_NEUTER);
    expect_u("COMMIT + shape refused -> pass", o.action(N48_SD_ARM_COMMIT, 0, N48_XV_SHAPE, 1), N48_SD_ACT_PASS);

    // --- accounting: a run like the one a hardware boot would produce ---
    static n48_sd_stats s;
    std::memset(&s, 0, sizeof s);
    // WindowServer (pid 1053): 3 frames blocked on one unknown program, 1 on a second.
    for (int i = 0; i < 3; i++) o.note(&s, 1053, "WindowServer", 1, N48_XV_PGM_UNKNOWN, kKeyUnseen, N48_SD_ACT_NEUTER, 1);
    o.note(&s, 1053, "WindowServer", 1, N48_XV_PGM_UNKNOWN, kKeyUnseen + 1, N48_SD_ACT_NEUTER, 1);
    // SecurityAgent (pid 1071): 2 frames whose programs are all ours, blocked at the IB policy; 1 translated.
    o.note(&s, 1071, "SecurityAgent", 1, N48_XV_SEG_POLICY, 0, N48_SD_ACT_NEUTER, 1);
    o.note(&s, 1071, "SecurityAgent", 1, N48_XV_SEG_POLICY, 0, N48_SD_ACT_NEUTER, 2);
    o.note(&s, 1071, "SecurityAgent", 1, N48_XV_TRANSLATE, 0, N48_SD_ACT_TRANSLATE, 1);
    // tri (pid 754): one timestamp-only frame the source neuter does not accept.
    o.note(&s, 754, "tri", 0, N48_XV_SHAPE, 0, N48_SD_ACT_PASS, 0);

    expect_u("frames total", s.frames, 8);
    expect_u("translated total", s.translated, 1);
    expect_u("neutered total", s.neutered, 6);
    expect_u("refused total (shape)", s.refused, 1);
    expect_u("three pids", s.used, 3);
    expect_u("pid 1053 is row 0", (uint64_t)(uint32_t)s.row[0].pid, 1053);
    expect_u("  its name", (uint64_t)(std::strcmp(s.row[0].name, "WindowServer") == 0), 1);
    expect_u("  4 frames, all neutered", s.row[0].out[N48_SD_NEUTERED], 4);
    expect_u("  none translated", s.row[0].out[N48_SD_TRANSLATED], 0);
    expect_u("  every one blocked on an unknown program", s.row[0].by_reason[N48_XV_PGM_UNKNOWN], 4);
    expect_u("pid 1071 translated 1", s.row[1].out[N48_SD_TRANSLATED], 1);
    expect_u("  neutered 2", s.row[1].out[N48_SD_NEUTERED], 2);
    expect_u("  IBs counted across frames", s.row[1].ibs, 4);
    expect_u("  segment policy twice", s.row[1].by_reason[N48_XV_SEG_POLICY], 2);
    expect_u("pid 754 refused, not neutered", s.row[2].out[N48_SD_REFUSED], 1);
    expect_u("  and its verdict histogram stays empty (it was never judged)", s.row[2].by_reason[N48_XV_SHAPE], 0);

    // The per-key tally: WindowServer's two unknown keys are separate rows, and a shape refusal is not in it at all.
    expect_u("tally rows: 2 unknown keys + seg-policy + translate", s.keys.used, 4);
    expect_u("tally: the first unknown key seen 3 times", s.keys.row[0].frames, 3);
    expect_u("tally: key of row 0", s.keys.row[0].key, kKeyUnseen);
    expect_u("tally: by verdict PGM_UNKNOWN 4", s.keys.by_verdict[N48_XV_PGM_UNKNOWN], 4);
    expect_u("tally: by verdict TRANSLATE 1", s.keys.by_verdict[N48_XV_TRANSLATE], 1);
    expect_u("tally: shape refusals are NOT counted by verdict", s.keys.by_verdict[N48_XV_SHAPE], 0);
}

static void extra_checks()
{
    // pid table overflow is counted, never silent
    static n48_sd_stats s;
    std::memset(&s, 0, sizeof s);
    for (uint32_t i = 0; i < N48_SD_PIDS + 5u; i++)
        n48_sd_note(&s, (int32_t)(100 + i), "p", 1, N48_XV_PGM_UNKNOWN, i, N48_SD_ACT_NEUTER, 1);
    expect_u("pid table full at N48_SD_PIDS", s.used, N48_SD_PIDS);
    expect_u("pid overflow counted", s.pid_overflow, 5);
    expect_u("totals still complete", s.frames, N48_SD_PIDS + 5u);
    expect_u("a long name is truncated, not overrun", (uint64_t)std::strlen(s.row[0].name), 1);

    std::memset(&s, 0, sizeof s);
    n48_sd_note(&s, 7, "aVeryLongProcessNameIndeed", 1, N48_XV_TRANSLATE, 0, N48_SD_ACT_TRANSLATE, 1);
    expect_u("name truncated to N48_SD_NAME-1", (uint64_t)std::strlen(s.row[0].name), N48_SD_NAME - 1u);

    // program samples: dedupe by (pid, key, va), overflow counted, head copied
    std::memset(&s, 0, sizeof s);
    const uint32_t head[8] = { 0xb0802004u, 0x7e000200u, 0x7e020201u, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) n48_sd_sample_pgm(&s, 1053, 0, 0x400008e00ull, kKeyUnseen, 34, head, 8);
    expect_u("the same program is sampled once", s.nsample, 1);
    expect_u("repeats counted", s.sample_overflow, 3);
    expect_u("head kept", s.sample[0].head[0], 0xb0802004u);
    expect_u("extent kept", s.sample[0].extent, 34);
    for (uint32_t i = 0; i < N48_SD_SAMPLES + 3u; i++)
        n48_sd_sample_pgm(&s, 1053, 1, 0x400020000ull + i * 0x100u, kKeyUnseen + 1u + i, 8, head, 8);
    expect_u("sample ring full", s.nsample, N48_SD_SAMPLES);
    expect_u("sample overflow counted (3 repeats + 4 over the ring)", s.sample_overflow, 3 + 4);
    expect_u("a short head is zero-filled, never read past", s.sample[1].head[7], 0);

    expect_u("action names", (uint64_t)(std::strcmp(n48_sd_action_name(N48_SD_ACT_TRANSLATE), "TRANSLATE") == 0), 1);
}

// ---- the program memo ('s cost result) --------------------------------------------------------------------------------
typedef void (*MemoPutFn)(n48_sd_memo *, uint64_t, uint64_t, const n48_xv_program *, uint32_t);
static MemoPutFn gMemoPut = &n48_sd_memo_put;

// Mutant 8: the memo stores an identification whose bytes are OURS — the one class that could let a stale entry translate a
// frame over bytes that are no longer ours.
static void m8_memo_put(n48_sd_memo *m, uint64_t epoch, uint64_t va, const n48_xv_program *p, uint32_t first_dw)
{
    /* 0.0.310: the defect is now "remember an ours-row with NO validator", which is the one shape that cannot be
     * revalidated and so could let a stale row translate a frame over bytes that are no longer ours. */
    n48_sd_memo_put(m, epoch, va, p, p->bytes_are_ours ? 0u : first_dw);
    if (p->bytes_are_ours) {
        n48_xv_program q = *p;
        for (uint32_t i = 0; i < m->used; i++)
            if (m->row[i].va == va) { m->row[i].valid = 1; m->row[i].ours = 1; m->row[i].check_dw = 0; m->row[i].key = q.key; }
    }
}

static void memo_checks()
{
    static n48_sd_memo m;
    std::memset(&m, 0, sizeof m);
    n48_xv_program p {}, got {};

    // a miss on an empty table, then a hit after a store
    expect_u("memo: empty table misses", n48_sd_memo_get(&m, 1, 0x400020700ull, &got, 0, 0), 0);
    n48_sd_identify(&p, 0, 0, 0, 0, 0, kKeyUnseen);                       // UNKNOWN, bytes not ours
    gMemoPut(&m, 1, 0x400020700ull, &p, 0xb0802004u);
    expect_u("memo: stored", m.stores, 1);
    expect_u("memo: hit after a store", n48_sd_memo_get(&m, 1, 0x400020700ull, &got, 0, 0), 1);
    expect_u("  the key comes back", got.key, kKeyUnseen);
    expect_u("  the class comes back", got.key_class, N48_XV_PGM_KEY_UNKNOWN);
    expect_u("  and never claims the bytes are ours", got.bytes_are_ours, 0);
    expect_u("memo: a different VA still misses", n48_sd_memo_get(&m, 1, 0x400008e00ull, &got, 0, 0), 0);

    // 0.0.310: an ours-row IS remembered now, but only WITH a validator, and a hit must be revalidated by one
    // dword before it is trusted. Run m4c3 measured the old "never remember ours" rule refusing 398 of 411
    // identifications exactly when substitution began working.
    n48_sd_identify(&p, 0, 0, 0, 1, kKeyFca15, 0);                        // ours
    expect_u("  (control) that identification does say the bytes are ours", p.bytes_are_ours, 1);
    gMemoPut(&m, 1, 0x40002c200ull, &p, 0xb0802004u);
    uint32_t need = 0, chk = 0;
    expect_u("memo: an ours-row WITH a validator is remembered", n48_sd_memo_get(&m, 1, 0x40002c200ull, &got, &need, &chk), 1);
    expect_u("  the hit says the bytes are ours", got.bytes_are_ours, 1);
    expect_u("  and demands revalidation", need, 1);
    expect_u("  handing back the dword to compare", chk, 0xb0802004u);
    expect_u("  the matching dword accepts the row", n48_sd_memo_ours_ok(&m, 0x40002c200ull, 0xb0802004u), 1);
    expect_u("  a DIFFERENT dword rejects it", n48_sd_memo_ours_ok(&m, 0x40002c200ull, 0xdeadbeefu), 0);
    expect_u("  the stale row is counted", m.stale, 1);
    expect_u("  and is gone", n48_sd_memo_get(&m, 1, 0x40002c200ull, &got, &need, &chk), 0);
    // an ours-row with NO validator cannot be revalidated, so it is refused outright
    gMemoPut(&m, 1, 0x40002c400ull, &p, 0u);
    expect_u("memo: an ours-row with NO validator is refused", n48_sd_memo_get(&m, 1, 0x40002c400ull, &got, &need, &chk), 0);
    expect_u("  and the refusal is counted", m.not_stored, 1);

    // THE EPOCH: a residency copy drops everything
    const uint64_t before = m.drops;
    expect_u("memo: a new epoch drops the table", n48_sd_memo_get(&m, 2, 0x400020700ull, &got, 0, 0), 0);
    expect_u("  the drop is counted", m.drops, before + 1);
    expect_u("  and the table is empty", m.used, 0);

    // a full table stops learning and never lies
    std::memset(&m, 0, sizeof m);
    n48_sd_identify(&p, 0, 0, 0, 0, 0, kKeyUnseen);
    for (uint32_t i = 0; i < N48_SD_MEMO_ROWS + 4u; i++) gMemoPut(&m, 1, 0x400000000ull + i * 0x100u, &p, 0xb0802004u);
    expect_u("memo: table full at N48_SD_MEMO_ROWS", m.used, N48_SD_MEMO_ROWS);
    expect_u("memo: evictions counted", m.evictions, 4);
    expect_u("memo: the first VA is still right", n48_sd_memo_get(&m, 1, 0x400000000ull, &got, 0, 0), 1);
    expect_u("memo: a VA past the table is a miss, not a wrong answer",
             n48_sd_memo_get(&m, 1, 0x400000000ull + (N48_SD_MEMO_ROWS + 1u) * 0x100u, &got, 0, 0), 0);

    // re-storing the same VA updates in place rather than growing
    const uint32_t used = m.used;
    n48_sd_identify(&p, 1, 0, kKeyPathInt, 0, 0, 0);                      // Apple's, no translation
    gMemoPut(&m, 1, 0x400000000ull, &p, 0xb0802004u);
    expect_u("memo: re-store does not grow the table", m.used, used);
    expect_u("memo: re-store updates the row", n48_sd_memo_get(&m, 1, 0x400000000ull, &got, 0, 0), 1);
    expect_u("  to the new class", got.key_class, N48_XV_PGM_KEY_NO_XLAT);

    // the shape of the win m4c1 measured: 12 programs, 23 frames, one read each
    std::memset(&m, 0, sizeof m);
    n48_sd_identify(&p, 0, 0, 0, 0, 0, kKeyUnseen);
    uint32_t reads = 0;
    for (uint32_t f = 0; f < 23u; f++)
        for (uint32_t k = 0; k < 12u; k++) {
            const uint64_t va = 0x400020700ull + k * 0x100u;
            if (!n48_sd_memo_get(&m, 1, va, &got, 0, 0)) { reads++; gMemoPut(&m, 1, va, &p, 0xb0802004u); }
        }
    expect_u("memo: 23 frames x 12 programs need only 12 reads", reads, 12);
    expect_u("memo: the other 264 are hits", m.hits, 23u * 12u - 12u);
}

// ---- 0.0.311: THE SECOND ROUTE TO "OUR CODE WILL RUN" -------------------------------------------------
// A relocated program's bytes at Apple's VA stay APPLE'S for ever, by design, so the rung that refuses on
// !bytes_are_ours refuses it permanently - which is how one key held every SecurityAgent frame at
// program-no-translation. The bit that lifts it must come from a REAL PLACEMENT, because a program claimed
// translatable with no code in the arena would point a draw at an address holding nothing: it renders garbage rather
// than refusing, which is the worst failure shape this project has.
typedef void (*RelocFn)(n48_xv_program *, uint32_t);

// Mutant 9: `relocated` claimed WITHOUT a placement - the mandated planted defect.
static void m9_relocation(n48_xv_program *p, uint32_t placed)
{
    (void)placed;
    p->relocated = (p->key_class == N48_XV_PGM_KEY_XLAT) ? 1u : 0u;
}

static uint32_t decide_one(const n48_xv_program &pgm)
{
    n48_xv_frame f {};
    f.armed = 1; f.shape_ok = 1; f.reader_ok = 1; f.budget_left = 1;
    f.nib = 1; f.ib[0].len = 8; f.ib[0].got = 8; f.ib[0].walk = 8; f.ib[0].vram_pages = 0;
    f.npgm = 1; f.pgm[0] = pgm;
    f.nseg = 0;                      /* nothing accepted yet, so passing the program rungs lands on segment-policy */
    uint64_t key = 0; uint32_t detail = 0;
    return n48_xv_decide(&f, &key, &detail);
}

static void reloc_checks(RelocFn reloc)
{
    const uint64_t K = 0xe3619022d1d4f224ull;     /* RectPosTexFast_VS, the key that blocked every frame */
    n48_xv_program p {};

    /* we hold a gfx1201 program for this key (SC_F_RELOCATE arrives as sc_subst = 1), and Apple's bytes are at the VA */
    n48_sd_identify(&p, 1, 1, K, 0, 0, 0);
    expect_u("reloc: an entry we hold code for is class XLAT", p.key_class, N48_XV_PGM_KEY_XLAT);
    expect_u("reloc: ... and Apple's bytes are still Apple's", p.bytes_are_ours, 0u);

    /* NOT placed: the arena holds nothing for it, so the frame must still refuse */
    reloc(&p, 0);
    expect_u("reloc: no placement -> not relocated", p.relocated, 0u);
    expect_u("reloc: ... and the frame answers program-not-substituted", decide_one(p), (uint64_t)N48_XV_PGM_NOT_OURS);

    /* placed: our code IS in the arena, the draw will name it, and the program rungs are satisfied */
    reloc(&p, 1);
    expect_u("reloc: a real placement -> relocated", p.relocated, 1u);
    expect_u("reloc: ... and the frame gets past the programs", decide_one(p), (uint64_t)N48_XV_SEG_POLICY);

    /* a program whose bytes we could not identify must NOT reach into the arena: its key is whatever the bytes made */
    n48_xv_program u {};
    n48_sd_identify(&u, 0, 0, 0, 0, 0, K);
    expect_u("reloc: an unidentified program is UNKNOWN", u.key_class, N48_XV_PGM_KEY_UNKNOWN);
    reloc(&u, 1);
    expect_u("reloc: ... and a placement does not make it relocated", u.relocated, 0u);

    /* a key we hold NO gfx1201 program for cannot be relocated either, whatever the arena says */
    n48_xv_program nx {};
    n48_sd_identify(&nx, 1, 0, K, 0, 0, 0);
    expect_u("reloc: a key with no translation is NO_XLAT", nx.key_class, N48_XV_PGM_KEY_NO_XLAT);
    reloc(&nx, 1);
    expect_u("reloc: ... and stays unrelocated", nx.relocated, 0u);

    /* the in-place route is untouched: substituted bytes pass with no placement at all */
    n48_xv_program o {};
    n48_sd_identify(&o, 0, 0, 0, 1, K, 0);
    reloc(&o, 0);
    expect_u("reloc: substituted-in-place still passes with no placement", decide_one(o), (uint64_t)N48_XV_SEG_POLICY);
}

int main(int argc, char **argv)
{
    const bool mutants = argc > 1 && std::strcmp(argv[1], "--no-mutants") != 0;
    (void)mutants;
    const Ops real { &n48_sd_identify, &n48_sd_action, &n48_sd_note };
    checks(real);
    extra_checks();
    memo_checks();
    reloc_checks(&n48_sd_relocation);
    const int realFail = gFail, realRun = gRun;
    std::printf("%d/%d passed\n", realRun - realFail, realRun);

    // ---- planted-defect control: each mutant must break at least one check ----
    struct { const char *name; Ops ops; } mut[] = {
        { "M1 identify: 'ours' beats the cache's verified compare", { &m1_identify, &n48_sd_action, &n48_sd_note } },
        { "M2 identify: an unknown program is called translatable", { &m2_identify, &n48_sd_action, &n48_sd_note } },
        { "M3 identify: Apple's bytes reported as already ours", { &m3_identify, &n48_sd_action, &n48_sd_note } },
        { "M4 action: DECIDE translates (liveness regression)", { &n48_sd_identify, &m4_action, &n48_sd_note } },
        { "M5 action: a failed rewrite still goes through", { &n48_sd_identify, &m5_action, &n48_sd_note } },
        { "M6 note: a shape-refused frame counted as neutered", { &n48_sd_identify, &n48_sd_action, &m6_note } },
        { "M7 note: the per-key tally fed for unjudged frames", { &n48_sd_identify, &n48_sd_action, &m7_note } },
    };
    int caught = 0;
    for (auto &m : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        gMutant = m.name;
        checks(m.ops);
        memo_checks();
        const int f = gFail, r = gRun;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", m.name, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
        gQuiet = 0;
    }
    // M9 replaces the relocation rule rather than one of the three, so it runs on its own. It is the MANDATED mutant:
    // `relocated` claimed with no placement is the defect that renders instead of refusing.
    gQuiet = 1; gFail = 0; gRun = 0;
    reloc_checks(&m9_relocation);
    const int m9fail = gFail, m9run = gRun;
    gQuiet = 0;
    std::printf("mutant %-58s %s (%d of %d checks fail)\n",
                "M9 reloc: relocated claimed with NO placement", m9fail ? "CAUGHT" : "NOT CAUGHT", m9fail, m9run);
    if (m9fail) caught++;

    // M8 replaces the memo's store rather than one of the three rules, so it runs on its own.
    gQuiet = 1; gFail = 0; gRun = 0;
    gMemoPut = &m8_memo_put;
    memo_checks();
    const int m8fail = gFail, m8run = gRun;
    gMemoPut = &n48_sd_memo_put;
    gQuiet = 0;
    std::printf("mutant %-58s %s (%d of %d checks fail)\n",
                "M8 memo: an ours-row remembered with NO validator", m8fail ? "CAUGHT" : "NOT CAUGHT", m8fail, m8run);
    if (m8fail) caught++;
    const int nmut = (int)(sizeof(mut) / sizeof(mut[0])) + 2;   /* + M8 (memo) and M9 (relocation) */
    (void)gMutant;
    std::printf("mutants caught %d/%d\n", caught, nmut);
    return (realFail || caught != nmut) ? 1 : 0;
}
