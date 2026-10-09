//
//  native_disp.h - the kernel side of the display pipe (kext 0.0.613, #11 step 11h.2): entry points the ops table, the nub, the accel verb dispatch and the user client call.
//  Everything is behind boot-arg navi48-metal-disp=1 (latched once, n48disp_latched_on). With it absent every function here either returns "off" at once or is not reached.
//  The decisions are amd/native_disp_pure.h, the sequencing amd/native_disp_flow.h (both host-tested); amd/native_disp.cpp only supplies the kernel primitives.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

bool     n48disp_latched_on(void);                 // boot-arg navi48-metal-disp == 1, read and latched ONCE at the first call (first writer wins, never re-read)
uint32_t n48disp_fact_bits(void);                  // the fact bits adopt turned on at run time (0 until then, and 0 forever with the latch off)
uint32_t n48metal_factory_mask_now(void);          // Navi48MetalNub.cpp: the mask the aux kext's factories see = the boot-arg's mask, plus (latch ON) n48disp_fact_bits()

// ops-table hooks (Navi48MetalNub.cpp wires them into the ABI-2 members and the trace / vhook paths)
int      n48disp_hook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret);   // disp_hook: slots 267 / 277 / 278 / 279 of Navi48DisplayPipe
void    *n48disp_pci_device(void *ctx);            // pci_device: the GPU's IOPCIDevice (latch ON), else NULL
void     n48disp_on_trace(uint32_t event, uint64_t a, uint64_t b);   // the aux kext's N48_TR_DISPPIPE / N48_TR_DM_START events (latch ON only)
int      n48disp_res62(void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret);   // Navi48Resource slot 62 (type 0xC0 surfaces); 1 = handled, 0 = the generic vhook path (latch OFF: always 0)
void     n48disp_on_ws_client_closed(bool closingIsWsSession);   // 0.0.617 (K1): the native client closed / died: WindowServer's session (0.0.627: keyed on the session, not on "not admin") disarms an armed pipe; other clients never do; latch OFF: returns at once
void     n48disp_on_hung(void);                    // 0.0.617 (K2): the HUNG latch was set: disarm an armed pipe; latch OFF: returns at once
void     n48disp_on_withdraw(void);                // the nub was withdrawn / the kext stops: forget the pipes, disarm the mirror, release the accelerator reference

// 0.0.652 (M5): pipeadopt succeeded this boot (gD.adopted); one atomic load. fbpublish refuses while it is true (and the display nub's presence refuses pipeadopt / pipearm 1: n48disp::fb_interlock_refuses).
bool     n48disp_pipe_adopted(void);
// Navi48DisplayNub.cpp: a Navi48DisplayNub is published (never withdrawn in M5). One atomic load.
bool     n48fb_nub_exists(void);
// 0.0.653: Navi48MetalNub.cpp: the Metal nub was published at any time this boot (sticky: stays true after a withdraw). One atomic load. fbpublish refuses while it is true.
bool     n48metal_nub_published(void);

// 0.0.659 (M6 Stage 1a): boot-arg navi48-m6 == 1, read and latched ONCE at the first call (first writer wins, never re-read); default OFF. With it OFF every M6 branch is skipped and the code is 0.0.658's.
bool     n48m6_latched_on(void);
// 0.0.661 (M6 Stage 1b): boot-arg navi48-m6flip == 1, read and latched ONCE (default OFF). It does nothing without navi48-m6 as well: every Stage-1b path asks BOTH. With it OFF the code is 0.0.660's byte for byte.
bool     n48m6flip_latched_on(void);
// 0.0.662 (M6 Stage 2): boot-arg navi48-m6flip1 == 1, read and latched ONCE (default OFF): instance 1 (the monitor A) additionally needs it. With it OFF the code is 0.0.661's byte for byte.
bool     n48m6flip1_latched_on(void);
// 0.0.661: the M6 surface table's generation (n48m6::PubGate::gen: bumped by every change request); instance 2's Status2 hands it to the bundle so a stale table is re-read. 0 with the latch OFF.
uint32_t n48disp_m6_gen(void);
// 0.0.661 (R5): flush a deferred (rate-limited) table publish when its gap has passed; Status2 calls it (the 1 s keep-alive).
void     n48disp_m6_flush(void);
// Navi48DisplayNub.cpp: the nub of ONE display index (1 = the monitor B, 2 = the monitor A) is published. One atomic load.
bool     n48fb_nub_exists_idx(uint32_t index);
// DisplayPipeGuard.cpp: the AGDC service's M6 counters for `m6stat` page 1: o[0] = the framebuffer count of the last 0x980 reply, o[1] = 0x921 replies for a non-DP endpoint, o[2] = 0x711 replies for a non-DP endpoint, o[3] = the last endpoint dword seen
// (0xFFFFFFFF = none yet), o[4], o[5] = 0. Read-only.
void     navi48_agdc_m6_stat(uint64_t o[8]);
void     navi48_agdc_m6_stat2(uint64_t o[12]);   // 0.0.662: per-instance 0x921 / 0x711 counts, the endpoint bitmask, nfb, out-of-range count, last endpoint

// the accel verbs 83..87 (Navi48Bringup::accelExperiment). out[0] = the status (n48disp::Status), out[1..] verb specific. Returns the status.
uint32_t n48disp_verb(uint32_t action, uint64_t arg, uint64_t *out, unsigned count);

// Navi48Bringup.cpp: the console buffer (the RDNA4FB / GOP scanout, vram+[off, off+len)) and a bounds-checked write into it through the BAR0 aperture.
bool     navi48_console_region(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes);
bool     navi48_console_write(uint64_t consoleOff, const void *src, size_t len);
