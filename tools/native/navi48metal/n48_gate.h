// n48_gate.h - the pure half of the bundle's load policy outside WindowServer (GPU-apps stage G4, kext 0.0.640; an internal design note section 2 and the G2 review).
// No Metal, no IOKit, no I/O: host test test-gate.c compiles this very header and drives the very functions Navi48Device.m calls (n48_admit, n48_pc_open, n48_fallback_ok).
//
// Three kinds of process load the bundle (every Metal-enumerating process does):
//   WS    WindowServer (getprogname() == "WindowServer"). Today's rules, bit for bit: the safety gates only decline on a POSITIVE reading.
//   ROOT  a root tool with N48M_ALLOW=1 (mtlprobe and friends). The same gates, but they now FAIL CLOSED (below).
//   OTHER any other process (an ordinary user application). Admitted only if the KERNEL admits it: the bundle opens (and at once closes) a probe connection to Navi48Bringup and
//         reads QueryInfo; the kernel's open policy (uid >= 501 + navi48-apps=1 + navi48-multisession=1 + the allow-list) is the only decision, never the process name. The kernel must
//         also report the APP role (ABI 1.10 budget flags): a root process the kernel admits as root stays declined unless it set N48M_ALLOW=1, as before. 0.0.641: and a process with
//         euid < 501 (root, daemons) is not probed at all (n48g_declined_by_class).
//
// FAIL CLOSED outside WindowServer. The G2 review found the three safety gates fail OPEN in a sandbox: stat("/private/tmp/n48m-off") is denied (EPERM, not ENOENT) and was read as
// "no kill file"; the nub properties Navi48,Ready / Navi48,AutoDisarmed are filtered by the sandbox's iokit-get-properties allow-list, so a denied read came back as "absent" and was
// read as "proceed". Outside WindowServer an answer that is not a positive "all clear" now declines:
//   * kill file: only ENOENT / ENOTDIR (a real "no such file") means absent; any other stat failure is UNREADABLE and declines.
//   * Navi48,Ready must be PRESENT and 1 (the kext publishes it on every nub since 0.0.612). Absent / unreadable / 0 declines.
//   * Navi48,AutoDisarmed: 1 declines. (The kext does not publish this property today, so an absent or denied read cannot be told apart from "not published": it cannot fail closed.
//     The display-pipe state is WindowServer's concern; an APP session never reaches the scanout selectors. Recorded as an open doubt.)
//   * the probe's QueryInfo: unreadable, N48N_INFO_HUNG set, or the APP role not reported, declines.
#ifndef N48_GATE_H
#define N48_GATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define N48G_CLASS_WS    0
#define N48G_CLASS_ROOT  1
#define N48G_CLASS_OTHER 2
static inline int n48g_class(int isWs, int allowRoot) { return isWs ? N48G_CLASS_WS : allowRoot ? N48G_CLASS_ROOT : N48G_CLASS_OTHER; }

// 0.0.641 (G4 review HIGH 2): only a process whose EFFECTIVE uid is an ordinary user's (>= 501, the same bound as the kernel's kAppMinUid) is ever probed. A root process without N48M_ALLOW=1 and a
// system daemon (euid < 501, _windowserver's 88 included when it is not named WindowServer) is declined BY CLASS, exactly as in 0.0.632, with no kernel open at all: the probe would otherwise open a real
// session as root for every Metal-enumerating daemon (stalling WindowServer's exclusive open with the switches off, resetting the seqno as first opener).
#define N48G_APP_MIN_EUID 501u
static inline int n48g_declined_by_class(int cls, uint32_t euid) { return cls == N48G_CLASS_OTHER && euid < N48G_APP_MIN_EUID; }
#define N48G_DECLINE_BY_CLASS_TEXT "process is not WindowServer and N48M_ALLOW=1 (as root) is not set"

// ---- the kill file ----
#define N48G_KILL_ABSENT     0
#define N48G_KILL_PRESENT    1
#define N48G_KILL_UNREADABLE 2
static inline int n48g_kill_state(int statRc, int err) {
    if (statRc == 0) return N48G_KILL_PRESENT;
    return (err == ENOENT || err == ENOTDIR) ? N48G_KILL_ABSENT : N48G_KILL_UNREADABLE;
}
// ignored: the root test bypass (N48M_TEST_IGNORE_KILL) applied to a PRESENT file. WindowServer: only a present file declines (today). Everyone else: anything but a clean "absent" declines.
static inline int n48g_kill_declines(int isWs, int state, int ignored) {
    if (state == N48G_KILL_PRESENT) return !ignored;
    return isWs ? 0 : (state != N48G_KILL_ABSENT);
}

// ---- the nub flags: -1 = property absent (or filtered: the registry cannot tell), -2 = the nub (parent entry) could not be reached, 0 / 1 = the value ----
#define N48G_FLAG_ABSENT   (-1)
#define N48G_FLAG_NONUB    (-2)
static inline int n48g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }
static inline int n48g_autodisarm_declines(int isWs, int flag) { (void)isWs; return flag == 1; }

// ---- which service the N48N connection is opened on (G6, bundle 8) ----
// A sandboxed application's profile admits user clients only on a service that conforms to IOAccelerator (an internal design note "G6 design"). The accelerator port this bundle was initialised with IS such a
// service, and the aux kext's Navi48Accelerator hands out the same native client on it. So the APP class (CLASS_OTHER: not WindowServer, not an N48M_ALLOW root tool) probes on THAT port and tells Mesa to open
// "Navi48Accelerator" (radv_darwin_set_service_class); WindowServer and root tools keep Mesa's default, Navi48Bringup, untouched (no setter call at all).
#define N48G_SVC_ACCEL        "Navi48Accelerator"
static inline const char *n48g_mesa_service_for_class(int cls) { return cls == N48G_CLASS_OTHER ? N48G_SVC_ACCEL : (const char *)0; }   // NULL = leave Mesa's default
#define N48G_MESA_SETTER      "radv_darwin_set_service_class"

// ---- the kernel probe ----
#define N48G_UC_TYPE          0x4E34384Eu   // 'N48N'
#define N48G_SEL_HELLO        0u
#define N48G_SEL_QUERYINFO    1u
#define N48G_HELLO_F_MINOR    (1ull << 8)
#define N48G_INFO_SIZE        192u
#define N48G_INFO_FLAGS_OFF   8u            // n48n_info.flags
#define N48G_INFO_HUNG        1u
#define N48G_INFO_BUDGET_OFF  172u          // n48n_info.reserved[3] (ABI 1.10): the budget flags word
#define N48G_BUDGET_APP       2u            // N48N_BUDGET_APP: the kernel admitted this session as an allow-listed APPLICATION (not as root)
#define N48G_KR_EXCLUSIVE     0xe00002c5u   // kIOReturnExclusiveAccess: every session slot is busy, or a previous client is still closing
#define N48G_PROBE_RETRIES    50            // x 10 ms: the same courtesy Mesa's winsys extends to a client that is still closing
#define N48G_PROBE_OK     0
#define N48G_PROBE_NOSVC  1
#define N48G_PROBE_OPEN   2
#define N48G_PROBE_HELLO  3
#define N48G_PROBE_INFO   4
#define N48G_PROBE_HUNG   5
#define N48G_PROBE_NOTAPP 6
static inline int n48g_probe_retry(uint32_t openRc, int tries) { return openRc == N48G_KR_EXCLUSIVE && tries < N48G_PROBE_RETRIES; }
// budgetFlags: n48n_info.reserved[3]. A root process is admitted by the kernel too, as root - without N48M_ALLOW=1 that must stay declined, so the verdict
// needs the kernel to say APP.
static inline int n48g_probe_verdict(int haveSvc, uint32_t openRc, uint32_t helloRc, uint32_t infoRc, uint32_t infoFlags, uint32_t budgetFlags) {
    if (!haveSvc) return N48G_PROBE_NOSVC;
    if (openRc != 0u) return N48G_PROBE_OPEN;
    if (helloRc != 0u) return N48G_PROBE_HELLO;
    if (infoRc != 0u) return N48G_PROBE_INFO;
    if ((infoFlags & N48G_INFO_HUNG) != 0u) return N48G_PROBE_HUNG;
    if ((budgetFlags & N48G_BUDGET_APP) == 0u) return N48G_PROBE_NOTAPP;
    return N48G_PROBE_OK;
}
static inline const char *n48g_probe_text(int v) {
    switch (v) {
    case N48G_PROBE_OK:    return "the kernel admitted this process";
    case N48G_PROBE_NOSVC: return "process is not WindowServer or an N48M_ALLOW=1 root tool, and the Navi48Bringup service was not found (or the sandbox denied the lookup)";
    case N48G_PROBE_OPEN:  return "process is not WindowServer or an N48M_ALLOW=1 root tool, and the kernel refused its N48N open (not allow-listed, or the user-application role is off)";
    case N48G_PROBE_HELLO: return "the kernel probe's Hello failed";
    case N48G_PROBE_INFO:  return "the kernel probe's QueryInfo could not be read";
    case N48G_PROBE_HUNG:  return "the kernel reports the GPU HUNG";
    case N48G_PROBE_NOTAPP: return "the kernel admitted this process as root, not as an allow-listed application (a root tool needs N48M_ALLOW=1)";
    default:               return "?";
    }
}

// ---- the pipeline cache file (T2) ----
// WS and ROOT: as before (the test-hook directory when allowed, else /private/var/tmp when writable, else /private/tmp; one shared file name). OTHER: a file of its OWN in the user's cache
// directory (confstr _CS_DARWIN_USER_CACHE_DIR, which ends with '/'), named after the program; with no such directory there is NO cache (never /private/var/tmp: a user's file there would
// sit in the sticky directory beside WindowServer's and could block its rename). Returns the path length, 0 = no cache.
static inline size_t n48g_pcache_path(char *out, size_t n, int cls, const char *envDirAllowed, int varTmpWritable, const char *userCacheDir, const char *prog) {
    if (n == 0) return 0;
    out[0] = 0;
    int w;
    if (cls == N48G_CLASS_OTHER) {
        if (!userCacheDir || !userCacheDir[0]) return 0;
        char nm[48]; size_t k = 0;
        for (const char *p = prog; p && *p && k < 40; p++) {
            const char c = *p;
            nm[k++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') ? c : '_';
        }
        if (k == 0) { memcpy(nm, "app", 3); k = 3; }
        nm[k] = 0;
        const size_t dl = strlen(userCacheDir);
        w = snprintf(out, n, "%s%sn48m-pipecache.%s.bin", userCacheDir, (userCacheDir[dl - 1] == '/') ? "" : "/", nm);
    } else {
        const char *dir = (envDirAllowed && envDirAllowed[0]) ? envDirAllowed : (varTmpWritable ? "/private/var/tmp" : "/private/tmp");
        w = snprintf(out, n, "%s/n48m-pipecache.bin", dir);
    }
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// Who writes the file: WindowServer; an N48M_ALLOW root tool only with N48M_PCACHE_SAVE; an OTHER process (an application) its own file.
static inline int n48g_pcache_save(int cls, int rootSaveEnv) { return cls == N48G_CLASS_WS ? 1 : cls == N48G_CLASS_ROOT ? rootSaveEnv : 1; }

// ---- a spvcache miss ----
// The hot-swap fallback / no-op placeholder instead of an NSError: WindowServer, the two root test switches, and now a process the kernel admitted as an application (a Metal client
// must not crash on a missing translation; the real pipeline is swapped in when the translator has produced it).
static inline int n48g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) { return isWs || force || testAsWs || appAdmitted; }

// ---- the AIR dump directories (0.0.641 safety rules, 0.0.652 / bundle build 6: ONE SUBDIRECTORY PER UID; C1 finding A) ----
// 0.0.641: /tmp/n48m was created 0700 and used only when owned by the caller. That made it per-BOOT, not per-user: the first uid to create it (WindowServer, uid 88) locked every uid-501 app out of
// dumping, so an admitted app's spvcache misses stayed magenta for good (an internal design note "G6 compatibility", finding A).
// Now:  PARENT  /tmp/n48m            a real directory (lstat, never a symlink), mode 01777 (sticky, world-writable: every uid can make its own subdirectory, none can remove another's),
//                                    owned by root, by the translate daemon's user (N48G_DAEMON_UID) or by the caller. A parent that is OURS (owner == caller) with the wrong mode is repaired to 01777
//                                    without following links (fchmodat AT_SYMLINK_NOFOLLOW); a parent owned by anyone else with the wrong mode (or a symlink) means: skip dumping.
//       SUBDIR  /tmp/n48m/<euid>     created by that uid, 0700, lstat-checked: a real directory OWNED BY THE CALLER (a planted subdir of someone else, or a symlink, skips the dump).
//                                    The daemon's user is granted list+search on it by an ACL entry (n48_dumpacl.h), so the mode stays 0700 for everyone else.
// The daemon scans every numeric subdirectory and trusts a file only when the directory AND the file are owned by the uid in the directory's name (pc-translate.sh).
#ifndef N48G_DUMP_DIR
#define N48G_DUMP_DIR "/tmp/n48m"   // tests (test-dumpdir.sh) compile the real helper with -DN48G_DUMP_DIR pointing into a scratch directory
#endif
#define N48G_DAEMON_UID 4294967294u   // `nobody` (uid -2): the unprivileged account com.navi48.translate runs as (INSTALL.md "translate daemon")
static inline int n48g_dumpdir_ok(int lstatRc, int isDir, int isLnk, uint32_t ownerUid, uint32_t callerUid) { return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }
static inline int n48g_dumpdir_needs_narrow(uint32_t mode) { return (mode & 0077u) != 0u; }
#define N48G_DUMP_ROOT_MODE 01777u
static inline int n48g_dumproot_ok(int lstatRc, int isDir, int isLnk, uint32_t ownerUid, uint32_t mode, uint32_t callerUid) {
    return lstatRc == 0 && isDir && !isLnk && (ownerUid == 0u || ownerUid == callerUid || ownerUid == N48G_DAEMON_UID) && (mode & 07777u) == N48G_DUMP_ROOT_MODE;
}
static inline int n48g_dumproot_needs_fix(uint32_t ownerUid, uint32_t callerUid, uint32_t mode) { return ownerUid == callerUid && (mode & 07777u) != N48G_DUMP_ROOT_MODE; }
// "<parent>/<uid>" for the calling process's own subdirectory; returns the length, 0 on overflow.
static inline size_t n48g_dumpsub_path(char *out, size_t n, uint32_t uid) {
    const int w = snprintf(out, n, "%s/%u", N48G_DUMP_DIR, (unsigned)uid);
    if (w <= 0 || (size_t)w >= n) { if (n) out[0] = 0; return 0; }
    return (size_t)w;
}

// ---- the bounded synchronous wait on a spvcache miss (bundle build 6, C1) ----
// An ADMITTED APPLICATION (the kernel admitted it; never WindowServer, never a root test switch) that misses the spvcache dumps the AIR and then waits up to N48G_SYNC_WAIT_MS for the translate
// daemon to install the .spv (polling the same lookup directories) before it settles for the hot-swap FALLBACK pipeline. WindowServer keeps today's immediate fallback: its render thread must
// never block. A daemon that is not running must not stall an app for 3 s on EVERY pipeline: after N48G_SYNC_STRIKES consecutive timeouts in a process the wait is switched off for that process
// (any hit re-arms it).
#define N48G_SYNC_WAIT_MS 3000
#define N48G_SYNC_POLL_MS 50
#define N48G_SYNC_STRIKES 3
typedef struct { int strikes; } n48g_sync;
static inline int n48g_syncwait_applies(int isWs, int appAdmitted, int force, int testAsWs) { return appAdmitted && !isWs && !force && !testAsWs; }
static inline int n48g_sync_allowed(const n48g_sync *s) { return s->strikes < N48G_SYNC_STRIKES; }
static inline void n48g_sync_result(n48g_sync *s, int found) { if (found) s->strikes = 0; else if (s->strikes < 1000) s->strikes++; }
// One step of the wait loop: elapsedMs since the wait began, found = the lookup now succeeds. Returns -1 = found (stop, use it), 0 = timed out (stop, fall back), >0 = sleep that many ms and look again.
static inline int n48g_sync_step(int elapsedMs, int found) {
    if (found) return -1;
    if (elapsedMs >= N48G_SYNC_WAIT_MS) return 0;
    const int left = N48G_SYNC_WAIT_MS - elapsedMs;
    return left < N48G_SYNC_POLL_MS ? left : N48G_SYNC_POLL_MS;
}

// ---- the GPU family claim (bundle build 6, C1 finding B) ----
// Metal 3 implies argument buffers / bindless resources / MTLBuffer.gpuAddress; this bundle has none of them (gpuAddress returns 0, "argument buffers / pointers are not implemented"). So Metal3 is NOT claimed.
// Mac2 and Common1..3 stay (what the compositor and the apps' base paths use). argumentBuffersSupport reports tier 1 (MTLArgumentBuffersTier1 == 0), the lowest.
#define N48G_FAMILY_MAC2     2002
#define N48G_FAMILY_COMMON1  3001
#define N48G_FAMILY_COMMON2  3002
#define N48G_FAMILY_COMMON3  3003
#define N48G_FAMILY_METAL3   5001
#define N48G_ARGBUF_TIER1    0
// WindowServer keeps the pre-build-6 answer for Metal3 (YES): the GPU desktop has run with it since #11 and nothing it uses needs argument buffers; changing it there is an untested display-path risk (reviewer. Apps get NO.
static inline int n48g_supports_family(long family, int isWs) {
    switch (family) {
    case N48G_FAMILY_MAC2: case N48G_FAMILY_COMMON1: case N48G_FAMILY_COMMON2: case N48G_FAMILY_COMMON3: return 1;
    case N48G_FAMILY_METAL3: return isWs ? 1 : 0;
    default: return 0;    // Apple1..9, Mac1, MacCatalyst, Metal4, unknown
    }
}

#endif /* N48_GATE_H */
