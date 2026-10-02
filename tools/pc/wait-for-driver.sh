#!/bin/sh
# Wait until the bring-up driver is actually READY, not merely until sshd answers.
#
# Why this exists: after a reboot, `ssh navi48 true` succeeds ~30 s in, while APFS
# is still mounting volumes and the bring-up ladder is still running. Commands run
# at that point get "Navi48Bringup not found in the IORegistry" — which reads
# exactly like a driver regression and nearly caused a good build (0.0.106) to be
# reverted on an earlier run. The readiness condition is the SERVICE, not the shell.
#
# Usage:  tools/pc/wait-for-driver.sh [timeout_seconds]   (default 240)
set -u
HOST=navi48
TIMEOUT=${1:-240}
STEP=10
elapsed=0

printf 'waiting for sshd on %s ... ' "$HOST"
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if ssh -o ConnectTimeout=5 -o BatchMode=yes "$HOST" true 2>/dev/null; then
        echo "up (${elapsed}s)"
        break
    fi
    sleep "$STEP"; elapsed=$((elapsed + STEP))
done
if [ "$elapsed" -ge "$TIMEOUT" ]; then echo "TIMEOUT"; exit 1; fi

printf 'waiting for Navi48Bringup to publish and finish the ladder ... '
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    out=$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$HOST" \
          'sudo ~/navi48-staging/navi48test info 2>/dev/null | grep -E "^Stage reached"' 2>/dev/null)
    case "$out" in
        *"Stage reached"*)
            echo "ready (${elapsed}s)"
            echo "  $out"
            exit 0
            ;;
    esac
    sleep "$STEP"; elapsed=$((elapsed + STEP))
done
echo "TIMEOUT after ${TIMEOUT}s — service never became ready"
echo "  (this IS worth investigating; it is not the boot race)"
exit 1
