#!/bin/zsh
# accel-run.sh — ONE Phase 4 accelerator measurement, captured whole, reduced to a
# ~30-line summary.  RUN ON THE PC (staged to ~/navi48-staging/accel-run.sh).
#
#   accel-run.sh <run-name> [step ...]
#   accel-run.sh <run-name> synctables setvspace pm4powerup kiqenable gfxmap pm4powerup sleep5 blit
#   accel-run.sh --force <run-name>            second run in the same boot (see below)
#   accel-run.sh --dry-run <run-name> [step…]  print every command, run nothing
#   ACCEL_RUN_SUMMARY_ONLY=<dir> accel-run.sh  re-summarise an existing run dir and exit
#
# Steps given on the command line REPLACE the default list; readiness and `fire`
# always run first because the accelerator does not attach until the TTL hook is
# installed. Default list = the the project notes cycle (since an earlier run; until then
# the default was the obsolete 0.0.177 logreset+kiqstamp sequence). The order is
# load-bearing:
#
#   synctables  Apple's GART PTEs into ours   gfxmap's PTE check refuses without it
#   setvspace   mem+0x98 -> 1  (gate 2)       MUST precede pm4powerup
#   pm4powerup  startKIQ stores the doorbell at ring+0xc0
#   kiqenable   ring+0xa8 -> 1                REFUSES on a NULL doorbell, hence after pm4powerup
#   gfxmap      software KIQ emulation        never with kiqstamp in the same boot
#   pm4powerup  GFX10PM4Engine::powerUp returns TRUE (~40 ms)
#   sleep5
#
# Extra step tokens (all [A-Za-z0-9._-], so accel-cycle.sh passes them through):
#   blit | blit-<va>   the whole the project notes blit phase, expanded in place to
#                      counters blit2start xlatregs neuterpoll sdmamap sleep2 vmstate
#                      faultclear sleep3 vmib-<va> sdmastate reg-0x21c0 reg-0x3054
#                      reg-0x2830 parkreads vmstate pagecopy counters blit2wait pagecopy
#                                                                   (<va> default 0x400017b00)
#   copyarm            accel pagecopy 1: arm the 0.0.201 residency copy for this boot
#                      (put it BEFORE blit, after the cycle; without it the skip stays)
#   shadercache        accel shadercache 1 (0.0.239, an earlier analysis): arm the hash-keyed
#                      substitution of gfx1201 code for Apple's GFX10 shaders at the residency
#                      copy - kernsub's seam with its one hard-coded kernel generalised to a keyed
#                      lookup in the embedded cache blob. PLACE IT BEFORE blit2start, not after:
#                      unlike kernsub it retains no Apple pointer and can only act while a copy
#                      runs, so arming it after the client has paged substitutes nothing (it says
#                      so in the log). shadercacheread reads the counters, shadercacheoff disarms.
#                      shadercacheadj (0.0.265, an earlier analysis) arms as mode 3: entries carrying
#                      register adjustments stop being refused. It applies NO register itself - the
#                      render path's per-identity profile does - so it is valid only for stages our
#                      draw policy translates. Needed for the vertex-relocation measurement, because
#                      SimpleTextureFragment carries adjustments and mode 1 refuses it
#   kernsub            accel kernsub 1 (0.0.204, an earlier analysis): substitute a gfx1201 blit
#                      kernel for Apple's Navi21 one at the residency copy's shader region
#                      (+0xfb00), guarded by an exact match on Apple's bytes. Put it AFTER
#                      blit2start (the copy must have run) and BEFORE the CP dispatches
#   kerndiag           accel kernsub 2 (0.0.206, an earlier analysis): substitute the DIAGNOSTIC
#                      kernel blit_diag_gfx1201 (INSTRUMENT: records {ttmp9,s8,v0,0x600d600d}
#                      and {ttmp7,ttmp8,v0,0x600d0b0b} into blit2's destination page, copies
#                      nothing). Same placement as kernsub; one kernel mode per boot
#   kerndiagmin        accel kernsub 3: the minimal diagnostic (record A only), the fallback
#   vmpage-<va>        accel vmpage <va> (0.0.206): the whole 4 KiB page behind <va> in Apple's
#                      VMID-2 table, nonzero lines logged, diag records decoded, decoder self-test
#   diagreads          the section 328 read set, while blit2 lives: vmpage-0x400004000 counters
#                      reg-0x219e reg-0x2005 reg-0x2006 reg-0x200e reg-0x200f reg-0x21a0
#   blit2start[-<s>]   probe-wrap.sh blit2 1, wait for blit2.state `committed` (max 20 s), hold
#                      <s> seconds (default 3), go. The takeover MUST land before Apple's hang
#                      timeout: a 20 s hold let Apple time out and replay first, and the CP never
#                      fetched the blit IB (upload-03, r28, r29; an earlier analysis). blit2
#                      STAYS ALIVE (rule 20: every read happens while it lives)
#   blit2wait          wait (max 330 s) for blit2 to print its DATA line and exit, then
#                      copy /tmp/trace-probe.txt to blit2-output.txt. Never kills it.
#   reg-<dword>        navi48test reg <dword>        vmib-<va>   accel vmib <va>
#   ringib-<chan>      accel ringib <chan>           sleepN      sleep N seconds
#   parkreads          read-only park decode , also inside `blit`:
#                      reg 0x21a0 CP_STAT, 0x219e CP_STALLED_STAT2, 0x21a1/0x21a2 ME/PFP
#                      header dumps, 0xc0ce/0xc0c0 IB1 BUFSZ/CMD_BUFSZ, 0x2005 0x2006 0x200e
#                      0x200f GRBM_STATUS_SE0-3, 0x2e00/0x2e01 dispatch initiator/DIM_X,
#                      0x2316 SQ host traps
#   renderib / renderblank / renderxlat[-ngg]   accel renderxlat 0 / 3 / 1 [0x101] (0.0.209, notes
#                      section 335): AFTER xlatregs, BEFORE sdmamap. Content search for Apple's render IB
#                      among the pending page-table leaves: census + dump / BLANK instrument / translate
#   renderdraw / renderdrawcheck   accel renderxlat 4 / 5 (0.0.213, an earlier analysis): the milestone-2 DRAW policy
#                      (legacy VS re-homed to gfx12 NGG, our records' resource words, radeonsi's NGG state, shader
#                      identity guarded); 4 writes in place, 5 is the same translation and checks, writing nothing
#   rendergate         skip the later sdmamap unless the renderxlat rewrite landed (status 1/2)
#   tristart-clear|tri|draw|drawinj[-<s>], triwait[-<s>], triclear, tridraw   the tri render client and phases
#                      triwait-<s> bounds the wait at <s> s and sets LIVE_ABORT if tri has not exited by then
#                      (an earlier analysis; uc1's warm-up hung for 300 s and voided that boot)
#                      (0.0.212: `draw` = DontCare load + CPU prefill + the m2tri pair compiled by Apple;
#                      `drawinj` = the same with our packed gfx1201 pair from tri-inject/, refused without it;
#                      0.0.216 `drawinstr` = the same with the instrument VS from tri-inject-instr/, section 347 (0.0.219:
#                      the ABSOLUTE instrument on the merged layout, section 354; the full instrument is retired);
#                      0.0.217 `drawinjring` / `drawinstrring` = the same plus `tri --ring`, the gfx12 NGG ring buffer)
#   renderdrawring / renderdrawringcheck   0.0.217 : accel renderxlat <ring base|0x200|4 or 5>, the
#                      draw policy plus the GE rings at tri's printed ring base and SPI_SHADER_PGM_RSRC3_GS 0xfffffdfd;
#                      ABORTs (nothing sent, gate stays closed) when tri printed no 64 KiB-aligned ring base
#   renderdrawringb / renderdrawringbcheck   0.0.218 : the same plus flag 0x400 (the gfx12 preamble
#                      delta, review report 9 experiment B): scalar base|0x600|mode. 0.0.219 : 0.0.218's
#                      flag 0x800 (VS USER_SGPR 6) is retired and the kext refuses it
#   renderdrawringc / renderdrawringccheck   0.0.220 : renderdrawringb plus flag 0x1000, the gfx12 raster/CB
#                      delta (CB_COLOR0_VIEW2, FDCC_CONTROL, VRS override/info, HISZ override, CB_MEM0_INFO): scalar base|0x1600|mode
#   ringreads          reg 0x2c87 RSRC3_GS, 0x2c88 RSRC4_GS, 0xc268-0xc26b GE_POS/PRIM_RING_BASE/SIZE, 0xc444-0xc447
#                      SPI_GS_THROTTLE_CNTL1/2 + SPI_ATTRIBUTE_RING_BASE/SIZE (read-only)
#   ringpages          accel vmpage of the first page of the attribute, position and primitive rings (read-only)
#   tridump-clear|draw|drawinj   0.0.212 (review report 7 rec. 1): tri builds the encoder,
#                      endEncoding, NEVER commits, and dumps every PM4 stream in its own memory into
#                      <run>/tridump-<v>/. No GPU work. Waits (max 90 s) for tri to exit, never kills it
#   prefire-<step>     0.0.267 : any step token prefixed `prefire-` runs AFTER the
#                      console-user and one-measurement gates and BEFORE `fire`, in the order given. If a
#                      prefire step fails, the run ABORTS without firing (nothing armed). The console user
#                      is checked a SECOND time immediately before `fire`.
#   bootchain-0x<m>    notes/M4-CONTEXT-LATCH.md: accel bootchain <m> - sets the in-kext chain's mode for THIS boot; the kext
#                      refuses it after `fire`, so use it as `prefire-bootchain-0x3d` (0x1d + bit 5 = THE CONTEXT LATCH:
#                      no client VM context until the SDMA drain, render drain, source hook and ring neuter are all live)
#   drawclient         notes/M4-CONTEXT-LATCH.md: start the FORCED-DRAW client ($T/drawclient 60 50) in the background, as
#                      root, stdout in <run>/drawclient.out; use it as `prefire-drawclient`: it polls IOKit (no Metal) until
#                      the accelerator appears, then asks for the device at once and maps, blits and draws every 50 ms for
#                      60 s. Fails (aborting a prefire) unless it printed its start line. drawclientread prints its record
#   pairing | pairingread | pairingoff   accel pairing 1 / (read) / 2 (0.0.267, an earlier analysis): the
#                      display-pairing stamp is OPT-IN; `prefire-pairing` enables it for this boot
#   wslogstart         P2 : start two live unified-log streams at --level info, via
#                      /usr/bin/log (zsh has a `log` BUILTIN): wslog.txt = WindowServer + subsystem
#                      com.apple.CoreDisplay; kernlog.txt = kernel senders AppleGPUWrangler,
#                      IOAcceleratorFamily2, IOGraphicsFamily. Both also take our logger marker, and
#                      the step FAILS unless the marker is seen in both files (the positive control)
#   wslogstop          marker, stop both streams, count and extract the GPU-arrival lines into
#                      wslog-gpu.txt; prints `P2 ` lines the summary quotes
#   logmark[-<tag>]    one `N48-P2-MARK` line into the unified log (positive control for the streams)
#   wsstate[-<tag>]    console user, WindowServer/loginwindow pid+start (ps saved first), U-state
#                      processes, IOAccelerator nub count, every IOUserClient's creator (saved first)
#   displayregistry    ~/navi48-staging/display-registry (IORegistry reads only, an earlier analysis)
#   wslogstart-a       TEST A (0.0.268, an earlier analysis): as wslogstart, with the broader predicates -
#                      WindowServer, loginwindow, subsystems com.apple.CoreDisplay / com.apple.Metal /
#                      com.apple.SkyLight*, and kernel senders AMDRadeonX6000, IOAcceleratorFamily2,
#                      AppleGPUWrangler, IOGraphicsFamily
#   wskill             TEST A: console user must be root (checked in the step), then
#                      `launchctl kickstart -k system/com.apple.WindowServer` under a 20 s watchdog; records
#                      the kill epoch in .kill-epoch and the old/new WindowServer pid. FAILS without killing
#                      unless the console user is root. On an ARMED recipe (one carrying `armgate` or `commitarm`)
#                      it ALSO refuses unless the COMMIT arm's own read-back has confirmed, and it prints one
#                      `armwskill:` line giving the arm's wall clock, its own, and the gap in seconds
#   armgate[-<s>]      an earlier analysis: THE ARM GATE, and the ONE token an armed recipe must carry. Put it
#                      IMMEDIATELY BEFORE `wskill`, in place of the `sleep300` arm window (`armgate-300`). It
#                      declares "this run arms at COMMIT" and then WAITS (default 240 s, `<s>` overrides) for
#                      `gfx-commit: ARM LEVEL IS 2 (COMMIT); one-shot ARMED` to appear in driverlog-stream.txt -
#                      so the out-of-band arm batch DELAYS the kill instead of the kill outrunning the arm. It
#                      sends NOTHING to the kext: the stream is already running, so the gate costs no verb and
#                      adds no log line. NEEDS `logstream-start` earlier in the list (the only place that line is
#                      readable - `logstream` owns the ring, so `accel gfxneuter 4` prints only packed scalars,
#                      an earlier analysis). Returns the instant the arm is seen. On timeout, or with no stream, it
#                      sets LIVE_ABORT, which SKIPS `wskill` and every later client step: an unarmed window is a
#                      void measurement, not a result
#   commitarm          the arm IN the step list instead of out of band: read-only `accel gfxneuter 4` first (the
#                      pre-arm checklist must print CLEAN - the kext refuses a dirty world itself), console root,
#                      no abort of any kind, then `accel gfxneuter 260` (= `4 | 1 << 8`, the one-shot) and the
#                      read-back. Confirms the arm for `wskill` exactly as `armgate` does. Same placement: after
#                      the switch block and the fence switch, BEFORE `wskill`
#   sakill             TEST S (notes sections 486-487): console user must be root, then `kill -TERM` of the running
#                      SecurityAgent ONLY (WindowServer and loginwindow untouched); records the kill epoch and waits
#                      30 s for a new SecurityAgent pid. On an armed boot whose WindowServer started BEFORE fire it
#                      puts an ACCELERATED SecurityAgent under the SOFTWARE compositor. FAILS without killing
#                      unless the console user is root; never escalates to loginwindow or WindowServer.
#                      sacomp1 (section 487): SecurityAgent DEFERS SIGTERM (4 xpc transactions) and is not relaunched
#   sakill-kill        the same with `kill -KILL` (sanctioned for sacomp2, an earlier analysis)
#   agdcreg-<tag>      READ-ONLY IORegistry: AppleGraphicsDeviceControl, AMDRadeonX6000_AmdAgdcServices and IOAccelDisplayPipe
#                      nodes (-l -d 1 each, 20 s alarm; never -l on the accelerator) and the plain IOService tree filtered for
#                      AGDC / display-pipe / presentment names -> agdc-<tag>-*.txt
#   sleepuntilk-<s>    sleep until <s> seconds after the recorded kill epoch (no-op if already past)
#   kextlog-<tag>      navi48test log -> driverlog-<tag>.txt (60 s timeout), with counts of census, submit
#                      attribution, VM-context, SpecialAMDKey and writeTail lines
#   Every TEST A snapshot command runs under a perl alarm (tmo), so a blocked probe cannot hang the run.
#   drain              LIVENESS (0.0.269, an earlier analysis): `accel drain`, READ-ONLY - the state and counters of the
#                      boot-chain drain (navi48-boot-chain bit 2), which translates every Apple SDMA submission
#                      before its doorbell. Put it AFTER a soak: it is a read, but a liveness proof sends nothing
#                      to the kext until the soak is over
#   soak-<s>           LIVENESS: wait <s> seconds sending NOTHING to the kext. Every 30 s it records the console user,
#                      the U-state count and the kernel-log hang lines (needs wslogstart-a). If anyone other than root
#                      appears at the console it ABORTS the soak and every later client step is SKIPPED (the brief's
#                      console gate: finish safely, reboot clean, stop)
#   livecheck-<tag>    LIVENESS: the same one-shot read as a soak tick (console user, U state, kernel-log hang lines)
#   m4adopt            M4 route c' wall 1 (notes/M4-FRAME-DELIVERY.md section 6): console user must be root (checked
#                      in the step, LIVE_ABORT otherwise), then ~/navi48-staging/m4-adopt under a 120 s alarm -> m4-adopt.txt.
#                      Surface create/destroy before and after IOServiceRequestProbe(accel, 0) then (accel, 1). Adoption
#                      stamps Apple's pairing keys: this boot is spent afterwards (rule 86 - never restart WindowServer on it)
#   flushhook-<n>      0.0.272 : accel flushhook <n> - 1 LOG-ONLY per-surface externalMethod hook on every
#                      AMDAccelSurface minted afterwards, 3 log + copy of a flushed surface into the scanout (refused unless
#                      scanout-1 passed), 2 pass-through; flushhookread reads the counters
#   scanout-<n>        0.0.272: accel scanout <n> - 0 geometry read, 1 the POSITIVE CONTROL (kext bars -> pre-flight copy ->
#                      SDMA0 QUEUE0 copy into a 256x64 scanout rectangle at (64,96) -> every pixel read back through BAR0),
#                      2 restore that rectangle; scanoutread = 0
#   m4flushlock0       the same with --lock-options 0 (lock in backing) instead of 1 (flush2, an earlier analysis)
#   gfxneuter-<n>      0.0.278 : accel gfxneuter <n> under hardtmo - 1 ARMS the GFX neuter (every VMID-2
#                      INDIRECT_BUFFER of a new GFX frame NOPed in Apple's ring before the doorbell: the wrapper and its own
#                      stamp run, the client's stream is DROPPED), 2 disarms; gfxneuterread reads the counters
#   gfxcapture-<n>     0.0.282 : accel gfxcapture <n> under hardtmo - 1 ARMS the READ-ONLY capture of every GFX
#                      submission at the source hook into the kext's binary capture ring (needs gfxneuter-1 first), 2 disarms;
#                      gfxcaptureread reads the counters
#   gfxprobe-<n>       0.0.283 : accel gfxprobe <n> - 1 ARMS the copy-back probe (fills SecurityAgent's last VRAM
#                      drawable of each captured submission with magenta; needs gfxcapture-1), 2 disarms; gfxproberead reads
#   capstream-start    0.0.282: navi48test capstream streams that ring to capture.bin (250 ms, nohup, stopfile); capstream-stop
#   finishread         0.0.279 : accel finishread under hardtmo - the 2D context's set_surface / finish (MPHWSync) /
#                      blit counters per process, READ-ONLY; ht-scanout-3 reads what the scanout holds (control rectangle, sampled hash)
#   copyongate         0.0.278: reads the flush-hook counters; only if at least one flush (selector 10) was seen does it run
#                      `accel flushhook 3` (log + whole-frame copy into the scanout, refused unless scanout-1 passed)
#   neutergate-<s>     0.0.278: the neuter's positive control after a tristart: wait up to <s> s for tri's `status: <n>` line;
#                      PASS needs `status: 4` (MTLCommandBufferStatusCompleted), neuter frames >= 1, 0 read-back
#                      mismatches and (0.0.279) at least one submission neutered AT THE SOURCE, else LIVE_ABORT (wskill and every later client step SKIPPED - no restart on a neuter that
#                      does not retire)
#   m4flush[-<rows>]   0.0.272: the walls 2-3 test client ~/navi48-staging/m4-flush --rows <rows> (default 128) --hold 5,
#                      console user root checked in the step (LIVE_ABORT otherwise), 180 s alarm -> m4-flush-<k>.txt
#   flushdrop          accel flushdrop (0.0.202 INSTRUMENT, an earlier analysis): after
#                      xlatregs, before sdmamap, rewrite the CS_PARTIAL_FLUSH after
#                      Apple's compute dispatch into two NOPs
#
# CAPTURE DISCIPLINE (each rule paid for by a lost run):
#   - nothing is redirected to /dev/null and nothing is grepped before it is saved;
#     every step's stdout AND stderr land in transcript.txt verbatim.
#   - summary.txt is derived from the saved files afterwards, and prints MISSING
#     for any pattern that must exist but does not — an empty result is a claim
#     about the query until a positive control says otherwise.
#   - one measurement per boot: `fire` installs once, and the accelerator can be
#     left wedged. --force overrides; the transcript records that it was used.
#   - THE ARMED ORDERING, and the script now refuses a list that breaks it . The review's binding
#     order for the class-A instrument is unchanged and must stay in this relative order:
#         ... ringmap  gfxneuter-279 (switch 23)  ringmap  gfxneuter-1  gfxneuter-3  ...
#         ... <the switch block>  gfxneuter-14 (the fence switch)  gfxcapture-1  capstream-start
#         ... armgate-300        <- the arm block completes HERE, and the gate holds the kill until it has
#         ... wskill            <- the window opens at the kill, never before the arm
#         ... wscompose-<run>   <- the frames that are scored
#     Every pre-arm verb - the switches, `ringmap 1`, the gate read-backs, the fence switch, the arm and its
#     read-back - is before `armgate`; `armgate` is before `wskill`. The closing read-back block after the window
#     is untouched.
set -u
# 0.0.269 : the kernel-log lines that mean Apple's engines died (hang detector, failed reset,
# permanent refusal). Defined here so the summary-only mode sees it too.
# 0.0.270 (rule 88): Apple's FAILURE path and its WARNINGS are counted apart. live1's six "hang lines"
# were warnings with Apple's own verdict "not hung, keep running"; counting both as one pattern read a clean run as failed.
LIVE_FATAL_PAT='is hung|GPURestartBegin|Failed to reset|could not be reset|Further submissions will not be accepted|restart previously failed'
# (rule 103: a watcher's silence is not the machine's). arm5 logged FIVE restart/timeout strings and read the
# run as healthy. THREE of them were genuinely unmatched and are added here: GPURestartSignaled, GPURestartEnqueued,
# GPURestartDeferred. The other two were ALREADY matched - `GFX event timeout` by `event timeout` and
# `checkGPUProgress() - Signaling hardware error` by `Signaling hardware error` - so's "matches NONE of them" is
# too strong; what failed in was reading only the FATAL count, not the warning count beside it (both are printed,
# :606 and :1586). WARN, NOT FATAL, and deliberately: arm5 logged GPURestartDeferred 28 times and GPURestartSkipped once
# and the machine never reset or panicked, so a fatal pattern here would abort every future run on a routine deferred
# restart - and aborting DESTROYS the capture, which is the opposite of what rule 103 asks for. The watcher's job is to
# speak, not to kill the run. GPURestartBegin - a restart that actually began - stays FATAL.
LIVE_WARN_PAT='event timeout|Signaling hardware error|GPURestartSkipped|GPURestartSignaled|GPURestartEnqueued|GPURestartDeferred|is not hung'
LIVE_HANG_PAT="$LIVE_FATAL_PAT|$LIVE_WARN_PAT"

T=$HOME/navi48-staging
N48=$T/navi48test
RUNS=$HOME/navi48-runs
DRY=0
FORCE=0
# ------------------------------------------------------------------ THE ARM ----
# an earlier analysis (run `arm12`): the COMMIT arm landed 84 s AFTER `wskill`. The arm was thrown OUT OF BAND while the
# recipe sat in `sleep300`; the sleep expired with the operator's batch still running and the script marched straight
# on - arm12's own transcript reads `STEP 50 sleep300 END rc=0 ms=300004 04:04:02.190509000` then
# `STEP 51 wskill START 04:04:02.197976000`, EIGHT MILLISECONDS later. 731 of 896 gate attempts then happened at ARM
# LEVEL 1 (`gfx-commit:   not-armed 731 frame(s)` vs `not-translate 165`), `COMMITTED 0`, and the run's Q1 never ran.
# The invariant this machinery makes STRUCTURAL instead of positional: the arm is LIVE BEFORE THE KILL, and a slow
# batch DELAYS THE KILL rather than the kill outrunning the arm. It changes nothing about what is measured - same
# verbs, same read-first lines, same capstream-start/stop and irq-pre/post placement - and a recipe that carries
# neither `armgate` nor `commitarm` behaves exactly as it did before.
ARM_REQUIRED=0    # `armgate` / `commitarm` in the recipe: this run intends a COMMIT arm, so every WindowServer
                  # restart path REFUSES until the arm's own read-back has confirmed it
ARM_CONFIRMED=0
ARM_WALL=""       # wall clock at which the confirming read-back was first SEEN in the log stream
ARM_EPOCH=0
# The kext's own verdict line, from `HWLOG("gfx-commit: ARM LEVEL IS %u (%s); one-shot %s%s. ...")` in
# src/apple/AppleHardwareHook.cpp. CONFIRMED verbatim in arm12's driverlog-stream.txt:
#   AppleHardwareHook: gfx-commit: ARM LEVEL IS 2 (COMMIT); one-shot ARMED (one-shot) - CHANGED BY THIS VERB. ARMED: ...
# `ARMED` is the only shot state that counts; SPENT and OFF do not match, and ARM LEVEL 1 (DECIDE) does not match.
ARM_OK_PAT='gfx-commit: ARM LEVEL IS 2 \(COMMIT\); one-shot ARMED'
zmodload zsh/datetime 2>/dev/null || true

now_s() { if [[ -n ${EPOCHREALTIME:-} ]]; then print -r -- "$EPOCHREALTIME"; else date +%s; fi }
wall()  { date +%T.%N }

# ---------------------------------------------------------------- summary ----
# Reads <dir>/transcript.txt, <dir>/driverlog.txt and <dir>/health.txt (any of the
# last two may be absent) and writes <dir>/summary.txt. Safe to run anywhere,
# including on the host Mac against captured logs — it touches no hardware.
summarize() {
    local D=$1
    local TRF=$D/transcript.txt DLF=$D/driverlog.txt HLF=$D/health.txt SUM=$D/summary.txt
    if [[ ! -f $TRF ]]; then print -r -- "summarize: no $TRF"; return 1; fi

    local ALL; ALL=$(mktemp -t accelsum) || return 1
    cat "$TRF" > "$ALL"
    [[ -f $HLF ]] && cat "$HLF" >> "$ALL"
    [[ -f $DLF ]] && cat "$DLF" >> "$ALL"
    # Driver-log patterns come from driverlog.txt; fall back to the transcript so a
    # run whose `log` step failed still gets counted rather than silently zeroed.
    local DRV=$DLF DRVSRC=driverlog.txt
    if [[ ! -f $DLF ]]; then DRV=$TRF; DRVSRC="transcript.txt (driverlog.txt MISSING)"; fi
    # 0.0.276 : a streamed run keeps most of its driver log in driverlog-stream.txt (the ring was consumed).
    local DSTREAM=""
    if [[ -f $D/driverlog-stream.txt ]]; then
        DSTREAM=$(mktemp -t accelstream) || return 1
        cat "$D/driverlog-stream.txt" > "$DSTREAM"; [[ -f $DLF ]] && cat "$DLF" >> "$DSTREAM"
        DRV=$DSTREAM; DRVSRC="driverlog-stream.txt + driverlog.txt"
        cat "$D/driverlog-stream.txt" >> "$ALL"
    fi

    local trl dll
    trl=$(wc -l < "$TRF" | tr -d ' ')
    dll=0; [[ -f $DLF ]] && dll=$(wc -l < "$DLF" | tr -d ' ')

    {
    print -r -- "run ${D:t} on $(hostname -s 2>/dev/null) — summarised $(date '+%F %T')"
    print -r -- "corpus: transcript ${trl} lines, driverlog ${dll} lines, health.txt $([[ -f $HLF ]] && echo present || echo MISSING)"

    # --- what code actually ran -------------------------------------------
    local kver
    kver=$(grep -iE 'com\.navi48\.bringup|Navi48Bringup' "$ALL" 2>/dev/null \
           | grep -oE '\([0-9]+\.[0-9]+\.[0-9]+\)' | head -1 | tr -d '()')
    print -r -- "kext (kmutil showloaded): ${kver:-MISSING — no com.navi48.bringup version in health.txt}"

    local cd_ sr_
    cd_=$(grep -E '"Navi48,ComputeDispatch" =' "$ALL" | head -1 | sed -E 's/.*= *//')
    sr_=$(grep -E '"Navi48,StageResult" =' "$ALL" | head -1 | sed -E 's/.*= *//')
    print -r -- "ComputeDispatch = ${cd_:-MISSING}   StageResult = ${sr_:-MISSING}"

    # --- per-step exit status and wall-clock duration ----------------------
    local steps
    steps=$(grep -E '^===> STEP .* END ' "$TRF")
    if [[ -z $steps ]]; then
        print -r -- "steps: MISSING — no '===> STEP … END rc=… ms=…' markers in transcript.txt"
    else
        print -r -- "steps (exit status / wall clock):"
        print -r -- "$steps" | awk '{n=$3; nm=$4; rc="?"; ms="?";
            for(i=1;i<=NF;i++){ if($i ~ /^rc=/) rc=substr($i,4); if($i ~ /^ms=/) ms=substr($i,4) }
            printf "  %-2s %-11s rc=%-4s %7s ms\n", n, nm, rc, ms }'
    fi

    # --- the two result lines ----------------------------------------------
    local pm4 stamp
    pm4=$(grep -E 'engine\+0x340 AFTER' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
    print -r -- "pm4powerup: ${pm4:-MISSING — no 'engine+0x340 AFTER' line in $DRVSRC}"
    stamp=$(grep -E 'poller.*fires.*fallbacks' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
    print -r -- "kiqstamp:   ${stamp:-MISSING — no 'poller … fires … fallbacks' line in $DRVSRC}"

    # --- counts (from $DRVSRC; the denominators are printed so 0 is readable) ---
    # kiqstamp's ARMED banner *describes* PROPAGATED and FALLBACK while explaining
    # itself. That one line is a legend, not an event, and counting it inflated
    # PROPAGATED by one on every run. Exclude it and say how many were excluded.
    local LEG='FALLBACK (logged) if not'
    local EV nlg; EV=$(mktemp -t accelev)
    nlg=$(grep -cF "$LEG" "$DRV")
    grep -vF "$LEG" "$DRV" > "$EV"
    local nfire nprop nfb nstart nstop nblk
    nfire=$(grep -cE 'FIRE #' "$EV")
    nprop=$(grep -cE 'PROPAGATED' "$EV")
    nfb=$(grep -cE 'FALLBACK' "$EV")
    nstart=$(grep -cE 'startEngineQueue\(queue=' "$EV")
    nstop=$(grep -cE 'stopEngineQueue\(queue=' "$EV")
    nblk=$(grep -cE 'regwrite #.* BLOCKED' "$EV")
    print -r -- "counts over $DRVSRC ($nlg legend line excluded): FIRE#=$nfire PROPAGATED=$nprop FALLBACK=$nfb startEngineQueue=$nstart stopEngineQueue=$nstop regwriteBLOCKED=$nblk"
    local dw
    dw=$(grep -E 'regwrite #.* BLOCKED' "$EV" | grep -oE 'dword 0x[0-9a-f]+' | sort -u | sed 's/dword //' | tr '\n' ' ' | sed 's/ *$//')
    rm -f "$EV"
    print -r -- "blocked dwords: ${dw:-MISSING (0 BLOCKED lines found)}"

    # --- the tri render phase (only when a tristart step ran) -----------------
    if grep -qE '^===> STEP [0-9]+ tristart' "$TRF"; then
        print -r -- "tri render phase (transcript):"
        local bl
        bl=$(grep -E '^### tri\.state (now|final):' "$TRF" | sed 's/^### //' | cut -c1-150)
        print -r -- "${bl:-MISSING - no '### tri.state now/final' line}" | sed 's/^/  /'
        bl=$(grep -E '^### takeover starts' "$TRF" | tail -1 | sed 's/^### //')
        print -r -- "  ${bl:-takeover timing: MISSING}"
        bl=$(grep -E '^### tri (EXITED|STILL RUNNING)' "$TRF" | tail -1 | sed 's/^### //' | cut -c1-150)
        print -r -- "  ${bl:-tri exit: MISSING}"
        bl=$(grep -E '^TRI: ' "$TRF" | tail -1 | cut -c1-220)
        print -r -- "  ${bl:-VERDICT MISSING - no 'TRI:' line from tri (the render did not complete)}"
        # 0.0.235 : the async completion result. Printed only when --nowait ran, so a
        # missing line on an ordinary suite run is correct and says so rather than reading as a failure.
        if grep -qE '^===> STEP [0-9]+ tristart-suitenw' "$TRF"; then
            bl=$(grep -E '^TRI-ASYNC: ' "$TRF" | tail -1 | cut -c1-220)
            print -r -- "  ${bl:-TRI-ASYNC MISSING - tristart-suitenw ran but tri printed no TRI-ASYNC line}"
            bl=$(grep -E '^\[tri\] NOWAIT:' "$TRF" | tail -1)
            print -r -- "  ${bl:-NOWAIT registration line MISSING}"
        fi
        bl=$(grep -E '^TRI-CASE: ' "$TRF" | cut -c1-260)   # 0.0.224 : one line per suite case
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/  /'
        bl=$(grep -E '^\[tri\] SUITE pipeline ' "$TRF" | cut -c1-200)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/  /'
        bl=$(grep -E '^\[tri\] INJECT:' "$TRF" | tr '\n' ';' | cut -c1-200)
        [[ -n $bl ]] && print -r -- "  inject: $bl"
        bl=$(grep -E '^  (renderxlat status|draw \(modes 4/5\))' "$TRF" | tr -s ' ' | tr '\n' ';' | cut -c1-400)
        print -r -- "  renderxlat: ${bl:-MISSING - no renderxlat step output}"
        bl=$(grep -E '^\[tri\] RING buffer|^TRI-RING:|^### ring base from tri|^ABORT (renderdrawring|ringpages)|^  GE rings / CU' "$TRF" | cut -c1-260)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/  ring: /'
        bl=$(grep -E 'renderib: (GE ring pages|ring scan check|page count check|.*REFUSING)|render-draw: (GE rings|gfx12 preamble)|: chan [0-9]+ ring wptr|vm-fault LATCHED|irq: client 10 src (0|239) ' "$DRV" | head -16 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/    /'
        bl=$(grep -E '^reg\[0x(2c87|2c88|c26[89ab]|c44[4-7])\]' "$TRF" | tr '\n' ' ' | cut -c1-900)
        [[ -n $bl ]] && print -r -- "  ring/CU reads (in step order): $bl"
        bl=$(grep -E '^### (render gate (OPEN|CLOSED)|SKIPPED)' "$TRF" | sed 's/^### //' | tr '\n' ';' | cut -c1-200)
        print -r -- "  gate: ${bl:-MISSING - no rendergate line}"
        bl=$(grep -E 'renderib: (candidate [0-9]+:|[0-9]+ pending host page|PM4 at VA [0-9a-fx]+ \(run)|render-xlat: (self-test|WROTE|BLANK|TRANSLATE|.*REFUS)|render-draw: (draw-policy|DRAW translate|VS program|PS program|WROTE|mode 5|.*REFUSING)' "$DRV" | head -14 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-260)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/    /'
        bl=$(grep -E 'render-suite: (profile|stream [0-9]+ at|segment [0-9]+(:| DRAW)|translated stream|WROTE the|mode 7|.*REFUSING|.*PARTIAL)' "$DRV" | head -40 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)   # 0.0.224
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/    /'
        bl=$(grep -E '^### suite: ' "$TRF" | tail -1)
        [[ -n $bl ]] && print -r -- "  $bl"
        bl=$(grep -E 'Apple rings mapped' "$TRF" | tail -1 | sed -E 's/^ +//')
        print -r -- "  sdmamap: ${bl:-MISSING}"
        bl=$(grep -E 'faults\[GCVM' "$DRV" | tail -2 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
        print -r -- "${bl:-MISSING - no 'faults[GCVM' line}" | sed 's/^/  /'
        bl=$(grep -E '^reg\[0x' "$TRF" | tr '\n' ' ' | cut -c1-300)
        print -r -- "  reg: ${bl:-MISSING}"
    fi

    # --- tri dumps (0.0.212, an earlier analysis): no commit, userspace only ---------------
    if grep -qE '^===> STEP [0-9]+ tridump-' "$TRF"; then
        print -r -- "tri dumps (transcript; files in tridump-<v>/):"
        local bl
        bl=$(grep -E '^### tridump-[a-z]+ (EXITED|STILL RUNNING)' "$TRF" | sed 's/^### //' | cut -c1-160)
        print -r -- "${bl:-MISSING - no '### tridump-<v> EXITED' line}" | sed 's/^/  /'
        bl=$(grep -E '^(\[tri\] DUMP-REGIONS|TRI-DUMP:|\[tri\] INJECT:)' "$TRF" | cut -c1-230)
        print -r -- "${bl:-MISSING - no DUMP-REGIONS / TRI-DUMP line}" | sed 's/^/  /'
        bl=$(grep -cE '^\[tri\] DUMP (init|draw|end) s[0-9]+' "$TRF")
        print -r -- "  [tri] DUMP stream lines: $bl"
    fi

    # --- the in-kext boot chain (0.0.237, an earlier analysis) ------------------
    if grep -qE '^===> STEP [0-9]+ bootchain' "$TRF"; then
        print -r -- "boot chain (the accelerator-start sequence run in-kext, no ssh verb):"
        local bl
        bl=$(grep -E '^  (bootchain state|mode \(navi48-boot-chain\)|FAILED AT|synctables|setvspace / kiqenable|pm4powerup #1|gfxmap / copyarm|phase A wall clock|phase B takeover|  xlatregs)' "$TRF" \
             | tr -s ' ' | tr '\n' ';' | cut -c1-600)
        print -r -- "  ${bl:-MISSING - no bootchain out-scalars (the verb never returned)}"
        bl=$(grep -E 'boot-chain: ' "$DRV" | head -16 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-260)
        print -r -- "${bl:-  MISSING - no 'boot-chain' line in the driver log}" | sed 's/^/    /'
    fi

    # --- the completion bridge (0.0.235, an earlier analysis) -------------------
    if grep -qE '^===> STEP [0-9]+ eopbridge' "$TRF"; then
        print -r -- "completion bridge (IH -> Apple's checkTimestamps):"
        local bl
        bl=$(grep -E '^  (eopbridge|EOP entries seen|checkTimestamps calls|nothing outstanding|channels RETIRED|refusals|last chan)' "$TRF" \
             | tr -s ' ' | tr '\n' ';' | cut -c1-500)
        print -r -- "  ${bl:-MISSING - no eopbridge out-scalars (the verb never returned)}"
        bl=$(grep -E 'eop-bridge: ' "$DRV" | head -8 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "${bl:-  MISSING - no 'eop-bridge' line in the driver log}" | sed 's/^/    /'
        bl=$(grep -E '^  (IRQ|interrupts)|irq: .* eop ' "$TRF" | tail -2 | cut -c1-200)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/  /'
    fi

    # --- the VM-context observe boot (0.0.247, an earlier analysis) -------------
    # READ-ONLY throughout. Every line prints MISSING rather than nothing (rule 25).
    if grep -qE '^===> STEP [0-9]+ vmctx' "$TRF"; then
        print -r -- "VM context observe (VMM slots 40/41, READ-ONLY):"
        local bl
        bl=$(grep -E '^  (observe pair|createVMContext calls|releaseVMContext calls|live contexts now|root NOW|Apple CONTEXT2 root|live roots AGREEING|root at create|last root at release|root\[511\]|ENG17 ADDR_RANGE)' "$TRF" \
             | tr -s ' ' | tr '\n' ';' | cut -c1-700)
        print -r -- "  ${bl:-MISSING - no vmctx out-scalars (the verb never returned)}"
        bl=$(grep -E 'vmctx: (createVMContext|releaseVMContext|entry [0-9]+ -)' "$DRV" | head -10 \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "${bl:-  MISSING - no per-context 'vmctx:' line in $DRVSRC}" | sed 's/^/    /'
        bl=$(grep -E 'vmctx: (hook state|DONE|GCVM_INVALIDATE_ENG17)' "$DRV" | tail -3 \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "${bl:-  MISSING - no 'vmctx: DONE' line in $DRVSRC}" | sed 's/^/    /'
        bl=$(grep -E 'VM manager .* hooked: ' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-260)
        print -r -- "  ${bl:-MISSING - no 'VM manager ... hooked' line: patch_vmm never ran}"

        # --- 0.0.252 : the slot-37 unmapVA observe hook ----
        # The teardown line is the whole reason a boot is spent here, and it lives in
        # the driver log, which can hit capacity and drop its closing lines (rule 27).
        # Surface it in the summary instead. Every line prints MISSING (rule 25).
        print -r -- "  mapVA/unmapVA hooks (slots 36/37) - the ARM, the withdrawal and the re-arm:"
        bl=$(grep -E '^  (unmapVA \(slot 37\) fires|root already 0 at entry|withdrawals / refusals|contexts patched / ref)' "$TRF" \
             | tr -s ' ' | tr '\n' ';' | cut -c1-500)
        print -r -- "    ${bl:-MISSING - no slot-37 out-scalars (rootwriteread never returned)}"
        # P2, THE MEASUREMENT: a root that was live at entry and reads 0 at return means
        # THIS call freed the page, so this call's entry was the last safe instant.
        bl=$(grep -E 'unmapva: \*\*\* ROOT FREED BY THIS CALL \*\*\*' "$DRV" \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-400)
        print -r -- "${bl:-    MISSING - no 'ROOT FREED BY THIS CALL' line in $DRVSRC: either no client teardown was seen, or the root is freed somewhere this hook cannot see (section 411 P2)}" | sed 's/^/    /'
        # P1: did slot 37 fire for a client context at all? Its ENTRY lines carry the
        # falsifier too - ctx+0x98+0x20 reading 0 at entry means the hook is still late.
        bl=$(grep -E 'unmapva: ENTRY for ctx' "$DRV" | head -6 \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "${bl:-    MISSING - no 'unmapva: ENTRY' line in $DRVSRC: slot 37 never fired for a recorded context (section 411 P1 FALSIFIED)}" | sed 's/^/    /'
        # P3: how many unmaps the root SURVIVED - the re-arm traffic the final design pays.
        bl=$(grep -E 'unmapva: return for ctx' "$DRV" | tail -3 \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "${bl:-    (no 'unmapva: return' line - no mid-life unmap was logged, which is section 411 P3's expected 0)}" | sed 's/^/    /'
        bl=$(grep -E 'vmctx-patch: .* slot 37' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300)
        print -r -- "    ${bl:-MISSING - no 'vmctx-patch ... slot 37' line: the slot-37 patch never installed}"
        bl=$(grep -E 'rootwrite: REFUSING mode 1' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-200)
        [[ -n $bl ]] && print -r -- "    $bl"
    fi

    # --- 0.0.356 : rootwrite on WindowServer's context, BY OWNER -------
    # Every line prints MISSING rather than nothing (rule 25). Verify against the driver log, not this block.
    if grep -qE '^===> STEP [0-9]+ rootwritews' "$TRF"; then
        print -r -- "rootwrite BY OWNER (0.0.356, WindowServer's context):"
        local rl
        rl=$(grep -E '^  (rootwrite  |guard verdict|target \(0.0.356\)|owner / CONTEXT2|block-clear witness|gfx power at the write|walk BEFORE the arm|walk through the root|entry written / read|fault status after)' "$TRF" | tr -s ' ' | tail -10)
        print -r -- "${rl:-  MISSING - no rootwrite out-scalar lines in the transcript}" | sed 's/^/  /'
        rl=$(grep -E 'rootwrite: (BY OWNER -|WALK VA|WRITTEN\.|G8 GFX POWER BRACKET)|rootwrite-arm: (GUARD VERDICT|G5 BLOCK-CLEAR)' "$DRV" | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-300 | tail -12)
        print -r -- "${rl:-  MISSING - no 'rootwrite:' by-owner lines in $DRVSRC}" | sed 's/^/    /'
        rl=$(grep -cE 'unmapva: WITHDRAWN' "$DRV"); print -r -- "  withdrawals logged: $rl; re-arms logged: $(grep -cE 'unmapva: RE-ARMED' "$DRV"); releases of an armed context: $(grep -cE 'RELEASE OF AN ARMED CONTEXT' "$DRV")"
    fi

    # --- the blit phase (only when a blit2start step ran) --------------------
    # Every line prints MISSING rather than nothing (rule 25).
    if grep -qE '^===> STEP [0-9]+ blit2start ' "$TRF"; then
        print -r -- "blit phase (transcript + $DRVSRC):"
        local bl npo nsk
        bl=$(grep -E '^### blit2\.state (now|final):' "$TRF" | sed 's/^### //' | cut -c1-150)
        print -r -- "${bl:-MISSING - no '### blit2.state now/final' line (blit2start/blit2wait did not complete)}" | sed 's/^/  /'
        bl=$(grep -E '^### takeover starts' "$TRF" | tail -1 | sed 's/^### //')
        print -r -- "  ${bl:-takeover timing: MISSING - no '### takeover starts' line}"
        bl=$(grep -E '^### blit2 (EXITED|STILL RUNNING)' "$TRF" | tail -1 | sed 's/^### //' | cut -c1-150)
        print -r -- "  ${bl:-blit2 exit: MISSING - no '### blit2 EXITED/STILL RUNNING' line}"
        bl=$(grep -E '^(status: |DATA: |error: )' "$TRF" | tr '\n' ';' | cut -c1-160)
        print -r -- "  blit2 output: ${bl:-MISSING - no 'status:/DATA:' line from blit2}"
        bl=$(grep -E 'Apple rings mapped' "$TRF" | tail -1 | sed -E 's/^ +//')
        print -r -- "  sdmamap: ${bl:-MISSING - no 'Apple rings mapped' line}"
        bl=$(grep -E '^  (vmib|first dword|page \(physical\)|CP_STAT) ' "$TRF" | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ' ')
        print -r -- "  vmib: ${bl:-MISSING - no vmib out-scalar lines}"
        bl=$(grep -E 'vmib: [0-9]+ dwords from' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-120)
        print -r -- "  vmib dump: ${bl:-MISSING - no 'vmib: N dwords from' line in $DRVSRC}"
        bl=$(grep -A1 -E 'vmib: [0-9]+ dwords from' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-120)
        [[ -n $bl ]] && print -r -- "    $bl"
        bl=$(grep -E 'vmib: CP_STAT' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
        print -r -- "  CP: ${bl:-MISSING - no 'vmib: CP_STAT' line in $DRVSRC}"
        bl=$(grep -E '^reg\[0x' "$TRF" | tr '\n' ' ' | cut -c1-300)
        print -r -- "  reg: ${bl:-MISSING - no 'reg[0x...] =' lines}"
        # section 317: the park decode, first and last read of each register (MISSING if never read)
        rv() { local x; x=$(grep -E "^reg\[$1\] = " "$TRF" | sed -E 's/.* = //' \
                            | awk 'NR==1{f=$0} {l=$0} END{if (NR==1) print f; else if (NR>1) print f "|" l}')
               print -r -- "${x:-MISSING}"; }
        print -r -- "  park (first|last): CP_STAT=$(rv 0x21a0) STALLED2=$(rv 0x219e) ME_HDR=$(rv 0x21a1) PFP_HDR=$(rv 0x21a2)"
        print -r -- "    IB1 BUFSZ/CMD=$(rv 0xc0ce)/$(rv 0xc0c0) SE0-3=$(rv 0x2005),$(rv 0x2006),$(rv 0x200e),$(rv 0x200f) INIT=$(rv 0x2e00) DIM_X=$(rv 0x2e01) SQ_TRAPS=$(rv 0x2316)"
        if grep -qE '^===> STEP [0-9]+ flushdrop ' "$TRF"; then
            bl=$(grep -E '^  (flushdrop|blit IB \(VMID 2\) VA|host page \(physical\)|DISPATCH_DIRECT at dword|rewritten dwords at|read-back mismatches) ' "$TRF" \
                 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-300)
            print -r -- "  flushdrop verb: ${bl:-MISSING - a flushdrop step ran but printed no result}"
            bl=$(grep -E 'flushdrop: (REWROTE|already|REFUSING|.*REFUSING)' "$DRV" | head -3 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
            print -r -- "${bl:-MISSING - no flushdrop REWROTE/REFUSING line in $DRVSRC}" | sed 's/^/    /'
        fi
        bl=$(grep -E 'faults\[GCVM' "$DRV" | tail -2 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
        print -r -- "${bl:-MISSING - no 'faults[GCVM' line in $DRVSRC}" | sed 's/^/  /'
        bl=$(grep -E '^interrupts ' "$TRF" | sed -n '1p;$p' | cut -c1-90 | tr '\n' '|')
        print -r -- "  counters (first|last): ${bl:-MISSING - no 'interrupts' line from counters}"
        npo=$(grep -c 'pagecopy-observe:' "$DRV"); nsk=$(grep -c 'pageTexture SKIPPED' "$DRV")
        print -r -- "  pagecopy-observe lines: $npo   pageTexture SKIPPED lines: $nsk"
        grep -E 'pagecopy-observe: .*(SOURCE|refused|physical=|could not|descriptor=|census|readBytes)' "$DRV" | head -10 \
            | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-160 | sed 's/^/    /'
        if grep -qE '^===> STEP [0-9]+ (pagecopy|copyarm) ' "$TRF"; then
            bl=$(grep -E '^  (pagecopy|copies \(sysmem->VRAM\)|read-back dwords|read-back MISMATCHED|unhandled \(skipped\)|page-outs \(skipped\)|last destination \(VRAM\)|last copy time|failed mid-copy) ' "$TRF" \
                 | tail -9 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-300)
            print -r -- "  pagecopy verb (last): ${bl:-MISSING - a pagecopy step ran but printed no counters}"
        fi
        bl=$(grep -E 'residency-copy: ' "$DRV" | grep -E 'COPIED|now reads|UNHANDLED|REFUS|FAILED' | head -5 \
             | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
        [[ -n $bl ]] && print -r -- "$bl" | sed 's/^/    /'
        if grep -qE '^===> STEP [0-9]+ (vmpage-0x[0-9a-fA-F]+) ' "$TRF"; then
            # 0.0.206 : the diag records, first and last vmpage read
            local nvp
            nvp=$(grep -cE '^===> STEP [0-9]+ vmpage-0x[0-9a-fA-F]+ START' "$TRF")
            for k in 1 $nvp; do
                bl=$(awk -v want="$k" '/^===> STEP [0-9]+ vmpage-0x[0-9a-fA-F]+ START/{n++} n==want && /^  vmpage[^:]*: /' "$TRF" \
                     | sed -E 's/^  vmpage +: /walk: /; s/^  vmpage ?//; s/ +: /=/' | tr '\n' ';' | cut -c1-700)
                print -r -- "  vmpage read $k of $nvp: ${bl:-MISSING - that vmpage step printed no result}"
                (( nvp == 1 )) && break
            done
            bl=$(grep -E 'vmpage: decoder self-test' "$DRV" | tail -1 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
            print -r -- "    ${bl:-MISSING - no 'vmpage: decoder self-test' line in $DRVSRC}"
            grep -E 'vmpage: rec ' "$DRV" | head -8 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-150 | sed 's/^/    /'
            bl=$(grep -cE 'vmpage: rec ' "$DRV")
            print -r -- "    vmpage record lines in $DRVSRC: $bl"
        fi
        if grep -qE '^===> STEP [0-9]+ (kernsub|kerndiag|kerndiagmin) ' "$TRF"; then
            bl=$(grep -E '^  (kernsub|shader VRAM address|gfx1201 kernel bytes|read-back MISMATCHED|substitutions this boot|armed|kernel mode|kernel dwords) ' "$TRF" \
                 | tail -8 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-320)
            print -r -- "  kernsub verb: ${bl:-MISSING - a kernsub step ran but printed no result}"
            bl=$(grep -E '(kernsub|residency-copy): .*(SUBSTITUTED|already substituted|REFUSED)' "$DRV" | head -3 \
                 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-170)
            print -r -- "${bl:-MISSING - no kernsub SUBSTITUTED/REFUSED line in $DRVSRC}" | sed 's/^/    /'
        fi
    fi

    # --- P2 (0.0.267, an earlier analysis): WindowServer's GPU-arrival handling --------
    if grep -qE '^===> STEP [0-9]+ wslogstop ' "$TRF"; then
        grep -E '^P2 |^positive control \(start marker seen\)' "$TRF" | cut -c1-260
        bl=$(grep -E '^  pairing verdict|^  decided at accel start|^  keys stamped' "$TRF" | tail -3 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';')
        print -r -- "pairing verb (last): ${bl:-MISSING - no pairing verb output}"
        bl=$(grep -E 'pairing: (decision taken|NOT stamping|STAMPED|WITHDRAWN)' "$DRV" | head -3 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-200)
        print -r -- "${bl:-MISSING - no pairing decision line in $DRVSRC}" | sed 's/^/    /'
        grep -E '^  framebuffer\[[0-9]+\] regID=.* -> kr=' "$TRF" | head -3 | sed 's/^ */display-registry: /'
        grep -E '^(IOAccelerator nubs|U-state processes): ' "$TRF" | tr '\n' ';' | sed 's/^/wsstate (in order): /'; print -r -- ""
        grep -E '^ +[0-9]+ .*WindowServer' "$TRF" | head -4 | sed 's/^ */WindowServer row: /'
    fi

    # --- TEST A (0.0.268, an earlier analysis) -----------------------------------
    if grep -qE '^===> STEP [0-9]+ wskill ' "$TRF"; then
        grep -E '^(WindowServer (before|after)|kill epoch|console user immediately before the kill|kickstart )' "$TRF" | cut -c1-220 | sed 's/^/testA: /'
        grep -E '^(console user|IOAccelerator nubs|U-state processes|accelerator subtree entries|framebuffer IOAccelTypes lines|now kill\+)' "$TRF" | tr '\n' ';' | cut -c1-900 | sed 's/^/testA snapshots: /'; print -r -- ""
        grep -E '^kextlog |^  CENSUS lines' "$TRF" | cut -c1-300 | sed 's/^/testA /'
        # an earlier analysis: the arm/kill ordering, from the transcript's own lines. A MISSING here on an armed run
        # means the recipe carried no `armgate` - which is the defect that voided arm12, not a formatting problem.
        bl=$(grep -E '^(armwskill:|armgate|ARM GATE|commitarm)' "$TRF" | cut -c1-260)
        print -r -- "${bl:-MISSING - no armgate/armwskill line: this recipe declared no COMMIT arm }" | sed 's/^/arm order: /'
    fi

    # --- M4 route c' adoption (notes/M4-FRAME-DELIVERY.md section 6) --------------
    if grep -qE '^===> STEP [0-9]+ m4adopt ' "$TRF"; then
        print -r -- "m4adopt (m4-adopt.txt via transcript):"
        bl=$(grep -E '^m4adopt (exit|launch)|^console user immediately before m4-adopt' "$TRF" | cut -c1-200)
        print -r -- "${bl:-MISSING - no m4adopt launch/exit line}" | sed 's/^/  /'
        bl=$(grep -E '^\[probe\]|surface-create verdict|^  (accelerator|framebuffer) regID|^\[keys |^  IOAccel(Types|Index|Revision) |^  IOCFPlugInTypes |IOConnectCallScalarMethod\(conn, sel=7' "$TRF" | cut -c1-220)
        print -r -- "${bl:-MISSING - m4-adopt printed none of its result lines}" | sed 's/^/  /'
        if [[ -f $D/kernlog.txt ]]; then
            print -r -- "  kernlog 'failed to get the vram descriptors': $(grep -c 'failed to get the vram descriptors' "$D/kernlog.txt"); IOAcceleratorFamily2 lines $(grep -c '(IOAcceleratorFamily2)' "$D/kernlog.txt"); IOGraphicsFamily lines $(grep -c '(IOGraphicsFamily)' "$D/kernlog.txt")"
            grep -E 'vram descriptors|IOAccelDisplay|display pipe|IOAcceleratorFamily2' "$D/kernlog.txt" | head -8 | cut -c1-240 | sed 's/^/    kernlog: /'
        fi
        for f in driverlog-pre driverlog-post; do
            [[ -f $D/$f.txt ]] && print -r -- "  $f: regwrite BLOCKED $(grep -c 'regwrite #.* BLOCKED' "$D/$f.txt"), createVMContext $(grep -c 'createVMContext #' "$D/$f.txt"), pairing lines $(grep -c 'pairing: ' "$D/$f.txt"), lines $(wc -l < "$D/$f.txt" | tr -d ' ')"
        done
    fi

    # --- M4 walls 2-3 (0.0.272, an earlier analysis) --------------------------------
    if grep -qE '^===> STEP [0-9]+ (flushhook|scanout|m4flush|keysgate)' "$TRF"; then
        print -r -- "m4 walls 2-3 (transcript out-scalars + $DRVSRC):"
        grep -E '^  (flushhook status|mode / installed|surfaces minted|foreign / guard|selector 7|shape calls|lock / unlock|flush calls|copies ok|last copy status|last flushed id)' "$TRF" | tail -11 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-700 | sed 's/^/  flushhook (last): /'; print -r -- ""
        grep -E '^  (scanout status|geometry / plan|scanout VRAM|rectangle x|pre-flight mism|fence us|rectangle mism|samples expected|interlock / runs)' "$TRF" | sed -E 's/ +: /=/; s/^ +//' | cut -c1-200 | sed 's/^/  scanout: /'
        grep -E '^(m4flush (launch|exit)|  selector (7|9|3|4) |  IOServiceOpen|\[flush\]|\[unlock\]|  WROTE|  info decoded|  stride used|  RDNA4FB IOAccelTypes)' "$TRF" | cut -c1-220 | sed 's/^/  m4-flush: /'
        grep -E 'flush-hook: (install|first surface|surface .* GUARD|mode now|#[0-9]+ surface|  lock info|accelerator display|  framebuffer\[|  buffer\[|COPY of)|scanout: (RDNA4FB|SDMA0|pattern buffer|PRE-FLIGHT|POSITIVE|READBACK|RESTORED)|scanout-copy: ' "$DRV" | head -60 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-330
        grep -E '^KEYS GATE|^  (selector 7 refused|shape refused|WindowServer refused)' "$TRF" | cut -c1-220 | sed 's/^/  /'
        grep -E 'flush-hook: (REFUSED|REJECTIONS)' "$DRV" | head -24 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-360
        print -r -- "  census [WS] lines: $(grep -c 'CENSUS #[0-9]* .*\[WS\]' "$DRV")"
    fi

    # --- the GFX neuter (0.0.278, an earlier analysis) ---------------------------------
    if grep -qE '^===> STEP [0-9]+ (gfxneuter|neutergate)' "$TRF"; then
        print -r -- "gfx neuter (transcript + $DRVSRC):"
        grep -E '^(neutergate:|NEUTER GATE|COPY GATE)' "$TRF" | cut -c1-220 | sed 's/^/  /'
        grep -E '^  (neuter armed|not-an-IB|CP_RB0_RPTR / WPTR|render drain / walked)' "$TRF" | tail -4 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-600 | sed 's/^/  verb (last): /'; print -r -- ""
        print -r -- "  0.0.279: source-neutered lines $(grep -c 'gfx-src: submission #' "$DRV"), source refusal lines $(grep -c 'gfx-src: submission NOT neutered' "$DRV"), PUBLISHED-BEFORE-HOOK lines $(grep -c 'PUBLISHED BEFORE THE HOOK' "$DRV"), ctx2d-call lines $(grep -c 'ctx2d-call: ' "$DRV"), sync-RETURNED lines $(grep -c "WindowServer's sync RETURNED" "$DRV"), after-sync surface lines $(grep -c 'AFTER A SYNC RETURNED' "$DRV"), CONTENT READ lines $(grep -c 'scanout: CONTENT READ' "$DRV")"
        grep -E 'gfx-src: (INSTALLED|install|channel slot|no AMDGFX)|ctx2d-call: (slot 296|hook )' "$DRV" | head -6 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-260
        grep -E '^  (method hook|set_surface / blit|finish calls|WindowServer finish|control rect|sampled FNV|sampled non-black|centre / TL|source neuter|published before)' "$TRF" | tail -12 | sed 's/^/   /'
        print -r -- "  kext: neutered-frame lines $(grep -c 'gfx-neuter: frame #' "$DRV"), RACE lines $(grep -c 'gfx-neuter: .*RACE' "$DRV"), STOPPED-walk lines $(grep -c 'gfx-neuter: .*walk STOPPED' "$DRV"), FLUSH lines $(grep -c 'flush-hook: FLUSH' "$DRV"), COPY lines $(grep -c 'flush-hook: COPY #' "$DRV"), FRAME CONTENT lines $(grep -c 'scanout-staged: FRAME CONTENT' "$DRV")"
        grep -E 'gfx-neuter: (frame #|control|  pid)' "$DRV" | head -14 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-300
        grep -E 'flush-hook: (FLUSH|COPY #)|scanout-staged: (FRAME CONTENT|READBACK|SDMA copy)|scanout-copy: ' "$DRV" | head -12 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-360
        grep -E '^TRI:|^status: ' "$TRF" | head -3 | cut -c1-220 | sed 's/^/  tri: /'
    fi

    # --- the source capture (0.0.282, an earlier analysis) -------------------------------
    if grep -qE '^===> STEP [0-9]+ (gfxcapture|capstream|gfxprobe)' "$TRF"; then
        print -r -- "gfx capture (transcript + $DRVSRC):"
        grep -E '^  (capture armed|submissions seen|IBs / regions|programs  |CONTEXT2 disagreed|ring dropped)' "$TRF" | tail -6 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-600 | sed 's/^/  verb (last): /'; print -r -- ""
        print -r -- "  capture.bin: $(wc -c < "$D/capture.bin" 2>/dev/null | tr -d ' ' || print MISSING) byte(s); $(grep -m1 'capstream end' "$D/capstream.out" 2>/dev/null || print 'capstream end line MISSING')"
        print -r -- "  kext: frame lines $(grep -c 'gfx-capture: F[0-9]* on ' "$DRV"), control lines $(grep -c 'gfx-capture: control' "$DRV")"
        grep -E '^  (probe armed|drawables FILLED|refused plan/page|not SecurityAgent)' "$TRF" | tail -4 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | cut -c1-400 | sed 's/^/  probe verb (last): /'; print -r -- ""
        grep -E 'gfx-probe: F[0-9]+ ' "$DRV" | head -4 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-360
        grep -E 'gfx-capture: F[0-9]+ on ' "$DRV" | head -6 | sed -E 's/^[A-Za-z0-9]+: /    /' | cut -c1-420
    fi

    # --- LIVENESS (0.0.269, an earlier analysis) -----------------------------------
    if grep -qE '^===> STEP [0-9]+ (soak-[0-9]+|drain|livecheck-[A-Za-z0-9]+) ' "$TRF" || grep -q 'drain: ' "$DRV" 2>/dev/null; then
        bl=$(grep -E 'drain: (\*\*\* LIVE \*\*\*|NOT LIVE|REFUSED \(reason)' "$DRV" | head -2 | sed -E 's/^[A-Za-z0-9]+: //' | cut -c1-240)
        print -r -- "drain arm: ${bl:-MISSING - no drain LIVE/NOT LIVE/REFUSED line in $DRVSRC}"
        print -r -- "drain lines: sub $(grep -c 'drain: chan [0-9]* sub #' "$DRV") (logged, cap 256), FIRST-per-pid $(grep -c 'drain: FIRST submission serviced for pid' "$DRV"), REFUSAL samples $(grep -c 'drain: REFUSAL sample' "$DRV"), RING STOP samples $(grep -c 'drain: RING STOP sample' "$DRV"), GPUVM_INV samples $(grep -c 'drain: GPUVM_INV sample' "$DRV"), POLL samples $(grep -c 'drain: POLL sample' "$DRV"), boot-chain [8]/[9] $(grep -cE 'boot-chain: \[(8|9)\]' "$DRV")"
        grep -E 'drain: FIRST submission serviced for pid' "$DRV" | head -8 | sed -E 's/^[A-Za-z0-9]+: /  /' | cut -c1-160
        grep -E '^  (drain state|submissions translated|IBs  |write fails|polls neutered|GPUVM_INV passed|max latency|ring stops)' "$TRF" | tail -8 | sed -E 's/ +: /=/; s/^ +//' | tr '\n' ';' | sed 's/^/drain verb (last): /'; print -r -- ""
        grep -E '^LIVE ' "$TRF" | sed -n '1p;$p' | cut -c1-240
        print -r -- "LIVE ticks: $(grep -cE '^LIVE soak-t' "$TRF"); worst FATAL count seen: $(grep -oE 'kernlog FATAL lines [0-9]+' "$TRF" | awk '{print $4}' | sort -n | tail -1); worst warning count: $(grep -oE 'warning lines [0-9]+' "$TRF" | awk '{print $3}' | sort -n | tail -1)"
        [[ -f $D/kernlog.txt ]] && print -r -- "kernlog: $(wc -l < "$D/kernlog.txt" | tr -d ' ') lines; FATAL-pattern lines $(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt"); warning-pattern lines $(grep -cE "$LIVE_WARN_PAT" "$D/kernlog.txt"); AMDRadeonX6000 lines $(grep -c '(AMDRadeonX6000)' "$D/kernlog.txt")"
        grep -E 'render-drain: (ARMED|ATTEMPT|renderxlat|keystone)|boot-chain: \[10\]|eop-bridge: SDMA TRAP' "$DRV" 2>/dev/null | head -8 | sed -E 's/^[A-Za-z0-9]+: /  /' | cut -c1-230
        grep -E '^  SDMA trap entries' "$TRF" | tail -1
    fi

    # --- anything alarming --------------------------------------------------
    # 'fault' is matched with a guard so "default" does not false-positive.
    local alerts nal
    alerts=$( { grep -nE 'PANIC|REFUS|refused|not a kernel pointer|AT CAPACITY' "$ALL"
                grep -niE '(^|[^e])fault' "$ALL"; } 2>/dev/null | sort -t: -k1,1n -u )
    nal=$(print -r -- "$alerts" | grep -c . )
    if (( nal == 0 )); then
        print -r -- "alerts: none — PANIC|fault|REFUS|refused|'not a kernel pointer'|'AT CAPACITY' over $(wc -l < "$ALL" | tr -d ' ') lines"
    else
        print -r -- "alerts: $nal line(s) match PANIC|fault|REFUS|refused|'not a kernel pointer'|'AT CAPACITY':"
        print -r -- "$alerts" | head -3 | cut -c1-170 | sed 's/^/  /'
        (( nal > 3 )) && print -r -- "  … $(( nal - 3 )) more (grep the saved files)"
    fi

    # --- the driver log's own trailer ---------------------------------------
    local bytes
    bytes=$(grep -E '\([0-9]+ bytes of driver log' "$ALL" | tail -1 | sed -E 's/^ *//' | cut -c1-140)
    print -r -- "driver log: ${bytes:-MISSING — no '(N bytes of driver log)' trailer; the log step did not complete}"

    # --- health -------------------------------------------------------------
    local gb ga verdict
    gb=$(sed -n 's/^### gpuRestart-before //p' "$ALL" | head -1)
    ga=$(sed -n 's/^### gpuRestart-after //p' "$ALL" | head -1)
    local gn; gn=$(sed -n 's/^### gpuRestart-new-by-mtime //p' "$ALL" | head -1)
    if [[ -n $gn ]]; then
        if (( gn > 0 )); then verdict="$gn NEW report(s) written during the run (by mtime; one is coincident, several are a rate change - rule 30)"; else verdict="no new report (by mtime)"; fi
        print -r -- "gpuRestart: $verdict   [directory count before=$gb after=$ga is capped, not evidence]"
        # 0.0.264 : the BOOT-attributed number is the honest denominator.
        # A report written after a previous run's capture closed still lands in this run's
        # mtime window and inflates it - that is exactly how r98 reported "two in fifteen
        # minutes" when one of the two belonged to r97's boot.
        local gt bt
        gt=$(sed -n 's/^### gpuRestart-this-boot //p' "$ALL" | head -1)
        bt=$(sed -n 's/^### kern-boottime //p' "$ALL" | head -1)
        print -r -- "gpuRestart THIS BOOT: ${gt:-MISSING — no '### gpuRestart-this-boot' marker} (kern.boottime ${bt:-unknown}; attributed by boot, not by mtime — reports older than that boot belong to a PREVIOUS boot and are NOT this run's)"
    elif [[ -n $gb && -n $ga ]]; then
        if (( ga > gb )); then verdict="$(( ga - gb )) NEW report(s) — coincident, not attributed"; else verdict="no new report"; fi
        print -r -- "gpuRestart: before=$gb after=$ga -> $verdict (count only: capped directory, check mtimes)"
    else
        print -r -- "gpuRestart: MISSING — no '### gpuRestart-before/after' markers in health.txt"
    fi
    # Prefer health.txt's own `ls -lt` block: the transcript may quote an older
    # listing from earlier in the run, and "newest" must mean newest.
    local newest
    [[ -f $HLF ]] && newest=$(sed -n '/^### newest DiagnosticReports/,/^### /p' "$HLF" \
                              | grep -oE '[A-Za-z0-9_.+-]+\.(gpuRestart|panic|ips|diag|spin)' | head -1)
    [[ -n ${newest:-} ]] || newest=$(grep -oE '[A-Za-z0-9_.+-]+\.(gpuRestart|panic|ips|diag|spin)' "$ALL" | head -1)
    print -r -- "newest DiagnosticReports file: ${newest:-MISSING — no *.gpuRestart/*.panic/*.ips filename in the capture}"
    } > "$SUM" 2>&1

    rm -f "$ALL"
    [[ -n $DSTREAM ]] && rm -f "$DSTREAM"
    cat "$SUM"
}

# Summary-only mode: no hardware, no run dir created. Used to test the extractor
# against previously captured logs.
if [[ -n ${ACCEL_RUN_SUMMARY_ONLY:-} ]]; then
    summarize "$ACCEL_RUN_SUMMARY_ONLY"
    exit $?
fi

# ------------------------------------------------------------------ args ----
while [[ ${1:-} == -* ]]; do
    case "$1" in
        --force)   FORCE=1; shift ;;
        --dry-run|-n) DRY=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) print -r -- "unknown option $1"; exit 2 ;;
    esac
done
RUN=${1:-}
[[ -n $RUN ]] || { print -r -- "usage: accel-run.sh [--force] [--dry-run] <run-name> [step ...]"; exit 2 }
shift
typeset -a STEPS
if (( $# )); then STEPS=("$@"); else STEPS=(synctables setvspace pm4powerup kiqenable gfxmap pm4powerup sleep5); fi

# `blit` / `blit-<va>` expands in place to the blit phase (the project notes):
# the takeover verbs only after the client has submitted, every read while it lives,
# vmstate before faultclear so the dispatch's latched fault is not discarded unread.
typeset -a EXPANDED PARK
PARK=(reg-0x21a0 reg-0x219e reg-0x21a1 reg-0x21a2 reg-0xc0ce reg-0xc0c0 reg-0x2005 reg-0x2006
      reg-0x200e reg-0x200f reg-0x2e00 reg-0x2e01 reg-0x2316)
for s in "${STEPS[@]}"; do
    case "$s" in
        parkreads) EXPANDED+=("${PARK[@]}") ;;
        ringreads) EXPANDED+=(reg-0x2c87 reg-0x2c88 reg-0xc268 reg-0xc269 reg-0xc26a reg-0xc26b reg-0xc444 reg-0xc445
                              reg-0xc446 reg-0xc447) ;;
        diagreads) EXPANDED+=(vmpage-0x400004000 counters reg-0x219e reg-0x2005 reg-0x2006 reg-0x200e
                              reg-0x200f reg-0x21a0) ;;
        blit|blit-0x[0-9a-fA-F]*)
            bva=0x400017b00; [[ $s == blit-* ]] && bva=${s#blit-}
            EXPANDED+=(counters blit2start xlatregs neuterpoll sdmamap sleep2 vmstate faultclear sleep3
                       "vmib-$bva" sdmastate reg-0x21c0 reg-0x3054 reg-0x2830 "${PARK[@]}" vmstate pagecopy counters
                       blit2wait pagecopy) ;;
        # tri render phases . The render IB is rewritten by content BEFORE sdmamap
        # (rule 65); `rendergate` skips sdmamap unless that rewrite landed, so Apple's untranslated
        # render frame is never fetched. triclear = E2 (translated clear), tridraw = E3 (triangle).
        triclear|tridraw)
            mo=clear; [[ $s == tridraw ]] && mo=tri
            EXPANDED+=(counters "tristart-$mo" xlatregs renderxlat rendergate neuterpoll sdmamap sleep2 gfxstate
                       vmstate faultclear sleep3 reg-0x21c0 reg-0x3054 reg-0x2830 "${PARK[@]}" vmstate chanstate
                       counters sleep30 chanstate counters triwait) ;;
        *)  EXPANDED+=("$s") ;;
    esac
done
STEPS=("${EXPANDED[@]}")
# 0.0.267 : prefire-<step> tokens run before `fire`.
typeset -a PREFIRE MAIN
for s in "${STEPS[@]}"; do
    if [[ $s == prefire-* ]]; then PREFIRE+=("${s#prefire-}"); else MAIN+=("$s"); fi
done
STEPS=("${MAIN[@]}")

# ---------------------------------------------------- THE ARM ORDERING CHECK ----
# an earlier analysis. arm12's list ended `... gfxneuter-14 gfxcapture-1 capstream-start sleep300 wskill
# wscompose-arm12 ...` with the arm thrown OUT OF BAND inside the sleep; the sleep expired, `wskill` started 8 ms
# later, the arm landed 84 s after it, and 731 of 896 gate attempts were judged at ARM LEVEL 1. This runs BEFORE
# anything touches hardware - before the readiness read, before `fire`, and in --dry-run too - so a list that breaks
# the invariant costs no boot. It refuses, it never reorders: the operator owns the recipe.
typeset -i _ai=0 _wi=0 _si=0 _rawarm=0 _i=0
for s in "${STEPS[@]}"; do
    _i=$(( _i + 1 ))
    # section 661: `wswitness` and `iopparse-<n>` restart WindowServer exactly as `wskill` does.
    if (( _wi == 0 )) && [[ $s == (wskill|wswitness|iopparse-[0-9]*) ]]; then _wi=$_i; fi
    [[ $s == (armgate|armgate-[0-9]*|commitarm) ]] && _ai=$_i
    [[ $s == gfxneuter-260 ]] && _rawarm=$_i
    if (( _wi == 0 )) && [[ $s == sleep[0-9]* ]] && (( ${s#sleep} >= 60 )); then _si=$_i; fi
done
if (( _wi )); then
    if (( _ai && _ai > _wi )); then
        print -r -- "ABORT: the step list puts the arm gate (step $_ai) AFTER the WindowServer restart (step $_wi)."
        print -r -- "       an earlier analysis: the arm must be LIVE BEFORE the window opens. Move \`armgate\`/\`commitarm\` before \`wskill\`."
        exit 2
    fi
    if (( _rawarm && _rawarm > _wi )); then
        print -r -- "ABORT: \`gfxneuter-260\` (the COMMIT arm) is at step $_rawarm, AFTER the WindowServer restart at step $_wi."
        exit 2
    fi
    if (( _rawarm && ! _ai )); then
        print -r -- "ABORT: the list arms with a bare \`gfxneuter-260\` and carries no \`armgate\`/\`commitarm\`, so nothing"
        print -r -- "       reads the arm back before the kill. Use \`commitarm\` (it reads the checklist first and reads the"
        print -r -- "       arm back), or keep \`gfxneuter-260\` and add \`armgate\` immediately before \`wskill\`."
        exit 2
    fi
    # A long sleep before the restart IS an out-of-band arm window - that is the only thing it has ever been in an
    # armed recipe (arm11, arm12: `sleep300 wskill`), and no judge-only recipe has one (hp7, hp9 go straight to
    # `wskill`). If one is there, the gate must sit BETWEEN it and the kill, or the kill can outrun the arm again.
    if (( _si && _si < _wi && _ai < _si )); then
        print -r -- "ABORT: step $_si is a long sleep (\`${STEPS[$_si]}\`) immediately before the WindowServer restart at step $_wi,"
        print -r -- "       with no \`armgate\`/\`commitarm\` between them. In an armed recipe that sleep IS the out-of-band arm"
        print -r -- "       window, and when the batch overruns it the kill fires anyway - that is exactly what voided arm12"
        print -r -- "       (an earlier analysis: the arm landed 84 s AFTER wskill, not-armed 731 of 896 gate attempts, COMMITTED 0)."
        print -r -- "       Replace it with \`armgate-${STEPS[$_si]#sleep}\`, which waits for the arm and returns the instant it lands."
        exit 2
    fi
    if (( ! _ai )); then
        print -r -- "### NOTE: this recipe restarts WindowServer (step $_wi \`${STEPS[$_wi]}\`) and declares NO COMMIT arm."
        print -r -- "###       That is correct for a judge-only run. If you intend to arm, the list MUST carry \`armgate\` before it."
    fi
fi

BLIT_HOLD=3         # seconds after blit2 reports `committed` before the takeover verbs (section 315)
BLIT_COMMIT_MAX=20  # seconds to wait for blit2.state to say `committed`
BLIT_WAIT_MAX=330   # blit2 polls its command buffer 300 s, then prints DATA and exits
BLIT2=$T/blit2

# blit2start: the Metal client, through the staged probe-wrap.sh (it backgrounds blit2 with
# stdout in /tmp/trace-probe.txt and returns after BLIT_HOLD seconds, blit2 still running).
blit2_start() {
    print -r -- "### blit2.state before: $(stat -f '%Sm' "$T/blit2.state" 2>&1) : $(cat "$T/blit2.state" 2>&1)"
    if [[ ! -x $T/probe-wrap.sh || ! -x $BLIT2 ]]; then
        print -r -- "ABORT blit2start: $T/probe-wrap.sh or $BLIT2 is missing or not executable"
        return 1
    fi
    local running; running=$(pgrep -x blit2 | tr '\n' ' ')
    if [[ -n ${running// /} ]]; then
        print -r -- "ABORT blit2start: a blit2 is already running (pid $running) - one Metal probe per boot (rule 21)"
        return 1
    fi
    local hold=${1:-$BLIT_HOLD} t0 waited=1 st="" mt=0
    t0=$(date +%s)
    print -r -- "### $T/probe-wrap.sh $BLIT2 1   (blit2 stdout+stderr -> /tmp/trace-probe.txt); launch at epoch $t0"
    "$T/probe-wrap.sh" "$BLIT2" 1
    local rc=$?
    while (( waited < BLIT_COMMIT_MAX )); do
        st=$(cat "$T/blit2.state" 2>&1); mt=$(stat -f '%m' "$T/blit2.state" 2>/dev/null || print 0)
        [[ $st == committed* && $mt -ge $t0 ]] && break
        sleep 1; waited=$((waited+1))
    done
    print -r -- "### blit2 pid(s): $(pgrep -x blit2 | tr '\n' ' ')"
    print -r -- "### blit2.state now: $(stat -f '%Sm' "$T/blit2.state" 2>&1) : $st (polled ${waited}s, max ${BLIT_COMMIT_MAX}s)"
    sleep "$hold"
    print -r -- "### takeover starts $(( $(date +%s) - t0 ))s after the blit2 launch (hold ${hold}s after committed)"
    return $rc
}

# blit2wait: bounded wait for blit2 to finish on its own. It is never killed (rule 20).
blit2_wait() {
    local t=0 p=""
    while (( t < BLIT_WAIT_MAX )); do
        p=$(pgrep -x blit2 | tr '\n' ' ')
        [[ -z ${p// /} ]] && break
        (( t % 60 == 0 )) && print -r -- "### t+${t}s blit2 still running (pid $p); state: $(cat "$T/blit2.state" 2>&1)"
        sleep 5; t=$((t+5))
    done
    if [[ -z ${p// /} ]]; then
        print -r -- "### blit2 EXITED within ${t}s of the wait"
    else
        print -r -- "### blit2 STILL RUNNING after ${BLIT_WAIT_MAX}s (pid $p) - left running; reboot before the next measurement"
    fi
    print -r -- "### blit2.state final: $(stat -f '%Sm' "$T/blit2.state" 2>&1) : $(cat "$T/blit2.state" 2>&1)"
    print -r -- "### /tmp/trace-probe.txt (blit2 stdout), $(wc -l < /tmp/trace-probe.txt 2>&1 | tr -d ' ') line(s):"
    cat /tmp/trace-probe.txt 2>&1
    cp /tmp/trace-probe.txt "$RUNS/${RUN:-unknown}/blit2-output.txt" 2>&1
    return 0
}

# tri_start <clear|tri> [hold]: the milestone-2 render client (notes/MILESTONE2-DESIGN.md Q4). Same
# shape as blit2_start: launch it, wait for tri.state `committed`, hold, return with tri STILL ALIVE
# (rule 20). One Metal probe per boot (rule 21), so it refuses if a blit2 or tri is already running.
# 0.0.212: `drawinj` = mode draw with --inject ~/navi48-staging/tri-inject (refused without its manifest);
# no other mode injects (before 0.0.212 every mode injected whenever the manifest existed).
TRI=$T/tri
tri_start() {
    local mode=${1:-clear} hold=${2:-$BLIT_HOLD}
    print -r -- "### tri.state before: $(stat -f '%Sm' "$T/tri.state" 2>&1) : $(cat "$T/tri.state" 2>&1)"
    [[ -x $TRI ]] || { print -r -- "ABORT tristart: $TRI is missing or not executable"; return 1; }
    local running; running=$(pgrep -x tri; pgrep -x blit2)
    if [[ -n ${running//[$'\n' ]/} ]]; then
        print -r -- "ABORT tristart: a Metal probe is already running (pid $running) - one per boot (rule 21)"
        return 1
    fi
    local -a inj ringArg; inj=(); ringArg=()
    if [[ $mode == suitenw* ]]; then   # 0.0.235 : the suite committed WITHOUT a blocking wait.
        # tri --nowait registers an addCompletedHandler before commit and then waits for THAT, never for
        # cb.status - polling cb.status is itself a wait path on this stack and would hide the very thing
        # being measured. Only Apple's asynchronous retire can fire the handler, and on our stack only the
        # kext's IH -> checkTimestamps bridge can drive that retire (an earlier analysis addendum 2).
        local cases=${mode#suitenw}; cases=${cases#.}; cases=${cases//./,}
        [[ -f $T/tri-suite/manifest.json ]] || { print -r -- "ABORT tristart-suitenw: no $T/tri-suite/manifest.json"; return 1; }
        ringArg=(--ring --nowait); inj=(--inject "$T/tri-suite"); [[ -n $cases ]] && inj+=(--cases "$cases"); mode=suite
    elif [[ $mode == suite* ]]; then   # 0.0.224 : the suite, one command buffer, one pass per case, + ring
        local cases=${mode#suite}; cases=${cases#.}; cases=${cases//./,}
        [[ -f $T/tri-suite/manifest.json ]] || { print -r -- "ABORT tristart-suite: no $T/tri-suite/manifest.json"; return 1; }
        ringArg=(--ring); inj=(--inject "$T/tri-suite"); [[ -n $cases ]] && inj+=(--cases "$cases"); mode=suite
    fi
    if [[ $mode == drawinjring || $mode == drawinstrring ]]; then   # 0.0.217 : + the NGG ring buffer
        ringArg=(--ring); mode=${mode%ring}
    fi
    if [[ $mode == drawinj ]]; then
        [[ -f $T/tri-inject/manifest.json ]] || { print -r -- "ABORT tristart-drawinj: no $T/tri-inject/manifest.json"; return 1; }
        mode=draw; inj=(--inject "$T/tri-inject")
    elif [[ $mode == drawinstr ]]; then   # 0.0.216 : the FULL instrument VS + m2_tri_fs
        [[ -f $T/tri-inject-instr/manifest.json ]] || { print -r -- "ABORT tristart-drawinstr: no $T/tri-inject-instr/manifest.json"; return 1; }
        mode=draw; inj=(--inject "$T/tri-inject-instr")
    fi
    rm -f "$T/tri-target.ppm" "$T/tri-target.bgra" "$T/tri-vb-page.bin" "$T/tri-ring-attr.bin" "$T/tri-ring-pos.bin" \
          "$T/tri-ring-prim.bin" "$T"/tri-case-*.ppm "$T"/tri-case-*.bgra 2>&1   # 0.0.215/0.0.217/0.0.224: no stale readback images
    local t0; t0=$(date +%s)
    print -r -- "### launch: nohup $TRI $mode ${inj[*]} ${ringArg[*]} > /tmp/trace-tri.txt 2>&1 &   at epoch $t0"
    ( nohup "$TRI" $mode "${inj[@]}" "${ringArg[@]}" > /tmp/trace-tri.txt 2>&1 & )
    local waited=1 st="" mt=0
    while (( waited < BLIT_COMMIT_MAX )); do
        st=$(cat "$T/tri.state" 2>&1); mt=$(stat -f '%m' "$T/tri.state" 2>/dev/null || print 0)
        [[ $st == committed* && $mt -ge $t0 ]] && break
        sleep 1; waited=$((waited+1))
    done
    print -r -- "### tri pid(s): $(pgrep -x tri | tr '\n' ' ')"
    print -r -- "### tri.state now: $(stat -f '%Sm' "$T/tri.state" 2>&1) : $st (polled ${waited}s, mode $mode${inj:+, INJECTING}${ringArg:+, RING})"
    (( ${#ringArg} )) && print -r -- "### ring: $(grep -m1 -E '^\[tri\] RING buffer gpuAddress' /tmp/trace-tri.txt 2>&1)"
    sleep "$hold"
    print -r -- "### takeover starts $(( $(date +%s) - t0 ))s after the tri launch (hold ${hold}s after committed)"
    return 0
}

# tri_dump <clear|draw|drawinj> (0.0.212, an earlier analysis): tri --dump, which never commits. Output in
# <run>/tridump-<v>/ (fetched with the run). Bounded wait for tri to exit on its own; never killed.
tri_dump() {
    local v=${1:-clear} mode=${1:-clear}
    [[ -x $TRI ]] || { print -r -- "ABORT tridump: $TRI is missing or not executable"; return 1; }
    local running; running=$(pgrep -x tri; pgrep -x blit2)
    if [[ -n ${running//[$'\n' ]/} ]]; then
        print -r -- "ABORT tridump: a Metal probe is already running (pid $running)"; return 1
    fi
    local -a inj; inj=()
    if [[ $v == drawinj ]]; then
        [[ -f $T/tri-inject/manifest.json ]] || { print -r -- "ABORT tridump-drawinj: no $T/tri-inject/manifest.json"; return 1; }
        mode=draw; inj=(--inject "$T/tri-inject")
    elif [[ $v == suite* ]]; then   # 0.0.224 : the suite's encoders, never committed
        local cases=${v#suite}; cases=${cases#.}; cases=${cases//./,}
        [[ -f $T/tri-suite/manifest.json ]] || { print -r -- "ABORT tridump-suite: no $T/tri-suite/manifest.json"; return 1; }
        mode=suite; inj=(--ring --inject "$T/tri-suite"); [[ -n $cases ]] && inj+=(--cases "$cases"); v=suite
    fi
    local out=$RUNS/${RUN:-unknown}/tridump-$v t=0
    mkdir -p "$out"
    print -r -- "### tridump: nohup $TRI $mode ${inj[*]} --dump $out > $out/tri-output.txt 2>&1 &"
    ( nohup "$TRI" $mode "${inj[@]}" --dump "$out" > "$out/tri-output.txt" 2>&1 & )
    sleep 1
    local tmax=90; [[ $v == suite ]] && tmax=240   # 0.0.224: the suite builds several pipelines before its dump
    while (( t < tmax )) && pgrep -x tri > /dev/null; do sleep 1; t=$((t+1)); done
    if pgrep -x tri > /dev/null; then
        print -r -- "### tridump-$v STILL RUNNING after $tmax s (pid $(pgrep -x tri | tr '\n' ' ')) - NOT killed; state: $(cat "$T/tri.state" 2>&1)"
        return 1
    fi
    print -r -- "### tridump-$v EXITED after ~$((t+1)) s; state: $(cat "$T/tri.state" 2>&1); $(ls "$out" | wc -l | tr -d ' ') file(s) in $out"
    cat "$out/tri-output.txt" 2>&1
    return 0
}

# 0.0.334 : `triwait-<s>` bounds the wait. In run uc1 the tri WARM-UP FRAME hung and
# tri_wait sat on it for the full BLIT_WAIT_MAX (transcript: `triwait END rc=0 ms=300722`, against 42 ms in
# gate1), so the boot reached `GPU restart previously failed` before the WindowServer restart and the whole
# run was VOID. A warm-up whose working case is tens of milliseconds must not be allowed to consume a boot.
# On exceeding the bound this sets LIVE_ABORT, which SKIPS wskill/wswitness, m4adopt, agdc-1 and every later
# client step: restarting the compositor on a GPU whose warm-up frame just hung is strictly worse than not
# running at all, and the wind-down steps (pipeguardread, ucproberead, kextlog-end) still run and still
# capture. Bare `triwait` keeps the old unbounded behaviour.
tri_wait() {
    local t=0 p="" max=${1:-$BLIT_WAIT_MAX}
    while (( t < max )); do
        p=$(pgrep -x tri | tr '\n' ' ')
        [[ -z ${p// /} ]] && break
        (( t % 60 == 0 )) && print -r -- "### t+${t}s tri still running (pid $p); state: $(cat "$T/tri.state" 2>&1)"
        sleep 5; t=$((t+5))
    done
    if [[ -z ${p// /} ]]; then print -r -- "### tri EXITED within ${t}s of the wait (bound ${max}s)"
    else
        LIVE_ABORT=1
        print -r -- "### triwait-bound: tri STILL RUNNING after ${max}s (pid $p) - left running (never killed)."
        print -r -- "### TRIWAIT BOUND EXCEEDED - the warm-up frame did not retire; LIVE_ABORT set, so wskill/wswitness and every later client step are SKIPPED . Reboot before the next measurement."
    fi
    print -r -- "### tri.state final: $(stat -f '%Sm' "$T/tri.state" 2>&1) : $(cat "$T/tri.state" 2>&1)"
    print -r -- "### /tmp/trace-tri.txt (tri stdout), $(wc -l < /tmp/trace-tri.txt 2>&1 | tr -d ' ') line(s):"
    cat /tmp/trace-tri.txt 2>&1
    cp /tmp/trace-tri.txt "$RUNS/${RUN:-unknown}/tri-output.txt" 2>&1
    # 0.0.215: tri's readback images (the 64x64 target as PPM and raw BGRA, and the vertex-buffer page), written
    # after its TRI: line; tri_start deletes stale copies, so a missing file here means tri never wrote one.
    local f
    for f in tri-target.ppm tri-target.bgra tri-vb-page.bin tri-ring-attr.bin tri-ring-pos.bin tri-ring-prim.bin \
             $(cd "$T" 2>/dev/null && ls tri-case-*.ppm tri-case-*.bgra 2>/dev/null); do   # 0.0.224: the suite's per-case readbacks
        if [[ -f $T/$f ]]; then cp "$T/$f" "$RUNS/${RUN:-unknown}/$f" 2>&1; print -r -- "### $f: $(wc -c < "$T/$f" | tr -d ' ') bytes, copied"
        else print -r -- "### $f: MISSING (tri wrote none)"; fi
    done
    return 0
}

# renderdrawring / renderdrawringcheck (0.0.217, an earlier analysis): the draw policy plus the GE rings at the base tri
# printed (64 KiB aligned) and RSRC3_GS: `accel renderxlat <base|0x200|mode>`. No base in tri's output = nothing sent.
tri_ring_base() {
    local l; l=$(grep -m1 -E '^\[tri\] RING buffer gpuAddress' /tmp/trace-tri.txt 2>&1)
    print -r -- "$l" | grep -oE 'base 0x[0-9a-f]+' | head -1 | sed 's/^base //'
}
render_draw_ring() {
    local mode=$1 flags=${2:-0x200} base; base=$(tri_ring_base)   # 0.0.219: flags 0x600 = RSRC3_GS | preamble delta (0x800 retired)
    print -r -- "### ring base from tri: ${base:-MISSING}"
    if [[ -z $base ]] || (( base == 0 || (base & 0xffff) != 0 )); then
        print -r -- "ABORT renderdrawring: no 64 KiB-aligned ring base in /tmp/trace-tri.txt - nothing sent"; return 1
    fi
    local arg; arg=$(printf '0x%x' $(( base | flags | mode )))
    print -r -- "+ sudo $N48 accel renderxlat $arg   (ring base $base | flags $flags | mode $mode)"
    sudo "$N48" accel renderxlat "$arg"
}
# renderdrawsuite[check][-0xFLAGS] (0.0.224, an earlier analysis): accel renderxlat <ring base | flags | passes << 4 | 6 or 7>, the
# suite's draws translated segment by segment. Flags default 0x1600 (CU + preamble + raster/CB, the milestone-2 set); allowed bits
# 0x200 CU, 0x400 preamble, 0x1000 raster/CB, 0x2000 Apple's IDX_FORMAT, 0x4000 Apple's GS_OUT_PRIM_TYPE, 0.0.226: 0x8000 the ring-offsets
# table tri wrote past the rings into SPI_SHADER_PGM_LO/HI_GS . The pass count is tri's
# `[tri] SUITE <n> case(s)` line; no ring base or no count = nothing sent. 0.0.232: modes 8/9 (renderdrawsuitedesc[check]) are
# modes 6/7 plus the gfx10 -> gfx12 image and sampler descriptor translation for every segment whose fragment stage samples
# .
render_draw_suite() {
    local mlo=$1 fl=${2#-} base n
    [[ -z $fl ]] && fl=0x1600
    base=$(tri_ring_base)
    n=$(grep -m1 -oE '^\[tri\] SUITE [0-9]+ case' /tmp/trace-tri.txt 2>&1 | grep -oE '[0-9]+' | head -1)
    print -r -- "### suite: ring base ${base:-MISSING}, passes ${n:-MISSING}, flags $fl, mode $mlo"
    if [[ -z $base || -z $n ]] || (( base == 0 || (base & 0xffff) != 0 || n < 1 || n > 8 )); then
        print -r -- "ABORT renderdrawsuite: no 64 KiB-aligned ring base or no pass count in /tmp/trace-tri.txt - nothing sent"; return 1
    fi
    if (( (fl & ~0xF600) != 0 )); then
        print -r -- "ABORT renderdrawsuite: flags $fl outside 0xF600 - nothing sent"; return 1
    fi
    local arg; arg=$(printf '0x%x' $(( base | fl | (n << 4) | mlo )))
    print -r -- "+ sudo $N48 accel renderxlat $arg   (ring base $base | flags $fl | passes $n << 4 | mode $mlo)"
    sudo "$N48" accel renderxlat "$arg"
}
# renderdrawsuiteown[check][-0xFLAGS] (0.0.255, an earlier analysis): the same as renderdrawsuite, but with bit 0x100
# set and NO ring base. The kext drives the geometry rings from its OWN mapping - the region ringmap reserved and
# the root write pointed Apple's page table at - so the translate no longer depends on the client having allocated
# a ring buffer and printed its address. Deliberately does NOT abort on a missing ring base: that abort IS the
# client dependency being removed here. The pass count still comes from tri's SUITE line, because the client still
# issues the draws; only the ring memory stops being its job.
render_draw_suite_own() {
    local mlo=$1 fl=${2#-} n
    [[ -z $fl ]] && fl=0x1600
    n=$(grep -m1 -oE '^\[tri\] SUITE [0-9]+ case' /tmp/trace-tri.txt 2>&1 | grep -oE '[0-9]+' | head -1)
    print -r -- "### suite (KEXT-OWNED rings, no client base): passes ${n:-MISSING}, flags $fl, mode $mlo"
    if [[ -z $n ]] || (( n < 1 || n > 8 )); then
        print -r -- "ABORT renderdrawsuiteown: no pass count in /tmp/trace-tri.txt - nothing sent"; return 1
    fi
    if (( (fl & ~0xF600) != 0 )); then
        print -r -- "ABORT renderdrawsuiteown: flags $fl outside 0xF600 - nothing sent"; return 1
    fi
    local arg; arg=$(printf '0x%x' $(( 0x100 | fl | (n << 4) | mlo )))
    print -r -- "+ sudo $N48 accel renderxlat $arg   (OWN mapping 0x100 | flags $fl | passes $n << 4 | mode $mlo)"
    sudo "$N48" accel renderxlat "$arg"
}
ring_pages() {
    local base; base=$(tri_ring_base)
    [[ -n $base ]] || { print -r -- "ABORT ringpages: no ring base in /tmp/trace-tri.txt"; return 1; }
    local off h
    for off in 0 0x580000 0x980000; do
        h=$(printf '0x%x' $(( base + off )))
        print -r -- "+ sudo $N48 accel vmpage $h   (ring base $base + $off)"
        sudo "$N48" accel vmpage "$h"
    done
}

# rendergate : open only when the last `renderxlat` step printed status 1 (written)
# or 2 (already written). Closed, the later `sdmamap` is SKIPPED: Apple's SDMA frames then never run,
# the GFX frame naming the render IB is never written, and the CP never fetches untranslated render state.
GATE_CLOSED=0
render_gate() {
    local l; l=$(grep -E '^  renderxlat status +: ' "$TR" | tail -1)
    print -r -- "### render gate: last renderxlat result: ${l:-NONE}"
    if [[ $l == *": 1 ("* || $l == *": 2 ("* ]]; then
        print -r -- "### render gate OPEN - the render IB was rewritten before sdmamap"
    else
        GATE_CLOSED=1
        print -r -- "### render gate CLOSED - sdmamap will be SKIPPED; the CP never fetches Apple's render frame"
    fi
    return 0
}
skip_step() { print -r -- "### SKIPPED $1: render gate closed (no rewritten render IB)"; return 0; }

# token -> argv. `logreset` is a TOP-LEVEL navi48test verb, not an accel verb —
# running it as `accel logreset` is refused as an unknown verb. `sleepN` is a bare
# sleep. Everything else is `accel <token>`.
typeset -a reply
# ------------------------------------------------------------------ P2 ----
# 0.0.267 . WindowServer's handling of our accelerator's ARRIVAL, captured live.
# CoreDisplay logs through os_log_create("com.apple.CoreDisplay", "default"): Logger::Info at
# OS_LOG_TYPE_INFO, Logger::Warning at DEFAULT, both "%{public}s" . Info is never
# persisted, so `log show` after the fact cannot see "GPUWrangler Add event: Begin"; only a live
# stream at --level info can. zsh has a `log` builtin, so the binary is always /usr/bin/log.
P2_WPRED='process == "WindowServer" OR subsystem == "com.apple.CoreDisplay" OR (process == "logger" AND eventMessage CONTAINS "N48-P2-MARK")'
P2_KPRED='(process == "kernel" AND (sender == "AppleGPUWrangler" OR sender == "IOAcceleratorFamily2" OR sender == "IOGraphicsFamily")) OR (process == "logger" AND eventMessage CONTAINS "N48-P2-MARK")'
P2_GPU_PAT='GPUWrangler|pre-existing GPU|Missing GPU object|AcquireMetal|MetalDevice|[Aa]ccelerator|IOAccel|N48-P2-MARK'

p2_logmark() {
    local tag=${1:-mark}
    /usr/bin/logger -t n48p2 "N48-P2-MARK $RUN $tag $(date +%T)"
    print -r -- "logger marker sent: N48-P2-MARK $RUN $tag"
}

# start one stream: <file> <pidfile> <predicate>. `exec` keeps the pid the pidfile names.
p2_stream() {
    sudo /bin/sh -c 'echo $$ > "$1"; exec /usr/bin/log stream --level info --predicate "$2"' \
        p2stream "$2" "$3" > "$1" 2>&1 < /dev/null &   # stdin only; the stream's OUTPUT is the file
}

p2_stop_streams() {
    local pf pid k
    for pf in "$D/.wslog.pid" "$D/.kernlog.pid"; do
        [[ -f $pf ]] || continue
        pid=$(cat "$pf" 2>/dev/null)
        if [[ -n $pid ]] && sudo kill -0 "$pid" 2>/dev/null; then
            sudo kill -INT "$pid"
            for k in 1 2 3 4 5 6 7 8 9 10; do sudo kill -0 "$pid" 2>/dev/null || break; sleep 1; done
            sudo kill -0 "$pid" 2>/dev/null && { print -r -- "stream pid $pid ignored SIGINT for 10 s - SIGTERM"; sudo kill -TERM "$pid"; }
            print -r -- "stopped stream pid $pid ($pf:t)"
        fi
        rm -f "$pf"
    done
}

# 0.0.268 : TEST A's broader streams.
A_WPRED='process == "WindowServer" OR process == "loginwindow" OR subsystem == "com.apple.CoreDisplay" OR subsystem == "com.apple.Metal" OR subsystem BEGINSWITH "com.apple.SkyLight" OR (process == "logger" AND eventMessage CONTAINS "N48-P2-MARK")'
# 0.0.334 : the RDNA4FB half is ADDITIVE. Every FBLOG line is prefixed "AMDRDNA4FB: ", so match on
# the message text rather than on a sender name we have never verified. This is what tells a VBL registration our
# framebuffer ACCEPTED ("vbl: emulated VBL armed, period <n> us", framebuffer.cpp:1546) apart from one it never saw.
A_KPRED='(process == "kernel" AND (sender == "AMDRadeonX6000" OR sender == "IOAcceleratorFamily2" OR sender == "AppleGPUWrangler" OR sender == "IOGraphicsFamily" OR eventMessage CONTAINS "AMDRDNA4FB")) OR (process == "logger" AND eventMessage CONTAINS "N48-P2-MARK")'

# tmo <seconds> <cmd...>: run under a perl alarm so a blocked probe cannot hang the harness. A process in
# uninterruptible wait ignores the signal, which the caller then sees as a hang - rc 142 means it fired.
tmo() { local t=$1; shift; perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' "$t" "$@" }

p2_log_start() {
    local wp=$P2_WPRED kp=$P2_KPRED
    [[ ${1:-} == a ]] && { wp=$A_WPRED; kp=$A_KPRED; }
    [[ -f $D/.wslog.pid || -f $D/.kernlog.pid ]] && { print -r -- "wslogstart: streams already started in this run"; return 1; }
    p2_stream "$D/wslog.txt" "$D/.wslog.pid" "$wp"
    p2_stream "$D/kernlog.txt" "$D/.kernlog.pid" "$kp"
    sleep 3
    print -r -- "wslog predicate  : $wp"
    print -r -- "kernlog predicate: $kp"
    print -r -- "stream pids      : wslog $(cat "$D/.wslog.pid" 2>/dev/null) kernlog $(cat "$D/.kernlog.pid" 2>/dev/null)"
    p2_logmark start
    local k nw=0 nk=0
    for k in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        nw=$(grep -c "N48-P2-MARK $RUN start" "$D/wslog.txt" 2>/dev/null); nk=$(grep -c "N48-P2-MARK $RUN start" "$D/kernlog.txt" 2>/dev/null)
        (( nw > 0 && nk > 0 )) && break
    done
    print -r -- "positive control (start marker seen): wslog $nw, kernlog $nk after ${k}s"
    if (( nw == 0 || nk == 0 )); then
        print -r -- "wslogstart: a stream is NOT live - an empty capture would be a claim about the capture, not about WindowServer. FAILING."
        head -5 "$D/wslog.txt" "$D/kernlog.txt" 2>&1
        p2_stop_streams
        return 1
    fi
    return 0
}

p2_log_stop() {
    p2_logmark end
    sleep 3
    p2_stop_streams
    local f
    for f in wslog kernlog; do
        print -r -- "P2 $f: $(wc -l < "$D/$f.txt" | tr -d ' ') lines; markers start=$(grep -c "N48-P2-MARK $RUN start" "$D/$f.txt") end=$(grep -c "N48-P2-MARK $RUN end" "$D/$f.txt")"
    done
    grep -nE "$P2_GPU_PAT" "$D/wslog.txt" | grep -v ':Filtering the log data' > "$D/wslog-gpu.txt"
    grep -nE "$P2_GPU_PAT" "$D/kernlog.txt" | grep -v ':Filtering the log data' > "$D/kernlog-gpu.txt"
    print -r -- "P2 CoreDisplay [INFO] lines: $(grep -c '\[INFO\] - ' "$D/wslog.txt")   [WARN] lines: $(grep -c '\[WARN\] - ' "$D/wslog.txt")   (WindowServer lines of ANY kind: $(grep -c ' WindowServer: ' "$D/wslog.txt"); CoreDisplay-subsystem lines: $(grep -c 'com.apple.CoreDisplay:' "$D/wslog.txt"))"
    print -r -- "P2 'GPUWrangler Add event: Begin': $(grep -c 'GPUWrangler Add event: Begin' "$D/wslog.txt")   'Add event: Done': $(grep -c 'GPUWrangler Add event: Done' "$D/wslog.txt")   'Found pre-existing GPU object when adding GPU': $(grep -c 'Found pre-existing GPU object' "$D/wslog.txt")"
    print -r -- "P2 other GPUWrangler events (Remove/Eject/Terminated/Unknown): $(grep -cE 'GPUWrangler (Remove|Eject|EjectFinalize|EjectFinalized|EjectCanel|Terminated) event|GPUWrangler Process Unknown event' "$D/wslog.txt")   'Failed to create MetalDevice': $(grep -c 'Failed to create MetalDevice' "$D/wslog.txt")"
    print -r -- "P2 GPU-pattern lines: wslog-gpu.txt $(wc -l < "$D/wslog-gpu.txt" | tr -d ' '), kernlog-gpu.txt $(wc -l < "$D/kernlog-gpu.txt" | tr -d ' ') (pattern: $P2_GPU_PAT)"
    head -40 "$D/wslog-gpu.txt" | cut -c1-240
    return 0
}

p2_ws_state() {
    local tag=${1:-state}
    print -r -- "console user: $(stat -f %Su /dev/console 2>&1)"
    ps -axo pid,ppid,user,lstart,etime,stat,comm > "$D/ps-$tag.txt" 2>&1
    grep -E 'WindowServer|loginwindow' "$D/ps-$tag.txt"
    ps -axo stat,pid,etime,comm > "$D/ps-stat-$tag.txt" 2>&1
    print -r -- "U-state processes: $(awk 'NR > 1 && $1 ~ /U/' "$D/ps-stat-$tag.txt" | wc -l | tr -d ' ')"
    awk 'NR > 1 && $1 ~ /U/' "$D/ps-stat-$tag.txt" | head -12
    tmo 20 ioreg -w0 -r -c IOAccelerator -d 1 > "$D/ioaccel-$tag.txt" 2>&1
    print -r -- "IOAccelerator nubs: $(grep -c '+-o ' "$D/ioaccel-$tag.txt")"
    grep '+-o ' "$D/ioaccel-$tag.txt" | cut -c1-200
    # 0.0.268: the accelerator's children as a TREE (no -l: its PerformanceStatistics is never serialised,
    # an earlier analysis) and the framebuffer's own properties (the pairing keys live there)
    tmo 20 ioreg -w0 -r -c IOAccelerator -d 3 > "$D/acceltree-$tag.txt" 2>&1
    print -r -- "accelerator subtree entries: $(grep -c '+-o ' "$D/acceltree-$tag.txt") (acceltree-$tag.txt)"
    tmo 20 ioreg -w0 -l -r -c IOFramebuffer -d 1 > "$D/framebuffer-$tag.txt" 2>&1
    print -r -- "framebuffer IOAccelTypes lines: $(grep -c '"IOAccelTypes" = ' "$D/framebuffer-$tag.txt")"
    tmo 30 ioreg -w0 -l -r -c IOUserClient > "$D/userclients-$tag.txt" 2>&1
    print -r -- "IOUserClients: $(grep -c '+-o ' "$D/userclients-$tag.txt") (saved userclients-$tag.txt); class + creator:"
    awk '/\+-o /{ c = $0; sub(/.*<class /, "", c); sub(/,.*/, "", c) }
         /"IOUserClientCreator"/{ v = $0; sub(/.*= /, "", v); print c "  " v }' "$D/userclients-$tag.txt" \
        | sort | uniq -c | sort -rn | head -40 | cut -c1-160
    return 0
}

# ------------------------------------------------------------- TEST A ----
# 0.0.268 . WindowServer restarted on an ARMED boot with the pairing keys stamped.
# : ask CoreDisplay to dump the IOPresentment capabilities it holds for each display surface.
# The notification is unprivileged and is posted as the CONSOLE user, because that is who owns the
# surfaces; posting it as root reaches a different session. Then look for what it wrote. Reports what it
# found rather than judging it - the three-way outcome is decided in the notes, not here.
iop_dump() {
    print -r -- "iopdump: /var/tmp before:"
    ls -la /var/tmp/surface_*_iop_capabilities.plist 2>/dev/null || print -r -- "  (none)"
    local cu; cu=$(stat -f %Su /dev/console)
    print -r -- "iopdump: posting com.apple.CoreDisplay.DisplaySurfaceDumpIOPCapabilities as console user '$cu'"
    if [[ "$cu" == "root" ]]; then
        notifyutil -p com.apple.CoreDisplay.DisplaySurfaceDumpIOPCapabilities
    else
        sudo -u "$cu" notifyutil -p com.apple.CoreDisplay.DisplaySurfaceDumpIOPCapabilities
    fi
    print -r -- "iopdump: notifyutil rc=$?"
    sleep 3
    print -r -- "iopdump: /var/tmp after:"
    ls -la /var/tmp/surface_*_iop_capabilities.plist 2>/dev/null || print -r -- "  *** NOTHING WRITTEN ***"
    for f in /var/tmp/surface_*_iop_capabilities.plist(N); do
        print -r -- "iopdump: --- $f ---"
        plutil -p "$f" 2>&1 | head -40
    done
    return 0
}

# an earlier analysis: cqprobe polled WITH ITS POSITIVE CONTROL. cqprobe alone cannot distinguish "no
# thread is inside submit_command_buffers" from "nothing was submitting at the moment we looked" - 585
# of 644 wscompose samples in the whole corpus read U-state 0, so a quiet moment is the COMMON case.
# Each poll therefore records the uninterruptible-wait set alongside the probe, so an IDLE reading is
# only ever read as a negative when a Metal client is demonstrably stuck at the same instant.
a_cq_poll() {
    local n=${1:-10} i
    print -r -- "cqpoll: $n polls of cqprobe, each with its U-state positive control"
    for i in $(seq 1 "$n"); do
        print -r -- "--- poll $i/$n  $(date +%H:%M:%S.%N)"
        print -r -- "U-state set (the POSITIVE CONTROL - an IDLE probe means nothing without one):"
        ps -axo pid,stat,wchan,comm | awk '$2 ~ /U/ {print "   " $0}' || true
        sudo "$N48" accel cqprobe
        sleep 3
    done
    print -r -- "cqpoll: done"
}

# an earlier analysis: the compositor witness. The rename must already be live (fbname-1 EARLIER in the
# step list) because UseIOPresentment poisons a process-wide flag on first failure, so the class must
# read AMD* before the WindowServer that should use it starts . No hold is armed:
# dtrace -W grabs the new process at exec, which is earlier than any hold could help.
a_ws_witness() {
    local cu old new script out dpid
    script=~/navi48-staging/wscomp.d
    out="$D/wscomp.out"
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before the restart: $cu"
    if [[ $cu != root ]]; then
        print -r -- "wswitness: REFUSING - '$cu' is logged in at the console; WindowServer NOT killed"
        return 1
    fi
    arm_guard_ok wswitness || return 1   # section 661: this path restarts WindowServer exactly as wskill does
    if [[ ! -r $script ]]; then print -r -- "wswitness: REFUSING - $script is not staged"; return 1; fi
    print -r -- "wswitness: framebuffer class as WindowServer will see it:"
    ioreg -c IOFramebuffer -d1 2>/dev/null | grep -E '\+-o ' | head -4
    old=$(pgrep -x WindowServer | head -1)
    print -r -- "WindowServer before: pid ${old:-none}"
    #: -Z. wscomp.d now also probes the IOKit user-client API (which clients WindowServer opens, which external
    # methods it calls on each, and every IOConnectMapMemory). Without -Z a single probe description that matches
    # nothing in this build of the framework makes dtrace refuse the WHOLE script, which would cost a boot; -Z only
    # relaxes zero-match descriptions and cannot change what a matching probe does. The probes are untouched.
    sudo dtrace -Z -q -s "$script" -W WindowServer -o "$out" > "$D/wscomp.err" 2>&1 &
    dpid=$!
    print -r -- "wswitness: dtrace -Z (job $dpid) started with -W WindowServer; waiting 8 s for it to arm"
    sleep 8
    if ! pgrep -x dtrace > /dev/null; then
        print -r -- "wswitness: ABORT - dtrace is not running after 8 s; stderr:"; cat "$D/wscomp.err" 2>/dev/null; return 1
    fi
    date +%s > "$D/.kill-epoch"
    print -r -- "wswitness: restarting WindowServer (dtrace grabs the new one at exec)"
    sudo launchctl kickstart -k system/com.apple.WindowServer > "$D/kickstart.txt" 2>&1 &
    sleep 30
    new=$(pgrep -x WindowServer | head -1)
    print -r -- "WindowServer after: pid ${new:-none} ($( [[ -n $new && $new != "$old" ]] && echo RESTARTED || echo NOT-RESTARTED ))"
    sudo pkill -INT -x dtrace 2>/dev/null; sleep 3; sudo pkill -TERM -x dtrace 2>/dev/null; sleep 2
    pgrep -x dtrace > /dev/null && print -r -- "wswitness: WARNING dtrace still running" || print -r -- "wswitness: dtrace exited"
    print -r -- "wswitness: dtrace stderr:"; cat "$D/wscomp.err" 2>/dev/null
    print -r -- "wswitness: output ($(wc -l < "$out" 2>/dev/null | tr -d ' ') lines):"; cat "$out" 2>/dev/null
}

# 0.0.322 (notes sections 601-604): the held capability-parse measurement.
# RUN 1 (iop1) FAILED FOR A REASON THE TRANSCRIPT NAMED: finding the new WindowServer pid took 709
# pgrep iterations (~2 s), which is LONGER THAN THE HOLD, so dtrace was not even started inside the
# window. Polling for a pid cannot win a race against a 1500 ms hold. The fix removes the race
# entirely: `dtrace -W <name>` waits for a process of that name to exec and grabs it AT EXEC, before
# any of its code runs, so the probes are live long before the gather. The hold stays as insurance.
a_iop_parse() {
    local ms=${1:-3000} cu old new script out dpid
    script=~/navi48-staging/iopparse.d
    out="$D/iopparse.out"
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before the hold: $cu"
    if [[ $cu != root ]]; then
        print -r -- "iopparse: REFUSING - '$cu' is logged in at the console; nothing armed, WindowServer NOT killed"
        return 1
    fi
    arm_guard_ok iopparse || return 1   # section 661: this path restarts WindowServer exactly as wskill does
    if [[ ! -r $script ]]; then print -r -- "iopparse: REFUSING - $script is not staged"; return 1; fi
    print -r -- "iopparse: arming the one-shot hold for ${ms} ms"
    sudo "$N48" accel agdchold "$ms"
    old=$(pgrep -x WindowServer | head -1)
    print -r -- "WindowServer before: pid ${old:-none}"
    # dtrace FIRST, waiting for the next WindowServer exec; give it time to compile and start waiting.
    sudo dtrace -q -s "$script" -W WindowServer -o "$out" > "$D/iopparse.err" 2>&1 &
    dpid=$!
    print -r -- "iopparse: dtrace (job $dpid) started with -W WindowServer; waiting 8 s for it to arm"
    sleep 8
    if ! pgrep -x dtrace > /dev/null; then
        print -r -- "iopparse: ABORT - dtrace is not running after 8 s; stderr:"
        cat "$D/iopparse.err" 2>/dev/null
        return 1
    fi
    date +%s > "$D/.kill-epoch"
    print -r -- "iopparse: restarting WindowServer (dtrace will grab the new one at exec)"
    sudo launchctl kickstart -k system/com.apple.WindowServer > "$D/kickstart.txt" 2>&1 &
    sleep 25
    new=$(pgrep -x WindowServer | head -1)
    print -r -- "WindowServer after: pid ${new:-none} ($( [[ -n $new && $new != "$old" ]] && echo RESTARTED || echo NOT-RESTARTED ))"
    sudo pkill -INT -x dtrace 2>/dev/null
    sleep 3
    sudo pkill -TERM -x dtrace 2>/dev/null
    sleep 2
    pgrep -x dtrace > /dev/null && print -r -- "iopparse: WARNING dtrace still running" || print -r -- "iopparse: dtrace exited"
    print -r -- "iopparse: dtrace stderr:"
    cat "$D/iopparse.err" 2>/dev/null
    print -r -- "iopparse: output ($(wc -l < "$out" 2>/dev/null | tr -d ' ') lines):"
    cat "$out" 2>/dev/null
    print -r -- "iopparse: ORIGIN line count: $(grep -c ORIGIN "$out" 2>/dev/null)"
    print -r -- "iopparse: the hold counters after the run:"
    sudo "$N48" accel agdchold
}

a_ws_kill() {
    local cu old new k
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before the kill: $cu"
    if [[ $cu != root ]]; then
        print -r -- "wskill: REFUSING - '$cu' is logged in at the console; WindowServer NOT killed"
        return 1
    fi
    # an earlier analysis: the kill may not outrun the arm. One line, whether or not this recipe arms.
    arm_guard_ok wskill || return 1
    old=$(pgrep -x WindowServer | head -1)
    print -r -- "WindowServer before: pid ${old:-none}"
    ps -axo pid,user,lstart,comm | grep -E 'WindowServer|loginwindow' | grep -v grep
    date +%s > "$D/.kill-epoch"
    print -r -- "kill epoch $(cat "$D/.kill-epoch") at $(wall): sudo launchctl kickstart -k system/com.apple.WindowServer (20 s watchdog)"
    sudo launchctl kickstart -k system/com.apple.WindowServer > "$D/kickstart.txt" 2>&1 &
    local kpid=$!
    for k in $(seq 1 20); do
        sleep 1
        new=$(pgrep -x WindowServer | head -1)
        [[ -n $new && $new != "$old" ]] && break
    done
    if kill -0 $kpid 2>/dev/null; then print -r -- "kickstart still running after ${k}s (left alone)"; else wait $kpid; print -r -- "kickstart rc=$? after <= ${k}s: $(cat "$D/kickstart.txt" 2>/dev/null | tr '\n' ' ')"; fi
    print -r -- "WindowServer after: pid ${new:-none} ($( [[ -n $new && $new != "$old" ]] && echo RESTARTED || echo NOT-RESTARTED ))"
    ps -axo pid,user,lstart,comm | grep -E 'WindowServer|loginwindow' | grep -v grep
    [[ -n $new && $new != "$old" ]]
}

# an earlier analysis: TEST S. The only process it signals is SecurityAgent (the login panel's renderer), so the WindowServer
# that started before fire - SkyLight "Software compositor activated." - stays on screen while a relaunched SecurityAgent
# opens the armed accelerator. Proposed, not yet run: it needs the reviewer's approval (a new PC-side kill).
a_sa_kill() {
    local sig=${1:-TERM} cu old new ws k
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before the SecurityAgent kill: $cu"
    if [[ $cu != root ]]; then
        print -r -- "sakill: REFUSING - '$cu' is logged in at the console; SecurityAgent NOT killed"
        return 1
    fi
    old=$(pgrep -x SecurityAgent | head -1)
    ws=$(pgrep -x WindowServer | head -1)
    print -r -- "SecurityAgent before: pid ${old:-none}; WindowServer pid ${ws:-none} (not signalled)"
    if [[ -z $old ]]; then
        print -r -- "sakill: no SecurityAgent is running - nothing killed"
        return 1
    fi
    ps -axo pid,ppid,user,lstart,comm | grep -E 'WindowServer|loginwindow|SecurityAgent' | grep -v grep
    date +%s > "$D/.kill-epoch"
    [[ $sig == (TERM|KILL) ]] || { print -r -- "sakill: signal '$sig' is not sanctioned - nothing killed"; return 1; }
    print -r -- "kill epoch $(cat "$D/.kill-epoch") at $(wall): sudo kill -$sig $old (SecurityAgent only, 30 s wait for a new pid)"
    sudo kill -"$sig" "$old"
    for k in $(seq 1 30); do
        sleep 1
        new=$(pgrep -x SecurityAgent | head -1)
        [[ -n $new && $new != "$old" ]] && break
    done
    print -r -- "SecurityAgent after: pid ${new:-none} ($( [[ -n $new && $new != "$old" ]] && echo RELAUNCHED || echo NOT-RELAUNCHED )) after ${k}s; WindowServer now pid $(pgrep -x WindowServer | head -1) (was ${ws:-none})"
    ps -axo pid,ppid,user,lstart,comm | grep -E 'WindowServer|loginwindow|SecurityAgent' | grep -v grep
    [[ -n $new && $new != "$old" ]]
}

# an earlier analysis: read-only registry state for the display-pipe question (route b). Each read has its own alarm; no -l on the
# accelerator itself  - only on the named AGDC / pipe classes, one level deep.
agdc_reg() {
    local tag=$1 c f
    for c in AppleGraphicsDeviceControl AMDRadeonX6000_AmdAgdcServices IOAccelDisplayPipe; do
        f="$D/agdc-$tag-$c.txt"
        tmo 20 ioreg -w0 -r -c "$c" -l -d 1 > "$f" 2>&1
        print -r -- "agdcreg $tag: $c rc=$? nodes $(grep -c '+-o ' "$f") (agdc-$tag-$c.txt)"
    done
    f="$D/agdc-$tag-tree.txt"
    tmo 30 ioreg -w0 -p IOService > "$f" 2>&1
    print -r -- "agdcreg $tag: IOService tree rc=$? $(wc -l < "$f" | tr -d ' ') lines; AGDC / pipe / presentment nodes:"
    grep -iE 'agdc|graphicsdevicecontrol|displaypipe|presentment|fbpresenter|DisplayMachine' "$f" | head -40
    return 0
}

# 0.0.286 : route b's first test. agdc_publish refuses unless the console is root and `pipeguard` reads
# armed-and-verified; agdc_probe runs tools/pc/agdc-probe.c (IOKit reads, the AGDC open IOPresentment makes, and
# IOPresentmentCreateForRegistryID) under hardtmo, then keeps the probe's own unified-log lines (IOPresentment logs there).
agdc_publish() {
    local cu g
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before agdc publish: $cu"
    [[ $cu == root ]] || { LIVE_ABORT=1; print -r -- "agdc-1: REFUSING - '$cu' is logged in at the console"; return 1; }
    g=$(hardtmo 20 sudo "$N48" accel pipeguard 2>&1)
    print -r -- "$g"
    print -r -- "$g" | grep -qE 'armed / verified now +: 1 / 1' || { print -r -- "agdc-1: REFUSING - pipeguard is not armed and verified"; return 1; }
    hardtmo 30 sudo "$N48" accel agdc 1
}

# 0.0.287 : tools/pc/fbbench.c mapping the scanout through Navi48Bringup's user client (target navi48sc or
# navi48vr), the path CoreDisplay's IOConnectMapMemory takes; the bytes it writes are the bytes it read.
fbbench_user() {
    local tg=$1 f rc
    f="$D/fbbench-user-$tg.txt"
    [[ -x $T/fbbench ]] || { print -r -- "ABORT fbbenchuser: $T/fbbench is missing or not executable"; return 1; }
    print -r -- "fbbenchuser launch $(wall): hardtmo 120 sudo $T/fbbench target=$tg rows=128 > ${f:t}"
    hardtmo 120 sudo "$T/fbbench" "target=$tg" rows=128 > "$f" 2>&1
    rc=$?
    print -r -- "fbbenchuser exit $(wall): rc=$rc; $(wc -l < "$f" | tr -d ' ') line(s):"
    cat "$f"
    return $rc
}

agdc_probe() {
    local cu rc t0
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before agdc-probe: $cu"
    [[ $cu == root ]] || { LIVE_ABORT=1; print -r -- "agdcprobe: REFUSING - '$cu' is logged in at the console"; return 1; }
    [[ -x $T/agdc-probe ]] || { print -r -- "ABORT agdcprobe: $T/agdc-probe is missing or not executable"; return 1; }
    t0=$(date '+%Y-%m-%d %H:%M:%S')
    print -r -- "agdcprobe launch $(wall): hardtmo 90 sudo $T/agdc-probe > agdc-probe.txt"
    hardtmo 90 sudo "$T/agdc-probe" > "$D/agdc-probe.txt" 2>&1
    rc=$?
    print -r -- "agdcprobe exit $(wall): rc=$rc; $(wc -l < "$D/agdc-probe.txt" | tr -d ' ') line(s):"
    cat "$D/agdc-probe.txt"
    sleep 2
    tmo 60 /usr/bin/log show --start "$t0" --info --debug --predicate 'process == "agdc-probe" OR eventMessage CONTAINS[c] "AGDC" OR eventMessage CONTAINS[c] "ADGC"' > "$D/agdc-probe-log.txt" 2>&1
    print -r -- "agdcprobe unified log since $t0: $(wc -l < "$D/agdc-probe-log.txt" | tr -d ' ') line(s) (agdc-probe-log.txt); first 40:"
    head -40 "$D/agdc-probe-log.txt" | cut -c1-300
    return $rc
}

a_sleep_until_kill() {
    local want=$1 e now
    e=$(cat "$D/.kill-epoch" 2>/dev/null)
    [[ -n $e ]] || { print -r -- "sleepuntilk: no kill epoch recorded - not sleeping"; return 1; }
    now=$(date +%s)
    # 0.0.279 (rule 94): the wait watches the console every 2 s; a login on the armed boot ends it at once.
    while (( e + want > now )); do
        if [[ $(stat -f %Su /dev/console 2>&1) != (root|_windowserver) ]]; then   # 0.0.280: _windowserver owns it while WindowServer restarts
            print -r -- "sleepuntilk: CONSOLE LOGIN during the wait at $(wall) - ending the wait (rule 94)"
            break
        fi
        sleep 2; now=$(date +%s)
    done
    print -r -- "now kill+$(( $(date +%s) - e ))s"
}

a_kext_log() {
    local tag=$1 f="$D/driverlog-$1.txt" e
    tmo 60 sudo "$N48" log > "$f" 2>&1
    print -r -- "kextlog $tag rc=$? -> driverlog-$tag.txt $(wc -l < "$f" | tr -d ' ') lines"
    e=$(cat "$D/.kill-epoch" 2>/dev/null); [[ -n $e ]] && print -r -- "  (kill+$(( $(date +%s) - e ))s)"
    print -r -- "  CENSUS lines $(grep -c 'shadercache: CENSUS #' "$f"); census-resource lines $(grep -c 'census of this resource' "$f"); submit-attrib $(grep -c 'submit-attrib #' "$f"); writeTail $(grep -c 'writeTail #' "$f"); VM contexts $(grep -c 'createVMContext #[0-9]* -> ctx .* in the context of pid' "$f"); SpecialAMDKey $(grep -c 'SpecialAMDKey #' "$f") (type 0x17 reset-and-replay $(grep -c 'type=0x17' "$f")); COPIED $(grep -c 'residency-copy: .*COPIED' "$f"); AT CAPACITY $(grep -c 'AT CAPACITY' "$f")"
    grep -E 'submit-attrib #|in the context of pid' "$f" | sed -E 's/.*from pid/  submit from pid/; s/.*createVMContext (#[0-9]+) -> ctx [^ ]+ in the context of/  vmctx \1 in/' | sort | uniq -c | head -20
    return 0
}

# 0.0.291 : the monitored post-kill capture for the NEUTER T-compose. the reviewer's present-fence report
# (an internal review note) settled that tcompose1 (section 498) panicked on the channel-26 GFX hang
# (untranslated session Metal GFX parking the live CP), NOT the display pipe or the census. This step attacks that: after
# wskill it watches, every ~3 s, for the "GFX is hung"/GPURestart/"Failed to reset" kernlog line that fired last time and
# for any safety-core alarm (a built FLIP or a DCN register write), captures the kext log and the per-slot / per-pid
# censuses at kill-relative marks, and WINDS THE SHIM DOWN (`pipeshim 0`) at the FIRST sign of trouble - it never lets a
# blocked WindowServer run toward the 3-min watchdog (rule 92, the reviewer's 90 s cap). It sends only reads to the kext plus that
# one shim-off; `pipeshim 2` is a re-assert that resets no counter. It cannot itself reboot: a hang still crash-loops
# WindowServer to the watchdog, but the evidence is on disk first and nothing of ours drove it.
WSCOMPOSE_ABORT=0
WS_ROUTEA=0   # set by a_routea_arm on a VERIFIED route-A arm; changes the watch's wind-down (the reviewer: keep pipeshim 2)

#: the LIVE console-framebuffer base, read from the registry, independent of anything route A reports.
# `Console,BaseAddress` is a decimal number in ioreg; print it as 0x... so it compares against the kext's hex.
# The framebuffer class is renamed at runtime by `fbname-1`, so ask by the IOFramebuffer superclass
# FIRST and fall back to the original RDNA4FB name; an unreadable base returns non-zero and the caller fails CLOSED.
a_console_fb_base() {
    local raw dec
    raw=$( { tmo 20 ioreg -w0 -l -r -c IOFramebuffer -d 1 2>/dev/null; tmo 20 ioreg -w0 -l -r -c RDNA4FB -d 1 2>/dev/null; } )
    dec=$(print -r -- "$raw" | sed -nE 's/.*"Console,BaseAddress" *= *([0-9]+).*/\1/p' | head -1)
    [[ -n $dec ]] || return 1
    printf '0x%x\n' "$dec"
    return 0
}

# 0.0.295 (an internal review note): arm corrected route A and VERIFY the ARMED state before the
# WindowServer restart. the reviewer's hard gates: status 0, armed/mode/verified 1/1/1, the scanout base EQUAL TO THE CONSOLE
# FRAMEBUFFER BASE, object len 0x800000, one pipe, a non-null real-prepare pointer. On any doubt set LIVE_ABORT so wskill
# and every later client step are skipped - route A must never be left half-armed across a restart.
#
# (m4c14 was VOIDED by this): the base gate was the LITERAL 0xc0000000. The framebuffer base MOVED to 0xd0000000 on
# the cold boot, so the gate refused a correctly armed route A and silently skipped wskill and every client step.
# The gate's MEANING is "we armed the buffer that is really scanning out", so it now compares the armed base against the
# live `Console,BaseAddress` read out of the registry - exact match, non-zero, and unreadable fails closed. BOTH numbers
# are printed on every run, pass or fail, so a mismatch is visible rather than silent. Deleting the check was not an
# option: it is what stops route A adopting a buffer nothing is scanning out of.
a_routea_arm() {
    local out st armed phys len pipes prep live
    out=$(hardtmo 20 sudo "$N48" accel routea 1 2>&1)
    print -r -- "$out"
    st=$(print -r -- "$out"    | sed -nE 's/.*routea status *: ([0-9]+).*/\1/p'                          | head -1)
    armed=$(print -r -- "$out" | sed -nE 's#.*verified *: ([0-9]+) / ([0-9]+) / ([0-9]+).*#\1\2\3#p'      | head -1)
    phys=$(print -r -- "$out"  | sed -nE 's#.*obj len *: (0x[0-9a-f]+) / 0x[0-9a-f]+.*#\1#p'              | head -1)
    len=$(print -r -- "$out"   | sed -nE 's#.*obj len *: 0x[0-9a-f]+ / (0x[0-9a-f]+).*#\1#p'              | head -1)
    pipes=$(print -r -- "$out" | sed -nE 's#.*pipes / fmt / bpp *: ([0-9]+) / .*#\1#p'                    | head -1)
    prep=$(print -r -- "$out"  | sed -nE 's/.*real prepare \(passthru\) *: (0x[0-9a-f]+).*/\1/p'          | head -1)
    #: the object length is Console,Length taken LIVE (0x7e9000 = 1920x1080x4 on this card), NOT the reviewer's
    # assumed 0x800000 - the kext is right, so the gate is a non-zero length, not an exact length.
    live=$(a_console_fb_base) || live=""
    print -r -- "routea-1: SCANOUT BASE CHECK - route A armed ${phys:-UNREPORTED} ; live console framebuffer (ioreg Console,BaseAddress) ${live:-UNREADABLE}"
    if [[ ${st:-1} != 0 || ${armed:-000} != 111 || -z $live || $live == 0x0 || -z $phys || $phys == 0x0 || $phys != $live || -z $len || $len != 0x[0-9a-f]* || $len == 0x0 || ${pipes:-0} != 1 || -z $prep || $prep == 0x0 ]]; then
        LIVE_ABORT=1
        print -r -- "routea-1: REFUSING - route A did not arm cleanly (status ${st:-?}, armed/mode/verified ${armed:-?}, phys ${phys:-?}, live fb base ${live:-?}, len ${len:-?}, pipes ${pipes:-?}, real-prepare ${prep:-?}); wskill and later client steps SKIPPED"
        return 1
    fi
    WS_ROUTEA=1
    print -r -- "routea-1: ARMED + VERIFIED - phys $phys == live console framebuffer base $live, len $len (live Console,Length) pipes $pipes real-prepare $prep; keeping pipeshim 2 for the whole armed window (the reviewer)"
    return 0
}

# The watch's wind-down on an abort. Route B: turn the shim off. Route A: DO NOT (the reviewer - pipeshim 2 keeps the native flip
# off our object's high vtable slots); a reboot restores the swapped vptr, so only record state and let the caller reboot.
ws_winddown() {
    if (( WS_ROUTEA )); then
        print -r -- "wscompose $1: route A armed - NOT winding pipeshim to 0 (the reviewer: keep pipeshim 2); reboot restores. Recording routea + pipeshim state."
        # 0.0.338: `accel pipeshim` with no scalar sends 0 and TURNS THE SHIM OFF. This branch exists precisely to
        # NOT wind the shim down, and its own "recording" read was doing it anyway - the second site of the bug run
        # mmv1 caught in `pipeshimread`. 3 is the read that changes nothing.
        { hardtmo 20 sudo "$N48" accel routea; hardtmo 20 sudo "$N48" accel pipeshim 3; } > "$D/wscompose-$1-winddown.txt" 2>&1
    else
        hardtmo 20 sudo "$N48" accel pipeshim 0 > "$D/wscompose-$1-winddown.txt" 2>&1
    fi
}
a_ws_compose() {
    local tag=$1 cap=${2:-90} e now el mark hungbase hung fl dcn ready depth pres cu nu
    e=$(cat "$D/.kill-epoch" 2>/dev/null)
    [[ -n $e ]] || { print -r -- "wscompose $tag: no kill epoch recorded - refusing to run"; return 1; }
    hungbase=0; [[ -f $D/kernlog.txt ]] && hungbase=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
    print -r -- "wscompose $tag: watch for GFX hang ('$LIVE_FATAL_PAT', baseline $hungbase), FLIPS/DCN, ring depth, console; cap ${cap}s; marks 5/15/30/60"
    local -a marks=(5 15 30 60); local mi=1
    while :; do
        now=$(date +%s); el=$(( now - e ))
        (( el >= cap )) && { print -r -- "wscompose $tag: reached the ${cap}s cap at kill+${el}s with NO abort condition - WindowServer survived; stopping the watch"; break; }
        cu=$(stat -f %Su /dev/console 2>&1)
        if [[ $cu != root && $cu != _windowserver ]]; then
            CONSOLE_ABORT=1; WSCOMPOSE_ABORT=1
            print -r -- "wscompose $tag: CONSOLE USER '$cu' at kill+${el}s (rule 94) - winding the shim down and stopping"
            ws_winddown "$tag"
            break
        fi
        hung=0; [[ -f $D/kernlog.txt ]] && hung=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
        if (( hung > hungbase )); then
            WSCOMPOSE_ABORT=1
            print -r -- "wscompose $tag: GFX HANG / RESET line appeared at kill+${el}s (kernlog fatal count $hungbase -> $hung) - ABORTING, winding the shim down"
            grep -nE "$LIVE_FATAL_PAT" "$D/kernlog.txt" | tail -8 | cut -c1-260 | sed 's/^/  hang: /'
            ws_winddown "$tag"
            a_kext_log "$tag-hang"
            break
        fi
        local pg="$D/wscompose-$tag-pg-$el.txt"
        hardtmo 20 sudo "$N48" accel pipeguard > "$pg" 2>&1
        fl=$(grep -E 'REFUSED FLIPS' "$pg" | grep -oE '[0-9]+' | head -1)
        dcn=$(grep -E 'WRITE_DATA walked / DCN' "$pg" | sed -E 's#.*: *[0-9]+ */ *([0-9]+).*#\1#')
        ready=$(grep -E 'pipe\[0\] readiness' "$pg" | grep -oE 'active\(\+0x298\) [0-9]+' | grep -oE '[0-9]+$')
        if [[ ${fl:-0} != 0 || ${dcn:-0} != 0 ]]; then
            WSCOMPOSE_ABORT=1
            print -r -- "wscompose $tag: SAFETY-CORE ALARM at kill+${el}s - FLIPS ${fl:-?}, DCN ${dcn:-?} - ABORTING, winding the shim down"
            ws_winddown "$tag"
            a_kext_log "$tag-alarm"
            break
        fi
        local ps="$D/wscompose-$tag-ps-$el.txt"
        hardtmo 20 sudo "$N48" accel pipeshim ${PIPESHIM_TICK_ARG:-2} > "$ps" 2>&1     # re-asserts the present mode every tick (keeps the shim active); 0.0.412: 4 after a pipeshim-4 step so S1 (forced linear) survives the tick; 0.0.416: 5 after a pipeshim-5 step so the GCR_REQ survives the tick
        depth=$(grep -E 'ring submit / retire' "$ps" | grep -oE 'depth -?[0-9]+' | grep -oE '\-?[0-9]+')
        pres=$(grep -E 'presents ok / refused' "$ps" | sed -E 's/.*: *([0-9]+).*/\1/')
        nu=$(ps -axo stat= 2>/dev/null | awk '$1 ~ /U/' | wc -l | tr -d ' ')
        local ra="$D/wscompose-$tag-ra-$el.txt" ifb="?"
        if (( WS_ROUTEA )); then
            hardtmo 20 sudo "$N48" accel routea > "$ra" 2>&1
            ifb=$(grep -E 'slot267 initFb / ready' "$ra" | sed -E 's#.*: *([0-9]+) / ([0-9]+).*#\1/\2#')
        fi
        print -r -- "LIVE wscompose-t$el $(wall): console $cu, U-state $nu, +0x298 ${ready:-?}, initFb/ready ${ifb:-?}, ring depth ${depth:-?}, presents ${pres:-?}, FLIPS ${fl:-0}, DCN ${dcn:-0}"
        if [[ -n $depth ]] && (( depth >= 3 )); then
            WSCOMPOSE_ABORT=1
            print -r -- "wscompose $tag: RING DEPTH ${depth} >= 3 at kill+${el}s (wait_for_queue_slot hazard) - ABORTING, winding the shim down"
            ws_winddown "$tag"
            a_kext_log "$tag-ring"
            break
        fi
        if (( mi <= ${#marks} )) && (( el >= marks[mi] )); then
            mark=${marks[mi]}; mi=$(( mi + 1 ))
            print -r -- "wscompose $tag: MARK kill+${mark}s (at kill+${el}s) - capturing kextlog + per-slot census + per-pid GFX census"
            a_kext_log "$tag-k$mark"
            hardtmo 20 sudo "$N48" accel emcensus   > "$D/wscompose-$tag-emcensus-k$mark.txt" 2>&1
            hardtmo 20 sudo "$N48" accel gfxneuter  > "$D/wscompose-$tag-gfxneuter-k$mark.txt" 2>&1
            hardtmo 20 sudo "$N48" accel gfxcensus  > "$D/wscompose-$tag-gfxcensus-k$mark.txt" 2>&1
            hardtmo 20 sudo "$N48" accel finishread > "$D/wscompose-$tag-finish-k$mark.txt" 2>&1
            # 0.0.295: route-A engagement (initFb/ready/guard/presents) and a read-only scanout thumbnail (FNV-1a + luma)
            # so a composited frame is visible in the captures, not only on the monitor.
            hardtmo 20 sudo "$N48" accel routea     > "$D/wscompose-$tag-routea-k$mark.txt" 2>&1
            hardtmo 20 sudo "$N48" accel scanout 4  > "$D/wscompose-$tag-thumb-k$mark.txt" 2>&1
            # 0.0.333 : what actually reached IOAccelDisplayPipeUserClient2, class resolved in-kernel.
            hardtmo 20 sudo "$N48" accel ucprobe    > "$D/wscompose-$tag-ucprobe-k$mark.txt" 2>&1
        fi
        sleep 3
    done
    print -r -- "wscompose $tag: done at kill+$(( $(date +%s) - e ))s, WSCOMPOSE_ABORT=$WSCOMPOSE_ABORT"
    return 0
}

# ------------------------------------------------------------ LIVENESS ----
# 0.0.269 . A soak that sends nothing to the kext, and the kernel-log lines that would mean the
# engines died: Apple's hang detector (event timeout / "is hung"), the failed reset and the permanent refusal.
LIVE_ABORT=0
CONSOLE_ABORT=0   # 0.0.279 (rule 94): a console login seen on the ARMED boot - from then on only capture steps run
live_check() {
    local tag=$1 cu nu kh=MISSING kt=MISSING
    cu=$(stat -f %Su /dev/console 2>&1)
    nu=$(ps -axo stat= 2>/dev/null | awk '$1 ~ /U/' | wc -l | tr -d ' ')
    local kf=MISSING kw=MISSING
    if [[ -f $D/kernlog.txt ]]; then
        kh=$(grep -cE "$LIVE_HANG_PAT" "$D/kernlog.txt")
        kf=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
        kw=$(grep -cE "$LIVE_WARN_PAT" "$D/kernlog.txt")
        kt=$(grep -c '(AMDRadeonX6000)' "$D/kernlog.txt")
    fi
    print -r -- "LIVE $tag $(wall): console $cu, U-state $nu, kernlog FATAL lines $kf, warning lines $kw (both: hang lines $kh; AMDRadeonX6000 lines $kt)"
    [[ $cu == root ]] || { LIVE_ABORT=1; print -r -- "LIVE $tag: CONSOLE USER '$cu' APPEARED - aborting the soak; later client steps are SKIPPED"; return 1; }
    return 0
}
live_soak() {
    local want=$1 t=0 step=30
    print -r -- "LIVE soak: ${want}s, nothing sent to the kext; a tick every ${step}s"
    live_check soak-t0 || return 1
    while (( t < want )); do
        (( want - t < step )) && step=$(( want - t ))
        sleep $step; t=$(( t + step ))
        live_check "soak-t$t" || return 1
    done
    [[ -f $D/kernlog.txt ]] && grep -nE "$LIVE_FATAL_PAT" "$D/kernlog.txt" | head -12 | cut -c1-260 | sed 's/^/LIVE FATAL line: /'
    [[ -f $D/kernlog.txt ]] && grep -nE "$LIVE_WARN_PAT" "$D/kernlog.txt" | head -12 | cut -c1-260 | sed 's/^/LIVE warning line: /'
    return 0
}

# ------------------------------------------------------------ M4 route c' ----
# (notes/M4-FRAME-DELIVERY.md section 6, notes/M4-CPRIME-DESIGN.md section 3). The adoption probe is a test
# client: the console user is checked immediately before it, and a non-root console sets LIVE_ABORT (every later client
# step is skipped; the brief's console gate is finish safely, reboot clean, stop).
m4_adopt() {
    local cu rc
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before m4-adopt: $cu"
    if [[ $cu != root ]]; then
        LIVE_ABORT=1
        print -r -- "m4adopt: REFUSING - '$cu' is logged in at the console; m4-adopt NOT run"
        return 1
    fi
    [[ -x $T/m4-adopt ]] || { print -r -- "ABORT m4adopt: $T/m4-adopt is missing or not executable"; return 1; }
    print -r -- "m4adopt launch $(wall): tmo 120 sudo $T/m4-adopt > m4-adopt.txt"
    tmo 120 sudo "$T/m4-adopt" > "$D/m4-adopt.txt" 2>&1
    rc=$?
    print -r -- "m4adopt exit $(wall): rc=$rc (142 = the 120 s alarm fired); $(wc -l < "$D/m4-adopt.txt" | tr -d ' ') line(s):"
    cat "$D/m4-adopt.txt"
    return $rc
}

# 0.0.276 (rule 92). hardtmo <s> <cmd...>: run the command in the background and wait AT MOST <s>
# seconds; one still running then is ABANDONED (left running, never waited on) and the step returns 142. tmo's perl
# alarm cannot end a process in uninterruptible wait - testa2's `ioreg -r -c IOAccelerator` blocked past it and hung
# the whole run. Liveness is checked with ps, which works across users (kill -0 on a root child would not).
hardtmo() {
    local t=$1; shift
    "$@" &
    local pid=$! k=0
    while (( k < t * 5 )); do
        ps -p $pid > /dev/null 2>&1 || { wait $pid; return $?; }
        sleep 0.2; k=$((k + 1))
    done
    print -r -- "hardtmo: '$*' still running after ${t}s (pid $pid) - ABANDONED, not waited on"
    return 142
}

# 0.0.276: the driver log streamed to disk (navi48test logstream: read, append, F_FULLFSYNC, consume, every second).
logstream_start() {
    [[ -f $D/.logstream.pid ]] && { print -r -- "logstream: already started"; return 1; }
    rm -f "$D/.logstream-stop"
    ( sudo nohup "$N48" logstream "$D/driverlog-stream.txt" 250 1800 "$D/.logstream-stop" > "$D/logstream.out" 2>&1 < /dev/null & print $! > "$D/.logstream.pid" )
    sleep 2
    print -r -- "logstream: started (wrapper pid $(cat "$D/.logstream.pid" 2>/dev/null)); $(wc -c < "$D/driverlog-stream.txt" 2>/dev/null | tr -d ' ') byte(s) on disk after 2 s"
    grep -m1 '### logstream start' "$D/driverlog-stream.txt" || { print -r -- "logstream: NO start header on disk - FAILING"; return 1; }
    return 0
}
# notes/M4-CONTEXT-LATCH.md: the forced-draw client, started before `fire` so that it asks for a device the instant the
# accelerator appears - the draw hp2's login panel made by chance, made on purpose. It is never killed by the recipe.
drawclient_start() {
    [[ -x $T/drawclient ]] || { print -r -- "drawclient: $T/drawclient is missing or not executable - FAILING"; return 1; }
    pgrep -x drawclient > /dev/null && { print -r -- "drawclient: already running ($(pgrep -x drawclient | tr '\n' ' ')) - FAILING"; return 1; }
    ( sudo nohup "$T/drawclient" 60 50 "$D/drawclient.state" 180 > "$D/drawclient.out" 2>&1 < /dev/null & print $! > "$D/.drawclient.pid" )
    sleep 1
    print -r -- "drawclient: started (wrapper pid $(cat "$D/.drawclient.pid" 2>/dev/null)); state: $(cat "$D/drawclient.state" 2>&1)"
    grep -m1 'drawclient: .* start (pid' "$D/drawclient.out" || { print -r -- "drawclient: NO start line - FAILING"; cat "$D/drawclient.out" 2>/dev/null; return 1; }
    return 0
}
drawclient_read() {
    print -r -- "drawclient: running: $(pgrep -x drawclient | tr '\n' ' ' || true); state: $(cat "$D/drawclient.state" 2>&1)"
    grep -E 'drawclient: .* (ACCELERATOR APPEARED|DEVICE OBTAINED|returned NO DEVICE|command queue|pipeline|DRAWING|GAVE UP|done|STILL in flight)' \
        "$D/drawclient.out" 2>/dev/null | head -20
    tail -3 "$D/drawclient.out" 2>/dev/null
    return 0
}
# 0.0.282 : the binary capture ring streamed to disk the same way (navi48test capstream).
capstream_start() {
    [[ -f $D/.capstream.pid ]] && { print -r -- "capstream: already started"; return 1; }
    rm -f "$D/.capstream-stop"
    ( sudo nohup "$N48" capstream "$D/capture.bin" 250 1800 "$D/.capstream-stop" > "$D/capstream.out" 2>&1 < /dev/null & print $! > "$D/.capstream.pid" )
    sleep 2
    print -r -- "capstream: started (wrapper pid $(cat "$D/.capstream.pid" 2>/dev/null)); $(wc -c < "$D/capture.bin" 2>/dev/null | tr -d ' ') byte(s) on disk after 2 s"
    grep -m1 'capstream: start' "$D/capstream.out" || { print -r -- "capstream: NO start line - FAILING"; return 1; }
    return 0
}
capstream_stop() {
    touch "$D/.capstream-stop"
    local k
    for k in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "navi48test capstream" > /dev/null || break; sleep 1; done
    print -r -- "capstream: stop requested; $(wc -c < "$D/capture.bin" 2>/dev/null | tr -d ' ') byte(s) on disk"
    cat "$D/capstream.out" 2>/dev/null
    rm -f "$D/.capstream.pid"
    return 0
}
logstream_stop() {
    touch "$D/.logstream-stop"
    local k
    for k in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "navi48test logstream" > /dev/null || break; sleep 1; done
    print -r -- "logstream: stop requested; $(wc -c < "$D/driverlog-stream.txt" 2>/dev/null | tr -d ' ') byte(s) on disk; $(tail -1 "$D/driverlog-stream.txt" 2>/dev/null)"
    cat "$D/logstream.out" 2>/dev/null
    rm -f "$D/.logstream.pid"
    return 0
}

# 0.0.276: a snapshot that never touches the accelerator (no ioreg of IOAccelerator or IOUserClient) - console user, ps and
# the U-state processes - with every probe behind hardtmo. For use after `wskill` on an adopted boot (rule 92).
ws_safe() {
    local tag=$1
    print -r -- "console user: $(stat -f %Su /dev/console 2>&1) at $(wall)"
    hardtmo 10 ps -axo stat,pid,etime,comm > "$D/ps-stat-$tag.txt" 2>&1
    print -r -- "U-state processes: $(awk 'NR > 1 && $1 ~ /U/' "$D/ps-stat-$tag.txt" | wc -l | tr -d ' ')"
    awk 'NR > 1 && $1 ~ /U/' "$D/ps-stat-$tag.txt" | head -12
    grep -E 'WindowServer|SecurityAgent|loginwindow' "$D/ps-stat-$tag.txt" | head -6
    return 0
}

# 0.0.278 : the GFX neuter's positive control, before any WindowServer restart. tri committed one GFX frame
# with the neuter armed: the neuter must have NOPed its IB (frames >= 1, 0 read-back mismatches) and Apple must report the
# command buffer COMPLETED (tri prints `status: 4` when its poll sees MTLCommandBufferStatusCompleted - its frame's own stamp
# was written by the GPU). Anything else sets LIVE_ABORT, which SKIPS wskill: a neuter that does not retire must not be
# discovered by a watchdog panic.
neuter_gate() {
    local s=${1:-45} t=0 st="" out fr mm
    while (( t < s )); do
        st=$(grep -m1 -E '^status: [0-9]+' /tmp/trace-tri.txt 2>/dev/null)
        [[ -n $st ]] && break
        sleep 1; t=$((t + 1))
    done
    print -r -- "neutergate: tri status line after ${t}s: ${st:-NONE}; tri.state: $(cat "$T/tri.state" 2>&1)"
    out=$(hardtmo 20 sudo "$N48" accel gfxneuter 2>&1)
    print -r -- "$out"
    fr=$(print -r -- "$out" | sed -nE 's/^  neuter armed \/ frames +: [0-9]+ \/ ([0-9]+) .*/\1/p')
    mm=$(print -r -- "$out" | sed -nE 's/.*read-back mismatches ([0-9]+)\).*/\1/p')
    # 0.0.279 : a frame neutered at the source counts as neutered (the IB packet was never written)
    local sr; sr=$(print -r -- "$out" | sed -nE 's/^  source neuter .*submissions ([0-9]+), IBs.*/\1/p')
    [[ -n $fr && -n $sr ]] && fr=$(( fr + sr ))
    print -r -- "NEUTER GATE: tri '${st:-NONE}', neutered frames ${fr:-MISSING} (source ${sr:-MISSING}), read-back mismatches ${mm:-MISSING}"
    # 0.0.279: the WindowServer restart needs the race-proof neuter proven on hardware - tri's frame neutered AT THE SOURCE
    if [[ $st != "status: 4" || -z $fr || $fr -lt 1 || $mm != 0 || -z $sr || $sr -lt 1 ]]; then
        LIVE_ABORT=1
        print -r -- "NEUTER GATE FAILED - wskill and every later soak/client step will be SKIPPED"
        return 1
    fi
    print -r -- "NEUTER GATE PASSED"
    return 0
}

# 0.0.278 : the brief's order - the scanout copy goes on only once WindowServer's flushes are witnessed.
copy_on_gate() {
    local out n
    out=$(hardtmo 20 sudo "$N48" accel flushhook 2>&1)
    print -r -- "$out"
    n=$(print -r -- "$out" | sed -nE 's/^  flush calls \(non-zero\) +: ([0-9]+) .*/\1/p')
    if [[ -z $n || $n -lt 1 ]]; then
        print -r -- "COPY GATE: flush calls ${n:-MISSING} - the copy is NOT turned on"
        return 1
    fi
    print -r -- "COPY GATE: flush calls $n - turning the scanout copy on (flushhook 3)"
    hardtmo 20 sudo "$N48" accel flushhook 3
}

# 0.0.275 : test A with liveness drops prefire-pairing because adoption stamps Apple's keys. Before wskill
# this proves they are there: RDNA4FB carries IOAccelTypes naming our accelerator AND display-registry resolves kr=0x0. If
# either is missing it sets LIVE_ABORT, which SKIPS wskill (and every later soak/client step): no restart without keys.
keys_gate() {
    local n kr
    tmo 20 ioreg -w0 -l -r -c RDNA4FB -d 1 > "$D/keysgate-framebuffer.txt" 2>&1
    grep -E '"IOAccel(Types|Index|Revision)" = |"IOCFPlugInTypes" = ' "$D/keysgate-framebuffer.txt"
    n=$(grep -cE '"IOAccelTypes" = "IOService:.*AMDRadeonX6000_AMDNavi21GraphicsAccelerator"' "$D/keysgate-framebuffer.txt")
    tmo 30 "$T/display-registry" > "$D/keysgate-registry.txt" 2>&1
    cat "$D/keysgate-registry.txt"
    kr=$(grep -cE 'framebuffer\[[0-9]+\] regID=.* -> kr=0x0 ' "$D/keysgate-registry.txt")
    print -r -- "KEYS GATE: IOAccelTypes naming our accelerator on RDNA4FB: $n; display-registry kr=0x0 lines: $kr"
    if (( n == 0 || kr == 0 )); then
        LIVE_ABORT=1
        print -r -- "KEYS GATE FAILED - wskill and every later soak/client step will be SKIPPED"
        return 1
    fi
    print -r -- "KEYS GATE PASSED"
    return 0
}

# 0.0.356 : the ONE step that writes into WindowServer's page table. Refused unless nothing has
# aborted the run (no console login, no GFX hang in wscompose, no LIVE_ABORT), and unless `tri` has exited - the
# brief's rule: tri must never submit inside a window where the rootwrite is live.
rootwrite_ws() {
    local cu tp hung=0
    cu=$(stat -f %Su /dev/console 2>&1)
    tp=$(pgrep -x tri | tr '\n' ' ')
    [[ -f $D/kernlog.txt ]] && hung=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
    print -r -- "rootwritews gate $(wall): console $cu, LIVE_ABORT $LIVE_ABORT, WSCOMPOSE_ABORT ${WSCOMPOSE_ABORT:-0}, CONSOLE_ABORT $CONSOLE_ABORT, tri pid(s) '${tp// /}', kernlog GFX hang/reset lines $hung"
    # _windowserver owns /dev/console for seconds while a restarted WindowServer comes up; it is not a login (rule 94).
    if [[ $cu != (root|_windowserver) ]] || (( LIVE_ABORT || CONSOLE_ABORT || ${WSCOMPOSE_ABORT:-0} || hung )) || [[ -n ${tp// /} ]]; then
        print -r -- "rootwritews: REFUSING - the gate above is not clean; NOTHING sent to the kext (report-only read follows)"
        hardtmo 20 sudo "$N48" accel rootwrite 2
        return 1
    fi
    hardtmo 20 sudo "$N48" accel rootwrite 3
}

# 0.0.356 : a SECOND WindowServer restart, to watch the rootwrite's withdrawal at WindowServer's
# teardown (unmapVA) and at its context release. Only on a healthy WindowServer: no abort of any kind, and the
# WindowServer process NOT in uninterruptible wait (a U-state WindowServer cannot be torn down, rule 92).
ws_kill_if_ok() {
    local st hung=0
    st=$(ps -axo stat=,comm= 2>/dev/null | awk '$2 ~ /WindowServer$/ {print $1}' | head -1)
    [[ -f $D/kernlog.txt ]] && hung=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
    print -r -- "wskillifok gate $(wall): WindowServer stat '${st:-none}', LIVE_ABORT $LIVE_ABORT, WSCOMPOSE_ABORT ${WSCOMPOSE_ABORT:-0}, CONSOLE_ABORT $CONSOLE_ABORT, kernlog GFX hang/reset lines $hung"
    if [[ -z $st || $st == *U* ]] || (( LIVE_ABORT || CONSOLE_ABORT || ${WSCOMPOSE_ABORT:-0} || hung )); then
        print -r -- "wskillifok: SKIPPED - WindowServer is not healthy or the run has aborted; WindowServer NOT killed"
        return 1
    fi
    a_ws_kill
}

# ---------------------------------------------------------------- THE ARM ----
# an earlier analysis. Three pieces: the reader, the gate, and the in-list arm. None of them changes what is measured.

# Speak to the run's SUMMARY stream as well as to transcript.txt. `tools/accel-cycle.sh` redirects this script's
# stdout to ~/navi48-runs/<run>.remote.txt; fd 3 is a copy of that stream, opened at the top of the live run, so the
# arm gate can announce itself where the operator is watching without the 30 s ticks (which stay in transcript.txt)
# flooding the file. A no-op when fd 3 is not open: --dry-run, ACCEL_RUN_SUMMARY_ONLY, or the functions sourced for an
# offline test. This changes nothing the gate does - only where its start/confirm/expiry lines are also visible.
arm_summary() { [[ -e /dev/fd/3 ]] && print -r -- "$@" >&3; }

# The LAST `gfx-commit: ARM LEVEL IS` line the kext has printed. The ONLY place it is readable during a run is the
# live log stream: `logstream` consumes the kext's ring, so a verb's own delta comes back empty (an earlier analysis -
# "never `navi48test log` - `logstream` owns the ring"), and arm11's `STEP 77 gfxneuter-4` is the proof, printing the
# packed CLI scalars and no `gfx-commit:` line at all. Reading the file costs the kext NOTHING.
arm_stream_line() {
    local from=${1:-1}
    [[ -f $D/driverlog-stream.txt ]] || return 1
    tail -n "+$from" "$D/driverlog-stream.txt" 2>/dev/null | grep -a 'gfx-commit: ARM LEVEL IS' | tail -1
    return 0
}
arm_stream_lines() { wc -l < "$D/driverlog-stream.txt" 2>/dev/null | tr -d ' ' || print 0 }

# Stamp the arm the moment its read-back is first SEEN. The stream carries no timestamps of its own, so this is the
# observation time, not the kext's - within one 250 ms stream poll plus the gate's own 2 s tick, and it is labelled
# as such wherever it is printed.
arm_confirm() {
    ARM_CONFIRMED=1
    ARM_WALL=$(wall)
    ARM_EPOCH=$(date +%s)
}

# THE GUARD every WindowServer restart path calls. Section 661's lesson: `wskill` is not the only path that restarts
# WindowServer - `wswitness` and `iopparse-<n>` do exactly the same thing - so all three ask this, not just the one.
arm_guard_ok() {
    local who=$1
    (( ARM_REQUIRED )) || { print -r -- "armwskill: $who at $(wall) - this recipe declared NO COMMIT arm (no \`armgate\`, no \`commitarm\`); the restart is ungated"; return 0 }
    if (( ! ARM_CONFIRMED )); then
        print -r -- "$who: REFUSING - this recipe declares a COMMIT arm and the arm's own read-back has NOT confirmed"
        local _l; _l=$(arm_stream_line 2>/dev/null) || _l=""
        print -r -- "$who:   the kext has never printed \`ARM LEVEL IS 2 (COMMIT); one-shot ARMED\`; last arm line: ${_l:-NONE}"
        print -r -- "$who:   an earlier analysis: an unarmed window is a VOID measurement (arm12: not-armed 731 of 896 gate attempts, COMMITTED 0). WindowServer NOT killed"
        LIVE_ABORT=1
        return 1
    fi
    print -r -- "armwskill: ARM CONFIRMED at $ARM_WALL (epoch $ARM_EPOCH, first seen in driverlog-stream.txt), $who at $(wall) (epoch $(date +%s)), GAP $(( $(date +%s) - ARM_EPOCH )) s - the window opens AFTER the arm"
    return 0
}

# armgate[-<s>]: declare the arm and WAIT for it. Returns the instant the arm is seen, so a batch that finishes
# early costs nothing; a slow batch delays the kill instead of the kill outrunning the arm.
arm_gate() {
    local want=${1:-240} t=0 tick=2 last="" lvl
    ARM_REQUIRED=1
    print -r -- "armgate $(wall): this recipe ARMS at COMMIT. Waiting up to ${want}s for \`ARM LEVEL IS 2 (COMMIT); one-shot ARMED\` in driverlog-stream.txt. NOTHING is sent to the kext."
    if [[ ! -f $D/driverlog-stream.txt ]]; then
        LIVE_ABORT=1
        print -r -- "ARM GATE FAILED - no $D/driverlog-stream.txt: \`logstream-start\` must be earlier in the list, and it is the ONLY place the arm's verdict line is readable while a run is live . wskill and every later client step will be SKIPPED"
        arm_summary "===> ARM GATE FAILED (armgate step ${step_no:-?}): no driverlog-stream.txt, so \`logstream-start\` must precede armgate and the arm can never be seen; wskill and every later client step will be SKIPPED."
        return 1
    fi
    arm_summary "===> ARM GATE WAITING (armgate step ${step_no:-?}): send the out-of-band COMMIT arm batch NOW. Live view is $D/transcript.txt - this summary stays silent until the gate ends. Window ${want}s; NOTHING is sent to the kext."
    while (( t <= want )); do
        last=$(arm_stream_line)
        if [[ -n $last ]] && print -r -- "$last" | grep -qaE "$ARM_OK_PAT"; then
            arm_confirm
            print -r -- "armgate: ARM CONFIRMED after ${t}s at $ARM_WALL"
            print -r -- "armgate:   $last"
            arm_summary "===> ARM GATE CONFIRMED after ${t}s at $ARM_WALL - the COMMIT arm is live; continuing to the WindowServer kill."
            return 0
        fi
        (( t > 0 && t % 30 == 0 )) && print -r -- "armgate t+${t}s $(wall): still not armed; last arm line: ${last:-NONE yet}"
        sleep $tick; t=$(( t + tick ))
    done
    LIVE_ABORT=1
    print -r -- "armgate: last arm line after ${want}s: ${last:-NONE - the kext never printed one}"
    print -r -- "ARM GATE FAILED - the COMMIT arm never confirmed in ${want}s; wskill and every later client step will be SKIPPED (an earlier analysis: a window judged at ARM LEVEL 1 is void, not a result)"
    arm_summary "===> ARM GATE FAILED (expired after ${want}s): the COMMIT arm never confirmed; wskill and every later client step will be SKIPPED."
    return 1
}

# fcgate[-<s>] (: the fast-copy positive control must PASS before the WindowServer kill.
# Reads the stream only; sends NOTHING. STAGING REFUSED, POSITIVE CONTROL FAILED / CONTROL not run, a last switch-63
# line that is not ON-full (831), a console user, any who line, or the timeout = LIVE_ABORT (wskill and every later
# client step SKIPPED).
fc_gate() {
    local want=${1:-60} t=0 w pass=0 bad=0 cu nu l63
    if [[ ! -f $D/driverlog-stream.txt ]]; then
        LIVE_ABORT=1; print -r -- "FC GATE FAILED - no driverlog-stream.txt (logstream-start must precede fcgate); wskill SKIPPED"; return 1
    fi
    while (( t <= want )); do
        w=$(grep -a -F 'fastcopy63: ' "$D/driverlog-stream.txt")
        bad=$(print -r -- "$w" | grep -a -c -E 'STAGING REFUSED|POSITIVE CONTROL FAILED|CONTROL not run')
        pass=$(print -r -- "$w" | grep -a -c -F 'fastcopy63: POSITIVE CONTROL PASSED')
        (( bad || pass )) && break
        sleep 2; t=$(( t + 2 ))
    done
    print -r -- "$w" | grep -a -E 'STAGING|POSITIVE CONTROL|CONTROL not run|switch 63 is' | cut -c1-400
    l63=$(print -r -- "$w" | grep -a -F 'fastcopy63: switch 63 is ' | tail -1)
    cu=$(stat -f %Su /dev/console 2>&1); nu=$(who 2>/dev/null | wc -l | tr -d ' ')
    print -r -- "fcgate $(wall) after ${t}s: PASSED $pass, refused/failed $bad, console $cu, users $nu"
    if (( bad || ! pass )) || [[ $l63 != *'switch 63 is ON, full MM verify (831)'* ]] || [[ $cu != root ]] || (( nu != 0 )); then
        LIVE_ABORT=1; print -r -- "FC GATE FAILED - NO KILL: wskill and every later client step will be SKIPPED"; return 1
    fi
    print -r -- "FC GATE PASSED"; return 0
}

# commitarm: the arm as a STEP, for a recipe that does not want an out-of-band batch at all. The pre-arm checklist
# is the kext's own (`n48_cm_arm_missing`) - it refuses a dirty world itself and names each missing item - so this
# reads it first and sends the arm only on its CLEAN line.
commit_arm() {
    local cu nu hung=0 l0 l1 t=0 last="" new
    ARM_REQUIRED=1
    cu=$(stat -f %Su /dev/console 2>&1)
    nu=$(who 2>/dev/null | wc -l | tr -d ' ')
    [[ -f $D/kernlog.txt ]] && hung=$(grep -cE "$LIVE_FATAL_PAT" "$D/kernlog.txt")
    print -r -- "commitarm gate $(wall): console $cu, login sessions $nu, LIVE_ABORT $LIVE_ABORT, WSCOMPOSE_ABORT ${WSCOMPOSE_ABORT:-0}, CONSOLE_ABORT $CONSOLE_ABORT, kernlog GFX hang/reset lines $hung"
    if [[ $cu != root ]] || (( nu != 0 || LIVE_ABORT || CONSOLE_ABORT || ${WSCOMPOSE_ABORT:-0} || hung )); then
        LIVE_ABORT=1
        print -r -- "commitarm: REFUSING - never arm with a console user logged in, or on a run that has aborted; NOTHING sent to the kext"
        return 1
    fi
    if [[ ! -f $D/driverlog-stream.txt ]]; then
        LIVE_ABORT=1
        print -r -- "commitarm: REFUSING - no $D/driverlog-stream.txt, so the arm could not be read back ; \`logstream-start\` must be earlier in the list. NOTHING sent to the kext"
        return 1
    fi
    # READ FIRST, always: mode 0 reads and arms nothing.
    l0=$(arm_stream_lines)
    print -r -- "commitarm: the pre-arm checklist, READ ONLY (accel gfxneuter 4, mode 0):"
    hardtmo 20 sudo "$N48" accel gfxneuter 4
    t=0
    while (( t < 10 )); do
        sleep 1; t=$(( t + 1 ))
        new=$(tail -n "+$(( l0 + 1 ))" "$D/driverlog-stream.txt" 2>/dev/null | grep -a 'gfx-commit:')
        [[ -n $new ]] && break
    done
    print -r -- "$new"
    if [[ -z $new ]] || ! print -r -- "$new" | grep -qa 'the pre-arm checklist is CLEAN'; then
        LIVE_ABORT=1
        print -r -- "commitarm: REFUSING - the kext did not print \`the pre-arm checklist is CLEAN\` within ${t}s; NOTHING armed"
        return 1
    fi
    l1=$(arm_stream_lines)
    print -r -- "commitarm $(wall): SENDING accel gfxneuter 260 (= 4 | 1 << 8, the one-shot)"
    hardtmo 20 sudo "$N48" accel gfxneuter 260
    t=0
    while (( t < 15 )); do
        sleep 1; t=$(( t + 1 ))
        last=$(arm_stream_line $(( l1 + 1 )))
        [[ -n $last ]] && break
    done
    print -r -- "commitarm: read-back after ${t}s: ${last:-NONE}"
    if [[ -n $last ]] && print -r -- "$last" | grep -qaE "$ARM_OK_PAT"; then
        arm_confirm
        print -r -- "commitarm: ARM CONFIRMED at $ARM_WALL"
        return 0
    fi
    LIVE_ABORT=1
    print -r -- "ARM GATE FAILED - \`accel gfxneuter 260\` did not read back as ARMED; wskill and every later client step will be SKIPPED"
    return 1
}

# 0.0.272 : the walls 2-3 test client, gated like m4adopt.
M4FLUSH_N=0
m4_flush() {
    local rows=${1:-128} lockopt=${2:-1} cu rc f
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "console user immediately before m4-flush: $cu"
    if [[ $cu != root ]]; then
        LIVE_ABORT=1
        print -r -- "m4flush: REFUSING - '$cu' is logged in at the console; m4-flush NOT run"
        return 1
    fi
    [[ -x $T/m4-flush ]] || { print -r -- "ABORT m4flush: $T/m4-flush is missing or not executable"; return 1; }
    M4FLUSH_N=$(( M4FLUSH_N + 1 )); f="$D/m4-flush-$M4FLUSH_N.txt"
    print -r -- "m4flush launch $(wall): tmo 180 sudo $T/m4-flush --rows $rows --lock-options $lockopt --hold 5 > ${f:t}"
    tmo 180 sudo "$T/m4-flush" --rows "$rows" --lock-options "$lockopt" --hold 5 > "$f" 2>&1
    rc=$?
    print -r -- "m4flush exit $(wall): rc=$rc (142 = the 180 s alarm fired); $(wc -l < "$f" | tr -d ' ') line(s):"
    cat "$f"
    return $rc
}

step_argv() {
    case "$1" in
        ht-*)                                           step_argv "${1#ht-}"; reply=(hardtmo 20 "${reply[@]}") ;;   # 0.0.276: any plain verb under hardtmo
        drain)                                          reply=(sudo "$N48" accel drain) ;;       # 0.0.269 : READ-ONLY drain counters
        soak-[0-9]*)                                    reply=(live_soak "${1#soak-}") ;;        # 0.0.269: a soak that sends nothing to the kext
        livecheck-[A-Za-z0-9]*)                         reply=(live_check "${1#livecheck-}") ;;  # 0.0.269: one soak tick
        logreset|info|counters|log|metrics|poke|submit) reply=(sudo "$N48" "$1") ;;
        sleep[0-9]*)                                    reply=(sleep "${1#sleep}") ;;
        reg-0x[0-9a-fA-F]*)                             reply=(sudo "$N48" reg "${1#reg-}") ;;
        vmib-0x[0-9a-fA-F]*)                            reply=(sudo "$N48" accel vmib "${1#vmib-}") ;;
        ringib-[0-9]*)                                  reply=(sudo "$N48" accel ringib "${1#ringib-}") ;;
        copyarm)                                        reply=(sudo "$N48" accel pagecopy 1) ;;
        copyoutarm)                                     reply=(sudo "$N48" accel pagecopy 2) ;;   # 0.0.284 : page-in AND page-out copies
        eopbridge)                                      reply=(sudo "$N48" accel eopbridge 1) ;;   # 0.0.235 : ARM the IH -> checkTimestamps bridge
        eopbridgeoff)                                   reply=(sudo "$N48" accel eopbridge 2) ;;
        eopbridgeread)                                  reply=(sudo "$N48" accel eopbridge) ;;     # read the counters, arm nothing
        bootchain)                                      reply=(sudo "$N48" accel bootchain) ;;
        bootchain-0x[0-9a-fA-F]*)                       reply=(sudo "$N48" accel bootchain "${1#bootchain-}") ;;   # M4-CONTEXT-LATCH: BEFORE fire (prefire-)
        drawclient)                                     reply=(drawclient_start) ;;                # M4-CONTEXT-LATCH: the forced-draw client (prefire-)
        drawclientread)                                 reply=(drawclient_read) ;;
        pairing)                                        reply=(sudo "$N48" accel pairing 1) ;;    # 0.0.267 : enable the OPT-IN display-pairing stamp; BEFORE fire (use prefire-pairing)
        pairingread)                                    reply=(sudo "$N48" accel pairing) ;;
        pairingoff)                                     reply=(sudo "$N48" accel pairing 2) ;;    # disable, or withdraw our stamped keys (registry only)
        wslogstart)                                     reply=(p2_log_start) ;;                  # 0.0.267 : P2 live log streams
        wslogstart-a)                                   reply=(p2_log_start a) ;;                # 0.0.268 : TEST A, broader predicates
        wskill)                                         reply=(a_ws_kill) ;;
        armgate)                                        reply=(arm_gate 240) ;;     # an earlier analysis: THE ARM GATE - wait for the COMMIT arm, then let wskill fire
        armgate-[0-9]*)                                 reply=(arm_gate "${1#armgate-}") ;;
        fcgate)                                         reply=(fc_gate 60) ;;       # : the fast-copy control must PASS before wskill
        fcgate-[0-9]*)                                  reply=(fc_gate "${1#fcgate-}") ;;
        commitarm)                                      reply=(commit_arm) ;;       # an earlier analysis: the arm as a STEP (read-only checklist -> gfxneuter 260 -> read-back)
        iopparse-[0-9]*)                                reply=(a_iop_parse "${1#iopparse-}") ;;   # 0.0.322 : the HELD capability parse
        wswitness)                                      reply=(a_ws_witness) ;;                  # an earlier analysis: does WindowServer take the Metal compositor
        cqpoll-[0-9]*)                                  reply=(a_cq_poll "${1#cqpoll-}") ;;      # an earlier analysis: cqprobe + its positive control, polled
        sakill)                                         reply=(a_sa_kill TERM) ;;                # an earlier analysis: TEST S, SecurityAgent only
        sakill-kill)                                    reply=(a_sa_kill KILL) ;;                # an earlier analysis: sacomp2, SIGKILL, SecurityAgent only
        agdcreg-[A-Za-z0-9]*)                           reply=(agdc_reg "${1#agdcreg-}") ;;      # an earlier analysis: read-only AGDC / display-pipe registry
        m4adopt)                                        reply=(m4_adopt) ;;                      # M4 route c' wall 1 (notes/M4-FRAME-DELIVERY.md section 6)
        pipeguard-1)                                    reply=(hardtmo 20 sudo "$N48" accel pipeguard 1) ;;   # 0.0.286 : ARM the display-pipe safety core
        pipeguardread)                                  reply=(hardtmo 20 sudo "$N48" accel pipeguard) ;;     # read: refusals, pipe readiness fields, WRITE_DATA guard
        ucprobe-1)                                      reply=(hardtmo 20 sudo "$N48" accel ucprobe 1) ;;     # 0.0.333 : LOG-ONLY hook on the accelerator's newUserClient + the display-pipe client's externalMethod
        ucproberead)                                    reply=(hardtmo 20 sudo "$N48" accel ucprobe) ;;       # read: clients minted by class, selectors seen at IOAccelDisplayPipeUserClient2, pipe+0x2a4
        agdc-1)                                         reply=(agdc_publish) ;;                                # 0.0.286: publish the AGDC nub (console root, pipeguard armed)
        agdcread)                                       reply=(hardtmo 20 sudo "$N48" accel agdc) ;;
        fbbench)                                        reply=(hardtmo 60 sudo "$N48" accel fbbench) ;;       # 0.0.286 : in-kernel write throughput per cache mode
        fbbench-[0-9]*)                                 reply=(hardtmo 60 sudo "$N48" accel fbbench "${1#fbbench-}") ;;
        fbwc-1)                                         reply=(hardtmo 20 sudo "$N48" accel fbwc 1) ;;
        fbwcread)                                       reply=(hardtmo 20 sudo "$N48" accel fbwc) ;;
        pipemode)                                       reply=(hardtmo 20 sudo "$N48" accel pipemode) ;;      # 0.0.288: route B readiness, READ BACK the fields
        pipemode-1)                                     reply=(hardtmo 20 sudo "$N48" accel pipemode 1) ;;    # route B: write the readiness fields and set pipe+0x298
        pipeshim-1)                                     reply=(hardtmo 20 sudo "$N48" accel pipeshim 1) ;;    # participate + census every transaction, present nothing
        pipeshim-2)                                     reply=(hardtmo 20 sudo "$N48" accel pipeshim 2) ;;    # participate + SDMA0 QUEUE0 present of plane 0
        pipeshim-4)                                     PIPESHIM_TICK_ARG=4; reply=(hardtmo 20 sudo "$N48" accel pipeshim 4) ;;    # 0.0.412: mode 2 present with the swizzle-3 plane FORCED down the linear row copy (instrument for uniform test content). Against a kext older than 0.0.412, 4 returns status 2 and changes nothing.
        pipeshim-5)                                     PIPESHIM_TICK_ARG=5; reply=(hardtmo 20 sudo "$N48" accel pipeshim 5) ;;    # 0.0.416 (an internal design note G3/G4): mode 2 present with the SDMA GCR_REQ (GL2 write-back + invalidate) immediately before the tiled copy in the SAME submission. Against a kext older than 0.0.416, 5 returns status 2 and changes nothing.
        pipeshimread)                                   reply=(hardtmo 20 sudo "$N48" accel pipeshim 3) ;;    # 0.0.338: 3 = READ ONLY. A BARE `accel pipeshim` SENDS SCALAR 0, WHICH TURNS THE SHIM OFF -- run mmv1 caught this step disarming the shim one step after arming it, so the whole armed window ran with the guard refusing. Against an OLD kext, 3 returns status 2 and still changes nothing.
        routea-1)                                       reply=(a_routea_arm) ;;                                # 0.0.295 (reviewed routeA-implreview): ARM + VERIFY corrected route A
        routea-0)                                       reply=(hardtmo 20 sudo "$N48" accel routea 0) ;;       # runtime mode off (inert; reboot to fully restore)
        routearead)                                     reply=(hardtmo 20 sudo "$N48" accel routea) ;;         # read: armed/mode/verified, initFb/ready, guard, presents, scanout base/len
        emcensusoff)                                     reply=(hardtmo 20 sudo "$N48" accel emcensus 2) ;;    # 0.0.291 : DISARM the event-machine census (safety core stays armed); call BEFORE pipeguard-1
        emcensuson)                                      reply=(hardtmo 20 sudo "$N48" accel emcensus 1) ;;    # re-arm the census
        emcensusread)                                    reply=(hardtmo 20 sudo "$N48" accel emcensus) ;;      # read: policy, armed/verified, per-slot event-machine hit breakdown
        # 0.0.315: an explicit cap. `wscompose-<tag>.<seconds>` watches for that long instead of 90.
        # A brief that sets an armed-window bound (e.g. "<= 87 s") and a recipe whose own cap is 90 contradict each
        # other, and the recipe wins silently - runs m4c5 and m4c6 ran to t88 and t89 under a stated 87 s bound.
        # Matched BEFORE the bare form, because the bare pattern would swallow the dotted one.
        wscompose-[A-Za-z0-9]*.[0-9]|wscompose-[A-Za-z0-9]*.[0-9][0-9]|wscompose-[A-Za-z0-9]*.[0-9][0-9][0-9])
            reply=(a_ws_compose "${${1#wscompose-}%.*}" "${1##*.}") ;;
        wscompose-[A-Za-z0-9]*)                          reply=(a_ws_compose "${1#wscompose-}" 90) ;;          # 0.0.291: the monitored post-kill capture with the GFX-hang / FLIP / DCN / console / 90 s abort
        agdcprobe)                                      reply=(agdc_probe) ;;                                  # 0.0.286: tools/pc/agdc-probe.c, console-gated
        fbbenchuser-[a-z0-9]*)                          reply=(fbbench_user "${1#fbbenchuser-}") ;;             # 0.0.287 : userspace fbbench through our user client
        m4flush)                                        reply=(m4_flush 128) ;;                  # 0.0.272 : walls 2-3 test client
        m4flush-[0-9]*)                                 reply=(m4_flush "${1#m4flush-}") ;;
        m4flushlock0)                                   reply=(m4_flush 128 0) ;;                # 0.0.272 flush2 : lock in backing
        flushhook-[0-9]*)                               reply=(sudo "$N48" accel flushhook "${1#flushhook-}") ;;
        fbname-[0-9]*)                                  reply=(sudo "$N48" accel fbname "${1#fbname-}") ;;   # : the runtime class rename, 1 arms / 0 restores
        iopdump)                                        reply=(iop_dump) ;;                                 # : CoreDisplay's IOP capabilities dump
        flushhookread)                                  reply=(sudo "$N48" accel flushhook) ;;
        flushhookrej)                                   reply=(sudo "$N48" accel flushhook 4) ;;   # 0.0.275 : rejection counters
        keysgate)                                       reply=(keys_gate) ;;                        # 0.0.275: the pairing-keys positive control before wskill
        logstream-start)                                reply=(logstream_start) ;;                  # 0.0.276 : driver log to disk
        logstream-stop)                                 reply=(logstream_stop) ;;
        capstream-start)                                reply=(capstream_start) ;;                  # 0.0.282 : capture ring to disk
        capstream-stop)                                 reply=(capstream_stop) ;;
        gfxcapture-[0-9]*)                              reply=(hardtmo 20 sudo "$N48" accel gfxcapture "${1#gfxcapture-}") ;;
        gfxcaptureread)                                 reply=(hardtmo 20 sudo "$N48" accel gfxcapture) ;;
        gfxprobe-[0-9]*)                                reply=(hardtmo 20 sudo "$N48" accel gfxprobe "${1#gfxprobe-}") ;;   # 0.0.283 
        gfxproberead)                                   reply=(hardtmo 20 sudo "$N48" accel gfxprobe) ;;
        wssafe-[A-Za-z0-9]*)                            reply=(ws_safe "${1#wssafe-}") ;;           # 0.0.276: no accelerator-touching probe
        gfxcensus-[0-9]*)                               reply=(hardtmo 20 sudo "$N48" accel gfxcensus "${1#gfxcensus-}") ;;
        gfxcensusread)                                  reply=(hardtmo 20 sudo "$N48" accel gfxcensus) ;;
        stampgapsafe)                                   reply=(hardtmo 20 sudo "$N48" accel stampgap) ;;
        gfxneuter-[0-9]*)                               reply=(hardtmo 20 sudo "$N48" accel gfxneuter "${1#gfxneuter-}") ;;   # 0.0.278 
        gfxneuterread)                                  reply=(hardtmo 20 sudo "$N48" accel gfxneuter) ;;
        finishread)                                     reply=(hardtmo 20 sudo "$N48" accel finishread) ;;   # 0.0.279 
        neutergate-[0-9]*)                              reply=(neuter_gate "${1#neutergate-}") ;;
        copyongate)                                     reply=(copy_on_gate) ;;                     # 0.0.278: copy on only after flushes are seen
        scanout-[0-9]*)                                 reply=(sudo "$N48" accel scanout "${1#scanout-}") ;;
        scanoutread)                                    reply=(sudo "$N48" accel scanout 0) ;;
        # 0.0.417 (an internal design note, D4): the SDMA0_DCC_CNTL no-PTE compression set/restore. No
        # tick interaction - each is a one-shot verb. sdmadcc-1 captures-then-clears SDMA0 only; sdmadcc-2
        # restores; sdmadcc-0 reads SDMA0+SDMA1 raw and decoded.
        sdmadcc-0)                                      reply=(sudo "$N48" accel sdmadcc 0) ;;
        sdmadcc-1)                                      reply=(sudo "$N48" accel sdmadcc 1) ;;
        sdmadcc-2)                                      reply=(sudo "$N48" accel sdmadcc 2) ;;
        sleepuntilk-[0-9]*)                             reply=(a_sleep_until_kill "${1#sleepuntilk-}") ;;
        kextlog-[A-Za-z0-9]*)                           reply=(a_kext_log "${1#kextlog-}") ;;
        wslogstop)                                      reply=(p2_log_stop) ;;
        logmark)                                        reply=(p2_logmark mark) ;;
        logmark-[A-Za-z0-9]*)                           reply=(p2_logmark "${1#logmark-}") ;;
        wsstate)                                        reply=(p2_ws_state state) ;;
        wsstate-[A-Za-z0-9]*)                           reply=(p2_ws_state "${1#wsstate-}") ;;
        displayregistry)                                reply=(tmo 30 "$T/display-registry") ;;    # 0.0.237 : read what the in-kext boot chain did; arms nothing
        kernsub)                                        reply=(sudo "$N48" accel kernsub 1) ;;
        kerndiag)                                       reply=(sudo "$N48" accel kernsub 2) ;;
        kerndiagmin)                                    reply=(sudo "$N48" accel kernsub 3) ;;
        kernoffen)                                      reply=(sudo "$N48" accel kernsub 4) ;;
        shadercache)                                    reply=(sudo "$N48" accel shadercache 1) ;;
        shadercacheread)                                reply=(sudo "$N48" accel shadercache) ;;
        shadercacheoff)                                 reply=(sudo "$N48" accel shadercache 2) ;;
        shadercacheadj)                                 reply=(sudo "$N48" accel shadercache 3) ;;   # 0.0.265 
        ringmap)                                        reply=(sudo "$N48" accel ringmap 1) ;;   # 0.0.244 (an earlier analysis (i)/(ii)): reserve the tail, build OUR OWN L1 block + leaves, walk them. Writes NOTHING into Apple's page table
        ringmapread)                                    reply=(sudo "$N48" accel ringmap) ;;     # reserve + report only; builds nothing
        vmctx)                                          reply=(sudo "$N48" accel vmctx) ;;      # 0.0.247 : the OBSERVE BOOT of the root-write review 7.1. READ-ONLY - the VMM slot-40/41 hooks and this verb write NOTHING. Run it WHILE A CLIENT IS ALIVE: the root read out of the context object is only populated after the client's first mapVA
        rootwriteread)                                  reply=(sudo "$N48" accel rootwrite) ;;   # 0.0.250 : milestone 3 increment (iii) REPORT ONLY - evaluate guards G1..G6 against the live state and print what WOULD happen. Writes nothing anywhere
        rootwrite)                                      reply=(sudo "$N48" accel rootwrite 1) ;; # 0.0.250: PERFORM the single 8-byte root PDE write into Apple's live VMID-2 root slot 511, if and only if all six guards pass. RUN IT WHILE A METAL CLIENT IS ALIVE
        # 0.0.356 : rootwrite on WINDOWSERVER'S context, selected BY OWNER (creator pid/name
        # recorded at createVMContext, alive now under that name, the only such live context with a root);
        # CONTEXT2 agreement is a veto, gfx power is measured at the write, and VA 0x23f0000000 is walked through
        # the SELECTED root. `rootwritewsread` REPORTS ONLY (mode 2); `rootwritews` PERFORMS (mode 3) and is gated.
        rootwritewsread)                                reply=(hardtmo 20 sudo "$N48" accel rootwrite 2) ;;
        rootwritews)                                    reply=(rootwrite_ws) ;;
        wskillifok)                                     reply=(ws_kill_if_ok) ;;   # 0.0.356: a SECOND wskill, only on a healthy WindowServer
        rearmdrainread)                                 reply=(sudo "$N48" accel rearmdrain) ;;   # 0.0.261 : REPORT ONLY. Instrument C reads root[511] AFTER the sdmamap drain; instrument D dumps EVERY pending packet overlapping the root page with NO opcode filter, with a positive control. Writes nothing anywhere
        rearmdrain)                                     reply=(sudo "$N48" accel rearmdrain 1) ;; # 0.0.261: PERFORM the re-arm of root[511] AFTER Apple deferred clearWithDMA has drained through our own sdmamap takeover. Same single rootwrite_arm_context path, same six guards; refuses unless slot 511 reads EXACTLY zero
        vmpage-0x[0-9a-fA-F]*)                          reply=(sudo "$N48" accel vmpage "${1#vmpage-}") ;;
        blit2start)                                     reply=(blit2_start) ;;
        blit2start-[0-9]*)                              reply=(blit2_start "${1#blit2start-}") ;;
        blit2wait)                                      reply=(blit2_wait) ;;
        tristart-clear|tristart-tri|tristart-draw|tristart-drawinj|tristart-drawinstr|tristart-drawinjring|tristart-drawinstrring)
            reply=(tri_start "${1#tristart-}") ;;
        tristart-clear-[0-9]*|tristart-tri-[0-9]*|tristart-draw-[0-9]*|tristart-drawinj-[0-9]*|tristart-drawinstr-[0-9]*|tristart-drawinjring-[0-9]*|tristart-drawinstrring-[0-9]*)
            local mh=${1#tristart-}; reply=(tri_start "${mh%-*}" "${mh##*-}") ;;
        tridump-clear|tridump-draw|tridump-drawinj)     reply=(tri_dump "${1#tridump-}") ;;
        tristart-suitenw|tristart-suitenw.[a-z0-9.]*)   reply=(tri_start "${1#tristart-}") ;;          # 0.0.235 : the suite with NO blocking wait
        tristart-suitenw-[0-9]*|tristart-suitenw.[a-z0-9.]*-[0-9]*)
            local mw=${1#tristart-}; reply=(tri_start "${mw%-*}" "${mw##*-}") ;;
        tristart-suite|tristart-suite.[a-z0-9.]*)       reply=(tri_start "${1#tristart-}") ;;          # 0.0.224 
        tristart-suite-[0-9]*|tristart-suite.[a-z0-9.]*-[0-9]*)
            local ms=${1#tristart-}; reply=(tri_start "${ms%-*}" "${ms##*-}") ;;
        tridump-suite|tridump-suite.[a-z0-9.]*)         reply=(tri_dump "${1#tridump-}") ;;
        triwait)                                        reply=(tri_wait) ;;
        triwait-[0-9]*)                                 reply=(tri_wait "${1#triwait-}") ;;   # 0.0.334 / an earlier analysis: bounded warm-up wait
        renderib)                                       reply=(sudo "$N48" accel renderxlat 0) ;;
        renderblank)                                    reply=(sudo "$N48" accel renderxlat 3) ;;
        renderxlat)                                     reply=(sudo "$N48" accel renderxlat 1) ;;
        renderxlat-ngg)                                 reply=(sudo "$N48" accel renderxlat 0x101) ;;
        renderdraw)                                     reply=(sudo "$N48" accel renderxlat 4) ;;
        renderdrawcheck)                                reply=(sudo "$N48" accel renderxlat 5) ;;
        renderdrawring)                                 reply=(render_draw_ring 4) ;;
        renderdrawringcheck)                            reply=(render_draw_ring 5) ;;
        renderdrawringb)                                reply=(render_draw_ring 4 0x600) ;;
        renderdrawringbcheck)                           reply=(render_draw_ring 5 0x600) ;;
        renderdrawringc)                                reply=(render_draw_ring 4 0x1600) ;;
        renderdrawsuite|renderdrawsuite-0x[0-9a-fA-F]*)             reply=(render_draw_suite 6 "${1#renderdrawsuite}") ;;        # 0.0.224
        renderdrawsuitecheck|renderdrawsuitecheck-0x[0-9a-fA-F]*)   reply=(render_draw_suite 7 "${1#renderdrawsuitecheck}") ;;
        renderdrawsuitedesc|renderdrawsuitedesc-0x[0-9a-fA-F]*)     reply=(render_draw_suite 8 "${1#renderdrawsuitedesc}") ;;    # 0.0.232 
        renderdrawsuitedesccheck|renderdrawsuitedesccheck-0x[0-9a-fA-F]*) reply=(render_draw_suite 9 "${1#renderdrawsuitedesccheck}") ;;
        # 0.0.255 : modes 8/9 again, but with bit 0x100 and NO client ring base - the kext drives the
        # geometry rings from its own mapping. Drop-in for renderdrawsuitedesc[check], so the draw can be compared
        # byte for byte against the runs that used a client-allocated ring buffer.
        renderdrawsuiteown|renderdrawsuiteown-0x[0-9a-fA-F]*)         reply=(render_draw_suite_own 8 "${1#renderdrawsuiteown}") ;;
        renderdrawsuiteowncheck|renderdrawsuiteowncheck-0x[0-9a-fA-F]*) reply=(render_draw_suite_own 9 "${1#renderdrawsuiteowncheck}") ;;
        renderdrawringccheck)                           reply=(render_draw_ring 5 0x1600) ;;
        ringpages)                                      reply=(ring_pages) ;;
        rendergate)                                     reply=(render_gate) ;;
        sdmamap)  if (( GATE_CLOSED )); then reply=(skip_step sdmamap); else reply=(sudo "$N48" accel sdmamap); fi ;;
        *)                                              reply=(sudo "$N48" accel "$1") ;;
    esac
}

D=$RUNS/$RUN
TR=$D/transcript.txt
if (( DRY )); then
    print -r -- "DRY RUN — nothing below is executed."
    print -r -- "  mkdir -p $D"
    print -r -- "  [readiness]  sudo $N48 info            (must exit 0 and print 'Stage reached')"
    print -r -- "  [gate]       stat -f %Su /dev/console   (refuse unless 'root': never arm with a user logged in)"
    print -r -- "  [gate]       sudo $N48 accel status    (refuse if 'installed = yes' and no --force; force=$FORCE)"
    print -r -- "  [health]     ls /Library/Logs/DiagnosticReports | grep -c gpuRestart   -> ### gpuRestart-before"
    for s in "${PREFIRE[@]}"; do step_argv "$s"; print -r -- "  [prefire]    ${reply[*]}   (a failure ABORTS before fire)"; done
    print -r -- "  [gate]       stat -f %Su /dev/console   (again, immediately before fire)"
    print -r -- "  [step 2]     sudo $N48 accel fire"
    print -r -- "  [poll]       sudo $N48 accel status every 5 s until 'TTL calls so far' is unchanged in two consecutive polls (max 60 s)"
    i=2
    for s in "${STEPS[@]}"; do
        step_argv "$s"; i=$((i+1))
        print -r -- "  [step $i]     ${reply[*]}"
    done
    print -r -- "  [after]      sudo $N48 log                                   -> $D/driverlog.txt"
    print -r -- "  [after]      ioreg -rc Navi48Bringup -l -w0 | grep -E '\"Navi48,[A-Za-z0-9]+\" =' | sort  -> $D/health.txt"
    print -r -- "  [after]      ls /Library/Logs/DiagnosticReports | grep -c gpuRestart   -> ### gpuRestart-after"
    print -r -- "  [after]      ls -lt /Library/Logs/DiagnosticReports | head -3 ; uptime ; kmutil showloaded --list-only | grep -i navi48"
    print -r -- "  [after]      summarize $D                                    -> $D/summary.txt (printed to stdout)"
    if (( _wi )); then
        if (( _ai )); then
            print -r -- "  [arm order]  ARM GATE at step $(( _ai + 2 )) \`${STEPS[$_ai]}\` -> WindowServer restart at step $(( _wi + 2 )) \`${STEPS[$_wi]}\`: OK, the arm precedes the window."
            print -r -- "  [arm order]  \`wskill\` REFUSES (and sets LIVE_ABORT) unless the kext has printed \`ARM LEVEL IS 2 (COMMIT); one-shot ARMED\`."
        else
            print -r -- "  [arm order]  no \`armgate\`/\`commitarm\` in this list: judge-only, the restart at step $(( _wi + 2 )) is ungated."
        fi
    fi
    exit 0
fi

# fd 3 is this LIVE run's SUMMARY stream. accel-cycle.sh redirects the script's stdout to
# ~/navi48-runs/<run>.remote.txt, so duplicating it here lets the arm gate (arm_summary) speak to the
# operator watching that file without its 30 s ticks - which stay in transcript.txt - flooding it.
# Opened only here, after --dry-run and ACCEL_RUN_SUMMARY_ONLY have exited, and before any step runs.
exec 3>&1

[[ -x $N48 ]] || { print -r -- "ABORT: $N48 is not there — run tools/stage-to-pc.sh from the host Mac first"; exit 1 }

# Never clobber a previous capture: evidence is the whole point of this script.
if [[ -d $D && -n $(ls -A "$D" 2>/dev/null) ]]; then
    OLD=$D-old-$(date +%Y%m%d-%H%M%S)
    mv "$D" "$OLD"
    print -r -- "note: an earlier $D existed — moved to $OLD"
fi
mkdir -p "$D" || exit 1
: > "$TR"

step_no=0
run_step() {
    local name=$1
    step_argv "$name"
    local -a cmd; cmd=("${reply[@]}")
    step_no=$((step_no+1))
    local t0 t1 s e rc ms
    t0=$(wall); s=$(now_s)
    { print -r -- ""; print -r -- "===> STEP $step_no $name START $t0"; print -r -- "+ ${cmd[*]}" } >> "$TR"
    "${cmd[@]}" >> "$TR" 2>&1
    rc=$?
    e=$(now_s); t1=$(wall)
    ms=$(printf '%.0f' $(( (e - s) * 1000 )))
    print -r -- "===> STEP $step_no $name END rc=$rc ms=$ms $t1" >> "$TR"
    print -r -- "  step $step_no $name rc=$rc ${ms}ms"
    LAST_RC=$rc
    return $rc
}

{
  print -r -- "### accel-run $RUN on $(hostname -s 2>/dev/null) at $(date '+%F %T')  force=$FORCE"
  if (( ${#PREFIRE} )); then print -r -- "### steps: [prefire: ${PREFIRE[*]}] fire ${STEPS[*]}"
  else print -r -- "### steps: fire ${STEPS[*]}"; fi
  print -r -- "### uptime: $(uptime)"
} >> "$TR"

# --- readiness: the SERVICE, not the shell -----------------------------------
run_step info
if (( LAST_RC != 0 )) || ! grep -q 'Stage reached' "$TR"; then
    print -r -- "ABORT: readiness failed — 'navi48test info' rc=$LAST_RC and/or no 'Stage reached' line."
    print -r -- "       Not a boot race if wait-for-driver.sh already passed; investigate. See $TR"
    exit 1
fi

# --- console user: never arm with someone logged in (notes sections 332-333) ------
# Once Apple's accelerator is armed every Metal client blocks (run disp2: Safari, WebKit.GPU,
# NotificationCenter, the display probe all in U state), and a reboot of that boot froze.
cu=$(stat -f %Su /dev/console 2>&1)
{ print -r -- ""; print -r -- "===> GATE console user $(wall): $cu"; who 2>&1 } >> "$TR"
if [[ $cu != root ]]; then
    print -r -- "REFUSING: '$cu' is logged in at the console - user logged in, ask them to log out."
    print -r -- "          Never arm the accelerator with a console user (notes sections 332-333)."
    exit 1
fi

# --- one measurement per boot ------------------------------------------------
status_now=$(sudo "$N48" accel status 2>&1)
{ print -r -- ""; print -r -- "===> GATE accel status (pre-fire) $(wall)"; print -r -- "$status_now" } >> "$TR"
if print -r -- "$status_now" | grep -q 'installed = yes'; then
    if (( FORCE )); then
        print -r -- "### WARNING: accelerator already fired this boot; --force given, continuing" >> "$TR"
        print -r -- "WARNING: already fired this boot — --force given, this is NOT a clean measurement"
    else
        print -r -- "REFUSING: accel status already says 'installed = yes' — one measurement per boot."
        print -r -- "          Reboot, or pass --force if you meant to re-run on a used accelerator."
        exit 1
    fi
fi

# --- health BEFORE -----------------------------------------------------------
# The DiagnosticReports directory keeps a capped set (40 gpuRestart reports seen) and rotates the
# oldest out, so a before/after COUNT reads 'no new report' during a reset storm (r41).
# New reports are counted by modification time against a marker touched here.
GPU_BEFORE=$(ls /Library/Logs/DiagnosticReports 2>/dev/null | grep -c gpuRestart)
touch "$D/.run-start"

# - THE COMPLETION BLIND SPOT. `Navi48,IRQEop` / `Navi48,IRQFaults` exist (Navi48Bringup.cpp:3624-3625, published by
# publishInterruptCounters) but were published ONLY from start() (:3684), so every ioreg read - including health.txt at the
# end of this script - returned the BOOT-TIME snapshot. makes `accel status` re-publish them, so this pair of reads is
# a real measurement: the run's EOP and fault counts are post MINUS pre. These counters are CUMULATIVE - never add two reads.
irq_snapshot() {
    local tag=$1
    {
        print -r -- "### irq-$tag $(wall)"
        hardtmo 20 sudo "$N48" accel status 2>&1 | grep -E 'TTL calls|installed' || true
        ioreg -rc Navi48Bringup -l -w0 2>/dev/null | grep -E '"Navi48,IRQ[A-Za-z]+" =' | sed -E 's/^[ |]+//' | sort
    } > "$D/irq-$tag.txt" 2>&1
    grep -E 'IRQEop|IRQFaults|IRQCount' "$D/irq-$tag.txt" 2>/dev/null | tr '\n' ' '
}
print -r -- "===> IRQ COUNTERS BEFORE: $(irq_snapshot pre)" >> "$TR"

# --- prefire steps (0.0.267, an earlier analysis) -------------------------------
for s in "${PREFIRE[@]}"; do
    run_step "$s"
    if (( LAST_RC != 0 )); then
        print -r -- "===> PREFIRE $s FAILED rc=$LAST_RC - ABORTING WITHOUT FIRING" >> "$TR"
        print -r -- "ABORT: prefire step $s failed (rc=$LAST_RC) - nothing armed. See $TR"
        p2_stop_streams >> "$TR" 2>&1
        exit 1
    fi
done
if (( ${#PREFIRE} )); then
    cu=$(stat -f %Su /dev/console 2>&1)
    print -r -- "===> GATE console user immediately before fire $(wall): $cu" >> "$TR"
    if [[ $cu != root ]]; then
        print -r -- "REFUSING: '$cu' is logged in at the console (checked immediately before fire) - nothing armed."
        p2_stop_streams >> "$TR" 2>&1
        exit 1
    fi
fi

# --- fire, then wait for the TTL count to plateau ----------------------------
run_step fire
prev=""; stable=0; waited=0; cur=""
while (( waited < 60 )); do
    sleep 5; waited=$((waited+5))
    out=$(sudo "$N48" accel status 2>&1)
    { print -r -- ""; print -r -- "===> POLL accel status t+${waited}s $(wall)"; print -r -- "$out" } >> "$TR"
    cur=$(print -r -- "$out" | sed -n 's/.*TTL calls so far[^:]*: *//p' | tail -1)
    if [[ -n $cur && $cur == $prev ]]; then stable=$((stable+1)); else stable=0; fi
    prev=$cur
    (( stable >= 2 )) && break
done
if (( stable >= 2 )); then
    print -r -- "===> FIRE SETTLED TTL calls = $cur after ${waited}s" >> "$TR"
    print -r -- "  fire settled: TTL calls = $cur after ${waited}s"
else
    print -r -- "===> FIRE NOT SETTLED after ${waited}s, last TTL calls = ${cur:-?}" >> "$TR"
    print -r -- "  WARNING: TTL count never plateaued in ${waited}s (last = ${cur:-?}); continuing"
fi

# --- the steps ---------------------------------------------------------------
for s in "${STEPS[@]}"; do
    # 0.0.279 (rule 94): the console is checked before EVERY step once armed, not only at the gates. A login ends the
    # measurement: every later step that arms, launches, kills or waits is skipped, and only the reads and stream stops run.
    if (( ! CONSOLE_ABORT )); then
        cu=$(stat -f %Su /dev/console 2>&1)
        # 0.0.280: /dev/console belongs to _windowserver for a few seconds while a restarted WindowServer comes up;
        # wsneuter2 took that for a login and skipped every wait. Only a real account is a login.
        if [[ $cu != (root|_windowserver) ]]; then
            CONSOLE_ABORT=1; LIVE_ABORT=1
            print -r -- "===> CONSOLE LOGIN DETECTED on the ARMED boot before step $s: console user '$cu' at $(wall) - the run is ABORTED, only capture steps run (rule 94); report 'user logged in - ask them to log out' and reboot only once the console is root again" >> "$TR"
            print -r -- "  CONSOLE LOGIN DETECTED ('$cu') before $s - run aborted, capturing only"
        fi
    fi
    if (( CONSOLE_ABORT )) && [[ $s != (logstream-stop|capstream-stop|gfxcaptureread|gfxproberead|pagecopy|wslogstop|wssafe-*|kextlog-*|gfxneuterread|gfxcensusread|emcensusread|stampgapsafe|ht-*|drain|counters|flushhookread|flushhookrej|eopbridgeread|shadercacheread|scanoutread|finishread|reg-0x*|parkreads|wscompose-*) ]]; then
        print -r -- "===> SKIPPED $s: console login on the armed boot (rule 94) $(wall)" >> "$TR"
        print -r -- "  SKIPPED $s (console login, rule 94)"
        continue
    fi
    #: `wswitness` and `iopparse-*` RESTART WindowServer exactly as `wskill` does, but neither was in this
    # list, so a_routea_arm's own message ("wskill and later client steps SKIPPED") was false for the two recipes
    # that use them - a half-armed route A could still be carried across a restart. Both are now gated.
    if (( LIVE_ABORT )) && [[ $s == (blit*|tri*|soak-*|m4*|wskill|wskillifok|rootwritews|wswitness|iopparse-*|sakill|copyongate|neutergate-*|agdc-1|agdcprobe|armgate|armgate-*|commitarm) ]]; then
        print -r -- "===> SKIPPED $s: LIVE_ABORT is set - read the cause line above; a console user is only ONE cause (CONSOLE_ABORT prints its own line) $(wall)" >> "$TR"
        print -r -- "  SKIPPED $s (console user appeared)"
        continue
    fi
    run_step "$s"
done

# a stream left running (no wslogstop in the list, or it failed) is stopped here, never left behind
if [[ -f $D/.wslog.pid || -f $D/.kernlog.pid ]]; then
    print -r -- "===> CLEANUP streams still running at the end of the step list $(wall)" >> "$TR"
    p2_stop_streams >> "$TR" 2>&1
fi

# --- capture the driver log and the health snapshot --------------------------
sudo "$N48" log > "$D/driverlog.txt" 2> "$D/driverlog.stderr.txt"; cat "$D/driverlog.stderr.txt" >> "$D/driverlog.txt"   # trailer goes to stderr; keep it after the log body, not spliced into it
print -r -- "===> IRQ COUNTERS AFTER: $(irq_snapshot post)" >> "$TR"   #: the other half of the delta
GPU_AFTER=$(ls /Library/Logs/DiagnosticReports 2>/dev/null | grep -c gpuRestart)
{
  print -r -- "### gpuRestart-before $GPU_BEFORE"
  print -r -- "### gpuRestart-after $GPU_AFTER"
  print -r -- "### gpuRestart-new-by-mtime $(find /Library/Logs/DiagnosticReports -name '*.gpuRestart' -newer "$D/.run-start" 2>/dev/null | wc -l | tr -d ' ')"
  find /Library/Logs/DiagnosticReports -name '*.gpuRestart' -newer "$D/.run-start" 2>/dev/null | sort | head -40
  # 0.0.264 : attribute each report to a BOOT, not to a file mtime alone.
  # M3-REARM-SITE section 6: r98's "two in fifteen minutes" counted one report that belongs to
  # r97's boot - it was written seven minutes AFTER r97's capture closed, so r97 honestly
  # reported "no new report" and r98 then counted it against windows that had no such tail.
  # That is not comparing like with like, and it is a false rate signal that would otherwise
  # steer the next decision. kern.boottime separates them and costs nothing.
  # TWO BUGS were caught here by testing this block against real output before the run, and
  # both would have corrupted the very signal it exists to fix. Recorded so neither returns:
  #
  #  1. The obvious parse `s/.*sec *= *([0-9]+).*/\1/` is WRONG. sysctl prints
  #     `{ sec = 1789410684, usec = 366110 } ...` and `.*sec` is greedy, so it matches through
  #     "u-sec" and captures the MICROSECONDS - 366110, not an epoch. Every report would then
  #     compare as newer than "boot", manufacturing exactly the false rate signal this removes.
  #     The pattern is therefore anchored on the literal brace, which "usec" cannot match.
  #  2. The zsh glob qualifier form `*.gpuRestart(N)` FAILED in testing ("no matches found"),
  #     and `zsh -n` still passes it because that checks syntax only. This uses find/stat/awk
  #     instead - no glob qualifiers, no shell options to depend on - and returns a clean 0
  #     when the directory holds nothing.
  BOOT_EPOCH=$(sysctl -n kern.boottime 2>/dev/null | sed -E 's/^\{[[:space:]]*sec[[:space:]]*=[[:space:]]*([0-9]+).*/\1/')
  print -r -- "### kern-boottime ${BOOT_EPOCH:-unknown}"
  print -r -- "### gpuRestart-this-boot $(find /Library/Logs/DiagnosticReports -name '*.gpuRestart' -exec stat -f '%m' {} + 2>/dev/null | awk -v b="${BOOT_EPOCH:-0}" '$1 >= b' | wc -l | tr -d ' ')"
  print -r -- "### gpuRestart-report-ages (mtime epoch, date, name; boot epoch ${BOOT_EPOCH:-?} - anything BELOW it belongs to a PREVIOUS boot and is NOT this run's)"
  find /Library/Logs/DiagnosticReports -name '*.gpuRestart' -exec stat -f '    %m  %Sm  %N' {} + 2>/dev/null | sort -rn | head -12
  #: both halves of the IRQ delta, side by side and labelled CUMULATIVE so no one sums them.
  print -r -- "### irq-delta (CUMULATIVE counters - the run's figure is AFTER minus BEFORE, never a sum)"
  cat "$D/irq-pre.txt" 2>/dev/null || print -r -- "(no irq-pre.txt)"
  cat "$D/irq-post.txt" 2>/dev/null || print -r -- "(no irq-post.txt)"
  print -r -- "### ioreg Navi48 properties"
  ioreg -rc Navi48Bringup -l -w0 2>/dev/null | grep -E '"Navi48,[A-Za-z0-9]+" =' | sed -E 's/^[ |]+//' | sort
  print -r -- "### newest DiagnosticReports"
  ls -lt /Library/Logs/DiagnosticReports 2>/dev/null | head -3
  print -r -- "### uptime"
  uptime
  print -r -- "### kmutil"
  kmutil showloaded --list-only 2>/dev/null | grep -i navi48 || print -r -- "(no navi48 kext in kmutil showloaded)"
} > "$D/health.txt" 2>&1

print -r -- ""
print -r -- "=== summary ($D/summary.txt) ==="
summarize "$D"
