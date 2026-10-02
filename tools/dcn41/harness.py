#!/usr/bin/env python3
"""harness.py - register-trace equivalence of src/dcn41 against Linux's own DCN 4.01 functions, with mutants.

  python3 tools/dcn41/harness.py build     compile the Linux objects, the harness and src/dcn41; link (stubbing the
                                          Linux symbols no scenario reaches, each stub aborts loudly if ever called)
  python3 tools/dcn41/harness.py run       run the harness (every scenario on 4 seeds, plus planted controls)
  python3 tools/dcn41/harness.py mutants   apply each mutant to a copy of src/dcn41, rebuild our side only, and
                                          require the harness (or the unit tests) to FAIL on it
  python3 tools/dcn41/harness.py all       build, run, mutants

Linux inputs, UNMODIFIED, from re/linux-dc (238650ef6c7c): dc/dc_helper.c, dc/irq/irq_service.c,
dc/irq/dcn401/irq_service_dcn401.c, dc/hubp/dcn401/dcn401_hubp.c, dc/hubp/dcn20/dcn20_hubp.c,
dc/optc/dcn10/dcn10_optc.c, dc/optc/dcn30/dcn30_optc.c, dc/optc/dcn401/dcn401_optc.c, and text extracted verbatim from
dc/resource/dcn401/dcn401_resource.c (the register macro block, the optc/hubp register arrays and the two create
functions) into $BUILD/harness/linux_tables.inc. Kernel headers the clone lacks come from tools/dcn41/linuxshim.

Every compile, link and run is one guarded child (dcn41lib.run), strictly sequential. Logs in $DCN41_BUILD/harness.
"""
import pathlib, re, shutil, sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

H = L.HERE / "harness"
OUT = L.BUILD / "harness"
D = L.DISPLAY
LINUX_SRCS = ["dc/dc_helper.c", "dc/irq/irq_service.c", "dc/irq/dcn401/irq_service_dcn401.c",
              "dc/hubp/dcn401/dcn401_hubp.c", "dc/hubp/dcn20/dcn20_hubp.c", "dc/optc/dcn10/dcn10_optc.c",
              "dc/optc/dcn30/dcn30_optc.c", "dc/optc/dcn401/dcn401_optc.c"]
LINUX_INC = [L.HERE / "linuxshim/include", D / "dc/inc", D / "dc/inc/hw", D / "dc/clk_mgr", D / "dc/hwss",
             D / "dc/resource", D / "dc/dsc", D / "dc/optc", D / "dc/dpp", D / "dc/hubbub", D / "dc/dccg",
             D / "dc/hubp", D / "dc/dio", D / "dc/dwb", D / "dc/hpo", D / "dc/mmhubbub", D / "dc/mpc", D / "dc/opp",
             D / "dc/pg", D / "dc/soc_and_ip_translator", D / "modules/inc", D / "dmub/inc", L.AMD / "include/asic_reg",
             L.AMD / "include", D, D / "include", D / "dc", D / "amdgpu_dm"]
LINUX_CFLAGS = ["-std=gnu11", "-O1", "-g", "-w", "-fno-strict-aliasing", "-fwrapv"]
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
EXE = OUT / "dcn41_harness"
DMUB_EXE = OUT / "dcn41_dmub_harness"
# amd/include/atombios.h is not in the re/linux-dc sparse clone; the dce112 command-table helper needs its
# ATOM_TRANSMITTER_DIGMODE_V6_* and ATOM_ENCODER_INIT. They come from the second local tree re/m2/linux (Linux 5878583,
# "Merge tag 'nfsd-7.3-1'", the same 7.3 merge window), searched LAST so nothing in re/linux-dc is shadowed.
M2_INCLUDE = L.ROOT / "re/m2/linux/drivers/gpu/drm/amd/include"
DMUB_LINUX_SRCS = ["dc/bios/command_table2.c", "dc/bios/command_table_helper2.c", "dc/bios/command_table_helper.c",
                   "dc/bios/dce112/command_table_helper2_dce112.c", "dmub/src/dmub_srv.c", "dmub/src/dmub_dcn401.c",
                   "dmub/src/dmub_reg.c"]

# (name, file under src/dcn41, exact text, replacement). Each must match exactly once.
MUTANTS = [
    ("flip-high-part-shift", "dcn41_hubp.c", "(uint32_t)(mc_addr >> 32)));", "(uint32_t)(mc_addr >> 31)));"),
    ("flip-vmid-on-immediate", "dcn41_hubp.c", "    if (!immediate) {\n", "    if (immediate) {\n"),
    ("flip-tmz-inverted", "dcn41_hubp.c", "PRIMARY_SURFACE_TMZ, tmz ? 1 : 0));", "PRIMARY_SURFACE_TMZ, tmz ? 0 : 1));"),
    ("flip-type-inverted", "dcn41_hubp.c", "SURFACE_FLIP_TYPE, immediate ? 1 : 0));", "SURFACE_FLIP_TYPE, immediate ? 0 : 1));"),
    ("flip-stereo-rmw-dropped", "dcn41_hubp.c",
     "    rc = dcn41_update(dev, fc, DCN41_M(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_IN_STEREOSYNC), 0);\n",
     "    rc = DCN41_OK;\n"),
    ("pending-compare-inverted", "dcn41_hubp.c", "earliest != dev->hubp_request_addr[hubp];",
     "earliest == dev->hubp_request_addr[hubp];"),
    ("pending-high-low-swapped", "dcn41_hubp.c", "earliest = ((uint64_t)hi << 32) | lo;",
     "earliest = ((uint64_t)lo << 32) | hi;"),
    ("flip-int-value", "dcn41_hubp.c", "SURFACE_FLIP_INT_MASK, 1));", "SURFACE_FLIP_INT_MASK, 0));"),
    ("irq-disable-keeps-bit", "dcn41_irq.c", "enable ? row.enable_mask : 0);", "enable ? row.enable_mask : row.enable_mask);"),
    ("irq-set-skips-ack", "dcn41_irq.c", "rc = dcn41_irq_ack_row(dev, &row);      /*", "rc = DCN41_OK;      /*"),
    ("hpd-polarity-inverted", "dcn41_irq.c", "DC_HPD_INT_POLARITY, sense ? 0 : 1);", "DC_HPD_INT_POLARITY, sense ? 1 : 0);"),
    ("vupdate-enable-copy-paste", "dcn41_irq.c",
     "row->enable_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_INT_EN);",
     "row->enable_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_INT_EN);"),
    ("pflip-ack-wrong-field", "dcn41_irq.c",
     "row->ack_mask = DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_CLEAR);",
     "row->ack_mask = DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_TYPE);"),
    ("ih-hpd-rx-boundary", "dcn41_irq.c", "        if (src_data0 <= 5) {\n", "        if (src_data0 <= 6) {\n"),
    ("ih-pflip-range", "dcn41_irq.c", "src_id <= DCN41_IH_SRC_PFLIP_HUBP0 + 5)", "src_id <= DCN41_IH_SRC_PFLIP_HUBP0 + 6)"),
    ("lock-wait-tries", "dcn41_otg.c", "__UPDATE_LOCK_STATUS__SHIFT, 1, 1, 10);", "__UPDATE_LOCK_STATUS__SHIFT, 1, 1, 9);"),
    ("unlock-writes-one", "dcn41_otg.c", "OTG_MASTER_UPDATE_LOCK, 0));\n}", "OTG_MASTER_UPDATE_LOCK, 1));\n}"),
    ("lock-sel-not-inst", "dcn41_otg.c", "OTG_MASTER_UPDATE_LOCK_SEL, otg));", "OTG_MASTER_UPDATE_LOCK_SEL, 0));"),
    ("position-fields-swapped", "dcn41_otg.c",
     "pos->horizontal_count = DCN41_FG(DCN41_OTG_OTG_STATUS_POSITION, OTG_HORZ_COUNT, v);",
     "pos->horizontal_count = DCN41_FG(DCN41_OTG_OTG_STATUS_POSITION, OTG_VERT_COUNT, v);"),
    ("active-size-sum", "dcn41_otg.c", "OTG_H_BLANK_START, hb) -", "OTG_H_BLANK_START, hb) +"),
    ("wait-delay-off-by-one", "dcn41_io.h", "dev->udelay(dev->cookie, delay_us);", "dev->udelay(dev->cookie, delay_us + 1);"),
    ("regs-vstartup-clear-mask", "dcn41_regs.h",
     "#define DCN41_OTG_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK 0x00000010u",
     "#define DCN41_OTG_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK 0x00000020u"),
    ("regs-hubpreq2-flip-control-offset", "dcn41_regs.h", "(i) == 1 ? 0x06efu : (i) == 2 ? 0x07cbu",
     "(i) == 1 ? 0x06efu : (i) == 2 ? 0x07ccu"),
    ("dmub-header-payload-shift", "dcn41_dmub.c", "((payload_bytes << DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT)",
     "((payload_bytes << (DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT - 1))"),
    ("dmub-encoder-hdmi-10bpc-scale", "dcn41_dmub.c", "pclk_10khz = (pclk_10khz * 30) / 24;",
     "pclk_10khz = (pclk_10khz * 30) / 25;"),
    ("dmub-tx-symclk-units", "dcn41_dmub.c", "symclk), pixel_clock_khz / 10);", "symclk), pixel_clock_khz / 100);"),
    ("dmub-pclk-crtc-gets-pll", "dcn41_dmub.c", "crtc_id), crtc_id);", "crtc_id), pll_id);"),
    ("dmub-ring-full-off-by-one", "dcn41_dmub.c", "return data_count == ring->capacity - DCN41_DMUB_CMD_SIZE;",
     "return data_count == ring->capacity;"),
    ("dmub-ring-wrap", "dcn41_dmub.c", "    if (next >= ring->capacity)\n        next %= ring->capacity;",
     "    if (next > ring->capacity)\n        next %= ring->capacity;"),
    ("dmub-gpint-ack-keeps-status", "dcn41_dmub.c", "acked = reg & ~DCN41_DMUB_GPINT_STATUS_MASK;", "acked = reg;"),
    ("dmub-gpint-delay", "dcn41_dmub.c", "        dev->udelay(dev->cookie, 1);\n        rc = dcn41_read(dev, addr, &v);",
     "        dev->udelay(dev->cookie, 2);\n        rc = dcn41_read(dev, addr, &v);"),
    ("dmub-return-data-rptr0", "dcn41_dmub.c", "ring->rptr == 0 ? ring->capacity - DCN41_DMUB_CMD_SIZE :",
     "ring->rptr == 0 ? 0 :"),
    ("dmub-fw-meta-scan-range", "dcn41_dmub.c", "    for (i = 0; i < 16; ++i)", "    for (i = 0; i < 15; ++i)"),
    ("dmub-caps-no-ret-status", "dcn41_dmub.c", "sizeof(struct dcn41_dmub_feature_caps), true);",
     "sizeof(struct dcn41_dmub_feature_caps), false);"),
    ("dmub-probe-region4-base", "dcn41_dmub.c", "st->inbox1_base == DCN41_DMUB_REGION4_INBOX1_BASE &&\n",
     "st->inbox1_base != DCN41_DMUB_REGION4_INBOX1_BASE &&\n"),
    ("dmub-layout-field-swap", "dcn41_dmub.h", "    uint8_t pll_id, encoderobjid, encoder_mode,",
     "    uint8_t encoderobjid, pll_id, encoder_mode,"),
    ("regs-frame-count-mask", "dcn41_regs.h",
     "#define DCN41_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK 0x00ffffffu",
     "#define DCN41_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK 0x01ffffffu"),
]


def extract_tables():
    """Linux's own register-table text from dcn401_resource.c, verbatim, with the line ranges recorded."""
    src = D / "dc/resource/dcn401/dcn401_resource.c"
    lines = src.read_text().splitlines()

    def find(text, start=0):
        for i in range(start, len(lines)):
            if lines[i].strip() == text:
                return i
        raise SystemExit(f"extract: '{text}' not found in {src.name}: refusing")

    def function(sig_first_line):
        i = find(sig_first_line)
        depth, seen, j = 0, False, i
        while j < len(lines):
            depth += lines[j].count("{") - lines[j].count("}")
            seen |= "{" in lines[j]
            if seen and depth == 0:
                return i, j
            j += 1
        raise SystemExit(f"extract: unbalanced function {sig_first_line}")

    a0 = find("#define BASE_INNER(seg) ctx->dcn_reg_offsets[seg]")
    a1 = find("static struct bios_registers bios_regs;", a0)
    b0 = find("#define optc_regs_init(id)\\")
    b1 = find("static struct dcn_hubbub_registers hubbub_reg;", b0)
    t0, t1 = function("static struct timing_generator *dcn401_timing_generator_create(")
    h0, h1 = function("static struct hubp *dcn401_hubp_create(")
    out = [f"/* GENERATED by tools/dcn41/harness.py from {src.relative_to(L.ROOT)} (Linux {L.LINUX_COMMIT}); verbatim. */"]
    for (s, e, what) in ((a0, a1, "register macros"), (b0, b1, "optc/hubp register arrays"),
                         (t0, t1 + 1, "dcn401_timing_generator_create"), (h0, h1 + 1, "dcn401_hubp_create")):
        out.append(f"/* ---- {what}: dcn401_resource.c lines {s + 1}-{e} ---- */")
        out += lines[s:e]
    text = "\n".join(out) + "\n"
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "linux_tables.inc").write_text(text)
    print(f"extract: linux_tables.inc from dcn401_resource.c lines {a0 + 1}-{a1}, {b0 + 1}-{b1}, {t0 + 1}-{t1 + 1}, "
          f"{h0 + 1}-{h1 + 1}")


def cc(args, what, log):
    r = L.run([L.CLANG, *args], what, log=log)
    L.check(r, what)
    return r


def build_linux(srcs=LINUX_SRCS, side="linux_side", main="main", prefix="linux_", extra_inc=()):
    L.need_linux()
    extract_tables()
    objs = []
    inc = [f"-I{p}" for p in LINUX_INC] + [f"-I{p}" for p in extra_inc]
    for s in srcs:
        o = OUT / (prefix + pathlib.Path(s).stem + ".o")
        cc([*LINUX_CFLAGS, *inc, "-c", str(D / s), "-o", str(o)], f"linux {s}", OUT / (o.stem + ".log"))
        objs.append(o)
    o = OUT / f"{side}.o"
    cc([*LINUX_CFLAGS, *inc, f"-I{OUT}", f"-I{H}", f"-I{L.SRC}", "-c", str(H / f"{side}.c"), "-o", str(o)],
       f"{side}.c", OUT / f"{side}.log")
    objs.append(o)
    for name in ("trace", main):
        o = OUT / f"{name}.o"
        cc(["-std=c11", "-O1", "-g", "-Wall", "-Werror", *SAN, f"-I{H}", f"-I{L.SRC}", "-c", str(H / f"{name}.c"),
            "-o", str(o)], f"{name}.c", OUT / f"{name}.log")
        objs.append(o)
    return objs


def ours_objects(src_dir, tag, side="ours_side"):
    """Our side: src/dcn41 (or a mutant copy) + the side file, one compile each, sanitized, -Werror."""
    d = OUT / tag
    d.mkdir(parents=True, exist_ok=True)
    objs = []
    for s in sorted(pathlib.Path(src_dir).glob("*.c")) + [H / f"{side}.c"]:
        o = d / (s.stem + ".o")
        cc(["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *SAN, f"-I{src_dir}", f"-I{H}", "-c", str(s),
            "-o", str(o)], f"{tag} {s.name}", d / (s.stem + ".log"))
        objs.append(o)
    return objs


UNDEF = re.compile(r'^\s+"_([A-Za-z0-9_]+)", referenced from:')


def link(objs, exe, log, stubname="stubs"):
    exe.parent.mkdir(parents=True, exist_ok=True)
    stubs_o = OUT / f"{stubname}.o"
    args = [*SAN, *map(str, objs)]
    if stubs_o.exists():
        args.append(str(stubs_o))
    r = L.run([L.CLANG, *args, "-o", str(exe)], f"link {exe.name}", log=log)
    if r.returncode == 0:
        return
    missing = sorted({m.group(1) for line in r.stderr.splitlines() for m in [UNDEF.match(line)] if m})
    if not missing:
        L.check(r, "link")
    stubs = OUT / f"{stubname}.c"
    known = set()
    if stubs.exists():
        known = set(re.findall(r"^void ([A-Za-z0-9_]+)\(void\)", stubs.read_text(), re.M))
    allsyms = sorted(known | set(missing))
    body = ["/* GENERATED by harness.py: Linux symbols the compiled DC files reference but no scenario reaches. */",
            "#include <stdio.h>", "#include <stdlib.h>",
            "static void unexpected(const char *s) { fprintf(stderr, \"HARNESS: unexpected call to stub %s\\n\", s); "
            "abort(); }"]
    body += [f"void {s}(void); void {s}(void) {{ unexpected(\"{s}\"); }}" for s in allsyms]
    stubs.write_text("\n".join(body) + "\n")
    cc(["-std=c11", "-O1", "-w", "-c", str(stubs), "-o", str(stubs_o)], f"{stubname}.c", OUT / f"{stubname}.log")
    print(f"link: {len(allsyms)} Linux symbols stubbed (abort if called): {', '.join(allsyms)}")
    r = L.run([L.CLANG, *SAN, *map(str, objs), str(stubs_o), "-o", str(exe)], f"relink {exe.name}", log=log)
    L.check(r, "relink")


def cmd_build():
    objs = build_linux()
    (OUT / "linux_objs.txt").write_text("\n".join(map(str, objs)) + "\n")
    link(objs + ours_objects(L.SRC, "ours"), EXE, OUT / "link.log")
    print(f"build: {EXE}")
    objs = build_linux(DMUB_LINUX_SRCS, "dmub_linux_side", "dmub_main", "linux_dmub_", extra_inc=(M2_INCLUDE,))
    (OUT / "dmub_linux_objs.txt").write_text("\n".join(map(str, objs)) + "\n")
    link(objs + ours_objects(L.SRC, "ours_dmub", "dmub_ours_side"), DMUB_EXE, OUT / "dmub_link.log", "dmub_stubs")
    print(f"build: {DMUB_EXE}")


def run_exe(exe, dump):
    r = L.run([str(exe), str(dump)], f"run {exe.name}", log=dump.with_suffix(".stdout.log"))
    return r


def cmd_run():
    rc = 0
    for exe, dump in ((EXE, OUT / "traces.txt"), (DMUB_EXE, OUT / "dmub_traces.txt")):
        r = run_exe(exe, dump)
        print(r.stdout.strip())
        if r.stderr.strip():
            print(r.stderr.strip()[-2000:])
        rc |= r.returncode
    return rc


def cmd_mutants():
    import build as B
    linux_objs = [pathlib.Path(p) for p in (OUT / "linux_objs.txt").read_text().split()]
    dmub_objs = [pathlib.Path(p) for p in (OUT / "dmub_linux_objs.txt").read_text().split()]
    caught = 0
    rows = []
    for name, fname, old, new in MUTANTS:
        mdir = OUT / "mutants" / name / "src"
        if mdir.exists():
            shutil.rmtree(mdir)
        shutil.copytree(L.SRC, mdir, ignore=shutil.ignore_patterns("tests"))
        p = mdir / fname
        text = p.read_text()
        n = text.count(old)
        if n != 1:
            raise SystemExit(f"mutant {name}: the target text occurs {n} times in {fname} (must be exactly once)")
        p.write_text(text.replace(old, new))
        exe = OUT / "mutants" / name / "dcn41_harness"
        dexe = OUT / "mutants" / name / "dcn41_dmub_harness"
        try:
            link(linux_objs + ours_objects(mdir, f"mutants/{name}/obj"), exe, OUT / "mutants" / name / "link.log")
            r = run_exe(exe, OUT / "mutants" / name / "traces.txt")
            link(dmub_objs + ours_objects(mdir, f"mutants/{name}/dobj", "dmub_ours_side"), dexe,
                 OUT / "mutants" / name / "dmub_link.log", "dmub_stubs")
            rd = run_exe(dexe, OUT / "mutants" / name / "dmub_traces.txt")
            h_fail = r.returncode != 0 or rd.returncode != 0
            summary = [ln for ln in r.stdout.splitlines() + rd.stdout.splitlines()
                       if (ln.startswith("harness: ") and "scenario runs" in ln) or
                       (ln.startswith("dmub harness: ") and " runs, " in ln)]
            summary = ["; ".join(summary)]
        except SystemExit as e:           # a mutant that does not even compile counts as caught by the compiler
            h_fail, summary = True, [f"build failed: {e}"]
        try:
            u_rc = B.cmd_unit(mdir, tag=f"harness/mutants/{name}/unit")
        except SystemExit:                 # the unit build refused the mutant (e.g. -Werror)
            u_rc = -1
        caught_by = [w for w, f in (("harness", h_fail), ("unit", u_rc != 0)) if f]
        ok = bool(caught_by)
        caught += ok
        rows.append(f"mutant {name:36s} {'CAUGHT by ' + '+'.join(caught_by) if ok else 'SURVIVED'}"
                    f"  [{summary[0] if summary else ''}]")
        print(rows[-1], flush=True)
    (OUT / "mutants.txt").write_text("\n".join(rows) + f"\nmutants caught {caught} of {len(MUTANTS)}\n")
    print(f"mutants caught {caught} of {len(MUTANTS)}")
    return 0 if caught == len(MUTANTS) else 1


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    rc = 0
    if what in ("build", "all"):
        cmd_build()
    if what in ("run", "all"):
        rc |= cmd_run()
    if what in ("mutants", "all"):
        rc |= cmd_mutants()
    return rc


if __name__ == "__main__":
    sys.exit(main())
