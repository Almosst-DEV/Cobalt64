#!/usr/bin/env python3
"""Translate dumped AIR (mtlprobe dumpair <dir>, or /tmp/n48m/<sha>.air) into spvcache/<sha>.spv + <sha>.meta.json (native #11 step 11e).

usage: add-air.py <air dir or files...> [--local NAME=X,Y,Z ...]
The key is the sha256 of the file content (= sha256 of -[MTLFunction bitcodeData], measured equal for 275/275 functions).
Runs metal2vulkan (navi48-s0-fixes build, --features serde; it spawns llvm-dis itself) with --emit-meta, then an independent
`spirv-val --target-env vulkan1.2`; the .spv is installed only if both pass. The .meta.json is a TRIMMED copy of the
ShaderReflection (stage, entry_point, bindings[kind, metal_index, descriptor, static_sampler], vertex_attributes, local_size,
kernel_dispatch, function_constants): the bundle reads static samplers and the compute dispatch contract from it.
Kernels: metal2vulkan's default is `--local 64,1,1` with ThreadsDynamic dispatch (the safe default); --local NAME=X,Y,Z sets the
nominal threadgroup size for kernels whose AIR name is NAME (the .txt sidecar of dumpair holds the name).
"""
import glob, hashlib, json, os, struct, subprocess, sys
M2V = os.environ.get("M2V", os.path.expanduser("~/navi48-native/metal2vulkan/target/release/metal2vulkan"))
env = dict(os.environ); env["PATH"] = "/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:" + env.get("PATH", "")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spvcache")
args = sys.argv[1:]; locs = {}; files = []
i = 0
while i < len(args):
    if args[i] == "--local": k, v = args[i + 1].split("="); locs[k] = v; i += 2
    elif os.path.isdir(args[i]): files += sorted(glob.glob(os.path.join(args[i], "*.air"))); i += 1
    else: files.append(args[i]); i += 1
def trim(m):
    b = []
    for x in m.get("bindings", []):
        b.append({k: x.get(k) for k in ("kind", "metal_index", "descriptor", "param_index", "address_space", "access", "type_name", "static_sampler", "texture_shape") if k in x})
    return {"trimmed_from_reflection_version": m.get("reflection_version"), "stage": m.get("stage"), "entry_point": m.get("entry_point"), "bindings": b,
            "vertex_attributes": m.get("vertex_attributes"), "local_size": m.get("local_size"), "max_work_group_size": m.get("max_work_group_size"),
            "kernel_dispatch": m.get("kernel_dispatch"), "function_constants": m.get("function_constants"), "descriptor_layout": m.get("descriptor_layout"),
            "runtime_sampler_specializations": m.get("runtime_sampler_specializations"), "runtime_storage_image_specializations": m.get("runtime_storage_image_specializations")}
def patch_storage_formats(b, meta):
    """Storage images: metal2vulkan picks a concrete format (R32f for float) because AIR does not know the runtime format
    (REFLECTION.md, "Runtime storage-image specialization"). The bundle binds whatever format the MTLTexture has, so declare
    the image `Unknown` and add the StorageImage{Read,Write}WithoutFormat capabilities the module's accesses need: this is the
    module metal2vulkan produces when it is told a runtime format with no exact SPIR-V token and the device supports
    read/write-without-format (RADV does). Returns (bytes, patched image types)."""
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
    if not need: need = {55, 56}   # access unknown ("Storage"): both
    ins = []
    for c in sorted(need - have): ins += [(2 << 16) | 17, c]
    w[lastcap:lastcap] = ins
    return struct.pack("<%dI" % len(w), *w), patched
n = 0
for f in files:
    sha = hashlib.sha256(open(f, "rb").read()).hexdigest()
    txt = os.path.splitext(f)[0] + ".txt"
    name = open(txt).read().split()[0] if os.path.exists(txt) else ""
    spv = f"/tmp/add-air-{sha}.spv"; js = f"/tmp/add-air-{sha}.json"
    cmd = [M2V, f, spv, "--emit-meta", js]
    if name in locs: cmd += ["--local", locs[name]]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0: print(f"FAIL translate {sha} {name}: {r.stderr.strip()[-300:]}"); continue
    v = subprocess.run(["spirv-val", "--target-env", "vulkan1.2", spv], capture_output=True, text=True, env=env)
    if v.returncode != 0: print(f"FAIL spirv-val {sha} {name}: {v.stdout[-300:]}{v.stderr[-300:]}"); continue
    b = open(spv, "rb").read(); assert b[:4] == b"\x03\x02\x23\x07"
    meta = json.load(open(js))
    b, np_ = patch_storage_formats(b, meta)
    if np_:
        open(spv, "wb").write(b)
        v = subprocess.run(["spirv-val", "--target-env", "vulkan1.2", spv], capture_output=True, text=True, env=env)
        if v.returncode != 0: print(f"FAIL spirv-val after storage-format patch {sha} {name}: {v.stdout[-300:]}{v.stderr[-300:]}"); continue
    dst = os.path.join(out, sha + ".spv")
    note = ""
    if os.path.exists(dst) and open(dst, "rb").read() != b: note = " (DIFFERS from the installed .spv; new one written)"
    open(dst, "wb").write(b)
    m2 = trim(meta)
    if np_: m2["storage_image_format_patch"] = "Unknown + StorageImage{Read,Write}WithoutFormat"
    json.dump(m2, open(os.path.join(out, sha + ".meta.json"), "w"), separators=(",", ":"))
    print(f"OK {sha} {name} {len(b)} bytes{note}" + (f" [storage formats patched: {np_}]" if np_ else "")); n += 1
print(f"installed {n} of {len(files)}")
