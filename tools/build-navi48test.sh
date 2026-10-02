#!/bin/zsh
# Cross-build the userspace harness on the host Mac for the PC (x86_64 macOS).
set -euo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$P/tools/pc/navi48test"
clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra \
      -I "$P/src/navi48-bringup/src/apple" \
      -framework IOKit -framework CoreFoundation \
      -o "$OUT" "$P/tools/pc/navi48test.c"
file "$OUT"
echo "built $OUT — stage with tools/stage-to-pc.sh, run on the PC as: sudo ~/navi48-staging/navi48test info"
