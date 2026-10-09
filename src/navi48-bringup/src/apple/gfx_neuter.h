// gfx_neuter.h — the one dword the GFX neuter writes (0.0.278). Pure C, host-tested by tests/gfx_neuter_test.cpp;
// the kext compiles the SAME header.
//
// A PM4 type-3 INDIRECT_BUFFER header (0xC0023F00: type 3, count 2, opcode 0x3F) becomes a type-3 NOP of the SAME length
// (0xC0021000: count 2, opcode 0x10). Only the opcode byte changes, so the packet still spans 4 dwords, the walk after it lands on
// the same next header, and a COND_EXEC that counts dwords over it is unchanged. Anything that is not exactly a count-2 type-3
// INDIRECT_BUFFER is returned as 0 (refuse; 0 is never a valid NOP header here).
#ifndef N48_GFX_NEUTER_H
#define N48_GFX_NEUTER_H

#include <stdint.h>

static inline uint32_t n48_gfxn_is_ib(uint32_t h)
{
    return (h >> 30) == 3u && ((h >> 8) & 0xFFu) == 0x3Fu && ((h >> 16) & 0x3FFFu) == 2u;
}

static inline uint32_t n48_gfxn_nop_for(uint32_t h)
{
    if (!n48_gfxn_is_ib(h)) return 0u;
    return (h & ~0x0000FF00u) | (0x10u << 8);
}

/* ---- 0.0.279: the neuter AT THE SOURCE ------------------------------------------------------------------
 * wsneuter1 showed a frame can reach the CP before the writeTail hook runs, so a rewrite of the ring can lose.
 * AMDGFX10PM4GraphicsChannel::commitIndirectCommandBuffer @0xbe20a00 (vtable slot 95, 25G83) builds each GFX frame directly
 * in the ring block (getRingBlock be20a65, memcpy of the 128-dword template be20a87), writes one INDIRECT_BUFFER packet per
 * IB entry, commits the block and submits. Its IB-list loop skips an entry whose first dword is 0 (be20bab-be20baf
 * `movl (%rax),%edi; testl %edi,%edi; je`), leaving the template's 0xFFFF1000 padding (the template is NOP-filled at
 * be21161) - so zeroing the entries' first dwords BEFORE calling the original means the IB packet never exists in the ring.
 * AMD_SUBMIT_COMMAND_BUFFER_INFO as that function reads it:
 *   +0x00 u32 flags: 0x2 -> timestamp-only frame, 0x10 -> single-IB frame (no length check), neither -> the IB-list frame
 *   +0x04 u32 low 4 bits = VMID (be20bde),  +0x14 u32 IB count (be20b91),  +0x18 u32 stamp (be20a56)
 *   IB entry i at +0x4c + 0x28*i: +0x00 u32 length|flags (be20bab), +0x0c u64 VA (be20bba)
 * The packet for entry i lands at template dword 0x2d + 4*i (be20ba2: +0xc0, header at -0xc); room for 4 before the
 * frame's RELEASE_MEM at 0x3d. */
#define N48_SCB_FLAGS          0x00u
#define N48_SCB_VMID           0x04u
#define N48_SCB_COUNT          0x14u
#define N48_SCB_STAMP          0x18u
#define N48_SCB_ENTRY0         0x4cu
#define N48_SCB_ENTRY_SIZE     0x28u
#define N48_SCB_ENTRY_VA       0x0cu
#define N48_SCB_MAX_IBS        4u
#define N48_TMPL_IB0_DWORD     0x2du
#define N48_TMPL_DWORDS        128u
#define N48_TMPL_NOP           0xFFFF1000u

/* 0.0.362: reason 2 was N48_SRC_NOT_VMID2, "not VMID 2" - the assumption hp3 refuted (: VMID 2 was the
 * forced-draw client's). It is now N48_SRC_NOT_WS: of the proven shape, but NOT WindowServer's by the OWNER of its VM context
 * (ws_ident.h). The shape check below no longer looks at the VMID at all; who the frame belongs to is the caller's question. */
enum { N48_SRC_OK = 0, N48_SRC_NOT_LIST = 1, N48_SRC_NOT_WS = 2, N48_SRC_COUNT = 3, N48_SRC_TEMPLATE = 4, N48_SRC_ARG = 5 };

static inline uint32_t n48_rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) | ((uint32_t)p[off + 2u] << 16) | ((uint32_t)p[off + 3u] << 24);
}
static inline void n48_wr32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v; p[off + 1u] = (uint8_t)(v >> 8); p[off + 2u] = (uint8_t)(v >> 16); p[off + 3u] = (uint8_t)(v >> 24);
}

/* THE SHAPE the source neuter is proven on: returns N48_SRC_OK and *nOut = the IB count, or a shape refusal (NOT_LIST, COUNT,
 * TEMPLATE, ARG). `tmpl` is the channel's IB-list template (chan+0x138), N48_TMPL_DWORDS long; every slot the IBs would occupy
 * must still be Apple's padding, so a skipped entry leaves exactly what an IB-less frame has.
 * 0.0.362: this was n48_gfxsrc_check, which also refused every VMID but 2 (N48_SRC_NOT_VMID2). The VMID test is
 * GONE from the shape: the rules and their order are otherwise unchanged, so for a VMID-2 frame this answers exactly what
 * n48_gfxsrc_check answered (tests/gfx_neuter_test.cpp: the frozen 0.0.361 check, over every generated submission). */
static inline uint32_t n48_gfxsrc_shape(const uint8_t *info, const uint32_t *tmpl, uint32_t *nOut)
{
    *nOut = 0u;
    if (!info || !tmpl) return N48_SRC_ARG;
    if (n48_rd32(info, N48_SCB_FLAGS) & 0x12u) return N48_SRC_NOT_LIST;
    const uint32_t n = n48_rd32(info, N48_SCB_COUNT);
    if (n == 0u || n > N48_SCB_MAX_IBS) return N48_SRC_COUNT;
    for (uint32_t k = 0; k < 4u * N48_SCB_MAX_IBS; k++)
        if (tmpl[N48_TMPL_IB0_DWORD + k] != N48_TMPL_NOP) return N48_SRC_TEMPLATE;
    *nOut = n;
    return N48_SRC_OK;
}

/* Zero entry i's first dword (saving it) so the original emits no packet for it; restore puts every saved dword back. */
static inline void n48_gfxsrc_zero(uint8_t *info, uint32_t n, uint32_t *saved)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t off = N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE * i;
        saved[i] = n48_rd32(info, off);
        n48_wr32(info, off, 0u);
    }
}
static inline void n48_gfxsrc_restore(uint8_t *info, uint32_t n, const uint32_t *saved)
{
    for (uint32_t i = 0; i < n; i++) n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE * i, saved[i]);
}

/* ---- 0.0.358: THE VMID ESCAPE, CLOSED AT BOTH LAYERS -------------------------------------------------------
 * notes/M4-X9-COMPLETE.md, CONFIRMED in 6 of 6 runs: SecurityAgent's Metal frame is VMID 3. n48_gfxsrc_check above
 * returns N48_SRC_NOT_VMID2 for it, the hook handed it to Apple untouched, and the ring walk recorded only `vmid == 2`
 * INDIRECT_BUFFERs, so its gfx10 IB (`00190000 00000004 03000e90`) reached the CP, which parked ON that packet
 * (CP_RB0_RPTR 0x62d = frame 0x600 + 0x2d) in every one of those runs. Two changes, both DROP-ONLY:
 *   1. THE SOURCE (n48_gfxsrc_route): a frame of exactly the shape the source neuter is proven on (IB-list, 1..4 IBs, every
 *      template slot still Apple's padding) on another VMID is classed OTHER_VMID and NEUTERED. It is routed around the
 *      decision entirely, so it can never be translated: the set of frames that reach the decision (and so the only frames
 *      COMMIT could ever pass) is byte-for-byte n48_gfxsrc_check's N48_SRC_OK set, unchanged.
 *   2. THE RING (n48_gfxn_walk): every count-2 INDIRECT_BUFFER of ANY VMID is recorded for the ring NOP, VMID 2 in its own
 *      list exactly as before and every other VMID in a second list, so no VMID-2 position the old walk recorded can be
 *      displaced by another VMID's. It is the backstop for every shape the source still passes (not an IB-list frame, count,
 *      template).
 * The frozen copies of the old logic and the planted defects (the old VMID-2-only filters) are in tests/gfx_neuter_test.cpp.
 * 0.0.362: layer 1 is now keyed on WindowServer's OWNER, not on VMID 2 (n48_gfxsrc_route_owner below; the
 * 0.0.358-0.0.361 route is frozen in the test). Layer 2, the ring walk, is unchanged: it NOPs IBs of every VMID either way. */
enum { N48_ROUTE_PASS = 0, N48_ROUTE_DECIDE = 1, N48_ROUTE_NOT_WS = 2 };

/* 0.0.362: THE ROUTE, BY OWNER. 0.0.358-0.0.361 routed on the VMID number (n48_gfxsrc_route: DECIDE for VMID 2,
 * OTHER_VMID for any other VMID), and hp3 showed VMID 2 was the forced-draw client's while WindowServer drew on VMID 3:
 * WindowServer's frames were dropped and the draw client's were judged as the compositor's. The route now takes WHOSE frame it
 * is as an input - `is_ws`, ws_identity's answer (ws_ident.h: exactly one live context owned by a living WindowServer, whose
 * root is the page-table base of THIS frame's VMID, on the VMID recorded for it) - and the VMID number decides nothing:
 *   DECIDE   the proven shape AND WindowServer's by owner                   -> judged (and at DECIDE still NEUTERED)
 *   NOT_WS   the proven shape AND NOT WindowServer's (every other client,    -> NEUTERED at the source, never judged
 *            and every frame at all while WindowServer is unidentified or
 *            ambiguous: fail closed)
 *   PASS     not the proven shape (NOT_LIST / COUNT / TEMPLATE / ARG)        -> Apple, untouched; the ring walk is the backstop
 * PASS is the SAME set of submissions 0.0.361 passed (the VMID never mattered for it), and DECIDE + NOT_WS the same set
 * 0.0.361 split into DECIDE + OTHER_VMID. So with COMMIT not armed - where DECIDE's action is NEUTER (n48_sd_action) - every
 * submission reaches the CP exactly as it did on 0.0.361; the only thing that moves is WHICH frames are judged
 * (tests/gfx_neuter_test.cpp: the frozen 0.0.361 route, and the hook's outcome over every arm but COMMIT). `*why` is
 * N48_SRC_OK for DECIDE, N48_SRC_NOT_WS for NOT_WS and the shape reason for PASS; `*nOut` the IB count for DECIDE and NOT_WS. */
static inline uint32_t n48_gfxsrc_route_owner(const uint8_t *info, const uint32_t *tmpl, uint32_t is_ws, uint32_t *nOut,
                                              uint32_t *why)
{
    uint32_t n = 0u;
    *nOut = 0u;
    *why = n48_gfxsrc_shape(info, tmpl, &n);
    if (*why != N48_SRC_OK) return N48_ROUTE_PASS;
    *nOut = n;
    if (is_ws == 1u) return N48_ROUTE_DECIDE;
    *why = N48_SRC_NOT_WS;
    return N48_ROUTE_NOT_WS;
}

/* The ring walk of hook_gfxWriteTail, moved here so the host test runs the code the kext runs. Walks [from, from + n) modulo
 * `size`: Apple's one-dword padding and type 2 are one dword, type 3 is count + 2, anything else STOPS the walk (stop_at is
 * then the dword it stopped on, else n). `fn`, when non-null, is called on every type-3 packet in order, before the IB test
 * (the kext's WRITE_DATA instrument). */
#define N48_GFXN_MAX_IBS 16u
typedef struct {
    uint32_t packets;                    /* type-3 packets */
    uint32_t ibs;                        /* count-2 INDIRECT_BUFFERs of ANY VMID */
    uint32_t ibs_vmid2, ibs_other;       /* split of `ibs` by the VMID nibble of the packet's control dword */
    uint32_t npos2, npos_other;          /* positions recorded, each list capped at N48_GFXN_MAX_IBS */
    uint32_t pos2[N48_GFXN_MAX_IBS];     /* ring dword index (mod size) of each VMID-2 IB header - the old walk's list */
    uint32_t pos_other[N48_GFXN_MAX_IBS];/* ... and of each IB of any OTHER VMID (0.0.358) */
    uint32_t vmid_other[N48_GFXN_MAX_IBS];
    uint64_t stop_at;
    uint32_t stop_hdr;
    uint64_t ib2_va; uint32_t ib2_len;   /* the first VMID-2 IB, as the old walk reported it */
} n48_gfxn_walk_t;
typedef void (*n48_gfxn_pkt_fn)(void *ctx, uint64_t i, uint32_t h, uint32_t cnt);

static inline void n48_gfxn_walk(const volatile uint32_t *g, uint64_t from, uint64_t n, uint32_t size, n48_gfxn_walk_t *w,
                                 n48_gfxn_pkt_fn fn, void *ctx)
{
    w->packets = w->ibs = w->ibs_vmid2 = w->ibs_other = w->npos2 = w->npos_other = w->stop_hdr = w->ib2_len = 0u;
    w->ib2_va = 0u;
    w->stop_at = n;
    if (!g || !size) { w->stop_at = 0u; return; }
    for (uint64_t i = 0; i < n; ) {
        const uint32_t h = g[(from + i) % size];
        if (h == N48_TMPL_NOP) { i++; continue; }
        const uint32_t type = h >> 30;
        if (type == 2u) { i++; continue; }
        if (type != 3u) { w->stop_at = i; w->stop_hdr = h; break; }
        w->packets++;
        const uint32_t cnt = (h >> 16) & 0x3FFFu, op = (h >> 8) & 0xFFu;
        if (fn) fn(ctx, i, h, cnt);
        if (op == 0x3Fu && cnt == 2u && i + 3u < n) {
            const uint32_t ctl = g[(from + i + 3u) % size], vm = (ctl >> 24) & 0xFu;
            w->ibs++;
            if (vm == 2u) {
                if (w->npos2 < N48_GFXN_MAX_IBS) w->pos2[w->npos2++] = (uint32_t)((from + i) % size);
                w->ibs_vmid2++;
                if (!w->ib2_va) {
                    w->ib2_va = ((uint64_t)g[(from + i + 2u) % size] << 32) | (g[(from + i + 1u) % size] & ~3u);
                    w->ib2_len = ctl & 0xFFFFFu;
                }
            } else {
                if (w->npos_other < N48_GFXN_MAX_IBS) {
                    w->pos_other[w->npos_other] = (uint32_t)((from + i) % size);
                    w->vmid_other[w->npos_other] = vm;
                    w->npos_other++;
                }
                w->ibs_other++;
            }
        }
        i += (uint64_t)cnt + 2u;
    }
}

/* ---- 0.0.359: THE COMMITTED FRAME'S EXEMPTION FROM THE RING NOP ---------------------------------------------
 * Verified by the reviewer (project notes): translation runs only while gGfxNeuter == 1, and while it
 * is 1 the writeTail walk NOPed EVERY INDIRECT_BUFFER, the committed frame's included - so COMMIT, as built through 0.0.358,
 * could never have drawn. This is the one exemption, and it is built to be INERT unless COMMIT is armed:
 *   an IB is spared ONLY when (1) the arm read at the walk is COMMIT, (2) a record is IN FLIGHT - set by hook_gfxCommitIB's
 *   TRANSLATE branch after its token matched THIS submission block, stamp, IB count, IB 0 VA and length, and cleared when
 *   Apple's original returns - (3) that token's seq is the seq the gate answered COMMIT for, (4) the record is unused (one
 *   token, one IB), (5) the submission had exactly one IB, (6) the walk runs on the thread that took the TRANSLATE branch
 *   (commitIndirectCommandBuffer calls ring->submit -> writeTail synchronously: , be20e1e / be1bc46), (7) the walk
 *   reached its end and found exactly ONE INDIRECT_BUFFER, of the VMID the token's frame was judged WindowServer's on
 *   (0.0.362, : ex->vmid, recorded by ws_ident.h from the hardware - until 0.0.361 this clause said "of VMID 2",
 *   the assumption hp3 refuted), (8) at the dword where the IB-list template puts IB 0
 *   (Apple's wptr at the call + 0x2d, gfx_neuter.h N48_TMPL_IB0_DWORD), and (9) that packet names the token's VA and length,
 *   and (10, M4-WS-VMID-VALID) the page-table base of that VMID, read at the walk, carries the VALID bit and names WindowServer's
 *   bound root (ws_valid.h n48_wsv_base_valid, filled by the kext): a committed frame on an invalid base is never spared.
 * Every other case returns the NOP list 0.0.358 built, position for position (tests/gfx_neuter_test.cpp: the frozen-list
 * property and the planted defects - the exemption without the arm check, and with a stale token). */
#define N48_GFXN_ARM_COMMIT 2u   /* == gfx_src_decide.h N48_SD_ARM_COMMIT; AppleHardwareHook.cpp static_asserts it */
enum {
    N48_GFXN_EX_NONE = 0,        /* no record in flight: the ordinary case, nothing to decide */
    N48_GFXN_EX_SPARED,          /* every condition held: the committed IB is NOT NOPed */
    N48_GFXN_EX_NOT_ARMED,       /* (1) */
    N48_GFXN_EX_GATE,            /* (3) the token's seq is 0 or not the one the gate answered COMMIT for */
    N48_GFXN_EX_USED,            /* (4) */
    N48_GFXN_EX_NIB,             /* (5) */
    N48_GFXN_EX_THREAD,          /* (6) */
    N48_GFXN_EX_WALK,            /* (7) the walk stopped, or there is no ring */
    N48_GFXN_EX_FRAME,           /* (7) not exactly one IB, or not on the token's (WindowServer's recorded) VMID */
    N48_GFXN_EX_POSITION,        /* (8) */
    N48_GFXN_EX_IDENTITY,        /* (9) */
    N48_GFXN_EX_BASE,            /* (10) M4-WS-VMID-VALID: the VMID's page-table base has bit 0 clear, or is not WindowServer's root */
    N48_GFXN_EX_HEAPGEN,         /* (11) build 0.0.495, switch 62: a shader-heap copy overlapping a substituted program started
                                  * or completed since the frame's verdict, is in progress, or poisoned one of its programs
                                  * (gfx_heapgen.h n48_hg_judge). Asked LAST, and only when the kext set heap_refuse. */
    N48_GFXN_EX_REASONS
};
static inline const char *n48_gfxn_ex_name(uint32_t r)
{
    static const char *const n[N48_GFXN_EX_REASONS] = {
        "none in flight", "SPARED", "not-armed", "gate/seq", "used", "ib-count", "thread", "walk", "frame", "position",
        "identity", "base-invalid", "heap-gen" };
    return r < N48_GFXN_EX_REASONS ? n[r] : "?";
}

/* 0.0.426 (an internal design note binding B8) — a multi-IB committed frame's exemption record carries EVERY IB, not
 * just IB 0. `N48_GFXN_EX_MAX_IBS` is the same 4 the submission shape allows (N48_XV_MAX_IBS); the ring walk's own cap is
 * N48_GFXN_MAX_IBS above, which is larger, so a recorded frame's positions always fit. OFF (mib 0) the arrays are neither
 * filled nor read and every clause above is byte for byte 0.0.425's. */
#define N48_GFXN_EX_MAX_IBS 4u

typedef struct {
    uint32_t inflight;    /* (2) 1 only between the TRANSLATE branch's token match and the return of Apple's original */
    uint32_t seq;         /* (3) the matched token's seq */
    uint32_t gate_seq;    /* (3) the seq n48_cm_gate answered N48_CM_OK for (0 when it did not) */
    uint32_t used;        /* (4) */
    uint32_t nib;         /* (5) IBs in the submission */
    uint32_t same_thread; /* (6) filled at the walk */
    uint32_t pos_known;   /* (8) expect_pos was read from Apple's ring at the call */
    uint32_t expect_pos;  /* (8) ring dword (mod size) of IB 0's header */
    uint64_t va;          /* (9) IB 0's VA, as the token holds it */
    uint32_t len;         /* (9) IB 0's length in dwords, as the token holds it */
    uint32_t stamp;       /* the submission's stamp (logged; matched at the hook, not re-read from the ring) */
    uint32_t vmid;        /* (7) 0.0.362: the VMID the frame was judged WindowServer's on (ws_ident.h); 0 = none, refuses */
    uint32_t base_valid;  /* (10) M4-WS-VMID-VALID: 1 only when the kext read that VMID's base at the walk and n48_wsv_base_valid held */
    /* 0.0.426 (B8) — THE MULTI-IB RECORD. Read only when `mib` is 1. */
    uint32_t mib;                                  /* 1: spare all nib IBs or none */
    uint64_t va_k[N48_GFXN_EX_MAX_IBS];            /* IB k's VA, [0] == va */
    uint32_t len_k[N48_GFXN_EX_MAX_IBS];           /* IB k's declared dwords, [0] == len */
    /* build 0.0.495 (11): 1 = the kext's switch-62 answer for THIS frame at the walk is a refusal. Defaults 0 (a zeroed
     * record, switch 62 OFF), so every record built before 0.0.495 answers exactly as it did. */
    uint32_t heap_refuse;
} n48_gfxn_exempt_t;

static inline uint32_t n48_gfxn_exempt_why(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                           uint32_t arm, const n48_gfxn_exempt_t *ex)
{
    if (!ex || ex->inflight != 1u) return N48_GFXN_EX_NONE;
    if (arm != N48_GFXN_ARM_COMMIT) return N48_GFXN_EX_NOT_ARMED;
    if (ex->seq == 0u || ex->gate_seq != ex->seq) return N48_GFXN_EX_GATE;
    if (ex->used) return N48_GFXN_EX_USED;
    if (ex->nib != 1u) {
        /* 0.0.426 (B8): with `mib` 0 this is 0.0.425's `nib != 1` refusal. With it 1 a multi-IB frame is allowed, up to the
         * submission shape's own 4; nib 0 or > 4 refuses under the SAME reason (NIB), exactly as the 0.0.425 clause refused
         * a count it could not describe. */
        if (!ex->mib || ex->nib == 0u || ex->nib > N48_GFXN_EX_MAX_IBS) return N48_GFXN_EX_NIB;
    }
    if (ex->same_thread != 1u) return N48_GFXN_EX_THREAD;
    if (!g || !size || !w || w->stop_at != n) return N48_GFXN_EX_WALK;
    /* ---- 0.0.426 (B8): THE MULTI-IB FRAME. Spare only when the walk finds EXACTLY nib IBs, ALL on the token's VMID, at
     * expect_pos + 4k, each naming IB k's own VA and length. The whole frame is spared or none of it is: a partial spare
     * (one IB left NOPed) would be a frame whose bytes came from two streams, which no run has ever produced and which the
     * design forbids. The single-IB clause below is 0.0.425's, unchanged. */
    if (ex->mib) {
        if (ex->pos_known != 1u) return N48_GFXN_EX_POSITION;
        if (ex->vmid == 0u || ex->vmid > 15u) return N48_GFXN_EX_FRAME;
        if (w->ibs != ex->nib || w->npos2 + w->npos_other != ex->nib) return N48_GFXN_EX_FRAME;
        for (uint32_t k = 0; k < ex->nib; k++) {
            const uint32_t wantp = (uint32_t)(((uint64_t)ex->expect_pos + 4ull * (uint64_t)k) % (uint64_t)size);
            uint32_t vm = 0u; bool found = false;
            for (uint32_t j = 0; j < w->npos2; j++) if (w->pos2[j] == wantp) { vm = 2u; found = true; break; }
            if (!found) for (uint32_t j = 0; j < w->npos_other; j++) if (w->pos_other[j] == wantp) { vm = w->vmid_other[j]; found = true; break; }
            if (!found || vm != ex->vmid) return N48_GFXN_EX_POSITION;
            const uint32_t h = g[wantp % size], lo = g[(wantp + 1u) % size], hi = g[(wantp + 2u) % size], ctl = g[(wantp + 3u) % size];
            const uint64_t va = ((uint64_t)hi << 32) | (lo & ~3u);
            if (!n48_gfxn_is_ib(h) || ((ctl >> 24) & 0xFu) != ex->vmid || (ctl & 0xFFFFFu) != ex->len_k[k] || va != ex->va_k[k])
                return N48_GFXN_EX_IDENTITY;
        }
        if (ex->base_valid != 1u) return N48_GFXN_EX_BASE;
        if (ex->heap_refuse) return N48_GFXN_EX_HEAPGEN;   /* build 0.0.495: after every older clause */
        return N48_GFXN_EX_SPARED;
    }
    /* 0.0.362: exactly ONE IB in the walk, of ANY list, and it must be on the token's VMID. For ex->vmid == 2 this is
     * 0.0.361's clause exactly (a VMID-2 IB is always in pos2, never in pos_other); tests/gfx_neuter_test.cpp pins it. */
    if (w->ibs != 1u || w->npos2 + w->npos_other != 1u || w->ibs_vmid2 + w->ibs_other != 1u) return N48_GFXN_EX_FRAME;
    const uint32_t oneVmid = w->npos2 ? 2u : w->vmid_other[0];
    if (ex->vmid == 0u || ex->vmid > 15u || oneVmid != ex->vmid) return N48_GFXN_EX_FRAME;
    const uint32_t p = w->npos2 ? w->pos2[0] : w->pos_other[0];
    if (ex->pos_known != 1u || p != ex->expect_pos) return N48_GFXN_EX_POSITION;
    const uint32_t h = g[p % size], lo = g[(p + 1u) % size], hi = g[(p + 2u) % size], ctl = g[(p + 3u) % size];
    const uint64_t va = ((uint64_t)hi << 32) | (lo & ~3u);
    if (!n48_gfxn_is_ib(h) || ((ctl >> 24) & 0xFu) != ex->vmid || (ctl & 0xFFFFFu) != ex->len || va != ex->va)
        return N48_GFXN_EX_IDENTITY;
    /* M4-WS-VMID-VALID: checked LAST, so every answer but SPARED is exactly aed4ff2's; only a frame that would have been spared can
     * now be refused, and only while COMMIT is armed (the arm is rung 1). */
    if (ex->base_valid != 1u) return N48_GFXN_EX_BASE;
    /* build 0.0.495 (11): LAST again, so only a frame that would have been spared can be refused by it. */
    if (ex->heap_refuse) return N48_GFXN_EX_HEAPGEN;
    return N48_GFXN_EX_SPARED;
}

/* The ring NOP's position list. 0.0.358 built it inline in hook_gfxWriteTail: VMID 2's positions, then every other VMID's.
 * It still does, and returns that list untouched unless n48_gfxn_exempt_why answers SPARED, in which case the one position
 * is removed and returned in *spared (else 0xFFFFFFFF). `pos` must hold 2 * N48_GFXN_MAX_IBS. */
static inline uint32_t n48_gfxn_nop_list(const volatile uint32_t *g, uint32_t size, uint64_t n, const n48_gfxn_walk_t *w,
                                         uint32_t arm, const n48_gfxn_exempt_t *ex, uint32_t *pos, uint32_t *spared,
                                         uint32_t *why)
{
    uint32_t np = 0u;
    for (uint32_t k = 0; k < w->npos2; k++) pos[np++] = w->pos2[k];
    for (uint32_t k = 0; k < w->npos_other; k++) pos[np++] = w->pos_other[k];
    *spared = 0xFFFFFFFFu;
    *why = n48_gfxn_exempt_why(g, size, n, w, arm, ex);
    if (*why != N48_GFXN_EX_SPARED) return np;
    /* 0.0.426 (B8): THE WHOLE FRAME OR NONE OF IT. A single-IB record spares its one position (0.0.425's rule, including
     * the belt-and-braces `pos[0] == one`); a multi-IB record spares only when the walk's list IS the frame's nib positions,
     * in which case the list we return is EMPTY - never a subset. Both clauses are unreachable if exempt_why said SPARED,
     * and both refuse (FRAME) rather than guess. */
    if (ex && ex->mib) {
        if (np != ex->nib) { *why = N48_GFXN_EX_FRAME; return np; }
        *spared = pos[0];
        return 0u;
    }
    const uint32_t one = w->npos2 ? w->pos2[0] : (w->npos_other ? w->pos_other[0] : 0xFFFFFFFFu);   /* 0.0.362: either list */
    if (np != 1u || pos[0] != one) { *why = N48_GFXN_EX_FRAME; return np; }
    *spared = pos[0];
    return 0u;
}

/* 0.0.427 ( condition (1)) — HOW MANY IBs A SPARED FRAME SPARES.
 *
 * SPARED is a FRAME-level answer (all of the frame's IBs or none, B8), but the writeTail site's ring accounting is
 * PER-IB: `gRd.ibsArmed` counts every IB the walk found, and the RING_NOP identity (gfx_dep.h) books a spared IB in
 * N48_DEPC_GN_EXEMPT exactly as it books a NOPed one in N48_DEPC_GN_IBS. Through 0.0.426 the site used
 * `SPARED ? 1u : 0u`, so a spared 2-IB frame left IB 1 unaccounted: the frame was still handed to gfx_neuter_frame as
 * `found 1` with an EMPTY position list (n48_gfxn_nop_list returns nothing for a mib spare), which booked `over = 1`
 * into gGn.overCap and then N48_DEP_GFX_ESCAPED. This helper is the count both places must use: the frame's whole nib
 * when it is a multi-IB spare, 1 for the single-IB case (byte for byte 0.0.426), 0 when the frame was not spared. Pure,
 * so the kext and the host test agree on the one number that keeps the identity clean. */
static inline uint32_t n48_gfxn_spared_n(uint32_t why, uint32_t mib, uint32_t nib)
{
    if (why != N48_GFXN_EX_SPARED) return 0u;
    if (mib && nib > 1u) return nib;
    return 1u;
}

/* A model of the original's IB-list loop (be20b91-be20c08) over a copied template, for the host test: writes an
 * INDIRECT_BUFFER packet for every entry whose first dword is non-zero. */
static inline void n48_gfxsrc_model_commit(const uint8_t *info, uint32_t *block)
{
    const uint32_t n = n48_rd32(info, N48_SCB_COUNT), flags = n48_rd32(info, N48_SCB_FLAGS), vmid = n48_rd32(info, N48_SCB_VMID);
    for (uint32_t i = 0; i < n && i < N48_SCB_MAX_IBS; i++) {
        const uint32_t off = N48_SCB_ENTRY0 + N48_SCB_ENTRY_SIZE * i;
        const uint32_t len = n48_rd32(info, off);
        if (!len) continue;
        const uint32_t valo = n48_rd32(info, off + N48_SCB_ENTRY_VA), vahi = n48_rd32(info, off + N48_SCB_ENTRY_VA + 4u);
        const uint32_t at = N48_TMPL_IB0_DWORD + 4u * i;
        block[at] = 0xC0023F00u;
        block[at + 1u] = valo & ~3u;
        block[at + 2u] = vahi;
        block[at + 3u] = (len & 0xFFFFFu) | ((vmid & 0xFu) << 24) | ((flags & ~3u) << 29);
    }
}

#endif
