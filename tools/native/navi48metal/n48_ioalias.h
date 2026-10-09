// n48_ioalias.h: bundle build 16, app-fix round 1 (an internal design note section P1, "Preview: the image disappears"). Pure C (host test: test-ioalias.c).
//
// THE PROBLEM. Core Image wraps ONE IOSurface in a NEW texture object for every use. A texture on "path (b)" (optimal image + a copy to / from the surface's pages) only copies back into the pages
// at the END of the command buffer, and uploads from the pages at its FIRST touch. So a second wrapper that reads the surface later in the same command buffer uploads stale pages (a fresh surface is
// zero: transparent), and a linear ("path (a)") sibling that writes the pages directly is overwritten at the end by the older path (b) image.
//
// THE MODEL. Per command buffer, per RANGE = (IOSurface id, plane byte offset): H = the surface's pages (the truth the CPU and path (a) wrappers see), and one image per path (b) wrapper object.
//   * a path (b) wrapper that WRITES makes its image newer than H (dirty) and every OTHER path (b) wrapper's image stale (not valid);
//   * a path (a) wrapper that writes changes H directly: every path (b) wrapper's image becomes stale;
//   * any object (other than the dirty writer) that touches the range first makes the dirty writer copy back NOW (flush), so H is current; the flushed writer is no longer dirty (the caller removes
//     it from the end-of-command-buffer write-back list, or it would clobber a later writer with older bytes);
//   * a path (b) wrapper that needs the contents and whose image is not valid uploads (again) from H, even if it was touched before in this command buffer.
// "When in doubt, copy back and re-upload": correct, only slower. Different planes (different offsets) never interact; the key includes the offset.
//
// THE GATE (n48ia_enabled): applications only. WindowServer stays byte-identical to build 14 and only COUNTS how often the case occurs (the decisions are computed but not acted on). Kill file
// /private/tmp/n48m-noioalias turns the actions off in applications too.
#ifndef N48_IOALIAS_H
#define N48_IOALIAS_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// 1 = act on the decisions. killedStat: the kill file exists (or could not be read: fail to the OLD behaviour, i.e. off).
static inline int n48ia_enabled(int isWs, int killedStat) { return !isWs && !killedStat; }

typedef struct { uintptr_t obj; uint8_t isB, valid; } n48ia_o;
typedef struct { uint32_t sid; uint64_t off; uintptr_t writer; uint8_t writerB, dirty; n48ia_o *o; size_t n, c; } n48ia_r;
typedef struct { n48ia_r *r; size_t n, c; } n48ia;
typedef struct { uintptr_t flush; int upload, reupload; } n48ia_act;   // flush: copy this writer back NOW (0 = none); upload: the toucher needs an upload / barrier; reupload: ... and it had been touched before

static inline void n48ia_init(n48ia *a) { memset(a, 0, sizeof *a); }
static inline void n48ia_destroy(n48ia *a) { for (size_t i = 0; i < a->n; i++) free(a->r[i].o); free(a->r); memset(a, 0, sizeof *a); }
static inline n48ia_r *n48ia_range(n48ia *a, uint32_t sid, uint64_t off) {
    for (size_t i = 0; i < a->n; i++) if (a->r[i].sid == sid && a->r[i].off == off) return &a->r[i];
    if (a->n == a->c) { size_t nc = a->c ? a->c * 2 : 8; n48ia_r *nr = (n48ia_r *)realloc(a->r, nc * sizeof *nr); if (!nr) return NULL; a->r = nr; a->c = nc; }
    n48ia_r *r = &a->r[a->n++]; memset(r, 0, sizeof *r); r->sid = sid; r->off = off; return r;
}
static inline n48ia_o *n48ia_obj(n48ia_r *r, uintptr_t obj, int isB, int *fresh) {
    for (size_t i = 0; i < r->n; i++) if (r->o[i].obj == obj) { *fresh = 0; return &r->o[i]; }
    if (r->n == r->c) { size_t nc = r->c ? r->c * 2 : 4; n48ia_o *no = (n48ia_o *)realloc(r->o, nc * sizeof *no); if (!no) return NULL; r->o = no; r->c = nc; }
    n48ia_o *o = &r->o[r->n++]; o->obj = obj; o->isB = (uint8_t)(isB != 0); o->valid = 0; *fresh = 1; return o;
}
// `obj` touches range (sid, off): isB = path (b) wrapper, need = it reads the surface's contents, write = it writes. The decision is returned; the state is updated as if the caller carried it out.
// On a failed allocation the answer is the conservative one (upload, no flush) and the state is left alone.
static inline n48ia_act n48ia_touch(n48ia *a, uint32_t sid, uint64_t off, uintptr_t obj, int isB, int need, int write) {
    n48ia_act d = { 0, 0, 0 };
    n48ia_r *r = n48ia_range(a, sid, off); int fresh = 0;
    n48ia_o *o = r ? n48ia_obj(r, obj, isB, &fresh) : NULL;
    if (!r || !o) { d.upload = need; return d; }
    if (r->writer && r->writer != obj && r->writerB && r->dirty) { d.flush = r->writer; r->dirty = 0; }   // the pages must be current before anybody else looks at or changes them
    if (isB) {
        if (need && !o->valid) { d.upload = 1; d.reupload = !fresh; o->valid = 1; }
        else if (!need && write) o->valid = 1;   // a full overwrite: the image is the newest copy
    } else {
        d.upload = need && (fresh || (r->writer && r->writer != obj)); d.reupload = d.upload && !fresh;   // a barrier only; "reupload" = touched before and somebody else wrote since
    }
    if (write) {
        for (size_t i = 0; i < r->n; i++) if (r->o[i].obj != obj && r->o[i].isB) r->o[i].valid = 0;
        r->writer = obj; r->writerB = (uint8_t)(isB != 0); r->dirty = (uint8_t)(isB != 0);
        if (isB) o->valid = 1;
    }
    return d;
}
// Writers still dirty at the end of the command buffer (what the end-of-command-buffer write-back must copy). Test helper.
static inline size_t n48ia_dirty(const n48ia *a, uintptr_t *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < a->n; i++) if (a->r[i].dirty && k < cap) out[k++] = a->r[i].writer;
    return k;
}
#endif
