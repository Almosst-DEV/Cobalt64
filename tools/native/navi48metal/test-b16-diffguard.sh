#!/bin/zsh
# test-b16-diffguard.sh - the scanout diff guard for bundle build 16 (app-fix round 1, an internal design note): the P5 / P1 / P4 work must stay OUT of the scanout / present / M6 code so the reviewer can merge it
# with the Stage 2 builder's bundle 15. Against the base commit (default c94ddf0a, the sources bundle 16 was started from):
#   1. n48_m6route.h, n48_m6x.h, n48_scanabi.h, n48_dispflip.h, n48_plane.h are byte-identical;
#   2. every scanout / present / M6 function of Navi48Device.m (name mentions scanout, present, dispflip, n48x_ (M6), n48m6, flip, n48cbl_, n48h_, n48s_) is identical to the base (extracted and compared);
#   3. NO changed hunk of Navi48Device.m lies inside the line range from the "bundle 13 (M6 Stage 1b)" comment through the end of n48h_wait (the M6 block + the present queue helpers) of the base;
#   4. the two display-surface classify blocks of the IOSurface texture init are byte-identical (the baseless branch's classify and the mapped path's tail "S5.2a: is this one of CoreDisplay's display surfaces");
#   5. n48_xlate.h changed ONLY by P4's sharing: a single hunk that adds lines (no deletion) inside n48x_translate_job and contains the installed-pair early-out.
#   usage: test-b16-diffguard.sh [BASE_COMMIT]        (N48_DG_NEW=<file>: a modified copy of Navi48Device.m, to prove the guard fires)
set -u
D="$(cd "$(dirname "$0")" && pwd)"; BASE="${1:-c94ddf0a}"; cd "$D" || exit 2
ROOT="$(git rev-parse --show-toplevel)"; REL="${D#$ROOT/}"
fail=0; n=0
chk() { n=$((n+1)); if ! eval "$2"; then fail=$((fail+1)); echo "FAIL: $1"; fi; }
for h in n48_m6route.h n48_m6x.h n48_scanabi.h n48_dispflip.h n48_plane.h; do chk "$h is identical to $BASE" "git -C $ROOT diff --quiet $BASE -- $REL/$h"; done
T=$(mktemp -d "${TMPDIR:-/tmp}/b16dg.XXXXXX"); case "$T" in */b16dg.??????) ;; *) echo bad tmp; exit 2;; esac
trap 'rm -rf -- "$T"' EXIT
git show "$BASE:$REL/Navi48Device.m" > "$T/base.m"; cp "${N48_DG_NEW:-Navi48Device.m}" "$T/new.m"
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

# 3. hunks vs the M6 / present region
git show "$BASE:$REL/Navi48Device.m" > "$T/base2.m"
python3 - "$T/base.m" "$T/new.m" <<'PY'
import re,sys,subprocess,difflib
a=open(sys.argv[1]).read().split("\n"); b=open(sys.argv[2]).read().split("\n")
start=next(i for i,l in enumerate(a) if "bundle 13 (M6 Stage 1b): INSTANCE 2" in l)+1
hw=next(i for i,l in enumerate(a) if l.startswith("static void n48h_wait("))
end=next(i for i in range(hw,len(a)) if a[i]=="}")+1
bad=0
for tag,i1,i2,j1,j2 in difflib.SequenceMatcher(None,a,b,autojunk=False).get_opcodes():
    if tag=="equal": continue
    lo,hi=i1+1,max(i2,i1+1)   # 1-based base lines touched (an insertion touches the gap after line i1)
    if i1==i2: lo=hi=i1       # insertion after base line i1: inside iff start <= i1 < end
    if (tag!="insert" and not (hi<start or lo>end)) or (tag=="insert" and start<=i1<end):
        bad+=1; print("  CHANGED INSIDE the M6/present region (base lines %d-%d, region %d-%d): %s" % (lo,hi,start,end,(b[j1] if j1<len(b) else "")[:100]))
print("M6/present region base lines %d-%d; hunks inside: %d" % (start,end,bad))
sys.exit(1 if bad else 0)
PY
chk "no changed hunk of Navi48Device.m lies inside the M6 block + present queue helpers of the base" "[ $? -eq 0 ]"
# 4. the classify blocks
python3 - "$T/base.m" "$T/new.m" <<'PY'
import sys
def blk(p,start,stop):
    s=open(p).read(); i=s.index(start); j=s.index(stop,i); return s[i:j]
ok=True
for (st,sp) in [("        // P6: same rule as the mapped surfaces (n48s_classify","        return self;\n    }\n    if (((uintptr_t)base"),("    // S5.2a: is this one of CoreDisplay's display surfaces","    #undef IOSFAIL")]:
    x=blk(sys.argv[1],st,sp); y=blk(sys.argv[2],st,sp)
    if x!=y: ok=False; print("  CLASSIFY BLOCK CHANGED:", st[:60])
    if len(x)<400: ok=False; print("  classify block suspiciously short:", st[:60])
print("classify blocks identical" if ok else "classify blocks DIFFER")
sys.exit(0 if ok else 1)
PY
chk "the two display-surface classify blocks of the texture init are byte-identical" "[ $? -eq 0 ]"
# 5. n48_xlate.h
git -C "$ROOT" diff -U0 "$BASE" -- "$REL/n48_xlate.h" > "$T/xl.diff"
chk "n48_xlate.h: exactly one hunk" "[ \$(/usr/bin/grep -c '^@@' $T/xl.diff) -eq 1 ]"
chk "n48_xlate.h: the hunk deletes nothing" "! /usr/bin/grep -q '^-[^-]' $T/xl.diff"
chk "n48_xlate.h: the hunk is P4's installed-pair early-out" "/usr/bin/grep -q 'S_ISREG(dst.st_mode)) return N48X_OK;' $T/xl.diff && /usr/bin/grep -q 'P4 (build 16)' $T/xl.diff"
echo "test-b16-diffguard: $n checks, $fail failed"; [ $fail -eq 0 ]
