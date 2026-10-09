// gfx_heapgen.h — build 0.0.495 (: RUN D's hang). THE SHADER-HEAP COPY / SUBSTITUTION RACE, PURE HALF.
//
// WHAT RUN D SHOWED (driverlog-stream.txt of run10i, in this order): the gate answered COMMIT for token seq 12 (an ordinary
// green fill whose programs were judged `all-programs-ours`); then `COPIED #62 ... bytes=0x9000 -> VRAM [0x10020000,0x10029000)
// ... GPU VA 0x400028000` rewrote WindowServer's shader heap with Apple's gfx10 bytes; then the RING EXEMPTION spared seq 12;
// and only AFTER that did the shader cache re-apply our gfx1201 programs (`SUBSTITUTED ... GPUPass[fragment] ... at resource
// +0x2300` and 30 more). The CP ran f23 over gfx10 code and the GPU hung. The residency copy (Navi48AccelPeer.cpp
// residency_copy_to_vram) writes Apple's bytes FIRST and substitutes AFTER (shadercache_scan_resource), on the copier's own
// thread with no lock shared with the submit path; the program memo (gfx_src_decide.h) revalidates one dword only at verdict
// time. So between the copy's first write and the substitution's last one, VRAM holds gfx10 code at an address a committed
// frame was told is ours.
//
// SWITCH 62 (default OFF) closes it two ways, both of which live on top of this header:
//   1. SUBSTITUTE IN THE COPY (the primary fix). Before the copy's first VRAM write, the copier scans the SAME backing bytes
//      with the SAME shader-cache scan and verification (sc_scan_window's key + byte compare, sc_subst_render's capacity
//      check) that the post-copy substitution runs, and records each program it would substitute as a PATCH: the resource
//      offset, the rendered length and the rendered bytes. The write loop then overlays those bytes onto each 256-byte batch
//      it read from the backing, BEFORE the batch is written (n48_hg_overlay). VRAM therefore never holds Apple's gfx10
//      bytes at a substituted program's address, not even transiently: the bytes the copy writes there ARE ours.
//      WHY IT CANNOT WRITE OUTSIDE THE PROGRAM'S EXTENT: n48_hg_overlay changes only the bytes of the batch buffer that lie in
//      [p.off, p.off + p.nb) - the patch's own range, which is exactly the range the post-copy substitution writes today (the
//      same render, the same `nb`, bounded by the entry's capacity) - and the batch buffer's destination is the copy's own
//      pre-flighted VRAM range, unchanged. It never extends a batch, never adds a write, never moves one.
//   2. A HEAP GENERATION COUNTER (the backstop). A copy that overlaps any substituted program (it carries a patch or a
//      poisoned program, or its destination overlaps a VRAM range a substitution has ever written) bumps `gen` when it
//      STARTS (before its first write) and again when its substitution COMPLETES (the copy scope's close, after the post-copy
//      substitution and kernsub): odd = in progress. `active` counts such copies, so two concurrent copies cannot make `gen`
//      look even while one of them is still writing. The judged frame records `gen`/`active` BEFORE it reads any program
//      (n48_hg_frame_begin); the commit (gfxsrc_commit_try's `live`) and the ring exemption (gfx_neuter.h's heap_refuse) both
//      refuse the frame when anything moved (n48_hg_judge). A program that could not be patched in the copy - or a copy that
//      failed or did not read back clean - is POISONED by GPU VA, and every frame that names a program in a poisoned range is
//      refused until a later overlapping copy completes cleanly.
//
// build 0.0.496 (the review of 0.0.495), two fixes, both of switch 62's ON behaviour only:
//   F4 THE OVERLAY RE-CHECK (n48_hg_overlay_checked). The pre-scan matched Apple's program at the patch's offset in the backing
//      as it read then; the write loop reads the backing again, batch by batch. Before a batch is overlaid, the batch's backing
//      bytes over the matched Apple program's range [p.off, p.off + p.apple_nb) must still equal the bytes the pre-scan matched
//      (kept in the copy's apple arena). A patch whose bytes differ is marked BAD: it is not overlaid in this batch nor in any
//      later one, the copier POISONS the program's range and the copy completes UNCLEAN (fail-closed). A patch with no recorded
//      Apple bytes (apple_nb 0) cannot be re-checked: the copier poisons it at the pre-scan instead of patching it.
//   F1 THE REGISTRY PRUNES AND NEVER SATURATES ON A NORMAL RUN (n48_hg_reg_add, n48_hg_reg_prune). 0.0.495's registry only grew;
//      RUN C (run10h) reached 133 distinct substituted ranges - every one of them still live at the end (8 heaps; each heap's last
//      patched copy re-substitutes its whole union) - so it saturated and every later copy counted as overlapping. Now: (a) a
//      copy that completes CLEAN with NO patch and NO poisoned program, while it is the only overlapping copy in progress,
//      removes every registry range it fully covers that no substitution touched since it began (each entry carries the
//      registry's add/touch sequence); a range a patched copy (re)registered after the pruning copy began, or while another
//      overlapping copy is in progress, is never removed; (b) a new range arriving when the registry is FULL is merged into
//      the hull of the nearest entry within N48_HG_REG_MERGE_GAP (a superset: more copies overlap, never fewer) instead of
//      setting reg_over; reg_over (every copy overlaps) remains only for a range with no entry that close.
//
// Pure C that compiles as C++; no heap, no libc beyond memcpy; the kext compiles THIS header, and tests/gfx_copyguard_test.cpp
// drives the same functions with RUN D's real order (section T18) and, 0.0.496, RUN C's real registry sequence (section T20).
#ifndef N48_GFX_HEAPGEN_H
#define N48_GFX_HEAPGEN_H

#include <stdint.h>
#include <string.h>

// ---- capacities ------------------------------------------------------------------------------------------------------------
#define N48_HG_PATCH_MAX   64u               // programs one copy may patch in place (RUN D's heap: 31)
#define N48_HG_ARENA_BYTES (64u * 1024u)     // the rendered bytes of one copy's patches (RUN D's heap: 0x9000 bytes in all)
#define N48_HG_REG_MAX     128u              // distinct VRAM ranges a substitution has written this boot
#define N48_HG_POISON_MAX  32u               // poisoned program ranges tracked before the table saturates (then: ALL refused)
#define N48_HG_PVA_MAX     64u               // program VAs one judged frame names (== N48_XV_MAX_PGMS)
#define N48_HG_REG_MERGE_GAP (1ull << 20)    // 0.0.496 F1: a full registry merges a new range into an entry at most this far away

// One program patched in place: resource offset, rendered length, where its bytes live in the copy's arena, and the VRAM
// address the copy's own segment walk gives for `off` (the registry's entry).
// 0.0.496 F4: `apple_nb`/`apple_off` - the matched Apple program's length and where the pre-scan's copy of its bytes lives in the
// copy's apple arena; `bad` - the re-check found the backing changed: never overlaid again in this copy.
typedef struct { uint32_t off, nb, arena_off, pad; uint64_t vram; uint32_t apple_nb, apple_off, bad, pad2; } n48_hg_patch;
// One poisoned range. `va_ok` 0 means the copy's GPU VA was unavailable: the entry then matches EVERY frame (fail closed)
// until a clean copy over its VRAM range clears it.
typedef struct { uint64_t va_lo, va_hi, vram_lo, vram_hi; uint32_t va_ok, used; } n48_hg_poison;
typedef struct { uint64_t lo, hi, seq; } n48_hg_range;   // seq (0.0.496 F1): the registry sequence of its last add/touch

enum { N48_HG_OK = 0u, N48_HG_IN_PROGRESS, N48_HG_CHANGED, N48_HG_POISONED, N48_HG_STALE, N48_HG_REASONS };
static inline const char *n48_hg_reason_name(uint32_t r)
{
    static const char *const n[N48_HG_REASONS] = { "ok", "copy-in-progress", "generation-changed", "program-poisoned",
                                                    "stale-verdict" };
    return r < N48_HG_REASONS ? n[r] : "?";
}

// ---- build 0.0.533 (; an internal design note): SWITCH 88's TYPES (the judge is further down) ------------
// The copier's side keeps a RECORD of every copy that bumped the generation: the generation it began at and the one it ended at
// (0 = still writing), its VRAM range, its GPU VA range, the pid it was made for and whether it rewrote the SAME resource's SAME
// VRAM range as that resource's previous copy (`in_place`). The frame's side maps every 4 KiB page of each program's window to
// VRAM. Under switch 88 the judge then refuses only when a copy that moved the generation could have touched the frame's programs.
#define N48_HG88_RING      16u               // copy records (the 16th push evicts: a closed one first, the oldest active last)
#define N48_HG88_PG_MAX    128u              // VRAM pages one frame's programs span (FULL = unresolved: refused)
#define N48_HG88_RES_MAX   32u               // resources whose last copy's VRAM range is remembered (in_place)
#define N48_HG88_PGM_BYTES (1344u * 4u)      // a program's window, kXdPgmDwords * 4 (the kext static_asserts it)
typedef struct {
    uint64_t tok;                                  // the record's token (0 = a free row); never reused
    uint64_t begin_gen, end_gen;                   // the generation after its start bump / after its end bump (0 = still active)
    uint64_t vram_lo, vram_hi;                     // [vram_lo, vram_hi) when range_ok
    uint64_t va_lo, va_hi;                         // [va_lo, va_hi) in the copier's VM when va_ok
    int32_t  pid;                                  // the copying process (0 = kernel / unknown: every frame's pid)
    uint32_t range_ok, va_ok, in_place;
} n48_hg88_rec;
typedef struct { uintptr_t res; uint64_t lo, hi; } n48_hg88_res;
// One copy's identity, filled by the copier (Navi48AccelPeer.cpp ic_begin) and handed to hw_hg_copy_begin, which returns the
// record's token in `tok` (0 = no record: the copy did not bump). hw_hg_copy_end closes the record by that token.
typedef struct { uintptr_t res; uint64_t va, bytes, tok; int32_t pid; uint32_t va_ok, range_ok, pad; } n48_hg_copy_id;
enum { N48_HG88_SPARE = 0u, N48_HG88_OVERLAP, N48_HG88_NOT_IN_PLACE, N48_HG88_UNRESOLVED, N48_HG88_LOST, N48_HG88_UNTRACKED,
       N48_HG88_REASONS };

typedef struct {
    uint64_t gen;                                  // bumped at an overlapping copy's start and at its completion
    uint32_t active;                               // overlapping copies between start and completion
    uint32_t nreg, reg_over;                       // the substituted-VRAM registry; reg_over: every copy overlaps
    uint64_t reg_seq;                              // 0.0.496 F1: bumped by every registry add or touch
    uint64_t reg_pruned, reg_merged;               // 0.0.496 F1: ranges removed by clean no-patch copies; merged when full
    n48_hg_range reg[N48_HG_REG_MAX];
    n48_hg_poison poison[N48_HG_POISON_MAX];
    uint32_t poison_over;                          // the table saturated: every frame with a program is refused (sticky)
    // counters for the log lines and the `gfxneuter 62` report
    uint64_t copies_bumped, copies_patched, programs_patched, bytes_patched;
    uint64_t copies_poisoned, programs_poisoned, poison_cleared, copies_clean;
    uint64_t judged, refused[N48_HG_REASONS], ex_refused;
    // build 0.0.533 (switch 88): THE COPY RECORDS, under gHgLock, kept for every bumping copy whatever 88 says (a record is
    // arithmetic; 88 OFF never reads one). Appended: nothing above moves.
    uint64_t c88_tok;                              // tokens issued
    n48_hg88_rec c88[N48_HG88_RING];
    uint64_t c88_evict_end;                        // the largest end_gen of an EVICTED record (closed at eviction, or ended after it)
    uint32_t c88_lost_active, c88_res_next;        // ACTIVE records evicted and not yet ended; the resource table's next row
    n48_hg88_res c88_res[N48_HG88_RES_MAX];
    uint64_t c88_pushed, c88_evicted, c88_untracked, c88_orphan_ends;
    // 88's own counts (the kext's judge and walk helpers, under gHgLock)
    uint64_t j88_judged, j88_spared[2], j88_ref[N48_HG88_REASONS], j88_walk_spared;
} n48_hg_state;

// The judged frame's record: the latch, the generation it was judged at, and the program VAs it named.
typedef struct {
    uint32_t on;                                   // switch 62, latched once at the top of the frame's pass
    uint32_t snap_active;
    uint64_t judged;                               // gXdC.judged + 1 at the top of the pass: THIS frame
    uint64_t snap_gen;
    uint32_t npva, pva_over;
    uint64_t pva[N48_HG_PVA_MAX];
    // build 0.0.533 (switch 88): appended. `r88`: 88 ON in the start-up window, latched at the top of the pass (0: the judge is
    // 0.0.532's); the pid the programs were identified for; every program page's VRAM offset (the struct copy into the committed
    // record carries them to the walk). `unresolved`: a page that is host memory, unmappable, or the list FULL.
    uint32_t r88, pid_set, pid_mixed;
    int32_t  pid;
    uint32_t npg, unresolved;
    uint64_t pg[N48_HG88_PG_MAX];
} n48_hg_frame;

// ---- the registry: where a substitution has ever written -------------------------------------------------------------------
static inline int n48_hg_overlaps(uint64_t lo1, uint64_t hi1, uint64_t lo2, uint64_t hi2) { return lo1 < hi2 && lo2 < hi1; }

static inline int n48_hg_reg_overlaps(const n48_hg_state *s, uint64_t lo, uint64_t hi)
{
    if (s->reg_over) return 1;                     // a saturated registry cannot say "no": every copy overlaps
    for (uint32_t i = 0; i < s->nreg && i < N48_HG_REG_MAX; i++)
        if (n48_hg_overlaps(s->reg[i].lo, s->reg[i].hi, lo, hi)) return 1;
    return 0;
}

static inline void n48_hg_reg_add(n48_hg_state *s, uint64_t lo, uint64_t hi)
{
    if (hi <= lo) return;
    s->reg_seq++;
    for (uint32_t i = 0; i < s->nreg && i < N48_HG_REG_MAX; i++)
        if (s->reg[i].lo <= lo && hi <= s->reg[i].hi) { s->reg[i].seq = s->reg_seq; return; }   // already covered: TOUCHED
    if (s->nreg >= N48_HG_REG_MAX) {
        // 0.0.496 F1 (b): FULL. Merge into the nearest entry's hull when it is close (a superset: never fewer overlaps).
        uint32_t best = N48_HG_REG_MAX; uint64_t bestGap = ~0ull;
        for (uint32_t i = 0; i < N48_HG_REG_MAX; i++) {
            const n48_hg_range *r = &s->reg[i];
            const uint64_t gap = hi < r->lo ? r->lo - hi : (r->hi < lo ? lo - r->hi : 0ull);
            if (gap < bestGap) { bestGap = gap; best = i; }
        }
        if (best < N48_HG_REG_MAX && bestGap <= N48_HG_REG_MERGE_GAP) {
            n48_hg_range *r = &s->reg[best];
            if (lo < r->lo) r->lo = lo;
            if (hi > r->hi) r->hi = hi;
            r->seq = s->reg_seq;
            s->reg_merged++;
            return;
        }
        s->reg_over = 1u;
        return;
    }
    s->reg[s->nreg].lo = lo; s->reg[s->nreg].hi = hi; s->reg[s->nreg].seq = s->reg_seq; s->nreg++;
}

// 0.0.496 F1 (a): THE PRUNE, asked by a copy that bumped, at its completion and BEFORE n48_hg_copy_end (so `active` still counts
// it). Only a copy that completed CLEAN, patched nothing and poisoned nothing, while it is the ONLY overlapping copy in progress,
// removes the registry ranges it FULLY covers whose last add/touch precedes `seq_begin` (the registry sequence when the copy
// began): its bytes replaced ours there and nothing substituted there since. Returns the ranges removed.
static inline uint32_t n48_hg_reg_prune(n48_hg_state *s, uint32_t clean, uint32_t npatched, uint32_t npoisoned, uint64_t seq_begin,
                                        uint64_t vram_lo, uint64_t vram_hi)
{
    if (!clean || npatched || npoisoned || s->active != 1u || vram_hi <= vram_lo) return 0u;
    uint32_t removed = 0u;
    for (uint32_t i = 0; i < s->nreg && i < N48_HG_REG_MAX; ) {
        const n48_hg_range *r = &s->reg[i];
        if (r->lo >= vram_lo && r->hi <= vram_hi && r->seq <= seq_begin) {
            s->reg[i] = s->reg[s->nreg - 1u];
            s->nreg--;
            removed++;
            continue;
        }
        i++;
    }
    s->reg_pruned += removed;
    return removed;
}

// ---- the poison table --------------------------------------------------------------------------------------------------------
static inline void n48_hg_poison_add(n48_hg_state *s, const n48_hg_poison *e)
{
    for (uint32_t i = 0; i < N48_HG_POISON_MAX; i++)
        if (!s->poison[i].used) { s->poison[i] = *e; s->poison[i].used = 1u; s->programs_poisoned++; return; }
    s->poison_over = 1u;                           // never silently dropped: saturation refuses every frame
    s->programs_poisoned++;
}

// build 0.0.509 item 2 (F-2 of the 0.0.496 review,): a STICKY entry (used == N48_HG_POISON_STICKY) marks a range a
// late SDMA write may still land in (a fast-copy chunk whose fence did not land): poisoned for the rest of the boot, never
// cleared by a clean copy. Saturation refuses every frame, as for any entry.
#define N48_HG_POISON_STICKY 2u
static inline void n48_hg_poison_add_sticky(n48_hg_state *s, const n48_hg_poison *e)
{
    for (uint32_t i = 0; i < N48_HG_POISON_MAX; i++)
        if (!s->poison[i].used) { s->poison[i] = *e; s->poison[i].used = N48_HG_POISON_STICKY; s->programs_poisoned++; return; }
    s->poison_over = 1u;
    s->programs_poisoned++;
}

// A clean completed copy clears every poisoned range it wholly covers: by VA when both carry one, else by VRAM.
// 0.0.509: never a sticky entry.
static inline void n48_hg_poison_clear(n48_hg_state *s, uint32_t va_ok, uint64_t va_lo, uint64_t va_hi, uint64_t vram_lo,
                                       uint64_t vram_hi)
{
    for (uint32_t i = 0; i < N48_HG_POISON_MAX; i++) {
        n48_hg_poison *p = &s->poison[i];
        if (!p->used || p->used == N48_HG_POISON_STICKY) continue;
        const int byVa = va_ok && p->va_ok && p->va_lo >= va_lo && p->va_hi <= va_hi;
        const int byVram = p->vram_hi > p->vram_lo && p->vram_lo >= vram_lo && p->vram_hi <= vram_hi;
        if (byVa || (!p->va_ok && byVram) || (p->va_ok && !va_ok && byVram)) { p->used = 0u; s->poison_cleared++; }
    }
}

// ---- the copy's two bumps ------------------------------------------------------------------------------------------------
// START: called BEFORE the copy's first VRAM write. `overlaps` is the caller's answer (a patch, a poisoned program, or the
// registry). Returns 1 when this copy bumped (and must therefore call n48_hg_copy_end exactly once).
static inline uint32_t n48_hg_copy_begin(n48_hg_state *s, uint32_t overlaps)
{
    if (!overlaps) return 0u;                      // a copy that overlaps no substituted program changes nothing
    s->active++;
    s->gen++;
    s->copies_bumped++;
    return 1u;
}

// COMPLETION: called once, after the post-copy substitution, for a copy that bumped. `clean` = every byte written, read back
// equal, and every patch overlaid in full. A clean copy first clears the poison it covers; an unclean one poisons its whole
// range. Then the copy's own unpatchable programs (`add`, `nadd`) are poisoned, whatever `clean` says. Then the second bump.
static inline void n48_hg_copy_end(n48_hg_state *s, uint32_t clean, uint32_t va_ok, uint64_t va_lo, uint64_t va_hi,
                                   uint64_t vram_lo, uint64_t vram_hi, const n48_hg_poison *add, uint32_t nadd)
{
    if (clean) { n48_hg_poison_clear(s, va_ok, va_lo, va_hi, vram_lo, vram_hi); s->copies_clean++; }
    else {
        n48_hg_poison whole;
        whole.va_lo = va_lo; whole.va_hi = va_hi; whole.vram_lo = vram_lo; whole.vram_hi = vram_hi;
        whole.va_ok = va_ok; whole.used = 1u;
        n48_hg_poison_add(s, &whole);
    }
    for (uint32_t i = 0; add && i < nadd; i++) n48_hg_poison_add(s, &add[i]);
    if (!clean || nadd) s->copies_poisoned++;
    s->gen++;
    if (s->active) s->active--;
}

// ---- the verdict side ------------------------------------------------------------------------------------------------------
// The top of the judged frame's pass, BEFORE any program is read. `on` is switch 62's value, latched here and only here.
static inline void n48_hg_frame_begin(n48_hg_frame *f, uint32_t on, uint64_t judged, const n48_hg_state *s)
{
    f->on = on ? 1u : 0u;
    f->judged = judged;
    f->snap_gen = on ? s->gen : 0ull;
    f->snap_active = on ? s->active : 0u;
    f->npva = 0u; f->pva_over = 0u;
    f->r88 = 0u; f->pid_set = 0u; f->pid_mixed = 0u; f->pid = 0; f->npg = 0u; f->unresolved = 0u;   // 0.0.533: n48_hg88_latch sets r88
}

// One program VA the frame names (gfxsrc_identify). A full list sets pva_over, which refuses while any poison stands.
static inline void n48_hg_frame_note_pva(n48_hg_frame *f, uint64_t va)
{
    if (!f->on) return;
    for (uint32_t i = 0; i < f->npva; i++) if (f->pva[i] == va) return;
    if (f->npva >= N48_HG_PVA_MAX) { f->pva_over = 1u; return; }
    f->pva[f->npva++] = va;
}

static inline int n48_hg_frame_poisoned(const n48_hg_state *s, const n48_hg_frame *f)
{
    uint32_t any = 0u;
    for (uint32_t i = 0; i < N48_HG_POISON_MAX; i++) {
        const n48_hg_poison *p = &s->poison[i];
        if (!p->used) continue;
        any = 1u;
        if (!p->va_ok) return 1;                   // a range we could not name by VA: every frame
        for (uint32_t k = 0; k < f->npva && k < N48_HG_PVA_MAX; k++)
            if (f->pva[k] >= p->va_lo && f->pva[k] < p->va_hi) return 1;
    }
    if (s->poison_over) return 1;
    if (any && f->pva_over) return 1;              // a program we did not record may be the poisoned one
    return 0;
}

// ---- build 0.0.533 (; an internal design note): SWITCH 88, THE RANGE-PRECISE HEAP-GENERATION JUDGE ----------
// RUN AE (run11q) and RUN AB2 (run11m) lost their start-up fill to `copy-in-progress` for a copy that could not touch the fill's
// programs: AE's was SecurityAgent's heap (another VM, VRAM [0x12300000,0x12309000)), AB2's a WindowServer heap at VA 0x4011f0000
// with neither program in it. The generation counter is GLOBAL, so today's judge refuses on ANY overlapping copy. Under 88
// (344 ON, start-up window only; 600 OFF, the default) the judge still refuses on every copy that could have touched the frame's
// programs, and only then:
//   a RELEVANT copy is one still active, or one that ended after the frame's snapshot (end_gen > snap_gen); the frame is refused
//   (with today's answer: copy-in-progress or generation-changed) when any relevant copy
//     - has an UNKNOWN range (the untracked no-slot copy)                          -> untracked
//     - overlaps one of the frame's program pages in VRAM                          -> overlap
//     - is not IN PLACE (not the same resource's same VRAM range as its last copy) -> not-in-place
//     - was made for the frame's pid (or pid 0, or a frame of several pids) and its GPU VA range meets a program's window, or its
//       VA is unknown                                                               -> overlap
//   or when the frame's pages are UNRESOLVED (host memory, unmappable, the page or program list FULL), or events were LOST (a record
//   evicted after the snapshot or while active, or the active records do not account for `active`, or nothing recorded explains the
//   move). The poison and STALE answers are unchanged. With `r88` 0 (latched) every answer is 0.0.532's.

// The pass's latch: 88 ON (`on88`) and the start-up window (`startup`: a continuous arm before CONTINUOUS START), 62 latched ON.
static inline void n48_hg88_latch(n48_hg_frame *f, uint32_t on88, uint32_t startup)
{
    f->r88 = (f->on && on88 && startup) ? 1u : 0u;
}
// This pass already recorded the program VA (its pages were mapped when it was first named).
static inline int n48_hg88_va_known(const n48_hg_frame *f, uint64_t va)
{
    for (uint32_t i = 0; i < f->npva && i < N48_HG_PVA_MAX; i++) if (f->pva[i] == va) return 1;
    return 0;
}
static inline void n48_hg88_note_pid(n48_hg_frame *f, int32_t pid)
{
    if (!f->r88) return;
    if (!f->pid_set) { f->pid = pid; f->pid_set = 1u; }
    else if (f->pid != pid) f->pid_mixed = 1u;
}
// One 4 KiB page of a program's window: its VRAM offset when `resolved` (mapped, not host memory), else the frame is unresolved.
static inline void n48_hg88_note_page(n48_hg_frame *f, uint64_t vram_page, uint32_t resolved)
{
    if (!f->r88) return;
    if (!resolved) { f->unresolved = 1u; return; }
    vram_page &= ~0xfffull;
    for (uint32_t i = 0; i < f->npg && i < N48_HG88_PG_MAX; i++) if (f->pg[i] == vram_page) return;
    if (f->npg >= N48_HG88_PG_MAX) { f->unresolved = 1u; return; }
    f->pg[f->npg++] = vram_page;
}

// THE COPIER's RECORD, in the bump's own gHgLock section (hw_hg_copy_begin). Every copy that reaches it updates the resource
// table; a copy that bumped also pushes a record. Returns the record's token (0: none).
static inline uint32_t n48_hg88_res_note(n48_hg_state *s, uintptr_t res, uint64_t lo, uint64_t hi, uint32_t range_ok)
{
    if (!res) return 0u;
    for (uint32_t i = 0; i < N48_HG88_RES_MAX; i++) {
        n48_hg88_res *r = &s->c88_res[i];
        if (r->res != res) continue;
        const uint32_t same = (range_ok && r->hi > r->lo && r->lo == lo && r->hi == hi) ? 1u : 0u;
        r->lo = range_ok ? lo : 0ull; r->hi = range_ok ? hi : 0ull;     // an unknown range: the NEXT copy is not in place either
        return same;
    }
    if (!range_ok) return 0u;
    n48_hg88_res *r = &s->c88_res[s->c88_res_next % N48_HG88_RES_MAX];
    s->c88_res_next++;
    r->res = res; r->lo = lo; r->hi = hi;
    return 0u;                                                             // no previous copy known: NOT in place
}
static inline uint64_t n48_hg88_push(n48_hg_state *s, const n48_hg_copy_id *id, uint64_t vram_lo, uint64_t vram_hi, uint32_t in_place)
{
    uint32_t v = N48_HG88_RING, act = N48_HG88_RING;
    for (uint32_t i = 0; i < N48_HG88_RING && v == N48_HG88_RING; i++) if (!s->c88[i].tok) v = i;
    if (v == N48_HG88_RING) {                                              // full: the closed record that ended first, else the oldest
        for (uint32_t i = 0; i < N48_HG88_RING; i++) {
            const n48_hg88_rec *r = &s->c88[i];
            if (r->end_gen) { if (v == N48_HG88_RING || r->end_gen < s->c88[v].end_gen) v = i; }
            else if (act == N48_HG88_RING || r->tok < s->c88[act].tok) act = i;
        }
        if (v == N48_HG88_RING) v = act;
        n48_hg88_rec *e = &s->c88[v];
        s->c88_evicted++;
        if (e->end_gen) { if (e->end_gen > s->c88_evict_end) s->c88_evict_end = e->end_gen; }
        else s->c88_lost_active++;                                         // its end can no longer be matched: LOST until it ends
    }
    n48_hg88_rec *r = &s->c88[v];
    r->tok = ++s->c88_tok;
    r->begin_gen = s->gen; r->end_gen = 0ull;
    r->range_ok = (id && id->range_ok && vram_hi > vram_lo) ? 1u : 0u;
    r->vram_lo = r->range_ok ? vram_lo : 0ull; r->vram_hi = r->range_ok ? vram_hi : 0ull;
    r->va_ok = (id && id->va_ok && id->bytes) ? 1u : 0u;
    r->va_lo = r->va_ok ? id->va : 0ull; r->va_hi = r->va_ok ? id->va + id->bytes : 0ull;
    r->pid = id ? id->pid : 0;
    r->in_place = in_place ? 1u : 0u;
    s->c88_pushed++;
    if (!r->range_ok) s->c88_untracked++;
    return r->tok;
}
static inline uint64_t n48_hg88_copy_note(n48_hg_state *s, uint32_t bumped, uint64_t vram_lo, uint64_t vram_hi, const n48_hg_copy_id *id)
{
    const uint32_t rok = (id && id->range_ok && vram_hi > vram_lo) ? 1u : 0u;
    const uint32_t in_place = n48_hg88_res_note(s, id ? id->res : 0u, vram_lo, vram_hi, rok);
    return bumped ? n48_hg88_push(s, id, vram_lo, vram_hi, in_place) : 0ull;
}
// THE COPIER's CLOSE, in hw_hg_copy_end's gHgLock section AFTER n48_hg_copy_end (its second bump): end_gen = the generation now.
// A token no longer in the ring was evicted while active: its end is recorded as lost events.
static inline void n48_hg88_close(n48_hg_state *s, uint64_t tok)
{
    for (uint32_t i = 0; tok && i < N48_HG88_RING; i++)
        if (s->c88[i].tok == tok && !s->c88[i].end_gen) { s->c88[i].end_gen = s->gen; return; }
    s->c88_orphan_ends++;
    if (tok && tok <= s->c88_tok && s->c88_lost_active) s->c88_lost_active--;
    if (s->gen > s->c88_evict_end) s->c88_evict_end = s->gen;
}

// Today's (0.0.532's) answer to "did anything move since the snapshot" - the three generation clauses of n48_hg_judge, in order.
static inline uint32_t n48_hg88_moved(const n48_hg_state *s, const n48_hg_frame *f)
{
    if ((f->snap_gen & 1ull) || f->snap_active) return N48_HG_IN_PROGRESS;
    if ((s->gen & 1ull) || s->active) return N48_HG_IN_PROGRESS;
    if (s->gen != f->snap_gen) return N48_HG_CHANGED;
    return N48_HG_OK;
}
// 88's verdict over the relevant copies: N48_HG88_SPARE (none could touch the frame's programs) or the first refusal's reason.
// `*first` (optional): the ring row of the first relevant copy (N48_HG88_RING if none).
static inline uint32_t n48_hg88_eval(const n48_hg_state *s, const n48_hg_frame *f, uint32_t *first)
{
    if (first) *first = N48_HG88_RING;
    if (f->unresolved || f->pva_over) return N48_HG88_UNRESOLVED;
    if (s->c88_lost_active || s->c88_evict_end > f->snap_gen) return N48_HG88_LOST;
    uint32_t nrel = 0u, nact = 0u;
    for (uint32_t i = 0; i < N48_HG88_RING; i++) {
        const n48_hg88_rec *r = &s->c88[i];
        if (!r->tok) continue;
        if (!r->end_gen) nact++;
        if (r->end_gen && r->end_gen <= f->snap_gen) continue;             // ended before the snapshot: not relevant
        if (first && !nrel) *first = i;
        nrel++;
        if (!r->range_ok) return N48_HG88_UNTRACKED;
        for (uint32_t k = 0; k < f->npg && k < N48_HG88_PG_MAX; k++)
            if (n48_hg_overlaps(f->pg[k], f->pg[k] + 0x1000ull, r->vram_lo, r->vram_hi)) return N48_HG88_OVERLAP;
        if (!r->in_place) return N48_HG88_NOT_IN_PLACE;
        if (r->pid == 0 || !f->pid_set || f->pid_mixed || r->pid == f->pid) {   // the same VM (or cannot tell): the VA clause
            if (!r->va_ok) return N48_HG88_OVERLAP;
            for (uint32_t k = 0; k < f->npva && k < N48_HG_PVA_MAX; k++)
                if (n48_hg_overlaps(f->pva[k], f->pva[k] + N48_HG88_PGM_BYTES, r->va_lo, r->va_hi)) return N48_HG88_OVERLAP;
        }
    }
    if (nact != s->active) return N48_HG88_LOST;                           // a copy in progress with no record (or the reverse)
    if (!nrel) return N48_HG88_LOST;                                       // the generation moved and no record says why
    return N48_HG88_SPARE;
}
// 88 turned today's move-refusal into a spare for this frame (the walk's and the rescue line's count).
static inline uint32_t n48_hg88_rescued(const n48_hg_state *s, const n48_hg_frame *f)
{
    return (f->on && f->r88 && n48_hg88_moved(s, f) != N48_HG_OK && n48_hg88_eval(s, f, (uint32_t *)0) == N48_HG88_SPARE) ? 1u : 0u;
}

// The one answer both the commit and the ring exemption use. `judged_now` is the ordinal the caller believes it is judging
// (the commit: gXdC.judged + 1; the exemption: the recorded frame's own, so STALE cannot fire there). N48_HG_OK only when the
// frame was judged with no overlapping copy in progress, none started or completed since, and none of its programs poisoned.
// build 0.0.533: under switch 88 (`r88`, latched) a move is forgiven only when n48_hg88_eval spares it; poison and STALE as before.
static inline uint32_t n48_hg_judge(const n48_hg_state *s, const n48_hg_frame *f, uint64_t judged_now)
{
    if (!f->on) return N48_HG_OK;                  // switch 62 OFF (latched): the pre-0.0.495 answer, always
    if (f->judged != judged_now) return N48_HG_STALE;
    const uint32_t moved = n48_hg88_moved(s, f);
    if (moved != N48_HG_OK && !(f->r88 && n48_hg88_eval(s, f, (uint32_t *)0) == N48_HG88_SPARE)) return moved;
    if (n48_hg_frame_poisoned(s, f)) return N48_HG_POISONED;
    return N48_HG_OK;
}

// The heapgen88 lines (n48log's 512-byte cap; tests/gfx_hg88_test.cpp measures both at their numeric widest).
#define N48_HG88_FMT \
    "heapgen88: M%u; judged %llu; spared disjoint in-progress/changed %llu/%llu; refused overlap %llu, not-in-place %llu, " \
    "unresolved %llu, lost %llu, untracked %llu; walk spared %llu"
#define N48_HG88_ARGS(on, s) \
    (on) ? 1u : 2u, (unsigned long long)(s)->j88_judged, (unsigned long long)(s)->j88_spared[0], \
    (unsigned long long)(s)->j88_spared[1], (unsigned long long)(s)->j88_ref[N48_HG88_OVERLAP], \
    (unsigned long long)(s)->j88_ref[N48_HG88_NOT_IN_PLACE], (unsigned long long)(s)->j88_ref[N48_HG88_UNRESOLVED], \
    (unsigned long long)(s)->j88_ref[N48_HG88_LOST], (unsigned long long)(s)->j88_ref[N48_HG88_UNTRACKED], \
    (unsigned long long)(s)->j88_walk_spared
#define N48_HG88_RESCUE_FMT \
    "heapgen88: RESCUED judged frame %llu (%s) - relevant copy over VRAM [%#llx,%#llx) pid %d, frame pid %d, %u program page(s) " \
    "disjoint; generation at the verdict %llu, now %llu (line %u of at most 16)"

// ---- build 0.0.510 PART B: THE COPY WAITS FOR A COMMITTED FRAME's WALK, switch 72 -------------
// RUN L (run10r,): a residency copy over a substituted heap STARTED between a frame's commit judge and its ring walk; the
// walk then refused the exemption (heap-gen), the IB was NOPed, the flight never ran and the continuous run stopped. The
// generation rule is right (the copy could have changed a program the frame was judged on); the TIMING is what cost the frame.
// Switch 72 (default OFF) moves such a copy LATER, never earlier, and never changes an answer:
//   - COMMIT PENDING. The commit judge (hg_commit_judge), when it answers OK for a frame headed for COMMIT and 72 is latched ON
//     for the pass, sets `pending` in the SAME gHgLock section as the judge (tentative: seq 0, the committing thread, the time).
//     The gate's record stamps the frame's token seq (hg_commit_record), or clears it when the gate did not commit. The ring walk
//     (hg_exempt_refuse) clears it in the SAME gHgLock section as its own judge, after it (spared OR refused). Every exit of the
//     commit hook clears the committing thread's flag (keystone withdrawal, token mismatch, the NOPed or un-walked frame's
//     teardown); a disarm (the one-shot's finish, `gfxneuter 5`, 72 OFF) clears it; a flag older than N48_HG_PEND_EXPIRE_US is
//     EXPIRED by the next copier that sees it.
//   - THE COPIER (hw_hg_copy_begin), holding gHgLock, BEFORE it computes its overlap and bumps: while `pending` is set by
//     ANOTHER thread and the copy would bump, it releases gHgLock, delays (IODelay, then IOSleep), and takes it again - at most
//     N48_HG_WAIT_US (100 ms) in all; then it proceeds EXACTLY as before (its overlap, the bump, the registry). A TIMEOUT
//     proceeds too: the walk still refuses (fail-closed). A copier on the committing thread itself never waits (it would wait
//     for itself). The check and the bump are one gHgLock section, so no frame can be judged between them.
// n48_hg_judge is untouched, so with 72 ON the walk refuses everything it refused before for the same order of events; the flag
// only changes WHICH order happens (tests/gfx_copyguard_test.cpp T26).
#define N48_HG_WAIT_US         100000u       // the copier's bound on one wait (the brief's 100 ms)
#define N48_HG_PEND_EXPIRE_US  1000000u      // a flag this old is a leak: the next copier clears it (and counts it)
#define N48_HG_WAIT_FINE_US    1000u         // IODelay(N48_HG_WAIT_STEP_US) for the first 1 ms, then IOSleep(1)
#define N48_HG_WAIT_STEP_US    20u

// Why a flag was cleared (the report's counts).
enum { N48_HG_PCLR_WALK = 0u, N48_HG_PCLR_GATE, N48_HG_PCLR_EXIT, N48_HG_PCLR_DISARM, N48_HG_PCLR_EXPIRED, N48_HG_PCLR_REPLACED,
       N48_HG_PCLR_TIMEOUT, N48_HG_PCLR_REASONS };   /* build 0.0.511: TIMEOUT (LOW-2 of the 0.0.510 review) */
typedef struct {
    uint32_t pending;          // a committed (or about-to-be-committed) frame's walk has not answered
    uint32_t seq;              // its gate token seq (0 = tentative: judged, not yet recorded)
    uintptr_t thread;          // the committing thread
    uint64_t set_us;           // when it was set
    uint64_t sets, clr[N48_HG_PCLR_REASONS];
    uint64_t waits, wait_us_total, wait_us_max, timeouts, self_skips, not_bumping;
    uint64_t unpatched;        // build 0.0.511: copies that found a flag but were not fully patched: bumped at once, no wait
    // build 0.0.532 (; an internal design note): SWITCH 72 M3 (840). Appended; M1/M2 never read or write them
    // except the two tests below (`m3` 0: one load each). PRE = the fields above; LIVE = the fields from `live` on.
    uint32_t m3;               // M3 in force (the verb, under gHgLock when it exists)
    uint32_t startup;          // latched at the judge: the pre-plane window (continuous arm, CONTINUOUS START not yet reached)
    uint64_t pre_fence_off;    // PRE: the committing flight's owned fence slot (VRAM offset), stamped right after its ring push
    uint32_t pre_want, pre_fence_seq;   // PRE: that fence's value (0 = fence-less) and the seq it was stamped for
    uint32_t live, live_seq;   // LIVE: a SPARED start-up fill is (or may be) on the GPU; its token seq
    uint32_t want, live_startup;        // LIVE: the value only its own fence writes; the startup latch it was set under
    uint64_t fence_off, live_set_us;    // LIVE: its owned slot, and when LIVE was set
    uint64_t lclr[8];          // LIVE ends, by N48_HG_LCLR_* (N48_HG_LCLR_REASONS <= 8, asserted below)
    uint64_t live_sets, fenceless, live_waits, live_wait_us_max, walknop_post_start, scope_skips, fence_unreadable;
    // 0.0.532 fix pass (the HIGH review's SHOULD-FIX): under M3 PRE outlives the walk (to EXIT 3). `pre_walk_ok`: the walk has
    // answered SPARED for this PRE, so a PRE timeout from here on bumps UNDER A RUNNING FRAME - not fail-closed: counted apart
    // (`pre_timeout_after_walk`, never in `timeouts`) and reported as TIMEOUT_LIVE.
    uint32_t pre_walk_ok;
    uint64_t pre_timeout_after_walk;
} n48_hg_pend;

// ---- build 0.0.532: SWITCH 72 M3 (840) - HOLD THE HEAP COPY FOR START-UP FILLS UNTIL THE FILL RETIRES, BY ITS OWN FENCE --------
//: the start-up voids were fills the walk NOPed for heap-gen - a residency copy started after the fill's judge and before its
// walk. (the design's corrections): 0.0.511's wait ended at the WALK, so a waiting copy then landed while the spared frame
// ran (the F4-BAD gap,); and the ring sees a retirement only at a judged frame or switch 65's expiry poll, while the waiting
// copier holds accel+0x88 (every submit takes it) - so THE COPIER READS THE FLIGHT'S FENCE ITSELF. Two phases:
//   PRE  (judge -> EXIT 3): as 0.0.511's flag, but the walk does NOT clear it under M3 (n48_hg_pend_clear refuses WALK), so it holds
//        from the walk to the hook's EXIT 3 with no gap. Set only in the start-up window (`startup`, latched at the judge).
//   LIVE (EXIT 3 SPARED with a fence -> the fence reads `want`): the copier waits for it (any bumping copy, patched or not), reading
//        the fence slot with gHgLock RELEASED. Ended by: the copier's read (RET_COPIER), the judged-frame poll (RET_POLL), switch
//        65's expiry poll (RET_EXPIRY), the flight's own expiry (FLIGHT_EXPIRED), the copier's 100 ms bound (TIMEOUT_LIVE, a
//        FINDING: the copy then writes under a possibly running frame), or a 2 s age (LIVE_AGE, == kKsFlightUs). Never by exits
//        1/2/4, a disarm or a withdrawal. A newer promotion REPLACES it (in-order EOP is a stop condition: the newest retiring
//        implies the older ones have).
#define N48_HG72_M3            3u            // the flag's value for `72 | 3 << 8` (= 840)
#define N48_HG_LIVE_AGE_US     2000000u      // == kKsFlightUs (the kext asserts it)
enum { N48_HG_LCLR_RET_COPIER = 0u, N48_HG_LCLR_RET_POLL, N48_HG_LCLR_RET_EXPIRY, N48_HG_LCLR_FLIGHT_EXPIRED, N48_HG_LCLR_TIMEOUT_LIVE,
       N48_HG_LCLR_LIVE_AGE, N48_HG_LCLR_REPLACED, N48_HG_LCLR_REASONS };
typedef char n48_hg_lclr_fits[(N48_HG_LCLR_REASONS <= 8u) ? 1 : -1];

// Switch 72's own setter (n48_ra_set knows only M 1 and M 2): M 1 -> 1 (328: kept byte-identical; NEVER to be sent - the F4-BAD
// gap), M 2 -> 0 (584, OFF, the default), M 3 -> 3 (840). Anything else: 0, `*flag` untouched.
static inline int n48_hg72_set(uint32_t m, uint32_t *flag)
{
    if (m != 1u && m != 2u && m != N48_HG72_M3) return 0;
    if (flag) *flag = (m == 1u) ? 1u : (m == N48_HG72_M3 ? N48_HG72_M3 : 0u);
    return 1;
}

// build 0.0.511 (MEDIUM-1 of the 0.0.510 review): MAY THIS COPY WAIT AT ALL? A waiting copy lands just AFTER the
// walk, i.e. while the spared frame is on the GPU. Only a copy that writes OUR bytes over every substituted program it overlaps
// may do that: it carries patches (np), none of its programs is unpatchable (npoison 0), the overlay is possible (!no_overlay:
// not a re-tiled copy) and its pre-scan finished with a plan (!scan_failed, !no_plan). Any other copy bumps at once, exactly as
// 0.0.509 did (the walk refuses). The F4 re-check (a patch whose Apple bytes changed, found while the copy writes) is not known
// when the copier decides; it is the remaining gap the review's full fix (clear at the flight's retirement) would close.
static inline uint32_t n48_hg_copy_may_wait(uint32_t np, uint32_t npoison, uint32_t no_overlay, uint32_t scan_failed, uint32_t no_plan)
{
    return (np && !npoison && !no_overlay && !scan_failed && !no_plan) ? 1u : 0u;
}

// The judge answered OK for a frame headed for COMMIT, 72 latched ON: under gHgLock, in the judge's own section.
static inline void n48_hg_pend_set(n48_hg_pend *pd, uintptr_t thread, uint64_t now_us)
{
    // build 0.0.532 (M3): only in the start-up window (`startup`, latched by the judge just before this call); after
    // CONTINUOUS START M3 sets nothing, which is 72 OFF's behaviour. M1/M2 (`m3` 0): one load, unchanged below.
    if (pd->m3) {
        if (!pd->startup) { pd->scope_skips++; return; }
        pd->pre_fence_off = 0ull; pd->pre_want = 0u; pd->pre_fence_seq = 0u;   // this frame's fence is stamped after its push
        pd->pre_walk_ok = 0u;                                                    // fix pass: the walk has not answered yet
    }
    if (pd->pending) pd->clr[N48_HG_PCLR_REPLACED]++;   // one flag: a newer committing frame replaces an older one's
    pd->pending = 1u; pd->seq = 0u; pd->thread = thread; pd->set_us = now_us;
    pd->sets++;
}
// The gate's record: its token seq (committed), or 0 (the gate did not commit: the tentative flag is cleared).
static inline void n48_hg_pend_record(n48_hg_pend *pd, uint32_t gate_seq, uintptr_t thread)
{
    if (!pd->pending || pd->thread != thread) return;
    if (gate_seq) { pd->seq = gate_seq; return; }
    pd->pending = 0u; pd->clr[N48_HG_PCLR_GATE]++;
}
// Clear: the walk answered for `seq` (or on the committing thread), the committing thread's hook returned, or a disarm (thread 0:
// any flag). Returns 1 when a flag was cleared.
static inline uint32_t n48_hg_pend_clear(n48_hg_pend *pd, uint32_t why, uint32_t seq, uintptr_t thread)
{
    if (!pd->pending) return 0u;
    // build 0.0.532 (M3): the walk never ends PRE - it holds to the hook's EXIT 3, which promotes a SPARED fill to LIVE
    // (n48_hg_pend_exit3). This covers n48_hg_walk_answer and the walk's record-mismatch refusal alike.
    if (pd->m3 && why == N48_HG_PCLR_WALK) return 0u;
    const uint32_t mine = thread == 0u || pd->thread == thread || (seq != 0u && pd->seq == seq);
    if (!mine) return 0u;
    pd->pending = 0u;
    if (why < N48_HG_PCLR_REASONS) pd->clr[why]++;
    return 1u;
}

// THE WALK'S ANSWER, under gHgLock: n48_hg_judge on the committed record, THEN the flag is cleared - one section, so a copy that
// was waiting can bump only after the answer is taken. Returns n48_hg_judge's answer unchanged.
static inline uint32_t n48_hg_walk_answer(const n48_hg_state *s, const n48_hg_frame *rec, n48_hg_pend *pd, uint32_t seq,
                                          uintptr_t thread)
{
    const uint32_t why = n48_hg_judge(s, rec, rec->judged);
    // 0.0.532 fix pass (M3 only): record whether the walk SPARED the frame this PRE holds (PRE then persists to EXIT 3).
    if (pd->m3 && pd->pending && pd->thread == thread && (!seq || pd->seq == seq)) pd->pre_walk_ok = (why == N48_HG_OK) ? 1u : 0u;
    (void)n48_hg_pend_clear(pd, N48_HG_PCLR_WALK, seq, thread);
    return why;
}

// One look by the copier, under gHgLock. `bumps`: this copy would bump now (its own patches/poison, or the registry).
enum { N48_HG_PW_GO = 0u, N48_HG_PW_WAIT, N48_HG_PW_TIMEOUT, N48_HG_PW_SELF, N48_HG_PW_EXPIRED, N48_HG_PW_NOBUMP };
static inline uint32_t n48_hg_pend_look(n48_hg_pend *pd, uintptr_t me, uint32_t bumps, uint64_t now_us, uint64_t t0_us)
{
    if (!pd->pending) return N48_HG_PW_GO;
    if (!bumps) return N48_HG_PW_NOBUMP;                 // a copy that will not bump changes nothing: no reason to wait
    if (pd->thread == me) return N48_HG_PW_SELF;         // the committing thread itself: waiting would wait for itself
    if (now_us >= pd->set_us && now_us - pd->set_us >= N48_HG_PEND_EXPIRE_US) {
        pd->pending = 0u; pd->clr[N48_HG_PCLR_EXPIRED]++;
        return N48_HG_PW_EXPIRED;
    }
    if (now_us >= t0_us && now_us - t0_us >= N48_HG_WAIT_US) return N48_HG_PW_TIMEOUT;
    return N48_HG_PW_WAIT;
}
static inline uint32_t n48_hg_wait_delay_us(uint64_t el_us) { return el_us < N48_HG_WAIT_FINE_US ? N48_HG_WAIT_STEP_US : 1000u; }

// THE COPIER's WAIT, driven through callbacks (the kext: gHgLock, clock_get_uptime, IODelay/IOSleep; the test: a scripted
// scheduler whose `delay` runs the other thread's steps). ENTERED AND LEFT WITH THE LOCK HELD; it releases the lock only around a
// delay. `own`: the copy carries patches or unpatchable programs (it bumps whatever the registry says); the registry overlap is
// re-asked on every look, under the lock. Returns the last look (never N48_HG_PW_WAIT); `*waited_us` the time spent waiting.
typedef struct {
    uint64_t (*now_us)(void *ctx);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*delay)(void *ctx, uint32_t us);
    // build 0.0.532 (M3): read one dword of VRAM (the LIVE flight's owned fence slot). Called by n48_hg_copy_wait3 ONLY with the
    // lock RELEASED (the kext: navi48_vram_read_mm - gVramMmLock only, never gXdLock). Returns 1 when read. n48_hg_copy_wait never
    // calls it (0.0.511's wait, M1/M2, may leave it null).
    uint32_t (*read_fence)(void *ctx, uint64_t vram_off, uint32_t *val);
} n48_hg_wait_ops;
static inline uint32_t n48_hg_copy_wait(const n48_hg_wait_ops *o, void *ctx, const n48_hg_state *s, n48_hg_pend *pd, uintptr_t me,
                                        uint32_t own, uint64_t vram_lo, uint64_t vram_hi, uint64_t *waited_us)
{
    const uint64_t t0 = o->now_us(ctx);
    uint64_t now = t0;
    uint32_t d = N48_HG_PW_GO, delayed = 0u;
    for (uint32_t guard = 0; guard < 200000u; guard++) {       // bounded twice: the elapsed time and the iterations
        const uint32_t bumps = (own || n48_hg_reg_overlaps(s, vram_lo, vram_hi)) ? 1u : 0u;
        d = n48_hg_pend_look(pd, me, bumps, now, t0);
        if (d != N48_HG_PW_WAIT) break;
        o->unlock(ctx);
        o->delay(ctx, n48_hg_wait_delay_us(now - t0));
        o->lock(ctx);
        delayed = 1u;
        now = o->now_us(ctx);
    }
    if (d == N48_HG_PW_WAIT) d = N48_HG_PW_TIMEOUT;             // the iteration bound: treated as the time bound
    const uint64_t w = now >= t0 ? now - t0 : 0u;
    if (delayed) {
        pd->waits++; pd->wait_us_total += w;
        if (w > pd->wait_us_max) pd->wait_us_max = w;
    }
    // build 0.0.511 (LOW-2 of the 0.0.510 review): a TIMEOUT also clears the flag (any thread's, in this same gHgLock section):
    // the copy bumps now and the walk will refuse anyway, so every later copy need not wait out the bound again.
    if (d == N48_HG_PW_TIMEOUT) { pd->timeouts++; (void)n48_hg_pend_clear(pd, N48_HG_PCLR_TIMEOUT, 0u, 0u); }
    else if (d == N48_HG_PW_SELF) pd->self_skips++;
    else if (d == N48_HG_PW_NOBUMP) pd->not_bumping++;
    if (waited_us) *waited_us = w;
    return d;
}

// ---- build 0.0.532: M3's pure steps (under gHgLock in the kext) --------------------------------------------------------------
// THE FENCE STAMP, right after the committing flight's ring push (n48_fr_push): its owned slot and value, kept on PRE until EXIT 3.
// Only for the PRE this thread set and the gate stamped with `seq`; anything else is left alone (the flight then counts as
// fence-less at EXIT 3: 0.0.511's behaviour, fail-safe).
static inline uint32_t n48_hg_pend_fence(n48_hg_pend *pd, uint32_t seq, uintptr_t thread, uint64_t vram_off, uint32_t want)
{
    if (!pd->m3 || !pd->pending || !seq || pd->seq != seq || pd->thread != thread) return 0u;
    pd->pre_fence_off = vram_off; pd->pre_want = want; pd->pre_fence_seq = seq;
    return 1u;
}

// THE ONE PROMOTE/CLEAR DECISION, at the commit hook's EXIT 3 (Apple's original returned; the walk's answer `spared` is known).
// Only the committing thread's PRE. SPARED with a fence: LIVE (PRE cleared). SPARED fence-less: cleared and counted `fenceless`
// (0.0.511's behaviour: the copy may land under the running frame). Anything else (NOPed, un-walked): cleared.
enum { N48_HG_X3_NONE = 0u, N48_HG_X3_PROMOTED, N48_HG_X3_FENCELESS, N48_HG_X3_CLEARED };
static inline uint32_t n48_hg_pend_exit3(n48_hg_pend *pd, uint32_t seq, uint32_t spared, uintptr_t thread, uint64_t now_us)
{
    if (!pd->m3 || !pd->pending || pd->thread != thread) return N48_HG_X3_NONE;
    pd->pending = 0u; pd->clr[N48_HG_PCLR_EXIT]++;
    if (!spared) return N48_HG_X3_CLEARED;
    if (!seq || pd->seq != seq || pd->pre_fence_seq != seq || pd->pre_want == 0u) { pd->fenceless++; return N48_HG_X3_FENCELESS; }
    if (pd->live) pd->lclr[N48_HG_LCLR_REPLACED]++;          // the newest spared flight is the one held
    pd->live = 1u; pd->live_seq = seq; pd->want = pd->pre_want; pd->fence_off = pd->pre_fence_off;
    pd->live_set_us = now_us; pd->live_startup = pd->startup;
    pd->live_sets++;
    return N48_HG_X3_PROMOTED;
}

// LIVE ends for token seq `seq` (a retirement seen by the ring's polls, the flight's own expiry, or the copier's own read).
static inline uint32_t n48_hg_pend_retired(n48_hg_pend *pd, uint32_t seq, uint32_t why)
{
    if (!pd->live || !seq || pd->live_seq != seq) return 0u;
    pd->live = 0u;
    if (why < N48_HG_LCLR_REASONS) pd->lclr[why]++;
    return 1u;
}

// THE M3 COPIER's WAIT (the kext calls it instead of n48_hg_copy_wait when `m3`). Entered and left with the lock held; the lock is
// released only around a delay and around the fence read. Waits while PRE is set by ANOTHER thread and this copy may wait (fully
// patched: `may_wait`, n48_hg_copy_may_wait), or while LIVE is set (any bumping copy: landing after the retirement is safe even for an
// unpatched one). SELF applies to PRE only. Bound: N48_HG_WAIT_US in all. A LIVE still set at the bound is TIMEOUT_LIVE (a finding;
// LIVE cleared so no later copy waits it out again); a PRE is cleared at its timeout as 0.0.511 does. Returns the last answer
// (N48_HG_PW_GO / _NOBUMP / _SELF / _EXPIRED / _TIMEOUT / _TIMEOUT_LIVE); `*waited_us` the time spent.
#define N48_HG_PW_TIMEOUT_LIVE 6u
static inline uint32_t n48_hg_copy_wait3(const n48_hg_wait_ops *o, void *ctx, const n48_hg_state *s, n48_hg_pend *pd, uintptr_t me,
                                         uint32_t own, uint32_t may_wait, uint64_t vram_lo, uint64_t vram_hi, uint64_t *waited_us)
{
    const uint64_t t0 = o->now_us(ctx);
    uint64_t now = t0;
    uint32_t d = N48_HG_PW_TIMEOUT, delayed = 0u, sawLive = 0u, self = 0u, expired = 0u, liveWait = 0u, preWait = 0u, done = 0u;
    for (uint32_t guard = 0; guard < 200000u && !done; guard++) {   // bounded twice: the elapsed time and the iterations
        const uint32_t bumps = (own || n48_hg_reg_overlaps(s, vram_lo, vram_hi)) ? 1u : 0u;
        if (!bumps) { d = (pd->pending || pd->live) ? N48_HG_PW_NOBUMP : N48_HG_PW_GO; done = 1u; break; }
        if (pd->live) {
            sawLive = 1u;
            if (now >= pd->live_set_us && now - pd->live_set_us >= N48_HG_LIVE_AGE_US) {
                (void)n48_hg_pend_retired(pd, pd->live_seq, N48_HG_LCLR_LIVE_AGE);
            } else if (o->read_fence) {
                const uint32_t ls = pd->live_seq, want = pd->want;
                const uint64_t off = pd->fence_off;
                uint32_t val = 0u;
                o->unlock(ctx);
                const uint32_t got = o->read_fence(ctx, off, &val);
                o->lock(ctx);
                if (!got) pd->fence_unreadable++;
                else if (val == want) (void)n48_hg_pend_retired(pd, ls, N48_HG_LCLR_RET_COPIER);   // exact epoch|ordinal only
                now = o->now_us(ctx);
            }
        }
        liveWait = pd->live;
        preWait = 0u;
        if (pd->pending && may_wait) {
            if (pd->thread == me) self = 1u;                                   // SELF: PRE only
            else if (now >= pd->set_us && now - pd->set_us >= N48_HG_PEND_EXPIRE_US) {
                pd->pending = 0u; pd->clr[N48_HG_PCLR_EXPIRED]++; expired = 1u;
            } else preWait = 1u;
        }
        if (!liveWait && !preWait) {
            d = expired ? N48_HG_PW_EXPIRED : (self ? N48_HG_PW_SELF : N48_HG_PW_GO);
            done = 1u; break;
        }
        if (now >= t0 && now - t0 >= N48_HG_WAIT_US) break;               // the time bound
        o->unlock(ctx);
        o->delay(ctx, n48_hg_wait_delay_us(now - t0));
        o->lock(ctx);
        delayed = 1u;
        now = o->now_us(ctx);
    }
    // 0.0.532 fix pass: a PRE whose walk already SPARED the frame is a running frame too (EXIT 3 has not promoted it yet): its
    // timeout is TIMEOUT_LIVE-class, captured BEFORE the PRE timeout-clear below.
    const uint32_t preAfterWalk = preWait && pd->pre_walk_ok;
    if (!done) d = (liveWait || preAfterWalk) ? N48_HG_PW_TIMEOUT_LIVE : N48_HG_PW_TIMEOUT;   // the time bound, or the iteration bound
    const uint64_t w = now >= t0 ? now - t0 : 0u;
    if (delayed) {
        pd->waits++; pd->wait_us_total += w;
        if (w > pd->wait_us_max) pd->wait_us_max = w;
        if (sawLive) { pd->live_waits++; if (w > pd->live_wait_us_max) pd->live_wait_us_max = w; }
    }
    if (!done) {
        if (liveWait && pd->live) (void)n48_hg_pend_retired(pd, pd->live_seq, N48_HG_LCLR_TIMEOUT_LIVE);   // a FINDING
        if (preAfterWalk) pd->pre_timeout_after_walk++;                   // a FINDING (fix pass): not a fail-closed PRE timeout
        else if (preWait) pd->timeouts++;
        if (preWait) (void)n48_hg_pend_clear(pd, N48_HG_PCLR_TIMEOUT, 0u, 0u);
    } else if (d == N48_HG_PW_SELF) pd->self_skips++;
    else if (d == N48_HG_PW_NOBUMP) pd->not_bumping++;
    if (waited_us) *waited_us = w;
    return d;
}

// The M3 line (the third heapwait72 line; T27 measures it at its numeric widest).
#define N48_HG_PEND_FMT3 \
    "heapwait72: M3 live set %llu, retired by copier/poll/expiry %llu/%llu/%llu, flight-expired %llu, fenceless %llu, timeouts " \
    "pre/live %llu/%llu, PRE after a spared walk %llu, live-waits max %llu us, post-START walk-NOP heap-gen %llu"
#define N48_HG_PEND_ARGS3(pd) \
    (unsigned long long)(pd)->live_sets, (unsigned long long)(pd)->lclr[N48_HG_LCLR_RET_COPIER], \
    (unsigned long long)(pd)->lclr[N48_HG_LCLR_RET_POLL], (unsigned long long)(pd)->lclr[N48_HG_LCLR_RET_EXPIRY], \
    (unsigned long long)(pd)->lclr[N48_HG_LCLR_FLIGHT_EXPIRED], (unsigned long long)(pd)->fenceless, \
    (unsigned long long)(pd)->timeouts, (unsigned long long)(pd)->lclr[N48_HG_LCLR_TIMEOUT_LIVE], \
    (unsigned long long)(pd)->pre_timeout_after_walk, (unsigned long long)(pd)->live_wait_us_max, (unsigned long long)(pd)->walknop_post_start

// Two lines (n48log's 512-byte cap; T26 measures both at their numeric widest): the switch and the flag's life, then the copier.
#define N48_HG_PEND_FMT \
    "heapwait72: switch 72 is %s%s; commits pending set %llu, cleared by the walk %llu, gate %llu, hook exit %llu, disarm %llu, " \
    "expired %llu, replaced %llu, timeout %llu; pending now %u (seq %u, age %llu us)"
#define N48_HG_PEND_ARGS(on, why, pd, age) \
    (on) == N48_HG72_M3 ? "M3 (840)" : (on) ? "ON (328)" : "OFF (584, default)", (why), (unsigned long long)(pd)->sets, \
    (unsigned long long)(pd)->clr[N48_HG_PCLR_WALK], (unsigned long long)(pd)->clr[N48_HG_PCLR_GATE], \
    (unsigned long long)(pd)->clr[N48_HG_PCLR_EXIT], (unsigned long long)(pd)->clr[N48_HG_PCLR_DISARM], \
    (unsigned long long)(pd)->clr[N48_HG_PCLR_EXPIRED], (unsigned long long)(pd)->clr[N48_HG_PCLR_REPLACED], \
    (unsigned long long)(pd)->clr[N48_HG_PCLR_TIMEOUT], (pd)->pending, (pd)->seq, (unsigned long long)(age)
#define N48_HG_PEND_FMT2 \
    "heapwait72: copier waits %llu (max %llu us, total %llu us), timeouts %llu (the walk then refuses, as without 72), on the " \
    "committing thread %llu (never waits), not bumping %llu, not fully patched %llu (bumped at once)"
#define N48_HG_PEND_ARGS2(pd) \
    (unsigned long long)(pd)->waits, (unsigned long long)(pd)->wait_us_max, (unsigned long long)(pd)->wait_us_total, \
    (unsigned long long)(pd)->timeouts, (unsigned long long)(pd)->self_skips, (unsigned long long)(pd)->not_bumping, \
    (unsigned long long)(pd)->unpatched

// ---- the overlay ----------------------------------------------------------------------------------------------------------
// The batch the copy is about to write holds resource bytes [res_off, res_off + take). Every patch byte that falls inside it
// replaces the backing's byte at the same offset. Returns the number of bytes overlaid. Nothing outside [p.off, p.off + p.nb)
// of any patch, and nothing outside [0, take) of the batch, is touched.
static inline uint32_t n48_hg_overlay(const n48_hg_patch *p, uint32_t n, const uint8_t *arena, uint64_t res_off, uint8_t *raw,
                                      uint32_t take)
{
    uint32_t done = 0u;
    const uint64_t bEnd = res_off + (uint64_t)take;
    for (uint32_t i = 0; p && arena && raw && i < n; i++) {
        const uint64_t pLo = p[i].off, pHi = (uint64_t)p[i].off + p[i].nb;
        const uint64_t lo = pLo > res_off ? pLo : res_off;
        const uint64_t hi = pHi < bEnd ? pHi : bEnd;
        if (lo >= hi) continue;
        memcpy(raw + (size_t)(lo - res_off), arena + p[i].arena_off + (size_t)(lo - pLo), (size_t)(hi - lo));
        done += (uint32_t)(hi - lo);
    }
    return done;
}

// 0.0.496 F4: THE OVERLAY WITH ITS RE-CHECK. First, for every patch not already bad that overlaps this batch: the batch's bytes
// (still exactly the backing's) over the matched Apple program's range [p.off, p.off + p.apple_nb) must equal the pre-scan's copy
// of them in `apple`; a patch with a difference, or with no recorded Apple bytes, is marked bad. Then every patch that is not
// bad is overlaid (n48_hg_overlay's placement, one patch at a time). `*newly_bad` counts the patches marked bad by THIS batch.
// Returns the bytes overlaid. The checks all run before any overlay, so one patch's bytes can never be compared as another's.
static inline uint32_t n48_hg_overlay_checked(n48_hg_patch *p, uint32_t n, const uint8_t *arena, const uint8_t *apple,
                                              uint64_t res_off, uint8_t *raw, uint32_t take, uint32_t *newly_bad)
{
    uint32_t bad = 0u, done = 0u;
    const uint64_t bEnd = res_off + (uint64_t)take;
    for (uint32_t i = 0; p && raw && i < n; i++) {
        if (p[i].bad) continue;
        const uint64_t pLo = p[i].off, pHi = (uint64_t)p[i].off + p[i].nb;
        if (!n48_hg_overlaps(pLo, pHi, res_off, bEnd)) continue;           // this batch holds none of the patch's bytes
        if (!p[i].apple_nb || !apple) { p[i].bad = 1u; bad++; continue; }  // nothing to re-check against: never overlaid
        const uint64_t aHi = pLo + p[i].apple_nb;
        const uint64_t lo = pLo > res_off ? pLo : res_off;
        const uint64_t hi = aHi < bEnd ? aHi : bEnd;
        if (lo < hi && memcmp(raw + (size_t)(lo - res_off), apple + p[i].apple_off + (size_t)(lo - pLo), (size_t)(hi - lo)) != 0) {
            p[i].bad = 1u; bad++;
        }
    }
    for (uint32_t i = 0; p && arena && raw && i < n; i++)
        if (!p[i].bad) done += n48_hg_overlay(&p[i], 1u, arena, res_off, raw, take);
    if (newly_bad) *newly_bad = bad;
    return done;
}

// ---- the report line (n48log's 512-byte cap; the test measures it at its numeric widest) ---------------------------------
// Two lines, so neither can pass the cap: the switch and the copies, then the refusals.
#define N48_HG_FMT \
    "shaderheap62: switch 62 is %s%s; gen %llu act %u; copies bumped %llu clean %llu, patched in place %llu (%llu pgm, " \
    "%llu B), poisoned %llu (%llu range(s), %llu cleared%s); registry %u%s"
#define N48_HG_ARGS(on, why, s) \
    (on) ? "ON" : "OFF (default)", (why), (unsigned long long)(s)->gen, (s)->active, \
    (unsigned long long)(s)->copies_bumped, (unsigned long long)(s)->copies_clean, (unsigned long long)(s)->copies_patched, \
    (unsigned long long)(s)->programs_patched, (unsigned long long)(s)->bytes_patched, \
    (unsigned long long)(s)->copies_poisoned, (unsigned long long)(s)->programs_poisoned, \
    (unsigned long long)(s)->poison_cleared, (s)->poison_over ? ", TABLE FULL: every frame refused" : "", \
    (s)->nreg, (s)->reg_over ? " FULL (every copy overlaps)" : ""
#define N48_HG_FMT2 \
    "shaderheap62: commits refused for a generation change: copy-in-progress %llu, generation-changed %llu, " \
    "program-poisoned %llu, stale-verdict %llu; ring exemptions refused (heap-gen) %llu; frames judged under 62 %llu; registry " \
    "pruned %llu merged %llu"
#define N48_HG_ARGS2(s) \
    (unsigned long long)(s)->refused[N48_HG_IN_PROGRESS], (unsigned long long)(s)->refused[N48_HG_CHANGED], \
    (unsigned long long)(s)->refused[N48_HG_POISONED], (unsigned long long)(s)->refused[N48_HG_STALE], \
    (unsigned long long)(s)->ex_refused, (unsigned long long)(s)->judged, (unsigned long long)(s)->reg_pruned, \
    (unsigned long long)(s)->reg_merged

#endif /* N48_GFX_HEAPGEN_H */
