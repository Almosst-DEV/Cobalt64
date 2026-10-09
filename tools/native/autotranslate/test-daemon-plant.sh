#!/bin/bash
# test-daemon-plant.sh - planted breaks for test-daemon.sh: each plant edits a scratch copy of pc-translate.sh / pc-translate.py (text must occur exactly once) and test-daemon.sh must FAIL.
D="$(cd "$(dirname "$0")" && pwd)"; esc=0; tot=0
# scratch copies live in ONE mktemp dir that is prefix-checked before it is removed; nothing else is ever removed
B=$(mktemp -d "${N48_TEST_TMP:-${TMPDIR:-/private/tmp}}/n48dplant.XXXXXX") || exit 2; B=$(cd "$B" && pwd -P)
case "$B" in */n48dplant.??????) ;; *) echo "unexpected scratch dir '$B'"; exit 2;; esac
cleanup() { case "$B" in */n48dplant.??????) [ -d "$B" ] && rm -rf -- "$B";; esac; }; trap cleanup EXIT
plant() {  # file old new desc
  local S; S=$(mktemp -d "$B/p.XXXXXX"); cp "$D"/* "$S/"
  OLD="$2" NEW="$3" F="$S/$1" python3 - <<'PY' || { echo "PLANT '$4': DID NOT APPLY"; esc=$((esc+1)); return; }
import os,sys
s=open(os.environ['F']).read()
if s.count(os.environ['OLD'])!=1: print("count",s.count(os.environ['OLD'])); sys.exit(3)
open(os.environ['F'],'w').write(s.replace(os.environ['OLD'],os.environ['NEW']))
PY
  tot=$((tot+1))
  if bash "$S/test-daemon.sh" >"$S/out" 2>&1; then echo "PLANT '$4': *** ESCAPED ***"; esc=$((esc+1)); else echo "PLANT '$4': CAUGHT ($(grep -m1 '^FAIL' "$S/out" | cut -c1-90))"; fi
}
plant pc-translate.sh '[ -f "$f" ] && [ ! -L "$f" ] || continue' '[ -f "$f" ] || continue' "a symlinked .air is followed"
plant pc-translate.sh 'POLL=${N48_POLL:-0.5}' 'POLL=${N48_POLL:-0.5}; N48_SKIP_OWNER=1' "both uid-ownership rules (directory and file) are off"
plant pc-translate.sh '[ -d "$d" ] && [ ! -L "$d" ] || continue' '[ -d "$d" ] || continue' "a symlinked subdirectory is followed"
plant pc-translate.sh '  grep -q "^$1 $KEY " "$DONE" && return 0' '  grep -q "^$1 " "$DONE" && return 0' "the done-list ignores the translator key (old FAILs never retried)"
plant pc-translate.sh 'if [ "$sz" -lt 1 ] || [ "$sz" -gt $MAXBYTES ]' 'if false' "no input size cap in the daemon"
plant pc-translate.py 'if sz.st_size < 1 or sz.st_size > maxb:' 'if False:' "no size cap in the translator"
plant pc-translate.py 'if data[:4] != b"BC\xc0\xde" and data[:4] != b"\xde\xc0\x17\x0b":' 'if False:' "no bitcode magic check"
plant pc-translate.py 'env["METAL2VULKAN_SPIRV_VAL"] = validator' 'env["METAL2VULKAN_SPIRV_VAL"] = os.path.join(HERE, "n48-spirv-val-stub")' "the real validator is ignored"
plant pc-translate.sh "      case \"\$sha\" in *[!0-9a-f]*) continue;; esac" ":" "non-hex names are accepted"
plant pc-translate.py 'if rss is not None and rss > max_rss_kb:' 'if False:' "the memory watchdog is off"
plant pc-translate.py 'if time.monotonic() - t0 > timeout:' 'if False:' "the wall-clock kill is off"
plant pc-translate.sh '  # (an ok under an OLD key, or a legacy "<sha> ok", is NOT decided: it is re-translated while the AIR exists)' '  if grep -Eq "^$1 [0-9a-f]{12} ok\$" "$DONE" && [ -f "$OUT/$1.spv" ]; then return 0; fi' "key change retries only FAILs again (old oks stay)"
plant pc-translate.sh 'backup_old() {  # $1 sha' 'backup_old() { return 1  # $1 sha' "no backup of the old outputs"
plant pc-translate.py $'install(os.path.join(out, sha + ".meta.json"), json.dumps(m2, separators=(",", ":")).encode())\ninstall(os.path.join(out, sha + ".spv"), b)' $'install(os.path.join(out, sha + ".spv"), b)\ninstall(os.path.join(out, sha + ".meta.json"), json.dumps(m2, separators=(",", ":")).encode())' "spv written before meta"
plant pc-translate.py 'os.chmod(tmp, 0o644); os.rename(tmp, path)' 'os.chmod(tmp, 0o644); open(path, "wb").write(data); os.unlink(tmp)' "outputs written in place (no temp + rename)"
plant pc-translate.sh 'echo "$sha $k" >> "$STATE/stale-logged"; log "$sha stale-kept (no AIR) old-key=$k"' ':' "stale entries are never logged"
plant pc-translate.sh 'echo "$sha $k" >> "$STATE/stale-logged"; ' '' "stale entries are logged on every run"
plant pc-translate.sh '    n=$((n+1)); [ $n -le 2 ] && continue' '    n=$((n+1)); continue' "backups never pruned"
echo "test-daemon-plant: $tot plants, $esc escaped"; [ $esc -eq 0 ]
