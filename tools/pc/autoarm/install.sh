#!/bin/bash
# install.sh - for the reviewer, ON THE PC, as root:  sudo ./install.sh [--load]
# Copies n48-autoarm.sh + n48-autoarm-watch.sh to "/Library/Application Support/Navi48/autoarm/" (root:wheel 755), the plist to /Library/LaunchDaemons (root:wheel 644), creates
# /Library/Logs/Navi48 (root:wheel 755) and, if absent, "/Library/Application Support/Navi48/apps.txt" (Maps, Preview; 644). Writes autoarm/install.manifest listing exactly what it added.
# WITHOUT --load nothing runs now: launchd loads the daemon at the next boot (the script then runs once per boot). WITH --load it is bootstrapped now, which RUNS IT NOW (RunAtLoad):
# it refuses by itself when a user is logged in, when the boot is not fresh (armed / nub published) or when the kill switch exists.
# Kill switch (never installed, you create it): touch "/Library/Application Support/Navi48/autoarm-off".  Opt-in one-shot warm reboot on the 1080p fallback:
# touch "/Library/Application Support/Navi48/autoarm-reboot-on-fallback".
# Test override: AA_ROOT=<fake root> (then no chown, no launchctl).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
R=${AA_ROOT:-}
SUPD="$R/Library/Application Support/Navi48"; DIR="$SUPD/autoarm"; LOGD="$R/Library/Logs/Navi48"; PL="$R/Library/LaunchDaemons/com.navi48.autoarm.plist"; MAN="$DIR/install.manifest"
LOAD=0; [ "${1:-}" = "--load" ] && LOAD=1
if [ -z "$R" ] && [ "$(id -u)" != 0 ]; then echo "install.sh: run as root (sudo)"; exit 2; fi
for f in n48-autoarm.sh n48-autoarm-watch.sh com.navi48.autoarm.plist; do [ -f "$HERE/$f" ] || { echo "install.sh: $HERE/$f missing"; exit 2; }; done
if [ -f "$MAN" ]; then echo "install.sh: already installed ($MAN exists); run uninstall.sh first"; exit 2; fi
own() { [ -n "$R" ] || chown root:wheel "$@"; }
ADDED=""
APPS_DEFAULT='# one executable name per line (first 16 characters count); re-added with accel appallow add at every boot after a healthy arm
Maps
Preview'
mkd() { if [ ! -d "$1" ]; then mkdir -p "$1"; chmod 755 "$1"; own "$1"; ADDED="$ADDED
dir $1"; fi; }
mkd "$R/Library/Application Support"; mkd "$SUPD"; mkd "$DIR"; mkd "$LOGD"; mkd "$R/Library/LaunchDaemons"
put() { install -m "$3" "$1" "$2"; own "$2"; ADDED="$ADDED
file $2"; }
put "$HERE/n48-autoarm.sh" "$DIR/n48-autoarm.sh" 755
put "$HERE/n48-autoarm-watch.sh" "$DIR/n48-autoarm-watch.sh" 755
put "$HERE/com.navi48.autoarm.plist" "$PL" 644
if [ ! -e "$SUPD/apps.txt" ]; then printf '%s\n' "$APPS_DEFAULT" >"$SUPD/apps.txt"; chmod 644 "$SUPD/apps.txt"; own "$SUPD/apps.txt"; ADDED="$ADDED
conf $SUPD/apps.txt"; fi
printf '%s\n' "$ADDED" | sed '/^$/d' >"$MAN"; chmod 644 "$MAN"; own "$MAN"
echo "installed:"; sed 's/^/  /' "$MAN"
if [ $LOAD = 1 ] && [ -z "$R" ]; then launchctl bootstrap system /Library/LaunchDaemons/com.navi48.autoarm.plist && echo "bootstrapped (the script is running now; see /Library/Logs/Navi48/autoarm.log)"; else echo "not loaded: runs at the next boot (or: sudo launchctl bootstrap system /Library/LaunchDaemons/com.navi48.autoarm.plist)"; fi
