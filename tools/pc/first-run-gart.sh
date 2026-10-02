#!/bin/zsh
# first-run-gart.sh — the first hardware run of 0.0.79's GART mirror.
# Boot config: stage17-accel-gart. ONE run per boot: the Metal probe hangs and
# wedges the accelerator, so reboot before any further measurement.
set -u
OUT=/tmp/first-run-gart; mkdir -p $OUT
T=$HOME/navi48-staging
reg() { sudo $T/navi48test reg $1 2>/dev/null | tail -1; }
faults() {
  echo "  GCVM  STATUS_LO=$(reg 0x2830)  HI=$(reg 0x2831)  ADDR_LO=$(reg 0x2832)  ADDR_HI=$(reg 0x2833)"
  echo "  MMVM  STATUS_LO=$(reg 0x1a4f0)  HI=$(reg 0x1a4f1)  ADDR_LO=$(reg 0x1a4f2)  ADDR_HI=$(reg 0x1a4f3)"
}
if pgrep -q computeprobe; then echo "REFUSING: a stuck computeprobe exists — reboot first"; exit 1; fi
if sudo $T/navi48test accel status 2>/dev/null | grep -q "installed = yes"; then
  echo "REFUSING: accelerator already fired this boot — reboot first"; exit 1; fi

echo "=== 0. before ==="; sudo $T/navi48test counters; faults
echo "=== 1. preload Apple's accelerator (loads without matching) so its probes exist ==="
sudo kmutil load -b com.apple.kext.AMDRadeonX6000 >/dev/null 2>&1; sleep 2
echo "=== 2. fire ==="
sudo $T/navi48test accel fire 2>&1 | head -2
sleep 8
sudo $T/navi48test log 2>/dev/null > $OUT/log-after-fire.txt
echo "=== 3. mirror hook: installed / refused / mirrored ==="
grep -E "gart-mirror|bind_at|REFUS" $OUT/log-after-fire.txt | head -40
echo "=== 4. compute dispatch under the ring trace (the probe is expected to hang) ==="
sudo dtrace -s ~/dtrace/AF-ring.d -c "$T/probe-wrap.sh $T/computeprobe 20" > $OUT/af-ring.txt 2>&1
echo "--- probe ---"; cat /tmp/trace-probe.txt
echo "--- ring trace (first 90 lines) ---"; head -90 $OUT/af-ring.txt
echo "=== 5. after ==="; sudo $T/navi48test counters; faults
sudo $T/navi48test log 2>/dev/null > $OUT/log-after-probe.txt
echo "mirror/bind_at lines total: $(grep -cE 'gart-mirror|bind_at' $OUT/log-after-probe.txt)"
grep -E "gart-mirror|bind_at" $OUT/log-after-probe.txt | tail -25
echo "=== 6. where the probe is stuck ==="
sudo dtrace -n 'profile-199 /execname == "computeprobe"/ { @[stack(16)] = count(); }' -c "/bin/sleep 5" 2>&1 | tail -45
echo "full results in $OUT — REBOOT before the next measurement"
