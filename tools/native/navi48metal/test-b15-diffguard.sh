#!/bin/zsh
# test-b15-diffguard.sh - the REVERSE diff guard of bundle 15 (M6 Stage 2): bundle 15 changes the M6 / scanout / present code (per-instance state for the monitor A) and must NOT touch bundle 14's in-process shader translation
# (NATIVE-S8-INPROC.md). Against the base commit (default 2e5f7a43, bundle 14's sources):
#   1. n48_xlate.h and the in-process tests (test-xlate.c, test-xlate-corpus.c / .sh, test-b14-plant.sh, test-b14-diffguard.sh) are byte-identical;
#   2. no ADDED or REMOVED line of Navi48Device.m (git diff -U0) mentions the in-process translation: N48XL, the n48x_* in-process functions, 'inproc', n48_xlate.h, its N48X_* macros, n48x_engine, n48g_sync;
#   3. every in-process function of Navi48Device.m (extracted brace-balanced from the base and from the working copy) is identical - and there are at least 12 of them.
#   usage: test-b15-diffguard.sh [BASE_COMMIT]      (N48_DG_NEW=<file>: a modified copy of Navi48Device.m, to prove the guard fires)
set -u
D="$(cd "$(dirname "$0")" && pwd)"; BASE="${1:-2e5f7a43}"; cd "$D" || exit 2
ROOT="$(git rev-parse --show-toplevel)"; REL="${D#$ROOT/}"
fail=0; n=0
chk() { n=$((n+1)); if ! eval "$2"; then fail=$((fail+1)); echo "FAIL: $1"; fi; }
for f in n48_xlate.h test-xlate.c test-xlate-corpus.c test-xlate-corpus.sh test-b14-plant.sh test-b14-diffguard.sh; do chk "$f is identical to $BASE" "git diff --quiet $BASE -- $REL/$f"; done
T=$(mktemp -d "${TMPDIR:-/tmp}/b15dg.XXXXXX"); case "$T" in */b15dg.??????) ;; *) echo bad tmp; exit 2;; esac
trap 'rm -rf -- "$T"' EXIT
git show "$BASE:$REL/Navi48Device.m" > "$T/base.m"; cp "${N48_DG_NEW:-Navi48Device.m}" "$T/new.m"
python3 - "$T/base.m" "$T/new.m" <<'PY'
import re,sys,difflib
RX=re.compile(r'\bN48XL\b|\bn48x_(gate_read|active|resources|xlib_path|setup_locked|user_dir|sandboxed|touch_hit|translate_fn|array_nonempty|prop|linkage_check|linkage_render|linkage_compute|test_inproc|logged|touched|timedout)\b|\binproc\b|n48_xlate\.h|\bN48X_(PATH_MAX|KILL_FILE|ENV_PREFIX|LIB_NAME|OK|E_[A-Z_]+|EXT_[A-Z]+|WORKER_STACK)\b|\bn48x_engine|\bn48g_sync|\bn48x_(killed_from_stat|env_dirty|applies|file_uuid|override_hash|key|parent_path|dir_path|prepare_dirs|skip_side_dir|touch|file_path)\b')
a=open(sys.argv[1]).read().split("\n"); b=open(sys.argv[2]).read().split("\n")
bad=[]
for l in difflib.unified_diff(a,b,lineterm="",n=0):
    if l.startswith(("---","+++","@@")): continue
    if RX.search(l): bad.append(l[:150])
print("changed lines mentioning the in-process translation: %d" % len(bad))
for l in bad[:10]: print("  ",l)
def funcs(L):
    out={}; i=0
    while i<len(L):
        l=L[i]
        m=re.match(r'^(?:static\s+|-\s*\(|\+\s*\()[^;{=]*\{\s*(?://.*)?$', l)
        if m and RX.search(l) and not l.startswith("//"):
            d=0; j=i; txt=[]
            while j<len(L):
                txt.append(L[j]); d+=L[j].count("{")-L[j].count("}"); j+=1
                if d<=0 and j>i: break
            out[l.strip()[:140]]="\n".join(txt); i=j; continue
        i+=1
    return out
fa,fb=funcs(a),funcs(b)
chg=[k for k in fa if fb.get(k)!=fa[k]]
print("in-process functions compared: %d; changed/missing: %d" % (len(fa),len(chg)))
for k in chg[:10]: print("  CHANGED:",k)
sys.exit(1 if bad or chg or len(fa)<12 else 0)
PY
prc=$?
chk "no line of Navi48Device.m that mentions the in-process translation changed, and its functions are identical" "[ $prc -eq 0 ]"
echo "test-b15-diffguard: $n checks, $fail failed"; [ $fail -eq 0 ]
