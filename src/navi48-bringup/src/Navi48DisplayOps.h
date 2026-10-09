/*
 *  Navi48DisplayOps.h - the interface between the bring-up kext (owner of the GPU and of the monitor B's and the monitor A's display planes; ESP-injected: rebuilds are free) and the Navi48Framebuffer of the
 *  Navi48Accel aux kext (ANY rebuild of the aux kext needs the new-install-path procedure), an internal design note "M5 build spec: Navi48Framebuffer for the monitor B", section 3, and (0.0.658,
 *  ABI 2) "monitor A (instance 1) plane + framebuffer spec at 1080p60", corrections 5 and 6 and section 4.
 *  BYTE-IDENTICAL copies live in src/navi48-bringup/src/ and tools/native/navi48accel/src/ (both host tests compare them). Plain C, append-only, like Navi48MetalOps.h.
 *
 *  RULE: the aux framebuffer is THIN. It answers IOFramebuffer's questions from an IMMUTABLE SNAPSHOT (N48DispSnap) taken ONCE by the bring-up kext's `fbpublish 2` verb (after the
 *  plane was held: `disp2 fbhold 2`), and it touches no register. Every decision (is the snapshot sane, may the framebuffer start) is made by the functions at the end of this file,
 *  which both sides compile (so they cannot disagree).
 *
 *  Reached through the nub: nub->callPlatformFunction(N48_DISP_FN_SYMBOL, false, &ops (a const struct N48DispOps pointer), 0, 0, 0) returns kIOReturnSuccess and stores a pointer to a
 *  STATIC const table (valid while the bring-up kext is loaded).
 */
#ifndef NAVI48_DISPLAY_OPS_H
#define NAVI48_DISPLAY_OPS_H

#include <stdint.h>

#define N48_DISP_FN_SYMBOL    "n48.disp.ops"
#define N48_DISP_NUB_CLASS    "Navi48DisplayNub"
#define N48_DISP_INDEX_KEY    "Navi48DisplayIndex"      /* the nub's OSNumber property; the aux personalities match Navi48DisplayIndex = 1 (the monitor B) and = 2 (the monitor A, ABI 2) */
#define N48_DISP_ABI          2u                        /* 0.0.658: ABI 2 = the per-index geometry table below (the table the bring-up kext builds snapshots from and both sides validate them against); the struct shapes are ABI 1's */
#define N48_DISP_ABI_MIN      1u                        /* the oldest table the monitor B's framebuffer (index 1) still accepts: an ABI-1 bring-up kext (0.0.652..657) publishes index 1 only */
#define N48_DISP_ABI_MULTI    2u                        /* index 2 (the monitor A) needs a bring-up kext that speaks ABI 2: an ABI-1 table is refused cleanly (N48_DOV_ABI) */
#define N48_DISP_OPS_MAGIC    0x4438344Eu               /* 'N48D' little-endian (N = 0x4E is the low byte) */
#define N48_DISP_OPS_MIN      72u                       /* ABI 1 and 2: every consumer requires at least this much */

/* the monitor B SINK-C's mode, the one mode the M5 framebuffer offers (the CTA DTD at EDID block 1 + 0x64: 2560x1440 @ 59.95 Hz, 241.50 MHz, 2720 x 1481 total) */
#define N48_DISP_INDEX        1u
#define N48_DISP_OTG          2u
#define N48_DISP_DDC_LINE     3u
#define N48_DISP_W            2560u
#define N48_DISP_H            1440u
#define N48_DISP_PITCH        10240u                    /* 2560 * 4 */
#define N48_DISP_FMT_ARGB8888 1u                        /* SURFACE_CONFIG 8; the kext's fill is 0xFFRRGGBB */
#define N48_DISP_PIXHZ        241500000ull
#define N48_DISP_HTOTAL       2720ull
#define N48_DISP_VTOTAL       1481ull
#define N48_DISP_REFRESH1616  ((uint32_t)((N48_DISP_PIXHZ << 16) / (N48_DISP_HTOTAL * N48_DISP_VTOTAL)))   /* 16.16 fixed point, about 59.95 Hz */
#define N48_DISP_BYTES        14745600ull               /* 2560 * 1440 * 4 = 225 * 64 KiB */
#define N48_DISP_EDID_BLOCKS  2u
#define N48_DISP_EDID_BYTES   256u

/* 0.0.658 (ABI 2): the monitor A SINK-B's mode (instance 1 = display index 2): 1920x1080 @ 60 Hz, 148.50 MHz, 2200 x 1125 total, DDC line 2. The aperture is the exact frame (8,294,400 = 2025 pages); the allocation behind it is 127 x 64 KiB = 8,323,072 bytes. */
#define N48_DISPA_INDEX       2u
#define N48_DISPA_OTG         1u
#define N48_DISPA_DDC_LINE    2u
#define N48_DISPA_W           1920u
#define N48_DISPA_H           1080u
#define N48_DISPA_PITCH       7680u                     /* 1920 * 4 */
#define N48_DISPA_PIXHZ       148500000ull
#define N48_DISPA_HTOTAL      2200ull
#define N48_DISPA_VTOTAL      1125ull
#define N48_DISPA_REFRESH1616 0x3C0000u                 /* exactly 60 Hz = (148500000 << 16) / (2200 * 1125) */
#define N48_DISPA_BYTES       8294400ull                /* 1920 * 1080 * 4 */
#define N48_DISP_INST_MONA    1u                        /* the disp2 instances (navi48_disp2.h N48D2_INST_*): the monitor A is INSTANCE 1 but display INDEX 2, the monitor B instance 2 but index 1 (correction 6): ONE table maps them */
#define N48_DISP_INST_MONB    2u

/* flags (N48DispOps.flags): none defined in ABI 1 (0) */
/* trace events (framebuffer -> bring-up; the bring-up kext decides what to log) */
#define N48_DTR_START_OK      1u    /* a = the framebuffer object */
#define N48_DTR_START_REFUSED 2u    /* a = the N48_DSV_* / start verdict */
#define N48_DTR_POWER         3u    /* a = the power state recorded */
#define N48_DTR_ENABLE        4u    /* enableController */
#define N48_DTR_SETMODE       5u    /* a = mode id, b = depth */

struct N48DispSnap {
    uint32_t index;          /*   0  the display index: N48_DISP_INDEX (1, the monitor B) or N48_DISPA_INDEX (2, the monitor A, ABI 2); the geometry below is the n48disp_geom_for_index() row of this index */
    uint32_t otg;            /*   4  the monitor B: 2 (OTG2 / HUBP2 / DIG3); the monitor A: 1 (OTG1 / HUBP1 / DIG2) */
    uint32_t ddcLine;        /*   8  the monitor B: 3; the monitor A: 2 */
    uint32_t w;              /*  12  2560 / 1920 */
    uint32_t h;              /*  16  1440 / 1080 */
    uint32_t pitchBytes;     /*  20  10240 / 7680 */
    uint32_t fmt;            /*  24  N48_DISP_FMT_ARGB8888 */
    uint32_t refresh1616;    /*  28  N48_DISP_REFRESH1616 / N48_DISPA_REFRESH1616 */
    uint64_t pixHz;          /*  32  241500000 / 148500000 */
    uint64_t aperPhys;       /*  40  bar0Phys + off[0]: the CPU-visible address of buffer A (64 KiB aligned) */
    uint64_t aperLen;        /*  48  N48_DISP_BYTES / N48_DISPA_BYTES: the exact frame (the allocation behind it is a multiple of 64 KiB) */
    uint64_t mcA;            /*  56  the MC address of buffer A (what HUBP2 scans) */
    uint64_t mcB;            /*  64  the MC address of buffer B (never scanned in M5) */
    uint32_t edidLen;        /*  72  256 */
    uint8_t  edid[256];      /*  76  blocks 0 and 1 of the display, read over its DDC line (the monitor B: 3, the monitor A: 2) */
};                           /* 336 (8-byte padded) */

struct N48DispOps {
    uint32_t magic;          /*   0  N48_DISP_OPS_MAGIC */
    uint32_t abi;            /*   4  N48_DISP_ABI */
    uint32_t size;           /*   8  sizeof(struct N48DispOps) as built into the bring-up kext */
    uint32_t kext_build;     /*  12  the bring-up kext build (658 = 0.0.658) */
    uint32_t flags;          /*  16  0 */
    uint32_t rsv;            /*  20  0 */
    int  (*snapshot)(void *nub, struct N48DispSnap *out);   /*  24  copies the IMMUTABLE snapshot OF THAT NUB (one nub per display index); 0 = ok, non-zero = refused (not one of our nubs / none published) */
    int  (*power)(void *nub, uint32_t state);               /*  32  M5: logs only (the monitor B keeps its last image); 0 = ok */
    void (*trace)(uint32_t ev, uint64_t a, uint64_t b);     /*  40  N48_DTR_*; NULL = silent */
    void *flip;              /*  48  NULL in ABI 1; M6: int (*flip)(void *nub, uint32_t idx) */
    void *flip_status;       /*  56  NULL in ABI 1; M6: int (*flip_status)(void *nub, uint64_t *out) */
    void *vbl_sample;        /*  64  NULL in ABI 1; M6: int (*vbl_sample)(void *nub, uint64_t *out) */
};                           /*  72 */

#ifdef __cplusplus
static_assert(sizeof(struct N48DispSnap) == 336, "N48DispSnap is 336 bytes");
static_assert(__builtin_offsetof(struct N48DispSnap, edidLen) == 72 && __builtin_offsetof(struct N48DispSnap, edid) == 76, "N48DispSnap: edid at 76");
static_assert(sizeof(struct N48DispOps) == N48_DISP_OPS_MIN, "N48DispOps ABI 1 and 2 are 72 bytes");
static_assert(__builtin_offsetof(struct N48DispOps, snapshot) == 24 && __builtin_offsetof(struct N48DispOps, flip) == 48, "N48DispOps: hooks at 24, M6 placeholders at 48");
#else
_Static_assert(sizeof(struct N48DispSnap) == 336, "N48DispSnap is 336 bytes");
_Static_assert(sizeof(struct N48DispOps) == N48_DISP_OPS_MIN, "N48DispOps ABI 1 and 2 are 72 bytes");
#endif

/* ---- the per-index geometry table (0.0.658, ABI 2): the ONE table that maps a disp2 instance to its display index, OTG, DDC line and mode (spec correction 6). Both kexts and the CLI compile it. ---------------------------------------- */
struct N48DispGeom {
    uint32_t index, inst, otg, ddcLine, w, h, pitchBytes, fmt, refresh1616;
    uint64_t pixHz, aperLen;
};
/* 1 when `index` is a known display index (1 = the monitor B on instance 2 / OTG2 / DDC line 3, 2 = the monitor A on instance 1 / OTG1 / DDC line 2) and *g is filled; 0 otherwise (g untouched) */
static inline int n48disp_geom_for_index(uint32_t index, struct N48DispGeom *g)
{
    if (!g) return 0;
    if (index == N48_DISP_INDEX) {
        g->index = N48_DISP_INDEX; g->inst = N48_DISP_INST_MONB; g->otg = N48_DISP_OTG; g->ddcLine = N48_DISP_DDC_LINE; g->w = N48_DISP_W; g->h = N48_DISP_H; g->pitchBytes = N48_DISP_PITCH;
        g->fmt = N48_DISP_FMT_ARGB8888; g->refresh1616 = N48_DISP_REFRESH1616; g->pixHz = N48_DISP_PIXHZ; g->aperLen = N48_DISP_BYTES;
        return 1;
    }
    if (index == N48_DISPA_INDEX) {
        g->index = N48_DISPA_INDEX; g->inst = N48_DISP_INST_MONA; g->otg = N48_DISPA_OTG; g->ddcLine = N48_DISPA_DDC_LINE; g->w = N48_DISPA_W; g->h = N48_DISPA_H; g->pitchBytes = N48_DISPA_PITCH;
        g->fmt = N48_DISP_FMT_ARGB8888; g->refresh1616 = N48_DISPA_REFRESH1616; g->pixHz = N48_DISPA_PIXHZ; g->aperLen = N48_DISPA_BYTES;
        return 1;
    }
    return 0;
}
/* the display index of a disp2 instance: instance 2 (the monitor B) -> 1, instance 1 (the monitor A) -> 2, anything else -> 0 (none) */
static inline uint32_t n48disp_index_for_inst(uint32_t inst)
{
    return inst == N48_DISP_INST_MONB ? N48_DISP_INDEX : inst == N48_DISP_INST_MONA ? N48_DISPA_INDEX : 0u;
}

/* ---- the status of the bring-up kext's verb `fbpublish 1|2` (accel action 105; out[0]); the CLI (navi48test.c) prints them with n48disp_fbp_name -------------------------------------------------------------------- */
#define N48_FBP_OK              0u
#define N48_FBP_BAD_ARG         1u
#define N48_FBP_OFF             2u
#define N48_FBP_NO_DEVICE       3u
#define N48_FBP_BUSY            4u
#define N48_FBP_NOT_HELD        5u
#define N48_FBP_EXISTS          6u
#define N48_FBP_PIPE_ADOPTED    7u
#define N48_FBP_WS_OPEN         8u
#define N48_FBP_GATE            9u
#define N48_FBP_EDID_READ       10u
#define N48_FBP_EDID_BAD        11u
#define N48_FBP_EDID_MISMATCH   12u
#define N48_FBP_APERTURE        13u
#define N48_FBP_SNAPSHOT        14u
#define N48_FBP_PUBLISH_FAILED  15u
#define N48_FBP_METAL_NUB       16u    /* 0.0.653: the Metal nub was already published this boot (the accelerator and its display machine may exist) */
#define N48_FBP_COUNT           17u
static inline const char *n48disp_fbp_name(uint32_t s)
{
    switch (s) {
    case N48_FBP_OK: return "OK: the display nub is published (Navi48DisplayIndex 1 for `fbpublish 2` = the monitor B, 2 for `fbpublish 1` = the monitor A); the aux framebuffer may now match it";
    case N48_FBP_BAD_ARG: return "bad argument (fbpublish 2 = the monitor B, fbpublish 1 = the monitor A: the disp2 instance, nothing else)";
    case N48_FBP_OFF: return "boot-arg navi48-fb2 is not 1: the verb is OFF, nothing was touched";
    case N48_FBP_NO_DEVICE: return "no bound display layer (BAR5 / IP discovery / allowlist not armed)";
    case N48_FBP_BUSY: return "another disp2 / fbpublish call is running";
    case N48_FBP_NOT_HELD: return "the plane of this instance is not HELD (run plane, show, fbhold on the same instance first): REFUSED, no register was read";
    case N48_FBP_EXISTS: return "this instance's display nub is already published (there is no withdraw in M5: reboot)";
    case N48_FBP_PIPE_ADOPTED: return "the display pipe is adopted (pipeadopt): the accelerator's display machine could give one of our frames to the DP; REFUSED, nothing was read";
    case N48_FBP_WS_OPEN: return "a WindowServer native session is open: REFUSED, nothing was read";
    case N48_FBP_GATE: return "a live gate failed (EARLIEST_INUSE is not buffer A, FLIP_PENDING, underflow, SEG_ALLOC_ERR, TIMEOUT, ODM underflow, a VM fault or DET): REFUSED";
    case N48_FBP_EDID_READ: return "the display's EDID could not be read over its DDC line (the monitor B: line 3, the monitor A: line 2; blocks 0 and 1): REFUSED";
    case N48_FBP_EDID_BAD: return "an EDID block has a wrong checksum, or block 0 lacks the EDID header: REFUSED";
    case N48_FBP_EDID_MISMATCH: return "EDID block 0 differs from (or RDNA4FB's EDID,DDC<line> property is missing for) the one RDNA4FB published (the monitor B: EDID,DDC3, the monitor A: EDID,DDC2): REFUSED";
    case N48_FBP_APERTURE: return "the aperture (BAR0 physical + buffer A's offset, the exact frame: 14,745,600 bytes for the monitor B, 8,294,400 for the monitor A) is misaligned or outside BAR0: REFUSED";
    case N48_FBP_SNAPSHOT: return "the snapshot failed its validation: REFUSED, nothing published";
    case N48_FBP_PUBLISH_FAILED: return "the nub could not be created or attached";
    case N48_FBP_METAL_NUB: return "the Metal nub was already published this boot (the accelerator's display machine could give one of our frames to the DP): REFUSED, nothing was read; reboot, run fbpublish before any Metal publish";
    default: return "unknown";
    }
}

/* ---- the shared decisions (both sides compile these; the host tests drive them) ---------------------------------------------------------------------------------------------------- */

/* ops-table verdicts (the framebuffer's start refuses anything but N48_DOV_OK) */
#define N48_DOV_OK        0u
#define N48_DOV_NULL      1u
#define N48_DOV_MAGIC     2u
#define N48_DOV_ABI       3u
#define N48_DOV_SIZE      4u
#define N48_DOV_MISSING   5u    /* snapshot / power hook absent */
static inline uint32_t n48disp_ops_check(const struct N48DispOps *o)
{
    if (!o) return N48_DOV_NULL;
    if (o->magic != N48_DISP_OPS_MAGIC) return N48_DOV_MAGIC;
    if (o->abi < N48_DISP_ABI_MIN) return N48_DOV_ABI;             /* append-only: ABI 1 and every newer abi are accepted (the monitor B's index 1) */
    if (o->size < N48_DISP_OPS_MIN) return N48_DOV_SIZE;
    if (!o->snapshot || !o->power) return N48_DOV_MISSING;
    return N48_DOV_OK;
}

/* 0.0.658: the ops check FOR A DISPLAY INDEX: index 2 (the monitor A) additionally needs ABI >= 2, so an ABI-1 bring-up kext (0.0.652..657, which knows no monitor A geometry) is refused cleanly with N48_DOV_ABI.
   An unknown index (neither 1 nor 2) is refused with N48_DOV_ABI too (the framebuffer was matched to a nub this build does not know). */
static inline uint32_t n48disp_ops_check_for(const struct N48DispOps *o, uint32_t index)
{
    const uint32_t v = n48disp_ops_check(o);
    if (v != N48_DOV_OK) return v;
    if (index == N48_DISP_INDEX) return N48_DOV_OK;
    if (index == N48_DISPA_INDEX) return o->abi >= N48_DISP_ABI_MULTI ? N48_DOV_OK : N48_DOV_ABI;
    return N48_DOV_ABI;
}

/* EDID: one 128-byte block sums to 0 (mod 256); block 0 also starts 00 FF FF FF FF FF FF 00 */
static inline int n48disp_edid_sum_ok(const uint8_t *b)
{
    unsigned i; uint32_t s = 0;
    for (i = 0; i < 128u; i++) s += b[i];
    return (s & 0xFFu) == 0u;
}
static inline int n48disp_edid_header_ok(const uint8_t *b)
{
    return b[0] == 0x00 && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF && b[4] == 0xFF && b[5] == 0xFF && b[6] == 0xFF && b[7] == 0x00;
}

/* snapshot verdicts: 0 = valid, else the first thing that is wrong. The bring-up kext builds the snapshot and calls this BEFORE publishing; the framebuffer calls it again before it starts. */
#define N48_DSV_OK          0u
#define N48_DSV_NULL        1u
#define N48_DSV_INDEX       2u
#define N48_DSV_GEOMETRY    3u    /* otg / ddcLine / w / h / pitch / fmt / refresh / pixel clock / aperture length */
#define N48_DSV_APERTURE    4u    /* aperPhys zero, not 64 KiB aligned, or wrapping */
#define N48_DSV_MC          5u    /* mcA zero / not 64 KiB aligned / equal to mcB / mcB zero or not aligned */
#define N48_DSV_EDID_LEN    6u
#define N48_DSV_EDID_SUM    7u    /* a block's checksum is wrong */
#define N48_DSV_EDID_HDR    8u    /* block 0 lacks the EDID header */
static inline uint32_t n48disp_snap_valid(const struct N48DispSnap *s)
{
    struct N48DispGeom g;
    if (!s) return N48_DSV_NULL;
    if (!n48disp_geom_for_index(s->index, &g)) return N48_DSV_INDEX;       /* 0.0.658: index 1 = the monitor B (2560x1440), index 2 = the monitor A (1920x1080); anything else is refused */
    if (s->otg != g.otg || s->ddcLine != g.ddcLine || s->w != g.w || s->h != g.h || s->pitchBytes != g.pitchBytes ||
        s->fmt != g.fmt || s->refresh1616 != g.refresh1616 || s->pixHz != g.pixHz || s->aperLen != g.aperLen) return N48_DSV_GEOMETRY;
    if (s->aperPhys == 0u || (s->aperPhys & 0xFFFFull) != 0u || s->aperPhys + s->aperLen < s->aperPhys) return N48_DSV_APERTURE;
    if (s->mcA == 0u || (s->mcA & 0xFFFFull) != 0u || s->mcB == 0u || (s->mcB & 0xFFFFull) != 0u || s->mcA == s->mcB) return N48_DSV_MC;
    if (s->edidLen != N48_DISP_EDID_BYTES) return N48_DSV_EDID_LEN;
    if (!n48disp_edid_header_ok(s->edid)) return N48_DSV_EDID_HDR;
    if (!n48disp_edid_sum_ok(s->edid) || !n48disp_edid_sum_ok(s->edid + 128)) return N48_DSV_EDID_SUM;
    return N48_DSV_OK;
}

#endif /* NAVI48_DISPLAY_OPS_H */
