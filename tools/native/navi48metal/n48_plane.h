// n48_plane.h: P2 "multi-plane IOSurface textures" - the pure geometry / validation of one plane of an IOSurface (host-testable, no Apple or Vulkan types).
// Navi48Device.m's -[N48Texture initWithDevice:descriptor:iosurface:plane:error:] feeds it the plane's own geometry (IOSurfaceGet*OfPlane) and the plane's byte
// offset inside the imported allocation (IOSurfaceGetBaseAddressOfPlane - IOSurfaceGetBaseAddress). Test: test-plane.c.
//   pc 0 : a surface that reports no planes (the plane-0 geometry is the surface's own); pc 1 : a one-plane surface; both accept only plane 0 (today's rule).
//   pc >= 2: bi-planar / tri-planar; plane p < pc is accepted when its geometry matches the descriptor.
// Refusal codes keep today's numbers: 74 plane index, 75 size, 76 bytes per element, 77 row bytes, 81 bound; 93 is new (plane offset not a multiple of the element size).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    size_t pc, plane;                       // IOSurfaceGetPlaneCount, the requested plane
    size_t pw, ph, pbpe, pbpr;              // the plane's width, height, bytes per element, bytes per row
    uint64_t poff;                          // the plane's byte offset from the imported base (UINT64_MAX = the plane lies below the base)
    uint64_t alloc;                         // IOSurfaceGetAllocSize
    uint64_t dw, dh, dbpp;                  // the descriptor's width, height and the pixel format's bytes per pixel
} n48pl_in;
typedef struct { int ok; int code; char why[192]; uint64_t off; size_t rowlen; } n48pl_out;   // rowlen = bufferRowLength in texels

static inline int n48pl_fail(n48pl_out *o, int code, const char *why) { o->ok = 0; o->code = code; snprintf(o->why, sizeof o->why, "%s", why); return 0; }

// Everything judged from the plane's own geometry and the descriptor (checks 74 / 75 / 76 / 77, plane-indexed). No allocation size, no base needed.
static inline int n48pl_geom(const n48pl_in *in, n48pl_out *o) {
    char b[192];
    o->ok = 1; o->code = 0; o->why[0] = 0; o->off = 0; o->rowlen = 0;
    if (in->pc <= 1) { if (in->plane != 0) { snprintf(b, sizeof b, "plane %lu of a single-plane surface", (unsigned long)in->plane); return n48pl_fail(o, 74, b); } }
    else if (in->plane >= in->pc) { snprintf(b, sizeof b, "plane %lu of a %zu-plane surface", (unsigned long)in->plane, in->pc); return n48pl_fail(o, 74, b); }
    // Build 7: a descriptor INSIDE the plane is accepted (Core Image asks for e.g. 308x514 on a 320x576 surface); a larger one is refused. The image extent stays the descriptor's, the row length comes from the surface.
    if (in->dw > in->pw || in->dh > in->ph) {
        snprintf(b, sizeof b, "descriptor %lux%lu exceeds IOSurface %zux%zu", (unsigned long)in->dw, (unsigned long)in->dh, in->pw, in->ph); return n48pl_fail(o, 75, b); }
    if (in->pbpe != in->dbpp) {
        snprintf(b, sizeof b, "IOSurface bytes per element %zu != %u bytes of pixel format", in->pbpe, (unsigned)in->dbpp); return n48pl_fail(o, 76, b); }
    if (in->pbpe == 0 || in->pbpr % in->pbpe || in->pbpr < in->pw * in->pbpe) {
        snprintf(b, sizeof b, "IOSurface bytesPerRow %zu not usable for a %zu-wide %zu-byte image", in->pbpr, in->pw, in->pbpe); return n48pl_fail(o, 77, b); }
    return 1;
}
// The plane must lie inside the allocation: poff + bpr*height <= alloc (overflow-safe), and the offset must be a whole number of elements (a Vulkan bufferOffset rule).
// On success o->off is the byte offset to import-relative bind / copy at and o->rowlen the bufferRowLength in texels.
static inline int n48pl_bound(const n48pl_in *in, n48pl_out *o) {
    char b[192];
    o->ok = 1; o->code = 0; o->why[0] = 0; o->off = 0; o->rowlen = 0;
    if (in->pbpe == 0) return n48pl_fail(o, 77, "plane has zero bytes per element");
    if (in->pbpr != 0 && in->ph > UINT64_MAX / in->pbpr) return n48pl_fail(o, 81, "bytesPerRow*height overflows");
    uint64_t need = (uint64_t)in->pbpr * in->ph;
    if (in->poff > in->alloc || need > in->alloc - in->poff) {
        snprintf(b, sizeof b, "plane %lu offset %llu + bytesPerRow*height %llu > allocSize %llu", (unsigned long)in->plane, (unsigned long long)in->poff, (unsigned long long)need, (unsigned long long)in->alloc);
        return n48pl_fail(o, 81, b); }
    if (in->poff % in->pbpe) {
        snprintf(b, sizeof b, "plane %lu offset %llu is not a multiple of its %zu-byte element", (unsigned long)in->plane, (unsigned long long)in->poff, in->pbpe); return n48pl_fail(o, 93, b); }
    o->off = in->poff; o->rowlen = in->pbpr / in->pbpe;
    return 1;
}
// Path (a): the LINEAR image may be bound straight into the import at the plane's offset only when the offset meets the image's alignment and the image fits behind it.
static inline int n48pl_bind_ok(uint64_t poff, uint64_t req_size, uint64_t req_align, uint64_t ialloc) {
    if (poff > ialloc || req_size > ialloc - poff) return 0;
    if (req_align && (poff % req_align)) return 0;
    return 1;
}
