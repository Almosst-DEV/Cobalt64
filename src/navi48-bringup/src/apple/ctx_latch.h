// ctx_latch.h — THE CONTEXT LATCH (notes/M4-BOOT-RACE.md option (d), notes/M4-CONTEXT-LATCH.md). Pure C, host-tested by
// tests/ctx_latch_test.cpp; the kext compiles the SAME header into hook_createVMContext and the boot chain.
//
// THE INVARIANT. No client may hold a GPU memory context before every protection is live: the SDMA drain, the render drain,
// the source hook and the ring neuter. Every client submission needs a VM context first (mapVA, then VMPT work on chan 14; a
// command buffer, then the GFX ring), and every context is created through AMDHWVMM slot 40, which our hook owns from before
// Apple's accelerator starts. So the door is createVMContext: CLOSED from kext load, OPEN only on a sample in which all four
// protections report themselves live, and FAILED - closed for the rest of the boot - on any chain failure.
//
// WHY WAITING IS SAFE WHERE HOLDING WORK WAS NOT (hp2,). A client that waits here has no context, so it has submitted
// nothing and Apple holds no stamp of it; the hang detector has nothing to time. The ring-level gate hp2 had (SDMA rings
// accepted and not fetched) killed it because the work was already stamped.
//
// WHAT THIS HEADER DECIDES, AND WHAT IT LEAVES TO THE KEXT.
//   - It decides, from data only: whether a caller is admitted, bypasses, waits or is refused, and when the latch opens or
//     fails. It never reads a clock, a lock or a global; the caller passes `now` and the observed protection mask.
//   - The kext derives each protection bit from that protection's OWN state (gDrainState, gRenderDrainState + the ring's live
//     vtable, gGs.installState + the channel's live vtable, gGfxNeuter), never from a chain step counter.
//   - The kext decides the caller's IDENTITY (proc_selfpid() == 0, kernel_task) and passes it as `who`. The bypass is by
//     identity ONLY: nothing here admits a client because of when it arrived.
//
// THE WAIT BOUND IS ONE BOOT-WIDE DEADLINE, anchored by the first caller that has to wait. Every later waiter shares it.
// Past it, a still-closed latch REFUSES - and a refusal is NEVER a NULL context (n48_latch_action): M4-CONTEXT-LATCH.md
// section 3 CONFIRMED by disassembly that a NULL from createVMContext panics the kernel. AMDAccelTask::init returns false
// (0xbdf5563 `48 85 c0 / 74 61`) before IOAccelTask::init has written this+0x10 (0x145bad8c `4c 89 6b 10`, reached only after
// the context), createUserGPUTask then releases the task, and AMDAccelTask::free dereferences it unconditionally (0xbdf576c
// `48 8b 47 10`, 0xbdf5770 `48 8b b8 58 1a 00 00`). So a refused client is PARKED: it keeps sleeping, never calls Apple while
// the latch is not OPEN, and is admitted if a slow chain opens it later. A FAILED latch parks it until the boot ends.
#ifndef N48_CTX_LATCH_H
#define N48_CTX_LATCH_H

#include <stdint.h>

// Protection bits (the kext fills them from each protection's own state).
enum {
    N48_LP_SDMA_DRAIN = 1u << 0,   // gDrainState == LIVE (every SDMA ring hooked, translated, mapped)
    N48_LP_RENDER     = 1u << 1,   // gRenderDrainState 1/2 and Apple's GFX ring still carries our vtable copy
    N48_LP_SRC_HOOK   = 1u << 2,   // gGs.installState 1 and the PM4 GFX channel still carries our vtable copy
    N48_LP_NEUTER     = 1u << 3,   // gGfxNeuter == 1
    N48_LP_ALL        = 0xFu,
};

enum { N48_LATCH_CLOSED = 0, N48_LATCH_OPEN = 1, N48_LATCH_FAILED = 2 };

// Why it FAILED (sticky for the boot).
enum {
    N48_LF_NONE = 0,
    N48_LF_MODE = 1,          // the boot asked for the latch without the chain bits that can open it
    N48_LF_NO_LOCK = 2,       // IOLockAlloc failed: nothing could ever wake a waiter
    N48_LF_THREAD = 3,        // the chain's thread could not be started
    N48_LF_CHAIN_STEP = 4,    // Phase A refused at a step
    N48_LF_PARTIAL = 5,       // the chain ended with fewer than all four protections live
    N48_LF_HOOK_MISSING = 6,  // the createVMContext hook itself is not installed: the latch cannot hold anyone
};

// Who is calling createVMContext. Decided by the kext from the calling proc, never from a time.
enum { N48_LC_CLIENT = 0, N48_LC_KERNEL = 1 };

// One call's decision.
enum {
    N48_LD_PASS_OFF       = 0,   // the latch is not enforced this boot: Apple's call, exactly as 0.0.360
    N48_LD_ADMIT          = 1,   // OPEN: Apple's call
    N48_LD_BYPASS         = 2,   // kernel_task: Apple's call in any state (Apple's own start needs it; hp1/hp2 dl 1811)
    N48_LD_WAIT           = 3,   // CLOSED and before the deadline: sleep, then decide again
    N48_LD_REFUSE_FAILED  = 4,   // FAILED: refused (the kext PARKS it: never NULL, see n48_latch_action)
    N48_LD_REFUSE_TIMEOUT = 5,   // CLOSED at or past the deadline: refused (PARKED; admitted if the chain opens it later)
    N48_LD_REFUSE_INTR    = 6,   // the wait was interrupted: refused (PARKED)
};

typedef struct {
    uint32_t enforced;       // 1 when this boot asked for the latch (navi48-boot-chain bit 5)
    uint32_t state;          // N48_LATCH_*
    uint32_t fail_why;       // N48_LF_*
    uint32_t live_at_open;   // the mask sampled when it opened (N48_LP_ALL by construction)
    uint32_t live_at_end;    // the mask sampled at the chain's end, whichever way it went
    uint32_t pad;
    uint64_t bound;          // the boot-wide wait bound, in the unit of `now`
    uint64_t deadline;       // 0 until the first waiter anchors it; then `now + bound` of that first waiter
} n48_latch;

static inline const char *n48_latch_state_name(uint32_t s)
{
    return s == N48_LATCH_CLOSED ? "CLOSED" : s == N48_LATCH_OPEN ? "OPEN" : s == N48_LATCH_FAILED ? "FAILED" : "?";
}

static inline const char *n48_latch_decision_name(uint32_t d)
{
    switch (d) {
    case N48_LD_PASS_OFF:       return "PASS (latch not enforced)";
    case N48_LD_ADMIT:          return "ADMITTED (latch open)";
    case N48_LD_BYPASS:         return "BYPASS (kernel_task, by identity)";
    case N48_LD_WAIT:           return "WAIT";
    case N48_LD_REFUSE_FAILED:  return "REFUSED (latch FAILED for this boot) - PARKED, never NULL";
    case N48_LD_REFUSE_TIMEOUT: return "REFUSED (boot-wide deadline passed, latch still closed) - PARKED, never NULL";
    case N48_LD_REFUSE_INTR:    return "REFUSED (wait interrupted) - PARKED, never NULL";
    default:                    return "?";
    }
}

static inline const char *n48_latch_fail_name(uint32_t w)
{
    switch (w) {
    case N48_LF_NONE:         return "none";
    case N48_LF_MODE:         return "latch bit without phase A + drain + render bits";
    case N48_LF_NO_LOCK:      return "IOLockAlloc failed";
    case N48_LF_THREAD:       return "the chain thread could not start";
    case N48_LF_CHAIN_STEP:   return "phase A refused at a step";
    case N48_LF_PARTIAL:      return "the chain ended with a partial protection set";
    case N48_LF_HOOK_MISSING: return "the createVMContext hook is not installed";
    default:                  return "?";
    }
}

// Kext load: CLOSED. `enforced` 0 leaves every call PASS_OFF (0.0.360 behaviour). `mode_ok` is the caller's judgement that
// the boot's chain can open it at all (phase A + drain + render requested); an enforced latch without it fails at once, so
// no client ever waits for a chain that was never going to run.
static inline void n48_latch_init(n48_latch *l, uint32_t enforced, uint32_t mode_ok, uint64_t bound)
{
    l->enforced = enforced ? 1u : 0u;
    l->state = N48_LATCH_CLOSED;
    l->fail_why = N48_LF_NONE;
    l->live_at_open = 0; l->live_at_end = 0; l->pad = 0;
    l->bound = bound;
    l->deadline = 0;
    if (l->enforced && !mode_ok) { l->state = N48_LATCH_FAILED; l->fail_why = N48_LF_MODE; }
}

// A chain failure. CLOSED -> FAILED, sticky for the boot. An OPEN latch stays open: it only ever opened on a sample in which
// all four protections were live, and every context it admitted exists already.
static inline void n48_latch_fail(n48_latch *l, uint32_t why)
{
    if (l->state == N48_LATCH_CLOSED) { l->state = N48_LATCH_FAILED; l->fail_why = why ? why : N48_LF_CHAIN_STEP; }
}

// The chain's end: the ONLY transition to OPEN, and it needs every bit in one sample. Anything less is a failure for the
// boot, never a partial open. Returns the resulting state.
static inline uint32_t n48_latch_chain_end(n48_latch *l, uint32_t live)
{
    l->live_at_end = live;
    if (l->state != N48_LATCH_CLOSED) return l->state;
    if ((live & N48_LP_ALL) == N48_LP_ALL) { l->state = N48_LATCH_OPEN; l->live_at_open = live & N48_LP_ALL; }
    else { l->state = N48_LATCH_FAILED; l->fail_why = N48_LF_PARTIAL; }
    return l->state;
}

// One createVMContext call (re-entered after every wake and every park slice). `now` is monotonic, in the unit of `bound`. The first caller that
// has to wait anchors the boot-wide deadline; the order of the tests is the policy:
//   not enforced -> PASS; kernel_task -> BYPASS; OPEN -> ADMIT; FAILED -> REFUSE; interrupted -> REFUSE;
//   CLOSED -> WAIT until the deadline, then REFUSE.
static inline uint32_t n48_latch_decide(n48_latch *l, uint32_t who, uint64_t now, uint32_t interrupted)
{
    if (!l->enforced) return N48_LD_PASS_OFF;
    if (who == N48_LC_KERNEL) return N48_LD_BYPASS;
    if (l->state == N48_LATCH_OPEN) return N48_LD_ADMIT;
    if (l->state != N48_LATCH_CLOSED) return N48_LD_REFUSE_FAILED;
    if (interrupted) return N48_LD_REFUSE_INTR;
    if (l->deadline == 0) l->deadline = (now + l->bound) ? now + l->bound : 1u;
    if (now >= l->deadline) return N48_LD_REFUSE_TIMEOUT;
    return N48_LD_WAIT;
}

// Does this decision call Apple's createVMContext? The ONLY three that do.
static inline uint32_t n48_latch_calls_apple(uint32_t d)
{
    return d == N48_LD_PASS_OFF || d == N48_LD_ADMIT || d == N48_LD_BYPASS;
}

// What the kext DOES with a decision. There is deliberately no "return NULL" action (see the header comment): the three that
// call Apple call Apple, WAIT sleeps until the boot-wide deadline, and every refusal PARKS - it sleeps in slices and decides
// again, so the only way out of the latch for a client is an OPEN latch.
enum { N48_LA_CALL_APPLE = 0, N48_LA_SLEEP_TO_DEADLINE = 1, N48_LA_PARK = 2 };
static inline uint32_t n48_latch_action(uint32_t d)
{
    if (n48_latch_calls_apple(d)) return N48_LA_CALL_APPLE;
    return d == N48_LD_WAIT ? N48_LA_SLEEP_TO_DEADLINE : N48_LA_PARK;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE GFX PROTECTIONS' ARM PLAN (0.0.360+: armed by the chain at [10], and by `accel gfxneuter 1`). One function for both,
// so the recipe's later `gfxneuter 1` is provably a no-op once the chain has armed them: after the plan is applied, the plan
// is empty (tests/ctx_latch_test.cpp, the idempotence property).
// ---------------------------------------------------------------------------------------------------------------------
enum { N48_GP_REFUSE = 1u << 0, N48_GP_SET_NEUTER = 1u << 1, N48_GP_INSTALL_SRC = 1u << 2 };

// render_state: gRenderDrainState (0 off, 1 armed, 2 wrote, 3 refused). neuter: gGfxNeuter. src_state: gGs.installState
// (0 never tried, 1 installed, 2 refused). A refused install is retried, as the verb always did.
static inline uint32_t n48_gfxprot_plan(uint32_t render_state, uint32_t neuter, uint32_t src_state)
{
    if (render_state != 1u && render_state != 2u) return N48_GP_REFUSE;
    uint32_t p = 0;
    if (neuter != 1u) p |= N48_GP_SET_NEUTER;
    if (src_state != 1u) p |= N48_GP_INSTALL_SRC;
    return p;
}

#endif
