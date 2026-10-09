#!/usr/bin/env python3
"""Translate ONE dumped AIR file on the PC and install it into the side spvcache (port of tools/native/navi48metal/add-air.py).

usage: pc-translate.py <sha.air> [--out DIR] [--m2v PATH] [--overrides FILE] [--validator PATH] [--timeout SECONDS] [--max-bytes N] [--max-rss-kb N]
Pipeline: n48-llvm-dis (clang) -> metal2vulkan --emit-meta [--local NAME=X,Y,Z] -> storage-image format patch -> trimmed meta.
Installs <sha>.meta.json first, then <sha>.spv (each via temp name + rename, chmod 644). metal2vulkan hard-requires a spirv-val: METAL2VULKAN_SPIRV_VAL is --validator (a real x86_64 spirv-val when installed next to this script, else n48-spirv-val-stub, which is always
"valid"; with the stub, equivalence with the host-side validated output rests on byte-identity, see NATIVE-S4-METAL-GAPS.md).
C1 limits: the memory cap is a WATCHDOG (--max-rss-kb, default 1 GiB, polled every 0.2 s over the translator's whole process group with ps): setrlimit RLIMIT_AS / RLIMIT_DATA are accepted but not enforced on macOS
(measured: a 2 GiB allocation succeeds under ulimit -v 1048576). Input larger than --max-bytes (default 4 MiB) or empty is refused; the translator runs in its own process group and is killed after --timeout seconds (default 20); a SIGALRM backstop
kills everything after timeout+10 s.
Exit 0 and prints "OK sha name bytes" on success; prints "FAIL ..." and exits 1 otherwise.
"""
import glob, hashlib, json, os, signal, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[1:]
f = a[0]; out = "/private/var/tmp/n48m-spv"; m2v = os.path.join(HERE, "metal2vulkan"); ovf = os.path.join(HERE, "overrides.txt")
validator = os.path.join(HERE, "n48-spirv-val-stub"); timeout = 20.0; maxb = 4 * 1024 * 1024; max_rss_kb = 1048576
i = 1
while i < len(a):
    if a[i] == "--out": out = a[i + 1]
    elif a[i] == "--m2v": m2v = a[i + 1]
    elif a[i] == "--overrides": ovf = a[i + 1]
    elif a[i] == "--validator": validator = a[i + 1]
    elif a[i] == "--timeout": timeout = float(a[i + 1])
    elif a[i] == "--max-bytes": maxb = int(a[i + 1])
    elif a[i] == "--max-rss-kb": max_rss_kb = int(a[i + 1])
    i += 2
env = dict(os.environ); env["METAL2VULKAN_LLVM_DIS"] = os.path.join(HERE, "n48-llvm-dis"); env["METAL2VULKAN_SPIRV_VAL"] = validator
locs = {}
if os.path.exists(ovf):
    for l in open(ovf):
        l = l.strip()
        if l and not l.startswith("#") and "=" in l: k, v = l.split("=", 1); locs[k.strip()] = v.strip()

def trim(m):
    b = []
    for x in m.get("bindings", []):
        b.append({k: x.get(k) for k in ("kind", "metal_index", "descriptor", "param_index", "address_space", "access", "type_name", "static_sampler", "texture_shape") if k in x})
    return {"trimmed_from_reflection_version": m.get("reflection_version"), "stage": m.get("stage"), "entry_point": m.get("entry_point"), "bindings": b,
            "vertex_attributes": m.get("vertex_attributes"), "local_size": m.get("local_size"), "max_work_group_size": m.get("max_work_group_size"),
            "kernel_dispatch": m.get("kernel_dispatch"), "function_constants": m.get("function_constants"), "descriptor_layout": m.get("descriptor_layout"),
            "runtime_sampler_specializations": m.get("runtime_sampler_specializations"), "runtime_storage_image_specializations": m.get("runtime_storage_image_specializations")}

def patch_storage_formats(b, meta):
    w = list(struct.unpack("<%dI" % (len(b) // 4), b)); p = 5; need = set(); patched = 0; lastcap = None; have = set()
    acc = {x.get("access") for x in meta.get("bindings", []) if x.get("kind") == "StorageImage"}
    while p < len(w):
        op, wc = w[p] & 0xffff, w[p] >> 16
        if op == 17: lastcap = p + wc; have.add(w[p + 1])
        if op == 25 and w[p + 7] == 2 and w[p + 8] != 0:
            w[p + 8] = 0; patched += 1
            if "ReadOnly" in acc or "ReadWrite" in acc or not acc: need.add(55)
            if "WriteOnly" in acc or "ReadWrite" in acc or not acc: need.add(56)
        p += wc
    if not patched: return b, 0
    if not need: need = {55, 56}
    ins = []
    for c in sorted(need - have): ins += [(2 << 16) | 17, c]
    w[lastcap:lastcap] = ins
    return struct.pack("<%dI" % len(w), *w), patched

def install(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as fh: fh.write(data)
    os.chmod(tmp, 0o644); os.rename(tmp, path)

def group_rss_kb(pgid):   # resident KiB of every process in the group; None when ps is unavailable (then only the time limit applies)
    try:
        o = subprocess.run(["/bin/ps", "-axo", "pgid=,rss="], capture_output=True, text=True, timeout=2).stdout
        return sum(int(r) for g, r in (l.split() for l in o.splitlines() if len(l.split()) == 2) if g.isdigit() and int(g) == pgid and r.isdigit())
    except Exception: return None
_child = [None]
def _alarm(*_):   # backstop: kill the translator's own process group (never ours: we share a group with the daemon's shell)
    try:
        if _child[0]: os.killpg(_child[0], signal.SIGKILL)
    except Exception: pass
    os._exit(3)
signal.signal(signal.SIGALRM, _alarm); signal.alarm(int(timeout) + 10)
try: sz = os.lstat(f)
except OSError as e: print(f"FAIL cannot stat input: {e}"); sys.exit(1)
import stat as _st
if not _st.S_ISREG(sz.st_mode): print("FAIL input is not a regular file"); sys.exit(1)
if sz.st_size < 1 or sz.st_size > maxb: print(f"FAIL input size {sz.st_size} outside 1..{maxb}"); sys.exit(1)
data = open(f, "rb").read(maxb + 1)
if len(data) > maxb: print("FAIL input grew past the size cap"); sys.exit(1)
if data[:4] != b"BC\xc0\xde" and data[:4] != b"\xde\xc0\x17\x0b": print("FAIL not LLVM bitcode (no BC C0DE / wrapper magic)"); sys.exit(1)
sha = hashlib.sha256(data).hexdigest()
base = os.path.splitext(f)[0]
name = ""
for side in sorted(glob.glob(base + ".*.json")):
    try: name = json.load(open(side)).get("function", ""); break
    except Exception: pass
if not name and os.path.exists(base + ".txt"): name = open(base + ".txt").read().split()[0]
with tempfile.TemporaryDirectory(prefix="n48tr-") as td:
    spv = os.path.join(td, "o.spv"); js = os.path.join(td, "o.json")
    cmd = [m2v, f, spv, "--emit-meta", js]
    if name in locs: cmd += ["--local", locs[name]]
    pr = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, start_new_session=True)
    _child[0] = pr.pid
    import time
    t0 = time.monotonic(); why = None
    while True:
        try: so, se = pr.communicate(timeout=0.2); break
        except subprocess.TimeoutExpired:
            if time.monotonic() - t0 > timeout: why = f"timeout after {timeout:g} s"
            else:
                rss = group_rss_kb(pr.pid)
                if rss is not None and rss > max_rss_kb: why = f"memory {rss} KiB over the {max_rss_kb} KiB cap"
            if why:
                try: os.killpg(pr.pid, signal.SIGKILL)
                except Exception: pr.kill()
                pr.communicate(); print(f"FAIL {why} {sha} {name}"); sys.exit(1)
    if pr.returncode != 0: print(f"FAIL translate {sha} {name}: {se.strip()[-300:]}"); sys.exit(1)
    b = open(spv, "rb").read()
    if b[:4] != b"\x03\x02\x23\x07": print(f"FAIL bad magic {sha} {name}"); sys.exit(1)
    meta = json.load(open(js))
b, np_ = patch_storage_formats(b, meta)
m2 = trim(meta)
if np_: m2["storage_image_format_patch"] = "Unknown + StorageImage{Read,Write}WithoutFormat"
os.makedirs(out, exist_ok=True)
install(os.path.join(out, sha + ".meta.json"), json.dumps(m2, separators=(",", ":")).encode())
install(os.path.join(out, sha + ".spv"), b)
print(f"OK {sha} {name} {len(b)} bytes" + (f" [storage formats patched: {np_}]" if np_ else ""))
