#!/bin/zsh
# Cross-build the bounded pointer-mover on the host Mac for the PC (x86_64 macOS) and AUDIT its imports.
#   tools/pc/ptrmove     warp-only (default, no event can be created: no CGEvent* import at all)
#   tools/pc/ptrmove-ev  adds --post-moved (CGEventCreateMouseEvent with the constant kCGEventMouseMoved + CGEventPost)
# The build FAILS if `nm -u` of either binary shows a keyboard / button / scroll / text-input / event-tap / event-retype /
# accessibility-action symbol, or ANY symbol outside the exact allow-list below, or (ptrmove-ev) if any call to
# CGEventCreateMouseEvent is not immediately preceded by `movl $0x5, %esi` (type kCGEventMouseMoved = 5).
# Usage and PC launch commands: tools/runkit/ptrmove.md
set -euo pipefail
P="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$P/tools/pc/ptrmove.c"
CFLAGS=(-arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra -Werror
        -framework ApplicationServices -framework IOKit -framework CoreFoundation)

COMMON=(_CGDisplayBounds _CGMainDisplayID _CGPreflightPostEventAccess _CGWarpMouseCursorPosition
        _IOPMAssertionDeclareUserActivity _IOPMAssertionRelease ___CFConstantStringClassReference ___sincos_stret
        ___stack_chk_fail ___stack_chk_guard ___stderrp ___stdoutp _atof _atoi _fflush _fprintf _fwrite _getuid
        _mach_absolute_time _mach_timebase_info _printf _signal _sscanf _strcmp _usleep dyld_stub_binder)
EVONLY=(_CFRelease _CGEventCreateMouseEvent _CGEventPost)

# Anything that can type, click, scroll, focus, retarget or retype an event. Checked before the allow-list so the
# failure names the reason.
DENY='Keyboard|KeyCode|KeyDown|KeyUp|Unicode|Scroll|Button|Click|PostMouseEvent|PostKeyboard|IOHIDPost|NXPost|IOHIDSetMouse|CGEventSetType|CGEventSetIntegerValueField|CGEventSetDoubleValueField|CGEventSetFlags|CGEventSetLocation|CGEventCreateFromData|CGEventCreateCopy|CGEventTap|CGEventPostToP|CGEventSourceSet|CGRequestPostEventAccess|CGRequestListenEventAccess|AXUIElement|AXAPI|TIS|TextInput|NSEvent|objc_msgSend|Focus|dlsym|dlopen|NSLookupSymbol'

audit() {  # $1 binary, rest: allowed symbols
    local bin="$1"; shift
    local -a allow=("$@")
    local syms; syms="$(nm -u "$bin")"
    echo "== nm -u $bin"; print -r -- "$syms"
    if print -r -- "$syms" | /usr/bin/grep -E -q "$DENY"; then
        echo "AUDIT FAIL: forbidden symbol(s) in $bin:"; print -r -- "$syms" | /usr/bin/grep -E "$DENY"; return 1
    fi
    local s bad=0
    for s in ${(f)syms}; do
        (( ${allow[(Ie)$s]} )) || { echo "AUDIT FAIL: $s is not on the allow-list for $bin"; bad=1; }
    done
    (( bad == 0 )) || return 1
    file "$bin" | /usr/bin/grep -q 'x86_64' || { echo "AUDIT FAIL: $bin is not x86_64"; return 1; }
    [[ "$(strings -a "$bin" | /usr/bin/grep -c 'ptrmove-1')" -ge 1 ]] || { echo "AUDIT FAIL: version token missing in $bin"; return 1; }
    echo "AUDIT PASS: $bin"
}

clang "${CFLAGS[@]}"                  -o "$P/tools/pc/ptrmove"    "$SRC"
clang "${CFLAGS[@]}" -DPTRMOVE_EVENTS -o "$P/tools/pc/ptrmove-ev" "$SRC"

audit "$P/tools/pc/ptrmove"    "${COMMON[@]}"
audit "$P/tools/pc/ptrmove-ev" "${COMMON[@]}" "${EVONLY[@]}"

# ptrmove-ev: every call to CGEventCreateMouseEvent must pass type 5 (kCGEventMouseMoved) in %esi.
DIS="$(otool -tvV "$P/tools/pc/ptrmove-ev")"
NCALL="$(print -r -- "$DIS" | /usr/bin/grep -c 'callq.*_CGEventCreateMouseEvent' || true)"
# For each call: the LAST instruction in the 8 before it that writes %esi/%rsi/%si must be `movl $0x5, %esi`, and no
# control transfer (jmp/jcc/call) may sit between that write and the call.
NOK="$(print -r -- "$DIS" | awk '
  { line[NR] = $0 }
  /callq.*_CGEventCreateMouseEvent/ {
    ok = 0
    for (k = NR - 1; k >= NR - 8 && k > 0; k--) {
      if (line[k] ~ /\t(j[a-z]+|callq)\t/) break
      if (line[k] ~ /, %(e|r)?si$/) { ok = (line[k] ~ /\tmovl\t\$0x5, %esi$/); break }
    }
    n += ok
  }
  END { print n + 0 }')"
echo "== ptrmove-ev: CGEventCreateMouseEvent calls $NCALL, each with type = movl \$0x5,%esi: $NOK"
print -r -- "$DIS" | /usr/bin/grep -B6 'callq.*_CGEventCreateMouseEvent'
[[ "$NCALL" -ge 1 && "$NCALL" == "$NOK" ]] || { echo "AUDIT FAIL: a CGEventCreateMouseEvent call without the constant type 5"; exit 1; }
# the warp-only binary must not reference the event API at all
[[ "$(strings -a "$P/tools/pc/ptrmove" | /usr/bin/grep -c 'CGEvent')" == 0 ]] || { echo "AUDIT FAIL: ptrmove references CGEvent"; exit 1; }

echo "BUILD+AUDIT PASS: $P/tools/pc/ptrmove $P/tools/pc/ptrmove-ev"
