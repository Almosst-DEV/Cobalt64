#!/bin/zsh
# Build the G2 capture tool for x86_64 and ad-hoc sign it. Runs on the host Mac (build
# only); the binary itself runs on the PC. Neither hardened nor library-validated,
# so it can swizzle Apple's driver class.
set -e
here=${0:A:h}
out=${1:-$here/g2capture}
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O2 \
    -Wall -Wextra -Wno-unused-parameter \
    -framework Foundation -framework Metal \
    "$here/g2capture.m" -o "$out"
# Ad-hoc sign, no hardened runtime (-o runtime) and no library-validation entitlement.
codesign --force --sign - "$out"
echo "built $out"
lipo -info "$out"
codesign -dv --verbose=2 "$out" 2>&1 | grep -E 'flags=|Signature=|Identifier=' || true
