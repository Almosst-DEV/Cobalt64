#!/usr/bin/env python3
"""Build the T2/T3 corpus dir: sha.air + sha.txt (function name) copies, manifest.tsv (sha, name, set, expect), and the
n48-llvm-dis text sha.clang.ll. usage: prep.py OUTDIR SPVCACHE_DIR CENSUS2 [N_CENSUS] [N_FAIL]"""
import glob, hashlib, os, shutil, subprocess, sys
out, spvc, census = sys.argv[1], sys.argv[2], os.path.expanduser(sys.argv[3])
ncen = int(sys.argv[4]) if len(sys.argv) > 4 else 1000
nfail = int(sys.argv[5]) if len(sys.argv) > 5 else 100
os.makedirs(out, exist_ok=True)
cache = {os.path.basename(f)[:-4] for f in glob.glob(os.path.join(spvc, "*.spv"))}
rows = []   # (lib, file, name, size, sha, cls)
for tsv in sorted(glob.glob(os.path.join(census, "res", "*", "results.tsv"))):
    lib = os.path.basename(os.path.dirname(tsv))
    for l in list(open(tsv))[1:]:
        r = l.rstrip("\n").split("\t")
        if len(r) < 6 or r[5] not in ("OK", "TRANSLATE_FAIL") or r[0] == "class": continue
        rows.append((lib, r[0], r[1], int(r[3]), r[4], r[5], r[8] if len(r) > 8 else ""))
by_sha = {}
for r in rows: by_sha.setdefault(r[4], r)
chosen = []; seen = set()
missing = sorted(cache - set(by_sha))
for sha in sorted(cache & set(by_sha)):
    chosen.append((by_sha[sha], "spvcache")); seen.add(sha)
ok = [r for r in rows if r[5] == "OK" and r[4] not in seen and r[3] <= 4 * 1024 * 1024]
fl = [r for r in rows if r[5] == "TRANSLATE_FAIL" and r[4] not in seen and r[3] <= 4 * 1024 * 1024]
def pick(lst, n):
    uniq = {}
    for r in lst: uniq.setdefault(r[4], r)
    u = sorted(uniq.values(), key=lambda r: r[4])
    if len(u) <= n: return u
    step = len(u) / n
    return [u[int(i * step)] for i in range(n)]
for r in pick(ok, ncen): chosen.append((r, "census2-ok")); seen.add(r[4])
# every post-validation reject (they exercise the Skip path's two rejects) plus a spread of the rest
rej = [r for r in fl if "write survived folding" in r[6] or "imageblock cell" in r[6]]
rest = [r for r in fl if r not in rej]
for r in rej + pick([r for r in rest if "no !air" not in r[6]], nfail // 2) + pick([r for r in rest if "no !air" in r[6]], nfail // 2):
    if r[4] not in seen: chosen.append((r, "census2-fail")); seen.add(r[4])
extra = os.environ.get("N48_EXTRA_TSV")
if extra:
    for l in open(extra):
        sha, path = l.rstrip("\n").split("\t")
        if sha in seen: continue
        txt = os.path.splitext(path)[0] + ".txt"
        name = ""
        if os.path.exists(txt):
            t = open(txt).read().split()
            name = t[0] if t else ""
        d = open(path, "rb").read(); assert hashlib.sha256(d).hexdigest() == sha
        chosen.append((("extra", os.path.basename(path), name, len(d), sha, "OK?", ""), "spvcache")); seen.add(sha)
here = os.path.dirname(os.path.abspath(__file__)); 
man = open(os.path.join(out, "manifest.tsv"), "w")
extrapath = {l.split("\t")[0]: l.rstrip("\n").split("\t")[1] for l in open(extra)} if extra else {}
for (lib, f, name, size, sha, cls, *_), st in chosen:
    src = extrapath[sha] if lib == "extra" else os.path.join(census, "air", lib, f)
    d = open(src, "rb").read()
    assert hashlib.sha256(d).hexdigest() == sha, src
    open(os.path.join(out, sha + ".air"), "wb").write(d)
    open(os.path.join(out, sha + ".txt"), "w").write(name + "\n")
    man.write(f"{sha}\t{name}\t{st}\t{cls}\t{len(d)}\n")
man.close()
print("spvcache entries:", len(cache), "with a census AIR:", len(cache & set(by_sha)), "without:", len(missing), "found elsewhere:", len(extrapath), "unresolved:", len(set(missing)-set(extrapath)))
open(os.path.join(out, "spvcache-missing.txt"), "w").write("\n".join(missing) + "\n")
from collections import Counter
print(Counter(st for _, st in chosen))
