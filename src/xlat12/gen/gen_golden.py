#!/usr/bin/env python3
"""
gen_golden.py - golden vectors for src/xlat12 built from Apple's REAL packets.

1. Transcribes the PM4 packets emitted by four AMDRadeonX6000MTLDriver writers
   (macOS 26.6.2 build 25G83, unslid addresses):
     amdMtl_GFX10_WriteStaticHwRegs              0x7ffb111da2fb
     amdMtl_GFX10_WriteRenderPipelineHwCtxRegs   0x7ffb111da69b
     amdMtl_GFX10_WriteRenderPipelineHwShRegs    0x7ffb111da996
     amdMtl_GFX10_WriteRenderEncoderHwCtxRegs    0x7ffb111daae6
   Every transcribed header/index is RE-VERIFIED against a fresh llvm-objdump of the
   dylib (and const-table bytes against the file), and every PKT3-shaped immediate
   in each function must be accounted for. A mismatch refuses (exit 2).
2. Builds input streams from those packets with deterministic non-trivial values.
3. Computes the expected gfx12 output with an INDEPENDENT Python oracle that reads
   Mesa gfx103.json / gfx12.json directly (it does not use the generated C tables
   or repack code).
4. Emits tests/golden_vectors.h, plus the per-record register sets (the design's
   42-register render-pipeline set and 93-register encoder set).

Run: python3 src/xlat12/gen/gen_golden.py
"""
import json, os, re, subprocess, sys, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DYLIB = os.path.join(REPO, "re/tahoe-26.6.2-x86_64/out/dylibs/AMDRadeonX6000MTLDriver")
OBJDUMP = "/opt/homebrew/opt/llvm/bin/llvm-objdump"
MESA = os.path.join(REPO, "re/graphics/src/mesa/src/amd/registers")
RECS = os.path.join(REPO, "re/graphics/mtl-regrecs.txt")
TEXT_VMADDR = 0x7ffb110d2000   # __TEXT vmaddr, fileoff 0 (otool -l)

def die(m):
    sys.stderr.write("gen_golden: REFUSING - %s\n" % m); sys.exit(2)

for p in (DYLIB, OBJDUMP, os.path.join(MESA, "gfx103.json"), RECS):
    if not os.path.exists(p): die("missing %s" % p)

CTX, SH, UCF = 0x28000, 0xB000, 0x30000
OP_CTX, OP_SH, OP_UCF, OP_SHI, OP_UCFI = 0x69, 0x76, 0x79, 0x9B, 0x7A
BASE = {OP_CTX: CTX, OP_SH: SH, OP_UCF: UCF, OP_SHI: SH, OP_UCFI: UCF}

def hdr(op, cnt, flags=0):
    return 0xC0000000 | ((cnt & 0x3FFF) << 16) | ((op & 0xFF) << 8) | (flags & 0xFF)

# ------------------------------------------------------------ transcription --
# kinds: 'mabs' movabsq imm64 = index<<32 | header ; 'movl' movl header, index 0 via
# zero-extended movq ; 'hdrimm' header immediate (movl/orl) with the index carried by
# the instruction at `ix` ; 'const' 16-byte const table [hdr, index, value, next] ;
# 'constnext' header in const table slot 3, index from movl at `ix`.
# cond: None, 'b7' (HwInfoRec+0xcc bit 7), 'nav2' (+0xcd bit 1), 'nav1' (+0xcd bit 0)
def P(kind, op, at, index, n, cond=None, ix=None, idx=None, loop=None):
    return dict(kind=kind, op=op, at=at, index=index, n=n, cond=cond, ix=ix, idx=idx, loop=loop)

STATIC = (0x7ffb111da2fb, 0x7ffb111da69b, [
    P('mabs', OP_CTX, 0x7ffb111da302, 0x109, 2), P('movl', OP_CTX, 0x7ffb111da316, 0x000, 1),
    P('mabs', OP_CTX, 0x7ffb111da328, 0x008, 2), P('mabs', OP_CTX, 0x7ffb111da33e, 0x2b0, 2),
    P('mabs', OP_CTX, 0x7ffb111da353, 0x2d3, 1), P('mabs', OP_CTX, 0x7ffb111da368, 0x080, 1),
    P('mabs', OP_CTX, 0x7ffb111da37d, 0x083, 1), P('mabs', OP_CTX, 0x7ffb111da393, 0x08c, 2),
    P('mabs', OP_CTX, 0x7ffb111da3a8, 0x0d7, 1), P('mabs', OP_CTX, 0x7ffb111da3bd, 0x206, 1),
    P('mabs', OP_CTX, 0x7ffb111da3d5, 0x208, 1), P('mabs', OP_CTX, 0x7ffb111da3f0, 0x20c, 1),
    P('mabs', OP_CTX, 0x7ffb111da40f, 0x20e, 3), P('mabs', OP_CTX, 0x7ffb111da432, 0x280, 2),
    P('mabs', OP_CTX, 0x7ffb111da44d, 0x283, 1), P('mabs', OP_CTX, 0x7ffb111da46c, 0x2f5, 3),
    P('mabs', OP_CTX, 0x7ffb111da48e, 0x2f9, 1), P('mabs', OP_CTX, 0x7ffb111da4aa, 0x313, 2),
    P('mabs', OP_CTX, 0x7ffb111da4d7, 0x1d4, 1, cond='nav2'),
    P('mabs', OP_CTX, 0x7ffb111da4f9, 0x103, 1), P('mabs', OP_CTX, 0x7ffb111da50d, 0x297, 1),
    P('mabs', OP_CTX, 0x7ffb111da522, 0x2a3, 1),
    P('hdrimm', OP_CTX, 0x7ffb111da543, 0x2a6, 1, cond='!nav1', ix=0x7ffb111da569),
    P('hdrimm', OP_CTX, 0x7ffb111da55a, 0x2a6, 2, cond='nav1', ix=0x7ffb111da569),
    P('mabs', OP_CTX, 0x7ffb111da57c, 0x2ae, 1), P('mabs', OP_CTX, 0x7ffb111da5a2, 0x2d4, 1),
    P('mabs', OP_CTX, 0x7ffb111da5c0, 0x2e5, 2), P('mabs', OP_CTX, 0x7ffb111da5df, 0x316, 2),
    P('mabs', OP_UCF, 0x7ffb111da5f4, 0x249, 2), P('mabs', OP_UCF, 0x7ffb111da60c, 0x259, 2),
    P('mabs', OP_UCF, 0x7ffb111da624, 0x25f, 2),
    P('mabs', OP_SHI, 0x7ffb111da638, 0x001, 1, idx=3), P('mabs', OP_SHI, 0x7ffb111da64c, 0x007, 1, idx=3),
    P('mabs', OP_SHI, 0x7ffb111da660, 0x041, 1, idx=3), P('mabs', OP_SHI, 0x7ffb111da674, 0x101, 1, idx=3),
    P('mabs', OP_SHI, 0x7ffb111da688, 0x107, 1, idx=3),
])

PIPE_CTX = (0x7ffb111da69b, 0x7ffb111da996, [
    P('mabs', OP_CTX, 0x7ffb111da6a5, 0x08e, 2), P('mabs', OP_CTX, 0x7ffb111da6b6, 0x1e0, 8),
    P('mabs', OP_CTX, 0x7ffb111da6e6, 0x003, 2), P('mabs', OP_CTX, 0x7ffb111da708, 0x201, 3),
    P('mabs', OP_CTX, 0x7ffb111da72f, 0x2dc, 1), P('mabs', OP_CTX, 0x7ffb111da749, 0x1ff, 1),
    P('mabs', OP_CTX, 0x7ffb111da763, 0x207, 1), P('mabs', OP_CTX, 0x7ffb111da77d, 0x293, 1),
    P('mabs', OP_CTX, 0x7ffb111da797, 0x2f8, 1), P('mabs', OP_CTX, 0x7ffb111da7b5, 0x30e, 3),
    P('mabs', OP_CTX, 0x7ffb111da7d3, 0x191, 32),
    P('const', OP_CTX, 0x7ffb1149b2c0, 0x1b1, 1),                       # loaded @7ffb111da7fc
    P('constnext', OP_CTX, 0x7ffb1149b2c0, 0x1b3, 4, ix=0x7ffb111da816),
    P('mabs', OP_CTX, 0x7ffb111da841, 0x1b8, 1), P('mabs', OP_CTX, 0x7ffb111da85e, 0x1ba, 1),
    P('mabs', OP_CTX, 0x7ffb111da87d, 0x1c2, 4), P('mabs', OP_CTX, 0x7ffb111da8a3, 0x1d5, 3),
    P('mabs', OP_CTX, 0x7ffb111da8b8, 0x1d8, 8), P('mabs', OP_CTX, 0x7ffb111da8e5, 0x286, 2),
    P('mabs', OP_CTX, 0x7ffb111da8fd, 0x2a1, 1), P('mabs', OP_CTX, 0x7ffb111da915, 0x2d5, 2),
    P('mabs', OP_CTX, 0x7ffb111da92d, 0x2db, 1), P('mabs', OP_CTX, 0x7ffb111da944, 0x2ab, 1),
    P('mabs', OP_CTX, 0x7ffb111da96b, 0x290, 2, cond='b7'),
    P('mabs', OP_CTX, 0x7ffb111da983, 0x29b, 1, cond='b7'),
])

PIPE_SH = (0x7ffb111da996, 0x7ffb111daa70, [
    P('const', OP_SHI, 0x7ffb1149b2d0, 0x046, 1, cond='b7', idx=3),     # loaded @7ffb111da9a2
    P('constnext', OP_SH, 0x7ffb1149b2d0, 0x047, 5, cond='b7', ix=0x7ffb111da9b4),
    P('mabs', OP_SH, 0x7ffb111da9d8, 0x0c8, 2), P('mabs', OP_SH, 0x7ffb111da9f1, 0x08a, 2),
    P('mabs', OP_SH, 0x7ffb111daa0b, 0x148, 2), P('mabs', OP_SH, 0x7ffb111daa25, 0x108, 4),
    P('mabs', OP_SH, 0x7ffb111daa4c, 0x008, 4),
])

def enc_packets():
    L = [P('mabs', OP_CTX, 0x7ffb111daafa, 0x104, 5)]
    for i in range(8):
        L.append(P('hdrimm', OP_CTX, 0x7ffb111dab34, 0x318 + 15 * i, 14, ix=0x7ffb111dab41, loop=i))
        for b, ixa in ((0x390, 0x7ffb111dab73), (0x398, 0x7ffb111dab8d), (0x3a0, 0x7ffb111daba7),
                       (0x3a8, 0x7ffb111dabc1), (0x3b0, 0x7ffb111dabdb), (0x3b8, 0x7ffb111dabf5)):
            L.append(P('hdrimm', OP_CTX, 0x7ffb111dab1d, b | i, 1, ix=ixa, loop=i))
    L += [
        P('mabs', OP_CTX, 0x7ffb111dac18, 0x001, 1),
        P('mabs', OP_CTX, 0x7ffb111dac43, 0x018, 2, cond='nav2'),
        P('mabs', OP_CTX, 0x7ffb111dac60, 0x01f, 1), P('mabs', OP_CTX, 0x7ffb111dac77, 0x10b, 3),
        P('mabs', OP_CTX, 0x7ffb111dac8f, 0x200, 1), P('mabs', OP_CTX, 0x7ffb111daca6, 0x002, 1),
        P('mabs', OP_CTX, 0x7ffb111dacbd, 0x005, 1), P('mabs', OP_CTX, 0x7ffb111dacd4, 0x007, 1),
        P('mabs', OP_CTX, 0x7ffb111dacec, 0x00a, 2), P('mabs', OP_CTX, 0x7ffb111dacfe, 0x010, 6),
        P('mabs', OP_CTX, 0x7ffb111dad2d, 0x01a, 5), P('mabs', OP_CTX, 0x7ffb111dad5b, 0x2af, 1),
        P('mabs', OP_CTX, 0x7ffb111dad72, 0x2b2, 1), P('mabs', OP_CTX, 0x7ffb111dad86, 0x00c, 2),
        P('mabs', OP_CTX, 0x7ffb111dada2, 0x081, 2), P('mabs', OP_CTX, 0x7ffb111dadb8, 0x090, 2),
        P('hdrimm', OP_CTX, 0x7ffb111dadd7, 0x094, 'a5x2', cond='a5', ix=0x7ffb111dade2),
        P('hdrimm', OP_CTX, 0x7ffb111dae15, 0x0b4, 'a4x2', ix=0x7ffb111dae1f),
        P('mabs', OP_CTX, 0x7ffb111dae59, 0x0f0, 3),
        P('hdrimm', OP_CTX, 0x7ffb111dae74, 0x10f, 'a4x6', ix=0x7ffb111dae7d),
        P('mabs', OP_CTX, 0x7ffb111daead, 0x204, 2),
        P('mabs', OP_CTX, 0x7ffb111daed3, 0x212, 1, cond='nav2'),
        P('mabs', OP_CTX, 0x7ffb111daeee, 0x282, 1), P('mabs', OP_CTX, 0x7ffb111daf04, 0x292, 1),
        P('mabs', OP_CTX, 0x7ffb111daf24, 0x2de, 6), P('mabs', OP_CTX, 0x7ffb111daf3b, 0x2fe, 16),
        P('mabs', OP_CTX, 0x7ffb111daf6d, 0x2fa, 4), P('mabs', OP_CTX, 0x7ffb111daf89, 0x311, 2),
        P('mabs', OP_CTX, 0x7ffb111dafb8, 0x1b7, 1, cond='nav2'),
        P('mabs', OP_CTX, 0x7ffb111dafd5, 0x2ad, 1),
    ]
    return L
ENCODER = (0x7ffb111daae6, 0x7ffb111daff4, enc_packets())

# ------------------------------------------------------------- verification --
def disasm(lo, hi):
    out = subprocess.run([OBJDUMP, "-d", "--start-address=0x%x" % lo, "--stop-address=0x%x" % hi, DYLIB],
                         capture_output=True, text=True).stdout
    lines = {}
    for ln in out.splitlines():
        m = re.match(r"^([0-9a-f]+):\s", ln)
        if m: lines[int(m.group(1), 16)] = ln
    if not lines: die("empty disassembly for 0x%x..0x%x" % (lo, hi))
    return lines

def imm_at(lines, a):
    ln = lines.get(a)
    if ln is None: die("no instruction at 0x%x" % a)
    m = re.search(r"\$(-?0x[0-9a-f]+)", ln)
    if not m: die("no immediate at 0x%x: %s" % (a, ln.strip()))
    return int(m.group(1), 16) & 0xFFFFFFFFFFFFFFFF, ln

dylib = open(DYLIB, "rb").read()
def const_dwords(a):
    return struct.unpack_from("<4I", dylib, a - TEXT_VMADDR)

def verify(name, fn):
    lo, hi, pk = fn
    lines = disasm(lo, hi)
    accounted = set()
    for p in pk:
        op = p["op"]
        if p["kind"] in ("mabs", "movl"):
            imm, ln = imm_at(lines, p["at"])
            n = p["n"]
            want_idx = ((p["idx"] or 0) << 28) | p["index"]
            if (imm & 0xFFFFFFFF) != hdr(op, n) or (imm >> 32) != (want_idx if p["kind"] == "mabs" else 0):
                die("%s @0x%x: immediate 0x%x != header 0x%08x index 0x%x" % (name, p["at"], imm, hdr(op, n), want_idx))
            accounted.add(p["at"])
        elif p["kind"] == "hdrimm":
            imm, ln = imm_at(lines, p["at"])
            cnt = p["n"] if isinstance(p["n"], int) else 0
            if (imm & 0xFFFFFFFF) != hdr(op, cnt):
                die("%s @0x%x: header immediate 0x%x != 0x%08x" % (name, p["at"], imm, hdr(op, cnt)))
            ix, _ = imm_at(lines, p["ix"])
            base_ix = p["index"] if p["loop"] is None else (p["index"] & ~0x7 if p["index"] >= 0x390 else 0x318)
            if ix != base_ix:
                die("%s index instruction @0x%x: 0x%x != 0x%x" % (name, p["ix"], ix, base_ix))
            accounted.add(p["at"])
        elif p["kind"] == "const":
            d = const_dwords(p["at"])
            want = ((p["idx"] or 0) << 28) | p["index"]
            if d[0] != hdr(op, p["n"]) or d[1] != want:
                die("%s const @0x%x: %08x %08x != %08x %08x" % (name, p["at"], d[0], d[1], hdr(op, p["n"]), want))
        elif p["kind"] == "constnext":
            d = const_dwords(p["at"])
            ix, _ = imm_at(lines, p["ix"])
            if d[3] != hdr(op, p["n"]) or ix != p["index"]:
                die("%s const-next @0x%x: %08x / index 0x%x" % (name, p["at"], d[3], ix))
    # completeness: every PKT3 SET-family header immediate in range is transcribed
    for a, ln in lines.items():
        for m in re.finditer(r"\$(0x[0-9a-f]+)", ln):
            v = int(m.group(1), 16); lo32 = v & 0xFFFFFFFF
            if (lo32 & 0xC00000FD) == 0xC0000000 and ((lo32 >> 8) & 0xFF) in (OP_CTX, OP_SH, OP_UCF, OP_SHI, OP_UCFI):
                if a not in accounted:
                    die("%s: untranscribed PKT3 immediate 0x%x at 0x%x" % (name, v, a))
    return len(lines)

# -------------------------------------------------------------- oracle ------
class DB:
    def __init__(self, f):
        j = json.load(open(os.path.join(MESA, f)))
        self.name, self.addr = {}, {}
        for e in j["register_mappings"]:
            self.name.setdefault(e["name"], e)
            self.addr.setdefault(e["map"]["at"], e["name"])
        self.types = j["register_types"]
    def fields(self, nm):
        e = self.name[nm]; t = self.types.get(e.get("type_ref", nm))
        return None if t is None else [(f["name"], f["bits"][0], f["bits"][1]) for f in t["fields"]]
D10, D12 = DB("gfx103.json"), DB("gfx12.json")
LEGACY = re.compile(r"^SPI_SHADER_(PGM_(LO|HI|RSRC[1-4])_VS|LATE_ALLOC_VS|USER_DATA_VS_\d+)$")

# 0.0.389 (notes 880 H6, 881) - XLAT12_CLS_REUSED. 92 gfx10.3 addresses are ALIASES: the gfx10.3 name is absent
# from gfx12 and gfx12 puts a DIFFERENT register at that address. The translator does NOT re-emit at 91 of them -
# re-emitting a CB_COLOR0_CMASK value into CB_COLOR0_ATTRIB3 would be worse than dropping it. These TWO are the
# exception because mesa's own gfx12 path writes a KNOWN CONSTANT to the register that now lives there for a
# colour-only pass (radv_gfx12_emit_null_ds_state R_028B94_PA_SC_HIZ_INFO = SURFACE_ENABLE(0); ac_cmdbuf.c's gfx12
# preamble R_028B98_PA_SC_HIS_INFO = SURFACE_ENABLE(0)), so the mapped value comes from mesa, not from Apple's
# bits. The list is the SAME two rows as src/xlat12/xlat12_reused.h and is a COPY of it: for these two addresses
# this oracle is NOT independent of the C, and their proof is the capture fixture and the planted defect instead.
REUSED = {0x28b94: "PA_SC_HIZ_INFO", 0x28b98: "PA_SC_HIS_INFO"}

# build 0.0.453 item 8 (xlat12_ib.c's d_swmode_repack, build 0.0.451 item 8): this
# oracle had NO enum awareness for CB_COLORn_ATTRIB3's COLOR_SW_MODE or DB_Z_INFO/DB_STENCIL_INFO's SW_MODE - both
# fields are copied by oracle_repack() like any other plain bitfield, by NAME and POSITION alone, which is correct
# for a counter or an offset but wrong for an enum whose gfx12 numbering differs from gfx10's (mode 22
# ADDR_SW_4KB_D_X must become 2 ADDR3_4KB_2D, not the low 3 bits of 22, which is 6 - the SAME defect 0.0.451 item 8
# found in the generated C repack tables). tests/golden_vectors.h's 4 encoder_* rows were HAND-CORRECTED past this
# gap (0.0.452 item 3); this closes it in the generator itself, the "proper fix" that hand-correction's own comment
# flagged as out of scope for a "small item".
#
# SWMODE_MAP is a COPY of xlat12_desc.h's kXlat12SwModeG10ToG12 (itself generated by tools/gen-desc-xlat.py,
# "matched BY NAME through addrlib's own per-mode flag tables" - not from either register database, which carries
# only the field POSITION, never tiling-mode semantics). This is the SAME disclosed non-independence REUSED above
# already has, and for the same reason: addrlib's tiling-mode equivalence is not a register-field-LAYOUT fact this
# oracle exists to verify independently a second time - it is the ALREADY-established input BOTH d_swmode_repack
# and this oracle must apply identically, exactly as REUSED's two rows both read from mesa's known preamble value.
SWMODE_MAP = {
    0: 0, 1: 1, 2: 1, 5: 2, 6: 2, 9: 3, 10: 3, 17: 3, 18: 3, 21: 2, 22: 2, 23: 2, 24: 3, 25: 3, 26: 3, 27: 3,
}
# a10 -> (gfx10 SW_MODE field lo, gfx12 SW_MODE field lo, gfx12 SW_MODE field width) - the SAME (inLo, outLo, outW)
# triples d_swmode_repack's two call sites pass (xlat12_ib.c: CB_COLORn_ATTRIB3 is (14, 15, 3); DB_Z_INFO/
# DB_STENCIL_INFO is (4, 4, 5)).
SWMODE_FIELDS = {}
for _a in (0x28ee0, 0x28ee4, 0x28ee8, 0x28eec, 0x28ef0, 0x28ef4, 0x28ef8, 0x28efc):
    SWMODE_FIELDS[_a] = (14, 15, 3)
for _a in (0x28040, 0x28044):
    SWMODE_FIELDS[_a] = (4, 4, 5)

def oracle_class(a10):
    nm = D10.addr.get(a10)
    if nm is None: return ("unknown", None, None, None)
    if a10 in REUSED:
        if D12.addr.get(a10) != REUSED[a10]:
            die("reused 0x%05x: gfx12.json names %s, not %s" % (a10, D12.addr.get(a10), REUSED[a10]))
        return ("reused", nm, a10, None)
    if nm not in D12.name: return ("legacy_vs" if LEGACY.match(nm) else "absent", nm, 0, None)
    a12 = D12.name[nm]["map"]["at"]
    f10, f12 = D10.fields(nm), D12.fields(nm)
    if f10 is not None and f12 is not None and f10 != f12:
        if not set(x[0] for x in f10) & set(x[0] for x in f12):
            return ("unknown", nm, None, None)        # no shared field: no established mapping
        return ("field_repack", nm, a12, (f10, f12))
    return ("identical" if a12 == a10 else "moved", nm, a12, None)

def oracle_repack(v, ff):
    f10, f12 = ff; src = {n: (lo, hi) for n, lo, hi in f10}; o = 0
    for n, lo, hi in f12:
        if n in src:
            slo, shi = src[n]; w = min(shi - slo, hi - lo) + 1
            o |= ((v >> slo) & ((1 << w) - 1)) << lo
    return o & 0xFFFFFFFF

def op_for(a):
    if CTX <= a < 0x30000: return OP_CTX
    if SH <= a < 0xC000: return OP_SH
    if UCF <= a < 0x40000: return OP_UCF
    return None

PRIMGEN = [f for f in D10.fields("VGT_SHADER_STAGES_EN") if f[0] == "PRIMGEN_EN"][0][1]
VSE = D10.name["VGT_SHADER_STAGES_EN"]["map"]["at"]

def oracle(stream, ngg):
    """Independent re-implementation of the translation contract in xlat12.h."""
    out, st = [], dict(regs_in=0, identical=0, moved=0, repacked=0, absent=0, legacy=0, reused=0,
                       set_out=0, split=0)
    i = 0
    while i < len(stream):
        h = stream[i]; op = (h >> 8) & 0xFF; flags = h & 0xFF; body = ((h >> 16) & 0x3FFF) + 1
        pkt = stream[i + 1:i + 1 + body]
        if op in (OP_CTX, OP_SH, OP_UCF, OP_SHI, OP_UCFI):
            isidx = op in (OP_SHI, OP_UCFI)
            off = pkt[0] & (0xFFFF if isidx else 0xFFFFFFFF); idx = pkt[0] >> 28
            runs = []
            for k, v in enumerate(pkt[1:]):
                a10 = BASE[op] + ((off + k) << 2)
                st["regs_in"] += 1
                cls, nm, a12, ff = oracle_class(a10)
                if cls == "unknown": return ("ERR_UNKNOWN_REG", a10, out, st, ngg)
                if cls == "absent": st["absent"] += 1; continue
                if cls == "legacy_vs":
                    if ngg or v == 0: st["legacy"] += 1; continue
                    return ("ERR_VS_MODE_UNKNOWN", a10, out, st, ngg)
                if a10 == VSE:
                    if not (v >> PRIMGEN) & 1: return ("ERR_LEGACY_VS", a10, out, st, ngg)
                    ngg = True
                v10 = v   # build 0.0.453 item 8: the RAW gfx10 value, needed below after `v` is overwritten
                # build 0.0.453 item 8: CB_COLORn_ATTRIB3's COLOR_SW_MODE / DB_Z_INFO's and DB_STENCIL_INFO's
                # SW_MODE, remapped BY NAME through SWMODE_MAP - mirroring xlat12.c's OWN sequencing exactly (the
                # renderxlat path this oracle checks, 0.0.452 item 3 F5, xlat12.c lines ~266-288): the refusal is
                # checked and returned BEFORE regs_repacked is incremented, so a register that refuses is counted
                # in neither "repacked" nor any other class - it never reaches the increment at all. Getting this
                # ordering wrong (checking AFTER incrementing "repacked") was caught here BY THIS SCRIPT'S OWN
                # direct comparison against a live xlat12_translate() run over these exact _in arrays (see the
                # regeneration note at the top of this file) - it silently counted the REFUSING register as one
                # more successful repack, which is wrong for the SAME reason d_tbl_fail's callers must not count a
                # refused draw as translated.
                if a10 in SWMODE_FIELDS and cls == "field_repack":
                    inLo, outLo, outW = SWMODE_FIELDS[a10]
                    g10Mode = (v10 >> inLo) & 0x1F
                    if g10Mode not in SWMODE_MAP:
                        return ("ERR_SWMODE", a10, out, st, ngg)
                if cls == "reused": v = 0; st["reused"] += 1        # the gfx12 register here shares no field: all 0
                elif cls == "field_repack":
                    v = oracle_repack(v, ff)
                    if a10 in SWMODE_FIELDS:
                        inLo, outLo, outW = SWMODE_FIELDS[a10]
                        g12Mode = SWMODE_MAP[(v10 >> inLo) & 0x1F]   # already proven present, above
                        outMask = ((1 << outW) - 1) << outLo
                        v = (v & ~outMask) | ((g12Mode << outLo) & outMask)
                    st["repacked"] += 1
                elif cls == "moved": st["moved"] += 1
                else: st["identical"] += 1
                if isidx:
                    runs.append([op, (a12 - BASE[op]) >> 2, [v], True])
                    continue
                o = op_for(a12); off12 = (a12 - BASE[o]) >> 2
                if runs and runs[-1][0] == o and runs[-1][1] + len(runs[-1][2]) == off12:
                    runs[-1][2].append(v)
                else:
                    runs.append([o, off12, [v], False])
            for o, off12, vals, ix in runs:
                out.append(hdr(o, len(vals), flags))
                out.append(((idx << 28) | off12) if ix else off12)
                out += vals
            st["set_out"] += len(runs)
            st["split"] += max(0, len(runs) - 1)
        else:
            out += stream[i:i + 1 + body]
        i += 1 + body
    return ("OK", 0, out, st, ngg)

# ------------------------------------------------------------- streams ------
def val(a, salt):
    x = (a * 2654435761 + salt * 40503 + 0x6b43a9b5) & 0xFFFFFFFF
    x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
    x = x or 0x5A5A5A5A
    if a == VSE: x |= 1 << PRIMGEN          # an NGG pipeline, as radeonsi emits
    return x

def build(fn, cfg, salt, drop_addrs=()):
    s = []
    for p in fn[2]:
        c = p["cond"]
        if c:
            neg = c.startswith("!"); key = c.lstrip("!")
            on = bool(cfg.get(key))
            if on == neg: continue
        n = p["n"]
        if n == 'a5x2': n = 2 * cfg["a5"]
        elif n == 'a4x2': n = 2 * cfg["a4"]
        elif n == 'a4x6': n = 6 * cfg["a4"]
        index = p["index"]
        addrs = [BASE[p["op"]] + ((index + k) << 2) for k in range(n)]
        if any(a in drop_addrs for a in addrs): continue
        s.append(hdr(p["op"], n))
        s.append(((p["idx"] or 0) << 28) | index)
        for k, a in enumerate(addrs):
            v = val(a, salt)
            if p["at"] == 0x7ffb111dad86 and k == 0: v = 0   # constant 0 @7ffb111dad94
            s.append(v)
    return s

def main():
    counts = {}
    for nm, fn in (("WriteStaticHwRegs", STATIC), ("WriteRenderPipelineHwCtxRegs", PIPE_CTX),
                   ("WriteRenderPipelineHwShRegs", PIPE_SH), ("WriteRenderEncoderHwCtxRegs", ENCODER)):
        counts[nm] = verify(nm, fn)
    print("transcription verified against llvm-objdump:", counts)

    UNNAMED_ENC = {0x28060, 0x28064, 0x283c0, 0x283c4, 0x283c8, 0x286dc}
    vecs = []
    def add(name, stream, ngg_in, note):
        r = oracle(stream, ngg_in)
        vecs.append(dict(name=name, inp=stream, status=r[0], err=r[1], exp=r[2] if r[0] == "OK" else [],
                         st=r[3], ngg_in=ngg_in, ngg_out=r[4], note=note))
    add("pipeline_b7_0", build(PIPE_CTX, {}, 1) + build(PIPE_SH, {}, 2), False,
        "WriteRenderPipelineHwCtxRegs + HwShRegs, HwInfoRec+0xcc bit7 clear; ctx starts UNKNOWN")
    add("pipeline_b7_1", build(PIPE_CTX, {"b7": 1}, 3) + build(PIPE_SH, {"b7": 1}, 4), False,
        "same with bit7 set: legacy-VS slot and VGT_GS_MODE written; NGG learnt from VGT_SHADER_STAGES_EN")
    add("static_nav2_ngg", build(STATIC, {"nav2": 1}, 5), True,
        "WriteStaticHwRegs as Navi2x (isNavi2xBased=1, isNavi1xBased=0); ctx seeded NGG")
    add("static_nav2_unknown", build(STATIC, {"nav2": 1}, 5), False,
        "same stream, ctx UNKNOWN: the non-zero RSRC4_VS index write must be refused")
    RSRC4 = {0x0B004, 0x0B404}   # no shared field names gfx10.3 -> gfx12: refused (gen_tables no-shared rule)
    add("static_named_ngg", build(STATIC, {"nav2": 1}, 5, RSRC4), True,
        "WriteStaticHwRegs minus the two RSRC4 index writes; ctx seeded NGG")
    add("static_named_unknown", build(STATIC, {"nav2": 1}, 5, RSRC4), False,
        "same, ctx UNKNOWN: the non-zero RSRC4_VS (legacy-VS) index write must be refused")
    add("encoder_faithful_nav2", build(ENCODER, {"nav2": 1, "a4": 1, "a5": 1}, 6), True,
        "WriteRenderEncoderHwCtxRegs exactly as emitted for 1 viewport/1 scissor (Navi2x): refused at the first unnamed register")
    add("encoder_faithful_nav1", build(ENCODER, {"a4": 1, "a5": 1}, 7), True,
        "same with isNavi2xBased=0: the unconditional x3 @0x283c0 run still refuses")
    add("encoder_named_nav2", build(ENCODER, {"nav2": 1, "a4": 1, "a5": 1}, 6, UNNAMED_ENC), True,
        "encoder minus the three packets carrying registers no gfx10.3 source names")
    add("encoder_named_4vp", build(ENCODER, {"nav2": 1, "a4": 4, "a5": 4}, 8, UNNAMED_ENC), True,
        "encoder minus unnamed packets, 4 viewports / 4 scissors")

    # field-repack cases: the ADDRESSES and names come from the generated table (so every
    # repack row is covered); the expected values come only from this oracle.
    tab = open(os.path.join(REPO, "src/xlat12/xlat12_tables.h")).read()
    rows = re.findall(r'\{ 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, (\w+), XLAT12_CLS_(\w+), "(\w+)"', tab)
    if not rows: die("could not parse src/xlat12/xlat12_tables.h (run gen_tables.py first)")
    cases = []
    for g10s, g12s, rp, cls, nm in rows:
        g10, g12 = int(g10s, 16), int(g12s, 16)
        if cls != "FIELD_REPACK": continue
        if nm not in D10.name or nm not in D12.name:
            die("table repack %s not in both Mesa databases" % nm)
        f10, f12 = D10.fields(nm), D12.fields(nm)
        if f10 is None or f12 is None or f10 == f12:
            die("oracle disagrees that %s is a field repack" % nm)
        if D12.name[nm]["map"]["at"] != g12:
            die("oracle disagrees on gfx12 address of %s" % nm)
        for v in (0xFFFFFFFF, (0x5A5AA5A5 ^ g10) & 0xFFFFFFFF, val(g10, 99)):
            cases.append((g10, nm, v, oracle_repack(v, (f10, f12))))
    print("repack cases: %d over %d registers" % (len(cases), len(cases) // 3))

    # record sets (design's 42 and 93)
    txt = open(RECS).read(); recs = {}
    for m in re.finditer(r"^(GFX10_\w+) \((\d+)\): (.*)$", txt, re.M):
        recs[m.group(1)] = m.group(3).split()

    outp = os.path.join(REPO, "src/xlat12/tests/golden_vectors.h")
    with open(outp, "w") as f:
        f.write("/* GENERATED by src/xlat12/gen/gen_golden.py - do not edit.\n"
                " * Packets transcribed from AMDRadeonX6000MTLDriver (25G83) writers and re-verified\n"
                " * against llvm-objdump; expected output from an independent Python oracle over\n"
                " * Mesa gfx103.json/gfx12.json @4519cc56. */\n")
        f.write("#ifndef XLAT12_GOLDEN_VECTORS_H\n#define XLAT12_GOLDEN_VECTORS_H\n#include <stdint.h>\n\n")
        f.write("typedef struct { const char *name; uint32_t g10; } GoldenReg;\n")
        f.write("typedef struct {\n  const char *name; const char *note;\n  const uint32_t *in; uint32_t in_len;\n"
                "  const uint32_t *exp; uint32_t exp_len;\n  const char *status; uint32_t err_addr;\n"
                "  uint32_t ngg_in, ngg_out;\n  uint32_t regs_in, identical, moved, repacked, dropped_absent, dropped_legacy,"
                " set_packets_out, set_runs_split, regs_reused;\n} GoldenVec;\n\n")
        for v in vecs:
            for tag, arr in (("in", v["inp"]), ("exp", v["exp"])):
                f.write("static const uint32_t g_%s_%s[] = {" % (v["name"], tag))
                for k, w in enumerate(arr):
                    f.write(("\n  " if k % 8 == 0 else " ") + "0x%08x," % w)
                if not arr: f.write(" 0")
                f.write("\n};\n")
        f.write("\nstatic const GoldenVec kGolden[] = {\n")
        for v in vecs:
            st = v["st"]
            f.write('  { "%s", "%s", g_%s_in, %d, g_%s_exp, %d, "%s", 0x%05x, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },\n'
                    % (v["name"], v["note"], v["name"], len(v["inp"]), v["name"], len(v["exp"]), v["status"], v["err"],
                       int(v["ngg_in"]), int(v["ngg_out"]), st["regs_in"], st["identical"], st["moved"], st["repacked"],
                       st["absent"], st["legacy"], st["set_out"], st["split"], st["reused"]))
        f.write("};\n#define GOLDEN_COUNT (sizeof(kGolden)/sizeof(kGolden[0]))\n\n")
        for rn, tag in (("GFX10_RenderPipelineHwCtxRegsRec", "pipeline42"), ("GFX10_RenderEncoderHwCtxRegsRec", "encoder93")):
            lst = recs[rn]
            f.write("/* %s (%d names, mtl-regrecs.txt): g10 from gfx103.json, 0 = not named there */\n" % (rn, len(lst)))
            f.write("static const GoldenReg g_rec_%s[] = {\n" % tag)
            for nm in lst:
                a = D10.name[nm]["map"]["at"] if nm in D10.name else 0
                f.write('  { "%s", 0x%05x },\n' % (nm, a))
            f.write("};\n")
        f.write("\ntypedef struct { uint32_t g10; const char *name; uint32_t in, exp; } RepackCase;\n")
        f.write("static const RepackCase kRepackCases[] = {\n")
        for g10, nm, v, e in cases:
            f.write('  { 0x%05x, "%s", 0x%08x, 0x%08x },\n' % (g10, nm, v, e))
        f.write("};\n#define REPACK_CASE_COUNT (sizeof(kRepackCases)/sizeof(kRepackCases[0]))\n")
        f.write("\n#endif\n")
    for v in vecs:
        print("%-24s in %4d  %-20s err 0x%05x  out %4d  %s" % (v["name"], len(v["inp"]), v["status"], v["err"], len(v["exp"]), v["st"]))
    print("wrote", outp)

if __name__ == "__main__":
    main()
