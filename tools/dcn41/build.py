#!/usr/bin/env python3
"""build.py - guarded host build and tests of src/dcn41 (the DCN 4.1 display register layer).

  python3 tools/dcn41/build.py unit        compile src/dcn41 + tests/test_dcn41.c (C11, ASan/UBSan, -Werror) and run
  python3 tools/dcn41/build.py cxx         compile every src/dcn41 .c as C++17 (-Werror): the kext is C++
  python3 tools/dcn41/build.py kextcheck   compile as x86_64 kernel code with the kext Makefile's own flags; 0 undefined
                                           symbols, 0 SSE/x87 instructions (src/xlat12/tests/fpu_count.py)
  python3 tools/dcn41/build.py allowcontrol  plant defects in the write allowlist table; the tests must FAIL
  python3 tools/dcn41/build.py regs        the four generators --check (every generated file is fresh)
  python3 tools/dcn41/build.py all         regs, unit, allowcontrol, cxx, kextcheck (then run harness.py and dmub/modes tools)

Every compiler and test child goes through dcn41lib.run: one at a time, memory_pressure >= 40% free before each, the
1.5 GB / 90 s guard on each. Outputs and logs go to $DCN41_BUILD (default $TMPDIR/dcn41-build), never the repo.
"""
import pathlib, subprocess, sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

WARN = ["-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wcast-align", "-Wstrict-prototypes", "-Wmissing-prototypes",
        "-Wvla", "-Wconversion", "-Wno-sign-conversion"]
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
MKSDK = L.ROOT / "src/RDNA4FB/MacKernelSDK"
OBJDUMP = "/opt/homebrew/opt/llvm/bin/llvm-objdump"
FPU_COUNT = L.ROOT / "src/xlat12/tests/fpu_count.py"


def lib_sources(extra=()):
    return sorted(str(p) for p in L.SRC.glob("*.c")) + list(extra)


def sdk():
    return subprocess.run(["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"], capture_output=True, text=True,
                          check=True).stdout.strip()


def cmd_regs():
    for tool in ("gen_regs.py", "gen_dmub_layout.py", "gen_modes.py", "gen_census.py", "gen_allow.py"):
        r = L.run([sys.executable, str(L.HERE / tool), "--check"], f"{tool} --check")
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip())
        L.check(r, f"{tool} --check")


def cmd_unit(src_dir=None, tag="unit"):
    src_dir = pathlib.Path(src_dir) if src_dir else L.SRC
    out = L.BUILD / tag
    out.mkdir(parents=True, exist_ok=True)
    rc = 0
    for test in ("test_dcn41", "test_dcn41_dmub", "test_dcn41_modes", "test_dcn41_allow",
                 "test_dcn41_otg_timing"):
        exe = out / test
        srcs = sorted(str(p) for p in src_dir.glob("*.c")) + [str(L.SRC / f"tests/{test}.c")]
        r = L.run([L.CLANG, "-std=c11", "-O1", "-g", *WARN, *SAN, f"-I{src_dir}", *srcs, "-o", str(exe)],
                  f"{tag} {test} compile", log=out / f"{test}-compile.log")
        L.check(r, f"{tag} {test} compile")
        r = L.run([str(exe)], f"{tag} {test} run", log=out / f"{test}-run.log")
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "(no output)")
        rc |= r.returncode
    return rc


def cmd_allowcontrol():
    """Positive control for the allowlist test suite: plant defects, check the suite catches them.

    A guard whose tests always pass is indistinguishable from a guard that permits everything, so the
    allowlist is not trusted until its own tests have been shown to FAIL on a broken table. Two
    mutants, each applied to a private copy of src/dcn41 in $DCN41_BUILD (never the repo):

      1. widen one display range so that it swallows the SMU mailbox at 0x16282;
      2. delete the hard-deny entry, which re-opens the SMN indirect windows.
    """
    import re as _re, shutil
    out = L.BUILD / "allowctl"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    hdr_name = "dcn41_allow_ranges.h"
    mutants = {
        # widen the LAST allowed range (found by pattern, not by name) until it swallows the SMU
        # mailbox at 0x16282 - the exact mistake the allowlist exists to make impossible
        "swallow-smu": lambda t: _re.sub(
            r'(\{ 0x[0-9a-f]{8}u, )0x[0-9a-f]{8}u(, \du, "[^"]*" \},(?!.*\n.*\{ 0x))',
            r'\g<1>0x00020000u\g<2>', t, count=0, flags=_re.S),
        # move the hard-deny window off the low BAR5 page, re-opening MM_INDEX and the NBIO
        # PCIE_INDEX/RSMU pairs - the SMN escape hatch
        "no-hard-deny": lambda t: t.replace("{ 0x00000000u, 0x000000ffu, \"the low BAR5 page",
                                            "{ 0xfffffffeu, 0xffffffffu, \"the low BAR5 page"),
    }
    caught = 0
    for tag, mutate in mutants.items():
        d = out / tag
        d.mkdir(parents=True, exist_ok=True)
        for f in list(L.SRC.glob("*.c")) + list(L.SRC.glob("*.h")):
            shutil.copy(f, d / f.name)
        (d / "tests").mkdir(exist_ok=True)
        shutil.copy(L.SRC / "tests/test_dcn41_allow.c", d / "tests/test_dcn41_allow.c")
        orig = (d / hdr_name).read_text()
        mutated = mutate(orig)
        if mutated == orig:
            raise SystemExit(f"allowcontrol: mutant '{tag}' changed nothing - the planted defect missed")
        (d / hdr_name).write_text(mutated)
        exe = d / "test_allow"
        r = L.run([L.CLANG, "-std=c11", "-O1", "-g", *WARN, *SAN, f"-I{d}",
                   str(d / "dcn41_allow.c"), str(d / "tests/test_dcn41_allow.c"), "-o", str(exe)],
                  f"allowcontrol {tag} compile", log=d / "compile.log")
        L.check(r, f"allowcontrol {tag} compile")
        r = L.run([str(exe)], f"allowcontrol {tag} run", log=d / "run.log")
        last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "(no output)"
        if r.returncode == 0:
            print(f"allowcontrol: mutant '{tag}' was NOT caught: {last}")
        else:
            caught += 1
            print(f"allowcontrol: mutant '{tag}' CAUGHT -> {last}")
    print(f"allowcontrol: {caught} of {len(mutants)} planted defects caught")
    if caught != len(mutants):
        raise SystemExit("allowcontrol: FAILED - the allowlist tests do not catch a broken table")


def cmd_cxx():
    out = L.BUILD / "cxx"
    out.mkdir(parents=True, exist_ok=True)
    for s in lib_sources():
        o = out / (pathlib.Path(s).stem + ".o")
        r = L.run([L.CLANG, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-fno-rtti",
                   f"-I{L.SRC}", "-c", s, "-o", str(o)], f"cxx {pathlib.Path(s).name}", log=out / (o.stem + ".log"))
        L.check(r, f"cxx {s}")
    print(f"cxx: {len(lib_sources())} files compile as C++17 with -Werror")


def cmd_kextcheck():
    out = L.BUILD / "kext"
    out.mkdir(parents=True, exist_ok=True)
    kflags = ["-arch", "x86_64", "-target", "x86_64-apple-macos11.0", "-isysroot", sdk(), f"-I{MKSDK}/Headers",
              f"-I{L.SRC}", "-mmacosx-version-min=11.0", "-DKERNEL", "-DKERNEL_PRIVATE", "-DDRIVER_PRIVATE", "-DAPPLE",
              "-DNeXT", "-D_FORTIFY_SOURCE=0", "-nostdinc", "-fno-builtin", "-fno-common", "-fno-stack-protector",
              "-mkernel", "-fapple-kext", "-Wall", "-Werror", "-Os", "-g"]
    objs = []
    for lang, extra in (("c", ["-std=c11"]), ("c++", ["-x", "c++", "-std=c++17", "-fno-exceptions", "-fno-rtti"])):
        for s in lib_sources():
            o = out / f"{pathlib.Path(s).stem}.{'cc' if lang == 'c++' else 'c'}.o"
            r = L.run([L.CLANG, *kflags, *extra, "-c", s, "-o", str(o)], f"kext {lang} {pathlib.Path(s).name}",
                      log=out / (o.name + ".log"))
            L.check(r, f"kext {lang} {s}")
            objs.append(o)
    undef = []
    for suffix in ("c", "cc"):          # merge the module's objects (ld -r) so cross-file references resolve
        group = [str(o) for o in objs if o.name.endswith(f".{suffix}.o")]
        merged = out / f"dcn41_{suffix}_merged.o"
        r = L.run(["/usr/bin/ld", "-r", "-arch", "x86_64", *group, "-o", str(merged)], f"ld -r {suffix}",
                  log=out / f"ld_{suffix}.log")
        L.check(r, f"ld -r {suffix}")
        r = L.run(["/usr/bin/nm", "-u", str(merged)], f"nm {merged.name}")
        undef += [f"{merged.name}: {u}" for u in r.stdout.split()]
        r = L.run(["/usr/bin/nm", "-gU", str(merged)], f"nm -gU {merged.name}")
        print(f"kextcheck: {merged.name} exports {len(r.stdout.split(chr(10))) - 1} symbols")
    print(f"kextcheck: {len(objs)} x86_64 kernel objects; undefined symbols after ld -r: {undef if undef else 'none'}")
    r = L.run([sys.executable, str(FPU_COUNT), OBJDUMP, *map(str, objs)], "fpu_count")
    L.check(r, "fpu_count")
    n = r.stdout.strip()
    print(f"kextcheck: SSE/x87 instructions: {n}")
    # the generated mode table is data only: compile it once under the same kernel flags
    modes_c = out / "modes_tu.c"
    modes_c.write_text('#include "dcn41.h"\n#include "dcn41_modes.h"\n'
                       'unsigned dcn41_mode_count(void);\nunsigned dcn41_mode_count(void) { return DCN41_MODE_COUNT; }\n')
    r = L.run([L.CLANG, *kflags, "-std=c11", "-c", str(modes_c), "-o", str(out / "modes_tu.o")], "kext modes table",
              log=out / "modes_tu.log")
    L.check(r, "kext modes table")
    print("kextcheck: the generated mode table compiles as x86_64 kernel code")

    # positive control for the counter: the same flags on a file that does use double arithmetic
    ctl = out / "fpu_control.c"
    ctl.write_text("double dcn41_fpu_control(double a, unsigned b);\n"
                   "double dcn41_fpu_control(double a, unsigned b) { return a * 1.5 + (double)b; }\n")
    r = L.run([L.CLANG, *kflags, "-std=c11", "-c", str(ctl), "-o", str(out / "fpu_control.o")], "kext fpu control",
              log=out / "fpu_control.log")
    if r.returncode == 0:
        rc = L.run([sys.executable, str(FPU_COUNT), OBJDUMP, str(out / "fpu_control.o")], "fpu_count control")
        print(f"kextcheck: positive control (double arithmetic, same flags) counts {rc.stdout.strip()} SSE/x87 "
              f"instructions")
        if rc.stdout.strip() in ("", "0"):
            raise SystemExit("kextcheck: the SSE/x87 counter missed the positive control: FAILED")
    else:
        print("kextcheck: positive control: the kernel flags reject double arithmetic outright:",
              r.stderr.strip().splitlines()[-1] if r.stderr.strip() else f"rc={r.returncode}")
    r = L.run(["/usr/bin/lipo", "-info", str(objs[0])], "lipo")
    print("kextcheck:", r.stdout.strip())
    if undef or n != "0":
        raise SystemExit("kextcheck: FAILED")


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("regs", "all"):
        cmd_regs()
    if what in ("unit", "all"):
        if cmd_unit():
            raise SystemExit("unit: FAILED")
    if what in ("allowcontrol", "all"):
        cmd_allowcontrol()
    if what in ("cxx", "all"):
        cmd_cxx()
    if what in ("kextcheck", "all"):
        cmd_kextcheck()
    return 0


if __name__ == "__main__":
    sys.exit(main())
