#!/bin/zsh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The command-line tool must not change by a byte when the Skip path was added. Runs two builds of the CLI (the base commit's and this branch's)
# over a corpus laid out by the Navi48 bundle's xlate-corpus-prep.py (<sha>.air next to manifest.tsv) and compares exit code, the .spv, the --emit-meta JSON
# and stdout, one AIR at a time.
#   usage: cli-identical.sh BASE_BINARY NEW_BINARY CORPUS_DIR LLVM_DIS_SCRIPT SPIRV_VAL_STUB OVERRIDES_FILE
set -u
BASE=$1; NEW=$2; C=$3; DIS=$4; VAL=$5; OVR=$6
T=$(mktemp -d "${TMPDIR:-/tmp}/clicmp.XXXXXX"); case "$T" in */clicmp.??????) ;; *) echo "bad scratch $T"; exit 2;; esac
trap 'case "$T" in */clicmp.??????) rm -rf -- "$T";; esac' EXIT
export METAL2VULKAN_LLVM_DIS=$DIS METAL2VULKAN_SPIRV_VAL=$VAL
same=0; diff=0; n=0
while IFS=$'\t' read -r sha name set cls size; do
  loc=$(/usr/bin/grep -m1 "^$name=" "$OVR" 2>/dev/null | cut -d= -f2)
  args=(); [ -n "$name" ] && [ -n "$loc" ] && args=(--local "$loc")
  "$BASE" "$C/$sha.air" "$T/a.spv" --emit-meta "$T/a.json" "${args[@]}" > "$T/a.out" 2>&1; ra=$?
  "$NEW"  "$C/$sha.air" "$T/b.spv" --emit-meta "$T/b.json" "${args[@]}" > "$T/b.out" 2>&1; rb=$?
  n=$((n+1)); ok=1
  [ $ra -eq $rb ] || ok=0
  sed -e "s|$T/a|$T/X|g" "$T/a.out" > "$T/a.o2"; sed -e "s|$T/b|$T/X|g" "$T/b.out" > "$T/b.o2"
  # stdout/stderr differ in repro-dir paths (pid, nanosecond stamp): compare with those scrubbed
  sed -E -e 's/metal2vulkan-repro-[0-9]+-[0-9]+/REPRO/g' "$T/a.o2" > "$T/a.o3"; sed -E -e 's/metal2vulkan-repro-[0-9]+-[0-9]+/REPRO/g' "$T/b.o2" > "$T/b.o3"
  cmp -s "$T/a.o3" "$T/b.o3" || ok=0
  if [ $ra -eq 0 ]; then cmp -s "$T/a.spv" "$T/b.spv" || ok=0; cmp -s "$T/a.json" "$T/b.json" || ok=0; fi
  rm -f "$T"/a.* "$T"/b.*
  if [ $ok -eq 1 ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $sha $name rc $ra/$rb"; fi
done < "$C/manifest.tsv"
echo "cli-identical: $n inputs, $same identical, $diff different"
[ $diff -eq 0 ]
