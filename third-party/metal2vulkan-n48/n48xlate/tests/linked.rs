// SPDX-License-Identifier: LGPL-3.0-or-later
//! `n48x_translate_linked` (ABI 2): direct visible-function references resolved to the exact authored
//! dependencies before translation. Fixtures are the small kernels of `linked_functions.rs`' own tests.
use n48xlate::{n48x_abi_version, n48x_free, n48x_translate, n48x_translate_linked, N48xDep};
use std::os::raw::c_char;

const ENTRY: &str = r#"
define void @main(ptr addrspace(1) %output) {
entry:
  %value = call i32 @add_one.MTL_VISIBLE_FN_REF(i32 41)
  store i32 %value, ptr addrspace(1) %output, align 4
  ret void
}
declare i32 @add_one.MTL_VISIBLE_FN_REF(i32) section "air.externally_defined"
!air.kernel = !{!0}
!air.visible_function_references = !{!4}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"uint"}
!4 = !{!"air.visible_function_reference", ptr @add_one.MTL_VISIBLE_FN_REF, !"add_one"}
"#;
const NO_REF: &str = r#"
define void @main(ptr addrspace(1) %output) {
entry:
  store i32 42, ptr addrspace(1) %output, align 4
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"uint"}
"#;
const ADD_ONE: &str = "define i32 @add_one(i32 %x) {\nentry:\n  %y = add i32 %x, 1\n  ret i32 %y\n}\n";
const ADD_TWO: &str = "define i32 @add_one(i32 %x) {\nentry:\n  %y = add i32 %x, 2\n  ret i32 %y\n}\n";

struct Out {
    rc: i32,
    msg: String,
    spv: Vec<u8>,
    meta: Vec<u8>,
}

fn run(f: impl FnOnce(*mut *mut u8, *mut usize, *mut *mut u8, *mut usize, *mut c_char, usize) -> i32) -> Out {
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let rc = f(&mut spv, &mut spv_len, &mut meta, &mut meta_len, err.as_mut_ptr(), err.len());
    let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().into_owned();
    let (mut s, mut m) = (Vec::new(), Vec::new());
    if rc == 0 {
        unsafe {
            s = std::slice::from_raw_parts(spv, spv_len).to_vec();
            m = std::slice::from_raw_parts(meta, meta_len).to_vec();
            n48x_free(spv, spv_len);
            n48x_free(meta, meta_len);
        }
    } else {
        assert!(spv.is_null() && meta.is_null() && spv_len == 0 && meta_len == 0, "outputs must be zeroed on failure");
    }
    Out { rc, msg, spv: s, meta: m }
}

fn linked(ll: &str, deps: &[(&str, &str)]) -> Out {
    let ds: Vec<N48xDep> = deps
        .iter()
        .map(|(s, t)| N48xDep { symbol: s.as_ptr(), symbol_len: s.len(), ll: t.as_ptr(), ll_len: t.len() })
        .collect();
    run(|a, b, c, d, e, f| unsafe {
        n48x_translate_linked(ll.as_ptr(), ll.len(), std::ptr::null(), ds.as_ptr(), ds.len(), a, b, c, d, e, f)
    })
}
fn plain(ll: &str) -> Out {
    run(|a, b, c, d, e, f| unsafe { n48x_translate(ll.as_ptr(), ll.len(), std::ptr::null(), a, b, c, d, e, f) })
}
/// The opcodes of a SPIR-V binary.
fn ops(spv: &[u8]) -> Vec<u32> {
    let w: Vec<u32> = spv.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
    let mut p = 5;
    let mut v = Vec::new();
    while p < w.len() {
        let wc = (w[p] >> 16) as usize;
        if wc == 0 {
            break;
        }
        v.push(w[p] & 0xffff);
        p += wc;
    }
    v
}
const OP_IADD: u32 = 128;

#[test]
fn abi_version_is_two() {
    assert_eq!(n48x_abi_version(), 2);
}

#[test]
fn the_unlinked_entry_is_refused_and_the_linked_one_translates() {
    let p = plain(ENTRY);
    assert_eq!(p.rc, 1, "{}", p.msg);
    assert!(p.msg.contains("visible function reference"), "{}", p.msg);
    let l = linked(ENTRY, &[("add_one", ADD_ONE)]);
    assert_eq!(l.rc, 0, "{}", l.msg);
    assert!(ops(&l.spv).contains(&OP_IADD), "the authored definition must be emitted");
    assert!(std::str::from_utf8(&l.meta).unwrap().starts_with('{'));
}

#[test]
fn the_dependency_decides_the_output() {
    // This is why the bundle's cache key must contain the dependencies: the same entry linked to a different function is a different program.
    let a = linked(ENTRY, &[("add_one", ADD_ONE)]);
    let b = linked(ENTRY, &[("add_one", ADD_TWO)]);
    assert_eq!((a.rc, b.rc), (0, 0), "{} {}", a.msg, b.msg);
    assert_ne!(a.spv, b.spv);
}

#[test]
fn a_missing_dependency_is_refused_by_name() {
    let r = linked(ENTRY, &[]);
    assert_eq!(r.rc, 1);
    assert!(r.msg.contains("add_one"), "{}", r.msg);
    let r = linked(ENTRY, &[("something_else", ADD_ONE)]);
    assert_eq!(r.rc, 1);
    assert!(r.msg.contains("add_one"), "{}", r.msg);
}

#[test]
fn unreached_dependencies_are_ignored_even_when_they_are_junk() {
    let a = linked(ENTRY, &[("add_one", ADD_ONE)]);
    let b = linked(ENTRY, &[("unused", "this is not llvm"), ("add_one", ADD_ONE), ("also_unused", "define void @other() { ret void }")]);
    assert_eq!(b.rc, 0, "{}", b.msg);
    assert_eq!(a.spv, b.spv, "unreached dependencies must not change the module");
}

#[test]
fn an_entry_with_no_reference_is_byte_identical_to_the_unlinked_path() {
    let p = plain(NO_REF);
    assert_eq!(p.rc, 0, "{}", p.msg);
    let l = linked(NO_REF, &[("add_one", ADD_ONE)]);
    assert_eq!(l.rc, 0, "{}", l.msg);
    assert_eq!(p.spv, l.spv);
    assert_eq!(p.meta, l.meta);
    let l0 = linked(NO_REF, &[]);
    assert_eq!(p.spv, l0.spv);
}

#[test]
fn a_dependency_that_references_another_is_followed() {
    let outer = "define i32 @add_one(i32 %x) {\nentry:\n  %y = call i32 @helper.MTL_VISIBLE_FN_REF(i32 %x)\n  ret i32 %y\n}\ndeclare i32 @helper.MTL_VISIBLE_FN_REF(i32) section \"air.externally_defined\"\n!air.visible_function_references = !{!0}\n!0 = !{!\"air.visible_function_reference\", ptr @helper.MTL_VISIBLE_FN_REF, !\"helper\"}\n";
    let helper = "define i32 @helper(i32 %x) {\nentry:\n  %y = add i32 %x, 7\n  ret i32 %y\n}\n";
    let r = linked(ENTRY, &[("add_one", outer), ("helper", helper)]);
    assert_eq!(r.rc, 0, "{}", r.msg);
    assert!(ops(&r.spv).contains(&OP_IADD));
    let r = linked(ENTRY, &[("add_one", outer)]);
    assert_eq!(r.rc, 1);
    assert!(r.msg.contains("helper"), "{}", r.msg);
}

#[test]
fn a_dependency_is_sanitised_like_the_entry() {
    // llvm-dis prints llvm.used / global_ctors roots; the entry's sanitiser drops them, and so must the dependency's, or they land in the linked module.
    let raw = format!(
        "target triple = \"air64_v27-apple-macosx15.0.0\"\n; ModuleID = 'dep'\n@llvm.used = appending global [1 x ptr] [ptr @add_one], section \"llvm.metadata\"\n@llvm.global_ctors = appending global [0 x {{ i32, ptr, ptr }}] []\n{ADD_ONE}"
    );
    let r = linked(ENTRY, &[("add_one", &raw)]);
    assert_eq!(r.rc, 0, "a dependency carrying llvm.used must translate: {}", r.msg);
    let clean = linked(ENTRY, &[("add_one", ADD_ONE)]);
    assert_eq!(r.spv, clean.spv, "sanitising must make the raw and the clean dependency the same program");
}

#[test]
fn the_linkage_handed_to_the_specialiser_is_made_of_sanitised_modules() {
    // Observable at the seam: whatever the emitter happens to tolerate, every module of the linkage is exactly what the entry's own sanitiser produces
    // (Vulkan triple, no ModuleID comment, no llvm.used / global_ctors roots).
    let raw = format!(
        "target triple = \"air64_v27-apple-macosx15.0.0\"\n; ModuleID = 'dep'\n@llvm.used = appending global [1 x ptr] [ptr @add_one], section \"llvm.metadata\"\n@llvm.compiler.used = appending global [1 x ptr] [ptr @add_one], section \"llvm.metadata\"\n{ADD_ONE}"
    );
    let (san_entry, _) = metal2vulkan::tools::sanitize_ll_text_with_datalayout(ENTRY);
    let l = n48xlate::linkage_for(&san_entry, &[("add_one", raw.as_str())]).unwrap();
    assert_eq!(l.visible_references.len(), 1);
    let m = &l.visible_references[0].module_ll;
    assert_eq!(m, &metal2vulkan::tools::sanitize_ll_text_with_datalayout(&raw).0);
    assert!(!m.contains("llvm.used") && !m.contains("llvm.compiler.used") && !m.contains("ModuleID"), "{m}");
    assert!(m.contains("target triple = \"spirv") || !m.contains("apple-macosx"), "{m}");
}

#[test]
fn conflicting_authoring_and_bad_arguments() {
    let r = linked(ENTRY, &[("add_one", ADD_ONE), ("add_one", ADD_TWO)]);
    assert_eq!(r.rc, 1, "{}", r.msg);
    assert!(r.msg.contains("authored twice"), "{}", r.msg);
    // the same module twice is fine
    assert_eq!(linked(ENTRY, &[("add_one", ADD_ONE), ("add_one", ADD_ONE)]).rc, 0);
    // empty symbol / empty text / null entry
    let d = [N48xDep { symbol: std::ptr::null(), symbol_len: 0, ll: ADD_ONE.as_ptr(), ll_len: ADD_ONE.len() }];
    let r = run(|a, b, c, e, f, g| unsafe { n48x_translate_linked(ENTRY.as_ptr(), ENTRY.len(), std::ptr::null(), d.as_ptr(), 1, a, b, c, e, f, g) });
    assert_eq!(r.rc, 3);
    let r = run(|a, b, c, e, f, g| unsafe { n48x_translate_linked(std::ptr::null(), 4, std::ptr::null(), std::ptr::null(), 0, a, b, c, e, f, g) });
    assert_eq!(r.rc, 3);
    let r = run(|a, b, c, e, f, g| unsafe { n48x_translate_linked(ENTRY.as_ptr(), ENTRY.len(), std::ptr::null(), std::ptr::null(), 5, a, b, c, e, f, g) });
    assert_eq!(r.rc, 3, "ndeps > 0 with a null deps pointer");
    let r = run(|a, b, c, e, f, g| unsafe { n48x_translate_linked(ENTRY.as_ptr(), ENTRY.len(), std::ptr::null(), std::ptr::null(), n48xlate::MAX_DEPS + 1, a, b, c, e, f, g) });
    assert_eq!(r.rc, 3, "too many dependencies");
}
