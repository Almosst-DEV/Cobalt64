// ws_ident.h — WHICH GPU ADDRESS SPACE IS WINDOWSERVER'S: identity by the OWNER of its VM context (0.0.362).
// Pure C, host-tested by tests/ws_ident_test.cpp (with planted defects); the kext compiles the SAME header.
//
// THE DEFECT THIS REPLACES (hp3): every GFX safety path took "VMID 2" to mean "WindowServer". It does not. GPU
// address spaces follow the order in which contexts come to be used (hp3: the forced-draw client's root appeared first and it
// got VMID 2, WindowServer 1142 VMID 3, SecurityAgent 1263 VMID 4), so on hp3 WindowServer's frames were dropped as "another
// VMID" and the draw client's were judged as if they were the compositor's. A VMID number is an ACCIDENT OF ORDER, never an
// identity; nor is timing ("the context created after wskill", "the first to draw").
//
// THE IDENTITY, as `rootwrite`'s BY OWNER selection already makes it:
//   a CANDIDATE is a live record of a VM context whose creating process was named "WindowServer" AT createVMContext (the hook
//   runs on the creator's own thread, so pid and name are the owner's, recorded there), whose creator pid is alive NOW under
//   that same name (a killed WindowServer's context can outlive it: rw1,), whose object RE-VALIDATES (its vtable and
//   its task pointer ctx->0x28 are the ones recorded at create: a recycled allocation fails), and which has a root page.
//   EXACTLY ONE candidate -> BOUND to it (its create #, ctx, task, pid and root). None -> NONE. Two or more -> AMBIGUOUS.
//   The selection is re-run for EVERY frame (n48_ws_update), so a binding can never outlive the evidence that made it.
// THE FRAME: judged as WindowServer's (N48_WSF_JUDGE) only when BOUND, the frame's VMID is 1..15, the hardware page-table
// base of THAT VMID (GCVM_CONTEXTn_PAGE_TABLE_BASE, read by the caller) is WindowServer's root, and - once one frame has been
// judged - the VMID is the one recorded for this binding. Anything else is DROPPED with a reason. The VMID is RECORDED from the
// hardware at the first judged frame, never assumed: it does not exist at createVMContext (the root appears at the first mapVA
// and the VMID when the context is first put on the GPU).
// FAIL CLOSED: NONE, AMBIGUOUS, an unreadable context, a moved VMID -> nothing is judged; every frame is dropped and counted,
// exactly as every non-VMID-2 frame was dropped before 0.0.362.
// A RESTART (wskill): the old WindowServer's pid is gone, so its record stops being a candidate at the next frame even if Apple
// never tears its context down; the new WindowServer's context becomes the one candidate once its root exists and is bound
// FRESH (its VMID recorded anew). n48_ws_gone is the explicit drop: the kext calls it when the bound context's root is freed at
// unmapVA and when the context is released.
#ifndef N48_WS_IDENT_H
#define N48_WS_IDENT_H

#include <stdint.h>

#define N48_WS_OWNER      "WindowServer"
#define N48_WS_ROOT_MASK  0x0000FFFFFFFFF000ull

/* One recorded VM context, as the kext observes it at this frame. */
typedef struct {
    uint32_t live;          /* our record says created and not released */
    uint32_t seq;           /* its createVMContext number: part of the identity, NEVER a selector */
    uint64_t ctx, task;     /* recorded at createVMContext */
    int32_t  pid;           /* the process whose thread created it */
    uint32_t ws_at_create;  /* that process was named N48_WS_OWNER at createVMContext */
    uint32_t ws_now;        /* the same pid is alive NOW and still named N48_WS_OWNER */
    uint32_t id_ok;         /* the object re-validates: vtable and ctx->0x28 task are the recorded ones */
    uint64_t root;          /* the root page read NOW through the re-validated object (0: none yet, or freed) */
} n48_ws_rec;

enum { N48_WS_NONE = 0, N48_WS_BOUND = 1, N48_WS_AMBIGUOUS = 2 };

/* What n48_ws_update did. */
enum {
    N48_WSU_KEEP = 0,       /* bound, and the one candidate is the same context with the same root */
    N48_WSU_BIND,           /* was not bound: bound now */
    N48_WSU_REBIND,         /* was bound to something else (another context, or the same one with a new root): bound FRESH */
    N48_WSU_UNBIND_NONE,    /* was bound; no candidate now */
    N48_WSU_UNBIND_AMBIG,   /* was bound; two or more candidates now */
    N48_WSU_STILL_NONE,
    N48_WSU_STILL_AMBIG,
    N48_WSU_REASONS
};

/* The frame's verdict. */
enum {
    N48_WSF_JUDGE = 0,      /* WindowServer's frame: its VMID's page-table base is WindowServer's root, on its recorded VMID */
    N48_WSF_NONE,           /* no WindowServer context identified */
    N48_WSF_AMBIGUOUS,      /* more than one candidate: refuse */
    N48_WSF_VMID,           /* the frame's VMID is 0 (not a client) or out of range */
    N48_WSF_HW,             /* that VMID's hardware context could not be opened, or names no root */
    N48_WSF_OTHER,          /* that VMID's page-table base is NOT WindowServer's root: another client's frame */
    N48_WSF_MOVED,          /* WindowServer's root, but not on the VMID recorded for this binding */
    N48_WSF_REASONS
};

typedef struct {
    uint32_t state;         /* N48_WS_NONE / BOUND / AMBIGUOUS */
    uint32_t seq;           /* the bound context (valid while BOUND) */
    uint64_t ctx, task, root;
    int32_t  pid;
    uint32_t vmid;          /* 0 until the first judged frame of this binding, then fixed */
    uint32_t ncand;         /* candidates at the last selection */
    uint32_t gen;           /* bumped on every change of binding or state: the kext logs each transition once */
    uint64_t binds, rebinds, unbinds, gone, ambiguous_seen, vmid_latched;
    uint64_t frames[N48_WSF_REASONS];
} n48_ws;

static inline uint32_t n48_ws_is_candidate(const n48_ws_rec *r)
{
    return r->live && r->ws_at_create && r->ws_now && r->id_ok && (r->root & N48_WS_ROOT_MASK) != 0u &&
           r->ctx != 0u && r->task != 0u && r->pid > 0;
}

/* The number of candidates; *idx is the first one's index (meaningful only when the count is 1). */
static inline uint32_t n48_ws_select(const n48_ws_rec *r, uint32_t n, uint32_t *idx)
{
    uint32_t c = 0u;
    *idx = 0xFFFFFFFFu;
    for (uint32_t i = 0; r && i < n; i++) {
        if (!n48_ws_is_candidate(&r[i])) continue;
        if (c == 0u) *idx = i;
        c++;
    }
    return c;
}

static inline void n48_ws_clear_binding(n48_ws *w)
{
    w->seq = 0u; w->ctx = 0u; w->task = 0u; w->root = 0u; w->pid = -1; w->vmid = 0u;
}

/* Re-run the selection over this frame's observation and move the binding. */
static inline uint32_t n48_ws_update(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    uint32_t idx = 0xFFFFFFFFu;
    const uint32_t c = n48_ws_select(r, n, &idx);
    const uint32_t was = w->state;
    w->ncand = c;
    if (c != 1u) {
        n48_ws_clear_binding(w);
        w->state = c ? N48_WS_AMBIGUOUS : N48_WS_NONE;
        if (c > 1u) w->ambiguous_seen++;
        if (was == N48_WS_BOUND) { w->unbinds++; w->gen++; return c ? N48_WSU_UNBIND_AMBIG : N48_WSU_UNBIND_NONE; }
        if (was != w->state) w->gen++;
        return c ? N48_WSU_STILL_AMBIG : N48_WSU_STILL_NONE;
    }
    const n48_ws_rec *k = &r[idx];
    const uint64_t root = k->root & N48_WS_ROOT_MASK;
    if (was == N48_WS_BOUND && w->seq == k->seq && w->ctx == k->ctx && w->task == k->task && w->pid == k->pid &&
        w->root == root)
        return N48_WSU_KEEP;
    n48_ws_clear_binding(w);                  /* a new binding starts with NO recorded VMID */
    w->state = N48_WS_BOUND;
    w->seq = k->seq; w->ctx = k->ctx; w->task = k->task; w->pid = k->pid; w->root = root;
    w->binds++; w->gen++;
    if (was == N48_WS_BOUND) { w->rebinds++; return N48_WSU_REBIND; }
    return N48_WSU_BIND;
}

/* The kext's entry: `complete` 0 means its context table overflowed (a context exists that it holds no record of), so "exactly
 * one" cannot be proven - refuse as AMBIGUOUS whatever the records say. complete 1 is n48_ws_update. */
static inline uint32_t n48_ws_update_table(n48_ws *w, const n48_ws_rec *r, uint32_t n, uint32_t complete)
{
    if (complete) return n48_ws_update(w, r, n);
    const uint32_t was = w->state;
    n48_ws_clear_binding(w);
    w->state = N48_WS_AMBIGUOUS;
    w->ncand = 0xFFFFFFFFu;
    w->ambiguous_seen++;
    if (was == N48_WS_BOUND) { w->unbinds++; w->gen++; return N48_WSU_UNBIND_AMBIG; }
    if (was != N48_WS_AMBIGUOUS) w->gen++;
    return N48_WSU_STILL_AMBIG;
}

/* The explicit drop: the bound context's root was freed (unmapVA) or the context was released. Returns 1 if it was bound. */
static inline uint32_t n48_ws_gone(n48_ws *w, uint32_t seq, uint64_t ctx)
{
    if (w->state != N48_WS_BOUND || w->seq != seq || w->ctx != ctx) return 0u;
    n48_ws_clear_binding(w);
    w->state = N48_WS_NONE;
    w->gone++; w->unbinds++; w->gen++;
    return 1u;
}

/* Judge one frame. `vmid` is the submission's VMID field (low 4 bits); `hw_ok` / `hw_root` are the page-table base of THAT
 * VMID's hardware context, read by the caller at this frame (hw_ok 0 = the context could not be opened). Call n48_ws_update
 * first, for the same frame. */
static inline uint32_t n48_ws_frame(n48_ws *w, uint32_t vmid, uint32_t hw_ok, uint64_t hw_root)
{
    uint32_t v;
    if (w->state == N48_WS_AMBIGUOUS) v = N48_WSF_AMBIGUOUS;
    else if (w->state != N48_WS_BOUND) v = N48_WSF_NONE;
    else if (vmid == 0u || vmid > 15u) v = N48_WSF_VMID;
    else if (!hw_ok || (hw_root & N48_WS_ROOT_MASK) == 0u) v = N48_WSF_HW;
    else if ((hw_root & N48_WS_ROOT_MASK) != w->root) v = N48_WSF_OTHER;
    else if (w->vmid != 0u && w->vmid != vmid) v = N48_WSF_MOVED;
    else {
        if (w->vmid == 0u) { w->vmid = vmid; w->vmid_latched++; }
        v = N48_WSF_JUDGE;
    }
    w->frames[v]++;
    return v;
}

static inline const char *n48_ws_state_name(uint32_t s)
{
    return s == N48_WS_NONE ? "NONE (no WindowServer context identified - judging NOTHING)"
         : s == N48_WS_BOUND ? "BOUND" : s == N48_WS_AMBIGUOUS ? "AMBIGUOUS (more than one candidate - judging NOTHING)" : "?";
}
static inline const char *n48_ws_update_name(uint32_t u)
{
    static const char *const n[N48_WSU_REASONS] = { "kept", "BOUND", "REBOUND (fresh: new context or new root)",
                                                    "UNBOUND (no candidate now)", "UNBOUND (more than one candidate now)",
                                                    "still none", "still ambiguous" };
    return u < N48_WSU_REASONS ? n[u] : "?";
}
static inline const char *n48_ws_frame_name(uint32_t v)
{
    static const char *const n[N48_WSF_REASONS] = { "JUDGED (WindowServer's)", "dropped: no WindowServer identified",
                                                    "dropped: WindowServer AMBIGUOUS", "dropped: VMID 0 or out of range",
                                                    "dropped: VMID's hardware context unreadable",
                                                    "dropped: NOT WindowServer's (another client's root)",
                                                    "dropped: WindowServer's root on a VMID other than the recorded one" };
    return v < N48_WSF_REASONS ? n[v] : "?";
}

#endif
