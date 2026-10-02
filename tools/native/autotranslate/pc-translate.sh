#!/bin/bash
# Navi48 on-PC auto-translator (native #12). Run as root by LaunchDaemon com.navi48.translate; files live in /usr/local/navi48/.
# Every 2 s: for each /private/tmp/n48m/<sha>.air not in the done-list and not in the bundle's Resources/spvcache, run
# pc-translate.py (clang IR -> metal2vulkan --emit-meta -> storage-format patch -> trimmed meta) and install meta first, spv last
# into /private/var/tmp/n48m-spv/. Log: /private/var/tmp/n48m-translate.log. Failures are recorded as "<sha> FAIL" in the done-list
# (not retried; delete that line to retry). SRC/OUT/BUNDLE/DONE can be overridden by env (used by tests).
SRC=${N48_SRC:-/private/tmp/n48m}
OUT=${N48_OUT:-/private/var/tmp/n48m-spv}
BUNDLE=${N48_BUNDLE:-/Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/spvcache}
DONE=${N48_DONE:-$OUT/.done}
LOG=${N48_LOG:-/private/var/tmp/n48m-translate.log}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; chmod 755 "$OUT" 2>/dev/null; touch "$DONE"
echo "$(date '+%F %T') start (src=$SRC out=$OUT)" >> "$LOG"
while true; do
  for air in "$SRC"/*.air; do
    [ -f "$air" ] || continue
    sha=${air##*/}; sha=${sha%.air}
    grep -q "^$sha" "$DONE" && continue
    if [ -f "$BUNDLE/$sha.spv" ]; then echo "$sha bundle" >> "$DONE"; continue; fi
    # size+mtime stable across 0.5 s so a half-written dump is never read
    s1=$(stat -f '%z %m' "$air"); sleep 0.5; s2=$(stat -f '%z %m' "$air"); [ "$s1" = "$s2" ] || continue
    msg=$(/usr/bin/python3 "$HERE/pc-translate.py" "$air" --out "$OUT" --m2v "$HERE/metal2vulkan" --overrides "$HERE/overrides.txt" 2>&1 | head -3)
    echo "$(date '+%F %T') $sha $msg" >> "$LOG"
    case "$msg" in OK*) echo "$sha ok" >> "$DONE";; *) echo "$sha FAIL" >> "$DONE";; esac
  done
  sleep 2
done
