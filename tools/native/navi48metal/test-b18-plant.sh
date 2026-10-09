#!/bin/zsh
# test-b18-plant.sh - planted breaks for bundle build 18 (app-fix round 2, an internal design note): P2 (occlusion queries), P3 (linked functions in the in-process translation), the selector census and its vetted list,
# the diff guard (test-b17-diffguard.sh apps), and the fork's linked entry point. Same discipline as test-b16-plant.sh: copy the REAL files into a scratch tree, apply ONE break (the script proves the text was there exactly once),
# run the suites and demand that at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   usage: test-b18-plant.sh           env: TMPDIR (where the scratch tree goes), N48_M2V_DIR (the fork, default ~/navi48-native/metal2vulkan; its lib.rs is backed up and RESTORED around every fork plant)
#   Fast mode: C compiles at -O0, no sanitizers; the fork's tests run in release mode (the dependencies are already built).
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b18-plant.XXXXXX")"
case "$SCR" in */b18-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
M2V="${N48_M2V_DIR:-$HOME/navi48-native/metal2vulkan}"; FLIB="$M2V/n48xlate/src/lib.rs"; FBAK="$SCR/lib.rs.orig"
[ -f "$FLIB" ] && cp "$FLIB" "$FBAK"
restore_fork() { [ -f "$FBAK" ] && [ -f "$FLIB" ] && cmp -s "$FBAK" "$FLIB" || { [ -f "$FBAK" ] && cp "$FBAK" "$FLIB"; }; }
trap 'restore_fork; rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/c"
cfresh() { rm -rf -- "$W"; mkdir -p "$W/autotranslate"; cp "$D"/n48_xlate.h "$D"/n48_gate.h "$D"/n48_occ.h "$D"/n48_census.h "$D"/Navi48Device.m "$D"/test-occ.c "$D"/test-xlate-linked.c "$D"/test-xlate.c "$D"/test-census.m "$W/"; cp "$D/../autotranslate/overrides.txt" "$W/autotranslate/"; }
c_test() {
  OUT=""
  local t
  for t in occ xlate-linked; do
    ( cd "$W" && cc -O0 -Wall -Wextra -Werror -o t-$t test-$t.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-$t: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
    ( cd "$W" && TMPDIR="$SCR" ./t-$t . ) >"$SCR/o0" 2>&1 || { OUT="test-$t: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "${OUT#test-$t: }" ] || OUT="test-$t died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
  done
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror -DN48X_TESTHOOKS -o t-xlate test-xlate.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-xlate: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && TMPDIR="$SCR" ./t-xlate . autotranslate/overrides.txt ) >"$SCR/o0" 2>&1 || { OUT="test-xlate: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "${OUT#test-xlate: }" ] || OUT="test-xlate died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
  ( cd "$W" && clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -o t-census test-census.m ) >"$SCR/cc.out" 2>&1 || { OUT="test-census: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-census . ) >"$SCR/o0" 2>&1 || { OUT="test-census: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "${OUT#test-census: }" ] || OUT="test-census died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
  return 0
}
apply() {   # apply <file> <old> <new>  -> exit 3 if the text is not there exactly once
  OLD="$2" NEW="$3" FILE="$1" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
}
cplant() {   # cplant <id> <file in the scratch dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  cfresh; apply "$W/$file" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if c_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
fplant() {   # fplant <id> <old> <new> <description>: a break in the FORK's n48xlate/src/lib.rs; its cargo tests must fail; lib.rs is restored afterwards
  local id="$1" old="$2" new="$3" desc="$4"
  restore_fork; apply "$FLIB" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then restore_fork; echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  local res; res="$( cd "$M2V" && PATH="/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH" cargo test --release -p n48xlate --test linked 2>&1 )"
  restore_fork
  if echo "$res" | /usr/bin/grep -q "^test result: ok"; then echo "PLANT $id: $desc: *** ESCAPED (the fork's tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by the fork's linked tests: $(echo "$res" | /usr/bin/grep -m1 -E '^test .* FAILED|^error' | cut -c1-100)"; fi
}
gplant() {   # gplant <id> <old> <new> <description>: a break in Navi48Device.m that the DIFF GUARD (apps mode against 8d8785c0) must refuse
  local id="$1" old="$2" new="$3" desc="$4"
  cfresh; apply "$W/Navi48Device.m" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if ( cd "$D" && N48_DG_NEW="$W/Navi48Device.m" ./test-b17-diffguard.sh apps >"$SCR/g.out" 2>&1 ); then echo "PLANT $id: $desc: *** ESCAPED (the guard passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by the diff guard: $(/usr/bin/grep -m1 -E 'CHANGED:|hunk inside|SHARED function changed|differs from the base|lacks|apps-owned' "$SCR/g.out" | cut -c1-110)"; fi
}
cfresh; total=$((total+1)); if c_test; then echo "BASELINE (C): the unmodified scratch copy passes"; else echo "BASELINE (C): FAILED: $OUT"; escaped=$((escaped+1)); fi
total=$((total+1)); if ( cd "$D" && ./test-b17-diffguard.sh apps >"$SCR/g.out" 2>&1 ); then echo "BASELINE (guard): the real tree passes the apps guard"; else echo "BASELINE (guard): FAILED"; tail -3 "$SCR/g.out"; escaped=$((escaped+1)); fi
M=Navi48Device.m; O=n48_occ.h; X=n48_xlate.h; C=n48_census.h
# ---- P2: occlusion queries
cplant 1  $M "    if (_occ && n48occ_draw(_occ, &q, &precise)) vkCmdBeginQuery([_cb vk], _qp, _qbase + q, (precise && N48R.occPrecise) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);" "    if (_occ && n48occ_draw(_occ, &q, &precise)) { (void)q; }" "no query is ever begun (the draws are never counted)"
cplant 2  $M "    [self n48OccEndQuery];
    vkCmdEndRenderPass([_cb vk]); _inPass = NO;" "    vkCmdEndRenderPass([_cb vk]); _inPass = NO;" "the open query is not ended before the render pass ends (a pass split / the pass end leaves it open)"
cplant 3  $M "        if (_inPass) { [self n48EndPass]; N48LOGR(\"render pass split: framebuffer fetch" "        if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; N48LOGR(\"render pass split: framebuffer fetch" "one pass-split site bypasses n48EndPass (the query stays open across the pass end)"
cplant 4  $M "    if (endq) vkCmdEndQuery([_cb vk], _qp, _qbase + _occ->activeQ);" "    (void)endq;" "a mode / offset change does not end the query"
cplant 5  $O "    { uint64_t v = sum != 0 ? 1u : 0u; return o->accumulate ? (old | v) : v; }" "    { uint64_t v = sum; return o->accumulate ? (old | v) : v; }" "Boolean is written as the sample COUNT instead of 1"
cplant 6  $O "    if (!ok || !res) sum = 1;" "    if (!ok || !res) sum = 0;" "the fail-safe writes 0 (hidden) instead of visible"
cplant 7  $O "    if (o->overflow) sum += 1;   // fail-safe: some draws were not measured, assume they were visible" "    (void)0;" "an overflowed pass (more intervals than slots) is not answered visible"
cplant 8  $O "    if (o->u[u].c) return o->accumulate ? old + sum : sum;" "    if (o->u[u].c) return sum;" "Accumulate is treated as Reset for Counting"
cplant 9  $O "    { uint64_t v = sum != 0 ? 1u : 0u; return o->accumulate ? (old | v) : v; }" "    { uint64_t v = sum != 0 ? 1u : 0u; return v; }" "Accumulate is treated as Reset for Boolean"
cplant 10 $O "    else for (uint32_t q = 0; q < o->nq; q++) if (o->ivu[q] == u) sum += res[q];" "    else for (uint32_t q = 0; q < o->nq; q++) if (o->ivu[q] == u) sum = res[q];" "the intervals of one offset are not summed (the last one wins)"
cplant 11 $O "    if ((int)mode == o->mode && (mode == N48OCC_DISABLED || off == o->off)) return bad ? -1 : 0;   // nothing changed: the open interval continues" "    if (0) return bad ? -1 : 0;" "re-selecting the same mode and offset splits the interval"
cplant 12 $O "    *precise = o->mode == N48OCC_COUNTING;" "    *precise = 1;" "the PRECISE flag is asked for even for Boolean"
cplant 13 $O "    int bad = mode > N48OCC_COUNTING || (mode != N48OCC_DISABLED && ((off & 7u) != 0 || off > o->buflen || o->buflen - off < 8));" "    int bad = mode > N48OCC_COUNTING;" "an offset that is not a multiple of 8 or lies past the buffer is accepted"
cplant 14 $O "    if (o->dead || o->mode == N48OCC_DISABLED || o->ui < 0 || o->active) return 0;" "    if (o->mode == N48OCC_DISABLED || o->ui < 0 || o->active) return 0;" "a pass without a query pool still begins queries"
cplant 15 $M "    if (_qp != VK_NULL_HANDLE) vkCmdResetQueryPool([_cb vk], _qp, _qbase, N48OCC_MAXQ);" "    (void)0;" "the query indices are not reset at encoder creation"
cplant 16 $M "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "- (void)n48PoolDone { [self n48UCRelease]; if (_pser) n48_fence_close(_pser); }" "the visibility buffers are never resolved"
cplant 17 $M "VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);" "VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);" "the results are waited for (a lost device would hang the completion thread)"
cplant 18 $M "            if (ok && !n48occ_read(o, raw, res)) ok = NO;   // an interval that is not available answers visible" "            for (uint32_t q = 0; q < o->nq; q++) res[q] = raw[2 * q];" "an unavailable query is trusted at the glue (its result is read as a count)"
cplant 27 $O "    for (uint32_t q = 0; q < o->nq; q++) { if (!raw[2 * q + 1]) return 0; res[q] = raw[2 * q]; }" "    for (uint32_t q = 0; q < o->nq; q++) { res[q] = raw[2 * q]; }" "n48occ_read trusts an unavailable interval"
cplant 19 $M "    if (N48R.ok && N48R.occOK && _fence != VK_NULL_HANDLE) ran = vkGetFenceStatus(N48R.dev, _fence) == VK_SUCCESS;" "    ran = YES;" "a command buffer whose fence did not succeed is trusted"
cplant 20 $M "    if ([d respondsToSelector:@selector(visibilityResultType)]) acc = (long)[(id)d visibilityResultType] == 1;   // MTLVisibilityResultTypeAccumulate" "    acc = 0;" "visibilityResultType is ignored (Accumulate behaves as Reset)"
cplant 21 $M ".occlusionQueryPrecise = pf.occlusionQueryPrecise };" "};" "occlusionQueryPrecise is not enabled on the device"
cplant 22 $M "    if (!vb) return;
    if (![vb isKindOfClass:[N48Buffer class]]" "    if (!vb) return;
    if (0 && ![vb isKindOfClass:[N48Buffer class]]" "a visibility buffer that is not an N48Buffer is dereferenced"
cplant 23 $M "    [self n48OccInit:d];   // build 18 (P2): outside any render pass (the previous encoder has ended), so the query reset is legal" "    (void)d;" "the encoder never sets up its occlusion bookkeeping"
cplant 24 $M "        for (uint32_t u = 0; u < o->nu && base; u++) {" "        for (uint32_t u = 0; u < 0 && base; u++) {" "no offset is written"
cplant 25 $M "    if (*qp == VK_NULL_HANDLE) n48occ_dead(o);" "    (void)0;" "a pool that could not be created is not flagged for the fail-safe"
cplant 26 $M "    [self n48OccDraw];   // build 18 (P2): the first draw after a visibility mode / offset change (or pass split) begins its query interval" "    (void)0;" "n48PrepareDraw never begins a query"
# ---- P3: linked functions
cplant 30 $X "        if (i && !n48x_link_cmp(&v[i], &v[i - 1])) continue;
        CC_SHA256_Update(&c, v[i].sym" "        continue;
        CC_SHA256_Update(&c, v[i].sym" "the cache key ignores the dependencies"
cplant 31 $X "    CC_SHA256_Update(&c, entrySha, 64); CC_SHA256_Update(&c, \"\\n\", 1);" "    CC_SHA256_Update(&c, \"\\n\", 1);" "the cache key ignores the entry"
cplant 32 $X "    qsort(v, n, sizeof *v, n48x_link_cmp);" "    (void)0;" "the dependencies are not sorted (the key depends on the order the client listed them)"
cplant 33 $X "        if (i && !n48x_link_cmp(&v[i], &v[i - 1])) continue;" "        (void)0;" "a function listed twice changes the key"
cplant 34 $M "    { id v = n48x_prop(linked, \"functions\"); if ([v isKindOfClass:[NSArray class]]) [fns addObjectsFromArray:v]; v = n48x_prop(linked, \"privateFunctions\"); if ([v isKindOfClass:[NSArray class]]) [fns addObjectsFromArray:v]; }" "    { id v = n48x_prop(linked, \"functions\"); if ([v isKindOfClass:[NSArray class]]) [fns addObjectsFromArray:v]; }" "privateFunctions are ignored (the dependencies Maps links)"
cplant 35 $M "    id gr = n48x_prop(linked, \"groups\");
    if ([gr isKindOfClass:[NSDictionary class]]) for (id k in [(NSDictionary *)gr allKeys])" "    id gr = nil;
    if ([gr isKindOfClass:[NSDictionary class]]) for (id k in [(NSDictionary *)gr allKeys])" "groups are ignored"
cplant 36 $M "    if (bf || pl) why = @\"binaryFunctions or preloaded libraries are present\";" "    if (0) why = @\"binaryFunctions or preloaded libraries are present\";" "binaryFunctions / preloaded libraries no longer refuse"
cplant 37 $M "    for (NSString *cs in (lk ? @[ lk->lsha, sha ] : @[ sha ])) {" "    for (NSString *cs in @[ sha ]) {" "the lookup never tries the linked key"
cplant 38 $M "    if (lk) sha = lk->lsha;
    NSString *name = [fn respondsToSelector:@selector(name)] ? [fn name] : @\"\";" "    NSString *name = [fn respondsToSelector:@selector(name)] ? [fn name] : @\"\";" "the translation of a linked stage is installed under the entry's own sha"
cplant 39 $M "        rc = ld ? n48x_engine_run_linked(&N48XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, ld, nd, waitMs, err, sizeof err) : N48X_E_IO;" "        rc = ld ? n48x_engine_run(&N48XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, waitMs, err, sizeof err) : N48X_E_IO;" "the dependencies are not sent to the translator"
cplant 40 $X "            if (dp->air_to_text(dp->ctx, ld[i].air, ld[i].airLen, &t, &l, why, sizeof why) != 0 || !t) { rc = N48X_E_READER; goto refused; }" "            t = strdup(\"\"); l = 0; (void)why;" "a dependency is not read by the reader (its text is empty)"
cplant 41 $X "        const int tr = nld ? dp->translate_linked(dp->ctx, text, tl, loc, dl, nld, &spv, &sl, &meta, &ml, why, sizeof why)
                           : dp->translate(" "        const int tr = nld ? dp->translate(dp->ctx, text, tl, loc, &spv, &sl, &meta, &ml, why, sizeof why)
                           : dp->translate(" "a linked job calls the plain translator"
cplant 42 $X "#define N48X_ABI_VERSION   2u" "#define N48X_ABI_VERSION   1u" "the bundle accepts ABI 1 only"
cplant 43 $M "    N48_LK_SCOPE;   // build 18 (P3): the link contexts live for this creation only" "    (void)0;" "the render creation does not clear its link contexts (a stale key would be used for the next pipeline)"
cplant 44 $M "#define N48_LK_SCOPE n48_lkscope lks_ __attribute__((cleanup(n48_lk_scope_end))) = { 0 }" "#define N48_LK_SCOPE n48_lkscope lks_ = { 0 }" "the scope no longer clears anything"
cplant 45 $M "    N48_LK_SCOPE; n48x_linkage_render(_hsDesc);" "    (void)0;" "the hot-swap rebuild of a render pipeline looks under the plain sha"
cplant 46 $M "    N48LinkCtx *lk_ = n48_lk_for(fn); if (lk_) return lk_->lsha;" "    (void)0;" "the hot-swap watcher of a linked stage watches the entry's own sha"
cplant 47 $M "            if (n48x_link_sha(key, esha.UTF8String, deps.count, syms, shas) != 0) why = @\"cannot form the linked cache key\";" "            if (n48x_link_sha(key, esha.UTF8String, 0, syms, shas) != 0) why = @\"cannot form the linked cache key\";" "the key is formed with no dependencies at the glue"
cplant 48 $M "        if (prev) { if ([prev isEqualToString:sha]) continue; if (why) *why = [NSString stringWithFormat:@\"the name '%@' is linked to two different functions\", nm]; return nil; }" "        if (prev) { continue; }" "one name linked to two different bodies is silently merged"
cplant 49 $M "    else if (!n48x_active()) why = @\"this process does not translate in process\";" "    else if (0) why = @\"this process does not translate in process\";" "WindowServer / an unadmitted process resolves linkage in process"
fplant 50 "let (s, _) = sanitize_ll_text_with_datalayout(ll);" "let s = ll.to_string();" "the fork does not sanitise a dependency"
fplant 52 "        need.extend(metal2vulkan::linked_functions::visible_function_reference_symbols(module)?);" "        let _ = module;" "the fork does not follow a dependency's own references"
fplant 53 "        metal2vulkan::specialize_linked_module(&san_entry, stage, &linkage)?" "        san_entry" "the fork never resolves the references"
fplant 54 "pub const ABI_VERSION: u32 = 2;" "pub const ABI_VERSION: u32 = 1;" "the fork reports ABI 1"
# ---- the census and the vetted list
cplant 60 $M "        unsigned total = n48cen_run(pairs);" "        unsigned total = 0; (void)pairs;" "the census is silenced at +load"
cplant 61 $M "    if (self) n48_census_log();   // build 18: once per process that got a device" "    (void)0;" "no process ever logs the census"
cplant 62 $C "        if (!class_respondsToSelector(c, sel)) {" "        if (class_respondsToSelector(c, sel)) {" "the census reports what the class HAS"
cplant 63 $C "    if ([pn isEqualToString:@\"NSObject\"] || [seen containsObject:pn]) return;" "    if ([pn isEqualToString:@\"NSObject\"] || [seen containsObject:pn]) return;
    if (p) return;" "the census ignores every protocol"
cplant 64 $C "    unsigned np = 0; Protocol * __unsafe_unretained *pl = protocol_copyProtocolList(p, &np);
    for (unsigned i = 0; i < np; i++) n48cen_collect(pl[i], seen, out);" "    unsigned np = 0; Protocol * __unsafe_unretained *pl = protocol_copyProtocolList(p, &np); (void)pl;" "the census skips inherited protocols"
cplant 65 $M "- (void)setFragmentVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i { (void)t; (void)i; N48_ONCE(\"setFragmentVisibleFunctionTable:atBufferIndex: ignored (vetted no-op: pipelines that call through a function table are refused at creation)\"); }" "- (void)setFragmentVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i { (void)t; (void)i; }
- (void)setVertexVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i { (void)t; (void)i; }" "a second selector joins the vetted list (and the reason is no longer logged)"
cplant 66 $M "- (void)doesNotRecognizeSelector:(SEL)sel {
    os_log(OS_LOG_DEFAULT, \"Navi48Metal: UNRECOGNIZED selector %{public}s\", sel_getName(sel));" "- (void)forwardInvocation:(NSInvocation *)inv { (void)inv; }
- (void)doesNotRecognizeSelector:(SEL)sel {
    os_log(OS_LOG_DEFAULT, \"Navi48Metal: UNRECOGNIZED selector %{public}s\", sel_getName(sel));" "a blanket forwardInvocation swallows every unimplemented selector"
# ---- the diff guard
gplant 70 "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"1\"); }" "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"2\"); }" "a scanout function is changed"
gplant 71 "    pthread_mutex_lock(&N48S.mu);
    ((n48h_ctx *)c)->slept = n48_now() - t0;" "    pthread_mutex_lock(&N48S.mu);
    ((n48h_ctx *)c)->slept = n48_now() - t0 + 1;" "a hunk inside the M6 + present region"
gplant 72 "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "n48PoolDone gets more than the one declared call"
gplant 73 "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "- (void)n48PoolDone { [self n48UCRelease]; if (_pser) n48_fence_close(_pser); }" "n48PoolDone loses the declared call"
gplant 74 "static BOOL n48x_active(void) {
    n48x_gate_read();" "static BOOL n48x_active(void) {
    (void)0;
    n48x_gate_read();" "a bundle-14 in-process function outside the apps-owned four is changed"
echo "test-b18-plant: $total planted/baseline runs, $escaped escaped"
[ $escaped -eq 0 ]
