#!/bin/bash
#
# metal-kernel-to-kext.sh — take a compute kernel out of a system .metallib and
# turn it into a blob the bring-up kext can dispatch.
#
# This is the whole B1 pipeline in one command:
#
#   .metallib  --extract-air.py-->  AIR .ll
#              --air-to-amdgpu.py-> AMDGPU .ll
#              --llc gfx1201------>  .s
#              --build-shader.sh-->  .bin + kernel descriptor + embedded C
#
# Nothing in it uses Apple's compiler, which cannot target this GPU at all.
#
# USAGE
#   tools/metal-kernel-to-kext.sh <file.metallib> <module-index> <name>
#
# e.g.  tools/metal-kernel-to-kext.sh \
#         /System/Library/Frameworks/Metal.framework/Versions/A/Resources/MTLECBE.metallib \
#         0 progress_track
#
# The resulting shader uses the standard HSA kernarg convention, so dispatch it
# with the kext's hsaAbi path (boot-arg navi48-hsa-abi=1). See
# notes/B1-COMPILER-BRIDGE.md.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$REPO/../.." && pwd)"          # the Navi48 repo root
LLVM_BIN="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"

[ $# -eq 3 ] || { sed -n '2,24p' "$0"; exit 2; }
LIB="$1"; IDX="$2"; NAME="$3"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "metal-kernel-to-kext.sh: $LIB [module $IDX] -> $NAME"

# 1. AIR out of the container
"$ROOT/tools/extract-air.py" "$LIB" "$WORK" >/dev/null
AIR=$(ls "$WORK"/*."$IDX".ll 2>/dev/null | head -1)
[ -n "$AIR" ] || { echo "no module $IDX in $LIB" >&2; exit 1; }
echo "  AIR:        $(basename "$AIR")  ($(grep -c '^' "$AIR") lines)"

# 2. translate
"$ROOT/tools/air-to-amdgpu.py" "$AIR" "$WORK/amd.ll" >/dev/null
echo "  translated: $(grep -c '^define amdgpu_kernel' "$WORK/amd.ll") kernel(s)"
grep '^;' "$WORK/amd.ll" | sed 's/^/  /' || true

# 3. compile for the card
"$LLVM_BIN/llc" -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 "$WORK/amd.ll" -o "$REPO/shaders/$NAME.s"
echo "  compiled:   shaders/$NAME.s"

# 4. through the existing shader pipeline: .bin, descriptor, embedded C
"$REPO/tools/build-shader.sh" "$NAME"
