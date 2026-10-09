#!/bin/zsh
# native_m6_plant.sh - planted breaks for tests/native_disp_test.cpp section N (build 0.0.659, M6 Stage 1a: the routing guard, the per-pipe adopt, the interlock lifts, the per-endpoint AGDC answers, the OFF-identity battery).
# Discipline: copy the sources the test reads into a scratch tree, apply ONE break to the REAL code (a header, a flow, the kernel glue, a call site), build the real test against the copy and demand
# it FAILS (a compile error, a failing check, a crash or a hang all count). A plant whose text is not found exactly once is itself reported as an escape. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_m6_plant.sh [first-plant-id last-plant-id]
# The table and the runner are Python (the breaks are multi-line and quote-heavy); this wrapper only hands it the tree root.
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
exec python3 -u - "$ROOT" "${1:-0}" "${2:-9999}" <<'PY'
import os, shutil, subprocess, sys, tempfile, time

ROOT, FIRST, LAST = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
K = 'src/navi48-bringup'
FILES = [f'{K}/src/dcn/navi48_dispread.h', f'{K}/src/dcn/navi48_dmubcmd.h', f'{K}/src/amd/native_disp_pure.h', f'{K}/src/amd/native_disp_flow.h', f'{K}/src/amd/native_metal_pure.h', f'{K}/src/amd/native_disp.cpp', f'{K}/src/amd/native_disp.h',
         f'{K}/src/amd/native_s1c.h', f'{K}/src/Navi48MetalNub.cpp', f'{K}/src/Navi48NativeClient.cpp', f'{K}/src/dcn/navi48_dcn.cpp', f'{K}/src/dcn/navi48_dcn.hpp', f'{K}/src/Navi48Bringup.cpp', f'{K}/src/Navi48UserClient.cpp', f'{K}/src/Navi48Bringup.hpp', f'{K}/src/Navi48MetalOps.h',
         f'{K}/Info.plist', f'{K}/tests/native_disp_test.cpp', f'{K}/tests/native_fb_monb_battery.h', f'{K}/tests/native_disp_plant.sh', 'tools/pc/navi48test.c',
         # 0.0.652 (M5): native_disp_pure.h includes the disp2 header and native_fb_pure.h (-> Navi48DisplayOps.h) (the control FAILED unmodified at 0.0.651 without the disp2 header); the M section of the test reads the nub, the Makefile, the allocator and the variants
         f'{K}/src/dcn/navi48_disp2.h', f'{K}/src/amd/native_fb_pure.h', f'{K}/src/Navi48DisplayOps.h', f'{K}/src/Navi48DisplayNub.cpp', f'{K}/src/Navi48DisplayNub.hpp', f'{K}/Makefile', f'{K}/src/amd/native_s1c.cpp',
         'variants/configs/stage17-native-1440-metal-disp-amfi-fb2.plist', 'variants/configs/stage17-native-1440-metal-disp-amfi-disp2.plist',
         # 0.0.659 (M6 Stage 1a): the pure header, the OFF-identity battery, the AGDC glue and its pure header, the ABI header and the two variants section N reads
         f'{K}/src/amd/native_m6_pure.h', f'{K}/tests/native_m6_off_battery.h', f'{K}/src/apple/DisplayPipeGuard.cpp', f'{K}/src/apple/display_pipe_guard.h', f'{K}/src/Navi48NativeABI.h',
         'variants/configs/stage17-native-1440-metal-disp-amfi-ms-apps.plist', 'variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist']
FBP, DNUB, DOPS, MPURE, MK, VAR = (f'{K}/src/amd/native_fb_pure.h', f'{K}/src/Navi48DisplayNub.cpp', f'{K}/src/Navi48DisplayOps.h', f'{K}/src/amd/native_metal_pure.h', f'{K}/Makefile', 'variants/configs/stage17-native-1440-metal-disp-amfi-fb2.plist')
AUXOPS = 'tools/native/navi48accel/src/Navi48DisplayOps.h'
PURE, FLOW, GLUE, NUB, BRG, UCL, CLI, PLIST = (f'{K}/src/amd/native_disp_pure.h', f'{K}/src/amd/native_disp_flow.h', f'{K}/src/amd/native_disp.cpp', f'{K}/src/Navi48MetalNub.cpp',
                                              f'{K}/src/Navi48Bringup.cpp', f'{K}/src/Navi48UserClient.cpp', 'tools/pc/navi48test.c', f'{K}/Info.plist')
AUX = os.environ.get('N48_AUX_ROOT', ROOT)

scr = tempfile.mkdtemp(prefix='nm6-plant.')
def fresh():
    shutil.rmtree(scr, ignore_errors=True)
    for f in FILES:
        d = os.path.join(scr, f); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(ROOT, f), d)
    for an in ('Navi48MetalOps.h', 'Navi48DisplayOps.h'):
        a = os.path.join(AUX, 'tools/native/navi48accel/src/' + an)
        if os.path.exists(a):
            d = os.path.join(scr, 'tools/native/navi48accel/src/' + an); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(a, d)
def build_run():
    r = subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', f'{K}/src', '-I', f'{K}/src/amd', f'{K}/tests/native_disp_test.cpp', '-o', os.path.join(scr, 't')],
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
    if os.environ.get('N48_PLANT_DRYRUN'): print('PLANT %d: anchor found once (dry run, not built)' % pid); return      # 0.0.660: check the anchors without paying for the builds
    t0 = time.time(); rc, msg = build_run(); dt = time.time() - t0
    if rc == 0: print('PLANT %d: %s: *** ESCAPED (%s) *** [%.1fs]' % (pid, desc, msg, dt)); escaped += 1
    else: print('PLANT %d: %s: %s [%.1fs]' % (pid, desc, msg, dt))

if not os.environ.get('N48_PLANT_DRYRUN'): control()
M6P = f'{K}/src/amd/native_m6_pure.h'
DPG = f'{K}/src/apple/DisplayPipeGuard.cpp'
DCN = f'{K}/src/dcn/navi48_dcn.cpp'
BAT = f'{K}/tests/native_m6_off_battery.h'
VARM6 = 'variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist'
S1C = f'{K}/src/amd/native_s1c.h'
# ---- the briefed breaks -----------------------------------------------------------------------------------------------------------------------------------------------------------
plant(501, M6P, 'BRIEF 1: a monitor B surface is MAPPED to instance 0 (the learn stores 0 for every pipe): the DP pipe then presents it', '    __atomic_store_n(&e.inst, inst, __ATOMIC_RELAXED);', '    __atomic_store_n(&e.inst, 0u, __ATOMIC_RELAXED);')
plant(502, M6P, 'BRIEF 1: the guard no longer compares the surface\'s instance with the pipe\'s', '    if (l.inst != pipeInst) return kRouteWrongInst;\n', '')
plant(503, M6P, 'BRIEF 2: pipe-0-only adopt - only slot 0 is judged for ownership', '        if (!p.classOurs || !p.traced) return kEntsNotOurs;', '        if (i == 0 && (!p.classOurs || !p.traced)) return kEntsNotOurs;')
plant(504, M6P, 'BRIEF 2: pipe-0-only adopt - back-links and framebuffers of slots > 0 are not judged', '        if (!p.backAccel || !p.backDm || !p.fbOk) return kEntsMismatch;', '        if (i == 0 && (!p.backAccel || !p.backDm || !p.fbOk)) return kEntsMismatch;')
plant(505, GLUE, 'BRIEF 2: pipe-0-only adopt (kernel): the probe reads only the first pipe slot', 'for (uint32_t i = 0; i < p->count && i < n48m6::kMaxEnt; ++i) {', 'for (uint32_t i = 0; i < 1 && i < p->count; ++i) {')
plant(506, M6P, 'BRIEF 3: 0x921 ignores the endpoint (the list position is always the DP)', 'return (nfb > 1u && ep < nfb) ? ep : 0u; }', 'return 0u; }')
plant(507, M6P, 'BRIEF 3: 0x921 / 0x711 never read the endpoint dword', '    const uint32_t ep = n48_agdc_rd32(out, 0u);\n    g.lastEp = ep;', '    const uint32_t ep = 0u;\n    g.lastEp = ep;')
plant(508, M6P, 'BRIEF 3: 0x921 answers the monitor B\'s timing for every non-DP endpoint', '*kr = (agdc_timing_for_inst(inst, &t) && !n48_agdc_fill_link_config_t(out, len, &t)) ? 0u : 0xe00002c2u;', '*kr = (agdc_timing_for_inst(kInstMonB, &t) && !n48_agdc_fill_link_config_t(out, len, &t)) ? 0u : 0xe00002c2u;')
plant(509, DPG, 'BRIEF 3: the kernel never asks the per-endpoint answerer', 'const bool m6done = gAgm.nfb > 1u && n48m6::agdc_answer(', 'const bool m6done = false && n48m6::agdc_answer(')
plant(510, M6P, 'BRIEF 4: perform copies from a non-DP pipe (pipe_may_copy is true for every instance)', 'constexpr bool pipe_may_copy(uint32_t pipeInst) { return pipeInst == kInstDp; }', 'constexpr bool pipe_may_copy(uint32_t pipeInst) { return pipeInst != kInstNone; }')
plant(511, FLOW, 'BRIEF 4: perform_inner no longer completes a non-DP pipe without a copy', '        if (!n48m6::pipe_may_copy(pinst)) {', '        if (false) {')
plant(512, FLOW, 'BRIEF 4: perform_inner ignores the latch (a registered monitor B pipe copies into the DP console)', '    const bool m6 = e.m6_on();\n    uint32_t pinst = n48m6::kInstDp;', '    const bool m6 = false;\n    uint32_t pinst = n48m6::kInstDp;')
plant(513, M6P, 'BRIEF 5: the latch is read live (every call reads the boot-arg again)', '    uint32_t l = __atomic_load_n(slot, __ATOMIC_ACQUIRE);\n    if (l == kLatchUnset) {', '    uint32_t l = kLatchUnset;\n    if (l == kLatchUnset) {')
plant(514, GLUE, 'BRIEF 5: the latch accessor reads the boot-arg directly instead of latch_get', 'return n48m6::latch_is_on(n48m6::latch_get(&gM6Latch, []() -> uint32_t {', 'return n48m6::latch_is_on(n48m6::latch_value(true, 1)); (void)(&gM6Latch, []() -> uint32_t {')
plant(515, GLUE, 'BRIEF 5: the latch is read at TWO sites', 'uint32_t n48disp_fact_bits(void) {', 'static bool m6_second(void) { uint32_t v = 0; return PE_parse_boot_argn("navi48-m6", &v, sizeof(v)) && v == 1u; }\nuint32_t n48disp_fact_bits(void) {')
plant(516, 'src/navi48-bringup/src/amd/native_disp_pure.h', 'BRIEF 6: the OFF path changed - fb_interlock_refuses ignores the latch argument default (a nub refuses pipeadopt even under m6)', '{ return !m6On && displayNubExists &&', '{ return displayNubExists &&')
plant(517, FLOW, 'BRIEF 6: the OFF path changed - the submit hook wires only after learning even with the latch off (always learns)', '            if (e.m6_on()) { inst = e.pipe_inst(self); (void)m6_learn_flow(e, self, args[0], inst); }', '            { inst = e.pipe_inst(self); (void)m6_learn_flow(e, self, args[0], inst); }')
plant(518, FLOW, 'BRIEF 6: the OFF path changed - the stamps use the instance sample even with the latch off', '    if (e.m6_on()) vi = e.pipe_inst(pipe);', '    vi = e.pipe_inst(pipe);')
plant(519, 'src/navi48-bringup/src/amd/native_fb_pure.h', 'BRIEF 6: the OFF path changed - fbpublish lifts R2 without the latch', '    if (p.m6) return kOk; ', '    if (true) return kOk; ')
plant(520, 'src/navi48-bringup/src/amd/native_metal_pure.h', 'BRIEF 6: the OFF path changed - the Metal nub publishes beside a display nub without the latch', '    if (g.dispNub && !g.m6) return kExclusive; ', '    if (false) return kExclusive; ')
plant(521, BAT, 'BRIEF 6: the battery itself: a changed scenario input (the golden no longer matches)', 'scenario("disarmed", [&](BatWorld &w, H &hh) { w.e.armedFlag = false;', 'scenario("disarmed", [&](BatWorld &w, H &hh) { w.e.armedFlag = true;')
plant(522, 'src/navi48-bringup/src/amd/native_disp_pure.h', 'BRIEF 6: the OFF path changed - pipe_ok in the legacy form accepts two pipes', 'return p.accelOk && p.dmReadable && p.count == 1u && p.havePipe', 'return p.accelOk && p.dmReadable && p.count >= 1u && p.havePipe')
# ---- the table, the guard and the learn --------------------------------------------------------------------------------------------------------------------------------------
plant(523, M6P, 'route_verdict ignores ambiguity', '    if (l.ambiguous) return kRouteAmbiguous;\n', '')
plant(524, M6P, 'route_verdict accepts an unseen surface', '    if (!l.found) return kRouteUnknown;\n', '')
plant(525, M6P, 'route_verdict accepts a surface with no readable ID', '    if (!haveId) return kRouteNoId;\n', '')
plant(526, M6P, 'route_verdict accepts a pipe on no known framebuffer', '    if (!inst_valid(pipeInst)) return kRouteBadPipe;\n', '')
plant(527, M6P, 'learn: an ID on a second pipe is not flagged (the first sighting wins silently)', '        else { ent_mark(e, inst, tx, sq); __atomic_store_n(&e.flags, kEntAmbiguous, __ATOMIC_RELEASE); r = kLBecameAmbiguous; }', '        else { ent_mark(e, inst, tx, sq); r = kLSame; }')
plant(528, M6P, 'learn: ID 0 is accepted', '    if (id == 0u) { __atomic_add_fetch(&t.learn[kLBadId], 1ull, __ATOMIC_RELAXED); return kLBadId; }\n', '')
plant(529, M6P, 'learn: an unknown instance is accepted', '    if (!inst_valid(inst)) { __atomic_add_fetch(&t.learn[kLBadInst], 1ull, __ATOMIC_RELAXED); return kLBadInst; }\n', '')
plant(530, M6P, 'learn: a full table overwrites the last entry', '    } else if (n < kMaxSurf) {', '    } else if (n <= kMaxSurf) {')
plant(531, M6P, 'lookup reports an unseen ID as instance 0', 'Look l { false, false, kInstNone };\n    if (id == 0u) return l;', 'Look l { false, false, kInstDp };\n    if (id == 0u) return l;')
plant(532, M6P, 'inst_of_fb maps an unknown pointer to the DP', '    if (fb == 0ull) return kInstNone;\n    for (uint32_t i = 0; i < kInstCount; ++i) if (m.fb[i] == fb) return i;\n    return kInstNone;', '    if (fb == 0ull) return kInstNone;\n    for (uint32_t i = 0; i < kInstCount; ++i) if (m.fb[i] == fb) return i;\n    return kInstDp;')
plant(533, M6P, 'ents: the expected instance set is not enforced', '    return seen == x.expectMask ? kEntsOk : kEntsInst;', '    return kEntsOk;')
plant(534, M6P, 'ents: two pipes on one instance are accepted', ' || (seen & (1u << p.inst)) != 0u) return kEntsInst;', ') return kEntsInst;')
plant(535, M6P, 'ents: a NULL slot is not recognised', '        if (p.nullp) return kEntsNull;\n', '')
plant(536, M6P, 'expect_mask forgets the DP', 'return 1u | (mona ? (1u << kInstMonA) : 0u)', 'return (mona ? (1u << kInstMonA) : 0u)')
plant(537, M6P, 'the instance <-> index table is swapped', 'constexpr uint32_t index_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISP_INDEX : inst == kInstMonA ? N48_DISPA_INDEX : 0u; }', 'constexpr uint32_t index_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISPA_INDEX : inst == kInstMonA ? N48_DISP_INDEX : 0u; }')
plant(538, M6P, 'the monitor B\'s OTG is OTG1', 'constexpr uint32_t otg_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISP_OTG : inst == kInstMonA ? N48_DISPA_OTG : 0u; }', 'constexpr uint32_t otg_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISPA_OTG : inst == kInstMonA ? N48_DISPA_OTG : 0u; }')
plant(539, M6P, 'the property blob drops the flags', 'w32(kBlobHdr + i * kBlobEnt + 8, t.e[i].flags);', 'w32(kBlobHdr + i * kBlobEnt + 8, 0u);')
plant(540, M6P, 'the property blob has no count', 'w32(0, kBlobVersion); w32(4, n);', 'w32(0, kBlobVersion); w32(4, 0u);')
plant(541, M6P, 'the 0x980 list is cut to the first framebuffer', '!n48_agdc_fill_gpu_capability(out, len, pci, g.fb, g.nfb)', '!n48_agdc_fill_gpu_capability(out, len, pci, g.fb, 1u)')
plant(542, M6P, 'the monitor A\'s pixel clock is the monitor B\'s', '    t->pixel_clock = g.pixHz; t->live = 0u;', '    t->pixel_clock = N48_DISP_PIXHZ; t->live = 0u;')
plant(543, M6P, 'an AGDC list of one framebuffer is answered here (the DP loses its live-raster answers)', '    if (g.nfb <= 1u || g.nfb > kInstCount || !out || !kr) return false;', '    if (g.nfb > kInstCount || !out || !kr) return false;')
plant(544, M6P, 'the DP\'s endpoint is answered here with the monitor B\'s raster', '    if (inst == kInstDp) return false;', '    if (false) return false;')
# ---- the flows -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(545, FLOW, 'the guard is bypassed on the DP\'s pipe', '    if (m6 && m6_route_flow(e, surf, pinst) != n48m6::kRouteOk) return kPfRouteRefused;', '    if (m6 && m6_route_flow(e, surf, pinst) == n48m6::kRouteBadPipe) return kPfRouteRefused;')
plant(546, FLOW, 'a pipe on no known framebuffer completes as "other instance" instead of being refused', '            if (pinst == n48m6::kInstNone) { e.m6_note_route(n48m6::kRouteBadPipe, pinst); return kPfRouteRefused; }', '            if (false) { e.m6_note_route(n48m6::kRouteBadPipe, pinst); return kPfRouteRefused; }')
plant(547, FLOW, 'the submit hook wires the descriptor of a non-DP pipe', '            if (inst == n48m6::kInstDp) (void)submit_prepare(e, args[0]);', '            (void)submit_prepare(e, args[0]);')
plant(548, FLOW, 'the learn trusts a transaction that names another pipe', '    if (tp != pipe) { e.m6_note_learn_skip(n48m6::kSkPipeMismatch); return n48m6::kLBadId; }', '    if (false) { e.m6_note_learn_skip(n48m6::kSkPipeMismatch); return n48m6::kLBadId; }')
plant(549, FLOW, 'the learn reads an ID from an object that fails the IOSurface class check', '    if (!kptr_ok(surf) || !e.class_derives(surf, n48m6::kSurfClass)) { e.m6_note_learn_skip(n48m6::kSkClass); return n48m6::kLBadId; }', '    if (!kptr_ok(surf)) { e.m6_note_learn_skip(n48m6::kSkClass); return n48m6::kLBadId; }')
plant(550, FLOW, 'the guard reads an ID from an object that fails the IOSurface class check', '    return kptr_ok(surf) && e.class_derives(surf, n48m6::kSurfClass) && e.rd32(surf + n48m6::kSurfId, id) && *id != 0u;', '    return kptr_ok(surf) && e.rd32(surf + n48m6::kSurfId, id) && *id != 0u;')
plant(551, M6P, 'the surface ID is read at the wrong offset', 'constexpr uint32_t kSurfId = 0x10u;', 'constexpr uint32_t kSurfId = 0x14u;')
plant(552, M6P, 'the transaction\'s pipe is read at the wrong offset', 'constexpr uint32_t kTxnPipe = 0x28u;', 'constexpr uint32_t kTxnPipe = 0x30u;')
plant(553, FLOW, 'the vblank stamps of a non-DP pipe come from the DP\'s OTG', 'if (!(vi == n48m6::kInstDp ? e.vbl_sample(&s) : e.vbl_sample_inst(vi, &s))) return kVblNoTiming;', 'if (!e.vbl_sample(&s)) return kVblNoTiming;')
plant(554, FLOW, 'a pipe on no known framebuffer still gets stamps', '    if (vi == n48m6::kInstNone) return kVblNoTiming;', '')
plant(555, FLOW, 'the stamps are not counted per instance', '    if (e.m6_on()) e.m6_note_stamp(vi, kVblWrote, s.periodNs);', '')
plant(556, FLOW, 'learn runs after wiring (order)', '            if (e.m6_on()) { inst = e.pipe_inst(self); (void)m6_learn_flow(e, self, args[0], inst); }     // 0.0.659 (M6): learn surface ID -> instance from the transaction (reads only)\n            if (inst == n48m6::kInstDp) (void)submit_prepare(e, args[0]);', '            if (e.m6_on()) inst = e.pipe_inst(self);\n            if (inst == n48m6::kInstDp) (void)submit_prepare(e, args[0]);\n            if (e.m6_on()) { (void)m6_learn_flow(e, self, args[0], inst); }')
plant(557, FLOW, 'the guard runs after the first source read', '    if (m6 && m6_route_flow(e, surf, pinst) != n48m6::kRouteOk) return kPfRouteRefused;     // 0.0.659: the routing guard, before any source read\n', '')
# ---- pure verdicts, admission and the interlocks --------------------------------------------------------------------------------------------------------------------------------
PURE_ = f'{K}/src/amd/native_disp_pure.h'
plant(558, PURE_, 'pipe_ok ignores the per-pipe entries', '    if (p.ents.n != 0u)       // 0.0.659 (M6)', '    if (false && p.ents.n != 0u)       // 0.0.659 (M6)')
plant(559, PURE_, 'pipe_ok does not compare the entry count with the display machine\'s', 'return p.accelOk && p.dmReadable && p.count == p.ents.n && n48m6::ents_verdict(p.ents) == n48m6::kEntsOk;', 'return p.accelOk && p.dmReadable && n48m6::ents_verdict(p.ents) == n48m6::kEntsOk;')
plant(560, PURE_, 'adopt\'s precheck does not see a NULL pipe among the entries', '(before.nullPipe || n48m6::ents_any_null(before.ents))', '(before.nullPipe)')
plant(561, PURE_, 'verb 106 is admitted without the display latch', '|| action == kActFbPublish || action == kActM6Stat || action == kActM6XStat || action == kActVramStat)); }', '|| action == kActFbPublish || action == kActM6XStat || action == kActVramStat)) || action == kActM6Stat; }')
plant(562, PURE_, 'verb 106 takes any page', '(action == kActM6Stat && arg < (uint64_t)n48m6::kStatPages) ||', '(action == kActM6Stat) ||')
plant(563, PURE_, 'verb 106 is not a pipe verb (n48disp_verb refuses it)', ' || action == kActReload || action == kActM6Stat; }', ' || action == kActReload; }')
plant(564, 'src/navi48-bringup/src/amd/native_fb_pure.h', 'R2 lifts the existing-nub refusal under the latch too', '    if (p.nubExists) return kExists;', '    if (p.nubExists && !p.m6) return kExists;')
plant(565, 'src/navi48-bringup/src/amd/native_fb_pure.h', 'R2 is not lifted under the latch', '    if (p.m6) return kOk; ', '    if (false) return kOk; ')
plant(566, 'src/navi48-bringup/src/amd/native_metal_pure.h', 'R3 is not lifted under the latch', '    if (g.dispNub && !g.m6) return kExclusive; ', '    if (g.dispNub) return kExclusive; ')
plant(567, PURE_, 'R4 is not lifted under the latch', '{ return !m6On && displayNubExists &&', '{ return displayNubExists &&')
plant(568, GLUE, 'the verb passes the wrong latch to the interlock', 'n48disp::fb_interlock_refuses(action, arg, n48fb_nub_exists(), n48m6_latched_on())', 'n48disp::fb_interlock_refuses(action, arg, n48fb_nub_exists(), true)')
plant(569, NUB, 'the Metal publish never reads the latch', '	g.m6 = n48m6_latched_on(); ', '	g.m6 = false; ')
plant(570, DNUB, 'fbpublish never reads the latch', '		pre.m6 = n48m6_latched_on(); ', '		pre.m6 = false; ')
# ---- the glue ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
plant(571, GLUE, 'a pipe\'s instance is looked up from the wrong word (pipe+0x90)', 'rd64(pipe + n48disp::kPipeFb, &fb)', 'rd64(pipe + n48disp::kPipeDm, &fb)')
plant(572, GLUE, 'the framebuffer map is never recorded at adopt', '        if (n48m6_latched_on()) m6_record_map();\n', '')
plant(573, GLUE, 'the table property is never published', 'const bool ok = Navi48MetalNub::setPublishedData(N48M6_PROP_SURF, gM6Blob, n);', 'const bool ok = false;')
plant(574, GLUE, 'the learn does not publish a change', '            m6_publish();\n        }\n        return r;', '        }\n        return r;')
plant(575, GLUE, 'the latch property is never set', '(void)Navi48MetalNub::setPublishedNumber(N48M6_PROP_LATCH, 1ull);', '')
plant(576, GLUE, 'the framebuffer map is not cleared when the pipes are forgotten', '            __atomic_store_n(&gD.m6Fb.fb[i], 0ull, __ATOMIC_RELEASE);\n', '')
plant(577, GLUE, 'the verb answers 106 nowhere', '    } else if (action == n48disp::kActM6Stat) {', '    } else if (action == 9999u) {')
plant(578, BRG, 'accelExperiment does not route 106', ' || action == 90 || action == n48disp::kActM6Stat) {', ' || action == 90) {')
plant(579, DCN, 'the per-instance vblank sample reads the DP\'s OTG', '	const uint32_t otg = n48m6::otg_of_inst(inst);', '	const uint32_t otg = 0u;')
plant(580, DCN, 'scan_query does not report the latch', '	if (n48m6_latched_on()) flags |= N48N_SCANQ_M6; ', '	if (false) flags |= N48N_SCANQ_M6; ')
plant(581, M6P, 'a published nub without a started framebuffer does not refuse pipeagdc (the 0.0.659 plant c06: return false became continue)', '        if (v == kColRefuse) { e.refuse(inst); return false; }', '        if (v == kColRefuse) { continue; }')
plant(582, DPG, 'a failed AGDC build leaves the multi-framebuffer list in place', '        if (st != 0u) gAgm.nfb = 0u; ', '        if (false) gAgm.nfb = 0u; ')
plant(583, DPG, 'the AGDC list is built without the latch', '        if (ok && n48m6_latched_on()) ok = collect_m6();', '        if (ok) ok = collect_m6();')
plant(584, DNUB, 'the nub finds a framebuffer of any class', 'strcmp(cn, "Navi48Framebuffer") == 0 && svc->metaCast("IOFramebuffer")', 'svc->metaCast("IOFramebuffer")')
plant(585, CLI, 'the CLI sends the wrong action for m6stat', 'else if (what && !strcmp(what, "m6stat")) in = 106;', 'else if (what && !strcmp(what, "m6stat")) in = 85;')
plant(586, CLI, 'the CLI asks page 0 for every page', 'dr_call(106, page, o)', 'dr_call(106, 0, o)')
plant(587, S1C, 'the native client still reports build 658', 'kN1cKextBuild = 664;', 'kN1cKextBuild = 658;')
plant(588, VARM6, 'the variant lacks navi48-m6=1', 'navi48-fb2=1 navi48-m6=1</string>', 'navi48-fb2=1</string>')
plant(589, VARM6, 'the variant lacks navi48-fb2=1', 'navi48-disp2=1 navi48-fb2=1 navi48-m6=1</string>', 'navi48-disp2=1 navi48-m6=1</string>')
plant(590, VARM6, 'the variant drops navi48-metal-ws', 'navi48-metal-ws=1 ', '')
plant(591, 'src/navi48-bringup/src/Navi48NativeABI.h', 'the latch flag moves in the kext ABI header', '#define N48N_SCANQ_M6         (1u << 5)', '#define N48N_SCANQ_M6         (1u << 6)')
plant(592, M6P, 'an unknown framebuffer is reported as present in the entry check', '        if (!inst_valid(p.inst) ||', '        if (')
plant(593, FLOW, 'the restart-loop guard counts the monitor B\'s pipe too', '    const bool counted = n48m6::restart_guard_counts(m6, m6 ? e.pipe_inst(pipe) : n48m6::kInstDp);', '    const bool counted = true;')
plant(594, FLOW, 'the restart-loop guard counts no pipe under the latch', '    const bool counted = n48m6::restart_guard_counts(m6, m6 ? e.pipe_inst(pipe) : n48m6::kInstDp);', '    const bool counted = !m6;')
plant(595, DCN, 'scan_query: the latch line splits the DTO_VALID if/else (the else binds to the latch: a 0.0.658 change with the latch OFF)', 'if (pixHz != 0ull) flags |= N48N_SCANQ_DTO_VALID;\n\telse {', 'if (pixHz != 0ull) flags |= N48N_SCANQ_DTO_VALID;\n\tif (n48m6_latched_on()) flags |= N48N_SCANQ_M6;\n\telse {')
# ---- 0.0.660 (queue 290): E1 endpoint = index, S1 self-healing table, S2 serialised publish, S5, N2, N4, T1, K1, S7 ----------------------------------------------------------------------------------
plant(596, M6P, 'E1: the endpoint is the MASK-BIT reading again (0.0.659: both endpoints answered the DP)', 'return (nfb > 1u && ep < nfb) ? ep : 0u; }', 'return (nfb > 1u && ep >= 1u && ep <= nfb) ? ep - 1u : 0u; }')
plant(597, M6P, 'E1: an endpoint past the list answers the LAST entry instead of the DP', 'return (nfb > 1u && ep < nfb) ? ep : 0u; }', 'return nfb > 1u ? (ep < nfb ? ep : nfb - 1u) : 0u; }')
plant(598, M6P, 'E1: an out-of-range endpoint is not counted', '    if (!agdc_ep_in_range(ep, g.nfb)) { g.nOor++; g.lastOor = ep; }', '')
plant(599, M6P, 'E1: the range test is off by one', 'constexpr bool agdc_ep_in_range(uint32_t ep, uint32_t nfb) { return ep < nfb; }', 'constexpr bool agdc_ep_in_range(uint32_t ep, uint32_t nfb) { return ep <= nfb; }')
plant(600, M6P, 'E1: the last out-of-range endpoint is not kept', 'g.nOor++; g.lastOor = ep;', 'g.nOor++;')
plant(601, M6P, 'S1: a stale old owner never lets go (every re-learn on another pipe is ambiguous)', '        else if (owner_stale(e, owner, __atomic_load_n(&t.txn[owner], __ATOMIC_RELAXED), sq)) { ent_fill(e, id, inst, 0u, tx, sq); r = kLReplaced; }', '')
plant(602, M6P, 'S1: staleness ignores the owner\'s own submit count', '    if ((uint32_t)(tn - __atomic_load_n(&e.tx[o], __ATOMIC_RELAXED)) >= kStaleSubmits) return true;\n', '')
plant(603, M6P, 'S1: staleness ignores the table clock (an idle owner never goes stale)', '    return now - __atomic_load_n(&e.sq[o], __ATOMIC_RELAXED) >= kStaleEvents;', '    return false;')
plant(604, M6P, 'S1: kStaleSubmits is 1 (a live owner is called stale at once: a mirrored surface loses its ambiguity)', 'constexpr uint32_t kStaleSubmits = 4u * kMaxSurf;', 'constexpr uint32_t kStaleSubmits = 1u;')
plant(605, M6P, 'S1: kStaleEvents is huge (an idle monitor B keeps the DP refused for ever)', 'constexpr uint32_t kStaleEvents = 256u;', 'constexpr uint32_t kStaleEvents = 0x7fffffffu;')
plant(606, M6P, 'S1: the eviction takes the MOST recently seen entry', '    for (uint32_t i = 1; i < kMaxSurf; ++i) { const uint64_t s = __atomic_load_n(&t.e[i].seen, __ATOMIC_RELAXED); if (s < bs) { bs = s; best = i; } }', '    for (uint32_t i = 1; i < kMaxSurf; ++i) { const uint64_t s = __atomic_load_n(&t.e[i].seen, __ATOMIC_RELAXED); if (s > bs) { bs = s; best = i; } }')
plant(607, M6P, 'S1: a routed present does not update the last-seen', '        if (inst_valid(o)) { ent_mark(t.e[i], o, __atomic_load_n(&t.txn[o], __ATOMIC_RELAXED), __atomic_load_n(&t.seq, __ATOMIC_RELAXED)); done = true; }', '        if (inst_valid(o)) { done = true; }')
plant(608, M6P, 'S1: reset leaves the entry count (the old IDs stay visible)', '    __atomic_store_n(&t.n, 0u, __ATOMIC_RELEASE);                            // readers see an empty table first, then the entries are cleared\n', '')
plant(609, M6P, 'S1: reset leaves the per-pipe clocks', '    for (uint32_t k = 0; k < kInstCount; ++k) __atomic_store_n(&t.txn[k], 0u, __ATOMIC_RELAXED);\n', '')
plant(610, M6P, 'S1: an ambiguous entry whose other owner went stale is never healed', '            if (live == 0u) { ent_fill(e, id, inst, 0u, tx, sq); r = kLReplaced; }', '            if (false) { ent_fill(e, id, inst, 0u, tx, sq); r = kLReplaced; }')
plant(611, M6P, 'S1: a re-learned ID is not republished', 'constexpr bool learn_changed(uint32_t r) { return r == kLNew || r == kLBecameAmbiguous || r == kLReplaced || r == kLEvicted; }', 'constexpr bool learn_changed(uint32_t r) { return r == kLNew || r == kLBecameAmbiguous || r == kLEvicted; }')
plant(612, M6P, 'S1: an eviction is not republished', 'constexpr bool learn_changed(uint32_t r) { return r == kLNew || r == kLBecameAmbiguous || r == kLReplaced || r == kLEvicted; }', 'constexpr bool learn_changed(uint32_t r) { return r == kLNew || r == kLBecameAmbiguous || r == kLReplaced; }')
plant(613, FLOW, 'S1 ORDER: the arm resets the table AFTER the gate byte is written', '    if (e.m6_on()) e.m6_reset();                                             // 0.0.660 (M6, S1): every arm / re-arm starts from an empty surface table, BEFORE the byte opens the gate (a submit learned after it is kept)\n    const bool ok = e.arm_write(1u);', '    const bool ok = e.arm_write(1u);\n    if (e.m6_on()) e.m6_reset();')
plant(614, FLOW, 'S1: the arm never resets the table', '    if (e.m6_on()) e.m6_reset();                                             // 0.0.660 (M6, S1): every arm / re-arm starts from an empty surface table, BEFORE the byte opens the gate (a submit learned after it is kept)\n', '')
plant(615, FLOW, 'S1 ORDER: pipereload resets the table AFTER the restart window opened', '    if (e.m6_on() && !e.m6flip_on()) e.m6_reset();                          // 0.0.660 (M6, S1): the table is emptied (and republished) when the window OPENS. 0.0.661 (R1) with navi48-m6flip: NOT here - the old WindowServer is still submitting and would refill it; the reset moves to the new start\'s first slot-267 (reload_on_init_fb). The latch OFF never reaches it\n    ReloadWin &w = e.rw();\n', '    ReloadWin &w = e.rw();\n')
plant(616, FLOW, 'S1: pipereload never resets the table (variant of 615: reset moved after note_reload)', '    e.note_reload(kRwOpened);\n}', '    e.note_reload(kRwOpened);\n    if (e.m6_on()) e.m6_reset();\n}')
plant(617, FLOW, 'S1: the arm resets the table with the latch OFF too', '    if (e.m6_on()) e.m6_reset();                                             // 0.0.660 (M6, S1): every arm / re-arm starts from an empty surface table, BEFORE the byte opens the gate (a submit learned after it is kept)', '    e.m6_reset();')
plant(618, FLOW, 'S1: a routed present is not a sighting', '    if (v == n48m6::kRouteOk) e.m6_touch(id);', '')
plant(619, FLOW, 'S1: a REFUSED present is a sighting too', '    if (v == n48m6::kRouteOk) e.m6_touch(id);', '    e.m6_touch(id);')
plant(620, GLUE, 'S1: the glue reset does not republish the empty table', '        m6_publish();\n    }\n    void m6_touch(uint32_t id)', '    }\n    void m6_touch(uint32_t id)')
plant(621, M6P, 'S2: the owner publishes without building first (it republishes the previous blob)', '        const uint32_t n = build();\n        if (n != 0u && publish(n)) { g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }\n        seam();', '        const uint32_t n = 8u;\n        if (n != 0u && publish(n)) { g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }\n        seam();')
plant(622, M6P, 'S2: the owner does not look again after letting go of the gate', '        if (__atomic_load_n(&g.gen, __ATOMIC_SEQ_CST) == seen) return done;      // nobody asked since the build started; otherwise (a request arrived while we published, or while we let go) go round again', '        return done;')
plant(623, M6P, 'S2 ORDER: a caller bumps the generation only if it wins the gate (a loser leaves no trace)', '    __atomic_add_fetch(&g.gen, 1u, __ATOMIC_SEQ_CST);                           // BEFORE the CAS: a caller that loses has already told the owner\n', '')
plant(624, GLUE, 'S2: the blob is a ~200-byte local of the submit path again', '        (void)n48m6::publish_serialised(gD.m6Pub,', '        uint8_t blob[n48m6::kBlobMax]; (void)blob;\n        (void)n48m6::publish_serialised(gD.m6Pub,')
plant(625, GLUE, 'S2: a second writer of the property bypasses the gate', '        m6_publish();\n    }\n    void m6_touch(uint32_t id)', '        (void)Navi48MetalNub::setPublishedData(N48M6_PROP_SURF, gM6Blob, 8u);\n    }\n    void m6_touch(uint32_t id)')
plant(626, M6P, 'S2: a failed property write is counted as a publish', '        if (n != 0u && publish(n)) { g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }\n        seam();', '        if (n != 0u) { (void)publish(n); g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }\n        seam();')
plant(627, M6P, 'S5: the restart guard exempts an unplaceable pipe (the 0.0.659 fail-open)', 'constexpr bool restart_guard_counts(bool m6, uint32_t pipeInst) { return !m6 || (pipeInst != kInstMonA && pipeInst != kInstMonB); }', 'constexpr bool restart_guard_counts(bool m6, uint32_t pipeInst) { return !m6 || pipeInst == kInstDp; }')
plant(628, M6P, 'S5: the restart guard exempts nothing under the latch', 'return !m6 || (pipeInst != kInstMonA && pipeInst != kInstMonB); }', 'return true; }')
plant(629, M6P, 'N2: pack_sat does not saturate (a big counter spills into its neighbour)', '    for (uint32_t i = 0; i < n && sh < 64u; ++i) { out |= sat_bits(v[i], bits[i]) << sh; sh += bits[i]; }', '    for (uint32_t i = 0; i < n && sh < 64u; ++i) { out |= v[i] << sh; sh += bits[i]; }')
plant(630, M6P, 'N2: sat_bits is off by one at the boundary', 'constexpr uint64_t sat_bits(uint64_t v, uint32_t bits) { return (bits >= 64u || v < (1ull << bits)) ? v : ((1ull << bits) - 1ull); }', 'constexpr uint64_t sat_bits(uint64_t v, uint32_t bits) { return (bits >= 64u || v <= (1ull << bits)) ? v : ((1ull << bits) - 1ull); }')
plant(631, GLUE, 'N2: the route counters are packed by raw shift-or again', '              out[11] = n48m6::pack_sat(f, n48m6::kPackRoute, 4u); }', '              out[11] = f[0] | (f[1] << 16) | (f[2] << 32) | (f[3] << 48); }')
plant(632, GLUE, 'N2: the learn bookkeeping is packed by raw shift-or again', '              out[12] = n48m6::pack_sat(f, n48m6::kPackLearnSkip, 8u); }', '              out[12] = f[0] | (f[1] << 8) | (f[2] << 16) | (f[3] << 24) | (f[4] << 32) | (f[5] << 40) | (f[6] << 48) | (f[7] << 56); }')
plant(633, M6P, 'N2: the learn-skip layout is not 8 bits per field', 'constexpr uint8_t kPackLearnSkip[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };', 'constexpr uint8_t kPackLearnSkip[8] = { 16, 8, 8, 8, 8, 8, 8, 8 };')
plant(634, M6P, 'N4: the static_assert beside the downward loops is gone', 'static_assert(kInstMonA >= 1u && kInstMonB > kInstMonA, "the downward instance loops (monitor B, then monitor A) end because kInstMonA >= 1");\n', '')
plant(635, M6P, 'T1 (c06): a published nub whose framebuffer has not started is skipped, not refused', '        if (v == kColRefuse) { e.refuse(inst); return false; }\n        if (v == kColUse) n++;', '        if (v == kColRefuse) { continue; }\n        if (v == kColUse) n++;')
plant(636, M6P, 'T1: collect counts a published nub that is not started as used', 'constexpr uint32_t collect_verdict(bool nubPublished, bool fbStarted) { return !nubPublished ? kColSkip : fbStarted ? kColUse : kColRefuse; }', 'constexpr uint32_t collect_verdict(bool nubPublished, bool fbStarted) { return !nubPublished ? kColSkip : (void)fbStarted, kColUse; }')
plant(637, M6P, 'T1: the 0x980 list order puts the monitor A before the monitor B', '    for (uint32_t inst = kInstMonB; inst >= kInstMonA; --inst) if (have[inst]) instOut[k++] = inst;', '    for (uint32_t inst = kInstMonA; inst <= kInstMonB; ++inst) if (have[inst]) instOut[k++] = inst;')
plant(638, M6P, 'T1 (c08): the raster check ignores the active size', 'constexpr bool raster_accepted(bool lit, uint32_t aw, uint32_t ah, uint32_t w, uint32_t h) { return lit && w != 0u && h != 0u && aw == w && ah == h; }', 'constexpr bool raster_accepted(bool lit, uint32_t aw, uint32_t ah, uint32_t w, uint32_t h) { return lit && w != 0u && h != 0u; }')
plant(639, M6P, 'T1: the raster check ignores the width', '&& aw == w && ah == h; }', '&& ah == h; }')
plant(640, M6P, 'T1: the raster check accepts an unlit OTG', 'return lit && w != 0u && h != 0u && aw == w && ah == h; }', 'return w != 0u && h != 0u && aw == w && ah == h; }')
plant(641, M6P, 'T1: raster_flow forgets the +1 of the totals', '    *hTot = ht1 + 1u; *vTot = vt1 + 1u;', '    *hTot = ht1; *vTot = vt1;')
plant(642, DCN, 'T1: the per-instance sample no longer runs the tested raster acceptance', '	if (!n48m6::raster_flow(renv, otg, w, h, &hTot, &vTot)) return false;', '	hTot = 2720u; vTot = 1481u; (void)renv;')
plant(643, DPG, 'T1: pipeagdc\'s collect has a loop of its own again', 'bool collect_m6() { mn = 1u; return n48m6::collect_flow(*this, &mn); }', 'bool collect_m6() { mn = 1u; return true; }')
plant(644, M6P, 'K1: a pipe that never submitted reports age 0 (looks alive)', 'return lastNs == 0ull ? kAgeNever :', 'return lastNs == 0ull ? 0ull :')
plant(645, GLUE, 'K1: the submit hook no longer stamps the pipe\'s clock', '        if (n48m6::inst_valid(inst)) __atomic_store_n(&gD.m6LastTxnNs[inst], now_ns(), __ATOMIC_RELAXED);        // 0.0.660 (K1)\n', '')
plant(646, GLUE, 'K1: m6stat page 3 is not served', '        } else if (arg == 3ull) {                                         // page 3 (0.0.660, K1)', '        } else if (arg == 9ull) {                                         // page 3 (0.0.660, K1)')
plant(647, PURE_, 'K1: verb 106 refuses page 3', '(action == kActM6Stat && arg < (uint64_t)n48m6::kStatPages) ||', '(action == kActM6Stat && arg <= 2ull) ||')
plant(648, GLUE, 'S7: the framebuffer reference is retained a second time from its raw address', '            gD.m6FbRef[i] = refs[i];', '            refs[i]->retain(); gD.m6FbRef[i] = refs[i];')
plant(649, GLUE, 'S7: the probe leaks the collected references', '        m6_release_refs(refs);                                             // the map only held addresses for the comparison above\n', '')
plant(650, GLUE, 'S7: record_map leaks the fresh reference of an already recorded slot', '            if (gD.m6FbRef[i]) { refs[i]->release(); refs[i] = nullptr; continue; }', '            if (gD.m6FbRef[i]) { continue; }')
plant(651, M6P, 'E1: agdc_instance reverses the list order (endpoint 0 is no longer the DP)', 'return (listInst != nullptr && nfb >= 1u && agdc_position(ep, nfb) < nfb) ? listInst[agdc_position(ep, nfb)] : kInstDp;', 'return (listInst != nullptr && nfb >= 1u) ? listInst[ep < nfb ? nfb - 1u - ep : 0u] : kInstDp;')
# ---- 0.0.661 (M6 Stage 1b, queue 293 review R1 / the flip-OFF identity), behind navi48-m6flip -------------------------------------------------------------------------------------------------------------
plant(660, FLOW, 'R1: with m6flip the reset still runs when the restart window OPENS (the dying WindowServer refills the table afterwards)', 'if (e.m6_on() && !e.m6flip_on()) e.m6_reset();', 'if (e.m6_on()) e.m6_reset();')
plant(661, FLOW, 'R1: no reset at the new start\'s first slot-267 (a leftover entry survives into the new start)', '        if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', '')
plant(662, FLOW, 'R1: the reset repeats at EVERY slot-267 of the new start (it would wipe the new start\'s own learns)', '__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', 'e.m6_on() && e.m6flip_on()) e.m6_reset();')
plant(663, FLOW, 'R1: the reset needs only the flip latch (m6flip without m6 resets)', '__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', '__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6flip_on()) e.m6_reset();')
plant(664, M6P, 'flip-OFF identity: the eviction victim rule applies with the latch OFF (victim_slot always)', 'ent_fill(t.e[m6flip ? victim_slot(t) : lru_slot(t)], id, inst, 0u, tx, sq);', 'ent_fill(t.e[victim_slot(t)], id, inst, 0u, tx, sq);')
plant(665, M6P, 'flip-OFF identity: the version-1 blob changes', 'w32(0, kBlobVersion); w32(4, n);\n    for (uint32_t i = 0; i < n; ++i) { w32(kBlobHdr + i * kBlobEnt, t.e[i].id);', 'w32(0, kBlobVersion); w32(4, n + 0u);\n    for (uint32_t i = 1; i < n; ++i) { w32(kBlobHdr + i * kBlobEnt, t.e[i].id);')
plant(666, GLUE, 'flip-OFF identity: every table change publishes through the rate gate even with the latch OFF', '        if (!n48m6flip_latched_on()) { m6_publish_now(); return; }\n', '')
plant(667, GLUE, 'R2: the blob version follows a constant true (version 2 is built with the latch OFF)', 'const bool v2 = n48m6flip_latched_on();', 'const bool v2 = true;')
plant(668, FLOW, 'R1: the reset runs at the first slot-267 even BEFORE the old client\'s close was seen (a 267 inside the window resets)', '    if (__atomic_load_n(&w.closeSeen, __ATOMIC_ACQUIRE) != 0u) {\n        if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();', '    if (e.m6_on() && e.m6flip_on() && __atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u) e.m6_reset();\n    if (__atomic_load_n(&w.closeSeen, __ATOMIC_ACQUIRE) != 0u) {\n        if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && false) e.m6_reset();')
shutil.rmtree(scr, ignore_errors=True)
print('native_m6_plant: %d plant(s), %d escaped' % (total, escaped))
sys.exit(1 if escaped else 0)
PY
