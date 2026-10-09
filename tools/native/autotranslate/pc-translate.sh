#!/bin/bash
# Navi48 on-PC auto-translator (native #12, hardened in C1). Runs as the unprivileged system user `nobody` (LaunchDaemon com.navi48.translate, UserName); files live in /usr/local/navi48/.
# Dump area: /private/tmp/n48m/<uid>/<sha>.air (+ sidecars), one subdirectory per uid, made by the bundle (n48_gate.h). Every 0.5 s, for each numeric subdirectory and each <64 hex>.air in it:
#   - the subdirectory must be a real directory (not a symlink) owned by the uid in its name, the file a real regular file (not a symlink) owned by that same uid, size 1 B .. 4 MiB;
#   - not already decided under the CURRENT translator key (see KEY), not already in the bundle's Resources/spvcache;
#   - then ONE translation under: sandbox-exec (n48-translate.sb: read dumps + translator, write only OUT and STATE, no network), ulimit -t 25 CPU s (ulimit -v/-d are NOT enforced on macOS), a 1 GiB resident-memory watchdog and a 20 s wall-clock
#     kill inside pc-translate.py (process-group kill), plus its own SIGALRM backstop at 30 s. Meta installed first, spv last (pc-translate.py).
# Done-list lines: "<sha> bundle" | "<sha> <KEY> ok" | "<sha> <KEY> FAIL". KEY = a hash of the translator (version tag, metal2vulkan, pc-translate.py, n48-llvm-dis, overrides, validator, profile): a changed
# translator RE-TRANSLATES EVERY input whose AIR is still in the dump dir, old FAILs AND old oks (a correctness fix must replace previously-ok outputs). Legacy "<sha> ok"/"<sha> FAIL" lines count as an old key.
# Before a re-translation the installed <sha>.meta.json/.spv are COPIED to $STATE/backup/<old key>/ (only the last 2 backup keys are kept; pruning removes plain files + rmdir, only inside $STATE/backup); the new files then replace them by rename (meta first,
# spv last, pc-translate.py), so a hot-swapping app never sees a half-written or missing file. If the new translation FAILs the old outputs are dropped from OUT (the backup keeps them). An ok entry whose AIR is gone (/tmp cleared) keeps its outputs and is logged once
# as "stale-kept (no AIR)" (state file $STATE/stale-logged).
# Env overrides (tests): N48_SRC N48_OUT N48_STATE N48_BUNDLE N48_HERE N48_ONCE=1 (one pass, then exit) N48_NO_SANDBOX=1 N48_SKIP_OWNER=1 (skip the uid-ownership rule) N48_POLL (seconds).
SRC=${N48_SRC:-/private/tmp/n48m}
OUT=${N48_OUT:-/private/var/tmp/n48m-spv}
STATE=${N48_STATE:-/private/var/tmp/n48m-translate-state}
BUNDLE=${N48_BUNDLE:-/Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/spvcache}
HERE=${N48_HERE:-$(cd "$(dirname "$0")" && pwd)}
DONE=$STATE/done
LOG=$STATE/translate.log
POLL=${N48_POLL:-0.5}
MAXBYTES=4194304
TVER="n48tr-2"
umask 022
mkdir -p "$STATE" "$STATE/tmp" "$OUT" 2>/dev/null; chmod 755 "$OUT" 2>/dev/null; chmod 700 "$STATE" 2>/dev/null; touch "$DONE"
export TMPDIR=$STATE/tmp

# The validator: a real spirv-val next to the scripts when present (x86_64 build, INSTALL.md), else the stub (always "valid").
PY=/Library/Developer/CommandLineTools/usr/bin/python3; [ -x "$PY" ] || PY=/usr/bin/python3   # the real binary: the /usr/bin shim needs xcrun's cache in /var/folders (denied in the sandbox)
if [ -x "$HERE/spirv-val" ]; then VAL=$HERE/spirv-val; else VAL=$HERE/n48-spirv-val-stub; fi
hashfile() { [ -f "$1" ] && /usr/bin/shasum -a 256 "$1" | cut -c1-16 || echo none; }
KEY=$(printf '%s %s %s %s %s %s %s' "$TVER" "$(hashfile "$HERE/metal2vulkan")" "$(hashfile "$HERE/pc-translate.py")" "$(hashfile "$HERE/n48-llvm-dis")" "$(hashfile "$HERE/overrides.txt")" "$(hashfile "$VAL")" "$(hashfile "$HERE/n48-translate.sb")" | /usr/bin/shasum -a 256 | cut -c1-12)
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
log "start (src=$SRC out=$OUT state=$STATE key=$KEY validator=$VAL sandbox=$([ -n "$N48_NO_SANDBOX" ] && echo OFF || echo on) uid=$(id -u))"

# stat -f (lstat semantics): %u owner, %z size; [ -L ] / [ -d ] / [ -f ] reject symlinks and other types
decided() {  # $1 sha: 0 = nothing to do
  grep -q "^$1 bundle\$" "$DONE" && return 0
  grep -q "^$1 $KEY " "$DONE" && return 0
  # (an ok under an OLD key, or a legacy "<sha> ok", is NOT decided: it is re-translated while the AIR exists)
  return 1
}
oldkey_of() {  # $1 sha: the key of its last done line ("legacy" for old-format lines or none)
  local l; l=$(grep "^$1 " "$DONE" | tail -1); set -- $l
  if [ $# -ge 3 ]; then echo "$2"; else echo legacy; fi
}
backup_old() {  # $1 sha: copy installed outputs to backup/<old key>/ (the pair stays in place for the app); 0 = something saved
  local e bk
  [ -f "$OUT/$1.spv" ] || [ -f "$OUT/$1.meta.json" ] || return 1
  bk=$STATE/backup/$(oldkey_of "$1"); mkdir -p "$bk"
  for e in meta.json spv; do
    if [ -f "$OUT/$1.$e" ]; then cp -p -- "$OUT/$1.$e" "$bk/$1.$e.tmp" && mv -f -- "$bk/$1.$e.tmp" "$bk/$1.$e"; fi
  done
  log "$1 backup of old outputs -> ${bk##*/}"
  prune_backups
  return 0
}
drop_old() {  # $1 sha: the new translation failed; the old (possibly wrong) outputs leave OUT (spv first, then meta); they are in the backup
  rm -f -- "$OUT/$1.spv"; rm -f -- "$OUT/$1.meta.json"
  log "$1 old outputs dropped (new translation FAILed; kept in backup)"
}
prune_backups() {  # keep the 2 most recently touched backup key dirs; files + rmdir only, never outside $STATE/backup
  local n=0 b p f
  for b in $(ls -t "$STATE/backup" 2>/dev/null); do
    n=$((n+1)); [ $n -le 2 ] && continue
    case "$b" in *[!0-9a-zA-Z]*|"") continue;; esac
    p=$STATE/backup/$b; [ -d "$p" ] && [ ! -L "$p" ] || continue
    for f in "$p"/*; do if [ -f "$f" ] && [ ! -L "$f" ]; then rm -f -- "$f"; fi; done
    rmdir -- "$p" 2>/dev/null; log "backup $b pruned"
  done
}
stale_scan() {  # ok entries (old key) whose AIR is gone: outputs stay, logged once per (sha, key)
  local sha a b k st d have
  while read -r sha a b; do
    if [ -n "$b" ]; then k=$a; st=$b; else k=legacy; st=$a; fi
    [ "$st" = ok ] && [ "$k" != "$KEY" ] && [ "$k" != bundle ] || continue
    [ -f "$OUT/$sha.spv" ] || continue
    have=
    for d in "$SRC"/[0-9]*; do [ -e "$d/$sha.air" ] || [ -L "$d/$sha.air" ] && { have=1; break; }; done
    [ -z "$have" ] || continue
    grep -q "^$sha $k\$" "$STATE/stale-logged" 2>/dev/null && continue
    echo "$sha $k" >> "$STATE/stale-logged"; log "$sha stale-kept (no AIR) old-key=$k"
  done < <(awk '{l[$1]=$0} END{for (s in l) print l[s]}' "$DONE")
}
translate_one() {  # $1 file $2 sha ; prints "OK ..." / "FAIL ..."
  local air=$1
  if [ -n "$N48_NO_SANDBOX" ]; then
    ( ulimit -t 25; exec "$PY" "$HERE/pc-translate.py" "$air" --out "$OUT" --m2v "$HERE/metal2vulkan" --overrides "$HERE/overrides.txt" --validator "$VAL" --timeout 20 --max-bytes $MAXBYTES ) 2>>"$LOG" | head -3
  else
    ( ulimit -t 25; exec /usr/bin/sandbox-exec -f "$HERE/n48-translate.sb" -D "SRC=$SRC" -D "HERE=$HERE" -D "OUT=$OUT" -D "STATE=$STATE" \
        "$PY" "$HERE/pc-translate.py" "$air" --out "$OUT" --m2v "$HERE/metal2vulkan" --overrides "$HERE/overrides.txt" --validator "$VAL" --timeout 20 --max-bytes $MAXBYTES ) 2>>"$LOG" | head -3
  fi
}
scan_once() {
  local d uid f sha o sz msg had
  for d in "$SRC"/[0-9]*; do
    [ -d "$d" ] || continue
    uid=${d##*/}
    case "$uid" in *[!0-9]*) continue;; esac
    [ -d "$d" ] && [ ! -L "$d" ] || continue
    read -r o <<<"$(stat -f '%u' "$d" 2>/dev/null)"
    if [ -z "$N48_SKIP_OWNER" ] && [ "$o" != "$uid" ]; then continue; fi
    for f in "$d"/*.air; do
      [ -e "$f" ] || [ -L "$f" ] || continue
      sha=${f##*/}; sha=${sha%.air}
      case "$sha" in *[!0-9a-f]*) continue;; esac
      [ ${#sha} -eq 64 ] || continue
      [ -r "$f" ] || continue   # unreadable (not yet granted / mid-write): not a verdict, retry next pass
      decided "$sha" && continue
      [ -f "$f" ] && [ ! -L "$f" ] || continue
      read -r o sz <<<"$(stat -f '%u %z' "$f" 2>/dev/null)"
      if [ -z "$N48_SKIP_OWNER" ] && [ "$o" != "$uid" ]; then continue; fi
      if [ -f "$BUNDLE/$sha.spv" ]; then echo "$sha bundle" >> "$DONE"; continue; fi
      if [ "$sz" -lt 1 ] || [ "$sz" -gt $MAXBYTES ]; then log "$sha uid=$uid REJECT size $sz"; backup_old "$sha" && drop_old "$sha"; echo "$sha $KEY FAIL" >> "$DONE"; continue; fi
      had=; backup_old "$sha" && had=1
      msg=$(translate_one "$f" "$sha")
      log "$sha uid=$uid $msg"
      case "$msg" in OK*) echo "$sha $KEY ok" >> "$DONE";; *) [ -n "$had" ] && drop_old "$sha"; echo "$sha $KEY FAIL" >> "$DONE";; esac
    done
  done
}
first=1
while true; do
  scan_once
  if [ -n "$first" ]; then stale_scan; first=; fi
  [ -n "$N48_ONCE" ] && exit 0
  sleep "$POLL"
done
