// n48_texdesc.h (bundle 9, app crash study item 2): the pure mapping from a Metal texture descriptor to the Vulkan image, so cube and 2D-array textures can exist.
// Pure C (no Metal / Vulkan headers: the numeric values are asserted against the real enums in Navi48Device.m with _Static_assert), host-tested by test-texdesc.c and test-vkimage.c.
// an internal design note, "App crash study", item (2): VectorKit (Photos, Maps) creates MTLTextureTypeCube textures and aborts on nil.
//
// Rule that binds this file: every descriptor the bundle accepted before build 9 (2D, 1D, 1DArray, 3D) maps EXACTLY as before; test-texdesc.c sweeps a grid against a verbatim copy of the old check.
#ifndef N48_TEXDESC_H
#define N48_TEXDESC_H
#include <stdint.h>
#include <stddef.h>

// MTLTextureType values (asserted in Navi48Device.m)
enum { N48TD_MTL_1D = 0, N48TD_MTL_1DARRAY = 1, N48TD_MTL_2D = 2, N48TD_MTL_2DARRAY = 3, N48TD_MTL_2DMS = 4, N48TD_MTL_CUBE = 5, N48TD_MTL_CUBEARRAY = 6, N48TD_MTL_3D = 7, N48TD_MTL_2DMSARRAY = 8, N48TD_MTL_BUFFER = 9 };
// MTLTextureUsage bits
enum { N48TD_USE_SHADERWRITE = 2, N48TD_USE_RENDERTARGET = 4 };
// VkImageType / VkImageViewType values and the create flag (asserted in Navi48Device.m)
enum { N48TD_VK_IMAGE_1D = 0, N48TD_VK_IMAGE_2D = 1, N48TD_VK_IMAGE_3D = 2 };
enum { N48TD_VK_VIEW_1D = 0, N48TD_VK_VIEW_2D = 1, N48TD_VK_VIEW_3D = 2, N48TD_VK_VIEW_CUBE = 3, N48TD_VK_VIEW_1D_ARRAY = 4, N48TD_VK_VIEW_2D_ARRAY = 5 };
#define N48TD_VK_CREATE_CUBE_COMPATIBLE 0x10u
// texture kind stored by the texture object; 0 is the plain 2D texture so a zero-initialised object (IOSurface-, buffer-backed and view textures) stays 2D
enum { N48TD_K_2D = 0, N48TD_K_1D = 1, N48TD_K_1DARRAY = 2, N48TD_K_3D = 3, N48TD_K_CUBE = 4, N48TD_K_2DARRAY = 5, N48TD_K_2DMS = 6 };   // bundle 10: 2DMS
#define N48TD_CUBE_FACES 6u

typedef struct {
    int ok;                 // 1: the descriptor is accepted
    const char *why;        // refusal text (ok == 0)
    unsigned kind;          // N48TD_K_*
    unsigned imageType;     // N48TD_VK_IMAGE_*
    unsigned viewType;      // N48TD_VK_VIEW_*
    unsigned layers;        // VkImageCreateInfo.arrayLayers and the sampling view's layerCount
    unsigned flags;         // extra VkImageCreateFlags (cube-compatible)
    unsigned layersIvar;    // the value the texture stores in _layers (0 = not a layered type: n48Layers() is then 1)
    unsigned samples;       // bundle 10: VkImageCreateInfo.samples (1 for everything but a 2D multisample texture)
    unsigned levels;        // mip levels (0 -> 1)
    unsigned depth;         // depth (0 -> 1)
    int is1D, is3D;
} n48td_t;

// d_* are the raw descriptor fields.  usage is MTLTextureUsage.  Returns 1 when accepted (and fills *o), else 0 with o->why set.
static inline int n48td_map(unsigned long tt, unsigned long w, unsigned long h, unsigned long depthRaw, unsigned long arrayLength, unsigned long sampleCount,
                            unsigned long mipLevelsRaw, unsigned long usage, n48td_t *o) {
    unsigned long levels = mipLevelsRaw ? mipLevelsRaw : 1, depth = depthRaw ? depthRaw : 1;
    int is1D = tt == N48TD_MTL_1D || tt == N48TD_MTL_1DARRAY, is3D = tt == N48TD_MTL_3D;
    o->ok = 0; o->why = NULL; o->kind = N48TD_K_2D; o->imageType = N48TD_VK_IMAGE_2D; o->viewType = N48TD_VK_VIEW_2D; o->layers = 1; o->flags = 0; o->layersIvar = 0;
    o->levels = (unsigned)levels; o->depth = (unsigned)depth; o->is1D = is1D; o->is3D = is3D; o->samples = 1;
    if (tt == N48TD_MTL_CUBEARRAY) { o->why = "cube-array textures (MTLTextureTypeCubeArray) are not supported; only cube (6 faces) and 2D-array textures exist"; return 0; }
    if (tt == N48TD_MTL_2DMS) {   // bundle 10: MSAA colour / depth targets.  The sample count itself (2, 4, 8) is checked against the device in Navi48Device.m (n48_depth.h n48ms_texture_ok).
        if (sampleCount < 2) { o->why = "a 2D multisample texture needs sampleCount >= 2"; return 0; }
        if (arrayLength != 1) { o->why = "a 2D multisample texture has arrayLength 1 (2DMultisampleArray is not supported)"; return 0; }
        if (depth != 1) { o->why = "a 2D multisample texture has depth 1"; return 0; }
        if (levels != 1) { o->why = "a 2D multisample texture has one mip level"; return 0; }
        if (usage & N48TD_USE_SHADERWRITE) { o->why = "ShaderWrite on a multisample texture is not supported (a storage image cannot be multisampled here)"; return 0; }
        o->kind = N48TD_K_2DMS; o->imageType = N48TD_VK_IMAGE_2D; o->viewType = N48TD_VK_VIEW_2D; o->layers = 1; o->flags = 0; o->layersIvar = 0; o->samples = (unsigned)sampleCount;
        o->ok = 1; return 1;
    }
    if (tt == N48TD_MTL_CUBE) {
        if (w != h) { o->why = "a cube texture must be square (width must equal height)"; return 0; }
        if (sampleCount != 1) { o->why = "multisampled cube textures are not supported"; return 0; }
        if (arrayLength != 1) { o->why = "a cube texture has arrayLength 1 (arrays of cubes are CubeArray, not supported)"; return 0; }
        if (depth != 1) { o->why = "a cube texture has depth 1"; return 0; }
        if (usage & N48TD_USE_SHADERWRITE) { o->why = "ShaderWrite on a cube texture is not supported (a storage image cannot be a cube view here)"; return 0; }
        o->kind = N48TD_K_CUBE; o->imageType = N48TD_VK_IMAGE_2D; o->viewType = N48TD_VK_VIEW_CUBE; o->layers = N48TD_CUBE_FACES; o->flags = N48TD_VK_CREATE_CUBE_COMPATIBLE; o->layersIvar = N48TD_CUBE_FACES;
        o->ok = 1; return 1;
    }
    if (tt == N48TD_MTL_2DARRAY) {
        if (sampleCount != 1) { o->why = "multisampled 2D-array textures are not supported"; return 0; }
        if (arrayLength < 1) { o->why = "a 2D-array texture needs arrayLength >= 1"; return 0; }
        if (depth != 1) { o->why = "a 2D-array texture has depth 1"; return 0; }
        if (usage & N48TD_USE_SHADERWRITE) { o->why = "ShaderWrite on a 2D-array texture is not supported (a storage image cannot be an array view here)"; return 0; }
        o->kind = N48TD_K_2DARRAY; o->imageType = N48TD_VK_IMAGE_2D; o->viewType = N48TD_VK_VIEW_2D_ARRAY; o->layers = (unsigned)arrayLength; o->flags = 0; o->layersIvar = (unsigned)arrayLength;
        o->ok = 1; return 1;
    }
    // ---- the types accepted before build 9: this block is the old check, unchanged ----
    if (!(tt == N48TD_MTL_2D || is1D || is3D) || (is3D ? arrayLength != 1 : depth != 1) || sampleCount != 1 ||
        (tt != N48TD_MTL_1DARRAY && arrayLength != 1) || arrayLength < 1 || (is1D && levels != 1)) {
        o->why = "only single-sample 2D (mips), 3D (mips), cube (mips), 2D-array and single-level 1D / 1DArray textures"; return 0; }
    o->kind = tt == N48TD_MTL_1DARRAY ? N48TD_K_1DARRAY : tt == N48TD_MTL_1D ? N48TD_K_1D : is3D ? N48TD_K_3D : N48TD_K_2D;
    o->imageType = is1D ? N48TD_VK_IMAGE_1D : is3D ? N48TD_VK_IMAGE_3D : N48TD_VK_IMAGE_2D;
    o->viewType = is1D ? (tt == N48TD_MTL_1DARRAY ? N48TD_VK_VIEW_1D_ARRAY : N48TD_VK_VIEW_1D) : is3D ? N48TD_VK_VIEW_3D : N48TD_VK_VIEW_2D;
    o->layers = is1D ? (unsigned)arrayLength : 1; o->layersIvar = is1D ? (unsigned)arrayLength : 0; o->flags = 0;
    o->ok = 1; return 1;
}

// The texture's own layer count (what n48Layers returns for a root texture): barriers, region checks, mip blits, readbacks.
static inline unsigned n48td_nlayers(unsigned layersIvar) { return layersIvar ? layersIvar : 1; }
// What textureType / arrayLength report.
static inline unsigned n48td_mtl_type(unsigned kind) {
    switch (kind) { case N48TD_K_1D: return N48TD_MTL_1D; case N48TD_K_1DARRAY: return N48TD_MTL_1DARRAY; case N48TD_K_3D: return N48TD_MTL_3D; case N48TD_K_CUBE: return N48TD_MTL_CUBE; case N48TD_K_2DARRAY: return N48TD_MTL_2DARRAY; case N48TD_K_2DMS: return N48TD_MTL_2DMS; default: return N48TD_MTL_2D; }
}
static inline unsigned long n48td_array_length(unsigned kind, unsigned layersIvar) {
    switch (kind) { case N48TD_K_1D: case N48TD_K_1DARRAY: case N48TD_K_2DARRAY: return layersIvar; default: return 1; }   // the cube is one texture of 6 faces: arrayLength 1
}
// Texture types that are an array of 2D images in Vulkan (cube and 2D array): their render-target views are per-layer 2D views.
static inline int n48td_is_layered2d(unsigned kind) { return kind == N48TD_K_CUBE || kind == N48TD_K_2DARRAY; }
// VkImageSubresourceLayers.baseArrayLayer for a Metal slice (3D textures put the depth in the extent: layer 0).
static inline uint32_t n48td_base_layer(int is3D, unsigned long slice) { return is3D ? 0u : (uint32_t)slice; }

// The single-level view for an attachment / storage image of (level, layer).  Old kinds: the sampling view type with layerCount = layers, base 0, and only layer 0 exists.
// Cube and 2D array: a plain 2D view of one layer.  Returns 0 when the layer does not exist.
typedef struct { unsigned viewType, baseLayer, layerCount; } n48td_att_t;
static inline int n48td_att_view(unsigned kind, unsigned layersIvar, unsigned long layer, n48td_att_t *a) {
    unsigned n = n48td_nlayers(layersIvar);
    if (layer >= n) return 0;
    if (n48td_is_layered2d(kind)) { a->viewType = N48TD_VK_VIEW_2D; a->baseLayer = (unsigned)layer; a->layerCount = 1; return 1; }
    a->viewType = kind == N48TD_K_1DARRAY ? N48TD_VK_VIEW_1D_ARRAY : kind == N48TD_K_1D ? N48TD_VK_VIEW_1D : kind == N48TD_K_3D ? N48TD_VK_VIEW_3D : N48TD_VK_VIEW_2D;
    a->baseLayer = 0; a->layerCount = (kind == N48TD_K_1D || kind == N48TD_K_1DARRAY) ? layersIvar : 1; return 1;
}
// Heap sizing: the layer count the pixel bytes are multiplied by.  (The old code: 1DArray = arrayLength, everything else 1.)
static inline unsigned long n48td_heap_layers(unsigned long tt, unsigned long arrayLength) {
    return tt == N48TD_MTL_1DARRAY ? arrayLength : tt == N48TD_MTL_2DARRAY ? arrayLength : tt == N48TD_MTL_CUBE ? N48TD_CUBE_FACES : tt == N48TD_MTL_CUBEARRAY ? N48TD_CUBE_FACES * arrayLength : 1;
}
#endif
