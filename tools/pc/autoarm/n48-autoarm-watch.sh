#!/bin/bash
# n48-autoarm-watch.sh - started by n48-autoarm.sh after its ONE WindowServer restart. Same logic as the n48watch.sh inside tools/pc/arm-gpu-desktop.sh:
# when the WindowServer pid changes again, remove the headless flag and the start counter so a later WindowServer start is not forced headless.
PATH=/usr/bin:/bin:/usr/sbin:/sbin
LOG=${N48_WATCH_LOG:-/var/tmp/n48watch.log}; TMPD=${AA_TMP:-/private/tmp}
P=$(pgrep -x WindowServer); echo "autoarm-v1 start $(date) ws=$P" >> "$LOG"
while true; do
  Q=$(pgrep -x WindowServer)
  if [ -n "$Q" ] && [ "$Q" != "$P" ]; then rm -f "$TMPD/n48m-headless-no" "$TMPD/n48m-starts"; echo "$(date) ws $P -> $Q: flag+counter removed" >> "$LOG"; P=$Q; fi
  sleep 0.5
done
