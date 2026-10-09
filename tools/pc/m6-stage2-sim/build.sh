#!/bin/bash
# build.sh - build fakecli: a COPY of the real tools/pc/navi48test.c (only dr_call, open_service and main replaced) linked with sim.c. Native arch (no PC contact). Output: $1 (default ./fakecli)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=${N48_ROOT:-$(cd "$HERE/../../.." && pwd)}
OUT=${1:-$HERE/fakecli}
W=$(mktemp -d /private/tmp/m6s2sim-build.XXXXXX)      # own mktemp dir, removed below only if it has that prefix
sed -e 's/^static int dr_call(uint64_t action, uint64_t arg, uint64_t out\[16\]) {/static int dr_call_real(uint64_t action, uint64_t arg, uint64_t out[16]) {/' \
    -e 's/^static int open_service(void) {/static int open_service_real(void) {/' "$ROOT/tools/pc/navi48test.c" >"$W/navi48test_sim.c"
/usr/bin/grep -q 'dr_call_real' "$W/navi48test_sim.c" && /usr/bin/grep -q 'open_service_real' "$W/navi48test_sim.c" || { echo "build.sh: the sed anchors no longer match navi48test.c (dr_call / open_service signatures changed)"; exit 1; }
cat >>"$W/navi48test_sim.c" <<'EOT'
// ---- appended by build.sh: the simulated kernel
int sim_call(unsigned long long action, unsigned long long arg, unsigned long long *out);
static int dr_call(uint64_t action, uint64_t arg, uint64_t out[16]) { return sim_call(action, arg, (unsigned long long *)out); }
static int open_service(void) { return 0; }
EOT
# the stubs must be visible BEFORE their first use: declare them right after the includes instead of at the end
python3 - "$W/navi48test_sim.c" <<'PY'
import sys,re
p=sys.argv[1]; s=open(p).read()
i=s.rindex("// ---- appended by build.sh")
tail=s[i:]; s=s[:i]
m=list(re.finditer(r'^#include .*$', s, re.M))[-1]
decl="\nint sim_call(unsigned long long, unsigned long long, unsigned long long *);\nstatic int dr_call(uint64_t, uint64_t, uint64_t[16]);\nstatic int open_service(void);\n"
s=s[:m.end()]+decl+s[m.end():]+tail.replace("int sim_call(unsigned long long action, unsigned long long arg, unsigned long long *out);\n","")
open(p,'w').write(s)
PY
clang -O1 -w -Dmain=real_main -I "$ROOT/tools/pc" -I "$ROOT/src/navi48-bringup/src/apple" -framework IOKit -framework CoreFoundation -c "$W/navi48test_sim.c" -o "$W/navi48test_sim.o"
clang -O1 -w -I "$ROOT/tools/pc" -c "$HERE/sim.c" -o "$W/sim.o"
clang "$W/navi48test_sim.o" "$W/sim.o" -framework IOKit -framework CoreFoundation -o "$OUT"
case "$W" in /private/tmp/m6s2sim-build.*) rm -r "$W" ;; esac
echo "built $OUT"
