// gfx_xlat_verdict.h — the TRANSLATE-OR-NEUTER decision at the source hook, and its accounting (designed 0.0.282).
// Pure C, host-tested by tests/gfx_xlat_verdict_test.cpp; not yet called by the kext (gfxsrc_verdict still answers NEUTER).
//
// The kext fills one n48_xv_frame from what the source capture already reads (gcap_submission: the reader root, each IB's dwords
// and walk, the programs the IB names with their keys and whether the bytes at each VA are OUR substituted gfx1201 program) plus the
// result of the IB policy (per segment) and of the descriptor translation. n48_xv_decide returns TRANSLATE only when every check
// passes and otherwise the FIRST blocking reason in a fixed order, with the key or register it concerns, so the tally says exactly
// what to build next. The order is the order of dependence: nothing about an IB can be judged without reading it, a frame whose
// programs are not ours must never reach the CP translated, and the IB policy is only meaningful over programs we control.
#ifndef N48_GFX_XLAT_VERDICT_H
#define N48_GFX_XLAT_VERDICT_H

#include <stdint.h>

enum {
    N48_XV_TRANSLATE = 0,
    N48_XV_ARMED_OFF,        /* translation not armed (the default): NEUTER */
    N48_XV_SHAPE,            /* not a frame the source route decides (0.0.362: the proven shape AND WindowServer's by owner) */
    N48_XV_NO_READER,        /* no page table maps a type-3 header at IB 0 (capture why 0 or 3) */
    N48_XV_IB_SHORT,         /* an IB page did not read (got < len) */
    N48_XV_IB_WALK,          /* an IB is not well-formed PM4 to exactly its length */
    N48_XV_IB_NOT_HOST,      /* an IB lies (partly) in VRAM: the in-place write path is host pages only */
    N48_XV_PGM_UNKNOWN,      /* a program's key is in no table we hold */
    N48_XV_PGM_NOT_OURS,     /* the key is known and translatable but the bytes at the VA are still Apple's (not substituted) */
    N48_XV_PGM_NO_XLAT,      /* the key is known but has no gfx1201 program (internal shader, failed check, too large) */
    N48_XV_SEG_POLICY,       /* the IB policy refused a segment (reason in `detail`: opcode or register) */
    N48_XV_DESC,             /* a descriptor record did not translate */
    N48_XV_TARGET_VRAM,      /* a colour target is in VRAM: WindowServer's CPU compositor would not see the draw */
    N48_XV_BUDGET,           /* the per-boot translated-frame budget is spent */
    /* 0.0.387 ( and the census below): OUR OWN TABLE RAN OUT. This is NOT a statement about the frame's
     * shaders - not one of its programs was examined - and for four runs it was reported as `program-unknown key 0`,
     * where it was 285 of arm9's 517 program-unknown frames and 282 of arm13's. It is appended, never inserted, so
     * every other reason keeps the numeric value four runs of logs were written with. */
    N48_XV_PGM_TABLE_OVERFLOW,
    N48_XV_REASONS
};

static inline const char *n48_xv_reason_name(uint32_t r)
{
    static const char *const n[N48_XV_REASONS] = {
        "TRANSLATE", "armed-off", "shape", "no-reader", "ib-short", "ib-walk", "ib-not-host", "program-unknown",
        "program-not-substituted", "program-no-translation", "segment-policy", "descriptor", "target-in-vram", "budget",
        "program-table-overflow" };
    return r < N48_XV_REASONS ? n[r] : "?";
}

#define N48_XV_MAX_IBS  4u
/* 0.0.387 — WHY 64, AND WHY THE OLD 16 WAS A MEASUREMENT AND NOT A LIMIT.
 *
 * MEASURED, over the IB bodies `capdecode.py` recovers from `arm9` and `arm13` (160 frames each, the only frames whose
 * bodies the capture keeps), by running THIS KEXT'S OWN `n48_gcap_scan` over every IB of every frame and deduplicating
 * program VAs across the IBs exactly as the gather does:
 *
 *     max distinct program VAs per frame, uncapped .................... 33   (arm9 33, arm13 33)
 *     max distinct program VAs per frame, under the gather's items[64]  19   (arm9 19, arm13 19)
 *     frames over 16 under the gather ................................. 24 of 160 (arm9), 23 of 160 (arm13)
 *
 * The scan's own per-IB `pgm[32]` dedupe array NEVER saturated on either run (max 21 distinct programs in one IB), so
 * the 33 is a real number and not another table's ceiling. 64 covers it with the whole measured distribution to spare
 * and is still bounded: the scan can emit at most 32 distinct programs per IB and a frame carries at most
 * N48_XV_MAX_IBS of them, so 128 is the structural ceiling and nothing above it could ever be reached.
 *
 * COST: sizeof(n48_xv_program) is 24, so n48_xv_frame goes 744 -> 1896 bytes. Both kext instances are `static` by
 * deliberate design (this runs deep in Apple's submit path under gXdLock), so the whole cost is .bss: two frames
 * +2304 B plus AppleHardwareHook's `pgmVa[N48_XV_MAX_PGMS]` 128 -> 512 B = +2688 B. No kernel stack grows. */
#define N48_XV_MAX_PGMS 64u
#define N48_XV_MAX_SEGS 32u

enum { N48_XV_PGM_KEY_UNKNOWN = 0, N48_XV_PGM_KEY_NO_XLAT = 1, N48_XV_PGM_KEY_XLAT = 2 };

typedef struct {
    uint64_t key;            /* shader-cache key of APPLE'S program for this VA (from the cache entry, or read before substitution) */
    uint32_t key_class;      /* N48_XV_PGM_KEY_* */
    uint32_t bytes_are_ours; /* 1 when the bytes at the VA now match our gfx1201 identity for that key */
    /* 0.0.311: RELOCATED - our gfx1201 build of this program is PLACED IN OUR OWN ARENA and the translated
     * draw will point SPI_SHADER_PGM_LO/HI_ES at that copy. Apple's bytes at the VA stay Apple's BY DESIGN and for ever,
     * so `bytes_are_ours` is 0 for a relocated program and always will be: a ladder that refuses on !bytes_are_ours
     * alone refuses such a program permanently, which is exactly what held RectPosTexFast_VS - and therefore every
     * SecurityAgent frame - at `program-no-translation`. This is the SECOND route to "our code will run", and it
     * is set from a real placement in the arena, never from the cache flag alone: an entry we hold code for but have not
     * placed must still refuse, because pointing a draw at an address holding nothing renders garbage instead of
     * refusing. The kext sets this from n48_reloc_find(), which answers only for bytes actually uploaded. */
    uint32_t relocated;
} n48_xv_program;

typedef struct {
    uint32_t armed, shape_ok, reader_ok, budget_left;
    uint32_t nib;
    struct { uint32_t len, got, walk, vram_pages; } ib[N48_XV_MAX_IBS];
    uint32_t npgm;
    n48_xv_program pgm[N48_XV_MAX_PGMS];
    uint32_t nseg;
    uint32_t seg_status[N48_XV_MAX_SEGS];   /* 0 accepted by the IB policy, else its status */
    uint32_t seg_detail[N48_XV_MAX_SEGS];   /* opcode or gfx10 register the policy refused on */
    uint32_t desc_status;                   /* 0 all descriptor records translated (or none), else the first refusal.: the
                                             * kext sets it from the first identified program that reads a client descriptor
                                             * (0x10000 | class << 12 | identity), since none is translated on that path */
    uint32_t target_vram;                   /* 1 when any colour target page is VRAM */
} n48_xv_frame;

/* Returns the verdict (N48_XV_TRANSLATE or a reason); *key_out the program key a program reason concerns, *detail_out the IB index,
 * segment index << 16 | policy detail low 16 bits, or descriptor status, as fits the reason. */
static inline uint32_t n48_xv_decide(const n48_xv_frame *f, uint64_t *key_out, uint32_t *detail_out)
{
    *key_out = 0u; *detail_out = 0u;
    if (!f->armed) return N48_XV_ARMED_OFF;
    if (!f->shape_ok) return N48_XV_SHAPE;
    if (!f->reader_ok) return N48_XV_NO_READER;
    if (f->nib == 0u || f->nib > N48_XV_MAX_IBS) return N48_XV_SHAPE;
    for (uint32_t i = 0; i < f->nib; i++) {
        if (f->ib[i].got < f->ib[i].len) { *detail_out = i; return N48_XV_IB_SHORT; }
        if (f->ib[i].walk != f->ib[i].len) { *detail_out = i; return N48_XV_IB_WALK; }
        if (f->ib[i].vram_pages) { *detail_out = i; return N48_XV_IB_NOT_HOST; }
    }
    /* 0.0.387: its OWN reason. The gather sets npgm = N48_XV_MAX_PGMS + 1 when it ran out of table, and until now that
     * answered `program-unknown` with *key_out left at 0 - indistinguishable in the tally from a program whose bytes
     * carry no gfx10 terminator. Not one program of such a frame was examined, so no key is set here either. */
    if (f->npgm > N48_XV_MAX_PGMS) return N48_XV_PGM_TABLE_OVERFLOW;
    for (uint32_t k = 0; k < f->npgm; k++)
        if (f->pgm[k].key_class == N48_XV_PGM_KEY_UNKNOWN) { *key_out = f->pgm[k].key; return N48_XV_PGM_UNKNOWN; }
    for (uint32_t k = 0; k < f->npgm; k++)
        if (f->pgm[k].key_class == N48_XV_PGM_KEY_NO_XLAT) { *key_out = f->pgm[k].key; return N48_XV_PGM_NO_XLAT; }
    for (uint32_t k = 0; k < f->npgm; k++)
        if (!f->pgm[k].bytes_are_ours && !f->pgm[k].relocated) { *key_out = f->pgm[k].key; return N48_XV_PGM_NOT_OURS; }
    if (f->nseg == 0u || f->nseg > N48_XV_MAX_SEGS) { *detail_out = f->nseg; return N48_XV_SEG_POLICY; }
    for (uint32_t s = 0; s < f->nseg; s++)
        if (f->seg_status[s]) { *detail_out = (s << 16) | (f->seg_detail[s] & 0xFFFFu); return N48_XV_SEG_POLICY; }
    if (f->desc_status) { *detail_out = f->desc_status; return N48_XV_DESC; }
    if (f->target_vram) return N48_XV_TARGET_VRAM;
    if (!f->budget_left) return N48_XV_BUDGET;
    return N48_XV_TRANSLATE;
}

/* ---- the tally: (pid, verdict, key) -> frames, a fixed table; overflow is counted, never silent (rule 72) ---- */
#define N48_XV_TALLY_ROWS 64u
typedef struct { int32_t pid; uint32_t verdict; uint64_t key; uint64_t frames; } n48_xv_row;
typedef struct { n48_xv_row row[N48_XV_TALLY_ROWS]; uint32_t used; uint64_t overflow; uint64_t by_verdict[N48_XV_REASONS]; } n48_xv_tally;

static inline void n48_xv_count(n48_xv_tally *t, int32_t pid, uint32_t verdict, uint64_t key)
{
    if (verdict < N48_XV_REASONS) t->by_verdict[verdict]++;
    for (uint32_t i = 0; i < t->used; i++)
        if (t->row[i].pid == pid && t->row[i].verdict == verdict && t->row[i].key == key) { t->row[i].frames++; return; }
    if (t->used < N48_XV_TALLY_ROWS) {
        n48_xv_row *r = &t->row[t->used++];
        r->pid = pid; r->verdict = verdict; r->key = key; r->frames = 1u;
        return;
    }
    t->overflow++;
}

/* build 0.0.474 item 3 (10B-COVERAGE.md Q4 item 4 contract (3)) — LOG-ONLY. n48_xv_decide (above) names only
 * the FIRST N48_XV_PGM_KEY_UNKNOWN key it meets in `f->pgm[]`, in ladder order, as every verdict does - correct for
 * a single fixed blocking reason, but a 10b run also wants EVERY unknown key the gather already put in `f->pgm[]`
 * for that frame, not only the one the verdict happens to name. The gather already holds them all (up to
 * N48_XV_MAX_PGMS per frame); this reads that array a second time and writes nothing of `f`'s own - no new
 * decision, no new gather. Returns the count written to `keys_out` (<= cap); `*total_out` is the frame's TRUE
 * unknown-key count, which can exceed `cap` (never silent - rule 72). Pure, host-tested
 * (tests/gfx_xlat_verdict_test.cpp). */
#define N48_XV_UNKNOWN_CAP 8u
static inline uint32_t n48_xv_unknown_keys(const n48_xv_frame *f, uint64_t *keys_out, uint32_t cap, uint32_t *total_out)
{
    uint32_t shown = 0u, total = 0u;
    if (f) {
        const uint32_t np = f->npgm < N48_XV_MAX_PGMS ? f->npgm : N48_XV_MAX_PGMS;
        for (uint32_t k = 0; k < np; k++) {
            if (f->pgm[k].key_class != N48_XV_PGM_KEY_UNKNOWN) continue;
            total++;
            if (shown < cap) {
                if (keys_out) keys_out[shown] = f->pgm[k].key;
                shown++;
            }
        }
    }
    if (total_out) *total_out = total;
    return shown;
}

/* args: judged frame, total unknown keys, shown (<= N48_XV_UNKNOWN_CAP), then N48_XV_UNKNOWN_CAP keys, zero-padded
 * past `shown`. Measured at widest numerics by tests/gfx_xlat_verdict_test.cpp against f3_reader.h's
 * N48_LOG_CAP_BODY bound (this header does not include f3_reader.h - the constant is duplicated as a literal 491
 * in that test, exactly as f3_reader_test.cpp's own bound is the source of truth for the number itself). */
#define N48_XV_UNKNOWN_FMT \
    "pgm-unknown-all: frame %llu total %u shown %u keys %#018llx %#018llx %#018llx %#018llx %#018llx %#018llx " \
    "%#018llx %#018llx"

#endif
