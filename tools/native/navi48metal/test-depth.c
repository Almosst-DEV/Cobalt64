// test-depth.c: host test of n48_depth.h (bundle 10: depth/stencil textures and pipeline state, MSAA, multisample resolve), plus source pins that tie the REAL Navi48Device.m to it.
// Build/run (from this directory):  cc -O1 -Wall -Wextra -Werror -o /tmp/test-depth test-depth.c && /tmp/test-depth .
//   argv[1] = the directory holding Navi48Device.m and the headers (default ".").
// The rule this file enforces: nothing a build-9 client could do changes.  A colour-only, single-sample pipeline keeps key 0, the three old dynamic states, one sample and no bias
// (swept below); the colour table never answers for a depth format; D24S8 is never supported; and the pieces that make depth work (the aspect of every barrier / view / copy, the depth write bit,
// the stencil operations, the resolve) are pinned both as pure tables and as text in the real source.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n48_depth.h"
static int fails, checks;
#define CHECK(c, ...) do { int ok_ = (c) ? 1 : 0; checks++; printf("%s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define QUIET(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = calloc(1, (size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } fclose(f); return b; }
static int count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }
static const n48ds_face_in_t DEF = { N48DS_CMP_ALWAYS, 0, 0, 0 };

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    n48dp_fmt_t f;
    // ---- formats (item 1) ----
    CHECK(n48dp_fmt(250, &f) && f.vk == 124 && f.aspects == N48DP_ASP_DEPTH && f.bpp == 2, "Depth16Unorm (250) -> VK_FORMAT_D16_UNORM (124), depth aspect, 2 bytes");
    CHECK(n48dp_fmt(252, &f) && f.vk == 126 && f.aspects == N48DP_ASP_DEPTH && f.bpp == 4, "Depth32Float (252) -> VK_FORMAT_D32_SFLOAT (126), depth aspect, 4 bytes");
    CHECK(n48dp_fmt(253, &f) && f.vk == 127 && f.aspects == N48DP_ASP_STENCIL && f.bpp == 1, "Stencil8 (253) -> VK_FORMAT_S8_UINT (127), stencil aspect, 1 byte");
    CHECK(n48dp_fmt(260, &f) && f.vk == 130 && f.aspects == (N48DP_ASP_DEPTH | N48DP_ASP_STENCIL) && f.bpp == 8, "Depth32Float_Stencil8 (260) -> VK_FORMAT_D32_SFLOAT_S8_UINT (130), depth|stencil aspects, 8 bytes");
    CHECK(!n48dp_fmt(255, &f) && f.why && strstr(f.why, "D24S8"), "Depth24Unorm_Stencil8 (255) is NEVER supported (\"%s\")", f.why ? f.why : "");
    CHECK(!n48dp_fmt(262, &f) && f.why && strstr(f.why, "D24S8"), "X24_Stencil8 (262) -> refused (it is the D24S8 stencil view)");
    CHECK(!n48dp_fmt(261, &f) && f.why && strstr(f.why, "X32_Stencil8"), "X32_Stencil8 (261) -> refused with a stencil-view message (\"%s\")", f.why ? f.why : "");
    CHECK(!n48dp_fmt(80, &f) && !n48dp_is_dsformat(80) && !n48dp_is_dsformat(70) && !n48dp_is_dsformat(0) && !n48dp_is_dsformat(115), "BGRA8Unorm (80), RGBA8Unorm (70), Invalid (0), RGBA16Float (115) are not depth/stencil formats");
    { int all = 1; for (unsigned long pf = 0; pf < 300; pf++) { int ds = n48dp_is_dsformat(pf); int exp = pf == 250 || pf == 252 || pf == 253 || pf == 255 || pf == 260 || pf == 261 || pf == 262; if (ds != exp) all = 0; }
      CHECK(all, "n48dp_is_dsformat is true for exactly 250, 252, 253, 255, 260, 261, 262 (sweep 0..299)"); }
    // ---- aspects everywhere (barriers, views, copies) ----
    CHECK(n48dp_barrier_aspect(N48DP_ASP_DEPTH) == 2 && n48dp_barrier_aspect(N48DP_ASP_STENCIL) == 4 && n48dp_barrier_aspect(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL) == 6 && n48dp_barrier_aspect(0) == N48DP_ASP_COLOR,
          "barrier aspect: D = DEPTH(2), S8 = STENCIL(4), D32S8 = DEPTH|STENCIL(6), colour (0) = COLOR(1)");
    CHECK(n48dp_view_aspect(N48DP_ASP_DEPTH) == N48DP_ASP_DEPTH && n48dp_view_aspect(N48DP_ASP_STENCIL) == N48DP_ASP_STENCIL && n48dp_view_aspect(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL) == N48DP_ASP_DEPTH,
          "sampling view aspect: ONE aspect (D16/D32 depth, S8 stencil, D32S8 its depth)");
    CHECK(n48dp_copy_aspect(N48DP_ASP_DEPTH) == 2 && n48dp_copy_aspect(N48DP_ASP_STENCIL) == 4 && n48dp_copy_aspect(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL) == 0, "buffer copy aspect: depth, stencil, and 0 (refused) for the combined format");
    // ---- descriptor rules ----
    { const char *why = NULL; unsigned all = N48DP_FEAT_SAMPLED | N48DP_FEAT_DS_ATT;
      CHECK(n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_PRIVATE, all, &why) && !why, "2D, RenderTarget, Private -> accepted");
      CHECK(n48dp_check(N48DP_T_2DMS, N48DP_USE_RT | N48DP_USE_READ, N48DP_ST_PRIVATE, all, &why), "2DMultisample, RenderTarget|ShaderRead, Private -> accepted");
      CHECK(n48dp_check(N48DP_T_2D, N48DP_USE_READ | N48DP_USE_PFV, N48DP_ST_PRIVATE, all, &why), "PixelFormatView usage is accepted (and ignored)");
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_SHARED, all, &why) && strstr(why, "Private"), "Shared storage -> refused with a Private message (\"%s\")", why);
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_MANAGED, all, &why) && strstr(why, "Private"), "Managed storage -> refused");
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_MEMORYLESS, all, &why), "Memoryless storage -> refused");
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_WRITE, N48DP_ST_PRIVATE, all, &why) && strstr(why, "ShaderWrite"), "ShaderWrite on a depth texture -> refused");
      for (unsigned long tt = 0; tt < 10; tt++) { int r = n48dp_check(tt, N48DP_USE_RT, N48DP_ST_PRIVATE, all, &why); QUIET(r == (tt == 2 || tt == 4), "texture type %lu: depth accepted only for 2D and 2DMS", tt); }
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_PRIVATE, N48DP_FEAT_SAMPLED, &why) && strstr(why, "attachment"), "the device lacks depth/stencil attachment support -> a RenderTarget depth texture is refused with a message");
      CHECK(!n48dp_check(N48DP_T_2D, N48DP_USE_READ, N48DP_ST_PRIVATE, N48DP_FEAT_DS_ATT, &why) && strstr(why, "sample"), "the device cannot sample the format -> a ShaderRead depth texture is refused");
      CHECK(n48dp_check(N48DP_T_2D, N48DP_USE_RT, N48DP_ST_PRIVATE, N48DP_FEAT_DS_ATT, &why), "RenderTarget only needs the attachment feature"); }
    // ---- MSAA (item 3) ----
    CHECK(n48ms_device_supports(1, 0, 0, 0) && n48ms_device_supports(1, 1, 0, 0), "1 sample: always supported (even before RADV is open, even with an empty mask)");
    CHECK(n48ms_device_supports(4, 0, 0, 0) && !n48ms_device_supports(2, 0, 0, 0) && !n48ms_device_supports(8, 0, 0, 0), "before RADV is open: 4 only (Vulkan requires it); asking never opens RADV");
    CHECK(n48ms_device_supports(2, 1, 0xF, 0xF) && n48ms_device_supports(4, 1, 0xF, 0xF) && n48ms_device_supports(8, 1, 0xF, 0xF), "with limits 1|2|4|8: 2, 4 and 8 are supported");
    CHECK(!n48ms_device_supports(8, 1, 0x7, 0xF) && !n48ms_device_supports(8, 1, 0xF, 0x7) && n48ms_device_supports(4, 1, 0x7, 0x7), "a count must be in the colour AND the depth mask (8 missing from either -> NO)");
    CHECK(!n48ms_device_supports(3, 1, 0xFF, 0xFF) && !n48ms_device_supports(0, 1, 0xFF, 0xFF) && !n48ms_device_supports(16, 1, 0xFF, 0xFF) && !n48ms_device_supports(5, 0, 0, 0), "3, 0, 5 and 16 samples are never supported");
    CHECK(n48ms_texture_ok(4, 0x7, 0xF) && !n48ms_texture_ok(4, 0x3, 0xF) && !n48ms_texture_ok(4, 0xF, 0x3) && !n48ms_texture_ok(8, 0x7, 0xF) && n48ms_texture_ok(2, 0x3, 0x7),
          "a multisample texture: the count must be in the FORMAT's own sample counts and the device mask (4 with a format that offers only 1|2 -> refused)");
    CHECK(!n48ms_texture_ok(1, 0xF, 0xF) && !n48ms_texture_ok(3, 0xF, 0xF), "n48ms_texture_ok is for multisample counts only (1 and 3 are not)");
    // ---- store actions and resolve ----
    { n48sa_t a;
      CHECK(n48sa_map(N48SA_STORE, 0, 0, &a) && !a.resolve && n48sa_map(N48SA_DONTCARE, 0, 0, &a) && !a.resolve && n48sa_map(N48SA_UNKNOWN, 0, 1, &a) && !a.resolve, "Store, DontCare, Unknown: no resolve (single or multisample, with or without a resolve texture)");
      CHECK(n48sa_map(N48SA_STORE, 1, 1, &a) && !a.resolve, "Store with a resolveTexture present: no resolve (Metal ignores the texture)");
      CHECK(n48sa_map(N48SA_RESOLVE, 1, 1, &a) && a.resolve, "MultisampleResolve with a resolve texture on a multisample attachment: resolve");
      CHECK(n48sa_map(N48SA_STORE_RESOLVE, 1, 1, &a) && a.resolve, "StoreAndMultisampleResolve: resolve (the attachment is stored anyway)");
      CHECK(!n48sa_map(N48SA_RESOLVE, 0, 1, &a) && a.why && strstr(a.why, "resolveTexture"), "MultisampleResolve without a resolve texture -> refused (\"%s\")", a.why ? a.why : "");
      CHECK(!n48sa_map(N48SA_STORE_RESOLVE, 1, 0, &a) && a.why && strstr(a.why, "single-sample"), "a resolve store action on a single-sample attachment -> refused");
      CHECK(!n48sa_map(N48SA_CUSTOM, 1, 1, &a) && a.why && strstr(a.why, "CustomSampleDepthStore"), "CustomSampleDepthStore -> refused");
      CHECK(!n48sa_map(77, 0, 0, &a), "an unknown store action -> refused");
      const char *why = NULL;
      CHECK(n48sa_target_ok(64, 32, 44, N48DP_ASP_COLOR, 64, 32, 44, 1, &why) && !why, "resolve target of the same size and format -> ok");
      CHECK(!n48sa_target_ok(64, 32, 44, N48DP_ASP_COLOR, 64, 32, 44, 4, &why) && strstr(why, "single-sample"), "resolve target with 4 samples -> refused");
      CHECK(!n48sa_target_ok(64, 32, 44, N48DP_ASP_COLOR, 32, 32, 44, 1, &why) && strstr(why, "size"), "resolve target of another size -> refused");
      CHECK(!n48sa_target_ok(64, 32, 44, N48DP_ASP_COLOR, 64, 32, 37, 1, &why) && strstr(why, "format"), "resolve target of another pixel format -> refused");
      CHECK(!n48sa_target_ok(64, 32, 126, N48DP_ASP_DEPTH, 64, 32, 126, 1, &why) && strstr(why, "colour"), "a depth attachment cannot be resolved -> refused"); }
    // ---- render-pass operations (item 2) ----
    { n48rp_ds_t o;
      n48rp_ds(N48DP_ASP_DEPTH, 1, N48LA_CLEAR, 0, 0, 0, &o);
      CHECK(o.depthLoad == N48VK_CLEAR && o.depthStore == N48VK_STORE && o.stencilLoad == N48VK_LOAD_DONT_CARE && o.stencilStore == N48VK_STORE_DONT_CARE && o.undefinedInitial, "D32: depth Clear -> CLEAR + STORE, no stencil aspect (DONT_CARE), may start UNDEFINED");
      n48rp_ds(N48DP_ASP_DEPTH, 1, N48LA_LOAD, 0, 0, 0, &o);
      CHECK(o.depthLoad == N48VK_LOAD && !o.undefinedInitial, "D32: depth Load of defined contents -> LOAD, the old layout is kept");
      n48rp_ds(N48DP_ASP_DEPTH, 1, N48LA_LOAD, 0, 0, 1, &o);
      CHECK(o.depthLoad == N48VK_LOAD_DONT_CARE && o.undefinedInitial, "D32: depth Load but the contents are undefined (first use) -> DONT_CARE from UNDEFINED");
      n48rp_ds(N48DP_ASP_DEPTH, 1, N48LA_DONTCARE, 0, 0, 0, &o);
      CHECK(o.depthLoad == N48VK_LOAD_DONT_CARE && o.depthStore == N48VK_STORE, "D32: depth DontCare load -> DONT_CARE, still STORE (a split pass continues from it)");
      n48rp_ds(N48DP_ASP_STENCIL, 0, 0, 1, N48LA_CLEAR, 0, &o);
      CHECK(o.stencilLoad == N48VK_CLEAR && o.stencilStore == N48VK_STORE && o.depthLoad == N48VK_LOAD_DONT_CARE && o.depthStore == N48VK_STORE_DONT_CARE, "S8: stencil Clear -> CLEAR + STORE, no depth aspect");
      n48rp_ds(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL, 1, N48LA_CLEAR, 1, N48LA_LOAD, 0, &o);
      CHECK(o.depthLoad == N48VK_CLEAR && o.stencilLoad == N48VK_LOAD && !o.undefinedInitial, "D32S8: depth Clear + stencil Load -> the pass must NOT start UNDEFINED (stencil is loaded)");
      n48rp_ds(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL, 1, N48LA_CLEAR, 1, N48LA_CLEAR, 0, &o);
      CHECK(o.undefinedInitial && o.depthLoad == N48VK_CLEAR && o.stencilLoad == N48VK_CLEAR, "D32S8: both Clear -> may start UNDEFINED");
      n48rp_ds(N48DP_ASP_DEPTH | N48DP_ASP_STENCIL, 1, N48LA_CLEAR, 0, 0, 0, &o);
      CHECK(o.stencilLoad == N48VK_LOAD && !o.undefinedInitial && o.stencilStore == N48VK_STORE, "D32S8 attached as depth only: the stencil aspect is preserved (LOAD / STORE), never discarded"); }
    // ---- compare / stencil operation maps (written out; the numbering is asserted against the real enums in Navi48Device.m) ----
    { static const unsigned expc[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }, expo[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };   // VK_COMPARE_OP_NEVER..ALWAYS, VK_STENCIL_OP_KEEP..DECREMENT_AND_WRAP
      int okc = 1, oko = 1; for (unsigned long m = 0; m < 8; m++) { if (n48ds_cmp(m) != expc[m]) okc = 0; if (n48ds_op(m) != expo[m]) oko = 0; }
      CHECK(okc, "MTLCompareFunction Never..Always -> VkCompareOp NEVER, LESS, EQUAL, LESS_OR_EQUAL, GREATER, NOT_EQUAL, GREATER_OR_EQUAL, ALWAYS");
      CHECK(oko, "MTLStencilOperation Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap -> VkStencilOp KEEP, ZERO, REPLACE, INCREMENT_AND_CLAMP, DECREMENT_AND_CLAMP, INVERT, INCREMENT_AND_WRAP, DECREMENT_AND_WRAP");
      CHECK(n48ds_cmp(3) == 3 && n48ds_cmp(4) == 4 && n48ds_op(3) == 3 && n48ds_op(4) == 4 && n48ds_op(6) == 6 && n48ds_op(7) == 7, "the clamp / wrap operations stay distinct (3 vs 6, 4 vs 7)");
      CHECK(n48ds_cmp(99) == N48DS_CMP_ALWAYS && n48ds_op(99) == N48DS_OP_KEEP, "an out-of-range value maps to Always / Keep"); }
    // ---- the pipeline variant key ----
    { n48ds_vk_t v;
      CHECK(n48ds_key(0, 0, 1, 1, &DEF, &DEF) == 0 && n48ds_key(0, 0, 3, 0, &DEF, &DEF) == 0, "a pipeline with no depth/stencil attachment: key 0 whatever the bound state says (the colour-only world is untouched)");
      uint32_t k = n48ds_key(1, 0, 1 /*Less*/, 1, &DEF, &DEF); n48ds_decode(k, &v);
      CHECK(k != 0 && v.valid && v.depthTest && v.depthWrite && v.depthCompareOp == 1 && !v.stencilTest, "depth Less, write on: valid, test on, write on, op LESS, no stencil test");
      k = n48ds_key(1, 0, 1, 0, &DEF, &DEF); n48ds_decode(k, &v);
      CHECK(v.depthTest && !v.depthWrite && v.depthCompareOp == 1, "depth Less, write OFF: the write bit is clear");
      CHECK(n48ds_key(1, 0, 1, 1, &DEF, &DEF) != n48ds_key(1, 0, 1, 0, &DEF, &DEF), "the depth write flag is part of the key (a pipeline variant per write setting)");
      for (unsigned long c = 0; c < 8; c++) { n48ds_decode(n48ds_key(1, 0, c, 0, &DEF, &DEF), &v); QUIET(v.depthCompareOp == n48ds_cmp(c), "depth compare %lu round-trips", c); }
      k = n48ds_key(1, 0, 7, 0, &DEF, &DEF); n48ds_decode(k, &v);
      CHECK(v.valid && v.depthTest && !v.depthWrite && v.depthCompareOp == N48DS_CMP_ALWAYS && !v.stencilTest, "the default state (Always, no write) on a depth pipeline: a valid non-zero key, test on (a no-op)");
      n48ds_face_in_t fa = { 2 /*Equal*/, 0 /*Keep*/, 1 /*Zero*/, 2 /*Replace*/ }, ba = { 4 /*Greater*/, 5 /*Invert*/, 6 /*IncWrap*/, 7 /*DecWrap*/ };
      k = n48ds_key(1, 1, 1, 1, &fa, &ba); n48ds_decode(k, &v);
      CHECK(v.stencilTest && v.front.compareOp == N48DS_CMP_EQUAL && v.front.failOp == N48DS_OP_KEEP && v.front.depthFailOp == N48DS_OP_ZERO && v.front.passOp == N48DS_OP_REPLACE, "front stencil: compare Equal, fail Keep, depthFail Zero, pass Replace round-trip to the right Vulkan fields");
      CHECK(v.back.compareOp == N48DS_CMP_GREATER && v.back.failOp == N48DS_OP_INVERT && v.back.depthFailOp == N48DS_OP_INC_WRAP && v.back.passOp == N48DS_OP_DEC_WRAP, "back stencil: compare Greater, fail Invert, depthFail IncrementWrap, pass DecrementWrap round-trip");
      k = n48ds_key(0, 1, 7, 0, &fa, &ba); n48ds_decode(k, &v);
      CHECK(v.valid && !v.depthTest && v.stencilTest && v.front.passOp == N48DS_OP_REPLACE, "stencil-only pipeline: depth test OFF, stencil test on");
      k = n48ds_key(1, 0, 1, 1, &fa, &ba); n48ds_decode(k, &v);
      CHECK(!v.stencilTest && v.front.compareOp == N48DS_CMP_ALWAYS && v.front.passOp == N48DS_OP_KEEP && v.back.passOp == N48DS_OP_KEEP, "a depth-only pipeline ignores the state's stencil faces (no stencil attachment -> no stencil test)");
      CHECK(n48ds_key(1, 1, 1, 1, &DEF, &DEF) == n48ds_key(1, 0, 1, 1, &DEF, &DEF) + 0 && !(n48ds_key(1, 1, 1, 1, &DEF, &DEF) & (1u << 4)), "stencil attachment present but both faces default (Always / Keep): the stencil test stays off");
      n48ds_face_in_t only_fail = { 7, 1, 0, 0 };
      CHECK(n48ds_key(1, 1, 1, 1, &only_fail, &DEF) & (1u << 4), "one face doing anything (here: fail op Zero) turns the stencil test on");
      n48ds_face_in_t only_cmp = { 0, 0, 0, 0 };
      CHECK(n48ds_key(1, 1, 1, 1, &DEF, &only_cmp) & (1u << 4), "compare Never on the back face alone also turns the stencil test on");
      CHECK(n48ds_key(1, 1, 1, 1, &fa, &ba) != n48ds_key(1, 1, 1, 1, &ba, &fa), "swapping the front and back faces gives another key"); }
    // ---- the pipeline's fixed configuration (item 4: a colour-only pipeline is exactly the old one) ----
    { n48pc_t c; const char *why = NULL; int sweep_ok = 1; long n = 0;
      for (unsigned long rs = 0; rs <= 1; rs++) for (int a2c = 0; a2c <= 1; a2c++) for (unsigned mask = 0; mask < 16; mask++) {
          int ok = n48pc_config(0, 0, rs, a2c, mask, &c, &why); n++;
          if (!(ok && c.ndyn == 3 && c.dyn[0] == 0 && c.dyn[1] == 1 && c.dyn[2] == 4 && c.samples == 1 && c.depthBias == 0 && c.alphaToCoverage == 0 && c.hasDS == 0 && !why)) sweep_ok = 0; }
      CHECK(sweep_ok, "colour-only, rasterSampleCount 0 or 1, alphaToCoverage on or off, any device mask (%ld cases): exactly VIEWPORT, SCISSOR, BLEND_CONSTANTS, 1 sample, no bias, no alpha-to-coverage, no depth/stencil state", n);
      CHECK(n48pc_config(1, 0, 1, 0, 0xF, &c, &why) && c.hasDS && c.samples == 1 && c.depthBias == 1 && c.ndyn == 7 && c.dyn[3] == N48VK_DYN_STENCIL_COMPARE_MASK && c.dyn[4] == N48VK_DYN_STENCIL_WRITE_MASK && c.dyn[5] == N48VK_DYN_STENCIL_REFERENCE && c.dyn[6] == N48VK_DYN_DEPTH_BIAS,
            "depth pipeline: the three old dynamic states + stencil compare mask, write mask, reference + depth bias, bias enabled");
      CHECK(n48pc_config(0, 1, 1, 0, 0xF, &c, &why) && c.hasDS && c.depthBias == 0 && c.ndyn == 6, "stencil-only pipeline: stencil dynamic states, no depth bias");
      CHECK(n48pc_config(1, 1, 4, 1, 0xF, &c, &why) && c.samples == 4 && c.alphaToCoverage == 1, "4 samples with alphaToCoverage: the pipeline carries both");
      CHECK(n48pc_config(0, 0, 4, 0, 0xF, &c, &why) && c.samples == 4 && c.alphaToCoverage == 0 && c.ndyn == 3, "a 4-sample colour-only pipeline keeps the three dynamic states");
      CHECK(!n48pc_config(0, 0, 4, 0, 0x3, &c, &why) && why && strstr(why, "does not support"), "4 samples on a device whose mask lacks 4 -> refused (\"%s\")", why ? why : "");
      CHECK(!n48pc_config(0, 0, 3, 0, 0xF, &c, &why) && !n48pc_config(0, 0, 16, 0, 0xFF, &c, &why) && !n48pc_config(1, 0, 6, 0, 0xFF, &c, &why), "rasterSampleCount 3, 6 and 16 -> refused"); }
    // ---- source pins on the REAL Navi48Device.m ----
    char *m = slurp(dir, "Navi48Device.m");
    CHECK(m != NULL, "Navi48Device.m is readable");
    if (m) {
        CHECK(strstr(m, "#include \"n48_depth.h\"") != NULL, "Navi48Device.m includes n48_depth.h");
        CHECK(strstr(m, "- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }") != NULL && !strstr(m, "isDepth24Stencil8PixelFormatSupported { return YES; }"), "isDepth24Stencil8PixelFormatSupported answers NO");
        CHECK(strstr(m, "n48dp_barrier_aspect([t n48Aspects]), 0, [t n48Levels], 0, [t n48Layers] } };") != NULL, "every layout barrier (n48_tex_to) names the texture's own aspects, not COLOR");
        CHECK(strstr(m, "n48dp_barrier_aspect(_aspects), lv, 1, av.baseLayer, av.layerCount }") != NULL, "the attachment view carries every aspect of a depth/stencil format");
        CHECK(strstr(m, "isDS ? n48dp_view_aspect(_aspects) : VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, ti.layers }") != NULL, "the sampling view of a depth/stencil texture carries ONE aspect (n48dp_view_aspect)");
        CHECK(strstr(m, "if (!n48dp_check((unsigned long)tt, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned)ff, &why))") != NULL, "a depth/stencil texture descriptor goes through n48dp_check (Private only, 2D / 2DMS, device features)");
        CHECK(strstr(m, "if (!n48ms_texture_ok(ti.samples, fc,") != NULL && strstr(m, ".samples = (VkSampleCountFlagBits)ti.samples") != NULL, "a multisample texture is checked against the format's sample counts and created with ti.samples");
        CHECK(strstr(m, "return n48ms_device_supports((unsigned long)n, N48R.ok ? 1 : 0, m, m) ? YES : NO; }") != NULL && strstr(m, "- (BOOL)supportsTextureSampleCount:(NSUInteger)n { return [self supportsSampleCount:n]; }") != NULL, "supportsSampleCount: / supportsTextureSampleCount: answer from n48ms_device_supports");
        CHECK(strstr(m, "if (isDS) iu = ((ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)") != NULL && strstr(m, "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);   // bundle 10") != NULL, "a depth image's usage comes from the device's feature bits (depth/stencil attachment, sampled, transfer)");
        // the colour format table never answers for a depth format; the depth table has exactly the four formats
        { const char *a = strstr(m, "static const N48Fmt n48_fmts[] = {"), *b = a ? strstr(a, "\n};") : NULL;
          CHECK(a && b && !memmem(a, (size_t)(b - a), "Depth", 5) && !memmem(a, (size_t)(b - a), "Stencil", 7), "the colour format table (n48_fmts) holds no depth / stencil format (n48_fmt answers NULL for them as before)");
          const char *c = strstr(m, "static const N48Fmt n48_dsfmts[] = {"), *e = c ? strstr(c, "\n};") : NULL;
          CHECK(c && e && count(c, "N48F_DS }") >= 1 && !memmem(c, (size_t)(e - c), "Depth24", 7) && !memmem(c, (size_t)(e - c), "X32_Stencil8", 12) && !memmem(c, (size_t)(e - c), "X24_Stencil8", 12) &&
                memmem(c, (size_t)(e - c), "MTLPixelFormatDepth16Unorm", 26) && memmem(c, (size_t)(e - c), "MTLPixelFormatDepth32Float,", 27) && memmem(c, (size_t)(e - c), "MTLPixelFormatStencil8", 22) && memmem(c, (size_t)(e - c), "MTLPixelFormatDepth32Float_Stencil8", 35),
                "the depth table holds Depth16Unorm, Depth32Float, Stencil8, Depth32Float_Stencil8 and no D24S8 / stencil-view format"); }
        // pipelines
        CHECK(strstr(m, "n48pc_config(d.depthAttachmentPixelFormat != MTLPixelFormatInvalid, d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid, (unsigned long)d.rasterSampleCount") != NULL, "pipeline creation asks n48pc_config with the descriptor's depth / stencil formats and rasterSampleCount");
        CHECK(!strstr(m, "depth/stencil attachments not implemented in 10d"), "the old depth/stencil refusal is gone");
        CHECK(strstr(m, ".pDepthStencilState = _pc.hasDS ? &dss : NULL") != NULL && strstr(m, ".rasterizationSamples = (VkSampleCountFlagBits)_pc.samples") != NULL && strstr(m, ".dynamicStateCount = _pc.ndyn") != NULL, "the Vulkan pipeline takes the depth/stencil state, the sample count and the dynamic states from the config");
        CHECK(strstr(m, "n48ds_decode(dk, &dv);") != NULL && strstr(m, ".depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp") != NULL, "VkPipelineDepthStencilStateCreateInfo is filled from the decoded key (write enable, compare op)");
        CHECK(strstr(m, ".front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 }") != NULL &&
              strstr(m, ".back = { (VkStencilOp)dv.back.failOp, (VkStencilOp)dv.back.passOp, (VkStencilOp)dv.back.depthFailOp, (VkCompareOp)dv.back.compareOp, 0, 0, 0 }") != NULL, "front and back VkStencilOpState get fail / pass / depthFail / compare in Vulkan's order");
        CHECK(strstr(m, "return n48ds_key(_dpf != MTLPixelFormatInvalid, _spf != MTLPixelFormatInvalid, di.cmp, di.write, &di.f, &di.b);") != NULL, "the variant key is n48ds_key of the pipeline's formats and the bound state (compare, WRITE flag, both faces)");
        CHECK(strstr(m, "uint32_t dk0 = [self n48DSKey:NULL];") != NULL && strstr(m, "((uint64_t)dk0 << 24)") != NULL && strstr(m, "((uint64_t)front << 16) | ((uint64_t)dk << 24));") != NULL, "the first (validation) pipeline variant is built with the default state's key (0 for colour-only: the old variant key)");
        CHECK(strstr(m, "ads[na] = (VkAttachmentDescription){ .format = f->vk, .samples = (VkSampleCountFlagBits)_pc.samples, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,") != NULL && strstr(m, ".pDepthStencilAttachment = nd ? &dref : NULL };") != NULL, "pipeline render-pass compatibility: colour attachments carry the sample count; the subpass names the depth/stencil attachment");
        CHECK(strstr(m, "separate depth and stencil attachment formats are not supported") != NULL, "separate depth and stencil pipeline formats are refused with a message");
        // render pass
        CHECK(strstr(m, "n48rp_ds(asp, dtI != nil, (unsigned)da.loadAction, stI != nil, (unsigned)sa.loadAction, [dt layout] == VK_IMAGE_LAYOUT_UNDEFINED, &ops);") != NULL, "the depth/stencil attachment's load / store come from n48rp_ds");
        CHECK(strstr(m, "_cvs[na] = (VkClearValue){ .depthStencil = { (float)da.clearDepth, (uint32_t)sa.clearStencil } };") != NULL, "clearDepth and clearStencil become the Vulkan clear value");
        CHECK(strstr(m, "[dt n48AttViewLevel:(uint32_t)lvl layer:(uint32_t)slc]") != NULL, "the depth/stencil attachment's level and slice select the view");
        CHECK(strstr(m, "n48_mk_rp_c(_ads, na, _nd, NO, &_rp1c)") != NULL && strstr(m, ".attachmentCount = na + _nd, .pAttachments = views") != NULL, "the first render pass and the framebuffer include the depth/stencil attachment");
        CHECK(strstr(m, "rp = n48_mk_rp_c(a2, _na, _nd, _fetch, &rpc);") != NULL && strstr(m, "a2[_na].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a2[_na].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;") != NULL, "a continuation pass LOADs the depth and stencil that the first part stored");
        CHECK(strstr(m, ".rasterizationSamples") != NULL && strstr(m, "n48sa_map((unsigned long)ca.storeAction, ca.resolveTexture != nil, tsm > 1, &sat)") != NULL, "each colour attachment's store action and resolve texture go through n48sa_map");
        CHECK(strstr(m, "k.na = na + nd; k.fetch = (fetch ? 1u : 0u) | (nd << 1);") != NULL, "the render-pass cache key counts the depth attachment (a colour-only pass: the old key)");
        // draws
        CHECK(strstr(m, "[_pso n48DSVk] != _dsVkFmt || [_pso n48SampleCount] != _encSamples") != NULL, "a draw checks that the pipeline's depth/stencil format and sample count are the pass's");
        CHECK(strstr(m, "uint32_t dk = [_pso n48DSKey:&_dsi];") != NULL && strstr(m, "vkp = [_pso pipelineForTopology:topo cull:cull front:front ds:dk];") != NULL && strstr(m, "uint32_t fkey = (uint32_t)front | (dk << 1);") != NULL, "a draw looks the pipeline up with the bound state's key, and the last-answer cache keys on it too");
        CHECK(strstr(m, "vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]); vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);") != NULL && strstr(m, "vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead)") != NULL &&
              strstr(m, "vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite)") != NULL && strstr(m, "vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2])") != NULL, "a draw sets the stencil masks, both references and the depth bias dynamically");
        CHECK(strstr(m, "- (void)setStencilReferenceValue:(uint32_t)v { _sref[0] = _sref[1] = v; }") != NULL && strstr(m, "_dsi = [(N48DepthStencilState *)s n48Info]; [_cb n48Retain:s];") != NULL && strstr(m, "_bias[0] = a; _bias[1] = N48R.dbClamp ? c : 0.0f; _bias[2] = b;") != NULL, "setStencilReferenceValue:, setDepthStencilState: and setDepthBias: record their values");
        CHECK(strstr(m, "- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i { if (i < _na) _sact[i] = a;") != NULL, "setColorStoreAction:atIndex: updates the attachment's store action (a late MultisampleResolve)");
        // resolve
        { const char *e = strstr(m, "- (void)endEncoding {\n    if (_ended) return;\n    if (!_begun && ![_cb n48Error]) [self n48BeginPass];"); const char *r = e ? strstr(e, "[self n48ResolveAll];") : NULL, *b = e ? strstr(e, "n48_full_barrier([_cb vk]);\n    N48LOGR(\"N48RenderEncoder endEncoding") : NULL, *ep = e ? strstr(e, "if (_inPass) { [self n48EndPass]; }") : NULL;   // build 18: the pass ends through the one n48EndPass helper (it ends the open occlusion query first)
          CHECK(e && r && b && ep && ep < r && r < b, "endEncoding ends the render pass, THEN resolves, THEN the barrier (resolve never runs inside a pass and is not skipped)"); }
        CHECK(strstr(m, "vkCmdResolveImage(cmd, [src vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [dst vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);") != NULL && strstr(m, "if (!sat.resolve) continue;") != NULL, "the resolve is a vkCmdResolveImage from the attachment's level / slice into the resolve texture's");
        CHECK(strstr(m, ".srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _slvl[i], _sslice[i], 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _rlvl[i], _rslice[i], 1 }") != NULL, "the resolve names the source level / slice and the destination level / slice");
        // blit and descriptors
        CHECK(strstr(m, ".imageSubresource = { [t n48CopyAspect], (uint32_t)level, n48td_base_layer(is3, slice), 1 }") != NULL && strstr(m, ".srcSubresource = { [a n48Aspects], (uint32_t)sl") != NULL && strstr(m, ".dstSubresource = { [b n48Aspects], (uint32_t)dl") != NULL, "blit copies use the texture's aspect (buffer copies: the single copy aspect; texture to texture: every aspect)");
        CHECK(strstr(m, "if ([t n48Samples] > 1) { [self n48Fail:[NSString stringWithFormat:@\"%s texture(%u): a %lu-sample") != NULL && strstr(m, "sampling a multisample texture (texture2d_ms) is not supported - resolve it first") != NULL, "binding a multisample texture to a sampled-image slot fails the command buffer with a clear message");
        CHECK(strstr(m, "if (root->_samples > 1) VFAIL(100") != NULL && strstr(m, "pixel-format views of a depth/stencil texture") != NULL, "views of multisample textures and pixel-format views of depth textures are refused");
        CHECK(strstr(m, "const N48Fmt *f = n48_fmt_any(d.pixelFormat); if (!f) return NO;") != NULL, "heap sizing knows the depth formats");
        CHECK(strstr(m, "_Static_assert((int)MTLPixelFormatDepth16Unorm == N48DP_MTL_DEPTH16") != NULL && strstr(m, "(int)MTLStoreActionStoreAndMultisampleResolve == N48SA_STORE_RESOLVE") != NULL && strstr(m, "(int)VK_STENCIL_OP_DECREMENT_AND_WRAP == N48DS_OP_DEC_WRAP") != NULL, "the header's numeric values are asserted against the real Metal and Vulkan enums");
    }
    printf("test-depth: %d checks, %d failed\n", checks, fails); return fails ? 1 : 0;
}
