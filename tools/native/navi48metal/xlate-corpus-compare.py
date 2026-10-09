#!/usr/bin/env python3
"""Compares test-xlate-corpus output (OUT/<sha>.spv, .meta.json, .fail, results.tsv) with the daemon reference (CORPUS/ref/<sha>.spv, .meta.json).
usage: xlate-corpus-compare.py CORPUS OUT [LL_OUT]
Classes: identical (spv bytes equal and meta JSON-equal), spv-different, meta-different, refused-by-both, failed (the daemon translated, we did not), extra (we translated, the daemon refused).
With LL_OUT (the reader-only pass) also counts how many of the reader's texts equal the n48-llvm-dis text (CORPUS/<sha>.clang.ll) after dropping the ModuleID comment and the triple/datalayout lines.
Exit 0 only when every row is identical or refused by both."""
import json, os, sys
corpus, out = sys.argv[1], sys.argv[2]
llout = sys.argv[3] if len(sys.argv) > 3 else None
rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(corpus, "manifest.tsv"))]
res = {}
for l in open(os.path.join(out, "results.tsv")):
    p = l.rstrip("\n").split("\t")
    if len(p) >= 2: res[p[0]] = (int(p[1]), p[2] if len(p) > 2 else "")
def rd(p):
    try: return open(p, "rb").read()
    except OSError: return None
cnt = {"identical": 0, "spv-different": 0, "meta-different": 0, "refused-by-both": 0, "failed": 0, "extra": 0}
bad = []
byset = {}
for r in rows:
    sha, name = r[0], r[1]
    rs, rm = rd(f"{corpus}/ref/{sha}.spv"), rd(f"{corpus}/ref/{sha}.meta.json")
    os_, om = rd(f"{out}/{sha}.spv"), rd(f"{out}/{sha}.meta.json")
    k = None
    if rs is not None and os_ is not None:
        if rs != os_: k = "spv-different"
        elif json.loads(rm) != json.loads(om or b"null"): k = "meta-different"
        else: k = "identical"
    elif rs is None and os_ is None: k = "refused-by-both"
    elif rs is not None: k = "failed"
    else: k = "extra"
    cnt[k] += 1
    st = byset.setdefault(r[2] if len(r) > 2 else "?", {}); st[k] = st.get(k, 0) + 1
    if k not in ("identical", "refused-by-both"): bad.append((k, sha, name, str(res.get(sha)) if k == "failed" else ""))
print("T3 %s: " % os.path.basename(out.rstrip("/")) + ", ".join(f"{k} {v}" for k, v in cnt.items()) + f"  (of {len(rows)})")
for sname, st in sorted(byset.items()): print("   set %-14s " % sname + ", ".join(f"{k} {v}" for k, v in sorted(st.items())))
for b in bad[:int(os.environ.get("N48X_SHOW", "8"))]: print("  ", *[str(x)[:200] for x in b])
if bad:
    from collections import Counter
    sig = Counter((b[3][:90] if b[0] == "failed" else b[0]) for b in bad)
    print("   causes:", "; ".join(f"{n} x {k}" for k, n in sig.most_common(6)))
sc = os.environ.get("N48X_SPVCACHE")
if sc:   # the bundle's shipped spvcache is a second reference for the rows of the spvcache set
    n = same = diff = nofile = 0
    for r in rows:
        if len(r) < 3 or r[2] != "spvcache": continue
        a, b = rd(f"{sc}/{r[0]}.spv"), rd(f"{out}/{r[0]}.spv")
        n += 1
        if b is None: nofile += 1
        elif a == b: same += 1
        else: diff += 1
    print(f"   vs the SHIPPED spvcache bytes ({n} rows): identical {same}, different {diff}, we produced none {nofile}")
if llout:
    same = diff = missing = 0; first = []
    def norm(t):
        return "\n".join(l for l in t.splitlines() if not l.startswith(("; ModuleID", "target triple", "target datalayout", "source_filename")))
    for r in rows:
        sha = r[0]; a = rd(f"{llout}/{sha}.ll"); b = rd(f"{corpus}/{sha}.clang.ll")
        if a is None or b is None: missing += 1; continue
        if norm(a.decode("utf-8", "replace")) == norm(b.decode("utf-8", "replace")): same += 1
        else:
            diff += 1
            if len(first) < 3: first.append(sha)
    print(f"   reader text vs n48-llvm-dis text (header lines dropped): same {same}, different {diff}, reader gave none {missing}")
    for s in first: print("    text differs:", s)
sys.exit(0 if cnt["spv-different"] + cnt["meta-different"] + cnt["failed"] + cnt["extra"] == 0 else 1)
