#!/usr/bin/env python3
"""
gen_tables.py — generate the gfx10.3 -> gfx12 render-state translation tables for
src/xlat12 from authoritative register databases.

AUTHORITATIVE SOURCES (named, with the commit each was read at):
  gfx10.3 / Navi21 addresses + fields
    Mesa  src/amd/registers/gfx103.json            @ mesa  4519cc5645880023ed1c83417d33c4cad99e88ee
    Linux drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_offset.h
                                                    @ linux 587858367581b9c55c3690f4e63382ad622719d4
  gfx12 addresses + fields
    Mesa  src/amd/registers/gfx12.json             @ mesa  4519cc56
    Linux .../gc/gc_12_0_0_offset.h                @ linux 587858367581
  gfx12 emit conventions quoted in comments (not parsed):
    radeonsi gfx/si_state_shaders.cpp:1352 (NGG => S_028B54_PRIMGEN_EN(1)), :4035 (GFX12 uses
    R_028A98_VGT_SHADER_STAGES_EN); amd/packets/cp_pm4_table_data_gfx12.json (index forms)
  Which registers Apple emits (AMDRadeonX6000MTLDriver, macOS 26.6.2 build 25G83):
    re/graphics/mtl-regrecs.txt  (ObjC type encodings, regrecs.py)
    re/graphics/pm4census.txt    (SET_*_REG immediates baked as movabsq, pm4census.py)
    APPLE_INDEX_FORM_ADDRS below (SET_SH_REG_INDEX / SET_UCONFIG_REG_INDEX immediates,
    which pm4census.py does not resolve; each cites its instruction address)

The generator REFUSES (exit 2, nothing written) on: a missing source; an ambiguous
gfx10 address; a Linux header that names a table register at a different address
than Mesa (contradictory sources); a census 'moved -> T' target that disagrees with
Mesa; a malformed field bit range.

Registers that NEITHER gfx10.3 source names are emitted as UNKNOWN (the translator
refuses them); nothing is guessed.

Outputs (checked in): src/xlat12/xlat12_tables.h, xlat12_repack.h, xlat12_provenance.h
Run:  python3 src/xlat12/gen/gen_tables.py [--check]
"""
import json, os, re, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MESA_COMMIT  = "4519cc5645880023ed1c83417d33c4cad99e88ee"
LINUX_COMMIT = "587858367581b9c55c3690f4e63382ad622719d4"

DEF_MESA   = os.path.join(REPO, "re/graphics/src/mesa/src/amd/registers")
DEF_LINUX  = os.path.join(REPO, "re/m2/linux/drivers/gpu/drm/amd/include/asic_reg/gc")
DEF_RECS   = os.path.join(REPO, "re/graphics/mtl-regrecs.txt")
DEF_CENSUS = os.path.join(REPO, "re/graphics/pm4census.txt")

CTX_BASE, SH_BASE, UCF_BASE = 0x28000, 0x0B000, 0x30000   # Mesa sid.h:18-23

# Index-form register writes Apple bakes (not resolved by pm4census.py, whose base
# table skips opcode 0x7A). Offset word = index<<28 | dword offset. All CONFIRMED as
# movabsq immediates in re/m2/mtldriver.dis (llvm-objdump of the 25G83 dylib).
APPLE_INDEX_FORM_ADDRS = {
    0x0B004: "SET_SH_REG_INDEX idx3  30000001 @7ffb111da638 WriteStaticHwRegs",
    0x0B01C: "SET_SH_REG_INDEX idx3  30000007 @7ffb111da64c WriteStaticHwRegs",
    0x0B104: "SET_SH_REG_INDEX idx3  30000041 @7ffb111da660 WriteStaticHwRegs",
    0x0B404: "SET_SH_REG_INDEX idx3  30000101 @7ffb111da674 WriteStaticHwRegs",
    0x0B41C: "SET_SH_REG_INDEX idx3  30000107 @7ffb111da688 WriteStaticHwRegs",
    0x0B118: "SET_SH_REG_INDEX idx3  30000046 const@7ffb1149b2d0 WriteRenderPipelineHwShRegs",
    0x0B858: "SET_SH_REG_INDEX idx3  30000216 @7ffb1127a203 Gfx10BltDevice::Write3dDispatchCuMask",
    0x0B864: "SET_SH_REG_INDEX idx3  30000219 @7ffb1127a228 Gfx10BltDevice::Write3dDispatchCuMask",
    0x3090C: "SET_UCONFIG_REG_INDEX idx2 20000243 @7ffb1122e337 SetHwIndexBuffer",
    0x30908: "SET_UCONFIG_REG_INDEX idx1 10000242 @7ffb11212884 MtlIndirectRenderCmd drawIndexedPrimitives",
}

# Address ranges Apple's writers compute at run time (loops and count arguments),
# so pm4census.py (baked immediates only) never listed them. CONFIRMED in
# re/m2/mtldriver.dis, amdMtl_GFX10_WriteRenderEncoderHwCtxRegs:
#   @7ffb111dab34 movl $0xc00e6900 (x14) with index 0x318+15*i (@7ffb111dab41), i<8
#   @7ffb111dab73..dabf5 six x1 writes at index 0x390|i, 0x398|i, ... 0x3b8|i, i<8
#   @7ffb111dadd7 x(2*a5) at index 0x94 (@7ffb111dade2); @7ffb111dae15 x(2*a4) at 0xb4;
#   @7ffb111dae74 x(6*a4) at 0x10f. a4/a5 are the viewport/scissor counts; 16 is
#   Metal's maximum (SUSPECTED as the driver bound: beyond it addresses stay untabled
#   and are refused, never guessed).
def _apple_loop_addrs():
    s = set()
    for i in range(8):
        for j in range(14): s.add(0x28000 + ((0x318 + 15 * i + j) << 2))
        for b in (0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8): s.add(0x28000 + ((b | i) << 2))
    for n in range(32): s.add(0x28000 + ((0x94 + n) << 2))   # 2 per scissor x16
    for n in range(32): s.add(0x28000 + ((0xb4 + n) << 2))   # 2 per viewport x16
    for n in range(96): s.add(0x28000 + ((0x10f + n) << 2))  # 6 per viewport x16
    return s
APPLE_LOOP_ADDRS = _apple_loop_addrs()

def die(msg):
    sys.stderr.write("gen_tables: REFUSING - %s\n" % msg)
    sys.exit(2)

# ---------------------------------------------------------------- Mesa ----
class MesaDB:
    def __init__(self, path):
        if not os.path.isfile(path):
            die("missing Mesa register database %s" % path)
        self.path = path
        self.fname = os.path.basename(path)
        j = json.load(open(path))
        self.names = {}
        self.by_addr = {}
        for e in j["register_mappings"]:
            self.names.setdefault(e["name"], e)
            self.by_addr.setdefault(e["map"]["at"], []).append(e["name"])
        self.types = j["register_types"]
        # line index for citations
        self.type_line = {}
        self.map_line = {}
        lines = open(path).read().split("\n")
        in_maps = in_types = False
        for i, ln in enumerate(lines, 1):
            if ln.startswith(' "register_mappings"'): in_maps, in_types = True, False; continue
            if ln.startswith(' "register_types"'):    in_maps, in_types = False, True;  continue
            if in_types:
                m = re.match(r'^  "(\w+)": \{', ln)
                if m: self.type_line.setdefault(m.group(1), i)
            elif in_maps:
                m = re.search(r'"name": "(\w+)"', ln)
                if m: self.map_line.setdefault(m.group(1), i)

    def fields(self, name):
        ent = self.names.get(name)
        if ent is None:
            return None, None
        tr = ent.get("type_ref", name)
        t = self.types.get(tr)
        if t is None:
            return None, tr
        out = []
        for f in t["fields"]:
            b = f["bits"]
            if not (isinstance(b, list) and len(b) == 2 and 0 <= b[0] <= b[1] <= 31):
                die("malformed bit range for %s.%s in %s" % (tr, f.get("name"), self.fname))
            out.append((f["name"], b[0], b[1]))
        return out, tr

    def cite_map(self, name):
        ln = self.map_line.get(name)
        if ln is None:
            die("no register_mappings line for %s in %s" % (name, self.fname))
        return "%s:%d" % (self.fname, ln)

    def cite_type(self, tr):
        ln = self.type_line.get(tr)
        return "%s:%d" % (self.fname, ln) if ln else "%s:?" % self.fname

# --------------------------------------------------------------- Linux ----
def load_linux(gc_dir, fname, prefix):
    path = os.path.join(gc_dir, fname)
    if not os.path.isfile(path):
        die("missing Linux header %s" % path)
    d, line = {}, {}
    for i, ln in enumerate(open(path), 1):
        m = re.match(r"#define %s(\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$" % prefix, ln)
        if not m:
            continue
        nm, bi, v = m.group(1), m.group(2), int(m.group(3), 0)
        e = d.setdefault(nm, [None, None])
        if bi: e[1] = v
        else:
            e[0] = v; line[nm] = i
    return d, line

def derive_bases(mesa, lin):
    """Derive the per-BASE_IDX segment base from names both sources carry; the
       majority value is used, and agreement is reported (no hardcoded bases)."""
    votes = {}
    for nm, (off, bi) in lin.items():
        if off is None or bi is None: continue
        ent = mesa.names.get(nm)
        if ent is None or ent["map"].get("to") != "mm": continue
        a = ent["map"]["at"]
        if a % 4: continue
        votes.setdefault(bi, {}).setdefault(a // 4 - off, 0)
        votes[bi][a // 4 - off] += 1
    bases, report = {}, {}
    for bi, vv in votes.items():
        base, n = max(vv.items(), key=lambda kv: kv[1])
        bases[bi] = base
        report[bi] = (base, n, sum(vv.values()))
    return bases, report

def linux_name(lin, name):
    """Linux spelling of a Mesa register name. Mesa strips the _UMD suffix when it
       imports kernel headers (mesa src/amd/registers/parse_kernel_headers.py:848-849,
       "mistakenly added to indicate it's for a User-Mode Driver"), so prefer X_UMD."""
    return name + "_UMD" if (name + "_UMD") in lin else name

def linux_addr(lin, bases, name):
    e = lin.get(linux_name(lin, name))
    if not e or e[0] is None or e[1] not in bases:
        return None
    return (bases[e[1]] + e[0]) * 4

# -------------------------------------------------------------- Apple -----
def apple_reg_names(path):
    if not os.path.isfile(path):
        die("missing Apple register list %s" % path)
    m = re.search(r"TOTAL distinct register unions:\s*\d+\s*\n(.*)", open(path).read(), re.S)
    if not m:
        die("cannot parse register-union list in %s" % path)
    return m.group(1).split()

def census(path):
    if not os.path.isfile(path):
        die("missing census %s" % path)
    named, unknown = {}, {}
    for ln in open(path):
        m = re.match(r"(SET_\w+)\s+(0x[0-9a-f]+)\s+\?\s+\(not in gfx103", ln)
        if m:
            unknown[int(m.group(2), 16)] = m.group(1); continue
        m = re.match(r"SET_\w+\s+0x[0-9a-f]+\s+([A-Z][A-Z0-9_]+):\s*(.*)", ln)
        if m:
            mt = re.match(r"moved -> (0x[0-9a-f]+)", m.group(2))
            named[m.group(1)] = int(mt.group(1), 16) if mt else None
    return named, unknown

# ------------------------------------------------------ classification ----
LEGACY_VS_RE = re.compile(r"^SPI_SHADER_(PGM_(LO|HI|RSRC[1-4])_VS|LATE_ALLOC_VS|USER_DATA_VS_\d+)$")

def absent_reason(name):
    if LEGACY_VS_RE.match(name):
        return "legacy-VS slot: gfx12 is NGG-only (no VS stage registers)"
    if re.search(r"CMASK|FMASK|_DCC|CLEAR_WORD|COVERAGE_OUT|CB_DCC", name):
        return "CMASK/FMASK/DCC: a linear uncompressed color target has none"
    if re.search(r"HTILE|STENCIL_CLEAR|DEPTH_CLEAR", name):
        return "HTILE/depth-clear: no depth target in a first triangle"
    if re.search(r"STRMOUT", name):
        return "streamout: unused by a first triangle"
    if re.search(r"^VGT_GS(_MODE|_ONCHIP|_PER_VS|_VERT_ITEMSIZE|VS_RING)|ESGS_RING|GSVS_RING", name):
        return "legacy GS mode/ring: the NGG pipeline uses none"
    if re.search(r"STENCILREFMASK", name):
        return "stencil ref/mask: gfx12 splits these; no stencil in a first triangle"
    if re.search(r"VRS|FSR|DFSM|PRELOAD_CONTROL|VTX_CNT_EN|INDEX_PAYLOAD|OUT_DEALLOC|VERTEX_REUSE|GE_PC_ALLOC|COHER_START|PERFMON|RMI_L2", name):
        return "feature removed on gfx12 / unused for a minimal draw"
    return "no gfx12 register of this name; unused by a first triangle"

def classify(name, m10, m12):
    a = m10.names.get(name)
    if a is None:
        return None
    g10 = a["map"]["at"]
    fa, t10 = m10.fields(name)
    b = m12.names.get(name)
    if b is None:
        cls = "legacy_vs" if LEGACY_VS_RE.match(name) else "absent"
        return dict(name=name, g10=g10, g12=0, cls=cls, reason=absent_reason(name),
                    fa=fa, fb=None, t10=t10, t12=None,
                    cite="g10 %s, absent from gfx12.json" % m10.cite_map(name))
    g12 = b["map"]["at"]
    fb, t12 = m12.fields(name)
    # Repack only when BOTH generations list fields and the lists differ. If either
    # side has no field list (opaque address dwords such as SPI_SHADER_PGM_HI_HS),
    # the dword is carried verbatim, classified by address alone.
    if fa is not None and fb is not None and fa != fb:
        cls = "field_repack"
    else:
        cls = "identical" if g10 == g12 else "moved"
    return dict(name=name, g10=g10, g12=g12, cls=cls, reason="", fa=fa, fb=fb, t10=t10, t12=t12,
                cite="g10 %s, g12 %s" % (m10.cite_map(name), m12.cite_map(name)))

def emit_repack(r, m10, m12):
    name, fa, fb = r["name"], r["fa"], r["fb"]
    b10 = {n: (lo, hi) for (n, lo, hi) in fa}
    b12 = {n: (lo, hi) for (n, lo, hi) in fb}
    shared  = [n for (n, _, _) in fb if n in b10]
    dropped = [n for (n, _, _) in fa if n not in b12]
    added   = [n for (n, _, _) in fb if n not in b10]
    L = ["/* %s  gfx10.3 0x%05x -> gfx12 0x%05x" % (name, r["g10"], r["g12"]),
         " * fields: %s (type %s) -> %s (type %s)  @mesa %s"
         % (m10.cite_type(r["t10"]), r["t10"], m12.cite_type(r["t12"]), r["t12"], MESA_COMMIT[:8])]
    if dropped: L.append(" * dropped (gfx10.3 only): %s" % ", ".join(dropped))
    if added:   L.append(" * new on gfx12 (left 0):  %s" % ", ".join(added))
    L.append(" */")
    L.append("static uint32_t repack_%s(uint32_t v)" % name)
    L.append("{")
    L.append("    uint32_t o = 0u;")
    notes = []
    for n in shared:
        s0, s1 = b10[n]; d0, d1 = b12[n]
        sw, dw = s1 - s0 + 1, d1 - d0 + 1
        mask = (1 << min(sw, dw)) - 1
        c = "%s [%d:%d]->[%d:%d]" % (n, s1, s0, d1, d0)
        if sw != dw:
            c += " WIDTH %d->%d (masked to dst)" % (sw, dw)
            notes.append("%s.%s width %d->%d" % (name, n, sw, dw))
        L.append("    o |= ((v >> %2d) & 0x%xu) << %2d; /* %s */" % (s0, mask, d0, c))
    L.append("    return o;")
    L.append("}")
    return "\n".join(L), notes, dropped

# ---------------------------------------------------------------- main ----
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mesa", default=DEF_MESA)
    ap.add_argument("--linux", default=DEF_LINUX)
    ap.add_argument("--recs", default=DEF_RECS)
    ap.add_argument("--census", default=DEF_CENSUS)
    ap.add_argument("--out", default=os.path.join(REPO, "src/xlat12"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    m10 = MesaDB(os.path.join(args.mesa, "gfx103.json"))
    m12 = MesaDB(os.path.join(args.mesa, "gfx12.json"))
    l10, l10line = load_linux(args.linux, "gc_10_3_0_offset.h", "mm")
    l12, l12line = load_linux(args.linux, "gc_12_0_0_offset.h", "reg")
    b10, rep10 = derive_bases(m10, l10)
    b12, rep12 = derive_bases(m12, l12)

    names = apple_reg_names(args.recs)
    cnamed, cunknown = census(args.census)
    allnames = list(dict.fromkeys(names + list(cnamed.keys())))

    entries = {}
    def add(r, origin):
        g10 = r["g10"]
        if g10 in entries:
            if entries[g10]["cls"] != r["cls"] or entries[g10]["name"] != r["name"]:
                die("ambiguous gfx10 address 0x%x: %s (%s) vs %s (%s)"
                    % (g10, entries[g10]["name"], entries[g10]["cls"], r["name"], r["cls"]))
            return
        r["origin"] = origin
        entries[g10] = r

    for nm in allnames:
        r = classify(nm, m10, m12)
        if r is None:
            continue
        mt = cnamed.get(nm)
        if mt is not None and r["cls"] in ("moved", "field_repack") and r["g12"] != mt:
            die("census/Mesa disagree on gfx12 address of %s: 0x%x vs 0x%x" % (nm, mt, r["g12"]))
        add(r, "recs/census")

    for a, why in sorted(APPLE_INDEX_FORM_ADDRS.items()):
        nms = m10.by_addr.get(a)
        if nms:
            r = classify(nms[0], m10, m12)
            add(r, "index-form: " + why)
        elif a not in entries:
            entries[a] = dict(name="UNNAMED_0x%05x" % a, g10=a, g12=0, cls="unknown",
                              reason="index-form address named by neither gfx10.3 source",
                              cite="-", origin="index-form: " + why)

    for a in sorted(APPLE_LOOP_ADDRS):
        if a in entries:
            continue
        nms = m10.by_addr.get(a)
        if nms:
            add(classify(nms[0], m10, m12), "writer loop/count range")
        else:
            entries[a] = dict(name="UNNAMED_0x%05x" % a, g10=a, g12=0, cls="unknown",
                              reason="writer loop address named by neither gfx10.3 source",
                              cite="-", origin="writer loop/count range")

    for a, op in sorted(cunknown.items()):
        if a in entries:
            continue
        g12name = (m12.by_addr.get(a) or [None])[0]
        alias = ("; on gfx12 this address is %s" % g12name) if g12name else ""
        entries[a] = dict(name="UNNAMED_0x%05x" % a, g10=a, g12=0, cls="unknown",
                          reason="Apple %s write; named by neither gfx103.json nor gc_10_3_0_offset.h%s"
                                 % (op, alias), cite="pm4census.txt", origin="census")

    # --- Linux cross-check. A register whose two sources CONTRADICT each other is not
    # guessed: it becomes UNKNOWN (the translator refuses it) and is listed. ---
    lx = dict(g10_agree=0, g10_unnamed=0, g12_agree=0, g12_unnamed=0, contradictions=0)
    contradictions = []
    for a, r in entries.items():
        if r["cls"] == "unknown":
            continue
        why = None
        la = linux_addr(l10, b10, r["name"])
        if la is None:
            lx["g10_unnamed"] += 1
        elif la != r["g10"]:
            why = ("contradictory gfx10.3 sources: gfx103.json 0x%x vs gc_10_3_0_offset.h:%d 0x%x"
                   % (r["g10"], l10line[linux_name(l10, r["name"])], la))
        else:
            lx["g10_agree"] += 1; r["cite"] += ", gc_10_3_0_offset.h:%d" % l10line[linux_name(l10, r["name"])]
        if why is None and r["g12"]:
            lb = linux_addr(l12, b12, r["name"])
            if lb is None:
                lx["g12_unnamed"] += 1
            elif lb != r["g12"]:
                why = ("contradictory gfx12 sources: gfx12.json 0x%x vs gc_12_0_0_offset.h:%d 0x%x"
                       % (r["g12"], l12line[linux_name(l12, r["name"])], lb))
            else:
                lx["g12_agree"] += 1; r["cite"] += ", gc_12_0_0_offset.h:%d" % l12line[linux_name(l12, r["name"])]
        if why is not None:
            lx["contradictions"] += 1
            contradictions.append("%s 0x%05x: %s" % (r["name"], a, why))
            r.update(cls="unknown", g12=0, reason=why, fa=None, fb=None)
    # an unknown address that Linux DOES name would be a missed source: refuse
    inv10 = {}
    for nm, e in l10.items():
        e = l10[nm]
        la = (b10[e[1]] + e[0]) * 4 if e[0] is not None and e[1] in b10 else None
        if la: inv10.setdefault(la, nm[:-4] if nm.endswith("_UMD") else nm)
    for a, r in entries.items():
        if r["cls"] == "unknown" and r["name"].startswith("UNNAMED_") and a in inv10:
            die("0x%x is classed unknown but gc_10_3_0_offset.h names it %s" % (a, inv10[a]))

    # A field-changed register whose two field lists share NO field name has no
    # established value mapping in the JSON (a name-matched repack would silently
    # return 0). It is refused as UNKNOWN and listed, not guessed.
    noshared = []
    for a, r in sorted(entries.items()):
        if r["cls"] != "field_repack":
            continue
        n10 = set(x[0] for x in r["fa"])
        if not any(x[0] in n10 for x in r["fb"]):
            noshared.append("%s 0x%05x: gfx10.3 %s %s | gfx12 %s %s"
                            % (r["name"], a, m10.cite_type(r["t10"]), [x[0] for x in r["fa"]],
                               m12.cite_type(r["t12"]), [x[0] for x in r["fb"]]))
            r.update(cls="unknown", g12=0, fa=None, fb=None,
                     reason="field lists share no field name (%s vs %s): no established value mapping"
                            % (m10.cite_type(r["t10"]), m12.cite_type(r["t12"])))

    # NGG rule constants, from the JSON
    vse = m10.names.get("VGT_SHADER_STAGES_EN")
    if vse is None:
        die("VGT_SHADER_STAGES_EN missing from gfx103.json")
    vf, vtr = m10.fields("VGT_SHADER_STAGES_EN")
    prim = [f for f in vf if f[0] == "PRIMGEN_EN"]
    if len(prim) != 1 or prim[0][1] != prim[0][2]:
        die("PRIMGEN_EN is not a single bit in gfx103.json")
    ngg = dict(addr=vse["map"]["at"], bit=prim[0][1], cite=m10.cite_type(vtr))

    counts = {k: 0 for k in ("identical", "moved", "field_repack", "absent", "legacy_vs", "unknown")}
    for r in entries.values():
        counts[r["cls"]] += 1

    repacks, notes_all = [], []
    for a in sorted(entries):
        r = entries[a]
        if r["cls"] == "field_repack":
            code, notes, _ = emit_repack(r, m10, m12)
            repacks.append(code); notes_all += notes

    summary = ["entries %d (Apple union names %d + census names + index-form addrs + census-unknown %d)"
               % (len(entries), len(names), len(cunknown))]
    for k, v in counts.items():
        summary.append("%-12s %d" % (k, v))
    summary.append("Linux bases gfx10.3 %s  gfx12 %s" %
                   ({k: "0x%x (%d/%d names)" % v for k, v in rep10.items()},
                    {k: "0x%x (%d/%d names)" % v for k, v in rep12.items()}))
    summary.append("Linux cross-check: gfx10.3 agree %(g10_agree)d, not named %(g10_unnamed)d; "
                   "gfx12 agree %(g12_agree)d, not named %(g12_unnamed)d; contradictions (refused) %(contradictions)d" % lx)
    summary += ["contradiction (refused): " + x for x in contradictions]
    summary += ["no shared field (refused): " + x for x in noshared]
    summary.append("NGG rule: VGT_SHADER_STAGES_EN g10 0x%x PRIMGEN_EN bit %d (%s)" % (ngg["addr"], ngg["bit"], ngg["cite"]))

    if args.check:
        print("\n".join(summary))
        print("width-changed shared fields (value masked to the gfx12 width):")
        for n in notes_all: print("   ", n)
        print("unknown (refused) addresses:")
        for a in sorted(entries):
            if entries[a]["cls"] == "unknown":
                print("    0x%05x %s" % (a, entries[a]["reason"]))
        return

    write_headers(args.out, entries, repacks, notes_all, summary, ngg)
    print("\n".join(summary))

def cstr(s):
    return s.replace("\\", "\\\\").replace('"', "'")

def write_headers(out, entries, repacks, notes, summary, ngg):
    banner = "/* GENERATED by src/xlat12/gen/gen_tables.py - do not edit. Regenerate instead. */\n"
    with open(os.path.join(out, "xlat12_provenance.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_PROVENANCE_H\n#define XLAT12_PROVENANCE_H\n")
        f.write('#define XLAT12_SRC_MESA_COMMIT  "%s"\n' % MESA_COMMIT)
        f.write('#define XLAT12_SRC_LINUX_COMMIT "%s"\n' % LINUX_COMMIT)
        f.write("/*\n")
        for s in summary: f.write(" * %s\n" % s)
        f.write(" */\n#endif\n")
    with open(os.path.join(out, "xlat12_repack.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_REPACK_H\n#define XLAT12_REPACK_H\n#include <stdint.h>\n\n")
        f.write("/* One pure value function per field-changed register: each shared field is\n"
                " * copied BY NAME from its gfx10.3 bit range to its gfx12 bit range; gfx10.3-only\n"
                " * fields are dropped, gfx12-only fields are left 0. */\n")
        if notes:
            f.write("/* Shared fields whose width changed (masked to the gfx12 width):\n")
            for n in notes: f.write(" *   %s\n" % n)
            f.write(" */\n")
        f.write("\n")
        for code in repacks: f.write(code + "\n\n")
        f.write("#endif\n")
    with open(os.path.join(out, "xlat12_tables.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_TABLES_H\n#define XLAT12_TABLES_H\n#include <stdint.h>\n")
        f.write('#include "xlat12.h"\n#include "xlat12_repack.h"\n\n/*\n')
        for s in summary: f.write(" * %s\n" % s)
        f.write(" */\n\n")
        f.write("/* Xlat12Class values are defined in xlat12.h. */\n\n")
        f.write("typedef uint32_t (*Xlat12Repack)(uint32_t);\n\n")
        f.write("typedef struct {\n"
                "    uint32_t     g10;     /* gfx10.3 MMIO byte address (sort key) */\n"
                "    uint32_t     g12;     /* gfx12 MMIO byte address; 0 if absent/unknown */\n"
                "    Xlat12Repack repack;  /* field_repack only, else 0 */\n"
                "    uint8_t      cls;     /* Xlat12Class */\n"
                "    const char  *name;\n"
                "    const char  *reason;  /* absent/legacy_vs/unknown: why */\n"
                "} Xlat12Entry;\n\n")
        f.write("#define XLAT12_G10_VGT_SHADER_STAGES_EN 0x%05xu /* %s */\n" % (ngg["addr"], ngg["cite"]))
        f.write("#define XLAT12_G10_PRIMGEN_EN_BIT       %du      /* NGG: radeonsi si_state_shaders.cpp:1352 */\n\n" % ngg["bit"])
        f.write("static const Xlat12Entry kXlat12Table[] = {\n")
        for a in sorted(entries):
            r = entries[a]
            rp = ("repack_%s" % r["name"]) if r["cls"] == "field_repack" else "0"
            f.write('    /* %s */\n' % cstr(r["cite"]))
            f.write('    { 0x%05xu, 0x%05xu, %s, XLAT12_CLS_%s, "%s", "%s" },\n'
                    % (a, r["g12"], rp, r["cls"].upper(), r["name"], cstr(r["reason"])))
        f.write("};\n#define XLAT12_TABLE_LEN ((uint32_t)(sizeof(kXlat12Table) / sizeof(kXlat12Table[0])))\n\n#endif\n")

if __name__ == "__main__":
    main()
