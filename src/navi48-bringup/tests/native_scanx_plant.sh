#!/bin/zsh
# native_scanx_plant.sh - planted breaks for tests/native_scanx_test.cpp (build 0.0.661, M6 Stage 1b: the monitor B's scanout, instance 2: HUBP2 / OTG2, ABI 1.11, behind navi48-m6=1 AND navi48-m6flip=1; the review items R2 R3 R4 R5).
# Discipline (as native_disp2_plant.sh): copy the sources the test reads into a scratch tree, apply ONE break to the REAL code (the flow header, the pure table header, the kernel glue, the dispatch, the policy, the ABI, the CLI, the plist),
# build the real test against the copy and demand it FAILS (a compile error, a failing check, a crash or a hang all count). A plant whose text is not found exactly once is itself reported as an escape.
# The CONTROL (no break) must pass.   run from anywhere:  src/navi48-bringup/tests/native_scanx_plant.sh [first-plant-id last-plant-id]
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec python3 -u - "$ROOT" "${1:-0}" "${2:-9999}" <<'PY'
import os, shutil, subprocess, sys, tempfile, time

ROOT, FIRST, LAST = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
K = 'src/navi48-bringup'
FLOW, PURE, DISP2, M6P, GLUE, S1C, S1CH, CLIENT, POL, ABI, DISPC, DFLOW, BRG, PLIST, CLI, DPURE = (f'{K}/src/dcn/navi48_scanx_flow.h', f'{K}/src/dcn/navi48_scanout_pure.h', f'{K}/src/dcn/navi48_disp2.h', f'{K}/src/amd/native_m6_pure.h',
    f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/amd/native_s1c.cpp', f'{K}/src/amd/native_s1c.h', f'{K}/src/Navi48NativeClient.cpp', f'{K}/src/amd/native_open_policy_pure.h', f'{K}/src/Navi48NativeABI.h', f'{K}/src/amd/native_disp.cpp',
    f'{K}/src/amd/native_disp_flow.h', f'{K}/src/Navi48Bringup.cpp', f'{K}/Info.plist', 'tools/pc/navi48test.c', f'{K}/src/amd/native_disp_pure.h')
FILES = [FLOW, PURE, DISP2, M6P, GLUE, S1C, S1CH, CLIENT, POL, ABI, DISPC, DFLOW, BRG, PLIST, CLI, DPURE, f'{K}/src/Navi48DisplayOps.h', f'{K}/src/apple/display_pipe_guard.h', f'{K}/tests/native_scanx_test.cpp', f'{K}/tests/native_scanx_plant.sh',
         'variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist', 'variants/configs/stage17-native-1440-metal-disp-amfi-m6flip.plist', 'variants/configs/stage17-native-1440-metal-disp-amfi-m6flip3.plist']
import glob
FILES += [os.path.relpath(f, ROOT) for pat in (f'{K}/src/*.h', f'{K}/src/amd/*.h', f'{K}/src/dcn/*.h', f'{K}/src/apple/display_pipe_guard.h') for f in glob.glob(os.path.join(ROOT, pat))]
FILES = sorted(set(FILES))
scr = tempfile.mkdtemp(prefix='nscanx-plant.')
def fresh():
    shutil.rmtree(scr, ignore_errors=True)
    for f in FILES:
        d = os.path.join(scr, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(ROOT, f), d)
def build_run():
    cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', f'{K}/src', '-I', f'{K}/src/dcn', '-I', f'{K}/src/amd',
           f'{K}/tests/native_scanx_test.cpp', '-o', os.path.join(scr, 't')]
    r = subprocess.run(cmd, cwd=scr, capture_output=True, text=True)
    if r.returncode != 0:
        first = next((l for l in (r.stderr + r.stdout).splitlines() if 'error' in l), '')
        return 2, 'CAUGHT at compile time: ' + first[:140]
    try:
        r = subprocess.run([os.path.join(scr, 't'), '.'], cwd=scr, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return 1, 'CAUGHT by a hang'
    if os.environ.get('N48_PLANT_SHOW'): print('\n'.join((r.stdout + r.stderr).splitlines()[-40:]))
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
    plantn(pid, desc, [(path, old, new)])
def plantn(pid, desc, edits):
    """edits: [(path, old, new)] - every old text must occur EXACTLY ONCE in its file."""
    global total, escaped
    if pid < FIRST or pid > LAST: return
    fresh(); total += 1
    texts = {}
    for path, old, new in edits:
        f = os.path.join(scr, path); s = texts.get(path) or open(f).read()
        n = s.count(old)
        if n != 1:
            print('PLANT %d: %s: PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r' % (pid, desc, n, old[:70])); escaped += 1; return
        texts[path] = s.replace(old, new)
    if os.environ.get('N48_PLANT_DRY'): print('PLANT %d: text found exactly once (dry run)' % pid); return
    for path, s in texts.items(): open(os.path.join(scr, path), 'w').write(s)
    t0 = time.time(); rc, msg = build_run(); dt = time.time() - t0
    if rc == 0: print('PLANT %d: %s: *** ESCAPED (%s) *** [%.1fs]' % (pid, desc, msg, dt)); escaped += 1
    else: print('PLANT %d: %s: %s [%.1fs]' % (pid, desc, msg, dt))

if FIRST == 0: control()
# ---- the spec's three ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(1, FLOW, 'SPEC 1: a slot outside an instance\'s allowlist - an instance-0 / untagged id is accepted as a present target', 'constexpr bool id_ok(const XDesc &d, uint64_t id) { return id >= (uint64_t)d.slotTag && id < (uint64_t)(d.slotTag + n48scan::kMaxSlots); }', 'constexpr bool id_ok(const XDesc &d, uint64_t id) { return id < (uint64_t)(d.slotTag + n48scan::kMaxSlots); }')
plant(2, FLOW, 'SPEC 1: ... the writer admits any address (no registered-slot test)', 'else if (mode == kModePresent) ok = mc != 0ull && mc != x.bufA && mc != x.bufB && n48scan::flip_target_ok(x.tbl, mc);', 'else if (mode == kModePresent) ok = mc != 0ull && mc != x.bufA && mc != x.bufB;')
plant(3, FLOW, 'SPEC 2: a flip to A through the present path (the writer no longer excludes A: flip_target_ok admits the console)', 'ok = mc != 0ull && mc != x.bufA && mc != x.bufB && n48scan::flip_target_ok(x.tbl, mc);', 'ok = mc != 0ull && mc != x.bufB && n48scan::flip_target_ok(x.tbl, mc);')
plantn(4, 'SPEC 2: a flip to B through the present path (register admits B, the writer does not exclude it)', [(FLOW, 'ok = mc != 0ull && mc != x.bufA && mc != x.bufB && n48scan::flip_target_ok(x.tbl, mc);', 'ok = mc != 0ull && mc != x.bufA && n48scan::flip_target_ok(x.tbl, mc);'), (FLOW, 'if (ranges_overlap(mc, bytes, x.bufB, d.allocBytes) || ranges_overlap(mc, bytes, x.bufA, d.allocBytes)) { rc = kBadArg; break; }   // never B, and never A\'s whole allocation (not just the frame): 0.0.662 judges the ALLOCATION size', '')])
plant(5, S1C, 'SPEC 3: no instance-2 restore in the N48N close / teardown (restore2 is a no-op)', 'void restore2(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(2u, how, r); }', 'void restore2(const char *how, uint64_t r[2]) { (void)how; (void)r; }')
plant(6, FLOW, 'SPEC 3: the teardown flow skips restore2', '    t.restore2(how, r2);                                         // before ANY BO is touched\n', '')
plant(7, S1C, 'SPEC 3: bo_release forgets instance 2\'s pins (a BO with instance-2 slots is freed without Restore2)', '    if (b.pinMask2 != 0u && scan_unpin2(h, b, mode)) b.pinLeak = 1u; // 0.0.661: and instance 2\'s A before ITS slots\' memory\n', '')
# ---- the contract's additions -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(10, FLOW, 'mixed halves: the writer no longer judges the whole address (HIGH may differ from A\'s)', ' if (ok && ((mc >> 48) != 0ull || (mc >> 32) != (x.bufA >> 32) || (mc & 0xFFFFull) != 0ull)) ok = false;', ' if (ok && ((mc >> 48) != 0ull || (mc & 0xFFFFull) != 0ull)) ok = false;')
plant(11, FLOW, 'reuse judged on HUBP0\'s EARLIEST_INUSE (the status flags read a HUBP0 register)', 'const uint32_t pl = e.rd(d.regAddr), ph = e.rd(d.regAddrHigh), el = e.rd(d.regEarlyLo), eh', 'const uint32_t pl = e.rd(d.regAddr), ph = e.rd(d.regAddrHigh), el = e.rd(d.regEarlyLo - 2u * 0xdcu), eh')
plant(12, FLOW, 'no idle watchdog for instance 2', '    else if (n48scan::idle_expired(e.now_ns(), x.lastActivityNs)) why = "idle watchdog: 5 s without a scanout call";\n', '')
plant(13, FLOW, 'Restore2 is not called on BoFree of a slot being shown (the plan is never "full")', '        full = plan.fullRelease;\n', '        full = false;\n')
plant(14, FLOW, 'a present is allowed during a restore / with wantRestore', '        if (x.restoring || x.wantRestore) { x.refused++; rc = kNotReady; break; }\n        x.lastActivityNs = e.now_ns();\n        Geom gm; read_geom(e, d, &gm);\n        if (!geom_good(x, gm)) {', '        x.lastActivityNs = e.now_ns();\n        Geom gm; read_geom(e, d, &gm);\n        if (!geom_good(x, gm)) {')
plantn(15, 'Acquire2 without HELD (both stage checks gone)', [(FLOW, 'if (e.d2_stage() != N48D2_PL_HELD) { x.lastAcquireWhy = kWhyStage;', 'if (false) { x.lastAcquireWhy = kWhyStage;'), (FLOW, 'if (e.d2_stage() != N48D2_PL_HELD || e.d2_mc(0) != A || e.d2_mc(1) != B) {', 'if (e.d2_mc(0) != A || e.d2_mc(1) != B) {')])
plant(16, S1C, 'the latch-OFF path reaches the new code: SCANX_ACQUIRE has no latch gate', 'IOReturn n1c_scanx_acquire(const N1cRef &ref, uint64_t inst, uint64_t flags, uint64_t out[5]) {\n    if (!sess_hello(ref)) return kIOReturnNotReady;\n    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;\n    if (!scanx_on()) return kIOReturnUnsupported;\n', 'IOReturn n1c_scanx_acquire(const N1cRef &ref, uint64_t inst, uint64_t flags, uint64_t out[5]) {\n    if (!sess_hello(ref)) return kIOReturnNotReady;\n    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;\n')
plant(17, GLUE, 'the latch-OFF path reaches the new code: the glue\'s Present has no latch test', 'uint32_t scanXPresent(uint32_t inst, uint64_t slotId, uint64_t flags, uint64_t out[3]) {\n\tconst n48scanx::XDesc *d = n48scanx::desc_of(inst);\n\tif (d == nullptr || d->inst != inst) return n48scan::kBadArg;\n\tif (!sx_latched_inst(inst)) return n48scan::kUnsupported;\n', 'uint32_t scanXPresent(uint32_t inst, uint64_t slotId, uint64_t flags, uint64_t out[3]) {\n\tconst n48scanx::XDesc *d = n48scanx::desc_of(inst);\n\tif (d == nullptr || d->inst != inst) return n48scan::kBadArg;\n')
plant(18, GLUE, 'the gate stops asking the latches first', '\tuint32_t gate() {\n\t\tif (!sx_latched_inst(d.inst)) return n48scan::kUnsupported;\n', '\tuint32_t gate() {\n')
plant(19, FLOW, 'a write to 0x3c8b (FLIP_CONTROL) is admitted by the allowlist', 'constexpr bool write_reg_ok(const XDesc &d, uint32_t abs) { return abs == d.regAddrHigh || abs == d.regAddr; }', 'constexpr bool write_reg_ok(const XDesc &d, uint32_t abs) { return abs == d.regAddrHigh || abs == d.regAddr || abs == d.regFlipControl; }')
plant(20, FLOW, 'swapped register order: LOW before HIGH', '    if (!e.wr(d.regAddrHigh, (uint32_t)(mc >> 32))) { x.writeFailed++; return false; }\n    if (!e.wr(d.regAddr, (uint32_t)mc)) { x.writeFailed++; return false; }\n', '    if (!e.wr(d.regAddr, (uint32_t)mc)) { x.writeFailed++; return false; }\n    if (!e.wr(d.regAddrHigh, (uint32_t)(mc >> 32))) { x.writeFailed++; return false; }\n')
plant(21, FLOW, 'the writer also programs FLIP_CONTROL after the address', '    x.lastWritten = mc;\n    return true;\n}', '    (void)e.wr(d.regFlipControl, 0u);\n    x.lastWritten = mc;\n    return true;\n}')
# ---- Acquire2 preconditions, one by one ----------------------------------------------------------------------------------------------------------------------------------------------------------
plant(30, FLOW, 'Acquire2 does not require OTG2 to be counting', "if (fc0 == 0xFFFFFFFFu || fc1 == 0xFFFFFFFFu || ((fc0 ^ fc1) & kFcMask) == 0u) {", "if (false) {")
plant(31, FLOW, 'Acquire2 does not judge the viewport / pitch / format / swizzle / SURFACE_CONTROL', "if (dim != (d.w | (d.h << 16)) || pitch != d.pitchReg || cfg != kFmtArgb8888 || (tile & 0x1Fu) != 0u || ctl != 0u ||\n            geom_check(gm.w, gm.h, gm.pitchPx, gm.fmt, gm.sw, gm.dcc) != kGeomOk) {", "if (dim == 0xFFFFFFFFu && pitch == 0xFFFFFFFFu && cfg == 0xFFFFFFFFu && tile == 0xFFFFFFFFu && ctl == 0xFFFFFFFFu) {")
plant(32, FLOW, 'Acquire2 does not demand an exactly restorable plane (VMID / FLIP_TYPE / viewport start)', 'if (!restore_exact(vmid & 0xFu, (ctl & 0x1u) != 0u, (fcr & kFlipTypeMask) >> 1, vps)) {', 'if (vmid == 0xFFFFFFFFu && fcr == 0xFFFFFFFFu && vps == 0xFFFFFFFFu) {')
plant(33, FLOW, 'Acquire2 does not require programmed == EARLIEST == A', 'if (prog != A || early != A) {', 'if (prog != A) {')
plant(34, FLOW, 'Acquire2 ignores a pending flip', 'if (pend) { x.lastAcquireWhy = kWhyPending;', 'if (false) { x.lastAcquireWhy = kWhyPending;')
plant(35, FLOW, 'Acquire2 ignores underflow / blank / VTG_SEL', "if ((hc & N48D2_HUBP_UNDERFLOW_MASK) != 0u || (hc & N48D2_HUBP_BLANK_EN_MASK) != 0u || ((hc >> 4) & 0xFu) != d.otgInst) {", "if (hc == 0xFFFFFFFFu) {")
plant(36, FLOW, 'Acquire2 ignores a VM fault', 'if (e.rd(d.regVmFault) != 0u) {', 'if (false) {')
plant(37, FLOW, 'Acquire2 accepts disp2 saying B is the front', 'if (e.d2_cur() != 0u) { x.lastAcquireWhy = kWhyCur;', 'if (false) { x.lastAcquireWhy = kWhyCur;')
plant(38, FLOW, 'Acquire2 does not start (or does not need) the watchdog', 'if (!e.start_watchdog(x.gen)) {', 'if (false) {')
plant(39, FLOW, 'Acquire2 sleeps under the lock (the counting wait keeps the lock)', '    e.unlock();\n    e.sleep_ms(kCountWaitMs);                                    // the OTG must COUNT: judged over a real interval, with the lock dropped\n    e.lock();', '    e.sleep_ms(kCountWaitMs);\n')
# ---- Register2 / Present2 -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(40, FLOW, 'Register2 forgets the DP / monitor A ranges', '        for (uint32_t i = 0; i < ex.n && i < kMaxExcl; i++) if (ex.bytes[i] != 0ull && ranges_overlap(mc, bytes, ex.lo[i], ex.bytes[i])) clash = true;', '        (void)ex;')
plant(41, FLOW, 'Register2 returns UNtagged slot ids', 'out[0] = id_of(d, slot); out[1] = mc;', 'out[0] = slot; out[1] = mc;')
plant(42, FLOW, 'Register2 skips the plane geometry', '        if (!geom_good(x, gm)) { rc = kNotReady; break; }\n        if (in.width', '        if (in.width')
plant(43, FLOW, 'Present2 never refuses a slot the hardware is fetching / that is pending', '        if ((x.tbl.pendActive && x.tbl.pendSlot == k) || early == x.tbl.s[k].mc) { x.reuseInuseRefused++; x.refused++; rc = kBusy; break; }\n', '')
plant(44, FLOW, 'Present2 refuses only on EARLIEST (a pending slot may be programmed again)', '(x.tbl.pendActive && x.tbl.pendSlot == k) || early == x.tbl.s[k].mc', 'early == x.tbl.s[k].mc')
plant(45, FLOW, 'Present2 does not count the refusal', 'x.reuseInuseRefused++; x.refused++; rc = kBusy; break;', 'x.refused++; rc = kBusy; break;')
plant(46, FLOW, 'a failed present write does not ask for the restore (the half-written address is left)', '            x.wantRestore = true; x.wantWhy = "a present write failed (fail toward A)";    // the plane may hold a half-written address: the watchdog restores A\n', '')
plant(47, FLOW, 'a failed present write leaves the table bookkeeping (pending forever)', '            x.tbl = saved; x.refused++;\n            x.wantRestore', '            x.refused++;\n            x.wantRestore')
plant(48, FLOW, 'Present2 does not poll the previous latch first (replaced counts wrongly)', '        (void)poll_locked(e, d, x);                                  // resolve the previous Present first: a latch not seen yet is not "replaced"\n', '')
plant(49, FLOW, 'Present2 forgets to re-read the geometry', '        Geom gm; read_geom(e, d, &gm);\n        if (!geom_good(x, gm)) {\n            x.geomRefused++;', '        Geom gm; read_geom(e, d, &gm); gm = Geom{ x.width, x.height, x.pitchPx, 8u, 0u, false };\n        if (!geom_good(x, gm)) {\n            x.geomRefused++;')
# ---- latch bookkeeping, Status2, the keep-alive ---------------------------------------------------------------------------------------------------------------------------------------------------
plant(50, FLOW, 'the latch is keyed on EARLIEST instead of the raw pending bit', 'if (!n48scan::latch_observe(x.tbl, (fcr & kFlipPendingMask) != 0u, fcNow)) return false;', 'if (!n48scan::latch_observe(x.tbl, false, fcNow)) return false;')
plant(51, FLOW, 'the monitor B\'s frame counter is OTG1\'s (the stamps and the latch frame would come from the other display)', 'N48D2_OTG2_FRAME_COUNT, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK', 'N48D2_OTG1_FRAME_COUNT, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK')
plant(52, FLOW, 'Status2 is not the keep-alive (it does not count as activity)', '    if (x.acquired && !x.restoring) { x.lastActivityNs = e.now_ns(); (void)poll_locked(e, d, x); }       // a status call IS the keep-alive: it counts as activity', '    if (x.acquired && !x.restoring) { (void)poll_locked(e, d, x); }')
plant(53, FLOW, 'Status2 reports slot ids untagged', 'o->frontSlot = x.tbl.front == kNoSlot ? kNoSlot : id_of(d, x.tbl.front);', 'o->frontSlot = x.tbl.front;')
plant(54, FLOW, 'Status2 reports B as the console', 'o->consoleMc = x.haveConsole ? x.bufA : prog;', 'o->consoleMc = x.haveConsole ? x.bufB : prog;')
# ---- Restore2 -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(60, FLOW, 'Restore2 never polls (reports verified at once)', '        for (uint32_t i = 0; i < kPolls; i++) {\n            e.unlock();', '        for (uint32_t i = 0; i < 0u; i++) {\n            e.unlock();')
plant(61, FLOW, 'Restore2 sleeps with the lock held', '            e.unlock();\n            e.sleep_ms(1u);                                          // a latch happens within a frame; polled, never waited for by interrupt\n            e.lock();\n', '            e.sleep_ms(1u);\n')
plant(62, FLOW, 'Restore2 claims verified without the readback', '    verified = wrote && rd && restore_verified(prog, early, pend, A);\n', '    verified = true;\n')
plant(63, FLOW, 'Restore2 does not leave disp2\'s cur = A', '    if (verified) e.d2_set_cur_a();', '    if (false) e.d2_set_cur_a();')
plant(64, FLOW, 'Restore2 is not idempotent (writes again when nothing is acquired)', '    if (!x.acquired) {                                           // nothing to do;', '    if (!x.acquired && x.restoreFailed) {                                           // nothing to do;')
plant(65, FLOW, 'Restore2 forgets restoreFailed (an unverified restore is not remembered: the BO would be freed)', '    x.restoreFailed = !verified;\n    if (verified) e.d2_set_cur_a();', '    x.restoreFailed = false;\n    if (verified) e.d2_set_cur_a();')
plant(66, FLOW, 'Restore2 does not tear the table down', '    table_init(x.tbl, A, d.bytes);\n    x.acquired = false;', '    x.acquired = false;')
plant(67, FLOW, 'Restore2 writes through the PRESENT mode (a slot allowlist instead of A only)', 'bool wrote = write_addr(e, d, x, A, kModeRestore);', 'bool wrote = write_addr(e, d, x, A, kModePresent);')
plant(68, FLOW, 'the restore writer admits any 48-bit address', 'if (mode == kModeRestore) ok = x.bufA != 0ull && mc == x.bufA;', 'if (mode == kModeRestore) ok = x.bufA != 0ull;')
plant(69, FLOW, 'an expectGen of another acquisition restores anyway', 'if (expectGen != 0u && x.gen != expectGen) { e.unlock(); if (out) { out[0] = 1; out[1] = 0; } return 0u; }', '(void)expectGen;')
# ---- the watchdog ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(70, FLOW, 'the watchdog ignores wantRestore', '    if (x.wantRestore) why = x.wantWhy ? x.wantWhy : "requested";\n    else if (!geom_good(x, gm))', '    if (false) why = x.wantWhy ? x.wantWhy : "requested";\n    else if (!geom_good(x, gm))')
plant(71, FLOW, 'the watchdog ignores a geometry change', '    else if (!geom_good(x, gm)) why = "live plane geometry changed";\n', '')
plant(72, FLOW, 'the watchdog does not count its restores', '    if (why) x.wdRestores++;\n', '')
plant(73, FLOW, 'the watchdog runs for any generation (a stale thread restores a new acquisition)', '    if (!x.acquired || x.gen != myGen) { e.unlock(); return true; }\n', '    if (!x.acquired) { e.unlock(); return true; }\n')
plant(74, FLOW, 'the watchdog does not poll (latches are seen only by callers)', '    (void)poll_locked(e, d, x);\n    Geom gm; read_geom(e, d, &gm);\n    // S3:', '    Geom gm; read_geom(e, d, &gm);\n    // S3:')
# ---- teardown / bo_gone ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(80, FLOW, 'teardown restores instance 2 BEFORE instance 0 (the order of the two is the contract\'s)', '    t.restore0(how, r0);\n    t.restore1(how, r1);\n    t.restore2(how, r2);                                         // before ANY BO is touched', '    t.restore2(how, r2);\n    t.restore1(how, r1);\n    t.restore0(how, r0);                                         // before ANY BO is touched')
plant(81, FLOW, 'an unverified instance-2 restore does not leak its BOs', '        if (r2[0] == 0ull && t.pin2(h) != 0u) t.mark_leak(h);\n', '')
plant(82, FLOW, 'an unverified instance-0 restore leaks nothing', '        if (r0[0] == 0ull && t.pin0(h) != 0u) t.mark_leak(h);\n', '')
plant(83, FLOW, 'the teardown touches a BO before the restores finish', '    t.restore0(how, r0);\n    t.restore1(how, r1);\n    t.restore2(how, r2);                                         // before ANY BO is touched\n    for (uint32_t h = 1; h < t.nbo(); h++) {\n        if (!t.used(h)) continue;', '    for (uint32_t h = 1; h < 2u; h++) (void)t.used(h);\n    t.restore0(how, r0);\n    t.restore1(how, r1);\n    t.restore2(how, r2);                                         // before ANY BO is touched\n    for (uint32_t h = 1; h < t.nbo(); h++) {\n        if (!t.used(h)) continue;')
plant(84, FLOW, 'bo_gone reports verified for an unverified earlier restore', '    if (!x.acquired && x.gen == pinGen && x.restoreFailed) rc = 2u;', '')
plant(85, FLOW, 'bo_gone drops a pending slot without the restore', 'const UnpinPlan plan = unpin_plan(x.tbl, (uint8_t)pinMask, alwaysFull || x.restoring, early, ok);', 'const UnpinPlan plan = unpin_plan(x.tbl, (uint8_t)pinMask, alwaysFull || x.restoring, 0ull, true);')
plant(86, S1C, 'scan_teardown restores only instance 0 (the pre-0.0.661 body)', '    n48scanx::teardown_all(te, how, r, r1, r2);\n', '    (void)n48dcn::scanRelease(how, r); (void)te; (void)r1; (void)r2;\n')
plant(87, GLUE, 'the kext stop forgets instance 2', '\tscanXShutdown();                        // 0.0.661 (M6 Stage 1b): instance 2\'s A is restored (and its watchdog waited for) before the registers go away; inert when the latches are OFF\n', '')
# ---- disp2 interplay / pins -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(90, FLOW, 'GPU-held reports a latch failure (the latch bit stays in the hold verdict for codes 1..3)', '(((heldCode >= 1u && heldCode <= 3u) ? (bad & ~(uint32_t)N48D2_H_LATCH) : bad) | kHoldGpuHeld);', '(bad | kHoldGpuHeld);')
plant(91, FLOW, 'GPU-held code 0 for an acquired plane', 'if (!acquired) return 0u;\n    for', 'if (!acquired && false) return 0u;\n    for')
plant(93, GLUE, 'fbLive keeps the latch bit while GPU-held (a held monitor B reads as a latch failure)', '*holdBad = n48scanx::hold_verdict_gpu(n48d2_hold_verdict(g, pl->cur, pl->mc[0]), scanXGpuHeld(inst));', '*holdBad = n48d2_hold_verdict(g, pl->cur, pl->mc[0]);')
plant(94, GLUE, 'disp2 status / crc say nothing about GPU-held', '	if ((op == N48D2_OP_STATUS || op == N48D2_OP_CRC) && (inst == N48D2_INST_MONB || inst == N48D2_INST_MONA)) {', '	if (false && (op == N48D2_OP_STATUS || op == N48D2_OP_CRC) && (inst == N48D2_INST_MONB || inst == N48D2_INST_MONA)) {')
plant(92, DISP2, 'the disp2 guard admits any HIGH half (a slot address passes the A / B set)', 'static inline int n48d2_aset_lo_ok_i(uint32_t inst, uint32_t v) { const struct n48d2_aset *x = n48d2_aset_get_i(inst); unsigned i; for (i = 0; i < x->n; i++) if (x->lo[i] == v) return 1; return 0; }', 'static inline int n48d2_aset_lo_ok_i(uint32_t inst, uint32_t v) { (void)inst; (void)v; return 1; }')
# ---- ABI / policy / client / CLI / versions --------------------------------------------------------------------------------------------------------------------------------------------------------
plant(100, POL, 'a uid-88 (WindowServer) client cannot reach selectors 22..26', ' ||\n           (sel >= (uint32_t)N48N_SEL_SCANX_ACQUIRE && sel <= (uint32_t)N48N_SEL_SCANX_RELEASE);', ';\n           (void)0;')
plant(101, POL, 'an APP client reaches selectors 22..26', 'return sel == (uint32_t)N48N_SEL_HELLO || sel == (uint32_t)N48N_SEL_QUERYINFO ||\n           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;', 'return sel == (uint32_t)N48N_SEL_HELLO || sel == (uint32_t)N48N_SEL_QUERYINFO ||\n           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST || (sel >= 22u && sel <= 26u);')
plant(102, ABI, 'the ABI minor stays 10', '#define N48N_ABI_MINOR     12u ', '#define N48N_ABI_MINOR     10u ')
plant(103, ABI, 'selector 24 is renumbered', 'N48N_SEL_SCANX_PRESENT  = 24,', 'N48N_SEL_SCANX_PRESENT  = 28,')
plant(104, ABI, 'the m6flip flag bit is the m6 bit', '#define N48N_SCANQ_M6FLIP     (1u << 6)', '#define N48N_SCANQ_M6FLIP     (1u << 5)')
plant(105, CLIENT, 'the client\'s Present shape check accepts the wrong scalar count', 'case N48N_SEL_SCANX_PRESENT:\n\t\tif (!shape(3, 3, 0, 0))', 'case N48N_SEL_SCANX_PRESENT:\n\t\tif (!shape(2, 3, 0, 0))')
plant(106, S1C, 'SCANX_PRESENT does not require the session to hold instance 0', '    if (scan_foreign(s) || !scanx_owner(s)) return kIOReturnNotPermitted;\n    return (IOReturn)n48dcn::scanXPresent((uint32_t)inst, slot, flags, out);', '    if (scan_foreign(s)) return kIOReturnNotPermitted;\n    return (IOReturn)n48dcn::scanXPresent((uint32_t)inst, slot, flags, out);')
plant(107, S1C, 'SCANX_REGISTER checks the instance before the latches (a read before the refusal order)', 'IOReturn n1c_scanx_register(const N1cRef &ref, uint64_t inst, const n48n_scan_reg *in, uint64_t out[2]) {\n    if (!sess_hello(ref)) return kIOReturnNotReady;\n    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;\n    if (!scanx_on()) return kIOReturnUnsupported;\n    if (!scanx_inst_ok(inst) || in->reserved0 != 0u) return kIOReturnBadArgument;', 'IOReturn n1c_scanx_register(const N1cRef &ref, uint64_t inst, const n48n_scan_reg *in, uint64_t out[2]) {\n    if (!sess_hello(ref)) return kIOReturnNotReady;\n    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;\n    if (!scanx_inst_ok(inst) || in->reserved0 != 0u) return kIOReturnBadArgument;\n    if (!scanx_on()) return kIOReturnUnsupported;')
plant(108, GLUE, 'the glue\'s register write skips the allowlist', '\t\tif (!dcn41_allow_write(&gDcn.allow, abs, v, "scanx")) { N48LOG("scanx: the DCN allowlist REFUSED write %#010x = %#010x (instance %u)", abs, v, d.inst); return false; }\n', '')
plant(109, GLUE, 'the glue\'s register write skips write_reg_ok', '\t\tif (!n48scanx::write_reg_ok(d, abs)) { N48LOG("scanx: write of %#06x REFUSED (instance %u): not this HUBP\'s primary address HIGH / LOW", abs, d.inst); return false; }\n', '')
plant(110, GLUE, 'the snapshot of the DP / monitor A ranges is taken AFTER instance 2\'s lock (lock-order break)', '\tsx_excl_snapshot(inst, &ex);\n\tconst n48scanx::RegIn in', '\tconst n48scanx::RegIn in')
plant(111, GLUE, 'instance 0\'s Register no longer refuses a clash with instance 2', 'n48m6_latched_on() && n48m6flip_latched_on() && scanXClash(mc, bytes)', 'false && scanXClash(mc, bytes)')
plant(112, GLUE, 'scan_query does not report the m6flip latch', 'if (n48m6_latched_on() && n48m6flip_latched_on()) flags |= N48N_SCANQ_M6FLIP;', 'if (false) flags |= N48N_SCANQ_M6FLIP;')
plant(113, CLI, 'the CLI has no m6xstat verb', 'else if (what && !strcmp(what, "m6xstat")) in = 107;', '')
plant(114, BRG, 'accelExperiment does not serve action 107', 'n48dcn::scanXReport(argScalar, v, 13)', 'n48dcn::scanXReport(argScalar, v, 12)')
plant(115, DPURE, 'action 107 is not admitted', 'action == kActM6Stat || action == kActM6XStat || action == kActVramStat)); }', 'action == kActM6Stat)); }')
plant(116, PLIST, 'the kext version is not bumped', '<key>CFBundleVersion</key>\n\t<string>0.0.664</string>', '<key>CFBundleVersion</key>\n\t<string>0.0.660</string>')
plant(117, 'variants/configs/stage17-native-1440-metal-disp-amfi-m6flip.plist', 'the m6flip variant lacks the boot-arg', 'navi48-m6=1 navi48-m6flip=1</string>', 'navi48-m6=1</string>')
plant(118, 'variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist', 'the m6 variant gains navi48-m6flip (the OFF variant must stay as it was)', 'navi48-m6=1</string>', 'navi48-m6=1 navi48-m6flip=1</string>')
# ---- the review items --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(120, M6P, 'R2: the blob carries the request count at ENTRY (curGen not set under the gate)', '        __atomic_store_n(&g.curGen, seen, __ATOMIC_SEQ_CST);                    // 0.0.661 (R2): the build reads it (blob version 2 carries it)\n', '')
plant(121, M6P, 'R2: the published generation advances even when the publish FAILED', 'if (n != 0u && publish(n)) { g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }', 'if (n != 0u && publish(n)) { g.published++; done++; } __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST);')
plant(122, M6P, 'R2: blob version 2 drops the generation word', 'w32(0, kBlobVersion2); w32(4, n); w32(8, gen);', 'w32(0, kBlobVersion2); w32(4, n); w32(8, gen & 0u);')
plant(123, DISPC, 'R2: Status2 reports the REQUEST count (gen), not the published generation', '__atomic_load_n(&gD.m6Pub.pubGen, __ATOMIC_ACQUIRE) : 0u; }', '__atomic_load_n(&gD.m6Pub.gen, __ATOMIC_ACQUIRE) : 0u; }')
plant(124, M6P, 'R3: the eviction ignores the owner\'s own distance (lru_slot again)', '        ent_fill(t.e[m6flip ? victim_slot(t) : lru_slot(t)], id, inst, 0u, tx, sq);', '        ent_fill(t.e[m6flip ? lru_slot(t) : lru_slot(t)], id, inst, 0u, tx, sq);')
plant(125, M6P, 'R3: the victim is the SMALLEST distance (the live entry goes first)', 'if (!have || d > bd || (d == bd && sn < bs)) { have = true; bd = d; bs = sn; best = i; }', 'if (!have || d < bd || (d == bd && sn < bs)) { have = true; bd = d; bs = sn; best = i; }')
plant(126, DISPC, 'R3: the glue evicts by lru_slot (the latch is not passed to learn_surface)', 'n48m6::learn_surface(gD.m6Surf, id, inst, n48m6flip_latched_on())', 'n48m6::learn_surface(gD.m6Surf, id, inst)')
plant(127, M6P, 'R4: reset_table writes inst 0 (= the DP) again', '__atomic_store_n(&t.e[i].inst, kInstNone, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].flags, 0u, __ATOMIC_RELAXED);', '__atomic_store_n(&t.e[i].inst, 0u, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].flags, 0u, __ATOMIC_RELAXED);')
plant(128, M6P, 'R4: blob_build_v2 does not take the table\'s busy flag', '    uint32_t tries = 0u;\n    while (!acquire_busy(t)) { if (++tries > (1u << 16)) return 0u; }\n    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE) > kMaxSurf ? kMaxSurf : __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);\n    auto w32 = [&](uint32_t off, uint32_t v) { out[off] = (uint8_t)v; out[off + 1] = (uint8_t)(v >> 8); out[off + 2] = (uint8_t)(v >> 16); out[off + 3] = (uint8_t)(v >> 24); };\n    w32(0, kBlobVersion2);', '    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE) > kMaxSurf ? kMaxSurf : __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);\n    auto w32 = [&](uint32_t off, uint32_t v) { out[off] = (uint8_t)v; out[off + 1] = (uint8_t)(v >> 8); out[off + 2] = (uint8_t)(v >> 16); out[off + 3] = (uint8_t)(v >> 24); };\n    w32(0, kBlobVersion2);')
plant(129, M6P, 'R4: blob_build_v2 forgets to release the busy flag', '    __atomic_store_n(&t.busy, 0u, __ATOMIC_RELEASE);\n    return kBlobHdr2 + n * kBlobEnt;', '    return kBlobHdr2 + n * kBlobEnt;')
plant(130, M6P, 'R4: lookup_surface does not re-read the id (a torn entry is answered)', '            if (__atomic_load_n(&t.e[i].id, __ATOMIC_ACQUIRE) == id) { l.found = true;', '            if (true) { l.found = true;')
plant(131, M6P, 'R5: the publish rate gate never defers', 'nowNs - last < kPubGapNs) { __atomic_store_n(&r.dirty, 1u', 'nowNs - last < 0ull) { __atomic_store_n(&r.dirty, 1u')
plant(132, M6P, 'R5: a deferred publish is never flushed', '    return __atomic_load_n(&r.dirty, __ATOMIC_ACQUIRE) != 0u && (nowNs < last || nowNs - last >= kPubGapNs);', '    return false && (nowNs < last || nowNs - last >= kPubGapNs);')
plant(133, DISPC, 'R5: Status2 / the DP status call does not flush a deferred publish (the flush body is empty)', '    KernelEnv ke; (void)n48m6::pub_rate_flush(gD.m6Rate, now_ns(), [&ke]() { ke.m6_publish_now(); });', '')
plant(134, DFLOW, 'R1: the reset still runs when the window OPENS (with the latch)', 'if (e.m6_on() && !e.m6flip_on()) e.m6_reset();', 'if (e.m6_on()) e.m6_reset();')
plant(135, DFLOW, 'R1: no reset at the new start\'s first slot-267', 'if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', '')
plant(136, DFLOW, 'R1: the reset repeats at EVERY slot-267 of the new start', 'if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', 'if (e.m6_on() && e.m6flip_on()) e.m6_reset();')
# ---- the safety review's conditions (M1 S1 S2 S3 S4 S-2 N1) ---------------------------------------------------------------------------------------------------------------------------------------------
plant(140, M6P, 'M1: a deferred publish is never flushed (flush_due is always false)', '    if (!pub_rate_flush_due(r, nowNs)) return false;\n    uint64_t last = __atomic_load_n(&r.lastNs, __ATOMIC_ACQUIRE);', '    if (true) return false;\n    uint64_t last = __atomic_load_n(&r.lastNs, __ATOMIC_ACQUIRE);')
plant(141, M6P, 'M1: a deferral does not remember itself (dirty never set: the last learn of a burst is lost)', '{ __atomic_store_n(&r.dirty, 1u, __ATOMIC_RELEASE); return false; }', '{ return false; }')
plant(142, M6P, 'M1: the winner does not clear dirty before it builds', '{ (void)__atomic_exchange_n(&r.dirty, 0u, __ATOMIC_ACQ_REL); return true; }', '{ return true; }')
plant(143, M6P, 'M1: the flush is not atomic (two flushers both publish)', '    if (!__atomic_compare_exchange_n(&r.lastNs, &last, nowNs, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;\n    (void)__atomic_exchange_n(&r.dirty, 0u, __ATOMIC_ACQ_REL);\n    publish();', '    (void)last; r.lastNs = nowNs; r.dirty = 0u;\n    publish();')
plant(144, S1C, 'M1: the DP\'s 1 s status call does not flush a deferred table publish', '    n48disp_m6_flush();   // 0.0.661 (review M1): the DP\'s 1 s keep-alive also flushes a deferred table publish (instance 2 may never be acquired); inert unless both latches are ON\n', '')
plant(145, DISPC, 'M1: a submit (m6_learn) does not flush a deferred publish', '        m6_flush_deferred();\n        const uint32_t r = n48m6::learn_surface', '        const uint32_t r = n48m6::learn_surface')
plant(146, DISPC, 'S-2: the per-learn log line is not rate-limited', 'if (!n48m6flip_latched_on() || n48m6::log_admit(gD.m6LogNs, now_ns())) {', '{')
plant(147, M6P, 'S-2: log_admit always admits', 'if (last != 0ull && nowNs >= last && nowNs - last < 1000000000ull) return false;', 'if (false) return false;')
plant(148, FLOW, 'S1: code 4 (EARLIEST is A, B or unknown) drops the latch bit too', '(((heldCode >= 1u && heldCode <= 3u) ? (bad & ~(uint32_t)N48D2_H_LATCH) : bad) | kHoldGpuHeld)', '(((heldCode >= 1u) ? (bad & ~(uint32_t)N48D2_H_LATCH) : bad) | kHoldGpuHeld)')
plant(149, FLOW, 'S1: a GPU-held plane does not refuse fbpublish (no kHoldGpuHeld in the verdict)', '(bad & ~(uint32_t)N48D2_H_LATCH) : bad) | kHoldGpuHeld);', '(bad & ~(uint32_t)N48D2_H_LATCH) : bad));')
plant(150, FLOW, 'S2: Restore2 programs A once only', 'attempt <= kRestoreAttempts && !verified; attempt++) {', 'attempt <= 1u && !verified; attempt++) {')
plant(151, FLOW, 'S2: the second round polls without programming A again', 'if (attempt > 1u) { wrote = write_addr(e, d, x, A, kModeRestore) || wrote;', 'if (attempt > 1u) {')
plant(152, FLOW, 'S3: Acquire2 ignores the surface update lock (FLIP_CONTROL bit 0)', 'if ((fcr & kFlipUpdLockMask) != 0u || (e.rd(d.regOtgUpdLock) & 0x101u) != 0u) {', 'if ((e.rd(d.regOtgUpdLock) & 0x101u) != 0u) {')
plant(153, FLOW, 'S3: Acquire2 ignores OTG2\'s master update lock', 'if ((fcr & kFlipUpdLockMask) != 0u || (e.rd(d.regOtgUpdLock) & 0x101u) != 0u) {', 'if ((fcr & kFlipUpdLockMask) != 0u) {')
plant(154, FLOW, 'S3: the watchdog does not act on a changed VMID / flip type / TMZ', '    else if (exactBad) why =', '    else if (exactBad && false) why =')
plant(155, FLOW, 'S3: the watchdog does not judge TMZ', '!n48scan::restore_exact(wv & 0xFu, (wc & 0x1u) != 0u,', '!n48scan::restore_exact(wv & 0xFu, false,')
plant(156, FLOW, 'S3: the watchdog does not judge the VMID', '!n48scan::restore_exact(wv & 0xFu,', '!n48scan::restore_exact(0u,')
plant(157, FLOW, 'S3: the watchdog does not judge the flip type', '(wf & kFlipTypeMask) >> 1, ws)', '0u, ws)')
plant(158, FLOW, 'S3: the watchdog does not judge the viewport start', '(wf & kFlipTypeMask) >> 1, ws)', '(wf & kFlipTypeMask) >> 1, 0u)')
plant(159, S1C, 'S4: SCANX_ACQUIRE does not return the free visible-VRAM figure', ' if (rc == kIOReturnSuccess) out[3] = gCtx->gmc.vram_alloc.bytes_free(); }', ' }')
plant(160, CLIENT, 'N1: SCANX_ACQUIRE binds the display layer before the latch check', "(the fifth is the instance's geometry)\n\t\tif (n48m6_latched_on() && n48m6flip_latched_on()) (void)n48dcn::bind(owner);", "(the fifth is the instance's geometry)\n\t\t(void)n48dcn::bind(owner);")
plant(161, CLIENT, 'S4: the Acquire shape check expects 3 outputs', 'case N48N_SEL_SCANX_ACQUIRE: {\n\t\tif (!(soc == 4u || soc == 5u) || !shape(2, soc, 0, 0))', 'case N48N_SEL_SCANX_ACQUIRE: {\n\t\tif (!shape(2, 3, 0, 0))')
plant(162, CLIENT, 'N1: SCANX_STATUS binds before the latch check', 'case N48N_SEL_SCANX_STATUS:\n\t\tif (!shape(1, 4, 0, sizeof(n48n_scan_status))) return kIOReturnBadArgument;\n\t\tif (n48m6_latched_on() && n48m6flip_latched_on()) (void)n48dcn::bind(owner);', 'case N48N_SEL_SCANX_STATUS:\n\t\tif (!shape(1, 4, 0, sizeof(n48n_scan_status))) return kIOReturnBadArgument;\n\t\t(void)n48dcn::bind(owner);')
# ---- 0.0.662 (Stage 2): instance 1, the per-instance wiring, the instance-swap plants S1..S11, HW1 ----------------------------------------------------------------------------------------------------------
plant(200, S1C, 'SPEC 3 (inst 1): no monitor A restore in the N48N close / teardown (restore1 is a no-op)', 'void restore1(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(1u, how, r); }', 'void restore1(const char *how, uint64_t r[2]) { (void)how; (void)r; }')
plant(201, S1C, 'SPEC 3 (inst 1): bo_release forgets instance 1\'s pins (a BO with monitor A slots is freed without Restore1)', '    if (b.pinMask1 != 0u && scan_unpin1(h, b, mode)) b.pinLeak = 1u;', '')
plant(202, FLOW, 'the teardown flow skips restore1', '    t.restore1(how, r1);\n', '')
plant(203, FLOW, 'an unverified instance-1 restore does not leak its BOs', '        if (r1[0] == 0ull && t.pin1(h) != 0u) t.mark_leak(h);\n', '')
plant(204, FLOW, 'instance-1 pins are judged with instance 2\'s verdict', 'if (r1[0] == 0ull && t.pin1(h) != 0u)', 'if (r2[0] == 0ull && t.pin1(h) != 0u)')
plant(205, S1C, 'the teardown environment forgets to clear instance-1 pins', ' s->bo[h].pinMask1 = 0; s->bo[h].pinMask2 = 0; }', ' s->bo[h].pinMask2 = 0; }')
plant(206, S1C, 'the teardown environment reports no instance-1 pins (never leaks them)', 'uint32_t pin1(uint32_t h) { return s->bo[h].pinMask1; }', 'uint32_t pin1(uint32_t h) { (void)h; return 0u; }')
plant(207, S1C, 'instance 1 is admitted without the navi48-m6flip1 latch', 'return n48scanx::inst_admitted(inst, n48m6flip1_latched_on());', 'return n48scanx::inst_admitted(inst, true);')
plant(208, FLOW, 'inst_admitted admits instance 1 with the latch OFF', '|| (inst == 1ull && flip1Latched); }', '|| inst == 1ull; }')
plant(209, GLUE, 'sx_latched_inst(1) ignores navi48-m6flip1', 'inst == 1u ? (sx_latched() && n48m6flip1_latched_on()) : false; }', 'inst == 1u ? sx_latched() : false; }')
plant(210, DISPC, 'the flip1 latch reads the wrong boot-arg', 'PE_parse_boot_argn("navi48-m6flip1"', 'PE_parse_boot_argn("navi48-m6flip"')
plant(211, 'variants/configs/stage17-native-1440-metal-disp-amfi-m6flip3.plist', 'the m6flip3 variant lacks the boot-arg', 'navi48-m6flip=1 navi48-m6flip1=1</string>', 'navi48-m6flip=1</string>')
plant(212, 'variants/configs/stage17-native-1440-metal-disp-amfi-m6flip.plist', 'the 1b variant gains navi48-m6flip1', 'navi48-m6=1 navi48-m6flip=1</string>', 'navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1</string>')
plant(213, ABI, 'the ABI minor stays 11', '#define N48N_ABI_MINOR     12u ', '#define N48N_ABI_MINOR     11u ')
plant(214, ABI, 'the m6flip1 flag bit is the m6flip bit', '#define N48N_SCANQ_M6FLIP1    (1u << 7)', '#define N48N_SCANQ_M6FLIP1    (1u << 6)')
plant(215, GLUE, 'scan_query does not report the m6flip1 latch', 'flags |= N48N_SCANQ_M6FLIP1;', 'flags |= 0u;')
plant(216, FLOW, 'the monitor A descriptor has the monitor B\'s geometry', '    1u, 1u, 0u, 0x10u, 1920u, 1080u, 1920u, 0x77Fu, 8294400ull, 8323072ull,', '    1u, 1u, 0u, 0x10u, 2560u, 1440u, 2560u, 0x9FFu, 14745600ull, 14745600ull,')
plant(217, FLOW, 'S1: the kDesc entries 1 <-> 2 are swapped in desc_of', 'inst == 1u ? &kDescMonA : inst == 2u ? &kDescMonB : nullptr; }', 'inst == 1u ? &kDescMonB : inst == 2u ? &kDescMonA : nullptr; }')
plant(218, FLOW, 'S3: the slot tags 0x10 <-> 0x20 are swapped', '    1u, 1u, 0u, 0x10u, 1920u,', '    1u, 1u, 0u, 0x20u, 1920u,')
plant(219, GLUE, 'S2: SXEnv reads the other instance\'s plane state (d2 index reversed)', 'uint32_t d2_stage() { return __atomic_load_n(&gD2Pl[d.d2idx].stage, __ATOMIC_ACQUIRE); }', 'uint32_t d2_stage() { return __atomic_load_n(&gD2Pl[1u - d.d2idx].stage, __ATOMIC_ACQUIRE); }')
plant(220, FLOW, 'S2: the descriptors\' d2 indices are swapped', '    1u, 1u, 0u, 0x10u,', '    1u, 1u, 1u, 0x10u,')
plant(221, S1C, 'S4: pinGen1 is compared against generation 2', 'const uint32_t gen = n48dcn::scanXGeneration((uint32_t)inst);', 'const uint32_t gen = n48dcn::scanXGeneration(2u);')
plant(222, S1C, 'S5: bo_gone(1) reads HUBP2 (scan_unpinx passes instance 2)', 'n48dcn::scanXBoGone(inst, mask, pgen,', 'n48dcn::scanXBoGone(2u, mask, pgen,')
plant(223, FLOW, 'S5: bo_gone judges with the other descriptor\'s registers', 'const bool ok = !x.restoring && read_plane(e, d, &prog, &early, &pend);\n        const UnpinPlan', 'const bool ok = !x.restoring && read_plane(e, kDescMonB, &prog, &early, &pend);\n        const UnpinPlan')
plant(224, M6P, 'S7: the AGDC list is built [DP, monitor A, monitor B]', 'for (uint32_t inst = kInstMonB; inst >= kInstMonA; --inst) if (have[inst]) instOut[k++] = inst;', 'for (uint32_t inst = kInstMonA; inst <= kInstMonB; ++inst) if (have[inst]) instOut[k++] = inst;')
plant(225, M6P, 'S7: the endpoint dword maps to the reverse list position', 'constexpr uint32_t agdc_position(uint32_t ep, uint32_t nfb) { return (nfb > 1u && ep < nfb) ? ep : 0u; }', 'constexpr uint32_t agdc_position(uint32_t ep, uint32_t nfb) { return (nfb > 1u && ep < nfb) ? nfb - ep : 0u; }')
plant(226, GLUE, 'S10: gVblX is indexed by the other instance', 'VblCache &c = gVblX[inst - 1u];', 'VblCache &c = gVblX[2u - inst];')
plant(227, GLUE, 'S11: the hold verdict is applied to the other instance', 'n48d2_hold_verdict(g, pl->cur, pl->mc[0]), scanXGpuHeld(inst));', 'n48d2_hold_verdict(g, pl->cur, pl->mc[0]), scanXGpuHeld(inst == 1u ? 2u : 1u));')
plant(228, GLUE, 'Acquire1 returns no geometry word', 'out[4] = (uint64_t)d->w | ((uint64_t)d->h << 16) | ((uint64_t)d->pitchPx << 32);', 'out[4] = 0ull;')
plant(230, GLUE, 'the clash set forgets the monitor A pair/slots', 'for (uint32_t inst = 1u; inst <= 2u; inst++) {\n\t\tconst n48scanx::XDesc *d = n48scanx::desc_of(inst);\n\t\tif (d == nullptr || !sx_latched_inst(inst)) continue;', 'for (uint32_t inst = 2u; inst <= 2u; inst++) {\n\t\tconst n48scanx::XDesc *d = n48scanx::desc_of(inst);\n\t\tif (d == nullptr || !sx_latched_inst(inst)) continue;')
plant(231, GLUE, 'the snapshot omits the other display\'s slots', 'IOLock *ol = gSXLock[other];', 'IOLock *ol = nullptr;')
plant(232, GLUE, 'kext stop restores only the monitor B', 'for (uint32_t inst = 1u; inst <= 2u; inst++) {\n\t\tif (gSXLock[inst] == nullptr) continue;', 'for (uint32_t inst = 2u; inst <= 2u; inst++) {\n\t\tif (gSXLock[inst] == nullptr) continue;')
plant(233, FLOW, 'the writer ignores the state\'s descriptor', 'bool ok = x.descInst == d.inst;', 'bool ok = true;')
plant(234, FLOW, 'Acquire does not record the descriptor in the state', '        x.descInst = d.inst;\n', '')
plant(235, M6P, 'item 10: the AGDC per-instance counter counts the wrong instance', 'if (cmd == N48_AGDC_CMD_LINK_CONFIG) g.n921i[inst]++;', 'if (cmd == N48_AGDC_CMD_LINK_CONFIG) g.n921i[kInstDp]++;')
plant(236, M6P, 'item 10: the endpoint bitmask is not kept', 'g.epSeen |= agdc_ep_bit(ep);', '')
plant(237, DPURE, 'item 11: action 107 takes no instance', '(action == kActM6XStat && m6xstat_arg_ok(arg))', '(action == kActM6XStat && arg <= 2ull)')
plant(238, GLUE, 'item 11: scanXReport ignores the instance byte', 'uint32_t inst = (uint32_t)((arg >> 8) & 0xFFull);', 'uint32_t inst = 0u;')
plant(239, CLIENT, 'Acquire accepts only four output words', '(soc == 4u || soc == 5u) || !shape(2, soc, 0, 0)', '(soc == 4u) || !shape(2, soc, 0, 0)')
plant(240, CLIENT, 'Acquire copies five words into a four-word buffer', 'for (uint32_t i = 0; i < soc; i++) so[i] = o5[i];', 'for (uint32_t i = 0; i < 5u; i++) so[i] = o5[i];')
plant(241, S1C, 'Register does not check the returned slot id belongs to the instance', 'if (xd == nullptr || !n48scanx::id_ok(*xd, o2[0])) {', 'if (xd == nullptr) {')
plant(242, S1C, 'Release clears the other instance\'s pins', 'uint8_t &mask = inst == 1ull ? s->bo[h].pinMask1 : s->bo[h].pinMask2;', 'uint8_t &mask = s->bo[h].pinMask2;')
plant(243, DISPC, 'HW1: the publish buffer is back to the v1 size (blob_build_v2 refuses, the property is never written)', 'uint8_t gM6Blob[n48m6::kBlobBufBytes];', 'uint8_t gM6Blob[n48m6::kBlobMax];')
plant(244, M6P, 'HW1: kBlobBufBytes is the v1 size', 'constexpr uint32_t kBlobBufBytes = kBlobMax2 > kBlobMax ? kBlobMax2 : kBlobMax;', 'constexpr uint32_t kBlobBufBytes = kBlobMax;')
shutil.rmtree(scr, ignore_errors=True)
print('native_scanx_plant: %d plant(s) (+ the control), %d escaped' % (total - (1 if FIRST == 0 else 0), escaped))
sys.exit(1 if escaped else 0)
PY
