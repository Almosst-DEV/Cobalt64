/*
 * test_dcn41_modes.c - host checks on the generated mode table (src/dcn41/dcn41_modes.h).
 *
 * Two kinds of check: internal consistency of every row (totals, the OTG register values optc1_program_timing would
 * write, the VSTARTUP bound, the link requirement), and hand-written expectations for three rows taken from the EDID
 * bytes and from notes/DISPLAY-TRACK.md's own T1 predictions (CEA 1080p60 leaves OTG_H_TOTAL 2199 and OTG_V_TOTAL
 * 1124). The refresh column is checked against the module's own dcn41_nominal_refresh_mhz, so the table and the kext
 * arithmetic cannot drift apart.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dcn41.h"
#include "dcn41_modes.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct dcn41_mode *find(const char *monitor, const char *mode)
{
    size_t i;
    for (i = 0; i < DCN41_MODE_COUNT; i++)
        if (!strcmp(dcn41_modes[i].monitor, monitor) && !strcmp(dcn41_modes[i].mode, mode))
            return &dcn41_modes[i];
    return NULL;
}

int main(void)
{
    size_t i;
    const struct dcn41_mode *m;

    CHECK(DCN41_MODE_COUNT == 9, "9 modes, got %zu", DCN41_MODE_COUNT);
    for (i = 0; i < DCN41_MODE_COUNT; i++) {
        const struct dcn41_mode *e = &dcn41_modes[i];
        uint16_t v_front = e->v_front < 1 ? 1 : e->v_front;    /* apply_front_porch_workaround */
        CHECK(e->h_total == e->h_active + e->h_front + e->h_sync + e->h_back, "%s %s h_total", e->monitor, e->mode);
        CHECK(e->v_total == e->v_active + e->v_front + e->v_sync + e->v_back, "%s %s v_total", e->monitor, e->mode);
        CHECK(e->otg_h_total == e->h_total - 1 && e->otg_v_total == e->v_total - 1, "%s %s totals - 1", e->monitor,
              e->mode);
        CHECK(e->otg_h_sync_a_end == e->h_sync && e->otg_v_sync_a_end == e->v_sync, "%s %s sync ends", e->monitor,
              e->mode);
        CHECK(e->otg_h_blank_start == e->h_total - e->h_front &&
              e->otg_h_blank_end == e->h_total - e->h_front - e->h_active, "%s %s h blank", e->monitor, e->mode);
        CHECK(e->otg_v_blank_start == e->v_total - v_front &&
              e->otg_v_blank_end == e->v_total - v_front - e->v_active, "%s %s v blank", e->monitor, e->mode);
        CHECK(e->otg_h_sync_a_pol == (e->h_pos ? 0 : 1) && e->otg_v_sync_a_pol == (e->v_pos ? 0 : 1),
              "%s %s polarities", e->monitor, e->mode);
        CHECK(e->refresh_mhz == dcn41_nominal_refresh_mhz(e->pix_clk_100hz, e->h_total, e->v_total),
              "%s %s refresh %u vs %" PRIu64, e->monitor, e->mode, e->refresh_mhz,
              dcn41_nominal_refresh_mhz(e->pix_clk_100hz, e->h_total, e->v_total));
        CHECK(e->max_vstartup == (e->v_total - e->v_active - 1 > 1023 ? 1023 : e->v_total - e->v_active - 1),
              "%s %s vstartup bound", e->monitor, e->mode);
        if (e->signal == DCN41_MODE_SIGNAL_DP) {
            CHECK(e->otg_start_point == 1 && e->tmds_pixel_rate_div == 1, "%s %s DP start point / divider",
                  e->monitor, e->mode);
            CHECK(e->req_kbps_8 == e->pix_clk_100hz / 10 * 8 * 3, "%s %s 8 bpc bandwidth", e->monitor, e->mode);
            CHECK(e->link_kbps_8 >= e->req_kbps_8 && e->link_kbps_10 >= e->req_kbps_10, "%s %s link fits",
                  e->monitor, e->mode);
            CHECK(e->dp_lanes_8 == 4 || e->dp_lanes_8 == 2 || e->dp_lanes_8 == 1, "%s %s lanes", e->monitor, e->mode);
        } else {
            CHECK(e->otg_start_point == 0 && e->tmds_pixel_rate_div == 4, "%s %s TMDS start point / divider",
                  e->monitor, e->mode);
            CHECK(e->hdmi_tmds_khz_8 == e->pix_clk_100hz / 10, "%s %s 8 bpc TMDS rate", e->monitor, e->mode);
            CHECK(e->hdmi_tmds_khz_10 == e->pix_clk_100hz / 10 * 10 / 8, "%s %s 10 bpc TMDS rate", e->monitor,
                  e->mode);
            CHECK(e->hdmi_scramble_8 == (e->hdmi_tmds_khz_8 > 340000) &&
                  e->hdmi_scramble_10 == (e->hdmi_tmds_khz_10 > 340000), "%s %s scrambling", e->monitor, e->mode);
            CHECK(e->hdmi_within_dio_8 == (e->hdmi_tmds_khz_8 <= 600000), "%s %s DIO limit", e->monitor, e->mode);
        }
    }

    /* the stage-2 first-light mode: a 1080p panel's own detailed timing, CEA 1080p60 */
    m = find("SINK-B", "1920x1080@60");
    CHECK(m != NULL, "1080p60 present");
    if (m) {
        CHECK(m->pix_clk_100hz == 1485000u && m->refresh_mhz == 60000u, "148.5 MHz, 60.000 Hz");
        CHECK(m->h_total == 2200 && m->v_total == 1125 && m->otg_h_total == 2199 && m->otg_v_total == 1124,
              "DISPLAY-TRACK T1 prediction 2: H_TOTAL 2199, V_TOTAL 1124");
        CHECK(m->otg_h_blank_start == 2112 && m->otg_h_blank_end == 192, "h blank 2112/192");
        CHECK(m->otg_v_blank_start == 1121 && m->otg_v_blank_end == 41, "v blank 1121/41");
        CHECK(m->h_sync == 44 && m->v_sync == 5 && m->h_pos && m->v_pos, "CEA sync widths and polarities");
        CHECK(m->phyid == 2 && m->dig == 2 && m->hpd == 3 && m->ddc == 2, "UNIPHY_C, DIGC, HPD3, DDC2");
        CHECK(m->hdmi_tmds_khz_8 == 148500u && !m->hdmi_scramble_8 && m->hdmi_within_dio_8, "no scrambling needed");
        CHECK(m->max_vstartup == 44, "vblank 45 - 1");
    }
    /* the SINK-A's high-refresh mode, straight from its CTA extension */
    m = find("SINK-A", "2560x1440@165");
    CHECK(m != NULL, "1440p165 present");
    if (m) {
        CHECK(m->source == DCN41_MODE_EDID_CTA_DTD, "from the CTA extension");
        CHECK(m->pix_clk_100hz == 6466400u && m->refresh_mhz == 165000u, "646.64 MHz, 165.000 Hz");
        CHECK(m->h_total == 2666 && m->v_total == 1470, "2666 x 1470");
        CHECK(m->dp_lanes_8 == 4 && m->dp_rate_code_8 == 0x14, "4 lanes HBR2 at 8 bpc");
        CHECK(m->dp_rate_code_10 == 0x1e, "HBR3 at 10 bpc");
        CHECK(m->link_kbps_8 == 17280000u && m->req_kbps_8 == 15519360u, "HBR2 x4 = 17.28 Gbps vs 15.52 needed");
    }
    /* the 4K sink at 4K60 over HDMI: scrambling on, inside the 600 MHz DIO limit at 8 bpc only */
    m = find("SINK-C", "3840x2160@60");
    CHECK(m != NULL, "4K60 present");
    if (m) {
        CHECK(m->pix_clk_100hz == 5940000u && m->h_total == 4400 && m->v_total == 2250, "594 MHz, 4400 x 2250");
        CHECK(m->hdmi_scramble_8 && m->hdmi_within_dio_8, "scrambling at 594 MHz, inside 600 MHz");
        CHECK(m->hdmi_tmds_khz_10 == 742500u && !m->hdmi_within_dio_10, "10 bpc needs 742.5 MHz: beyond DIO, FRL");
    }
    printf("test_dcn41_modes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
