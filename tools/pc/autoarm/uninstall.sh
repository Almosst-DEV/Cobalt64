#!/bin/bash
# uninstall.sh - ON THE PC as root: boots the daemon out (if loaded) and removes exactly the files/dirs listed in autoarm/install.manifest (written by install.sh), nothing else.
# NOT removed because install.sh did not add them: the logs in /Library/Logs/Navi48 (the dir only if install.sh created it AND it is empty), the kill switch, the autoarm-state dir, a pre-existing apps.txt.
# Test override: AA_ROOT=<fake root> (then no launchctl).
set -eu
R=${AA_ROOT:-}
DIR="$R/Library/Application Support/Navi48/autoarm"; MAN="$DIR/install.manifest"
if [ -z "$R" ] && [ "$(id -u)" != 0 ]; then echo "uninstall.sh: run as root (sudo)"; exit 2; fi
[ -f "$MAN" ] || { echo "uninstall.sh: $MAN not found: nothing was installed by install.sh"; exit 2; }
if [ -z "$R" ]; then launchctl bootout system/com.navi48.autoarm 2>/dev/null && echo "booted out" || echo "(not loaded)"; fi
APPS_DEFAULT='# one executable name per line (first 16 characters count); re-added with accel appallow add at every boot after a healthy arm
Maps
Preview'
LINES=$(cat "$MAN"); rm -f -- "$MAN"
# files first
while IFS= read -r line; do
  case "$line" in file\ *) p=${line#file }
    case "$p" in "$R"/Library/*) rm -f -- "$p"; echo "removed $p" ;; *) echo "REFUSING to remove '$p' (not under $R/Library)" ;; esac ;;
  conf\ *) p=${line#conf }   # apps.txt: removed only while it is still exactly the text install.sh wrote
    if [ -f "$p" ] && [ "$(cat "$p")" = "$APPS_DEFAULT" ]; then rm -f -- "$p"; echo "removed $p"; else echo "kept $p (edited by hand)"; fi ;; esac
done <<EOT
$LINES
EOT
# then directories, deepest first, only if empty (rmdir never removes content)
printf '%s\n' "$LINES" | tail -r | while IFS= read -r line; do
  case "$line" in dir\ *) p=${line#dir }; case "$p" in "$R"/Library/*) rmdir -- "$p" 2>/dev/null && echo "removed dir $p" || echo "kept dir $p (not empty)" ;; esac ;; esac
done
