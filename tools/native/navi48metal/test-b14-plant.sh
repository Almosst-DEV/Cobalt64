#!/bin/zsh
# test-b14-plant.sh - planted breaks for bundle build 14 (in-process shader translation, NATIVE-S8-INPROC.md section 1): n48_xlate.h / test-xlate.c, the glue in Navi48Device.m, and the fork's Rust (n48xlate + the Validation::Skip carrier).
# Same discipline as test-b13-plant.sh: copy the REAL files into a scratch tree, apply ONE break (the script proves the text was there exactly once), run the suite and demand that it FAILS (a compile error counts).
# C breaks run test-xlate.c; Rust breaks copy the fork (without target/) into the scratch tree and run its cargo tests (n48xlate corpus + hooks, on a small corpus subset that keeps every post-validation reject) with a scratch target dir.
#   usage: test-b14-plant.sh [--c-only]        env: N48X_CORPUS (a prepared corpus dir with ref/, for the Rust plants), N48_FORK_DIR (default ~/navi48-native/metal2vulkan), TMPDIR (where the scratch tree goes)
#   Fast mode: C compiles at -O0; the Rust plants build in release (the translator is too slow in debug).
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b14-plant.XXXXXX")"
case "$SCR" in */b14-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
FORK="${N48_FORK_DIR:-$HOME/navi48-native/metal2vulkan}"; CORPUS="${N48X_CORPUS:-}"
escaped=0; total=0
W="$SCR/c"
cfresh() { rm -rf -- "$W"; mkdir -p "$W/autotranslate"; cp "$D"/n48_xlate.h "$D"/n48_gate.h "$D"/Navi48Device.m "$D"/test-xlate.c "$W/"; cp "$D/../autotranslate/overrides.txt" "$W/autotranslate/"; }
c_test() {
  OUT=""
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror -DN48X_TESTHOOKS -o t-xlate test-xlate.c ) >"$SCR/cc.out" 2>&1 || { OUT="compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && TMPDIR="$SCR" ./t-xlate . autotranslate/overrides.txt ) >"$SCR/o0" 2>&1 || { OUT="test-xlate: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "$OUT" ] || OUT="test-xlate died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
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
cplant() {   # cplant <id> <file in the scratch c dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  cfresh; apply "$W/$file" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if c_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
cfresh; total=$((total+1)); if c_test; then echo "BASELINE (C): the unmodified scratch copy passes"; else echo "BASELINE (C): FAILED: $OUT"; escaped=$((escaped+1)); fi
H=n48_xlate.h; M=Navi48Device.m
cplant 1  $H "    if (isWs) return 0;
    if (killed) return 0;" "    (void)isWs; if (killed) return 0;" "WindowServer allowed in n48x_applies"
cplant 2  $H "    return admitted ? 1 : 0;" "    (void)admitted; return 1;" "an unadmitted process allowed in n48x_applies"
cplant 3  $H "    if (killed) return 0;
    if (envDirty) return 0;" "    (void)killed; if (envDirty) return 0;" "the kill file ignored by n48x_applies"
cplant 4  $H "    if (envDirty) return 0;
    if (rootAllow)" "    (void)envDirty; if (rootAllow)" "the METAL2VULKAN_ environment check ignored by n48x_applies"
cplant 7  $H "    CC_SHA256_Update(&c, readerId, 16);
" "    (void)readerId;
" "the cache key omits the reader's UUID"
cplant 8  $H '    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_META) || n48x_write_atomic(path, meta, ml) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the meta file"); goto done; }
    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_SPV) || n48x_write_atomic(path, spv, sl) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the spv file"); goto done; }' '    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_SPV) || n48x_write_atomic(path, spv, sl) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the spv file"); goto done; }
    if (!n48x_file_path(path, sizeof path, dir, sha, N48X_EXT_META) || n48x_write_atomic(path, meta, ml) != 0) { rc = N48X_E_IO; snprintf(why, sizeof why, "cannot write the meta file"); goto done; }' ".spv written before .meta.json"
cplant 9  $H "    if (hasCrash) return N48X_PL_SKIP_CRASH;
" "    (void)hasCrash;
" "a .crash marker is ignored by the plan"
cplant 10 $H "    int rc = pthread_attr_setstacksize(&a, stackBytes);" "    int rc = 0; (void)stackBytes;" "the worker runs on the default stack"
cplant 11 $M "            NSMutableArray *m = [bundleOnly mutableCopy];
            if (u) { [m addObject:u]; userFor = u; }" "            NSMutableArray *m = [NSMutableArray array];
            if (u) { [m addObject:u]; userFor = u; }
            [m addObjectsFromArray:bundleOnly];" "the user cache directory is looked up before the bundle spvcache"
cplant 13 $H "    const int mk = n48x_marker_create(dir, sha);
    if (mk == 1)" "    const int mk = 0;
    if (mk == 1)" "no crash marker is written before translating"
cplant 14 $H "static inline int n48x_killed_from_stat(int statRc, int err) { return n48g_kill_state(statRc, err) != N48G_KILL_ABSENT; }" "static inline int n48x_killed_from_stat(int statRc, int err) { (void)err; return statRc == 0; }" "an unreadable kill file counts as absent (fail open)"
cplant 15 $M "const BOOL skipSide = u && n48x_skip_side_dir(n48_is_ws(), n48x_sandboxed());" "const BOOL skipSide = NO;" "a sandboxed application still looks in the side directory"
cplant 16 $M "    return n48x_applies(n48_is_ws(), atomic_load(&n48_app_admitted), n48_allow(), n48x_test_inproc(), N48XL.killed, N48XL.envDirty) ? YES : NO;" "    return n48x_applies(0, atomic_load(&n48_app_admitted), n48_allow(), n48x_test_inproc(), N48XL.killed, N48XL.envDirty) ? YES : NO;" "the glue tells n48x_applies it is never WindowServer"
cplant 17 $H "    if (r->SetOpaquePointers) r->SetOpaquePointers(ctx, 1);   // before anything is read into the context
" "" "the reader no longer switches Apple's LLVM to opaque pointers"
cplant 18 $M "    if (!inproc) (void)n48_dump_pipeline(d);" "    (void)n48_dump_pipeline(d);" "the render miss still dumps the AIR when it translates in process"
cplant 19 $H "    if (!n48x_sha_ok(sha) || !dir || !dir[0])" "    if (!dir || !dir[0])" "the job accepts an unvalidated sha"
cplant 20 $H "    case N48X_PL_SKIP_FAIL:" "    case 99:" "the negative cache is not honoured"
if [ "${1:-}" != "--c-only" ]; then
  if [ -z "$CORPUS" ] || [ ! -d "$CORPUS/ref" ]; then echo "Rust plants need N48X_CORPUS (a corpus dir with ref/)"; escaped=$((escaped+1)); else
  SUB="$SCR/corpus"; mkdir -p "$SUB/ref"
  python3 - "$CORPUS" "$SUB" <<'PY'
import sys,shutil,os
c,s=sys.argv[1:3]; okn=0; keep=[]
for l in open(c+"/manifest.tsv"):
    f=l.rstrip("\n").split("\t"); sha=f[0]; has=os.path.exists(f"{c}/ref/{sha}.spv")
    if has:
        if okn>=60: continue
        okn+=1
    keep.append(l)
    shutil.copy(f"{c}/{sha}.clang.ll", s)
    for e in (".spv",".meta.json"):
        if has: shutil.copy(f"{c}/ref/{sha}{e}", s+"/ref")
open(s+"/manifest.tsv","w").write("".join(keep))
PY
  R="$SCR/fork"; rm -rf -- "$R"; mkdir -p "$R"
  ( cd "$FORK" && tar --exclude=./target --exclude=./.git -cf - . ) | ( cd "$R" && tar -xf - )
  export PATH=/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH CARGO_TARGET_DIR="$SCR/target"
  rust_test() {
    OUT=""
    ( cd "$R" && N48X_CORPUS="$SUB" N48X_OVERRIDES="$D/../autotranslate/overrides.txt" cargo test --release -p n48xlate --features test-hooks --test corpus --test hooks ) >"$SCR/cargo.out" 2>&1 && return 0
    OUT="$(/usr/bin/grep -m1 -E 'FAILED|panicked|error(\[|:)|SIGABRT|signal' "$SCR/cargo.out" | cut -c1-110)"; [ -n "$OUT" ] || OUT="cargo test failed"; return 1
  }
  total=$((total+1)); if rust_test; then echo "BASELINE (Rust): the unmodified fork copy passes the corpus + hooks tests"; else echo "BASELINE (Rust): FAILED: $OUT"; escaped=$((escaped+1)); fi
  rplant() {   # rplant <id> <file in the fork copy> <old> <new> <description>
    local id="$1" file="$2" old="$3" new="$4" desc="$5"
    cp "$FORK/$file" "$R/$file"; apply "$R/$file" "$old" "$new"; local rc=$?; total=$((total+1))
    if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); cp "$FORK/$file" "$R/$file"; return; fi
    if rust_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
    cp "$FORK/$file" "$R/$file"
  }
  rplant 5  src/lib.rs "        reject_function_constant_erased_effects(san_ll, &constructed.module)?;
        reject_imageblock_only_effects(san_ll, &constructed.module)?;" "        if validation == Validation::External {
            reject_function_constant_erased_effects(san_ll, &constructed.module)?;
            reject_imageblock_only_effects(san_ll, &constructed.module)?;
        }" "the Skip path drops the two post-validation rejects"
  rplant 6  src/lib.rs "        if validation == Validation::External {
            tools::spirv_val_bytes(&constructed.bytes, tmp)?;" "        if true {
            tools::spirv_val_bytes(&constructed.bytes, tmp)?;" "the validator is called on the Skip path"
  rplant 12 n48xlate/src/lib.rs "    let outcome = catch_unwind(AssertUnwindSafe(|| translate_bytes(bytes, local)));" "    let outcome: Result<_, Box<dyn std::any::Any + Send>> = Ok(translate_bytes(bytes, local));" "catch_unwind removed (the panic fixture)"
  fi
fi
echo "b14 plants: $total checks, $escaped escaped"; [ $escaped -eq 0 ]
