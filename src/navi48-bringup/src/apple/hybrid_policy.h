// hybrid_policy.h — build 0.0.503 (an internal design note and step H1): WHO MAY OPEN THE ACCELERATOR.
// Pure C, host-tested by tests/gfx_copyguard_test.cpp section T22 (with planted breaks); the kext compiles the SAME header.
//
// THE HYBRID. WindowServer composites on the GPU; every other process draws on the CPU. The door every Metal, GL, 2D and
// video client passes is the accelerator's IOGraphicsAccelerator2::newUserClient (static 0x145bfe02, vtable slot 239),
// which Navi48AccelPeer.cpp swaps on OUR per-instance copy of the accelerator's vtable (ucp_hook_new_user_client). With
// switch 68 ON the hook refuses every task that is not WindowServer, with kIOReturnNotPermitted and *handler NULL,
// BEFORE Apple's function is called; Metal's device creation then returns nil (HYBRID.md.1, CONFIRMED from bytes).
//
// THE IDENTITY (HYBRID.md.2): the calling process's name is exactly "WindowServer" (ws_ident.h's N48_WS_OWNER, the
// owner rule the rest of the kext uses) AND its uid is 88 (_windowserver). A name alone is not an identity (any process
// can call itself WindowServer); a uid alone is not either. No name (unknown caller) is never WindowServer.
//
// SWITCH 68, DEFAULT OFF. `gfxneuter 68 | M << 8`: M 1 ON (= 324), M 2 OFF (= 580, the default and the boot value), bare
// `68` reads and changes nothing. OFF, n48_hy_decide ADMITS every caller: the hook calls Apple exactly as before. The
// switch is changed only while NO arm stands (n48_hy_switch_refused, one-shot or continuous); the kext reads it ONCE per
// newUserClient call (a latch), and that one value feeds both the decision and the counters.
//
// Clients opened before 68 goes ON stay admitted (nothing here closes them): they are counted as `others_off`, the
// pre-existing figure the H1 run compares `others_on` (which must stay 0) against.
#ifndef N48_HYBRID_POLICY_H
#define N48_HYBRID_POLICY_H

#include <stdint.h>
#include "ws_ident.h"   // N48_WS_OWNER "WindowServer"

#ifdef __cplusplus
extern "C" {
#endif

#define N48_HY_WS_UID     88u     /* _windowserver */
#define N48_HY_NAME_MAX   64u     /* the longest name compared; proc_selfname's buffer is smaller */

enum { N48_HY_ADMIT = 0u, N48_HY_REFUSE = 1u };

/* 1 iff `name` is exactly N48_WS_OWNER (bounded, no libc) and `uid` is N48_HY_WS_UID. */
static inline uint32_t n48_hy_is_ws(const char *name, uint32_t uid)
{
    const char *w = N48_WS_OWNER;
    uint32_t i;
    if (!name || uid != N48_HY_WS_UID) return 0u;
    for (i = 0u; i < N48_HY_NAME_MAX; i++) {
        if (name[i] != w[i]) return 0u;
        if (w[i] == '\0') return 1u;       /* both ended together: an exact match, "WindowServerX" is not */
    }
    return 0u;
}

/* THE POLICY. `sw_on` is switch 68 as latched for this call. OFF admits everyone (today's behaviour). */
static inline uint32_t n48_hy_decide(const char *name, uint32_t uid, uint32_t sw_on)
{
    if (!sw_on) return N48_HY_ADMIT;
    return n48_hy_is_ws(name, uid) ? N48_HY_ADMIT : N48_HY_REFUSE;
}

/* The counters the report line prints. */
typedef struct {
    uint64_t ws;          /* WindowServer (name + uid 88) admitted, switch ON or OFF */
    uint64_t others_off;  /* anything else admitted while 68 was OFF: the pre-existing clients */
    uint64_t refused;     /* refused while 68 was ON (Apple never called) */
    uint64_t others_on;   /* anything else ADMITTED while 68 was ON: must stay 0 */
} n48_hy_counts;

/* Account one decision. `sw_on` must be the SAME latched value n48_hy_decide was given. */
static inline void n48_hy_count(n48_hy_counts *c, uint32_t decision, const char *name, uint32_t uid, uint32_t sw_on)
{
    if (!c) return;
    if (decision == N48_HY_REFUSE) { c->refused++; return; }
    if (n48_hy_is_ws(name, uid)) { c->ws++; return; }
    if (sw_on) c->others_on++; else c->others_off++;
}

/* The kext's lines (Navi48AccelPeer.cpp prefixes "Navi48AccelPeer: "; the driver log keeps 511 bytes a line). The host test
 * expands each with its widest arguments (20-digit counters, a 32-byte name, the longest `how`) and bounds it. */
#define N48_HY_TOKEN "hybrid68-newuserclient 0.0.503"
#define N48_HY_REFUSE_FMT "hybrid68: REFUSED newUserClient - pid %d name \"%s\" uid %u connect type %u -> kIOReturnNotPermitted, " \
                          "*handler NULL, Apple NOT called (refusal %llu)"
#define N48_HY_ADMIT_FMT  "hybrid68: admitted WindowServer - pid %d uid %u connect type %u (switch 68 ON; admission %llu)"
#define N48_HY_REPORT_FMT "hybrid68: switch 68 %s%s%s. hook %s, accelerator %s at install; admitted WindowServer %llu, " \
                          "others while OFF (pre-existing) %llu, others while ON %llu (must be 0); refused %llu; owner != " \
                          "caller %llu [" N48_HY_TOKEN "]"
#define N48_HY_INERT_TXT  " - INERT: the slot-239 hook is not installed"

/* The verb's mid-arm guard:a change (not a bare read) is refused while ANY arm stands (one-shot or continuous). */
static inline uint32_t n48_hy_switch_refused(uint32_t is_read, uint32_t arm_stands)
{
    return (!is_read && arm_stands) ? 1u : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* N48_HYBRID_POLICY_H */
