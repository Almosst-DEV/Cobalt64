#!/bin/zsh
# stage-to-pc.sh — push the current bring-up kext, the three OpenCore configs and the ESP helper to
# the PC's ~/navi48-staging. RUN ON THE AIR; the PC must be booted into macOS.
#   tools/stage-to-pc.sh            stage everything
#   tools/stage-to-pc.sh status     just show what the PC is running right now
set -eu
P=${0:A:h:h}
HOST=${NAVI48_HOST:-navi48}
if ! ssh -o BatchMode=yes -o ConnectTimeout=6 "$HOST" true 2>/dev/null; then
  echo "PC ($HOST) not reachable — is it on and booted into macOS?"; exit 1; fi
if [[ "${1:-}" == status ]]; then
  ssh "$HOST" 'echo "up: $(uptime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; \
    echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4" || echo "  (neither Navi48Bringup nor RDNA4FB loaded)"; \
    ~/navi48-staging/esp-config.sh show 2>/dev/null || true'
  exit 0
fi
# Build the KEXT before copying it, for the same reason navi48test is built here:
# this script used to scp whatever build/ happened to contain. A failed `make`
# then shipped the PREVIOUS kext silently, so the machine ran old code while the
# source said otherwise -- indistinguishable from a driver bug, and the exact
# trap that cost a cycle on an earlier run with the kTtlHiTailReserve build error.
( cd "$P/src/navi48-bringup" && make ) || { echo "kext build FAILED - not staging a stale kext"; exit 1; }

# Refuse to ship a kext whose built version does not match the source Info.plist.
SRC_VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$P/src/navi48-bringup/Info.plist" 2>/dev/null)
BLD_VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$P/src/navi48-bringup/build/Navi48Bringup.kext/Contents/Info.plist" 2>/dev/null)
if [[ -n "$SRC_VER" && "$SRC_VER" != "$BLD_VER" ]]; then
    echo "version mismatch: source says $SRC_VER, built kext says $BLD_VER - not staging"
    exit 1
fi
echo "staging kext $BLD_VER"

[[ -d "$P/src/navi48-bringup/build/Navi48Bringup.kext" ]] || { echo "build the kext first (make in src/navi48-bringup)"; exit 1; }
[[ -f "$P/src/navi48-bringup/build/Navi48Bringup.kext/Contents/_CodeSignature/CodeResources" ]] || { echo "kext is NOT signed — refusing to stage (run make again)"; exit 1; }
ssh "$HOST" 'mkdir -p ~/navi48-staging/configs && rm -rf ~/navi48-staging/Navi48Bringup.kext'
scp -rq "$P/src/navi48-bringup/build/Navi48Bringup.kext" "$HOST:~/navi48-staging/"
scp -q "$P"/variants/configs/*.plist "$HOST:~/navi48-staging/configs/"
# Every PC-side helper is staged from the repo, so the PC never runs a copy that
# was scp'd by hand months ago. esp-kext.sh in particular used NOT to be staged:
# the ESP install path -- the one that decides which kext actually boots -- was
# whatever happened to be on the PC. probe-wrap.sh (the blit phase launcher), blit2.m (the source of the
# Metal client whose binary lives only on the PC) and trace-pageon.d (read-only dtrace) joined.
for h in esp-config.sh esp-kext.sh accel-run.sh read-result.sh probe-wrap.sh blit2.m tri.m trace-pageon.d iopparse.d wscomp.d display-topology.sh reboot-pc.sh; do
    [[ -f "$P/tools/pc/$h" ]] || { echo "missing tools/pc/$h - not staging a partial helper set"; exit 1; }
    scp -q "$P/tools/pc/$h" "$HOST:~/navi48-staging/" || { echo "scp of $h FAILED"; exit 1; }
done
# Build the test tool BEFORE copying it. Without this the script happily ships a
# stale binary: on an earlier run `accel rebaseib` was staged against a navi48test
# that predated the verb, silently ran `status` instead, and looked exactly like a
# driver refusing the command. Same shape as the bug -- two artefacts that must
# stay in sync, with nothing enforcing it.
if [[ -x "$P/tools/build-navi48test.sh" ]]; then
    "$P/tools/build-navi48test.sh" || { echo "navi48test build FAILED - not staging a stale binary"; exit 1; }
fi
[[ -x "$P/tools/pc/navi48test" ]] && scp -q "$P/tools/pc/navi48test" "$HOST:~/navi48-staging/" && ssh "$HOST" 'chmod +x ~/navi48-staging/navi48test' 
# The display-pairing probe (notes/DISPLAY-PAIRING.md Q5, userspace only, no GPU submission) is built
# here for x86_64 like navi48test, so the PC never runs a hand-copied binary. It creates an MTLDevice,
# which opens Apple's accelerator user client: run it only on a boot with no blit measurement pending,
# and never after a killed Metal client (joined with display-topology.sh).
if [[ -f "$P/tools/pc/display-pairing.m" ]]; then
    clang -arch x86_64 -mmacosx-version-min=26.0 -fobjc-arc -framework Foundation -framework Metal \
          -framework CoreGraphics "$P/tools/pc/display-pairing.m" -o "$P/tools/pc/display-pairing" \
        || { echo "display-pairing build FAILED - not staging a stale binary"; exit 1; }
    scp -q "$P/tools/pc/display-pairing" "$HOST:~/navi48-staging/" || { echo "scp of display-pairing FAILED"; exit 1; }
fi
# The milestone-2 triangle render client (notes/MILESTONE2-DESIGN.md Q4): x86_64, ad-hoc signed,
# not library-validated so it may swizzle. Renders into an offscreen texture and reads the pixels
# back with a TRI: verdict; same one-Metal-probe-per-boot rules as blit2. Cross-built here so the PC
# never runs a hand-copied binary (blit2 predates this and was hand-built once on the PC).
if [[ -f "$P/tools/pc/tri.m" ]]; then
    # 0.0.212: xlat12's walk and census are linked in for `tri --dump` (the same code the kext runs).
    clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -I"$P/src/xlat12" "$P/tools/pc/tri.m" \
          "$P/src/xlat12/xlat12.c" "$P/src/xlat12/xlat12_ib.c" \
          -framework Foundation -framework Metal -o "$P/tools/pc/tri" \
        || { echo "tri build FAILED - not staging a stale binary"; exit 1; }
    codesign --force --sign - "$P/tools/pc/tri" >/dev/null 2>&1
    lipo -info "$P/tools/pc/tri" | grep -q 'architecture: x86_64' \
        || { echo "tri is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/tri" "$HOST:~/navi48-staging/" || { echo "scp of tri FAILED"; exit 1; }
fi
# notes/M4-CONTEXT-LATCH.md: the forced-draw client for the context latch's proving run (hp3). x86_64, ad-hoc signed,
# cross-built here like tri so the PC never runs a hand-copied binary. Started by accel-run.sh `prefire-drawclient`.
if [[ -f "$P/tools/pc/drawclient.m" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra "$P/tools/pc/drawclient.m" \
          -framework Foundation -framework Metal -framework IOKit -framework CoreFoundation -o "$P/tools/pc/drawclient" \
        || { echo "drawclient build FAILED - not staging a stale binary"; exit 1; }
    codesign --force --sign - "$P/tools/pc/drawclient" >/dev/null 2>&1
    lipo -info "$P/tools/pc/drawclient" | grep -q 'architecture: x86_64' \
        || { echo "drawclient is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/drawclient" "$HOST:~/navi48-staging/" || { echo "scp of drawclient FAILED"; exit 1; }
fi
# 0.0.212 : the milestone-2 injected shader pair for `tri draw --inject`, staged as
# ~/navi48-staging/tri-inject/{manifest.json, m2_tri_vs.packed.bin, m2_tri_fs.packed.bin}. The packed
# records are the compiler's (re/graphics/g2/tri/m2tri, tools/gfx-pack.py); each is refused unless
# its sha256 equals the one re/graphics/g2/tri/manifest.json records.
TRIPAIR="$P/re/graphics/g2/tri/m2tri"
if [[ -f "$TRIPAIR/m2_tri_vs.packed.bin" && -f "$TRIPAIR/m2_tri_fs.packed.bin" ]]; then
    for st in vs fs; do
        want=$(python3 -c "import json,sys; m=json.load(open('$P/re/graphics/g2/tri/manifest.json')); p=m['pairs'][0]; print(p['vertex' if '$st'=='vs' else 'fragment']['packed_sha256'])")
        have=$(shasum -a 256 "$TRIPAIR/m2_tri_$st.packed.bin" | cut -d' ' -f1)
        [[ -n $want && $want == $have ]] || { echo "tri-inject: m2_tri_$st.packed.bin sha256 $have != manifest $want - not staging"; exit 1; }
    done
    TI=$(mktemp -d -t tri-inject)
    cp "$TRIPAIR/m2_tri_vs.packed.bin" "$TRIPAIR/m2_tri_fs.packed.bin" "$TI/"
    printf '{"vertex":"m2_tri_vs.packed.bin","fragment":"m2_tri_fs.packed.bin","pair":"re/graphics/g2/tri/m2tri"}\n' > "$TI/manifest.json"
    ssh "$HOST" 'mkdir -p ~/navi48-staging/tri-inject' && scp -q "$TI"/* "$HOST:~/navi48-staging/tri-inject/" \
        || { echo "scp of tri-inject FAILED"; exit 1; }
    rm -rf "$TI"
    echo "staged tri-inject (m2tri pair, sha256 checked)"
fi
# 0.0.224 : the suite's injected pairs for `tri suite --inject` (tristart-suite / tridump-suite), staged as
# ~/navi48-staging/tri-suite/{manifest.json, <key>-vs.packed.bin, <key>-fs.packed.bin}. manifest.json maps each pipeline key
# tri builds to its pair: {"suite": {"<key>": {"vertex": ..., "fragment": ..., "pair": <repo dir>}}}. Every packed record is
# refused unless its sha256 equals the one its own pair manifest (tools/gfx-pair.py) records.
SUITE_PAIRS=( "m2tri re/graphics/g2/tri/m2tri" "m2mvp re/graphics/g2/tri/m2tri_mvp" "m2col re/graphics/g2/tri/m2col"
              "m2half re/graphics/g2/tri/m2half" "simple re/graphics/g2/pairs/simple" )
# 0.0.265: TRI_SUITE_SKIP is a space-separated list of pipeline keys to leave
# UNINJECTED, so Apple's OWN compiled shaders reach the GPU for them and the kext's shader cache is
# what has to recognise and substitute them. Unset (the default) stages every pair and reproduces
# r99's step list exactly, which is the standing regression. `TRI_SUITE_SKIP=simple` is the vertex
# relocation measurement: the tex case then depends on the cache, base and blend stay injected.
if [[ -n ${TRI_SUITE_SKIP:-} ]]; then
    KEEP=()
    for kp in "${SUITE_PAIRS[@]}"; do
        skip=0
        for s in ${=TRI_SUITE_SKIP}; do [[ ${kp%% *} == $s ]] && skip=1; done
        (( skip )) && echo "tri-suite: SKIPPING pair ${kp%% *} - Apple's own shaders will be used" || KEEP+=("$kp")
    done
    SUITE_PAIRS=("${KEEP[@]}")
fi
TS=$(mktemp -d -t tri-suite)
SUITE_JSON=""
for kp in "${SUITE_PAIRS[@]}"; do
    key=${kp%% *}; dir=$P/${kp#* }
    [[ -f $dir/manifest.json ]] || { echo "tri-suite: $dir/manifest.json missing - not staging"; exit 1; }
    for st in vertex fragment; do
        f=$(python3 -c "import json; print(json.load(open('$dir/manifest.json'))['stages']['$st']['packed'])")
        want=$(python3 -c "import json; print(json.load(open('$dir/manifest.json'))['stages']['$st']['packed_sha256'])")
        have=$(shasum -a 256 "$P/$f" | cut -d' ' -f1)
        [[ -n $want && $want == $have ]] || { echo "tri-suite: $f sha256 $have != manifest $want - not staging"; exit 1; }
        cp "$P/$f" "$TS/$key-${st:0:1}s.packed.bin"
    done
    SUITE_JSON="$SUITE_JSON${SUITE_JSON:+, }\"$key\": {\"vertex\": \"$key-vs.packed.bin\", \"fragment\": \"$key-fs.packed.bin\", \"pair\": \"${kp#* }\"}"
done
printf '{"suite": {%s}}\n' "$SUITE_JSON" > "$TS/manifest.json"
python3 -c "import json,sys; json.load(open('$TS/manifest.json'))" || { echo "tri-suite: manifest.json is not valid JSON - not staging"; exit 1; }
ssh "$HOST" 'rm -rf ~/navi48-staging/tri-suite && mkdir -p ~/navi48-staging/tri-suite' && scp -q "$TS"/* "$HOST:~/navi48-staging/tri-suite/" \
    || { echo "scp of tri-suite FAILED"; exit 1; }
rm -rf "$TS"
echo "staged tri-suite (${#SUITE_PAIRS} pairs, sha256 checked)"
# 0.0.219 : the ABSOLUTE instrument vertex stage on the merged NGG layout (re/graphics/g2/instr/abs,
# tools/b1-tests/m2instr/build.py; its head is the rebuilt m2_tri_vs's) paired with the unchanged m2_tri_fs, staged as
# ~/navi48-staging/tri-inject-instr/ for `tristart-drawinstr[ring]`. It replaces 0.0.216's FULL instrument, which stores
# through s4:s5 and which the 0.0.219 renderdraw guard refuses. Each packed record is refused unless its sha256 equals
# the abs manifest's.
# 0.0.220 : N48_INSTR=<dir> stages another instrument directory with the same manifest schema
# ({vertex, fragment, vertex_packed_sha256, fragment_packed_sha256}), e.g. re/graphics/g2/instr/rec (the recording m2_tri_vs).
# 0.0.223 (notes sections 360 and 363): the default is the RECORDING m2_tri_vs (re/graphics/g2/instr/rec), the documented
# instrument baseline since 0.0.223. The absolute instrument's head is the pre-EXEC-prologue m2_tri_vs head, which the 0.0.223
# renderdraw guard refuses (nothing written, gate closed), so staging abs by default staged a draw that could never run. The
# plain milestone-2 pair is NOT an instrument and is staged separately above as tri-inject (tristart-drawinjring).
INSTR="${N48_INSTR:-$P/re/graphics/g2/instr/rec}"
IVS=$( [[ -f "$INSTR/manifest.json" ]] && python3 -c "import json; print(json.load(open('$INSTR/manifest.json'))['vertex'])" )
if [[ -f "$INSTR/manifest.json" && -n $IVS && -f "$INSTR/$IVS" && -f "$INSTR/m2_tri_fs.packed.bin" ]]; then
    for kv in "vertex $IVS" "fragment m2_tri_fs.packed.bin"; do
        role=${kv%% *}; fn=${kv#* }
        want=$(python3 -c "import json; m=json.load(open('$INSTR/manifest.json')); print(m['$role'+'_packed_sha256'] if m['$role']=='$fn' else '')")
        have=$(shasum -a 256 "$INSTR/$fn" | cut -d' ' -f1)
        [[ -n $want && $want == $have ]] || { echo "tri-inject-instr: $fn sha256 $have != manifest '$want' - not staging"; exit 1; }
    done
    TI=$(mktemp -d -t tri-inject-instr)
    cp "$INSTR/$IVS" "$INSTR/m2_tri_fs.packed.bin" "$TI/"
    printf '{"vertex":"%s","fragment":"m2_tri_fs.packed.bin","pair":"%s"}\n' "$IVS" "${INSTR#$P/}" > "$TI/manifest.json"
    ssh "$HOST" 'mkdir -p ~/navi48-staging/tri-inject-instr' && scp -q "$TI"/* "$HOST:~/navi48-staging/tri-inject-instr/" \
        || { echo "scp of tri-inject-instr FAILED"; exit 1; }
    rm -rf "$TI"
    echo "staged tri-inject-instr ($IVS from ${INSTR#$P/} + m2_tri_fs, sha256 checked)"
fi
# The registry-only display-pairing probe : IOKit reads, no Metal, no CoreGraphics,
# no user client, so it cannot wait on the GPU and is the variant allowed on an ARMED boot (still never
# with a user logged in). Built here for x86_64 so the PC never runs a hand-copied binary.
if [[ -f "$P/tools/pc/display-registry.c" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
          "$P/tools/pc/display-registry.c" -o "$P/tools/pc/display-registry" \
        || { echo "display-registry build FAILED - not staging a stale binary"; exit 1; }
    scp -q "$P/tools/pc/display-registry" "$HOST:~/navi48-staging/" || { echo "scp of display-registry FAILED"; exit 1; }
fi
# Milestone 4 route c' (notes/M4-FRAME-DELIVERY.md section 6, notes/M4-CPRIME-DESIGN.md section 3): the adoption
# probe. IOKit calls only: finds the accelerator and RDNA4FB, prints the framebuffer's pairing keys, creates and
# destroys one accelerator surface before and after IOServiceRequestProbe(accel, 0) then (accel, 1). No lock, shape,
# flush, submission or Metal call. Built here for x86_64 like display-registry so the PC never runs a hand-copied binary.
if [[ -f "$P/tools/pc/m4-adopt.c" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra -framework IOKit -framework CoreFoundation \
          "$P/tools/pc/m4-adopt.c" -o "$P/tools/pc/m4-adopt" \
        || { echo "m4-adopt build FAILED - not staging a stale binary"; exit 1; }
    lipo -info "$P/tools/pc/m4-adopt" | grep -q 'architecture: x86_64' \
        || { echo "m4-adopt is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/m4-adopt" "$HOST:~/navi48-staging/" || { echo "scp of m4-adopt FAILED"; exit 1; }
fi
# 0.0.272 : the route c' walls 2-3 test client. IOKit calls only: a surface with id 0x1000 and
# CoreDisplay's mode 0x8424, CoreDisplay's framebuffer shape, a write lock, the client stripes, unlock, flush, hold,
# close. It includes src/navi48-bringup/src/apple/scanout_copy.h so its pattern is the one the kext checks.
if [[ -f "$P/tools/pc/m4-flush.c" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra -I"$P/src/navi48-bringup/src/apple" \
          -framework IOKit -framework CoreFoundation "$P/tools/pc/m4-flush.c" -o "$P/tools/pc/m4-flush" \
        || { echo "m4-flush build FAILED - not staging a stale binary"; exit 1; }
    lipo -info "$P/tools/pc/m4-flush" | grep -q 'architecture: x86_64' \
        || { echo "m4-flush is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/m4-flush" "$HOST:~/navi48-staging/" || { echo "scp of m4-flush FAILED"; exit 1; }
fi
# display brief, Part A: fbbench times writes into RDNA4FB's kIOFBVRAMMemory mapping per cache mode
# (0x1 0x101 0x201 0x301 0x401) against RAM, writing back the bytes it read. Shared connection (type 1) only, never
# the server connection; no accelerator, no Metal. Run as root on an UNARMED boot at the login window.
if [[ -f "$P/tools/pc/fbbench.c" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
          "$P/tools/pc/fbbench.c" -o "$P/tools/pc/fbbench" \
        || { echo "fbbench build FAILED - not staging a stale binary"; exit 1; }
    lipo -info "$P/tools/pc/fbbench" | grep -q 'architecture: x86_64' \
        || { echo "fbbench is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/fbbench" "$HOST:~/navi48-staging/" || { echo "scp of fbbench FAILED"; exit 1; }
fi
# 0.0.286 : agdc-probe, route b's first test from userspace (IOKit reads, the AGDC open IOPresentment makes,
# IOPresentmentCreateForRegistryID). No Metal, no accelerator user client, never IOPresentmentInitialize or a transaction.
if [[ -f "$P/tools/pc/agdc-probe.c" ]]; then
    clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -framework IOKit -framework CoreFoundation \
          "$P/tools/pc/agdc-probe.c" -o "$P/tools/pc/agdc-probe" \
        || { echo "agdc-probe build FAILED - not staging a stale binary"; exit 1; }
    lipo -info "$P/tools/pc/agdc-probe" | grep -q 'architecture: x86_64' \
        || { echo "agdc-probe is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/tools/pc/agdc-probe" "$HOST:~/navi48-staging/" || { echo "scp of agdc-probe FAILED"; exit 1; }
fi
# The milestone-2 G2 capture tool (src/g2capture/README.md; an earlier analysis): x86_64,
# ad-hoc signed, userspace only. It builds one render pipeline state through Apple's Metal driver
# and swizzles the variant init to dump Apple's GFX10 packed binaries; it creates no command buffer
# and submits nothing, but it allocates through the accelerator's user client. Built here like
# display-pairing so the PC never runs a hand-copied binary; same boot rules as display-pairing
# (never with a blit measurement pending on that boot, never after a killed Metal client).
if [[ -f "$P/src/g2capture/g2capture.m" ]]; then
    zsh "$P/src/g2capture/build.sh" || { echo "g2capture build FAILED - not staging a stale binary"; exit 1; }
    lipo -info "$P/src/g2capture/g2capture" | grep -q 'architecture: x86_64' \
        || { echo "g2capture is not an x86_64 binary - not staging"; exit 1; }
    scp -q "$P/src/g2capture/g2capture" "$HOST:~/navi48-staging/" || { echo "scp of g2capture FAILED"; exit 1; }
fi
ssh "$HOST" 'chmod +x ~/navi48-staging/esp-config.sh ~/navi48-staging/esp-kext.sh ~/navi48-staging/accel-run.sh ~/navi48-staging/read-result.sh ~/navi48-staging/probe-wrap.sh ~/navi48-staging/display-topology.sh ~/navi48-staging/display-pairing ~/navi48-staging/g2capture ~/navi48-staging/reboot-pc.sh ~/navi48-staging/display-registry ~/navi48-staging/tri ~/navi48-staging/m4-adopt ~/navi48-staging/m4-flush ~/navi48-staging/fbbench ~/navi48-staging/agdc-probe 2>/dev/null; \
  echo "staged:"; ls -la ~/navi48-staging/Navi48Bringup.kext/Contents/MacOS ~/navi48-staging/configs; \
  echo; echo "kext build id: $(strings ~/navi48-staging/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup | grep -m1 "psp-stage: requested" >/dev/null && echo "has PSP stage" || echo "NO PSP STAGE?")"'
echo
# The old hint here named 3-install-bringup.sh and esp-config.sh. Both were wrong
# for a kext deploy: 3-install-bringup.sh copies to /Library/Extensions, which is a
# decoy on this machine (no KDK, so the Auxiliary collection cannot be rebuilt --
# OpenCore injects the ESP copy instead), and esp-config.sh swaps the whole
# config.plist and its boot-args, which a kext deploy must never do.
echo "next, ON THE PC:  sudo ~/navi48-staging/esp-kext.sh install   then   ~/navi48-staging/esp-kext.sh show   → reboot only if it names $BLD_VER"
echo "or, from the host Mac, the whole cycle in one:  tools/accel-cycle.sh --deploy <run-name>"
