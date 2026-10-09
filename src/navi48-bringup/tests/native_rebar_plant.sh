#!/bin/zsh
# native_rebar_plant.sh - planted breaks for tests/native_rebar_test.cpp (build 0.0.663 / 0.0.664, Resizable BAR support, an internal design note section 3 + `vramstat`).
# For each plant: copy the real sources into a scratch tree, apply the break (exact-once replacements; the script first proves each replaced text occurs exactly once),
# compile the real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count).
# A plant the suite lets through is a hole: the script exits non-zero and says which. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_rebar_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/nrb-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools/pc"
  rsync -a --exclude fw "$ROOT/$SR/" "$SCR/$SR/"          # the generated firmware arrays are not read by any pin
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_rebar_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/pc/navi48test.c" "$SCR/tools/pc/"
  mkdir -p "$SCR/variants/configs" "$SCR/variants/RESCUE/EFI/OC"
  cp "$ROOT"/variants/configs/stage17-native-1440-metal-disp-amfi-ms-apps.plist "$ROOT"/variants/configs/stage17-native-1440-metal-disp-amfi-ms-apps-rebar*.plist \
     "$ROOT"/variants/configs/stage17-native-1440-metal-disp-amfi-m6flip3.plist "$ROOT"/variants/configs/stage17-native-1440-metal-disp-amfi-m6flip3-rebar*.plist "$SCR/variants/configs/"
  cp "$ROOT/variants/RESCUE/EFI/OC/config.plist" "$SCR/variants/RESCUE/EFI/OC/"
}
build_run() {   # prints the verdict line; returns 0 if the suite PASSED, 1 if it failed, 2 if it did not compile
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -I $SR -I $SR/amd $K/tests/native_rebar_test.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then echo "CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return 2; fi
  out=$(cd "$SCR" && perl -e 'alarm 120; exec @ARGV' ./t . 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "the suite passed: $(echo "$out" | tail -1)"; return 0; fi
  if [ $trc -ge 128 ]; then echo "CAUGHT by a crash or hang (signal $((trc-128)))"; return 1; fi
  echo "CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"; return 1
}
plant() {   # plant <id> <file relative to repo root> <description> <old1> <new1> [<old2> <new2> ...]
  local id="$1" file="$2" desc="$3"; shift 3
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  fresh
  local pairs=""
  while [ $# -ge 2 ]; do pairs="${pairs}${1}"$'\x1e'"${2}"$'\x1d'; shift 2; done
  PAIRS="$pairs" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read()
for pair in os.environ['PAIRS'].split('\x1d'):
    if not pair: continue
    o,n=pair.split('\x1e')
    if s.count(o)!=1:
        print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
    s=s.replace(o,n)
open(p,'w').write(s)
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED ($msg) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: $msg"; fi
}
control() {
  fresh; total=$((total+1))
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "CONTROL native_rebar_test (no break): $msg"; else echo "CONTROL native_rebar_test (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
}
PURE=$SR/amd/rebar_pure.h
B=$SR/Navi48Bringup.cpp
GMC=$SR/amd/gmc_v12_0.cpp
S1C=$SR/amd/native_s1c.cpp
NB=$SR/amd/nbif_v6_3_1.cpp
DP=$SR/amd/native_disp_pure.h
CLI=tools/pc/navi48test.c
control
# ---- the contract's seven planted breaks ----
plant 1 $PURE "the clamp is removed (vramLimit = the mapped length)" 'uint64_t lim = mapLen < cap ? mapLen : cap;' 'uint64_t lim = mapLen;'
plant 2 $B "vramLimit = bar0Size (the old line)" 'dev.vramLimit = bar0Limit;' 'dev.vramLimit = bar0Size;'
plant 3 $PURE "the address cross-check is dropped" '    if (in.cfgAddr != in.ioAddr) { r.reason = kAddrMismatch; return r; }
' ''
plant 4 $PURE "the ReBAR mismatch check is dropped" 'if (in.rebarPresent) { if (in.rebarBytes != in.ioLen) { r.reason = kRebarMismatch; return r; } }' 'if (in.rebarPresent) { }'
plant 5 $PURE "only the low 32 bits of the BAR address are read" 'return (((lo & 0x6u) == 0x4u) ? ((uint64_t)hi << 32) : 0ull) | (uint64_t)(lo & ~0xFu);' 'return (uint64_t)(lo & ~0xFu);'
plant 6 $B "the top aperture check is skipped" 'for (int i = 0; i < 3; i++) all = apertureCheck(pt[i]) && all;' 'for (int i = 0; i < 2; i++) all = apertureCheck(pt[i]) && all;'
plant 7 $B "the check zeroes the bytes instead of restoring them" 'if (!vramWrite(off, before, sizeof(before))) ok = false;' 'if (!vramMemset(off, 0, sizeof(before))) ok = false;'
# ---- item 1 / the planning rules ----
plant 8 $B "kMaxBar0Map is 256 MiB again" 'constexpr uint64_t kMaxBar0Map    = n48rebar::kMaxBar0Map;' 'constexpr uint64_t kMaxBar0Map    = 256ULL << 20;'
plant 9 $PURE "the production map cap is 16 GiB" 'constexpr uint64_t kMaxBar0Map  = kGiB;' 'constexpr uint64_t kMaxBar0Map  = 16 * kGiB;'
plant 10 $PURE "a BAR above 256 MiB with no readable capability is planned" 'else if (in.ioLen > kLegacyBar) { r.reason = kNoRebarCap; return r; }' ''
plant 11 $PURE "the middle check point is the bottom one" 'out[0] = vramBase; out[1] = mid; out[2] = top;' 'out[0] = vramBase; out[1] = vramBase; out[2] = top;'
plant 12 $PURE "the top check point is not at the top" 'const uint64_t top = vramLimit - kApertureTail;' 'const uint64_t top = vramBase + 64ull * 1024;'
plant 13 $PURE "vramLimit is not rounded down to 64 KiB" 'lim &= ~(kLimitAlign - 1ull);' ''
plant 14 $PURE "the 64 MiB slack below the reserve is 0" 'constexpr uint64_t kLimitSlack  = 64ull << 20;' 'constexpr uint64_t kLimitSlack  = 0ull;'
plant 15 $PURE "the console may run past vramLimit" 'in.consoleOff > lim || in.consoleLen > lim - in.consoleOff' 'false'
plant 16 $PURE "a non power of two length is planned" 'if (!is_pow2(in.ioLen)) { r.reason = kLenNotPow2; return r; }' ''
plant 17 $PURE "a misaligned address is planned" 'if ((in.ioAddr & (in.ioLen - 1ull)) != 0ull) { r.reason = kAddrMisaligned; return r; }' ''
plant 18 $PURE "length 0 is not named (falls to the power-of-two refusal)" 'if (in.ioLen == 0ull) { r.reason = kLenZero; return r; }' ''
plant 19 $B "the plan is ignored (an unplanned BAR is mapped)" 'if (!pl.ok) { N48LOG("vram: BAR0 refused' 'if (false) { N48LOG("vram: BAR0 refused'
plant 20 $B "a mapped length that differs from the plan is accepted" 'if (bar0Size != winLen) {' 'if (false) {'
plant 21 $B "the plan's hi reserve is 0" 'pin.hiReserve = amdgpu::kVramHiTotalReserve' 'pin.hiReserve = 0'
plant 22 $B "the console is not added to the final plan" 'if (bootFbOff != ~0ULL) { pin.consoleOff = bootFbOff; pin.consoleLen = bootFbLen; }' ''
plant 23 $B "the plan's map cap is 16 GiB" 'pin.mapCap = kMaxBar0Map;' 'pin.mapCap = 16ULL << 30;'
plant 24 $B "finishVramPlan is skipped (bar0Limit stays 0)" 'if (!finishVramPlan())         {' 'if (false)         {'
plant 25 $B "the three-point check is removed from the stage" 'if (!apertureCheckWindow())    {' 'if (false)    {'
plant 26 $B "the restore is not read back" 'if (back != before[i]) {' 'if (false) {'
# ---- the allocator sizes ----
plant 27 $GMC "gmc_mc_init ignores vramLimit" 'n48rebar::visible_size(dev.vramLimit, dev.bar0Size, gmc.real_vram_size, kFallbackVisibleVRAM);' 'n48rebar::visible_size(0, dev.bar0Size, gmc.real_vram_size, kFallbackVisibleVRAM);'
plant 28 $PURE "visible_size ignores vramLimit" 'if (vramLimit > 0ull && (realVram == 0ull || vramLimit <= realVram)) return vramLimit;' ''
plant 29 $PURE "the hi pool swallows the reserve" 'p.hiSize = vramBytes - hiReserve - vramLimit;' 'p.hiSize = vramBytes - vramLimit;'
plant 30 $GMC "the hi pool test is the old arithmetic" 'if (pools.hi) {' 'if (dev.vramSizeBytes > dev.vramLimit + kVramHiTotalReserve) {'
# ---- the latch ----
plant 31 $B "the conflict is never reported" 'amdgpu::n1c_latch_bar0_conflict(u.conflict);' 'amdgpu::n1c_latch_bar0_conflict(false);'
plant 32 $B "the latch ignores config space (no union)" 'n48rebar::bar0_latch(io0Base, io0Len, cfgBase, cfgLen)' 'n48rebar::bar0_latch(io0Base, io0Len, 0, 0)'
plant 33 $PURE "a BAR0 IOPCIFamily dropped is not a conflict" '        o.conflict = true;
        if (cfgLen' '        o.conflict = false;
        if (cfgLen'
plant 34 $PURE "a size disagreement is not a conflict" 'if (ioBase != cfgBase || (cfgLen != 0ull && cfgLen != ioLen)) o.conflict = true;' 'if (ioBase != cfgBase) o.conflict = true;'
plant 35 $S1C "n1c_open ignores the BAR0 disagreement" 'if (__atomic_load_n(&gBar0Conflict, __ATOMIC_ACQUIRE) == 2u) {' 'if (false) {'
# ---- the census ----
plant 36 $B "the census WRITES config space" 'return n48rebar::census_read([pci](uint32_t o) -> uint32_t { return pci->configRead32(o); });' 'return n48rebar::census_read([pci](uint32_t o) -> uint32_t { pci->configWrite32(0x100, 0); return pci->configRead32(o); });'
plant 37 $PURE "the walk follows a self pointer" 'if (nx < kExtCapStart || (nx & 3u) != 0u || nx == o) return false;' 'if (nx < kExtCapStart || (nx & 3u) != 0u) return false;'
plant 38 $PURE "an entry's BAR index is its position" 'c.e[i].bar = c.e[i].ctrlReg & 7u;' 'c.e[i].bar = i;'
plant 39 $PURE "the size code is read as 2^(code+10)" '(1ull << (20u + entry_code(ctrl)))' '(1ull << (10u + entry_code(ctrl)))'
plant 40 $PURE "unreadable extended space is not flagged" 'c.extReadable = x100 != 0xFFFFFFFFu && x100 != rd(0u);' 'c.extReadable = true;'
plant 41 $B "the census is not published as properties" 'setProperty("ReBAR,Found", cen.found);' ''
# ---- the doorbell log ----
plant 42 $NB "the self-ring high half is not logged" 'NBIF_LOG("enable_doorbell_selfring_aperture: writing BASE_HIGH %#010x%s"' 'NBIF_LOG("enable_doorbell_selfring_aperture: writing HIGH %#010x%s"'
# ---- vramstat ----
plant 43 $DP "vramstat is not admitted" 'action == kActM6XStat || action == kActVramStat)); }' 'action == kActM6XStat)); }'
plant 44 $DP "vramstat takes an argument" '(action == kActVramStat && arg == 0ull) ||' '(action == kActVramStat) ||'
plant 45 $CLI "the CLI has no vramstat verb" 'else if (what && !strcmp(what, "vramstat")) in = 108;' ''
plant 46 $CLI "the known list forgets vramstat" 'm6stat m6xstat vramstat\n");' 'm6stat m6xstat\n");'
plant 47 $B "vramstat writes VRAM" 'v[10] = g.visible_vram_size;' 'v[10] = g.visible_vram_size; vramMemset(0, 0, 4);'
plant 48 $B "vramstat reports used bytes as free" 'v[2] = g.vram_alloc.bytes_free();' 'v[2] = g.vram_alloc.bytes_used();'
plant 49 $CLI "the CLI usage string forgets vramstat" '|m6xstat [0|1|2]|vramstat|capstream' '|m6xstat [0|1|2]|capstream'
plant 50 $SR/Navi48UserClient.cpp "the user client's table comment forgets the verb" '108 = vramstat' '108 = (removed)'
# ---- version ----
plant 51 $K/Info.plist "the version is not bumped" '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.662</string>'
plant 52 $SR/amd/native_s1c.h "Hello reports the old build" 'constexpr uint32_t kN1cKextBuild = 664;' 'constexpr uint32_t kN1cKextBuild = 662;'
# ---- item 9: the variants ----
V=variants/configs
plant 53 $V/stage17-native-1440-metal-disp-amfi-ms-apps-rebar0.plist "-rebar0 keeps npci=0x2000" '-v keepsyms=1 alcid=7' '-v keepsyms=1 npci=0x2000 alcid=7'
plant 54 $V/stage17-native-1440-metal-disp-amfi-m6flip3-rebar1g.plist "-rebar1g keeps ResizeAppleGpuBars 0" '<key>ResizeAppleGpuBars</key>
			<integer>10</integer>' '<key>ResizeAppleGpuBars</key>
			<integer>0</integer>'
plant 55 $V/stage17-native-1440-metal-disp-amfi-ms-apps-rebar1g.plist "-rebar1g sets ResizeGpuBars (the UEFI-wide one) as well" '<key>ResizeGpuBars</key>
			<integer>-1</integer>' '<key>ResizeGpuBars</key>
			<integer>10</integer>'
plant 56 $V/stage17-native-1440-metal-disp-amfi-m6flip3-rebar0.plist "-rebar0 differs from the live variant elsewhere" 'navi48-m6flip1=1' 'navi48-m6flip1=0'
plant 57 variants/RESCUE/EFI/OC/config.plist "RESCUE loses npci (it must stay untouched)" ' npci=0x2000' ''
# ---- 0.0.664 (F2 F3 F4 F5 F7 + the vramstat census) ----
plant 58 $PURE "F3: a length above the 1 GiB cap is planned (capped)" '    if (in.ioLen > in.mapCap) { r.reason = kLenTooBig; return r; }' ''
plant 59 $NB "F2: the self-ring aperture is enabled even when a base read-back mismatched" 'const bool selfringOn = enable && kr == kIOReturnSuccess;' 'const bool selfringOn = enable;'
plant 60 $B "F2: mapDoorbells does not compare IOPCIFamily's BAR2 address with config space" 'if (!n48rebar::bar_addr_agree((uint64_t)bar->getPhysicalAddress(), n48rebar::cfg_bar_addr(clo, chi))) {' 'if (false) {'
plant 61 $PURE "F2: bar_addr_agree ignores the config-space address" 'return ioAddr != 0ull && ioAddr == cfgAddr;' 'return ioAddr != 0ull;'
plant 62 $B "F4: a plan refusal keeps BAR0 mapped" 'if (!finishVramPlan())         { dropBar0();' 'if (!finishVramPlan())         {'
plant 63 $B "F4: an aperture-check refusal keeps BAR0 mapped" 'if (!apertureCheckWindow())    { dropBar0();' 'if (!apertureCheckWindow())    {'
plant 64 $B "F4: dropBar0 leaves bar0Limit set" 'bar0Phys = 0; bar0Limit = 0; vramBase = 0;' 'bar0Phys = 0; vramBase = 0;'
plant 65 $B "F5: the layout is not judged" 'if (!n48rebar::layout_ok(' 'if (false && !n48rebar::layout_ok('
plant 66 $PURE "F5: vramBase may sit inside the console cursor / below the console" '    if (vramBase < cEnd) return false;
' ''
plant 67 $PURE "F5: the check points are not tested against the console" '    for (int i = 0; i < 3; i++) if (pt[i] < cEnd && pt[i] + 16ull > consoleOff) return false;
' ''
plant 68 $PURE "F5: the visible pool may overlap the console" '    if (vramLimit > visBase && visBase < cEnd && vramLimit > consoleOff) return false;
' ''
plant 69 $PURE "F5: the 64 KiB cursor is not counted" 'const uint64_t cEnd = consoleOff + consoleLen + kCursorBytes;' 'const uint64_t cEnd = consoleOff + consoleLen;'
plant 70 $PURE "F7: aliased extended space is readable" ' && x100 != rd(0u);' ';'
plant 71 $PURE "F7: an unreadable extended space is still walked" '    if (!c.extReadable) return c;
' ''
plant 72 $B "vramstat does not report the supported-sizes mask" '((uint64_t)rebarMask << 32)' '0ull'
plant 73 $CLI "the CLI does not name the ResizeAppleGpuBars code" 'ResizeAppleGpuBars code %u' 'code %u'
plant 74 $PURE "entry_min_bytes returns the largest size" 'for (uint32_t k = 0; k < 32u; k++) if ((supportedMask >> k) & 1u) return 1ull << (20u + k);' 'for (uint32_t k = 31; k < 32u; k--) if ((supportedMask >> k) & 1u) return 1ull << (20u + k);'
plant 75 $GMC "F5: the layout offset is not tied to the allocator's" 'static_assert(kGMCVRAMAllocOffset == n48rebar::kVisAllocOffset' 'static_assert(true || kGMCVRAMAllocOffset == n48rebar::kVisAllocOffset'
echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
