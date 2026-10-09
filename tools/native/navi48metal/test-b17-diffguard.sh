#!/bin/zsh
# test-b17-diffguard.sh - the ONE merged diff guard of bundle 17 (replaces test-b14/b15/b16-diffguard.sh, which each compare against their OWN base and so fail on the other track's changes once merged).
# Bundle 17 = bundle 16 (app-fix round 1: ledger / F1 / F2 / F3 / P1 ioalias / P4) + bundle 15 (M6 Stage 2: the monitor A, per-instance scanout) on one tree. Two parallel tracks keep changing this directory, so the guard has three modes.
#   test-b17-diffguard.sh merge   [WORKFILE]   (default) proves the MERGE lost and duplicated nothing, against the three commits it came from:
#        ANC = 05d300cd (merge-base), B15 = 1cb1c128 (Stage 2 as built), B16 = db9e89b3 (app-fix round 1 as built).
#        M1  for EVERY file of tools/native/navi48metal: touched by only one track -> byte-identical to that track's version; touched by neither -> identical to ANC;
#            touched by both (Navi48Device.m, INSTALL.md, Info.plist, test pins) -> LINE-MULTISET IDENTITY: for every distinct line, count(M) = count(ANC) + (count(B15)-count(ANC)) + (count(B16)-count(ANC)),
#            i.e. neither track's added line is missing or duplicated and neither's removed line survived (bundle numbers 14..17 are normalised first: each track pinned its own).
#        M2  ownership inside Navi48Device.m: every scanout / present / M6 function (name mentions scanout, present, dispflip, n48x_, n48m6, N48X, flip, n48cbl_, n48h_, n48s_) is byte-identical to B15's, EXCEPT the
#            functions both tracks edited (SHARED in fn.py, each checked to carry the markers of both); every app-fix function (ledger, F1/F2/F3, ioalias, impcache, use count) is identical to B16's;
#            every bundle-14 in-process translation function is identical to ANC's.
#        M3  the two display-surface classify blocks of the texture init (the baseless branch's and the mapped path's tail) are byte-identical to ANC (and so to both tracks).
#        M4  both tracks' tokens are in the file (T1 ledger, n48m-noioalias, n48m-noflip1, inst_geom_mismatch, n48x_plan_geom_ok, n48UCRelease, N48F1_FALL ...) and the bundle version is 17 in Info.plist.
#   test-b17-diffguard.sh display [BASE]     the DISPLAY track (M6 / scanout work) must not change the app-fix code: against BASE (default f85c5e42, the merge) the app-fix and bundle-14 functions and
#        the headers n48_ledger.h n48_ioalias.h n48_impcache.h n48_xlate.h are identical.
#   test-b17-diffguard.sh apps [BASE]        (default BASE 2d779c19 = bundle 18 committed, the base of build 19; was 8d8785c0 = bundle 17 with the review fixes for build 18)  the APPS track must not change the display code: the M6 headers (n48_m6route.h n48_m6x.h n48_scanabi.h n48_dispflip.h n48_plane.h) and every scanout / present / M6
#        function (except the SHARED ones) are identical to BASE, no changed hunk lies inside the M6 block + present helpers of BASE, and the bundle-14 functions and n48_xlate.h are identical
#        (n48_xlate.h is the apps track's own P4 file only in the merge; after the merge the apps track may change it, so it is NOT pinned in this mode - only the bundle-14 functions of Navi48Device.m are).
#   Bundle 20 (HDMI rubberband fix, an internal design note Q3): the DISPLAY track changed the display code on purpose. Default BASE of both modes is now 4eb7cab3 (= bundle 19 committed, the base of build 20).
#        display mode (BASE 4eb7cab3) additionally proves NOTHING ELSE changed: (a) every n48_*.h except n48_dispflip.h and n48_m6x.h is identical to BASE; (b) every changed hunk of Navi48Device.m lies inside one of the
#        functions n48s_disp_account, n48s_tick, n48x_recopy_locked, n48s_complete_run, n48DispSeq, or is the new function n48x_rubber_summary_locked; (c) test-b20-display-delta.patch (the accepted delta) IS the diff
#        BASE..tree of exactly n48_dispflip.h, n48_m6x.h and Navi48Device.m.
#        apps mode (BASE 4eb7cab3) judges the tree against BASE PLUS that accepted delta (the bundle-20 display code is not in git yet): the display headers and every scanout / M6 function must equal base+delta.
#        Regenerate the delta ONLY with a new display build:  git diff --relative=tools/native/navi48metal 4eb7cab3 -- n48_dispflip.h n48_m6x.h Navi48Device.m > test-b20-display-delta.patch
#   (N48_DG_NEW=<file>: a modified copy of Navi48Device.m, to prove the guard fires.)
set -u
D="$(cd "$(dirname "$0")" && pwd)"; cd "$D" || exit 2
MODE="${1:-merge}"; ARG2="${2:-}"
ROOT="$(git rev-parse --show-toplevel)"; REL="${D#$ROOT/}"
ANC=05d300cd; B15=1cb1c128; B16=db9e89b3; MRG=f85c5e42   # MRG = the merge commit (bundle 17 as merged, before the review fixes)
B17=8d8785c0   # the end of bundle 17 (merge + review fixes). From build 18 on, MERGE mode judges THIS commit, not the working tree: it is a history check (the merge lost nothing, and only the review fixes came after it), so later builds do not turn it red; apps / display modes judge the working tree against their BASE.
fail=0; n=0
chk() { n=$((n+1)); if ! eval "$2"; then fail=$((fail+1)); echo "FAIL: $1"; fi; }
T=$(mktemp -d "${TMPDIR:-/tmp}/b17dg.XXXXXX"); case "$T" in */b17dg.??????) ;; *) echo bad tmp; exit 2;; esac
trap 'rm -rf -- "$T"' EXIT
if [ -n "${N48_DG_NEW:-}" ]; then cp "$N48_DG_NEW" "$T/new.m"; elif [ "$MODE" = merge ]; then git -C "$ROOT" show "$MRG:$REL/Navi48Device.m" > "$T/new.m"; else cp "$D/Navi48Device.m" "$T/new.m"; fi
cat > "$T/fn.py" <<'PY'
import re
SCAN=re.compile(r'scanout|present|dispflip|n48x_|n48m6|N48X|flip|n48cbl_|n48h_|n48s_', re.I)
APPS=re.compile(r'n48_led|N48LED|n48led|ioalias|n48ia|n48_imp_|impcache|n48uc|n48_ios_touch|UCRelease|n48IOAlias|n48IOTex|n48IOUnwrite|n48f1|n48_import_alloc', re.I)
XL=re.compile(r'\bN48XL\b|\bn48x_(gate_read|active|resources|xlib_path|setup_locked|user_dir|sandboxed|touch_hit|translate_fn|array_nonempty|prop|linkage_check|linkage_render|linkage_compute|test_inproc|logged|touched|timedout|killed_from_stat|env_dirty|applies|file_uuid|override_hash|key|parent_path|dir_path|prepare_dirs|skip_side_dir|touch|file_path)\b|inproc|n48_xlate\.h|\bn48x_engine|\bn48g_sync')
# functions both tracks had to edit (compared by markers, never by identity)
SHARED = {
 "n48_ios_usek": ["n48_ios_touch"],
 "n48s_record_copy_img": ["n48x_desc", "xd ? N48XI(pl->inst).buf[slot]"],
 "n48Finish": ["n48_ios_writeback", "n48x_desc((uint32_t)pinst)", "gw_, gh_, gp_"],
 "n48DispDone": ["n48s_complete(_dinst"],
}
def funcs(path):
    L=open(path).read().split("\n"); out={}; i=0
    while i<len(L):
        l=L[i]
        m=re.match(r'^(?:static\s+|-\s*\(|\+\s*\()[^;{=]*\{\s*(?://.*)?$', l)
        if m and not l.startswith("//"):
            d=0; j=i; txt=[]
            while j<len(L):
                txt.append(L[j]); d+=L[j].count("{")-L[j].count("}"); j+=1
                if d<=0 and j>i: break
            key=l.strip()[:150]; out[key]=out.get(key,"")+"\n".join(txt)
            i=j; continue
        i+=1
    return out
def shared_name(k):
    for s in SHARED:
        if s in k: return s
    return None
PY
G() { git -C "$ROOT" show "$1:$REL/$2" > "$3" 2>/dev/null; }

if [ "$MODE" = merge ]; then
  files=$( { git -C "$ROOT" ls-tree -r --name-only "$ANC" -- "$REL"; git -C "$ROOT" ls-tree -r --name-only "$B15" -- "$REL"; git -C "$ROOT" ls-tree -r --name-only "$B16" -- "$REL"; } | sort -u | sed "s|^$REL/||" | /usr/bin/grep -v '^build/' | /usr/bin/grep -v '^spvcache/' )
  nfiles=0; both=0
  for f in ${(f)files}; do
    nfiles=$((nfiles+1))
    rm -f "$T/cur"; git -C "$ROOT" show "$MRG:$REL/$f" > "$T/cur" 2>/dev/null; cur="$T/cur"; [ -s "$cur" ] || : > "$cur"; [ "$f" = Navi48Device.m ] && cur="$T/new.m"
    rm -f "$T/a" "$T/b15" "$T/b16"
    G $ANC "$f" "$T/a"; G $B15 "$f" "$T/b15"; G $B16 "$f" "$T/b16"
    [ -s "$T/a" ] || : > "$T/a"; [ -s "$T/b15" ] || : > "$T/b15"; [ -s "$T/b16" ] || : > "$T/b16"
    # bundle numbers 14..17 are normalised to N in every copy: each track pinned its own number (14 ancestor, 15, 16, merged 17); M4 checks the real version separately
    for x in a b15 b16; do perl -pe 's/(?<![0-9])1[4-7](?![0-9])/N/g' "$T/$x" > "$T/n$x"; done
    perl -pe 's/(?<![0-9])1[4-7](?![0-9])/N/g' "$cur" > "$T/ncur"
    t15=1; cmp -s "$T/na" "$T/nb15" && t15=0; t16=1; cmp -s "$T/na" "$T/nb16" && t16=0
    if [ $t15 -eq 0 ] && [ $t16 -eq 0 ]; then chk "$f: untouched by both tracks -> identical to $ANC" "cmp -s $T/na $T/ncur"
    elif [ $t15 -eq 1 ] && [ $t16 -eq 0 ]; then chk "$f: Stage 2 only -> identical to $B15" "cmp -s $T/nb15 $T/ncur"
    elif [ $t15 -eq 0 ] && [ $t16 -eq 1 ]; then chk "$f: app-fix only -> identical to $B16" "cmp -s $T/nb16 $T/ncur"
    else
      both=$((both+1))
      python3 - "$T/na" "$T/nb15" "$T/nb16" "$T/ncur" "$f" <<'PY'
import sys,collections
a,b15,b16,m=[collections.Counter(open(p,errors="replace").read().split("\n")) for p in sys.argv[1:5]]
bad=[]
for k in set(a)|set(b15)|set(b16)|set(m):
    want=a[k]+(b15[k]-a[k])+(b16[k]-a[k])
    if m[k]!=want: bad.append((k,want,m[k]))
real=bad
print("%s: line-multiset identity (bundle numbers normalised): %d lines differ from ANC+B15+B16" % (sys.argv[5],len(real)))
for k,w,g in real[:8]: print("   want %d got %d: %s" % (w,g,k[:130]))
sys.exit(1 if real else 0)
PY
      chk "$f: touched by both tracks -> every line of both tracks present exactly once, nothing resurrected" "[ $? -eq 0 ]"
    fi
  done
  chk "at least 20 files compared, and at least one is touched by both tracks (guard is not vacuous)" "[ $nfiles -ge 20 ] && [ $both -ge 1 ]"
  G $ANC Navi48Device.m "$T/anc.m"; G $B15 Navi48Device.m "$T/b15.m"; G $B16 Navi48Device.m "$T/b16.m"
  python3 - "$T" <<'PY'
import sys,re
sys.path.insert(0,sys.argv[1]); from fn import *
T=sys.argv[1]
A,B15,B16,M=[funcs(T+"/"+x) for x in ("anc.m","b15.m","b16.m","new.m")]
bad=[]; nscan=napp=nxl=nsh=0
for k,v in M.items():
    s=shared_name(k)
    if s:
        nsh+=1
        for tok in SHARED[s]:
            if tok not in v: bad.append("SHARED %s lacks marker %r"%(k[:70],tok))
        continue
    if XL.search(k):
        nxl+=1
        if k in A and A[k]!=v: bad.append("bundle-14 in-process function changed: "+k[:90])
    elif SCAN.search(k):
        nscan+=1
        if k in B15 and B15[k]!=v: bad.append("scanout/M6 function differs from Stage 2's: "+k[:90])
    elif APPS.search(k):
        napp+=1
        if k in B16 and B16[k]!=v: bad.append("app-fix function differs from app-fix round 1's: "+k[:90])
print("functions: scanout/M6 %d, app-fix %d, in-process %d, shared %d; violations %d" % (nscan,napp,nxl,nsh,len(bad)))
for b in bad[:10]: print("  ",b)
sys.exit(1 if bad or nscan<20 or napp<8 or nxl<12 or nsh<3 else 0)
PY
  chk "function ownership: scanout/M6 == Stage 2's, app-fix == round 1's, in-process == bundle 14's, shared functions carry both tracks' markers" "[ $? -eq 0 ]"
  python3 - "$T/anc.m" "$T/new.m" <<'PY'
import sys
def blk(p,start,stop):
    s=open(p).read(); i=s.index(start); j=s.index(stop,i); return s[i:j]
ok=True
for (st,sp) in [("        // P6: same rule as the mapped surfaces (n48s_classify","        return self;\n    }\n    if (((uintptr_t)base"),("    // S5.2a: is this one of CoreDisplay's display surfaces","    #undef IOSFAIL")]:
    x=blk(sys.argv[1],st,sp); y=blk(sys.argv[2],st,sp)
    if x!=y: ok=False; print("  CLASSIFY BLOCK CHANGED:", st[:60])
    if len(x)<400: ok=False; print("  classify block suspiciously short:", st[:60])
print("classify blocks identical to the common ancestor" if ok else "classify blocks DIFFER")
sys.exit(0 if ok else 1)
PY
  chk "the two display-surface classify blocks of the texture init are byte-identical to the ancestor" "[ $? -eq 0 ]"
  for tok in 'T1 ledger:' 'n48m-noioalias' 'n48m-nousecount' 'n48m-noimpcache' 'N48F1_FALL' 'n48UCRelease' 'n48m-noflip1' 'inst_geom_mismatch' 'n48x_plan_geom_ok' 'n48m-noflip2' 'n48m-noinproc'; do
    chk "token '$tok' present in Navi48Device.m or n48_m6x.h" "cat $T/new.m '$D/n48_m6x.h' | /usr/bin/grep -q -- '$tok'"; done
  chk "Info.plist bundle version is 17 (at $B17)" "git -C '$ROOT' show '$B17:$REL/Info.plist' | /usr/bin/grep -A1 CFBundleVersion | /usr/bin/grep -q '<string>17</string>'"
  # ---- M5: POST-MERGE edits (the working tree vs the merge commit): only the review fixes R-B1 / R-S2 and the guard / plant files, nothing else ----
  POSTOK="Navi48Device.m n48_m6x.h test-m6x.c test-b15-plant.sh test-b17-plant.sh test-b17-diffguard.sh INSTALL.md"
  changed=$( { git -C "$ROOT" diff --name-only "$MRG" "$B17" -- "$REL"; } | sed "s|^$REL/||")
  bad=""; for f in ${(f)changed}; do case " $POSTOK " in *" $f "*) ;; *) case "$f" in build/*|spvcache/*) ;; *) bad="$bad $f";; esac;; esac; done
  chk "files changed since the merge commit are only the review-fix files (unexpected:${bad:- none})" "[ -z '$bad' ]"
  if [ -z "${N48_DG_NEW:-}" ]; then
    git -C "$ROOT" show "$MRG:$REL/Navi48Device.m" > "$T/mrg.m"
    git -C "$ROOT" show "$B17:$REL/Navi48Device.m" > "$T/b17.m"
    python3 - "$T" "$T/b17.m" <<'PY'
import sys
sys.path.insert(0,sys.argv[1]); from fn import *
T=sys.argv[1]; A=funcs(T+"/mrg.m"); M=funcs(sys.argv[2])
ALLOW=("n48s_disp_account","n48Finish","n48x_k_acquire")   # R-B1 (gate + every-frame check), R-S2 (retry)
bad=[k[:100] for k,v in A.items() if M.get(k)!=v and not any(a in k for a in ALLOW)]
bad+= ["NEW function: "+k[:100] for k in M if k not in A]
print("functions changed since the merge commit outside the review-fix set %s: %d" % (ALLOW,len(bad)))
for b in bad[:8]: print("   ",b)
sys.exit(1 if bad else 0)
PY
    chk "Navi48Device.m: only n48s_disp_account, n48Finish and n48x_k_acquire changed since the merge commit" "[ $? -eq 0 ]"
  fi
else
  BASE="${ARG2:-4eb7cab3}"; G $BASE Navi48Device.m "$T/base.m"
  DELTA="$D/test-b20-display-delta.patch"; BF="$T/bf"; mkdir -p "$BF"; PATCHED=""
  if [ "$MODE" = apps ] && [ "$(git -C "$ROOT" rev-parse "$BASE^{commit}")" = "$(git -C "$ROOT" rev-parse "4eb7cab3^{commit}")" ] && [ -f "$DELTA" ]; then
    for f in n48_dispflip.h n48_m6x.h Navi48Device.m; do git -C "$ROOT" show "$BASE:$REL/$f" > "$BF/$f"; done
    ( cd "$BF" && patch -s -p1 < "$DELTA" ) >"$T/patch.out" 2>&1 || { echo "FAIL: the accepted display delta does not apply to $BASE"; cat "$T/patch.out"; exit 1; }
    cp "$BF/Navi48Device.m" "$T/base.m"; PATCHED=1
  fi
  hsame() { if [ -n "$PATCHED" ] && [ -f "$BF/$1" ]; then cmp -s "$BF/$1" "$D/$1"; else git -C "$ROOT" diff --quiet "$BASE" -- "$REL/$1"; fi; }
  if [ "$MODE" = display ]; then
    for h in n48_ledger.h n48_ioalias.h n48_impcache.h n48_xlate.h; do chk "$h identical to $BASE (the display track must not edit app-fix / bundle-14 headers)" "git -C $ROOT diff --quiet $BASE -- $REL/$h"; done
    python3 - "$T" <<'PY'
import sys,re
sys.path.insert(0,sys.argv[1]); from fn import *
T=sys.argv[1]; A=funcs(T+"/base.m"); M=funcs(T+"/new.m"); bad=[]; n=0
for k,v in A.items():
    if shared_name(k): continue
    if XL.search(k) or APPS.search(k):
        n+=1
        if M.get(k)!=v: bad.append(k[:100])
print("app-fix / in-process functions compared: %d; changed or missing: %d" % (n,len(bad)))
for b in bad[:10]: print("   CHANGED:",b)
sys.exit(1 if bad or n<20 else 0)
PY
    chk "no app-fix / in-process function of Navi48Device.m changed" "[ $? -eq 0 ]"
    # ---- bundle 20: nothing ELSE changed. (a) headers
    for h in $(cd "$D" && ls n48_*.h); do case "$h" in n48_dispflip.h|n48_m6x.h) continue;; esac; chk "$h identical to $BASE (bundle 20 touches only n48_dispflip.h and n48_m6x.h)" "git -C $ROOT diff --quiet $BASE -- $REL/$h"; done
    # (b) every changed hunk of Navi48Device.m lies inside an allowed function
    python3 - "$T" <<'PY'
import sys,re,difflib
T=sys.argv[1]
ALLOWED=("n48s_disp_account(","n48s_tick(","n48x_recopy_locked(","n48s_complete_run(","n48DispSeq")
NEWFN="static void n48x_rubber_summary_locked("
a=open(T+"/base.m").read().split("\n"); b=open(T+"/new.m").read().split("\n")
rng=[]; i=0
while i<len(a):
    l=a[i]
    if re.match(r'^(?:static\s+|-\s*\(|\+\s*\()[^;{=]*\{\s*(?://.*)?$', l) and not l.startswith("//"):
        d=0; j=i
        while j<len(a):
            d+=a[j].count("{")-a[j].count("}"); j+=1
            if d<=0 and j>i: break
        rng.append((i,j,l)); i=j; continue
    i+=1
bad=[]; nh=0; seen=set(); newfn=0
for tag,i1,i2,j1,j2 in difflib.SequenceMatcher(None,a,b,autojunk=False).get_opcodes():
    if tag=="equal": continue
    nh+=1
    ins="\n".join(b[j1:j2])
    f=[r for r in rng if (r[0]<=i1<r[1] if tag!="insert" else r[0]<i1<r[1])]
    if tag!="insert": f=[r for r in f if i2<=r[1]]
    if f:
        o=[x for x in ALLOWED if x in f[0][2]]
        if o: seen.add(o[0])
        else: bad.append("hunk in "+f[0][2][:80])
    elif tag=="insert" and NEWFN in ins and all(x.strip()=="" or x.lstrip().startswith("//") or x.startswith(("static","    ","}")) for x in b[j1:j2]): newfn+=1
    else: bad.append("hunk outside any allowed function at base line %d (%s)" % (i1+1,tag))
print("Navi48Device.m: %d changed hunks; functions touched: %s; new function n48x_rubber_summary_locked: %d" % (nh,sorted(seen),newfn))
for x in bad[:10]: print("   NOT ALLOWED:",x)
sys.exit(1 if bad or newfn!=1 or len(seen)<4 else 0)
PY
    chk "bundle 20: every changed hunk of Navi48Device.m lies inside an allowed function (or is the new n48x_rubber_summary_locked)" "[ $? -eq 0 ]"
    # (c) the accepted delta is exactly the tree's diff of the three display files (not judged when N48_DG_NEW substitutes Navi48Device.m: the hunk check above already judged that copy)
    if [ -f "$DELTA" ]; then
      git -C "$ROOT" diff --relative="$REL" "$BASE" -- "$REL/n48_dispflip.h" "$REL/n48_m6x.h" "$REL/Navi48Device.m" > "$T/cur.patch"
      if [ -z "${N48_DG_NEW:-}" ]; then chk "test-b20-display-delta.patch equals the tree's diff of n48_dispflip.h + n48_m6x.h + Navi48Device.m against $BASE" "cmp -s $T/cur.patch $DELTA"; fi
    else chk "test-b20-display-delta.patch exists" "false"; fi
  elif [ "$MODE" = apps ]; then
    for h in n48_m6route.h n48_m6x.h n48_scanabi.h n48_dispflip.h n48_plane.h; do chk "$h identical to $BASE${PATCHED:+ + the accepted bundle-20 delta} (the apps track must not edit the display headers)" "hsame $h"; done
    python3 - "$T" <<'PY'
import sys,re,difflib
sys.path.insert(0,sys.argv[1]); from fn import *
T=sys.argv[1]; A=funcs(T+"/base.m"); M=funcs(T+"/new.m"); bad=[]; n=0
# Build 18 (app-fix round 2, P3): the apps track OWNS the linked-function glue of the in-process translation and may change exactly these four functions (they read the linkage of a pipeline descriptor
# and install its link context). Every other bundle-14 in-process function, and every scanout / present / M6 function, stays pinned.
APPS_OWN = ("n48x_translate_fn(", "n48x_linkage_check(", "n48x_linkage_render(", "n48x_linkage_compute(")
# The ONE shared function the apps track may touch is n48PoolDone (it resolves the occlusion queries before commandBufferDidComplete); the other shared functions are pinned to the base here.
POOLDONE_MARKERS = ["n48OccResolve", "n48UCRelease", "n48_fence_close"]
nown=0
for k,v in A.items():
    if shared_name(k):
        if M.get(k)!=v: bad.append("SHARED function changed (only n48PoolDone may be touched): "+k[:80])
        continue
    if XL.search(k) or (SCAN.search(k) and not APPS.search(k)):
        if any(o in k for o in APPS_OWN): nown+=1; continue
        n+=1
        if M.get(k)!=v: bad.append(k[:100])
# n48PoolDone is a ONE-LINE method (funcs() only sees bodies that open on their own line), so it is compared as a line.
def oneline(path):
    return [l for l in open(path).read().split("\n") if l.startswith("- (void)n48PoolDone {")]
bl=oneline(T+"/base.m"); nl=oneline(T+"/new.m")
if len(bl)!=1 or len(nl)!=1: bad.append("n48PoolDone one-liner not found exactly once (base %d, new %d)" % (len(bl),len(nl)))
else:
    for tok in POOLDONE_MARKERS:
        if tok not in nl[0]: bad.append("n48PoolDone lacks %s" % tok)
    if "n48OccResolve" in bl[0]:   # build 19 and later: the base (bundle 18) already carries the one allowed call, so the line must be IDENTICAL to the base
        if nl[0]!=bl[0]: bad.append("n48PoolDone differs from the base (which already has n48OccResolve)")
        print("n48PoolDone (base already has n48OccResolve) identical to the base: %s" % ("yes" if nl[0]==bl[0] else "NO"))
        skip_pd=True
    else: skip_pd=False
    if not skip_pd:
      for tok in ("n48UCRelease","n48_fence_close"):
        if tok not in bl[0]: bad.append("base n48PoolDone lacks %s (guard out of date)" % tok)
      stripped=nl[0].replace("[self n48OccResolve]; ","",1)
      if stripped!=bl[0]: bad.append("n48PoolDone differs from the base by more than the one n48OccResolve call")
      print("n48PoolDone (the declared shared touch) = the base line + one [self n48OccResolve] call: %s" % ("yes" if stripped==bl[0] else "NO"))
if nown!=len(APPS_OWN): bad.append("apps-owned P3 functions found %d of %d" % (nown,len(APPS_OWN)))
print("scanout / M6 / in-process functions compared: %d (+%d apps-owned P3 link functions allowed to change, +shared pinned); changed or missing: %d" % (n,nown,len(bad)))
for b in bad[:10]: print("   CHANGED:",b)
a=open(T+"/base.m").read().split("\n"); b=open(T+"/new.m").read().split("\n")
start=next(i for i,l in enumerate(a) if l.startswith("// ---- bundle 13 (M6 Stage 1b)"))+1
hw=next(i for i,l in enumerate(a) if l.startswith("static void n48h_wait("))
end=next(i for i in range(hw,len(a)) if a[i]=="}")+1
inside=0
for tag,i1,i2,j1,j2 in difflib.SequenceMatcher(None,a,b,autojunk=False).get_opcodes():
    if tag=="equal": continue
    lo,hi=i1+1,max(i2,i1+1)
    if (tag!="insert" and not (hi<start or lo>end)) or (tag=="insert" and start<=i1<end): inside+=1; print("   hunk inside the M6/present region at base line",lo)
print("M6/present region %d-%d; hunks inside: %d" % (start,end,inside))
sys.exit(1 if bad or inside or n<30 else 0)
PY
    chk "no scanout / M6 / in-process function changed and no hunk inside the M6 + present region" "[ $? -eq 0 ]"
  else echo "usage: $0 merge|display|apps [BASE]"; exit 2; fi
fi
echo "test-b17-diffguard ($MODE): $n checks, $fail failed"; [ $fail -eq 0 ]
