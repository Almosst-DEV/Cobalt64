// gfx_src_decide.h — the TRANSLATE-OR-NEUTER decision POINT at the source hook: how one program at a VA is classified, what the
// hook does with a verdict, and the per-pid / per-key accounting (0.0.302-dev,  step 3,).
//
// Pure C, host-tested by tests/gfx_src_decide_test.cpp; the kext compiles the SAME header. It sits on top of two existing headers
// and adds nothing to their logic:
//   gfx_neuter.h        the frame SHAPE the source neuter accepts and the entry-zeroing that neuters it
//   gfx_xlat_verdict.h  n48_xv_decide: the ORDER of the blocking reasons over one already-gathered frame, and the (pid, verdict,
//                       key) tally
// What is new here:
//   1. n48_sd_identify — the ONE rule that turns "bytes at a program VA" into an n48_xv_program. There are exactly three answers
//      and they are not symmetric, because substitution at the residency copy REPLACES Apple's bytes with ours:
//        a) the shader cache verifies the bytes as APPLE's program for a key       -> key known; bytes_are_ours = 0
//        b) the bytes equal OUR rendered substitution for some substitutable entry -> key known; bytes_are_ours = 1
//        c) neither                                                                -> key UNKNOWN (the observed key is reported)
//      (b) cannot be found by sc_lookup: the cache's terminator is the gfx10 s_endpgm 0xbf810000 and our gfx1201 program ends in
//      0xbfb00000, so a substituted program has no gfx10 terminator to key on (src/shadercache/README.md "The key"). It is found by
//      comparing the bytes with what sc_subst_render would write, which is what the residency copy actually wrote.
//   2. n48_sd_action — the separation the hardware run needs: the VERDICT (what the frame would be) is computed on every armed
//      frame, the ACTION (what we do) is gated by the arm level. DECIDE counts and still neuters, so the engine-liveness property
//      of cannot regress while we measure. COMMIT is the only level that lets a translated frame through.
//   3. n48_sd_stats — per pid: frames, translated / neutered / refused, and the reason histogram; per (pid, verdict, key): frames,
//      through gfx_xlat_verdict.h's tally. Overflow is counted, never silent (rule 72).
#ifndef N48_GFX_SRC_DECIDE_H
#define N48_GFX_SRC_DECIDE_H

#include <stdint.h>
#include "gfx_xlat_verdict.h"

/* ---- arm levels and actions ---------------------------------------------------------------------------------------------- */
enum { N48_SD_ARM_OFF = 0, N48_SD_ARM_DECIDE = 1, N48_SD_ARM_COMMIT = 2 };
/* PASS: the frame is not the shape the source neuter accepts, so it is handed to Apple untouched (the writeTail neuter still sees
 * it,). NEUTER: the entries are zeroed for the call. TRANSLATE: the IB was rewritten and the frame goes through. */
enum { N48_SD_ACT_PASS = 0, N48_SD_ACT_NEUTER = 1, N48_SD_ACT_TRANSLATE = 2 };
/* The three buckets the run is read by. REFUSED is a frame we never judged (shape), not a frame we judged and declined. */
enum { N48_SD_TRANSLATED = 0, N48_SD_NEUTERED = 1, N48_SD_REFUSED = 2, N48_SD_OUTCOMES = 3 };

/* ---- 1. identifying one program ------------------------------------------------------------------------------------------- */
/* The three inputs a caller must supply, so the rule is testable without a cache blob or a page table:
 *   sc_hit    1 when sc_lookup returned SC_OK on the bytes AND the match verified (Apple's program, this key)
 *   sc_subst  1 when we hold a gfx1201 program for that entry - SC_F_SUBSTITUTE or, since 0.0.311,
 *             SC_F_RELOCATE. The rung this feeds asks only WHETHER a translation exists, not where it will live:
 *             a relocated program has one just as much as a substituted one, it simply runs from our arena.
 *   sc_key    that entry's key
 *   ours_hit  1 when the bytes equal our rendered substitution for entry `ours_key` (checked only when sc_hit == 0)
 *   observed  the key the bytes themselves produce (sc_extent + sc_key), or 0 when they have no gfx10 terminator
 * Ambiguity (SC_E_AMBIGUOUS) and every refusal must reach here as sc_hit = 0: the cache never substitutes an ambiguous match, so
 * the hook must not claim to know the program either. */
static inline void n48_sd_identify(n48_xv_program *p, uint32_t sc_hit, uint32_t sc_subst, uint64_t sc_key,
                                   uint32_t ours_hit, uint64_t ours_key, uint64_t observed)
{
    p->key = 0; p->key_class = N48_XV_PGM_KEY_UNKNOWN; p->bytes_are_ours = 0;
    if (sc_hit) {
        p->key = sc_key;
        p->key_class = sc_subst ? N48_XV_PGM_KEY_XLAT : N48_XV_PGM_KEY_NO_XLAT;
        p->bytes_are_ours = 0;
        return;
    }
    if (ours_hit) {
        p->key = ours_key;
        p->key_class = N48_XV_PGM_KEY_XLAT;
        p->bytes_are_ours = 1;
        return;
    }
    p->key = observed;   /* reported so an host Mac session can name it; 0 when the bytes have no gfx10 s_endpgm */
}

/* 0.0.311: the SECOND route to "our code will run for this program" - a copy of it PLACED IN OUR ARENA,
 * which the translated draw points SPI_SHADER_PGM_LO/HI_ES at. Called after n48_sd_identify with the answer from the
 * arena itself (n48_reloc_find), never from a cache flag: an entry we hold code for but have NOT placed must keep
 * refusing, because a draw aimed at an address holding nothing renders garbage instead of refusing - the worst failure
 * shape this project has. Two clauses, and both are load-bearing:
 *   - `placed` must come from the arena's own table, so the bit tracks what was really uploaded;
 *   - the key must be one we hold a translation for (KEY_XLAT), so that a placement cannot be claimed for a program
 *     whose bytes we could not identify at all - an UNKNOWN key is `observed`, computed from whatever was at the VA,
 *     and letting that reach into the arena would key our code off a stranger's bytes. */
static inline void n48_sd_relocation(n48_xv_program *p, uint32_t placed)
{
    p->relocated = (placed && p->key_class == N48_XV_PGM_KEY_XLAT) ? 1u : 0u;
}

/* build 0.0.517 (: fc5 stopped judging at `frames judged 4096 ... over the 4096-frame cap 2602`, ~kill+76 s)
 * - THE JUDGE NEVER STOPS. Through 0.0.516 a frame arriving with `judged >= gXdFrameCap` was counted and NEUTERED unjudged. The
 * cap protected no table: nothing in the judge is indexed by the frame number (every per-frame record is a fixed table with
 * its own overflow count, and every frame number it stores is 64-bit), so it only bounded the judge's cost on Apple's submit
 * thread. A desktop runs for hours, so the verb's value (`gfxneuter 3 | mark << 8`, default 1024) is now a MARK: this answers
 * 1 for a frame at or past it (`judged` = frames judged BEFORE this one), for the report line's count only. The frame is
 * judged either way; no decision reads this. */
static inline uint32_t n48_sd_past_mark(uint64_t judged, uint32_t mark)
{
    return judged >= (uint64_t)mark ? 1u : 0u;
}

/* ---- 2. verdict -> action -------------------------------------------------------------------------------------------------- */
/* `shape_ok` is n48_gfxsrc_check == N48_SRC_OK. `commit_ok` is 1 only when the caller has ALREADY rewritten the IB and read it
 * back; a failed rewrite must arrive here as 0 so the frame is neutered rather than half-translated. */
static inline uint32_t n48_sd_action(uint32_t arm, uint32_t shape_ok, uint32_t verdict, uint32_t commit_ok)
{
    if (!shape_ok) return N48_SD_ACT_PASS;
    if (arm != N48_SD_ARM_COMMIT) return N48_SD_ACT_NEUTER;          /* OFF and DECIDE both neuter: liveness cannot regress */
    if (verdict != N48_XV_TRANSLATE) return N48_SD_ACT_NEUTER;
    return commit_ok ? N48_SD_ACT_TRANSLATE : N48_SD_ACT_NEUTER;
}

/* ---- 3. accounting --------------------------------------------------------------------------------------------------------- */
#define N48_SD_PIDS      12u
#define N48_SD_NAME      20u
#define N48_SD_SAMPLES   16u

typedef struct {
    int32_t  pid;
    char     name[N48_SD_NAME];
    uint64_t frames, ibs;
    uint64_t out[N48_SD_OUTCOMES];              /* translated / neutered / refused */
    uint64_t by_reason[N48_XV_REASONS];         /* the verdict histogram, even for frames that were neutered anyway */
    uint64_t last_key;
    uint32_t last_verdict;
} n48_sd_pid_row;

/* One unknown program, kept so that counting is not the only witness (rule: keep samples of what you count). */
typedef struct {
    int32_t  pid;
    uint32_t stage, extent;
    uint64_t va, key;
    uint32_t head[8];
} n48_sd_sample;

typedef struct {
    n48_sd_pid_row row[N48_SD_PIDS];
    uint32_t used;
    uint64_t pid_overflow;
    n48_xv_tally keys;                          /* (pid, verdict, key) -> frames */
    n48_sd_sample sample[N48_SD_SAMPLES];
    uint32_t nsample;
    uint64_t sample_overflow;
    uint64_t frames, translated, neutered, refused, commit_failed, budget_spent;
} n48_sd_stats;

static inline n48_sd_pid_row *n48_sd_row(n48_sd_stats *s, int32_t pid, const char *name)
{
    for (uint32_t i = 0; i < s->used; i++)
        if (s->row[i].pid == pid) return &s->row[i];
    if (s->used >= N48_SD_PIDS) { s->pid_overflow++; return 0; }
    n48_sd_pid_row *r = &s->row[s->used++];
    r->pid = pid;
    if (name) {
        uint32_t i = 0;
        for (; i + 1u < N48_SD_NAME && name[i]; i++) r->name[i] = name[i];
        r->name[i] = 0;
    }
    return r;
}

/* Record one frame. `verdict` is n48_xv_decide's answer (meaningful only when shape_ok), `action` is n48_sd_action's. */
static inline void n48_sd_note(n48_sd_stats *s, int32_t pid, const char *name, uint32_t shape_ok, uint32_t verdict,
                               uint64_t key, uint32_t action, uint32_t nib)
{
    const uint32_t bucket = action == N48_SD_ACT_TRANSLATE ? N48_SD_TRANSLATED
                          : action == N48_SD_ACT_NEUTER    ? N48_SD_NEUTERED : N48_SD_REFUSED;
    s->frames++;
    if (bucket == N48_SD_TRANSLATED) s->translated++;
    else if (bucket == N48_SD_NEUTERED) s->neutered++;
    else s->refused++;
    n48_sd_pid_row *r = n48_sd_row(s, pid, name);
    if (r) {
        r->frames++;
        r->ibs += nib;
        r->out[bucket]++;
        if (shape_ok) {
            if (verdict < N48_XV_REASONS) r->by_reason[verdict]++;
            r->last_verdict = verdict;
            r->last_key = key;
        }
    }
    if (shape_ok) n48_xv_count(&s->keys, pid, verdict, key);
}

/* Keep one unknown program the frame named. Deduplicated by (pid, key, va) so a 60 Hz compositor does not fill the ring with one
 * program; every further occurrence is counted in sample_overflow. */
static inline void n48_sd_sample_pgm(n48_sd_stats *s, int32_t pid, uint32_t stage, uint64_t va, uint64_t key, uint32_t extent,
                                     const uint32_t *head, uint32_t nhead)
{
    for (uint32_t i = 0; i < s->nsample; i++)
        if (s->sample[i].pid == pid && s->sample[i].key == key && s->sample[i].va == va) { s->sample_overflow++; return; }
    if (s->nsample >= N48_SD_SAMPLES) { s->sample_overflow++; return; }
    n48_sd_sample *q = &s->sample[s->nsample++];
    q->pid = pid; q->stage = stage; q->va = va; q->key = key; q->extent = extent;
    for (uint32_t i = 0; i < 8u; i++) q->head[i] = i < nhead && head ? head[i] : 0u;
}

static inline const char *n48_sd_action_name(uint32_t a)
{
    return a == N48_SD_ACT_TRANSLATE ? "TRANSLATE" : a == N48_SD_ACT_NEUTER ? "neuter" : "pass";
}

/* ---- 4. the program memo: the same programs, frame after frame ---------------------------------------------------------------
 * Run m4c1 measured the cost of identifying programs on Apple's submit thread: WindowServer's longest sync went from
 * 46,649 us to 109,457 us, 2.35x, for 23 judged frames. The same run measured where it goes: 224 of 240 identifications were
 * repeats. The capture of gfxcap1 sharpens it - over 60 SecurityAgent frames there were 24 distinct (program VA, content) pairs
 * and 24 distinct VAs, i.e. **every VA held the same bytes every time**, while all 62 IB checksums were DISTINCT.
 *
 * So the memo is keyed by PROGRAM VA. It is NOT keyed by the IB's checksum: that was the obvious guess and the data refutes it -
 * an IB carries its own stamps and addresses, so no two are alike and such a memo would never hit once.
 *
 * SOUNDNESS, which decides what may be stored. A memo can go stale only if the bytes at a VA change, and the one thing that
 * rewrites them is a residency copy (Apple re-uploading, or our own substitution during it). Two defences, both cheap:
 *   (a) an EPOCH: the caller passes the residency-copy count, and the whole memo is dropped when it moves;
 *   (b) a CLASS RULE: an identification is stored only when the bytes at the VA are NOT ours. A stale "unknown" or "known, no
 *       translation" can only make us NEUTER a frame we could have translated - an opportunity, never a wrong translation. A
 *       stale "these bytes are ours" is the one answer that could let a frame through over bytes that are no longer ours, so it
 *       is never stored and is always re-read. That keeps COMMIT safe by construction rather than by argument.
 * On the m4c1 frames every program was Apple's-not-ours, so (b) costs nothing there and the memo still removes every repeat. */
#define N48_SD_MEMO_ROWS 64u

typedef struct { uint64_t va, key; uint32_t key_class, valid, ours, check_dw; } n48_sd_memo_row;
typedef struct {
    n48_sd_memo_row row[N48_SD_MEMO_ROWS];
    uint32_t used;
    uint64_t epoch;
    uint64_t hits, misses, stores, not_stored, evictions, drops, stale;
} n48_sd_memo;

/* Drop everything. `epoch` is recorded so the next lookup with the same epoch is a hit. */
static inline void n48_sd_memo_clear(n48_sd_memo *m, uint64_t epoch)
{
    for (uint32_t i = 0; i < N48_SD_MEMO_ROWS; i++) m->row[i].valid = 0;
    m->used = 0;
    m->epoch = epoch;
    m->drops++;
}

/* Returns 1 and fills *p on a hit. Any change of epoch drops the table first.
 * When the row records bytes that are OURS, *check_dw is the first dword seen at that address and the caller MUST re-read
 * that one dword and compare before trusting the row (n48_sd_memo_ours_ok). See the class rule above: 0.0.310
 * replaced "never remember ours" with "remember it, and revalidate with one dword", because run m4c3 measured the old
 * rule refusing 398 of 411 identifications precisely when substitution started working - the memo stopped helping exactly
 * as it became useful. One dword is one page mapping; the alternative was a 512-dword read, a cache lookup and up to 29
 * image compares. */
static inline uint32_t n48_sd_memo_get(n48_sd_memo *m, uint64_t epoch, uint64_t va, n48_xv_program *p,
                                       uint32_t *needs_check, uint32_t *check_dw)
{
    if (needs_check) *needs_check = 0;
    if (check_dw) *check_dw = 0;
    if (epoch != m->epoch) n48_sd_memo_clear(m, epoch);
    for (uint32_t i = 0; i < m->used; i++)
        if (m->row[i].valid && m->row[i].va == va) {
            p->key = m->row[i].key;
            p->key_class = m->row[i].key_class;
            p->bytes_are_ours = m->row[i].ours;
            if (m->row[i].ours) {
                if (needs_check) *needs_check = 1;
                if (check_dw) *check_dw = m->row[i].check_dw;
            }
            m->hits++;
            return 1;
        }
    m->misses++;
    return 0;
}

/*: a READ-ONLY peek for a log line - the key this table already holds for `va`, without touching hits/misses/epoch. It is
 * NOT an identification: it answers only what a previous identification stored, and 0 means "this table does not say", never
 * "no key". Used by the PROVENANCE refusal line so hp8 can name the program that asked as well as the surface. */
static inline uint64_t n48_sd_memo_peek_key(const n48_sd_memo *m, uint64_t epoch, uint64_t va, uint32_t *key_class)
{
    if (key_class) *key_class = 0u;
    if (!m || epoch != m->epoch) return 0ull;
    for (uint32_t i = 0; i < m->used && i < N48_SD_MEMO_ROWS; i++)
        if (m->row[i].valid && m->row[i].va == va) {
            if (key_class) *key_class = m->row[i].key_class;
            return m->row[i].key;
        }
    return 0ull;
}

/* The caller's verdict on a row that claimed the bytes are ours: `seen` is the dword it just re-read at that address.
 * Returns 1 when the row still holds. A mismatch invalidates the row and is counted, so a stale row costs one read. */
static inline uint32_t n48_sd_memo_ours_ok(n48_sd_memo *m, uint64_t va, uint32_t seen)
{
    for (uint32_t i = 0; i < m->used; i++)
        if (m->row[i].valid && m->row[i].va == va) {
            if (m->row[i].check_dw == seen) return 1;
            m->row[i].valid = 0;
            m->stale++;
            return 0;
        }
    return 0;
}

/* Store an identification. A row whose bytes are OURS also records `first_dw`, the first dword at that address, which a
 * later hit revalidates with a single read (n48_sd_memo_ours_ok). */
static inline void n48_sd_memo_put(n48_sd_memo *m, uint64_t epoch, uint64_t va, const n48_xv_program *p, uint32_t first_dw)
{
    if (epoch != m->epoch) n48_sd_memo_clear(m, epoch);
    if (p->bytes_are_ours && !first_dw) { m->not_stored++; return; }   /* no validator: do not remember it */
    for (uint32_t i = 0; i < m->used; i++)
        if (m->row[i].valid && m->row[i].va == va) {
            m->row[i].key = p->key; m->row[i].key_class = p->key_class;
            m->row[i].ours = p->bytes_are_ours; m->row[i].check_dw = first_dw;
            m->stores++;
            return;
        }
    if (m->used >= N48_SD_MEMO_ROWS) { m->evictions++; return; }   /* a full table stops learning; it never lies */
    n48_sd_memo_row *r = &m->row[m->used++];
    r->va = va; r->key = p->key; r->key_class = p->key_class; r->valid = 1;
    r->ours = p->bytes_are_ours; r->check_dw = first_dw;
    m->stores++;
}

#endif
