#!/bin/zsh
# test-b9-plant.sh - planted breaks for bundle build 9 (app crash study items 1 and 2): n48_cienv.h / test-cienv.c, n48_texdesc.h / test-texdesc.c / test-vkimage.c.
# For each plant: copy the REAL headers, Navi48Device.m and tests into a scratch tree, apply ONE break (the script first proves the text was there exactly once), build and run the
# named test(s) and demand that at least one FAILS (a compile error counts).  A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  tools/native/navi48metal/test-b9-plant.sh        (test-vkimage needs MoltenVK in /opt/homebrew; without it those runs are skipped and counted as "no vulkan")
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b9-plant.XXXXXX")"
case "$SCR" in */b9-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0; HAVEVK=0
[ -f /opt/homebrew/lib/libvulkan.dylib ] && [ -f /opt/homebrew/include/vulkan/vulkan.h ] && HAVEVK=1
fresh() { rm -rf -- "$SCR/w"; mkdir -p "$SCR/w"; cp "$D"/n48_cienv.h "$D"/n48_texdesc.h "$D"/Navi48Device.m "$D"/test-cienv.c "$D"/test-texdesc.c "$D"/test-vkimage.c "$SCR/w/"; }
run_tests() {   # prints one line per failed test into $OUT; returns 0 when everything passed
  OUT=""; local bad=0 rc
  ( cd "$SCR/w" && cc -O1 -Wall -Wextra -Werror -o t-cienv test-cienv.c ) >/dev/null 2>&1 || { OUT="test-cienv: compile error"; bad=1; }
  [ $bad -eq 0 ] && { ( cd "$SCR/w" && ./t-cienv . ) >"$SCR/o1" 2>&1 || { OUT="test-cienv: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o1" | cut -c1-110)"; bad=1; }; }
  ( cd "$SCR/w" && cc -O1 -Wall -Wextra -Werror -o t-texdesc test-texdesc.c ) >/dev/null 2>&1 || { OUT="$OUT | test-texdesc: compile error"; bad=1; }
  if [ -x "$SCR/w/t-texdesc" ]; then ( cd "$SCR/w" && ./t-texdesc . ) >"$SCR/o2" 2>&1 || { OUT="$OUT | test-texdesc: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o2" | cut -c1-110)"; bad=1; }; fi
  if [ $HAVEVK -eq 1 ]; then
    ( cd "$SCR/w" && clang -I/opt/homebrew/include test-vkimage.c -L/opt/homebrew/lib -lvulkan -o t-vkimage ) >/dev/null 2>&1 || { OUT="$OUT | test-vkimage: compile error"; bad=1; }
    if [ -x "$SCR/w/t-vkimage" ]; then ( cd "$SCR/w" && ./t-vkimage ) >"$SCR/o3" 2>&1 || { OUT="$OUT | test-vkimage: $(/usr/bin/grep -m1 -E '^FAIL' "$SCR/o3" | cut -c1-110)"; bad=1; }; fi
  fi
  return $bad
}
plant() {   # plant <id> <file> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/w/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if run_tests; then echo "PLANT $id: $desc: *** ESCAPED (every test passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by ${OUT# | }"; fi
}
fresh; total=$((total+1)); if run_tests; then echo "BASELINE: the unmodified scratch copy passes (cienv, texdesc$([ $HAVEVK -eq 1 ] && echo ', vkimage' || echo '; no vulkan: vkimage skipped'))"; else echo "BASELINE: FAILED: $OUT"; escaped=$((escaped+1)); fi
C=n48_cienv.h; T=n48_texdesc.h; M=Navi48Device.m
# ---- item 1: the Core Image switch ----
plant 1  $C "    if (get(N48CE_VAR) != NULL) return N48CE_ALREADY_SET;
    return set(N48CE_VAR, N48CE_VALUE, 0) == 0 ? N48CE_SET : N48CE_SET_FAILED;" "    (void)get; return set(N48CE_VAR, N48CE_VALUE, 1) == 0 ? N48CE_SET : N48CE_SET_FAILED;" "CIENV 1: a preset value (\"1\") is overwritten with \"0\""
plant 2  $C "    if (get(N48CE_VAR) != NULL) return N48CE_ALREADY_SET;" "    if (get(N48CE_VAR) != NULL && get(N48CE_VAR)[0] == '0') return N48CE_ALREADY_SET;" "CIENV 2: only a preset \"0\" is respected (a preset \"1\" is replaced)"
plant 3  $C "#define N48CE_VALUE \"0\"" "#define N48CE_VALUE \"1\"" "CIENV 3: the variable is set to \"1\" (the crashing path stays on)"
plant 4  $C "return set(N48CE_VAR, N48CE_VALUE, 0) == 0" "return set(N48CE_VAR, N48CE_VALUE, 1) == 0" "CIENV 4: setenv overwrites (a value that appeared between the check and the set is lost)"
plant 5  $M "__attribute__((constructor)) static void n48_cienv_ctor(void) {" "static void n48_cienv_ctor(void) {" "CIENV 5: the constructor attribute is removed (nothing ever runs it; the PC leg (a) would crash)"
plant 6  $M "    int r = n48ce_apply((n48ce_getenv_fn)getenv, setenv);" "    setenv(N48CE_VAR, N48CE_VALUE, 1); int r = n48ce_apply((n48ce_getenv_fn)getenv, setenv);" "CIENV 6: the constructor overwrites by itself before asking the pure function"
plant 7  $M "    int r = n48ce_apply((n48ce_getenv_fn)getenv, setenv);" "    int r = N48CE_SET;" "CIENV 7: the constructor never calls n48ce_apply"
# ---- item 2: cube and 2D-array textures ----
plant 10 $T "o->flags = N48TD_VK_CREATE_CUBE_COMPATIBLE; o->layersIvar = N48TD_CUBE_FACES;" "o->flags = 0; o->layersIvar = N48TD_CUBE_FACES;" "TEX 10: VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT is dropped"
plant 11 $T "static inline uint32_t n48td_base_layer(int is3D, unsigned long slice) { return is3D ? 0u : (uint32_t)slice; }" "static inline uint32_t n48td_base_layer(int is3D, unsigned long slice) { (void)is3D; (void)slice; return 0u; }" "TEX 11: uploads always go to slice 0 (header)"
plant 12 $M "
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)lvl, n48td_base_layer(self->_is3D, slice), 1 }" "
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)lvl, 0, 1 }" "TEX 12: replaceRegion always uploads to slice 0 (bundle)"
plant 13 $T "static inline unsigned n48td_nlayers(unsigned layersIvar) { return layersIvar ? layersIvar : 1; }" "static inline unsigned n48td_nlayers(unsigned layersIvar) { (void)layersIvar; return 1; }" "TEX 13: n48Layers returns 1 (header)"
plant 14 $M "- (uint32_t)n48Layers { return _root ? [_root n48Layers] : n48td_nlayers((unsigned)_layers); }" "- (uint32_t)n48Layers { return _root ? [_root n48Layers] : 1; }" "TEX 14: n48Layers returns 1 (bundle: slice 5 refused, barriers cover one layer)"
plant 15 $T "        if (w != h) { o->why = \"a cube texture must be square (width must equal height)\"; return 0; }
" "        (void)w; (void)h;
" "TEX 15: a cube with w != h is accepted"
plant 16 $T "is1D ? (tt == N48TD_MTL_1DARRAY ? N48TD_VK_VIEW_1D_ARRAY : N48TD_VK_VIEW_1D)" "is1D ? (N48TD_VK_VIEW_1D)" "TEX 16: an old accepted 1DArray descriptor gets the 1D view type"
plant 17 $T "o->layers = is1D ? (unsigned)arrayLength : 1; o->layersIvar = is1D ? (unsigned)arrayLength : 0; o->flags = 0;" "o->layers = is1D ? (unsigned)arrayLength : 1; o->layersIvar = is1D ? (unsigned)arrayLength : 0; o->flags = N48TD_VK_CREATE_CUBE_COMPATIBLE;" "TEX 17: every old 2D / 1D / 3D descriptor now carries the cube-compatible flag"
plant 18 $T "(tt != N48TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (is1D && levels != 1)) {" "(tt != N48TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (levels != 1)) {" "TEX 18: an old 2D texture with mips is refused"
plant 19 $T "        if (sampleCount != 1) { o->why = \"multisampled cube textures are not supported\"; return 0; }
" "" "TEX 19: a multisampled cube is accepted"
plant 20 $T "    if (tt == N48TD_MTL_CUBEARRAY) { o->why" "    if (0) { o->why" "TEX 20: CubeArray is no longer refused"
plant 21 $T "        if (usage & N48TD_USE_SHADERWRITE) { o->why = \"ShaderWrite on a cube texture" "        if (0) { o->why = \"ShaderWrite on a cube texture" "TEX 21: ShaderWrite on a cube is accepted"
plant 22 $T "        if (arrayLength < 1) { o->why = \"a 2D-array texture needs arrayLength >= 1\"; return 0; }
" "" "TEX 22: a 2DArray with arrayLength 0 is accepted"
plant 23 $T "o->viewType = N48TD_VK_VIEW_2D_ARRAY; o->layers = (unsigned)arrayLength;" "o->viewType = arrayLength > 1 ? N48TD_VK_VIEW_2D_ARRAY : N48TD_VK_VIEW_2D; o->layers = (unsigned)arrayLength;" "TEX 23: a 2DArray of length 1 gets a plain 2D view"
plant 24 $T "if (n48td_is_layered2d(kind)) { a->viewType = N48TD_VK_VIEW_2D; a->baseLayer = (unsigned)layer; a->layerCount = 1; return 1; }" "if (n48td_is_layered2d(kind)) { a->viewType = N48TD_VK_VIEW_2D; a->baseLayer = 0; a->layerCount = 1; return 1; }" "TEX 24: every per-layer attachment view is layer 0"
plant 25 $T "    if (layer >= n) return 0;" "    (void)n;" "TEX 25: the attachment view accepts a layer past the end"
plant 26 $M "ca.slice >= [t n48Layers]" "ca.slice != 0" "TEX 26: the render pass still refuses every slice != 0"
plant 27 $M "[t n48AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice]" "[t n48AttViewLevel:(uint32_t)lvl layer:0]" "TEX 27: the render pass always attaches layer 0"
plant 28 $M "n48td_heap_layers((unsigned long)d.textureType, d.arrayLength)" "(d.textureType == MTLTextureType1DArray ? d.arrayLength : 1)" "TEX 28: heap sizing ignores cube / array layers"
plant 29 $M "| ti.flags;" ";" "TEX 29: the create flags never reach the image-format query and vkCreateImage"
plant 30 $M "    if (n48td_is_layered2d(root->_tdk)) VFAIL(98, @\"views of cube / 2D-array textures are not implemented\");
" "" "TEX 30: a 2D view of a cube root is built (its sliced view is not implemented)"
plant 31 $M "- (NSUInteger)arrayLength { return n48td_array_length(_tdk, (unsigned)_layers); }" "- (NSUInteger)arrayLength { return _layers ? _layers : 1; }" "TEX 31: a cube reports arrayLength 6"
plant 32 $M "- (MTLTextureType)textureType { return (MTLTextureType)n48td_mtl_type(_tdk); }" "- (MTLTextureType)textureType { return MTLTextureType2D; }" "TEX 32: textureType returns 2D for everything"
plant 33 $M "if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72" "if (d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72" "TEX 33: an IOSurface-backed texture accepts any texture type"
echo "test-b9-plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
