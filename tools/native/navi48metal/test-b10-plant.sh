#!/bin/zsh
# test-b10-plant.sh - planted breaks for bundle build 10 (depth/stencil and MSAA): n48_depth.h / test-depth.c / test-vkdepth.c, n48_texdesc.h (2DMS) / test-texdesc.c.
# For each plant: copy the REAL headers, Navi48Device.m and tests into a scratch tree, apply ONE break (the script first proves the text was there exactly once), build and run the
# named tests and demand that at least one FAILS (a compile error counts).  A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  tools/native/navi48metal/test-b10-plant.sh      (test-vkdepth needs MoltenVK in /opt/homebrew; without it that run is skipped and counted as "no vulkan")
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b10-plant.XXXXXX")"
case "$SCR" in */b10-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0; HAVEVK=0
[ -f /opt/homebrew/lib/libvulkan.dylib ] && [ -f /opt/homebrew/include/vulkan/vulkan.h ] && HAVEVK=1
fresh() { rm -rf -- "$SCR/w"; mkdir -p "$SCR/w"; cp "$D"/n48_depth.h "$D"/n48_texdesc.h "$D"/test-depth-shaders.h "$D"/Navi48Device.m "$D"/test-depth.c "$D"/test-texdesc.c "$D"/test-vkdepth.c "$SCR/w/"; }
run_tests() {   # prints one line per failed test into $OUT; returns 0 when everything passed
  OUT=""; local bad=0
  ( cd "$SCR/w" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-depth test-depth.c ) >/dev/null 2>&1 || { OUT="test-depth: compile error"; bad=1; }
  if [ -x "$SCR/w/t-depth" ]; then ( cd "$SCR/w" && ./t-depth . ) >"$SCR/o1" 2>&1 || { OUT="test-depth: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o1" | cut -c1-110)"; bad=1; }; fi
  ( cd "$SCR/w" && cc -O1 -Wall -Wextra -Werror -o t-texdesc test-texdesc.c ) >/dev/null 2>&1 || { OUT="$OUT | test-texdesc: compile error"; bad=1; }
  if [ -x "$SCR/w/t-texdesc" ]; then ( cd "$SCR/w" && ./t-texdesc . ) >"$SCR/o2" 2>&1 || { OUT="$OUT | test-texdesc: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o2" | cut -c1-110)"; bad=1; }; fi
  if [ $HAVEVK -eq 1 ]; then
    ( cd "$SCR/w" && clang -I/opt/homebrew/include test-vkdepth.c -L/opt/homebrew/lib -lvulkan -o t-vkdepth ) >/dev/null 2>&1 || { OUT="$OUT | test-vkdepth: compile error"; bad=1; }
    if [ -x "$SCR/w/t-vkdepth" ]; then ( cd "$SCR/w" && ./t-vkdepth ) >"$SCR/o3" 2>&1 || { OUT="$OUT | test-vkdepth: $(/usr/bin/grep -m1 -E '^FAIL' "$SCR/o3" | cut -c1-110)"; bad=1; }; fi
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
fresh; total=$((total+1)); if run_tests; then echo "BASELINE: the unmodified scratch copy passes (depth, texdesc$([ $HAVEVK -eq 1 ] && echo ', vkdepth' || echo '; no vulkan: vkdepth skipped'))"; else echo "BASELINE: FAILED: $OUT"; escaped=$((escaped+1)); fi
H=n48_depth.h; T=n48_texdesc.h; M=Navi48Device.m
# ---- the briefed breaks ----
plant 1  $H "static inline unsigned n48dp_barrier_aspect(unsigned aspects) { return aspects ? aspects : N48DP_ASP_COLOR; }" "static inline unsigned n48dp_barrier_aspect(unsigned aspects) { (void)aspects; return N48DP_ASP_COLOR; }" "WRONG ASPECT on a depth barrier (header): every barrier names COLOR"
plant 2  $M "n48dp_barrier_aspect([t n48Aspects]), 0, [t n48Levels], 0, [t n48Layers] } };" "VK_IMAGE_ASPECT_COLOR_BIT, 0, [t n48Levels], 0, [t n48Layers] } };" "WRONG ASPECT on a depth barrier (bundle): n48_tex_to uses COLOR"
plant 3  $H "if (depthWrite) k |= 1u << 3; }" "(void)depthWrite; }" "DEPTH WRITE ignored (header): the write bit is never in the key"
plant 4  $M "return n48ds_key(_dpf != MTLPixelFormatInvalid, _spf != MTLPixelFormatInvalid, di.cmp, di.write, &di.f, &di.b);" "return n48ds_key(_dpf != MTLPixelFormatInvalid, _spf != MTLPixelFormatInvalid, di.cmp, 0, &di.f, &di.b);" "DEPTH WRITE ignored (bundle): the bound state's write flag is replaced by 0"
plant 5  $H "case 3: return N48DS_OP_INC_CLAMP; case 4: return N48DS_OP_DEC_CLAMP;" "case 3: return N48DS_OP_DEC_CLAMP; case 4: return N48DS_OP_INC_CLAMP;" "STENCIL OP SWAPPED: IncrementClamp <-> DecrementClamp"
plant 6  $H "case 6: return N48DS_OP_INC_WRAP; case 7: return N48DS_OP_DEC_WRAP;" "case 6: return N48DS_OP_DEC_WRAP; case 7: return N48DS_OP_INC_WRAP;" "STENCIL OP SWAPPED: IncrementWrap <-> DecrementWrap"
plant 7  $H "o->front = (n48ds_vkface_t){ f & 7u, (f >> 3) & 7u, (f >> 9) & 7u, (f >> 6) & 7u };" "o->front = (n48ds_vkface_t){ f & 7u, (f >> 3) & 7u, (f >> 6) & 7u, (f >> 9) & 7u };" "STENCIL OP SWAPPED: the front face's pass and depth-fail operations are exchanged on decode"
plant 8  $H "o->resolve = 1; return 1;" "o->resolve = 0; return 1;" "MSAA RESOLVE SKIPPED (header): a resolve store action resolves nothing"
plant 9  $M "    [self n48ResolveAll];   // bundle 10: multisample resolve, outside the pass
" "" "MSAA RESOLVE SKIPPED (bundle): endEncoding never resolves"
plant 10 $M "        if (!sat.resolve) continue;
        if (![self n48CheckResolve:i])" "        continue;
        if (![self n48CheckResolve:i])" "MSAA RESOLVE SKIPPED (bundle): n48ResolveAll continues before every resolve"
plant 11 $M "    [self n48ResolveAll];   // bundle 10: multisample resolve, outside the pass
    n48_full_barrier([_cb vk]);" "    n48_full_barrier([_cb vk]);
    [self n48ResolveAll];" "MSAA RESOLVE after the barrier (ordering)"
plant 12 $H "return (formatCounts & b) && (deviceMask & b) ? 1 : 0;" "return ((formatCounts | 0xFu) & b) && (deviceMask & b) ? 1 : 0;" "SAMPLE COUNT 4 answered for an unsupported FORMAT (header): the format's own counts are ignored"
plant 13 $M "if (!n48ms_texture_ok(ti.samples, fc," "if (0 && !n48ms_texture_ok(ti.samples, fc," "SAMPLE COUNT 4 answered for an unsupported format (bundle): the texture never asks n48ms_texture_ok"
plant 14 $H "    if (!haveLimits) return n == 4;" "    if (!haveLimits) return 1;" "SAMPLE COUNT answered YES without the device limits (every count)"
plant 15 $M "return n48ms_device_supports((unsigned long)n, N48R.ok ? 1 : 0, m, m) ? YES : NO; }" "return n <= 8; }" "supportsSampleCount: answers YES for any count <= 8"
plant 16 $T "o->is3D = is3D; o->samples = 1;" "o->is3D = is3D; o->samples = 4;" "AN OLD ACCEPTED DESCRIPTOR mapped differently: every old texture now carries 4 samples"
plant 17 $T "(tt != N48TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (is1D && levels != 1)) {" "(tt != N48TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (levels != 1)) {" "AN OLD ACCEPTED DESCRIPTOR refused: a 2D texture with mips"
plant 18 $T "case N48TD_K_2DARRAY: return N48TD_MTL_2DARRAY; case N48TD_K_2DMS: return N48TD_MTL_2DMS;" "case N48TD_K_2DARRAY: return N48TD_MTL_2DARRAY; case N48TD_K_2DMS: return N48TD_MTL_2D;" "textureType of a multisample texture reports 2D"
plant 19 $H "    case N48DP_MTL_D24S8: case N48DP_MTL_X24S8: o->why = \"Depth24Unorm_Stencil8 is not supported (AMD has no D24S8 on this path; use Depth32Float_Stencil8)\"; return 0;" "    case N48DP_MTL_D24S8: o->vk = N48DP_VK_D32S8; o->aspects = N48DP_ASP_DEPTH | N48DP_ASP_STENCIL; o->bpp = 8; return 1;
    case N48DP_MTL_X24S8: o->why = \"Depth24Unorm_Stencil8 is not supported (AMD has no D24S8 on this path)\"; return 0;" "D24S8 answered YES (header): Depth24Unorm_Stencil8 maps to the D32S8 format"
plant 20 $M "- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }" "- (BOOL)isDepth24Stencil8PixelFormatSupported { return YES; }" "D24S8 answered YES (bundle): isDepth24Stencil8PixelFormatSupported"
plant 21 $M "{ MTLPixelFormatA8Unorm,         VK_FORMAT_R8_UNORM,                 1, N48F_A8 }," "{ MTLPixelFormatA8Unorm,         VK_FORMAT_R8_UNORM,                 1, N48F_A8 },
    { MTLPixelFormatDepth32Float,    VK_FORMAT_D32_SFLOAT,               4, 0 }," "a depth format leaks into the COLOUR format table (colour targets, IOSurface and buffer textures would accept it)"
plant 22 $M "{ MTLPixelFormatDepth32Float_Stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, N48F_DS }," "{ MTLPixelFormatDepth32Float_Stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, N48F_DS },
    { MTLPixelFormatDepth24Unorm_Stencil8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, N48F_DS }," "D24S8 added to the depth format table of the bundle"
# ---- more breaks around the same pieces ----
plant 23 $H "    if (storage != N48DP_ST_PRIVATE) {" "    if (0 && storage != N48DP_ST_PRIVATE) {" "a Shared / Managed depth texture is accepted"
plant 24 $H "unsigned dl = !hd ? N48VK_LOAD_DONT_CARE : !hasDepthDesc ? N48VK_LOAD : depthLoad == N48LA_CLEAR ? N48VK_CLEAR :" "unsigned dl = !hd ? N48VK_LOAD_DONT_CARE : !hasDepthDesc ? N48VK_LOAD : depthLoad == N48LA_CLEAR ? N48VK_LOAD :" "a depth attachment with loadAction Clear LOADs instead"
plant 25 $H "unsigned sl = !hs ? N48VK_LOAD_DONT_CARE : !hasStencilDesc ? N48VK_LOAD :" "unsigned sl = !hs ? N48VK_LOAD_DONT_CARE : !hasStencilDesc ? N48VK_LOAD_DONT_CARE :" "the stencil of a D32S8 texture attached as depth only is discarded"
plant 26 $H "    if (!hasDepthAtt && !hasStencilAtt) return 0;" "    if (0) return 0;" "a colour-only pipeline gets a non-zero depth/stencil key"
plant 27 $H "    if (o->hasDS) {
        o->depthBias" "    if (1) {
        o->depthBias" "a colour-only pipeline gets the stencil / bias dynamic states"
plant 28 $H "o->samples = (unsigned)n; o->alphaToCoverage" "o->samples = 1; o->alphaToCoverage" "a 4-sample pipeline is built with 1 sample"
plant 29 $H "case 1: return N48DS_CMP_LESS; case 2: return N48DS_CMP_EQUAL;" "case 1: return N48DS_CMP_EQUAL; case 2: return N48DS_CMP_LESS;" "depth compare Less <-> Equal swapped"
plant 30 $H "static inline unsigned n48dp_view_aspect(unsigned aspects) { return (aspects & N48DP_ASP_DEPTH) ? N48DP_ASP_DEPTH : N48DP_ASP_STENCIL; }" "static inline unsigned n48dp_view_aspect(unsigned aspects) { return (aspects & N48DP_ASP_DEPTH) ? N48DP_ASP_STENCIL : N48DP_ASP_DEPTH; }" "the sampling view of a depth texture carries the stencil aspect"
plant 31 $H "static inline unsigned n48dp_copy_aspect(unsigned aspects) { return (aspects == N48DP_ASP_DEPTH || aspects == N48DP_ASP_STENCIL) ? aspects : 0; }" "static inline unsigned n48dp_copy_aspect(unsigned aspects) { return aspects; }" "a buffer copy of the combined depth/stencil format is allowed"
plant 32 $H "if (!attachmentIsMS) { o->why = \"a multisample resolve of a single-sample attachment\"; return 0; }" "if (0 && !attachmentIsMS) { o->why = \"a multisample resolve of a single-sample attachment\"; return 0; }" "a resolve of a single-sample attachment is accepted"
plant 33 $H "if (srcFmt != dstFmt) {" "if (0 && srcFmt != dstFmt) {" "a resolve texture of another pixel format is accepted"
plant 34 $M "[_pso n48DSVk] != _dsVkFmt || [_pso n48SampleCount] != _encSamples" "0 || [_pso n48SampleCount] != _encSamples" "a draw no longer checks the pipeline's depth/stencil format against the pass's"
plant 35 $M "- (void)setStencilReferenceValue:(uint32_t)v { _sref[0] = _sref[1] = v; }" "- (void)setStencilReferenceValue:(uint32_t)v { (void)v; }" "setStencilReferenceValue: ignored"
plant 36 $M "_cvs[na] = (VkClearValue){ .depthStencil = { (float)da.clearDepth, (uint32_t)sa.clearStencil } };" "_cvs[na] = (VkClearValue){ .depthStencil = { 1.0f, (uint32_t)sa.clearStencil } };" "clearDepth ignored (always 1.0)"
plant 37 $M "            if (_begun) { a2[_na].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a2[_na].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; }" "" "a continuation pass does not LOAD the depth/stencil"
plant 38 $M "NSNumber *k = @((uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16) | ((uint64_t)dk << 24));" "NSNumber *k = @((uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16));" "pipeline variants ignore the depth/stencil state (one variant for every state)"
plant 39 $M "uint32_t fkey = (uint32_t)front | (dk << 1);" "uint32_t fkey = (uint32_t)front;" "the last-answer pipeline cache ignores the depth/stencil state"
plant 40 $M ".arrayLayers = ti.layers, .samples = (VkSampleCountFlagBits)ti.samples," ".arrayLayers = ti.layers, .samples = VK_SAMPLE_COUNT_1_BIT," "a multisample texture is created with 1 sample"
plant 41 $M "isDS ? n48dp_view_aspect(_aspects) : VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, ti.layers }" "VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, ti.layers }" "the sampling view of a depth texture names the COLOR aspect"
plant 42 $M "if ([t n48Samples] > 1) { [self n48Fail:[NSString stringWithFormat:@\"%s texture(%u): a %lu-sample" "if (0) { [self n48Fail:[NSString stringWithFormat:@\"%s texture(%u): a %lu-sample" "sampling a multisample texture is allowed"
plant 43 $M ".imageSubresource = { [t n48CopyAspect], (uint32_t)level, n48td_base_layer(is3, slice), 1 }," ".imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, n48td_base_layer(is3, slice), 1 }," "blit buffer copies of a depth texture name the COLOR aspect"
plant 44 $M "n48rp_ds(asp, dtI != nil, (unsigned)da.loadAction, stI != nil, (unsigned)sa.loadAction, [dt layout] == VK_IMAGE_LAYOUT_UNDEFINED, &ops);" "n48rp_ds(asp, dtI != nil, N48LA_LOAD, stI != nil, N48LA_LOAD, [dt layout] == VK_IMAGE_LAYOUT_UNDEFINED, &ops);" "the depth/stencil attachment's loadAction is ignored"
plant 45 $M "k.na = na + nd; k.fetch = (fetch ? 1u : 0u) | (nd << 1);" "k.na = na; k.fetch = (fetch ? 1u : 0u);" "the render-pass cache key forgets the depth attachment"
plant 46 $M "vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]); vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);" "vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[1]); vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[0]);" "front and back stencil reference values swapped at the draw"
plant 47 $M ".depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp" ".depthWriteEnable = VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp" "the Vulkan pipeline never enables depth writes"
echo "test-b10-plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
