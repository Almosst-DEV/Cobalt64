#!/bin/zsh
# esp-config.sh — swap the OpenCore config on the macOS install disk's ESP. RUN ON THE PC.
#
#   esp-config.sh show                 print which config is live
#   esp-config.sh production           restore the everyday config (RDNA4FB display, no bring-up)
#   esp-config.sh bringup              survey only  (navi48bringup=1 rdna4-off=1)
#   esp-config.sh bringup-psp          PSP stage    (navi48bringup=1 rdna4-off=1 navi48-psp=1)
#   esp-config.sh stage<N>             bring-up ladder to stage N (navi48-stage=N; 6=PSP ring … 15=SDMA)
#
# Configs come from ~/navi48-staging/configs/<name>.plist (scp'd from the host Mac). The current live
# config is always saved as EFI/OC/config.prev.plist first. Never touches the Windows disk or BIOS.
set -eu
STAGE=~/navi48-staging/configs
MNT=/Volumes/N48-ESP

# "/" is an APFS volume on a synthesized disk; its Physical Store names the real partition
# (e.g. disk2s2 on the SanDisk). The ESP is the EFI partition of that physical disk.
store=$(diskutil info / | awk -F': *' '/APFS Physical Store/{print $2; exit}')   # e.g. disk2s2
[[ -n "$store" ]] || { echo "cannot find the APFS physical store of /"; exit 1; }
disk=$(echo "$store" | sed -E 's#^(/dev/)?(disk[0-9]+)s[0-9]+$#\2#')            # e.g. disk2
esp=$(diskutil list "$disk" | awk '$2=="EFI"{print $NF; exit}')                  # e.g. disk2s1
[[ -n "$esp" ]] || { echo "no EFI partition on $disk (store $store)"; diskutil list "$disk"; exit 1; }

# The ESP may already be mounted elsewhere (a bare `diskutil mount` puts it at
# /Volumes/EFI). Reuse that mount instead of failing, and only unmount on exit
# if this script is what mounted it.
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

live_args() { plutil -extract 'NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args' raw -o - "$1" 2>/dev/null || echo '?'; }
case "${1:-show}" in
  show)
    echo "root store $store -> $disk -> ESP $esp (mounted at $MNT)"; echo "live boot-args: $(live_args "$MNT/EFI/OC/config.plist")" ;;
  *)
    src="$STAGE/$1.plist"; [[ -f "$src" ]] || { echo "unknown config '$1' — available: $(ls $STAGE | sed 's/\.plist$//' | tr '\n' ' ')"; exit 2; }
    plutil -lint "$src" >/dev/null
    sudo cp "$MNT/EFI/OC/config.plist" "$MNT/EFI/OC/config.prev.plist"
    sudo cp "$src" "$MNT/EFI/OC/config.plist"
    sync; echo "installed $1: boot-args = $(live_args "$MNT/EFI/OC/config.plist")"
    echo "reboot to use it (spacebar at the picker if the entry is hidden)" ;;
esac
