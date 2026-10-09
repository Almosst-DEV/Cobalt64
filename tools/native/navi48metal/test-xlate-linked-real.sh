#!/bin/zsh
# test-xlate-linked-real.sh - P3 (build 18) end-to-end on REAL AIR: Apple's compiler makes a fragment shader that calls a [[visible]] function from another library; the pipeline's linkedFunctions would link it.
# The AIR comes out of the host Mac's real Metal (MTLFunction.bitcodeData), goes through the OS's GPUCompiler LLVM reader and the HOST build of libn48xlate (aarch64: `cargo build --release -p n48xlate` in the fork).
#   usage (host Mac only): tools/native/navi48metal/test-xlate-linked-real.sh [path to the host libn48xlate.dylib]
set -u
D="$(cd "$(dirname "$0")" && pwd)"
XL="${1:-$HOME/navi48-native/metal2vulkan/target/release/libn48xlate.dylib}"
[ -f "$XL" ] || { echo "no libn48xlate at $XL (build it: cargo build --release -p n48xlate)"; exit 2; }
W="$(mktemp -d "${TMPDIR:-/tmp}/n48real.XXXXXX")"; case "$W" in */n48real.??????) ;; *) echo bad scratch; exit 2;; esac
trap 'rm -rf -- "$W"' EXIT
clang -fobjc-arc -framework Metal -framework Foundation -o "$W/gen" "$D/test-xlate-linked-real.m" 2>"$W/cc.err" || { cat "$W/cc.err"; exit 2; }
clang -O1 -Wall -Wextra -Werror -I"$D" -o "$W/real" "$D/test-xlate-linked-real.c" 2>"$W/cc2.err" || { cat "$W/cc2.err"; exit 2; }
"$W/gen" "$W" 2>&1 | /usr/bin/grep -v "^20" ; [ -f "$W/fs.air" ] && [ -f "$W/shade.air" ] || { echo "no AIR produced"; exit 2; }
"$W/real" "$XL" "$W/fs.air" "$W/shade.air" shade
