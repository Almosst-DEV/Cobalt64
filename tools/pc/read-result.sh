#!/bin/zsh
# read-result.sh — dump everything the bring-up kext reported this boot. RUN ON THE PC (or via ssh).
#   read-result.sh            kext kernel log (this boot) + IORegistry properties
#   read-result.sh log        only the log
#   read-result.sh ioreg      only the properties
set -u
what=${1:-all}
if [[ $what == all || $what == log ]]; then
  echo "=== Navi48Bringup kernel log (this boot: up $(uptime | sed -E 's/.*up ([^,]*),.*/\1/')) ==="
  mins=$(( $(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/' | xargs -I{} sh -c 'echo $(( ($(date +%s) - {}) / 60 + 2 ))') ))
  sudo log show --last "${mins}m" --info --predicate 'process == "kernel" AND eventMessage CONTAINS "Navi48Bringup"' --style compact 2>/dev/null \
    | grep -v '^Timestamp\|^Filtering' | sed -E 's/^[0-9-]+ ([0-9:.]+)[^N]*Navi48Bringup: /\1 /'
fi
if [[ $what == all || $what == ioreg ]]; then
  echo; echo "=== IORegistry: Navi48Bringup ==="
  ioreg -rc Navi48Bringup -l -w0 2>/dev/null | grep -E '"[A-Za-z0-9]+,[A-Za-z0-9]+" =' | sed -E 's/^[ |]+//' | sort
  echo; echo "boot-args: $(sysctl -n kern.bootargs)"
  echo "loaded: $(kmutil showloaded --list-only 2>/dev/null | grep -iE 'navi48|rdna4' | awk '{print $6, $7}' | tr '\n' ' ')"
fi
