//
//  native_disp_flow.h - the SEQUENCING of the display pipe (kext 0.0.613): adopt, arm, the four pipe hooks, the perform copy, slot 62, stamps. Templates over an
//  environment E, so the very code the kext runs is what tests/native_disp_test.cpp runs, against a fake environment (fake kernel memory, a fake console, a call
//  recorder): ordering and reachability are tested on the real flow, not on a source pin. No kernel header.
//
//  The kernel environment is src/amd/native_disp.cpp (KernelEnv). The decisions it calls are amd/native_disp_pure.h. E must provide (all pointers are addresses
//  as uint64_t; every read / write is kernel-pointer checked INSIDE E and returns false when refused):
//    bool latch_on();  uint32_t facts();  void facts_add(uint32_t bits);
//    bool rd8/rd16/rd32/rd64(uint64_t addr, T *out);   bool wr8/wr64(uint64_t addr, T v);
//    bool known_pipe(uint64_t p);   bool armed();
//    perform:  bool console(ConsoleInfo *c);  uint32_t scratch_acquire(uint8_t **buf, uint32_t *cap) /* 0 ok, 1 busy, 2 none */;  void scratch_release();
//              bool src_read(uint64_t md, uint64_t off, uint8_t *dst, uint64_t len);  bool console_write(uint64_t off, const uint8_t *src, uint64_t len);
//              uint64_t now_ns();  void note_perform(uint32_t reason, uint64_t bytes, uint64_t ns);  void note_source(const SourceLog &s);
//    slot 267: uint8_t *standin(uint64_t pipe);  void note_init_fb(uint64_t pipe, uint64_t res);
//    adopt:    bool adopt_begin();  void adopt_end(uint32_t st);  bool find_accel();  bool accel_layout_ok();  PipeProbe probe_pipe();  bool request_probe();
//              bool publish_caps();  void record_adopted(const PipeProbe &p);
//    arm:      bool arm_write(uint8_t v);  void disarm();
//    0.0.615:  CrashGuard &guard();  void note_autodisarm(uint32_t cause /* 0.0.617: AutoCause */);  void clear_autodisarm();
//    0.0.617 (K6): bool scan_active() /* lock-free: a native client owns the scanout plane */;  void note_scan_owned_submit();  Ival &ivl();  void forget_pipes();  void note_withdraw(bool wasArmed);  void note_null_pipe();
//    0.0.618:  bool vbl_on() /* the pipevbl switch */;  bool class_derives(uint64_t obj, const char *name) /* the class or a superclass is `name` */;  bool vbl_sample(VblSample *s) /* one OTG0 timing sample, read-only; false = none */;
//              void note_vbl(uint32_t reason, const VblPlan &p, uint64_t periodNs, uint64_t delayNs);
//    0.0.619:  ReloadWin &rw() /* the operator restart window state */;  void note_reload(uint32_t ev /* ReloadEv */);
//    stamps:   uint64_t accel_addr();  bool class_is(uint64_t obj, const char *name);
//    slot 62:  void note_res62(const Res62Plan &p, bool smKnown, bool surfKnown, bool flagsKnown, bool bit4);
//    0.0.616:  MdCache &mdc();  bool md_prepare(uint64_t md) /* retain + prepare(kIODirectionOut); true = prepared and held */;  void md_unprepare(uint64_t md) /* complete + release */;
//              bool hung();  void note_md(uint32_t ev, uint64_t md, uint64_t detail);
//
#pragma once
#include "native_disp_pure.h"

namespace n48disp {

struct ConsoleInfo { uint64_t width, height, stride, bytes; };
// 0.0.615 (G1): the crash-loop guard's state. Atomics only (the hooks take no lock). n = slot-267 calls since the arm; ts = the uptime stamps of the last three; performed = a slot-277 call was seen since the arm.
struct CrashGuard { uint64_t ts[3]; uint32_t n; uint32_t performed; };
inline void guard_reset(CrashGuard &g) {                                  // on every arm
    for (int i = 0; i < 3; ++i) __atomic_store_n(&g.ts[i], 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&g.performed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g.n, 0u, __ATOMIC_RELEASE);
}
inline void guard_on_perform(CrashGuard &g) { __atomic_store_n(&g.performed, 1u, __ATOMIC_RELEASE); }
inline bool guard_on_init_fb(CrashGuard &g, uint64_t nowNs) {             // true = trip (the caller disarms)
    const uint32_t idx = __atomic_fetch_add(&g.n, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g.ts[idx % 3u], nowNs, __ATOMIC_RELEASE);
    const uint64_t oldest = __atomic_load_n(&g.ts[(idx + 1u) % 3u], __ATOMIC_ACQUIRE);   // the call two before this one (idx-2 = idx+1 mod 3)
    return guard_trip(idx, nowNs, oldest);
}

// ---- 0.0.619 (R1): the operator restart window (the decisions are native_disp_pure.h reload_*) ---------------------------------------------------------------------------------
// Atomics only (the close and slot-267 hooks take no lock). openNs = the uptime stamp of the verb; open = the window is open (cleared by the first of: the new client's first slot 267 with the pipe still
// armed, expiry, a disarm of any kind); closeSeen = a uid-88 close was tolerated inside it (only then can a slot 267 close the window: the OLD WindowServer's own slot-267 calls do not).
struct ReloadWin { uint64_t openNs; uint32_t open; uint32_t closeSeen; uint64_t opens, tolerated, closed267, expired; };
inline void reload_shut(ReloadWin &w) {                                   // silent: the pipe was disarmed (any cause) or withdrawn, so a window has nothing left to tolerate
    __atomic_store_n(&w.open, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&w.closeSeen, 0u, __ATOMIC_RELEASE);
}
// True only when the window is open, inside its 15 s, and the GPU is not HUNG. An expired window is closed (and logged) by whoever sees it first.
template <class E> bool reload_honoured_now(E &e) {
    ReloadWin &w = e.rw();
    if (__atomic_load_n(&w.open, __ATOMIC_ACQUIRE) == 0u) return false;
    const uint64_t on = __atomic_load_n(&w.openNs, __ATOMIC_ACQUIRE);
    if (!reload_live(1u, on, e.now_ns())) {
        uint32_t exp = 1u;
        if (__atomic_compare_exchange_n(&w.open, &exp, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&w.closeSeen, 0u, __ATOMIC_RELEASE);
            __atomic_add_fetch(&w.expired, 1ull, __ATOMIC_RELAXED);
            e.note_reload(kRwExpired);
        }
        return false;
    }
    return reload_honoured(true, e.hung());
}
// The verb: open a fresh one-shot window (a second call restarts the 15 s).
template <class E> void reload_open_flow(E &e) {
    ReloadWin &w = e.rw();
    __atomic_store_n(&w.open, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&w.closeSeen, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&w.openNs, e.now_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&w.open, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&w.opens, 1ull, __ATOMIC_RELAXED);
    e.note_reload(kRwOpened);
}
// The state for the verb's report: 0 closed, 1 open, 2 open with the close tolerated; remainingNs = what is left of the 15 s.
template <class E> uint32_t reload_state_flow(E &e, uint64_t *remainingNs) {
    ReloadWin &w = e.rw();
    *remainingNs = 0ull;
    if (__atomic_load_n(&w.open, __ATOMIC_ACQUIRE) == 0u) return 0u;
    const uint64_t on = __atomic_load_n(&w.openNs, __ATOMIC_ACQUIRE), now = e.now_ns();
    const bool live = reload_live(1u, on, now);
    if (!live) { (void)reload_honoured_now(e); return 0u; }                  // the same expiry path as the hooks
    *remainingNs = kReloadWindowNs - (now - on);
    return reload_state(true, __atomic_load_n(&w.closeSeen, __ATOMIC_ACQUIRE) != 0u);
}
// K1 side: the uid-88 close while armed. true = tolerated (the caller must NOT disarm).
template <class E> bool reload_tolerate_close(E &e) {
    if (!reload_honoured_now(e)) return false;
    ReloadWin &w = e.rw();
    __atomic_store_n(&w.closeSeen, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&w.tolerated, 1ull, __ATOMIC_RELAXED);
    e.note_reload(kRwTolerated);
    return true;
}
// K3 side: a slot-267 call while armed. true = inside the window, NOT counted by the guard. The first one after a tolerated close closes the window (the pipe is armed: the caller checked).
template <class E> bool reload_on_init_fb(E &e) {
    if (!reload_honoured_now(e)) return false;
    ReloadWin &w = e.rw();
    if (__atomic_load_n(&w.closeSeen, __ATOMIC_ACQUIRE) != 0u) {
        uint32_t exp = 1u;
        if (__atomic_compare_exchange_n(&w.open, &exp, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&w.closeSeen, 0u, __ATOMIC_RELEASE);
            __atomic_add_fetch(&w.closed267, 1ull, __ATOMIC_RELAXED);
            e.note_reload(kRwClosed267);
        }
    }
    return true;
}

// ---- 0.0.616: the plane-0 accessors perform AND submit share, and the prepared-descriptor cache flows ---------------------------------------------------------------------
// One accessor, two callers: perform reads the descriptor through it and submit prepares the descriptor it finds through it, so they cannot disagree about which object is "the source".
// Returns 0 on success (surf, res set), else the PerfReason that perform reports.
template <class E> uint32_t plane0_locate(E &e, uint64_t txn, uint64_t *surf, uint64_t *res) {
    if (!kptr_ok(txn)) return kPfBadTxn;
    uint64_t dirty = 0, arr = 0;
    if (!e.rd64(txn + kTxnDirty, &dirty)) return kPfBadTxn;
    if ((dirty & kDirtyPlane0) == 0ull) return kPfNoPlane;
    if (!e.rd64(txn + kTxnPlanes, &arr) || !kptr_ok(arr)) return kPfBadArr;
    if (!e.rd64(arr + kPlaneSurf, surf) || !e.rd64(arr + kPlaneRes, res) || !kptr_ok(*surf) || !kptr_ok(*res)) return kPfBadSurf;
    return 0u;
}
// The SysMemory object's length and its memory descriptor (sysmem+0x40, +0xd0). false = unreadable / not kernel pointers.
template <class E> bool sysmem_md(E &e, uint64_t sm, uint64_t *smLen, uint64_t *md) {
    return kptr_ok(sm) && e.rd64(sm + kSmLen, smLen) && e.rd64(sm + kSmMd, md) && kptr_ok(*md);
}

constexpr uint64_t kMdDrainNs = 200ull * 1000000ull;   // teardown waits at most this long for a perform that is inside the entry
enum MdEnsure : uint32_t { kMdHit = 0, kMdFilled = 1, kMdPrepFail = 2, kMdFull = 3, kMdHungRefused = 4, kMdBadPtr = 5, kMdSkipDisarmed = 6, kMdNoSource = 7, kMdSkipScanOwned = 8 };
enum MdNote : uint32_t { kMdNoteFill = 0, kMdNotePrepFail = 1, kMdNoteEvict = 2, kMdNoteFull = 3, kMdNoteTorn = 4, kMdNoteLeak = 5, kMdNoteDrainFail = 6, kMdNoteHung = 7, kMdNoteMiss = 8 };

// Fill / refresh one descriptor. Runs ONLY from the submit hook (thread context, may block in prepare()). Never called from perform.
template <class E> uint32_t mdc_ensure(E &e, uint64_t md) {
    MdCache &c = e.mdc();
    if (!kptr_ok(md)) return kMdBadPtr;
    if (mdc_touch(c, md)) { __atomic_add_fetch(&c.hits, 1ull, __ATOMIC_RELAXED); return kMdHit; }
    if (e.hung()) { e.note_md(kMdNoteHung, md, 0ull); return kMdHungRefused; }   // HUNG: no new wiring and no eviction (see mdc_teardown)
    int slot = -1;
    for (uint32_t i = 0; i < kMdCacheN && slot < 0; ++i) {
        uint32_t exp = kMdEmpty;
        if (__atomic_compare_exchange_n(&c.e[i].state, &exp, (uint32_t)kMdFilling, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) slot = (int)i;
    }
    if (slot < 0) {                                                           // full: evict the least recently used entry that no perform is inside
        bool tried[kMdCacheN] = {};
        for (uint32_t n = 0; n < kMdCacheN && slot < 0; ++n) {
            int v = -1; uint64_t best = ~0ull;
            for (uint32_t i = 0; i < kMdCacheN; ++i) {
                if (tried[i] || __atomic_load_n(&c.e[i].state, __ATOMIC_ACQUIRE) != kMdReady) continue;
                const uint64_t l = __atomic_load_n(&c.e[i].last, __ATOMIC_RELAXED);
                if (v < 0 || l < best) { v = (int)i; best = l; }
            }
            if (v < 0) break;
            tried[v] = true;
            if (!mdc_retire(c, (uint32_t)v)) { __atomic_add_fetch(&c.evictBlocked, 1ull, __ATOMIC_RELAXED); continue; }   // busy: NEVER evicted
            const uint64_t old = c.e[v].md;
            e.md_unprepare(old);                                              // complete() + release(): no lock is held here, perform takes none
            __atomic_add_fetch(&c.evicted, 1ull, __ATOMIC_RELAXED);
            e.note_md(kMdNoteEvict, old, (uint64_t)v);
            __atomic_store_n(&c.e[v].md, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&c.e[v].state, (uint32_t)kMdFilling, __ATOMIC_RELEASE);
            slot = v;
        }
        if (slot < 0) { __atomic_add_fetch(&c.full, 1ull, __ATOMIC_RELAXED); e.note_md(kMdNoteFull, md, 0ull); return kMdFull; }
    }
    if (!e.md_prepare(md)) {
        __atomic_store_n(&c.e[slot].md, 0ull, __ATOMIC_RELEASE);
        __atomic_store_n(&c.e[slot].state, (uint32_t)kMdEmpty, __ATOMIC_RELEASE);
        __atomic_add_fetch(&c.prepFail, 1ull, __ATOMIC_RELAXED);
        e.note_md(kMdNotePrepFail, md, 0ull);
        return kMdPrepFail;
    }
    __atomic_store_n(&c.e[slot].md, md, __ATOMIC_RELEASE);                    // the descriptor first, the state last: a perform that sees Ready sees the descriptor
    __atomic_store_n(&c.e[slot].last, __atomic_add_fetch(&c.tick, 1ull, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&c.e[slot].state, (uint32_t)kMdReady, __ATOMIC_RELEASE);
    __atomic_add_fetch(&c.prepared, 1ull, __ATOMIC_RELAXED);
    e.note_md(kMdNoteFill, md, (uint64_t)slot);
    return kMdFilled;
}

// Disarm / withdraw: every Ready entry is retired (no perform can enter), waited for (bounded), then complete()d and release()d exactly once. A perform that does not drain in kMdDrainNs
// is LEAKED (the entry stays Retiring for good, nothing is completed or released). Under the HUNG latch everything is leaked: our complete() would only drop OUR wire count, but the release()
// may be the last reference to a descriptor whose pages a hung GPU can still reach, so nothing is unwired or freed on a hung boot (the project rule for every page set; at most 8 descriptors).
template <class E> void mdc_teardown(E &e) {
    MdCache &c = e.mdc();
    const bool hung = e.hung();
    for (uint32_t i = 0; i < kMdCacheN; ++i) {
        uint32_t exp = kMdReady;
        if (!__atomic_compare_exchange_n(&c.e[i].state, &exp, (uint32_t)kMdRetiring, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
        const uint64_t old = c.e[i].md;
        if (hung) { __atomic_add_fetch(&c.leaked, 1ull, __ATOMIC_RELAXED); e.note_md(kMdNoteLeak, old, 1ull); continue; }
        const uint64_t t0 = e.now_ns();
        while (__atomic_load_n(&c.e[i].busy, __ATOMIC_SEQ_CST) != 0u && e.now_ns() - t0 < kMdDrainNs) { }
        if (__atomic_load_n(&c.e[i].busy, __ATOMIC_SEQ_CST) != 0u) {
            __atomic_add_fetch(&c.drainFail, 1ull, __ATOMIC_RELAXED); __atomic_add_fetch(&c.leaked, 1ull, __ATOMIC_RELAXED);
            e.note_md(kMdNoteDrainFail, old, 0ull);
            continue;
        }
        e.md_unprepare(old);
        __atomic_add_fetch(&c.tornDown, 1ull, __ATOMIC_RELAXED);
        e.note_md(kMdNoteTorn, old, (uint64_t)i);
        __atomic_store_n(&c.e[i].md, 0ull, __ATOMIC_RELEASE);
        __atomic_store_n(&c.e[i].state, (uint32_t)kMdEmpty, __ATOMIC_RELEASE);
    }
}

// The submit hook's work: find the plane-0 descriptor the SAME way perform will, and make sure it is prepared. Only while armed (a disarmed pipe copies nothing: nothing to wire).
template <class E> uint32_t submit_prepare(E &e, uint64_t txn) {
    if (!e.armed()) return kMdSkipDisarmed;
    if (e.scan_active()) { e.note_scan_owned_submit(); return kMdSkipScanOwned; }   // 0.0.617 (K6): a native client owns the plane: nothing is looked up, retained or wired (the bundle presents; the v1 copy is off)
    uint64_t surf = 0, res = 0, sm = 0, smLen = 0, md = 0;
    if (plane0_locate(e, txn, &surf, &res) != 0u || !e.rd64(res + kResSysMem, &sm) || !sysmem_md(e, sm, &smLen, &md)) return kMdNoSource;
    const uint32_t r = mdc_ensure(e, md);
    if (!e.armed()) mdc_teardown(e);                                          // a disarm raced the fill: nothing stays wired on a disarmed pipe
    return r;
}

// 0.0.615 (P1): what the ops table's pci_device answers. The kernel side returns the IOPCIDevice only when this is true.
template <class E> bool pci_admit_flow(E &e) { return pci_admit(e.latch_on(), e.facts()); }

// 0.0.615 (P2): withdraw. The gate byte goes back to 0 FIRST (disarm: mirror, then byte), and only then is anything forgotten: a nub that goes away with the type-4 gate open would let the next
// WindowServer create a pipe that no hook table knows.
template <class E> void withdraw_flow(E &e) {
    const bool was = e.armed();
    e.disarm();
    mdc_teardown(e);                                                       // 0.0.616: every prepared descriptor completed and released (after the mirror is clear, before anything is forgotten)
    reload_shut(e.rw());                                                   // 0.0.619 (R1)
    e.forget_pipes();
    e.note_withdraw(was);
}

struct SourceLog { uint64_t sw, sh, sbpr, base, unk88, smLen; uint32_t bpe, fmt, planes; uint16_t rw, rh; uint64_t rbpr; uint8_t elemW; };

// ---- the v1 present: a CPU copy of plane 0 into the console buffer ---------------------------------------------------------------------------------------------
// Returns the reason (kPfCopied = the whole frame went). Never throws the copy past any bound: every byte written was admitted by bounds_check before the first write.
template <class E> uint32_t perform_inner(E &e, uint64_t txn, uint64_t *bytesOut) {
    *bytesOut = 0ull;
    if (!e.armed()) return kPfDisarmed;                                      // disarmed: complete the transaction (the caller returns success), copy nothing
    if (e.scan_active()) return kPfScanOwned;                                // 0.0.617 (K6): the scanout plane is a native client's: before any source read or descriptor use; the copy resumes by itself when the acquisition ends
    uint64_t surf = 0, res = 0;
    const uint32_t lr = plane0_locate(e, txn, &surf, &res);                  // 0.0.616: the accessor submit shares
    if (lr != 0u) return lr;
    SourceLog s {};
    uint8_t bpe8 = 0; uint16_t bpe16 = 0;
    if (!e.rd64(surf + kSurfW, &s.sw) || !e.rd64(surf + kSurfH, &s.sh) || !e.rd64(surf + kSurfBpr, &s.sbpr) || !e.rd16(surf + kSurfBpe, &bpe16) ||
        !e.rd64(surf + kSurfBase, &s.base) || !e.rd32(surf + kSurfFmt, &s.fmt) || !e.rd32(surf + kSurfPlanes, &s.planes) || !e.rd8(surf + kSurfElemW, &bpe8)) return kPfBadSurf;
    (void)e.rd64(surf + kSurfUnk88, &s.unk88);                                // logged only (no meaning in the 11h.1 table: never decided on)
    s.bpe = bpe16; s.elemW = bpe8;                                           // the element width (+0x72) is logged, not decided on
    uint64_t sm = 0, md = 0;
    if (!e.rd16(res + kResW, &s.rw) || !e.rd16(res + kResH, &s.rh) || !e.rd64(res + kResBpr, &s.rbpr) || !e.rd64(res + kResSysMem, &sm)) return kPfBadRes;
    if (!sysmem_md(e, sm, &s.smLen, &md)) return kPfBadSrc;
    e.note_source(s);
    if (!source_admitted(s.base, s.planes, s.fmt, s.bpe)) return kPfFormat;
    if (!geometry_agrees(s.sw, s.sh, s.sbpr, s.rw, s.rh, s.rbpr)) return kPfMismatch;
    ConsoleInfo c {};
    if (!e.console(&c)) return kPfNoConsole;
    const BoundsIn b { s.sw, s.sh, s.sbpr, s.smLen, c.width, c.height, c.stride, c.bytes };
    const uint32_t bv = bounds_check(b);
    if (bv != kBOk) return perf_reason_from_bounds(bv);
    // 0.0.616: the descriptor may be read ONLY if submit prepared it (a cache hit); a miss refuses the frame and never touches the descriptor. perform never wires.
    const int ci = mdc_acquire(e.mdc(), md);
    if (ci < 0) { __atomic_add_fetch(&e.mdc().misses, 1ull, __ATOMIC_RELAXED); e.note_md(kMdNoteMiss, md, 0ull); return kPfNotPrepared; }
    uint8_t *buf = nullptr; uint32_t cap = 0;
    const uint32_t sa = e.scratch_acquire(&buf, &cap);
    if (sa == 1u) { mdc_release(e.mdc(), ci); return kPfBusy; }
    if (sa != 0u) { mdc_release(e.mdc(), ci); return kPfNoScratch; }
    const uint64_t row = s.sw * 4ull;
    uint32_t reason = kPfCopied;
    if (!buf || cap < row) reason = kPfNoScratch;
    else for (uint64_t y = 0; y < s.sh; ++y) {
        if (!e.src_read(md, y * s.sbpr, buf, row)) { reason = kPfSrcRead; break; }
        if (!e.console_write(y * c.stride, buf, row)) { reason = kPfDstWrite; break; }
        *bytesOut += row;
    }
    e.scratch_release();
    mdc_release(e.mdc(), ci);
    return reason;
}
// ---- 0.0.618 (V1): the vblank timestamps ----------------------------------------------------------------------------------------------------------------------------------
// One OTG0 timing sample (all read-only): the refresh period, the nanoseconds from the sample to the next start of vertical blank, the mach_absolute_time at the sample and the timebase ratio
// (abs * numer / denom = ns).
struct VblSample { uint64_t periodNs, delayNs, nowAbs; uint32_t numer, denom; };
// Writes txn+0x178 = the next vblank (mach_absolute_time units) and txn+0x188 = that + one period, the two words Apple's executeTransaction fills before completion. Every refusal is a named reason and
// writes NOTHING; the two words are only ever written together, after the identity check, from a plan whose period is nonzero.
template <class E> uint32_t vbl_stamp_flow(E &e, uint64_t txn, uint32_t performReason) {
    if (!e.vbl_on()) return kVblOff;
    if (performReason == kPfDisarmed) return kVblDisarmed;                   // a disarmed pipe copies nothing and nobody listens
    if (!kptr_ok(txn)) return kVblBadTxn;
    if (!e.class_derives(txn, kTxnClass)) return kVblNotTxn;
    if (e.hung()) return kVblHung;                                           // the HUNG latch: no register is read
    VblSample s {};
    if (!e.vbl_sample(&s)) return kVblNoTiming;
    const VblPlan p = vbl_plan(s.nowAbs, s.delayNs, s.periodNs, s.numer, s.denom);
    if (!p.ok) return kVblBadMath;
    if (!e.wr64(txn + kTxnVblTime, p.t) || !e.wr64(txn + kTxnVblNext, p.next)) return kVblWriteFail;
    e.note_vbl(kVblWrote, p, s.periodNs, s.delayNs);
    return kVblWrote;
}
template <class E> uint32_t perform_flow(E &e, uint64_t txn) {
    const uint64_t t0 = e.now_ns();
    uint64_t bytes = 0;
    const uint32_t r = perform_inner(e, txn, &bytes);
    const uint32_t v = vbl_stamp_flow(e, txn, r);                            // 0.0.618 (V1): AFTER the copy and BEFORE the return, whatever perform_inner decided (scan-owned or copied): the family completes the transaction when we return
    if (v != kVblWrote) e.note_vbl(v, VblPlan{}, 0ull, 0ull);
    e.note_perform(r, bytes, e.now_ns() - t0);
    return r;
}

// ---- 0.0.617: auto-disarm from outside the hooks (K1 the WindowServer client's close, K2 the HUNG latch) --------------------------------------------------------------------
// While armed: the mirror and the gate byte go first (disarm), then the prepared-descriptor cache is torn down, then the cause is recorded and logged. Not armed: nothing at all (no log, no write).
// Called with NO kernel lock held by us; the teardown's bounded spin (kMdDrainNs per entry) waits only on performs, which take no lock (atomics + the BAR0 write), so it cannot deadlock with gCliLock.
// Under the HUNG latch mdc_teardown leaks instead of draining, so K2 never spins.
template <class E> bool auto_disarm_flow(E &e, uint32_t cause) {
    if (!e.armed()) return false;
    e.disarm();
    mdc_teardown(e);
    reload_shut(e.rw());                                                     // 0.0.619 (R1): a disarmed pipe has no restart to tolerate
    e.note_autodisarm(cause);
    return true;
}
template <class E> bool ws_client_closed_flow(E &e, bool adminClient) {   // K1
    if (!ws_close_disarms(e.latch_on(), adminClient)) return false;
    if (e.armed() && reload_tolerate_close(e)) return false;                 // 0.0.619 (R1): an announced operator restart (`pipereload`): the pipe stays armed; the window decides, K2 (HUNG) is never overridden
    return auto_disarm_flow(e, kAdWsClose);
}
template <class E> bool hung_latched_flow(E &e) {                           // K2
    if (!e.latch_on()) return false;
    return auto_disarm_flow(e, kAdHung);
}

// ---- slot 267: the stand-in memory object and the active flag -----------------------------------------------------------------------------------------------------
template <class E> int init_fb_flow(E &e, uint64_t pipe, uint64_t res, uint64_t *ret) {
    if (!e.known_pipe(pipe)) return 0;                                       // not a pipe we were told is ours: the family's own slot runs
    uint8_t *obj = e.standin(pipe);
    if (!obj) return 0;
    standin_fill(obj);                                                       // the object is well formed BEFORE the family can see it ...
    if (!e.wr8(pipe + kPipeActive, 1u)) return 0;                            // ... and the pipe is marked active only after it (the family sets this only when prepare() is true, which it is not)
    e.note_init_fb(pipe, res);
    *ret = (uint64_t)(uintptr_t)obj;
    if (e.armed() && !reload_on_init_fb(e) && guard_on_init_fb(e.guard(), e.now_ns())) { e.disarm(); mdc_teardown(e); reload_shut(e.rw()); e.note_autodisarm(kAdGuard); }   // 0.0.619 (R1): inside an operator restart window the call is not counted   // 0.0.615 (G1), 0.0.617 (K3): three starts inside 120 s, presented or not
    return 1;
}

// ---- the dispatcher the ops table's disp_hook runs ---------------------------------------------------------------------------------------------------------------
// 1 = handled (*ret is the slot's return value), 0 = not handled (the aux kext calls the FAMILY's own slot). A pipe we do not know is never handled.
template <class E> int hook_dispatch(E &e, uint32_t cls, uint32_t slot, uint64_t self, const uint64_t *args, uint32_t nargs, uint64_t *ret) {
    if (cls != N48_VC_DISPLAYPIPE || !args || !ret || nargs > 6u) return 0;
    if (!e.latch_on()) return 0;
    switch (slot) {
    case 267u:
        if (nargs != 2u) return 0;
        return init_fb_flow(e, self, args[1], ret);
    case 277u:                                                               // performTransaction
        if (nargs != 1u || !e.known_pipe(self)) return 0;
        guard_on_perform(e.guard());                                         // 0.0.615 (G1): a perform has reached the pipe since the arm
        (void)perform_flow(e, args[0]);
        *ret = 0ull;                                                         // always success: the ring slot retires whether or not a frame was copied
        return 1;
    case 278u:                                                               // isTransactionComplete
        if (nargs != 1u || !e.known_pipe(self)) return 0;
        *ret = iscomplete_result();
        return 1;
    case 279u: {                                                             // submitTransaction
        if (nargs != 1u || !e.known_pipe(self)) return 0;
        ival_note(e.ivl(), e.now_ns());                                      // 0.0.617 (K6): the transaction interval (every submit, whatever happens to it)
        uint32_t st = 0;
        const bool known = kptr_ok(args[0]) && e.rd32(args[0] + kTxnStatus, &st);
        *ret = submit_result(known, st);
        if (*ret == kWillPerform) (void)submit_prepare(e, args[0]);          // 0.0.616: wire the plane-0 descriptor HERE (thread context), never in perform
        return 1;
    }
    default: return 0;
    }
}

// ---- adopt --------------------------------------------------------------------------------------------------------------------------------------------------------------
// Order is the contract: the accelerator and its controls, the read-only precheck, the FACT BITS, the refusal if they are not on, the probe request, the read-only verification,
// the capabilities property, and only then the record. Nothing reaches requestProbe without the resource facts on (11h.1 correction 1: a pipe created without them panics).
template <class E> uint32_t adopt_inner(E &e) {
    if (!e.find_accel()) return kNoAccel;
    if (!e.accel_layout_ok()) return kAccelLayout;
    const PipeProbe before = e.probe_pipe();
    const uint32_t pre = adopt_precheck(before);
    if (pre != kOk && pre != kAlready) return pre;
    e.facts_add(kNeedFacts);
    if (!facts_ok(e.facts())) return kFactsOff;                              // refuse to run otherwise
    if (pre == kOk && !e.request_probe()) return kProbeFailed;
    const PipeProbe after = e.probe_pipe();
    const uint32_t v = pipe_verdict(after);
    if (v != kOk) return v;
    if (!e.publish_caps()) return kCapsFailed;
    e.record_adopted(after);
    return pre == kAlready ? kAlready : kOk;
}
template <class E> uint32_t adopt_flow(E &e) {
    if (!e.latch_on()) return kOff;
    if (!e.adopt_begin()) return kBusy;
    const uint32_t st = adopt_inner(e);
    if (st == kNullPipe) e.note_null_pipe();                                 // 0.0.615 (P3)
    e.adopt_end(st);
    return st;
}

// ---- arm -----------------------------------------------------------------------------------------------------------------------------------------------------------------
template <class E> uint32_t arm_flow(E &e, uint64_t arg) {
    ArmIn a { false, false, false, false };
    if (arg == 1ull) {                                                       // the probes run only for an arm: a disarm never depends on them
        a.latchOn = e.latch_on();
        a.factsOk = facts_ok(e.facts());
        if (a.latchOn && a.factsOk) { a.pipeOk = pipe_ok(e.probe_pipe()); if (a.pipeOk) a.accelLayoutOk = e.accel_layout_ok(); }
    }
    const uint32_t d = arm_decide(arg, a);
    if (d != kOk) return d;
    if (arg == 0ull) { e.disarm(); mdc_teardown(e); reload_shut(e.rw()); return kOk; }                             // always allowed
    guard_reset(e.guard());                                                  // 0.0.615 (G1): the counter starts from zero at every arm (before the byte opens)
    ival_reset(e.ivl());                                                     // 0.0.617 (K6): the interval statistics start with the arm
    const bool ok = e.arm_write(1u);
    if (ok) e.clear_autodisarm();                                            // 0.0.617 (K4): an explicit arm 1 is the only thing that re-arms, and it clears the "auto-disarmed" mark
    return ok ? (uint32_t)kOk : (uint32_t)kWriteFailed;
}

// ---- stamps (READ-ONLY, SUSPECTED layout) ------------------------------------------------------------------------------------------------------------------------------
struct StampsOut { uint64_t em; uint32_t count, clamped, completed, submitted; bool hazard; };
template <class E> uint32_t stamps_flow(E &e, StampsOut *o) {
    const uint64_t acc = e.accel_addr();
    uint64_t em = 0;
    if (!acc) return kNoAccel;
    if (!e.rd64(acc + kAccelEm, &em) || !kptr_ok(em) || !e.class_is(em, "Navi48EventMachine")) return kNoEventMachine;
    o->em = em;
    if (!e.rd32(em + kEmCount, &o->count) || !e.rd32(em + kEmDone, &o->completed) || !e.rd32(em + kEmSub, &o->submitted)) return kNoEventMachine;
    o->clamped = stamp_walk_bound(o->count);
    o->hazard = stamp_hazard(o->completed, o->submitted);
    return kOk;
}

// ---- slot 62 (type 0xC0 surfaces) and the shortcut -----------------------------------------------------------------------------------------------------------------------
// Where slot 62's outputs LAND (11h.1 F3: res+0xc8 allocation size, res+0xb8 bytes per row) is not stated as "the pointers ARE those fields" (they may be the caller's locals, copied after the call), so the
// output pointers are validated as the existing generic handler validates them (kernel half, 8-aligned, distinct), never by identity with these offsets.
constexpr uint32_t kResOutBpr = 0xb8u, kResOutAlloc = 0xc8u;
template <class E> int res62_flow(E &e, uint64_t res, const uint64_t *args, uint32_t nargs, uint64_t *ret, bool shortcutSwitch) {
    if (!e.latch_on() || !args || !ret || nargs != 2u || !kptr_ok(res)) return 0;
    if ((e.facts() & N48_FACT_RESOURCE) == 0u) return 0;
    // The output pointers: kernel half, 8-aligned, distinct (the same validation n48metal::vhook_handle applies to this slot); anything else is not written.
    if (!kptr_ok(args[0]) || !kptr_ok(args[1]) || args[0] == args[1] || (args[0] & 7ull) != 0ull || (args[1] & 7ull) != 0ull) return 0;
    Res62In in {};
    in.switchOn = shortcutSwitch;
    uint64_t sm = 0, dc = 0, surf = 0;
    uint8_t fl = 0;
    if (e.rd64(res + kResSysMem, &sm) && kptr_ok(sm)) {
        in.smKnown = e.rd64(sm + kSmLen, &in.smLen);
        in.flagsKnown = e.rd8(sm + kSmFlags, &fl);
        in.bit4 = (fl & kSmBit4) != 0u;
    }
    if (e.rd64(res + kResDc, &dc) && kptr_ok(dc) && e.rd64(dc + kDcSurf, &surf) && kptr_ok(surf)) in.surfKnown = e.rd64(surf + kSurfBpr, &in.surfBpr);
    const Res62Plan p = res62_plan(in);
    e.note_res62(p, in.smKnown, in.surfKnown, in.flagsKnown, in.bit4);      // a log line either way
    if (p.shortcut) {                                                       // independent of the outputs: it needs only the SysMemory's flags byte
        uint8_t f = 0; uint64_t smp = 0;
        if (e.rd8(res + kResShortcutFlag, &f) && e.rd64(res + kResSysMem, &smp)) {
            (void)e.wr64(res + kResPrepared, smp);                           // the pointer first, the flag last: a reader that sees the flag sees the pointer
            (void)e.wr8(res + kResShortcutFlag, (uint8_t)(f | kResShortcutBit));
        }
    }
    if (!p.handled) return 0;
    if (!e.wr64(args[0], p.allocSize) || !e.wr64(args[1], p.bytesPerRow)) return 0;
    *ret = 0ull;                                                            // paravirt's own slot returns 0; the hook's return 1 says "handled"
    return 1;
}

} // namespace n48disp
