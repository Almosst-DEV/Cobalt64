#!/usr/bin/env python3
"""gen_census.py - emit the exact T1 read-only register census as a TSV (src/dcn41/dcn41_census.tsv).

Every address in notes/DISPLAY-DESIGN.md section 6's T1 is produced here from the DCN 4.1 offset header, never typed by
hand: the script looks each name up in dcn_4_1_0_offset.h, reads its BASE_IDX from the header too, and computes the
absolute BAR5 dword as seg[base_idx] + offset with the segment bases our kext discovered on this card
(notes/DISPLAY-DESIGN.md section 2.1). It refuses to write anything if a name is missing or a BASE_IDX is outside the
five segments. The DMCUB rows are the same 27 registers src/dcn41/dcn41_dmub.c's probe table reads, in the same order,
and the script checks that correspondence against the .c file so the census and the probe cannot drift apart.

  python3 tools/dcn41/gen_census.py            # write the TSV
  python3 tools/dcn41/gen_census.py --check    # fail if the committed TSV differs
"""
import re
import sys

import dcn41lib as L

SEG = [0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00]   # CONFIRMED, our card's discovery log; DISPLAY-DESIGN section 2.1
OUT = L.SRC / "dcn41_census.tsv"

# group, register name template, how many passes, why / what to read out of it
ROWS = [
    ("otg",  "OTG{n}_OTG_CONTROL",                1, "master enable, field OTG_MASTER_EN: which OTG is lit"),
    ("otg",  "OTG{n}_OTG_H_TOTAL",                1, "h_total - 1; compare with dcn41_modes.h"),
    ("otg",  "OTG{n}_OTG_H_BLANK_START_END",      1, "blank start/end"),
    ("otg",  "OTG{n}_OTG_H_SYNC_A",               1, "sync start/end"),
    ("otg",  "OTG{n}_OTG_H_SYNC_A_CNTL",          1, "sync polarity"),
    ("otg",  "OTG{n}_OTG_V_TOTAL",                1, "v_total - 1"),
    ("otg",  "OTG{n}_OTG_V_BLANK_START_END",      1, "blank start/end"),
    ("otg",  "OTG{n}_OTG_V_SYNC_A",               1, "sync start/end"),
    ("otg",  "OTG{n}_OTG_V_SYNC_A_CNTL",          1, "sync polarity"),
    ("otg",  "OTG{n}_OTG_STATUS",                 2, "V_ACTIVE_DISP / vblank now: moves if the OTG runs"),
    ("otg",  "OTG{n}_OTG_STATUS_POSITION",        2, "vertical/horizontal count: must move between passes"),
    ("otg",  "OTG{n}_OTG_STATUS_FRAME_COUNT",     2, "frame count: +600 in 10 s at 60 Hz (T1 prediction 1)"),
    ("otg",  "OTG{n}_OTG_VSTARTUP_PARAM",         1, "VSTARTUP_START: the GOP's DML answer for the lit mode"),
    ("otg",  "OTG{n}_OTG_VUPDATE_PARAM",          1, "VUPDATE offset and width; width 0 would break the flip latch"),
    ("otg",  "OTG{n}_OTG_VREADY_PARAM",           1, "VREADY offset"),
    ("otg",  "OTG{n}_OTG_GLOBAL_SYNC_STATUS",     1, "the interrupt enables/status our dcn41_irq writes"),
    ("otg",  "OTG{n}_OTG_MASTER_UPDATE_LOCK",     1, "must read UPDATE_LOCK 0 before T3 flips"),
    ("otg",  "OTG{n}_OTG_VERTICAL_INTERRUPT0_CONTROL", 1, "VLINE0 arming left by the GOP"),
    ("otg",  "OTG{n}_OTG_VERTICAL_INTERRUPT1_CONTROL", 1, "VLINE1"),
    ("otg",  "OTG{n}_OTG_VERTICAL_INTERRUPT2_CONTROL", 1, "VLINE2"),
    ("otg",  "OTG{n}_OTG_CRC_CNTL",               1, "is CRC already on; T3 needs it"),
    ("hubp", "HUBP{n}_DCHUBP_CNTL",               1, "HUBP enable/blank: pairs a HUBP with the lit OTG"),
    ("hubp", "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS", 1, "the console's scanout address, low dword"),
    ("hubp", "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 1, "high dword"),
    ("hubp", "HUBPREQ{n}_DCSURF_SURFACE_EARLIEST_INUSE", 1, "what the hardware is actually fetching"),
    ("hubp", "HUBPREQ{n}_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 1, "high dword"),
    ("hubp", "HUBPREQ{n}_DCSURF_FLIP_CONTROL",    1, "SURFACE_UPDATE_PENDING and the flip mode bits"),
    ("hubp", "HUBP{n}_DCSURF_SURFACE_CONFIG",     1, "pixel format of the console surface"),
    ("dig",  "DIG{n}_DIG_FE_CNTL",                1, "stream encoder mode; which DIG carries a stream"),
    ("dig",  "DIG{n}_DIG_BE_CLK_CNTL",            1, "link encoder clock enable and source"),
    ("dig",  "DIG{n}_DIG_BE_EN_CNTL",             1, "DIG_BE_ENABLE: 0 on the dormant HDMI path (T1 prediction 12)"),
    ("dig",  "DIG{n}_HDMI_CONTROL",               1, "HDMI vs DVI, deep colour"),
    ("dig",  "DIG{n}_TMDS_CTL_BITS",              1, "TMDS control bits"),
    ("dig",  "DIG{n}_TMDS_CNTL",                  1, "TMDS pixel encoding"),
    ("dp",   "DP{n}_DP_VID_STREAM_CNTL",          1, "DP_VID_STREAM_ENABLE and its status bit"),
    ("dp",   "DP{n}_DP_LINK_CNTL",                1, "link training state left by the GOP"),
    ("dp",   "DP{n}_DP_CONFIG",                   1, "lane count / config"),
    ("misc", "DC_GPIO_HPD_Y",                     1, "which connectors have a sink attached (HPD level)"),
    ("misc", "DC_GPIO_HPD_EN",                    1, "HPD enables"),
    ("misc", "DCCG_GATE_DISABLE_CNTL",            1, "which display clocks are ungated"),
    ("misc", "OTG0_PIXEL_RATE_CNTL",              1, "pixel rate source and divider, OTG0 (base 1)"),
    ("misc", "OTG1_PIXEL_RATE_CNTL",              1, "OTG1"),
    ("misc", "OTG2_PIXEL_RATE_CNTL",              1, "OTG2"),
    ("misc", "OTG3_PIXEL_RATE_CNTL",              1, "OTG3"),
]
PER_INSTANCE = {"otg", "hubp", "dig", "dp"}
# the DMCUB rows, in dcn41_dmub.c's probe order
DMUB = [
    "DMCUB_CNTL", "DMCUB_CNTL2", "DMCUB_SEC_CNTL", "DMCUB_SCRATCH0", "DMCUB_SCRATCH7", "DMCUB_SCRATCH14",
    "DMCUB_SCRATCH15", "DMCUB_GPINT_DATAIN1", "DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR", "DMCUB_INBOX1_BASE_ADDRESS",
    "DMCUB_INBOX1_SIZE", "DMCUB_INBOX1_WPTR", "DMCUB_INBOX1_RPTR", "DMCUB_OUTBOX1_BASE_ADDRESS", "DMCUB_OUTBOX1_SIZE",
    "DMCUB_OUTBOX1_WPTR", "DMCUB_OUTBOX1_RPTR", "DMCUB_REGION3_CW4_OFFSET", "DMCUB_REGION3_CW4_OFFSET_HIGH",
    "DMCUB_REGION3_CW4_BASE_ADDRESS", "DMCUB_REGION3_CW4_TOP_ADDRESS", "DMCUB_REGION4_OFFSET",
    "DMCUB_REGION4_OFFSET_HIGH", "DMCUB_REGION4_TOP_ADDRESS", "DCN_VM_FB_LOCATION_BASE", "DCN_VM_FB_LOCATION_TOP",
    "DCN_VM_FB_OFFSET",
]
DMUB_NOTE = {
    "DMCUB_CNTL": "DMCUB_ENABLE: expect 1 (T1 prediction 10)",
    "DMCUB_CNTL2": "DMCUB_SOFT_RESET: expect 0",
    "DMCUB_SCRATCH0": "boot status: bits 0 (dal_fw) and 1 (mailbox_rdy) expected set",
    "DMCUB_SCRATCH7": "GPINT return data",
    "DMCUB_INBOX1_SIZE": "ring size; expect a multiple of 64",
    "DMCUB_INBOX1_WPTR": "expect == RPTR (idle) and a multiple of 64",
    "DMCUB_INBOX1_RPTR": "expect == WPTR",
    "DMCUB_REGION3_CW4_BASE_ADDRESS": "0x64000000 if the ring maps through CW4 (T1 prediction 11)",
    "DMCUB_REGION4_OFFSET": "REGION4 mapping, the alternative",
    "DCN_VM_FB_LOCATION_BASE": "T3's buffer must sit inside BASE..TOP",
}


def parse_header():
    """{name: (offset, base_idx)} from dcn_4_1_0_offset.h."""
    txt = L.OFFSET_H.read_text(errors="replace")
    off, base = {}, {}
    for m in re.finditer(r"^#define\s+reg([A-Za-z0-9_]+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$", txt, re.M):
        name, is_base, val = m.group(1), m.group(2), int(m.group(3), 0)
        (base if is_base else off)[name] = val
    return off, base


def probe_order_from_c():
    """the DMCUB names dcn41_dmub.c's probe table reads, in order - the census must match it exactly."""
    txt = (L.SRC / "dcn41_dmub.c").read_text()
    body = txt.split("dcn41_dmub_probe_regs[] = {", 1)[1].split("};", 1)[0]
    return [m.group(1) for m in re.finditer(r"\{\s*DCN41_([A-Z0-9_]+),", body)]


def build():
    off, base = parse_header()
    rows, problems = [], []

    def add(group, name, passes, note):
        if name not in off:
            problems.append("no regxx definition for %s" % name)
            return
        if name not in base:
            problems.append("no BASE_IDX for %s" % name)
            return
        b = base[name]
        if b >= len(SEG):
            problems.append("%s has BASE_IDX %d, outside the five segments" % (name, b))
            return
        rows.append((group, name, "0x%04x" % off[name], str(b), "0x%08x" % (SEG[b] + off[name]), str(passes), note))

    for group, tmpl, passes, note in ROWS:
        if group in PER_INSTANCE:
            for n in range(4):
                add(group, tmpl.format(n=n), passes, note if n == 0 else note + " (instance %d)" % n)
        else:
            add(group, tmpl, passes, note)
    want = probe_order_from_c()
    if want != DMUB:
        problems.append("DMCUB list differs from dcn41_dmub.c's probe table:\n  .c: %s\n  us: %s" % (want, DMUB))
    for name in DMUB:
        add("dmub", name, 1, DMUB_NOTE.get(name, "DMUB probe input (src/dcn41/dcn41_dmub.c)"))
    if problems:
        sys.exit("gen_census: refusing to write:\n  " + "\n  ".join(problems))

    head = ("# T1 read-only register census, generated by tools/dcn41/gen_census.py from %s\n"
            "# absolute = seg[base_idx] + offset, seg = %s (CONFIRMED, our card's discovery log)\n"
            "# passes = how many times to read it (2 = twice, 10 s apart)\n"
            "# READ ONLY. Nothing here clears on read: AUX SW_DATA and DC_I2C_DATA are excluded on purpose.\n"
            "group\tregister\toffset\tbase_idx\tabsolute\tpasses\tnote\n"
            % (L.OFFSET_H.name, " ".join("0x%x" % s for s in SEG)))
    return head + "".join("\t".join(r) + "\n" for r in rows), len(rows)


def main():
    text, n = build()
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            sys.exit("gen_census: %s is stale or missing - rerun without --check" % OUT)
        print("gen_census: %s matches the generator (%d registers)" % (OUT.name, n))
        return
    OUT.write_text(text)
    reads = sum(int(line.split("\t")[5]) for line in text.splitlines() if not line.startswith(("#", "group")))
    print("gen_census: wrote %s, %d registers, %d reads" % (OUT, n, reads))


if __name__ == "__main__":
    main()
