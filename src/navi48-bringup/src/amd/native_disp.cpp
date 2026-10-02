//
//  native_disp.cpp - the kernel side of the display pipe (kext 0.0.613, #11 step 11h.2). See native_disp.h. Default OFF: boot-arg navi48-metal-disp=1 (latched once).
//
//  What this file WRITES, with the latch ON and only on the verbs / hooks below: (1) adopt: the fact bits (a mask in this file), the IOAccelDisplayPipeCapabilities property on the
//  Navi48Accelerator, and one requestProbe(1) on it (the accelerator's own "probe the framebuffers" request); (2) arm: ONE byte of the accelerator object, the IOAccelConfig
//  +0x47 (accel+0xccf), read only by the type-4 user-client gate; (3) the pipe hooks: pipe+0x298 and the stand-in object's fields at slot 267, resource fields at slot 62,
//  and the v1 present: rows of plane 0 into the console buffer through the BAR0 aperture (Navi48Bringup::vramWrite), every byte pre-checked by bounds_check.
//  It touches no GPU register and no page table. The decisions are n48disp:: (host-tested with planted breaks); this file supplies the kernel primitives to the tested flows.
//
#include <string.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>

#include "native_disp.h"
#include "native_disp_flow.h"
#include "../dcn/navi48_dcn.hpp"   // 0.0.617 (K6): n48dcn::scanActive()
#include "native_s1c.h"      // n1c_hung(): the HUNG latch (0.0.616: the prepared-descriptor cache leaks instead of completing under it)
#include "amdgpu_log.h"
#include "Navi48MetalNub.hpp"

#define DLOG(fmt, ...) AMDGPU_LOG("disp", fmt, ##__VA_ARGS__)

IOService *navi48_bringup_pci(void);   // Navi48Bringup.cpp: our IOPCIDevice
bool navi48_bar0_write_combined(void); // Navi48Bringup.cpp (0.0.615, W1): the BAR0 kernel mapping was made write-combined

namespace {

// ---- the latch ----------------------------------------------------------------------------------------------------------------------------------------
volatile UInt32 gLatch = n48disp::kLatchUnset;

// ---- the state (atomics only: the hooks run on WindowServer's transaction path and take no lock) ------------------------------------------------------------------
struct State {
    volatile uint32_t factBits;                     // adopt's OR onto the factory mask
    uint64_t          pipes[n48disp::kMaxPipes];    // the pointers the aux kext's newDisplayPipe trace named as its own (append only per publish)
    volatile uint32_t nPipes;
    volatile uint32_t adoptBusy, adopted, armed, capsDone, shortcutOff;   // shortcutOff 0 = the shortcut switch is ON (the default)
    IOService        *accel;                        // retained from the first successful find until n48disp_on_withdraw
    uint64_t          adoptedPipe;
    uint8_t          *scratch;                      // one row of scratch, allocated at the first adopt, never freed (16 KiB once per boot)
    volatile uint32_t scratchBusy;
    // counters (statistics only: a torn update is harmless)
    uint64_t hookCalls[4], hookHandled[4];          // slots 267 / 277 / 278 / 279
    uint64_t submitPass, submitWill, initFb, standinRelease;
    uint64_t reasons[n48disp::kPfCount], bytes;
    n48disp::Dur dur;                               // the duration of a COPIED frame (refusals are not timed)
    uint64_t res62Calls, res62Handled, res62Unhandled, res62Shortcut, res62Refused;
    uint64_t pipeTraces, pipeTracesOurs, pipeTablefull, dmStarts, dmSubst, lastDmProvider;
    uint64_t arms, disarms, adoptTries, sourceLogged, autoDisarms, nullPipeAdopts;
    n48disp::Ival ivl;                              // 0.0.617 (K6): the interval between consecutive submits
    uint64_t submitScanSkipped;                     // 0.0.617 (K6): submits that did not wire because a native client owns the scanout plane
    volatile uint32_t vblOff;                       // 0.0.618 (V2): 0 = the vblank-timestamp writes are ON (the default with the latch ON), 1 = OFF (0.0.617 behaviour); `pipevbl 0|1`
    uint64_t          vbl[n48disp::kVblCount];      // 0.0.618: per-reason counts of the stamp attempts (index 0 = written)
    uint64_t          vblLastT, vblLastNext, vblLastPeriodNs, vblLastDelayNs, vblLastPeriodAbs;   // the last plan written (mach_absolute_time units for T / Next / PeriodAbs)
    volatile uint32_t autoCause;                    // 0.0.617 (K4): n48disp::AutoCause of the last auto-disarm, kept until the next explicit `pipearm 1`
    n48disp::ReloadWin reload;                      // 0.0.619 (R1): the operator restart window (`pipereload`); all zero = closed
    n48disp::CrashGuard guard;                      // 0.0.615 (G1)
    n48disp::MdCache mdc;                           // 0.0.616: the descriptors submit prepared; perform reads ONLY these (zero = all entries Empty)
    uint32_t lastAdopt;
};
State gD { };

bool rdy_shortcut() { return __atomic_load_n(&gD.shortcutOff, __ATOMIC_ACQUIRE) == 0u; }

uint64_t now_ns() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns; }

// ---- the stand-in memory object (11h.1 F1): static storage, one per pipe slot; slot 5 (release) and slot 39 (map) answer, every other slot returns 0 ------------------------------------------
alignas(16) uint8_t gStandIn[n48disp::kMaxPipes][n48disp::kObjSize];
void *gStandInVt[n48disp::kObjVtSlots];
uint64_t standin_stub(void *) { return 0; }                      // slot 39 (map) must return 0: map() and prepare() then return false, safely
void standin_release(void *) { __atomic_add_fetch(&gD.standinRelease, 1ull, __ATOMIC_RELAXED); }   // the family calls release on destroy: we own the storage, nothing is freed
void standin_init_vt() {
    if (gStandInVt[0]) return;
    for (uint32_t i = 0; i < n48disp::kObjVtSlots; ++i) gStandInVt[i] = reinterpret_cast<void *>(&standin_stub);
    gStandInVt[n48disp::kObjSlotRelease] = reinterpret_cast<void *>(&standin_release);
    gStandInVt[0] = reinterpret_cast<void *>(&standin_stub);     // published last: its non-NULL-ness is the "initialised" mark
}

// ---- the kernel environment the tested flows run against ------------------------------------------------------------------------------------------------------------------
struct KernelEnv {
    // -- memory: a kernel-pointer check, then the raw access. Every address comes from a family-owned field (offsets: amd/native_disp_pure.h, each with its source). --
    template <class T> bool rdT(uint64_t a, T *o) { if (!n48disp::kptr_ok(a)) return false; *o = *reinterpret_cast<volatile const T *>(static_cast<uintptr_t>(a)); return true; }
    template <class T> bool wrT(uint64_t a, T v) { if (!n48disp::kptr_ok(a)) return false; *reinterpret_cast<volatile T *>(static_cast<uintptr_t>(a)) = v; return true; }
    bool rd8(uint64_t a, uint8_t *o) { return rdT(a, o); }
    bool rd16(uint64_t a, uint16_t *o) { return rdT(a, o); }
    bool rd32(uint64_t a, uint32_t *o) { return rdT(a, o); }
    bool rd64(uint64_t a, uint64_t *o) { return rdT(a, o); }
    bool wr8(uint64_t a, uint8_t v) { return wrT(a, v); }
    bool wr64(uint64_t a, uint64_t v) { return wrT(a, v); }

    bool latch_on() { return n48disp_latched_on(); }
    uint32_t facts() { return n48metal_factory_mask_now(); }
    void facts_add(uint32_t bits) { __atomic_or_fetch(&gD.factBits, bits & n48disp::kNeedFacts, __ATOMIC_ACQ_REL); }

    // -- the pipes the aux kext told us are its own --
    bool known_pipe(uint64_t p) { return n48disp::pipe_table_find(gD.pipes, cap_n(), p) >= 0; }
    static uint32_t cap_n() { const uint32_t n = __atomic_load_n(&gD.nPipes, __ATOMIC_ACQUIRE); return n > n48disp::kMaxPipes ? n48disp::kMaxPipes : n; }
    bool armed() { return __atomic_load_n(&gD.armed, __ATOMIC_ACQUIRE) != 0u; }
    n48disp::CrashGuard &guard() { return gD.guard; }
    void note_autodisarm(uint32_t cause) {                                // 0.0.617: the cause is kept (K4) and named in the log line
        __atomic_add_fetch(&gD.autoDisarms, 1ull, __ATOMIC_RELAXED);
        __atomic_store_n(&gD.autoCause, cause < n48disp::kAdCount ? cause : (uint32_t)n48disp::kAdNone, __ATOMIC_RELEASE);
        DLOG("auto-disarm: %s; the gate byte is written back to 0 and the descriptor cache torn down; re-arm needs an explicit `pipearm 1`", n48disp::auto_cause_name(cause));
    }
    // -- 0.0.619: the operator restart window (flows: native_disp_flow.h reload_*) --
    n48disp::ReloadWin &rw() { return gD.reload; }
    void note_reload(uint32_t ev) {
        if (ev == n48disp::kRwOpened) DLOG("operator restart window: opened (%llu s); the next WindowServer client close is tolerated and slot-267 calls are not counted until the new client's first slot 267 or expiry", (unsigned long long)(n48disp::kReloadWindowNs / 1000000000ull));
        else if (ev == n48disp::kRwTolerated) DLOG("operator restart window: client close tolerated (the pipe stays armed)");
        else if (ev == n48disp::kRwClosed267) DLOG("operator restart window: closed (the new client's first slot 267 seen, the pipe is still armed)");
        else DLOG("operator restart window: expired (%llu s)", (unsigned long long)(n48disp::kReloadWindowNs / 1000000000ull));
    }
    // -- 0.0.618: the vblank timestamps (flow: native_disp_flow.h vbl_stamp_flow) --
    bool vbl_on() { return __atomic_load_n(&gD.vblOff, __ATOMIC_ACQUIRE) == 0u; }
    bool class_derives(uint64_t obj, const char *name) {                  // the object's class, or any superclass, is `name`
        uint64_t vt = 0;
        if (!n48disp::kptr_ok(obj) || !rd64(obj, &vt) || !n48disp::kptr_ok(vt)) return false;
        const OSMetaClass *mc = reinterpret_cast<const OSObject *>(static_cast<uintptr_t>(obj))->getMetaClass();
        for (uint32_t depth = 0; mc && depth < 12u; ++depth, mc = mc->getSuperClass()) {
            const char *c = mc->getClassName();
            if (c && strcmp(c, name) == 0) return true;
        }
        return false;
    }
    bool vbl_sample(n48disp::VblSample *s) {
        // The timebase ratio (ns = abs * numer / denom) from the kernel's own conversion of one second: numer = 1e9 ns, denom = the ticks in one second (x86: 1e9 / 1e9). clock_timebase_info is not
        // used because its export from the mach KPI cannot be confirmed from here; nanoseconds_to_absolutetime is already used by this kext.
        uint64_t perSec = 0;
        nanoseconds_to_absolutetime(1000000000ull, &perSec);
        if (perSec == 0ull || perSec > 0xffffffffull) return false;
        if (!n48dcn::vblSample(&s->periodNs, &s->delayNs, &s->nowAbs)) return false;
        s->numer = 1000000000u; s->denom = (uint32_t)perSec;
        return true;
    }
    void note_vbl(uint32_t reason, const n48disp::VblPlan &p, uint64_t periodNs, uint64_t delayNs) {
        const uint32_t r = reason < n48disp::kVblCount ? reason : n48disp::kVblCount - 1u;
        const uint64_t n = __atomic_add_fetch(&gD.vbl[r], 1ull, __ATOMIC_RELAXED);
        if (r == n48disp::kVblWrote) {
            gD.vblLastT = p.t; gD.vblLastNext = p.next; gD.vblLastPeriodAbs = p.periodAbs; gD.vblLastPeriodNs = periodNs; gD.vblLastDelayNs = delayNs;
        }
        if (n <= 3u || (r == n48disp::kVblWrote && (n & 0x3ffu) == 0u))
            DLOG("vbl stamps: %s (reason %u) #%llu; t_vbl %llu next %llu (abs), period %llu ns, to next vblank %llu ns", n48disp::vbl_name(r), (unsigned)r, (unsigned long long)n,
                 (unsigned long long)p.t, (unsigned long long)p.next, (unsigned long long)periodNs, (unsigned long long)delayNs);
    }
    bool scan_active() { return n48dcn::scanActive(); }                  // 0.0.617 (K6): one atomic load
    void note_scan_owned_submit() { __atomic_add_fetch(&gD.submitScanSkipped, 1ull, __ATOMIC_RELAXED); }
    n48disp::Ival &ivl() { return gD.ivl; }
    void clear_autodisarm() { __atomic_store_n(&gD.autoCause, 0u, __ATOMIC_RELEASE); }
    void note_null_pipe() {
        gD.nullPipeAdopts++;
        DLOG("adopt: NULL pipe stored by the family; reboot before any WindowServer restart");
    }
    void note_withdraw(bool wasArmed) { DLOG("nub withdrawn: %s, pipes forgotten, arm mirror cleared", wasArmed ? "the gate was ARMED and has been disarmed first" : "not armed"); }
    void forget_pipes() {
        __atomic_store_n(&gD.armed, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&gD.adopted, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&gD.capsDone, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&gD.nPipes, 0u, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < n48disp::kMaxPipes; ++i) __atomic_store_n(&gD.pipes[i], 0ull, __ATOMIC_RELEASE);
        gD.adoptedPipe = 0;
    }

    // -- perform --
    bool console(n48disp::ConsoleInfo *c) {
        uint64_t off = 0, len = 0; uint32_t w = 0, h = 0, row = 0;
        if (!navi48_console_region(&off, &len, &w, &h, &row)) return false;
        c->width = w; c->height = h; c->stride = row; c->bytes = len;
        return true;
    }
    uint32_t scratch_acquire(uint8_t **buf, uint32_t *cap) {
        if (!gD.scratch) return 2u;
        uint32_t exp = 0u;
        if (!__atomic_compare_exchange_n(&gD.scratchBusy, &exp, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1u;
        *buf = gD.scratch; *cap = n48disp::kScratchBytes;
        return 0u;
    }
    void scratch_release() { __atomic_store_n(&gD.scratchBusy, 0u, __ATOMIC_RELEASE); }
    bool src_read(uint64_t md, uint64_t off, uint8_t *dst, uint64_t len) {
        if (!n48disp::mdc_held(gD.mdc, md)) return false;                  // 0.0.616: never read a descriptor that is not a cache hit held by this perform (readBytes on an unwired IOGMD panics: m11h4-1)
        IOMemoryDescriptor *d = reinterpret_cast<IOMemoryDescriptor *>(static_cast<uintptr_t>(md));
        if (off > d->getLength() || len > d->getLength() - off) return false;
        return d->readBytes(static_cast<IOByteCount>(off), dst, static_cast<IOByteCount>(len)) == static_cast<IOByteCount>(len);
    }
    bool console_write(uint64_t off, const uint8_t *src, uint64_t len) { return navi48_console_write(off, src, static_cast<size_t>(len)); }
    uint64_t now_ns() { return ::now_ns(); }
    void note_perform(uint32_t r, uint64_t bytes, uint64_t ns) {
        __atomic_add_fetch(&gD.reasons[r < n48disp::kPfCount ? r : n48disp::kPfCount - 1u], 1ull, __ATOMIC_RELAXED);
        if (r == n48disp::kPfCopied) { __atomic_add_fetch(&gD.bytes, bytes, __ATOMIC_RELAXED); n48disp::dur_note(gD.dur, ns); }
        const uint64_t n = __atomic_load_n(&gD.reasons[r < n48disp::kPfCount ? r : 0u], __ATOMIC_RELAXED);
        if (n <= 3u || (r == n48disp::kPfCopied && (n & 0x3ffu) == 0u))
            DLOG("perform: %s (reason %u) #%llu, %llu bytes, %llu ns", n48disp::perf_name(r), (unsigned)r, (unsigned long long)n, (unsigned long long)bytes, (unsigned long long)ns);
    }
    void note_source(const n48disp::SourceLog &s) {
        if (__atomic_add_fetch(&gD.sourceLogged, 1ull, __ATOMIC_RELAXED) > 6ull) return;
        DLOG("perform source: IOSurface %llux%llu stride %llu elem %u/%u base %llu fmt %#x planes %u +0x88 %#llx; resource %ux%u stride %llu; SysMemory length %llu",
             (unsigned long long)s.sw, (unsigned long long)s.sh, (unsigned long long)s.sbpr, (unsigned)s.bpe, (unsigned)s.elemW, (unsigned long long)s.base, (unsigned)s.fmt, (unsigned)s.planes,
             (unsigned long long)s.unk88, (unsigned)s.rw, (unsigned)s.rh, (unsigned long long)s.rbpr, (unsigned long long)s.smLen);
    }

    // -- 0.0.616: the prepared-descriptor cache (flows: native_disp_flow.h mdc_ensure / mdc_teardown / submit_prepare) --
    n48disp::MdCache &mdc() { return gD.mdc; }
    bool hung() { return amdgpu::n1c_hung(); }
    // retain + prepare(kIODirectionOut) on a descriptor read from the family's SysMemory object. Thread context only (submit): prepare() may block. The object must look live (kernel pointer, kernel
    // vtable) and be an IOMemoryDescriptor by its metaclass before it is retained; a failed prepare gives the reference back.
    bool md_prepare(uint64_t a) {
        uint64_t vt = 0;
        if (!n48disp::kptr_ok(a) || !rd64(a, &vt) || !n48disp::kptr_ok(vt)) return false;
        IOMemoryDescriptor *d = OSDynamicCast(IOMemoryDescriptor, reinterpret_cast<OSObject *>(static_cast<uintptr_t>(a)));
        if (!d) return false;
        d->retain();
        if (d->prepare(kIODirectionOut) != kIOReturnSuccess) { d->release(); return false; }
        return true;
    }
    void md_unprepare(uint64_t a) {                                      // only ever called for an entry md_prepare succeeded on
        IOMemoryDescriptor *d = reinterpret_cast<IOMemoryDescriptor *>(static_cast<uintptr_t>(a));
        d->complete(kIODirectionOut);
        d->release();
    }
    void note_md(uint32_t ev, uint64_t md, uint64_t detail) {
        static const char *const kName[9] = { "prepared", "PREPARE FAILED", "evicted (complete + release)", "cache FULL (every entry busy), not prepared", "torn down (complete + release)",
                                              "LEAKED (HUNG latch)", "LEAKED (a perform did not drain)", "refused: HUNG latch", "perform MISS (not prepared by submit)" };
        const uint64_t n = ev == n48disp::kMdNoteFill ? gD.mdc.prepared : ev == n48disp::kMdNoteMiss ? gD.mdc.misses : ev == n48disp::kMdNotePrepFail ? gD.mdc.prepFail : 0ull;
        if ((ev == n48disp::kMdNoteFill || ev == n48disp::kMdNoteMiss || ev == n48disp::kMdNotePrepFail) && n > 8u && (n & 0xffu) != 0u) return;     // the first 8, then every 256th
        DLOG("md cache: descriptor %#llx %s (detail %llu); prepared %llu hits %llu misses %llu evicted %llu torn down %llu leaked %llu", (unsigned long long)md, ev < 9u ? kName[ev] : "?",
             (unsigned long long)detail, (unsigned long long)gD.mdc.prepared, (unsigned long long)gD.mdc.hits, (unsigned long long)gD.mdc.misses, (unsigned long long)gD.mdc.evicted,
             (unsigned long long)gD.mdc.tornDown, (unsigned long long)gD.mdc.leaked);
    }

    // -- slot 267 --
    uint8_t *standin(uint64_t pipe) {
        const int i = n48disp::pipe_table_find(gD.pipes, cap_n(), pipe);
        if (i < 0) return nullptr;
        standin_init_vt();
        uint8_t *o = gStandIn[i];
        *reinterpret_cast<void ***>(o) = gStandInVt;                      // the vptr at +0 (the rest is filled by the flow)
        return o;
    }
    void note_init_fb(uint64_t pipe, uint64_t res) {
        const uint64_t n = __atomic_add_fetch(&gD.initFb, 1ull, __ATOMIC_RELAXED);
        if (n <= 8u) DLOG("slot 267 on pipe %#llx resource %#llx -> stand-in object; the pipe is marked active (call %llu)", (unsigned long long)pipe, (unsigned long long)res, (unsigned long long)n);
    }

    // -- adopt --
    bool adopt_begin() { uint32_t exp = 0u; return __atomic_compare_exchange_n(&gD.adoptBusy, &exp, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
    void adopt_end(uint32_t st) {
        gD.lastAdopt = st; gD.adoptTries++;
        DLOG("pipe adopt: %s (%u)", n48disp::status_name(st), (unsigned)st);
        __atomic_store_n(&gD.adoptBusy, 0u, __ATOMIC_RELEASE);
    }
    // The accelerator: the Navi48Accelerator the aux kext published under OUR nub. Retained once (until withdraw); a later call only re-checks its provider.
    bool find_accel() {
        IOService *nub = Navi48MetalNub::published();
        if (!nub) return false;
        if (gD.accel) return gD.accel->getProvider() == nub;
        OSDictionary *m = IOService::serviceMatching("Navi48Accelerator");
        if (!m) return false;
        IOService *s = IOService::waitForMatchingService(m, 0);          // non-blocking lookup (the same call fbname uses)
        m->release();
        if (!s) return false;
        if (s->getProvider() != nub) { s->release(); return false; }
        gD.accel = s;                                                    // keeps the reference from the lookup
        return true;
    }
    uint64_t accel_addr() { return gD.accel ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(gD.accel)) : 0ull; }
    const char *class_name(uint64_t p) {
        uint64_t vt = 0;
        if (!n48disp::kptr_ok(p) || !rd64(p, &vt) || !n48disp::kptr_ok(vt)) return nullptr;     // a live object: a kernel pointer whose first word is a kernel pointer (its vtable)
        const OSMetaClass *mc = reinterpret_cast<const OSObject *>(static_cast<uintptr_t>(p))->getMetaClass();
        return mc ? mc->getClassName() : nullptr;
    }
    bool class_is(uint64_t obj, const char *name) { const char *c = class_name(obj); return c && strcmp(c, name) == 0; }
    // The accelerator's positive controls: the class (the registry lookup already matched it), the provider word (accel+0x368) equals OUR nub, and the IOAccelConfig at accel+0xc88 holds the
    // two values our own populate hook stored (n48disp::accel_layout_ok), which also proves accel+0xccf is the +0x47 byte.
    bool accel_layout_ok() {
        if (!find_accel()) return false;
        const uint64_t acc = accel_addr(), nub = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Navi48MetalNub::published()));
        uint64_t prov = 0; uint32_t f0 = 0, f4 = 0; uint8_t gate = 0xff;
        const bool ok = class_is(acc, "Navi48Accelerator") && rd64(acc + n48disp::kAccelProvider, &prov) && prov == nub && rd32(acc + n48disp::kAccelCfgF0, &f0) &&
                        rd32(acc + n48disp::kAccelCfgF4, &f4) && rd8(acc + n48disp::kAccelPipeGate, &gate);
        if (!ok || !n48disp::accel_layout_ok(f0, f4, gate)) {
            DLOG("accelerator controls FAILED: class ok %d, provider word %#llx (nub %#llx), cfg+0x08 %#x, cfg+0x28 low %#x, gate byte %u", (int)ok, (unsigned long long)prov, (unsigned long long)nub, (unsigned)f0, (unsigned)f4, (unsigned)gate);
            return false;
        }
        return true;
    }
    IOService *find_fb() {                                               // RDNA4FB under either name (the renamed class keeps matching by the registration name)
        static const char *const kNames[2] = { "RDNA4FB", "AMDRDNA4FB" };
        for (unsigned i = 0; i < 2; ++i) {
            OSDictionary *m = IOService::serviceMatching(kNames[i]);
            if (!m) continue;
            IOService *s = IOService::waitForMatchingService(m, 0);
            m->release();
            if (s) return s;
        }
        return nullptr;
    }
    n48disp::PipeProbe probe_pipe() {
        n48disp::PipeProbe p {};
        if (!find_accel()) return p;
        const uint64_t acc = accel_addr();
        uint64_t prov = 0;
        p.accelOk = class_is(acc, "Navi48Accelerator") && rd64(acc + n48disp::kAccelProvider, &prov) && prov == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Navi48MetalNub::published()));
        if (!p.accelOk) return p;
        uint64_t dm = 0;
        if (!rd64(acc + n48disp::kAccelDm, &dm) || !class_is(dm, "Navi48DisplayMachine")) return p;
        p.dmReadable = true;
        if (!rd32(dm + n48disp::kDmCount, &p.count)) p.count = 0;
        uint64_t pipe0 = 0;
        if (p.count >= 1u && rd64(dm + n48disp::kDmPipes, &pipe0) && pipe0 == 0ull) p.nullPipe = true;     // 0.0.615 (P3): counted but NULL
        if (p.count >= 1u && rd64(dm + n48disp::kDmPipes, &pipe0) && n48disp::kptr_ok(pipe0)) {
            p.havePipe = true;
            p.classOurs = class_is(pipe0, "Navi48DisplayPipe");
            p.traced = known_pipe(pipe0);
            uint64_t a = 0, d = 0, f = 0;
            const bool ok = rd64(pipe0 + n48disp::kPipeAccel, &a) && rd64(pipe0 + n48disp::kPipeDm, &d) && rd64(pipe0 + n48disp::kPipeFb, &f);
            IOService *fb = find_fb();
            if (fb) {
                const OSMetaClass *mc = fb->getMetaClass();
                const char *cn = mc ? mc->getClassName() : nullptr;
                p.fbOurs = cn && (strcmp(cn, "RDNA4FB") == 0 || strcmp(cn, "AMDRDNA4FB") == 0);
                p.backFb = ok && f == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fb));
                fb->release();
            }
            p.backAccel = ok && a == acc;
            p.backDm = ok && d == dm;
        }
        return p;
    }
    bool request_probe() {
        const IOReturn rc = gD.accel->requestProbe(1);                   // the family's own request: its display machine walks the framebuffers (our start override gives it the PCI device)
        DLOG("pipe adopt: requestProbe(1) -> %#x", (unsigned)rc);
        return rc == kIOReturnSuccess;
    }
    bool publish_caps() {
        OSDictionary *d = OSDictionary::withCapacity(2);
        if (!d) return false;
        bool ok = d->setObject("DisplayPipeSupported", kOSBooleanTrue) && d->setObject("TransactionsSupported", kOSBooleanTrue);
        ok = ok && gD.accel->setProperty("IOAccelDisplayPipeCapabilities", d);
        d->release();
        if (ok) { OSObject *back = gD.accel->copyProperty("IOAccelDisplayPipeCapabilities"); ok = back != nullptr; if (back) back->release(); }
        if (ok) __atomic_store_n(&gD.capsDone, 1u, __ATOMIC_RELEASE);
        return ok;
    }
    void record_adopted(const n48disp::PipeProbe &) {
        uint64_t dm = 0, pipe0 = 0;
        if (rd64(accel_addr() + n48disp::kAccelDm, &dm) && rd64(dm + n48disp::kDmPipes, &pipe0)) gD.adoptedPipe = pipe0;
        if (!gD.scratch) gD.scratch = static_cast<uint8_t *>(IOMalloc(n48disp::kScratchBytes));
        __atomic_store_n(&gD.adopted, 1u, __ATOMIC_RELEASE);
        DLOG("pipe adopt: recorded pipe %#llx (Navi48DisplayPipe on RDNA4FB), capabilities published, scratch %s", (unsigned long long)gD.adoptedPipe, gD.scratch ? "ready" : "NOT ALLOCATED");
    }

    // -- arm: the ONE byte, read back. The mirror flag (what perform tests) follows the verified write; a disarm clears it FIRST. --
    bool arm_write(uint8_t v) {
        if (!gD.accel) return false;
        if (v && !gD.scratch) gD.scratch = static_cast<uint8_t *>(IOMalloc(n48disp::kScratchBytes));      // a pipe verified without `adopt` having recorded it still needs its row buffer
        if (v && !gD.scratch) return false;                                                               // no row buffer: never open the gate
        const uint64_t a = accel_addr() + n48disp::kAccelPipeGate;
        uint8_t back = 0xff;
        if (!wr8(a, v) || !rd8(a, &back) || back != v) return false;
        __atomic_store_n(&gD.armed, v ? 1u : 0u, __ATOMIC_RELEASE);
        gD.arms++;
        DLOG("pipe arm: accel+0xccf = %u (read back %u)", (unsigned)v, (unsigned)back);
        return true;
    }
    void disarm() {
        __atomic_store_n(&gD.armed, 0u, __ATOMIC_RELEASE);                // the mirror first: perform stops copying at once
        gD.disarms++;
        uint8_t back = 0xff;
        if (gD.accel) {
            const uint64_t a = accel_addr() + n48disp::kAccelPipeGate;
            uint8_t cur = 0xff;
            if (rd8(a, &cur) && cur <= 1u) { (void)wr8(a, 0u); (void)rd8(a, &back); }     // a byte that is not 0 or 1 is not ours to write
        }
        DLOG("pipe arm 0: mirror cleared, accel+0xccf reads %u", (unsigned)back);
    }

    // -- slot 62 --
    void note_res62(const n48disp::Res62Plan &p, bool smKnown, bool surfKnown, bool flagsKnown, bool bit4) {
        const uint64_t n = ++gD.res62Calls;
        if (p.handled) gD.res62Handled++; else gD.res62Unhandled++;
        if (p.shortcut) gD.res62Shortcut++;
        if (p.shortcutRefused) gD.res62Refused++;
        // A log line for the shortcut decision EITHER way, each kind rate limited on its own counter (the first 8 of each, then every 256th): a run of applied shortcuts must not hide the first refusal.
        const uint64_t kc = p.shortcut ? gD.res62Shortcut : p.shortcutRefused ? gD.res62Refused : n;
        if (kc <= 8u || (kc & 0xffu) == 0u)
            DLOG("slot 62 call %llu: outputs %s (allocation size %llu, bytes per row %llu; SysMemory %s, IOSurface %s); shortcut %s (flags %s, bit 4 %d, switch %d)", (unsigned long long)n,
                 p.handled ? "handled" : "NOT handled (default)", (unsigned long long)p.allocSize, (unsigned long long)p.bytesPerRow, smKnown ? "read" : "unreadable", surfKnown ? "read" : "unreadable",
                 p.shortcut ? "APPLIED" : (p.shortcutRefused ? "REFUSED" : "not applied"), flagsKnown ? "read" : "unreadable", (int)bit4, (int)rdy_shortcut());
    }
};

static_assert(n48disp::kPfCount == 21u && n48disp::kPfScanOwned == 20u, "the stat pages cover reasons 0..20: page 0 has 0 and 1, page 1 has 2..11, page 2 has 12..19 (0.0.616: the copy time's minimum moved to the log line), page 3 (0.0.617) has 20 and the submit interval");
static_assert(n48disp::kAccelPipeGate < n48disp::kAccelSize && n48disp::kAccelCfgF4 + 4u <= n48disp::kAccelSize, "the accelerator reads and the one write lie inside the object");

// ---- the verb outputs ---------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t flags_word() {
    return n48disp::disp_flags(n48disp_latched_on(), __atomic_load_n(&gD.adopted, __ATOMIC_ACQUIRE) != 0u, __atomic_load_n(&gD.armed, __ATOMIC_ACQUIRE) != 0u,
                               __atomic_load_n(&gD.capsDone, __ATOMIC_ACQUIRE) != 0u, rdy_shortcut(), navi48_bar0_write_combined(),   // 0.0.615 (W1): bit 32 = BAR0 write-combined
                               __atomic_load_n(&gD.autoCause, __ATOMIC_ACQUIRE));                            // 0.0.617 (K4): bit 64 + the cause in bits 8..11
}

} // namespace

// ---- the public entry points ---------------------------------------------------------------------------------------------------------------------------------------------------------
bool n48disp_latched_on(void) {
    UInt32 l = gLatch;
    if (l == n48disp::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-metal-disp", &v, sizeof(v));
        if (OSCompareAndSwap(n48disp::kLatchUnset, n48disp::latch_value(present, v), &gLatch))
            DLOG("boot-arg navi48-metal-disp %s: the display pipe is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", n48disp::latch_is_on(gLatch) ? "ENABLED" : "OFF");
        l = gLatch;
    }
    return n48disp::latch_is_on(l);
}

uint32_t n48disp_fact_bits(void) { return __atomic_load_n(&gD.factBits, __ATOMIC_ACQUIRE); }

int n48disp_hook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) {
    (void)ctx;
    KernelEnv env;
    const int h = n48disp::hook_dispatch(env, cls, slot, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)), args, nargs, ret);
    const int k = slot == 267u ? 0 : slot == 277u ? 1 : slot == 278u ? 2 : slot == 279u ? 3 : -1;
    if (k >= 0 && cls == N48_VC_DISPLAYPIPE) {
        __atomic_add_fetch(&gD.hookCalls[k], 1ull, __ATOMIC_RELAXED);
        if (h) {
            __atomic_add_fetch(&gD.hookHandled[k], 1ull, __ATOMIC_RELAXED);
            if (k == 3) __atomic_add_fetch(*ret == n48disp::kWillPerform ? &gD.submitWill : &gD.submitPass, 1ull, __ATOMIC_RELAXED);
        }
    }
    return h;
}

void *n48disp_pci_device(void *ctx) {
    (void)ctx;
    KernelEnv env;
    return n48disp::pci_admit_flow(env) ? static_cast<void *>(navi48_bringup_pci()) : nullptr;     // 0.0.615 (P1): the PCI device only with the latch AND the resource facts on
}

void n48disp_on_trace(uint32_t event, uint64_t a, uint64_t b) {
    if (!n48disp_latched_on()) return;
    if (event == N48_TR_DM_START) {
        __atomic_add_fetch(&gD.dmStarts, 1ull, __ATOMIC_RELAXED);
        if (b) __atomic_add_fetch(&gD.dmSubst, 1ull, __ATOMIC_RELAXED);
        gD.lastDmProvider = a;
        return;
    }
    if (event != N48_TR_DISPPIPE) return;
    __atomic_add_fetch(&gD.pipeTraces, 1ull, __ATOMIC_RELAXED);
    if (!b || !a) return;                                                // the family's own pipe (or an allocation that failed): not ours to hook
    __atomic_add_fetch(&gD.pipeTracesOurs, 1ull, __ATOMIC_RELAXED);
    if (n48disp::pipe_table_find(gD.pipes, KernelEnv::cap_n(), a) >= 0) return;
    const uint32_t i = __atomic_fetch_add(&gD.nPipes, 1u, __ATOMIC_ACQ_REL);
    if (i >= n48disp::kMaxPipes) { gD.pipeTablefull++; return; }
    __atomic_store_n(&gD.pipes[i], a, __ATOMIC_RELEASE);                  // a NULL entry is skipped by pipe_table_find, so a reader racing this store is safe
}

int n48disp_res62(void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) {
    if (!n48disp_latched_on()) return 0;
    KernelEnv env;
    return n48disp::res62_flow(env, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)), args, nargs, ret, rdy_shortcut());
}

// 0.0.617 (K1): the native client closed or died (clientClose / stop). Only a client admitted by the uid-88 rule (adminClient false) while the pipe is armed disarms it. Called by the client with NO lock held,
// BEFORE n1c_close takes gCliLock: the descriptor-cache drain (up to 200 ms per entry) then cannot sit under the client lock, and the gate is shut before the (possibly long) idle wait of the close.
void n48disp_on_ws_client_closed(bool adminClient) {
    if (!n48disp_latched_on()) return;
    KernelEnv env;
    (void)n48disp::ws_client_closed_flow(env, adminClient);
}
// 0.0.617 (K2): the HUNG latch was set (Navi48MetalNub::hungLatched). Under HUNG the cache teardown leaks instead of draining, so this never spins.
void n48disp_on_hung(void) {
    if (!n48disp_latched_on()) return;
    KernelEnv env;
    (void)n48disp::hung_latched_flow(env);
}

void n48disp_on_withdraw(void) {
    if (!n48disp_latched_on()) return;
    KernelEnv env;
    n48disp::withdraw_flow(env);                                        // 0.0.615 (P2): the gate byte back to 0 FIRST (needs gD.accel), then the pipes are forgotten
    IOService *a = gD.accel; gD.accel = nullptr;
    if (a) a->release();
    DLOG("nub withdrawn: accelerator reference released");
}

uint32_t n48disp_verb(uint32_t action, uint64_t arg, uint64_t *out, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out[i] = 0;
    if (count < 13u || !n48disp::is_pipe_verb(action)) { if (count) out[0] = n48disp::kBadArg; return n48disp::kBadArg; }   // 88 (pipeagdc) is answered by DisplayPipeGuard.cpp, never here
    if (!n48disp::action_admitted(n48disp_latched_on(), action)) { out[0] = n48disp::kOff; return n48disp::kOff; }
    if (!n48disp::verb_args_ok(action, arg)) { out[0] = n48disp::kBadArg; return n48disp::kBadArg; }     // the legal arguments are the exemption table's
    KernelEnv env;
    uint32_t st = n48disp::kOk;
    if (action == n48disp::kActAdopt) {
        st = n48disp::adopt_flow(env);
        out[1] = n48metal_factory_mask_now(); out[2] = gD.adoptedPipe; out[3] = gD.pipeTracesOurs; out[4] = gD.dmStarts; out[5] = gD.dmSubst; out[6] = gD.lastDmProvider;
        out[7] = gD.pipeTraces; out[8] = env.accel_addr(); out[9] = flags_word(); out[10] = gD.adoptTries; out[11] = gD.pipeTablefull; out[12] = KernelEnv::cap_n();
    } else if (action == n48disp::kActArm) {
        st = n48disp::arm_flow(env, arg);
        uint8_t gate = 0xff;
        if (gD.accel) (void)env.rd8(env.accel_addr() + n48disp::kAccelPipeGate, &gate);
        out[1] = flags_word(); out[2] = gate; out[3] = gD.arms; out[4] = gD.disarms; out[5] = n48metal_factory_mask_now(); out[6] = gD.adoptedPipe;
        DLOG("pipe arm %llu: %s (%u); accel+0xccf now %u", (unsigned long long)arg, n48disp::status_name(st), (unsigned)st, (unsigned)gate);
    } else if (action == n48disp::kActStat) {
        out[1] = flags_word(); out[2] = n48metal_factory_mask_now();
        if (arg == 0ull) {
            out[3] = gD.hookCalls[0]; out[4] = gD.hookCalls[1]; out[5] = gD.hookCalls[2]; out[6] = gD.hookCalls[3];
            out[7] = gD.hookHandled[0] | (gD.hookHandled[1] << 16) | (gD.hookHandled[2] << 32) | (gD.hookHandled[3] << 48);   // four 16-bit counts
            out[8] = gD.submitPass; out[9] = gD.submitWill; out[10] = gD.bytes; out[11] = gD.reasons[n48disp::kPfCopied]; out[12] = gD.reasons[n48disp::kPfDisarmed];
        } else if (arg == 1ull) {
            for (unsigned r = 2; r <= 11u; ++r) out[r + 1u] = gD.reasons[r];                                   // out[3..12] = reasons 2..11
        } else if (arg == 2ull) {
            for (unsigned r = 12; r < n48disp::kPfScanOwned; ++r) out[r - 12u + 3u] = gD.reasons[r];            // out[3..10] = reasons 12..19 (19 = not prepared, 0.0.616)
            out[11] = n48disp::dur_avg(gD.dur); out[12] = gD.dur.max;                                         // ns, of copied frames only (0.0.616: the minimum is in the log line, out[10] holds reason 19)
        } else if (arg == 4ull) {                                                                              // 0.0.618 (V2), page 4: the vblank timestamps
            out[3] = env.vbl_on() ? 1u : 0u; out[4] = gD.vbl[n48disp::kVblWrote];
            out[5] = gD.vbl[n48disp::kVblOff] + gD.vbl[n48disp::kVblDisarmed]; out[6] = gD.vbl[n48disp::kVblBadTxn] + gD.vbl[n48disp::kVblNotTxn];
            out[7] = gD.vbl[n48disp::kVblNoTiming] + gD.vbl[n48disp::kVblHung]; out[8] = gD.vbl[n48disp::kVblBadMath] + gD.vbl[n48disp::kVblWriteFail];
            out[9] = gD.vblLastT; out[10] = gD.vblLastPeriodNs; out[11] = gD.vblLastDelayNs; out[12] = gD.vblLastPeriodAbs;
            DLOG("pipe stat page 4 (vblank timestamps): switch %s; written %llu, off %llu, disarmed %llu, bad-txn %llu, not-txn %llu, hung %llu, no-timing %llu, bad-math %llu, write-fail %llu; last t_vbl %llu next %llu (abs), period %llu ns (%llu abs), to-next-vblank %llu ns",
                 env.vbl_on() ? "ON" : "OFF", (unsigned long long)gD.vbl[0], (unsigned long long)gD.vbl[1], (unsigned long long)gD.vbl[2], (unsigned long long)gD.vbl[3], (unsigned long long)gD.vbl[4],
                 (unsigned long long)gD.vbl[5], (unsigned long long)gD.vbl[6], (unsigned long long)gD.vbl[7], (unsigned long long)gD.vbl[8], (unsigned long long)gD.vblLastT, (unsigned long long)gD.vblLastNext,
                 (unsigned long long)gD.vblLastPeriodNs, (unsigned long long)gD.vblLastPeriodAbs, (unsigned long long)gD.vblLastDelayNs);
        } else {                                                                                               // 0.0.617 (K6), page 3
            out[3] = gD.reasons[n48disp::kPfScanOwned]; out[4] = gD.submitScanSkipped; out[5] = gD.ivl.d.n; out[6] = gD.ivl.d.min; out[7] = n48disp::dur_avg(gD.ivl.d); out[8] = gD.ivl.d.max;
            out[9] = n48dcn::scanActive() ? 1u : 0u;                                                          // out[9]: a native client owns the scanout plane right now
        }
        DLOG("pipe stat page %llu: auto-disarmed: %s (count %llu); flags %#x (BAR0 %s) facts %#x, copied %llu disarmed %llu, bytes %llu, perform ns min/avg/max %llu/%llu/%llu (%llu frames), submit %llu will-perform %llu pass-through, slot 62 %llu calls %llu handled; scan-owned: perform %llu submit-skip %llu active %d, submit interval ns min/avg/max %llu/%llu/%llu (%llu); md cache prepared %llu hits %llu misses %llu prepFail %llu evicted %llu evictBlocked %llu full %llu torn %llu leaked %llu drainFail %llu",
             (unsigned long long)arg, n48disp::auto_cause_name(__atomic_load_n(&gD.autoCause, __ATOMIC_ACQUIRE)), (unsigned long long)gD.autoDisarms, (unsigned)flags_word(), (flags_word() & n48disp::kFlagBar0Wc) ? "WC" : "UNCACHED", (unsigned)n48metal_factory_mask_now(), (unsigned long long)gD.reasons[n48disp::kPfCopied], (unsigned long long)gD.reasons[n48disp::kPfDisarmed],
             (unsigned long long)gD.bytes, (unsigned long long)gD.dur.min, (unsigned long long)n48disp::dur_avg(gD.dur), (unsigned long long)gD.dur.max, (unsigned long long)gD.dur.n,
             (unsigned long long)gD.submitWill, (unsigned long long)gD.submitPass, (unsigned long long)gD.res62Calls, (unsigned long long)gD.res62Handled,
             (unsigned long long)gD.reasons[n48disp::kPfScanOwned], (unsigned long long)gD.submitScanSkipped, (int)n48dcn::scanActive(), (unsigned long long)gD.ivl.d.min, (unsigned long long)n48disp::dur_avg(gD.ivl.d), (unsigned long long)gD.ivl.d.max, (unsigned long long)gD.ivl.d.n,
             (unsigned long long)gD.mdc.prepared, (unsigned long long)gD.mdc.hits, (unsigned long long)gD.mdc.misses, (unsigned long long)gD.mdc.prepFail, (unsigned long long)gD.mdc.evicted,
             (unsigned long long)gD.mdc.evictBlocked, (unsigned long long)gD.mdc.full, (unsigned long long)gD.mdc.tornDown, (unsigned long long)gD.mdc.leaked, (unsigned long long)gD.mdc.drainFail);
    } else if (action == n48disp::kActStamps) {
        (void)env.find_accel();
        n48disp::StampsOut so {};
        st = n48disp::stamps_flow(env, &so);
        out[1] = so.em; out[2] = so.count; out[3] = so.clamped; out[4] = so.completed; out[5] = so.submitted; out[6] = so.hazard ? 1u : 0u; out[7] = 1u;   // out[7]: the layout is SUSPECTED
        DLOG("pipe stamps (SUSPECTED layout, em+0x30 / +0xf8 / +0xfc only): %s; event machine %#llx count %u (clamped %u) completed %u submitted %u hazard %d", n48disp::status_name(st), (unsigned long long)so.em,
             (unsigned)so.count, (unsigned)so.clamped, (unsigned)so.completed, (unsigned)so.submitted, (int)so.hazard);
    } else if (action == n48disp::kActVbl) {                             // 0.0.618 (V2): the vblank-timestamp switch
        if (arg > 1ull) st = n48disp::kBadArg;
        else __atomic_store_n(&gD.vblOff, arg ? 0u : 1u, __ATOMIC_RELEASE);
        out[1] = env.vbl_on() ? 1u : 0u; out[2] = gD.vbl[n48disp::kVblWrote]; out[3] = gD.vblLastPeriodNs; out[4] = gD.vblLastT; out[5] = gD.vblLastNext;
        DLOG("pipe vbl %llu: %s; the vblank timestamps are %s; written %llu, last period %llu ns", (unsigned long long)arg, n48disp::status_name(st), env.vbl_on() ? "ON" : "OFF",
             (unsigned long long)gD.vbl[n48disp::kVblWrote], (unsigned long long)gD.vblLastPeriodNs);
    } else if (action == n48disp::kActReload) {                          // 0.0.619 (R1): the operator restart window
        if (arg == 0ull) n48disp::reload_open_flow(env);                 // 0 (the default) opens a fresh 15 s window; 1 only reads the state
        uint64_t remNs = 0;
        const uint32_t rs = n48disp::reload_state_flow(env, &remNs);
        out[1] = rs; out[2] = remNs / 1000000ull; out[3] = gD.reload.opens; out[4] = gD.reload.tolerated; out[5] = gD.reload.closed267; out[6] = gD.reload.expired;
        out[7] = env.armed() ? 1u : 0u; out[8] = env.hung() ? 1u : 0u;
        DLOG("pipe reload %llu: %s; window %s, %llu ms left; opened %llu, closes tolerated %llu, closed by slot 267 %llu, expired %llu; pipe %s, GPU %s", (unsigned long long)arg, n48disp::status_name(st),
             n48disp::reload_state_name(rs), (unsigned long long)out[2], (unsigned long long)gD.reload.opens, (unsigned long long)gD.reload.tolerated, (unsigned long long)gD.reload.closed267,
             (unsigned long long)gD.reload.expired, env.armed() ? "ARMED" : "not armed", env.hung() ? "HUNG (the window is not honoured)" : "ok");
    } else {                                                             // kActShortcut
        if (arg > 1ull) st = n48disp::kBadArg;
        else __atomic_store_n(&gD.shortcutOff, arg ? 0u : 1u, __ATOMIC_RELEASE);
        out[1] = rdy_shortcut() ? 1u : 0u; out[2] = gD.res62Shortcut; out[3] = gD.res62Refused; out[4] = gD.res62Calls; out[5] = gD.res62Handled; out[6] = gD.res62Unhandled;
        DLOG("pipe shortcut %llu: %s; the 'already prepared' shortcut is %s; applied %llu, refused %llu, slot 62 calls %llu", (unsigned long long)arg, n48disp::status_name(st), rdy_shortcut() ? "ON" : "OFF",
             (unsigned long long)gD.res62Shortcut, (unsigned long long)gD.res62Refused, (unsigned long long)gD.res62Calls);
    }
    out[0] = st;
    return st;
}
