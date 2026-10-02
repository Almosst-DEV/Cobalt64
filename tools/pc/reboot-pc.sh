#!/bin/zsh
# reboot-pc.sh — the ONE way to reboot the PC from a script. RUN ON THE PC (staged to
# ~/navi48-staging/reboot-pc.sh by tools/stage-to-pc.sh; tools/accel-cycle.sh calls it).
#
#   reboot-pc.sh            gates + snapshot, then a graceful `shutdown -r now`
#   reboot-pc.sh --check    gates + snapshot only, never reboots (accel-cycle runs this first)
#   reboot-pc.sh --quick    gates + snapshot, then `sync; reboot -q` (see RISKS; only when a
#                           brief names it — accel-cycle never passes it)
#
# Why (14-: two reboots sent by the reviewer froze the PC and
# needed a power-cycle, and left no shutdownStall report. The second of those boots had a user
# logged in on an ARMED accelerator with GUI Metal clients stuck in uninterruptible wait (U state,
# an earlier analysis). Every scripted reboot, and every one of the nine recorded shutdown stalls,
# came back on its own: the three measured stalls were followed by the next kernel boot 22-25 s
# after the stall report. So this script refuses the two conditions tied to the freezes:
#   exit 3  a console user is logged in          -> STOP: "user logged in - ask them to log out"
#   exit 4  on an ARMED boot, a process other than the known-benign ones is in U state in two samples
#           3 s apart -> STOP and report the list. On an unarmed boot U-state processes are reported,
#           not refused: storagekitd sits in U state from boot on this machine with nothing armed
#, 0.0.207, 55 min up, 0 users) and scripted reboots with it present came back.
# Exit 0 = the reboot command was issued (the ssh session then drops), 2 = usage.
#
# RISKS of --quick: reboot(8) -q skips launchd's orderly termination of processes (it still
# syncs file systems). The ESP is FAT and is unmounted by esp-kext.sh after install, so an
# unflushed ESP is not expected, but a daemon mid-write can lose data. If the freeze is in the
# kernel's own shutdown path (a driver waiting on the GPU) --quick hangs the same way. It has
# never been tried on this machine: first use belongs on an unarmed boot, named by a brief.
set -u
MODE=reboot
case "${1:-}" in
    "")       ;;
    --check)  MODE=check ;;
    --quick)  MODE=quick ;;
    *)        print -r -- "usage: reboot-pc.sh [--check|--quick]"; exit 2 ;;
esac
T=$HOME/navi48-staging

print -r -- "### reboot-pc $MODE at $(date '+%F %T')"
print -r -- "### uptime: $(uptime)"
print -r -- "### boottime: $(sysctl -n kern.boottime 2>&1)"

# --- gate 1: console user ------------------------------------------------------
cu=$(stat -f %Su /dev/console 2>&1)
print -r -- "### console user: $cu"
print -r -- "### who:"; who 2>&1 | sed 's/^/    /'
if [[ $cu != root ]]; then
    print -r -- "REFUSING: '$cu' is logged in at the console - user logged in, ask them to log out "
    exit 3
fi

# --- snapshot: is the accelerator armed? -----------------------------------------
armed=unknown
if [[ -x $T/navi48test ]]; then
    st=$(sudo "$T/navi48test" accel status 2>&1)
    print -r -- "### accel status:"; print -r -- "$st" | sed 's/^/    /'
    if print -r -- "$st" | grep -q 'hook installed = yes'; then armed=yes
    elif print -r -- "$st" | grep -q 'hook installed = no'; then armed=no; fi
else
    print -r -- "### accel status: $T/navi48test missing"
fi
print -r -- "### accelerator armed: $armed"
print -r -- "### newest DiagnosticReports:"
ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -4 | sed 's/^/    /'

# --- gate 2: processes in uninterruptible wait, sampled twice ---------------------
ustate() { ps -axo pid=,stat=,etime=,comm= 2>/dev/null | awk '$2 ~ /U/ {print}'; }
u1=$(ustate); sleep 3; u2=$(ustate)
# Known-benign U-state processes, each seen stuck on a healthy UNARMED boot (see the header).
BENIGN_U=(storagekitd)
stuck="" benign=""
if [[ -n $u1 && -n $u2 ]]; then
    while read -r pid stat etime comm; do
        [[ -z $pid ]] && continue
        print -r -- "$u1" | awk -v p="$pid" '$1==p {found=1} END{exit !found}' || continue
        if (( ${BENIGN_U[(Ie)${comm:t}]} )); then benign="$benign $pid(${comm:t})"; else stuck="$stuck $pid(${comm:t})"; fi
    done <<< "$u2"
fi
print -r -- "### U-state processes (second sample):"
print -r -- "${u2:-    (none)}" | sed 's/^/    /'
[[ -n ${benign// /} ]] && print -r -- "### known-benign U-state:$benign"
if [[ -n ${stuck// /} ]]; then
    if [[ $armed == no ]]; then
        print -r -- "### WARNING: pid(s)$stuck in uninterruptible wait on an UNARMED boot - reported, not refused"
    else
        print -r -- "REFUSING: pid(s)$stuck stayed in uninterruptible wait for 3 s on an armed (or unknown) boot - a reboot may freeze ; report the list above"
        exit 4
    fi
fi

if [[ $MODE == check ]]; then
    print -r -- "### gates passed (--check: no reboot)"
    exit 0
fi

print -r -- "### gates passed; issuing the reboot at $(date '+%T')"
if [[ $MODE == quick ]]; then
    sudo sync
    sudo reboot -q
else
    sudo shutdown -r now
fi
exit 0
