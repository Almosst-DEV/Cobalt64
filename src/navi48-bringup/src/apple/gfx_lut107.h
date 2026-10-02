/* gfx_lut107.h — build 0.0.550 (its PLAN step 1): SWITCH 107 "lutretry", THE LUT LEARN RETRIES AND THE
 * PLANE FAILS CLOSED WITHOUT A LUT. `107 | M << 8`: M 1 ON (= 363), M 2 OFF (= 619, the default and the boot value), M 3 SHADOW
 * (= 875); bare `107` reads. Mid-arm guarded (the continuous guard, and any standing arm). Inert unless switch 32 is ON.
 *
 * WHY (CONFIRMED there from the run logs). GPUPass exports (LUT0.r[s.r], LUT1.r[s.g], LUT2.r[s.b], s.a) (gfx_lutfill.h,
 *), so a plane drawn with a zero LUT is black whatever its layer holds. Switch 32's learn is ONE-SHOT per arm scope
 * (AppleHardwareHook.cpp gfxsrc_lut_learn: `gLutAttempted = 1u; // H2: ONE read per arm scope`), and in RUN AX/AY it was spent on
 * a record that is not the LUT (`not-32_FLOAT`), after frame 1's P was refused by the copy guard; the LUT-NOT-READY rung then
 * never fires (its `lut_plane` needs gLutHave), so every plane frame committed black.
 *
 * WHAT ON CHANGES, AND ONLY THIS:
 *   (1) RETRY. The learn is no longer one-shot: it is asked again on every GPUPASS PLANE FRAME (the in-force fragment program is
 *       `ws_D_GPUPass` AND the frame's CB0 is a P plane slot: present73's slot VAs or the fill set's members / learned CB0s) that
 *       passes 0.0.549's own clauses (COMMIT arm, WindowServer BOUND, the two-surface plane shape, no LUT yet, an image heap),
 *       until a record is learned (gLutHave 1: decoded, this instrument's one shape, every page resolved VRAM and
 *       destination-checked), at most N48_LR_TRIES_MAX times per arm scope. It NEVER learns from a frame that is not a GPUPass
 *       plane frame. The learn itself is 0.0.549's gfxsrc_lut_learn, called unchanged; the retry only clears its one-shot flag
 *       first. The WRITE path (lutfill_thread: the ramp, navi48_vram_write_mm, the read-back of every batch, the HDP flush) is not
 *       touched, and at most ONE thread per arm scope is still started (gLutKicked's CAS).
 *   (2) FAIL-CLOSED. A GPUPass plane frame judged while gLutHave is 0 reaches the gate with lut_plane 1 and lut_ready 0, so the
 *       gate's existing LUT-NOT-READY rung refuses it instead of committing a black plane. REFUSAL DIRECTION ONLY: n48_lr_gate can
 *       only raise lut_plane and lower lut_ready, and only for that frame.
 * SHADOW: 0.0.549's one-shot learn exactly as OFF, plus a read-only PEEK on each GPUPass plane frame (the slot-4 record read and
 * decoded, never resolved, never written) logging what ON would learn; the gate is untouched.
 * OFF: this header is never asked; the learn site and the gate are 0.0.549's.
 *
 * Pure: no lock, no clock, no log, no register. Host-tested by tests/gfx_fslearn_test.cpp (lut107 section). */
#ifndef N48_GFX_LUT107_H
#define N48_GFX_LUT107_H

#include <stdint.h>

#define N48_LR_SWITCH 107u
enum { N48_LR_OFF = 0u, N48_LR_ON = 1u, N48_LR_SHADOW = 2u, N48_LR_MODES = 3u };
#define N48_LR_TRIES_MAX   256u   /* ON: learn attempts per arm scope (each is 0.0.549's one read + decode + page resolve) */
#define N48_LR_PEEKS_MAX   16u    /* SHADOW: record reads per arm scope */
#define N48_LR_REF_LINES   16u    /* refused-record lines per arm scope (the count is not capped) */
#define N48_LR_BLACK_LINES 8u     /* "committed with no LUT" lines per arm scope (the count is not capped) */
#define N48_LR_SET_MAX     16u    /* the plane-slot set: 8 present73 slots + 2 fill members + 5 learned CB0s = 15 */
#define N48_LR_GP_LINES    4u     /* 0.0.551: GP-NO-LUT / GP-OTHER-LUT lines per arm scope, each (the counts are not capped) */

/* The verb's M -> the mode. 0 is a read; anything else not listed answers N48_LR_MODES (refused, unchanged). */
static inline uint32_t n48_lr_mode_of_m(uint32_t m)
{
    return m == 1u ? (uint32_t)N48_LR_ON : m == 2u ? (uint32_t)N48_LR_OFF : m == 3u ? (uint32_t)N48_LR_SHADOW : (uint32_t)N48_LR_MODES;
}
static inline const char *n48_lr_mode_name(uint32_t mode)
{
    return mode == N48_LR_ON ? "ON" : mode == N48_LR_SHADOW ? "SHADOW" : "OFF (default)";
}

/* Is `cb0` one of the plane-slot VAs? 0 and the all-ones "unlearned" marker never match; a null set matches nothing. */
static inline uint32_t n48_lr_in_set(uint64_t cb0, const uint64_t *set, uint32_t n)
{
    if (!cb0 || cb0 == ~0ull || !set) return 0u;
    for (uint32_t i = 0u; i < n && i < N48_LR_SET_MAX; i++)
        if (set[i] && set[i] != ~0ull && set[i] == cb0) return 1u;
    return 0u;
}
/* THE GPUPASS PLANE FRAME: the in-force fragment program of THIS frame was `ws_D_GPUPass` (gp) AND its CB0 is a plane slot. */
static inline uint32_t n48_lr_plane_frame(uint32_t gp, uint32_t in_set) { return (gp && in_set) ? 1u : 0u; }

/* The learn site's action, asked only for a frame that already passed 0.0.549's clauses (COMMIT arm, BOUND, the two-surface
 * plane shape, !gLutHave, an image heap). */
enum { N48_LR_ACT_NONE = 0u,         /* nothing: OFF/SHADOW with the one shot spent */
       N48_LR_ACT_LEGACY = 1u,       /* OFF/SHADOW: 0.0.549's one-shot learn, unchanged */
       N48_LR_ACT_RETRY = 2u,        /* ON: clear the one-shot flag and learn (a GPUPass plane frame, under the cap) */
       N48_LR_ACT_SKIP_NONPLANE = 3u,/* ON: not a GPUPass plane frame - never learns from it */
       N48_LR_ACT_CAPPED = 4u,       /* ON: N48_LR_TRIES_MAX tries this arm scope */
       N48_LR_ACT_SKIP_SEG = 5u };   /* ON (0.0.551): a plane frame, but THIS segment's fragment program is not ws_D_GPUPass */
/* build 0.0.551 (the 0.0.550 review's MUST-FIX 2 and SHOULD 4). `set_n` is the plane-slot set's size NOW; `seg_gp` says the
 * segment being learned from ran `ws_D_GPUPass` (gLrF.psId, THIS frame). ON with an EMPTY set is exactly OFF (0.0.549's one-shot
 * LEGACY learn): with no slot there is no plane fact to retry on, and skipping would leave ON never learning (worse than OFF). Once a
 * slot exists ON retries as 0.0.550 did, and only from a GPUPass segment of a GPUPass plane frame. */
static inline uint32_t n48_lr_learn_act(uint32_t mode, uint32_t attempted, uint32_t plane, uint32_t tries, uint32_t set_n,
                                        uint32_t seg_gp)
{
    if (mode == N48_LR_ON && set_n) {
        if (!plane) return N48_LR_ACT_SKIP_NONPLANE;
        if (!seg_gp) return N48_LR_ACT_SKIP_SEG;
        if (tries >= N48_LR_TRIES_MAX) return N48_LR_ACT_CAPPED;
        return N48_LR_ACT_RETRY;
    }
    return attempted ? N48_LR_ACT_NONE : N48_LR_ACT_LEGACY;   /* OFF, SHADOW, and ON with an empty set: 0.0.549's one-shot */
}
/* SHADOW's read-only peek: a GPUPass plane frame, until one peek decoded a record ON would learn, under the cap. */
static inline uint32_t n48_lr_peek(uint32_t mode, uint32_t plane, uint32_t peeks, uint32_t would_learned)
{
    return (mode == N48_LR_SHADOW && plane && !would_learned && peeks < N48_LR_PEEKS_MAX) ? 1u : 0u;
}
/* THE GATE'S INPUTS, FAIL-CLOSED (ON only). Returns 1 when it changed them: a GPUPass plane frame with switch 32 ON and no LUT
 * learned is made a LUT-NOT-READY candidate (lut_plane 1, lut_ready 0). It can only RAISE lut_plane and LOWER lut_ready. */
static inline uint32_t n48_lr_gate(uint32_t mode, uint32_t lut_switch, uint32_t plane, uint32_t have, uint32_t *lut_plane,
                                   uint32_t *lut_ready)
{
    if (mode != N48_LR_ON || !lut_switch || !plane || have || !lut_plane || !lut_ready) return 0u;
    *lut_plane = 1u;
    *lut_ready = 0u;
    return 1u;
}
/* build 0.0.551 (the 0.0.550 review's SHOULD 3): A READY FLAG FROM AN EARLIER ARM SCOPE NEVER ADMITS IN A NEW ONE (ON only).
 * lutfill_thread tags gLutReady with the scope it verified (gLutReadyScope) just before it sets it; ON lowers lut_ready when that tag
 * is not the current gLutScope. Returns 1 when it lowered it. Refusal direction only; OFF and SHADOW never change it. */
static inline uint32_t n48_lr_ready_scope(uint32_t mode, uint32_t ready_scope, uint32_t scope, uint32_t *lut_ready)
{
    if (mode != N48_LR_ON || !lut_ready || !*lut_ready || ready_scope == scope) return 0u;
    *lut_ready = 0u;
    return 1u;
}
/* build 0.0.551 (the 0.0.550 review's MUST-FIX 1): THE SLOT-BLIND INSTRUMENT (every mode, log-only). A COMMITTED frame with a
 * ws_D_GPUPass draw (`gp`, gLrF.gpFrame == this frame), WHATEVER the plane-slot set says: 1 = no LUT learned (gLutHave 0),
 * 2 = a LUT learned but none of the frame's inputs sampled it (gXdBuild.lutPlane 0: a different LUT), 0 = neither. */
enum { N48_LR_GP_NONE = 0u, N48_LR_GP_NOLUT = 1u, N48_LR_GP_OTHERLUT = 2u };
static inline uint32_t n48_lr_gp_class(uint32_t committed, uint32_t gp, uint32_t have, uint32_t lut_plane)
{
    if (!committed || !gp) return N48_LR_GP_NONE;
    if (!have) return N48_LR_GP_NOLUT;
    return lut_plane ? (uint32_t)N48_LR_GP_NONE : (uint32_t)N48_LR_GP_OTHERLUT;
}
/* The instrument (every mode): a GPUPass plane frame the gate COMMITTED while no LUT is learned. */
static inline uint32_t n48_lr_black_risk(uint32_t committed, uint32_t plane, uint32_t have)
{
    return (committed && plane && !have) ? 1u : 0u;
}

/* ---- the lines (each <= 491 bytes at maximal fields: tests/gfx_fslearn_test.cpp lut107 section) ---- */
/* build 0.0.553 (switch 111, gfx_lutidx111.h): the REFUSED / LEARNED / SHADOW lines name the image-table entry read (`idx`):
 * 4 unless switch 111 ON selected the draw's own. */
#define N48_LR_FMT \
    "lutretry107: `gfxneuter 107 | M << 8` is %s (363 ON, 619 OFF default, 875 SHADOW; mid-arm guarded; switch 32 is %s)%s."
#define N48_LR_FMT2 \
    "lutretry107: learn tries %llu, learned %llu, refused %llu, non-plane skipped %llu, capped %llu; SHADOW peeks %llu (would " \
    "learn %llu, would refuse %llu); fail-closed flagged %llu, refused %llu; plane commits with no LUT %llu"
#define N48_LR_REF_FMT \
    "lutretry107: f%llu REFUSED learn record (try %u, %s, idx %u): program %s CB0 %#llx; T# fmt %u type %u %u x %u x %u slice(s), " \
    "base %#llx; why %s; refused %llu; plane-slot set %u"
#define N48_LR_LEARN_FMT \
    "lutretry107: f%llu LEARNED the LUT record on try %u (idx %u, %s): CB0 %#llx; LUT VA %#llx %u slice(s) x %u; the deferred write %s"
#define N48_LR_PEEK_FMT \
    "lutretry107: SHADOW f%llu: ON would %s: idx %u program %s CB0 %#llx; T# fmt %u type %u %u x %u x %u slice(s), base %#llx; %s; " \
    "peek %u of %u"
/* build 0.0.551: the slot-blind counts (every mode; at the verb and at every continuous STOP) and their capped lines. */
#define N48_LR_FMT3 \
    "lutretry107: GPUPass commits with no LUT %llu (whatever the plane-slot set), GPUPass commits with a LUT no input sampled " \
    "(lutPlane 0) %llu; ON: non-GPUPass segments skipped %llu, empty-set LEGACY learns %llu, stale-scope ready lowered %llu; " \
    "plane-slot set %u now%s"
#define N48_LR_GPNO_FMT \
    "lutretry107: GP-NO-LUT f%llu: a frame with a ws_D_GPUPass draw COMMITTED with no LUT learned (gLutHave 0, ready %u): CB0 %#llx " \
    "%s the plane-slot set (size %u); 32 %s, 107 %s; %llu such commit(s)"
#define N48_LR_GPOTHER_FMT \
    "lutretry107: GP-OTHER-LUT f%llu: a frame with a ws_D_GPUPass draw COMMITTED with a LUT learned (VA %#llx, ready %u) that none " \
    "of its inputs sampled (lutPlane 0): CB0 %#llx; 32 %s, 107 %s; %llu such commit(s)"
#define N48_LR_BLACK_FMT \
    "lutretry107: BLACK-RISK f%llu: a GPUPass plane frame COMMITTED with no LUT learned (gLutHave 0, ready %u): CB0 %#llx; " \
    "32 %s, 107 %s; %llu such commit(s)"

#endif /* N48_GFX_LUT107_H */
