#!/bin/bash
#
# fetch-firmware.sh - copy the AMD firmware the Navi48 bring-up kext embeds from a local
# linux-firmware checkout into src/navi48-bringup/firmware/ (gitignored; never commit it).
#
# This script does NOT use the network. Clone linux-firmware yourself first:
#     git clone https://gitlab.com/kernel-firmware/linux-firmware.git
#   (mirror: https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
#
# Usage:  tools/fetch-firmware.sh <path-to-linux-firmware-checkout>
#
# The blobs are AMD's, under AMD's redistribution licence (LICENSE.amdgpu in linux-firmware).
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 <path-to-linux-firmware-checkout>" >&2
    echo "clone it first: git clone https://gitlab.com/kernel-firmware/linux-firmware.git" >&2
    exit 2
fi
SRC="$1/amdgpu"
[ -d "$SRC" ] || { echo "fetch-firmware.sh: no amdgpu/ directory in $1" >&2; exit 2; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$REPO/src/navi48-bringup/firmware"
mkdir -p "$DST"

# name  sha256 of the copy this project was developed against (a different linux-firmware
# revision may differ: a mismatch is a warning, not an error)
BLOBS="
gc_12_0_1_imu     b3b301fb636efc77b63ce4d2ced0f90c851d03c19681852faa45598e6f5773fd
gc_12_0_1_me      5549655093724b3e3b5fa016e8f1ef6ce3976209432230852cd94acbc1c245cb
gc_12_0_1_mec     72b25088072d630d115748ee3607d0db481b22a15260af3d0d599e516a0d7abb
gc_12_0_1_pfp     6dc9e6d22e0f9ddd04f12eedb5d1f6affae6d76d2993567aafde0e1619071c15
gc_12_0_1_rlc     6ba4459532246a5c415d3cb33c9b1248294e48f67b827e2accb292a8d1a5c0ec
gc_12_0_1_uni_mes bf26e0300989ebd011e991d76396e2c1673e1caa502fb6d8413e387eb1456676
psp_14_0_3_sos    23bea01a0c6f36d00759d0765d46cb4cb4aa87398b2fbccacbf547a890c0bf51
psp_14_0_3_ta     38d6691bf64232946cd5ce5735363c7c2877830ff640b9b95516403257fe2922
sdma_7_0_1        73c29e1c1714ebc95d2221ba56e187910902891593010653bf9518937e414a59
smu_14_0_3        3221ef2ddb341570eeb727e1e16f170bfb2ea1230be4a7eb248d0b183fbfbf15
"

while read -r name want; do
    [ -n "$name" ] || continue
    if [ -f "$SRC/$name.bin" ]; then
        cp "$SRC/$name.bin" "$DST/$name.bin"
    elif [ -f "$SRC/$name.bin.zst" ] && command -v zstd >/dev/null 2>&1; then
        zstd -dqf -o "$DST/$name.bin" "$SRC/$name.bin.zst"
    else
        echo "MISSING  $name.bin (not in $SRC)" >&2
        exit 1
    fi
    got="$(shasum -a 256 "$DST/$name.bin" | awk '{print $1}')"
    if [ "$got" = "$want" ]; then echo "ok       $name.bin ($(wc -c < "$DST/$name.bin" | tr -d ' ') bytes)"
    else echo "DIFFERS  $name.bin (sha256 $got; developed against $want) - copied anyway"; fi
done <<< "$BLOBS"
cp "$1/LICENSE.amdgpu" "$DST/" 2>/dev/null || true
echo "copied into $DST"
