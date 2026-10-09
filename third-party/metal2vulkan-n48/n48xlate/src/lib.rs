// SPDX-License-Identifier: LGPL-3.0-or-later
//! n48xlate: the Navi48 in-process shader translator.
//!
//! One C-ABI entry point turns the text form of an Apple AIR module (already disassembled by the
//! caller with the operating system's own LLVM) into Vulkan SPIR-V plus the trimmed reflection the
//! Navi48 Metal bundle reads. It is the in-process equivalent of the daemon's pipeline
//! (`n48-llvm-dis`, `metal2vulkan --emit-meta [--local NAME=X,Y,Z]`, `patch_storage_formats`, `trim`),
//! minus every file and every process: a sandboxed application cannot start `spirv-val` or write a
//! scratch directory, and this code never tries to.
//!
//! The external `spirv-val` gate is skipped (see `metal2vulkan::Validation`). The daemon already runs
//! with an always-pass validator on the machine this serves, so the bytes are unchanged; the two
//! structural rejects that follow the validator still run.
//!
//! Every exported function is wrapped in `catch_unwind` and returns a code, never unwinds across the
//! C boundary. Return codes of `n48x_translate`: 0 ok, 1 refused (the translator said no), 2 panic
//! caught, 3 bad arguments.

use metal2vulkan::passes::{Stage, TransformOptions};
use metal2vulkan::reflect::ShaderReflection;
use metal2vulkan::tools::sanitize_ll_text_with_datalayout;
use serde_json::Value;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

/// Version of the C ABI below. The caller refuses a library whose version it does not know.
/// Version 2 adds `n48x_translate_linked` (direct visible-function references resolved to the exact authored
/// dependencies before translation); `n48x_translate` is unchanged.
pub const ABI_VERSION: u32 = 2;
/// Largest LLVM text accepted (the caller caps it too).
pub const MAX_LL_BYTES: usize = 64 * 1024 * 1024;

pub const RC_OK: c_int = 0;
pub const RC_REFUSED: c_int = 1;
pub const RC_PANIC: c_int = 2;
pub const RC_BAD_ARGS: c_int = 3;

/// The ABI version.
#[no_mangle]
pub extern "C" fn n48x_abi_version() -> u32 {
    ABI_VERSION
}

/// Free a buffer returned through `n48x_translate`. `len` must be the length it reported.
///
/// # Safety
/// `ptr`/`len` must come from `n48x_translate` and be freed once.
#[no_mangle]
pub unsafe extern "C" fn n48x_free(ptr: *mut u8, len: usize) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !ptr.is_null() {
            drop(Vec::from_raw_parts(ptr, len, len));
        }
    }));
}

/// Translate AIR text.
///
/// `ll`/`ll_len`: the module text as `llvm-dis` prints it. `local_xyz`: NULL, or three non-zero
/// `u32`s, the nominal kernel threadgroup size (the daemon's `--local NAME=X,Y,Z`). On success
/// `*spv`/`*spv_len` and `*meta`/`*meta_len` hold buffers to release with `n48x_free`. On a non-zero
/// return `err` (capacity `err_len`) holds a NUL-terminated message and the outputs are zeroed.
///
/// # Safety
/// All pointers must be valid for their stated lengths; the out pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn n48x_translate(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| translate_bytes(bytes, local)));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

/// One authored linked dependency: the logical Metal function name and the LLVM text of the AIR
/// module that defines it (as `llvm-dis` prints it; this library sanitises it).
#[repr(C)]
pub struct N48xDep {
    pub symbol: *const u8,
    pub symbol_len: usize,
    pub ll: *const u8,
    pub ll_len: usize,
}

/// Largest number of dependencies one call accepts, and the largest total LLVM text of the entry plus them.
pub const MAX_DEPS: usize = 256;

/// Translate AIR text whose direct visible-function references (`.MTL_VISIBLE_FN_REF`) are resolved
/// to the exact authored dependencies in `deps` first (the portable alternative to Metal's runtime
/// function linker; see `metal2vulkan::specialize_linked_module`). Everything else is as
/// `n48x_translate`. Each dependency is sanitised like the entry. Dependencies the entry does not
/// reach (transitively) are ignored, so a caller may pass every function the pipeline links; a
/// reference with no authored dependency is a refusal (code 1) naming it. An entry that references
/// nothing is translated exactly as `n48x_translate` would.
///
/// # Safety
/// All pointers must be valid for their stated lengths (`deps` for `ndeps` entries); the out pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn n48x_translate_linked(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    deps: *const N48xDep,
    ndeps: usize,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
        || (ndeps > 0 && deps.is_null())
        || ndeps > MAX_DEPS
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let mut total = ll_len;
    let mut owned: Vec<(String, Vec<u8>)> = Vec::with_capacity(ndeps);
    for i in 0..ndeps {
        let d = &*deps.add(i);
        if d.symbol.is_null() || d.symbol_len == 0 || d.ll.is_null() || d.ll_len == 0 {
            put_err(err, err_len, "bad arguments: empty dependency");
            return RC_BAD_ARGS;
        }
        total = total.saturating_add(d.ll_len);
        if total > MAX_LL_BYTES {
            put_err(err, err_len, "bad arguments: dependencies too large");
            return RC_BAD_ARGS;
        }
        let sym = match std::str::from_utf8(std::slice::from_raw_parts(d.symbol, d.symbol_len)) {
            Ok(x) => x.to_string(),
            Err(_) => {
                put_err(err, err_len, "bad arguments: dependency symbol is not UTF-8");
                return RC_BAD_ARGS;
            }
        };
        owned.push((sym, std::slice::from_raw_parts(d.ll, d.ll_len).to_vec()));
    }
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| translate_bytes_linked(bytes, local, &owned)));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

#[allow(clippy::type_complexity)]
unsafe fn finish(
    outcome: std::thread::Result<Result<(Vec<u8>, Vec<u8>), String>>,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    match outcome {
        Ok(Ok((s, m))) => {
            let (sp, sl) = leak(s);
            let (mp, ml) = leak(m);
            *spv = sp;
            *spv_len = sl;
            *meta = mp;
            *meta_len = ml;
            RC_OK
        }
        Ok(Err(e)) => {
            put_err(err, err_len, &e);
            RC_REFUSED
        }
        Err(p) => {
            let why = if let Some(s) = p.downcast_ref::<&str>() {
                (*s).to_string()
            } else if let Some(s) = p.downcast_ref::<String>() {
                s.clone()
            } else {
                "unknown panic".to_string()
            };
            put_err(err, err_len, &format!("panic: {why}"));
            RC_PANIC
        }
    }
}

fn leak(mut v: Vec<u8>) -> (*mut u8, usize) {
    v.shrink_to_fit();
    let len = v.len();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v);
    (ptr, len)
}

unsafe fn put_err(dst: *mut c_char, cap: usize, msg: &str) {
    if dst.is_null() || cap == 0 {
        return;
    }
    let n = msg.len().min(cap - 1);
    // Cut on a character boundary so the message stays valid UTF-8.
    let mut n = n;
    while n > 0 && !msg.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(msg.as_ptr(), dst as *mut u8, n);
    *dst.add(n) = 0;
}

/// Pure (no unsafe, no C types) core: text in, SPIR-V and trimmed meta JSON out.
pub fn translate_bytes(ll: &[u8], local: [u32; 3]) -> Result<(Vec<u8>, Vec<u8>), String> {
    #[cfg(feature = "test-hooks")]
    {
        if ll.starts_with(b";N48X-TEST-PANIC") {
            panic!("n48x test hook panic");
        }
        if ll.starts_with(b";N48X-TEST-EXIT") {
            // A deliberate process exit mid-translation (crash-guard fixture).
            std::process::exit(77);
        }
    }
    let text = std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    translate_text(text, local)
}

/// [`translate_bytes`] with authored linked dependencies (`n48x_translate_linked`).
pub fn translate_bytes_linked(
    ll: &[u8],
    local: [u32; 3],
    deps: &[(String, Vec<u8>)],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    #[cfg(feature = "test-hooks")]
    {
        if ll.starts_with(b";N48X-TEST-PANIC") {
            panic!("n48x test hook panic");
        }
    }
    let text = std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    let mut texts = Vec::with_capacity(deps.len());
    for (sym, bytes) in deps {
        let t = std::str::from_utf8(bytes)
            .map_err(|e| format!("linked dependency {sym:?}: LLVM text is not UTF-8: {e}"))?;
        texts.push((sym.as_str(), t));
    }
    translate_text_linked(text, local, &texts)
}

/// `stage` is read from the `!air.*` metadata lines exactly as `metal2vulkan::detect_stage` does.
pub fn detect_stage_text(san_ll: &str) -> Result<Stage, String> {
    if san_ll.contains("!air.vertex =") {
        Ok(Stage::Vertex)
    } else if san_ll.contains("!air.fragment =") {
        Ok(Stage::Fragment)
    } else if san_ll.contains("!air.kernel =") {
        Ok(Stage::Kernel)
    } else {
        Err("metal2vulkan: no !air.vertex/!air.fragment/!air.kernel stage metadata in module"
            .to_string())
    }
}

pub fn translate_text(ll_text: &str, local: [u32; 3]) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, None)
}

/// The authored dependencies the entry reaches, transitively, each sanitised: the exact input of
/// `metal2vulkan::specialize_linked_module`. A symbol authored twice with different modules is an error;
/// a reference nobody authored is left for `specialize_linked_module` to refuse by name.
pub fn linkage_for(
    san_entry: &str,
    deps: &[(&str, &str)],
) -> Result<metal2vulkan::linked_functions::LinkedFunctionLinkage, String> {
    use metal2vulkan::linked_functions::{LinkedFunctionLinkage, LinkedFunctionReference};
    use std::collections::{BTreeMap, BTreeSet};
    let mut sanitized: BTreeMap<&str, String> = BTreeMap::new();
    for (sym, ll) in deps {
        let (s, _) = sanitize_ll_text_with_datalayout(ll);
        match sanitized.get(sym) {
            Some(prev) if *prev != s => {
                return Err(format!("linked function {sym:?} is authored twice with different modules"));
            }
            _ => {
                sanitized.insert(*sym, s);
            }
        }
    }
    let mut need: Vec<String> =
        metal2vulkan::linked_functions::visible_function_reference_symbols(san_entry)?;
    let mut done: BTreeSet<String> = BTreeSet::new();
    let mut linkage = LinkedFunctionLinkage::default();
    while let Some(sym) = need.pop() {
        if !done.insert(sym.clone()) {
            continue;
        }
        let Some(module) = sanitized.get(sym.as_str()) else {
            continue; // specialize_linked_module names it
        };
        need.extend(metal2vulkan::linked_functions::visible_function_reference_symbols(module)?);
        linkage.visible_references.push(LinkedFunctionReference {
            symbol: sym,
            module_ll: module.clone(),
        });
    }
    Ok(linkage)
}

/// [`translate_text`] with authored dependencies: (logical function name, LLVM text of its module).
pub fn translate_text_linked(
    ll_text: &str,
    local: [u32; 3],
    deps: &[(&str, &str)],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, Some(deps))
}

/// `deps == None`: the unlinked path, exactly as before ABI 2 (an entry that names a visible function is refused by the translator). `Some`: the linked path.
fn translate_text_with(
    ll_text: &str,
    local: [u32; 3],
    deps: Option<&[(&str, &str)]>,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let (san_entry, datalayout) = sanitize_ll_text_with_datalayout(ll_text);
    let stage = detect_stage_text(&san_entry)?;
    let san_ll = match deps {
        None => san_entry,
        Some(deps) => {
            let linkage = linkage_for(&san_entry, deps)?;
            if !san_entry.contains(".MTL_VISIBLE_FN_REF")
                && !san_entry.contains("!air.visible_function_references")
                && linkage.is_empty()
            {
                san_entry // nothing to resolve: byte-identical to the unlinked path
            } else {
                metal2vulkan::specialize_linked_module(&san_entry, stage, &linkage)?
            }
        }
    };
    // The same options the command-line tool builds: only the nominal local size and the Metal
    // default vertex-amplification count of 1 are set.
    let options = TransformOptions {
        kernel_local_size: local,
        kernel_dispatch: None,
        raster_sample_count: None,
        vertex_amplification_count: 1,
        ..TransformOptions::default()
    };
    // Never used on the Skip path (it reaches only the scratch-file helpers, which never run).
    let tmp = Path::new("");
    let (spv, reflection) = metal2vulkan::translate_sanitized_native_reflected_unvalidated(
        &san_ll,
        stage,
        tmp,
        options,
        datalayout.as_deref(),
    )?;
    if spv.len() < 20 || spv[..4] != [0x03, 0x02, 0x23, 0x07] {
        return Err("bad SPIR-V magic".to_string());
    }
    // Text round trip, like the daemon's (the command-line tool writes the JSON, Python reads it):
    // `to_value` would widen `f32` fields to `f64` and print them with spurious digits.
    let reflection_text = serde_json::to_string(&reflection).map_err(|e| format!("meta: {e}"))?;
    let reflection_json: Value =
        serde_json::from_str(&reflection_text).map_err(|e| format!("meta: {e}"))?;
    let (spv, patched) = patch_storage_formats(&spv, &reflection_json);
    let meta = trim_meta(&reflection_json, patched);
    Ok((spv, meta))
}

/// Port of `patch_storage_formats` (pc-translate.py): the runtime format of a storage image is not
/// known from AIR, so declare the image `Unknown` and add the StorageImage{Read,Write}WithoutFormat
/// capabilities the module's accesses need. Returns the bytes and the number of image types patched.
pub fn patch_storage_formats(b: &[u8], meta: &Value) -> (Vec<u8>, usize) {
    if b.len() % 4 != 0 || b.len() < 20 {
        return (b.to_vec(), 0);
    }
    let mut w: Vec<u32> = b
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    let mut acc: Vec<&str> = Vec::new();
    if let Some(bs) = meta.get("bindings").and_then(Value::as_array) {
        for x in bs {
            if x.get("kind").and_then(Value::as_str) == Some("StorageImage") {
                if let Some(a) = x.get("access").and_then(Value::as_str) {
                    acc.push(a);
                } else {
                    acc.push("");
                }
            }
        }
    }
    let has = |n: &str| acc.contains(&n);
    let acc_empty = acc.is_empty();
    let mut p = 5usize;
    let mut need: Vec<u32> = Vec::new();
    let mut have: Vec<u32> = Vec::new();
    let mut patched = 0usize;
    let mut lastcap: Option<usize> = None;
    while p < w.len() {
        let op = w[p] & 0xffff;
        let wc = (w[p] >> 16) as usize;
        if wc == 0 {
            // Malformed stream: stop rather than loop.
            break;
        }
        if op == 17 && p + 1 < w.len() {
            lastcap = Some(p + wc);
            have.push(w[p + 1]);
        }
        if op == 25 && p + 8 < w.len() && w[p + 7] == 2 && w[p + 8] != 0 {
            w[p + 8] = 0;
            patched += 1;
            if has("ReadOnly") || has("ReadWrite") || acc_empty {
                if !need.contains(&55) {
                    need.push(55);
                }
            }
            if has("WriteOnly") || has("ReadWrite") || acc_empty {
                if !need.contains(&56) {
                    need.push(56);
                }
            }
        }
        p += wc;
    }
    if patched == 0 {
        return (b.to_vec(), 0);
    }
    if need.is_empty() {
        need = vec![55, 56];
    }
    need.sort_unstable();
    let mut ins: Vec<u32> = Vec::new();
    for c in need {
        if !have.contains(&c) {
            ins.push((2 << 16) | 17);
            ins.push(c);
        }
    }
    if let Some(at) = lastcap {
        if at <= w.len() {
            let tail = w.split_off(at);
            w.extend(ins);
            w.extend(tail);
        }
    }
    let mut out = Vec::with_capacity(w.len() * 4);
    for x in w {
        out.extend_from_slice(&x.to_le_bytes());
    }
    (out, patched)
}

/// Port of `trim` (pc-translate.py), serialised compactly in the daemon's key order.
pub fn trim_meta(m: &Value, patched: usize) -> Vec<u8> {
    let get = |k: &str| m.get(k).cloned().unwrap_or(Value::Null);
    let mut bindings: Vec<Value> = Vec::new();
    if let Some(bs) = m.get("bindings").and_then(Value::as_array) {
        for x in bs {
            let mut o = serde_json::Map::new();
            for k in [
                "kind",
                "metal_index",
                "descriptor",
                "param_index",
                "address_space",
                "access",
                "type_name",
                "static_sampler",
                "texture_shape",
            ] {
                if let Some(v) = x.get(k) {
                    o.insert(k.to_string(), v.clone());
                }
            }
            bindings.push(Value::Object(o));
        }
    }
    let mut out = String::from("{");
    let mut first = true;
    let mut put = |out: &mut String, k: &str, v: &Value| {
        if !first {
            out.push(',');
        }
        first = false;
        out.push_str(&serde_json::to_string(k).unwrap());
        out.push(':');
        out.push_str(&ordered(v));
    };
    put(&mut out, "trimmed_from_reflection_version", &get("reflection_version"));
    put(&mut out, "stage", &get("stage"));
    put(&mut out, "entry_point", &get("entry_point"));
    put(&mut out, "bindings", &Value::Array(bindings));
    for k in [
        "vertex_attributes",
        "local_size",
        "max_work_group_size",
        "kernel_dispatch",
        "function_constants",
        "descriptor_layout",
        "runtime_sampler_specializations",
        "runtime_storage_image_specializations",
    ] {
        put(&mut out, k, &get(k));
    }
    if patched > 0 {
        put(
            &mut out,
            "storage_image_format_patch",
            &Value::String("Unknown + StorageImage{Read,Write}WithoutFormat".to_string()),
        );
    }
    out.push('}');
    out.into_bytes()
}

fn ordered(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_else(|_| "null".to_string())
}

/// Convenience for tests and tools: the reflection type is re-exported so a dependent can inspect it.
pub type Reflection = ShaderReflection;
