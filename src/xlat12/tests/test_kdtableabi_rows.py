#!/usr/bin/env python3
"""build 0.0.470 (notes/design/NO-SAMPLER-CLASS10.md section 5, test N5) - EVERY kDTableAbi ROW AGAINST ITS OWN
PROGRAM'S ABI JSON, and every such program's xlat12_abi_ptrs.h row against the same JSON.

kDTableAbi (src/xlat12/xlat12_ib.c) is hand-transcribed. A no-sampler row that is wrong about its program (the program
really does sample) would leave Apple's S# untranslated and unnamed - the design's risk R7, a fail-OPEN guarded only by
this check. So every row is parsed out of the C source (never re-typed here) and compared with the `user_data` array of
the program's own .abi.json (re/pc-26.6.2/xlat/windowserver{,-r2}/work/<LABEL>/, re/graphics/g2/work/):

  class 19 (descriptor table)     <-> table
  class 1  (texture index)        <-> tex[:ntex]            (as a set; a class-10 row has none: tex = 0xff x3)
  class 2  (sampler index)        <-> samp                  (none <-> 0xff)
  class 11 (sampler-index table)  <-> samptbl, nsamp        (none <-> 0xff; nsamp = len(sampler_table.samplers))
  class 10 (texture-index table)  <-> textbl1 - 1, tent     (none <-> 0; tent = texture_table.textures' entry_offset,
                                                             in order; ntex = their count)
and the ABI-pointer row (same ndw/fnv, stage 0) must list exactly the first SGPR of every 64-bit POINTER entry
(classes 19, 3, 10, 11). ndw/fnv are recomputed from the program's own object (.pal.o, or g2's .o) and must
equal the row's. Planted breaks at the end: each mutation of a real row must be caught.
"""
import json
import pathlib
import re
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
XLAT = HERE.parent
ROOT = XLAT.parent.parent
IB_C = XLAT / "xlat12_ib.c"
# build 0.0.513: kDTableAbi's rows live in the GENERATED xlat12_dtable_rows.inc, which xlat12_ib.c
# #includes as the array's whole body; every read below splices it back in where the compiler does.
INC = XLAT / "xlat12_dtable_rows.inc"
DT_INC = '#include "xlat12_dtable_rows.inc"'
ABI_H = XLAT / "xlat12_abi_ptrs.h"
OBJCOPY = "/opt/homebrew/opt/llvm/bin/llvm-objcopy"

ROW_RE = re.compile(r'\{\s*(\d+)u,\s*0x([0-9a-f]+)u,\s*(\d+)u,\s*(\d+)u,\s*\{([^}]*)\},\s*(0x[0-9a-f]+|\d+)u,\s*'
                    r'(0x[0-9a-f]+|\d+)u,\s*(\d+)u,\s*(\d+)u,\s*\{([^}]*)\}\s*\},\s*/\*\s*([A-Za-z0-9_]+):')
ABI_RE = re.compile(r'\{\s*"([^"]*)",\s*(\d+)u,\s*0x([0-9a-fA-F]+)u,\s*(\d+)u,\s*(\d+)u,\s*\{([^}]*)\}\s*\}')

FAILS = []
# see the check that reads this, in main(). build 0.0.507: EMPTY - U and Y, the two rows 0.0.445 left
# without an ABI-pointer row (the gap that refused frame a as consumer-inputs-unproven), now have one and are checked
# like every other row. A name added back here is a program the D4' read-set path must decline: say why beside it.
NO_ABI_ROW = set()
# build 0.0.507: the two ABI-pointer rows this build adds, with the identity each must carry (from its own .pal.o)
NEW_507 = {"ws_U_TvcmXh_Isrc": (192, 0x92C6AE13), "ws_Y_TkfhBvcmXh_Isrc": (372, 0x5276813B)}
RUN = [0]
# build 0.0.491: the four rows this build adds (Z direct, AO direct, AN class 11, AF direct with an in-shader S#)
NEW_491 = ("ws_Z_TimgXh_Isrc", "ws_AO_TmuaXh_IsrcCcl_Icir", "ws_AN_TmuaXh_Isrc_Isrc", "ws_AF_variable_blur_downsample_frag_lph")


def check(what, ok):
    RUN[0] += 1
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        FAILS.append(what)


def ints(csv):
    return [int(x.strip().rstrip("u"), 0) for x in csv.split(",") if x.strip()]


def ib_text():
    """xlat12_ib.c with its kDTableAbi #include line replaced by the .inc's rows (the text the compiler sees)."""
    text = IB_C.read_text()
    if DT_INC in text.split("\n"):
        text = text.replace(DT_INC, INC.read_text(), 1)
    return text


def load_rows(text):
    body = text[text.index("static const DTableAbi kDTableAbi[] = {"):text.index("#define D_TBL_ABIS")]
    rows = []
    for m in ROW_RE.finditer(body):
        ndw, fnv, table, ntex, tex, samp, samptbl, nsamp, textbl1, tent, name = m.groups()
        rows.append({"name": name, "ndw": int(ndw), "fnv": int(fnv, 16), "table": int(table), "ntex": int(ntex),
                     "tex": ints(tex), "samp": int(samp, 0), "samptbl": int(samptbl, 0), "nsamp": int(nsamp),
                     "textbl1": int(textbl1), "tent": ints(tent)})
    return rows


def load_abi_rows(text):
    body = text[text.index("kXlat12AbiPtrs[] = {"):text.index("#define XLAT12_ABI_PTR_ROWS")]
    out = {}
    for m in ABI_RE.finditer(body):
        _nm, ndw, fnv, stage, nptr, slots = m.groups()
        out[(int(ndw), int(fnv, 16), int(stage))] = ints(slots)[:int(nptr)]
    return out


def program_files(name, ident=None):
    """(abi.json, object, kind) for a kDTableAbi row name: ws_<LABEL>_<fn> -> windowserver-r2/work/<LABEL>/ (then the
    round-1 windowserver/work/<LABEL>/), UberCompositeFragment -> g2's work dir. build 0.0.513: a row the generator
    added is named auto_<key>_<fn> (tools/auto-ws/emit_ws.py); it resolves to the windowserver{-r2}/work/*/<fn> program
    whose OWN object has the row's (ndw, fnv) - found by identity, never by a hand-typed label."""
    ma = re.match(r"auto_[0-9a-f]{16}_(.+)$", name)
    if ma and ident:
        tmp = XLAT / "build" / "n5_find.bin"
        (XLAT / "build").mkdir(exist_ok=True)
        for base in ("re/pc-26.6.2/xlat/windowserver-r2/work", "re/pc-26.6.2/xlat/windowserver/work"):
            for aj in sorted((ROOT / base).glob(f"*/{ma[1]}.abi.json")):
                obj = aj.with_name(f"{ma[1]}.pal.o")
                if obj.exists() and code_id(obj, "pal", tmp) == ident:
                    return aj, obj, "pal"
        return None, None, None
    if name == "UberCompositeFragment":
        d = ROOT / "re/graphics/g2/work"
        return d / "UberCompositeFragment.abi.json", d / "UberCompositeFragment.o", "pal"
    m = re.match(r"ws_([A-Z]+)_(.+)$", name)
    if not m:
        return None, None, None
    label, fn = m.groups()
    for base in ("re/pc-26.6.2/xlat/windowserver-r2/work", "re/pc-26.6.2/xlat/windowserver/work"):
        d = ROOT / base / label
        if (d / f"{fn}.abi.json").exists():
            return d / f"{fn}.abi.json", d / f"{fn}.pal.o", "pal"
    return None, None, None


def fnv32(dw):
    h = 2166136261
    for x in dw:
        h ^= x
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def code_id(obj, kind, tmp):
    if kind == "pal":
        subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.text", str(obj), str(tmp)], check=True)
        isa = tmp.read_bytes()
    else:
        isa = obj.read_bytes()
    dw = list(struct.unpack_from(f"<{len(isa) // 4}I", isa))
    while dw and dw[-1] == 0xBF9F0000:
        dw.pop()
    if not dw or dw[-1] != 0xBFB00000:
        return None, None
    return len(dw), fnv32(dw)


def expected(abi):
    """What a correct row says, from the JSON alone."""
    ud = abi.get("user_data") or []
    first = {}
    for u in ud:
        c, sg = u.get("class"), u.get("sgprs") or []
        if isinstance(c, str):
            c = int(c, 16)
        if sg:
            first.setdefault(c, []).append(sg[0])
    e = {"table": (first.get(19) or [None])[0], "tex": sorted(first.get(1, [])),
         "samp": (first.get(2) or [0xFF])[0], "samptbl": (first.get(11) or [0xFF])[0],
         "textbl1": (first.get(10) or [-1])[0] + 1,
         "nsamp": len((abi.get("sampler_table") or {}).get("samplers") or []) if 11 in first else 0,
         "tent": [t["entry_offset"] for t in (abi.get("texture_table") or {}).get("textures") or []] if 10 in first else [],
         "ptrs": sorted(s for c in (19, 3, 10, 11) for s in first.get(c, []))}
    return e


def row_problems(r, e):
    """Every disagreement between one parsed row and the JSON's expectation (empty list = the row is right)."""
    p = []
    if r["table"] != e["table"]:
        p.append(f"table {r['table']} vs JSON {e['table']}")
    if r["textbl1"]:
        if r["tex"][:3] != [0xFF, 0xFF, 0xFF]:
            p.append(f"class-10 row tex {r['tex']} (must be 0xff x3)")
        if r["textbl1"] != e["textbl1"]:
            p.append(f"textbl1 {r['textbl1']} vs JSON class-10 slot + 1 {e['textbl1']}")
        if r["tent"][:r["ntex"]] != e["tent"] or r["ntex"] != len(e["tent"]):
            p.append(f"tent {r['tent'][:r['ntex']]} ntex {r['ntex']} vs JSON texture_table {e['tent']}")
        if e["tex"]:
            p.append(f"class-10 row but the JSON also has class-1 words {e['tex']}")
    else:
        if e["textbl1"]:
            p.append(f"the JSON has a class-10 word at s{e['textbl1'] - 1} but the row has textbl1 0")
        if sorted(r["tex"][:r["ntex"]]) != e["tex"]:
            p.append(f"tex {r['tex'][:r['ntex']]} vs JSON class-1 {e['tex']}")
        if any(r["tent"]):
            p.append(f"tent {r['tent']} on a non-class-10 row")
    if r["samp"] != e["samp"]:
        p.append(f"samp {r['samp']:#x} vs JSON class-2 {e['samp']:#x}")
    if r["samptbl"] != e["samptbl"]:
        p.append(f"samptbl {r['samptbl']:#x} vs JSON class-11 {e['samptbl']:#x}")
    if r["nsamp"] != e["nsamp"]:
        p.append(f"nsamp {r['nsamp']} vs JSON sampler_table {e['nsamp']}")
    return p


# build 0.0.504: PROGRAM J (ws_J_VfxXghb, VERTEX). It binds no class-19 table, so it has NO
# kDTableAbi row; its rows are the identity (xlat12_shader_ids.h), desc (xlat12_shader_desc.h), read-set
# (xlat12_readset.h) and ABI-pointer (xlat12_abi_ptrs.h) rows, each checked here against its own ABI JSON and object.
J_NAME, J_DIR = "ws_J_VfxXghb", ROOT / "re/pc-26.6.2/xlat/windowserver-r2/work/J"
IDS_H, DESC_H, RS_H = XLAT / "xlat12_shader_ids.h", XLAT / "xlat12_shader_desc.h", XLAT / "xlat12_readset.h"


def j_rows():
    """J's four rows, parsed from the committed headers (never re-typed here)."""
    t = IDS_H.read_text()
    m = re.search(r'\{ "ws_J_VfxXghb", (\d+)u, (\d+)u, 0x([0-9a-f]+)u, \{([^}]*)\}, \{([^}]*)\}, (\d+)u, (\d+)u, (\d+)u, '
                  r'(\d+)u \}', t)
    idr = None if not m else {"stage": int(m[1]), "ndw": int(m[2]), "fnv": int(m[3], 16), "head": ints(m[4]),
                              "w": ints(m[5]), "io": int(m[6]), "slots": [int(m[7]), int(m[8]), int(m[9])]}
    m = re.search(r'\{ (\d+)u, (\d+)u, 0x([0-9a-f]+)u, (\d+)u, (\d+)u, (\d+)u, (\d+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u \},'
                  r'\s*/\* \w+\s+ws_J_VfxXghb \*/', DESC_H.read_text())
    dr = None if not m else {"stage": int(m[1]), "ndw": int(m[2]), "fnv": int(m[3], 16), "cls": int(m[4]),
                             "n_image": int(m[5]), "n_buffer": int(m[6]), "n_ring": int(m[7]),
                             "inl": [int(m[8], 16), int(m[9], 16)]}
    m = re.search(r'\{ "ws_J_VfxXghb", (\d+)u, (\d+)u, 0x([0-9a-f]+)u, (\d+)u,\s*\{([^}]*)\}, \{([^}]*)\}, \{([^}]*)\},'
                  r'\s*0x([0-9a-f]+)u, 0x([0-9a-f]+)u, (\d+)u, (\d+)u, (\d+)u \}', RS_H.read_text())
    rr = None if not m else {"stage": int(m[1]), "ndw": int(m[2]), "fnv": int(m[3], 16), "nptr": int(m[4]),
                             "slot": ints(m[5]), "vector": ints(m[6]), "desc_table": int(m[10]), "pd1": int(m[11]),
                             "pii": int(m[12])}
    return idr, dr, rr


def j_expected(tmp):
    abi = json.loads((J_DIR / "VfxXghb.abi.json").read_text())
    ndw, fnv = code_id(J_DIR / "VfxXghb.pal.o", "pal", tmp)
    subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.text", str(J_DIR / "VfxXghb.pal.o"), str(tmp)], check=True)
    head = list(struct.unpack_from("<4I", tmp.read_bytes()))
    reg = {k.upper(): int(v, 16) for k, v in abi["rsrc"]["registers"].items()}
    ptr_ud = sorted(u["user_data_index"][0] for u in abi["user_data"]
                    if u.get("class") in (19, 3, 10, 11) and u.get("sgprs"))
    return {"stage": 1 if abi["stage"] == "vertex" else 0, "ndw": ndw, "fnv": fnv, "head": head,
            "w": [reg["0X0B228"], reg["0X0B22C"], 0, 0], "io": len(abi.get("ring_params") or []),
            "ptrs": ptr_ud, "classes": sorted({u.get("class") for u in abi["user_data"]})}


def j_problems(idr, dr, rr, ap, e):
    """Every disagreement between J's four rows and its JSON/object (empty = right)."""
    p = []
    if idr is None or dr is None or rr is None or ap is None:
        return [f"row missing: ids {idr is not None} desc {dr is not None} readset {rr is not None} abi {ap is not None}"]
    for nm, r in (("ids", idr), ("desc", dr), ("readset", rr)):
        if (r["stage"], r["ndw"], r["fnv"]) != (e["stage"], e["ndw"], e["fnv"]):
            p.append(f"{nm} identity {(r['stage'], r['ndw'], hex(r['fnv']))} vs object {(e['stage'], e['ndw'], hex(e['fnv']))}")
    if idr["head"] != e["head"]:
        p.append(f"ids head {idr['head']} vs object {e['head']}")
    if idr["w"] != e["w"]:
        p.append(f"ids w {idr['w']} vs JSON registers {e['w']}")
    if idr["io"] != e["io"]:
        p.append(f"ids io {idr['io']} vs JSON ring_params {e['io']}")
    if idr["slots"] != [255, 255, 255]:
        p.append(f"ids sampling slots {idr['slots']} on a vertex program with no table")
    if (dr["cls"], dr["n_image"], dr["inl"]) != (2, 0, [0xFF, 0xFF]):
        p.append(f"desc cls {dr['cls']} n_image {dr['n_image']} inl {dr['inl']} (want RING 2, 0 images, no inline)")
    if sorted(ap) != e["ptrs"]:
        p.append(f"abi_ptrs {sorted(ap)} vs JSON pointer user_data_index {e['ptrs']}")
    if rr["slot"][:rr["nptr"]] != e["ptrs"] or rr["nptr"] != len(e["ptrs"]):
        p.append(f"readset slots {rr['slot'][:rr['nptr']]} nptr {rr['nptr']} vs JSON {e['ptrs']}")
    if (rr["pd1"], rr["pii"], rr["desc_table"]) != (1, 1, 0):
        p.append(f"readset pd1 {rr['pd1']} pii {rr['pii']} desc_table {rr['desc_table']} (want 1 1 0)")
    return p


def check_j(abi_rows, names, tmp):
    e = j_expected(tmp)
    idr, dr, rr = j_rows()
    ap = abi_rows.get((e["ndw"], e["fnv"], 1))
    check(f"0.0.504: J's object is 291 dw / 0xfa0982ef (got {e['ndw']}/{e['fnv']:#010x})", (e["ndw"], e["fnv"]) == (291, 0xFA0982EF))
    check(f"0.0.504: J binds no table/texture/sampler (JSON classes {e['classes']} = 3 and 7 only): no kDTableAbi row",
          e["classes"] == [3, 7] and J_NAME not in names)
    probs = j_problems(idr, dr, rr, ap, e)
    check(f"0.0.504: J's ids/desc/readset/abi_ptrs rows agree with its JSON and object" + (f" - {probs}" if probs else ""),
          not probs)
    check(f"0.0.504: J's readset slots equal its ABI-pointer row {ap} (5 pointers)", rr is not None and ap is not None
          and rr["slot"][:rr["nptr"]] == sorted(ap) and rr["nptr"] == 5)
    if probs or idr is None:
        return
    # PLANTED BREAKS: one wrong field per row kind; each must be caught against the SAME JSON/object
    plants = [
        ("J ids: fnv off by one", "ids", {"fnv": idr["fnv"] ^ 1}),
        ("J ids: RSRC1 word from another program (0x020f0000)", "ids", {"w": [0x020F0000, 0, 0, 0]}),
        ("J ids: io 5 (the un-laid-out build's parameter count)", "ids", {"io": 5}),
        ("J desc: IMAGE instead of RING", "desc", {"cls": 3}),
        ("J desc: stage fragment", "desc", {"stage": 0}),
        ("J readset: the fifth pointer (s14:s15, slot 6) dropped", "rs", {"slot": [4, 8, 10, 12, 0], "nptr": 4}),
        ("J readset: pd1 0 (the pre-0.0.504 row)", "rs", {"pd1": 0}),
        ("J readset: raw SGPR s12 instead of slot 4 (D4-8)", "rs", {"slot": [12, 6, 8, 10, 12]}),
    ]
    for what, kind, mut in plants:
        i2, d2, r2 = dict(idr), dict(dr), dict(rr)
        {"ids": i2, "desc": d2, "rs": r2}[kind].update(mut)
        check(f"BREAK-check: {what} - caught", bool(j_problems(i2, d2, r2, ap, e)))
    check("BREAK-check: J abi_ptrs without slot 6 (s14:s15) - caught",
          bool(j_problems(idr, dr, rr, [s for s in ap if s != 6], e)))


def main():
    text = ib_text()
    rows = load_rows(text)
    ndecl = len(re.findall(r"^#define D_TBL_ABI_\w+_INDEX", text, re.M))
    abi_rows = load_abi_rows(ABI_H.read_text())
    print(f"test_kdtableabi_rows (0.0.470 N5): {len(rows)} kDTableAbi rows parsed")
    # build 0.0.484: 34 - glass BD and BE (GLASS.md Q4) join the 32 rows 0.0.470 left.
    # build 0.0.491: 38 - Z, AO, AN, AF (: identity rows with a class-19 table and no row) join them.
    # build 0.0.513: 39 - the generator's one ADDED row, ws_M_TextureCopy (42/0x1ddbfcad), appended after main's 38.
    check(f"every kDTableAbi row parses in the grown shape (39 rows, the table's own count)", len(rows) == 39)
    check("0.0.513: the rows come from the generated xlat12_dtable_rows.inc (xlat12_ib.c #includes it as the whole body)",
          DT_INC in IB_C.read_text().split("\n") and INC.exists())
    m513 = [r for r in rows if (r["ndw"], r["fnv"]) == (42, 0x1DDBFCAD)]
    check("0.0.513: M (42/0x1ddbfcad) has exactly one row, the LAST (index 39, so D_TBL_ABI_M_INDEX 39u and every earlier "
          "index keep their meaning)", len(m513) == 1 and rows[-1] is m513[0])
    check("0.0.513: M is a NO-SAMPLER shape (samp 0xff, samptbl 0xff, textbl1 0: switch 51 gates it) with table s0, one "
          "texture s4", bool(m513) and m513[0]["samp"] == 0xFF and m513[0]["samptbl"] == 0xFF and m513[0]["textbl1"] == 0
          and m513[0]["table"] == 0 and m513[0]["ntex"] == 1 and m513[0]["tex"][0] == 4)
    names = {r["name"] for r in rows}
    for nm in NEW_491:
        check(f"0.0.491: {nm} has a kDTableAbi row (and is re-derived below like every other)", nm in names)
    check(f"every gated-index macro names a parsed row ({ndecl} macros, {len(rows)} rows)", ndecl <= len(rows) and ndecl == len(rows) - 2)
    (XLAT / "build").mkdir(exist_ok=True)
    tmp = XLAT / "build" / "n5_text.bin"
    exp = {}
    for r in rows:
        aj, obj, kind = program_files(r["name"], (r["ndw"], r["fnv"]))
        check(f"{r['name']}: its ABI JSON exists ({aj})", aj is not None and aj.exists())
        if aj is None or not aj.exists():
            continue
        abi = json.loads(aj.read_text())
        e = expected(abi)
        exp[r["name"]] = e
        probs = row_problems(r, e)
        check(f"{r['name']}: row agrees with its JSON user_data" + (f" - {probs}" if probs else ""), not probs)
        ndw, fnv = code_id(obj, kind, tmp)
        check(f"{r['name']}: ndw/fnv {r['ndw']}/{r['fnv']:#010x} = its object's own ({ndw}/{fnv:#010x})" if ndw else
              f"{r['name']}: object ends in s_endpgm", ndw == r["ndw"] and fnv == r["fnv"])
        ap = abi_rows.get((r["ndw"], r["fnv"], 0))
        if r["name"] in NO_ABI_ROW:
            # PRE-EXISTING (0.0.445), found by this check: U and Y have NO xlat12_abi_ptrs.h row. That is UNKNOWN, which
            # d_table_desc exports as in_ptr_known 0 and every consumer refuses (fail-CLOSED) - not a wrong list. Named
            # here so the exemption is visible, and pinned: the day a row appears it must be checked like every other.
            check(f"{r['name']}: KNOWN GAP - no ABI-pointer row (fail-closed: in_ptr_known 0); still absent", ap is None)
        elif r["name"] in NEW_507:
            # build 0.0.507: the row exists, at exactly the object's identity, and lists exactly the JSON's pointers
            check(f"0.0.507: {r['name']}: ABI-pointer row present at {NEW_507[r['name']][0]}/{NEW_507[r['name']][1]:#010x} "
                  f"and lists exactly the JSON's pointers {e['ptrs']} (row {ap})",
                  (r["ndw"], r["fnv"]) == NEW_507[r["name"]] and ap is not None and sorted(ap) == e["ptrs"]
                  and e["ptrs"] == [0, 4, 6])
        else:
            check(f"{r['name']}: its xlat12_abi_ptrs.h row lists exactly the JSON's pointers {e['ptrs']} (row {ap})",
                  ap is not None and sorted(ap) == e["ptrs"])
        if r["textbl1"]:
            check(f"{r['name']}: class 10 - its ABI row declares the entry-table slot s{r['textbl1'] - 1}",
                  ap is not None and (r["textbl1"] - 1) in ap)
        if r["samptbl"] != 0xFF:
            check(f"{r['name']}: class 11 - its ABI row declares the entry-table slot s{r['samptbl']}",
                  ap is not None and r["samptbl"] in ap)

    # PLANTED BREAKS: each mutation of a REAL row must be caught by row_problems against the SAME JSON
    by = {r["name"]: r for r in rows}
    plants = [
        ("T: a no-sampler row given a sampler (samp 12)", "ws_T_Tc3sXhu_Idst", {"samp": 12}),
        ("AS: a sampling row misread as no-sampler (samp 0xff) - R7, the fail-open", "ws_AS_TbdsXh_Icir_Isrc", {"samp": 0xFF}),
        ("AZ: class-10 tex {0,0,0} (the design's B9)", "ws_AZ_TimgBdrkXhn_IsrcCrd", {"tex": [0, 0, 0]}),
        ("AZ: dense entries tent {0,8,16} (the design's B8/B5 at the row)", "ws_AZ_TimgBdrkXhn_IsrcCrd", {"tent": [0, 8, 16]}),
        ("AZ: textbl slot moved (textbl1 7)", "ws_AZ_TimgBdrkXhn_IsrcCrd", {"textbl1": 7}),
        ("BA: nsamp 1", "ws_BA_TdfgXh_Isrc", {"nsamp": 1}),
        ("BC: tex s10 instead of s8", "ws_BC_TimgXh_IsrcCcl", {"tex": [10, 0, 0]}),
        # build 0.0.484 (GLASS.md Q4): the glass rows' two failure modes that matter - the second texture slot moved
        # onto the u pointer (s8), and a sampler invented for a program whose every sampler is in-shader
        ("BD: second texture slot s8 instead of s6", "ws_BD_glass_background_lph", {"tex": [4, 8, 0]}),
        ("BE: second texture slot s8 instead of s6", "ws_BE_glass_background_lph", {"tex": [4, 8, 0]}),
        ("BD: one texture only (ntex 1)", "ws_BD_glass_background_lph", {"ntex": 1}),
        ("BD: given a sampler (samp 8)", "ws_BD_glass_background_lph", {"samp": 8}),
        # build 0.0.491: one wrong field per new row, each of the kinds this build adds
        ("Z: texture s10 instead of s8 (the sampler slot)", "ws_Z_TimgXh_Isrc", {"tex": [10, 0, 0]}),
        ("Z: sampler s8 instead of s10", "ws_Z_TimgXh_Isrc", {"samp": 8}),
        ("AO: misread as no-sampler (samp 0xff) - R7", "ws_AO_TmuaXh_IsrcCcl_Icir", {"samp": 0xFF}),
        ("AN: class-11 table at s6 instead of s4", "ws_AN_TmuaXh_Isrc_Isrc", {"samptbl": 6}),
        ("AN: one sampler entry (nsamp 1)", "ws_AN_TmuaXh_Isrc_Isrc", {"nsamp": 1}),
        ("AF: sampler dropped (samp 0xff) although the JSON declares s6", "ws_AF_variable_blur_downsample_frag_lph", {"samp": 0xFF}),
        ("AF: texture s8 (the u pointer) instead of s4", "ws_AF_variable_blur_downsample_frag_lph", {"tex": [8, 0, 0]}),
    ]
    for what, name, mut in plants:
        if name not in by or name not in exp:
            check(f"plant setup: {name} present", False)
            continue
        r = dict(by[name]); r.update(mut)
        check(f"BREAK-check: {what} - caught", bool(row_problems(r, exp[name])))
    # and the ABI-pointer side: AZ's row with the class-10 slot dropped (the design's B7)
    if "ws_AZ_TimgBdrkXhn_IsrcCrd" in exp:
        r = by["ws_AZ_TimgBdrkXhn_IsrcCrd"]
        ap = [s for s in abi_rows.get((r["ndw"], r["fnv"], 0), []) if s != 4]
        check("BREAK-check: AZ's ABI row without s4 (the design's B7) no longer matches the JSON - caught",
              sorted(ap) != exp["ws_AZ_TimgBdrkXhn_IsrcCrd"]["ptrs"])
    # build 0.0.491: the two ABI-pointer rows this build adds (Z, AF) without a pointer: each must no longer match
    for nm, drop in (("ws_Z_TimgXh_Isrc", 6), ("ws_AF_variable_blur_downsample_frag_lph", 10)):
        if nm in exp:
            r = by[nm]
            ap = [s for s in abi_rows.get((r["ndw"], r["fnv"], 0), []) if s != drop]
            check(f"BREAK-check: {nm}'s ABI row without s{drop} no longer matches the JSON - caught", sorted(ap) != exp[nm]["ptrs"])
    # build 0.0.507: every kDTableAbi row now has an ABI-pointer row (the gap class found is empty)
    miss = sorted(r["name"] for r in rows if abi_rows.get((r["ndw"], r["fnv"], 0)) is None)
    check(f"0.0.507: every kDTableAbi row has an ABI-pointer row (missing: {miss})", not miss)
    check("0.0.507: NO_ABI_ROW is empty", not NO_ABI_ROW)
    # build 0.0.507 PLANTED BREAKS, per new row: one slot wrong (s6 -> s8, the texture-index word), a pointer
    # dropped (lod_bias s6), the table dropped (s0), and the whole row absent - each must fail the SAME check above.
    for nm in NEW_507:
        if nm not in exp:
            check(f"plant setup: {nm} present", False)
            continue
        r = by[nm]
        ap = abi_rows.get((r["ndw"], r["fnv"], 0)) or []
        want = exp[nm]["ptrs"]
        for what, bad in (("one slot wrong (s6 -> s8, a texture index)", [8 if s == 6 else s for s in ap]),
                          ("lod_bias pointer s6 dropped", [s for s in ap if s != 6]),
                          ("table pointer s0 dropped", [s for s in ap if s != 0]),
                          ("an extra slot s8 (an index is not a pointer)", ap + [8])):
            check(f"BREAK-check: {nm}'s ABI row with {what} no longer matches the JSON - caught", sorted(bad) != want)
        gone = {k: v for k, v in abi_rows.items() if k != (r["ndw"], r["fnv"], 0)}
        check(f"BREAK-check: {nm}'s ABI row absent - caught by the every-row check",
              nm in [x["name"] for x in rows if gone.get((x["ndw"], x["fnv"], 0)) is None])
    check_j(abi_rows, names, tmp)
    print(f"\ntest_kdtableabi_rows: {RUN[0]} checks, {len(FAILS)} failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
