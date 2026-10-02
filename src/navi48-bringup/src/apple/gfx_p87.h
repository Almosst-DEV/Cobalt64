// gfx_p87.h — build 0.0.531 item 4b: SWITCH 87, THE TEXT-ELEMENT INSTRUMENT, and the post-STOP image
// readback. READ-ONLY and decision-inert: nothing here writes a register, a page table, VRAM, a translated dword or anything a rule
// reads. Pure, header-only, host-tested by tests/gfx_p86_test.cpp. `87 | M << 8`: M 1 ON (= 343), M 2 OFF (= 599, the default and
// the boot value), bare `87` reads; M 3 GEOM and M 4 DUMP are the readback (below).
//
// (i) THE T# LOG. The translator's T# observer (xlat12_ib.h tex_note_ix, called beside tex_note with Apple's heap index) hands every
// texture record the table step READ. With 87 ON, records of the eight programs names - P 85, AN 62, AR 66, AT 68, AU 69,
// U 90, Z 95 (the dtable rows' kDTableAbi identities, ndw << 32 | fnv) and UberCompositeFragment (identity 9) - are stashed for
// THIS judged frame (N48_P87_REC records, a retry's re-read skipped), and printed after the frame's gate answered, ONLY if it
// answered COMMIT: one line per record, the first N48_P87_LINES per arm, then counted. A frame's family (a, a2, b, e: host-side
// names, tools/m4-xlat/cb0walk.py) is assigned from the printed frame number and first colour target.
//
// (ii) THE READBACK: `gfxneuter 87 | 3 << 8 | ...` (GEOM) records the NEXT dump's header fields (width, height, bytes per pixel,
// the swizzle mode, the operator's tag); `gfxneuter 87 | 4 << 8 | ...` (DUMP) reads a VRAM range [off, off + len) through the
// MM window (navi48_vram_read_mm, the vmpage reads' path) and prints it as hex lines in the driver log - the mechanism the
// scanout thumbnails (`scanout-thumb:` lines) and vmpage (`vmpage: pg[...]` lines) already use; tools/conductor/imgdump531.py
// turns the lines of a scp'd log into raw image files with a small header. Refused while a commit arm stands, for a range
// outside the VRAM aperture, and above N48_P87_DUMP_MAX bytes per call. Nothing outside [off, off + len) is ever read.
#ifndef N48_GFX_P87_H
#define N48_GFX_P87_H

#include <stdint.h>

#define N48_P87_IDS    8u
#define N48_P87_REC    48u        /* records stashed per judged frame (more are counted, not kept) */
#define N48_P87_LINES  64u        /* T# lines per arm, then counted */
/* build 0.0.540 item 6(b) (the late phase at judge frame ~506-554): once the 64 lines above are spent, committed
 * frames from judge frame N48_P87_LATE_FROM on get N48_P87_LATE_LINES more per arm (logging only; still switch 87 ON only). */
#define N48_P87_LATE_FROM  480ull
#define N48_P87_LATE_LINES 64u
#define N48_P87_DUMP_MAX 0x10000u /* bytes per DUMP call (64 KiB = 512 lines of 128 bytes) */
#define N48_P87_DUMP_LINE 128u    /* bytes per hex line */

enum { N48_P87_OFF = 0u, N48_P87_ON = 1u };

/* The eight identities: {ndw, fnv} from src/xlat12/xlat12_dtable_rows.inc, the name, the identity number of xlat12_shader_ids.h. */
static const struct { uint32_t ndw, fnv; const char *name; uint32_t ident; } kN48P87Ids[N48_P87_IDS] = {
    {  46u, 0xebaa377cu, "P",   85u },   /* ws_P_TimgXh_Ialp */
    { 122u, 0x7b3a6dfeu, "AN",  62u },   /* ws_AN_TmuaXh_Isrc_Isrc */
    { 138u, 0xa5000f40u, "AR",  66u },   /* ws_AR_TcimBltnXh_Icir */
    { 191u, 0xafe6b4ecu, "AT",  68u },   /* ws_AT_TbdsXh_Isup_Isrc */
    {  86u, 0x0e94d9e2u, "AU",  69u },   /* ws_AU_TcimXh_Isrc */
    { 192u, 0x92c6ae13u, "U",   90u },   /* ws_U_TvcmXh_Isrc */
    {  54u, 0xf91e4deeu, "Z",   95u },   /* ws_Z_TimgXh_Isrc */
    {  50u, 0x4912346bu, "UCF",  9u },   /* UberCompositeFragment */
};
/* The index of a program identity (ndw << 32 | fnv) in kN48P87Ids, or N48_P87_IDS. */
static inline uint32_t n48_p87_id(uint64_t ps_id)
{
    for (uint32_t k = 0; k < N48_P87_IDS; k++)
        if ((((uint64_t)kN48P87Ids[k].ndw << 32) | kN48P87Ids[k].fnv) == ps_id) return k;
    return N48_P87_IDS;
}

/* The gfx10 T# fields the line prints (the positions xlat12_desc.h's xlat12_img_desc_g10_to_g12 reads them from). */
typedef struct { uint64_t base; uint32_t w, h, fmt, sw, comp, wcomp, meta; } n48_p87_tsharp;
static inline n48_p87_tsharp n48_p87_decode(const uint32_t rec[8])
{
    n48_p87_tsharp t;
    t.base = ((uint64_t)(rec[1] & 0xFFu) << 40) | ((uint64_t)rec[0] << 8);
    t.w = ((((rec[1] >> 30) & 0x3u) | ((rec[2] & 0xFFFu) << 2))) + 1u;   /* WIDTH_LO [31:30] + WIDTH_HI [11:0]: width - 1 */
    t.h = ((rec[2] >> 14) & 0x3FFFu) + 1u;                                 /* HEIGHT [27:14]: height - 1 */
    t.fmt = (rec[1] >> 20) & 0x1FFu;                                        /* gfx10 FORMAT [28:20] */
    t.sw = (rec[3] >> 20) & 0x1Fu;                                          /* gfx10 SW_MODE [24:20] */
    t.comp = (rec[6] >> 21) & 1u;                                           /* COMPRESSION_EN */
    t.wcomp = (rec[6] >> 20) & 1u;                                          /* WRITE_COMPRESS_ENABLE */
    t.meta = (rec[6] >> 24) & 0xFFu;                                        /* META_DATA_ADDRESS_LO */
    return t;
}

/* build 0.0.535 item 4: `st_*` = the DRAW's OWN colour target 0 and window scissor as the translation's own
 * writes left them at this record's T# read (xlat12_tex_state: st_seen bit 0 CB_COLOR0_BASE, 1 _BASE_EXT, 2 WINDOW_SCISSOR_TL,
 * 3 _BR; a clear bit = inherited, unknown). Before this build the line printed only the frame's FIRST gathered target. */
typedef struct { uint32_t id, at, tex, heap; uint32_t rec[8]; uint32_t st_seen, st_cb0, st_cb0_ext, st_win_tl, st_win_br; } n48_p87_rec;
typedef struct {
    uint32_t on;                         /* N48_P87_*; the verb is its only writer */
    uint64_t frame;                      /* the judged frame the stash belongs to */
    uint32_t n;                          /* records kept this frame */
    n48_p87_rec r[N48_P87_REC];
    uint32_t lines;                      /* T# lines printed this arm */
    uint64_t seen, kept, overFrame, dupes, printed, suppressed, uncommitted, frames;
    uint64_t byId[N48_P87_IDS];
    /* the readback's next header (GEOM) and its counters */
    uint32_t gw, gh, gbpp, gsw, gtag, dumps, dumpRefused;
    uint32_t lateLines;                  /* build 0.0.540 item 6(b): late-window lines printed this arm */
    uint64_t latePrinted;
} n48_p87;

/* A new frame empties the stash. Returns 1 when the record was kept (0: not one of the eight, a retry's re-read, or no room). */
static inline uint32_t n48_p87_note(n48_p87 *s, uint64_t frame, uint64_t ps_id, uint32_t at_i, uint32_t heap, const uint32_t rec[8])
{
    if (!s || !rec || !s->on) return 0u;
    const uint32_t id = n48_p87_id(ps_id);
    if (id >= N48_P87_IDS) return 0u;
    s->seen++;
    if (s->frame != frame) { s->frame = frame; s->n = 0u; }
    const uint32_t at = at_i & 0xFFFFFFu, tex = at_i >> 24;
    for (uint32_t k = 0; k < s->n; k++)
        if (s->r[k].id == id && s->r[k].at == at && s->r[k].tex == tex) { s->dupes++; return 0u; }
    if (s->n >= N48_P87_REC) { s->overFrame++; return 0u; }
    n48_p87_rec *e = &s->r[s->n++];
    e->id = id; e->at = at; e->tex = tex; e->heap = heap;
    for (uint32_t k = 0; k < 8u; k++) e->rec[k] = rec[k];
    e->st_seen = 0u; e->st_cb0 = e->st_cb0_ext = e->st_win_tl = e->st_win_br = 0u;
    s->kept++; s->byId[id]++;
    return 1u;
}
/* build 0.0.535 item 4: the draw's own state for the record n48_p87_note JUST kept (call only when it answered 1). */
static inline void n48_p87_note_state(n48_p87 *s, uint32_t seen, uint32_t cb0, uint32_t cb0_ext, uint32_t win_tl, uint32_t win_br)
{
    if (!s || !s->n) return;
    n48_p87_rec *e = &s->r[s->n - 1u];
    e->st_seen = seen & 0xFu; e->st_cb0 = cb0; e->st_cb0_ext = cb0_ext; e->st_win_tl = win_tl; e->st_win_br = win_br;
}
/* The draw's own CB0 VA (BASE << 8 | BASE_EXT[7:0] << 40) and the window scissor's corners (gfx10 TL/BR: X [14:0], Y [30:16]). */
static inline uint64_t n48_p87_cb0_va(const n48_p87_rec *e) { return ((uint64_t)(e->st_cb0_ext & 0xFFu) << 40) | ((uint64_t)e->st_cb0 << 8); }
static inline uint32_t n48_p87_sc_x(uint32_t v) { return v & 0x7FFFu; }
static inline uint32_t n48_p87_sc_y(uint32_t v) { return (v >> 16) & 0x7FFFu; }
/* The frame's gate answered: how many of its kept records may be printed now (0 for an uncommitted frame or another frame's
 * stash); the rest are counted suppressed. The caller prints r[0 .. return) and the stash is emptied. */
static inline uint32_t n48_p87_frame_end(n48_p87 *s, uint64_t frame, uint32_t committed)
{
    if (!s || !s->on || s->frame != frame || !s->n) return 0u;
    const uint32_t n = s->n;
    s->n = 0u;
    s->frames++;
    if (!committed) { s->uncommitted += n; return 0u; }
    const uint32_t room = s->lines < N48_P87_LINES ? N48_P87_LINES - s->lines : 0u;
    const uint32_t out = n < room ? n : room;
    s->lines += out; s->printed += out;
    /* build 0.0.540 item 6(b): the late window, for what the early budget left, from judge frame N48_P87_LATE_FROM on */
    const uint32_t lroom = (frame >= N48_P87_LATE_FROM && s->lateLines < N48_P87_LATE_LINES) ? N48_P87_LATE_LINES - s->lateLines : 0u;
    const uint32_t lout = n - out < lroom ? n - out : lroom;
    s->lateLines += lout; s->latePrinted += lout; s->printed += lout;
    s->suppressed += n - out - lout;
    return out + lout;
}
/* A new arm (the continuous arm's START) or the switch turned ON: the per-arm line budget starts again. */
static inline void n48_p87_arm_reset(n48_p87 *s) { if (s) { s->lines = 0u; s->n = 0u; s->lateLines = 0u; } }

/* args: frame, THE DRAW's OWN CB0 VA, its note ("" or " inh": not written in the translation), the window scissor x0, y0, x1, y1
 * and its note, the frame's FIRST gathered CB0 (what 0.0.531-0.0.534 printed as "cb0"), name, identity, draw dword, texture
 * index, heap index, base, W, H, fmt, sw, comp, wcomp, meta, the raw 8 dwords (build 0.0.535 item 4) */
#define N48_P87_FMT "tex531: F%llu cb0 %#llx%s win %u,%u-%u,%u%s (frame first %#llx) %s(%u) draw dw %u tex %u heap %u base %#llx " \
    "%ux%u fmt %u sw %u dcc comp %u wcomp %u meta %#x T# %08x %08x %08x %08x %08x %08x %08x %08x"
#define N48_P87_REPORT_FMT "tex531: switch 87 %s%s. T# seen %llu kept %llu (dupe %llu, over %llu); frames %llu: printed %llu " \
    "suppressed %llu (arm lines %u/%u); uncommitted %llu; dumps %u refused %u"
/* build 0.0.540 item 6(b). args: the late window's first frame, lines this arm, the cap, printed since boot. */
#define N48_P87_REPORT3_FMT "tex531: late window (judge frames >= %llu): lines this arm %u/%u, printed %llu"
#define N48_P87_REPORT2_FMT "tex531: kept by identity: P %llu AN %llu AR %llu AT %llu AU %llu U %llu Z %llu UCF %llu"

/* =============================================================================================================================
 * THE READBACK. GEOM `87 | 3 << 8 | W << 16 | H << 30 | log2(bytes per pixel) << 44 | swizzle << 48 | tag << 56`
 *   (W, H 14 bits each, <= 16383; bpp 1, 2, 4, 8 or 16; swizzle 5 bits, the operator's copy of the T#'s SW_MODE; tag 8 bits).
 * DUMP `87 | 4 << 8 | (off >> 8) << 16 | (len >> 8) << 48`: off 256-byte aligned below 2^40, len in 256-byte units (<= 64 KiB).
 * ============================================================================================================================= */
enum { N48_P87_M_READ = 0u, N48_P87_M_ON = 1u, N48_P87_M_OFF = 2u, N48_P87_M_GEOM = 3u, N48_P87_M_DUMP = 4u };
enum { N48_P87_D_OK = 0u, N48_P87_D_ARMED, N48_P87_D_ZERO, N48_P87_D_BIG, N48_P87_D_APERTURE, N48_P87_D_NOVRAM, N48_P87_DS };
static inline uint64_t n48_p87_geom_arg(uint32_t w, uint32_t h, uint32_t bpp_log2, uint32_t sw, uint32_t tag)
{
    return 87ull | (3ull << 8) | ((uint64_t)(w & 0x3FFFu) << 16) | ((uint64_t)(h & 0x3FFFu) << 30) |
           ((uint64_t)(bpp_log2 & 0xFu) << 44) | ((uint64_t)(sw & 0x1Fu) << 48) | ((uint64_t)(tag & 0xFFu) << 56);
}
static inline void n48_p87_geom_set(n48_p87 *s, uint64_t arg)
{
    s->gw = (uint32_t)((arg >> 16) & 0x3FFFu); s->gh = (uint32_t)((arg >> 30) & 0x3FFFu);
    const uint32_t l2 = (uint32_t)((arg >> 44) & 0xFu);
    s->gbpp = l2 <= 4u ? (1u << l2) : 0u;
    s->gsw = (uint32_t)((arg >> 48) & 0x1Fu); s->gtag = (uint32_t)((arg >> 56) & 0xFFu);
}
static inline uint64_t n48_p87_dump_arg(uint64_t off, uint32_t len)
{
    return 87ull | (4ull << 8) | (((off >> 8) & 0xFFFFFFFFull) << 16) | ((uint64_t)((len >> 8) & 0xFFFFu) << 48);
}
/* The DUMP's range and its refusals. `vram_size` 0 = the aperture is unknown (refused). The range is read only when this answers OK,
 * and then exactly [*off, *off + *len). */
static inline uint32_t n48_p87_dump_plan(uint64_t arg, uint32_t armed, uint64_t vram_size, uint64_t *off, uint32_t *len)
{
    const uint64_t o = ((arg >> 16) & 0xFFFFFFFFull) << 8;
    const uint64_t l = ((arg >> 48) & 0xFFFFull) << 8;
    if (off) *off = o;
    if (len) *len = (uint32_t)(l <= 0xFFFFFFFFull ? l : 0xFFFFFFFFull);
    if (armed) return N48_P87_D_ARMED;
    if (!l) return N48_P87_D_ZERO;
    if (l > N48_P87_DUMP_MAX) return N48_P87_D_BIG;
    if (!vram_size) return N48_P87_D_NOVRAM;
    if (o >= vram_size || l > vram_size - o) return N48_P87_D_APERTURE;
    return N48_P87_D_OK;
}
static inline const char *n48_p87_dump_name(uint32_t d)
{
    return d == N48_P87_D_OK ? "OK" : d == N48_P87_D_ARMED ? "REFUSED - a commit arm stands (post-STOP only)"
         : d == N48_P87_D_ZERO ? "REFUSED - zero length" : d == N48_P87_D_BIG ? "REFUSED - above 64 KiB per call"
         : d == N48_P87_D_APERTURE ? "REFUSED - outside the VRAM aperture" : "REFUSED - the VRAM aperture is unknown";
}
/* 32-bit FNV-1a over the bytes, for the END line (the decoder checks it). */
static inline uint32_t n48_p87_fnv(uint32_t h, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
/* args: dump number, tag, VRAM offset, bytes, W, H, bpp, swizzle, lines to follow */
#define N48_P87_HDR_FMT "imgdump531: HDR dump %u tag %u vram %#llx bytes %u W %u H %u bpp %u swizzle %u lines %u"
/* args: dump number, byte offset in the dump, then up to 256 hex characters */
#define N48_P87_HEX_FMT "imgdump531: D %u %05x %s"
/* args: dump number, bytes printed, read failures, FNV-1a */
#define N48_P87_END_FMT "imgdump531: END dump %u bytes %u read-fail %u fnv %08x"
#define N48_P87_REF_FMT "imgdump531: DUMP vram %#llx bytes %u %s"
#define N48_P87_GEOM_FMT "imgdump531: GEOM for the next dump: W %u H %u bpp %u swizzle %u tag %u"

#endif /* N48_GFX_P87_H */
