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
#include "Navi48DisplayNub.hpp"     // 0.0.659 (M6): framebufferOf(index)

#define DLOG(fmt, ...) AMDGPU_LOG("disp", fmt, ##__VA_ARGS__)

IOService *navi48_bringup_pci(void);   // Navi48Bringup.cpp: our IOPCIDevice
bool navi48_bar0_write_combined(void); // Navi48Bringup.cpp (0.0.615, W1): the BAR0 kernel mapping was made write-combined

namespace {

// ---- the latch ----------------------------------------------------------------------------------------------------------------------------------------
volatile UInt32 gLatch = n48disp::kLatchUnset;
volatile uint32_t gM6Latch = n48m6::kLatchUnset;    // 0.0.659 (M6): boot-arg navi48-m6, read ONCE (first writer wins, n48m6::latch_get), default OFF
volatile uint32_t gM6FlipLatch = n48m6::kLatchUnset;   // 0.0.661 (M6 Stage 1b): boot-arg navi48-m6flip, read ONCE (n48m6::latch_get), default OFF
volatile uint32_t gM6Flip1Latch = n48m6::kLatchUnset;  // 0.0.662 (M6 Stage 2): boot-arg navi48-m6flip1, read ONCE (n48m6::latch_get), default OFF
uint8_t gM6Blob[n48m6::kBlobBufBytes];   // 0.0.662 (HW1): sized for the LARGEST blob (v2), not the v1 one
static_assert(sizeof gM6Blob >= n48m6::kBlobMax2 && sizeof gM6Blob >= n48m6::kBlobMax, "the publish buffer must hold a version-2 blob");
                  // 0.0.660 (S2): the published table's bytes; written only by the holder of State::m6Pub's gate (n48m6::publish_serialised)

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
    // ---- 0.0.659 (M6 Stage 1a): all of it stays zero with the navi48-m6 latch OFF ----
    n48m6::SurfTable m6Surf;                        // IOSurface ID -> instance, learned in the submit hook
    n48m6::FbMap     m6Fb;                          // framebuffer pointer -> instance, recorded by adopt from the verified pipes
    IOService       *m6FbRef[n48m6::kInstCount];    // the references that keep the mapped framebuffers alive (released at withdraw)
    uint64_t m6Txn[n48m6::kInstCount + 1u];         // submits seen, per pipe instance ([3] = a pipe on no known framebuffer)
    uint64_t m6NoCopy[n48m6::kInstCount];           // performs completed without any copy, per pipe instance
    uint64_t m6Route[n48m6::kRouteCount];           // routing-guard verdicts (DP pipe presents)
    uint64_t m6RouteBad[n48m6::kInstCount + 1u];    // guard refusals, per pipe instance
    uint64_t m6LearnSkip[n48m6::kSkCount];          // transactions the learn declined to read, by reason
    uint64_t m6Stamp[n48m6::kInstCount], m6StampFail[n48m6::kInstCount], m6Period[n48m6::kInstCount];     // vblank stamps written / refused per OTG-owning instance, and the last period
    uint64_t m6Published;                           // times the table property was written
    n48m6::PubRate m6Rate;                          // 0.0.661 (R5): the publish rate limiter (navi48-m6flip only; ATOMIC since the review M1)
    volatile uint64_t m6LogNs;                      // 0.0.661 (S-2): the per-learn log line is printed at most once a second
    n48m6::PubGate m6Pub;                           // 0.0.660 (S2): the one publisher at a time (n48m6::publish_serialised)
    uint64_t m6LastTxnNs[n48m6::kInstCount];        // 0.0.660 (K1): uptime ns of each pipe's last transaction (0 = none since boot): m6stat page 3 reports the age
    uint64_t m6Learned;                             // submits whose surface was learned (new, same or ambiguous)
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
        if (ev == n48disp::kRwOpened) DLOG("operator restart window: opened (%llu s); EVERY WindowServer client close is tolerated and slot-267 calls are not counted until the new client's first slot 267 has been seen and %llu s have passed since the last tolerated close, or expiry", (unsigned long long)(n48disp::kReloadWindowNs / 1000000000ull), (unsigned long long)(n48disp::kReloadSettleNs / 1000000000ull));
        else if (ev == n48disp::kRwTolerated) DLOG("operator restart window: client close tolerated (the pipe stays armed; the settle time restarts)");
        else if (ev == n48disp::kRwClosed267) DLOG("operator restart window: closed (the new client's slot 267 was seen and the last tolerated close is at least %llu s old, the pipe is still armed)", (unsigned long long)(n48disp::kReloadSettleNs / 1000000000ull));
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
    // -- 0.0.659 (M6 Stage 1a): the latch, the framebuffer -> instance map, the surface table, the routing guard's counters (flows: native_disp_flow.h m6_*) --
    bool m6_on() { return n48m6_latched_on(); }
    uint32_t pipe_inst(uint64_t pipe) {                                  // pipe+0x98 is the pipe's framebuffer; the map was recorded by adopt from the verified pipes
        uint64_t fb = 0;
        if (!rd64(pipe + n48disp::kPipeFb, &fb)) return n48m6::kInstNone;
        return n48m6::inst_of_fb(gD.m6Fb, fb);
    }
    n48m6::Look m6_lookup(uint32_t id) { return n48m6::lookup_surface(gD.m6Surf, id); }
    // 0.0.660 (S2): the table out as a property of the Metal nub (the bundle reads it). Two pipes' submit hooks can both change the table and both publish: n48m6::publish_serialised lets ONE caller publish at a time
    // and has it rebuild the blob whenever another change arrived meanwhile, so the LAST property written always carries the newest table (the hook never blocks: a caller that loses returns at once). The ~200-byte
    // blob is the file-scope gM6Blob (not the hook's stack), owned by the publisher that holds the gate.
    bool m6flip_on() { return n48m6flip_latched_on(); }
    void m6_publish_now() {
        const bool v2 = n48m6flip_latched_on();      // 0.0.661 (R2): blob version 2 carries the generation the build covers (PubGate::curGen, set under the gate); OFF = 0.0.660's version-1 blob, byte for byte
        (void)n48m6::publish_serialised(gD.m6Pub,
            [v2]() -> uint32_t { return v2 ? n48m6::blob_build_v2(gD.m6Surf, __atomic_load_n(&gD.m6Pub.curGen, __ATOMIC_ACQUIRE), gM6Blob, sizeof gM6Blob) : n48m6::blob_build(gD.m6Surf, gM6Blob, sizeof gM6Blob); },
            [](uint32_t n) -> bool { const bool ok = Navi48MetalNub::setPublishedData(N48M6_PROP_SURF, gM6Blob, n); if (ok) __atomic_add_fetch(&gD.m6Published, 1ull, __ATOMIC_RELAXED); return ok; });
    }
    // 0.0.661 (R5): with navi48-m6flip the publish is rate-limited (n48m6::kPubPerSec) while the table thrashes; a deferred change is flushed by the next change or by Status2 (n48disp_m6_flush). OFF: every change publishes at once, as 0.0.660.
    void m6_publish() {
        if (!n48m6flip_latched_on()) { m6_publish_now(); return; }
        if (n48m6::pub_rate_admit(gD.m6Rate, now_ns())) m6_publish_now();
    }
    // 0.0.661 (review M1): a deferred publish is flushed by ANY path that runs after its gap: every submit of every pipe (here), the DP's 1 s status call (n48disp_m6_flush from n1c_scan_status) and Status2.
    void m6_flush_deferred() { if (n48m6flip_latched_on()) (void)n48m6::pub_rate_flush(gD.m6Rate, now_ns(), [this]() { m6_publish_now(); }); }
    uint32_t m6_learn(uint32_t id, uint32_t inst) {
        m6_flush_deferred();
        const uint32_t r = n48m6::learn_surface(gD.m6Surf, id, inst, n48m6flip_latched_on());   // 0.0.661 (R3): with navi48-m6flip the eviction victim is the owner's own distance (victim_slot), not `seen`
        if (n48m6::learn_counted(r)) __atomic_add_fetch(&gD.m6Learned, 1ull, __ATOMIC_RELAXED);
        if (n48m6::learn_changed(r)) {
            if (!n48m6flip_latched_on() || n48m6::log_admit(gD.m6LogNs, now_ns())) {   // 0.0.661 (S-2): with navi48-m6flip the per-change line is rate-limited (1/s); OFF: every change logs, as 0.0.660
            DLOG("m6: surface %u %s on instance %u (table now %u IDs: DP %u, monitor A %u, monitor B %u; %u ambiguous)", id, r == n48m6::kLNew ? "learned" : r == n48m6::kLReplaced ? "RE-LEARNED (its old owner went stale)" : r == n48m6::kLEvicted ? "learned, EVICTING the least recently seen entry" : "SEEN ON TWO PIPES -> AMBIGUOUS", inst, (unsigned)gD.m6Surf.n,
                 n48m6::count_for_inst(gD.m6Surf, 0), n48m6::count_for_inst(gD.m6Surf, 1), n48m6::count_for_inst(gD.m6Surf, 2), n48m6::count_ambiguous(gD.m6Surf)); }
            m6_publish();
        }
        return r;
    }
    void m6_reset() {                                                    // 0.0.660 (S1): the table is emptied (a new WindowServer start allocates its surfaces afresh) and the empty table republished
        if (!n48m6::reset_table(gD.m6Surf)) { DLOG("m6: the surface table could not be reset (a learn held it); the next arm / restart window retries"); return; }
        DLOG("m6: surface table RESET (%llu so far); republishing the empty table", (unsigned long long)gD.m6Surf.resets);
        m6_publish();
    }
    void m6_touch(uint32_t id) { (void)n48m6::touch_surface(gD.m6Surf, id); }
    void m6_note_submit(uint32_t inst) {
        __atomic_add_fetch(&gD.m6Txn[n48m6::inst_valid(inst) ? inst : n48m6::kInstCount], 1ull, __ATOMIC_RELAXED);
        if (n48m6::inst_valid(inst)) __atomic_store_n(&gD.m6LastTxnNs[inst], now_ns(), __ATOMIC_RELAXED);        // 0.0.660 (K1)
    }
    void m6_note_learn_skip(uint32_t why) {
        const uint64_t n = __atomic_add_fetch(&gD.m6LearnSkip[why < n48m6::kSkCount ? why : 0u], 1ull, __ATOMIC_RELAXED);
        if (n <= 3u) DLOG("m6: transaction not learned (reason %u) #%llu", (unsigned)why, (unsigned long long)n);
    }
    void m6_note_route(uint32_t verdict, uint32_t pipeInst) {
        const uint64_t n = __atomic_add_fetch(&gD.m6Route[verdict < n48m6::kRouteCount ? verdict : 0u], 1ull, __ATOMIC_RELAXED);
        if (verdict != n48m6::kRouteOk) {
            __atomic_add_fetch(&gD.m6RouteBad[n48m6::inst_valid(pipeInst) ? pipeInst : n48m6::kInstCount], 1ull, __ATOMIC_RELAXED);
            if (n <= 3u) DLOG("m6: present REFUSED by the routing guard on the pipe of instance %u: %s #%llu", (unsigned)pipeInst, n48m6::route_name(verdict), (unsigned long long)n);
        }
    }
    void m6_note_nocopy(uint32_t pipeInst) { __atomic_add_fetch(&gD.m6NoCopy[pipeInst < n48m6::kInstCount ? pipeInst : 0u], 1ull, __ATOMIC_RELAXED); }
    void m6_note_stamp(uint32_t inst, uint32_t verdict, uint64_t periodNs) {
        if (!n48m6::inst_valid(inst)) return;                            // a pipe on no known framebuffer has no OTG to account against
        const uint32_t i = inst;
        if (verdict == n48disp::kVblWrote) { __atomic_add_fetch(&gD.m6Stamp[i], 1ull, __ATOMIC_RELAXED); gD.m6Period[i] = periodNs; }
        else __atomic_add_fetch(&gD.m6StampFail[i], 1ull, __ATOMIC_RELAXED);
    }
    bool vbl_sample_inst(uint32_t inst, n48disp::VblSample *s) {          // that instance's OWN OTG (OTG1 = the monitor A, OTG2 = the monitor B); the raster constants come from the per-index geometry table
        N48DispGeom g;
        if (!n48disp_geom_for_index(n48m6::index_of_inst(inst), &g)) return false;
        uint64_t perSec = 0;
        nanoseconds_to_absolutetime(1000000000ull, &perSec);
        if (perSec == 0ull || perSec > 0xffffffffull) return false;
        if (!n48dcn::vblSampleInst(inst, g.w, g.h, g.pixHz, &s->periodNs, &s->delayNs, &s->nowAbs)) return false;
        s->numer = 1000000000u; s->denom = (uint32_t)perSec;
        return true;
    }
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
        for (uint32_t i = 0; i < n48m6::kInstCount; ++i) {                // 0.0.659 (M6): the framebuffer map goes first (every pipe is unknown from now on), then the references
            __atomic_store_n(&gD.m6Fb.fb[i], 0ull, __ATOMIC_RELEASE);
            IOService *o = gD.m6FbRef[i]; gD.m6FbRef[i] = nullptr;
            if (o) o->release();
        }
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
        if (n48m6_latched_on() && p.count >= 1u) m6_probe_ents(&p, acc, dm);      // 0.0.659 (M6): EVERY pipe of the display machine is judged, not dm+0x88[0] only
        return p;
    }
    // 0.0.659 (M6 Stage 1a): one entry per pipe the display machine counts. The framebuffer map is read FRESH from the registry (RDNA4FB = instance 0; the framebuffer child of each published display nub =
    // its instance); a pipe's +0x98 must be one of them, each instance may hold one pipe, and the set of instances must be exactly the published ones plus the DP (n48m6::ents_verdict).
    // 0.0.660 (S7): the framebuffers are KEPT, not released and looked up again: refs[i] holds the retained service whose address is m->fb[i] (null when the instance has none). The CALLER owns the references
    // (m6_release_refs, or hands them to gD.m6FbRef), so the pointer in the map is the one that was verified and no raw address is ever re-retained.
    void m6_collect_fbs(n48m6::FbMap *m, IOService **refs) {
        for (uint32_t i = 0; i < n48m6::kInstCount; ++i) { m->fb[i] = 0ull; refs[i] = nullptr; }
        if (IOService *dp = find_fb()) { m->fb[n48m6::kInstDp] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dp)); refs[n48m6::kInstDp] = dp; }
        for (uint32_t inst = n48m6::kInstMonA; inst <= n48m6::kInstMonB; ++inst) {
            if (!n48fb_nub_exists_idx(n48m6::index_of_inst(inst))) continue;
            if (IOService *f = Navi48DisplayNub::framebufferOf(n48m6::index_of_inst(inst))) { m->fb[inst] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f)); refs[inst] = f; }
        }
    }
    void m6_release_refs(IOService **refs) { for (uint32_t i = 0; i < n48m6::kInstCount; ++i) if (refs[i]) { refs[i]->release(); refs[i] = nullptr; } }
    void m6_probe_ents(n48disp::PipeProbe *p, uint64_t acc, uint64_t dm) {
        n48m6::FbMap map; IOService *refs[n48m6::kInstCount];
        m6_collect_fbs(&map, refs);
        p->ents.n = p->count;                                              // more pipes than the table holds fails ents_verdict (kEntsCount)
        p->ents.expectMask = n48m6::expect_mask(n48fb_nub_exists_idx(n48m6::index_of_inst(n48m6::kInstMonB)), n48fb_nub_exists_idx(n48m6::index_of_inst(n48m6::kInstMonA)));
        for (uint32_t i = 0; i < p->count && i < n48m6::kMaxEnt; ++i) {
            n48m6::PipeEnt &e = p->ents.e[i];
            e = n48m6::PipeEnt {};
            e.inst = n48m6::kInstNone;
            uint64_t pp = 0;
            if (!rd64(dm + n48disp::kDmPipes + 8u * i, &pp)) continue;
            e.nullp = pp == 0ull;
            e.have = n48disp::kptr_ok(pp);
            if (!e.have) continue;
            e.classOurs = class_is(pp, "Navi48DisplayPipe");
            e.traced = known_pipe(pp);
            uint64_t a = 0, d = 0, f = 0;
            if (rd64(pp + n48disp::kPipeAccel, &a) && rd64(pp + n48disp::kPipeDm, &d) && rd64(pp + n48disp::kPipeFb, &f)) {
                e.backAccel = a == acc; e.backDm = d == dm; e.fb = f;
                e.inst = n48m6::inst_of_fb(map, f);
                e.fbOk = n48m6::inst_valid(e.inst);
            }
        }
        m6_release_refs(refs);                                             // the map only held addresses for the comparison above
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
    void m6_record_map() {                                               // the framebuffer -> instance map the hooks use, re-resolved NOW (adopt just verified the same registry state) and kept referenced until withdraw
        n48m6::FbMap map; IOService *refs[n48m6::kInstCount];
        m6_collect_fbs(&map, refs);                                        // 0.0.660 (S7): the references come from the collection itself; a slot already recorded gives its fresh reference back
        for (uint32_t i = 0; i < n48m6::kInstCount; ++i) {
            if (!refs[i]) continue;
            if (gD.m6FbRef[i]) { refs[i]->release(); refs[i] = nullptr; continue; }
            gD.m6FbRef[i] = refs[i];
            refs[i] = nullptr;
            __atomic_store_n(&gD.m6Fb.fb[i], map.fb[i], __ATOMIC_RELEASE);
        }
        (void)Navi48MetalNub::setPublishedNumber(N48M6_PROP_LATCH, 1ull);   // tells the bundle the latch is ON (its other channel is the scan_query flag)
        m6_publish();
        DLOG("m6: framebuffer map recorded: DP %#llx, monitor A %#llx, monitor B %#llx", (unsigned long long)gD.m6Fb.fb[0], (unsigned long long)gD.m6Fb.fb[1], (unsigned long long)gD.m6Fb.fb[2]);
    }
    void record_adopted(const n48disp::PipeProbe &) {
        uint64_t dm = 0, pipe0 = 0;
        if (rd64(accel_addr() + n48disp::kAccelDm, &dm) && rd64(dm + n48disp::kDmPipes, &pipe0)) gD.adoptedPipe = pipe0;
        if (!gD.scratch) gD.scratch = static_cast<uint8_t *>(IOMalloc(n48disp::kScratchBytes));
        if (n48m6_latched_on()) m6_record_map();
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

static_assert(n48disp::kPfCount == 23u && n48disp::kPfScanOwned == 20u && n48disp::kPfOtherInst == 21u && n48disp::kPfRouteRefused == 22u, "the stat pages cover reasons 0..20: page 0 has 0 and 1, page 1 has 2..11, page 2 has 12..19 (0.0.616: the copy time's minimum moved to the log line), page 3 (0.0.617) has 20 and the submit interval");
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

// 0.0.659 (M6 Stage 1a): boot-arg navi48-m6 == 1, read and latched ONCE (first writer wins, never re-read). Default OFF: with it absent every M6 branch of the flows is skipped and the code is 0.0.658's.
bool n48m6_latched_on(void) {
    return n48m6::latch_is_on(n48m6::latch_get(&gM6Latch, []() -> uint32_t {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-m6", &v, sizeof(v));
        DLOG("boot-arg navi48-m6 %s: the multi-display routing (R1-R5 lifted, the routing guard) is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", n48m6::latch_is_on(n48m6::latch_value(present, v)) ? "ENABLED" : "OFF");
        return n48m6::latch_value(present, v);
    }));
}

// 0.0.661 (M6 Stage 1b): boot-arg navi48-m6flip == 1, read and latched ONCE (first writer wins, never re-read). Default OFF. Every Stage-1b path (selectors 22..26, the flip-side notes, the Stage-1b CLI report) asks this AND navi48-m6.
bool n48m6flip_latched_on(void) {
    return n48m6::latch_is_on(n48m6::latch_get(&gM6FlipLatch, []() -> uint32_t {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-m6flip", &v, sizeof(v));
        DLOG("boot-arg navi48-m6flip %s: instance 2's GPU-composited scanout (ABI 1.11 selectors 22..26) is %s for this boot (it also needs navi48-m6=1)", present ? (v == 1u ? "=1" : "present but not 1") : "absent", n48m6::latch_is_on(n48m6::latch_value(present, v)) ? "ENABLED" : "OFF");
        return n48m6::latch_value(present, v);
    }));
}
// 0.0.662 (M6 Stage 2): boot-arg navi48-m6flip1 == 1, read and latched ONCE (first writer wins, never re-read). Default OFF. INSTANCE 1 (the monitor A's HUBP1 / OTG1 scanout) needs ALL THREE latches (navi48-m6, navi48-m6flip, navi48-m6flip1); with this one OFF a request
// for instance 1 is answered exactly as 0.0.661 answered it (kIOReturnBadArgument) and nothing of instance 1 exists.
bool n48m6flip1_latched_on(void) {
    return n48m6::latch_is_on(n48m6::latch_get(&gM6Flip1Latch, []() -> uint32_t {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-m6flip1", &v, sizeof(v));
        DLOG("boot-arg navi48-m6flip1 %s: instance 1's (the monitor A's) GPU-composited scanout (ABI 1.12) is %s for this boot (it also needs navi48-m6=1 and navi48-m6flip=1)", present ? (v == 1u ? "=1" : "present but not 1") : "absent", n48m6::latch_is_on(n48m6::latch_value(present, v)) ? "ENABLED" : "OFF");
        return n48m6::latch_value(present, v);
    }));
}
// 0.0.661 (R2): the generation of the LAST PUBLISHED table (PubGate::pubGen), never the request count: Status2 reports it, the bundle compares it with the generation inside the blob it read.
uint32_t n48disp_m6_gen(void) { return (n48m6_latched_on() && n48m6flip_latched_on()) ? __atomic_load_n(&gD.m6Pub.pubGen, __ATOMIC_ACQUIRE) : 0u; }
// 0.0.661 (R5, M1): a deferred publish goes out now if its gap has passed (called by Status2 AND by the DP's 1 s status call n1c_scan_status AND by every learn: the last state is always published within a second, with or without instance 2).
void n48disp_m6_flush(void) {
    if (!(n48m6_latched_on() && n48m6flip_latched_on())) return;
    KernelEnv ke; (void)n48m6::pub_rate_flush(gD.m6Rate, now_ns(), [&ke]() { ke.m6_publish_now(); });
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
void n48disp_on_ws_client_closed(bool closingIsWsSession) {
    if (!n48disp_latched_on()) return;
    KernelEnv env;
    (void)n48disp::ws_client_closed_flow(env, closingIsWsSession);
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

bool n48disp_pipe_adopted(void) { return __atomic_load_n(&gD.adopted, __ATOMIC_ACQUIRE) != 0u; }

uint32_t n48disp_verb(uint32_t action, uint64_t arg, uint64_t *out, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out[i] = 0;
    if (count < 13u || !n48disp::is_pipe_verb(action)) { if (count) out[0] = n48disp::kBadArg; return n48disp::kBadArg; }   // 88 (pipeagdc) is answered by DisplayPipeGuard.cpp, never here
    if (!n48disp::action_admitted(n48disp_latched_on(), action)) { out[0] = n48disp::kOff; return n48disp::kOff; }
    if (!n48disp::verb_args_ok(action, arg)) { out[0] = n48disp::kBadArg; return n48disp::kBadArg; }     // the legal arguments are the exemption table's
    if (n48disp::fb_interlock_refuses(action, arg, n48fb_nub_exists(), n48m6_latched_on())) {                                // 0.0.652 (M5): a display nub exists: the display machine must never get a pipe (nothing is read or written)
        out[0] = n48disp::kFbNub;
        DLOG("pipe verb %u arg %llu REFUSED: %s", action, (unsigned long long)arg, n48disp::status_name(n48disp::kFbNub));
        return n48disp::kFbNub;
    }
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
    } else if (action == n48disp::kActM6Stat) {                          // 0.0.659 (M6): READ-ONLY - per pipe transactions, surface IDs, ambiguous, refused presents, stamps per OTG
        const uint32_t on = n48m6_latched_on() ? 1u : 0u;
        out[1] = (on ? 1u : 0u) | (__atomic_load_n(&gD.adopted, __ATOMIC_ACQUIRE) ? 2u : 0u) | (__atomic_load_n(&gD.armed, __ATOMIC_ACQUIRE) ? 4u : 0u) | ((uint64_t)KernelEnv::cap_n() << 8) | ((uint64_t)gD.m6Surf.n << 16);
        if (arg == 0ull) {                                                // page 0: the pipes
            for (unsigned i = 0; i < n48m6::kInstCount; ++i) { out[2 + i] = gD.m6Txn[i]; out[5 + i] = n48m6::count_for_inst(gD.m6Surf, i); }
            out[8] = n48m6::count_ambiguous(gD.m6Surf);                    // surface IDs seen on two pipes
            out[9] = gD.m6Route[n48m6::kRouteUnknown] + gD.m6Route[n48m6::kRouteAmbiguous] + gD.m6Route[n48m6::kRouteWrongInst] + gD.m6Route[n48m6::kRouteNoId] + gD.m6Route[n48m6::kRouteBadPipe];   // refused presents (all reasons)
            out[10] = gD.m6NoCopy[1] + gD.m6NoCopy[2];                    // performs completed without a copy (pipes on the monitor A / the monitor B)
            out[11] = gD.m6Txn[n48m6::kInstCount];                        // submits on a pipe with no known framebuffer
            out[12] = gD.m6Route[n48m6::kRouteOk];                        // DP presents the guard let through
        } else if (arg == 1ull) {                                         // page 1: stamps per OTG, the guard's reasons, and the AGDC answers
            for (unsigned i = 0; i < n48m6::kInstCount; ++i) { out[2 + i] = gD.m6Stamp[i]; out[5 + i] = gD.m6StampFail[i]; }
            out[8] = gD.m6Period[0]; out[9] = gD.m6Period[1]; out[10] = gD.m6Period[2];
            { const uint64_t f[4] = { gD.m6Route[n48m6::kRouteUnknown], gD.m6Route[n48m6::kRouteAmbiguous], gD.m6Route[n48m6::kRouteWrongInst], gD.m6Route[n48m6::kRouteNoId] };
              out[11] = n48m6::pack_sat(f, n48m6::kPackRoute, 4u); }       // 0.0.660 (N2): each 16-bit field SATURATES (the CLI reads 65535 as "65535 or more")
        } else if (arg == 4ull) {                                         // page 4 (0.0.662, Stage 2 item 10): the AGDC replies per instance and the endpoint values seen
            uint64_t a2[12] = { 0 };
            navi48_agdc_m6_stat2(a2);
            for (unsigned i = 0; i < n48m6::kInstCount; ++i) { out[2 + i] = a2[i]; out[5 + i] = a2[3 + i]; }     // out[2..4]: 0x921 replies for the DP / monitor A / monitor B endpoints; out[5..7]: 0x711 replies
            out[8] = a2[6]; out[9] = a2[7]; out[10] = a2[8]; out[11] = a2[9];                                      // endpoint bitmask seen, nfb of the list, out-of-range endpoints, the last endpoint dword
        } else if (arg == 3ull) {                                         // page 3 (0.0.660, K1): the pipes' clocks and the table's self-healing
            const uint64_t now = now_ns();
            for (unsigned i = 0; i < n48m6::kInstCount; ++i) { out[2 + i] = n48m6::txn_age_ms(now, __atomic_load_n(&gD.m6LastTxnNs[i], __ATOMIC_RELAXED)); out[5 + i] = gD.m6Surf.txn[i]; }   // ms since each pipe's last transaction (~0 = none since boot); learned submits since the last reset
            out[8] = gD.m6Surf.learn[n48m6::kLReplaced]; out[9] = gD.m6Surf.learn[n48m6::kLEvicted];
            { const uint64_t a[2] = { gD.m6Surf.resets, gD.m6Surf.touches }, b[2] = { gD.m6Pub.published, gD.m6Pub.coalesced };
              out[10] = n48m6::pack_sat(a, n48m6::kPackResetTouch, 2u);  // table resets (low 16 bits) | routed-present sightings (high 48)
              out[11] = n48m6::pack_sat(b, n48m6::kPackPair32, 2u); }     // property writes made (low 32) | publishes folded into another publisher's (high 32)
        } else {                                                          // page 2: the surface IDs themselves (3 per pipe expected) and the learn bookkeeping
            for (unsigned i = 0; i < n48m6::kInstCount; ++i) {
                unsigned k = 0;
                for (uint32_t j = 0; j < gD.m6Surf.n && j < n48m6::kMaxSurf && k < 3u; ++j) if (gD.m6Surf.e[j].inst == i) out[2 + i * 3u + k++] = gD.m6Surf.e[j].id | ((uint64_t)(gD.m6Surf.e[j].flags & 1u) << 32);
            }
            out[11] = gD.m6Learned | (gD.m6Published << 32);
            { const uint64_t f[8] = { gD.m6LearnSkip[n48m6::kSkTxn], gD.m6LearnSkip[n48m6::kSkPipeMismatch], gD.m6LearnSkip[n48m6::kSkPlane], gD.m6LearnSkip[n48m6::kSkClass], gD.m6LearnSkip[n48m6::kSkIdRead],
                                      gD.m6Surf.learn[n48m6::kLEvicted], gD.m6Surf.learn[n48m6::kLBadInst], gD.m6Surf.learn[n48m6::kLBusy] };      // 0.0.660: the 6th field is now evictions (a full table evicts, it never refuses)
              out[12] = n48m6::pack_sat(f, n48m6::kPackLearnSkip, 8u); }       // 0.0.660 (N2): each 8-bit field SATURATES at 255
        }
        uint64_t ag[8] = { 0 };
        navi48_agdc_m6_stat(ag);
        if (arg == 1ull) out[12] = n48m6::pack_sat(ag, n48m6::kPackAgdc, 4u);                              // AGDC: nfb of the last 0x980 reply | 0x921 replies for non-DP endpoints | 0x711 for non-DP | the last endpoint dword seen (each field saturates, 0.0.660)
        if (arg == 3ull) { const uint64_t o2[2] = { ag[5], ag[6] }; out[12] = n48m6::pack_sat(o2, n48m6::kPackPair32, 2u); }       // AGDC endpoints outside the 0x980 list: how many (low 32) | the last one (high 32)
        DLOG("m6stat page %llu: latch %u adopted %d armed %d pipes %u table %u IDs (DP %u, monitor A %u, monitor B %u; %u ambiguous); submits DP %llu monitor A %llu monitor B %llu none %llu; guard ok %llu refused %llu (unknown %llu ambiguous %llu wrong-instance %llu no-id %llu bad-pipe %llu); no-copy monitor A %llu monitor B %llu; stamps DP %llu monitor A %llu monitor B %llu (failed %llu/%llu/%llu); AGDC nfb %llu 921-nonDP %llu 711-nonDP %llu",
             (unsigned long long)arg, on, (int)(__atomic_load_n(&gD.adopted, __ATOMIC_ACQUIRE) != 0u), (int)(__atomic_load_n(&gD.armed, __ATOMIC_ACQUIRE) != 0u), KernelEnv::cap_n(), (unsigned)gD.m6Surf.n,
             n48m6::count_for_inst(gD.m6Surf, 0), n48m6::count_for_inst(gD.m6Surf, 1), n48m6::count_for_inst(gD.m6Surf, 2), n48m6::count_ambiguous(gD.m6Surf),
             (unsigned long long)gD.m6Txn[0], (unsigned long long)gD.m6Txn[1], (unsigned long long)gD.m6Txn[2], (unsigned long long)gD.m6Txn[3], (unsigned long long)gD.m6Route[n48m6::kRouteOk],
             (unsigned long long)(gD.m6Route[1] + gD.m6Route[2] + gD.m6Route[3] + gD.m6Route[4] + gD.m6Route[5]), (unsigned long long)gD.m6Route[n48m6::kRouteUnknown], (unsigned long long)gD.m6Route[n48m6::kRouteAmbiguous],
             (unsigned long long)gD.m6Route[n48m6::kRouteWrongInst], (unsigned long long)gD.m6Route[n48m6::kRouteNoId], (unsigned long long)gD.m6Route[n48m6::kRouteBadPipe], (unsigned long long)gD.m6NoCopy[1], (unsigned long long)gD.m6NoCopy[2],
             (unsigned long long)gD.m6Stamp[0], (unsigned long long)gD.m6Stamp[1], (unsigned long long)gD.m6Stamp[2], (unsigned long long)gD.m6StampFail[0], (unsigned long long)gD.m6StampFail[1], (unsigned long long)gD.m6StampFail[2],
             (unsigned long long)ag[0], (unsigned long long)ag[1], (unsigned long long)ag[2]);
        if (arg == 3ull) DLOG("m6stat page 3: last-transaction age ms DP %llu monitor A %llu monitor B %llu (%llu = none since boot); learned submits since reset %u/%u/%u; re-learned %llu evicted %llu resets %llu; property writes %llu coalesced %llu; routed-present sightings %llu; AGDC endpoints outside the list %llu (last %llu)",
                              (unsigned long long)out[2], (unsigned long long)out[3], (unsigned long long)out[4], (unsigned long long)n48m6::kAgeNever, (unsigned)gD.m6Surf.txn[0], (unsigned)gD.m6Surf.txn[1], (unsigned)gD.m6Surf.txn[2],
                              (unsigned long long)out[8], (unsigned long long)out[9], (unsigned long long)gD.m6Surf.resets, (unsigned long long)gD.m6Pub.published, (unsigned long long)gD.m6Pub.coalesced, (unsigned long long)gD.m6Surf.touches, (unsigned long long)ag[5], (unsigned long long)ag[6]);
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
