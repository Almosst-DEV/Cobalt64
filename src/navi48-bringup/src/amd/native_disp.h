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
void     n48disp_on_ws_client_closed(bool adminClient);   // 0.0.617 (K1): the native client closed / died: a uid-88 (non-admin) client disarms an armed pipe; admin clients never do; latch OFF: returns at once
void     n48disp_on_hung(void);                    // 0.0.617 (K2): the HUNG latch was set: disarm an armed pipe; latch OFF: returns at once
void     n48disp_on_withdraw(void);                // the nub was withdrawn / the kext stops: forget the pipes, disarm the mirror, release the accelerator reference

// the accel verbs 83..87 (Navi48Bringup::accelExperiment). out[0] = the status (n48disp::Status), out[1..] verb specific. Returns the status.
uint32_t n48disp_verb(uint32_t action, uint64_t arg, uint64_t *out, unsigned count);

// Navi48Bringup.cpp: the console buffer (the RDNA4FB / GOP scanout, vram+[off, off+len)) and a bounds-checked write into it through the BAR0 aperture.
bool     navi48_console_region(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes);
bool     navi48_console_write(uint64_t consoleOff, const void *src, size_t len);
