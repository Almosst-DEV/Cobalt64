// test-texdesc.c: host test of n48_texdesc.h (bundle 9, app crash study item 2: cube and 2D-array textures), plus source pins that tie the REAL Navi48Device.m to it.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-texdesc test-texdesc.c && /tmp/test-texdesc .
//   argv[1] = the directory holding Navi48Device.m and n48_texdesc.h (default ".").
// The rule this file enforces: every descriptor the bundle accepted BEFORE build 9 (2D, 1D, 1DArray, 3D) maps EXACTLY as before.  The old mapping is reproduced below
// VERBATIM from build 8 (Navi48Device.m N48Texture initWithDevice:descriptor:error:, the check, VkImageType, arrayLayers, view type, _layers, textureType, arrayLength, heap layers)
// and a grid over every field is compared with the new function.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n48_texdesc.h"
static int fails, checks;
#define CHECK(c, ...) do { int ok_ = (c) ? 1 : 0; checks++; printf("%s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define QUIET(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- build 8, verbatim (types spelled with the numeric values: 1D 0, 1DArray 1, 2D 2, 3D 7) ----
typedef struct { int accepted, imageType, arrayLayers, viewType, layersIvar, mtlType; unsigned long arrayLengthReported, heapLayers; } old_t;
static old_t old_map(unsigned long tt, unsigned long w, unsigned long h, unsigned long dd, unsigned long al, unsigned long sc, unsigned long mip) {
    (void)w; (void)h; old_t o; memset(&o, 0, sizeof o);
    int is1D = tt == 0 || tt == 1, is3D = tt == 7; int arr1D = tt == 1;
    unsigned long levels = mip ? mip : 1, depth = dd ? dd : 1;
    if (!(tt == 2 || is1D || is3D) || (is3D ? al != 1 : depth != 1) || sc != 1 ||
        (tt != 1 && al != 1) || al < 1 || (is1D && levels != 1)) { o.accepted = 0; return o; }
    o.accepted = 1;
    o.imageType = is1D ? 0 : is3D ? 2 : 1;                                     // VK_IMAGE_TYPE_1D / 3D / 2D
    o.arrayLayers = is1D ? (int)al : 1;
    o.viewType = is1D ? (arr1D ? 4 : 0) : is3D ? 2 : 1;                        // 1D_ARRAY / 1D / 3D / 2D
    o.layersIvar = is1D ? (int)al : 0;                                         // _layers
    o.mtlType = is1D ? (arr1D ? 1 : 0) : is3D ? 7 : 2;                         // textureType
    o.arrayLengthReported = is1D ? (unsigned long)o.layersIvar : 1;             // arrayLength
    o.heapLayers = tt == 1 ? al : 1;                                           // n48_heap_tex_sa
    return o;
}
static int cube_ok(unsigned long w, unsigned long h, unsigned long dd, unsigned long al, unsigned long sc, unsigned long use, n48td_t *o) { return n48td_map(N48TD_MTL_CUBE, w, h, dd, al, sc, 1, use, o); }

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); return b; }
static int count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    n48td_t t;
    // ---- the new types ----
    CHECK(cube_ok(64, 64, 1, 1, 1, 1, &t) && t.kind == N48TD_K_CUBE && t.imageType == N48TD_VK_IMAGE_2D && t.viewType == N48TD_VK_VIEW_CUBE && t.layers == 6 && t.layersIvar == 6 && t.flags == N48TD_VK_CREATE_CUBE_COMPATIBLE,
          "cube 64x64: VkImageType 2D, view CUBE, 6 layers, flag CUBE_COMPATIBLE");
    CHECK(!cube_ok(64, 32, 1, 1, 1, 1, &t) && t.why, "cube with w != h -> refused (\"%s\")", t.why ? t.why : "");
    CHECK(!cube_ok(32, 64, 1, 1, 1, 1, &t), "cube with w < h -> refused");
    CHECK(n48td_map(N48TD_MTL_CUBE, 64, 64, 1, 1, 1, 7, 1, &t) && t.levels == 7 && t.layers == 6, "cube with mips (7 levels of 64) -> accepted, still 6 layers");
    CHECK(!n48td_map(N48TD_MTL_CUBE, 64, 64, 1, 1, 4, 1, 1, &t), "cube with MSAA (4 samples) -> refused");
    CHECK(!cube_ok(64, 64, 1, 6, 1, 1, &t), "cube with arrayLength 6 -> refused (an array of cubes is CubeArray)");
    CHECK(!cube_ok(64, 64, 1, 0, 1, 1, &t), "cube with arrayLength 0 -> refused");
    CHECK(!cube_ok(64, 64, 4, 1, 1, 1, &t), "cube with depth 4 -> refused");
    CHECK(cube_ok(64, 64, 0, 1, 1, 1, &t), "cube with depth 0 (the unset default) -> accepted");
    CHECK(!cube_ok(64, 64, 1, 1, 1, N48TD_USE_SHADERWRITE | 1, &t) && t.why && strstr(t.why, "ShaderWrite"), "ShaderWrite on a cube -> refused with a ShaderWrite message (\"%s\")", t.why ? t.why : "");
    CHECK(cube_ok(64, 64, 1, 1, 1, N48TD_USE_RENDERTARGET | 1, &t), "RenderTarget on a cube -> accepted (per-layer 2D views)");
    CHECK(!n48td_map(N48TD_MTL_CUBEARRAY, 64, 64, 1, 2, 1, 1, 1, &t) && t.why && strstr(t.why, "cube-array"), "CubeArray -> refused with a clear message (\"%s\")", t.why ? t.why : "");
    CHECK(!n48td_map(N48TD_MTL_CUBEARRAY, 64, 64, 1, 1, 1, 1, 1, &t), "CubeArray with arrayLength 1 -> refused too");
    CHECK(n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 5, 1, 1, 1, &t) && t.kind == N48TD_K_2DARRAY && t.imageType == N48TD_VK_IMAGE_2D && t.viewType == N48TD_VK_VIEW_2D_ARRAY && t.layers == 5 && t.layersIvar == 5 && t.flags == 0,
          "2DArray 32x16 x5: VkImageType 2D, view 2D_ARRAY, 5 layers, no flags");
    CHECK(n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 1, 1, 1, 1, &t) && t.viewType == N48TD_VK_VIEW_2D_ARRAY && t.layers == 1, "2DArray with arrayLength 1 -> view type is still 2D_ARRAY");
    CHECK(!n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 0, 1, 1, 1, &t), "2DArray with arrayLength 0 -> refused");
    CHECK(!n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 3, 4, 1, 1, &t), "2DArray with MSAA -> refused");
    CHECK(!n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 3, 1, 1, N48TD_USE_SHADERWRITE, &t) && t.why && strstr(t.why, "ShaderWrite"), "ShaderWrite on a 2DArray -> refused");
    CHECK(n48td_map(N48TD_MTL_2DARRAY, 32, 16, 1, 3, 1, 5, 1, &t) && t.levels == 5, "2DArray with mips -> accepted");
    // bundle 10: 2D multisample textures exist; 2DMultisampleArray and TextureBuffer are still refused
    CHECK(n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 1, 4, 1, 4, &t) && t.kind == N48TD_K_2DMS && t.imageType == N48TD_VK_IMAGE_2D && t.viewType == N48TD_VK_VIEW_2D && t.layers == 1 && t.layersIvar == 0 && t.flags == 0 && t.samples == 4 && t.levels == 1,
          "2DMultisample 32x16, 4 samples, RenderTarget: VkImageType 2D, view 2D, 1 layer, no flags, samples 4");
    CHECK(n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 1, 8, 1, 5, &t) && t.samples == 8, "2DMultisample with 8 samples and ShaderRead|RenderTarget -> accepted by the descriptor map (the device decides 8)");
    CHECK(!n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 1, 1, 1, 4, &t) && t.why && strstr(t.why, "sampleCount >= 2"), "2DMultisample with sampleCount 1 -> refused (\"%s\")", t.why ? t.why : "");
    CHECK(!n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 2, 4, 1, 4, &t) && !n48td_map(N48TD_MTL_2DMS, 32, 16, 3, 1, 4, 1, 4, &t) && !n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 1, 4, 2, 4, &t), "2DMultisample with arrayLength 2, depth 3 or 2 mip levels -> refused");
    CHECK(!n48td_map(N48TD_MTL_2DMS, 32, 16, 1, 1, 4, 1, N48TD_USE_SHADERWRITE, &t) && t.why && strstr(t.why, "ShaderWrite"), "ShaderWrite on a multisample texture -> refused");
    CHECK(!n48td_map(N48TD_MTL_2DMSARRAY, 32, 16, 1, 1, 4, 1, 1, &t) && !n48td_map(N48TD_MTL_BUFFER, 32, 16, 1, 1, 1, 1, 1, &t), "2DMultisampleArray, TextureBuffer -> still refused");
    CHECK(n48td_mtl_type(N48TD_K_2DMS) == N48TD_MTL_2DMS && n48td_array_length(N48TD_K_2DMS, 0) == 1 && n48td_nlayers(0) == 1 && n48td_heap_layers(N48TD_MTL_2DMS, 1) == 1, "2DMS kind reports textureType 2DMultisample, arrayLength 1, one layer");
    { n48td_att_t a; CHECK(n48td_att_view(N48TD_K_2DMS, 0, 0, &a) && a.viewType == N48TD_VK_VIEW_2D && a.baseLayer == 0 && a.layerCount == 1 && !n48td_att_view(N48TD_K_2DMS, 0, 1, &a), "2DMS attachment view: a plain 2D view of layer 0"); }
    for (unsigned long tt = 0; tt < 12; tt++) { n48td_t u; if (tt == 4) continue; if (n48td_map(tt, 16, 16, 1, 1, 1, 1, 1, &u)) CHECK(u.samples == 1, "type %lu (not 2DMS): samples stay 1", tt); }
    CHECK(!n48td_map(99, 32, 16, 1, 1, 1, 1, 1, &t), "an unknown type -> refused");
    // helper functions
    CHECK(n48td_nlayers(0) == 1 && n48td_nlayers(1) == 1 && n48td_nlayers(6) == 6, "n48Layers: 0 -> 1, 6 -> 6");
    CHECK(n48td_mtl_type(N48TD_K_CUBE) == N48TD_MTL_CUBE && n48td_mtl_type(N48TD_K_2DARRAY) == N48TD_MTL_2DARRAY && n48td_mtl_type(N48TD_K_2D) == N48TD_MTL_2D && n48td_mtl_type(N48TD_K_3D) == N48TD_MTL_3D &&
          n48td_mtl_type(N48TD_K_1D) == N48TD_MTL_1D && n48td_mtl_type(N48TD_K_1DARRAY) == N48TD_MTL_1DARRAY, "textureType per kind (cube -> Cube, 2DArray -> 2DArray)");
    CHECK(n48td_array_length(N48TD_K_CUBE, 6) == 1 && n48td_array_length(N48TD_K_2DARRAY, 5) == 5 && n48td_array_length(N48TD_K_2D, 0) == 1, "arrayLength: cube 1, 2DArray 5, 2D 1");
    CHECK(n48td_base_layer(0, 5) == 5 && n48td_base_layer(1, 5) == 0 && n48td_base_layer(0, 0) == 0, "slice -> baseArrayLayer: 2D/cube/array slice 5 -> 5, 3D -> 0");
    CHECK(n48td_heap_layers(N48TD_MTL_CUBE, 1) == 6 && n48td_heap_layers(N48TD_MTL_2DARRAY, 4) == 4 && n48td_heap_layers(N48TD_MTL_1DARRAY, 3) == 3 && n48td_heap_layers(N48TD_MTL_2D, 1) == 1, "heap sizing counts the layers (cube 6, 2DArray 4, 1DArray 3, 2D 1)");
    { n48td_att_t a; n48td_t c; cube_ok(64, 64, 1, 1, 1, 1, &c);
      CHECK(n48td_att_view(N48TD_K_CUBE, c.layersIvar, 3, &a) && a.viewType == N48TD_VK_VIEW_2D && a.baseLayer == 3 && a.layerCount == 1, "cube attachment, layer 3: a 2D view, baseArrayLayer 3, layerCount 1");
      CHECK(n48td_att_view(N48TD_K_CUBE, c.layersIvar, 5, &a) && a.baseLayer == 5 && !n48td_att_view(N48TD_K_CUBE, c.layersIvar, 6, &a), "cube attachment: layer 5 exists, layer 6 does not (slice < n48Layers)");
      CHECK(n48td_att_view(N48TD_K_2DARRAY, 4, 3, &a) && a.baseLayer == 3 && !n48td_att_view(N48TD_K_2DARRAY, 4, 4, &a), "2DArray(4) attachment: layer 3 exists, layer 4 does not");
      CHECK(n48td_att_view(N48TD_K_2D, 0, 0, &a) && a.viewType == N48TD_VK_VIEW_2D && a.baseLayer == 0 && a.layerCount == 1 && !n48td_att_view(N48TD_K_2D, 0, 1, &a), "plain 2D attachment: layer 0 only (the old slice != 0 refusal)");
      CHECK(n48td_att_view(N48TD_K_1DARRAY, 3, 0, &a) && a.viewType == N48TD_VK_VIEW_1D_ARRAY && a.layerCount == 3 && a.baseLayer == 0, "1DArray attachment view: as before (1D_ARRAY, all 3 layers, base 0)");
      CHECK(n48td_att_view(N48TD_K_3D, 0, 0, &a) && a.viewType == N48TD_VK_VIEW_3D && a.layerCount == 1 && n48td_att_view(N48TD_K_1D, 1, 0, &a) && a.viewType == N48TD_VK_VIEW_1D && a.layerCount == 1, "3D and 1D attachment views: as before"); }

    // ---- the identity proof: every previously accepted descriptor maps exactly as before ----
    { static const unsigned long tts[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 99 }, dims[] = { 1, 4, 16, 64, 100, 16384 }, deps[] = { 0, 1, 2, 5 }, als[] = { 0, 1, 2, 3, 6, 255 }, scs[] = { 0, 1, 2, 4 }, mips[] = { 0, 1, 2, 3, 7 }, uses[] = { 0, 1, 3, 4, 5, 0x10 };
      long total = 0, oldAcc = 0, same = 0, newAcc = 0, oldRefusedNewAccepted = 0, oldRefusedNewAcceptedNonNewTypes = 0;
      for (size_t a = 0; a < sizeof tts / sizeof *tts; a++) for (size_t b = 0; b < sizeof dims / sizeof *dims; b++) for (size_t c = 0; c < sizeof dims / sizeof *dims; c++) for (size_t d = 0; d < sizeof deps / sizeof *deps; d++)
        for (size_t e = 0; e < sizeof als / sizeof *als; e++) for (size_t f = 0; f < sizeof scs / sizeof *scs; f++) for (size_t g = 0; g < sizeof mips / sizeof *mips; g++) for (size_t u = 0; u < sizeof uses / sizeof *uses; u++) {
            unsigned long tt = tts[a], w = dims[b], h = dims[c], dd = deps[d], al = als[e], sc = scs[f], mp = mips[g], us = uses[u];
            old_t o = old_map(tt, w, h, dd, al, sc, mp); n48td_t n; int acc = n48td_map(tt, w, h, dd, al, sc, mp, us, &n); total++;
            if (acc) newAcc++;
            if (o.accepted) {
                oldAcc++;
                int ok = acc && n.samples == 1 && (int)n.imageType == o.imageType && (int)n.layers == o.arrayLayers && (int)n.viewType == o.viewType && (int)n.layersIvar == o.layersIvar && (int)n.flags == 0 &&
                         (int)n48td_mtl_type(n.kind) == o.mtlType && n48td_array_length(n.kind, n.layersIvar) == o.arrayLengthReported && n48td_heap_layers(tt, al) == o.heapLayers &&
                         n48td_nlayers(n.layersIvar) == (o.layersIvar ? (unsigned)o.layersIvar : 1u) && n.levels == (mp ? mp : 1) && n.depth == (dd ? dd : 1) && n.is1D == (tt == 0 || tt == 1) && n.is3D == (tt == 7);
                { n48td_att_t av; ok = ok && n48td_att_view(n.kind, n.layersIvar, 0, &av) && (int)av.viewType == o.viewType && av.baseLayer == 0 && (int)av.layerCount == (o.imageType == 0 ? o.arrayLayers : 1); }
                QUIET(ok, "old descriptor type %lu %lux%lu depth %lu array %lu samples %lu mips %lu usage %lx no longer maps identically", tt, w, h, dd, al, sc, mp, us);
                same += ok;
            } else if (acc) { oldRefusedNewAccepted++; if (!(tt == 3 || tt == 4 || tt == 5)) oldRefusedNewAcceptedNonNewTypes++; }
        }
      CHECK(oldAcc > 1000 && same == oldAcc, "identity sweep over %ld descriptors: the old build accepted %ld, %ld map identically (type, VkImageType, arrayLayers, view type, _layers, textureType, arrayLength, heap layers, attachment view)", total, oldAcc, same);
      CHECK(oldRefusedNewAcceptedNonNewTypes == 0, "nothing the old build refused is newly accepted except cube (5), 2DArray (3) and (bundle 10) 2DMultisample (4): %ld newly accepted, %ld outside those types", oldRefusedNewAccepted, oldRefusedNewAcceptedNonNewTypes);
      CHECK(newAcc > oldAcc, "the new function accepts more than the old (%ld vs %ld)", newAcc, oldAcc); }
    // spot values of the old set, written out (so a wrong transcription of the reference cannot hide a wrong header)
    CHECK(n48td_map(2, 100, 50, 1, 1, 1, 3, 7, &t) && t.viewType == 1 && t.imageType == 1 && t.layers == 1 && t.layersIvar == 0 && t.flags == 0, "2D 100x50 mips 3: VkImageType 2D, view 2D, 1 layer, _layers 0, no flags");
    CHECK(n48td_map(1, 16384, 1, 1, 3, 1, 1, 1, &t) && t.viewType == 4 && t.imageType == 0 && t.layers == 3 && t.layersIvar == 3, "1DArray 16384 x3 (the WindowServer LUT): VkImageType 1D, view 1D_ARRAY, 3 layers");
    CHECK(n48td_map(0, 100, 1, 1, 1, 1, 1, 1, &t) && t.viewType == 0 && t.imageType == 0 && t.layers == 1 && t.layersIvar == 1, "1D 100: VkImageType 1D, view 1D, 1 layer, _layers 1");
    CHECK(n48td_map(7, 8, 8, 4, 1, 1, 2, 1, &t) && t.viewType == 2 && t.imageType == 2 && t.layers == 1 && t.layersIvar == 0 && t.depth == 4, "3D 8x8x4 mips 2: VkImageType 3D, view 3D, 1 layer");
    CHECK(!n48td_map(0, 100, 1, 1, 1, 1, 2, 1, &t) && !n48td_map(2, 8, 8, 1, 2, 1, 1, 1, &t) && !n48td_map(2, 8, 8, 2, 1, 1, 1, 1, &t) && !n48td_map(2, 8, 8, 1, 1, 4, 1, 1, &t), "still refused: 1D with mips, 2D with arrayLength 2, 2D with depth 2, 2D with MSAA");

    // ---- source pins on the REAL Navi48Device.m ----
    char *m = slurp(dir, "Navi48Device.m");
    CHECK(m != NULL, "Navi48Device.m is readable");
    if (m) {
        CHECK(strstr(m, "#include \"n48_texdesc.h\"") != NULL, "Navi48Device.m includes n48_texdesc.h");
        const char *init = strstr(m, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err {\n    self = [super init];\n    if (!self) return nil;\n    _dev = dev; _w = d.width;");
        const char *init_end = init ? strstr(init, "\n// A single-level, identity-swizzle view") : NULL;
        CHECK(init && init_end, "the N48Texture descriptor initialiser is found");
        if (init && init_end) {
            const char *map = strstr(init, "n48td_map((unsigned long)tt, _w, _h, d.depth, d.arrayLength, d.sampleCount, d.mipmapLevelCount, (unsigned long)d.usage, &ti)");
            CHECK(map && map < init_end, "the initialiser asks n48td_map with every descriptor field");
            CHECK(strstr(init, "n48_err(31, [NSString stringWithFormat:@\"%s (type %lu") != NULL, "a refusal is error 31 with the header's text");
            CHECK(strstr(init, "| ti.flags;") && strstr(init, "VkImageCreateFlags icf =") && strstr(init, "VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) | ti.flags;"), "the create flags include ti.flags (CUBE_COMPATIBLE)");
            const char *q1 = strstr(init, "vkGetPhysicalDeviceImageFormatProperties ? vkGetPhysicalDeviceImageFormatProperties(N48R.pd, _fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp)");
            const char *q2 = strstr(init, "qr = vkGetPhysicalDeviceImageFormatProperties(N48R.pd, _fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp);");
            CHECK(q1 && q2, "both vkGetPhysicalDeviceImageFormatProperties calls pass the create flags (icf)");
            CHECK(strstr(init, "ti.layers > ifp.maxArrayLayers") != NULL, "maxArrayLayers >= layers is required");
            CHECK(strstr(init, "(VkImageType)ti.imageType") && strstr(init, ".arrayLayers = ti.layers") && strstr(init, ".viewType = (VkImageViewType)ti.viewType") && strstr(init, "(uint32_t)_levels, 0, ti.layers }"),
                  "VkImageType, arrayLayers, the view type and the view's layerCount come from the header");
            CHECK(strstr(init, "_layers = ti.layersIvar; _tdk = ti.kind;") != NULL, "_layers and the kind are stored from the header");
            CHECK(!strstr(init, "is1D ? (uint32_t)_layers : 1"), "no hand-written layer count is left in the initialiser");
        }
        CHECK(strstr(m, "- (uint32_t)n48Layers { return _root ? [_root n48Layers] : n48td_nlayers((unsigned)_layers); }") != NULL, "n48Layers is the header's n48td_nlayers (cube 6, array N)");
        CHECK(strstr(m, "- (MTLTextureType)textureType { return (MTLTextureType)n48td_mtl_type(_tdk); }") != NULL, "textureType comes from the kind");
        CHECK(strstr(m, "- (NSUInteger)arrayLength { return n48td_array_length(_tdk, (unsigned)_layers); }") != NULL, "arrayLength comes from the kind (cube 1)");
        CHECK(count(m, "n48td_base_layer(self->_is3D, slice)") == 2 && strstr(m, "n48td_base_layer(is3, slice)") != NULL, "replaceRegion, getBytes and the blit copies (n48_bic) all use n48td_base_layer(slice) as the base array layer");
        CHECK(!strstr(m, "self->_is3D ? 0 : (uint32_t)slice") && !strstr(m, "is3 ? 0 : (uint32_t)slice"), "no hand-written slice -> layer expression is left");
        CHECK(strstr(m, "n48td_heap_layers((unsigned long)d.textureType, d.arrayLength)") != NULL, "heap texture sizing counts the layers through n48td_heap_layers");
        CHECK(strstr(m, "ca.slice >= [t n48Layers]") != NULL && !strstr(m, "ca.slice != 0"), "the render pass refuses slice >= n48Layers (not slice != 0)");
        CHECK(strstr(m, "[t n48AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice]") != NULL, "the render pass attaches the per-layer view of ca.slice");
        CHECK(strstr(m, "BOOL wholeImg = [t n48Levels] > 1 || [t n48Layers] > 1;") != NULL, "a layered texture is moved to the attachment layout as a whole");
        CHECK(strstr(m, "if (n48td_is_layered2d(root->_tdk)) VFAIL(98") != NULL, "views of cube / 2D-array textures are refused (their sliced views are not implemented)");
        CHECK(strstr(m, "if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72") != NULL &&
              strstr(m, "if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) BFAIL(121") != NULL, "IOSurface- and buffer-backed textures are still 2D only");
        CHECK(strstr(m, "_Static_assert((unsigned)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT == N48TD_VK_CREATE_CUBE_COMPATIBLE") != NULL && strstr(m, "(int)MTLTextureTypeCube == N48TD_MTL_CUBE") != NULL, "the header's numeric values are asserted against the real enums");
    }
    printf("test-texdesc: %d checks, %d failed\n", checks, fails); return fails ? 1 : 0;
}
