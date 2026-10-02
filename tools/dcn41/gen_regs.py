#!/usr/bin/env python3
"""gen_regs.py - generate src/dcn41/dcn41_regs.h from Linux's DCN 4.1 register headers.

  python3 tools/dcn41/gen_regs.py            write src/dcn41/dcn41_regs.h
  python3 tools/dcn41/gen_regs.py --check    exit 1 if the checked-in header differs from a fresh generation

Inputs (gitignored clone re/linux-dc, commit 238650ef6c7c):
  drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_offset.h   regXXX offsets and regXXX_BASE_IDX
  drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_sh_mask.h  XXX__FIELD_MASK / XXX__FIELD__SHIFT

Every offset is emitted per instance straight from the header (never from a stride), every field mask and shift from
the instance-0 name (the convention of Linux's own *_MASK_SH_LIST_DCN401 lists), and the generator REFUSES when an
instance is missing, when instances disagree on BASE_IDX, or when another instance's mask/shift differs from
instance 0's. No arithmetic other than parsing happens here. Pure python, no compiler child.
"""
import pathlib, re, sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

OUT = L.SRC / "dcn41_regs.h"

# (group, instance prefix pattern with {i} or None for a single instance, count, register, fields)
SPEC = [
    # ---- OTG: VBL interrupt sources, the master update lock, position and frame counters, timing readout ----
    ("OTG", "OTG{i}_", 4, "OTG_GLOBAL_SYNC_STATUS",
     ["VSTARTUP_INT_EN", "VSTARTUP_EVENT_OCCURRED", "VSTARTUP_INT_STATUS", "VSTARTUP_EVENT_CLEAR",
      "VUPDATE_NO_LOCK_INT_EN", "VUPDATE_NO_LOCK_EVENT_OCCURRED", "VUPDATE_NO_LOCK_INT_STATUS",
      "VUPDATE_NO_LOCK_EVENT_CLEAR"]),
    ("OTG", "OTG{i}_", 4, "OTG_MASTER_UPDATE_LOCK", ["OTG_MASTER_UPDATE_LOCK", "UPDATE_LOCK_STATUS"]),
    ("OTG", "OTG{i}_", 4, "OTG_GLOBAL_CONTROL2", ["OTG_MASTER_UPDATE_LOCK_SEL"]),
    ("OTG", "OTG{i}_", 4, "OTG_STATUS_POSITION", ["OTG_VERT_COUNT", "OTG_HORZ_COUNT"]),
    ("OTG", "OTG{i}_", 4, "OTG_NOM_VERT_POSITION", ["OTG_VERT_COUNT_NOM"]),
    ("OTG", "OTG{i}_", 4, "OTG_STATUS_FRAME_COUNT", ["OTG_FRAME_COUNT"]),
    ("OTG", "OTG{i}_", 4, "OTG_V_BLANK_START_END", ["OTG_V_BLANK_START", "OTG_V_BLANK_END"]),
    ("OTG", "OTG{i}_", 4, "OTG_H_BLANK_START_END", ["OTG_H_BLANK_START", "OTG_H_BLANK_END"]),
    ("OTG", "OTG{i}_", 4, "OTG_CONTROL", ["OTG_MASTER_EN"]),
    ("OTG", "OTG{i}_", 4, "OTG_H_TOTAL", ["OTG_H_TOTAL"]),
    ("OTG", "OTG{i}_", 4, "OTG_V_TOTAL", ["OTG_V_TOTAL"]),
    # ---- HUBP / HUBPREQ: plane address (the flip), flip control and interrupt, earliest-in-use readback ----
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_FLIP_CONTROL",
     ["SURFACE_UPDATE_LOCK", "SURFACE_FLIP_TYPE", "SURFACE_FLIP_PENDING", "SURFACE_FLIP_MODE_FOR_STEREOSYNC",
      "SURFACE_FLIP_IN_STEREOSYNC"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_SURFACE_FLIP_INTERRUPT",
     ["SURFACE_FLIP_INT_MASK", "SURFACE_FLIP_INT_TYPE", "SURFACE_FLIP_CLEAR", "SURFACE_FLIP_OCCURRED",
      "SURFACE_FLIP_INT_STATUS"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_PRIMARY_SURFACE_ADDRESS", ["PRIMARY_SURFACE_ADDRESS"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", ["PRIMARY_SURFACE_ADDRESS_HIGH"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_SURFACE_CONTROL", ["PRIMARY_SURFACE_TMZ"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_SURFACE_EARLIEST_INUSE", ["SURFACE_EARLIEST_INUSE_ADDRESS"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "DCSURF_SURFACE_EARLIEST_INUSE_HIGH", ["SURFACE_EARLIEST_INUSE_ADDRESS_HIGH"]),
    ("HUBPREQ", "HUBPREQ{i}_", 4, "VMID_SETTINGS_0", ["VMID"]),
    ("HUBP", "HUBP{i}_", 4, "DCHUBP_CNTL", ["HUBP_IN_BLANK"]),
    # ---- HPD: hot-plug interrupt control and status (irq_service_dcn401.c hpd_int_entry / hpd0_ack) ----
    ("HPD", "HPD{i}_", 4, "DC_HPD_INT_CONTROL",
     ["DC_HPD_INT_ACK", "DC_HPD_INT_POLARITY", "DC_HPD_INT_EN", "DC_HPD_RX_INT_ACK", "DC_HPD_RX_INT_EN"]),
    ("HPD", "HPD{i}_", 4, "DC_HPD_INT_STATUS",
     ["DC_HPD_INT_STATUS", "DC_HPD_SENSE", "DC_HPD_SENSE_DELAYED", "DC_HPD_RX_INT_STATUS"]),
    # ---- DMCUB: outbox interrupt (dmub_outbox_int_entry), and the command-ring / boot-state registers ----
    ("DMCUB", None, 1, "DMCUB_INTERRUPT_ENABLE", ["DMCUB_OUTBOX1_READY_INT_EN"]),
    ("DMCUB", None, 1, "DMCUB_INTERRUPT_ACK", ["DMCUB_OUTBOX1_READY_INT_ACK"]),
    ("DMCUB", None, 1, "DMCUB_CNTL", ["DMCUB_ENABLE", "DMCUB_TRACEPORT_EN", "DMCUB_PWAIT_MODE_STATUS"]),
    ("DMCUB", None, 1, "DMCUB_CNTL2", ["DMCUB_SOFT_RESET"]),
    ("DMCUB", None, 1, "DMCUB_SEC_CNTL", ["DMCUB_SEC_RESET", "DMCUB_MEM_UNIT_ID", "DMCUB_SEC_RESET_STATUS"]),
    ("DMCUB", None, 1, "DMCUB_INBOX1_BASE_ADDRESS", ["DMCUB_INBOX1_BASE_ADDRESS"]),
    ("DMCUB", None, 1, "DMCUB_INBOX1_SIZE", ["DMCUB_INBOX1_SIZE"]),
    ("DMCUB", None, 1, "DMCUB_INBOX1_RPTR", ["DMCUB_INBOX1_RPTR"]),
    ("DMCUB", None, 1, "DMCUB_INBOX1_WPTR", ["DMCUB_INBOX1_WPTR"]),
    ("DMCUB", None, 1, "DMCUB_OUTBOX1_BASE_ADDRESS", ["DMCUB_OUTBOX1_BASE_ADDRESS"]),
    ("DMCUB", None, 1, "DMCUB_OUTBOX1_SIZE", ["DMCUB_OUTBOX1_SIZE"]),
    ("DMCUB", None, 1, "DMCUB_OUTBOX1_RPTR", ["DMCUB_OUTBOX1_RPTR"]),
    ("DMCUB", None, 1, "DMCUB_OUTBOX1_WPTR", ["DMCUB_OUTBOX1_WPTR"]),
    ("DMCUB", None, 1, "DMCUB_SCRATCH0", ["DMCUB_SCRATCH0"]),
    ("DMCUB", None, 1, "DMCUB_SCRATCH7", ["DMCUB_SCRATCH7"]),
    ("DMCUB", None, 1, "DMCUB_SCRATCH8", ["DMCUB_SCRATCH8"]),
    ("DMCUB", None, 1, "DMCUB_GPINT_DATAIN1", ["DMCUB_GPINT_DATAIN1"]),
    ("DMCUB", None, 1, "DMCUB_GPINT_DATAOUT", ["DMCUB_GPINT_DATAOUT"]),
    ("DMCUB", None, 1, "DMCUB_TIMER_CURRENT", ["DMCUB_TIMER_CURRENT"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW0_BASE_ADDRESS", ["DMCUB_REGION3_CW0_BASE_ADDRESS"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW0_TOP_ADDRESS", ["DMCUB_REGION3_CW0_TOP_ADDRESS", "DMCUB_REGION3_CW0_ENABLE"]),
    ("DMCUB", None, 1, "DMCUB_REGION4_OFFSET", ["DMCUB_REGION4_OFFSET"]),
    ("DMCUB", None, 1, "DMCUB_REGION4_OFFSET_HIGH", ["DMCUB_REGION4_OFFSET_HIGH"]),
    ("DMCUB", None, 1, "DMCUB_REGION4_TOP_ADDRESS", ["DMCUB_REGION4_TOP_ADDRESS", "DMCUB_REGION4_ENABLE"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW4_OFFSET", ["DMCUB_REGION3_CW4_OFFSET"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW4_OFFSET_HIGH", ["DMCUB_REGION3_CW4_OFFSET_HIGH"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW4_BASE_ADDRESS", ["DMCUB_REGION3_CW4_BASE_ADDRESS"]),
    ("DMCUB", None, 1, "DMCUB_REGION3_CW4_TOP_ADDRESS", ["DMCUB_REGION3_CW4_TOP_ADDRESS", "DMCUB_REGION3_CW4_ENABLE"]),
    ("DMCUB", None, 1, "DMCUB_SCRATCH14", ["DMCUB_SCRATCH14"]),
    ("DMCUB", None, 1, "DMCUB_SCRATCH15", ["DMCUB_SCRATCH15"]),
    ("DMCUB", None, 1, "DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR", ["DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR"]),
    # ---- DCN VM framebuffer window: MC address of VRAM byte 0 in 16 MiB units (dmub_dcn401_get_fb_base_offset) ----
    ("DCN", None, 1, "DCN_VM_FB_LOCATION_BASE", ["FB_BASE"]),
    ("DCN", None, 1, "DCN_VM_FB_LOCATION_TOP", ["FB_TOP"]),
    ("DCN", None, 1, "DCN_VM_FB_OFFSET", ["FB_OFFSET"]),
]

DEF = re.compile(r"^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)L?\s*$")


def parse(path):
    d = {}
    for line in path.read_text().splitlines():
        m = DEF.match(line.strip())
        if m:
            d[m.group(1)] = int(m.group(2), 0)
    return d


def generate():
    L.need_linux()
    off, msk = parse(L.OFFSET_H), parse(L.SHMASK_H)
    out = [
        "/* dcn41_regs.h - GENERATED by tools/dcn41/gen_regs.py; do not edit (regenerate, then --check).",
        " *",
        f" * Source: Linux {L.LINUX_COMMIT}, drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_offset.h and",
        " * dcn_4_1_0_sh_mask.h (MIT). Each register offset is the header's regNAME value for that instance, relative to",
        " * the DCN segment named by its _BASE_IDX; the absolute BAR5 dword is seg[BASE_IDX] + offset, with seg[] the DMU",
        " * (discovery hw id 271, v4.1.0) segment bases. Field masks and shifts are the instance-0 names, as Linux's own",
        " * DCN401 mask lists use; the generator refused if any instance disagreed.",
        " */",
        "#ifndef N48_DCN41_REGS_H",
        "#define N48_DCN41_REGS_H",
        "",
        "#define DCN41_BAD_OFFSET 0xFFFFFFFFu",
        "",
    ]
    max_by_base = {}
    for group, pat, count, reg, fields in SPEC:
        names = [f"reg{pat.format(i=i)}{reg}" if pat else f"reg{reg}" for i in range(count)]
        vals, bases = [], set()
        for n in names:
            if n not in off or f"{n}_BASE_IDX" not in off:
                raise SystemExit(f"gen_regs: {n} or its _BASE_IDX missing from {L.OFFSET_H.name}: refusing")
            vals.append(off[n])
            bases.add(off[f"{n}_BASE_IDX"])
        if len(bases) != 1:
            raise SystemExit(f"gen_regs: {reg}: instances disagree on BASE_IDX {sorted(bases)}: refusing")
        base = bases.pop()
        max_by_base[base] = max(max_by_base.get(base, 0), max(vals))
        cname = f"DCN41_{group}_{reg}" if pat else f"DCN41_{reg}"
        out.append("/* " + ", ".join(f"{n} 0x{v:04x}" for n, v in zip(names, vals)) + f"; BASE_IDX {base} */")
        if pat:
            chain = " : ".join(f"(i) == {i} ? 0x{v:04x}u" for i, v in enumerate(vals))
            out.append(f"#define {cname}(i) ((unsigned)({chain} : DCN41_BAD_OFFSET))")
        else:
            out.append(f"#define {cname} 0x{vals[0]:04x}u")
        out.append(f"#define {cname}_BASE_IDX {base}u")
        for fld in fields:
            mname0 = (f"{pat.format(i=0)}{reg}__{fld}" if pat else f"{reg}__{fld}")
            if f"{mname0}_MASK" not in msk or f"{mname0}__SHIFT" not in msk:
                raise SystemExit(f"gen_regs: {mname0}_MASK/__SHIFT missing from {L.SHMASK_H.name}: refusing")
            m0, s0 = msk[f"{mname0}_MASK"], msk[f"{mname0}__SHIFT"]
            if pat:
                for i in range(1, count):
                    mi = f"{pat.format(i=i)}{reg}__{fld}"
                    if f"{mi}_MASK" in msk and (msk[f"{mi}_MASK"], msk[f"{mi}__SHIFT"]) != (m0, s0):
                        raise SystemExit(f"gen_regs: {mi} differs from instance 0: refusing")
            out.append(f"#define {cname}__{fld}_MASK 0x{m0:08x}u  /* {mname0}_MASK */")
            out.append(f"#define {cname}__{fld}__SHIFT {s0}u")
        out.append("")
    out.append("/* largest offset used per BASE_IDX: dcn41_dev_init() checks seg[b] + this is inside the BAR5 window */")
    for b in sorted(max_by_base):
        out.append(f"#define DCN41_MAX_OFFSET_BASE{b} 0x{max_by_base[b]:04x}u")
    out.append(f"#define DCN41_BASE_IDX_USED_MASK 0x{sum(1 << b for b in max_by_base):x}u")
    out += ["", "#endif /* N48_DCN41_REGS_H */", ""]
    return "\n".join(out)


def main():
    text = generate()
    if "--check" in sys.argv:
        cur = OUT.read_text() if OUT.exists() else ""
        if cur != text:
            print(f"gen_regs --check: {OUT} is STALE (regenerate)")
            return 1
        print(f"gen_regs --check: {OUT.name} matches a fresh generation ({text.count(chr(10))} lines)")
        return 0
    OUT.write_text(text)
    print(f"gen_regs: wrote {OUT} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
