#!/bin/bash
# Arm the GPU (Metal) desktop on the DP. 0 users only (restarts WindowServer). Recipe from queue 146/151/265.
# Runs ON the PC (copy it there; paths below are the PC's).  Based on the reviewer's working copy, with two changes:
#   1. the first nub check no longer attempts a second publish when the nub is already published: `n48nub status` prints "Navi48MetalNub present, registry ID ..." (exit 0) or
#      "NO Navi48MetalNub in the registry" (exit 1); the old grep for "state 1\|published" never matched either text, so it always published twice;
#   2. the app-crash-study backstop (bundle 9, an internal design note "App crash study" item 1): CI_USE_MTL_DAG_FOR_CIKL_SRC=0 in the user launchd domain, for processes that
#      call a Core Image string kernel before any Metal device exists (the bundle's constructor sets it for every process that loads the bundle, but that is too late for those).
T=~/navi48-staging/navi48test
who | grep -q . && { echo "users logged in - refusing (restarts WindowServer)"; exit 2; }
if sudo -n ~/n48-metal/n48nub status 2>&1 | grep -q "Navi48MetalNub present"; then
  echo "nub already published: not publishing again"
else
  sudo -n ~/n48-metal/n48nub publish 2>&1 | tail -1
fi
sleep 2
sudo -n $T accel pipeadopt 2>&1 | grep -E "status"
sudo -n $T accel pipeagdc 1 2>&1 | grep -E "agdc status"
sudo -n $T accel fbname 1 2>&1 | grep -E "class name NOW"
sudo -n touch /private/tmp/n48m-headless-no; sudo -n chmod 644 /private/tmp/n48m-headless-no
sudo -n $T accel pipearm 1 2>&1 | grep 0xccf
sudo -n $T accel pipereload 2>&1 | grep -i "restart window"
sudo -n killall -9 WindowServer; sleep 15
cat > /tmp/n48watch.sh <<'W'
#!/bin/bash
LOG=/var/tmp/n48watch.log; P=$(pgrep -x WindowServer); echo "v2 start $(date) ws=$P" >> $LOG
while true; do
  Q=$(pgrep -x WindowServer)
  if [ -n "$Q" ] && [ "$Q" != "$P" ]; then rm -f /private/tmp/n48m-headless-no /private/tmp/n48m-starts; echo "$(date) ws $P -> $Q: flag+counter removed" >> $LOG; P=$Q; fi
  sleep 0.5
done
W
sudo -n nohup bash /tmp/n48watch.sh >/dev/null 2>&1 &
sleep 1
# bundle 9 backstop (app crash study item 1). With 0 users there may be no gui/501 launchd domain yet: a failure here is reported, not fatal (the bundle's own constructor still covers every process that creates a Metal device first).
sudo -n launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 || echo "note: launchctl asuser 501 setenv failed (no user domain yet?): the CIKL backstop is NOT in place"
a=$(sudo -n $T accel pipestat 4 2>/dev/null | grep "stamps written"); sleep 3; b=$(sudo -n $T accel pipestat 4 2>/dev/null | grep "stamps written")
echo "before: $a"; echo "after : $b"
