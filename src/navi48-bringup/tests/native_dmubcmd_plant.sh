#!/bin/zsh
# native_dmubcmd_plant.sh - planted breaks for tests/native_dmubcmd_test.cpp (build 0.0.625, multi-monitor stages M2 / M3: dmubsend / dmubmode / dmubctx - the first verbs that send to the display firmware).
# Discipline (as native_disp_plant.sh): copy the sources the test reads into a scratch tree, apply ONE break to the REAL code (a header, the flow, the kernel glue, the dispatch, the CLI, the plist),
# build the real test against the copy and demand it FAILS (a compile error, a failing check, a crash or a hang all count). A plant whose text is not found exactly once is itself reported as an
# escape. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_dmubcmd_plant.sh [first-plant-id last-plant-id]
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec python3 -u - "$ROOT" "${1:-0}" "${2:-9999}" <<'PY'
import os, shutil, subprocess, sys, tempfile, time

ROOT, FIRST, LAST = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
K = 'src/navi48-bringup'
H, F, G, BRG, PURE, UCL, CLI, PLIST, S1C, DRH = (f'{K}/src/dcn/navi48_dmubcmd.h', f'{K}/src/dcn/navi48_dmubcmd_flow.h', f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/Navi48Bringup.cpp', f'{K}/src/amd/native_disp_pure.h',
    f'{K}/src/Navi48UserClient.cpp', 'tools/pc/navi48test.c', f'{K}/Info.plist', f'{K}/src/amd/native_s1c.h', f'{K}/src/dcn/navi48_dispread.h')
COPY_DIRS = [f'{K}/src/amd', f'{K}/src/dcn', 'src/dcn41']
COPY_FILES = [BRG, UCL, CLI, PLIST, S1C, f'{K}/src/Navi48MetalOps.h', f'{K}/src/Navi48DisplayOps.h', f'{K}/src/Navi48NativeABI.h', f'{K}/src/amd/native_m6_pure.h', f'{K}/src/apple/display_pipe_guard.h', f'{K}/Makefile', f'{K}/tests/native_dmubcmd_test.cpp', f'{K}/tests/native_dmubcmd_plant.sh']

scr = tempfile.mkdtemp(prefix='ndmubcmd-plant.')
def fresh():
    assert os.path.basename(scr).startswith('ndmubcmd-plant.') and os.path.isdir(scr), 'refusing to rmtree a directory that is not this script\'s own temp dir'
    shutil.rmtree(scr, ignore_errors=True)
    for d in COPY_DIRS:
        shutil.copytree(os.path.join(ROOT, d), os.path.join(scr, d), ignore=shutil.ignore_patterns('*.o', 'build*'))
    for f in COPY_FILES:
        d = os.path.join(scr, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(ROOT, f), d)
def build_run():
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', f'{K}/src', '-I', f'{K}/src/dcn', '-I', f'{K}/src/amd', '-I', 'src/dcn41',
           f'{K}/tests/native_dmubcmd_test.cpp', '-x', 'c++', 'src/dcn41/dcn41_allow.c', '-o', os.path.join(scr, 't')]
    r = subprocess.run(cmd, cwd=scr, capture_output=True, text=True)
    if r.returncode != 0:
        first = next((l for l in (r.stderr + r.stdout).splitlines() if 'error' in l), '')
        return 2, 'CAUGHT at compile time: ' + first[:140]
    try:
        r = subprocess.run([os.path.join(scr, 't'), '.'], cwd=scr, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 1, 'CAUGHT by a hang'
    if r.returncode == 0: return 0, 'the suite passed: ' + r.stdout.strip().splitlines()[-1]
    if r.returncode < 0: return 1, 'CAUGHT by a crash (signal %d)' % -r.returncode
    fails = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
    return 1, 'CAUGHT by %d check(s); first: %s' % (len(fails), (fails[0] if fails else r.stdout.strip().splitlines()[-1])[:120])

total = escaped = 0
def control():
    global total, escaped
    fresh(); total += 1
    rc, msg = build_run()
    if rc != 0: print('CONTROL: *** FAILED UNMODIFIED (%s) ***' % msg); escaped += 1
    else: print('CONTROL: ' + msg)
def plant(pid, path, desc, old, new):
    global total, escaped
    if pid < FIRST or pid > LAST: return
    fresh(); total += 1
    f = os.path.join(scr, path); s = open(f).read()
    n = s.count(old)
    if n != 1:
        print('PLANT %d: %s: PLANT TEXT NOT FOUND EXACTLY ONCE (%d)' % (pid, desc, n)); escaped += 1; return
    open(f, 'w').write(s.replace(old, new))
    t0 = time.time(); rc, msg = build_run(); dt = time.time() - t0
    if rc == 0: print('PLANT %d: %s: *** ESCAPED (%s) *** [%.1fs]' % (pid, desc, msg, dt)); escaped += 1
    else: print('PLANT %d: %s: %s [%.1fs]' % (pid, desc, msg, dt))

control()
# ---- the pure decisions (navi48_dmubcmd.h) -------------------------------------------------------------------------------------------------------------------------------------
plant(1, H, 'the header allowlist is widened (sub 9 accepted)', 'return h == N48DM_HDR_DETECT || h == N48DM_HDR_BEGIN || h == N48DM_HDR_SETMODE || h == N48DM_HDR_ENABLE || h == N48DM_HDR_DISABLE;', 'return h == N48DM_HDR_DETECT || h == N48DM_HDR_BEGIN || h == N48DM_HDR_SETMODE || h == N48DM_HDR_ENABLE || h == N48DM_HDR_DISABLE || h == 0x04000980u;')
plant(2, H, 'the d1 allowlist is widened (context 0x7400 accepted)', 'static inline int n48dm_ctx3_ok(uint32_t c) { return c == N48DM_CTX_DFP2 || c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }', 'static inline int n48dm_ctx3_ok(uint32_t c) { return c == N48DM_CTX_DFP2 || c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4 || c == 0x7400u; }')
plant(3, H, 'dmubctx accepts the live DP context 0x6800', 'static inline int n48dm_ctx_ok(uint32_t c) { return c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }', 'static inline int n48dm_ctx_ok(uint32_t c) { return c == N48DM_CTX_DFP2 || c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }')
plant(4, H, 'dmubctx accepts a third context (0x7400)', 'static inline int n48dm_ctx_ok(uint32_t c) { return c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }', 'static inline int n48dm_ctx_ok(uint32_t c) { return c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4 || c == 0x7400u; }')
plant(5, H, 'the boot-arg latch turns on for any non-zero value', 'return (present && value == 1u) ? N48DM_LATCH_ON : N48DM_LATCH_OFF;', 'return (present && value != 0u) ? N48DM_LATCH_ON : N48DM_LATCH_OFF;')
plant(6, H, 'the latch is ON when unset', 'static inline int n48dm_latch_is_on(uint32_t latch) { return latch == N48DM_LATCH_ON; }', 'static inline int n48dm_latch_is_on(uint32_t latch) { return latch != N48DM_LATCH_OFF; }')
plant(7, H, 'the busy check is gone (WPTR != RPTR accepted)', '    if (p->wptr != p->rptr) return N48DR_RING_BUSY;\n', '')
plant(8, H, 'the wrap check is gone', '    if (p->wptr + N48DM_SLOT_BYTES >= p->size) return N48DR_RING_WRAP;', '    if (0) return N48DR_RING_WRAP;')
plant(9, H, 'the wrap check is off by one (WPTR + 0x40 == size allowed)', 'if (p->wptr + N48DM_SLOT_BYTES >= p->size) return N48DR_RING_WRAP;', 'if (p->wptr + N48DM_SLOT_BYTES > p->size) return N48DR_RING_WRAP;')
plant(10, H, 'the DMCUB soft-reset state is not checked', 'if (!p->enabled || p->soft_reset) return N48DR_DMUB_NOT_READY;', 'if (!p->enabled) return N48DR_DMUB_NOT_READY;')
plant(11, H, 'the ring placement is not checked (ring_at_base ignored)', 'if (!p->ring_region4 || !p->ring_at_base) return N48DR_RING_NOT_REGION4;', 'if (!p->ring_region4) return N48DR_RING_NOT_REGION4;')
plant(12, H, 'the poll bound is doubled', '#define N48DM_POLL_MAX 10000u', '#define N48DM_POLL_MAX 20000u')
plant(13, H, 'the poll step is doubled (200 ms)', '#define N48DM_POLL_STEP_US 10u', '#define N48DM_POLL_STEP_US 20u')
plant(14, H, 'the slot carries a leftover in dword 3', '    slot[0] = h; slot[1] = d1; slot[2] = d2;', '    slot[0] = h; slot[1] = d1; slot[2] = d2; slot[3] = d1;')
plant(15, H, 'a mode write needs no prior save', '    return s->saved ? 0u : (uint32_t)N48DR_NO_SAVE;', '    return 0u;')
plant(16, H, 'a second save may overwrite the original', '    if (op == N48DM_MODE_SAVE) return s->dirty ? (uint32_t)N48DR_SAVE_DIRTY : 0u;', '    if (op == N48DM_MODE_SAVE) return 0u;')
plant(17, H, 'the 1080p block is wrong (size dword)', '0x08081008u, 0x00000008u, 0x3a02007fu,', '0x08081008u, 0x00000008u, 0x3a02007eu,')
plant(18, H, 'the DP block is wrong (size dword)', '0x08081008u, 0x00000008u, 0x000000e2u,', '0x08081008u, 0x00000008u, 0x000000e3u,')
plant(19, H, 'dmubmode accepts another VBE mode (0x1d9)', 'return op == N48DM_MODE_SAVE || op == N48DM_MODE_RESTORE || op == N48DM_VBE_1080P || op == N48DM_VBE_DP;', 'return op == N48DM_MODE_SAVE || op == N48DM_MODE_RESTORE || op == N48DM_VBE_1080P || op == N48DM_VBE_DP || op == 0x1d9u;')
plant(20, H, 'the WPTR register offset is wrong', '#define N48DM_REG_INBOX1_WPTR 0x01d6u', '#define N48DM_REG_INBOX1_WPTR 0x01d8u')
plant(21, H, 'the begin command carries d2 = 0', 'case N48DM_K_BEGIN:   *h = N48DM_HDR_BEGIN;   *d1 = 0u;  *d2 = N48DM_D2_BEGIN; return 0u;', 'case N48DM_K_BEGIN:   *h = N48DM_HDR_BEGIN;   *d1 = 0u;  *d2 = 0u; return 0u;')
plant(22, H, 'setmode names the wrong mode-block offset', 'case N48DM_K_SETMODE: *h = N48DM_HDR_SETMODE; *d1 = N48DM_MODE_BLOCK; *d2 = ctx; return 0u;', 'case N48DM_K_SETMODE: *h = N48DM_HDR_SETMODE; *d1 = 0x4c20u; *d2 = ctx; return 0u;')
plant(23, H, 'the VBIOS handshake byte offset is wrong (the variables move)', '#define N48DM_VAR_RPTR     0x3118u', '#define N48DM_VAR_RPTR     0x311cu')
# ---- the flow (navi48_dmubcmd_flow.h) ---------------------------------------------------------------------------------------------------------------------------------------------
plant(30, F, 'no read-back compare of the slot', '    for (i = 0; i < N48DM_SLOT_DWORDS; i++)\n        if (back[i] != slot[i]) { o.status = N48DR_READBACK_MISMATCH; return o; }\n', '')
plant(31, F, 'the WPTR is written again on a timeout (retry)', '    if (!done) { o.status = N48DR_POLL_TIMEOUT; return o; }', '    if (!done) { (void)e.wrWptr(next); o.status = N48DR_POLL_TIMEOUT; return o; }')
plant(32, F, 'a timeout falls through to the variables write', '    if (!done) { o.status = N48DR_POLL_TIMEOUT; return o; }', '    if (!done) { o.status = N48DR_POLL_TIMEOUT; }')
plant(33, F, 'the WPTR is bumped right after the slot write, before the compare', '    if (!e.wr(base + wptr0, slot, N48DM_SLOT_DWORDS)) { o.status = N48DR_SLOT_WRITE_FAILED; return o; }', '    if (!e.wr(base + wptr0, slot, N48DM_SLOT_DWORDS)) { o.status = N48DR_SLOT_WRITE_FAILED; return o; }\n    (void)e.wrWptr(next);')
plant(34, F, 'the idle re-check is gone', '    if (e.wptr() != wptr0) { o.status = N48DR_RING_BUSY; return o; }', '    if (false) { o.status = N48DR_RING_BUSY; return o; }')
plant(35, F, 'the poll runs one read too many', 'for (uint32_t n = 0; n < pollMax; n++) {', 'for (uint32_t n = 0; n <= pollMax; n++) {')
plant(36, F, 'the +0x3118 variable gets the new WPTR instead of the old RPTR', 'const uint32_t vars[3] = { N48DM_VAR_CMDSIZE_VALUE, rptr0, next };', 'const uint32_t vars[3] = { N48DM_VAR_CMDSIZE_VALUE, next, next };')
plant(37, F, 'the variables are not read back', ' || !e.rd(base + N48DM_VAR_LASTSIZE, vback, 3u) || vback[0] != vars[0] || vback[1] != vars[1] || vback[2] != vars[2]) {', ' || !e.rd(base + N48DM_VAR_LASTSIZE, vback, 3u) || (vback[0] ^ vback[0]) != 0u) {')
plant(38, F, 'the window base ignores the REGION4 enable bit', 'return n48dr_region4_base(p.r4Lo, p.r4Hi, p.r4Enabled, p.fbBaseMc, p.vramSize, base);', 'return n48dr_region4_base(p.r4Lo, p.r4Hi, 1u, p.fbBaseMc, p.vramSize, base);')
plant(39, F, 'the OTG0 frame witness is not read', '    o.frames0 = e.frames();', '    o.frames0 = 0u;')
plant(40, F, 'the allowlist is checked AFTER the probe (reads happen first)', '    uint32_t st = 0u;\n    if (tpl) {\n        if (replay || !tp) st = N48DR_PAYLOAD_REFUSED;\n        else { hdr = tp->dw[0]; d1 = tp->dw[1]; d2 = tp->dw[2]; o.hdr = hdr; o.d1 = d1; o.d2 = d2; }\n    }\n    else if (!replay) st = n48dm_slot_check(hdr, d1, d2);\n    else if ((replaySrc % N48DM_SLOT_BYTES) != 0u || replaySrc >= N48DM_RING_SIZE) st = N48DR_REPLAY_REFUSED;\n    if (st != 0u) { o.status = st; return o; }\n\n    // ---- 2. the live state (reads only) and the window base\n    Probe p;\n    if (!e.probe(&p)) { o.status = N48DR_NO_DEVICE; return o; }', '    Probe p;\n    if (!e.probe(&p)) { o.status = N48DR_NO_DEVICE; return o; }\n    uint32_t st = 0u;\n    if (tpl) {\n        if (replay || !tp) st = N48DR_PAYLOAD_REFUSED;\n        else { hdr = tp->dw[0]; d1 = tp->dw[1]; d2 = tp->dw[2]; o.hdr = hdr; o.d1 = d1; o.d2 = d2; }\n    }\n    else if (!replay) st = n48dm_slot_check(hdr, d1, d2);\n    if (st != 0u) { o.status = st; return o; }')
plant(41, F, 'dmubmode writes without comparing', '    for (unsigned i = 0; i < N48DM_MODE_DWORDS; i++)\n        if (r.block[i] != words[i]) { r.status = N48DR_READBACK_MISMATCH; return r; }\n', '')
plant(42, F, 'dmubmode forgets the dirty flag on a VBE write', '    if (op != N48DM_MODE_RESTORE) { s.dirty = 1u; r.dirty = 1u; }', '    if (op != N48DM_MODE_RESTORE) { r.dirty = 1u; }')
plant(43, F, 'dmubctx never compares the read-back', '    r.status = r.after == r.value ? (uint32_t)N48DR_OK : (uint32_t)N48DR_READBACK_MISMATCH;', '    r.status = N48DR_OK;')
plant(44, F, 'dmubctx gate is skipped', '    uint32_t st = n48dm_ctx_gate(arg);\n    if (st != 0u) { r.status = st; return r; }', '    uint32_t st = 0u;')
plant(45, F, 'dmubmode save writes the window too', '        s.saved = 1u; s.dirty = 0u;', '        s.saved = 1u; s.dirty = 0u; (void)e.wr(base + N48DM_MODE_BLOCK, r.before, N48DM_MODE_DWORDS);')
# ---- the kernel glue (navi48_dcn.cpp) ----------------------------------------------------------------------------------------------------------------------------------------
plant(50, G, 'the OFF switch is ignored', '	if (!dm_latched_on()) { out[0] = N48DR_CMD_OFF; return N48DR_CMD_OFF; }\n', '')
plant(51, G, 'the OFF switch comes after the device test', '	if (!dm_latched_on()) { out[0] = N48DR_CMD_OFF; return N48DR_CMD_OFF; }\n	if (gLr.state != N48LR_READY || (needBound && (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev))) { out[0] = N48DR_NO_DEVICE; return N48DR_NO_DEVICE; }', '	if (gLr.state != N48LR_READY || (needBound && (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev))) { out[0] = N48DR_NO_DEVICE; return N48DR_NO_DEVICE; }\n	if (!dm_latched_on()) { out[0] = N48DR_CMD_OFF; return N48DR_CMD_OFF; }')
plant(52, G, 'the WPTR write skips the allowlist', 'if (a == DCN41_BAD_OFFSET || !dcn41_allow_write(&gDcn.allow, a, v, "dmubsend")) {', 'if (a == DCN41_BAD_OFFSET) {')
plant(53, G, 'a second register is written', '		amdgpu::WREG32(*gDcn.dev, a, v);\n		return true;', '		amdgpu::WREG32(*gDcn.dev, a, v);\n		amdgpu::WREG32(*gDcn.dev, a + 1u, v);\n		return true;')
plant(54, G, 'the register written is RPTR', 'const uint32_t a = dcn41_abs(&gDcn.d, N48DM_REG_INBOX1_WPTR, N48DM_REG_BASE_IDX);', 'const uint32_t a = dcn41_abs(&gDcn.d, N48DM_REG_INBOX1_RPTR, N48DM_REG_BASE_IDX);')
plant(55, G, 'dmubmode skips the latch', 'uint32_t dmubMode(uint64_t arg, uint64_t *out, unsigned outCount) {\n	const uint32_t early = dm_enter(out, outCount, false);', 'uint32_t dmubMode(uint64_t arg, uint64_t *out, unsigned outCount) {\n	const uint32_t early = 0u;')
plant(56, G, 'dmubctx skips the latch', 'uint32_t dmubCtx(uint64_t arg, uint64_t *out, unsigned outCount) {\n	const uint32_t early = dm_enter(out, outCount, false);', 'uint32_t dmubCtx(uint64_t arg, uint64_t *out, unsigned outCount) {\n	const uint32_t early = 0u;')
plant(57, G, 'the ring-at-base test is always true', 'p->pre.ring_at_base = (st.ring_addr != 0ull && st.ring_addr == (((uint64_t)st.region4_offset_high << 32) | st.region4_offset)) ? 1u : 0u;', 'p->pre.ring_at_base = 1u;')
plant(58, G, 'the REGION4 enable expression differs from region4read', 'p->r4Enabled = (st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u ? 1u : 0u;', 'p->r4Enabled = 1u;')
plant(59, G, 'the verbs are not single-flight', '	if (__atomic_exchange_n(&gDmBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { out[0] = N48DR_BUSY; return N48DR_BUSY; }', '')
plant(60, G, 'the boot-arg is read under the wrong name', 'PE_parse_boot_argn("navi48-dmubcmd", &v, sizeof(v))', 'PE_parse_boot_argn("navi48-dmub", &v, sizeof(v))')
plant(61, G, 'attach() no longer latches at start', '	(void)dm_latched_on();                                   // 0.0.625: latch navi48-dmubcmd at start (no register, no memory touched)\n', '')
plant(62, G, 'the poll waits through a raw IODelay', '	void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }\n	uint64_t now_us() { return now_ns() / 1000ull; }', '	void delay_us(uint32_t us) { IODelay(us); }\n	uint64_t now_us() { return now_ns() / 1000ull; }')
# ---- the dispatch, the exemption, the CLI, the version ----------------------------------------------------------------------------------------------------------------------
plant(70, BRG, 'the dispatch has no 95..97 arm', 'if (action == N48DM_ACT_SEND || action == N48DM_ACT_MODE || action == N48DM_ACT_CTX) {', 'if (false) {')
plant(71, BRG, 'dmubsend does not bind the allowlist layer', '		if (action == N48DM_ACT_SEND && n48dcn::dmubCmdLatchedOn()) (void)n48dcn::bind(this);', '')
plant(72, PURE, '95..97 are not exempt on a native boot', '(action >= kActDmubSend && action <= kActDmubCtx) ||', 'false ||')
plant(73, PURE, 'the action bound stops at 96', 'constexpr uint32_t kLastAction = 99u;', 'constexpr uint32_t kLastAction = 96u;')
plant(74, CLI, 'the CLI does not know dmubsend', '!strcmp(what, "dmubsend")) in = 95;', '!strcmp(what, "dmubsendX")) in = 95;')
plant(75, CLI, 'the CLI sends the dmubctx arguments in the wrong order', 'return cmd_dmubctx(arg, arg2, arg3);', 'return cmd_dmubctx(arg2, arg, arg3);')
plant(76, CLI, 'the M3 script drops the mode-block check', '    "    accel region4read 0x4c08 1                          pass ONLY if (dword & 0x00002000) == 0  (mode block byte +0x09 bit 0x20 clear); if set STOP, skip enable, roll back\\n"\n', '')
plant(77, CLI, 'the CLI does not pre-check the allowlist', '    const uint32_t pre = n48dm_slot_check(h, d1, d2);', '    const uint32_t pre = 0u;')
plant(78, PLIST, 'the version is not bumped', '<key>CFBundleVersion</key>\n\t<string>0.0.664</string>', '<key>CFBundleVersion</key>\n\t<string>0.0.624</string>')
plant(79, S1C, 'the native client build number is stale', 'kN1cKextBuild = 664;', 'kN1cKextBuild = 624;')
plant(80, DRH, 'the status table loses a name (two statuses share a text)', '    case N48DR_CTX_REFUSED: return "dmubctx accepts only ctx 0x7000 and 0x7800 (never 0x6800, the live DP context)";', '    case N48DR_CTX_REFUSED: return "the command header is not on the allowlist (0x04000a80 0x08000680 0x08000580 0x04000780 0x04000880): REFUSED, nothing written";')
# ---- 0.0.626: the 0.0.625 review's F4..F8 ----
plant(81, BRG, 'F4: the dispatcher binds the display layer before the latch is asked', 'if (action == N48DM_ACT_SEND && n48dcn::dmubCmdLatchedOn()) (void)n48dcn::bind(this);', 'if (action == N48DM_ACT_SEND) (void)n48dcn::bind(this);')
plant(82, G, 'F4: the exported latch answers yes whatever the boot-arg says', 'bool dmubCmdLatchedOn() { return dm_latched_on(); }', 'bool dmubCmdLatchedOn() { return true; }')
plant(83, H, 'F5: disable is allowed on 0x6800 again', '    case 0x08u:                           /* disable (0.0.626): ONLY the HDMI contexts 0x7000 / 0x7800, never 0x6800 (on the live DP context it is a console kill switch); d2 = 0 */\n        return (n48dm_ctx_ok(d1) && d2 == 0u)', '    case 0x08u:                           /* disable */\n        return (n48dm_ctx3_ok(d1) && d2 == 0u)')
plant(84, CLI, 'F5: the CLI script says disable is allowed anywhere', 'allowed ONLY on 0x7000 / 0x7800, never 0x6800', 'allowed on any context')
plant(85, CLI, 'F5: the CLI usage still offers disable <ctx>', 'disable <0x7000|0x7800>|replay <slot_off>|pclk', 'disable <ctx>|replay <slot_off>|pclk')
plant(86, F, 'F6: dmubmode writes on a busy ring', '    if (op != N48DM_MODE_SAVE) { st = n48dm_idle_gate(&p.pre); if (st != 0u) { r.status = st; return r; } }', '')
plant(87, F, 'F6: dmubctx writes on a busy ring', '    st = n48dm_idle_gate(&p.pre);       // 0.0.626 (F6): the marker write needs an idle ring\n    if (st != 0u) { r.status = st; return r; }\n', '')
plant(88, H, 'F6: the idle gate only notices WPTR ahead of RPTR', 'return p->wptr == p->rptr ? 0u : (uint32_t)N48DR_RING_NOT_IDLE;', 'return p->wptr <= p->rptr ? 0u : (uint32_t)N48DR_RING_NOT_IDLE;')
plant(89, H, 'F7: only a LARGER ring is refused', '    if (p->size != N48DM_RING_SIZE) return N48DR_RING_SIZE_REFUSED;', '    if (p->size > N48DM_RING_SIZE) return N48DR_RING_SIZE_REFUSED;')
plant(90, H, 'F7: the ring size is not checked', '    if (p->size != N48DM_RING_SIZE) return N48DR_RING_SIZE_REFUSED;', '')
plant(91, H, 'F7: the accepted ring size is 0x4000 (reaches +0x3100)', '#define N48DM_RING_SIZE    0x2000u', '#define N48DM_RING_SIZE    0x4000u')
plant(92, F, 'F8: a replay accepts an unaligned source', ' || replaySrc >= N48DM_RING_SIZE) st = N48DR_REPLAY_REFUSED;', ' || false) st = N48DR_REPLAY_REFUSED;')
plant(93, F, 'F8: a replay accepts a source at / above WPTR', 'if (replaySrc + N48DM_SLOT_BYTES > wptr0) {', 'if (replaySrc > wptr0 + N48DM_SLOT_BYTES) {')
plant(94, F, 'F8: the replay source skips the allowlist', '        st = n48dm_slot_check(slot[0], slot[1], slot[2]);\n        if (st != 0u) { o.status = st; return o; }\n', '')
plant(95, F, 'F8: the replay zero-fills like an ordinary send', '    if (!replay && !tpl) n48dm_build_slot(slot, hdr, d1, d2);', '    n48dm_build_slot(slot, hdr, d1, d2);')
plant(96, G, 'F8: the glue never passes the replay source on', '} else o = n48dm::send(env, 0u, 0u, 0u, replaySrc);', '} else o = n48dm::send(env, 0u, 0u, 0u);')
plant(97, H, 'F8: the replay argument ignores bits 48..63', '(a >> 48) != 0u) return N48DR_REPLAY_REFUSED;', 'false) return N48DR_REPLAY_REFUSED;')
plant(98, H, 'F8: the replay tag is an allowlisted header', '#define N48DM_REPLAY_TAG 0x52504c59u', '#define N48DM_REPLAY_TAG 0x04000a80u')
plant(99, CLI, 'F8: the CLI replay sends an ordinary all-zero command', 'dr_call(N48DM_ACT_SEND, n48dm_replay_arg(srcOff), o)', 'dr_call(N48DM_ACT_SEND, n48dm_send_arg(0u, 0u, 0u), o)')
plant(100, F, 'F8: a replay compares nothing it read back (the last dword is not verified)', '    for (i = 0; i < N48DM_SLOT_DWORDS; i++)\n        if (back[i] != slot[i])', '    for (i = 0; i < N48DM_SLOT_DWORDS - 1u; i++)\n        if (back[i] != slot[i])')
plant(101, DRH, 'the status count is stale', 'N48DR_STATUS_COUNT = 39', 'N48DR_STATUS_COUNT = 35')
# ---- 0.0.630 (stage M4c): the mainline VBIOS templates ----
plant(102, H, 'a template byte changed (pclk-otg3-on clock 148.5 MHz + 100 Hz)', '{ N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000003u, 0u } },', '{ N48DM_HDR_PCLK, 0x0016A8C9u, 0x00022017u, 0x00000003u, 0u } },')
plant(103, H, 'a template byte changed (phyc-enable HPD select 3 -> 2)', '{ "phyc-enable-dvi", { N48DM_HDR_XMIT, 0x04020102u, 0x00003A02u, 0x000C0003u, 0u } },', '{ "phyc-enable-dvi", { N48DM_HDR_XMIT, 0x04020102u, 0x00003A02u, 0x000C0002u, 0u } },')
plant(104, H, 'the transmitter header carries payload_bytes 16 again (0x10000180)', '#define N48DM_HDR_XMIT  0x3C000180u', '#define N48DM_HDR_XMIT  0x10000180u')
plant(105, H, 'another sub-1 / sub-2 payload is accepted (dword 3 ignored by the exact-slot check)', '            if (slot[i] != (i < 5u ? n48dm_tpls[t].dw[i] : 0u)) same = 0;', '            if (i != 3u && slot[i] != (i < 5u ? n48dm_tpls[t].dw[i] : 0u)) same = 0;')
plant(106, F, 'the poll bound for the new sub-types is back to 100 ms', '    const uint32_t pollMax = n48dm_poll_max_for(slot[0]);', '    const uint32_t pollMax = N48DM_POLL_MAX;')
plant(107, H, 'the old sub-types get the 2 s bound too', 'return n48dm_hdr_is_vbios_new(h) ? N48DM_POLL_MAX_VBIOS : N48DM_POLL_MAX;', 'return (void)h, N48DM_POLL_MAX_VBIOS;')
plant(108, H, 'the 2 s bound is 1 s', '#define N48DM_POLL_MAX_VBIOS 200000u', '#define N48DM_POLL_MAX_VBIOS 100000u')
plant(109, H, 'the template tag is an allowlisted header', '#define N48DM_TPL_TAG 0x314c5054u', '#define N48DM_TPL_TAG 0x04000a80u')
plant(110, H, 'a template ID past the table is legal (ID 8)', '(id >= 1u && id <= N48DM_TPL_COUNT)', '(id >= 1u && id <= N48DM_TPL_COUNT + 1u)')
plant(111, F, 'the flow skips the exact-slot check of a template', '        st = n48dm_tpl_slot_check(slot);\n        if (st != 0u) { o.status = st; return o; }\n', '')
plant(112, F, 'a template send also zero-fills like an ordinary one (the slot loses its payload)', '    if (!replay && !tpl) n48dm_build_slot(slot, hdr, d1, d2);', '    if (!replay) n48dm_build_slot(slot, hdr, d1, d2);')
plant(113, CLI, 'the CLI maps otg1-off to the ON template', '!strcmp(a2, "otg1-off") ? N48DM_TPL_PCLK_OTG1_OFF', '!strcmp(a2, "otg1-off") ? N48DM_TPL_PCLK_OTG1_ON')
plant(114, CLI, 'the CLI maps phyc disable to the ENABLE template', '!strcmp(a2, "disable") ? N48DM_TPL_PHYC_DISABLE', '!strcmp(a2, "disable") ? N48DM_TPL_PHYC_ENABLE_DVI')
plant(115, G, 'the glue forgets to pass the template ID on', 'n48dm::send(env, 0u, 0u, 0u, N48DM_NO_REPLAY, tplId)', 'n48dm::send(env, 0u, 0u, 0u, N48DM_NO_REPLAY, 0u)')
# ---- 0.0.632 (M4d follow-up): template 7, DIGX_ENCODER_CONTROL (sub 0) for DIGC / DVI ----
plant(116, H, 'digc-setup-dvi action changed (STREAM_SETUP 0x0F -> ENABLE 0x01)', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000000u, 0u } },', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020102u, 0x00003A02u, 0x00000000u, 0u } },')
plant(117, H, 'digc-setup-dvi carries bitpercolor 2 (8 bpc) where Linux leaves 0', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000000u, 0u } },', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000002u, 0u } },')
plant(118, H, 'the ordinary allowlist admits a sub-0 header (any DIGX_ENCODER_CONTROL payload)', '    if (!n48dm_hdr_ok(h)) return N48DR_HDR_REFUSED;\n    switch', '    if (h == 0x0C000080u) return 0u;\n    if (!n48dm_hdr_ok(h)) return N48DR_HDR_REFUSED;\n    switch')
plant(119, H, 'another sub-0 payload is accepted (dword 2, the pixel clock, ignored by the exact-slot check)', '            if (slot[i] != (i < 5u ? n48dm_tpls[t].dw[i] : 0u)) same = 0;', '            if (i != 2u && slot[i] != (i < 5u ? n48dm_tpls[t].dw[i] : 0u)) same = 0;')
plant(120, H, 'the sub-0 header gets only the 100 ms bound', 'h == N48DM_HDR_PCLK || h == N48DM_HDR_XMIT || h == N48DM_HDR_DIGENC; }', 'h == N48DM_HDR_PCLK || h == N48DM_HDR_XMIT; }')
plant(121, H, 'the sub-0 header carries payload_bytes 16 (0x10000080)', '#define N48DM_HDR_DIGENC 0x0C000080u', '#define N48DM_HDR_DIGENC 0x10000080u')
plant(122, CLI, 'the CLI maps digc setup to the PHY template', 'id = !strcmp(a2, "setup") ? N48DM_TPL_DIGC_SETUP_DVI : N48DM_TPL_NONE;', 'id = !strcmp(a2, "setup") ? N48DM_TPL_PHYC_ENABLE_DVI : N48DM_TPL_NONE;')
plant(123, CLI, 'the CLI no longer refuses digc disable by name', 'if (!strcmp(a2, "disable")) { fprintf(stderr, "accel dmubsend digc disable: refused', 'if (0 && !strcmp(a2, "disable")) { fprintf(stderr, "accel dmubsend digc disable: refused')
plant(124, H, 'digc-setup-dvi drops the pixel clock (14850 -> 0)', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000000u, 0u } },', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00000000u, 0x00000000u, 0u } },')
plant(125, H, 'digc-setup-dvi engine DIGB (the live DP encoder)', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000000u, 0u } },', '{ "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F01u, 0x00003A02u, 0x00000000u, 0u } },')
# ---- 0.0.633: the monitor B's five templates (8..12) --------------------------------------------------------------------------------------------------------------------------------------------------------
plant(130, H, "digd-setup-dvi carries the monitor A's digid 2", '{ "digd-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F03u,', '{ "digd-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u,')
plant(131, H, "phyd-enable-dvi carries the monitor A's hpdsel 3", '{ "phyd-enable-dvi", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u, 0x000C0004u, 0u } },', '{ "phyd-enable-dvi", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u, 0x000C0003u, 0u } },')
plant(132, H, "phyd-disable carries the monitor A's phyid 2", '{ "phyd-disable",    { N48DM_HDR_XMIT, 0x04020003u,', '{ "phyd-disable",    { N48DM_HDR_XMIT, 0x04020002u,')
plant(133, H, 'phyd-enable-dvi carries connobj 0x0B', '{ "phyd-enable-dvi", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u, 0x000C0004u, 0u } },', '{ "phyd-enable-dvi", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u, 0x000B0004u, 0u } },')
plant(134, H, 'phyd-disable carries symclk 14851', '{ "phyd-disable",    { N48DM_HDR_XMIT, 0x04020003u, 0x00003A02u,', '{ "phyd-disable",    { N48DM_HDR_XMIT, 0x04020003u, 0x00003A03u,')
plant(135, H, "pclk-otg2-on carries the monitor A's pll 0x16", '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000002u, 0u } },', '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022016u, 0x00000002u, 0u } },')
plant(136, H, "pclk-otg2-on carries crtc 3 (the OTG3 probe's)", '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000002u, 0u } },', '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000003u, 0u } },')
plant(137, H, 'pclk-otg2-off carries a clock', '{ "pclk-otg2-off",   { N48DM_HDR_PCLK, 0x00000000u,', '{ "pclk-otg2-off",   { N48DM_HDR_PCLK, 0x0016A8C8u,')
plant(138, H, 'pclk-otg2-on carries 297 MHz', '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u,', '{ "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x002D3190u,')
plant(139, H, 'the template count stays 11 (digd unreachable)', 'N48DM_TPL_DIGD_SETUP_DVI_1440 = 15, N48DM_TPL_COUNT = 15 };', 'N48DM_TPL_DIGD_SETUP_DVI_1440 = 15, N48DM_TPL_COUNT = 11 };')
plant(140, H, 'the exact-slot check skips the last template', '    for (t = 0; t < N48DM_TPL_COUNT; t++) {\n        int same = 1;', '    for (t = 0; t + 1 < N48DM_TPL_COUNT; t++) {\n        int same = 1;')
plant(141, CLI, 'the CLI swaps phyd enable and disable', 'else if (!strcmp(a1, "phyd")) id = !strcmp(a2, "enable") ? N48DM_TPL_PHYD_ENABLE_DVI : !strcmp(a2, "disable") ? N48DM_TPL_PHYD_DISABLE :', 'else if (!strcmp(a1, "phyd")) id = !strcmp(a2, "enable") ? N48DM_TPL_PHYD_DISABLE : !strcmp(a2, "disable") ? N48DM_TPL_PHYD_ENABLE_DVI :')
plant(142, CLI, 'the CLI no longer refuses digd disable', 'if (!strcmp(a2, "disable")) { fprintf(stderr, "accel dmubsend digd disable: refused', 'if (0) { fprintf(stderr, "accel dmubsend digd disable: refused')
plant(143, CLI, 'the CLI swaps otg2-on and otg2-off', '!strcmp(a2, "otg2-on") ? N48DM_TPL_PCLK_OTG2_ON : !strcmp(a2, "otg2-off") ? N48DM_TPL_PCLK_OTG2_OFF :', '!strcmp(a2, "otg2-on") ? N48DM_TPL_PCLK_OTG2_OFF : !strcmp(a2, "otg2-off") ? N48DM_TPL_PCLK_OTG2_ON :')
plant(144, H, 'the template argument accepts one id past the table', 'return (id >= 1u && id <= N48DM_TPL_COUNT) ? &n48dm_tpls[id - 1u]', 'return (id >= 1u && id <= N48DM_TPL_COUNT + 1u) ? &n48dm_tpls[id - 1u]')
# ---- 0.0.634: the monitor B's 1440p templates (13..15) ----------------------------------------------------------------------------------------------------------------------------------------------------
plant(145, H, 'pclk-otg2-1440-on carries 241.4999 MHz', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D998u,', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D997u,')
plant(146, H, 'pclk-otg2-1440-on carries the 1080p clock', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D998u,', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0016A8C8u,')
plant(147, H, 'pclk-otg2-1440-on uses the monitor A PLL', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D998u, 0x00022017u,', '{ "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D998u, 0x00022016u,')
plant(148, H, 'phyd-enable-dvi-1440 symclk is 24151', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u, 0x00005E56u,', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u, 0x00005E57u,')
plant(149, H, 'phyd-enable-dvi-1440 carries the 1080p symclk', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u, 0x00005E56u,', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u,')
plant(150, H, 'phyd-enable-dvi-1440 goes dual link (lanenum 8)', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u,', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x08020103u,')
plant(151, H, 'phyd-enable-dvi-1440 is HDMI mode (digmode 3)', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u,', '{ "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04030103u,')
plant(152, H, 'digd-setup-dvi-1440 symclk is 24149', '{ "digd-setup-dvi-1440",  { N48DM_HDR_DIGENC, 0x04020F03u, 0x00005E56u,', '{ "digd-setup-dvi-1440",  { N48DM_HDR_DIGENC, 0x04020F03u, 0x00005E55u,')
plant(153, H, 'digd-setup-dvi-1440 is digc (digid 2)', '{ "digd-setup-dvi-1440",  { N48DM_HDR_DIGENC, 0x04020F03u,', '{ "digd-setup-dvi-1440",  { N48DM_HDR_DIGENC, 0x04020F02u,')
plant(154, H, 'the template count forgets the 1440p ones', 'N48DM_TPL_DIGD_SETUP_DVI_1440 = 15, N48DM_TPL_COUNT = 15 };', 'N48DM_TPL_DIGD_SETUP_DVI_1440 = 15, N48DM_TPL_COUNT = 12 };')
plant(155, CLI, 'the CLI maps phyd enable-1440 to the 1080p enable', '!strcmp(a2, "enable-1440") ? N48DM_TPL_PHYD_ENABLE_DVI_1440 : N48DM_TPL_NONE;', '!strcmp(a2, "enable-1440") ? N48DM_TPL_PHYD_ENABLE_DVI : N48DM_TPL_NONE;')
plant(156, CLI, 'the CLI maps pclk otg2-1440-on to the 1080p on', '!strcmp(a2, "otg2-1440-on") ? N48DM_TPL_PCLK_OTG2_1440_ON : N48DM_TPL_NONE;', '!strcmp(a2, "otg2-1440-on") ? N48DM_TPL_PCLK_OTG2_ON : N48DM_TPL_NONE;')
plant(157, CLI, 'the CLI maps digd setup-1440 to the 1080p setup', '!strcmp(a2, "setup-1440") ? N48DM_TPL_DIGD_SETUP_DVI_1440 : N48DM_TPL_NONE;', '!strcmp(a2, "setup-1440") ? N48DM_TPL_DIGD_SETUP_DVI : N48DM_TPL_NONE;')
plant(158, CLI, 'the template send skips the DP watch before', 'if (dp_watch_read(dpw0) != 0) { printf("accel dmubsend %s %s: REFUSED by the CLI: the DP watch (dispcensus page 10) could not be read; nothing was sent\\n", a1, a2); return 1; }', '(void)dp_watch_read(dpw0);')
plant(159, CLI, 'the template send never compares the DP watch afterwards', 'else (void)dp_watch_report("dmubsend template", dpw0, dpw1);', '')
plant(160, CLI, 'the DP watch report never says DP DISTURBED', 'if (changed) printf("  *** DP DISTURBED (%s): %u of %u DP plane registers changed; STOP, run the ROLLBACK, do not retry this boot ***\\n", what, changed, N48D2_DPX_REGS);', 'if (changed) printf("  (%s): %u of %u\\n", what, changed, N48D2_DPX_REGS);')
plant(161, H, 'the monitor B\'s pclk-otg2-on is not refused while HELD (0.0.653)', 'id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }', 'id >= N48DM_TPL_PCLK_OTG2_OFF && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }')
plant(162, H, 'the monitor B\'s digd-setup-1440 is not refused while HELD', 'id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }', 'id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_PHYD_ENABLE_DVI_1440; }')
plant(163, H, 'the monitor A\'s digc-setup (7) is refused while HELD', 'id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }', 'id >= N48DM_TPL_DIGC_SETUP_DVI && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }')
plant(164, H, 'a raw disable of 0x7800 is not refused while HELD', '    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4;', '    case 0x0au: case 0x07u: return d1 == N48DM_CTX_DFP4;')
plant(165, H, 'a raw set-mode of 0x7800 is not refused while HELD', '    case 0x05u: return d2 == N48DM_CTX_DFP4;', '    case 0x05u: return 0;')
plant(166, H, 'a raw detect/enable of 0x7800 is not refused while HELD', '    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4;', '    case 0x08u: return d1 == N48DM_CTX_DFP4;')
plant(167, H, 'the raw refusal also hits the monitor A\'s 0x7000', '    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4;', '    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4 || d1 == N48DM_CTX_DFP3;')
plant(168, H, 'the raw refusal also hits mode-change begin / end', 'return d2 == N48DM_CTX_DFP4;\n    default: return 0;\n    }\n}\n', 'return d2 == N48DM_CTX_DFP4;\n    default: return 1;\n    }\n}\n')
plant(169, H, 'set-mode keys on d1 instead of the context in d2', '    case 0x05u: return d2 == N48DM_CTX_DFP4;', '    case 0x05u: return d1 == N48DM_CTX_DFP4;')
plant(170, H, 'the raw refusal judges malformed slots too', 'static inline int n48dm_slot_held_refuses(uint32_t h, uint32_t d1, uint32_t d2)\n{\n    if (n48dm_slot_check(h, d1, d2) != 0u) return 0;\n    switch', 'static inline int n48dm_slot_held_refuses(uint32_t h, uint32_t d1, uint32_t d2)\n{\n    switch')
plant(171, H, '0.0.655: the monitor A template refusal only covers the power-on template (the teardowns 4 and 6 slip through)', 'static inline int n48dm_tpl_mona_held_refuses(uint32_t id) { return id >= N48DM_TPL_PCLK_OTG1_ON && id <= N48DM_TPL_DIGC_SETUP_DVI; }', 'static inline int n48dm_tpl_mona_held_refuses(uint32_t id) { return id == N48DM_TPL_PCLK_OTG1_ON; }')
plant(172, H, '0.0.655: the monitor A raw-slot refusal judges the monitor B\'s context 0x7800 instead of 0x7000', 'case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP3;', 'case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4;')
shutil.rmtree(scr, ignore_errors=True)
print('native_dmubcmd_plant: %d plant(s), %d escaped' % (total, escaped))
sys.exit(1 if escaped else 0)
PY
