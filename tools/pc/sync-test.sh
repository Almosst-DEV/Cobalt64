#!/bin/zsh
set -u; T=$HOME/navi48-staging
if pgrep -q computeprobe; then echo "REFUSING: stuck computeprobe — reboot"; exit 1; fi
if sudo $T/navi48test accel status 2>/dev/null | grep -q "installed = yes"; then
  echo "REFUSING: already fired — reboot"; exit 1; fi
echo "=== counters before ==="; sudo $T/navi48test counters
echo "=== fire ==="; sudo $T/navi48test accel fire 2>&1 | head -1
sleep 4
echo "=== dispatch (populates Apple's +0x50 array, then hangs) ==="
$T/computeprobe > /tmp/sync-probe.txt 2>&1 &
sleep 14
echo "=== synctables (read Apple's GART array -> our GFX12 PT) ==="
sudo $T/navi48test accel synctables 2>&1
sleep 1
sudo pkill -9 computeprobe 2>/dev/null; sleep 1
echo "=== gart-sync log lines ==="
sudo $T/navi48test log 2>/dev/null | grep -iE "gart-sync" | tail -20
echo "=== counters after ==="; sudo $T/navi48test counters
echo "REBOOT before next measurement"
