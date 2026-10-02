#!/usr/bin/env python3
"""gen_modes.py - precompute the display modes of three attached sinks into src/dcn41/dcn41_modes.{h,tsv}.

  python3 tools/dcn41/gen_modes.py            write the header and the TSV
  python3 tools/dcn41/gen_modes.py --check    exit 1 if either checked-in file differs from a fresh generation

Inputs, all local, nothing downloaded:
  * the three EDIDs this card read on an earlier run, from edid-capture.txt
    (keys "EDID,AUX1", "EDID,DDC2", "EDID,DDC3" 128 bytes each and "IODisplayEDID" 256 bytes: the SINK-A with its
    CTA-861 extension). Detailed timings are decoded per VESA E-EDID 1.4 section 3.10.2.
  * VESA CVT 1.2 reduced blanking v1 and CVT 2.0 reduced blanking v2, implemented here, for the high-refresh modes no
    captured EDID carries. The v1 implementation is checked against the SINK-A's own 59.95 Hz detailed timing, which is
    a CVT-RB v1 timing: if it does not reproduce 2720 x 1481 at 241.50 MHz exactly, the generator refuses.
  * Linux 238650ef6c7c for everything that turns a timing into hardware numbers, each cited in the output:
    optc1_program_timing (the OTG register values), dcn401_calculate_dccg_tmds_div_value (the pixel-rate divider),
    dc_bandwidth_in_kbps_from_timing and dp_link_bandwidth_kbps (the DP link requirement), link_ddc.c write_scdc_data
    (scrambling above 340 MHz), dcn401_resource.c max_hdmi_pixel_clock (the 600 MHz DIO limit), frl_link_bandwidth_kbps
    (the FRL rates), CalculateMaxVStartup (the VSTARTUP bound).
No floating point reaches the header: every emitted number is an integer computed here.
"""
import pathlib, re, sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

EDID_LOG = L.ROOT / "edid-capture.txt"
OUT_H = L.SRC / "dcn41_modes.h"
OUT_TSV = L.SRC / "dcn41_modes.tsv"

# Connector layout of this card: "AtomBIOS,Connectors" =
# "DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,HDMI-A/ddc2/hpd3,HDMI-A/ddc3/hpd4" (framebuffer-pre.txt), and the
# board ROM's display object table (notes/DISPLAY-DESIGN.md 3.5) gives the PHY and DIG per path.
SINKS = {
    "AUX1": dict(name="SINK-A", link=1, signal="DP", phy="UNIPHY_B", phyid=1, dig="DIGB", hpd=2, ddc=1),
    "DDC2": dict(name="SINK-B", link=2, signal="HDMI", phy="UNIPHY_C", phyid=2, dig="DIGC", hpd=3, ddc=2),
    "DDC3": dict(name="SINK-C", link=3, signal="HDMI", phy="UNIPHY_D", phyid=3, dig="DIGD", hpd=4, ddc=3),
}
# DP link rate codes (dc_dp_types.h): code * 27 MHz * 10 bits = kbps per lane
DP_RATES = [("RBR", 0x06), ("HBR", 0x0A), ("HBR2", 0x14), ("HBR3", 0x1E)]
# frl_link_bandwidth_kbps (link_validation.c)
FRL_RATES = [("3G_3LANE", 9000000), ("6G_3LANE", 18000000), ("6G_4LANE", 24000000), ("8G_4LANE", 32000000),
             ("10G_4LANE", 40000000), ("12G_4LANE", 48000000)]
HDMI_TMDS_MAX_KHZ = 600000      # dcn401_resource.c: .max_hdmi_pixel_clock = 600000
HDMI_SCRAMBLE_KHZ = 340000      # link_ddc.c: "bool over_340_mhz = pix_clk > 340000 ? 1 : 0;"


def edids():
    text = EDID_LOG.read_text(errors="replace")
    out = {}
    for m in re.finditer(r'"(EDID,[A-Z0-9]+|IODisplayEDID)" = <([0-9a-f]+)>', text):
        out.setdefault(m.group(1), bytes.fromhex(m.group(2)))
    return out


def decode_dtd(b, o):
    pc_khz = (b[o] | (b[o + 1] << 8)) * 10
    if pc_khz == 0:
        return None
    ha = b[o + 2] | ((b[o + 4] >> 4) << 8)
    hb = b[o + 3] | ((b[o + 4] & 0xF) << 8)
    va = b[o + 5] | ((b[o + 7] >> 4) << 8)
    vb = b[o + 6] | ((b[o + 7] & 0xF) << 8)
    hso = b[o + 8] | (((b[o + 11] >> 6) & 3) << 8)
    hsw = b[o + 9] | (((b[o + 11] >> 4) & 3) << 8)
    vso = (b[o + 10] >> 4) | (((b[o + 11] >> 2) & 3) << 4)
    vsw = (b[o + 10] & 0xF) | ((b[o + 11] & 3) << 4)
    flags = b[o + 17]
    return dict(pix_clk_khz=pc_khz, h_active=ha, h_front=hso, h_sync=hsw, h_back=hb - hso - hsw, h_total=ha + hb,
                v_active=va, v_front=vso, v_sync=vsw, v_back=vb - vso - vsw, v_total=va + vb,
                interlaced=bool(flags & 0x80), h_pos=bool(flags & 2), v_pos=bool(flags & 4))


def cvt_rb(h_active, v_active, refresh, version):
    """VESA CVT reduced blanking. v1: CVT 1.2 (RB_H_BLANK 160, V sync from the aspect ratio); v2: CVT 2.0 (RB_H_BLANK
    80, V front porch 1, V sync 8). Integer arithmetic in picoseconds/hertz throughout."""
    if version == 1:
        rb_h_blank, h_sync, h_back_extra, v_fp = 160, 32, 0, 3
        v_sync = {(16, 9): 5, (16, 10): 6, (4, 3): 4, (5, 4): 7}.get(aspect(h_active, v_active), 10)
        min_v_bp, clock_step_khz = 6, 250                 # CVT 1.2 reduced blanking: 0.25 MHz clock step
    else:
        rb_h_blank, h_sync, h_back_extra, v_fp = 80, 32, 0, 1
        v_sync, min_v_bp, clock_step_khz = 8, 6, 1        # CVT 2.0 reduced blanking: 1 kHz clock step
    rb_min_v_blank_ns = 460000                        # 460 us
    h_total = h_active + rb_h_blank
    # h period estimate (ps): (1/refresh - min vblank) / active lines
    frame_ps = 10 ** 12 // refresh
    h_period_ps = (frame_ps - rb_min_v_blank_ns * 1000) // v_active
    vbi = rb_min_v_blank_ns * 1000 // h_period_ps + 1
    vbi = max(vbi, v_fp + v_sync + min_v_bp)
    v_total = v_active + vbi
    clk_khz = (refresh * v_total * h_total + 999) // 1000
    clk_khz = (clk_khz // clock_step_khz) * clock_step_khz
    return dict(pix_clk_khz=clk_khz, h_active=h_active, h_front=rb_h_blank - h_sync - (h_back_extra + 80 if version == 1 else 40),
                h_sync=h_sync, h_back=(h_back_extra + 80 if version == 1 else 40), h_total=h_total,
                v_active=v_active, v_front=v_fp, v_sync=v_sync, v_back=vbi - v_fp - v_sync, v_total=v_total,
                interlaced=False, h_pos=True, v_pos=False)


def aspect(w, h):
    from math import gcd
    g = gcd(w, h)
    return (w // g, h // g)


def otg_regs(t, signal):
    """optc1_program_timing (dcn10_optc.c:156), with apply_front_porch_workaround (v_front_porch >= 1)."""
    v_front = max(t["v_front"], 1)
    h_blank_start = t["h_total"] - t["h_front"]
    h_blank_end = h_blank_start - t["h_active"]
    v_blank_start = t["v_total"] - v_front
    v_blank_end = v_blank_start - t["v_active"]
    return dict(h_total=t["h_total"] - 1, h_sync_a_start=0, h_sync_a_end=t["h_sync"], h_blank_start=h_blank_start,
                h_blank_end=h_blank_end, h_sync_a_pol=0 if t["h_pos"] else 1, v_total=t["v_total"] - 1,
                v_sync_a_start=0, v_sync_a_end=t["v_sync"], v_blank_start=v_blank_start, v_blank_end=v_blank_end,
                v_sync_a_pol=0 if t["v_pos"] else 1, start_point=1 if signal == "DP" else 0,
                tmds_pixel_rate_div=1 if signal == "DP" else 4)


def refresh_mhz(t):
    return (t["pix_clk_khz"] * 1000 * 1000 + (t["h_total"] * t["v_total"]) // 2) // (t["h_total"] * t["v_total"])


def dp_requirement(t, bpc):
    """dc_bandwidth_in_kbps_from_timing (RGB) against dp_link_bandwidth_kbps (8b/10b, no FEC), searched in Linux's own
    order (decide_dp_link_settings, link_dp_capability.c:764-790): raise the lane count first, then the rate."""
    req_kbps = t["pix_clk_khz"] * bpc * 3
    for name, code in DP_RATES:
        for lanes in (1, 2, 4):
            bw = code * 27000 * 10 * lanes // 10000 * 8000
            if bw >= req_kbps:
                return req_kbps, lanes, name, code, bw
    return req_kbps, 0, "none", 0, 0


def hdmi_requirement(t, bpc):
    """TMDS character rate = pixel clock x bpc/8 for RGB deep colour (dce112_get_pix_clk_dividers_helper scales the
    pixel clock the same way); scrambling above 340 MHz; FRL above the 600 MHz DIO limit."""
    tmds_khz = t["pix_clk_khz"] * bpc // 8
    frl = None
    if tmds_khz > HDMI_TMDS_MAX_KHZ:
        need_kbps = t["pix_clk_khz"] * bpc * 3
        for name, cap in FRL_RATES:
            if cap * 16 // 18 >= need_kbps:            # 16b/18b FRL channel coding
                frl = (name, cap, need_kbps)
                break
        if frl is None:
            frl = ("above 12G_4LANE: needs DSC", 0, need_kbps)
    return tmds_khz, tmds_khz > HDMI_SCRAMBLE_KHZ, tmds_khz <= HDMI_TMDS_MAX_KHZ, frl


def max_vstartup(t):
    """CalculateMaxVStartup (dml2_core_dcn4_calcs.c:3727) with vblank_nom = v_total - v_active
    (dml21_translation_helper.c:202) and no writeback: vblank_size - max(1, ceil(0)) = vblank - 1, capped at 1023."""
    return min(t["v_total"] - t["v_active"] - 1, 1023)


def build():
    e = edids()
    for k in ("EDID,AUX1", "EDID,DDC2", "EDID,DDC3", "IODisplayEDID"):
        if k not in e:
            raise SystemExit(f"gen_modes: {k} not found in {EDID_LOG}: refusing")
    if e["IODisplayEDID"][:128] != e["EDID,AUX1"]:
        raise SystemExit("gen_modes: IODisplayEDID's base block is not EDID,AUX1: refusing")
    modes = []

    def add(sink, label, t, source, note=""):
        s = SINKS[sink]
        modes.append(dict(sink=sink, label=label, source=source, note=note, t=t, **s))

    # --- SINK-A (DP): base detailed timing and the three CTA extension timings (CONFIRMED EDID bytes) ---
    b = e["IODisplayEDID"]
    add("AUX1", "2560x1440@60", decode_dtd(b, 54), "EDID_DTD")
    ext = b[128:256]
    o = ext[2]
    labels = ["2560x1440@165", "2560x1440@144", "2560x1440@120", "1280x1440@60"]
    i = 0
    while o + 18 <= 127 and (ext[o] or ext[o + 1]) and i < len(labels):
        add("AUX1", labels[i], decode_dtd(ext, o), "EDID_CTA_DTD")
        o += 18
        i += 1
    # --- SINK-B (HDMI): base detailed timing; its CTA extension was not captured ---
    add("DDC2", "1920x1080@60", decode_dtd(e["EDID,DDC2"], 54), "EDID_DTD")
    add("DDC2", "1920x1080@120", cvt_rb(1920, 1080, 120, 2), "CVT_RB2",
        "the panel's range descriptor allows 48-120 Hz up to 340 MHz; its own 120 Hz timing lives in the CTA extension, not captured")
    # --- SINK-C (HDMI): base detailed timing; 120 Hz computed ---
    add("DDC3", "3840x2160@60", decode_dtd(e["EDID,DDC3"], 54), "EDID_DTD")
    add("DDC3", "3840x2160@120", cvt_rb(3840, 2160, 120, 2), "CVT_RB2",
        "range descriptor 48-120 Hz up to 1190 MHz; the sink's own 120 Hz timing (CTA, probably the 1188 MHz CEA one) was not captured")

    # positive control: the SINK-A's 60 Hz detailed timing is a CVT-RB v1 timing
    ctl = cvt_rb(2560, 1440, 60, 1)
    got = (ctl["h_total"], ctl["v_total"], ctl["pix_clk_khz"])
    if got != (2720, 1481, 241500):
        raise SystemExit(f"gen_modes: CVT-RB v1 control failed: {got} != (2720, 1481, 241500): refusing")
    return modes, got


def rows(modes):
    out = []
    for m in modes:
        t = m["t"]
        r = otg_regs(t, m["signal"])
        row = dict(monitor=m["name"], sink=m["sink"], link=m["link"], signal=m["signal"], phy=m["phy"],
                   phyid=m["phyid"], dig=m["dig"], hpd=m["hpd"], ddc=m["ddc"], mode=m["label"], source=m["source"],
                   pix_clk_100hz=t["pix_clk_khz"] * 10, refresh_mhz=refresh_mhz(t), h_active=t["h_active"],
                   h_front=t["h_front"], h_sync=t["h_sync"], h_back=t["h_back"], h_total=t["h_total"],
                   v_active=t["v_active"], v_front=t["v_front"], v_sync=t["v_sync"], v_back=t["v_back"],
                   v_total=t["v_total"], h_pol="+" if t["h_pos"] else "-", v_pol="+" if t["v_pos"] else "-",
                   max_vstartup=max_vstartup(t), note=m["note"], **{"otg_" + k: v for k, v in r.items()})
        if m["signal"] == "DP":
            for bpc in (8, 10):
                req, lanes, name, code, bw = dp_requirement(t, bpc)
                row[f"dp{bpc}_req_kbps"], row[f"dp{bpc}_lanes"], row[f"dp{bpc}_rate"] = req, lanes, name
                row[f"dp{bpc}_rate_code"], row[f"dp{bpc}_bw_kbps"] = code, bw
        else:
            for bpc in (8, 10):
                tmds, scram, within, frl = hdmi_requirement(t, bpc)
                row[f"hdmi{bpc}_tmds_khz"], row[f"hdmi{bpc}_scramble"] = tmds, int(scram)
                row[f"hdmi{bpc}_within_dio"] = int(within)
                row[f"hdmi{bpc}_frl"] = frl[0] if frl else ""
        out.append(row)
    return out


def tsv(rows_):
    cols = []
    for r in rows_:
        for k in r:
            if k not in cols:
                cols.append(k)
    lines = ["# GENERATED by tools/dcn41/gen_modes.py; see its header for the inputs and the Linux citations.",
             "\t".join(cols)]
    for r in rows_:
        lines.append("\t".join(str(r.get(c, "")) for c in cols))
    return "\n".join(lines) + "\n"


def header(rows_, ctl):
    out = [
        "/* dcn41_modes.h - GENERATED by tools/dcn41/gen_modes.py; do not edit (regenerate, then --check).",
        " *",
        " * Precomputed timings for three attached sinks, with the OTG register values",
        " * optc1_program_timing would write, the DCCG pixel-rate divider, the link requirement, and the VSTARTUP bound.",
        " * Sources per mode: EDID_DTD and EDID_CTA_DTD are the sink's own bytes (notes/logs/rdna4fb/"
        "edid-capture.txt);",
        " * CVT_RB2 is computed here because that sink's own high-refresh timing was never captured - SUSPECTED, replace",
        f" * it with the sink's CTA timing when the PC reads it. CVT-RB v1 control: 2560x1440@60 -> {ctl[0]}x{ctl[1]} "
        f"at {ctl[2]} kHz, equal to the SINK-A's own detailed timing.",
        " */",
        "#ifndef N48_DCN41_MODES_H",
        "#define N48_DCN41_MODES_H",
        "",
        "#include <stdint.h>",
        "",
        "enum dcn41_mode_source { DCN41_MODE_EDID_DTD = 0, DCN41_MODE_EDID_CTA_DTD, DCN41_MODE_CVT_RB2 };",
        "enum dcn41_mode_signal { DCN41_MODE_SIGNAL_DP = 0, DCN41_MODE_SIGNAL_HDMI };",
        "",
        "struct dcn41_mode {",
        "    const char *monitor;          /* EDID descriptor 0xFC */",
        "    const char *mode;             /* label */",
        "    uint8_t source;               /* enum dcn41_mode_source */",
        "    uint8_t signal;               /* enum dcn41_mode_signal */",
        "    uint8_t link, phyid, dig, hpd, ddc;   /* link index, ATOM phy id, DIG instance, HPD pin (1-based), DDC line */",
        "    uint32_t pix_clk_100hz;       /* SET_PIXEL_CLOCK's unit */",
        "    uint32_t refresh_mhz;         /* pixel clock / (h_total * v_total), millihertz */",
        "    uint16_t h_active, h_front, h_sync, h_back, h_total;",
        "    uint16_t v_active, v_front, v_sync, v_back, v_total;",
        "    uint8_t h_pos, v_pos;         /* sync polarities, 1 = positive */",
        "    /* optc1_program_timing's register values */",
        "    uint16_t otg_h_total, otg_h_sync_a_end, otg_h_blank_start, otg_h_blank_end;",
        "    uint16_t otg_v_total, otg_v_sync_a_end, otg_v_blank_start, otg_v_blank_end;",
        "    uint8_t otg_h_sync_a_pol, otg_v_sync_a_pol, otg_start_point, tmds_pixel_rate_div;",
        "    uint16_t max_vstartup;        /* CalculateMaxVStartup bound; DML21 picks the actual VSTARTUP */",
        "    /* link requirement at 8 and 10 bits per channel */",
        "    uint32_t req_kbps_8, req_kbps_10;         /* dc_bandwidth_in_kbps_from_timing, RGB */",
        "    uint32_t link_kbps_8, link_kbps_10;       /* DP: the chosen link's bandwidth; HDMI: 0 */",
        "    uint8_t dp_lanes_8, dp_rate_code_8, dp_lanes_10, dp_rate_code_10;   /* DP only */",
        "    uint32_t hdmi_tmds_khz_8, hdmi_tmds_khz_10;                          /* HDMI only */",
        "    uint8_t hdmi_scramble_8, hdmi_scramble_10, hdmi_within_dio_8, hdmi_within_dio_10;",
        "};",
        "",
        "static const struct dcn41_mode dcn41_modes[] = {",
    ]
    for r in rows_:
        src = {"EDID_DTD": "DCN41_MODE_EDID_DTD", "EDID_CTA_DTD": "DCN41_MODE_EDID_CTA_DTD",
               "CVT_RB2": "DCN41_MODE_CVT_RB2"}[r["source"]]
        sig = "DCN41_MODE_SIGNAL_DP" if r["signal"] == "DP" else "DCN41_MODE_SIGNAL_HDMI"
        dig = "ABCDEFG".index(r["dig"][-1])
        dp = r["signal"] == "DP"
        out.append(f'    {{ /* {r["monitor"]} {r["mode"]} ({r["source"]}) */')
        out.append(f'        "{r["monitor"]}", "{r["mode"]}", {src}, {sig},')
        out.append(f'        {r["link"]}, {r["phyid"]}, {dig}, {r["hpd"]}, {r["ddc"]},')
        out.append(f'        {r["pix_clk_100hz"]}u, {r["refresh_mhz"]}u,')
        out.append(f'        {r["h_active"]}, {r["h_front"]}, {r["h_sync"]}, {r["h_back"]}, {r["h_total"]},')
        out.append(f'        {r["v_active"]}, {r["v_front"]}, {r["v_sync"]}, {r["v_back"]}, {r["v_total"]},')
        out.append(f'        {1 if r["h_pol"] == "+" else 0}, {1 if r["v_pol"] == "+" else 0},')
        out.append(f'        {r["otg_h_total"]}, {r["otg_h_sync_a_end"]}, {r["otg_h_blank_start"]}, '
                   f'{r["otg_h_blank_end"]},')
        out.append(f'        {r["otg_v_total"]}, {r["otg_v_sync_a_end"]}, {r["otg_v_blank_start"]}, '
                   f'{r["otg_v_blank_end"]},')
        out.append(f'        {r["otg_h_sync_a_pol"]}, {r["otg_v_sync_a_pol"]}, {r["otg_start_point"]}, '
                   f'{r["otg_tmds_pixel_rate_div"]},')
        out.append(f'        {r["max_vstartup"]},')
        if dp:
            out.append(f'        {r["dp8_req_kbps"]}u, {r["dp10_req_kbps"]}u, {r["dp8_bw_kbps"]}u, '
                       f'{r["dp10_bw_kbps"]}u,')
            out.append(f'        {r["dp8_lanes"]}, 0x{r["dp8_rate_code"]:02x}, {r["dp10_lanes"]}, '
                       f'0x{r["dp10_rate_code"]:02x},')
            out.append("        0u, 0u, 0, 0, 0, 0,")
        else:
            out.append(f'        {r["hdmi8_tmds_khz"] * 24 // 8}u, {r["hdmi10_tmds_khz"] * 24 // 10}u, 0u, 0u,')
            out.append("        0, 0, 0, 0,")
            out.append(f'        {r["hdmi8_tmds_khz"]}u, {r["hdmi10_tmds_khz"]}u,')
            out.append(f'        {r["hdmi8_scramble"]}, {r["hdmi10_scramble"]}, {r["hdmi8_within_dio"]}, '
                       f'{r["hdmi10_within_dio"]},')
        out.append("    },")
    out += ["};", "", "#define DCN41_MODE_COUNT (sizeof(dcn41_modes) / sizeof(dcn41_modes[0]))", "",
            "#endif /* N48_DCN41_MODES_H */", ""]
    return "\n".join(out)


def main():
    modes, ctl = build()
    r = rows(modes)
    h, t = header(r, ctl), tsv(r)
    if "--check" in sys.argv:
        stale = [p for p, want in ((OUT_H, h), (OUT_TSV, t)) if (p.read_text() if p.exists() else "") != want]
        if stale:
            print("gen_modes --check: STALE:", ", ".join(p.name for p in stale))
            return 1
        print(f"gen_modes --check: {OUT_H.name} and {OUT_TSV.name} are fresh ({len(r)} modes); "
              f"CVT-RB v1 control {ctl[0]}x{ctl[1]} at {ctl[2]} kHz")
        return 0
    OUT_H.write_text(h)
    OUT_TSV.write_text(t)
    print(f"gen_modes: {len(r)} modes -> {OUT_H} and {OUT_TSV}; CVT-RB v1 control {ctl[0]}x{ctl[1]} at {ctl[2]} kHz")
    for row in r:
        print(f"  {row['monitor']:14s} {row['mode']:15s} {row['source']:13s} {row['pix_clk_100hz']/10000:8.3f} MHz "
              f"{row['h_total']}x{row['v_total']} {row['refresh_mhz']/1000:7.3f} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
