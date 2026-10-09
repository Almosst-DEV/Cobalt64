// n48_intfmt.h (bundle 19, NATIVE-S8-MENUS.md): the integer colour-format rules of the bundle, pure C so they are host-tested (test-intfmt.c) AND checked against Apple's Metal on the host Mac (test-intclear-semantics.m).
// Core Animation's large-shadow pass ping-pongs two MTLPixelFormatRG16Uint targets; refusing the format dropped the whole layer (menus, Spotlight). Two rules follow from accepting it:
//  (1) a Clear load action on an integer attachment fills VkClearColorValue.uint32, never .float32 (the float bits of a clear colour are not the integer the client asked for);
//  (2) the magenta FALLBACK fragment shader writes float4: into an integer attachment that is a type mismatch, so the pipeline masks the attachment off (colorWriteMask 0) until the real shader is hot-swapped in.
// Metal's integer clear semantics, MEASURED on the Apple M4 (macOS 27, RG16Uint, clearColor doubles; tools/native/mtlprobe `rg16uint` and test-intclear-semantics.m repeat it):
//    (12345, 65535) -> (12345, 65535)   (1.4, 1.5) -> (1, 1)   (2.5, 3.5) -> (2, 3)   (0.5, -1) -> (0, 0)   (-5, 70000) -> (0, 65535)   (65536, 65535.5) -> (65535, 65535)
//    (1e10, -1e10) -> (65535, 0)   (255.9999, 0.4999) -> (255, 0)   (4294967295, 1e300) -> (65535, 65535)   (NaN, 1) -> (0, 1)
//  i.e. per channel: NaN and anything <= 0 give 0; the fraction is TRUNCATED (not rounded: 1.5 -> 1, 2.5 -> 2, 3.5 -> 3); anything >= 2^bits - 1 saturates to 2^bits - 1.
#ifndef N48_INTFMT_H
#define N48_INTFMT_H
#include <stdint.h>

// Convert one MTLClearColor channel for an unsigned-integer attachment of `bits` bits per channel (1..32).
static inline uint32_t n48if_clear_u(double v, unsigned bits) {
    const uint32_t mx = bits >= 32 ? 0xFFFFFFFFu : (uint32_t)((1ull << bits) - 1ull);
    if (!(v > 0.0)) return 0;                  // NaN, -0, negatives
    if (v >= (double)mx) return mx;            // saturate
    return (uint32_t)v;                        // truncate toward zero
}

// The colour write mask of one pipeline attachment: `wm` is the mask the descriptor asked for (VkColorComponentFlags bits). A FALLBACK pipeline (the built-in float magenta shader) must not
// write into an integer attachment, so its mask is 0 there; real pipelines and float attachments keep the descriptor's mask unchanged.
static inline uint32_t n48if_write_mask(int fallback, int is_int_attachment, uint32_t wm) { return (fallback && is_int_attachment) ? 0u : wm; }

#endif
