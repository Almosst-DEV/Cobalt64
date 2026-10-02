//
//  AppleHardwareHook.hpp — stop Apple's accelerator from programming gfx12
//  memory-controller registers with GFX10 offsets.
//
//  WHY THIS EXISTS
//  ---------------
//  Kext 0.0.52 hard-hung the machine: no SSH, no ICMP, power-cycle only. The
//  cause, found statically afterwards:
//
//      AMDRadeonX6000_AMDHWGart::flushAndInvalidateCaches(start, size)
//          hardware->vtbl[0x1f0]()          // GFX10Hardware::flushHDPCache()
//          vmm = hardware->vtbl[0x2e8]()
//          vmm->vtbl[0x238](&info)          // GFX10VMM::programAndInvalidateVM()
//
//  Both write registers at GFX10 offsets, and the second POLLS an invalidate-ACK
//  bit. On gfx12 those registers are elsewhere, so the ACK never sets and the
//  kernel spins forever with no panic and no log. That is the hang, exactly.
//
//  HOW WE GET A HANDLE ON APPLE'S HARDWARE OBJECT
//  ----------------------------------------------
//  For free. `_TtlLibraryInitializationInput.pGmmCallbacks->context` is the
//  RTHardware instance itself — initializeTtl builds that callback block out of
//  `this`. So Navi48Ttl::initialize() already holds the pointer.
//
//  The VM manager does not exist yet at that moment (Hardware::init allocates it
//  much later), so we cannot patch it directly. Instead we hook the ACCESSOR:
//  Hardware slot 93 returns the VM manager, so our replacement calls the original,
//  patches the object it hands back the first time it sees one, and returns it.
//  By the time anything calls invalidate, the vtable is already ours.
//
#pragma once
#include <stdint.h>
#include "ws_resprov.h"   // : n48_rp_copy (pure C, no kernel types)
#include "gfx_heapgen.h"  // build 0.0.495: n48_hg_patch / n48_hg_poison (pure C, no kernel types)

namespace n48 {

struct HwHookResult {
    bool        installed { false };
    const char *why       { "not attempted" };
};

// Patch Apple's Hardware instance so the two register-touching calls in the GART
// path become ours. `hardware` is pGmmCallbacks->context from TTL initialize().
HwHookResult install_hardware_hooks(void *hardware);

// How many times each replacement ran, for the log and the user client.
uint32_t hw_hook_hdp_flushes();
uint32_t hw_hook_vm_invalidates();
uint32_t hw_hook_reg_writes();   // MMIO writes intercepted and blocked

// Drive AMDHardware::setMemoryAllocationsEnabled (vtable slot 78). See the
// implementation for why: it is what populates AMDHWVMM::0x28, which
// endVMPTUpdate dereferences.
// Must be called BEFORE hw_hook_enable_memory_allocations(): it is what carves
// the 68 MiB page-table region out of reserved VRAM and records its address in
// AMDHWVMM::0x50. Without it the memory-allocations cascade asks to reserve VRAM
// at physical address 0 and is refused, leaving the VM block allocators NULL.
// Hands the VRAM arenas to Apple's memory-manager allocators. Nothing in the
// accelerator's start path ever does this; its only caller, powerUpHW(), is
// never reached and panics if called (it programs GFX10 VM registers).
// Mirror Apple's GART mappings into our own page table (GFX12 PTEs).
// Opt-in via boot-arg navi48-mirror-gart=1. UNTESTED on hardware.
// 0.0.386 — derive the host-page range guard's RAM top from the EFI memory map, once, at start().
// READ-ONLY and idempotent: it maps boot_args' descriptor array, validates it, and either widens the guard's bound or
// leaves it at 0.0.385's 0x800000000 constant. It NEVER narrows it. Safe to call before anything else is set up; it
// touches no hardware register and installs no hook. See gfx_ramtop.h for the derivation and its refusals.
void hw_hook_ramtop_init();
// 0.0.612 (review item A): the top of DRAM as the EFI memory map derived it (the first byte above the highest RAM descriptor), or 0 when the map was not read cleanly (the guard then sits on the
// legacy 32 GiB constant, which is BELOW real RAM on this machine: not a bound to refuse pages by). Read-only.
uint64_t hw_hook_ramtop_derived_or_zero();

bool hw_hook_mirror_apple_gart();
bool hw_hook_enable_vram_allocations();
// Option 2: reformat Apple's already-built GART PTE array (AMDHWGart+0x50)
// into our GFX12 page table in one pass. Returns pages synced. See.
uint32_t hw_hook_sync_apple_gart();
bool hw_hook_set_virtual_space_ready(bool ready);
bool hw_hook_enable_memory_allocations(bool enable);

// Enable Apple's SDMA rings so its own submit path stops bailing.
//
//: nothing ever calls AMDGFX10SDMARing::enable(), so ring->0xa8 stays 0, so
// every AMDHWChannel::submitCommandBuffer bails at its first branch and the ring
// is never filled. Its only caller, AMDGFX10SDMAEngine::start(), is reached only
// through a vtable and never runs. This calls enable() directly, which is
// idempotent (`if (this->0xa8 & 1) return 1`) and touches no hardware once the
// register shadow is armed. Returns the number of rings enabled.
uint32_t hw_hook_enable_sdma_rings();

// Run Apple's own AMDHardware::startHWEngines (vtable slot 195). That is what
// fills ring->0xc0 (the doorbell) via the queue manager before calling enable().
// Enabling a ring without it panics in AMDRTRing::writeTail on the first submit.
uint32_t hw_hook_start_hw_engines();

// Read-only. Reports each ring's wptr, doorbell word and tail writeback.
// A non-zero doorbell word is direct proof that AMDRTRing::writeTail ran,
// i.e. Apple submitted and advanced the ring. Touches no hardware.
uint32_t hw_hook_report_ring_state();

// Read-only. Dumps what Apple wrote into each ring (ring->0x40 is the CPU base,
// ring->0x30 the size in dwords). requires decoding these packets and
// verifying every embedded address lies inside the GART before any queue is
// programmed to fetch them. Programs nothing; rings no doorbell.
uint32_t hw_hook_dump_rings();

// step 3, SAFE form: point SDMA1 QUEUE1 at Apple's ring with RB_ENABLE=0.
// Re-verifies ring alignment, power-of-two size and PTE residency first, and
// rings no doorbell. The engine fetches nothing.
//: follow the ring's INDIRECT packets and dump the buffers they point at.
// The engine executes these once enabled, so every address inside must be
// decoded and range-checked too. Read-only.
uint32_t hw_hook_dump_indirect_buffers();

//: rebase the CONST_FILL destinations in Apple's IB by fb_start, so they
// address local VRAM instead of faulting through an unprogrammed VM context.
// Verifies every packet against the reference layout AND against Apple's expected
// arena before writing anything; refuses wholesale on any mismatch. Returns the
// number of packets successfully rebased, 0 on refusal.
uint32_t hw_hook_rebase_ib_fills();

// enableRing=true sets RB_ENABLE=1: the engine really will fetch. Everything in
// (residency, decoded addresses) must hold first.
uint32_t hw_hook_program_apple_queue(bool enableRing = false);

// Ring the QUEUE1 doorbell with Apple's own wptr. enablequeue leaves WPTR=0, so
// this is the call that actually makes the engine fetch and execute. Run
// hw_hook_rebase_ib_fills() first.
uint32_t hw_hook_kick_apple_queue();

// Read the dwords the ring gates on (COND_EXE) and writes to (FENCE), to tell
// "the engine consumed the ring" apart from "the engine executed the work".
// Read-only.
uint32_t hw_hook_dump_ring_refs();

// Write the COND_EXE reference value into the gate Apple left clear, so the next
// kick actually executes the ring. Safe only because every packet behind the gate
// is already decoded and range-checked. Read-modify-verify.
uint32_t hw_hook_open_cond_exec_gate();

// Rewrite register-mode POLL_REGMEM packets that have a non-zero mask into the
// always-true form Apple uses everywhere else (ref=0, mask=0). Targets exactly the
// VM-flush ack poll that can never satisfy. Memory-mode polls untouched.
uint32_t hw_hook_neuter_register_polls();

// Dump the per-channel completion dword Apple's scheduler polls (channel+0xc0).
// Read-only. See: completion is polled memory, not an interrupt.
uint32_t hw_hook_dump_channel_completion();

// Write a probe through chan+0xc0 and read it back via GART to decide whether the
// completion slot and the FENCE target are one frame or two buffers, then set the
// completion dword to the value the ring's FENCE carries.
uint32_t hw_hook_poke_completion();

//: the completion dword is correct and Apple never reads it, because
// AMDSWScheduler::timestampUpdated reads through a pointer it is HANDED and
// nothing invokes it (our kext owns the IH, so Apple's ISR never runs). This
// calls AMDRadeonX6000_AMDHWChannel::timestampUpdated on chan 14 -- signature
// confirmed twice, by disassembly (only %rdi is read) and by the symbol's
// mangled `Ev` suffix. Geometry-checked at both ends; refuses on any mismatch.
uint32_t hw_hook_signal_completion();

//: chan+0xe0 == -1 means AMDHWChannel::init never registered chan 14 with
// the scheduler, so timestampUpdated takes path B (cache the stamp, no retire loop).
// This replays init's own sequence: get the scheduler from chan->0x20 vtbl[0x3b0],
// call AMDSWScheduler::addHWChannel(sched, chan, id), and ONLY on success set
// chan+0xe0 = id. addHWChannel is safe post-hoc -- it refuses NULL, id >= 32 and an
// already-occupied slot, each with no side effects.
uint32_t hw_hook_bind_channel();

//: READ-ONLY dump of the scheduler's HW-channel slot for chan 14. Every claim
// about sched+0x58 so far is static disassembly plusa's one-off trace, and the
// last two theories built without these numbers were both wrong.
// Uses Apple's own bounds-checked accessor getTimestampStat(id) to LOCATE the slot
// rather than reconstructing the indexing by hand.
uint32_t hw_hook_sched_state();

//: read-only dump of the STAMP array the Metal client's waitForStamp
// resolves against (accel+0x1f30 + index*64, bound at accel+0x1e78). showed the
// scheduler's own accounting completes while blit2 stays Scheduled, so the question
// is whether this array - one layer up - ever advances. Locates Apple's accelerator
// by metaCast in the service plane, exactly as Navi48AccelPeer does.
uint32_t hw_hook_stamp_state();

//: call AMDHWHandler::signalChannelStamp (vtable slot 0x230) -- the ONLY writer
// of the stamp array measured as permanently zero. Verifies by IDENTITY that
// chan+0x38 really is an AMDHWHandler before calling, so a wrong hypothesis costs a
// log line rather than a panic. Leaves chan+0xe0 alone, so path A keeps retiring the
// scheduler while the stamp is written alongside.
uint32_t hw_hook_signal_stamp();
uint32_t hw_hook_stamp_gap();
// 0.0.276, action 61 `gfxcensus [1|2]`: the read-only census of Apple's GFX frames and the IBs they name.
uint32_t hw_hook_gfx_census(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.278, action 62 `gfxneuter [1|2]`: VMID-2 INDIRECT_BUFFER packets NOPed in Apple's GFX ring before the doorbell.
uint32_t hw_hook_gfx_neuter(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.282: action 64 `gfxcapture [1|2]` - READ-ONLY capture at the source hook into the binary capture ring.
uint32_t hw_hook_gfx_capture(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.283: action 65 `gfxprobe [1|2]` - the copy-back probe (fills SecurityAgent's last VRAM drawable with magenta).
uint32_t hw_hook_gfx_probe(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.286: the display-pipe safety core's GFX-ring WRITE_DATA guard (mode 1 arms, 0 reads) and whether the GFX
// writeTail hook it lives in is installed this boot.
uint32_t hw_hook_dpg_writedata(uint32_t mode, uint64_t *out, unsigned count);
bool hw_hook_gfx_ring_hooked(void);
uint32_t hw_hook_run_checktimestamps();
uint32_t hw_hook_run_advance();

// MILESTONE 3 step 1 (0.0.235) — the IH -> Apple completion bridge. On an
// end-of-pipe IH entry, call Apple's own AMDSWScheduler::checkTimestamps so a
// command buffer retires WITHOUT a blocked client and without the forced
// `runcheckts` verb. Armed by `accel eopbridge 1` or boot-arg
// navi48-eop-bridge=1; inert otherwise. See the long derivation in the .cpp and
// notes/MILESTONE3-DESIGN.md Q2.
//   fire()    0 refused/disarmed, 1 retired something, 2 nothing outstanding,
//             3 called and nothing moved.
//   control() mode 1 arm, 2 disarm, else read; fills out[0..10] with
//             armed, entries, calls, skipped, retired, refused, lastReason,
//             lastChan, lastSubmitted, lastCompleted, lastAfter.
uint32_t hw_hook_eop_bridge_fire(uint32_t clientId);
// 0.0.270: the same bridge for the SDMA trap (IH client 0x0a src 49), one entry per drained frame.
uint32_t hw_hook_sdma_trap_bridge_fire(uint32_t clientId);
uint32_t hw_hook_eop_bridge_control(uint32_t mode, uint64_t *out, unsigned count);

// MILESTONE 3 step 3 (0.0.237) — the boot chain. Folds the per-boot ssh verb
// sequence into the kext's own start path as standing hooks, behind the boot-arg
// navi48-boot-chain (a bitmask, default 0 = OFF; bit 0 = the accelerator-start
// chain, bit 1 = the submission-time takeover). Every verb keeps working.
//   configure()  set the mode, from start().
//   arm()        called at Navi48AccelPeer's kReqAccelStarted; starts Phase A on
//                its own kernel thread (pm4powerup blocks 5 s and must not stall
//                Apple's accelerator start). Arms once per boot.
//   state()      0 idle, 1 running, 2 done, 3 failed; fills out[0..12].
void     hw_hook_boot_chain_configure(uint32_t mode);
// mach uptime in microseconds, the clock every boot-chain and context-latch line is stamped with (M4-CONTEXT-LATCH.md).
uint64_t hw_hook_uptime_us();
void     hw_hook_boot_chain_arm();
uint32_t hw_hook_boot_chain_state(uint64_t *out, unsigned count);
// 0.0.269: `drain` (action 58), READ-ONLY - the drain's state and counters (13 out-scalars) and a
// summary with the per-ring and per-pid tables in the log. The drain itself arms from the boot chain (bit 2).
uint32_t hw_hook_drain_state(uint64_t *out, unsigned count);

// Install observation-only hooks on Apple's SDMA ring (submit/getHead/writeTail,
// vtable slots 43/45/47). Each calls Apple's original and logs. Nothing is
// substituted and no doorbell is rung -- wiring the kick is a separate step.
uint32_t hw_hook_install_ring_hooks();

//: translate Apple's GCVM register operands (+0x28) in every IB on
// chan 14 so its CONTEXT2 setup and ENG6 invalidate address the registers it
// means. Two-sided guard; MC aperture sources are never redirected.
uint32_t hw_hook_translate_gcvm_regs();

//: call AMDSWScheduler::resume() to clear the pause a failed reset left set.
uint32_t hw_hook_resume_scheduler();

// 283: call GFX10PM4Engine::powerUp on the PM4 engine so doStart fills
// engine+0x340 - the NULL preempt() dereferenced. Exact-address guarded.
uint32_t hw_hook_pm4_power_up();

//: install an IOMemoryDescriptor through AMDHWMemory::setVirtualSpace (slot
// 37) so mem+0x98 bit 0 is set - the gate initComputeMQD(4) fails on.
uint32_t hw_hook_set_virtual_space();

//: enable the KIQ ring (a GFX10ComputeRing) so submitKIQFrame stops
// refusing. Exact-address guarded on GFX10ComputeRing::enable @0xbe1d120.
uint32_t hw_hook_kiq_enable();

// route 1 — the KIQ stamp instrument. See the long derivation in the .cpp:
// submitKIQFrame @0xbe18956 increments chan+0x80 and passes THAT value to
// waitForHwStamp, which returns true as soon as `stamp - chan+0x84 <= 0`
// (0xbe08852, again at 0xbe088e0, both re-reading +0x84 from memory).
//
// The cross-file accessor both verbs answer through. Filled even on a refusal
// (with whatever was actually read), because rule 14 says the result must have a
// return path that no log flood can erase.
//
// 0.0.176 — EXACTLY 13 fields, and that is a hard ceiling, not a style choice:
// IOConnectCallScalarMethod's io_scalar_inband64_t is uint64_t[16]
// (device_types.h:105) and outExtra starts at scalarOutput[3], so extras 3..15
// is all there is. Adding a 14th field silently loses it on the way out.
struct KiqStampInfo {
    uint64_t chan;        // the KIQ AMDGFX10KIQHWChannel at PM4 engine +0xe0
    uint32_t id;          // chan+0x0c — must read 1
    uint32_t submitted;   // chan+0x80, read live
    uint32_t completed;   // chan+0x84, read live
    // 0 idle, 1 armed, 2 fired (awaiting propagation), 3 gave up on the 20 s
    // bound, 4 settled (completed == submitted and quiet for 2 s).
    // BIT 8 is a separate fact, not part of the state: set when chan+0xe0 reads
    // -1, i.e. AMDHWChannel::timestampUpdated @0xbe08502 takes PATH B (the raw
    // `[0x84] = *[0xc0]` copy) rather than the SWScheduler path.
    uint32_t pollState;
    uint32_t baseline;    // (A) chan+0x80 sampled at ARM time
    uint32_t fires;       // (B) write-back-dword writes the poller has made
    uint32_t fallbacks;   // direct chan+0x84 writes after 300 ms of no propagation
    uint32_t lastWritten; // the stamp value written by the most recent fire
    uint32_t lastPropMs;  // ms from that fire to completed == submitted; ~0u = never
    uint64_t wbPtr;       // [chan+0xc0] — the dword the GPU would write
    uint32_t wbValue;     // *[chan+0xc0], read live
    // (C) the KIQ ring dump has NO field here on purpose: 13 is the ceiling and
    // a 64-dword hex dump could not ride a scalar anyway. It is log-only.
};

// Action 34: verify the channel's identity, sample chan+0x80 as a BASELINE, then
// ARM a bounded kernel-thread poller. On each submission past the baseline it
// EMULATES THE GPU — writing the write-back dword at *[chan+0xc0] — and then
// keeps polling to see whether Apple's own checkForTimestampUpdate propagates it
// into chan+0x84, falling back to a direct +0x84 write after 300 ms. It must be a
// thread, not the work loop: pm4powerup blocks inside waitForHwStamp for 5 s and
// can be holding the gate. Writes 32-bit words only; no register writes.
uint32_t hw_hook_kiq_stamp(KiqStampInfo *out);

// Action 35: read-only. The same channel state, everything the poller recorded,
// and a dump of the KIQ ring (chan+0x30) so the MAP_QUEUES frame Apple submitted
// is visible. Readable after pm4powerup returns.
uint32_t hw_hook_kiq_chan_state(KiqStampInfo *out);

// ---- 0.0.178 "gfxmap" (36) / "gfxstate" (37) -----------------------------
//
// What Navi48Ttl::startEngineQueue(queue=9) was handed and what it answered.
// initGraphicsMQD @0xbe254cd builds the IN struct and @0xbe2552f calls TTL slot
// 0x120 with `mov esi, 9`; on success it stores OUT+0x00 into ring->0xc0
// (@0xbe2555b, the doorbell POINTER) and OUT+0x08 into engine+0x248
// (@0xbe25565, the doorbell DWORD INDEX doStart later passes to
// submitMapQueuesPacket). Recording both is what lets the emulator work from
// the values Apple actually used rather than from constants.
struct AppleGfxQueueParams {
    bool     valid         { false };
    uint64_t ringGpu       { 0 };   // IN+0x08, = ring->0x48
    uint32_t sizeBytes     { 0 };   // IN+0x00 (0x80000)
    uint64_t wptrWbGpu     { 0 };   // IN+0x18, = ring->0xd0
    uint64_t mqdGpu        { 0 };   // IN+0x20, = engine+0x250 (VRAM-relative)
    void    *mqdCpu        { nullptr }; // IN+0x28
    uint32_t doorbellIndex { 0 };   // what we put at OUT+0x08
    void    *doorbellPtr   { nullptr }; // what we put at OUT+0x00
};
void hw_hook_note_apple_gfx_queue(const AppleGfxQueueParams &p);
// True once `accel gfxmap` has armed the emulator. startEngineQueue consults it
// before handing Apple a REAL BAR2 doorbell: with no emulator there is no
// takeover, so a real doorbell would point at OUR still-mapped kernel GFX queue.
bool hw_hook_gfxmap_armed();

// 13 fields exactly — the same hard ceiling KiqStampInfo documents
// (io_scalar_inband64_t is uint64_t[16] and outExtra starts at scalarOutput[3]).
struct GfxMapInfo {
    uint32_t armed;        // the emulator is running
    uint32_t proceed;      // the takeover was VERIFIED; the gate shadow is off
    uint32_t gateShadows;  // CP_RB0_RPTR reads answered with 1 instead of 0
    uint32_t frames;       // 32-dword KIQ frames decoded
    uint32_t stamps;       // WRITE_DATA packets emulated
    uint32_t refusals;     // WRITE_DATA packets refused (wrong destination)
    uint32_t mapResult;    // 0 not attempted, 1 MAPPED, 2 refused
    uint32_t failStep;     // see Navi48GfxTakeover::failStep
    uint32_t addKr;        // MES ADD_QUEUE IOReturn
    uint32_t removeKr;     // MES REMOVE_QUEUE IOReturn
    uint64_t mqdGpu;       // the MQD we handed the MES
    uint32_t doorbell;     // the doorbell DWORD index we mapped
    uint32_t lastPacket;   // (opcode << 8) | engine_sel of the newest frame
    // Not scalars — filled by gfxstate only, for the log and for navi48test's
    // register block. They ride in the same struct because the caller copies it
    // whole; the dispatch chooses which 13 go out.
    uint32_t regRptr, regWptr, regWptrHi, regCntl, regActive;
    uint32_t appleRingWptr;
};

// Action 36: arm the gfxmap emulator. SUPERSEDES kiqstamp — the two share the
// arm-once CAS, so whichever runs first refuses the other for the boot.
uint32_t hw_hook_gfx_map(GfxMapInfo *out);
// Action 37: read-only. Emulator state, the live gate registers, Apple's GFX
// ring header and content, and the two write-backs whose advance proves the CP
// consumed that ring.
uint32_t hw_hook_gfx_state(GfxMapInfo *out);

// ---- 0.0.185 "sdmamap" (38) / "sdmastate" (39) ---------------------------
//
// What Navi48Ttl::startEngineQueue was handed and what it answered for ONE SDMA
// (queue, inst) pair. AMDGFX10SDMAEngine::start @0xbe278c0 builds the IN struct
// on its own stack and consumes the OUT struct like this [CONFIRMED, bytes]:
//   0xbe2792a  movl $0x40000,-0x78(%rbp)            IN+0x00 = ring bytes
//   0xbe27931  movq 0x48(%r13),%rax ; ->-0x70       IN+0x08 = ring->0x48 (GPU VA)
//   0xbe27939  movq 0xd0(%r13),%rax ; ->-0x60       IN+0x18 = ring->0xd0 (wptr wb)
//   0xbe27962  callq *0x120(%rax)                   TTL slot 36
//   0xbe27978  48 8b 45 c0        movq -0x40(%rbp),%rax
//   0xbe2797c  49 89 85 c0 00 00 00  movq %rax,0xc0(%r13)   OUT+0x00 -> ring->0xc0
//   0xbe2798a  callq *0x130(%rax)                   ring enable()
// so OUT+0x00 IS the doorbell pointer, written exactly once, at engine start.
struct AppleSdmaQueueParams {
    bool     valid         { false };
    uint32_t queue         { 0 };   // AmdSwipQueueType: 7 = QUEUE0, 10 = QUEUE1
    uint32_t inst          { 0 };
    uint64_t ringGpu       { 0 };   // IN+0x08 = ring->0x48
    uint32_t sizeBytes     { 0 };   // IN+0x00 (0x40000)
    uint64_t wptrWbGpu     { 0 };   // IN+0x18 = ring->0xd0
    void    *doorbellPtr   { nullptr }; // what we returned at OUT+0x00
    uint32_t doorbellIndex { 0 };   // 0 unless we handed out a real BAR2 dword
    bool     realDoorbell  { false };
};
void hw_hook_note_apple_sdma_queue(const AppleSdmaQueueParams &p);
// True once `accel sdmamap` has armed the takeover. 0.0.196: startEngineQueue
// consults hw_hook_sdmamap_doorbell_for() per (queue type, inst); a queue with
// no mapping keeps the private aperture word it has always been handed.
bool hw_hook_sdmamap_armed();
// 0.0.196: has `sdmamap` claimed a hardware queue for THIS (queue type, inst)?
// Slot 36 asks per queue instead of testing a hardcoded (10,1).
bool hw_hook_sdmamap_doorbell_for(uint32_t queue, uint32_t inst,
                                  uint32_t *dbIndexOut);

// 13 fields exactly — the io_scalar_inband64_t ceiling KiqStampInfo documents.
struct SdmaMapInfo {
    uint32_t armed;        // the takeover is in force this boot
    uint32_t mapResult;    // 0 not attempted, 1 MAPPED, 2 refused
    uint32_t failStep;     // which refusal (see kSdmaMapStep* in the .cpp)
    uint32_t doorbell;     // the doorbell DWORD index we programmed
    uint64_t ringGpu;      // Apple's SDMA ring GPU VA (ring->0x48)
    uint32_t ringDwords;   // ring->0x30
    uint32_t ringWptr;     // ring->0x58, live
    uint64_t rptrReport;   // ring->0xb8 — READ, not assumed
    uint64_t wptrWb;       // ring->0xd0
    uint32_t regRptr;      // QUEUE1 RB_RPTR
    uint32_t regWptr;      // QUEUE1 RB_WPTR
    uint32_t regCntl;      // QUEUE1 RB_CNTL
    uint32_t fenceValue;   // the dword at the channel frame base (the FENCE target)
    // Not scalars — log-only, filled by both verbs.
    uint32_t regDoorbell, regDoorbellOff, ringFlags, q1TestResult;
    uint64_t oldDoorbellPtr, newDoorbellPtr, frameBase;
    // 0.0.196: how many Apple rings this run put on hardware and how many it
    // could not. They ride back packed into one out-scalar, so the 13-scalar
    // ABI ceiling is unchanged.
    uint32_t mappedCount, unmappedCount;
};

// Action 43 (`ringib`, 0.0.196): read-only, writes nothing anywhere. Decodes ONE
// Apple channel's SDMA ring packet by packet and then dumps and decodes every
// INDIRECT_BUFFER it references, read through our GART. `chanId` 0 means chan 14
// (the ring proved executes); any other value selects that channel id.
// Prints the (queue type, inst) the ring maps to, from slot 36's record.
// Returns the number of IBs decoded + 1, or 0 when it refused.
uint32_t hw_hook_ring_ib_dump(uint64_t chanArg);

// Action 38: ARM the SDMA takeover. 0.0.196: for EVERY Apple SDMA ring that has
// work (chan 14 first, then chan 13, then the rest with wptr > 0), PTE-checks
// every page of the ring and of its write-back frame, HDP-flushes, programs the
// next free hardware queue from amdgpu::kSDMAExtSlots (SDMA0/SDMA1 QUEUE1..7,
// QUEUE0 on both stays ours) with Apple's own write-back addresses and that
// slot's doorbell dword, and rewrites ring->0xc0 to the real BAR2 doorbell —
// but ONLY if ring->0xc0 still holds the exact private word our own TTL slot 36
// handed out for that (queue type, inst), which is the identity check. Rings
// with wptr 0 are skipped and logged; rings with work but no queue left are
// logged UNMAPPED. Idempotent: a channel already mapped is left alone.
//
// Rewriting ring->0xc0 after the queue was started is safe because
// AMDRTRing::writeTail re-reads it on every call and caches it nowhere
// [CONFIRMED @0xbe1bbc1: 49 8b 86 c0 00 00 00 movq 0xc0(%r14),%rax ;
//  48 89 18 movq %rbx,(%rax)].
uint32_t hw_hook_sdma_map(SdmaMapInfo *out);
// Action 39: read-only. EVERY hardware queue slot's registers (RB_RPTR/WPTR/
// CNTL/DOORBELL) with its mapped channel and that ring's fence dword, then
// Apple's chan-14 ring header, its wptr write-back and rptr report dwords, and a
// decode of the ring's first dwords. Writes nothing, anywhere.
uint32_t hw_hook_sdma_state(SdmaMapInfo *out);

// Action 40 (`faultclear`, 0.0.193): pulse GCVM_L2_PROTECTION_FAULT_CNTL bit 0
// so the FIRST-FAULT-LATCHED status belongs to the NEXT run, printing the
// decoded word it discards and the one that replaces it. The only register it
// writes is that CNTL, and it writes back the value it read. Returns 1 when the
// status reads 0 afterwards, 2 when a fault re-latched immediately, 0 = refused.
uint32_t hw_hook_vm_fault_clear();

// Action 41 (`vmstate`, 0.0.193): read-only. GCVM_CONTEXT2 (VMID 2 = Apple's
// blit VM) CNTL/BASE/START/END, the latched fault status decoded by field, and
// four entries of Apple's own page-table arena read through the MM_INDEX window
// (root PDE, L1[0xa], and two sub-table PTEs) decoded in BOTH the gfx10 and the
// gfx12 reading. Writes nothing. Returns 1 + the number of probed entries that
// read as gfx12 encoding; 0 = refused.
uint32_t hw_hook_vm_arena_state();

// Action 42 (`vmib`, 0.0.194): read-only, writes nothing anywhere. Software-walks
// Apple's VMID-2 page table (GCVM_CONTEXT2 BASE/START -> root -> L1 -> the
// 16-entry sub-table) for `va` (0 ='s blit IB at 0x4000a0000), logging every
// entry decoded, then reads 0x80 dwords of the page it lands on and decodes them
// as TYPE3 PM4. A SYSTEM leaf is reached with an IOMemoryDescriptor over the host
// physical page (kIODirectionIn, released again); a VRAM leaf through MM_INDEX.
// Also logs CP_STAT / GRBM_STATUS / CP_RB0_RPTR and the gfx12 CP IB registers.
// Returns 0 refused, 1 root invalid, 2 L1 invalid, 3 leaf invalid, 4 the page
// could not be read, 5 dumped from VRAM, 6 dumped from a host page.
uint32_t hw_hook_vm_ib_dump(uint64_t va, uint64_t *outPhys, uint32_t *outFirstDword,
                            uint32_t *outCpStat);

// Action 47 (`vmpage`, 0.0.206): read-only, writes nothing anywhere. The same walk as
// `vmib` for `va` (0 = blit2's destination V# base 0x400004000), then reads the WHOLE
// 4 KiB page from offset 0, logs its nonzero 8-dword lines, and decodes the
// blit_diag_gfx1201 records (section 328): every dword equal to 0x600d600d (record A)
// or 0x600d0b0b (record B) is taken as a record's d3, with d0..d2 the three dwords
// before it. A built-in self-test runs the decoder over a planted buffer first (the
// positive control). out[0]=walk status (as vmib), out[1]=page, out[2]=self-test ok,
// out[3]=nonzero dwords, out[4]=A hits, out[5]=B hits, out[6]=hits at dword index
// 3 mod 4, out[7]=first hit dword index (0xffffffff none), out[8]=gcd of the gaps
// between hits in dwords, out[9]/out[10]=min/max d0 of A records, out[11]=bit n set
// when an A record has d0 == n (n < 63; bit 63 = d0 >= 63), out[12]=max d2 of all
// records. `count` is the size of `out`.
uint32_t hw_hook_vm_page_scan(uint64_t va, uint64_t *out, unsigned count);

// Action 52 (`vmroots`, 0.0.242): READ-ONLY, and it writes nothing in Apple's page
// tables or anywhere else. The read notes 388's sub-questions 1-3 need: every live
// GCVM_CONTEXTn page-table register; every CONTEXTn page-table BASE write Apple has
// queued in its own SDMA rings this boot (which does NOT need the takeover, since the
// writes are in ring memory whether or not an engine ran them), folded into the
// DISTINCT root tables Apple has named; a full dump of each of those root pages with
// every valid entry decoded and the 256 MiB VA range it covers; and the reserved-tail
// map with arithmetic that adds up, naming where a VRAM-backed GE ring region could be
// carved outside vram_alloc_hi. `arg` non-zero additionally dumps the root page at that
// address. out[0]=1+distinct tables, [1]=live CONTEXT2 root, [2]=valid entries in the
// first table, [3]=its low-64 valid bitmask, [4]=highest all-zero root index,
// [5]=distinct tables, [6]/[7]=first two distinct roots, [8]/[9]=Apple arena bottom/top,
// [10]/[11]=free tail base/size, [12]=(base writes << 32) | tables dumped.
// Returns 1 + the number of distinct root tables; 0 = refused.
uint32_t hw_hook_vm_root_dump(uint64_t arg, uint64_t *out, unsigned count);

// Action 53 (`ringmap`, 0.0.244): MILESTONE 3 step 4, increments (i) and (ii) of notes
//. Reserve the VRAM tail region r80 measured free, build the kext-owned
// page-directory block (0x8000 = 4096 entries) and its 168 64 KiB leaf entries over the
// GE ring region, and verify the result with OUR OWN walker. *** IT DOES NOT WRITE INTO
// APPLE'S PAGE TABLE. *** It writes only our own L1 block, in VRAM carved outside
// vram_alloc_hi and below Apple's arena; Apple's root page is READ-ONLY here, and the
// slot the design wants (511) is read back and reported so "nothing of ours is live" is
// a measurement. The single 8-byte root PDE that would make the mapping live is
// increment (iii), held behind the adversarial review asks for. arg low byte:
// 0 (default) reserve and report only, 1 also build and verify.
// out[0]=status (0 refused, 1 reserved read-only, 2 built and verified, 3 built but a
// check mismatched), [1]=refusal reason, [2]=ring region base, [3]=ring region bytes,
// [4]=L1 block offset, [5]=leaf count, [6]=L1 entries written, [7]=read-back mismatches,
// [8]=(walk probes << 32) | probe mismatches, [9]=ring VA base, [10]=root slot,
// [11]=Apple's root slot entry (READ, never written), [12]=free tail size.
uint32_t hw_hook_ring_map(uint64_t arg, uint64_t *out, unsigned count);

// Action 54 (`vmctx`, 0.0.247): MILESTONE 3 step 4, the OBSERVE BOOT that
// notes/M3-ROOT-WRITE-REVIEW.md.1 requires before increment (iii)'s single
// 8-byte root PDE write. *** READ-ONLY: neither this verb nor the VMM slot-40/41
// hooks it reports on write anything - not Apple's page tables, not Apple's
// objects, not a register, not VRAM. *** Run it WHILE A METAL CLIENT IS ALIVE.
// It reports, for every AMDHWVMContext Apple created this boot, the root
// page-table address read out of the context object that owns it
// (ctx+0x98+0x20) at create, NOW, and at release; cross-checks the live value
// against the root Apple's CONTEXT2 register names; reads root[0] and root[511]
// of each live root (review guards G3 and G4); and reads GFXHUB engine 17's
// invalidation range registers (review.2).
// out[0]=state bits (1 observe pair installed, 2 geometry refused, 4 VMM hooked),
// [1]=creates, [2]=releases, [3]=live contexts, [4]/[5]=root NOW of the first two
// live contexts, [6]=Apple's CONTEXT2-derived root, [7]=live roots agreeing with
// it, [8]=(ENG17 ADDR_RANGE_LO32 << 32) | HI32, [9]=root[511] of the first live
// context, [10]=(non-zero-at-create count << 32) | first root at create,
// [11]=most recent root at release, [12]=(table overflow << 32) | entries used.
uint32_t hw_hook_vm_context_observe(uint64_t arg, uint64_t *out, unsigned count);

// Action 55 (`rootwrite`, 0.0.250): MILESTONE 3 step 4 increment (iii) — THE SINGLE
// 8-BYTE ROOT PDE WRITE, the first write this project has ever made into a live
// Apple VM context. Everything around it has been inert over six boots;
// this is what makes our mapping live.
//
// arg 0 = REPORT ONLY: evaluate guards G1..G6 against the live state and say what
//         would happen. Writes nothing. This is the default, deliberately.
// arg 1 = PERFORM the write, but only if all six guards pass.
//
// The write is TWO separate navi48_vram_write_mm calls, HIGH dword first, then LOW
// (review.3: the helper writes ascending and VALID is bit 0 of the low dword, so
// a single 2-dword call would briefly arm a VALID PDE with a garbage target). Then
// read-back, HDP flush, and a GFXHUB VMID-2 TLB invalidate through our OWN MMIO —
// never Apple's invalidateVM, which patch_vmm has already made inert (review.1).
//
// The withdrawal is NOT here: it runs in hook_unmapVA on Apple's own teardown path
// (AMDHWVMContext vtable slot 37), at the last instant the page is still ours.
// RETRACTED's pageOffPD hook point - it never fires for a client context.
// 0.0.252 hooks slot 37 as an OBSERVER and implements no withdrawal at all, so it
// REFUSES arg 1 outright rather than arm an entry it could not take back.
// out[0]=status (0 refused, 1 report-only, 2 WRITTEN and verified, 3 written but a
// read-back mismatched), [1]=the guard that refused (0 = none, 0xFF = refused before
// the guards ran), [2]=the root page, [3]=the entry we wrote, [4]=the entry read
// back, [5]=root[511] before, [6]=our L1 block offset, [7]=ring VA,
// [8]=(hdp << 1) | tlb flush results, [9]=fault status after,
// [10]=(withdrawals << 32) | withdrawal refusals,
// [11]=(contexts patched << 32) | patch refusals,
// [12]=the four slot-37 counters as CLAMPED 16-bit fields:
//      (unmapVA fires << 48) | (root freed << 32) | (root survived << 16) | zero-at-entry.
// 13 extras is a hard ceiling (see kAccelExtraScalars), which is why these are packed.
uint32_t hw_hook_root_write(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.261: re-arm the root PDE AFTER Apple's deferred clearWithDMA has drained
// through our own sdmamap takeover, and dump every pending packet that touches the root
// page with no opcode filter. Argument 0 REPORTS ONLY; 1 performs the re-arm. The write
// goes through the single rootwrite_arm_context path - no new write machinery.
uint32_t hw_hook_rearm_after_drain(uint64_t arg, uint64_t *out, unsigned count);

// Action 45 (`flushdrop`, 0.0.202/0.0.203): an INSTRUMENT. Run after
// xlatregs and before sdmamap. Takes the blit IB from Apple's GFX ring when its frame
// is there, otherwise (0.0.203, the r31 case) finds the IB's host page by content among
// the leaves still pending in Apple's SDMA PTEPDE packets, checks
// the IB's identity, and rewrites the EVENT_WRITE CS_PARTIAL_FLUSH that follows the
// compute DISPATCH_DIRECT into two one-dword NOPs. Returns 0 refused (nothing
// written), 1 rewritten and read back, 2 already rewritten, 3 read-back mismatch.
// out[0..8]: status, IB VA, IB dwords, host page, dispatch dword, rewritten dword,
// PTEPDE entries, SDMA IBs read, read-back mismatches, pages scanned, matches.
uint32_t hw_hook_blit_flush_drop(uint64_t *out, uint32_t nOut);

// Action 48 (`renderxlat`,; 0.0.208's version retired): content search for Apple's render IB
// among the page-table leaves pending in its SDMA IBs, run after xlatregs and BEFORE sdmamap (rule 65).
// arg low byte: 0 census + dump, 3 BLANK (instrument: non-proven packets -> NOPs, same length), 1 translate
// in place (xlat12_ib, NOP-padded); 2 refused. Bit 0x100 seeds NGG. 13 out-scalars, layout at the definition.
uint32_t hw_hook_render_xlat(uint64_t arg, uint64_t *out, uint32_t nOut);

// : the residency copy as a provenance source (ws_resprov.h). hw_resprov_on: both the descriptor path and the
// residency-provenance switch (`accel gfxneuter 11 | 1 << 8`) are on. hw_resprov_note_copy: one finished residency copy; fills
// WHO (the bound WindowServer, its context key, the ledger's arm and epoch) and records it if every rule holds. Returns the
// N48_RP_REC_* reason (N48_RP_REC_REASONS + 1 off, + 2 DROPPED: the ledger's lock was busy and the pending queue full, + 3 QUEUED:
// the ledger's lock was busy, the copy waits in the pending queue for the lock's next holder -). `copyNo` names it in logs.
bool hw_resprov_on();
uint32_t hw_resprov_note_copy(n48_rp_copy *c, int32_t pid, uint64_t *ctxOut, uint64_t copyNo);
// build 0.0.450 item 1, switch 46 (DEFAULT OFF): admits the T450 golden-tested kinds (4KB_D_X@8/64bpp,
// 256B_D@8bpp) through n48_rp_retile_kind/n48_rp_shape_check_kind, in addition to the always-on 32bpp 4KB_D_X path.
// Acts only while hw_resprov_on() is also true, exactly like every other resprov behaviour.
bool hw_resprov_kinds_on();
// build 0.0.486, switch 59 (DEFAULT OFF; notes/design/STATIC-RETILE.md): the backing-sourced copy of a texture whose
// system-memory backing is LINEAR (ws_resprov.h section 6). hw_resprov_lin_on: hw_resprov_on() AND switch 59.
// hw_resprov_lin_note: one copy-side event (ws_resprov.h N48_RP_LINEV_*), counted for switch 59's report line.
bool hw_resprov_lin_on();
void hw_resprov_lin_note(uint32_t ev);

// build 0.0.516, switch 73 (DEFAULT OFF;  (a),; gfx_present73.h): hold a present whose plane's last P did
// not commit. hw_p73_on: the switch is ON. hw_p73_present: the display shim's question for plane VRAM offset `phys` (1 copy,
// 0 hold; counted, the first holds logged) - asked by dpg_perform ONLY while hw_p73_on(). hw_p73_flush_held: the flush-hook
// copy (Navi48AccelPeer.cpp) skipped a copy because the switch is ON.
uint32_t hw_p73_on();
// build 0.0.518: 1 while a commit arm stands (level COMMIT or the shot ARMED) - flip mode's A/B test refuses then.
uint32_t hw_cm_armed();
// build 0.0.526 (; gfx_ks81.h items 1 and 5): asked by navi48_vram_read_mm/_write_mm ONLY inside switch 37's yield
// branch. hw_mkh_gate: 0 = the caller holds no keystone withdrawal marker (yield as today); else a token - with
// N48_MKH_GATE_SKIP set (switch 81 M4) the holder goes straight to IOLockLock; otherwise its spin is accounted by hw_mkh_spin.
uint32_t hw_mkh_gate(uintptr_t caller);
void hw_mkh_spin(uint32_t g, uint64_t us);
// build 0.0.528 item 4 (measurement only): hw_mkh_holder: does the caller hold a keystone withdrawal marker (no side effect)?
// hw_mkh_lockwait: that holder's IOLockLock(gVramMmLock) wait, in us, into the kswin2 L histogram. Asked by navi48_vram_read_mm /
// navi48_vram_write_mm around their IOLockLock; nothing decides on either.
uint32_t hw_mkh_holder(uintptr_t caller);
void hw_mkh_lockwait(uint64_t us);
uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo);
void hw_p73_flush_held();
void hw_p73_delivered();   // build 0.0.521 Part E: the copy 73 let through reached the glass (monotonic copies)
// build 0.0.538 ( PLAN item 2; gfx_p95.h), switch 95 (DEFAULT OFF): hw_p95_on: switches 73 and 95 are both ON.
// hw_p95_present: dpg_perform's question while hw_p95_on() - switch 73's own answer (hw_p73_present) for the plane in *phys, and,
// when 73 HELD it, the replay of a remembered held present whose P has COMMITTED since: 1 = copy, with *phys .. *swz replaced by
// the replayed plane's source and geometry when a replay was chosen; 0 = hold. 73 OFF: 1 (copy), as the plain question answers.
uint32_t hw_p95_on();
uint32_t hw_p95_present(uint64_t *phys, uint64_t *len, uint32_t *pw, uint32_t *ph, uint32_t *stride, uint32_t *surfW,
                        uint32_t *surfH, uint32_t *swz, uint64_t presentNo);
// build 0.0.539: dpg_perform, after its copy returned (made 1 = the copy happened): a replay chosen at THIS
// present is re-checked (its slot's seq, state and gate stamp against the pick's) and a race counted and logged (capped). Log-only.
void hw_p95_after_copy(uint64_t presentNo, uint32_t made);
// build 0.0.531 item 4 (log-only): the timed pageTexture entry (Navi48AccelPeer.cpp) hands its start (clock_get_uptime ticks);
// the page-in wall-time histogram (the bare `gfxneuter 86` prints it) counts the call. Nothing decides on it.
void hw_wt_pagein(uint64_t t0);

// build 0.0.495, switch 62 (DEFAULT OFF; , gfx_heapgen.h): the shader-heap copy / substitution race.
// hw_hg_on: switch 62 is ON (and its lock exists). The copier (Navi48AccelPeer.cpp residency_copy_to_vram) calls
// hw_hg_copy_begin BEFORE its first VRAM write with the patches it will overlay (and whether any program could not be patched);
// it answers 1 when the copy overlaps a substituted program and so bumped the heap generation, in which case exactly one
// hw_hg_copy_end follows, at the copy scope's close (after the post-copy substitution). hw_hg_note_substituted: one
// substitution the post-copy scan WROTE (the registry a later copy's overlap is judged against; kept only once 62 has been
// turned ON this boot). None of these touches a register, a page table or VRAM.
bool hw_hg_on();
// build 0.0.496 F1: hw_hg_copy_begin also returns the registry sequence at the copy's start (`regSeqOut`); hw_hg_copy_end
// takes the copy's patch count (whatever `clean` says) and that sequence, and prunes (n48_hg_reg_prune) before it completes.
// build 0.0.511 (MEDIUM-1 of the 0.0.510 review): `mayWait` = gfx_heapgen.h n48_hg_copy_may_wait (the copy is
// fully patched); switch 72's wait is taken only then - any other copy bumps at once, as 0.0.509 did.
// build 0.0.533 (switch 88, notes/design/HG88.md): `id` is the copy's identity (the resource, its GPU VA and length, the
// copying pid); hw_hg_copy_begin records the copy under gHgLock in the bump's own section and returns the record's token in
// `id->tok` (null `id`: the untracked copy, an UNKNOWN range); hw_hg_copy_end closes that record by `tok88`.
uint32_t hw_hg_copy_begin(uint64_t vramLo, uint64_t vramHi, const n48_hg_patch *p, uint32_t np, uint32_t npoison, uint64_t *regSeqOut,
                          uint32_t mayWait, n48_hg_copy_id *id);
void hw_hg_copy_end(uint32_t clean, uint32_t vaOk, uint64_t vaLo, uint64_t vaHi, uint64_t vramLo, uint64_t vramHi,
                    const n48_hg_poison *add, uint32_t nadd, uint32_t npatched, uint32_t bytesPatched, uint32_t npatches,
                    uint64_t regSeq, uint64_t tok88);
void hw_hg_note_substituted(uint64_t vram, uint32_t nb);
// build 0.0.509 item 2 (F-2): a fast-copy chunk of a copy that bumped (switch 63) whose fence did not land: its range is
// poisoned for the rest of the boot (gfx_heapgen.h n48_hg_poison_add_sticky; no clean copy clears it). Leaf lock only.
void hw_hg_poison_sticky(const n48_hg_poison *e);
// build 0.0.527 (notes/design/SKIP82.md; gfx_sk82.h), switch 82's reads of this file's state, READ-ONLY: the arm level
// (gXdArm), the ring neuter (gGfxNeuter), whether a committed frame is still PENDING/COMMITTED in the flight ring (gKsRing), and
// WindowServer's binding (gen << 32 | the bound context's seq, 0 when not bound). None takes a lock.
uint32_t hw_sk82_arm_level();
uint32_t hw_sk82_neuter();
uint32_t hw_sk82_flight_live();
// build 0.0.536 (switch 92, gfx_rv92.h): hw_rv92_on - switch 92 is ON (the residency copy then writes the RectPosTexFast_VS
// v3 alternative); hw_rv92_note - one such substitution was written (N48_RV92_NEW) or ON found no alternative and wrote today's
// image (N48_RV92_NOALT): the `rect92:` report's last two counters. Neither takes a lock or touches hardware.
bool hw_rv92_on();
void hw_rv92_note(uint32_t why);
// build 0.0.529 (fix pass MF-5): the bound WindowServer pid, -1 when WindowServer is not BOUND. Read-only.
int64_t hw_d84_ws_pid();
uint64_t hw_sk82_ws_binding();

} // namespace n48
