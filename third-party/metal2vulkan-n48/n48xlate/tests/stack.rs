// SPDX-License-Identifier: LGPL-3.0-or-later
//! T5: the worker thread's stack size matters. A deep module translated on a 512 KiB stack (the default
//! for a secondary thread on macOS) must crash, in a child process; the same module on 64 MiB must not.
//!
//! The module is the Navi48 probe kernel `fill_buf` with its body replaced by a deeply nested chain of conditions. The child mode is selected by N48X_STACK_CHILD="<stack KiB>:<chain length>".
use std::process::Command;

const TEMPLATE: &str = include_str!("fixtures/fill_buf_deep.ll.in");

/// `depth` nested conditions, every one a block that dominates the next, all joining at one exit.
fn module(depth: usize) -> String {
    let mut s = String::with_capacity(depth * 120);
    s.push_str("entry:\n  %0 = load i32, ptr addrspace(2) %n, align 4\n  br label %b0\n");
    for i in 0..depth {
        s.push_str(&format!("b{i}:\n  %t{i} = icmp ne i32 %gid, {}\n  br i1 %t{i}, label %b{}, label %end\n", i + 1, i + 1));
    }
    s.push_str(&format!(
        "b{depth}:\n  %idxprom = zext i32 %gid to i64\n  %arrayidx = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %idxprom\n  %xor = xor i32 %0, %gid\n  store i32 %xor, ptr addrspace(1) %arrayidx, align 4\n  br label %end\nend:\n  ret void\n"
    ));
    TEMPLATE.replace("@@BODY@@", &s)
}

/// Not a real test: the child half. Runs only when N48X_STACK_CHILD is set.
#[test]
fn stack_child() {
    let Ok(spec) = std::env::var("N48X_STACK_CHILD") else { return };
    let (kib, chain) = spec.split_once(':').expect("kib:chain");
    let (kib, chain): (usize, usize) = (kib.parse().unwrap(), chain.parse().unwrap());
    let ll = module(chain);
    let t = std::thread::Builder::new()
        .stack_size(kib * 1024)
        .spawn(move || n48xlate::translate_bytes(ll.as_bytes(), [64, 1, 1]).is_ok())
        .unwrap();
    let ok = t.join().unwrap();
    std::process::exit(if ok { 0 } else { 10 });
}

fn run_child(kib: usize, chain: usize) -> std::process::ExitStatus {
    Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "stack_child", "--nocapture", "--test-threads=1"])
        .env("N48X_STACK_CHILD", format!("{kib}:{chain}"))
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .unwrap()
}

#[test]
fn sane_module_translates_on_both_stacks() {
    for kib in [512usize, 65536] {
        let st = run_child(kib, 4);
        assert_eq!(st.code(), Some(0), "a 4-step module on {kib} KiB: {st:?}");
    }
}

#[test]
fn deep_module_needs_the_big_stack() {
    use std::os::unix::process::ExitStatusExt;
    let chain: usize = std::env::var("N48X_STACK_CHAIN").ok().and_then(|v| v.parse().ok()).unwrap_or(600);
    let small = run_child(512, chain);
    assert!(
        small.signal().is_some(),
        "the deep module ({chain} steps) must crash a 512 KiB stack (died by a signal); got {small:?}"
    );
    let big = run_child(65536, chain);
    assert_eq!(big.code(), Some(0), "the same module must translate on a 64 MiB stack; got {big:?}");
}
