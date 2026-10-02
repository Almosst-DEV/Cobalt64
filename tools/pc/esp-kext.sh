#!/bin/zsh
# esp-kext.sh — put the staged Navi48Bringup.kext on the ESP so OpenCore injects it.
# RUN ON THE PC.
#
#   esp-kext.sh install    copy ~/navi48-staging/Navi48Bringup.kext -> EFI/OC/Kexts/
#   esp-kext.sh remove     delete it from the ESP
#   esp-kext.sh show       what is on the ESP right now
#
# WHY THIS EXISTS
# ---------------
# A kext in /Library/Extensions goes into the Auxiliary Kernel Collection, and
# macOS will not put it there until a human clicks Allow in System Settings —
# once per rebuilt binary, forever. On a multi-year project driven over SSH that
# is the single blocking step. No csr-active-config bit removes it: the consent
# check is kernelmanagerd's own database (measured, twice).
#
# OpenCore injects kexts into the boot collection before the kernel starts, so
# they never reach kernelmanagerd and never need consent. That is how Lilu,
# VirtualSMC, AppleALC and the ethernet driver already load on this machine.
# Putting our kext there removes the click entirely.
#
# SAFETY
# ------
# An injected kext that panics panics at every boot, which is worse than one in
# the Aux collection. Two things keep that survivable:
#   1. Navi48Bringup::probe returns nullptr unless boot-arg navi48bringup=1, so
#      on the `production` config it is inert no matter how broken the build is.
#   2. The rescue partition on the USB stick boots with rdna4-off=1 and no
#      bring-up at all.
# Recovery from a bad build is therefore: boot `production` (or the stick), then
# `esp-kext.sh remove`.
#
# The kext must NOT also live in /Library/Extensions — that would load it twice.
# `install` says so if it finds one.
set -eu
STAGED=~/navi48-staging/Navi48Bringup.kext
MNT=/Volumes/N48-ESP

store=$(diskutil info / | awk -F': *' '/APFS Physical Store/{print $2; exit}')
[[ -n "$store" ]] || { echo "cannot find the APFS physical store of /"; exit 1; }
disk=$(echo "$store" | sed -E 's#^(/dev/)?(disk[0-9]+)s[0-9]+$#\2#')
esp=$(diskutil list "$disk" | awk '$2=="EFI"{print $NF; exit}')
[[ -n "$esp" ]] || { echo "no EFI partition on $disk"; exit 1; }

# The ESP may already be mounted somewhere else (diskutil picks /Volumes/EFI by
# default). Reuse that mount rather than failing, and only unmount on exit if we
# were the ones who mounted it.
MOUNTED_BY_US=0
existing=$(mount | awk -v d="/dev/$esp" '$1==d {for(i=1;i<=NF;i++) if($i=="on"){print $(i+1); exit}}')
if [[ -n "$existing" ]]; then
  MNT="$existing"
else
  sudo mkdir -p "$MNT"
  sudo diskutil mount -mountPoint "$MNT" "$esp" >/dev/null
  MOUNTED_BY_US=1
fi
cleanup() { [[ "$MOUNTED_BY_US" == 1 ]] && { diskutil unmount "$MNT" >/dev/null 2>&1 || diskutil unmount force "$MNT" >/dev/null 2>&1; } || true; }
trap cleanup EXIT
[[ -f "$MNT/EFI/OC/OpenCore.efi" ]] || { echo "$esp (at $MNT) has no OpenCore — refusing"; exit 1; }
DEST="$MNT/EFI/OC/Kexts/Navi48Bringup.kext"

case "${1:-show}" in
  install)
    [[ -d "$STAGED" ]] || { echo "nothing staged at $STAGED — run tools/stage-to-pc.sh first"; exit 1; }
    if [[ -d /Library/Extensions/Navi48Bringup.kext ]]; then
      echo "WARNING: /Library/Extensions/Navi48Bringup.kext still exists."
      echo "         Remove it and rebuild the collection, or the kext loads twice:"
      echo "           sudo rm -rf /Library/Extensions/Navi48Bringup.kext"
      echo "           sudo kmutil install --volume-root / --update-all"
    fi
    sudo rm -rf "$DEST"
    sudo mkdir -p "$MNT/EFI/OC/Kexts"
    sudo cp -R "$STAGED" "$DEST"
    # The ESP is FAT32: no xattrs, no symlinks, nothing to strip. Just make sure
    # the executable and Info.plist survived the copy.
    [[ -f "$DEST/Contents/MacOS/Navi48Bringup" && -f "$DEST/Contents/Info.plist" ]] \
      || { echo "copy looks incomplete — check $DEST"; exit 1; }
    sync
    ver=$(/usr/libexec/PlistBuddy -c "Print CFBundleVersion" "$DEST/Contents/Info.plist")
    echo "installed Navi48Bringup.kext $ver to the ESP (OpenCore will inject it at boot)"
    echo "the boot config must list it under Kernel > Add — the staged configs do"
    ;;
  remove)
    sudo rm -rf "$DEST"; sync; echo "removed Navi48Bringup.kext from the ESP" ;;
  show)
    echo "ESP $esp mounted at $MNT"
    if [[ -d "$DEST" ]]; then
      echo "injected kext: $(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$DEST/Contents/Info.plist")"
    else
      echo "injected kext: (none)"
    fi
    echo "Kexts on the ESP: $(ls "$MNT/EFI/OC/Kexts" 2>/dev/null | tr '\n' ' ')"
    ;;
  *) echo "usage: esp-kext.sh install|remove|show"; exit 2 ;;
esac
