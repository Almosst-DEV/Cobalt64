/* n48_spvrefl.h - minimal SPIR-V reflection for Navi48Metal pipeline creation (native #11 step 11e).
 * Ported from tools/native/shader-compile-census/compile-census.c (validated on 274/274 compositor-set modules):
 * descriptor sets/bindings from OpVariable + Decoration, push-constant size from member Offsets, vertex inputs from Location,
 * fragment colour outputs from Location/FragDepth. Names are prefixed n48s_. Header-only, static. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* ------------------------------------------------------------------------------------------------ SPIR-V model */
typedef struct {
   uint32_t op;        /* defining opcode (0 = unknown) */
   uint32_t width, n;  /* int/float width; vector/matrix/struct member count; array length id */
   uint32_t elem;      /* element / pointee / sampled type */
   uint32_t sc;        /* storage class (variable, pointer) */
   uint32_t dim, sampled;
   uint32_t cval;      /* OpConstant low word */
   uint32_t *mem;      /* struct members */
   uint32_t *moff, *mstride; /* member Offset / MatrixStride decorations (0xffffffff = none) */
   int set, bind, loc, builtin; /* -1 = none */
   int block, bufblock;
   uint32_t stride;    /* ArrayStride */
} N48sId;

typedef struct {
   uint32_t bound;
   N48sId *id;
   uint32_t ep_model, ep_id; char ep_name[128]; int has_ep;
} N48sMod;


static int n48s_parse(const uint32_t *w, size_t nw, N48sMod *m, char *why, size_t whyn)
{
   memset(m, 0, sizeof *m);
   if (w[0] != 0x07230203) { snprintf(why, whyn, "bad SPIR-V magic"); return 1; }
   m->bound = w[3];
   if (m->bound > 1000000) { snprintf(why, whyn, "id bound %u", m->bound); return 1; }
   m->id = calloc(m->bound + 1, sizeof(N48sId));
   for (uint32_t i = 0; i <= m->bound; i++) { m->id[i].set = m->id[i].bind = m->id[i].loc = m->id[i].builtin = -1; }
   /* pass 1: decorations and definitions (decorations may precede type defs, so member arrays are sized lazily) */
   size_t p = 5;
   while (p < nw) {
      uint32_t op = w[p] & 0xffff, wc = w[p] >> 16;
      if (!wc || p + wc > nw) { snprintf(why, whyn, "truncated instruction at word %zu", p); return 1; }
      const uint32_t *a = w + p + 1;
      #define ID(x) ((x) <= m->bound ? &m->id[x] : &m->id[0])
      switch (op) {
      case 15: /* OpEntryPoint model id name... */
         if (!m->has_ep) {
            m->has_ep = 1; m->ep_model = a[0]; m->ep_id = a[1];
            strncpy(m->ep_name, (const char *)&a[2], sizeof m->ep_name - 1);
         }
         break;
      case 71: { /* OpDecorate target decoration [args] */
         N48sId *t = ID(a[0]);
         switch (a[1]) {
         case 11: t->builtin = (int)a[2]; break;
         case 33: t->bind = (int)a[2]; break;
         case 34: t->set = (int)a[2]; break;
         case 30: t->loc = (int)a[2]; break;
         case 2: t->block = 1; break;
         case 3: t->bufblock = 1; break;
         case 6: t->stride = a[2]; break;
         }
         break; }
      case 72: { /* OpMemberDecorate struct member decoration [args] */
         N48sId *t = ID(a[0]); uint32_t mi = a[1];
         if (mi < 4096) {
            if (!t->moff) { t->moff = malloc(4096 * 4); t->mstride = malloc(4096 * 4); memset(t->moff, 0xff, 4096 * 4); memset(t->mstride, 0xff, 4096 * 4); }
            if (a[2] == 35) t->moff[mi] = a[3];
            if (a[2] == 7) t->mstride[mi] = a[3]; /* MatrixStride */
         }
         break; }
      case 21: { N48sId *t = ID(a[0]); t->op = op; t->width = a[1]; break; }
      case 22: { N48sId *t = ID(a[0]); t->op = op; t->width = a[1]; break; }
      case 20: { N48sId *t = ID(a[0]); t->op = op; t->width = 32; break; }
      case 23: case 24: { N48sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->n = a[2]; break; }
      case 25: { N48sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->dim = a[2]; t->sampled = a[6]; break; }
      case 26: { N48sId *t = ID(a[0]); t->op = op; break; }
      case 27: { N48sId *t = ID(a[0]); t->op = op; t->elem = a[1]; break; }
      case 28: { N48sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->n = a[2]; break; }
      case 29: { N48sId *t = ID(a[0]); t->op = op; t->elem = a[1]; break; }
      case 30: { N48sId *t = ID(a[0]); t->op = op; t->n = wc - 2; t->mem = malloc(4 * (t->n + 1)); for (uint32_t k = 0; k < t->n; k++) t->mem[k] = a[1 + k]; break; }
      case 32: { N48sId *t = ID(a[0]); t->op = op; t->sc = a[1]; t->elem = a[2]; break; }
      case 43: /* OpConstant type id value */ { N48sId *t = ID(a[1]); t->op = op; t->cval = a[2]; break; }
      case 59: /* OpVariable type id sc */ { N48sId *t = ID(a[1]); t->op = op; t->elem = a[0]; t->sc = a[2]; break; }
      }
      p += wc;
   }
   /* struct member decorations were attached to the struct id before its definition: nothing more to do (same N48sId slot). */
   return 0;
}

static uint32_t n48s_tsize(N48sMod *m, uint32_t t, int depth)
{
   if (depth > 32 || t > m->bound) return 0;
   N48sId *x = &m->id[t];
   switch (x->op) {
   case 21: case 22: return (x->width + 7) / 8;
   case 20: return 4;
   case 23: return x->n * n48s_tsize(m, x->elem, depth + 1);
   case 24: { uint32_t col = n48s_tsize(m, x->elem, depth + 1); N48sId *v = &m->id[x->elem]; (void)v; return x->n * col; }
   case 28: { N48sId *len = &m->id[x->n]; uint32_t es = x->stride ? x->stride : n48s_tsize(m, x->elem, depth + 1); return len->cval * es; }
   case 29: return 0;
   case 32: return 8;
   case 30: {
      uint32_t mx = 0;
      for (uint32_t k = 0; k < x->n; k++) {
         uint32_t off = (x->moff && x->moff[k] != 0xffffffffu) ? x->moff[k] : 0;
         uint32_t s = n48s_tsize(m, x->mem[k], depth + 1);
         /* a matrix member with a MatrixStride occupies n * stride */
         N48sId *mt = &m->id[x->mem[k]];
         if (mt->op == 24 && x->mstride && x->mstride[k] != 0xffffffffu) s = mt->n * x->mstride[k];
         if (off + s > mx) mx = off + s;
      }
      return mx; }
   }
   return 0;
}

/* ------------------------------------------------------------------------------------------------ layout */
#define N48S_MAXSET 32
#define N48S_MAXBIND 1024
typedef struct { int used; VkDescriptorType type; uint32_t count; } N48sBind;
typedef struct {
   N48sBind bind[N48S_MAXSET][N48S_MAXBIND]; int maxset;
   uint32_t pc_size;
   VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDescription vb[32]; int nva;
   int nout; int frag_depth; uint32_t outmask;   /* outmask: bit n set = the fragment stage writes output Location n (11e-2: an unwritten attachment keeps its contents, Metal semantics) */
   char why[256];
} N48sRefl;

static VkFormat n48s_fmt(N48sMod *m, uint32_t ty, uint32_t *ncomp)
{
   N48sId *x = &m->id[ty]; uint32_t n = 1;
   if (x->op == 23) { n = x->n; x = &m->id[x->elem]; }
   *ncomp = n;
   int flt = x->op == 22, sgn = 0;
   (void)sgn;
   /* OpTypeInt signedness is not stored: treat ints as UINT unless width says otherwise (dummy formats are fine) */
   if (flt && x->width == 32) { static const VkFormat f[] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (flt && x->width == 16) { static const VkFormat f[] = { 0, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (flt && x->width == 64) { static const VkFormat f[] = { 0, VK_FORMAT_R64_SFLOAT, VK_FORMAT_R64G64_SFLOAT, VK_FORMAT_R64G64B64_SFLOAT, VK_FORMAT_R64G64B64A64_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 32) { static const VkFormat f[] = { 0, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 16) { static const VkFormat f[] = { 0, VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 8) { static const VkFormat f[] = { 0, VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT }; return f[n > 4 ? 4 : n]; }
   return VK_FORMAT_R32G32B32A32_SFLOAT;
}

/* returns 0 ok, 1 = layout failure (reason in r->why) */
static int n48s_reflect(N48sMod *m, int stage /* 0 v, 1 f, 2 k */, N48sRefl *r)
{
   memset(r, 0, sizeof *r);
   r->maxset = -1;
   for (uint32_t i = 1; i <= m->bound; i++) {
      N48sId *v = &m->id[i];
      if (v->op != 59) continue;
      N48sId *ptr = &m->id[v->elem];
      uint32_t pointee = ptr->elem;
      if (v->sc == 9) { /* PushConstant */
         uint32_t s = n48s_tsize(m, pointee, 0); s = (s + 3) & ~3u;
         if (s > r->pc_size) r->pc_size = s;
      } else if (v->sc == 0 || v->sc == 2 || v->sc == 12) { /* UniformConstant, Uniform, StorageBuffer */
         uint32_t count = 1, t = pointee;
         while (m->id[t].op == 28 || m->id[t].op == 29) {
            if (m->id[t].op == 28) count *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1;
            t = m->id[t].elem;
         }
         N48sId *x = &m->id[t];
         VkDescriptorType dt;
         if (x->op == 30) dt = (v->sc == 12 || x->bufblock) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
         else if (x->op == 25) {
            if (x->dim == 6) dt = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            else if (x->dim == 5) dt = x->sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else dt = x->sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
         }
         else if (x->op == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
         else if (x->op == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
         else { snprintf(r->why, sizeof r->why, "unrecognised descriptor variable %%%u (sc %u, pointee op %u)", i, v->sc, x->op); return 1; }
         int set = v->set < 0 ? 0 : v->set, b = v->bind < 0 ? 0 : v->bind;
         if (set >= N48S_MAXSET) { snprintf(r->why, sizeof r->why, "set %d >= %d", set, N48S_MAXSET); return 1; }
         if (b >= N48S_MAXBIND) { snprintf(r->why, sizeof r->why, "binding %d >= %d", b, N48S_MAXBIND); return 1; }
         N48sBind *bd = &r->bind[set][b];
         if (bd->used && bd->type != dt) { snprintf(r->why, sizeof r->why, "set %d binding %d used as two descriptor types", set, b); return 1; }
         if (!bd->used || count > bd->count) bd->count = count;
         bd->used = 1; bd->type = dt;
         if (set > r->maxset) r->maxset = set;
      } else if (v->sc == 1 && v->builtin < 0) { /* Input */
         if (stage != 0) continue;
         if (v->loc < 0) continue;
         uint32_t t = pointee, reps = 1;
         while (m->id[t].op == 28) { reps *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1; t = m->id[t].elem; }
         uint32_t cols = 1;
         if (m->id[t].op == 24) { cols = m->id[t].n; t = m->id[t].elem; }
         uint32_t nc; VkFormat f = n48s_fmt(m, t, &nc);
         uint32_t slots = (m->id[m->id[t].op == 23 ? m->id[t].elem : t].width == 64 && nc > 2) ? 2 : 1;
         for (uint32_t k = 0; k < reps * cols; k++) {
            int loc = v->loc + (int)(k * slots);
            if (loc >= 32 || r->nva >= 32) { snprintf(r->why, sizeof r->why, "vertex input location %d >= 32", loc); return 1; }
            r->va[r->nva] = (VkVertexInputAttributeDescription){ (uint32_t)loc, (uint32_t)loc, f, 0 };
            r->vb[r->nva] = (VkVertexInputBindingDescription){ (uint32_t)loc, 32, VK_VERTEX_INPUT_RATE_VERTEX };
            r->nva++;
         }
      } else if (v->sc == 3 && stage == 1) { /* Output of a fragment shader */
         if (v->builtin == 22) { r->frag_depth = 1; continue; }
         if (v->builtin >= 0 || v->loc < 0) continue;
         uint32_t t = pointee, reps = 1;
         while (m->id[t].op == 28) { reps *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1; t = m->id[t].elem; }
         if ((int)(v->loc + reps) > r->nout) r->nout = v->loc + (int)reps;
         for (uint32_t k = 0; k < reps && v->loc + k < 32; k++) r->outmask |= 1u << (v->loc + k);
      }
   }
   if (r->nout > 8) { snprintf(r->why, sizeof r->why, "fragment outputs need %d color attachments (> 8)", r->nout); return 1; }
   return 0;
}


static void n48s_free(N48sMod *m)
{
   if (!m->id) return;
   for (uint32_t k = 0; k <= m->bound; k++) { free(m->id[k].mem); free(m->id[k].moff); free(m->id[k].mstride); }
   free(m->id); m->id = NULL;
}
