#!/bin/zsh
# test-dumpdir.sh - runs the REAL n48_dump_dir_ready() (extracted from Navi48Device.m) against a scratch dump root. Everything happens inside ONE mktemp dir (prefix-checked before it is removed);
# every symlink points inside it; the real /tmp/n48m is never touched (the root is compiled in with -DN48G_DUMP_DIR).
#   bundle 14 repair (queue 279, 6 of 8 failed): (1) the test's output buffer was 64 bytes, shorter than a scratch path under $TMPDIR (/var/folders/...), so the helper returned 0 silently before it did anything - it is 512 now (the production buffer, char sub[64] for /tmp/n48m/<uid>, is not affected); (2) macOS 27 `ls -le` prints the nobody ACL entry as its UUID, not as user:nobody - both spellings are accepted.
#   usage: test-dumpdir.sh   (N48_TEST_TMP = parent of the scratch dir, default $TMPDIR, else /private/tmp)
set -u
D="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d "${N48_TEST_TMP:-${TMPDIR:-/private/tmp}}/n48dumpdir.XXXXXX") || exit 2; W=${W:A}
case "$W" in */n48dumpdir.??????) ;; *) echo "unexpected scratch dir '$W'"; exit 2;; esac
trap 'case "$W" in */n48dumpdir.??????) [ -d "$W" ] && rm -rf -- "$W";; esac' EXIT
python3 - "$D/Navi48Device.m" "$W/t.c" <<'PY'
import sys
s=open(sys.argv[1]).read()
a=s.index('static int n48_dump_dir_ready(char *sub, size_t subn) {'); b=s.index('// A dump file appears under its final name only when complete')
h='''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "n48_gate.h"
#include "n48_dumpacl.h"
#define N48LOG(fmt, ...) printf("LOG: " fmt "\\n", ##__VA_ARGS__)
#define N48_ONCE(fmt, ...) N48LOG(fmt, ##__VA_ARGS__)
'''
m='int main(void){ char sub[512]; sub[0]=0; int r=n48_dump_dir_ready(sub,sizeof sub); printf("RESULT %d %s\\n",r,r?sub:""); return 0; }\n'
open(sys.argv[2],'w').write(h+s[a:b]+m)
PY
ROOT=$W/n48m
cc -O1 -Wall -Wextra -Werror -I"$D" -DN48G_DUMP_DIR="\"$ROOT\"" -o "$W/t" "$W/t.c" || { echo "compile failed"; exit 2; }
fails=0; runs=0
check() { runs=$((runs+1)); if ! eval "$2"; then fails=$((fails+1)); echo "FAIL: $1"; fi; }
ME=$(id -u)
out=$("$W/t"); check "fresh: parent created 1777, subdirectory <uid> 0700, ready" "[[ \"\$out\" == *'RESULT 1 '* ]] && [ \"\$(stat -f %Mp%Lp $ROOT)\" = 1777 ] && [ \"\$(stat -f %Lp $ROOT/$ME)\" = 700 ]"
check "the daemon user got an ACL entry on the subdirectory, the mode stayed 0700" "ls -led $ROOT/$ME | /usr/bin/grep -q -E '(user:nobody|FFFFEEEE-DDDD-CCCC-BBBB-AAAAFFFFFFFE) allow'"
out=$("$W/t"); check "second run: still ready, nothing changed" "[[ \"\$out\" == *'RESULT 1 '* ]] && [ \"\$(stat -f %Lp $ROOT/$ME)\" = 700 ]"
chmod 0755 "$ROOT"; out=$("$W/t"); check "a parent of ours at 0755 is repaired to 1777" "[[ \"\$out\" == *'RESULT 1 '* ]] && [ \"\$(stat -f %Mp%Lp $ROOT)\" = 1777 ]"
chmod 0777 "$ROOT/$ME"; out=$("$W/t"); check "a group/other-writable subdirectory of ours is narrowed to 0700" "[[ \"\$out\" == *'RESULT 1 '* ]] && [ \"\$(stat -f %Lp $ROOT/$ME)\" = 700 ]"
# planted symlinks, targets INSIDE the scratch dir
mkdir "$W/elsewhere"; chmod 0755 "$W/elsewhere"
rmdir "$ROOT/$ME" 2>/dev/null || true
ln -s "$W/elsewhere" "$ROOT/$ME"
out=$("$W/t"); check "a symlink planted as the subdirectory is refused and its target is untouched" "[[ \"\$out\" == *'RESULT 0'* ]] && [ \"\$(stat -f %Lp $W/elsewhere)\" = 755 ]"
[ -L "$ROOT/$ME" ] && rm -- "$ROOT/$ME"
mv "$ROOT" "$W/n48m.old"
ln -s "$W/elsewhere" "$ROOT"
out=$("$W/t"); check "a symlink planted as the PARENT is refused, nothing is created or chmod'ed through it" "[[ \"\$out\" == *'RESULT 0'* ]] && [ \"\$(stat -f %Lp $W/elsewhere)\" = 755 ] && [ -z \"\$(ls -A $W/elsewhere)\" ]"
[ -L "$ROOT" ] && rm -- "$ROOT"
mkdir "$ROOT"; chmod 0700 "$ROOT"; out=$("$W/t"); check "a 0700 parent of ours (what a pre-build-6 WindowServer made) is repaired and used" "[[ \"\$out\" == *'RESULT 1 '* ]] && [ \"\$(stat -f %Mp%Lp $ROOT)\" = 1777 ]"
echo "test-dumpdir: $runs checks, $fails failed"; [ $fails -eq 0 ]
