#!/bin/zsh
# Build Navi48Metal.bundle (x86_64, ad-hoc signed) into tools/native/navi48metal/build/.
#   build.sh            -> build/Navi48Metal.bundle (x86_64, for the PC) + arm64 compile/link sanity build (NOT shipped)
#   N48_LAZY=0 build.sh -> 9e variant (supportLazyInitialization not YES)
#   N48_9D=0  build.sh  -> without the 9d feature overrides
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
# Work dir is OUTSIDE the repo: the repo lives in a file-provider folder that stamps com.apple.FinderInfo on
# *.bundle directories, which codesign --verify rejects ("detritus not allowed").  The shippable artefact is
# build/Navi48Metal.bundle.tar (bsdtar --no-xattrs); extract it on the PC.
B="$D/build"; W="$(mktemp -d /tmp/navi48metal.XXXXXX)"; APP="$W/Navi48Metal.bundle"
LAZY="${N48_LAZY:-1}"; F9D="${N48_9D:-1}"
RADV="${N48_RADV_LIB:-$HOME/navi48-native/mesa-mac/build-x86_64/src/amd/vulkan/libvulkan_radeon.dylib}"   # x86_64 RADV (10a: shipped in Resources)
[ -f "$RADV" ] || { echo "build.sh: RADV dylib not found: $RADV" >&2; exit 1; }
VKINC="${N48_VK_INC:--I/opt/homebrew/include}"
rm -rf "$B"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$B/sanity"
cp "$D/Info.plist" "$APP/Contents/Info.plist"
cp "$RADV" "$APP/Contents/Resources/libvulkan_radeon.dylib"
mkdir -p "$APP/Contents/Resources/spvcache"; for f in "$D"/spvcache/*.spv "$D"/spvcache/*.meta.json; do cat "$f" > "$APP/Contents/Resources/spvcache/${f:t}"; done   # cat, not cp: cp(1) hung forever in lseek on spvcache/eee33f36...meta.json   # 10d SPIR-V cache
clang $VKINC -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror \
      -DN48_LAZY=$LAZY -DN48_9D=$F9D -I"$D" -bundle -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
      -o "$APP/Contents/MacOS/Navi48Metal" "$D/Navi48Device.m"
xattr -cr "$APP"
codesign --force --sign - "$APP/Contents/Resources/libvulkan_radeon.dylib"
codesign --force --sign - "$APP"
# arm64 build of the same source: compile/link sanity only (there is no Rosetta here to load x86_64); never install it.
clang $VKINC -arch arm64 -fobjc-arc -O2 -Wall -Wextra -Werror -DN48_LAZY=$LAZY -DN48_9D=$F9D \
      -I"$D" -bundle -framework Foundation -framework Metal -framework IOKit -framework IOSurface -o "$B/sanity/Navi48Metal-arm64.NOT-SHIPPED" "$D/Navi48Device.m"
codesign --verify --strict -v "$APP"
tar --no-xattrs --no-mac-metadata -C "$W" -cf "$B/Navi48Metal.bundle.tar" Navi48Metal.bundle 2>/dev/null || \
  tar --no-xattrs -C "$W" -cf "$B/Navi48Metal.bundle.tar" Navi48Metal.bundle
file "$APP/Contents/MacOS/Navi48Metal"
echo "built (lazy=$LAZY 9d=$F9D): $APP  and  $B/Navi48Metal.bundle.tar ; install: see INSTALL.md"
