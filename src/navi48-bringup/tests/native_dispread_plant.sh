#!/bin/zsh
# native_dispread_plant.sh - planted breaks for tests/native_dispread_test.cpp (build 0.0.622, multi-monitor stage M1: ddcread / dmubring / dispcensus).
# Discipline (as native_disp_plant.sh): copy the sources the test reads into a scratch tree, apply ONE break to the REAL code (a header, the flow, the kernel glue, the dispatch, the CLI, the plist),
# build the real test against the copy and demand it FAILS (a compile error, a failing check, a crash or a hang all count). A plant whose text is not found exactly once is itself reported as an
# escape. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_dispread_plant.sh [first-plant-id last-plant-id]
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec python3 -u - "$ROOT" "${1:-0}" "${2:-9999}" <<'PY'
import os, shutil, subprocess, sys, tempfile, time

ROOT, FIRST, LAST = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
K = 'src/navi48-bringup'
H, F, G, GH, BRG, PURE, UCL, CLI, PLIST, S1C, NUB = (f'{K}/src/dcn/navi48_dispread.h', f'{K}/src/dcn/navi48_dispread_flow.h', f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/dcn/navi48_dcn.hpp',
    f'{K}/src/Navi48Bringup.cpp', f'{K}/src/amd/native_disp_pure.h', f'{K}/src/Navi48UserClient.cpp', 'tools/pc/navi48test.c', f'{K}/Info.plist', f'{K}/src/amd/native_s1c.h', f'{K}/src/Navi48MetalNub.cpp')
CORE, CORE_H = 'src/dcn41/dcn41_core.c', 'src/dcn41/dcn41.h'
COPY_DIRS = [f'{K}/src/amd', f'{K}/src/dcn', 'src/dcn41']
COPY_FILES = [BRG, UCL, CLI, PLIST, S1C, NUB, GH, f'{K}/src/Navi48MetalOps.h', f'{K}/src/Navi48DisplayOps.h', f'{K}/src/Navi48NativeABI.h', f'{K}/src/amd/native_m6_pure.h', f'{K}/src/apple/display_pipe_guard.h', f'{K}/Makefile', 'tools/build-navi48test.sh', f'{K}/tests/native_dispread_test.cpp', f'{K}/tests/native_dispread_plant.sh']

scr = tempfile.mkdtemp(prefix='ndispread-plant.')
def fresh():
    shutil.rmtree(scr, ignore_errors=True)
    for d in COPY_DIRS:
        shutil.copytree(os.path.join(ROOT, d), os.path.join(scr, d), ignore=shutil.ignore_patterns('*.o', 'build*'))
    for f in COPY_FILES:
        d = os.path.join(scr, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(ROOT, f), d)
def build_run():
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', f'{K}/src', '-I', f'{K}/src/dcn', '-I', f'{K}/src/amd', '-I', 'src/dcn41',
           f'{K}/tests/native_dispread_test.cpp', '-x', 'c++', 'src/dcn41/dcn41_allow.c', 'src/dcn41/dcn41_dmub.c', 'src/dcn41/dcn41_core.c', '-o', os.path.join(scr, 't')]
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
# ---- the plan and the pure decisions (navi48_dispread.h) -----------------------------------------------------------------------------------------------------------------------
plant(1, H, 'line 1 is accepted (the DP / AUX-class line)', '#define N48DR_DDC_LINE_MIN 2u', '#define N48DR_DDC_LINE_MIN 1u')
plant(2, H, 'the segment pointer is written for block 1', 'p->has_seg = block >= 2u ? 1u : 0u;', 'p->has_seg = block >= 1u ? 1u : 0u;')
plant(3, H, 'the checksum skips the last byte (off by one)', 'for (i = 0; i < N48DR_EDID_BLOCK; i++) s += b[i];', 'for (i = 0; i < N48DR_EDID_BLOCK - 1u; i++) s += b[i];')
plant(4, H, 'a hardware / DMUB owner is not refused at entry', '    if (owner == 2u) return N48DR_HW_OWNED;\n', '')
plant(5, H, 'the poll bound is doubled (40 ms per transaction)', '#define N48DR_DDC_POLL_PER_TXN 2000u', '#define N48DR_DDC_POLL_PER_TXN 4000u')
plant(6, H, 'the reply is read one byte early', 'p->read_index = p->fifo_len;   /* the read data starts after the last address byte', 'p->read_index = (uint8_t)(p->fifo_len - 1u);   /* the read data starts after the last address byte')
plant(7, H, 'the offset byte loses its top bit (block 1 reads offset 0)', 'p->offset = (uint8_t)((block * N48DR_EDID_BLOCK) & 0xFFu);', 'p->offset = (uint8_t)((block * N48DR_EDID_BLOCK) & 0x7Fu);')
plant(8, H, 'a grant is accepted when hardware owns the engine', 'return arb != 0xFFFFFFFFu && ((arb & N48DR_ARB_RW_STATUS_MASK) >> N48DR_ARB_RW_STATUS_SHIFT) == 1u;', 'return arb != 0xFFFFFFFFu && ((arb & N48DR_ARB_RW_STATUS_MASK) >> N48DR_ARB_RW_STATUS_SHIFT) != 2u;')
plant(9, H, 'a NACK is not reported (keeps polling)', '    if (sts & N48DR_ST_NACK) return (int)N48DR_NACK;\n', '')
plant(10, H, 'ddcread accepts page 2', '((arg >> 16) & 0xFFu) <= 1u;', '((arg >> 16) & 0xFFu) <= 2u;')
plant(11, H, 'the DMUB sub-type names are swapped', 'sub == N48DR_DMUB_VBIOS_DIGX_ENCODER_CONTROL ? "DIGX_ENCODER_CONTROL" : sub == N48DR_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL ? "DIG1_TRANSMITTER_CONTROL"', 'sub == N48DR_DMUB_VBIOS_DIGX_ENCODER_CONTROL ? "DIG1_TRANSMITTER_CONTROL" : sub == N48DR_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL ? "DIGX_ENCODER_CONTROL"')
plant(12, H, 'the BAR0 reachability test can overflow', 'return ring_bytes != 0u && ring_vram_off <= bar0_size && ring_bytes <= bar0_size - ring_vram_off;', 'return ring_bytes != 0u && ring_vram_off + ring_bytes <= bar0_size;')
plant(13, H, 'the ring decode shows 26 commands', '#define N48DR_DMUB_MAX_DECODE 25u', '#define N48DR_DMUB_MAX_DECODE 26u')
plant(14, H, 'the header decode loses a payload_bytes bit', 'd.payload_bytes = (uint8_t)((h >> 24) & 0x3Fu);', 'd.payload_bytes = (uint8_t)((h >> 24) & 0x1Fu);')
plant(15, H, 'the census page holds 20 registers', '#define N48DR_CENSUS_PER_PAGE 24u', '#define N48DR_CENSUS_PER_PAGE 20u')
plant(16, H, 'the I2C DATA register offset moved out of the allowlist range', '#define N48DR_I2C_DATA           0x1eb2u', '#define N48DR_I2C_DATA           0x1fb2u')
plant(17, H, 'DC_I2C_ARBITRATION has the wrong offset', '#define N48DR_I2C_ARBITRATION    0x1e99u', '#define N48DR_I2C_ARBITRATION    0x1e9au')
plant(18, H, 'the QUERY_FEATURE_CAPS type number is wrong', '#define N48DR_DMUB_TYPE_QUERY_FEATURE_CAPS 6u', '#define N48DR_DMUB_TYPE_QUERY_FEATURE_CAPS 7u')
plant(19, H, 'a census register offset is wrong (DIG2 back end)', '{ "DIG2_DIG_BE_EN_CNTL", 2, 0x2305 }, { "DIG3_DIG_BE_EN_CNTL"', '{ "DIG2_DIG_BE_EN_CNTL", 2, 0x2306 }, { "DIG3_DIG_BE_EN_CNTL"')
# ---- the flow (navi48_dispread_flow.h) -----------------------------------------------------------------------------------------------------------------------------------
plant(30, F, 'the release is skipped on a timeout', '        if (!finished) { status = N48DR_TIMEOUT; break; }', '        if (!finished) { r.status = N48DR_TIMEOUT; return r; }')
plant(31, F, 'the release writes a non-allowlisted register', '    e.wr2(rSetup, 0u);\n    e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);', '    e.wr2(rSetup + 0x100u, 0u);\n    e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);')
plant(32, F, 'the GO is written even after a refused write', '        if (e.refused()) { status = N48DR_WRITE_REFUSED; break; }          // BEFORE the GO: a half-programmed engine never starts\n', '')
plant(33, F, 'the entry verdict is ignored (the engine is woken and taken from a hardware owner)', '    uint32_t v = n48dr_arb_entry(arb);\n    if (v != 0u) { r.status = v; r.arbAfter = arb; return r; }\n\n    // ---- 1.', '    uint32_t v = 0;\n\n    // ---- 1.')
plant(34, F, 'the first FIFO push does not set index 0', '(i == 0u ? N48DR_DATA_INDEX_WRITE : 0u) | ((uint32_t)p.fifo[i] << N48DR_DATA_SHIFT)', '((uint32_t)p.fifo[i] << N48DR_DATA_SHIFT)')
plant(35, F, 'the release never hands the engine back (no DONE_USING)', '    e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);\n    r.released = true;', '    r.released = true;')
plant(36, F, 'a request that was not granted is not withdrawn', '        e.wr2(N48DR_I2C_ARBITRATION, e.rd(2u, N48DR_I2C_ARBITRATION) | N48DR_ARB_SW_DONE_USING);\n        r.status = e.refused()', '        r.status = e.refused()')
plant(37, F, 'the re-check after the wake is dropped (a hardware owner that arrived meanwhile is overwritten)', '    arb = e.rd(2u, N48DR_I2C_ARBITRATION);\n    r.arb = arb;\n    v = n48dr_arb_entry(arb);\n    if (v != 0u) { r.status = v; r.arbAfter = arb; return r; }   // someone took it meanwhile', '    arb = e.rd(2u, N48DR_I2C_ARBITRATION);\n    r.arb = arb;\n    v = 0u;   // someone took it meanwhile')
plant(38, F, 'the poll loop is unbounded in practice (x10)', 'const uint32_t bound = n48dr_poll_bound(&p);', 'const uint32_t bound = n48dr_poll_bound(&p) * 10u;')
plant(39, F, 'the soft reset is dropped from the release', '    e.wr2(N48DR_I2C_CONTROL, N48DR_CTL_SOFT_RESET | N48DR_CTL_SW_STATUS_RESET);\n    e.wr2(rSetup, 0u);', '    e.wr2(rSetup, 0u);')
plant(40, F, 'the census reads one register too few per page', 'for (uint32_t i = 0; i < n; i++) { const struct n48dr_reg *r = n48dr_census_reg(page, i);', 'for (uint32_t i = 0; i + 1u < n; i++) { const struct n48dr_reg *r = n48dr_census_reg(page, i);')
plant(41, F, 'a ring page past the last command is served', 'if (page < 1u || page > n) return N48DR_PAGE_RANGE;', 'if (page < 1u || page > n + 1u) return N48DR_PAGE_RANGE;')
plant(42, F, 'the ring is read with a wrong stride', 'const uint64_t at = ringVramOff + (uint64_t)idx * N48DR_DMUB_CMD_SIZE;', 'const uint64_t at = ringVramOff + (uint64_t)idx * 32u;')
# ---- the kernel glue, the dispatch, the exemption, the CLI, the version ------------------------------------------------------------------------------------------------------------------------------------
plant(50, G, 'the glue reads the DMUB ring through MM_INDEX', 'uint32_t rd32(uint64_t off) { return amdgpu::RBAR0_32(*dev, off); }', 'uint32_t rd32(uint64_t off) { return amdgpu::RVRAM32_via_mm(*dev, off); }')
plant(51, G, 'ddcread bypasses the allowlist', '|| !dcn41_allow_write(&gDcn.allow, a, v, "ddcread")) {', '|| false) {')
plant(52, G, 'dmubring gets a second register write path', '	RoKextEnv ro;\n	if (arg == N48DR_DMUB_PAGE_SCRATCH) {', '	RoKextEnv ro;\n	amdgpu::WREG32(*static_cast<amdgpu::DeviceContext *>(gLr.d.cookie), 0x1u, 0u);\n	if (arg == N48DR_DMUB_PAGE_SCRATCH) {')
plant(53, G, 'ddcread is not single-flight', 'if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { n48dr_ddc_fill', 'if (false) { n48dr_ddc_fill')
plant(54, G, 'ddcread runs unarmed', 'if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) { n48dr_ddc_fill', 'if (false) { n48dr_ddc_fill')
plant(55, G, 'dmubring sends a GPINT command', '	struct dcn41_dmub_state st;\n	if (dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK)', '	struct dcn41_dmub_state st;\n	(void)dcn41_dmub_gpint(&gLr.d, 1u, 0u, 100u);\n	if (dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK)')
plant(56, BRG, 'the dispatch skips the argument check', '		if (!n48disp::verb_args_ok(action, argScalar)) {\n			st = N48DR_BAD_ARG;\n			v[0] = st;\n		} else if (action == N48DR_ACT_DDCREAD) {', '		if (false) {\n			st = N48DR_BAD_ARG;\n			v[0] = st;\n		} else if (action == N48DR_ACT_DDCREAD) {')
plant(57, BRG, 'dmubring also binds the allowlist layer', '			st = n48dcn::dmubRing(argScalar, v, 13);', '			(void)n48dcn::bind(this);\n			st = n48dcn::dmubRing(argScalar, v, 13);')
plant(58, PURE, 'dmubring is not exempt on a native boot', '(action == kActDmubRing && n48dr_dmub_arg_ok(arg)) || ', '')
plant(59, PURE, 'the action bound stays at 93 (region4read unreachable)', 'constexpr uint32_t kLastAction = 99u;', 'constexpr uint32_t kLastAction = 93u;')
plant(60, PURE, 'ddcread is exempt for any argument', '(action == kActDdcRead && n48dr_ddc_arg_ok(arg))', '(action == kActDdcRead)')
plant(61, UCL, 'the user client bound ignores the latch for the new verbs', 'if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;', 'if (action > 94) return kIOReturnBadArgument;')
plant(62, CLI, 'the CLI does not know ddcread', '    else if (what && !strcmp(what, "ddcread")) in = 91;\n', '')
plant(63, CLI, 'the CLI skips its own checksum', 'const uint32_t sum = n48dr_edid_sum(edid);', 'const uint32_t sum = 0;')
plant(64, PLIST, 'the version is not bumped', '<key>CFBundleVersion</key>\n\t<string>0.0.664</string>', '<key>CFBundleVersion</key>\n\t<string>0.0.622</string>')
plant(65, S1C, 'the native client build number is stale', 'kN1cKextBuild = 664;', 'kN1cKextBuild = 622;')
plant(66, NUB, 'an ops table keeps the old build', 'const N48MetalOps gOpsOff = {\n\tN48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 664u,', 'const N48MetalOps gOpsOff = {\n\tN48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 622u,')

# ---- region4read (0.0.624, stage M1.5): the range rule and the base (navi48_dispread.h), the flow, the glue, the dispatch, the exemption, the CLI -----------------------------------------------------------
plant(70, H, 'the window end is exclusive of its last dword (off by one, refuses the exact fit)', 'dwords * 4u <= (uint64_t)N48DR_R4_WINDOW - off;', 'dwords * 4u < (uint64_t)N48DR_R4_WINDOW - off;')
plant(71, H, 'the window end is one dword too far (off by one the other way)', 'dwords * 4u <= (uint64_t)N48DR_R4_WINDOW - off;', 'dwords * 4u <= (uint64_t)N48DR_R4_WINDOW - off + 4u;')
plant(72, H, 'a call may read 65 dwords', 'dwords >= 1u && dwords <= N48DR_R4_MAX_DWORDS && (off & 3u) == 0u', 'dwords >= 1u && dwords <= N48DR_R4_MAX_DWORDS + 1u && (off & 3u) == 0u')
plant(73, H, 'the offset alignment is not required', ' && (off & 3u) == 0u && off <= N48DR_R4_WINDOW', ' && off <= N48DR_R4_WINDOW')
plant(74, H, 'the window base alignment is not required', '    if ((base & 0xFFFu) != 0u) return N48DR_REGION4_BAD;\n', '')
plant(75, H, 'the window may run one byte past VRAM', 'if (base > vram_size || (uint64_t)N48DR_R4_WINDOW > vram_size - base) return N48DR_REGION4_BAD;', 'if (base > vram_size || (uint64_t)N48DR_R4_WINDOW > vram_size - base + 1u) return N48DR_REGION4_BAD;')
plant(76, H, 'the window base is hard-coded (the measured value) instead of computed', '    base = gpu - fb_base_mc;\n', '    base = 0x3dac00000ull;\n')
plant(77, H, 'a base past the end of VRAM is accepted (the size subtraction wraps)', 'if (base > vram_size || (uint64_t)N48DR_R4_WINDOW > vram_size - base) return N48DR_REGION4_BAD;', 'if ((uint64_t)N48DR_R4_WINDOW > vram_size - base) return N48DR_REGION4_BAD;')
plant(78, H, 'a disabled REGION4 is accepted', 'if (!enabled || gpu == 0u || gpu < fb_base_mc) return N48DR_REGION4_BAD;', 'if ((void)enabled, gpu == 0u || gpu < fb_base_mc) return N48DR_REGION4_BAD;')
plant(79, H, 'page 3 exists (an arg for a fourth page is legal)', 'return (dwords + N48DR_R4_PER_PAGE - 1u) / N48DR_R4_PER_PAGE;', 'return (dwords + N48DR_R4_PER_PAGE - 1u) / N48DR_R4_PER_PAGE + 1u;')
plant(80, F, 'the flow reads at a hard-coded base', 'm.rd(base + off, dst, dwords)', 'm.rd(0x3dac00000ull + off, dst, dwords)')
plant(81, F, 'the flow does not check the range', 'if (!dst || !n48dr_r4_range_ok(off, dwords)) return N48DR_BAD_ARG;', 'if (!dst) return N48DR_BAD_ARG;')
plant(82, F, 'the flow ignores the enable bit', 'n48dr_region4_base(off_lo, off_hi, enabled, fb_base_mc, fb_size, &base)', 'n48dr_region4_base(off_lo, off_hi, enabled | 1u, fb_base_mc, fb_size, &base)')
plant(83, F, 'the flow reads one dword too many', 'm.rd(base + off, dst, dwords)', 'm.rd(base + off, dst, dwords + 1u)')
plant(84, F, 'the flow reads before it validates the base', '    const uint32_t st = n48dr_region4_base(off_lo, off_hi, enabled, fb_base_mc, fb_size, &base);\n    if (base_out) *base_out = base;\n    if (st != N48DR_OK) return st;', '    const uint32_t st = n48dr_region4_base(off_lo, off_hi, enabled, fb_base_mc, fb_size, &base);\n    if (base_out) *base_out = base;\n    (void)st;')
plant(85, G, 'region4read writes VRAM instead of reading it', 'bool rd(uint64_t vramOff, uint32_t *dst, uint32_t n) { return navi48_vram_read_mm(vramOff, dst, n); }', 'bool rd(uint64_t vramOff, uint32_t *dst, uint32_t n) { return navi48_vram_write_mm(vramOff, dst, n); }')
plant(86, G, 'region4read gets a second (write) MM path', '			R4Mem mem;\n', '			R4Mem mem;\n			amdgpu::WVRAM32_via_mm(*dev, 0u, 0u);\n')
plant(87, G, 'region4read passes a hard-coded FB base', 'st.fb_base_mc, vramSize, off, dwords, &base, d);', '0x8000000000ull, vramSize, off, dwords, &base, d);')
plant(88, G, 'region4read is not single-flight', 'if (__atomic_exchange_n(&gR4Busy, 1u, __ATOMIC_ACQ_REL) != 0u)', 'if (false)')
plant(89, G, 'region4read never probes the DMCUB registers (a stale or absent base)', '		struct dcn41_dmub_state st;\n		if (dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK) {\n			status = N48DR_NO_DEVICE;', '		struct dcn41_dmub_state st = { };\n		if (false) {\n			status = N48DR_NO_DEVICE;')
plant(90, G, 'region4read writes a register', '			R4Mem mem;\n', '			R4Mem mem;\n			amdgpu::WREG32(*dev, 0x1u, 0u);\n')
plant(91, BRG, 'the dispatch has no region4read arm', '		} else if (action == N48DR_ACT_REGION4READ) {\n			st = n48dcn::region4Read(argScalar, v, 13);\n		} else {', '		} else {')
plant(92, PURE, 'region4read is exempt for any argument', '(action == kActRegion4Read && n48dr_r4_arg_ok(arg))', '(action == kActRegion4Read)')
plant(93, CLI, 'the CLI walks the contexts with the wrong pitch', 'const uint32_t cb = 0x6000 + c * 0x800;', 'const uint32_t cb = 0x6000 + c * 0x600;')
plant(94, CLI, 'the CLI dump stops short of the last context', 'r4_range("display contexts", 0x6000, 0x2000 / 4, ctx, &base)', 'r4_range("display contexts", 0x6000, 0x1800 / 4, ctx, &base)')
plant(95, CLI, 'the CLI does not know region4dump', '"region4read") || !strcmp(what, "region4dump")', '"region4read")')
# ---- 0.0.628 (stage M4a): the read-only address twin, the second census table, the glue's call sites ----------------------------------------------------------------------------------------------------
plant(100, CORE, 'the WRITE-side address accepts BASE_IDX 1 and 3 (dcn41_abs tests the read mask)', '        !(DCN41_BASE_IDX_USED_MASK & (1u << base_idx)))\n        return DCN41_BAD_OFFSET;\n    a = (uint64_t)dev->seg[base_idx] + offset;\n    if (a >= dev->mmio_dwords)\n        return DCN41_BAD_OFFSET;\n    return (uint32_t)a;\n}\n\n/* 0.0.628', '        !(DCN41_BASE_IDX_READ_MASK & (1u << base_idx)))\n        return DCN41_BAD_OFFSET;\n    a = (uint64_t)dev->seg[base_idx] + offset;\n    if (a >= dev->mmio_dwords)\n        return DCN41_BAD_OFFSET;\n    return (uint32_t)a;\n}\n\n/* 0.0.628')
plant(101, G, 'the census environment is back on the write-side address (BASE_IDX 1 reads the sentinel again)', 'const uint32_t a = dcn41_abs_rd(&gLr.d, off, baseIdx);', 'const uint32_t a = dcn41_abs(&gLr.d, off, baseIdx);')
plant(102, G, 'ddcread\'s prescale read is back on the write-side address', 'const uint32_t a = dcn41_abs_rd(&gDcn.d, off, baseIdx);', 'const uint32_t a = dcn41_abs(&gDcn.d, off, baseIdx);')
plant(103, G, 'ddcread\'s WRITE helper uses the read twin', 'const uint32_t a = dcn41_abs(&gDcn.d, off, 2u);', 'const uint32_t a = dcn41_abs_rd(&gDcn.d, off, 2u);')
plant(104, H, 'a register is dropped from the M4a table (RDPCSTX2 PHY_CNTL17)', '{ "RDPCSTX2_RDPCSTX_PHY_CNTL17", 2, 0x2b0a },', '')
plant(105, H, 'a wrong offset in the M4a table (PHYCSYMCLK)', '{ "PHYCSYMCLK_CLOCK_CNTL", 2, 0x0054 }', '{ "PHYCSYMCLK_CLOCK_CNTL", 2, 0x0055 }')
plant(106, H, 'a wrong BASE_IDX in the M4a table (SYMCLKC)', '{ "SYMCLKC_CLOCK_ENABLE", 1, 0x00a2 }', '{ "SYMCLKC_CLOCK_ENABLE", 2, 0x00a2 }')
plant(107, F, 'the page read ignores the M4a table (always the original one)', 'const struct n48dr_reg *r = n48dr_census_reg(page, i); vals[i] = e.rd(r->base_idx, r->off);', 'const struct n48dr_reg *r = &n48dr_census[(page & 1u) * N48DR_CENSUS_PER_PAGE + i]; vals[i] = e.rd(r->base_idx, r->off);')
plant(108, H, 'a page past the last M4a page is legal', 'static inline int n48dr_census_arg_ok(uint64_t arg) { return arg < N48DR_CENSUS_ALL_PAGES; }', 'static inline int n48dr_census_arg_ok(uint64_t arg) { return arg <= N48DR_CENSUS_ALL_PAGES; }')
plant(109, CORE_H, 'the read twin loses BASE_IDX 3 (read mask 0x6)', '#define DCN41_BASE_IDX_READ_MASK 0xEu', '#define DCN41_BASE_IDX_READ_MASK 0x6u')
plant(110, H, 'the CLI\'s absolute address for BASE_IDX 1 is wrong', '#define N48DR_SEG1 0x000000c0u', '#define N48DR_SEG1 0x000000c4u')
plant(111, F, 'the census reads the M4a page twice (a second read of the PHY registers)', 'vals[i] = e.rd(r->base_idx, r->off); }', 'vals[i] = e.rd(r->base_idx, r->off); vals[i] = e.rd(r->base_idx, r->off); }')
plant(112, CLI, 'the CLI usage forgets the M4a pages', 'dispcensus [0|1|2..9]|region4read', 'dispcensus [0|1]|region4read')
plant(113, PURE, 'dispcensus is exempt for any page', '(action == kActDispCensus && n48dr_census_arg_ok(arg))', '(action == kActDispCensus)')
# ---- 0.0.630 (stage M4c): census pages 8 and 9 ----
plant(114, H, 'a register is dropped from the M4c table (UNIPHYC_LINK_CNTL)', '{ "UNIPHYC_LINK_CNTL", 2, 0x2871 }, ', '')
plant(115, H, 'a wrong offset in the M4c table (OTG1_OTG_CLOCK_CONTROL)', '{ "OTG1_OTG_CLOCK_CONTROL", 2, 0x1c04 }', '{ "OTG1_OTG_CLOCK_CONTROL", 2, 0x1c05 }')
plant(116, H, 'a wrong BASE_IDX in the M4c table (PHYPLLB_PIXCLK_RESYNC_CNTL)', '{ "PHYPLLB_PIXCLK_RESYNC_CNTL", 1, 0x0041 }, { "PHYPLLC_PIXCLK_RESYNC_CNTL", 1, 0x0042 }, { "PHYPLLD_PIXCLK_RESYNC_CNTL", 1, 0x0043 },\n    { "UNIPHYB', '{ "PHYPLLB_PIXCLK_RESYNC_CNTL", 2, 0x0041 }, { "PHYPLLC_PIXCLK_RESYNC_CNTL", 1, 0x0042 }, { "PHYPLLD_PIXCLK_RESYNC_CNTL", 1, 0x0043 },\n    { "UNIPHYB')
plant(117, H, 'the last plane page (42) is not a legal page (0.0.634: was page 9 before the plane census)', '#define N48DR_CENSUS_ALL_PAGES (N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 2u + 1u + N48DR_CENSUS5_PAGES + 1u + N48DR_CENSUS7_PAGES)', '#define N48DR_CENSUS_ALL_PAGES (N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 2u + 1u + N48DR_CENSUS5_PAGES + N48DR_CENSUS7_PAGES)')
plant(118, H, 'page 9 reads the wrong registers (starts at the table head)', 'return &n48dr_census3[N48DR_CENSUS3_PAGE8_COUNT + i];', 'return &n48dr_census3[i];')
plant(119, H, 'page 8 holds 24 (the UPDATE_LOCK slips onto page 8)', '#define N48DR_CENSUS3_PAGE8_COUNT 23u', '#define N48DR_CENSUS3_PAGE8_COUNT 24u')
plant(120, F, 'the census page read writes through the environment (a write member is called)', 'vals[i] = e.rd(r->base_idx, r->off); }', 'vals[i] = e.rd(r->base_idx, r->off); e.wr2(r->off, vals[i]); }')
# ---- 0.0.633: scdcread (verb 99) -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(130, H, 'scdc offsets reach 0xFF', '#define N48DR_SCDC_OFF_MAX 0x5Fu', '#define N48DR_SCDC_OFF_MAX 0xFFu')
plant(131, H, 'scdc reads may run past 0x5f (end 0x100)', '#define N48DR_SCDC_END 0x60u', '#define N48DR_SCDC_END 0x100u')
plant(132, H, 'scdc lengths reach 255', '#define N48DR_SCDC_LEN_MAX 16u', '#define N48DR_SCDC_LEN_MAX 255u')
plant(133, H, 'the scdc write address is 0xA0 (the EDID EEPROM)', '#define N48DR_SCDC_ADDR_W 0xA8u', '#define N48DR_SCDC_ADDR_W 0xA0u')
plant(134, H, 'the scdc read address is 0xA1', '#define N48DR_SCDC_ADDR_R 0xA9u', '#define N48DR_SCDC_ADDR_R 0xA1u')
plant(135, H, 'the scdc plan writes TWO data bytes', 'p->t[k].addr8 = N48DR_SCDC_ADDR_W; p->t[k].wlen = 1u;', 'p->t[k].addr8 = N48DR_SCDC_ADDR_W; p->t[k].wlen = 2u;')
plant(136, H, 'the scdc plan reads 128 bytes', 'p->t[k].addr8 = N48DR_SCDC_ADDR_R; p->t[k].is_read = 1u; p->t[k].rlen = (uint16_t)len;', 'p->t[k].addr8 = N48DR_SCDC_ADDR_R; p->t[k].is_read = 1u; p->t[k].rlen = 128u;')
plant(137, H, 'the scdc plan accepts any line', 'if (!n48dr_ddc_line_ok(line) || !n48dr_scdc_range_ok(off, len)) return N48DR_BAD_ARG;', 'if (!n48dr_scdc_range_ok(off, len)) return N48DR_BAD_ARG;')
plant(138, H, 'the scdc reply is read one byte early', 'p->read_index = p->fifo_len;   /* 3: the read data', 'p->read_index = (uint8_t)(p->fifo_len - 1u);   /* 3: the read data')
plant(139, F, 'scdc_read does not check the plan', 'if (n48dr_plan_scdc(line, off, len, &p) != 0u) return r;', '(void)n48dr_plan_scdc(line, off, len, &p);')
plant(140, F, 'scdc_read reads a whole EDID block', 'return i2c_xfer(e, line, p, len);', 'return i2c_xfer(e, line, p, N48DR_EDID_BLOCK);')
plant(141, G, 'scdcread is not single-flight', 'if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { n48dr_scdc_fill', 'if (false) { n48dr_scdc_fill')
plant(142, G, 'scdcread runs unarmed', 'if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) { n48dr_scdc_fill', 'if (false) { n48dr_scdc_fill')
plant(143, G, 'scdcread skips its argument check', 'if (!n48dr_scdc_arg_ok(arg)) {', 'if (false) {')
plant(144, G, 'scdcread never releases the busy flag', '\t__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE);\n\treturn r.status;\n}', '\treturn r.status;\n}')
plant(145, BRG, 'the scdcread dispatch skips the argument check', '\t\tif (!n48disp::verb_args_ok(action, argScalar)) {\n\t\t\tst = N48DR_BAD_ARG;\n\t\t\tv[0] = st;\n\t\t} else {\n\t\t\t(void)n48dcn::bind(this);\n\t\t\tst = n48dcn::scdcRead', '\t\tif (false) {\n\t\t\tst = N48DR_BAD_ARG;\n\t\t\tv[0] = st;\n\t\t} else {\n\t\t\t(void)n48dcn::bind(this);\n\t\t\tst = n48dcn::scdcRead')
plant(146, BRG, 'the scdcread dispatch does not bind the display layer', '\t\t\t(void)n48dcn::bind(this);\n\t\t\tst = n48dcn::scdcRead', '\t\t\tst = n48dcn::scdcRead')
plant(147, PURE, 'scdcread is exempt with any argument', '(action == kActScdcRead && n48dr_scdc_arg_ok(arg))', '(action == kActScdcRead)')
plant(148, PURE, 'scdcread is not admitted (bound 98)', 'constexpr uint32_t kLastAction = 99u;', 'constexpr uint32_t kLastAction = 98u;')
plant(149, H, 'channel lock bits are read one bit low', 'return (b >> (1u + ch)) & 1u; }', 'return (b >> ch) & 1u; }')
plant(150, H, 'clock detected reads bit 1', 'static inline int n48dr_scdc_clock_detected(uint8_t b) { return b & 1u; }', 'static inline int n48dr_scdc_clock_detected(uint8_t b) { return b & 2u; }')
plant(151, H, 'the error count keeps bit 15', '(uint32_t)lo | ((uint32_t)(hi & 0x7Fu) << 8)', '(uint32_t)lo | ((uint32_t)(hi & 0xFFu) << 8)')
plant(152, H, 'the error valid bit is bit 6', 'if (valid) *valid = (hi >> 7) & 1u;', 'if (valid) *valid = (hi >> 6) & 1u;')
plant(153, H, 'scrambling enabled reads bit 1', 'static inline int n48dr_scdc_scrambling_enabled(uint8_t b) { return b & 1u; }', 'static inline int n48dr_scdc_scrambling_enabled(uint8_t b) { return b & 2u; }')
plant(154, H, 'the locked verdict ignores channel 2', 'return n48dr_scdc_clock_detected(b) && n48dr_scdc_ch_locked(b, 0) && n48dr_scdc_ch_locked(b, 1) && n48dr_scdc_ch_locked(b, 2); }', 'return n48dr_scdc_clock_detected(b) && n48dr_scdc_ch_locked(b, 0) && n48dr_scdc_ch_locked(b, 1); }')
plant(155, H, 'the scdc answer drops the length', '((uint64_t)(len & 0xFFu) << 8) | ((uint64_t)(released & 1u) << 16) | ((uint64_t)(seq & 0xFFu) << 24)', '((uint64_t)(released & 1u) << 16) | ((uint64_t)(seq & 0xFFu) << 24)')
plant(156, H, 'the scdc extract takes the second scalar from the wrong byte', 'out16[i] = (uint8_t)(v[2u + i / 8u] >> (8u * (i & 7u)));', 'out16[i] = (uint8_t)(v[2u + i / 4u] >> (8u * (i & 7u)));')
plant(157, CLI, 'the CLI accepts any line / range', 'if (!n48dr_ddc_line_ok(line) || !n48dr_scdc_range_ok(off, len)) { fprintf(stderr, "accel scdcread:', 'if (0) { fprintf(stderr, "accel scdcread:')
plant(158, CLI, 'the CLI sends action 98 for scdcread', 'else if (what && !strcmp(what, "scdcread")) in = 99;', 'else if (what && !strcmp(what, "scdcread")) in = 98;')
plant(159, CLI, 'the CLI decodes TMDS_Config backwards', 'scrambling %s, TMDS bit clock ratio %s\\n", b[i], n48dr_scdc_scrambling_enabled(b[i]) ? "ENABLED" : "off", n48dr_scdc_ratio_by_40(b[i]) ? "1/40" : "1/10");', 'scrambling %s, TMDS bit clock ratio %s\\n", b[i], n48dr_scdc_ratio_by_40(b[i]) ? "ENABLED" : "off", n48dr_scdc_scrambling_enabled(b[i]) ? "1/40" : "1/10");')
# ---- 0.0.634: the plane census (pages 10..42): a wrong BASE_IDX / offset / duplicate / dropped / data-port / out-of-family entry, the page map, the CLI ------------------------------------------------------------
plant(160, H, 'a census5 entry has the wrong BASE_IDX (DPPCLK_CTRL is BASE_IDX 1)', '{ "DPPCLK_CTRL", 1, 0x00a8 },\n    { "DPPCLK0_DTO_PARAM", 1, 0x0099 },', '{ "DPPCLK_CTRL", 2, 0x00a8 },\n    { "DPPCLK0_DTO_PARAM", 1, 0x0099 },')
plant(161, H, 'a census5 entry has the wrong offset (DET2_CTRL)', '{ "DCHUBBUB_DET2_CTRL", 2, 0x04bd },', '{ "DCHUBBUB_DET2_CTRL", 2, 0x04be },')
plant(162, H, 'a census5 entry is duplicated (DET1 twice, DET2 dropped)', '{ "DCHUBBUB_DET2_CTRL", 2, 0x04bd },', '{ "DCHUBBUB_DET1_CTRL", 2, 0x04bc },')
plant(163, H, 'a data port is read (CM0_CM_GAMCOR_LUT_DATA replaces CM0_CM_GAMCOR_LUT_INDEX)', '{ "CM0_CM_GAMCOR_LUT_INDEX",', '{ "CM0_CM_GAMCOR_LUT_DATA",')
plant(164, H, 'a census5 entry is outside the named families (DIG0_DIG_FE_CNTL replaces OTG2_OTG_GLOBAL_CONTROL2)', '{ "MPC_OUT2_CSC_MODE", 3, 0x0325 },\n    { "OTG2_OTG_GLOBAL_CONTROL2", 2, 0x1c90 },', '{ "MPC_OUT2_CSC_MODE", 3, 0x0325 },\n    { "DIG0_DIG_FE_CNTL", 2, 0x2093 },')
plant(165, H, 'the DP watch (census4) swaps two registers', '{ "MPC_OUT0_MUX", 3, 0x02f2 },\n    { "MPCC0_MPCC_TOP_SEL", 3, 0x0000 },', '{ "MPCC0_MPCC_TOP_SEL", 3, 0x0000 },\n    { "MPC_OUT0_MUX", 3, 0x02f2 },')
plant(166, H, 'the plane pages start one page late', '#define N48DR_CENSUS5_FIRST_PAGE (N48DR_CENSUS4_PAGE + 1u)', '#define N48DR_CENSUS5_FIRST_PAGE (N48DR_CENSUS4_PAGE + 2u)')
plant(167, H, 'the last plane page is not short (page count 33)', '#define N48DR_CENSUS5_PAGES 32u', '#define N48DR_CENSUS5_PAGES 33u')
plant(168, H, 'the plane table size is off by one', '#define N48DR_CENSUS5_COUNT 762u', '#define N48DR_CENSUS5_COUNT 761u')
plant(169, H, 'the plane pages index the wrong table slot', 'return &n48dr_census5[(page - N48DR_CENSUS5_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i];', 'return &n48dr_census5[(page - N48DR_CENSUS5_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i + 1u];')
plant(170, F, 'the census sequence writes (a write member on the environment)', 'vals[i] = e.rd(r->base_idx, r->off); }', 'vals[i] = e.rd(r->base_idx, r->off); e.wr(r->base_idx, r->off); }')
plant(171, H, 'the first HUBP2 entry moves in front of OTG2_GLOBAL_CONTROL2 (instance 2 not last)', '{ "OTG2_OTG_GLOBAL_CONTROL2", 2, 0x1c90 },\n    { "HUBP2_DCSURF_SURFACE_CONFIG", 2, 0x079d },', '{ "HUBP2_DCSURF_SURFACE_CONFIG", 2, 0x079d },\n    { "OTG2_OTG_GLOBAL_CONTROL2", 2, 0x1c90 },')
plant(172, CLI, 'the CLI `plane` skips the DP watch page', 'for (uint32_t p = N48DR_CENSUS4_PAGE; p <= last && rc == 0; p++) rc = dc_print_page(p);', 'for (uint32_t p = N48DR_CENSUS5_FIRST_PAGE; p <= last && rc == 0; p++) rc = dc_print_page(p);')
plant(173, CLI, 'the CLI `plane` stops one page early', 'const uint32_t last = !strcmp(a1, "dpwatch") ? N48DR_CENSUS4_PAGE : N48DR_CENSUS6_PAGE;', 'const uint32_t last = !strcmp(a1, "dpwatch") ? N48DR_CENSUS4_PAGE : N48DR_CENSUS6_PAGE - 1u;')
plant(174, CLI, 'the CLI does not flush every page', 'fflush(stdout);       /* a hang on a clock-gated pipe must show which page it was */', '')

# ---- 0.0.635 (M4c): page 43 = census6, the rest of the 15-row DP watch ---------------------------------------------------------------------------------------------------------------------------------------
plant(175, H, 'census6: ODM0_OPTC_INPUT_GLOBAL_CONTROL is one dword off', '    { "ODM0_OPTC_INPUT_GLOBAL_CONTROL", 2, 0x1aca },', '    { "ODM0_OPTC_INPUT_GLOBAL_CONTROL", 2, 0x1acb },')
plant(176, H, 'census6: the HUBP0 primary HIGH dword is the LOW one', '    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x060b },\n};', '    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x060a },\n};')
plant(177, H, 'page 43 is not a census page (the total drops it)', 'N48DR_CENSUS5_PAGES + 1u + N48DR_CENSUS7_PAGES)', 'N48DR_CENSUS5_PAGES + N48DR_CENSUS7_PAGES)')
plant(178, H, 'page 43 holds no register', '    if (page == N48DR_CENSUS6_PAGE) return N48DR_CENSUS6_COUNT;', '    if (page == N48DR_CENSUS6_PAGE) return 0u;')
plant(179, H, 'page 43 reads the plane census table', '    if (page == N48DR_CENSUS6_PAGE) return &n48dr_census6[i];\n', '')
plant(180, H, 'census6 swaps DPP_TOP0 and MPCC0 UPDATE_LOCK_SEL (the watch rows 11 / 12)', '    { "DPP_TOP0_DPP_CONTROL", 2, 0x0cc5 },\n    { "MPCC0_MPCC_UPDATE_LOCK_SEL", 3, 0x0005 },', '    { "MPCC0_MPCC_UPDATE_LOCK_SEL", 3, 0x0005 },\n    { "DPP_TOP0_DPP_CONTROL", 2, 0x0cc5 },')
plant(181, H, 'page 43 loses its last register (the primary HIGH dword)', '    if (page == N48DR_CENSUS6_PAGE) return N48DR_CENSUS6_COUNT;', '    if (page == N48DR_CENSUS6_PAGE) return N48DR_CENSUS6_COUNT - 1u;')

# ---- 0.0.654 (the monitor A census): pages 44..59 = census7, the instance-1 twins of the pipe-2 rows + 13 singles; READS only ---------------------------------------------------------------------------------------
plant(190, H, 'census7: a wrong BASE_IDX (OTG1_OTG_H_TOTAL is BASE_IDX 2)', '    { "OTG1_OTG_H_TOTAL", 2, 0x1baa },', '    { "OTG1_OTG_H_TOTAL", 3, 0x1baa },')
plant(191, H, 'census7: a wrong offset (DENTIST_DISPCLK_CNTL is 0x64)', '    { "DENTIST_DISPCLK_CNTL", 1, 0x0064 },', '    { "DENTIST_DISPCLK_CNTL", 1, 0x0065 },')
plant(192, H, 'census7: a data port is read (CM1_CM_GAMCOR_LUT_DATA replaces CM1_CM_CONTROL)', '    { "CM1_CM_CONTROL", 2, 0x0ed2 },', '    { "CM1_CM_GAMCOR_LUT_DATA", 2, 0x0ee4 },')
plant(193, H, 'census7: a duplicate (DOMAIN1_PG_STATUS twice, DENTIST_DISPCLK_CNTL dropped)', '    { "DENTIST_DISPCLK_CNTL", 1, 0x0064 },', '    { "DOMAIN1_PG_STATUS", 2, 0x0083 },')
plant(194, H, 'census7: the HUBP_3DLUT_FL pair has the stride-0xdc offset (0x2d7 + 0xdc is not the header)', '    { "HUBP1_3DLUT_FL_CONFIG", 3, 0x02d7 },', '    { "HUBP1_3DLUT_FL_CONFIG", 2, 0x03b3 },')
plant(195, H, 'census7: the table size is off by one', '#define N48DR_CENSUS7_COUNT 361u', '#define N48DR_CENSUS7_COUNT 360u')
plant(196, H, 'census7: the pages index the wrong table slot', 'return &n48dr_census7[(page - N48DR_CENSUS7_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i];', 'return &n48dr_census7[(page - N48DR_CENSUS7_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i + 1u];')
plant(197, H, 'census7: the last page is full-sized (page 59 holds 24)', '        return N48DR_CENSUS7_COUNT - f7 > N48DR_CENSUS_PER_PAGE ? N48DR_CENSUS_PER_PAGE : N48DR_CENSUS7_COUNT - f7;', '        return N48DR_CENSUS_PER_PAGE;')
plant(198, H, 'census7: a pipe-0 register replaces an instance-1 one (HUBP0_DCHUBP_CNTL for HUBP1_DCHUBP_CNTL)', '    { "HUBP1_DCHUBP_CNTL", 2, 0x06d0 },', '    { "HUBP0_DCHUBP_CNTL", 2, 0x05f4 },')
plant(199, H, 'census7: MPC_OUT2_MUX replaces MPC_OUT1_MUX (the monitor B\'s mux read as the monitor A\'s)', '    { "MPC_OUT1_MUX", 3, 0x02f6 },', '    { "MPC_OUT1_MUX", 3, 0x02fa },')
plant(200, H, 'census7: page 44 starts one page late', '#define N48DR_CENSUS7_FIRST_PAGE 44u', '#define N48DR_CENSUS7_FIRST_PAGE 45u')
plant(201, H, 'census7: a page past the last (60) is legal', '+ 1u + N48DR_CENSUS7_PAGES)\n', '+ 1u + N48DR_CENSUS7_PAGES + 1u)\n')
plant(202, G, 'census7: a file that writes registers references the census7 table', '#include "navi48_dispread_flow.h"', 'static const void *const gN48CensusLeak = &n48dr_census7[0];\n#include "navi48_dispread_flow.h"')
plant(203, CLI, 'census7: `dispcensus mona` starts at the plane pages', 'for (uint32_t p = N48DR_CENSUS7_FIRST_PAGE; p < N48DR_CENSUS_ALL_PAGES && rc == 0; p++) rc = dc_print_page(p);', 'for (uint32_t p = N48DR_CENSUS4_PAGE; p < N48DR_CENSUS_ALL_PAGES && rc == 0; p++) rc = dc_print_page(p);')
plant(204, CLI, 'census7: `dispcensus mona` stops one page early', 'p < N48DR_CENSUS_ALL_PAGES && rc == 0; p++) rc = dc_print_page(p);\n        return rc;\n    }\n    if (a1 && (!strcmp(a1, "plane")', 'p < N48DR_CENSUS_ALL_PAGES - 1u && rc == 0; p++) rc = dc_print_page(p);\n        return rc;\n    }\n    if (a1 && (!strcmp(a1, "plane")')
plant(205, H, 'census7: HUBPREQ1_DCSURF_SURFACE_PITCH dropped (a block register lost, a trailing duplicate of the next added)', '    { "HUBPREQ1_DCSURF_SURFACE_PITCH", 2, 0x06e3 },\n', '')

shutil.rmtree(scr, ignore_errors=True)
print('native_dispread_plant: %d plant(s), %d escaped' % (total, escaped))
sys.exit(1 if escaped else 0)
PY
