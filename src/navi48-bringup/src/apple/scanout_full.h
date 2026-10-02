// scanout_full.h — build 0.0.542 ( NEXT; the push condition's FULL-RES SCANOUT READBACK): `accel scanout full`.
//
// WHY. The push condition needs the password capsule, the name pill and the upper-right text judged in a FULL-RESOLUTION readback
// of what the display engine SCANS OUT, not of WindowServer's layer (the imgdump531 64 KiB block dumps, streamed as hex through the
// driver log) and not of the 64x36 `scanout 4` thumbnail. This is the pure half of `accel scanout 9` (`scanout full` in the CLI).
//
// WHAT THE VERB DOES (the kext's navi48_scanout_full, Navi48Bringup.cpp):
//   1. READS the display engine through the READ-ONLY dcn41 device n48dcn::attach built at start() (navi48_liveraster.h
//      n48lr_scan_surface: its write callback writes nothing): the lit OTG (must be OTG0, as flip mode's fmHubpGeom requires), HUBP0's
//      SURFACE_FLIP_PENDING, SURFACE_EARLIEST_INUSE (what the hardware is scanning), PRIMARY_SURFACE_ADDRESS (what it was last told),
//      viewport, pitch, pixel format, SW_MODE, and OTG0's frame counter;
//   2. decides (n48_sf_decide, below) WHICH buffer that is: the console A or flip mode's B (switch 74) - nothing else is ever read;
//   3. copies the WHOLE surface, 1 MiB at a time, by SDMA0 QUEUE0 COPY_LINEAR into switch 89's GART read-back buffer (1 MiB of
//      physically contiguous system memory, bound once, its read-direction positive control passed), each chunk with fastcopy.h's
//      n48_fc_submit_wait (the bounded fence wait: N48_FC_FENCE_TIMEOUT_US from the doorbell), then memcpy's it into a kernel capture
//      buffer; a fence that does not land RETIRES the read-back buffer for the boot (n48_sf_run's `retire`) and ends the capture;
//   4. re-reads the display engine: the surface must not have moved (else CHANGED, the capture is discarded);
//   5. prefixes the 256-byte header below and publishes the capture; `navi48test` pulls it through the user client's
//      kNavi48SelReadScanFull (4 KiB per call) and writes header + payload to a file; `scanout 10` frees it.
// READ-ONLY toward the display and WindowServer: no DCN register is written (the read-only device has no writer), the surface is
// only ever an SDMA SOURCE, the engine's only destination is the read-back buffer (n48_sf_kick_ok, at the pure plan AND at the ring).
//
// THE SOURCE ADDRESS AND FLIP MODE (the reconciliation). The hardware is the truth: SURFACE_EARLIEST_INUSE is what the display is
// scanning out right now. It must equal PRIMARY_SURFACE_ADDRESS with SURFACE_FLIP_PENDING clear (else a flip is IN PROGRESS: refused,
// FLIP_PENDING). Flip mode's own `front` is the EARLIEST_INUSE its LAST PRESENT read BEFORE it flipped, so after every completed flip
// it names the OTHER buffer: a difference is expected, recorded (N48_SF_F_FM_FRONT_DIFFERS, both addresses in the header), and never
// overrides the hardware. What IS refused: flip mode's gFmLock held (a present or a restore running: FLIP_BUSY, the kext's try-lock),
// a restore requested or unfinished (RESTORE), and a scanned address that is neither A nor an allocated B (FOREIGN: dcnflip's test
// pattern, or anything of Apple's). While the capture runs the kext HOLDS gFmLock, so flip mode can neither flip nor write A or B.
//
// PURE: no kernel headers. The host test is tests/scanout_full_test.cpp.
#ifndef N48_SCANOUT_FULL_H
#define N48_SCANOUT_FULL_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N48_SF_MODE          9u                 /* `accel scanout 9` = `accel scanout full` */
#define N48_SF_MODE_RELEASE  10u                /* `accel scanout 10`: free the capture */
#define N48_SF_VERSION       2u   /* build 0.0.543 item E5: + viewport start, 63/89 latch state (v1 read those bytes as reserved 0) */
#define N48_SF_HDR_BYTES     256u
#define N48_SF_BPP           4u                 /* 32-bit planes only (fmt 8 ARGB8888 / 10 ARGB2101010, as flip mode admits) */
#define N48_SF_MAX_PAYLOAD   (16u << 20)        /* 2560 x 1440 x 4 = 14.1 MiB fits; anything larger is refused */
#define N48_SF_CHUNK_BYTES   (1u << 20)         /* == N48_FC_STAGING_BYTES: switch 89's read-back buffer (static_assert in the kext) */
#define N48_SF_SW_LINEAR     0u
#define N48_SF_SW_64KB_2D    3u                 /* gfx12 ADDR3_64KB_2D (scanout_copy.h n48_addr3_64kb_2d_off_4bpe) */

/* Why a capture was refused (0 = captured). The CLI prints n48_sf_reason_name. */
enum {
    N48_SF_OK = 0, N48_SF_R_NO_CONTEXT = 1, N48_SF_R_NO_DCN = 2, N48_SF_R_OTG = 3, N48_SF_R_TEST_PATTERN = 4,
    N48_SF_R_FLIP_BUSY = 5, N48_SF_R_FLIP_PENDING = 6, N48_SF_R_RESTORE = 7, N48_SF_R_FOREIGN = 8, N48_SF_R_GEOM = 9,
    N48_SF_R_STAGING = 10, N48_SF_R_STAGING_BUSY = 11, N48_SF_R_RETIRED = 12, N48_SF_R_QUEUE = 13, N48_SF_R_DCC = 14,
    N48_SF_R_ALLOC = 15, N48_SF_R_FENCE = 16, N48_SF_R_RING = 17, N48_SF_R_REFUSED = 18, N48_SF_R_FENCES = 19,
    N48_SF_R_CHANGED = 20, N48_SF_R_ARG = 21, N48_SF_R_COUNT = 22
};
static inline const char *n48_sf_reason_name(uint32_t r)
{
    static const char *const n[N48_SF_R_COUNT] = {
        "captured", "no bring-up context", "the read-only DCN device is not built or a read failed",
        "OTG0 is not the lit OTG (HUBP0 is not the scanned plane)", "dcnflip's test pattern is on screen",
        "FLIP BUSY: flip mode's lock is held (a present or a restore is running)",
        "FLIP IN PROGRESS: SURFACE_FLIP_PENDING set, or the programmed address is not the one in use",
        "a flip-mode restore is requested or unfinished", "FOREIGN: the scanned surface is neither the console A nor flip mode's B",
        "geometry: unsupported format/swizzle, or the surface does not fit its buffer / the 16 MiB cap",
        "no staging: switch 89's read-back buffer is not bound and proven (send `gfxneuter 345` first: 89 ON binds and proves it)",
        "the fast-copy lock is held (a residency copy is running)", "the read-back buffer was RETIRED this boot (a fence that did not land)",
        "SDMA0 QUEUE0 is not up", "SDMA0_DCC_CNTL has a COMP_EN bit set", "the capture buffer could not be allocated",
        "FENCE TIMEOUT: a chunk's fence did not land in the bound (the read-back buffer is retired)",
        "the ring write or the doorbell failed", "a chunk was refused at the gate (source / destination / length)",
        "fence values exhausted", "CHANGED: the scanned surface moved during the copy (discarded)", "bad argument" };
    return r < N48_SF_R_COUNT ? n[r] : "?";
}

/* What the display engine answered (n48lr_scan_surface fills it; READ-ONLY). */
typedef struct n48_sf_dcn {
    uint32_t dcn_ok;        /* 1 = the device is built and every read below was made */
    uint32_t lit_otg;       /* the first master-enabled OTG; 0xffffffff none */
    uint32_t pending;       /* HUBP0 SURFACE_FLIP_PENDING */
    uint32_t fmt;           /* HUBP0 DCSURF_SURFACE_CONFIG SURFACE_PIXEL_FORMAT (8 ARGB8888, 10 ARGB2101010) */
    uint32_t sw_mode;       /* HUBP0 DCSURF_TILING_CONFIG SW_MODE (0 linear) */
    uint32_t vp_w, vp_h;    /* HUBP0 PRI_VIEWPORT_DIMENSION */
    uint32_t vp_x, vp_y;    /* build 0.0.543 item E5: HUBP0 DCSURF_PRI_VIEWPORT_START (0x05e9, BASE_IDX 2; X 0..15, Y 16..31) */
    uint32_t pitch_px;      /* HUBPREQ0 DCSURF_SURFACE_PITCH + 1 */
    uint32_t frame_count;   /* OTG0 OTG_STATUS_FRAME_COUNT */
    uint64_t primary;       /* HUBPREQ0 PRIMARY_SURFACE_ADDRESS(_HIGH): what the hardware was last told */
    uint64_t earliest;      /* HUBPREQ0 SURFACE_EARLIEST_INUSE(_HIGH): what it is scanning out */
} n48_sf_dcn;

/* The kext's side: the console (A) and flip mode's record (switch 74). */
typedef struct {
    uint32_t con_ok;                              /* RDNA4FB's Console,* geometry read and checked; MC base proven */
    uint32_t con_w, con_h, con_row_bytes;
    uint64_t a_mc, a_off, a_len;                  /* the console: MC, VRAM offset, Console,Length */
    uint32_t fm_have_b, fm_on, fm_engaged, fm_restore_req, fm_a_copy_pending, fm_test_held;
    uint64_t b_mc, b_off, b_len, fm_front;
    uint32_t armed;                               /* an arm stands (recorded only) */
    /* build 0.0.543 item E5: the fast copy's state as the copy left it (read under the fast-copy lock, recorded only). */
    uint32_t fc63_latched, fc63_why;              /* switch 63's path latched off (n48_fc_state.latched_off) and why (N48_FC_LATCH_*) */
    uint32_t fc89_state, fc89_mode;               /* 89's read-back buffer: rb_ok | rb_retired << 1 | pc_done << 2; 89's mode (0 OFF) */
} n48_sf_ctx;

/* Header flags. */
#define N48_SF_F_FM_ON            0x01u   /* flip mode ON (switch 74) */
#define N48_SF_F_FM_ENGAGED       0x02u
#define N48_SF_F_FM_FRONT_DIFFERS 0x04u   /* flip mode's last-present front != EARLIEST_INUSE (expected after a completed flip) */
#define N48_SF_F_GEOM_DIFFERS     0x08u   /* HUBP0's viewport / pitch differ from RDNA4FB's Console geometry */
#define N48_SF_F_ARMED            0x10u
#define N48_SF_F_WRITTEN_DURING   0x20u   /* a scanout writer (a present copy) ran during the capture (flip mode OFF: A is live) */
#define N48_SF_F_B_UNENGAGED      0x40u   /* the display scans B while flip mode is not engaged */

typedef struct {
    uint32_t which;         /* 1 = A (the console), 2 = B (flip mode's back buffer) */
    uint32_t flags;
    uint64_t surf_mc, surf_off, buf_len, bytes;
} n48_sf_plan;

/* The payload's byte count for a 32-bit plane: linear = pitch x 4 x height; 64KB_2D = whole 128x128 blocks over the pitch (the
 * blocks-per-row n48_addr3_64kb_2d_off_4bpe uses). 0 = unsupported (any other SW_MODE, zero or inconsistent dimensions). */
static inline uint64_t n48_sf_payload_bytes(uint32_t sw_mode, uint32_t pitch_px, uint32_t w, uint32_t h)
{
    if (!w || !h || !pitch_px || pitch_px < w || w > 16384u || h > 16384u || pitch_px > 16384u) return 0u;
    if (sw_mode == N48_SF_SW_LINEAR) return (uint64_t)pitch_px * N48_SF_BPP * h;
    if (sw_mode == N48_SF_SW_64KB_2D) return (uint64_t)((pitch_px + 127u) >> 7) * (uint64_t)((h + 127u) >> 7) * 65536u;
    return 0u;
}

/* THE DECISION, in this order: the device read -> OTG0 -> dcnflip's pattern -> a flip IN PROGRESS -> a restore -> the console ->
 * A or B (else FOREIGN) -> the geometry. 0 = *p filled. */
static inline uint32_t n48_sf_decide(const n48_sf_dcn *d, const n48_sf_ctx *c, n48_sf_plan *p)
{
    memset(p, 0, sizeof(*p));
    if (!d || !c) return N48_SF_R_ARG;
    if (!d->dcn_ok) return N48_SF_R_NO_DCN;
    if (d->lit_otg != 0u) return N48_SF_R_OTG;
    if (c->fm_test_held) return N48_SF_R_TEST_PATTERN;
    if (d->pending || d->primary != d->earliest) return N48_SF_R_FLIP_PENDING;
    if (c->fm_restore_req || c->fm_a_copy_pending || (c->fm_engaged && !c->fm_on)) return N48_SF_R_RESTORE;
    if (!c->con_ok || !c->a_mc || !c->a_len) return N48_SF_R_NO_CONTEXT;
    if (d->earliest == c->a_mc) {
        p->which = 1u; p->surf_mc = c->a_mc; p->surf_off = c->a_off; p->buf_len = c->a_len;
    } else if (c->fm_have_b && c->b_mc && c->b_len && d->earliest == c->b_mc) {
        p->which = 2u; p->surf_mc = c->b_mc; p->surf_off = c->b_off; p->buf_len = c->b_len;
        if (!c->fm_engaged) p->flags |= N48_SF_F_B_UNENGAGED;
    } else {
        return N48_SF_R_FOREIGN;
    }
    if (d->fmt != 8u && d->fmt != 10u) return N48_SF_R_GEOM;
    const uint64_t bytes = n48_sf_payload_bytes(d->sw_mode, d->pitch_px, d->vp_w, d->vp_h);
    if (!bytes || (bytes & 3u) || bytes > N48_SF_MAX_PAYLOAD || bytes > p->buf_len) return N48_SF_R_GEOM;
    p->bytes = bytes;
    if (c->fm_on) p->flags |= N48_SF_F_FM_ON;
    if (c->fm_engaged) p->flags |= N48_SF_F_FM_ENGAGED;
    if (c->fm_engaged && c->fm_front != d->earliest) p->flags |= N48_SF_F_FM_FRONT_DIFFERS;
    if (d->vp_w != c->con_w || d->vp_h != c->con_h || (uint64_t)d->pitch_px * 4u != c->con_row_bytes) p->flags |= N48_SF_F_GEOM_DIFFERS;
    if (c->armed) p->flags |= N48_SF_F_ARMED;
    return N48_SF_OK;
}

/* THE GATE (checked by n48_sf_run for every chunk and again by the kext at the ring): the engine reads ONLY the planned surface
 * and writes ONLY the read-back buffer, and no more of it than it holds. 1 = allowed. */
static inline uint32_t n48_sf_kick_ok(uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, const n48_sf_plan *p, uint64_t rb_mc,
                                      uint32_t rb_bytes)
{
    if (!p || !p->bytes || !p->surf_mc || !rb_mc || !bytes || (bytes & 3u) || bytes > rb_bytes || rb_bytes > N48_SF_CHUNK_BYTES)
        return 0u;
    if (dst_mc != rb_mc) return 0u;
    if (src_mc < p->surf_mc || src_mc + bytes < src_mc || src_mc + bytes > p->surf_mc + p->bytes) return 0u;
    return 1u;
}

/* The submission's answers (== fastcopy.h's N48_FC_SUB_*; static_assert in the kext). */
enum { N48_SF_SUB_LANDED = 1, N48_SF_SUB_TIMEOUT = 2, N48_SF_SUB_REFUSED = 3, N48_SF_SUB_RING = 4 };

typedef struct {
    void *ctx;
    uint32_t (*fence_next)(void *ctx);                                                          /* 0 = exhausted */
    uint32_t (*submit)(void *ctx, uint64_t src_mc, uint64_t dst_mc, uint32_t bytes, uint32_t fence);   /* N48_SF_SUB_*; bounded */
    void (*retire)(void *ctx, uint32_t sub);   /* a rung doorbell without a landed fence: the buffer is never used again */
    void (*copy_out)(void *ctx, uint64_t off, uint32_t bytes);   /* the landed chunk -> the capture buffer at `off` */
} n48_sf_ops;

/* THE COPY, in order: per chunk the gate, a fresh fence value, ONE submission (whose fence wait is bounded), and only on a landed
 * fence the copy-out. ANY other answer ends the capture: a timeout or a ring failure retires the buffer first; nothing is retried. */
static inline uint32_t n48_sf_run(const n48_sf_ops *o, const n48_sf_plan *p, uint64_t rb_mc, uint32_t *chunks)
{
    *chunks = 0u;
    if (!o || !p || !p->bytes) return N48_SF_R_ARG;
    for (uint64_t off = 0; off < p->bytes;) {
        const uint64_t left = p->bytes - off;
        const uint32_t take = left < N48_SF_CHUNK_BYTES ? (uint32_t)left : N48_SF_CHUNK_BYTES;
        const uint64_t src = p->surf_mc + off;
        if (!n48_sf_kick_ok(src, rb_mc, take, p, rb_mc, N48_SF_CHUNK_BYTES)) return N48_SF_R_REFUSED;
        const uint32_t f = o->fence_next(o->ctx);
        if (!f) return N48_SF_R_FENCES;
        const uint32_t sub = o->submit(o->ctx, src, rb_mc, take, f);
        if (sub != N48_SF_SUB_LANDED) {
            if (sub == N48_SF_SUB_TIMEOUT || sub == N48_SF_SUB_RING) o->retire(o->ctx, sub);
            return sub == N48_SF_SUB_TIMEOUT ? N48_SF_R_FENCE : sub == N48_SF_SUB_RING ? N48_SF_R_RING : N48_SF_R_REFUSED;
        }
        o->copy_out(o->ctx, off, take);
        off += take;
        (*chunks)++;
    }
    return N48_SF_OK;
}

/* ---- THE FILE HEADER (256 bytes, little-endian; the capture buffer's first bytes, written verbatim to the file) ---- */
typedef struct {
    char     magic[8];          /*   0 "N48SCAN1" */
    uint32_t version;           /*   8 N48_SF_VERSION */
    uint32_t hdr_bytes;         /*  12 N48_SF_HDR_BYTES */
    uint32_t width, height;     /*  16 HUBP0 viewport */
    uint32_t pitch_bytes;       /*  24 HUBP0 pitch x 4 */
    uint32_t bpp;               /*  28 4 */
    uint32_t dcn_format;        /*  32 SURFACE_PIXEL_FORMAT (8 ARGB8888, 10 ARGB2101010) */
    uint32_t sw_mode;           /*  36 SW_MODE (0 linear, 3 64KB_2D) */
    uint64_t surface_mc;        /*  40 SURFACE_EARLIEST_INUSE: the surface copied */
    uint64_t primary_mc;        /*  48 PRIMARY_SURFACE_ADDRESS */
    uint64_t payload_bytes;     /*  56 */
    uint64_t surface_vram_off;  /*  64 */
    uint32_t which;             /*  72 1 A (console), 2 B (flip mode) */
    uint32_t flags;             /*  76 N48_SF_F_* */
    uint64_t fm_front;          /*  80 flip mode's last-present front */
    uint64_t a_mc, b_mc;        /*  88, 96 */
    uint64_t uptime_us;         /* 104 at the copy's start */
    uint64_t cal_sec;           /* 112 calendar time (UTC seconds) at the copy's start */
    uint32_t cal_usec;          /* 120 */
    uint32_t copy_us;           /* 124 the whole copy (every chunk's submission, wait and copy-out) */
    uint32_t chunks;            /* 128 */
    uint32_t fnv32;             /* 132 FNV-1a over the payload */
    uint32_t con_w, con_h, con_row_bytes;   /* 136 RDNA4FB's Console geometry */
    uint32_t lit_otg;           /* 148 */
    uint32_t fc_before, fc_after;           /* 152 OTG0's frame counter before and after the copy */
    uint64_t seq;               /* 160 capture number this boot */
    uint32_t writers_during;    /* 168 scanout writes during the copy (0.0.543 E3: counted at the write, under gScanoutLock) */
    uint32_t reserved0;         /* 172 */
    /* build 0.0.543 item E5 (version 2): */
    uint32_t vp_x, vp_y;        /* 176, 180 HUBP0 PRI_VIEWPORT_START */
    uint32_t fc63_latched;      /* 184 switch 63's fast-copy path latched OFF (1) at the copy's end */
    uint32_t fc63_why;          /* 188 its latch reason (fastcopy.h N48_FC_LATCH_*; 0 while not latched) */
    uint32_t fc89_state;        /* 192 89's read-back buffer: rb_ok | rb_retired << 1 | pc_done << 2 */
    uint32_t fc89_mode;         /* 196 89's mode (0 OFF, 1 ON, 3 SHADOW) */
    uint8_t  reserved[56];      /* 200 zero */
} n48_sf_hdr;

static inline uint32_t n48_sf_fnv32(const uint8_t *b, uint64_t n)
{
    uint32_t h = 2166136261u;
    for (uint64_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static inline void n48_sf_hdr_fill(n48_sf_hdr *h, const n48_sf_dcn *d, const n48_sf_ctx *c, const n48_sf_plan *p)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "N48SCAN1", 8);
    h->version = N48_SF_VERSION; h->hdr_bytes = N48_SF_HDR_BYTES;
    h->width = d->vp_w; h->height = d->vp_h; h->pitch_bytes = d->pitch_px * N48_SF_BPP; h->bpp = N48_SF_BPP;
    h->dcn_format = d->fmt; h->sw_mode = d->sw_mode;
    h->surface_mc = d->earliest; h->primary_mc = d->primary;
    h->payload_bytes = p->bytes; h->surface_vram_off = p->surf_off;
    h->which = p->which; h->flags = p->flags;
    h->fm_front = c->fm_front; h->a_mc = c->a_mc; h->b_mc = c->fm_have_b ? c->b_mc : 0u;
    h->con_w = c->con_w; h->con_h = c->con_h; h->con_row_bytes = c->con_row_bytes; h->lit_otg = d->lit_otg;
    h->vp_x = d->vp_x; h->vp_y = d->vp_y;                                   /* build 0.0.543 item E5 */
    h->fc63_latched = c->fc63_latched; h->fc63_why = c->fc63_why; h->fc89_state = c->fc89_state; h->fc89_mode = c->fc89_mode;
}

/* The reader's check (the CLI before it writes the file): 0 = consistent; else the first field that is not. */
static inline uint32_t n48_sf_hdr_check(const n48_sf_hdr *h, uint64_t total)
{
    if (total < N48_SF_HDR_BYTES || memcmp(h->magic, "N48SCAN1", 8) != 0) return 1u;
    if (h->version != N48_SF_VERSION || h->hdr_bytes != N48_SF_HDR_BYTES) return 2u;
    if (h->payload_bytes > N48_SF_MAX_PAYLOAD || h->payload_bytes + N48_SF_HDR_BYTES != total) return 3u;
    if (h->bpp != N48_SF_BPP || n48_sf_payload_bytes(h->sw_mode, h->pitch_bytes / N48_SF_BPP, h->width, h->height) != h->payload_bytes)
        return 4u;
    return 0u;
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(n48_sf_hdr) == N48_SF_HDR_BYTES, "the scanout-full header is 256 bytes");
#else
_Static_assert(sizeof(n48_sf_hdr) == N48_SF_HDR_BYTES, "the scanout-full header is 256 bytes");
#endif

#endif /* N48_SCANOUT_FULL_H */
