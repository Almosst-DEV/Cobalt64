#!/bin/zsh
# test-gate-plant.sh - planted breaks for test-gate.c (kext 0.0.640, GPU-apps G4: the bundle's fail-closed gates, the kernel probe, the per-process cache path).
# For each plant: copy the REAL n48_gate.h, Navi48Device.m, test-gate.c and the kernel ABI header into a scratch tree, apply ONE break to the real code (the script first proves the text
# was there exactly once), compile and run the real test and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: exit non-zero.
#   run from anywhere:  tools/native/navi48metal/test-gate-plant.sh
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../../.." && pwd)"
SCR="${TMPDIR:-/tmp}/gate-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/tools/native/navi48metal" "$SCR/src/navi48-bringup/src"
  cp "$D/n48_gate.h" "$D/Navi48Device.m" "$D/test-gate.c" "$D/Info.plist" "$SCR/tools/native/navi48metal/"
  cp "$ROOT/src/navi48-bringup/src/Navi48NativeABI.h" "$SCR/src/navi48-bringup/src/"
}
build_run() {
  OUT=$(cd "$SCR/tools/native/navi48metal" && cc -O1 -Wall -Wextra -Werror -o "$SCR/t" test-gate.c 2>&1) || return 2
  OUT=$(cd "$SCR/tools/native/navi48metal" && "$SCR/t" . 2>&1) && return 0
  return 1
}
plant() {   # plant <id> <file in the metal dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/tools/native/navi48metal/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:70])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  build_run; rc=$?
  if [ $rc -eq 2 ]; then echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$OUT" | /usr/bin/grep -m1 -E 'error' | cut -c1-140)"; return; fi
  if [ $rc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$OUT" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$OUT" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"
}
fresh; total=$((total+1)); build_run; rc=$?
if [ $rc -ne 0 ]; then echo "BASELINE: the unmodified scratch copy does not pass (rc $rc): $(echo "$OUT" | /usr/bin/grep -m1 -E 'error|^FAIL' | cut -c1-140)"; escaped=$((escaped+1)); else echo "BASELINE: the unmodified scratch copy compiles and passes ($(echo "$OUT" | tail -1))"; fi
G=n48_gate.h; M=Navi48Device.m
plant 1  $G "return (err == ENOENT || err == ENOTDIR) ? N48G_KILL_ABSENT : N48G_KILL_UNREADABLE;" "return (err == ENOENT || err == ENOTDIR || err == EPERM) ? N48G_KILL_ABSENT : N48G_KILL_UNREADABLE;" "GATE 1: a sandbox-denied stat (EPERM) reads as 'no kill file' (fails OPEN)"
plant 2  $G "return isWs ? 0 : (state != N48G_KILL_ABSENT);" "(void)isWs; (void)state; return 0;" "GATE 2: an unreadable kill file declines nobody outside WindowServer (fails OPEN)"
plant 3  $G "return isWs ? 0 : (state != N48G_KILL_ABSENT);" "(void)isWs; return state != N48G_KILL_ABSENT;" "GATE 3: an unreadable kill file now declines WindowServer too (today's rule changed)"
plant 4  $G "if (state == N48G_KILL_PRESENT) return !ignored;" "(void)ignored; if (state == N48G_KILL_PRESENT) return 1;" "GATE 4: the root test bypass no longer works"
plant 5  $G "if (state == N48G_KILL_PRESENT) return !ignored;" "(void)ignored; if (state == N48G_KILL_PRESENT) return 0;" "GATE 5: a PRESENT kill file declines nobody"
plant 6  $G "static inline int n48g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }" "static inline int n48g_ready_declines(int isWs, int flag) { (void)isWs; return flag == 0; }" "GATE 6: an absent / unreadable Ready property proceeds outside WindowServer (fails OPEN)"
plant 7  $G "static inline int n48g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }" "static inline int n48g_ready_declines(int isWs, int flag) { (void)isWs; return flag != 1; }" "GATE 7: WindowServer declines when Ready is absent (today's rule changed)"
plant 8  $G "static inline int n48g_autodisarm_declines(int isWs, int flag) { (void)isWs; return flag == 1; }" "static inline int n48g_autodisarm_declines(int isWs, int flag) { (void)isWs; (void)flag; return 0; }" "GATE 8: AutoDisarmed == 1 declines nobody"
plant 9  $G "    if ((infoFlags & N48G_INFO_HUNG) != 0u) return N48G_PROBE_HUNG;" "    (void)infoFlags;" "PROBE 9: a HUNG GPU does not decline the application"
plant 10 $G "    if (infoRc != 0u) return N48G_PROBE_INFO;" "    (void)infoRc;" "PROBE 10: an unreadable QueryInfo does not decline (fails OPEN)"
plant 11 $G "    if (helloRc != 0u) return N48G_PROBE_HELLO;" "    (void)helloRc;" "PROBE 11: a failed Hello does not decline"
plant 12 $G "    if (openRc != 0u) return N48G_PROBE_OPEN;" "    (void)openRc;" "PROBE 12: a refused open does not decline (the kernel's verdict is ignored)"
plant 13 $G "static inline int n48g_probe_retry(uint32_t openRc, int tries) { return openRc == N48G_KR_EXCLUSIVE && tries < N48G_PROBE_RETRIES; }" "static inline int n48g_probe_retry(uint32_t openRc, int tries) { return openRc != 0u && tries < N48G_PROBE_RETRIES; }" "PROBE 13: every refusal is retried (the allow-list refusal too)"
plant 14 $G "static inline int n48g_probe_retry(uint32_t openRc, int tries) { return openRc == N48G_KR_EXCLUSIVE && tries < N48G_PROBE_RETRIES; }" "static inline int n48g_probe_retry(uint32_t openRc, int tries) { (void)tries; return openRc == N48G_KR_EXCLUSIVE; }" "PROBE 14: the retry is unbounded"
plant 15 $G "#define N48G_INFO_FLAGS_OFF   8u" "#define N48G_INFO_FLAGS_OFF   12u" "PROBE 15: the probe reads the flags word at the wrong offset"
plant 16 $G "#define N48G_SEL_QUERYINFO    1u" "#define N48G_SEL_QUERYINFO    2u" "PROBE 16: the probe calls ReadRegs instead of QueryInfo"
plant 17 $G "#define N48G_KR_EXCLUSIVE     0xe00002c5u" "#define N48G_KR_EXCLUSIVE     0xe00002c1u" "PROBE 17: the retried code is not ExclusiveAccess"
plant 18 $G "        if (!userCacheDir || !userCacheDir[0]) return 0;" "        if (!userCacheDir || !userCacheDir[0]) userCacheDir = \"/private/var/tmp/\";" "CACHE 18: an application with no user cache dir falls back to /private/var/tmp (the sticky directory beside WindowServer's file)"
plant 19 $G "c == '.' || c == '-' || c == '_') ? c : '_';" "c == '.' || c == '-' || c == '_' || c == '/') ? c : '_';" "CACHE 19: a '/' in the program name reaches the path"
plant 20 $G "static inline int n48g_pcache_save(int cls, int rootSaveEnv) { return cls == N48G_CLASS_WS ? 1 : cls == N48G_CLASS_ROOT ? rootSaveEnv : 1; }" "static inline int n48g_pcache_save(int cls, int rootSaveEnv) { (void)cls; (void)rootSaveEnv; return 1; }" "CACHE 20: a root tool also saves (its file would block WindowServer's rename)"
plant 21 $G "static inline int n48g_pcache_save(int cls, int rootSaveEnv) { return cls == N48G_CLASS_WS ? 1 : cls == N48G_CLASS_ROOT ? rootSaveEnv : 1; }" "static inline int n48g_pcache_save(int cls, int rootSaveEnv) { return cls == N48G_CLASS_WS ? 1 : rootSaveEnv; }" "CACHE 21: an application never saves its cache"
plant 22 $G "        const char *dir = (envDirAllowed && envDirAllowed[0]) ? envDirAllowed : (varTmpWritable ? \"/private/var/tmp\" : \"/private/tmp\");" "        const char *dir = \"/private/var/tmp\"; (void)envDirAllowed; (void)varTmpWritable;" "CACHE 22: WindowServer's directory choice is hard-coded"
plant 23 $G "    if (cls == N48G_CLASS_OTHER) {
        if (!userCacheDir" "    if (cls == N48G_CLASS_OTHER && 0) {
        if (!userCacheDir" "CACHE 23: an application gets WindowServer's shared cache file"
plant 24 $G "static inline int n48g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) { return isWs || force || testAsWs || appAdmitted; }" "static inline int n48g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) { (void)appAdmitted; return isWs || force || testAsWs; }" "FALLBACK 24: an admitted application still gets the NSError on a spvcache miss"
plant 25 $G "static inline int n48g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) { return isWs || force || testAsWs || appAdmitted; }" "static inline int n48g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) { (void)isWs; (void)force; (void)testAsWs; (void)appAdmitted; return 1; }" "FALLBACK 25: every process gets the fallback"
plant 26 $G "static inline int n48g_class(int isWs, int allowRoot) { return isWs ? N48G_CLASS_WS : allowRoot ? N48G_CLASS_ROOT : N48G_CLASS_OTHER; }" "static inline int n48g_class(int isWs, int allowRoot) { (void)allowRoot; return isWs ? N48G_CLASS_WS : N48G_CLASS_ROOT; }" "CLASS 26: no process is ever an 'other' process (the kernel probe never runs)"
plant 27 $M "        const char *why = n48_app_probe(port);" "        const char *why = NULL;" "ADMIT 27: n48_admit never asks the kernel (any process is admitted)"
plant 28 $M "    if (cls == N48G_CLASS_OTHER) {   // not WindowServer, not an N48M_ALLOW root tool: only the kernel can admit it" "    if (1) {   // not WindowServer, not an N48M_ALLOW root tool: only the kernel can admit it" "ADMIT 28: WindowServer and root tools also go through the probe"
plant 29 $M "    if (n48g_kill_declines(isWs, ks, killIgnored)) {" "    if (0) {" "ADMIT 29: n48_admit never applies the unreadable-kill-file rule"
plant 30 $M "    if (n48g_ready_declines(isWs, rdy)) return" "    if (rdy == 0) return" "ADMIT 30: n48_admit tests Ready with the old bare '== 0'"
plant 31 $M "            IOServiceClose(conn);   // the session is released at once; Mesa opens its own later" "            (void)conn;   // the session is released at once; Mesa opens its own later" "ADMIT 31: the probe connection is never closed (a session slot leaks per process)"
plant 32 $M "n48g_fallback_ok(n48_is_ws(), n48_force_fallback(), n48_test_fb_as_ws(), atomic_load(&n48_app_admitted))" "n48g_fallback_ok(n48_is_ws(), n48_force_fallback(), n48_test_fb_as_ws(), 0)" "ADMIT 32: the fallback rule never sees the admitted flag"
plant 33 $M "        atomic_store(&n48_app_admitted, 1);" "        atomic_store(&n48_app_admitted, 1);
        atomic_store(&n48_app_admitted, 1);" "ADMIT 33: the admitted flag is stored in a second place"
plant 35 $M "    if (!n48g_pcache_path(N48PC.path, sizeof N48PC.path, pcls, envdir, vtw, ucd, getprogname()))" "    if (!n48g_pcache_path(N48PC.path, sizeof N48PC.path, N48G_CLASS_WS, envdir, 1, ucd, getprogname()))" "CACHE 35: n48_pc_open always asks for WindowServer's path"
plant 36 $M "return N48G_FLAG_NONUB;" "return N48G_FLAG_ABSENT;" "NUB 36: an unreachable nub reads as 'property absent'"
plant 37 $M "    if (!v) return N48G_FLAG_ABSENT;" "    if (!v) return 1;" "NUB 37: an absent property reads as 'true'"
plant 38 $G "    if ((budgetFlags & N48G_BUDGET_APP) == 0u) return N48G_PROBE_NOTAPP;" "    (void)budgetFlags;" "PROBE 38: a root process the kernel admits as root is admitted as an application"
plant 39 $G "#define N48G_BUDGET_APP       2u" "#define N48G_BUDGET_APP       1u" "PROBE 39: the APP bit is the wrong bit (the VALID bit)"
plant 40 $G "#define N48G_INFO_BUDGET_OFF  172u" "#define N48G_INFO_BUDGET_OFF  168u" "PROBE 40: the probe reads the budget flags from the wrong word"
plant 41 $M "memcpy(&budgetFlags, info + N48G_INFO_BUDGET_OFF, sizeof budgetFlags);" "budgetFlags = 2u;" "PROBE 41: the bundle fakes the APP role instead of reading it"
# ---- 0.0.641 (G4 review fixes): the euid >= 501 probe scope, the dump directory, the bundle build ----
plant 42 $G "static inline int n48g_declined_by_class(int cls, uint32_t euid) { return cls == N48G_CLASS_OTHER && euid < N48G_APP_MIN_EUID; }" "static inline int n48g_declined_by_class(int cls, uint32_t euid) { (void)cls; (void)euid; return 0; }" "SCOPE 42: root and daemons are probed (the review's HIGH 2)"
plant 43 $G "#define N48G_APP_MIN_EUID 501u" "#define N48G_APP_MIN_EUID 500u" "SCOPE 43: the first probed euid is 500"
plant 44 $G "return cls == N48G_CLASS_OTHER && euid < N48G_APP_MIN_EUID; }" "return euid < N48G_APP_MIN_EUID; (void)cls; }" "SCOPE 44: WindowServer (and N48M_ALLOW root tools) are declined by class too"
plant 45 $M "    if (n48g_declined_by_class(cls, (uint32_t)geteuid())) return N48G_DECLINE_BY_CLASS_TEXT;
" "" "SCOPE 45: n48_admit never declines by class (every process reaches the kernel probe)"
plant 46 $M "if (n48g_declined_by_class(cls, (uint32_t)geteuid()))" "if (n48g_declined_by_class(cls, (uint32_t)getuid()))" "SCOPE 46: the decline uses the REAL uid, not the effective one (a setuid-root process is probed)"
plant 47 $M "if (n48g_declined_by_class(cls, (uint32_t)geteuid())) return N48G_DECLINE_BY_CLASS_TEXT;" "if (n48g_declined_by_class(cls, (uint32_t)geteuid())) {}" "SCOPE 47: the by-class decline is evaluated but not acted on"
plant 48 $G "#define N48G_DECLINE_BY_CLASS_TEXT \"process is not WindowServer and N48M_ALLOW=1 (as root) is not set\"" "#define N48G_DECLINE_BY_CLASS_TEXT \"declined\"" "SCOPE 48: the by-class reason is not 0.0.632's"
plant 49 $G "return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }" "(void)ownerUid; (void)callerUid; return lstatRc == 0 && isDir && !isLnk; }" "DUMP 49: a directory owned by someone else is used"
plant 50 $G "return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }" "(void)isLnk; return lstatRc == 0 && isDir && ownerUid == callerUid; }" "DUMP 50: a symlink is used"
plant 51 $G "return (mode & 0077u) != 0u; }" "return (mode & 0007u) != 0u; }" "DUMP 51: group-writable directories of ours are not narrowed"
plant 56 $M "    if (!n48g_dumpdir_ok(rc, " "    if (0 && !n48g_dumpdir_ok(rc, " "DUMP 56: the helper never applies the verdict"
# ---- bundle build 6 (C1): per-uid dump subdirectories, the bounded synchronous wait, the family claim ----
plant 52 $M "(void)mkdir(N48G_DUMP_DIR, N48G_DUMP_ROOT_MODE);" "(void)mkdir(N48G_DUMP_DIR, 0777);" "DUMP 52: the parent is created 0777 without the sticky bit"
plant 53 $M "const int rc = lstat(sub, &st);" "const int rc = stat(sub, &st);" "DUMP 53: the subdirectory is checked with stat (follows a symlink)"
plant 54 $M "(void)fchmodat(AT_FDCWD, N48G_DUMP_DIR, N48G_DUMP_ROOT_MODE, AT_SYMLINK_NOFOLLOW);" "(void)chmod(N48G_DUMP_DIR, N48G_DUMP_ROOT_MODE);" "DUMP 54: the parent repair follows a symlink (plain chmod)"
plant 55 $M "if (!n48_dump_dir_ready(sub, sizeof sub)) {" "if (0) {" "DUMP 55: the dump never checks its directory"
plant 57 $M "n48_dump_write(side.UTF8String, js) && n48_dump_write(air.UTF8String, bc);" "n48_dump_write(air.UTF8String, bc) && n48_dump_write(side.UTF8String, js);" "DUMP 57: the .air is written before its sidecar (the daemon can start without the sidecar)"
plant 59 $G "(ownerUid == 0u || ownerUid == callerUid || ownerUid == N48G_DAEMON_UID)" "(ownerUid == 0u || ownerUid == callerUid || ownerUid == N48G_DAEMON_UID || 1)" "ROOT 59: a parent owned by a THIRD user is accepted"
plant 60 $G "(mode & 07777u) == N48G_DUMP_ROOT_MODE;" "(mode & 01000u) != 0u;" "ROOT 60: a sticky parent with any other bits (0700, 4777) is accepted"
plant 61 $G "return ownerUid == callerUid && (mode & 07777u) != N48G_DUMP_ROOT_MODE; }" "return (mode & 07777u) != N48G_DUMP_ROOT_MODE; (void)ownerUid; (void)callerUid; }" "ROOT 61: a parent owned by someone else is also 'repaired' (chmod on another user's directory)"
plant 62 $G "snprintf(out, n, \"%s/%u\", N48G_DUMP_DIR, (unsigned)uid)" "snprintf(out, n, \"%s/x%u\", N48G_DUMP_DIR, (unsigned)uid)" "SUB 62: the per-uid subdirectory name is not the uid"
plant 63 $G "return appAdmitted && !isWs && !force && !testAsWs; }" "return appAdmitted && !force && !testAsWs; (void)isWs; }" "WAIT 63: WindowServer waits too (its immediate fallback is gone)"
plant 64 $G "#define N48G_SYNC_WAIT_MS 3000" "#define N48G_SYNC_WAIT_MS 30000" "WAIT 64: the wait budget is 30 s"
plant 65 $G "if (found) s->strikes = 0; else if (s->strikes < 1000) s->strikes++; }" "if (found) {} else if (s->strikes < 1000) s->strikes++; }" "WAIT 65: a hit does not re-arm the wait"
plant 66 $G "#define N48G_SYNC_STRIKES 3" "#define N48G_SYNC_STRIKES 1000" "WAIT 66: a dead daemon never switches the wait off"
plant 67 $G "return left < N48G_SYNC_POLL_MS ? left : N48G_SYNC_POLL_MS;" "return left < 0 ? left : N48G_SYNC_POLL_MS;" "WAIT 67: the last sleep overshoots the deadline"
plant 68 $G "case N48G_FAMILY_MAC2: case N48G_FAMILY_COMMON1:" "case N48G_FAMILY_METAL3: case N48G_FAMILY_MAC2: case N48G_FAMILY_COMMON1:" "FAMILY 68: Metal3 is claimed again"
plant 78 $G "case N48G_FAMILY_METAL3: return isWs ? 1 : 0;" "case N48G_FAMILY_METAL3: return 0;" "FAMILY 78: WindowServer loses its Metal3 answer"
plant 79 $M "    if (n48_is_ws()) {   // the base class" "    if (0) {   // the base class" "FAMILY 79: WindowServer gets Tier1 instead of the base answer"
plant 69 $M "return (NSUInteger)N48G_ARGBUF_TIER1; }" "return 1; }" "FAMILY 69: argument buffers report tier 2"
plant 70 $M "if (n48_sync_wait(\"render\", want, NO)) {" "if (0) {" "WAIT 70: the render miss never waits"
plant 71 $M "            if (!inproc && n48g_syncwait_applies(n48_is_ws(), atomic_load(&n48_app_admitted), n48_force_fallback(), n48_test_fb_as_ws())) {" "            if (!inproc) {" "WAIT 71: the render wait is no longer gated (WindowServer would wait)"
plant 72 $M "fchmod(fd, 0644)" "fchmod(fd, 0666)" "DUMP 72: dump files are world-writable"
plant 73 $M "O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600" "O_WRONLY | O_CREAT | O_TRUNC, 0600" "DUMP 73: the temp file follows a planted symlink"
plant 74 $M "fs.st_ino != st.st_ino ||" "0 ||" "DUMP 74: the open descriptor is not tied to the lstat'ed directory"
plant 75 $M "n48da_grant_list(fd, (uid_t)N48G_DAEMON_UID)" "n48da_grant_list(fd, (uid_t)0)" "DUMP 75: the ACL grant goes to the wrong account"
plant 76 $M "if (ksha && n48_sync_wait(\"kernel\", @[ ksha ], YES)) {" "if (ksha && n48_sync_wait(\"kernel\", @[ ksha ], NO)) {" "WAIT 76: the compute wait does not require the .meta.json (kernel_dispatch lives there)"
plant 77 $M "NSString *key = [shas componentsJoinedByString:@\"+\"];" "NSString *key = @\"k\";" "WAIT 77: the timed-out set is keyed by nothing (one timeout silences every pipeline)"
plant 58 Info.plist "<key>CFBundleVersion</key>
	<string>20</string>" "<key>CFBundleVersion</key>
	<string>2</string>" "VERSION 58: the bundle build is not bumped"
# ---- bundle 8 (G6): sandboxed applications open N48N on the accelerator port ----
plant 90 $M "    io_service_t svc = (io_service_t)port;   // G6: the accelerator port (0 = none); never released here" "    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(\"Navi48Bringup\"));" "G6 90: the APP probe goes back to a Navi48Bringup lookup by name (a sandbox denies the open)"
plant 91 $G "return cls == N48G_CLASS_OTHER ? N48G_SVC_ACCEL : (const char *)0;" "(void)cls; return N48G_SVC_ACCEL;" "G6 91: WindowServer and root tools are routed to the accelerator too"
plant 92 $G "return cls == N48G_CLASS_OTHER ? N48G_SVC_ACCEL : (const char *)0;" "(void)cls; return (const char *)0;" "G6 92: the application class keeps Mesa's default service (Navi48Bringup: denied by the sandbox)"
plant 93 $M "if (!setSvc) { N48R.err = n48_err(7, @\"no \" N48G_MESA_SETTER \" in RADV (an application needs the accelerator route)\"); goto fail; }" "if (!setSvc) { }" "G6 93: a RADV without the route setter is used anyway (fails OPEN onto the wrong service)"
plant 94 $M "            if (svcCls) {" "            if (0) {" "G6 94: the route setter is never called"
plant 95 $M "        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, N48G_HELLO_F_MINOR }" "        IOObjectRelease(svc);
        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, N48G_HELLO_F_MINOR }" "G6 95: the probe releases the port Metal gave the bundle (a reference it does not own)"
plant 96 $G "#define N48G_MESA_SETTER      \"radv_darwin_set_service_class\"" "#define N48G_MESA_SETTER      \"radv_darwin_set_service\"" "G6 96: the bundle looks up a setter name Mesa does not export"
plant 97 $M "const int src = setSvc(svcCls);
                if (src != 0) {" "const int src = setSvc(svcCls); (void)src;
                if (0) {" "G6 97: a refused service name is ignored"
echo "test-gate-plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
