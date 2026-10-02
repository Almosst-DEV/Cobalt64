#!/usr/bin/env python3
"""Install the census OK translations into spvcache/<sha256(bitcodeData)>.spv (native #11 step 11e).

usage: fill-spvcache.py [census2 dir, default ~/navi48-native/census2]
Libraries: SkyLight (SkyLightShaders.air64), QuartzCore (AIR slice), CoreDisplay, CoreImage default.
Every entry is checked: sha256 of the extracted .air blob must equal the results.tsv key (which the PC measured equal
to sha256(-[MTLFunction bitcodeData]) for 275/275 functions, NATIVE-S4-SHADER-CENSUS Part A), the .spv must start
with the SPIR-V magic, and two libraries sharing a key must have byte-identical .spv.
No sidecar is installed: the bundle reflects the SPIR-V (descriptor sets, push constants, vertex inputs, outputs) itself.
"""
import hashlib, os, shutil, sys
root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/navi48-native/census2")
libs = ["SkyLight__SkyLightShaders.air64", "QuartzCore__default", "CoreDisplay__default", "CoreImage__default"]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spvcache")
seen = {}; n = 0; dup = 0
for lib in libs:
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(root, "res", lib, "results.tsv"))][1:]
    for r in rows:
        f, name, stage, size, sha, cls = r[:6]
        if cls != "OK": continue
        air = os.path.join(root, "air", lib, f)
        spv = os.path.join(root, "res", lib, "spv", f.replace(".air", ".spv"))
        got = hashlib.sha256(open(air, "rb").read()).hexdigest()
        assert got == sha, f"{lib}/{f}: air sha {got} != results {sha}"
        b = open(spv, "rb").read()
        assert len(b) >= 20 and len(b) % 4 == 0 and b[:4] == b"\x03\x02\x23\x07", f"{spv}: not SPIR-V"
        if sha in seen:
            assert seen[sha] == b, f"{lib}/{f}: key {sha} has two different .spv"
            dup += 1; continue
        seen[sha] = b
        open(os.path.join(out, sha + ".spv"), "wb").write(b); n += 1
print(f"installed {n} new spvcache entries ({dup} duplicate keys skipped); spvcache now {len([x for x in os.listdir(out) if x.endswith('.spv')])} files")
