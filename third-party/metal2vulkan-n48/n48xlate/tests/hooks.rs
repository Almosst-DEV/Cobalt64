// SPDX-License-Identifier: LGPL-3.0-or-later
//! The C boundary: argument checks, return codes, and the catch_unwind guard. Built only with
//! `--features test-hooks` (the panic fixture does not exist in the shipped library).
#![cfg(feature = "test-hooks")]
use n48xlate::{n48x_abi_version, n48x_free, n48x_translate};
use std::os::raw::c_char;

fn call(ll: &[u8], local: Option<[u32; 3]>) -> (i32, String) {
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 256];
    let lp = local.as_ref().map_or(std::ptr::null(), |l| l.as_ptr());
    let rc = unsafe {
        n48x_translate(
            ll.as_ptr(), ll.len(), lp, &mut spv, &mut spv_len, &mut meta, &mut meta_len, err.as_mut_ptr(), err.len(),
        )
    };
    if rc != 0 {
        assert!(spv.is_null() && meta.is_null() && spv_len == 0 && meta_len == 0, "outputs must be zeroed on failure");
    } else {
        unsafe {
            n48x_free(spv, spv_len);
            n48x_free(meta, meta_len);
        }
    }
    let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().into_owned();
    (rc, msg)
}

#[test]
fn abi_version_is_two() {
    assert_eq!(n48x_abi_version(), 2);
}

#[test]
fn a_panic_is_caught_and_reported_as_2() {
    let (rc, msg) = call(b";N48X-TEST-PANIC\n", None);
    assert_eq!(rc, 2, "a panic inside translation must come back as code 2, got {rc} ({msg})");
    assert!(msg.starts_with("panic:"), "{msg}");
    // The library is still usable afterwards.
    let (rc, _) = call(b"not llvm at all\n", None);
    assert_eq!(rc, 1);
}

#[test]
fn bad_arguments_are_3() {
    assert_eq!(call(b"", None).0, 3);
    assert_eq!(call(b"x", Some([0, 1, 1])).0, 3);
    assert_eq!(call(b"x", Some([8, 0, 1])).0, 3);
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut n = 0usize;
    let rc = unsafe {
        n48x_translate(std::ptr::null(), 4, std::ptr::null(), &mut spv, &mut n, &mut spv, &mut n, std::ptr::null_mut(), 0)
    };
    assert_eq!(rc, 3);
    unsafe { n48x_free(std::ptr::null_mut(), 0) };
}

#[test]
fn refusals_are_1() {
    assert_eq!(call(b"define void @f() { ret void }\n", None).0, 1, "no stage metadata");
    assert_eq!(call(&[0xff, 0xfe, 0xfd], None).0, 1, "not UTF-8");
}
