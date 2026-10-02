// gfx_late540.h — build 0.0.540 item 6 (the capsule study): LATE-PHASE EVIDENCE. Every boot moves into a "late
// phase" at about judge frame 506-554, where today's instruments are blind (the capture's bodies stop at capture frame 160, tex531's
// 64 lines are spent by frame 86). LOGGING AND CAPTURE ONLY: nothing here decides anything, writes a register, a page table, a
// translated dword, or anything a rule reads. Pure, header-only; host-tested by tests/gfx_late540_test.cpp.
//   (a) THE CAPTURE'S LATE WINDOW (gcap_submission; the existing capture switch, `gfxcapture 1`): after the first 160 full frames
//       (unchanged), a frame's body is still captured when its IB-length signature has NOT been seen before in this capture arm (at
//       most N48_LT_NEW_BUDGET more), and N48_LT_TARGET_EACH of each of the six signatures the study names (whenever they occur).
//       Memory: the signature table is a file-scope static of N48_LT_SIGS rows (20 bytes each, 1280 bytes); the bodies land in the
//       capture's existing 32 MiB ring (no new allocation): at most N48_LT_NEW_BUDGET + 6 x N48_LT_TARGET_EACH = 36 more frames.
//   (b) tex531's LATE WINDOW (gfx_p87.h, switch 87): see N48_P87_LATE_* there.
//   (c) THE REAL GATE REASON (gfxsrc_commit_try; switch 96 ON): a frame that is NOT live reaches n48_cm_gate with no pages counted, so
//       the gate answers "page-not-host detail 0" - true of the write that never happened, not of why. n48_lt_live_first names the
//       FIRST failing clause of the kext's own `live` predicate, in the kext's order; logged with the frame's IB-length signature.
//   (d) A REFUSED SEGMENT (verdict segment-policy; switch 96 ON): the first refused segment's index, status (xlat12's reason code),
//       the policy's detail (opcode or register), its head dword position and the dword there.
//   (c) and (d) are capped by n48_lt_cap: N48_LT_EARLY lines from the arm's start, then N48_LT_LATE more from judge frame
//   N48_LT_LATE_FROM on; the rest counted.
#ifndef N48_GFX_LATE540_H
#define N48_GFX_LATE540_H

#include <stdint.h>

/* ---- (a) the capture's late window ---- */
#define N48_LT_SIGS         64u    /* signatures remembered per capture arm */
#define N48_LT_NEW_BUDGET   24u    /* late bodies of never-seen signatures per capture arm */
#define N48_LT_TARGET_EACH  2u     /* late bodies of each named signature per capture arm */
#define N48_LT_TARGETS      6u
#define N48_LT_MAX_IBS      4u     /* = N48_SCB_MAX_IBS */
typedef struct { uint32_t nib, len[N48_LT_MAX_IBS]; } n48_lt_sig;
typedef struct {
    n48_lt_sig seen[N48_LT_SIGS];
    uint32_t nseen;
    uint32_t newTaken, tgtTaken[N48_LT_TARGETS];
    uint64_t asked, takenNew, takenTgt, refusedBudget, tableFull;
} n48_lt_cap;
/* The six signatures the capsule study names: 16224|1776, 16192|1776, 16224|1760, 16192|1760, 14544, 14528. */
static inline uint32_t n48_lt_target(const n48_lt_sig *s)
{
    static const uint32_t t[N48_LT_TARGETS][3] = { { 2u, 16224u, 1776u }, { 2u, 16192u, 1776u }, { 2u, 16224u, 1760u },
                                                   { 2u, 16192u, 1760u }, { 1u, 14544u, 0u }, { 1u, 14528u, 0u } };
    for (uint32_t k = 0; k < N48_LT_TARGETS; k++)
        if (s->nib == t[k][0] && s->len[0] == t[k][1] && (s->nib < 2u || s->len[1] == t[k][2])) return k;
    return N48_LT_TARGETS;
}
static inline uint32_t n48_lt_sig_eq(const n48_lt_sig *a, const n48_lt_sig *b)
{
    if (a->nib != b->nib) return 0u;
    for (uint32_t k = 0; k < a->nib && k < N48_LT_MAX_IBS; k++) if (a->len[k] != b->len[k]) return 0u;
    return 1u;
}
static inline void n48_lt_cap_reset(n48_lt_cap *c)
{
    c->nseen = 0u; c->newTaken = 0u;
    for (uint32_t k = 0; k < N48_LT_TARGETS; k++) c->tgtTaken[k] = 0u;
}
/* Every captured frame (the first 160 too: `past_first` 0) records its signature; past the first 160 (`past_first` 1) the answer
 * is 1 when this frame's body should be captured: one of the named six under its own per-signature budget, else a signature not
 * seen before in this capture arm under the shared budget. A full table stops remembering (counted), never grows. */
static inline uint32_t n48_lt_cap_note(n48_lt_cap *c, const n48_lt_sig *s, uint32_t past_first)
{
    if (!c || !s || s->nib == 0u || s->nib > N48_LT_MAX_IBS) return 0u;
    uint32_t seen = 0u;
    for (uint32_t i = 0; i < c->nseen && i < N48_LT_SIGS; i++) if (n48_lt_sig_eq(&c->seen[i], s)) { seen = 1u; break; }
    uint32_t take = 0u;
    if (past_first) {
        c->asked++;
        const uint32_t t = n48_lt_target(s);
        if (t < N48_LT_TARGETS && c->tgtTaken[t] < N48_LT_TARGET_EACH) { c->tgtTaken[t]++; c->takenTgt++; take = 1u; }
        else if (!seen && c->newTaken < N48_LT_NEW_BUDGET) { c->newTaken++; c->takenNew++; take = 1u; }
        else if (!seen || t < N48_LT_TARGETS) c->refusedBudget++;
    }
    if (!seen) {
        if (c->nseen < N48_LT_SIGS) c->seen[c->nseen++] = *s;
        else c->tableFull++;
    }
    return take;
}
/* The signature as text ("16224|1776"), for the lines. `b` at least 48 bytes. */
static inline const char *n48_lt_sig_str(const n48_lt_sig *s, char *b, uint32_t cap)
{
    uint32_t o = 0u;
    if (!b || cap < 2u) return "";
    b[0] = 0;
    for (uint32_t k = 0; k < s->nib && k < N48_LT_MAX_IBS; k++) {
        char t[12]; uint32_t n = 0u, v = s->len[k];
        do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 11u);
        if (k && o + 1u < cap) b[o++] = '|';
        while (n && o + 1u < cap) b[o++] = t[--n];
    }
    b[o] = 0;
    return b;
}

/* ---- (c) / (d): the two-window line cap ---- */
#define N48_LT_EARLY      32u     /* lines from the arm's start */
#define N48_LT_LATE       64u     /* more lines, only for judge frames >= N48_LT_LATE_FROM */
#define N48_LT_LATE_FROM  480ull
typedef struct { uint32_t early, late; uint64_t suppressed; } n48_lt_lines;
static inline uint32_t n48_lt_cap_take(n48_lt_lines *l, uint64_t frame)
{
    if (!l) return 0u;
    if (l->early < N48_LT_EARLY) { l->early++; return 1u; }
    if (frame >= N48_LT_LATE_FROM && l->late < N48_LT_LATE) { l->late++; return 1u; }
    l->suppressed++;
    return 0u;
}
static inline void n48_lt_lines_reset(n48_lt_lines *l) { if (l) { l->early = 0u; l->late = 0u; } }

/* ---- (c): the first failing clause of gfxsrc_commit_try's `live`, in ITS order (arm, verdict, built, buffers, dependency,
 * memory-destination under ENFORCE, ring full, continuous without a fence, compute elide, draw elide, heap generation). 0 = live
 * (every clause holds: then the gate's own answer is the real one). The same boolean as the kext's line over every input
 * (tests/gfx_late540_test.cpp drives both). ---- */
enum { N48_LT_LIVE = 0u, N48_LT_NOT_ARMED, N48_LT_VERDICT, N48_LT_NOT_BUILT, N48_LT_NO_BUFFERS, N48_LT_DEP, N48_LT_MEMDST,
       N48_LT_RING_FULL, N48_LT_CONT_NO_FENCE, N48_LT_CS_ELIDE, N48_LT_DRAW_ELIDE, N48_LT_HEAPGEN, N48_LT_CLAUSES };
static inline const char *n48_lt_clause_name(uint32_t c)
{
    static const char *const n[N48_LT_CLAUSES] = { "live", "not-armed", "verdict", "not-built", "no-buffers", "DEPENDENCY-STALE",
        "MEMORY-DESTINATION", "RING-FULL", "CONTINUOUS-NO-FENCE", "COMPUTE-ELIDE-R1", "DRAW-ELIDE-R1", "SHADER-HEAP-GEN" };
    return c < N48_LT_CLAUSES ? n[c] : "?";
}
static inline uint32_t n48_lt_live_first(uint32_t armed, uint32_t translate, uint32_t built, uint32_t buffers_ok, uint32_t dep_ok,
                                         uint32_t md_enforce, uint32_t md_ok, uint32_t ring_full, uint32_t cont_on,
                                         uint32_t cont_fence_ok, uint32_t cs_elided, uint32_t draw_elided, uint32_t heap_refuse)
{
    if (!armed) return N48_LT_NOT_ARMED;
    if (!translate) return N48_LT_VERDICT;
    if (!built) return N48_LT_NOT_BUILT;
    if (!buffers_ok) return N48_LT_NO_BUFFERS;
    if (!dep_ok) return N48_LT_DEP;
    if (md_enforce && !md_ok) return N48_LT_MEMDST;
    if (ring_full) return N48_LT_RING_FULL;
    if (cont_on && !cont_fence_ok) return N48_LT_CONT_NO_FENCE;
    if (cs_elided && !(md_enforce && md_ok)) return N48_LT_CS_ELIDE;
    if (draw_elided && !(md_enforce && md_ok)) return N48_LT_DRAW_ELIDE;
    if (heap_refuse) return N48_LT_HEAPGEN;
    return N48_LT_LIVE;
}

/* ---- the lines (each <= 491 bytes at maximal fields; tests/gfx_late540_test.cpp) ---- */
/* (c) args: judge frame, signature, the first failing clause's name, the verdict number, the gate's answer name and detail, the
 * clause counts' index, line numbers early/late. */
#define N48_LT_GATE_FMT "late540: gate F%llu sig %s NOT LIVE - first failing clause %s (verdict %u); the gate itself answered %s " \
    "detail %#x (the pages were never counted); line %u+%u"
/* (d) args: judge frame, signature, segment index of nseg, xlat12 status, the policy's detail (opcode or register), head dword
 * position, the dword there, start, end, line numbers. */
#define N48_LT_SEG_FMT "late540: segment-policy F%llu sig %s seg %u of %u refused status %#x detail %#x head @%u = %08x start %u " \
    "end %u; line %u+%u"
/* (a) the capture verb's line. args: asked, taken new, taken named, refused budget, table rows, table full, the six named counts. */
#define N48_LT_CAP_FMT "gfx-capture late540: past frame 160 asked %llu, bodies taken new-signature %llu / named %llu, over budget %llu; " \
    "signatures %u of 64 (full %llu); named taken 16224|1776 %u 16192|1776 %u 16224|1760 %u 16192|1760 %u 14544 %u 14528 %u"
/* (c)/(d) the counters line (the perf540 verb). args: clause counts (11), segment-policy frames, gate lines suppressed, seg lines
 * suppressed. */
#define N48_LT_COUNT_FMT "late540: not-live frames by first clause: not-armed %llu verdict %llu not-built %llu no-buffers %llu dep %llu " \
    "memdst %llu ring-full %llu cont-no-fence %llu cs-elide %llu draw-elide %llu heap %llu; segment-policy frames %llu; " \
    "suppressed gate %llu seg %llu"

#endif /* N48_GFX_LATE540_H */
