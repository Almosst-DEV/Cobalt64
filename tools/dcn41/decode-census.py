#!/usr/bin/env python3
"""decode-census.py - decode a T1 census capture into named fields.

  python3 tools/dcn41/decode-census.py notes/logs/runs/<run>/passA.txt [--only OTG0,DIG1] [--nonzero]

Every field name, mask and shift comes from Linux's dcn_4_1_0_sh_mask.h (238650ef6c7c), looked up by
the census row's own register name: no bit position is typed here. A register whose name has no mask
entry in the header is printed raw and counted, so a silent lookup miss cannot masquerade as "no
fields set".
"""
import pathlib, re, sys, collections

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHMASK = ROOT / "re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_sh_mask.h"


def load_masks():
    """{REGNAME: [(field, mask, shift), ...]} from the sh_mask header."""
    masks, shifts = {}, {}
    txt = SHMASK.read_text()
    for m in re.finditer(r'^#define\s+(\w+?)__(\w+?)_MASK\s+(0x[0-9a-fA-F]+)L?\s*$', txt, re.M):
        masks[(m.group(1), m.group(2))] = int(m.group(3), 16)
    for m in re.finditer(r'^#define\s+(\w+?)__(\w+?)__SHIFT\s+(0x[0-9a-fA-F]+)L?\s*$', txt, re.M):
        shifts[(m.group(1), m.group(2))] = int(m.group(3), 16)
    out = collections.defaultdict(list)
    for (reg, fld), mask in masks.items():
        sh = shifts.get((reg, fld))
        if sh is None:
            continue
        out[reg].append((fld, mask, sh))
    for reg in out:
        out[reg].sort(key=lambda t: t[2])
    return out


def main():
    args = sys.argv[1:]
    path = None
    only, nonzero = None, False
    i = 0
    while i < len(args):
        if args[i] == "--only":
            only = [s.strip() for s in args[i + 1].split(",")]; i += 2
        elif args[i] == "--nonzero":
            nonzero = True; i += 1
        else:
            path = args[i]; i += 1
    if not path:
        raise SystemExit(__doc__)
    masks = load_masks()
    misses = []
    for line in pathlib.Path(path).read_text().splitlines():
        p = line.split()
        if len(p) < 5 or p[0] != "REG":
            continue
        abs_dw, val, _us, label = p[1], p[2], p[3], p[4]
        if val.startswith("ERR"):
            print(f"{label:52s} {abs_dw} ERROR"); continue
        v = int(val, 16)
        group, _, name = label.partition("/")
        if only and not any(name.startswith(o) for o in only):
            continue
        flds = masks.get(name)
        if flds is None:
            misses.append(name)
            print(f"{name:52s} {abs_dw} = {val}   (no mask entry in dcn_4_1_0_sh_mask.h)")
            continue
        parts = []
        for f, mask, sh in flds:
            fv = (v & mask) >> sh
            if nonzero and fv == 0:
                continue
            parts.append(f"{f}={fv}")
        print(f"{name:52s} {abs_dw} = {val}  " + " ".join(parts))
    if misses:
        print(f"\n# {len(misses)} register(s) had no mask entry: {sorted(set(misses))}")


if __name__ == "__main__":
    main()
