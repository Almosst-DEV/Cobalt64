#!/bin/zsh
# test-b19-plant.sh - planted breaks for bundle build 19 (missing menus: RG16Uint, an internal design note "Fix contract"): the table entry, the integer clear, the fallback write mask, the kill file, the header's
# conversion rules, the version, and the diff guard (apps mode, base 2d779c19). Same discipline as test-b18-plant.sh: copy the REAL files into a scratch tree, apply ONE break (the script proves the text was there exactly
# once), run the suites and demand at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   usage: test-b19-plant.sh           env: TMPDIR (where the scratch tree goes)        Fast mode: C compiles at -O0, no sanitizers.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b19-plant.XXXXXX")"
case "$SCR" in */b19-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/c"
cfresh() { rm -rf -- "$W"; mkdir -p "$W"; cp "$D"/n48_*.h "$D"/Navi48Device.m "$D"/Info.plist "$D"/test-intfmt.c "$D"/test-intclear-semantics.m  "$W/"; }
c_test() {
  OUT=""
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror -o t-intfmt test-intfmt.c -lm ) >"$SCR/cc.out" 2>&1 || { OUT="test-intfmt: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-intfmt . ) >"$SCR/o0" 2>&1 || { OUT="test-intfmt: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
  ( cd "$W" && clang -fobjc-arc -framework Metal -framework Foundation -I. -o t-intclear test-intclear-semantics.m ) >"$SCR/cc.out" 2>&1 || { OUT="test-intclear-semantics: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-intclear ) >"$SCR/o0" 2>&1; local rc=$?
  [ $rc -eq 3 ] && { OUT="test-intclear-semantics: SKIP (no Metal device): the plant run cannot judge the conversion"; return 1; }
  [ $rc -eq 0 ] || { OUT="test-intclear-semantics: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
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
gplant() {   # gplant <id> <old> <new> <description>: a break in Navi48Device.m that the DIFF GUARD (apps mode, default base 2d779c19) must refuse
  local id="$1" old="$2" new="$3" desc="$4"
  cfresh; apply "$W/Navi48Device.m" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if ( cd "$D" && N48_DG_NEW="$W/Navi48Device.m" ./test-b17-diffguard.sh apps >"$SCR/g.out" 2>&1 ); then echo "PLANT $id: $desc: *** ESCAPED (the guard passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by the diff guard: $(/usr/bin/grep -m1 -E 'CHANGED:|hunk inside|SHARED function changed|differs from the base|lacks|apps-owned' "$SCR/g.out" | cut -c1-110)"; fi
}
cfresh; total=$((total+1)); if c_test; then echo "BASELINE (C): the unmodified scratch copy passes"; else echo "BASELINE (C): FAILED: $OUT"; escaped=$((escaped+1)); fi
total=$((total+1)); if ( cd "$D" && ./test-b17-diffguard.sh apps >"$SCR/g.out" 2>&1 ); then echo "BASELINE (guard): the real tree passes the apps guard (base 2d779c19)"; else echo "BASELINE (guard): FAILED"; tail -3 "$SCR/g.out"; escaped=$((escaped+1)); fi
M=Navi48Device.m; H=n48_intfmt.h
# ---- the contract's four named breaks
cplant 1  $M "{ MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UINT,              4, N48F_UINT }," "{ MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UNORM,             4, N48F_UINT }," "the table maps RG16Uint to R16G16_UNORM (a float format: the shaders' uint outputs and reads would be wrong)"
cplant 3  $M "        if ([t n48IsUInt]) {   // bundle 19" "        if (0 && [t n48IsUInt]) {   // bundle 19" "the clear goes back to float32 for integer attachments (the uint branch is dead)"
cplant 4  $M "        wm = n48if_write_mask(fb, (f->flags & N48F_UINT) != 0, wm);   // bundle 19: the float magenta fallback shader must not write into an integer attachment" "        (void)fb;" "the fallback write-mask rule is dropped (float magenta bits written into the uint attachment)"
cplant 5  $M "v = stat(N48_NOUINT_FILE, &st) == 0;" "v = NO;" "the kill-file read is removed (/private/tmp/n48m-nouint does nothing)"
# ---- further breaks of the same four, different sites
cplant 6  $M "return ((n48_fmts[i].flags & N48F_UINT) && n48_nouint()) ? NULL : &n48_fmts[i];" "return &n48_fmts[i];" "n48_fmt() ignores the kill file (the file is read but nothing consults it)"
cplant 7  $M "#define N48F_UINT 4u" "#define N48F_UINT 2u" "the new flag collides with N48F_DS"
cplant 8  $M "n48if_clear_u(c.green, 16), n48if_clear_u(c.blue, 16)" "n48if_clear_u(c.green, 16), (uint32_t)c.blue" "one channel of the integer clear skips the Metal conversion"
cplant 10 $M "        } else
        _cvs[na] = (VkClearValue){ .color = { .float32" "        }
        _cvs[na] = (VkClearValue){ .color = { .float32" "the float32 fill is no longer the else arm: it runs after the uint fill and overwrites it"
cplant 11 $M "- (BOOL)n48IsUInt { return _fmt != NULL && (_fmt->flags & N48F_UINT) != 0; }" "- (BOOL)n48IsUInt { return NO; }" "n48IsUInt is always NO (the branch is unreachable)"
cplant 12 $M "wm = n48if_write_mask(fb, (f->flags & N48F_UINT) != 0, wm);" "wm = n48if_write_mask(0, (f->flags & N48F_UINT) != 0, wm);" "the mask rule is told the pipeline is never a fallback"
cplant 13 $M "#include \"n48_intfmt.h\"" "#include \"n48_texdesc.h\"" "n48_intfmt.h is no longer included"
# ---- the header's rules
cplant 14 $H "    return (uint32_t)v;                        // truncate toward zero" "    return (uint32_t)(v + 0.5);                // round" "the conversion rounds instead of truncating"
cplant 15 $H "    if (v >= (double)mx) return mx;            // saturate" "    if (v >= (double)mx) return (uint32_t)v;   // no saturation" "the conversion does not saturate"
cplant 16 $H "    if (!(v > 0.0)) return 0;                  // NaN, -0, negatives" "    if (v < 0.0) return 0;                     // negatives only" "NaN is not mapped to 0"
cplant 17 $H "return (fallback && is_int_attachment) ? 0u : wm; }" "return (fallback || is_int_attachment) ? 0u : wm; }" "the write mask is cleared for every integer attachment, even the real shader's"
cplant 18 $H "return (fallback && is_int_attachment) ? 0u : wm; }" "return (fallback && 0) ? 0u : wm; }" "the header's mask rule does nothing"
# ---- version
cplant 19 Info.plist "<key>CFBundleVersion</key>
	<string>20</string>" "<key>CFBundleVersion</key>
	<string>18</string>" "the bundle is still build 18"
# ---- diff guard: no change to scanout / present / M6 code
gplant 20 "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"1\"); }" "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"2\"); }" "a scanout function is edited"
gplant 21 "    pthread_mutex_lock(&N48S.mu);
    ((n48h_ctx *)c)->slept = n48_now() - t0;" "    pthread_mutex_lock(&N48S.mu);
    ((n48h_ctx *)c)->slept = n48_now() - t0 + 1;" "a hunk inside the M6 + present region"
gplant 22 "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "- (void)n48PoolDone { [self n48UCRelease]; if (_pser) n48_fence_close(_pser); }" "n48PoolDone loses its one allowed call (it must now be IDENTICAL to the base)"
echo "test-b19-plant: $total planted/baseline runs, $escaped escaped"
[ $escaped -eq 0 ]
