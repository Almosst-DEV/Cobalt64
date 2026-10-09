#!/bin/zsh
# native_agdc_plant.sh - planted breaks for tests/native_agdc_test.cpp (build 0.0.614, #11 step 11h.3: the native AGDC service, accel action 88).
# Discipline: copy the sources the test reads into a scratch tree, apply ONE break to the REAL code (a header, a flow, the kernel glue, a call site), build the real test against the copy and demand
# it FAILS (a compile error, a failing check, a crash or a hang all count). A plant whose text is not found exactly once is itself reported as an escape. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_agdc_plant.sh [first-plant-id last-plant-id]
# The table and the runner are Python (the breaks are multi-line and quote-heavy); this wrapper only hands it the tree root.
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec python3 -u - "$ROOT" "${1:-0}" "${2:-9999}" <<'PY'
import os, shutil, subprocess, sys, tempfile, time

ROOT, FIRST, LAST = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
K = 'src/navi48-bringup'
FILES = [f'{K}/src/dcn/navi48_dispread.h', f'{K}/src/dcn/navi48_dmubcmd.h', f'{K}/src/amd/native_agdc_pure.h', f'{K}/src/amd/native_agdc_flow.h', f'{K}/src/amd/native_disp_pure.h', f'{K}/src/amd/native_disp.cpp', f'{K}/src/amd/native_disp.h',
         f'{K}/src/amd/native_metal_pure.h', f'{K}/src/Navi48MetalOps.h', f'{K}/src/apple/DisplayPipeGuard.cpp', f'{K}/src/apple/Navi48Ttl.hpp', f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/dcn/navi48_dcn.hpp',
         f'{K}/src/Navi48Bringup.cpp', f'{K}/src/Navi48UserClient.cpp', f'{K}/tests/native_agdc_test.cpp', f'{K}/tests/native_agdc_plant.sh', 'tools/pc/navi48test.c',
         # native_disp_pure.h includes the disp2 header (0.0.631) and, since 0.0.652, native_fb_pure.h -> Navi48DisplayOps.h: without them the control FAILED UNMODIFIED at 0.0.651
         f'{K}/src/dcn/navi48_disp2.h', f'{K}/src/amd/native_fb_pure.h', f'{K}/src/Navi48DisplayOps.h',
         # 0.0.659 (M6 Stage 1a): native_disp_pure.h includes native_m6_pure.h (-> display_pipe_guard.h) and DisplayPipeGuard.cpp includes Navi48DisplayNub.hpp
         f'{K}/src/amd/native_m6_pure.h', f'{K}/src/apple/display_pipe_guard.h', f'{K}/src/Navi48DisplayNub.hpp']
PURE, FLOW, DISPP, DGLUE, DPG, TTL, DCN, DCNH, BRG, UCL, CLI = (f'{K}/src/amd/native_agdc_pure.h', f'{K}/src/amd/native_agdc_flow.h', f'{K}/src/amd/native_disp_pure.h', f'{K}/src/amd/native_disp.cpp',
        f'{K}/src/apple/DisplayPipeGuard.cpp', f'{K}/src/apple/Navi48Ttl.hpp', f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/dcn/navi48_dcn.hpp', f'{K}/src/Navi48Bringup.cpp', f'{K}/src/Navi48UserClient.cpp', 'tools/pc/navi48test.c')
AUX = os.environ.get('N48_AUX_ROOT', ROOT)

scr = tempfile.mkdtemp(prefix='nagdc-plant.')
def fresh():
    shutil.rmtree(scr, ignore_errors=True)
    for f in FILES:
        d = os.path.join(scr, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(ROOT, f), d)
def build_run():
    r = subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-O1', '-fsanitize=address,undefined', '-I', f'{K}/src', '-I', f'{K}/src/amd', f'{K}/tests/native_agdc_test.cpp', '-o', os.path.join(scr, 't')],
                       cwd=scr, capture_output=True, text=True)
    if r.returncode != 0:
        first = next((l for l in r.stderr.splitlines() if 'error' in l), '')
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

def plantm(pid, path, desc, pairs):
    global total, escaped
    if pid < FIRST or pid > LAST: return
    fresh(); total += 1
    f = os.path.join(scr, path); s = open(f).read()
    for old, new in pairs:
        n = s.count(old)
        if n != 1:
            print('PLANT %d: %s: PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %s' % (pid, desc, n, old[:60])); escaped += 1; return
        s = s.replace(old, new)
    open(f, 'w').write(s)
    t0 = time.time(); rc, msg = build_run(); dt = time.time() - t0
    if rc == 0: print('PLANT %d: %s: *** ESCAPED (%s) *** [%.1fs]' % (pid, desc, msg, dt)); escaped += 1
    else: print('PLANT %d: %s: %s [%.1fs]' % (pid, desc, msg, dt))

control()
# ---- the pure decisions (amd/native_agdc_pure.h, amd/native_disp_pure.h) --------------------------------------------------------------------------------------------
plant(1, PURE, 'the AGDC anchor address is wrong', 'constexpr uint64_t kAgdcMeta = 0x13d418e8ull;', 'constexpr uint64_t kAgdcMeta = 0x13d418e0ull;')
plant(2, PURE, 'the family anchor address is wrong', 'constexpr uint64_t kPipeMeta = 0x146036d8ull;', 'constexpr uint64_t kPipeMeta = 0x146036d0ull;')
plant(3, PURE, 'the slides need not agree', '((agdcMeta - kAgdcMeta) != (pipeMeta - kPipeMeta) || ((agdcMeta - kAgdcMeta) & 0xfffull) != 0ull)', '(((agdcMeta - kAgdcMeta) & 0xfffull) != 0ull)')
plant(4, PURE, 'the slide need not be page aligned', '((agdcMeta - kAgdcMeta) != (pipeMeta - kPipeMeta) || ((agdcMeta - kAgdcMeta) & 0xfffull) != 0ull)', '((agdcMeta - kAgdcMeta) != (pipeMeta - kPipeMeta))')
plant(5, PURE, 'the family metaclass need not be a kernel pointer', 'return (!kptr_ok(agdcMeta) || !kptr_ok(pipeMeta)) ?', 'return (!kptr_ok(agdcMeta)) ?')
plant(6, PURE, 'the AGDC metaclass need not be a kernel pointer', 'return (!kptr_ok(agdcMeta) || !kptr_ok(pipeMeta)) ?', 'return (!kptr_ok(pipeMeta)) ?')
plant(7, PURE, 'the kernel-half bound is moved', 'constexpr uint64_t kKernelHalf = 0xffffff7000000000ull;', 'constexpr uint64_t kKernelHalf = 0xffffff0000000000ull;')
plant(8, PURE, 'AMDRDNA4FB is not an allowed framebuffer name', '(str_eq(name, "RDNA4FB") || str_eq(name, "AMDRDNA4FB"))', '(str_eq(name, "RDNA4FB"))')
plant(9, PURE, 'IOFramebuffer is an allowed framebuffer name', '(str_eq(name, "RDNA4FB") || str_eq(name, "AMDRDNA4FB"))', '(str_eq(name, "RDNA4FB") || str_eq(name, "AMDRDNA4FB") || str_eq(name, "IOFramebuffer"))')
plant(10, PURE, 'the framebuffer name is compared as a prefix', 'while (*a && *a == *b) { ++a; ++b; }\n    return *a == *b;', 'while (*a && *a == *b) { ++a; ++b; }\n    return *b == 0;')
plant(11, PURE, 'argument 2 is legal', 'constexpr bool arg_ok(uint64_t arg) { return arg <= 1ull; }', 'constexpr bool arg_ok(uint64_t arg) { return arg <= 2ull; }')
plant(12, PURE, 'the held status number moves', 'kAlready = 12, kBadArg = 13, kHeld = 14, kNoFamily = 15, kStatusCount = 16', 'kAlready = 12, kBadArg = 13, kHeld = 16, kNoFamily = 15, kStatusCount = 17')
plant(13, PURE, 'the verb number moves', 'constexpr uint32_t kActAgdc = 88u;    // accel action 88:', 'constexpr uint32_t kActAgdc = 89u;    // accel action 88:')
plant(14, PURE, 'the unused status loses its name', 's == kUnused2 ? "(unused here', 's == 99u ? "(unused here')
plant(15, DISPP, 'the last admitted action is 87 again', 'constexpr uint32_t kLastAction = 99u;', 'constexpr uint32_t kLastAction = 87u;')
plant(16, DISPP, 'pipeagdc has no legal arguments', '(action == kActAgdc && arg <= 1ull) ||', 'false ||')
plant(17, DISPP, 'pipeagdc accepts argument 2', '(action == kActAgdc && arg <= 1ull) ||', '(action == kActAgdc && arg <= 2ull) ||')
plant(18, DISPP, 'n48disp_verb is taught to answer 88', 'constexpr bool is_pipe_verb(uint32_t action) { return (action >= kActAdopt && action <= kActShortcut) || action == kActVbl || action == kActReload || action == kActM6Stat; }', 'constexpr bool is_pipe_verb(uint32_t action) { return action >= kActAdopt && action <= kActAgdc; }')
plant(19, PURE, 'a forbidden pipe-offset spelling is introduced in the pure header', 'constexpr bool arg_ok(uint64_t arg) { return arg <= 1ull; }', 'constexpr bool arg_ok(uint64_t arg) { return arg <= 1ull; }   // see pipe+' + '0x299')
# ---- the flow (amd/native_agdc_flow.h) --------------------------------------------------------------------------------------------------------------------------------
plant(20, FLOW, 'the latch is not consulted', '    if (!e.latch_on()) return kOff;\n', '')
plant(21, FLOW, 'an already published object is not refused', '    if (e.published()) return kAlready;\n', '')
plant(22, FLOW, 'a row-120 hold does not refuse', '    if (e.mode_held()) return kHeld;\n', '')
plantm(23, FLOW, 'the hold is checked only after the class checks', [('    if (e.mode_held()) return kHeld;\n    const uint64_t am = e.agdc_meta();', '    const uint64_t am = e.agdc_meta();'), ('    if (!e.have_targets()) return kNoFbPci;', '    if (e.mode_held()) return kHeld;\n    if (!e.have_targets()) return kNoFbPci;')])
plant(24, FLOW, 'disagreeing anchors go on to the class checks', '    if (s.status != kPublished) return s.status;\n', '')
plant(25, FLOW, 'a failed class check is ignored', '    if (cc != kPublished) return cc;\n', '')
plant(26, FLOW, 'a missing framebuffer is ignored', '    if (!e.have_targets()) return kNoFbPci;\n', '    (void)e.have_targets();\n')
plant(27, FLOW, 'a missing wrangler is ignored', '    if (!e.wrangler()) return kNoWrangler;\n', '    (void)e.wrangler();\n')
plant(28, FLOW, 'the build gets a zero slide', 'return e.build(am, s.slide);', 'return e.build(am, 0ull);')
plant(29, FLOW, 'the state read goes on to publish', '    if (arg == 0ull) return kPublished;                          // the state read: never publishes, never reads the class\n', '')
plant(30, FLOW, 'the argument is not checked', '    if (!arg_ok(arg)) return kBadArg;\n', '')
plantm(31, FLOW, 'the family lookup comes before the AGDC lookup', [('    const uint64_t am = e.agdc_meta();\n    if (am == 0ull) return kNoAgdc;\n    const uint64_t pm = e.pipe_meta();\n    if (pm == 0ull) return kNoFamily;\n', '    const uint64_t pm = e.pipe_meta();\n    if (pm == 0ull) return kNoFamily;\n    const uint64_t am = e.agdc_meta();\n    if (am == 0ull) return kNoAgdc;\n')])
plant(32, FLOW, 'a missing AGDC class is not its own refusal', '    if (am == 0ull) return kNoAgdc;\n', '')
plant(33, FLOW, 'a missing family class is not its own refusal', '    if (pm == 0ull) return kNoFamily;\n', '')
plant(34, FLOW, 'the class checks run on the family metaclass', 'const uint32_t cc = e.class_checks(am, s.slide);', 'const uint32_t cc = e.class_checks(pm, s.slide);')
plant(35, FLOW, 'the build gets the family metaclass', 'return e.build(am, s.slide);', 'return e.build(pm, s.slide);')
plantm(36, FLOW, 'the published check comes before the latch', [('    if (!e.latch_on()) return kOff;\n    if (e.published()) return kAlready;\n', '    if (e.published()) return kAlready;\n    if (!e.latch_on()) return kOff;\n')])
plantm(37, FLOW, 'the wrangler is checked before the framebuffer', [('    if (!e.have_targets()) return kNoFbPci;\n    if (!e.wrangler()) return kNoWrangler;\n', '    if (!e.wrangler()) return kNoWrangler;\n    if (!e.have_targets()) return kNoFbPci;\n')])
# ---- the kernel glue (DisplayPipeGuard.cpp) ---------------------------------------------------------------------------------------------------------------------------
plant(40, DPG, 'the env never sees the hold', 'bool mode_held() { return n48dcn::modeHoldActive(); }', 'bool mode_held() { return false; }')
plant(41, DPG, 'the env never sees a published object', 'bool published() { return gAg.published != 0u; }', 'bool published() { return false; }')
plant(42, DPG, 'the env ignores the latch', 'bool latch_on() { return n48disp_latched_on(); }', 'bool latch_on() { return true; }')
plant(43, DPG, 'the family anchor is the wrong class', 'meta("IOAccelDisplayPipe")', 'meta("IOAccelPipe")')
plant(44, DPG, 'the AGDC anchor is the wrong class', 'meta("AppleGraphicsDeviceControl"))); }', 'meta("AppleGraphicsControl"))); }')
plant(45, DPG, 'the native class checks depend on X6000', '        const uint32_t cc = agdc_class_checks(reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)), static_cast<uintptr_t>(slide));', '        (void)navi48_x6000_slide(nullptr);\n        const uint32_t cc = agdc_class_checks(reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)), static_cast<uintptr_t>(slide));')
plant(46, DPG, 'finish gives the framebuffer back even after a publish', 'if (fb && st != n48agdc::kPublished) fb->release();', 'if (fb) fb->release();')
plantm(47, DPG, 'the flow runs before the lock is taken', [('    IOLockLock(gDpgLock);\n    const uint32_t st = n48agdc::native_flow(env, arg);\n    if (arg == 1ull) gAg.lastStatus = st;\n    IOLockUnlock(gDpgLock);', '    const uint32_t st = n48agdc::native_flow(env, arg);\n    IOLockLock(gDpgLock);\n    if (arg == 1ull) gAg.lastStatus = st;\n    IOLockUnlock(gDpgLock);')])
plant(48, DPG, 'the vtable slot 7 check reads slot 6', 'reinterpret_cast<uintptr_t>(vt[7]) - slide != 0x13d3be5e', 'reinterpret_cast<uintptr_t>(vt[6]) - slide != 0x13d3be5e')
plant(49, DPG, 'the class size check is loosened', 'if (mc->getClassSize() != kAgdcClassSize) { DPGLOG("agdc: REFUSED - class size', 'if (mc->getClassSize() > kAgdcClassSize) { DPGLOG("agdc: REFUSED - class size')
plant(50, DPG, 'the shared build forgets slot 266', '    copy[kVtHdr + 266] = reinterpret_cast<void *>(&agdc_vendor);\n', '')
plant(51, DPG, 'the translation route drops its pipe-guard requirement', '    if (!navi48_pipeguard_armed_all()) return 2;\n', '')
plant(52, DPG, 'the translation route drops its wrangler check', '    if (!agdc_have_wrangler()) return 9;\n    return agdc_build_locked(mc, slide, fb, pci, provider);', '    return agdc_build_locked(mc, slide, fb, pci, provider);')
plant(53, DPG, 'the env looks the framebuffer up under one name only', 'static const char *const kNames[2] = { "RDNA4FB", "AMDRDNA4FB" };', 'static const char *const kNames[2] = { "RDNA4FB", "RDNA4FB" };')
plant(54, DPG, 'the env skips the framebuffer name check', 'bool ok = fb && mc && n48agdc::fb_name_ok(mc->getClassName()) && pci && provider;', 'bool ok = fb && mc && pci && provider;')
plant(55, DPG, 'the glue anchor differs from the pure header\'s', 'kAgdcGMetaClass   = 0x13d418e8;', 'kAgdcGMetaClass   = 0x13d418e0;')
plant(56, DPG, 'the native control does not return the shared scalars', 'DPGLOG("agdc: native publish -> status %u (%s)", st, n48agdc::status_name(st));\n    agdc_fill_out(st, out, count);', 'DPGLOG("agdc: native publish -> status %u (%s)", st, n48agdc::status_name(st));\n    (void)out; (void)count;')
plant(57, DPG, 'finish is never called', '    env.finish(st);\n', '')
plant(58, DPG, 'the native env takes a lock of its own', '    bool wrangler() { return agdc_have_wrangler(); }', '    bool wrangler() { IOLockLock(gDpgLock); return agdc_have_wrangler(); }')
plant(59, DPG, 'the native env reads the legacy boot-arg', '    bool published() { return gAg.published != 0u; }', '    bool published() { uint32_t ba = 0; (void)PE_parse_boot_argn("navi48-agdc", &ba, sizeof(ba)); return gAg.published != 0u; }')
# ---- dcn / declarations ---------------------------------------------------------------------------------------------------------------------------------------------------
plant(60, DCN, 'the hold accessor needs BOTH words', 'n48mt::ld(gMt.launching) != 0u || n48mt::ld(gMt.held) != 0u;', 'n48mt::ld(gMt.launching) != 0u && n48mt::ld(gMt.held) != 0u;')
plant(61, DCN, 'the hold accessor ignores the held word', 'n48mt::ld(gMt.launching) != 0u || n48mt::ld(gMt.held) != 0u;', 'n48mt::ld(gMt.launching) != 0u;')
plant(62, DCNH, 'the accessor is not declared', 'bool modeHoldActive();', '')
plant(63, TTL, 'the native control is not declared', 'uint32_t navi48_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count);', '')
# ---- the call sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(70, BRG, 'the action-88 branch is unreachable', '	if (action == 88) {', '	if (action == 880) {')
plant(71, BRG, 'the action-88 branch skips the argument table', 'n48disp::verb_args_ok(action, argScalar) ? navi48_agdc_native_control(argScalar, v, 13) : (uint32_t)n48agdc::kBadArg;', 'navi48_agdc_native_control(argScalar, v, 13);')
plant(72, BRG, 'a bad argument is reported as success', 'return st == n48agdc::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;', 'return kIOReturnSuccess;')
plant(73, BRG, 'action 88 is routed to the pipe-verb handler', 'if (action == 83 || action == 84 || action == 85 || action == 86 || action == 87 || action == 89 || action == 90 || action == n48disp::kActM6Stat) {', 'if (action == 83 || action == 84 || action == 85 || action == 86 || action == 87 || action == 88 || action == 89 || action == 90 || action == n48disp::kActM6Stat) {')
plant(74, BRG, 'the native control is not called from the branch', 'navi48_agdc_native_control(argScalar, v, 13) : (uint32_t)n48agdc::kBadArg;', 'navi48_agdc_control(argScalar, v, 13) : (uint32_t)n48agdc::kBadArg;')
plant(75, DGLUE, 'n48disp_verb answers any new action', '!n48disp::is_pipe_verb(action)', '!n48disp::is_new_action(action)')
plant(76, UCL, 'the user client bound is fixed at 88', 'if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;', 'if (action > 88) return kIOReturnBadArgument;')
plant(80, CLI, 'the CLI has no pipeagdc', 'else if (what && !strcmp(what, "pipeagdc")) in = 88;', '')
plant(81, CLI, 'the CLI sends the wrong action for pipeagdc', 'else if (what && !strcmp(what, "pipeagdc")) in = 88;', 'else if (what && !strcmp(what, "pipeagdc")) in = 87;')
plant(82, CLI, 'the CLI does not decode pipeagdc', '    if (in == 88) {', '    if (in == 880) {')
plant(83, CLI, 'the CLI loses the hold status text', '"row-120 mode hold is up (the link timing would not be the framebuffer\'s)"', '"held"')

shutil.rmtree(scr, ignore_errors=True)
print('native_agdc_plant: %d plant(s), %d escaped' % (total, escaped))
sys.exit(1 if escaped else 0)
PY
