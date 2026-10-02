#!/bin/sh
# Copy the Navi 48 firmware this project needs out of a linux-firmware checkout you
# cloned yourself:   tools/fetch-firmware.sh /path/to/linux-firmware
# The firmware is AMD's and is covered by linux-firmware's LICENSE.amdgpu; it is not
# redistributed by this repository.
set -e
SRC="${1:?usage: fetch-firmware.sh /path/to/linux-firmware}"
DST="$(cd "$(dirname "$0")/.." && pwd)/src/navi48-bringup/firmware"
mkdir -p "$DST"
for f in gc_12_0_1_imu gc_12_0_1_me gc_12_0_1_mec gc_12_0_1_pfp gc_12_0_1_rlc gc_12_0_1_uni_mes \
         psp_14_0_3_sos psp_14_0_3_ta sdma_7_0_1 smu_14_0_3; do
  if [ -f "$SRC/amdgpu/$f.bin" ]; then cp "$SRC/amdgpu/$f.bin" "$DST/"
  elif [ -f "$SRC/amdgpu/$f.bin.zst" ] && command -v zstd >/dev/null; then zstd -dqf "$SRC/amdgpu/$f.bin.zst" -o "$DST/$f.bin"
  else echo "missing: $f.bin" >&2; exit 1; fi
done
cp "$SRC/LICENSE.amdgpu" "$DST/" 2>/dev/null || true
echo "firmware copied to $DST"
