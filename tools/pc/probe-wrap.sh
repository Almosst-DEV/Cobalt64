#!/bin/zsh
# Run a Metal probe that may hang, then exit anyway so `dtrace -c` can finish
# and print its END aggregation. The probe is left stuck on purpose; the machine
# must be rebooted before the next measurement.
"$1" > /tmp/trace-probe.txt 2>&1 &
sleep "${2:-15}"
exit 0
