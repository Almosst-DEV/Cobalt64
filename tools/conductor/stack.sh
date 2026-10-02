#!/bin/bash
# stack.sh: the stack frame (the first `subq $X, %rsp`) of the submit-path functions in a kext
# binary, and optionally the change against a second (later) binary.
#
# usage: tools/conductor/stack.sh [-s <symbol>]... <binary> [<binary-after>]
#   <binary>        a Mach-O, a .kext bundle, or a build dir holding Navi48Bringup.kext
#                   (for example src/navi48-bringup/build-snap447-armg)
#   <binary-after>  optional: print before, after and the delta, and flag any growth over 0x40
#   -s <symbol>     measure this mangled symbol instead of the default list (repeatable)
#
# Default symbols: hook_unmapVA, hook_gfxCommitIB, gfxsrc_decide_frame,
# xlat12_ib_translate_draw_ex and d_table_desc; since 0.0.536 also d_r2d_draw and
# xlat12_ib_rect2d_check (switch 92: the translate body's two new callees; its fallback is a
# second pass of the body itself, so it has no frame of its own).
# Since 0.0.538 also the present path: hw_p73_present (switches 94/95 run inside it), dpg_perform
# (its caller, which now hands switch 95 its copy inputs by address) and the GFX submission's
# timed wrapper hook_gfxCommitIB_timed. Since 0.0.541 also the page walk (gfxc_page, gfxc_page_pf under its 0.0.540 and
# 0.0.541 manglings - one of the two is absent in each build - gfxc_page_walk), the ask that opens switch 98's scope, hook_mapVA
# and the copy guard's open/close (switch 98's generation bumps). Since 0.0.542 also gfxc_page_wc (switch 98's cached walk, the
# 0.0.541 review's SHOULD-FIX) and hook_releaseVMContext (its new switch-98 bracket). A leaf that only pushes shows `absent`. Every build brief caps growth at 0x40 per
# function and moves any new submit-path buffer over 128 B into a file-scope static.
# Each binary's LC_UUID is printed so the table names what it measured.

SYMS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -s) SYMS+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) break ;;
  esac
done
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: $0 [-s <symbol>]... <binary> [<binary-after>]" >&2
  exit 2
fi
if [ ${#SYMS[@]} -eq 0 ]; then
  SYMS=(__ZN3n48L12hook_unmapVAEPvyy
        __ZN3n48L16hook_gfxCommitIBEPvS0_
        __ZN3n48L19gfxsrc_decide_frameEPKhjjPKNS_7WsFrameE
        _xlat12_ib_translate_draw_ex
        _d_table_desc
        _d_r2d_draw
        _xlat12_ib_rect2d_check
        __ZN3n4814hw_p73_presentEyy
        __ZL11dpg_performPvS_
        __ZN3n48L22hook_gfxCommitIB_timedEPvS0_
        __ZN3n48L10hook_mapVAEPvyS0_yyy
        __ZN3n48L12gfxc_page_pfERKNS_6GfxcVmEyRyRbPy
        __ZN3n48L12gfxc_page_pfERKNS_6GfxcVmEyRyRbPyPj
        __ZN3n48L14gfxc_page_walkERKNS_6GfxcVmEyRyRbPyPj
        __ZN3n48L9gfxc_pageERKNS_6GfxcVmEyRyRbPy
        __ZN3n48L12gfxc_page_wcERKNS_6GfxcVmEyRyRbPy
        __ZN3n48L21hook_releaseVMContextEPvS0_
        __ZN3n48L20gfxsrc_desc_tiled_okEPvyjjPKjPj
        __Z14navi48_cg_openyy
        __Z15navi48_cg_closeiyybbb)
fi

resolve() { # a build dir, a .kext or a Mach-O: print the Mach-O path
  local p=$1
  if [ -d "$p/Navi48Bringup.kext" ]; then p=$p/Navi48Bringup.kext; fi
  if [ -d "$p" ] && [ -f "$p/Contents/MacOS/Navi48Bringup" ]; then p=$p/Contents/MacOS/Navi48Bringup; fi
  if [ ! -f "$p" ]; then echo "no binary at $1" >&2; return 1; fi
  echo "$p"
}

frame() { # symbol, binary: print 0x... or "absent"
  local v
  v=$(otool -tV -arch x86_64 -p "$1" "$2" 2>/dev/null | sed -n '1,60p' \
      | /usr/bin/grep -m1 -E 'subq[[:space:]]+\$0x[0-9a-f]+, %rsp' \
      | sed -E 's/.*subq[[:space:]]+\$(0x[0-9a-f]+), %rsp.*/\1/')
  echo "${v:-absent}"
}

B1=$(resolve "$1") || exit 2
echo "before: $B1"
dwarfdump --uuid "$B1" 2>/dev/null | sed -n '1p'
if [ $# -eq 2 ]; then
  B2=$(resolve "$2") || exit 2
  echo "after:  $B2"
  dwarfdump --uuid "$B2" 2>/dev/null | sed -n '1p'
fi

over=0
for s in "${SYMS[@]}"; do
  a=$(frame "$s" "$B1")
  if [ -z "${B2:-}" ]; then
    printf '%-56s %s\n' "$s" "$a"
    continue
  fi
  b=$(frame "$s" "$B2")
  if [ "$a" = absent ] || [ "$b" = absent ]; then
    printf '%-56s %-8s %-8s %s\n' "$s" "$a" "$b" "(not comparable)"
    continue
  fi
  d=$(( b - a ))
  flag=""
  if [ $d -gt 64 ]; then flag="  OVER +0x40"; over=1; fi
  if [ $d -ge 0 ]; then ds=$(printf '+0x%x' $d); else ds=$(printf -- '-0x%x' $(( -d ))); fi
  printf '%-56s %-8s %-8s %s%s\n' "$s" "$a" "$b" "$ds" "$flag"
done
exit $over
