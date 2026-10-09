/*
 * Navi48AppKey.h - kext 0.0.640 (GPU-apps stage G4; op STRIKES added in 0.0.641): the identity key of the APP allow-list and the packing of accel action 104 `appallow`. ONE plain-C header shared by the kext
 * (amd/native_g4_pure.h, Navi48NativeClient.cpp) and tools/pc/navi48test.c, so the two sides can never disagree on how a process name becomes a key.
 *
 * WHAT IS KEYED (and what that is worth). The only identity a kext can read at IOServiceOpen time through the public KPI is proc_name(proc_selfpid()) (= proc_selfname()): the opener's p_comm, the executable's
 * base name cut to MAXCOMLEN (16) characters, set at exec. There is no KPI for the executable's path (vn_getpath needs a vnode, proc_getexecutablevnode is not in the SDK headers) and none
 * for the code-signing team id (csproc_get_teamid is KERNEL_PRIVATE). So the allow-list is a CONTAINMENT list for honest programs (default-deny: an unknown program never gets a GPU
 * session), NOT a security boundary: any local user can copy a binary to a file with an allowed name. The key is a 60-bit FNV-1a of those (up to) 16 characters, so a whole entry fits
 * in one accel scalar together with the 4-bit operation; the kext also logs the name behind every refusal so root can see what to add.
 */
#ifndef NAVI48_APP_KEY_H
#define NAVI48_APP_KEY_H

#include <stdint.h>

#define N48A_COMM_MAX   16u      /* MAXCOMLEN: the characters of the name that count */
#define N48A_KEY_BITS   60u
#define N48A_KEY_MASK   ((1ull << N48A_KEY_BITS) - 1ull)
#define N48A_OP_SHIFT   60u
#define N48A_OP_ADD     1u       /* accel 104: arg = (1 << 60) | key */
#define N48A_OP_REMOVE  2u       /* arg = (2 << 60) | key */
#define N48A_OP_LIST    3u       /* arg = (3 << 60) | page (0..3); nine entries per page */
#define N48A_OP_STRIKES 4u       /* 0.0.641: arg = (4 << 60) | 0; the per-name strike table (automatic recoveries blamed on an APP session), four (key, count) pairs */

static inline uint64_t n48a_comm_key(const char *comm) {
    uint64_t h = 0xcbf29ce484222325ull;
    if (comm != 0)
        for (unsigned i = 0; i < N48A_COMM_MAX && comm[i] != 0; i++) { h ^= (uint8_t)comm[i]; h *= 0x100000001b3ull; }
    h &= N48A_KEY_MASK;
    return h != 0ull ? h : 1ull;   /* 0 is the empty slot */
}
static inline uint64_t n48a_arg(uint32_t op, uint64_t keyOrPage) { return ((uint64_t)op << N48A_OP_SHIFT) | (keyOrPage & N48A_KEY_MASK); }

#endif /* NAVI48_APP_KEY_H */
