// SPDX-License-Identifier: LGPL-3.0-or-later
//! T2: the in-process translation equals the daemon pipeline over a corpus.
//!
//! Needs a corpus directory (`N48X_CORPUS`), laid out by the Navi48 bundle's `xlate-corpus-prep.py`:
//!   manifest.tsv         sha <TAB> function name <TAB> set <TAB> class <TAB> size
//!   <sha>.clang.ll       the LLVM text `n48-llvm-dis` printed for the AIR
//!   ref/<sha>.spv        the daemon's output (pc-translate.py + command-line tool + always-pass validator)
//!   ref/<sha>.meta.json  the daemon's trimmed meta
//! and the daemon's override table (`N48X_OVERRIDES`, autotranslate/overrides.txt).
//! A sha with no ref/<sha>.spv is one the daemon refused: the in-process translation must refuse it too.
//! Without N48X_CORPUS the tests print that they did nothing and pass; the bundle's driver script always sets it.
use std::collections::HashMap;
use std::path::PathBuf;

fn corpus() -> Option<(PathBuf, HashMap<String, [u32; 3]>)> {
    let dir = PathBuf::from(std::env::var_os("N48X_CORPUS")?);
    let mut locs = HashMap::new();
    if let Some(p) = std::env::var_os("N48X_OVERRIDES") {
        for l in std::fs::read_to_string(p).expect("overrides").lines() {
            let l = l.trim();
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = l.split_once('=') {
                let d: Vec<u32> = v.trim().split(',').map(|x| x.parse().unwrap()).collect();
                locs.insert(k.trim().to_string(), [d[0], d[1], d[2]]);
            }
        }
    }
    Some((dir, locs))
}

struct Row {
    sha: String,
    name: String,
    set: String,
}

fn rows(dir: &std::path::Path) -> Vec<Row> {
    std::fs::read_to_string(dir.join("manifest.tsv"))
        .expect("manifest.tsv")
        .lines()
        .map(|l| {
            let f: Vec<&str> = l.split('\t').collect();
            Row { sha: f[0].to_string(), name: f[1].to_string(), set: f[2].to_string() }
        })
        .collect()
}

#[test]
fn translation_equals_the_daemon_pipeline() {
    let Some((dir, locs)) = corpus() else {
        eprintln!("N48X_CORPUS not set: corpus test did nothing");
        return;
    };
    let (mut identical, mut meta_diff, mut spv_diff, mut refused_both, mut extra_ok, mut extra_fail) =
        (0usize, 0usize, 0usize, 0usize, 0usize, 0usize);
    let mut bad: Vec<String> = Vec::new();
    for r in rows(&dir) {
        let ll = std::fs::read(dir.join(format!("{}.clang.ll", r.sha))).expect("ll");
        let local = locs.get(&r.name).copied().unwrap_or([64, 1, 1]);
        let got = n48xlate::translate_bytes(&ll, local);
        let ref_spv = std::fs::read(dir.join(format!("ref/{}.spv", r.sha))).ok();
        match (got, ref_spv) {
            (Ok((spv, meta)), Some(rs)) => {
                let rm = std::fs::read(dir.join(format!("ref/{}.meta.json", r.sha))).expect("ref meta");
                let a: serde_json::Value = serde_json::from_slice(&meta).expect("our meta parses");
                let b: serde_json::Value = serde_json::from_slice(&rm).expect("ref meta parses");
                if spv != rs {
                    spv_diff += 1;
                    bad.push(format!("{} {} {}: spv differs", r.set, r.sha, r.name));
                } else if a != b {
                    meta_diff += 1;
                    bad.push(format!("{} {} {}: meta differs", r.set, r.sha, r.name));
                } else {
                    identical += 1;
                }
            }
            (Err(_), None) => refused_both += 1,
            (Ok(_), None) => {
                extra_ok += 1;
                bad.push(format!("{} {} {}: translated but the daemon refused", r.set, r.sha, r.name));
            }
            (Err(e), Some(_)) => {
                extra_fail += 1;
                bad.push(format!("{} {} {}: refused ({e}) but the daemon translated", r.set, r.sha, r.name));
            }
        }
    }
    eprintln!(
        "T2: identical {identical}, spv different {spv_diff}, meta different {meta_diff}, refused by both {refused_both}, \
         translated-only-by-us {extra_ok}, refused-only-by-us {extra_fail}"
    );
    for b in bad.iter().take(40) {
        eprintln!("  {b}");
    }
    assert!(bad.is_empty(), "{} differences", bad.len());
    assert!(identical > 0, "nothing compared");
}

/// The Skip path must not start the validator, and must still run the two structural rejects.
#[test]
fn skip_path_never_runs_the_validator_and_keeps_the_rejects() {
    let Some((dir, locs)) = corpus() else {
        eprintln!("N48X_CORPUS not set: skip-path test did nothing");
        return;
    };
    // A validator that always fails: the external gate would refuse every module.
    std::env::set_var("METAL2VULKAN_SPIRV_VAL", "/usr/bin/false");
    let mut ok = 0usize;
    let mut rejects_seen = 0usize;
    for r in rows(&dir) {
        let ll = std::fs::read(dir.join(format!("{}.clang.ll", r.sha))).expect("ll");
        let local = locs.get(&r.name).copied().unwrap_or([64, 1, 1]);
        let have_ref = dir.join(format!("ref/{}.spv", r.sha)).exists();
        let res = n48xlate::translate_bytes(&ll, local);
        match (&res, have_ref) {
            (Ok(_), true) => ok += 1,
            (Err(e), true) => panic!("{}: the validator was reached or the translation broke: {e}", r.sha),
            (Ok(_), false) => panic!("{}: translated but the daemon refused (a post-validation reject was dropped?)", r.sha),
            (Err(e), false) => {
                if e.contains("write survived folding") || e.contains("imageblock cell") || e.contains("imageblock") {
                    rejects_seen += 1;
                }
            }
        }
    }
    eprintln!("skip path: {ok} translated with an always-failing validator configured; {rejects_seen} post-validation rejects fired");
    assert!(ok > 0);
    assert!(rejects_seen > 0, "no corpus entry exercised the post-validation rejects");
}

/// External validation still validates: with the failing validator the file-based entry refuses.
#[test]
fn external_path_still_calls_the_validator() {
    let Some((dir, _)) = corpus() else {
        eprintln!("N48X_CORPUS not set: external-path test did nothing");
        return;
    };
    std::env::set_var("METAL2VULKAN_SPIRV_VAL", "/usr/bin/false");
    let r = rows(&dir)
        .into_iter()
        .find(|r| dir.join(format!("ref/{}.spv", r.sha)).exists())
        .expect("a translating row");
    let ll = std::fs::read_to_string(dir.join(format!("{}.clang.ll", r.sha))).unwrap();
    let (san, _) = metal2vulkan::tools::sanitize_ll_text_with_datalayout(&ll);
    let stage = n48xlate::detect_stage_text(&san).unwrap();
    let tmp = std::env::temp_dir().join(format!("n48xlate_ext_{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let src = tmp.join("in.ll");
    std::fs::write(&src, &ll).unwrap();
    let res = metal2vulkan::translate_reflected_with_options(
        src.to_str().unwrap(),
        stage,
        &tmp,
        metal2vulkan::passes::TransformOptions::default(),
    );
    let _ = std::fs::remove_file(&src);
    let _ = std::fs::remove_dir(&tmp);
    assert!(res.is_err(), "the external gate must refuse with an always-failing validator");
}
