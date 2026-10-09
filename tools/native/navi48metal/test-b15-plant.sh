#!/bin/zsh
# test-b15-plant.sh - planted breaks for bundle build 15 (M6 Stage 2: the monitor A as the third GPU-composited display, instance 1; the per-instance state array [DP, monitor A, monitor B]): n48_m6x.h / test-m6x.c, n48_m6route.h (version-2 blob generation), n48_dispflip.h (R6), the glue in Navi48Device.m, n48_scanabi.h and the
# kernel headers they mirror. Same discipline as test-b11-plant.sh: copy the REAL files into a scratch tree that keeps the repo's relative layout, apply ONE break (the script proves the text was there exactly once), build and run test-m6x,
# test-m6route and test-dispflip and demand that at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  tools/native/navi48metal/test-b15-plant.sh        (TMPDIR selects where the scratch tree goes)
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../../.." && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b15-plant.XXXXXX")"
case "$SCR" in */b15-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/tools/native/navi48metal"
fresh() {
  rm -rf -- "$SCR/tools" "$SCR/src"; mkdir -p "$W" "$SCR/src/navi48-bringup/src/amd"
  cp "$D"/n48_m6route.h "$D"/n48_m6x.h "$D"/n48_scanabi.h "$D"/Navi48Device.m "$D"/INSTALL.md "$D"/Info.plist "$D"/test-m6x.c "$D"/test-m6route.c "$D"/n48_dispflip.h "$D"/n48_impcache.h "$D"/test-dispflip.c "$W/"
  cp "$ROOT/src/navi48-bringup/src/amd/native_m6_pure.h" "$SCR/src/navi48-bringup/src/amd/"; cp "$ROOT/src/navi48-bringup/src/Navi48NativeABI.h" "$SCR/src/navi48-bringup/src/"
}
run_test() {   # 0 = all passed; prints the first failure into OUT
  OUT=""
  ( cd "$W" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-m6x test-m6x.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-m6x: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-m6x . ) >"$SCR/o0" 2>&1 || { OUT="test-m6x: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
  ( cd "$W" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-m6route test-m6route.c ) >"$SCR/cc1.out" 2>&1 || { OUT="test-m6route: compile error: $(/usr/bin/grep -m1 error "$SCR/cc1.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-m6route . ) >"$SCR/o1" 2>&1 || { OUT="test-m6route: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o1" | cut -c1-110)"; return 1; }
  ( cd "$W" && cc -O1 -Wall -Wextra -Werror -o t-dispflip test-dispflip.c ) >"$SCR/cc2.out" 2>&1 || { OUT="test-dispflip: compile error: $(/usr/bin/grep -m1 error "$SCR/cc2.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-dispflip ) >"$SCR/o2" 2>&1 || { OUT="test-dispflip: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o2" | cut -c1-110)"; return 1; }
  return 0
}
plant() {   # plant <id> <file relative to the scratch root> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if run_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
fresh; total=$((total+1)); if run_test; then echo "BASELINE: the unmodified scratch copy passes"; else echo "BASELINE: FAILED: $OUT"; escaped=$((escaped+1)); fi
X=tools/native/navi48metal/n48_m6x.h; M=tools/native/navi48metal/Navi48Device.m; A=tools/native/navi48metal/n48_scanabi.h
# ---- THE INSTANCE-SWAP PLANTS (bundle side) and the per-instance rules
plant 1  $X "    if (e->inst == N48M6_INST_MONA && n48x_on(xon_mask, N48X_PLAN_MONA)) return N48X_PLAN_MONA;
    if (e->inst == N48M6_INST_MONB && n48x_on(xon_mask, N48X_PLAN_MONB)) return N48X_PLAN_MONB;" "    if (e->inst == N48M6_INST_MONB && n48x_on(xon_mask, N48X_PLAN_MONA)) return N48X_PLAN_MONA;
    if (e->inst == N48M6_INST_MONA && n48x_on(xon_mask, N48X_PLAN_MONB)) return N48X_PLAN_MONB;" "S6: the table's MONA and MONB are swapped in the plan (N48M6_INST_MONA <-> MONB)"
plant 2  $X "    { N48X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  N48X_NOFLIP1_FILE, \"monitor A\" }," "    { N48X_INST_MONA, 0x10u, 2560u, 1440u, 10240u, 14745600ull, N48X_NOFLIP1_FILE, \"monitor A\" }," "S8: the monitor A's descriptor carries the DP's / monitor B's rectangle and slot size"
plant 3  $M "    VkDeviceSize all = xd ? (VkDeviceSize)n48x_frame_bytes(xd) : (VkDeviceSize)N48S.pitch * N48S.ph;" "    VkDeviceSize all = (VkDeviceSize)N48S.pitch * N48S.ph;" "S8: the monitor A's copy uses the DP's rectangle (a 14.7 MB copy into an 8.3 MB slot)"
plant 4  $M "    const uint32_t gw = xd ? xd->w : N48S.pw, gh = xd ? xd->h : N48S.ph, gp = xd ? xd->pitch_bytes : N48S.pitch;" "    const uint32_t gw = N48S.pw, gh = N48S.ph, gp = N48S.pitch;" "S8: the image copy uses the DP's geometry for an HDMI instance"
plant 5  $X "    { N48X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  N48X_NOFLIP1_FILE, \"monitor A\" },
    { N48X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, N48X_NOFLIP2_FILE, \"monitor B\" } };" "    { N48X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  N48X_NOFLIP2_FILE, \"monitor A\" },
    { N48X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, N48X_NOFLIP1_FILE, \"monitor B\" } };" "S9: the kill-file names are swapped"
plant 6  $X "    { N48X_INST_MONA, 0x10u, 1920u," "    { N48X_INST_MONA, 0x20u, 1920u," "S3: the monitor A's slot tag is the monitor B's"
plant 7  $X "static inline const n48x_desc_t *n48x_desc(uint32_t inst) { return (inst == N48X_INST_MONA || inst == N48X_INST_MONB) ? &n48x_descs[inst] : NULL; }" "static inline const n48x_desc_t *n48x_desc(uint32_t inst) { return (inst == N48X_INST_MONA || inst == N48X_INST_MONB) ? &n48x_descs[3u - inst] : NULL; }" "S1: the descriptor lookup returns the OTHER row"
plant 8  $X "    return n48x_surface_geom_ok((uint32_t)plan_inst, w, h, bpr);" "    (void)w; (void)h; (void)bpr; (void)dp_pitch; return 1;" "the size gate lets any HDMI-planned surface through (inst_geom_mismatch never fires)"
plant 9  $M "!n48x_plan_geom_ok(pi, gw, gh, gb, N48S.pw, N48S.ph, N48S.pitch)) { N48XI(pi).xs.inst_geom_mismatch++;" "0) { N48XI(pi).xs.inst_geom_mismatch++;" "the glue never applies the size gate"
plant 10 $M "if (!have || !n48x_plan_geom_ok(pi, gw, gh, gb, N48S.pw, N48S.ph, N48S.pitch))" "if (!n48x_plan_geom_ok(pi, gw, gh, gb, N48S.pw, N48S.ph, N48S.pitch))" "an HDMI-planned surface with no recorded geometry is let through"
plant 11 $X "static inline int n48x_enabled(const n48x_enable_t *e) { return n48x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != N48X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "static inline int n48x_enabled(const n48x_enable_t *e) { return n48x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && e->have_call && e->have_handle && !e->killfile; }" "the monitor A is enabled without the kernel's navi48-m6flip1 latch"
plant 12 $M ".kernel_m6flip1 = (q.flags & N48X_SCANQ_M6FLIP1) != 0" ".kernel_m6flip1 = (q.flags & N48X_SCANQ_M6FLIP) != 0" "the glue reads the m6flip bit for the third latch"
plant 13 $X "static inline int n48x_acq_geom_ok(const n48x_desc_t *d, uint32_t nout, uint64_t word) {
    if (!d) return 0;" "static inline int n48x_acq_geom_ok(const n48x_desc_t *d, uint32_t nout, uint64_t word) {
    (void)nout; (void)word; return d != 0;" "the Acquire's geometry word is never cross-checked"
plant 14 $X "    if (nout < 5u) return d->inst == N48X_INST_MONB;" "    if (nout < 5u) return 1;" "the monitor A accepts a four-word Acquire answer (no geometry)"
plant 15 $X "(word >> 32) * 4u == d->pitch_bytes;" "(word >> 32) * 4u == 10240u;" "the pitch of the geometry word is the monitor B's for both"
plant 16 $X "    if (o->geom_verdict(o->ctx) != 0) { o->off(o->ctx, N48DF_R_GEOM, \"the kernel's plane geometry for this instance is not the descriptor's\"); return N48DF_R_GEOM; }
" "" "the acquire driver ignores the geometry verdict"
plant 17 $M "    const int ok = n48x_acq_geom_ok(d, a->nout, a->ao[4]);" "    const int ok = 1;" "the glue's geometry operation always says yes"
plant 18 $M "    if (n48x_acquire_retry4(inst, rc)) { no = 4;" "    if (rc) { no = 4;" "the monitor A may fall back to a four-word Acquire"
plant 19 $X "static inline int n48x_id_ok(uint32_t inst, uint32_t id) { const n48x_desc_t *d = n48x_desc(inst); return d && id >= d->tag && id < d->tag + 3u; }" "static inline int n48x_id_ok(uint32_t inst, uint32_t id) { (void)inst; return id >= 0x10u && id < 0x23u; }" "a slot id of the other instance is accepted at registration"
plant 20 $X "    for (uint32_t i = 1; i <= 2; i++) if (((xon_mask >> i) & 1u) && n48x_surface_geom_ok(i, w, h, bpr)) m |= 1u << i;" "    for (uint32_t i = 2; i <= 2; i++) if (((xon_mask >> i) & 1u) && n48x_surface_geom_ok(i, w, h, bpr)) m |= 1u << i;" "the classifier never accepts an monitor A-sized surface (the monitor A would never be planned)"
plant 21 $M "    for (int xi = 1; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties" "    for (int xi = 2; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties" "the 1 s tick keeps only the monitor B alive (the monitor A would be reverted after 5 s)"
plant 22 $M "n48x_release_all_locked(void) { for (int i = 1; i <= 2; i++) n48x_do_release_locked(i); }" "n48x_release_all_locked(void) { for (int i = 2; i <= 2; i++) n48x_do_release_locked(i); }" "the DP's release forgets the monitor A's A"
plant 23 $M "    for (int xi = 1; xi <= 2; xi++) if (N48XI(xi).sm.acquired) { N48XI(xi).sm.state = N48DF_OFF; n48x_do_release_locked(xi); }" "    for (int xi = 2; xi <= 2; xi++) if (N48XI(xi).sm.acquired) { N48XI(xi).sm.state = N48DF_OFF; n48x_do_release_locked(xi); }" "the explicit release request forgets the monitor A"
plant 24 $M "static uint32_t n48x_onmask_locked(void) { uint32_t m = 0; for (int i = 1; i <= 2; i++)" "static uint32_t n48x_onmask_locked(void) { uint32_t m = 0; for (int i = 2; i <= 2; i++)" "the enable mask never names the monitor A"
plant 25 $M ".size = d->slot_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT" ".size = N48S.slotBytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT" "the HDMI slots are allocated with the DP's slot size"
plant 26 $M ".slot_bytes = d->slot_bytes, .slots = N48DF_MAX_SLOTS" ".slot_bytes = N48S.slotBytes, .slots = N48DF_MAX_SLOTS" "the pool budget uses the DP's slot size"
plant 27 $M "n48x_k_register(a->inst, X->mem[i], d->pitch_bytes, d->w, d->h, &sl, &mc)" "n48x_k_register(a->inst, X->mem[i], N48S.pitch, N48S.pw, N48S.ph, &sl, &mc)" "the monitor A's slots are registered with the DP's rectangle"
plant 28 $M "const uint64_t in[3] = { (uint64_t)inst, slot, 0 };" "const uint64_t in[3] = { 2u, slot, 0 };" "every present goes to instance 2"
plant 29 $M "N48X.call(N48R.dev, N48N_SEL_SCANX_STATUS, in, 1, NULL, 0, o, &no, st, &sz);" "N48X.call(N48R.dev, N48N_SEL_SCANX_STATUS, (const uint64_t[1]){ 2u }, 1, NULL, 0, o, &no, st, &sz);" "every status call asks instance 2 (the monitor A's keep-alive would be the monitor B's)"
plant 30 $M "stat(d->killfile, &kb) == 0" "stat(N48X_NOFLIP2_FILE, &kb) == 0" "both instances poll the monitor B's kill file"
plant 31 $M "fl[i] = X->st.slot[X->sid[i] - d->tag].flags;
            if (N48S.m6.latch" "fl[i] = X->st.slot[X->sid[i] - 0x20u].flags;
            if (N48S.m6.latch" "the plan indexes the kernel's slots by the monitor B's tag for both"
plant 32 $M "            const n48df_rect_t r = { 0, 0, (int32_t)d->w, (int32_t)d->h };" "            const n48df_rect_t r = { 0, 0, (int32_t)N48S.pw, (int32_t)N48S.ph };" "the HDMI plan's rectangle is the DP's"
plant 33 $M "IOSurfaceGetWidth(ios) == d->w && IOSurfaceGetHeight(ios) == d->h && IOSurfaceGetBytesPerRow(ios) == d->pitch_bytes" "IOSurfaceGetWidth(ios) == N48S.pw && IOSurfaceGetHeight(ios) == N48S.ph && IOSurfaceGetBytesPerRow(ios) == N48S.pitch" "the re-copy checks the DP's geometry"
plant 34 $M "if (N48XI(xi).sm.state == N48DF_ACTIVE && n48m6_table_stale(&N48S.m6, N48XI(xi).gen_known ? N48XI(xi).gen : 0u)) stale = 1;" "if (xi == 2 && N48XI(xi).sm.state == N48DF_ACTIVE && n48m6_table_stale(&N48S.m6, N48XI(xi).gen_known ? N48XI(xi).gen : 0u)) stale = 1;" "a stale monitor A table is not noticed at the completion"
plant 35 $M "        n48df_t *const sm = xinst ? &N48XI(inst).sm : &N48S.sm;" "        n48df_t *const sm = inst == 2 ? &N48XI(inst).sm : &N48S.sm;" "an monitor A frame completes on the DP's machine"
plant 37 $M "n48df_t *const dsm = n48x_desc((uint32_t)_dinst) ? &N48XI(_dinst).sm : &N48S.sm;" "n48df_t *const dsm = _dinst == 2 ? &N48XI(_dinst).sm : &N48S.sm;" "an monitor A submit is bookkept on the DP's machine"
plant 38 $A "#define N48N_SCANQ_M6FLIP1    (1u << 7)" "#define N48N_SCANQ_M6FLIP1    (1u << 6)" "the bundle's third-latch bit is the m6flip bit"
plant 39 $M "N48XI(pi).plan_count++;" "" "plan counters are not kept"
plant 40 tools/native/navi48metal/Info.plist "<string>20</string>" "<string>14</string>" "the bundle still says build 14"
plant 41 $M "n48x_classify_mask(N48S.pw, N48S.ph, N48S.pitch, n48x_onmask_locked(), (uint32_t)w, (uint32_t)h, (uint32_t)bpr)" "((w == N48S.pw && h == N48S.ph && bpr == N48S.pitch) ? 1u : 0u)" "the classifier accepts only the DP's plane size"
plant 42 $M "N48S.sg[k].sid = sid; N48S.sg[k].w = (uint32_t)w;" "N48S.sg[k].sid = sid; N48S.sg[k].w = (uint32_t)0;" "the classifier records no geometry"
plant 43 $X "    if (slot_inst == N48X_PLAN_DP && (e->inst == N48M6_INST_MONA || e->inst == N48M6_INST_MONB) && n48x_on(xon_mask, (int)e->inst)) return N48X_C_RECOPY;" "    if (slot_inst == N48X_PLAN_DP && e->inst == N48M6_INST_MONB && n48x_on(xon_mask, (int)e->inst)) return N48X_C_RECOPY;" "a tentative frame that turns out to be the monitor A's is not remembered for the re-copy"
# ---- (bundle 17: the guard is the merged test-b17-diffguard.sh in merge mode; the old test-b15-diffguard.sh compares against bundle 14's base and is red on a merged tree)
# ---- the diff guard must be GREEN on the real file and FIRE on a change of the in-process translation (it runs against a modified copy of Navi48Device.m)
total=$((total+1)); if "$D/test-b17-diffguard.sh" merge >"$SCR/g0.out" 2>&1; then echo "GUARD 0: the merged diff guard is green on the real tree"; else echo "GUARD 0: *** the merged diff guard is RED on the real tree ***"; head -5 "$SCR/g0.out"; escaped=$((escaped+1)); fi
guard() {   # guard <id> <old> <new> <description>
  local id="$1" old="$2" new="$3" desc="$4"; total=$((total+1))
  local g="$SCR/guard.m"; cp "$D/Navi48Device.m" "$g"
  OLD="$old" NEW="$new" FILE="$g" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1: print("GUARD TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  if [ $? -ne 0 ]; then echo "GUARD $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if N48_DG_NEW="$g" "$D/test-b17-diffguard.sh" merge >"$SCR/g.out" 2>&1; then echo "GUARD $id: $desc: *** ESCAPED (the diff guard stayed green) ***"; escaped=$((escaped+1)); else echo "GUARD $id: $desc: CAUGHT"; fi
}
guard 1 "static BOOL n48x_active(void) {" "static BOOL n48x_active(void) { (void)0;" "a line of an in-process function is changed"
guard 2 "const BOOL inproc = (e.code == 41 && n48x_active()) ? YES : NO;" "const BOOL inproc = (e.code == 41) ? YES : NO;" "a call site of the in-process translation is changed"
guard 3 "static void n48x_gate_read(void) {" "static void n48x_gate_read(void) {  /* touched */" "n48x_gate_read is touched"
echo "test-b15-plant: $total run (baseline included), $escaped escaped or did not apply"
exit $((escaped != 0))
