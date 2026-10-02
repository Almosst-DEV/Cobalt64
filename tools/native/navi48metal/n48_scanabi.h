/* n48_scanabi.h: the scanout structs and flags the bundle needs, copied VERBATIM from
 * ~/navi48-native/mesa-mac/include/darwin/navi48_native_abi.h (kext ABI 1.1, selectors 9..14) so the bundle build needs no Mesa include path.
 * tools/native/navi48metal/build.sh does not check this; the S5.2a section of notes/design/NATIVE-S5-FLIP.md records the line-by-line diff check. */
#ifndef N48_SCANABI_H
#define N48_SCANABI_H
#include <stdint.h>
#define N48N_SCAN_MAX_SLOTS   3u
#define N48N_SCAN_NO_SLOT     0xFFFFFFFFu
#define N48N_SCAN_FMT_ARGB8888 8u          /* HUBP SURFACE_PIXEL_FORMAT 8: dword 0xAARRGGBB = VK_FORMAT_B8G8R8A8_UNORM memory order */
/* n48n_scan_query.flags */
#define N48N_SCANQ_LIT        (1u << 0)    /* OTG0 is master-enabled */
#define N48N_SCANQ_NATIVE     (1u << 1)    /* native boot (S1b gate accepted) */
#define N48N_SCANQ_ACQUIRED   (1u << 2)    /* the plane is taken (by this or an earlier call of this client) */
#define N48N_SCANQ_GEOM_OK    (1u << 3)    /* linear ARGB8888, no DCC, plausible viewport/pitch: Acquire would proceed */
#define N48N_SCANQ_DTO_VALID  (1u << 4)    /* refresh derived from the DP DTO; else from the EDID row matching the raster, else 0 */
/* n48n_scan_slot.flags */
#define N48N_SCANSLOT_PENDING  (1u << 0)   /* this slot's Present is programmed and has not latched */
#define N48N_SCANSLOT_INUSE    (1u << 1)   /* HUBP0 EARLIEST_INUSE equals this slot's MC: the hardware is still fetching it */
#define N48N_SCANSLOT_REUSABLE (1u << 2)   /* neither pending nor in use: safe to overwrite */
/* n48n_scan_status.flags */
#define N48N_SCANST_RESTORING  (1u << 0)
#define N48N_SCANST_STORM      (1u << 1)   /* the rate guard tripped this session (the plane is being / was restored) */
#define N48N_SCANST_WANT_RESTORE (1u << 2)

struct n48n_scan_query {           /* 96 B, ScanoutQuery out */
    uint32_t h_active, v_active, h_total, v_total;        /*  0: live OTG timing */
    uint32_t pitch_px, hubp_format, sw_mode, otg;         /* 16: live HUBP; pitch in pixels */
    uint32_t refresh_mhz, pix_clk_khz, dcc_en, flags;     /* 32: millihertz; N48N_SCANQ_* */
    uint64_t frame_count;                                 /* 48: OTG_STATUS_FRAME_COUNT (24 bits), extended to 64 while acquired */
    uint64_t console_mc;                                  /* 56: the console plane address (recorded at Acquire; the live one before) */
    uint64_t plane_mc;                                    /* 64: HUBP0's programmed primary surface address now */
    uint64_t earliest_mc;                                 /* 72: HUBP0's EARLIEST_INUSE address now */
    uint32_t acquired, plane_w, plane_h, reserved;        /* 80: HUBP0's viewport (what ScanoutRegister's width / height must equal); reserved 0 */
};
struct n48n_scan_reg {             /* 32 B, ScanoutRegister in. width/height/format must equal the live plane; reserved0 must be 0 */
    uint32_t handle, reserved0;                           /*  0: BO handle from BoCreate (visible-pool VRAM only) */
    uint64_t offset;                                      /*  8: byte offset in the BO; offset + pitch_bytes*height <= BO size; MC 64 KiB aligned */
    uint32_t pitch_bytes, height;                         /* 16: pitch_bytes == live pitch * 4 */
    uint32_t width, format;                               /* 24: N48N_SCAN_FMT_ARGB8888 */
};
struct n48n_scan_slot {            /* 32 B */
    uint64_t mc;                                          /*  0 */
    uint64_t latched_frame;                               /*  8: extended OTG frame count when this slot's flip was SEEN to latch (0 = never) */
    uint32_t used, flags;                                 /* 16: N48N_SCANSLOT_* */
    uint32_t presents, latches;                           /* 24 */
};
struct n48n_scan_status {          /* 256 B, ScanoutStatus out */
    uint32_t acquired, flags, front_slot, pending_slot;   /*  0: slot ids or N48N_SCAN_NO_SLOT */
    uint64_t frame_count;                                 /* 16: extended OTG frame count now */
    uint64_t console_mc, plane_mc, earliest_mc;           /* 24 */
    uint64_t presents, latched, replaced, repeats;        /* 48: latched = DISTINCT presents seen latched; replaced = overwritten before shown; repeats = VUPDATEs that showed the same front */
    uint64_t vupdates, latch_irq, latch_poll, refused;    /* 80: VUPDATE IRQs while acquired; latches seen by the IRQ / by a poll; refused Presents */
    uint64_t first_latch_ns, last_latch_ns;               /* 112: kernel uptime ns of the first / last latch observation */
    uint64_t idle_ms, watchdog_restores, storm_trips, geom_refused;   /* 128 */
    struct n48n_scan_slot slot[N48N_SCAN_MAX_SLOTS];      /* 160 */
};
_Static_assert(sizeof(struct n48n_scan_query) == 96, "n48n_scan_query is 96 B");
_Static_assert(sizeof(struct n48n_scan_status) == 256, "n48n_scan_status is 256 B");
_Static_assert(sizeof(struct n48n_scan_reg) == 32, "n48n_scan_reg is 32 B");
#endif
