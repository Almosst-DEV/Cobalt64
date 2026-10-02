#!/bin/bash
# Navi48Accel verify: READ-ONLY. Run ON the PC (e.g. `ssh navi48 'bash -s' < verify.sh`). Changes nothing. Prints PASS/FAIL/INFO lines.
# PHASE 1 (after the aux rebuild + reboot, nub NOT yet published): the kext is in the aux KC at 0.0.3; it is NOT loaded yet (nothing runs at boot).
# PHASE 2 (after `n48nub publish`): the nub, the accelerator under it, the layout gate line, the family start.
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
ok(){ echo "PASS  $*"; }; no(){ echo "FAIL  $*"; }; inf(){ echo "INFO  $*"; }
echo "== Navi48Accel verify $(date '+%F %T') =="
inf "macOS $(sw_vers -productVersion) build $(sw_vers -buildVersion) (expect 26.6.2 / 25G83)"
inf "boot-args: $(nvram boot-args 2>/dev/null | cut -f2)"
[ -d /Library/Extensions/Navi48Bringup.kext ] && no "Navi48Bringup.kext is in /Library/Extensions (K4: loads twice; must be ESP only: move it to ~/navi48-staging/backup/)" || ok "no Navi48Bringup.kext in /Library/Extensions"
K=/Library/Extensions/Navi48Accel.kext; [ -d /Library/Extensions/Navi48AccelProbe.kext ] && echo "FAIL  the OLD /Library/Extensions/Navi48AccelProbe.kext (0.0.1) is still installed: same bundle id, remove it"
if [ -d "$K" ]; then inf "installed version $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $K/Contents/Info.plist 2>&1) (expect 0.0.3)"; else inf "$K not installed"; fi
ins=$(kmutil inspect -A "$AUX" 2>&1)
echo "$ins" | /usr/bin/grep -i accelprobe | sed 's/^/INFO  /'
echo "$ins" | /usr/bin/grep -qi accelprobe && ok "[PHASE 1] accelprobe is IN the aux KC" || no "[PHASE 1] accelprobe NOT in the aux KC"
echo "$ins" | /usr/bin/grep -qi RDNA4FB && ok "RDNA4FB still in the aux KC" || no "RDNA4FB MISSING from the aux KC (display driver lost)"
sl=$(kmutil showloaded --list-only 2>&1)
echo "$sl" | /usr/bin/grep -i "accelprobe" | sed 's/^/INFO  /'
echo "$sl" | /usr/bin/grep -q 'com.navi48.bringup' && ok "com.navi48.bringup loaded (ESP)" || inf "com.navi48.bringup not loaded"
nub=$(ioreg -c Navi48MetalNub -r -l 2>/dev/null)
if [ -n "$nub" ]; then
  ok "[PHASE 2] Navi48MetalNub is in the registry"
  echo "$nub" | /usr/bin/grep -E '"model"|Navi48,' | sed 's/^/INFO  /'
  acc=$(ioreg -c Navi48Accelerator -r -l 2>/dev/null)
  if [ -n "$acc" ]; then
    ok "[PHASE 2] Navi48Accelerator is in the registry"
    echo "$acc" | /usr/bin/grep -E 'MetalPluginName|MetalPluginClassName|IOMatchCategory|IOAccelRevision|AccelCaps|IOCFPlugInTypes|"IOClass"|IOProviderClass' | sed 's/^/INFO  /'
    echo "$acc" | /usr/bin/grep -q 'IOAccelRevision' && ok "IOAccelRevision published by the family start" || no "IOAccelRevision missing (family start did not finish?)"
    echo "$acc" | /usr/bin/grep -q '"MetalPluginName" = "Navi48Metal"' && ok "MetalPluginName on the accelerator itself" || no "MetalPluginName missing on the accelerator"
  else no "[PHASE 2] no Navi48Accelerator: see the log lines below"; fi
else inf "[PHASE 1] no Navi48MetalNub (not published: expected before n48nub publish)"; fi
L=$(/usr/bin/log show --last boot --predicate 'eventMessage CONTAINS "n48accel:" OR eventMessage CONTAINS "Navi48Bringup: metal:"' --style compact 2>/dev/null | /usr/bin/grep -v '^Timestamp\|^Filtering\|log run noninteractively')
[ -z "$L" ] && L=$(sudo -n dmesg 2>/dev/null | /usr/bin/grep -E 'n48accel:|Navi48Bringup: metal:')
echo "$L" | /usr/bin/grep -q 'n48accel: layout PASS' && ok "[GATE] runtime layout gate PASS: $(echo "$L" | /usr/bin/grep -m1 'layout PASS' | sed 's/.*n48accel: //')" || inf "no 'n48accel: layout PASS' line (expected only in PHASE 2)"
echo "$L" | /usr/bin/grep -q 'layout PASS 2370/2370' && ok "[GATE] aux 0.0.3: 12 classes / 2370 slots compared (IOAccelDisplayPipe included)" || inf "no 'layout PASS 2370/2370' line (aux 0.0.2 said 2064/2064; expected only in PHASE 2)"
echo "$L" | /usr/bin/grep -q 'layout FAIL' && no "[GATE] layout FAIL: $(echo "$L" | /usr/bin/grep -m1 'layout FAIL' | sed 's/.*n48accel: //')"
echo "$L" | /usr/bin/grep -E 'n48accel:|Navi48Bringup: metal:' | tail -30 | sed 's/^/INFO  /'
inf "panics: $(ls -t /Library/Logs/DiagnosticReports/Kernel* 2>/dev/null | head -1)"
inf "WindowServer / loginwindow pids: $(pgrep -x WindowServer | tr '\n' ' ') / $(pgrep -x loginwindow | tr '\n' ' ')"
