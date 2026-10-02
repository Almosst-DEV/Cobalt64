// gfx_tgtsample.h — DID OUR COMMITTED FRAME WRITE ITS OWN COLOUR TARGET? A BEFORE/AFTER SAMPLE, AND NOTHING ELSE.
// Pure C, host-tested by tests/gfx_tgtsample_test.cpp (with planted defects); the kext compiles the SAME header.
// 0.0.386. DEFAULT-INERT: nothing here runs unless `accel gfxneuter 24 | 1 << 8` was thrown this boot AND the caller
// is on the armed COMMIT path. ZERO WRITE PRIMITIVES: this header contains no writer of any kind, and the kext side
// reads through gfxc_read — the same read-only walker every IB of every frame already goes through.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THIS EXISTS
// ---------------------------------------------------------------------------------------------------------------------
// Step 7 on the map — "prove the frame drew the RIGHT PIXELS" — has never been measured. arm10 read the committed
// frame's colour target back with `accel vmpage` and found an image; arm11 then proved that image is APPLE'S OWN
// STATIC CONTENT, byte-identical on a boot that committed nothing. So the readback we have proves nothing
// about our pixels.
//
// A BEFORE-READ FROM THE VERB PATH IS STRUCTURALLY IMPOSSIBLE. `accel vmpage` resolves a VA through VMID 2, and
// VMID 2 has no page-table root until WindowServer binds — which happens after `wskill`, i.e. after the only moment
// at which a "before" would mean anything. The ONE place a before/after pair can be taken is INSIDE THE KEXT, at the
// COMMIT gate, where the committing frame's own `vm` is in hand and the target VA has already been resolved for the
// `FRAME TARGET` line.
//
// WHAT IT CAN AND CANNOT SETTLE, stated up front:
//   CHANGED   ⇒ our frame wrote the target. That is evidence FOR step 7 existing at all. It says NOTHING about
//               whether the pixels are RIGHT; that is the next question and this instrument does not answer it.
//   UNCHANGED ⇒ EITHER our frame did not write the target, OR it wrote exactly Apple's own bytes back. Both
//               readings are printed on the verdict line, because collapsing them is how became.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE GEOMETRY, AND WHY IT IS A SAMPLE AND NOT A DUMP
// ---------------------------------------------------------------------------------------------------------------------
// arm10's own statistics over the target put descriptors at the base and the image 2-4 MiB in. So: three blocks, at
// +0, +2 MiB and +4 MiB from the target VA; four rows of 256 dwords each per block (one 4 KiB page per block,
// contiguous). 3 x 4 x 256 = 3072 dwords = 12 KiB per sample. A digest per row — dwords read, non-zero count,
// distinct count (capped, and the cap is declared on the line), first four dwords — and, at the AFTER, the count of
// dwords that changed.
//
// WHY A CAPPED DISTINCT COUNT. An exact distinct count over 256 dwords is 32640 comparisons per row on Apple's submit
// thread. The cap makes the worst case 256 x 32 = 8192 per row, 98304 for the whole sample, and a saturated count is
// reported as a LOWER BOUND ("32+") rather than as a number. An instrument that quietly reports a bounded quantity as
// an exact one is the shape of `presents ok/refused`.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHEN THE AFTER IS TAKEN — AND IT REUSES THE WITHDRAWAL DEFERRAL'S OWN SIGNAL, NOT A NEW ONE
// ---------------------------------------------------------------------------------------------------------------------
// gfx_keystone.h's n48_ksd_eval already defines "in flight" for the keystone withdrawal deferral, and defines its END
// as the EARLIEST of (a) an observed end of pipe — gfx_fence828.h's sticky `n48_f828_watch.ever`, qualified by
// `committed` — and (b) a bounded timeout against the flight record's stamp. n48_ts_after_eval below asks the SAME
// two questions of the SAME two pieces of state, in the SAME clause order, and takes the AFTER sample at the first
// instant either answers yes. It is deliberately NOT a second definition of end-of-flight: if the deferral's
// definition is wrong, this instrument is wrong in exactly the same way and the two cannot disagree in a report.
//
// IT FAILS CLOSED THE OTHER WAY ROUND FROM THE DEFERRAL, AND THAT IS ON PURPOSE. For the deferral, uncertainty means
// "withdraw as today" — act. For a measurement, uncertainty means "do not take the sample", because a sample taken
// while the frame is still running would be read as an AFTER and is not one. So every clause that is not an observed
// end-of-pipe or an expired bound answers with a reason and takes nothing, and the run reports
// `AFTER: not taken (no end-of-flight)`.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT IS NOT HERE
// ---------------------------------------------------------------------------------------------------------------------
// No write primitive. No page walker (the kext hands this header dwords that gfxc_read already returned). No verdict,
// route, rung, budget or packet reads anything in this file: it is an INSTRUMENT, and the only consumer of every
// value it produces is a log line.
#ifndef N48_GFX_TGTSAMPLE_H
#define N48_GFX_TGTSAMPLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------------------------
 * THE SAMPLE GEOMETRY. Fixed at compile time: a sample geometry the run recipe could change by accident is a
 * measurement the run recipe could change by accident.
 * ---------------------------------------------------------------------------------------------------------------- */
#define N48_TS_BLOCKS      3u
#define N48_TS_ROWS        4u
#define N48_TS_ROW_DW      256u
#define N48_TS_BLOCK_DW    (N48_TS_ROWS * N48_TS_ROW_DW)        /* 1024 dw = 4 KiB = exactly one page */
#define N48_TS_TOTAL_DW    (N48_TS_BLOCKS * N48_TS_BLOCK_DW)    /* 3072 dw = 12 KiB per sample */
#define N48_TS_DISTINCT_CAP 32u

/* The byte offset from the target VA at which block `b` starts. N48_TS_OFF_BAD for any other index: a `default:`
 * that returned 0 would alias block 2 onto block 0 and no reader of the log could tell. */
#define N48_TS_OFF_BAD 0xFFFFFFFFFFFFFFFFull
static inline uint64_t n48_ts_block_off(uint32_t b)
{
    switch (b) {
    case 0u: return 0ull;
    case 1u: return 0x200000ull;   /* 2 MiB in */
    case 2u: return 0x400000ull;   /* 4 MiB in */
    default: return N48_TS_OFF_BAD;
    }
}

/* The whole window this sample touches, from the target VA: the last block's offset plus its page. */
#define N48_TS_SPAN (0x400000ull + 0x1000ull)

/* A target VA the sample may be taken at. gfx_desc_port.h's rule for a sane VA is "below 2^48 and does not wrap";
 * this asks it of the WHOLE window, not of the base, because a base that is fine and a +4 MiB that wraps is exactly
 * the case a base-only check waves through. */
static inline uint32_t n48_ts_va_ok(uint64_t va)
{
    if (!va) return 0u;
    if (va >= (1ull << 48)) return 0u;
    if (va + N48_TS_SPAN < va) return 0u;              /* wrapped */
    if (va + N48_TS_SPAN > (1ull << 48)) return 0u;    /* left the addressable range */
    return 1u;
}

/* ------------------------------------------------------------------------------------------------------------------
 * ONE ROW'S DIGEST.
 * ---------------------------------------------------------------------------------------------------------------- */
typedef struct {
    uint32_t got;        /* dwords actually read for this row (a short read is visible, never filled) */
    uint32_t nonzero;    /* of `got` */
    uint32_t distinct;   /* of `got`, saturating at N48_TS_DISTINCT_CAP */
    uint32_t capped;     /* 1 when `distinct` saturated and is therefore a LOWER BOUND, not a count */
    uint32_t first[4];   /* the first four dwords; 0 where `got` did not reach them */
} n48_ts_row;

static inline void n48_ts_digest(const uint32_t *p, uint32_t got, n48_ts_row *out)
{
    uint32_t seen[N48_TS_DISTINCT_CAP];
    uint32_t nseen = 0u, i, j;
    if (!out) return;
    out->got = 0u; out->nonzero = 0u; out->distinct = 0u; out->capped = 0u;
    out->first[0] = out->first[1] = out->first[2] = out->first[3] = 0u;
    if (!p || !got) return;
    if (got > N48_TS_ROW_DW) got = N48_TS_ROW_DW;   /* the caller's row length is the contract; clamp, never read past */
    out->got = got;
    for (i = 0u; i < 4u && i < got; i++) out->first[i] = p[i];
    for (i = 0u; i < got; i++) {
        if (p[i]) out->nonzero++;
        if (nseen < N48_TS_DISTINCT_CAP) {
            for (j = 0u; j < nseen; j++) if (seen[j] == p[i]) break;
            if (j == nseen) seen[nseen++] = p[i];
        }
    }
    out->distinct = nseen;
    out->capped = (nseen >= N48_TS_DISTINCT_CAP) ? 1u : 0u;
}

/* Dwords that differ over the first `n` of each buffer. The caller passes min(gotBefore, gotAfter) — see
 * n48_ts_cmp_n — so a dword that only ONE of the two reads returned is never counted as "changed": that is a
 * difference in what we could read, not a difference in what is there. */
static inline uint32_t n48_ts_cmp_n(uint32_t gotA, uint32_t gotB) { return gotA < gotB ? gotA : gotB; }

static inline uint32_t n48_ts_changed(const uint32_t *a, const uint32_t *b, uint32_t n)
{
    uint32_t i, c = 0u;
    if (!a || !b) return 0u;
    for (i = 0u; i < n; i++) if (a[i] != b[i]) c++;
    return c;
}

/* ------------------------------------------------------------------------------------------------------------------
 * THE VERDICT. Three states, and NEITHER of the two that compared anything may name a writer.
 *
 * 0.0.387 (the arm13 review's reviewer item): the CHANGED name USED TO READ "OUR COMMITTED FRAME WROTE
 * ITS COLOUR TARGET" AND THAT IS AN ATTRIBUTION THIS INSTRUMENT CANNOT MAKE. The BEFORE is taken immediately before
 * our rewrite, but the AFTER is taken in a LATER gfxsrc_decide_frame - a different submission entirely - so every
 * dword it reads follows whatever ran between the rewrite and the read, ours or Apple's. The honest reading is that
 * the target changed INSIDE THE WINDOW, and the string now says exactly that and nothing more. arm13 read
 * `TARGET CHANGED 256 of 3072` and the old string turned that into a claim about step 7 that the run's own author
 * had to retract in prose; a liar that has already lied once does not get another run.
 * ---------------------------------------------------------------------------------------------------------------- */
enum {
    N48_TS_V_NO_SAMPLE = 0,   /* nothing was compared: no AFTER, or nothing readable in both */
    N48_TS_V_CHANGED,
    N48_TS_V_UNCHANGED,
    N48_TS_V_COUNT
};

static inline uint32_t n48_ts_verdict(uint32_t sampled, uint32_t changed)
{
    if (!sampled) return N48_TS_V_NO_SAMPLE;
    if (changed > sampled) return N48_TS_V_NO_SAMPLE;   /* not possible from n48_ts_changed; refuse rather than print it */
    return changed ? N48_TS_V_CHANGED : N48_TS_V_UNCHANGED;
}

static inline const char *n48_ts_verdict_name(uint32_t v)
{
    switch (v) {
    case N48_TS_V_NO_SAMPLE: return "NO SAMPLE - nothing was compared, so this run says NOTHING about step 7";
    case N48_TS_V_CHANGED:   return "TARGET CHANGED INSIDE THE WINDOW - the target held different bytes at the read "
                                    "taken just before our rewrite and at the read taken at the first end of flight. "
                                    "THE WRITER IS NOT ESTABLISHED BY THIS INSTRUMENT: the AFTER is taken in a LATER "
                                    "decide-frame pass, so it follows whatever ran between the rewrite and the read - "
                                    "ours, or Apple's own frames. It is NOT evidence that our frame wrote the target, "
                                    "and it is NOT evidence that the pixels are RIGHT";
    case N48_TS_V_UNCHANGED: return "TARGET UNCHANGED - EITHER our frame did not write the target, OR it wrote "
                                    "exactly Apple's own bytes back. This instrument cannot tell those two apart";
    default:                 return "?";
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * WHEN THE BEFORE SAMPLE MAY BE TAKEN.
 * ---------------------------------------------------------------------------------------------------------------- */
enum {
    N48_TS_BF_OFF = 0,        /* the switch is off: the default, and the whole instrument is unreachable */
    N48_TS_BF_NOT_LIVE,       /* not the armed COMMIT + TRANSLATE path: there is no rewrite to bracket */
    N48_TS_BF_DONE,           /* a BEFORE is already published for a frame the gate committed; never overwrite it */
    N48_TS_BF_CAP,            /* the per-boot read budget is spent (refused frames can retry; this bounds them) */
    N48_TS_BF_NO_TARGET,      /* the frame named no usable colour target VA */
    N48_TS_BF_TAKE,
    N48_TS_BF_REASONS
};

typedef struct {
    uint32_t on;         /* the switch */
    uint32_t live;       /* gfxsrc_commit_try's own `live`: ARM_COMMIT && TRANSLATE && build ok && buffers && dep */
    uint32_t published;  /* a BEFORE is already published for a COMMITTED frame */
    uint32_t reads;      /* BEFORE reads spent this boot */
    uint32_t cap;        /* the budget. 0 refuses: an unbounded read budget on the submit path is not a bounded one */
    uint64_t tgt_va;     /* the frame's first colour-target VA, as the FRAME TARGET line reports it */
} n48_ts_before_in;

static inline uint32_t n48_ts_before_eval(const n48_ts_before_in *d)
{
    if (!d) return N48_TS_BF_OFF;
    if (!d->on) return N48_TS_BF_OFF;
    if (!d->live) return N48_TS_BF_NOT_LIVE;
    if (d->published) return N48_TS_BF_DONE;
    if (d->cap == 0u || d->reads >= d->cap) return N48_TS_BF_CAP;
    if (!n48_ts_va_ok(d->tgt_va)) return N48_TS_BF_NO_TARGET;
    return N48_TS_BF_TAKE;
}

static inline uint32_t n48_ts_before_take(uint32_t r) { return r == N48_TS_BF_TAKE ? 1u : 0u; }

static inline const char *n48_ts_before_name(uint32_t r)
{
    switch (r) {
    case N48_TS_BF_OFF:       return "NOT TAKEN: the switch is off (`gfxneuter 24 | 1 << 8`) - the default";
    case N48_TS_BF_NOT_LIVE:  return "NOT TAKEN: this frame is not on the armed COMMIT+TRANSLATE path";
    case N48_TS_BF_DONE:      return "NOT TAKEN: a BEFORE is already published for a committed frame";
    case N48_TS_BF_CAP:       return "NOT TAKEN: the per-boot BEFORE read budget is spent";
    case N48_TS_BF_NO_TARGET: return "NOT TAKEN: this frame names no usable colour-target VA";
    case N48_TS_BF_TAKE:      return "TAKEN: immediately before the IB rewrite, through the frame's own page table";
    default:                  return "?";
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * WHEN THE AFTER SAMPLE MAY BE TAKEN — the clause order MIRRORS gfx_keystone.h's n48_ksd_eval on purpose.
 * ---------------------------------------------------------------------------------------------------------------- */
enum {
    N48_TS_AF_OFF = 0,          /* the switch is off */
    N48_TS_AF_NO_BEFORE,        /* no BEFORE was published: there is nothing an AFTER could be compared against */
    N48_TS_AF_DONE,             /* the AFTER has already been taken; one pair per boot */
    N48_TS_AF_NO_CONTEXT,       /* this frame's page-table root is not the committed frame's - a VA means nothing here */
    N48_TS_AF_NO_FLIGHT,        /* nothing is recorded in flight */
    N48_TS_AF_TORN,             /* the flight record or the clock is not usable - take nothing */
    N48_TS_AF_IN_FLIGHT,        /* STILL IN FLIGHT: neither end-of-pipe nor the bound. THIS IS THE CLAUSE THAT MATTERS */
    N48_TS_AF_TAKE_EOP,         /* end of pipe OBSERVED - the flight is over, take the AFTER now */
    N48_TS_AF_TAKE_TIMEOUT,     /* the bound expired with no end-of-pipe - take it, and say which end it was */
    N48_TS_AF_REASONS
};

typedef struct {
    uint32_t on;
    uint32_t before_ok;   /* a BEFORE is published for a frame the COMMIT gate answered OK for */
    uint32_t done;        /* the AFTER has already been taken this boot */
    uint32_t same_root;   /* this frame's vm.root == the committed frame's vm.root */
    uint32_t flight;      /* gKsFlight.active - the flight record's own flag, written LAST by the producer */
    uint64_t start_us;    /* gKsFlight.at_us - written FIRST. 0 with flight=1 is TORN, exactly as n48_ksd_eval reads it */
    uint32_t now_ok;      /* the clock was read at all */
    uint64_t now_us;
    uint32_t eop;         /* ks_eop_seen(): gXdF828.committed && n48_f828_watch.ever && the committed record's own
                           * flight == gKsFlight.seq (0.0.418 E2). gfx_keystone.h clause (a) */
    uint64_t timeout_us;  /* kKsFlightUs, the deferral's own bound. gfx_keystone.h clause (b). 0 is refused */
} n48_ts_after_in;

static inline uint32_t n48_ts_after_eval(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;                          /* no input is not knowledge */
    if (!d->on) return N48_TS_AF_OFF;
    if (!d->before_ok) return N48_TS_AF_NO_BEFORE;
    if (d->done) return N48_TS_AF_DONE;
    if (!d->same_root) return N48_TS_AF_NO_CONTEXT;
    if (!d->flight) return N48_TS_AF_NO_FLIGHT;
    /* The producer writes `start_us` before `flight`, so `flight` without a stamp is a torn read. A clock we could
     * not read, or one that went backwards, is the same kind of ignorance. Verbatim from n48_ksd_eval. */
    if (!d->now_ok || d->start_us == 0ull || d->now_us < d->start_us) return N48_TS_AF_TORN;
    if (d->timeout_us == 0ull) return N48_TS_AF_TORN;
    if (d->eop) return N48_TS_AF_TAKE_EOP;                  /* the truer reason when both hold */
    if (d->now_us - d->start_us >= d->timeout_us) return N48_TS_AF_TAKE_TIMEOUT;
    return N48_TS_AF_IN_FLIGHT;                             /* *** THE SAMPLE IS NOT TAKEN HERE *** */
}

static inline uint32_t n48_ts_after_take(uint32_t r)
{
    return (r == N48_TS_AF_TAKE_EOP || r == N48_TS_AF_TAKE_TIMEOUT) ? 1u : 0u;
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0.0.390 ( part 5 (v),'s "the proving instrument") — THE SECOND SLOT, KEYED BY COMMIT TOKEN SEQ.
 *
 * named this as a defect of the instrument, not of the gate: `published` and `afterDone` are BOOT ONE-SHOTS latched
 * at the first N48_CM_OK, so on a two-frame chain (the fill, then the plane frame that samples it) THE SECOND COMMITTED
 * FRAME GETS NO PAIR AT ALL - and the second frame is the one the run is about. The rules below say which slot a sample
 * belongs in; the storage is the kext's.
 *
 * ONE SLOT IS 0.0.389 BYTE FOR BYTE. `nslots` is what the caller offers, and the kext offers 1 unless
 * `accel gfxneuter 28 | 1 << 8` is on. With one slot, n48_ts_slot_for answers slot 0 or nothing, exactly as the single
 * `published` flag did, and n48_ts_all_published answers that one flag.
 *
 * A SEQ IS AN IDENTITY, NOT AN INDEX. Slot k holds token seq `seq_of[k]`; a seq already held answers ITS OWN slot, so a
 * retry can never write a second copy of one frame's pair over another frame's. Seq 0 is not a key - the BEFORE is taken
 * before the gate has answered, so the kext claims a slot with the seq it will publish under and refuses a zero.
 * ---------------------------------------------------------------------------------------------------------------- */
#define N48_TS_SLOTS 2u
#define N48_TS_SLOT_NONE 0xFFFFFFFFu

/* `held[k]` 1 when slot k is claimed; `seq_of[k]` the COMMIT token seq it is claimed for. */
static inline uint32_t n48_ts_slot_for(const uint32_t *held, const uint32_t *seq_of, uint32_t nslots, uint32_t seq)
{
    if (!held || !seq_of || nslots == 0u || nslots > N48_TS_SLOTS || seq == 0u) return N48_TS_SLOT_NONE;
    for (uint32_t k = 0; k < nslots; k++) if (held[k] && seq_of[k] == seq) return k;
    for (uint32_t k = 0; k < nslots; k++) if (!held[k]) return k;
    return N48_TS_SLOT_NONE;
}

/* 0.0.410 — MAY THIS COMMIT BE SAMPLED AT ALL? Under the fill-set switch (`gfxneuter 33`) the FIRST
 * shot is fill A, whose surface already proved red/green; spending a sample slot on it leaves the plane -
 * the frame the run is about - with no pair, and by the time a later frame samples it, presents have overwritten it. So
 * with 33 ON the sample starts at the SECOND commit: the two slots go to fill B and the plane. `prior_commits` is the
 * number of commits the gate has already answered N48_CM_OK for this boot, read BEFORE this frame's own commit is
 * counted. OFF, this is always 0: the default slot order is 0.0.409's, byte for byte.
 * A ONE-SHOT IS NOT AN ARGUMENT FOR A SLOT: a commit refused at the gate is never counted by `prior_commits`, so the
 * sample simply starts at the first commit that reaches the ring. */
static inline uint32_t n48_ts_skip_commit(uint32_t fs_on, uint32_t prior_commits)
{
    return (fs_on && prior_commits == 0u) ? 1u : 0u;
}

/* 0.0.411 — MAY THIS BEFORE BE SKIPPED BECAUSE A LATER RUNG WILL REFUSE THE FRAME? The BEFORE read is
 * bounded at kTsBeforeCap for the boot, and an un-published BEFORE still spends a read. arm26 proved the cost: f6 and f8
 * (reservation REFUSE), f13 (LUT rung NOT-READY) and f16 (keystone-withdrawn after the gate) spent all four reads, and
 * the plane - the frame the run is about - got no slot. `fs_refuse` is the frame's own n48_fs_step verdict (gfx_fillset.h:
 * N48_FS_REFUSE == the gate's RESERVED-FOR-FILL rung will fire); `lut_refuse` is exactly n48_cm_gate's
 * `lut_switch && lut_plane && !lut_ready`. Both are computable BEFORE the rewrite (the caller computes them there), so a
 * frame they will refuse is not sampled at all: its slot is left for the frame that will commit. PURE; 0 when neither
 * rung is active, which is the whole default path.
 *
 * 0.0.420 — THE SAME PREDICATE CARRIES TWO MORE "LATER RUNG WILL REFUSE" SOURCES, one call each, so its body is unchanged
 * and its meaning only widens. (a) STEP10-PLAN P1's second window: a non-plane frame the appended `RESERVED-FOR-PLANE`
 * rung will refuse (gfx_fillset.h's n48_fs_plane_step == N48_FS_REFUSE). Its slot is the plane's whole point, and arm29's
 * "second pair" would otherwise spend it. (b)'s E2/E3 follow-up: a fence candidate whose ring region MOVED will be
 * NEUTERED at the commit-try (n48_f828_region_moved_reason != OK), so its BEFORE could never be compared. Both are
 * computable before the rewrite, like the two above. OFF, each call is (0,0) and this is exactly 0.0.419's predicate. */
static inline uint32_t n48_ts_skip_refused(uint32_t fs_refuse, uint32_t lut_refuse)
{
    return (fs_refuse || lut_refuse) ? 1u : 0u;
}

/* 1 when every slot the caller offers has PUBLISHED a BEFORE — the value n48_ts_before_in.published takes. */
static inline uint32_t n48_ts_all_published(const uint32_t *pub, uint32_t nslots)
{
    if (!pub || nslots == 0u || nslots > N48_TS_SLOTS) return 1u;   /* no slots is not "room": refuse */
    for (uint32_t k = 0; k < nslots; k++) if (!pub[k]) return 0u;
    return 1u;
}

static inline const char *n48_ts_after_name(uint32_t r)
{
    switch (r) {
    case N48_TS_AF_OFF:          return "AFTER: not taken - the switch is off (`gfxneuter 24 | 1 << 8`), the default";
    case N48_TS_AF_NO_BEFORE:    return "AFTER: not taken - no BEFORE was published for a committed frame";
    case N48_TS_AF_DONE:         return "AFTER: already taken - one before/after pair per boot";
    case N48_TS_AF_NO_CONTEXT:   return "AFTER: not taken - this frame's root is not the committed frame's";
    case N48_TS_AF_NO_FLIGHT:    return "AFTER: not taken (no end-of-flight) - nothing is recorded in flight";
    case N48_TS_AF_TORN:         return "AFTER: not taken - the flight record or the clock is unusable; fail closed";
    case N48_TS_AF_IN_FLIGHT:    return "AFTER: not taken (no end-of-flight) - the committed frame is STILL IN FLIGHT: "
                                        "no end-of-pipe observed and the bound has not expired";
    /* NOTE the wording: this line deliberately does NOT contain the literal token the end-of-pipe instrument's own
     * lines carry, so the build proof `strings -a <kext> | grep -c` for that token still counts ONLY that
     * instrument - the same rule ks_defer_line states for itself in AppleHardwareHook.cpp. */
    case N48_TS_AF_TAKE_EOP:     return "AFTER: taken at END OF PIPE OBSERVED (the sticky end-of-pipe latch)";
    case N48_TS_AF_TAKE_TIMEOUT: return "AFTER: taken at the BOUND, with NO end-of-pipe observed - the frame may still "
                                        "have been running; read the verdict with that in mind";
    default:                     return "?";
    }
}

/* =====================================================================================================================
 * 0.0.391 — THE READ-OUT'S FORMATS, HERE SO THE TEST CAN BOUND THEM.
 * =====================================================================================================================
 * `n48log` formats into `char line[512]` and vsnprintf TRUNCATES SILENTLY past it; HWLOG prepends "AppleHardwareHook: "
 * and appends "\n" (20 bytes), so the body cap is N48_LOG_CAP_BODY.
 *
 * SPLIT the AFTER verdict and its report twin and then recorded "worst case now 496 of 512". **That was false**,
 * and measured it: `ts_report_line`'s FIRST line still carried BOTH n48_ts_before_name (76 B) and
 * n48_ts_after_name (131 B) on top of its fixed text - 535 bytes at its shortest plausible arguments and 632 at worst -
 * and every instance of it in notes/logs/runs/arm18/driverlog-stream.txt is exactly 511 bytes, cut mid-word. So the
 * arithmetic is no longer written in a commit message: the formats live here and gfx_tgtsample_test.cpp measures every
 * one of them at its longest possible arguments, which is what catches the next one.
 * A = the switch and the geometry; B = the counts; C and D = the two long reason names, one line each. */
#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u    /* the same number f3_reader.h and gfx_commit.h define, for the same reason */
#endif
/* args: on/off, how, blocks, rows, row dwords, total dwords, distinct cap */
#define N48_TS_RPT_A_FMT "tgtsample: the COMMIT-gate target before/after sample (`gfxneuter 24 | M << 8`) is %s (%s). " \
                         "Geometry: %u block(s) at +0/+2 MiB/+4 MiB, %u rows of %u dwords each = %u dwords per sample; " \
                         "distinct counts saturate at %u and a saturated one is printed with a trailing '+'."
/* args: reads, cap, taken, published, afterDone */
#define N48_TS_RPT_B_FMT "tgtsample: BEFORE reads %u of %u, taken %u, PUBLISHED %u. AFTER taken %u. The two `last " \
                         "reason` strings are on the next two lines, one each: together they are longer than the " \
                         "logger's line."
#define N48_TS_RPT_C_FMT "tgtsample: BEFORE last reason: %s"     /* args: n48_ts_before_name */
#define N48_TS_RPT_D_FMT "tgtsample: AFTER last reason: %s"      /* args: n48_ts_after_name */
/* args: slots offered, N48_TS_SLOTS */
#define N48_TS_RPT_SLOTS_FMT "tgtsample: slots offered %u of %u (`gfxneuter 28 | 1 << 8` offers the second; the first " \
                             "is 0.0.389's)."
/* args: slot, CHANGED/UNCHANGED/NO SAMPLE, changed, sampled, token seq */
#define N48_TS_RPT_V_FMT "tgtsample: slot %u VERDICT: TARGET %s %u of %u sampled dwords (token seq %u)."
/* args: slot */
#define N48_TS_RPT_NONE_FMT "tgtsample: slot %u VERDICT: none - AFTER: not taken (no end-of-flight). This slot says " \
                            "NOTHING about whether our frame wrote its target."
/* args: slot, n48_ts_verdict_name or n48_ts_after_name */
#define N48_TS_RPT_NAME_FMT "tgtsample: slot %u %s"

#ifdef __cplusplus
}
#endif

#endif
