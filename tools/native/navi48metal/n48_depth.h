// n48_depth.h (bundle 10): the pure decisions for depth/stencil textures, depth/stencil pipeline state, MSAA and multisample resolve.
// Pure C (no Metal / Vulkan headers): the numeric values are asserted against the real enums in Navi48Device.m with _Static_assert, and the header is host-tested by test-depth.c
// (tables, plus source pins that tie the REAL Navi48Device.m to these functions) and used by test-vkimage.c (the MoltenVK call-shape tests) and by the bundle itself.
// an internal design note, "App crash study" (2) Risk and "Safari study" W6: depth/stencil attachments and MSAA are the next walls after the cube texture.
//
// Rule that binds this file: nothing a build-9 client could do changes.  A colour-only pipeline gets the key 0 (no depth/stencil state), the same three dynamic states,
// one sample and no depth bias; a single-sample colour attachment keeps STORE; every texture descriptor build 9 accepted maps as before (test-texdesc.c).
#ifndef N48_DEPTH_H
#define N48_DEPTH_H
#include <stdint.h>
#include <stddef.h>

// ---- Metal pixel formats and the Vulkan formats they map to (asserted in Navi48Device.m) ----
enum { N48DP_MTL_DEPTH16 = 250, N48DP_MTL_DEPTH32 = 252, N48DP_MTL_STENCIL8 = 253, N48DP_MTL_D24S8 = 255, N48DP_MTL_D32S8 = 260, N48DP_MTL_X32S8 = 261, N48DP_MTL_X24S8 = 262 };
enum { N48DP_VK_D16 = 124, N48DP_VK_D32 = 126, N48DP_VK_S8 = 127, N48DP_VK_D32S8 = 130 };
enum { N48DP_ASP_COLOR = 1, N48DP_ASP_DEPTH = 2, N48DP_ASP_STENCIL = 4 };                     // VkImageAspectFlagBits
enum { N48DP_FEAT_SAMPLED = 0x1, N48DP_FEAT_DS_ATT = 0x200 };                                  // VkFormatFeatureFlagBits
enum { N48DP_USE_READ = 1, N48DP_USE_WRITE = 2, N48DP_USE_RT = 4, N48DP_USE_PFV = 0x10 };      // MTLTextureUsage
enum { N48DP_ST_SHARED = 0, N48DP_ST_MANAGED = 1, N48DP_ST_PRIVATE = 2, N48DP_ST_MEMORYLESS = 3 };   // MTLStorageMode
enum { N48DP_T_2D = 2, N48DP_T_2DMS = 4 };                                                     // MTLTextureType

typedef struct {
    const char *why;      // refusal text (return value 0)
    unsigned vk;          // VkFormat
    unsigned aspects;     // every aspect the format has (N48DP_ASP_*): barriers, attachment views, image-to-image copies and clears use all of them
    unsigned bpp;         // bytes per texel for heap sizing (the packed D32S8 pair is 8)
} n48dp_fmt_t;

static inline int n48dp_is_dsformat(unsigned long pf) { return pf == N48DP_MTL_DEPTH16 || pf == N48DP_MTL_DEPTH32 || pf == N48DP_MTL_STENCIL8 || pf == N48DP_MTL_D24S8 || pf == N48DP_MTL_D32S8 || pf == N48DP_MTL_X32S8 || pf == N48DP_MTL_X24S8; }
// 1 when the bundle can make textures of this depth/stencil pixel format (then *o is filled); 0 with o->why otherwise.  D24S8 is NEVER supported: AMD has no D24S8 on this path.
static inline int n48dp_fmt(unsigned long pf, n48dp_fmt_t *o) {
    o->why = NULL; o->vk = 0; o->aspects = 0; o->bpp = 0;
    switch (pf) {
    case N48DP_MTL_DEPTH16: o->vk = N48DP_VK_D16;   o->aspects = N48DP_ASP_DEPTH;                     o->bpp = 2; return 1;
    case N48DP_MTL_DEPTH32: o->vk = N48DP_VK_D32;   o->aspects = N48DP_ASP_DEPTH;                     o->bpp = 4; return 1;
    case N48DP_MTL_STENCIL8: o->vk = N48DP_VK_S8;   o->aspects = N48DP_ASP_STENCIL;                   o->bpp = 1; return 1;
    case N48DP_MTL_D32S8:   o->vk = N48DP_VK_D32S8; o->aspects = N48DP_ASP_DEPTH | N48DP_ASP_STENCIL; o->bpp = 8; return 1;
    case N48DP_MTL_D24S8: case N48DP_MTL_X24S8: o->why = "Depth24Unorm_Stencil8 is not supported (AMD has no D24S8 on this path; use Depth32Float_Stencil8)"; return 0;
    case N48DP_MTL_X32S8: o->why = "stencil-only views (X32_Stencil8) of Depth32Float_Stencil8 are not supported"; return 0;
    default: o->why = "not a depth/stencil pixel format"; return 0;
    }
}
// The aspect a SAMPLED view of the format carries: Vulkan allows one aspect per descriptor view, so a combined format is sampled as its depth.
static inline unsigned n48dp_view_aspect(unsigned aspects) { return (aspects & N48DP_ASP_DEPTH) ? N48DP_ASP_DEPTH : N48DP_ASP_STENCIL; }
// The aspect of a layout barrier on the whole image: every aspect the format has (a depth barrier must name the depth aspect, not colour).
static inline unsigned n48dp_barrier_aspect(unsigned aspects) { return aspects ? aspects : N48DP_ASP_COLOR; }
// The aspect of a buffer<->image copy: only a single-aspect format can be copied to / from a buffer (a combined one needs the blit options); 0 = refuse.
static inline unsigned n48dp_copy_aspect(unsigned aspects) { return (aspects == N48DP_ASP_DEPTH || aspects == N48DP_ASP_STENCIL) ? aspects : 0; }

// Texture descriptor rules for the depth/stencil formats.  feats = the device's optimalTilingFeatures for the format.  1 = accepted; 0 with *why.
static inline int n48dp_check(unsigned long texType, unsigned long usage, unsigned long storage, unsigned feats, const char **why) {
    if (texType != N48DP_T_2D && texType != N48DP_T_2DMS) { *why = "depth/stencil textures are 2D or 2D multisample only (no cube, array, 1D or 3D)"; return 0; }
    if (storage != N48DP_ST_PRIVATE) { *why = "depth/stencil textures are Private storage only (Shared / Managed / Memoryless are refused)"; return 0; }
    if (usage & N48DP_USE_WRITE) { *why = "ShaderWrite on a depth/stencil texture is not supported (a depth image cannot be a storage image)"; return 0; }
    if ((usage & N48DP_USE_RT) && !(feats & N48DP_FEAT_DS_ATT)) { *why = "the device lacks depth/stencil attachment support for this format"; return 0; }
    if ((usage & N48DP_USE_READ) && !(feats & N48DP_FEAT_SAMPLED)) { *why = "the device cannot sample this depth/stencil format"; return 0; }
    *why = NULL; return 1;
}

// ---- MSAA ----
// VkSampleCountFlagBits for a Metal sample count: 2, 4, 8 only (16 and above are never offered); 0 = not a multisample count.
static inline unsigned n48ms_vkbits(unsigned long n) { return (n == 2 || n == 4 || n == 8) ? (unsigned)n : 0; }
// -supportsSampleCount: / -supportsTextureSampleCount:.  1 always.  With the device limits (colour mask and depth mask = framebufferColorSampleCounts, framebufferDepthSampleCounts & stencil): the count must be in both.
// Without them (RADV not open: asking must not open it): 4 only, which Vulkan requires of every device.
static inline int n48ms_device_supports(unsigned long n, int haveLimits, unsigned colorMask, unsigned depthMask) {
    if (n == 1) return 1;
    unsigned b = n48ms_vkbits(n); if (!b) return 0;
    if (!haveLimits) return n == 4;
    return (colorMask & b) && (depthMask & b) ? 1 : 0;
}
// A multisample TEXTURE of this format: the count must be in the image format's own sampleCounts (vkGetPhysicalDeviceImageFormatProperties) and in the device mask.
static inline int n48ms_texture_ok(unsigned long n, unsigned formatCounts, unsigned deviceMask) {
    unsigned b = n48ms_vkbits(n); if (!b) return 0;
    return (formatCounts & b) && (deviceMask & b) ? 1 : 0;
}

// ---- store actions and multisample resolve ----
enum { N48SA_DONTCARE = 0, N48SA_STORE = 1, N48SA_RESOLVE = 2, N48SA_STORE_RESOLVE = 3, N48SA_UNKNOWN = 4, N48SA_CUSTOM = 5 };   // MTLStoreAction
enum { N48LA_DONTCARE = 0, N48LA_LOAD = 1, N48LA_CLEAR = 2 };                                                                       // MTLLoadAction
typedef struct { const char *why; int resolve; } n48sa_t;
// 1 when the action is usable.  *resolve = 1: resolve the multisample attachment into resolveTexture at the end of the encoder.  The attachment is always stored (a pass may be split and continued).
static inline int n48sa_map(unsigned long action, int hasResolveTex, int attachmentIsMS, n48sa_t *o) {
    o->why = NULL; o->resolve = 0;
    switch (action) {
    case N48SA_DONTCARE: case N48SA_STORE: case N48SA_UNKNOWN: return 1;   // Unknown: the encoder's setColorStoreAction: decides before endEncoding; a resolve texture alone resolves nothing
    case N48SA_RESOLVE: case N48SA_STORE_RESOLVE:
        if (!hasResolveTex) { o->why = "a MultisampleResolve store action needs a resolveTexture"; return 0; }
        if (!attachmentIsMS) { o->why = "a multisample resolve of a single-sample attachment"; return 0; }
        o->resolve = 1; return 1;
    default: o->why = "store action CustomSampleDepthStore (programmable sample positions) is not supported"; return 0;
    }
}
// The resolve target must be a single-sample texture of the same size and format as the attachment's level (vkCmdResolveImage).  1 = ok.
static inline int n48sa_target_ok(unsigned srcW, unsigned srcH, unsigned srcFmt, unsigned srcAspects, unsigned dstW, unsigned dstH, unsigned dstFmt, unsigned dstSamples, const char **why) {
    if (srcAspects != N48DP_ASP_COLOR) { *why = "only a colour attachment can be resolved (a depth/stencil resolve needs vkCmdResolveImage's depth path, which Vulkan does not have)"; return 0; }
    if (dstSamples != 1) { *why = "the resolve texture must be single-sample"; return 0; }
    if (srcW != dstW || srcH != dstH) { *why = "the resolve texture's level size differs from the attachment's"; return 0; }
    if (srcFmt != dstFmt) { *why = "the resolve texture's pixel format differs from the attachment's"; return 0; }
    *why = NULL; return 1;
}

// ---- render-pass attachment operations for a depth/stencil attachment ----
enum { N48VK_LOAD = 0, N48VK_CLEAR = 1, N48VK_LOAD_DONT_CARE = 2, N48VK_STORE = 0, N48VK_STORE_DONT_CARE = 1 };   // VkAttachmentLoadOp / StoreOp
typedef struct { unsigned depthLoad, stencilLoad, depthStore, stencilStore; int undefinedInitial; } n48rp_ds_t;
// texAspects = the texture's own aspects; hasDepthDesc / hasStencilDesc = the pass descriptor names that aspect; *Load = its MTLLoadAction; layoutUndefined = the texture's contents are undefined (first use).
// An aspect the texture has but the descriptor does not name is preserved (LOAD).  The store is always STORE for an aspect that exists (a pass may be split and continued).
// undefinedInitial = every aspect that exists is cleared or discarded (so the pass may start from UNDEFINED and the driver may drop the old contents).
static inline void n48rp_ds(unsigned texAspects, int hasDepthDesc, unsigned depthLoad, int hasStencilDesc, unsigned stencilLoad, int layoutUndefined, n48rp_ds_t *o) {
    int hd = (texAspects & N48DP_ASP_DEPTH) != 0, hs = (texAspects & N48DP_ASP_STENCIL) != 0;
    unsigned dl = !hd ? N48VK_LOAD_DONT_CARE : !hasDepthDesc ? N48VK_LOAD : depthLoad == N48LA_CLEAR ? N48VK_CLEAR : depthLoad == N48LA_LOAD ? N48VK_LOAD : N48VK_LOAD_DONT_CARE;
    unsigned sl = !hs ? N48VK_LOAD_DONT_CARE : !hasStencilDesc ? N48VK_LOAD : stencilLoad == N48LA_CLEAR ? N48VK_CLEAR : stencilLoad == N48LA_LOAD ? N48VK_LOAD : N48VK_LOAD_DONT_CARE;
    if (layoutUndefined) { if (dl == N48VK_LOAD) dl = N48VK_LOAD_DONT_CARE; if (sl == N48VK_LOAD) sl = N48VK_LOAD_DONT_CARE; }
    o->depthLoad = dl; o->stencilLoad = sl;
    o->depthStore = hd ? N48VK_STORE : N48VK_STORE_DONT_CARE; o->stencilStore = hs ? N48VK_STORE : N48VK_STORE_DONT_CARE;
    o->undefinedInitial = layoutUndefined || ((!hd || dl != N48VK_LOAD) && (!hs || sl != N48VK_LOAD));
}

// ---- depth/stencil pipeline state ----
// MTLCompareFunction -> VkCompareOp and MTLStencilOperation -> VkStencilOp (written out, not assumed equal; the numbering is asserted in Navi48Device.m).
enum { N48DS_CMP_NEVER = 0, N48DS_CMP_LESS = 1, N48DS_CMP_EQUAL = 2, N48DS_CMP_LE = 3, N48DS_CMP_GREATER = 4, N48DS_CMP_NE = 5, N48DS_CMP_GE = 6, N48DS_CMP_ALWAYS = 7 };
enum { N48DS_OP_KEEP = 0, N48DS_OP_ZERO = 1, N48DS_OP_REPLACE = 2, N48DS_OP_INC_CLAMP = 3, N48DS_OP_DEC_CLAMP = 4, N48DS_OP_INVERT = 5, N48DS_OP_INC_WRAP = 6, N48DS_OP_DEC_WRAP = 7 };
static inline unsigned n48ds_cmp(unsigned long m) {
    switch (m) { case 0: return N48DS_CMP_NEVER; case 1: return N48DS_CMP_LESS; case 2: return N48DS_CMP_EQUAL; case 3: return N48DS_CMP_LE; case 4: return N48DS_CMP_GREATER;
                 case 5: return N48DS_CMP_NE; case 6: return N48DS_CMP_GE; default: return N48DS_CMP_ALWAYS; }
}
static inline unsigned n48ds_op(unsigned long m) {
    switch (m) { case 0: return N48DS_OP_KEEP; case 1: return N48DS_OP_ZERO; case 2: return N48DS_OP_REPLACE; case 3: return N48DS_OP_INC_CLAMP; case 4: return N48DS_OP_DEC_CLAMP;
                 case 5: return N48DS_OP_INVERT; case 6: return N48DS_OP_INC_WRAP; case 7: return N48DS_OP_DEC_WRAP; default: return N48DS_OP_KEEP; }
}
typedef struct { unsigned long cmp, fail, dfail, pass; } n48ds_face_in_t;   // Metal values of one MTLStencilDescriptor
// The pipeline variant key of the depth/stencil state: 0 = the pipeline has no depth/stencil attachment (the colour-only world, exactly as before).  Otherwise
//   bits 0-2 depth compare op, bit 3 depth write, bit 4 stencil test, bits 5-16 front face (cmp 3, fail 3, depthFail 3, pass 3), bits 17-28 back face, bit 29 "valid", bit 30 "has depth".
// The stencil test is on only when the pipeline has a stencil attachment AND a face does something (compare != Always or an op != Keep).  Masks and the reference are dynamic, not in the key.
static inline uint32_t n48ds_key(int hasDepthAtt, int hasStencilAtt, unsigned long depthCmp, int depthWrite, const n48ds_face_in_t *f, const n48ds_face_in_t *b) {
    if (!hasDepthAtt && !hasStencilAtt) return 0;
    uint32_t k = 1u << 29;
    if (hasDepthAtt) { k |= 1u << 30; k |= n48ds_cmp(depthCmp); if (depthWrite) k |= 1u << 3; }
    else k |= N48DS_CMP_ALWAYS;
    if (hasStencilAtt) {
        unsigned fc = n48ds_cmp(f->cmp), ff = n48ds_op(f->fail), fd = n48ds_op(f->dfail), fp = n48ds_op(f->pass), bc = n48ds_cmp(b->cmp), bf = n48ds_op(b->fail), bd = n48ds_op(b->dfail), bp = n48ds_op(b->pass);
        int on = fc != N48DS_CMP_ALWAYS || ff != N48DS_OP_KEEP || fd != N48DS_OP_KEEP || fp != N48DS_OP_KEEP || bc != N48DS_CMP_ALWAYS || bf != N48DS_OP_KEEP || bd != N48DS_OP_KEEP || bp != N48DS_OP_KEEP;
        if (on) { k |= 1u << 4; k |= (uint32_t)(fc | ff << 3 | fd << 6 | fp << 9) << 5; k |= (uint32_t)(bc | bf << 3 | bd << 6 | bp << 9) << 17; }
    }
    return k;
}
typedef struct { unsigned compareOp, failOp, passOp, depthFailOp; } n48ds_vkface_t;
typedef struct { int valid, depthTest, depthWrite, stencilTest; unsigned depthCompareOp; n48ds_vkface_t front, back; } n48ds_vk_t;   // the VkPipelineDepthStencilStateCreateInfo fields (numeric Vulkan values)
static inline void n48ds_decode(uint32_t k, n48ds_vk_t *o) {
    o->valid = (k >> 29) & 1u; o->depthTest = (k >> 30) & 1u; o->depthWrite = (k >> 3) & 1u; o->stencilTest = (k >> 4) & 1u; o->depthCompareOp = k & 7u;
    unsigned f = (k >> 5) & 0xFFFu, b = (k >> 17) & 0xFFFu;
    o->front = (n48ds_vkface_t){ f & 7u, (f >> 3) & 7u, (f >> 9) & 7u, (f >> 6) & 7u };   // compare, fail, pass, depthFail
    o->back = (n48ds_vkface_t){ b & 7u, (b >> 3) & 7u, (b >> 9) & 7u, (b >> 6) & 7u };
    if (!o->stencilTest) { o->front = (n48ds_vkface_t){ N48DS_CMP_ALWAYS, N48DS_OP_KEEP, N48DS_OP_KEEP, N48DS_OP_KEEP }; o->back = o->front; }
}

// ---- the render pipeline's fixed configuration (everything the old code hard-wired for a colour-only, single-sample pipeline stays hard-wired) ----
enum { N48VK_DYN_VIEWPORT = 0, N48VK_DYN_SCISSOR = 1, N48VK_DYN_DEPTH_BIAS = 3, N48VK_DYN_BLEND_CONSTANTS = 4, N48VK_DYN_STENCIL_COMPARE_MASK = 6, N48VK_DYN_STENCIL_WRITE_MASK = 7, N48VK_DYN_STENCIL_REFERENCE = 8 };   // VkDynamicState
typedef struct { unsigned ndyn; unsigned dyn[8]; unsigned samples; int depthBias; int alphaToCoverage; int hasDS; } n48pc_t;
// depthFmtSet / stencilFmtSet: the descriptor names a depth / stencil attachment format.  rasterSamples: the descriptor's rasterSampleCount (0 = 1).  deviceMask: the device's depth+colour sample mask.
// 1 = ok; 0 with *why for a sample count that is not 1, 2, 4 or 8 or that the device lacks.
static inline int n48pc_config(int depthFmtSet, int stencilFmtSet, unsigned long rasterSamples, int alphaToCoverage, unsigned deviceMask, n48pc_t *o, const char **why) {
    unsigned long n = rasterSamples ? rasterSamples : 1;
    o->ndyn = 0; o->dyn[o->ndyn++] = N48VK_DYN_VIEWPORT; o->dyn[o->ndyn++] = N48VK_DYN_SCISSOR; o->dyn[o->ndyn++] = N48VK_DYN_BLEND_CONSTANTS;
    o->samples = 1; o->depthBias = 0; o->alphaToCoverage = 0; o->hasDS = depthFmtSet || stencilFmtSet; *why = NULL;
    if (n != 1) {
        unsigned b = n48ms_vkbits(n);
        if (!b) { *why = "rasterSampleCount must be 1, 2, 4 or 8"; return 0; }
        if (!(deviceMask & b)) { *why = "the device does not support this rasterSampleCount"; return 0; }
        o->samples = (unsigned)n; o->alphaToCoverage = alphaToCoverage ? 1 : 0;   // alphaToCoverage means nothing for one sample: left off as before
    }
    if (o->hasDS) {
        o->depthBias = depthFmtSet ? 1 : 0;
        o->dyn[o->ndyn++] = N48VK_DYN_STENCIL_COMPARE_MASK; o->dyn[o->ndyn++] = N48VK_DYN_STENCIL_WRITE_MASK; o->dyn[o->ndyn++] = N48VK_DYN_STENCIL_REFERENCE;
        if (depthFmtSet) o->dyn[o->ndyn++] = N48VK_DYN_DEPTH_BIAS;
    }
    return 1;
}
#endif
