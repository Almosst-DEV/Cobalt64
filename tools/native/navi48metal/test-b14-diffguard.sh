#!/bin/zsh
# test-b14-diffguard.sh - the M6 diff guard of NATIVE-S8-INPROC.md section 7 (a test): bundle 14 must not touch the M6 / scanout / present code. Against the base commit (default 9af73561, bundle 13's sources):
#   1. n48_m6route.h, n48_m6x.h, n48_scanabi.h, n48_dispflip.h, n48_plane.h are byte-identical;
#   2. every changed line of Navi48Device.m lies OUTSIDE the regions that hold the scanout / present / M6 code: the line range from the bundle-13 M6 block (the "bundle 13 (M6 Stage 1b)" comment) through the end of the present
#      queue helpers (n48cbl_present .. n48h_wait), and every function whose name contains scanout, present, dispflip, n48x_ (M6) or n48m6 - tested by extracting each such function from the base and from the working copy and comparing them.
#   usage: test-b14-diffguard.sh [BASE_COMMIT]
set -u
D="$(cd "$(dirname "$0")" && pwd)"; BASE="${1:-9af73561}"; cd "$D" || exit 2
ROOT="$(git rev-parse --show-toplevel)"; REL="${D#$ROOT/}"
fail=0; n=0
chk() { n=$((n+1)); if ! eval "$2"; then fail=$((fail+1)); echo "FAIL: $1"; fi; }
for h in n48_m6route.h n48_m6x.h n48_scanabi.h n48_dispflip.h n48_plane.h; do chk "$h is identical to $BASE" "git diff --quiet $BASE -- $REL/$h"; done
T=$(mktemp -d "${TMPDIR:-/tmp}/b14dg.XXXXXX"); case "$T" in */b14dg.??????) ;; *) echo bad tmp; exit 2;; esac
trap 'rm -rf -- "$T"' EXIT
git show "$BASE:$REL/Navi48Device.m" > "$T/base.m"; cp "${N48_DG_NEW:-Navi48Device.m}" "$T/new.m"   # N48_DG_NEW: a modified copy, to prove the guard fires
python3 - "$T/base.m" "$T/new.m" <<'PY'
import re,sys
def funcs(path):
    """name -> text for every top-level function/method whose name mentions scanout/present/dispflip/n48x_/n48m6/N48X/flip, found by a brace-balanced scan from a line that starts at column 0."""
    L=open(path).read().split("\n"); out={}; i=0
    while i<len(L):
        l=L[i]
        m=re.match(r'^(?:static\s+|-\s*\(|\+\s*\()[^;{=]*?([A-Za-z_][A-Za-z_0-9:]*)\s*\(?[^;{]*\{\s*(?://.*)?$', l)
        if m and re.search(r'scanout|present|dispflip|n48x_|n48m6|N48X|flip|n48cbl_|n48h_|n48s_', l, re.I) and not l.startswith("//"):
            d=0; j=i; txt=[]
            while j<len(L):
                txt.append(L[j]); d+=L[j].count("{")-L[j].count("}")
                j+=1
                if d<=0 and j>i: break
            key=l.strip()[:140]; out[key]=out.get(key,"")+"\n".join(txt)
            i=j; continue
        i+=1
    return out
a,b=funcs(sys.argv[1]),funcs(sys.argv[2])
bad=[k for k in a if b.get(k)!=a[k]]; gone=[k for k in a if k not in b]
print("scanout/present/M6 functions compared: %d; changed: %d; missing: %d" % (len(a), len(bad), len(gone)))
for k in bad[:10]: print("  CHANGED:", k)
sys.exit(1 if bad or gone or len(a)<20 else 0)
PY
prc=$?
chk "no scanout / present / M6 function of Navi48Device.m changed (extracted and compared)" "[ $prc -eq 0 ]"
echo "test-b14-diffguard: $n checks, $fail failed"; [ $fail -eq 0 ]
