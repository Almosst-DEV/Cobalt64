#!/bin/sh
# Builds mtlprobe-x86_64, mtlprobe-arm64, universal mtlprobe, and tri.metallib if `xcrun metal` exists.
set -e
cd "$(dirname "$0")"
FW="-framework Metal -framework Foundation -framework IOKit -framework CoreGraphics -framework ImageIO -framework IOSurface"
for a in x86_64 arm64; do
  clang -fobjc-arc -O1 -arch $a -mmacosx-version-min=13.0 mtlprobe.m $FW -o mtlprobe-$a
done
lipo -create mtlprobe-x86_64 mtlprobe-arm64 -output mtlprobe
if xcrun -f metal >/dev/null 2>&1 && xcrun -f metallib >/dev/null 2>&1; then
  xcrun metal -c tri.metal -o tri.air && xcrun metallib tri.air -o tri.metallib && echo "built tri.metallib"
else
  echo "note: xcrun metal not available; skipping tri.metallib (--precompiled will report SKIP)"
fi
lipo -info mtlprobe
