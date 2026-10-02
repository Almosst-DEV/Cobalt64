#!/usr/bin/env python3
"""gen_allow.py - generate src/dcn41/dcn41_allow_ranges.h: the DCN 4.1 display write allowlist.

  python3 tools/dcn41/gen_allow.py            rewrite the header
  python3 tools/dcn41/gen_allow.py --check    fail if the header is not a fresh generation

Deny by default. The generated table is the ONLY thing that can make a register write we originate
legal, and every number in it is computed here from two sources, neither of them typed by hand:

  * the DCN 4.1 register offsets and their BASE_IDX, from Linux's dcn_4_1_0_offset.h (238650ef6c7c);
  * the DMU segment bases this card's own IP discovery table reports, quoted in SEGS below.

Absolute BAR5 dword = seg[BASE_IDX] + offset. Offsets within one BASE_IDX are merged into ranges
with a gap tolerance of MERGE_GAP dwords, so a range is a contiguous stretch of the display block
and not "the whole segment".

Segments 0 and 4 are DENIED outright and are never emitted:

  * BASE_IDX 0 (base 0x12) carries only Azalia audio registers (all 89 of them begin regAZ*), which
    we never write - and in ABSOLUTE dword space 0x12..0x5f OVERLAPS NBIO's indirect SMN windows:
    the discovery table gives NBIF v6.3.1 segment 1 = 0x14, so BIF_BX_PF0_RSMU_INDEX/DATA land at
    absolute 0x14/0x15 and BIF_BX0_PCIE_INDEX/DATA at 0x20/0x21 (src/navi48-bringup/src/amd/
    amdgpu_ip.h NBIORegs). Those windows reach ALL of SMN, including the SMU, SMUIO, THM and the
    SPI ROM. Allowing the DCN audio segment would therefore hand the display code the one escape
    hatch this allowlist exists to close.
  * BASE_IDX 4 (base 0x2403c00) is used by no register in the DCN 4.1 header at all, and
    0x2403c00 dwords is far outside the 1 MiB BAR5 window anyway.

HARD_DENY is checked BEFORE the allow table, so even a mis-generated range cannot let one through.
"""
import pathlib, re, subprocess, sys, collections

ROOT = pathlib.Path(__file__).resolve().parents[2]
OFFSETS = ROOT / "re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_offset.h"
OUT = ROOT / "src/dcn41/dcn41_allow_ranges.h"
REGS_H = ROOT / "src/dcn41/dcn41_regs.h"

# The DMU (discovery hw id 271, v4.1.0) segment bases measured on THIS card:
#   notes/logs/runs/sacomp1/driverlog-stream.txt:1413 and notes/logs/runs/adopt1/driverlog-post.txt:1371
#   "Navi48Ttl:   hwId 271 inst 0  v4.1.0  5 segs: 00000012 000000c0 000034c0 00009000 02403c00"
SEGS = (0x00000012, 0x000000c0, 0x000034c0, 0x00009000, 0x02403c00)
ALLOWED_BASE_IDX = (1, 2, 3)
MERGE_GAP = 0x40           # dwords; 0x40 = 256 bytes, one small register block
BAR5_DWORDS = 0x40000      # 1 MiB BAR5 (amd/amdgpu_regs.h bounds every read and write against the real size)

# Register families excluded from the allowlist even though the DCN header carries them. Azalia (HD
# audio) is display-adjacent silicon we never touch, and its BASE_IDX 3 ALIAS block sits at offsets
# 0x4b7000+, i.e. absolute 0x4c0000+ - outside the 1 MiB BAR5 window and on top of hwId 274's segment
# base 0x4c0000. The audit below caught exactly that before this exclusion existed, which is the
# control that says the audit works.
EXCLUDE_FAMILY_PREFIXES = ("AZ",)

# Absolute dwords that are never writable through this guard, whatever the allow table says.
# Every one is an indirect-access window: writing it selects a target anywhere in SMN or VRAM and the
# NEXT write goes there, which would defeat an address-range allowlist completely.
HARD_DENY = [
    (0x00000000, 0x000000ff, "the low BAR5 page: MM_INDEX/MM_DATA/MM_INDEX_HI (0x0/0x1/0x6) and NBIO's "
                             "BIF_BX1_PCIE_INDEX2/DATA2 (0xe/0xf), RSMU_INDEX/DATA (0x14/0x15) and "
                             "BIF_BX0_PCIE_INDEX/DATA (0x20/0x21) - indirect windows into all of SMN"),
]

# Documented blocks that must never be reachable. None of them overlaps a display range - the generator
# asserts that - but they are listed so a refusal names the block instead of only saying "out of range",
# and so the host test has real addresses to fire at. Bases from the same discovery dump as SEGS.
DENY_BLOCKS = [
    (0x00016000, 0x000163ff, "MP1/SMU (hwId 1 v14.0.3 segs 0x16000, 0x16200): the SMU message mailbox "
                             "regMP1_SMN_C2PMSG_66/82/90 lives at 0x16282/0x16292/0x1629a - power, voltage, "
                             "clock limits and DPM tables are all behind it"),
    (0x0001ce00, 0x0001cfff, "MP0/PSP (hwId 255 v14.0.3 segment 2): firmware load and the ROM path"),
    (0x000dc0000, 0x000e7ffff, "MP0/MP1 upper segments 0xdc0000/0xe00000/0xe40000"),
    (0x02458000, 0x024587ff, "hwId 293/273 v1.0.x (0x2458000/0x2458400): SMUIO-class blocks"),
]


def parse_offsets():
    txt = OFFSETS.read_text()
    off, bidx = {}, {}
    for m in re.finditer(r'^#define\s+(reg\w+)\s+(0x[0-9a-fA-F]+)\s*$', txt, re.M):
        off[m.group(1)] = int(m.group(2), 16)
    for m in re.finditer(r'^#define\s+(reg\w+)_BASE_IDX\s+(\d+)\s*$', txt, re.M):
        bidx[m.group(1)] = int(m.group(2))
    if len(off) < 5000:
        raise SystemExit(f"gen_allow: only {len(off)} offsets parsed from {OFFSETS} - wrong file?")
    return off, bidx


def family(name):
    """Hardware block a register name belongs to: regOTG2_OTG_CONTROL -> OTG2, regDMCUB_CNTL -> DMCUB."""
    return name[3:].split("_")[0]


def build_ranges():
    off, bidx = parse_offsets()
    per_base = collections.defaultdict(dict)       # base -> {offset: family}
    excluded_audio, excluded_window = [], []
    for name, v in off.items():
        b = bidx.get(name)
        if b is None or b not in ALLOWED_BASE_IDX:
            continue
        fam = family(name)
        if fam.startswith(EXCLUDE_FAMILY_PREFIXES):
            excluded_audio.append(name)
            continue
        if SEGS[b] + v >= BAR5_DWORDS:
            # Unreachable in any case: amd/amdgpu_regs.h RREG32/WREG32 refuse an address past the BAR.
            # On this part the whole HD-audio controller alias block (GLOBAL_CAPABILITIES, CORB_*,
            # RIRB_*, WALL_CLOCK_COUNTER, ... at BASE_IDX 3 offsets 0x4b7000+) lands here, absolute
            # 0x4c0000+, which is also hwId 274's segment base. Counted, not silently dropped.
            excluded_window.append(name)
            continue
        per_base[b][v] = fam
    ranges = []
    for b in sorted(per_base):
        items = sorted(per_base[b].items())
        run_lo, run_hi = items[0][0], items[0][0]
        fams = [items[0][1]]
        for v, fam in items[1:]:
            if v - run_hi <= MERGE_GAP:
                run_hi = v
                fams.append(fam)
            else:
                ranges.append((b, run_lo, run_hi, fams))
                run_lo, run_hi, fams = v, v, [fam]
        ranges.append((b, run_lo, run_hi, fams))
    out = []
    for b, lo, hi, fams in ranges:
        uniq = set(fams)
        # fams is in ascending address order, so first..last names the range by where it starts and ends.
        name = fams[0] if len(uniq) == 1 else f"{fams[0]}..{fams[-1]}"
        out.append({"base": b, "off_lo": lo, "off_hi": hi,
                    "abs_lo": SEGS[b] + lo, "abs_hi": SEGS[b] + hi,
                    "name": name, "nfam": len(uniq), "nreg": len(fams)})
    out.sort(key=lambda r: r["abs_lo"])
    print(f"gen_allow: excluded {len(excluded_audio)} Azalia register(s) and {len(excluded_window)} "
          f"register(s) whose absolute address is outside the {BAR5_DWORDS:#x}-dword BAR5 window "
          f"(first: {sorted(excluded_window)[0] if excluded_window else 'none'})")
    return out


def audit(ranges):
    """Every property the allowlist claims, checked here rather than asserted in prose."""
    problems = []
    # 1. ranges are sorted, disjoint and inside the BAR5 window
    for i, r in enumerate(ranges):
        if r["abs_hi"] >= BAR5_DWORDS:
            problems.append(f"range {r['name']} ends at 0x{r['abs_hi']:x}, outside the BAR5 window")
        if i and r["abs_lo"] <= ranges[i - 1]["abs_hi"]:
            problems.append(f"range {r['name']} overlaps {ranges[i-1]['name']}")
    # 2. no allowed range touches a hard-denied address or a denied block
    for r in ranges:
        for lo, hi, why in HARD_DENY + DENY_BLOCKS:
            if r["abs_lo"] <= hi and lo <= r["abs_hi"]:
                problems.append(f"range {r['name']} 0x{r['abs_lo']:x}-0x{r['abs_hi']:x} intersects "
                                f"denied 0x{lo:x}-0x{hi:x} ({why.split(':')[0]})")
    # 3. no non-DMU IP's segment base falls inside an allowed range. Bases quoted verbatim from the
    #    discovery dump in notes/logs/runs/adopt1/driverlog-post.txt:1360-1405 (every IP except hwId 271).
    other_bases = [
        0x00000c00, 0x02408c00, 0x00016c00, 0x02401800, 0x00016e00, 0x02401c00, 0x00017000, 0x02402000,
        0x00017200, 0x02402400, 0x0001b000, 0x0242d800, 0x0001b200, 0x0242dc00, 0x0001b400, 0x0242e000,
        0x0001b600, 0x0242e400, 0x0001bc00, 0x02433c00, 0x000001c0, 0x02409000, 0x02458400, 0x07e00300,
        0x00007000, 0x00c00000, 0x0240b800, 0x07c00000, 0x12400000, 0x00000580, 0x02409400, 0x000005a0,
        0x00b80000, 0x0240c400, 0x02404000, 0x00017400, 0x02401400, 0x00001260, 0x0000a000, 0x0001c000,
        0x02402c00, 0x004c0000, 0x02404800, 0x00000f20, 0x0240a400, 0x00011400, 0x02456800, 0x0001a000,
        0x02408800, 0x00016000, 0x00016200, 0x0001ce00, 0x00dc0000, 0x00e00000, 0x00e40000, 0x00000014,
        0x00000d20, 0x00010400, 0x0241b000, 0x04040000, 0x000010a0, 0x0240a000, 0x02411800, 0x068c0000,
        0x20000000, 0x00bc0000, 0x00016800, 0x00016a00, 0x02401000, 0x03440000, 0x00000ea0, 0x00500000,
        0x02420000, 0x00016600, 0x02400c00, 0x00014000, 0x002d6000, 0x02425800, 0x00007800, 0x00007e00,
        0x02458000, 0x07e00200, 0x02403000,
    ]
    for base in other_bases:
        for r in ranges:
            if r["abs_lo"] <= base <= r["abs_hi"]:
                problems.append(f"non-DMU IP segment base 0x{base:x} falls inside allowed range "
                                f"{r['name']} 0x{r['abs_lo']:x}-0x{r['abs_hi']:x}")
    # 4. every register src/dcn41 can address is covered (otherwise the layer would refuse itself)
    if REGS_H.exists():
        txt = REGS_H.read_text()
        bases = {m.group(1): int(m.group(2))
                 for m in re.finditer(r'^#define\s+(DCN41_\w+)_BASE_IDX\s+(\d+)u\s*$', txt, re.M)}
        n = 0
        for m in re.finditer(r'^/\* ((?:reg\w+ 0x[0-9a-f]+(?:, )?)+); BASE_IDX (\d) \*/$', txt, re.M):
            b = int(m.group(2))
            for rm in re.finditer(r'reg\w+ (0x[0-9a-f]+)', m.group(1)):
                a = SEGS[b] + int(rm.group(1), 16)
                n += 1
                if not any(r["abs_lo"] <= a <= r["abs_hi"] for r in ranges):
                    problems.append(f"dcn41_regs.h register at absolute 0x{a:x} (BASE_IDX {b}) is NOT "
                                    f"inside any allowed range")
        if n < 100:
            problems.append(f"only {n} dcn41_regs.h registers checked for coverage - the parse is wrong")
        coverage = n
    else:
        coverage = 0
    return problems, coverage


def render(ranges, coverage):
    total = sum(r["abs_hi"] - r["abs_lo"] + 1 for r in ranges)
    L = []
    A = L.append
    A("/* dcn41_allow_ranges.h - GENERATED by tools/dcn41/gen_allow.py; do not edit (regenerate, then --check).")
    A(" *")
    A(" * The DCN 4.1 DISPLAY write allowlist, as absolute BAR5 dword ranges. Absolute = seg[BASE_IDX] + offset")
    A(" * with the DMU segment bases this card's own IP discovery table reports:")
    A(" *   " + " ".join(f"{s:#010x}" for s in SEGS))
    A(" * (notes/logs/runs/adopt1/driverlog-post.txt:1371, hwId 271 inst 0 v4.1.0). Offsets and BASE_IDX come")
    A(" * from Linux 238650ef6c7c dcn_4_1_0_offset.h; offsets inside one segment are merged into a range when")
    A(f" * they are at most {MERGE_GAP:#x} dwords apart, so a range is a stretch of the display block, not a segment.")
    A(" *")
    A(" * Segments 0 and 4 are DENIED and appear nowhere below: segment 0 (base 0x12) holds only Azalia audio")
    A(" * registers and its absolute span 0x12..0x5f overlaps NBIO's indirect SMN windows at 0x14/0x15 and")
    A(" * 0x20/0x21, which reach the SMU, SMUIO, THM and the SPI ROM; segment 4 (0x2403c00) addresses no DCN")
    A(" * register and lies outside BAR5 entirely.")
    A(" *")
    A(f" * {len(ranges)} ranges, {total} dwords ({total * 4} bytes) writable in total, out of a {BAR5_DWORDS}-dword BAR5.")
    A(f" * Coverage check: all {coverage} register addresses src/dcn41/dcn41_regs.h can form fall inside a range.")
    A(" */")
    A("#ifndef N48_DCN41_ALLOW_RANGES_H")
    A("#define N48_DCN41_ALLOW_RANGES_H")
    A("")
    A("#include <stdint.h>")
    A("")
    A("struct dcn41_allow_range {")
    A("    uint32_t abs_lo;      /* first writable absolute BAR5 dword, inclusive */")
    A("    uint32_t abs_hi;      /* last writable absolute BAR5 dword, inclusive  */")
    A("    uint8_t base_idx;     /* DCN segment this range came from              */")
    A("    const char *name;     /* the hardware block(s) it covers               */")
    A("};")
    A("")
    A("struct dcn41_deny_range {")
    A("    uint32_t abs_lo;")
    A("    uint32_t abs_hi;")
    A("    const char *why;")
    A("};")
    A("")
    A(f"#define DCN41_ALLOW_RANGE_COUNT {len(ranges)}u")
    A(f"#define DCN41_ALLOW_TOTAL_DWORDS {total}u")
    A(f"#define DCN41_ALLOW_BAR5_DWORDS {BAR5_DWORDS}u")
    A("")
    A("/* Checked FIRST, before the allow table: indirect-access windows. A write here selects a target")
    A(" * anywhere in SMN or VRAM and the next write goes there, which would defeat an address allowlist. */")
    A(f"#define DCN41_HARD_DENY_COUNT {len(HARD_DENY)}u")
    A("static const struct dcn41_deny_range dcn41_hard_deny[DCN41_HARD_DENY_COUNT] = {")
    for lo, hi, why in HARD_DENY:
        A(f'    {{ {lo:#010x}u, {hi:#010x}u, "{why}" }},')
    A("};")
    A("")
    A("/* Checked SECOND. None of these intersects an allowed range (gen_allow.py asserts it); they are named")
    A(" * so a refusal says WHICH block was aimed at, and so the host test has real addresses to fire at. */")
    A(f"#define DCN41_DENY_BLOCK_COUNT {len(DENY_BLOCKS)}u")
    A("static const struct dcn41_deny_range dcn41_deny_blocks[DCN41_DENY_BLOCK_COUNT] = {")
    for lo, hi, why in DENY_BLOCKS:
        A(f'    {{ {lo:#010x}u, {hi:#010x}u, "{why}" }},')
    A("};")
    A("")
    A("/* Checked LAST, and only these make a write legal. Sorted by abs_lo, disjoint. */")
    A("static const struct dcn41_allow_range dcn41_allow_ranges[DCN41_ALLOW_RANGE_COUNT] = {")
    for r in ranges:
        A(f'    {{ {r["abs_lo"]:#010x}u, {r["abs_hi"]:#010x}u, {r["base"]}u, "{r["name"]}" }},'
          f'  /* seg{r["base"]} + {r["off_lo"]:#07x}..{r["off_hi"]:#07x}, {r["nreg"]} regs in {r["nfam"]} block(s) */')
    A("};")
    A("")
    A("/* The segment bases the table above was generated for. dcn41_allow_init refuses any other set:")
    A(" * an allowlist of absolute addresses means nothing if the bases moved. */")
    A("static const uint32_t dcn41_allow_expected_segs[5] = {")
    A("    " + ", ".join(f"{s:#010x}u" for s in SEGS))
    A("};")
    A("")
    A("#endif /* N48_DCN41_ALLOW_RANGES_H */")
    return "\n".join(L) + "\n"


def main():
    check = "--check" in sys.argv
    ranges = build_ranges()
    problems, coverage = audit(ranges)
    if problems:
        for p in problems:
            print("gen_allow: PROBLEM:", p)
        raise SystemExit(f"gen_allow: {len(problems)} problem(s) - refusing to write the allowlist")
    text = render(ranges, coverage)
    total = sum(r["abs_hi"] - r["abs_lo"] + 1 for r in ranges)
    if check:
        cur = OUT.read_text() if OUT.exists() else ""
        if cur != text:
            if cur:
                d = subprocess.run(["/usr/bin/diff", "-u", str(OUT), "-"], input=text,
                                   capture_output=True, text=True)
                print(d.stdout[:4000])
            raise SystemExit(f"gen_allow --check: {OUT} is NOT a fresh generation")
        print(f"gen_allow --check: OK - {len(ranges)} ranges, {total} writable dwords "
              f"({100.0*total/BAR5_DWORDS:.1f}% of BAR5), {coverage} dcn41_regs.h addresses all covered, "
              f"0 intersections with the denied blocks")
        return 0
    OUT.write_text(text)
    print(f"gen_allow: wrote {OUT} - {len(ranges)} ranges, {total} writable dwords "
          f"({100.0*total/BAR5_DWORDS:.1f}% of BAR5), {coverage} dcn41_regs.h addresses all covered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
