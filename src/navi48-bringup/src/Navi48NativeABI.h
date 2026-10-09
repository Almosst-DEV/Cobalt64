/*
 * Navi48NativeABI.h - the user-client ABI of the native stack (kext 0.0.601, native step S1c; ABI 1.1 scanout selectors added in 0.0.603; ABI 1.2 DAL experiment selector added in 0.0.604; ABI 1.3 timed mode-trial selector added in 0.0.605; ABI 1.4 grows its result and adds the resync / OTG-lock rows in 0.0.606; ABI 1.5 (0.0.607) enables row 120 behind navi48-row120=1 and reports the clock hold in the result's reserved words).
 *
 * ONE plain-C header shared by three consumers: the kext (Navi48NativeClient), the Mesa Darwin backend
 * (copied byte for byte to mesa-mac/include/darwin/navi48_native_abi.h) and the PC test tools
 * (tools/native/n48native.c, s1d_replay.c). The contract is an internal design note: where this
 * file and the contract disagree, the contract wins (and this file is the bug).
 *
 * Transport: IOServiceOpen(svc, task, N48N_UC_TYPE, &conn), then IOConnectCallMethod(conn, selector, ...).
 * Scalars are uint64_t, at most 16 each way. Every struct input is at most 4096 bytes and arrives inline.
 * Every reserved field must be 0. Little-endian, natural alignment, no packing.
 * CPU map of a BO: IOConnectMapMemory64(conn, memoryType = BO handle, ...).
 *
 * Only stdint.h is needed; the same text compiles as C11 and as C++11.
 */
#ifndef NAVI48_NATIVE_ABI_H
#define NAVI48_NATIVE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
#define N48N_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define N48N_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#define N48N_UC_TYPE       0x4E34384Eu      /* 'N48N' */
#define N48N_ABI_VERSION   1u            /* the MAJOR: unchanged, so every v1.0 client (Mesa, n48native, s1d-replay) still handshakes */
#define N48N_ABI_MINOR     12u           /* 1.12 (kext 0.0.662, M6 Stage 2): selectors 22..26 also accept INSTANCE 1 (the monitor A: HUBP1 / OTG1; slot ids tagged 0x10 | k) behind the boot-arg navi48-m6flip1=1 (with navi48-m6=1 and navi48-m6flip=1; with it OFF instance 1 is answered kIOReturnBadArgument exactly as 1.11 did); SCANX_ACQUIRE returns the instance's geometry in a fifth output word (a call with four output words still works: the fifth is not written); N48N_SCANQ_M6FLIP1 in scan_query.flags says all three latches are on; no struct changes. 1.11 (kext 0.0.661, M6 Stage 1b): selectors 22..26 N48N_SEL_SCANX_* - the scanout of INSTANCE 2 (the monitor B: HUBP2 / OTG2), poll-only, behind the boot-args navi48-m6=1 AND navi48-m6flip=1 (a latch OFF answers every one with Unsupported and touches nothing); N48N_SCANQ_M6FLIP in scan_query.flags says both are on; no struct changes. 1.10 (kext 0.0.640, GPU-apps G4): n48n_info.reserved[3..7] report the session's VRAM budget (N48N_INFO_R_* / N48N_BUDGET_* below), so a user-space allocator (Mesa/RADV) can size its VRAM heap; no selector, no struct size changes. A third client role (APP, a non-root uid >= 501 process on the kernel allow-list) exists behind the boot-args navi48-apps=1 + navi48-multisession=1: it reaches selectors 0, 1, 3..8 and 21 only (never ReadRegs 2, never scanout 9..14, never 15..20); a refused selector is kIOReturnNotPrivileged. 1.9 (kext 0.0.612): selector 21 N48N_SEL_BO_IMPORT_HOST (milestone #11 step 11c: import a page-aligned range of the CALLER's address space as a SYSTEM-memory BO; limits 64 MiB per BO, 256 MiB per client), the class of the N48N user client is renamed IOAccelNavi48NativeClient (WindowServer's sandbox admits iokit-open only for IOAccel* classes; the service name and type 'N48N' are unchanged), and a uid-88 caller is admitted behind the boot-arg navi48-metal-ws=1; 1.8 (kext 0.0.610): selectors 19 N48N_SEL_METAL_NUB_PUBLISH and 20 N48N_SEL_METAL_NUB_WITHDRAW (milestone #9 route A: the software Navi48MetalNub the aux accelerator kext matches; published only on demand, only with the boot-arg navi48-metal=1); 1.7 (kext 0.0.609): HELD mode for row 120: selector 17 N48N_SEL_MODE_HOLD (enter row 120 and RETURN with the mode up, a scanout client may then Acquire and present at 120 Hz) and selector 18 N48N_SEL_MODE_RELEASE (end it, wait, return the final result); verdict 16 HELD, deny 25 / 26, N48N_HOLD_* words; the result's former reserved0 / reserved1 (offsets 204 / 252) are hold_end / hold_flags (the size stays 512 B); 1.6 (kext 0.0.608): row 120's blanked-transition underflow reads, DIO attribution and lock-hold witness code in the result's former reserved[8] (the size stays 512 B); 1.5 (kext 0.0.607): row 120 (the 1440p 120 Hz trial) runs behind the boot-arg navi48-row120=1 with the clock hold (P4); the result's ext.reserved words carry the hold report (its size is unchanged); new deny reasons 22 / 23, verdict 15, flags 22..24; 1.4 (kext 0.0.606): selector 16's result grows from 256 to 512 B (underflow / resync / OTG-lock report) and it accepts the rows 1 (DP1 resync) and 2 (V_TOTAL under the OTG lock) and a flags word; 1.3 (kext 0.0.605): the timed mode-trial selector 16, native-S2d; 1.2 (kext 0.0.604): the DAL experiment selector 15, native-S2-DISPCLK; 1.1 (kext 0.0.603): the scanout selectors 9..14, native-S2a. Reported when Hello asks (below) and in n48n_info.reserved[2]. */
#define N48N_HELLO_F_MINOR (1u << 8)     /* Hello flags bit 8: "report the minor": out[0] = ABI | (MINOR << 16). Without it out[0] is exactly the major, as in v1.0. Bit 8, not 0: flags = 1, which T1 leg 1 sends and expects refused, still is. */
#define N48N_MAX_IBS       64u
#define N48N_MAX_BOS       4096u            /* handles 1..4095; 0 is never valid */
#define N48N_MAX_CTX       64u              /* ids 1..64 */
#define N48N_FENCE_SLOTS   16u
#define N48N_WAIT_CAP_NS   2000000000ull
#define N48N_SEQ_LAST      0xFFFFFFFFFFFFFFFFull   /* WaitSeq: "last emitted at entry" */

/* ---- selectors of the native client (own numbering; type 0 is the legacy Navi48UserClient) ---------------- */
enum {
    N48N_SEL_HELLO     = 0,   /* in: [0] client ABI (=1) [1] flags (0)          out: [0] kernel ABI [1] kext build [2] max IBs [3] vmid */
    N48N_SEL_QUERYINFO = 1,   /* struct out: n48n_info (192 B) */
    N48N_SEL_READREGS  = 2,   /* in: [0] ABSOLUTE dword offset (RREG32) [1] count 1..16 [2] instance (0xFFFFFFFF)   struct out: uint32_t[count] */
    N48N_SEL_BOCREATE  = 3,   /* struct in: n48n_gem_create_in (32 B)   out: [0] handle [1] size [2] placed domain [3] placement bits */
    N48N_SEL_BOFREE    = 4,   /* in: [0] handle */
    N48N_SEL_GEMVA     = 5,   /* struct in: n48n_gem_va (64 B) */
    N48N_SEL_CTX       = 6,   /* struct in: n48n_ctx (16 B)   struct out: n48n_ctx (16 B) */
    N48N_SEL_SUBMIT    = 7,   /* struct in: n48n_cs_in (32 + 32*n B)   out: [0] seqno */
    N48N_SEL_WAITSEQ   = 8,   /* in: [0] seqno [1] timeout ns (relative) [2] ctx id (ignored)   out: [0] busy [1] retired [2] emitted */
    N48N_SEL_COUNT     = 9,   /* the v1.0 selectors; unchanged so old clients' tables stay valid */
    /* ---- ABI 1.1 (kext 0.0.603): native scanout, contract section "ABI 1.1 addendum" of NATIVE-S1C-ABI.md. All need Hello first. ---- */
    N48N_SEL_SCAN_QUERY    = 9,   /* struct out: n48n_scan_query (96 B). Valid without Acquire. */
    N48N_SEL_SCAN_ACQUIRE  = 10,  /* in: [0] flags (0)   out: [0] console MC [1] OTG frame count (24-bit, extended) */
    N48N_SEL_SCAN_REGISTER = 11,  /* struct in: n48n_scan_reg (32 B)   out: [0] slot id [1] slot MC */
    N48N_SEL_SCAN_PRESENT  = 12,  /* in: [0] slot [1] flags (0)   out: [0] present id [1] target frame [2] VUPDATE count at program */
    N48N_SEL_SCAN_STATUS   = 13,  /* struct out: n48n_scan_status (256 B) */
    N48N_SEL_SCAN_RELEASE  = 14,  /* out: [0] console restore verified (1/0) [1] plane MC after; idempotent */
    N48N_SEL_COUNT_1_1     = 15,
    /* ---- ABI 1.2 (kext 0.0.604): the DISPCLK/DPPCLK experiment, contract section "ABI 1.2 addendum" of NATIVE-S1C-ABI.md. Needs Hello first. ---- */
    N48N_SEL_DAL_STEP      = 15,  /* in: [0] step (1 E1b, 2 E2, 3 E3, 4 E4) [1] flags (0)   struct out: n48n_dal_result (256 B). Blocks up to ~65 s (E4 dwell). Success = the step RAN; the verdict word says how it went. */
    N48N_SEL_COUNT_1_2     = 16,
    /* ---- ABI 1.3 (kext 0.0.605): the timed mode trial, native-S2d (contract text: the "ABI 1.3 addendum" block below the structs). Needs Hello first. ---- */
    N48N_SEL_MODE_TRIAL    = 16,  /* in: [0] row (1 resync, 2 vtotal, 50, 120; row 120 only with the boot-arg navi48-row120=1 since 0.0.607) [1] dwell ms (0..30000) [2] flags (N48N_MODE_TF_*; 0.0.605 required 0)   struct out: n48n_mode_result (512 B since ABI 1.4; 256 B in 1.3). Blocks up to ~dwell + 14 s. Success = the trial RAN or was DENIED; the verdict word says which. */
    N48N_SEL_COUNT_1_3     = 17,
    N48N_SEL_MODE_HOLD     = 17,  /* ABI 1.7 (0.0.609): in: [0] max hold ms (0 = 120000; 1000..300000) [1] flags (N48N_HOLD_F_*)   struct out: n48n_mode_result (512 B). Row 120 only. Runs the whole row-120 trial (same preconditions, apply, settle, 3 s rate window, judged) and, when that is a clean PASS so far, RETURNS with the mode HELD (verdict N48N_MODE_V_HELD, the entry snapshot) instead of dwelling; blocks up to ~20 s. Any other outcome returns the trial's ordinary verdict (DENIED or a failure, already restored). */
    N48N_SEL_MODE_RELEASE  = 18,  /* ABI 1.7: in: [0] flags (N48N_REL_F_QUERY = report only)   struct out: n48n_mode_result (512 B). Ends the hold (any session may call it), waits for the whole end sequence (scanout plane back to the console FIRST, then the timing restore + verify, then the clock release) and returns the FINAL result; with QUERY it returns the entry snapshot (verdict HELD) while held and never ends anything. DENIED with deny 25 when no hold ran. Blocks up to ~30 s. */
    N48N_SEL_COUNT_1_7     = 19,
    N48N_SEL_METAL_NUB_PUBLISH  = 19,  /* ABI 1.8 (0.0.610): in: [0] flags (0)   out: [0] state (1 = published) [1] nub registry entry ID [2] N48_METAL_ABI [3] sizeof(N48MetalOps). Needs Hello. Refused unless boot-arg navi48-metal=1, a native boot with S1b POSITIVE PASS, not HUNG, no nub yet (verdicts: Unsupported / NotReady / ExclusiveAccess / Busy). Never done at boot. */
    N48N_SEL_METAL_NUB_WITHDRAW = 20,  /* ABI 1.8: in: [0] flags (0)   out: [0] previous state. Needs Hello only. Terminates the nub (and with it the aux accelerator under it). NotFound with no nub. */
    N48N_SEL_COUNT_1_8     = 21,
    /* ---- ABI 1.9 (kext 0.0.612): host-memory import, milestone #11 step 11c. Needs Hello first. ---- */
    N48N_SEL_BO_IMPORT_HOST = 21, /* in: [0] host VA (4 KiB aligned, in the CALLER's address space) [1] size (4 KiB multiple, 1 page .. 64 MiB) [2] flags (0, or with [3] != 0 the GemVa vm flags R/W/X/MTYPE of the mapping) [3] GPU VA to map at (0 = do not map: map later with GemVa MAP on the handle, as RADV does)
                                     out: [0] BO handle [1] size [2] GPU VA mapped at import (canonical; 0 when [3] was 0) [3] placed bits (N48N_PLACED_HOST_IMPORT). Wires the pages (IOMemoryDescriptor::withAddressRange(task) + prepare()) and records their physical addresses; a mapping (here or by GemVa MAP) is SYSTEM|SNOOPED, per page. Total imported <= 4 GiB per client (0.0.621; 2 GiB in 0.0.620, 256 MiB before; separate from the GTT cap).
                                     BoFree, and the close of the client, unmap the PTEs, flush the TLB, then complete() and release the pages (under the HUNG latch they are leaked, never released). A host BO cannot be a scanout slot, a user-fence target or CPU-mapped by clientMemoryForType (NotPermitted). Errors: BadArgument (alignment, size 0, > 64 MiB, user range, flags), NoMemory (the per-client cap, no descriptor, no page table space), NotReady (no Hello), Aborted / Timeout (HUNG). */
    N48N_SEL_COUNT_1_9     = 22,
    /* ---- ABI 1.11 (kext 0.0.661, M6 Stage 1b): the scanout of INSTANCE 2 (the monitor B, HUBP2 / OTG2). Every selector takes in[0] = the instance, which must be 2 (anything else, a non-zero flag word or a foreign session is refused before any register read). Need Hello and
          the session must already hold instance 0 (the plane Acquire of selector 10). Slot ids of instance 2 are TAGGED: 0x20 | k. Behind navi48-m6=1 AND navi48-m6flip=1: with either OFF each answers Unsupported. Not for APP clients or the accelerator route. ---- */
    N48N_SEL_SCANX_ACQUIRE  = 22, /* in: [0] instance (2) [1] flags (0)   out: [0] A (instance 2's console MC) [1] OTG2 frame count (extended) [2] the M6 surface-table generation [3] the allocator's FREE visible-VRAM bytes at that moment (the bundle's pool budget, review S4) */
    N48N_SEL_SCANX_REGISTER = 23, /* in: [0] instance (2)   struct in: n48n_scan_reg (32 B)   out: [0] tagged slot id (0x20|k) [1] slot MC */
    N48N_SEL_SCANX_PRESENT  = 24, /* in: [0] instance (2) [1] tagged slot id [2] flags (0)   out: [0] present id [1] target frame [2] 0 */
    N48N_SEL_SCANX_STATUS   = 25, /* in: [0] instance (2)   struct out: n48n_scan_status (256 B; console_mc = A; slot ids tagged)   out: [0] M6 table generation [1] reuse_inuse_refused [2] restores | restore failures << 32 [3] write refusals | write failures << 32. A call IS the keep-alive. */
    N48N_SEL_SCANX_RELEASE  = 26, /* in: [0] instance (2)   out: [0] restore of A verified (1/0) [1] plane MC after; idempotent */
    N48N_SEL_COUNT_1_11     = 27,
    N48N_SEL_COUNT_1_12     = 27  /* ABI 1.12 (kext 0.0.662, M6 Stage 2): no new selector. Selectors 22..26 take in[0] = 1 (the monitor A, behind navi48-m6flip1=1 as well) or 2 (the monitor B); slot ids of instance 1 are 0x10 | k. SCANX_ACQUIRE: out[4] = the instance's geometry w | h << 16 | pitch_px << 32 when the caller asked for five output words (four still work). */
};
#define N48N_SCANX_INSTANCE   2u
#define N48N_SCANX_SLOT_TAG   0x20u
#define N48N_SCANX_INSTANCE_MONA   1u        /* ABI 1.12: the monitor A (HUBP1 / OTG1); behind navi48-m6flip1=1 */
#define N48N_SCANX_SLOT_TAG_MONA   0x10u

/* ---- the drm values the kernel interprets (identical to include/drm-uapi/amdgpu_drm.h) ----------------------- */
#define N48N_GEM_DOMAIN_CPU        0x1u
#define N48N_GEM_DOMAIN_GTT        0x2u
#define N48N_GEM_DOMAIN_VRAM       0x4u
#define N48N_GEM_DOMAIN_GDS        0x8u
#define N48N_GEM_DOMAIN_GWS        0x10u
#define N48N_GEM_DOMAIN_OA         0x20u
#define N48N_GEM_DOMAIN_DOORBELL   0x40u

#define N48N_GEM_CPU_ACCESS_REQUIRED (1ull << 0)
#define N48N_GEM_NO_CPU_ACCESS       (1ull << 1)
#define N48N_GEM_CPU_GTT_USWC        (1ull << 2)
#define N48N_GEM_VRAM_CLEARED        (1ull << 3)
#define N48N_GEM_VRAM_CONTIGUOUS     (1ull << 5)
#define N48N_GEM_VM_ALWAYS_VALID     (1ull << 6)
#define N48N_GEM_EXPLICIT_SYNC       (1ull << 7)
#define N48N_GEM_CP_MQD_GFX9         (1ull << 8)    /* refused */
#define N48N_GEM_ENCRYPTED           (1ull << 10)   /* refused */
#define N48N_GEM_PREEMPTIBLE         (1ull << 11)   /* refused */
#define N48N_GEM_DISCARDABLE         (1ull << 12)
#define N48N_GEM_UNCACHED            (1ull << 14)   /* forces MTYPE UC in the PTE */
#define N48N_GEM_GFX12_DCC           (1ull << 16)   /* ignored: no DCC PTE bit in S1c */
#define N48N_GEM_VIRTIO_SHARED       (1ull << 31)   /* Mesa-internal flag, ignored */

#define N48N_VA_OP_MAP        1u
#define N48N_VA_OP_UNMAP      2u
#define N48N_VA_OP_CLEAR      3u    /* Unsupported */
#define N48N_VA_OP_REPLACE    4u    /* Unsupported */

#define N48N_VM_DELAY_UPDATE  (1u << 0)
#define N48N_VM_PAGE_READABLE (1u << 1)
#define N48N_VM_PAGE_WRITEABLE (1u << 2)
#define N48N_VM_PAGE_EXECUTABLE (1u << 3)
#define N48N_VM_PAGE_PRT      (1u << 4)     /* Unsupported */
#define N48N_VM_MTYPE_MASK    (0xfu << 5)
#define N48N_VM_MTYPE_DEFAULT (0u << 5)
#define N48N_VM_MTYPE_NC      (1u << 5)
#define N48N_VM_MTYPE_WC      (2u << 5)
#define N48N_VM_MTYPE_CC      (3u << 5)
#define N48N_VM_MTYPE_UC      (4u << 5)
#define N48N_VM_MTYPE_RW      (5u << 5)
#define N48N_VM_PAGE_NOALLOC  (1u << 9)

#define N48N_CTX_OP_ALLOC         1u
#define N48N_CTX_OP_FREE          2u
#define N48N_CTX_OP_QUERY_STATE2  4u
#define N48N_CTX_QUERY2_RESET     (1ull << 0)
#define N48N_CTX_QUERY2_GUILTY    (1ull << 2)

#define N48N_HW_IP_GFX            0u
#define N48N_IB_FLAG_CE                  (1u << 0)   /* Unsupported */
#define N48N_IB_FLAG_PREAMBLE            (1u << 1)   /* accepted, ignored: a preamble is always executed */
#define N48N_IB_FLAG_PREEMPT             (1u << 2)   /* accepted, ignored */
#define N48N_IB_FLAG_TC_WB_NOT_INVALIDATE (1u << 3) /* accepted, ignored */
#define N48N_IB_FLAG_RESET_GDS_MAX_WAVE_ID (1u << 4) /* Unsupported */
#define N48N_IB_FLAG_SECURE              (1u << 5)   /* Unsupported */
#define N48N_IB_FLAG_EMIT_MEM_SYNC       (1u << 6)   /* Unsupported */

#define N48N_CS_HAS_FENCE  (1u << 0)

/* ---- structs (every one layout-identical to its drm twin; sizes asserted below) ------------------------------ */

/* == struct drm_amdgpu_gem_create_in, 32 B */
struct n48n_gem_create_in { uint64_t bo_size, alignment, domains, domain_flags; };

/* == struct drm_amdgpu_gem_va, 64 B. Timeline/syncobj fields must be 0 (Mesa consumes them). */
struct n48n_gem_va {
    uint32_t handle, _pad, operation, flags;                /* 0,4,8,12 */
    uint64_t va_address, offset_in_bo, map_size;            /* 16,24,32 */
    uint64_t vm_timeline_point;                             /* 40 */
    uint32_t vm_timeline_syncobj_out, num_syncobj_handles;  /* 48,52 */
    uint64_t input_fence_syncobj_handles;                   /* 56 */
};

/* == union drm_amdgpu_ctx, 16 B: in = {op, flags, ctx_id, priority}; out overlays it */
struct n48n_ctx { uint32_t op, flags, ctx_id; int32_t priority; };
/* out: ALLOC -> dword0 = ctx_id, dword1 = 0;  QUERY_STATE2 -> qword0 = N48N_CTX_QUERY2_*, dwords 2,3 = 0 */

/* == struct drm_amdgpu_cs_chunk_ib, 32 B */
struct n48n_cs_ib { uint32_t _pad, flags; uint64_t va_start; uint32_t ib_bytes, ip_type, ip_instance, ring; };

/* Submit header, 32 B, followed by num_ibs x n48n_cs_ib. structureInputSize must be 32 + 32*num_ibs. */
struct n48n_cs_in {
    uint32_t abi;             /* 0: = N48N_ABI_VERSION */
    uint32_t ctx_id;          /* 4: from Ctx ALLOC */
    uint32_t num_ibs;         /* 8: 1..64, executed in array order */
    uint32_t flags;           /* 12: bit0 N48N_CS_HAS_FENCE; other bits 0 */
    uint32_t fence_handle;    /* 16: == drm_amdgpu_cs_chunk_fence.handle (BO handle) */
    uint32_t fence_offset;    /* 20: == .offset, in BYTES (Mesa already multiplies by 8), 8-aligned */
    uint64_t reserved;        /* 24: 0 */
    /* struct n48n_cs_ib ibs[num_ibs]; at 32 */
};

struct n48n_info {                 /* 192 B */
    uint32_t abi_version, kext_build, flags, vmid;              /* 0,4,8,12  flags: N48N_INFO_* */
    uint32_t gb_addr_config, gc_version, ring_size_dw, max_ibs; /* 16..28: live GB_ADDR_CONFIG; (maj<<16)|(min<<8)|rev */
    uint64_t vram_vis_total, vram_vis_free;                     /* 32,40: BAR0-visible pool */
    uint64_t vram_hi_total, vram_hi_free;                       /* 48,56: pool above BAR0 */
    uint64_t gtt_cap, gtt_used, gtt_max_bo;                     /* 64,72,80 */
    uint64_t pt_cap, pt_used;                                   /* 88,96: page-table bytes, this client */
    uint64_t seq_emitted, seq_retired;                          /* 104,112 */
    uint64_t va_low_first, va_low_last;                         /* 120,128: 0x10000, 0x7FFFFFFFFFFF */
    uint64_t va_high_first, va_high_last;                       /* 136,144: 0xFFFF800000000000, 0xFFFFFFFFFFFFFFFF */
    uint32_t max_bos, fence_slots;                              /* 152,156 */
    uint32_t reserved[8];                                       /* 160..191 */
};
#define N48N_INFO_HUNG          (1u << 0)
#define N48N_INFO_S1B_POSITIVE  (1u << 1)
#define N48N_INFO_NATIVE_BOOT   (1u << 2)
/* ABI 1.10 (kext 0.0.640): n48n_info.reserved[] words (an older kext leaves them 0, which reads as "no budget reported").
 *   reserved[0],[1] = native write counter (diagnostic)   reserved[2] = ABI minor (1.1)
 *   reserved[3] = N48N_BUDGET_* flags
 *   reserved[4],[5] = the VRAM budget CAP in bytes, low / high dword: what ALL non-WindowServer sessions together may hold (25 % of the VRAM pools). 0 when N48N_BUDGET_VALID is clear.
 *   reserved[6],[7] = the bytes still AVAILABLE to this session now = min(cap - held by all non-WindowServer sessions, free bytes of the VRAM pools), low / high dword. A user-space
 *                     allocator should treat (bytes this session already holds + AVAILABLE) as its heap size; the budget is SHARED, so AVAILABLE can shrink between two queries. */
#define N48N_INFO_R_BUDGET_FLAGS 3u
#define N48N_INFO_R_BUDGET_CAP_LO 4u
#define N48N_INFO_R_BUDGET_CAP_HI 5u
#define N48N_INFO_R_BUDGET_AVAIL_LO 6u
#define N48N_INFO_R_BUDGET_AVAIL_HI 7u
#define N48N_BUDGET_VALID (1u << 0)   /* a VRAM budget applies to this session (multi-session boot, not WindowServer's session) */
#define N48N_BUDGET_APP   (1u << 1)   /* this session was admitted as an APP (the kernel allow-list role) */
/* BoCreate scalar out [3] */
#define N48N_PLACED_CPU_MAPPABLE (1u << 0)
#define N48N_PLACED_ZEROED       (1u << 1)
#define N48N_PLACED_HI_POOL      (1u << 2)
#define N48N_PLACED_HOST_IMPORT  (1u << 3)   /* ABI 1.9: the BO is an import of caller memory (SYSTEM pages) */
#define N48N_IMPORT_MAX_BO       (64ull << 20)   /* ABI 1.9: per BoImportHost call */
#define N48N_IMPORT_CAP          (4096ull << 20) /* ABI 1.9: per client, 4 GiB since kext 0.0.621 (2 GiB in 0.0.620, 256 MiB before), separate from the 512 MiB GTT cap */

/* ---- ABI 1.1: native scanout (kext 0.0.603) ------------------------------------------------------------------------------------ */
#define N48N_SCAN_MAX_SLOTS   3u
#define N48N_SCAN_NO_SLOT     0xFFFFFFFFu
#define N48N_SCAN_FMT_ARGB8888 8u          /* HUBP SURFACE_PIXEL_FORMAT 8: dword 0xAARRGGBB = VK_FORMAT_B8G8R8A8_UNORM memory order */
/* n48n_scan_query.flags */
#define N48N_SCANQ_LIT        (1u << 0)    /* OTG0 is master-enabled */
#define N48N_SCANQ_NATIVE     (1u << 1)    /* native boot (S1b gate accepted) */
#define N48N_SCANQ_ACQUIRED   (1u << 2)    /* the plane is taken (by this or an earlier call of this client) */
#define N48N_SCANQ_GEOM_OK    (1u << 3)    /* linear ARGB8888, no DCC, plausible viewport/pitch: Acquire would proceed */
#define N48N_SCANQ_DTO_VALID  (1u << 4)    /* refresh derived from the DP DTO; else from the EDID row matching the raster, else 0 */
#define N48N_SCANQ_M6FLIP1    (1u << 7)    /* 0.0.662 (M6 Stage 2): boot-arg navi48-m6flip1=1 is latched ON as well as navi48-m6=1 and navi48-m6flip=1: ABI 1.12's instance 1 (the monitor A) is live; a flag bit in an existing word */
#define N48N_SCANQ_M6FLIP     (1u << 6)    /* 0.0.661 (M6 Stage 1b): boot-arg navi48-m6flip=1 is latched ON as well as navi48-m6=1: ABI 1.11's selectors 22..26 (instance 2) are live; a flag bit in an existing word */
#define N48N_SCANQ_M6         (1u << 5)    /* 0.0.659 (M6 Stage 1a): boot-arg navi48-m6=1 is latched ON: the bundle presents only surfaces the kernel maps to instance 0 (property Navi48,M6Surf of the Metal nub); ABI minor unchanged (a flag bit in an existing word) */
/* n48n_scan_slot.flags */
#define N48N_SCANSLOT_PENDING  (1u << 0)   /* this slot's Present is programmed and has not latched */
#define N48N_SCANSLOT_INUSE    (1u << 1)   /* HUBP0 EARLIEST_INUSE equals this slot's MC: the hardware is still fetching it */
#define N48N_SCANSLOT_REUSABLE (1u << 2)   /* neither pending nor in use: safe to overwrite */
/* n48n_scan_status.flags */
#define N48N_SCANST_RESTORING  (1u << 0)
#define N48N_SCANST_STORM      (1u << 1)   /* the rate guard tripped this session (the plane is being / was restored) */
#define N48N_SCANST_WANT_RESTORE (1u << 2)

struct n48n_scan_query {           /* 96 B, ScanoutQuery out */
    uint32_t h_active, v_active, h_total, v_total;        /*  0: live OTG timing */
    uint32_t pitch_px, hubp_format, sw_mode, otg;         /* 16: live HUBP; pitch in pixels */
    uint32_t refresh_mhz, pix_clk_khz, dcc_en, flags;     /* 32: millihertz; N48N_SCANQ_* */
    uint64_t frame_count;                                 /* 48: OTG_STATUS_FRAME_COUNT (24 bits), extended to 64 while acquired */
    uint64_t console_mc;                                  /* 56: the console plane address (recorded at Acquire; the live one before) */
    uint64_t plane_mc;                                    /* 64: HUBP0's programmed primary surface address now */
    uint64_t earliest_mc;                                 /* 72: HUBP0's EARLIEST_INUSE address now */
    uint32_t acquired, plane_w, plane_h, reserved;        /* 80: HUBP0's viewport (what ScanoutRegister's width / height must equal); reserved 0 */
};
struct n48n_scan_reg {             /* 32 B, ScanoutRegister in. width/height/format must equal the live plane; reserved0 must be 0 */
    uint32_t handle, reserved0;                           /*  0: BO handle from BoCreate (visible-pool VRAM only) */
    uint64_t offset;                                      /*  8: byte offset in the BO; offset + pitch_bytes*height <= BO size; MC 64 KiB aligned */
    uint32_t pitch_bytes, height;                         /* 16: pitch_bytes == live pitch * 4 */
    uint32_t width, format;                               /* 24: N48N_SCAN_FMT_ARGB8888 */
};
struct n48n_scan_slot {            /* 32 B */
    uint64_t mc;                                          /*  0 */
    uint64_t latched_frame;                               /*  8: extended OTG frame count when this slot's flip was SEEN to latch (0 = never) */
    uint32_t used, flags;                                 /* 16: N48N_SCANSLOT_* */
    uint32_t presents, latches;                           /* 24 */
};
struct n48n_scan_status {          /* 256 B, ScanoutStatus out */
    uint32_t acquired, flags, front_slot, pending_slot;   /*  0: slot ids or N48N_SCAN_NO_SLOT */
    uint64_t frame_count;                                 /* 16: extended OTG frame count now */
    uint64_t console_mc, plane_mc, earliest_mc;           /* 24 */
    uint64_t presents, latched, replaced, repeats;        /* 48: latched = DISTINCT presents seen latched; replaced = overwritten before shown; repeats = VUPDATEs that showed the same front */
    uint64_t vupdates, latch_irq, latch_poll, refused;    /* 80: VUPDATE IRQs while acquired; latches seen by the IRQ / by a poll; refused Presents */
    uint64_t first_latch_ns, last_latch_ns;               /* 112: kernel uptime ns of the first / last latch observation */
    uint64_t idle_ms, watchdog_restores, storm_trips, geom_refused;   /* 128 */
    struct n48n_scan_slot slot[N48N_SCAN_MAX_SLOTS];      /* 160 */
};

/* ---- ABI 1.2: the DAL experiment (kext 0.0.604) ---------------------------------------------------------------------------------- */
#define N48N_DAL_STEP_E1B 1u
#define N48N_DAL_STEP_E2  2u
#define N48N_DAL_STEP_E3  3u
#define N48N_DAL_STEP_E4  4u
/* n48n_dal_result.verdict */
#define N48N_DAL_V_PASS      1u
#define N48N_DAL_V_REFUSED   2u    /* the PMFW answered non-OK, or answered OK and the clock did not follow (stop, latched) */
#define N48N_DAL_V_TIMEOUT   3u    /* the mailbox did not answer inside its bound (stop, latched; needs a cold power cycle) */
#define N48N_DAL_V_REGRESSED 4u    /* a clock read below its start value, or fell back under its threshold in the dwell (stop, latched) */
#define N48N_DAL_V_GLITCH    5u    /* a clock that must not move moved, DENTIST disagreed / CHG_DONE missing, the frame counter stalled or its rate drifted (stop, latched) */
#define N48N_DAL_V_DENIED    6u    /* nothing was sent: gate, level, order, latch, busy, baseline or pre-flight (no latch, except that a latched boot answers DENIED) */
#define N48N_DAL_V_SHORT     7u    /* E1b only: every query answered, but DISPCLK has <= 1 level or its DPM max is under 540 MHz (no latch) */
/* n48n_dal_result.flags */
#define N48N_DAL_F_E1B_DONE        (1u << 0)   /* E1b has completed on this boot and enables E2..E4 */
#define N48N_DAL_F_RESTORED        (1u << 1)   /* the restore ran and both clocks read at least their start value */
#define N48N_DAL_F_RESTORE_FAILED  (1u << 2)   /* a raise happened and the restore did not verify (or was not attempted after a timeout): cold power cycle */
#define N48N_DAL_F_UNIT_KHZ_SEEN   (1u << 3)   /* a DPM reply was >= 20000 and was read as kHz (design C4) */
#define N48N_DAL_F_UNDERFLOW_UNREAD (1u << 4)  /* always set in 0.0.604: the kext has no HUBP underflow read; watch the picture */
#define N48N_DAL_F_VUPDATE_UNCOUNTED (1u << 5) /* the VUPDATE IRQ counter counts only while the plane is acquired; the frame counter is the rate witness */
#define N48N_DAL_F_LATCHED         (1u << 6)   /* the stop latch is set (this step's, or an earlier one's) */
#define N48N_DAL_F_LOW_MAX         (1u << 7)   /* E1b: DISPCLK DPM max < 540 MHz */
#define N48N_DAL_F_ODM             (1u << 8)   /* E1b: DISPCLK DPM max < 520 MHz: 120 Hz impossible on one pipe, ODM is the fallback */
#define N48N_DAL_F_POLL_TIMEOUT    (1u << 9)   /* a ReturnHardMinStatus poll ran out its 1 s (the mailbox itself was answering; the restore is still attempted) */
#define N48N_DAL_F_AT_START        (1u << 10)  /* after the restore both DFS DIDs equal their start values */
#define N48N_DAL_F_ABOVE_START     (1u << 11)  /* after the restore both clocks read >= start but at least one is still above it (a floor is holding it up) */
#define N48N_DAL_F_FINE_GRAINED    (1u << 12)  /* E1b: a clock reported bit 31 in its level count (fine-grained: min and max only) */
#define N48N_DAL_UNDERFLOW_NA      0xFFFFFFFFu
struct n48n_dal_result {           /* 256 B */
    uint32_t step, verdict, latched, level;                       /*   0: level = the navi48-dalsmc boot-arg the kext read */
    uint32_t rc, flags, nmsg, fail_msg;                           /*  16: rc = the first failing IOReturn (0 none); nmsg = DAL messages sent; fail_msg = the message id that failed or was refused (0 none) */
    uint32_t vco_khz, pll_req, dentist_before, dentist_after;     /*  32 */
    uint32_t dfs_before[2], dfs_after[2];                         /*  48: raw DFS_CNTL, [0] DISPCLK [1] DPPCLK */
    uint32_t khz_before[2], khz_after[2], khz_restored[2], target_mhz[2];   /*  64: computed from the DFS readback with the C1 table */
    uint32_t frame_before, frame_after, dwell_ms, vupdates_delta; /*  96: OTG frame counter (24 bit) at the start of the dwell / its end; vupdates_delta counts only while acquired */
    uint32_t underflow, last_resp, last_arg, last_us;             /* 112: underflow = N48N_DAL_UNDERFLOW_NA (not read); the last message's reply, ARG and duration */
    uint32_t smu_version, if_version, header_version, hardmin_poll_us;   /* 128: E1b replies of 0x2 / 0x3 / 0x4; the 0x15 poll time of the last hard-min */
    uint32_t dpm_count[2], dpm_max_mhz[2];                        /* 144: E1b: levels and max (MHz) per clock, [0] DISPCLK [1] DPPCLK */
    uint32_t dc_max_mhz[2], grant_reply[2];                       /* 160: 0xC replies (normalised MHz); the raw ARG of the last 0x9 per clock (unit unspecified, design C4) */
    uint16_t dpm_mhz[2][16];                                      /* 176: E1b: each level in MHz */
    uint32_t deny, refuse, dc_max_resp, reserved;                 /* 240: deny = the pre-gate reason (n48dal::Deny), refuse = the allowlist / mailbox reason of the message that was NOT sent (n48dal::Refuse); dc_max_resp = the raw RESP of E1b's DISPCLK GetDcModeMaxDpmFreq (0 = not sent, 1 OK; any other value was recorded without stopping the step; DPPCLK's is never sent); reserved 0 */
};

/* ---- ABI 1.3: the timed mode trial (kext 0.0.605, native S2d) --------------------------------------------------------------------------
 * ABI 1.3 addendum (proposed text for an internal design note; the header is the only place it lives until the reviewer merges it):
 *   Selector 16 N48N_SEL_MODE_TRIAL. Native boots only (navi48-native=1 with S1b POSITIVE PASS), scanout plane NOT acquired, DISPCLK/DPPCLK at least what the row's DML
 *   needs (read from the DFS registers, never assumed), the live mode the 60 Hz census raster. It programs the display to the row's refresh (row 50 = the 60 Hz raster at a
 *   201 MHz pixel clock, 49.90 Hz, the live 60 Hz DLG/TTU stays; row 120 = 497.75 MHz, 2720x1525, the DML DLG/TTU/prefetch goldens; row 120 needs a passed row-50 trial on
 *   the same boot), verifies the OTG frame rate over 3 s (within 1 %), DIO_FIFO_ERROR / DIO_ERROR_COUNT unchanged and the DP stream still enabled, holds `dwell`, then restores
 *   EVERY register it wrote to the copy taken before any write this boot (reverse order) and verifies the 60 Hz rate is back. A watchdog thread restores if the trial thread
 *   has not reached its restore by settle + measure + dwell + 2 s. After any failure verdict a sticky latch refuses every further trial until the next boot.
 *   PASS needs the restore transition to be clean too: DIO_FIFO_ERROR 0, DIO_ERROR_COUNT not grown across the restore, DP1_DP_STEER_FIFO bit 4 clear, DIG1_DIG_FIFO_CTRL0 bits 29:28 clear.
 *   Row 120 is hard-denied (N48N_MODE_D_ROW120_OFF) in 0.0.605. The DAL step and the trial refuse each other.
 *   Time in the trial mode is about 0.5 s + 3 s + dwell. The hardware writes are the ones listed in navi48_modetrial_tables.h (the generated golden tables), nothing else.
 *   It never raises a clock: a row whose DML clocks exceed the live DFS readback is DENIED (n48dal / navi48-dalsmc is the only clock path).
 *   n48n_mode_result: verdict N48N_MODE_V_*, deny N48N_MODE_D_* (verdict DENIED: nothing was written), flags N48N_MODE_F_*. Tool: tools/native/n48mode.c.
 * ABI 1.4 addendum (kext 0.0.606, native S2-120HZ; design an internal design note P1 / P2 / P3):
 *   The result is 512 B: the first 256 B are the 1.3 layout unchanged, the second 256 B are `ext` (below). A client built for 1.3 must not call selector 16 on a 1.4 kext (the output size differs).
 *   Two new rows, both at 60 Hz with no clock or DTO change: row 1 = the DP1 stream resync (blank, then unblank, exactly Linux's order: field-exact writes of DP1_DP_VID_STREAM_CNTL,
 *   DP1_DP_STEER_FIFO and DIG1_DIG_FIFO_CTRL0 only; one retry; the stream is left blanked only if both attempts failed, and then the restore / watchdog / `dcnmode 0` / kext stop unblank it);
 *   row 2 = OTG0_OTG_V_TOTAL 1481 -> 1501 lines (expected 59.15 Hz) inside that resync, under the OTG master update lock (optc3_lock / optc1_unlock), DRR double-buffer mode 2 (start of
 *   frame) written before the lock and restored to its golden value last; the latch is confirmed by the pending bits and by OTG0_OTG_STATUS_POSITION.VERT_COUNT exceeding the old maximum.
 *   flags word (in[2]): N48N_MODE_TF_RESYNC wraps a row-50 trial's writes and its restore in the same resync. Row 2 and the TF_RESYNC row-50 trial need a passed row-1 resync on this boot.
 *   The underflow registers (HUBP0_DCHUBP_CNTL, ODM0_OPTC_INPUT_GLOBAL_CONTROL) are read before the trial (must be clean), after the settle (reported), every 250 ms during the rate window and
 *   the dwell (an underflow ends the trial: verdict UNDERFLOW, restored, latched) and after the restore (an underflow there makes the verdict RESTORE). They are cleared with their write-1
 *   strobes (never while ODM0_OPTC_INPUT_GLOBAL_CONTROL.INPUT_SOFT_RESET is set). Row 120 stays hard-denied. */
/* ABI 1.5 addendum (kext 0.0.607, native S2-120HZ; design an internal design note P4 / P5):
 *   Row 120 (2560x1440, 2720x1525, 497.75 MHz = 119.998 Hz) is DENIED (N48N_MODE_D_ROW120_OFF) unless the boot-arg navi48-row120=1 is present. With it, the trial needs: a native boot with S1b POSITIVE PASS and
 *   navi48-dalsmc=4; E1b (n48dal e1b) run on THIS boot; a passed row-50 trial, a passed row-1 resync and a row-2 PASS with N48N_MODE_F_LOCK_HELD_PROVEN on THIS boot (deny 15 / 23); the F6 / F7 / P1 baselines
 *   (OTG lock usable, FIFO health, underflow clean); the clock hold available (deny 22: the reason is ext.hold_pre, n48dal::HoldPre 1 level, 2 no E1b, 3 DAL latch, 4 hold state, 5 mailbox owner busy). The operator keeps the
 *   display awake (rdna4-nosleep or caffeinate -d): display sleep would race the DP1 blank.
 *   Sequence: (1) clock hold: DISPCLK then DPPCLK SetHardMinByFreq 530 MHz (each acknowledged and reported done), the DFS readback must reach 514.285 / 500.000 MHz; (2) DP1 blank; (3) DRR mode 2 then the OTG master update lock;
 *   (4) the OTG timing / global-sync / VTG / HUBP DLG-TTU registers (recorded, then written); (5) unlock, wait for the pending bits, V_TOTAL latch witness; (6) DP_DTO0 (the pixel clock moves AFTER the unlock);
 *   (7) DP1 MSA timing; (8) DP1 unblank; (9) settle 500 ms; (10) rate window 3 s (119.998 Hz +/- 1 %, underflow 0, DIO growth 0, steer / DIG clean, stream active, DFS >= need, sampled every 250 ms); (11) dwell;
 *   (12) THE restore: DTO first, then lock / OTG + HUBP reverse / unlock / pending / latch witness, then MSA, DRR mode, unblank (any failing wait continues and sets the restore bad); (13) verify the 60 Hz rate, stream, health,
 *   underflow; (14) only then release the clock hold (DPPCLK then DISPCLK to the 272 MHz floor). A restore that did not verify never releases (hold state STUCK: the clocks stay up, harmless at 60 Hz; cold power cycle).
 *   Any failure at any step goes to the restore. The watchdog restores registers only; it never touches a clock. New words: verdict 12 CLOCK_LOST (the DFS readback fell under the need while held), verdict 15 HOLD (the release
 *   did not verify), flags 22..24, the hold report in ext (hold_*). The result struct keeps its 512 B; the hold report lives in what 1.4 called ext.reserved. */
/* ABI 1.7 addendum (kext 0.0.609, native S2 milestone #8: the cube at 120 Hz; design: the "0.0.609" section of an internal design note and the comments of dcn/navi48_modetrial_pure.h):
 *   A mode trial and a scanout client exclude each other (scanAcquire refuses while a trial is busy; the trial refuses while the plane is acquired). The HELD mode lets a scanout client present WHILE the display runs at 1440p 120 Hz.
 *   Selector 17 N48N_SEL_MODE_HOLD (row 120 only, needs everything `n48mode 120` needs): runs the SAME trial on a kernel thread - deny gate, clock hold, blank, lock, writes, unlock, latch, DTO, MSA, unblank, 500 ms settle, the
 *   3 s judged rate window - and, when that is clean (no failure, rate inside 1 %, no underflow judged), does NOT dwell for a fixed time: it publishes the entry snapshot and returns it (verdict N48N_MODE_V_HELD) with the mode UP.
 *   Otherwise the caller gets the trial's ordinary result (DENIED, or a failure that has already been restored and latched). While HELD (busy stays 1: no other trial or DAL step can start) ScanoutAcquire is allowed - and
 *   only for the owning session, or for a HANDOFF hold's next session - with the live plane geometry check unchanged (row 120 writes no viewport / pitch / format register).
 *   The hold ENDS on: ModeRelease (selector 18, any session); the owning session's close or death; the max hold time in[0] (1000..300000 ms, 0 = 120000; the watchdog deadline counts from the first write and is that plus 12.5 s); a HANDOFF hold's 30 s with no
 *   taker; `dcnmode 0` or the kext stop (verdict ABORT, latched); a failure seen by the sampler (every 250 ms: HUBP / OPTC underflow, DIO FIFO error or count growth, DP stream, steer / DIG FIFO, the DFS readback under the need)
 *   or by the frame-counter stall detector (that verdict, restored, latched); the watchdog (a stalled hold thread: verdict WATCHDOG). EVERY end runs the same sequence, in this order: (1) `ending` (no further Acquire), the scanout
 *   plane is put back to the console and that restore is VERIFIED (the client's next Present answers NotReady); (2) the timing restore of the trial (DTO first, lock, OTG + HUBP registers, unlock, pending, latch witness, MSA,
 *   DRR, unblank; the blanked-transition underflow is recorded and cleared as in 0.0.608); (3) the 60 Hz rate, stream, FIFO health and underflow are verified; (4) only then the clock hold is released (it never releases on a bad
 *   restore, including a scanout put-back that did not verify: verdict RESTORE, hold_flags SCAN_BAD).
 *   Ownership: a native session is EXCLUSIVE, so the tool that starts a hold and the scanout client cannot be open together. in[1] = N48N_HOLD_F_HANDOFF makes the hold survive the close of the issuing session and passes it to the
 *   next session that ScanoutAcquires (which then owns it: its close ends the hold); without HANDOFF the issuing session owns the hold and its close ends it. `n48mode hold120 --run CMD` uses HANDOFF: it holds, closes, runs CMD,
 *   re-opens and calls MODE_RELEASE, which returns the FINAL result (immediately when CMD's own close already ended the hold). The result's hold_end says why it ended (N48N_HOLD_END_*), hold_flags what happened (N48N_HOLD_FL_*).
 *   Selector 18 N48N_SEL_MODE_RELEASE: ends a running hold and waits (<= ~30 s) for the whole end sequence, returning the final result; with N48N_REL_F_QUERY it only reports (the entry snapshot while held, else the last final
 *   result); DENIED (deny 25) when no hold ran this boot. The two selectors take no client lock. Lock order: see the comment above `gMt` in dcn/navi48_dcn.cpp.
 *   A refresh-rate note for clients: nothing in the scanout path assumes 60 Hz (the storm guard is 1000 IRQs / s, the idle watchdog 5 s, the restore polls 200 x 1 ms); ScanoutQuery reports the live rate from the DP DTO and the OTG totals. */
#define N48N_MODE_ROW_RESYNC      1u
#define N48N_MODE_ROW_VTOTAL      2u
#define N48N_MODE_ROW_50          50u
#define N48N_MODE_ROW_120         120u
/* result codes of the 0.0.606 hardware sequences (ext.rs_rc_*, lk_rc, ...) */
#define N48N_MODE_SEQ_OK               0u
#define N48N_MODE_SEQ_NOT_ENABLED      1u   /* blank: DP1_DP_VID_STREAM_CNTL.ENABLE was already 0 (Linux returns) */
#define N48N_MODE_SEQ_STATUS_STUCK     2u   /* DP1_DP_VID_STREAM_CNTL.STATUS did not go to 0 within 10 us x 10020 */
#define N48N_MODE_SEQ_WRITE            3u   /* a write was refused by the allowlist */
#define N48N_MODE_SEQ_DIG_RESET        4u   /* DIG1_DIG_FIFO_CTRL0.RESET_DONE did not follow RESET within 50 ms */
#define N48N_MODE_SEQ_NOT_ACTIVE       5u   /* after the unblank DP1_DP_VID_STREAM_CNTL did not read ENABLE + STATUS within 100 ms */
#define N48N_MODE_SEQ_UNREADABLE       6u   /* a register read back all-ones */
#define N48N_MODE_SEQ_LOCK_TIMEOUT     7u   /* OTG0_OTG_MASTER_UPDATE_LOCK.UPDATE_LOCK_STATUS did not rise (the lock was released again) */
#define N48N_MODE_SEQ_LOCK_STUCK       8u   /* the lock request or its status would not clear after the unlock */
#define N48N_MODE_SEQ_PENDING_TIMEOUT  9u   /* the double-buffer pending bits did not clear within 100 ms of the unlock */
#define N48N_MODE_SEQ_LATCH            10u  /* the V_TOTAL latch witness (VERT_COUNT) was not seen within 3 frames */
#define N48N_MODE_SEQ_HOLD             12u  /* the lock-hold witness: VERT_COUNT went past the old maximum while the V_TOTAL write was held under the lock */
#define N48N_MODE_SEQ_DRR              11u  /* OTG0_OTG_DOUBLE_BUFFER_CONTROL DRR mode did not read back what was written */
#define N48N_MODE_TF_RESYNC       (1u << 0)   /* in[2]: wrap a row-50 trial in the DP1 resync (needs a passed row-1 resync on this boot) */
#define N48N_MODE_TF_MASK         (N48N_MODE_TF_RESYNC)
#define N48N_MODE_MAX_DWELL_MS    30000u
#define N48N_MODE_MAX_HOLD_MS     300000u  /* ABI 1.7: the longest a HELD mode may last */
#define N48N_MODE_DEFAULT_HOLD_MS 120000u
#define N48N_HOLD_F_HANDOFF       (1u << 0)   /* MODE_HOLD in[1]: the hold survives the close of the issuing session and passes to the next session that ScanoutAcquires (a native session is exclusive: the tool that starts the hold closes, the scanout client opens); it then ends when THAT session closes; if none Acquires within 30 s it ends */
#define N48N_HOLD_F_MASK          (N48N_HOLD_F_HANDOFF)
#define N48N_REL_F_QUERY          (1u << 0)   /* MODE_RELEASE in[0]: report only */
#define N48N_REL_F_MASK           (N48N_REL_F_QUERY)
/* result.hold_end: why the hold ended (0 = it never ran / still held) */
#define N48N_HOLD_END_RELEASED    1u   /* ModeRelease */
#define N48N_HOLD_END_OWNER_CLOSED 2u  /* the owning session closed or died */
#define N48N_HOLD_END_MAX_TIME    3u   /* the max hold time elapsed */
#define N48N_HOLD_END_NO_TAKER    4u   /* HANDOFF: no session Acquired within 30 s of the issuing session closing */
#define N48N_HOLD_END_ABORT       5u   /* `dcnmode 0` or the kext stop */
#define N48N_HOLD_END_FAILURE     6u   /* the sampler (underflow / DIO FIFO / stream / clock lost) or the frame-counter stall detector ended it */
#define N48N_HOLD_END_WATCHDOG    7u   /* the watchdog had to restore (a stalled hold thread) */
/* result.hold_flags */
#define N48N_HOLD_FL_HANDOFF      (1u << 0)   /* the hold was started with HANDOFF */
#define N48N_HOLD_FL_TOOK_OVER    (1u << 1)   /* a session took the hold over by Acquire (HANDOFF) */
#define N48N_HOLD_FL_SCAN_ACTED   (1u << 2)   /* the scanout plane was acquired when the end began and was put back to the console first */
#define N48N_HOLD_FL_SCAN_BAD     (1u << 3)   /* THAT console restore did NOT verify (the verdict is RESTORE) */
#define N48N_HOLD_FL_ENTERED      (1u << 4)   /* the mode was HELD (a hold entry snapshot exists) */
/* n48n_mode_result.verdict */
#define N48N_MODE_V_PASS       1u
#define N48N_MODE_V_DENIED     2u    /* nothing was written (gate, latch, clocks, baseline ...): no latch */
#define N48N_MODE_V_RATE       3u    /* the OTG frame rate did not reach the row's rate (within 1 %) - restored, latched */
#define N48N_MODE_V_FIFO       4u    /* DIO_FIFO_ERROR set or DIO_ERROR_COUNT grew - restored, latched */
#define N48N_MODE_V_STREAM     5u    /* the DP stream or the OTG stopped - restored, latched */
#define N48N_MODE_V_STALL      6u    /* the OTG frame counter stalled (>= 250 ms) - restored, latched */
#define N48N_MODE_V_WRITE      7u    /* a register write was refused or failed - restored, latched */
#define N48N_MODE_V_RESTORE    8u    /* the restore did NOT verify (a register differs from the golden, or the 60 Hz rate is not back): run `dcnmode 0`, reboot if it persists */
#define N48N_MODE_V_WATCHDOG   9u    /* the watchdog thread had to restore - latched */
#define N48N_MODE_V_ABORT      10u   /* `dcnmode 0` or the kext stop asked the trial to end - restored, latched */
#define N48N_MODE_V_UNDERFLOW  11u   /* 0.0.606: HUBP0 / OPTC0 reported an underflow (or an unreadable status) during the rate window, the dwell or up to the restore - restored, latched */
#define N48N_MODE_V_CLOCK_LOST 12u   /* 0.0.607: the DFS readback fell under the row's need (or became unreadable) while the clock hold was up - restored, latched */
#define N48N_MODE_V_RESYNC     13u   /* 0.0.606: the DP1 blank / unblank did not complete (both attempts) - restored, latched */
#define N48N_MODE_V_HOLD       15u   /* 0.0.607: the trial and its restore verified, but the clock release did NOT (hold state STUCK: the clocks may stay at 545 MHz; harmless at 60 Hz; cold power cycle) - latched */
#define N48N_MODE_V_HELD       16u   /* ABI 1.7: MODE_HOLD returned with the row-120 mode UP (a snapshot, not an end): the hold is running */
#define N48N_MODE_V_LOCK       14u   /* 0.0.606: the OTG update lock was not acquired / released, DRR mode did not stick, or the V_TOTAL latch was not confirmed - restored, latched */
/* n48n_mode_result.deny (verdict DENIED) */
#define N48N_MODE_D_BAD_ROW    1u
#define N48N_MODE_D_BAD_DWELL  2u
#define N48N_MODE_D_LATCHED    3u    /* an earlier failure this boot */
#define N48N_MODE_D_GATE       4u    /* not a native boot with S1b POSITIVE PASS */
#define N48N_MODE_D_BUSY       5u    /* another trial is running */
#define N48N_MODE_D_NOT_ARMED  6u    /* the display layer did not bind */
#define N48N_MODE_D_ACQUIRED   7u    /* the scanout plane is acquired (or being restored) */
#define N48N_MODE_D_IN_USE     8u    /* dcnflip's pattern is held or a DCN interrupt source is enabled */
#define N48N_MODE_D_GOLDEN     9u    /* no golden copy could be taken (OTG0 not lit / unreadable) */
#define N48N_MODE_D_OTG        10u   /* OTG0 is not the only lit OTG, or its plane geometry is not the 2560x1440 linear ARGB8888 the goldens assume */
#define N48N_MODE_D_BASELINE   11u   /* the live raster / DTO / MSA are not the 60 Hz census mode the goldens were computed for */
#define N48N_MODE_D_STREAM     12u   /* DP1 is not the one enabled DP stream, or its status is not 'active' */
#define N48N_MODE_D_DRIFT      13u   /* a register the row writes no longer equals the golden copy: run `dcnmode 0` first */
#define N48N_MODE_D_CLOCKS     14u   /* DISPCLK / DPPCLK (DFS readback, C1 table) below what the row's DML needs, or unreadable / inconsistent */
#define N48N_MODE_D_STEP_DOWN  15u   /* row 120 before a passed row-50 trial on this boot */
#define N48N_MODE_D_NOTHING    16u   /* every register of the row already holds the row's value */
#define N48N_MODE_D_WATCHDOG   17u   /* the watchdog thread did not start */
#define N48N_MODE_D_FRAMES     18u   /* the OTG frame counter is not running at about 60 Hz before the trial */
#define N48N_MODE_D_UNDERFLOW  20u   /* 0.0.606: HUBP0 / OPTC0 underflow status set (or unreadable) before the trial and not clearable */
#define N48N_MODE_D_LOCK       21u   /* 0.0.606: the OTG master update lock is not usable: UPDATE_INSTANTLY set, lock or pending bits already set, DRR mode neither 0 nor 2, or unreadable */
#define N48N_MODE_D_HOLD       22u   /* 0.0.607: row 120's clock hold could not start or raise (ext.hold_pre = why it may not start, ext.hold_rc = how the raise failed; a failed raise is unwound before this answer) */
#define N48N_MODE_D_ROW2       23u   /* 0.0.607: row 120 before a row-2 PASS with LOCK_HELD_PROVEN on this boot */
#define N48N_MODE_D_ABORTED    24u   /* 0.0.607: `dcnmode 0` / the kext stop asked the trial to end while its clock hold was being raised: the hold was released at 60 Hz, nothing was written */
#define N48N_MODE_D_NOT_HELD   25u   /* ABI 1.7: MODE_RELEASE with no hold this boot */
#define N48N_MODE_D_ROW120_OFF 19u   /* row 120 is hard-denied unless the boot-arg navi48-row120=1 is present (0.0.607; hard-denied without exception in 0.0.605 / 0.0.606) */
/* n48n_mode_result.flags */
#define N48N_MODE_F_WROTE          (1u << 0)   /* at least one register was written */
#define N48N_MODE_F_RESTORED       (1u << 1)   /* every written register equals the golden again AND the 60 Hz rate is back */
#define N48N_MODE_F_RESTORE_FAILED (1u << 2)
#define N48N_MODE_F_LATCHED        (1u << 3)
#define N48N_MODE_F_WATCHDOG       (1u << 4)   /* the watchdog performed the restore */
#define N48N_MODE_F_DLG_LIVE       (1u << 5)   /* row 50: the live 60 Hz DLG/TTU stayed in place (no DML DLG/TTU register written) */
#define N48N_MODE_F_STEP_DOWN_DONE (1u << 6)   /* a row-50 trial has passed on this boot */
#define N48N_MODE_F_UNDERFLOW_UNREAD (1u << 7) /* 0.0.605: always set (no underflow read). 0.0.606: set only when an underflow register read back all-ones at least once (the read is then treated as an underflow) */
#define N48N_MODE_F_GOLDEN_AT_BIND (1u << 8)   /* the golden copy was taken at bind (before any native call could write); else at the first trial, still before any write */
#define N48N_MODE_F_RATE_TRIAL_OK  (1u << 9)
#define N48N_MODE_F_RATE_AFTER_OK  (1u << 10)
#define N48N_MODE_F_REGS_OK        (1u << 11)  /* after the restore every written register equals the golden under its mask */
#define N48N_MODE_F_UNDERFLOW_SEEN (1u << 13)  /* 0.0.606: an underflow was seen between the settle and the restore (the verdict is UNDERFLOW unless something worse happened) */
#define N48N_MODE_F_RESYNC_RUN     (1u << 14)  /* 0.0.606: the DP1 blank / unblank ran (row 1, row 2 or TF_RESYNC) */
#define N48N_MODE_F_LOCK_USED      (1u << 15)  /* 0.0.606: the OTG master update lock was taken (row 2) */
#define N48N_MODE_F_STILL_BLANKED  (1u << 16)  /* 0.0.606: THE DP1 STREAM IS STILL BLANKED after the restore (run `dcnmode 0`; reboot if it stays black) */
#define N48N_MODE_F_UF_CLEAR_REFUSED (1u << 17) /* 0.0.606: an underflow clear was refused because OPTC_INPUT_SOFT_RESET was set */
#define N48N_MODE_F_LATCH_CONFIRMED (1u << 18) /* 0.0.606: row 2: V_TOTAL latched (VERT_COUNT went past the old maximum, pending bits clear) */
#define N48N_MODE_F_LOCK_STILL_HELD (1u << 19) /* 0.0.606: THE OTG UPDATE LOCK IS STILL SET after the restore */
#define N48N_MODE_F_LOCK_HELD_PROVEN (1u << 20) /* 0.0.606: row 2: while blanked and locked, VERT_COUNT stayed at or below the old maximum for two frames (the write was held) */
#define N48N_MODE_F_UF_CLEARED     (1u << 21)  /* 0.0.606: an underflow clear strobe was WRITTEN (a DENIED verdict with this flag wrote that clear and nothing else) */
#define N48N_MODE_F_HOLD_HELD      (1u << 22)  /* 0.0.607: the clock hold was raised for this trial (row 120) */
#define N48N_MODE_F_HOLD_RELEASED  (1u << 23)  /* 0.0.607: ... and released after the verified 60 Hz restore (both clocks read below the need and at or above the start) */
#define N48N_MODE_F_HOLD_STUCK     (1u << 24)  /* 0.0.607: THE CLOCK HOLD IS STILL UP (restore not verified, or the release did not verify): the clocks stay raised, harmless at 60 Hz; cold power cycle */
#define N48N_MODE_F_HEALTH_BAD     (1u << 12)  /* after the restore: DIO_FIFO_ERROR set, DIO_ERROR_COUNT grew across the restore, DP1_DP_STEER_FIFO bit 4 set, or DIG1_DIG_FIFO_CTRL0 bits 29:28 set (verdict RESTORE) */
struct n48n_mode_ext {             /* 256 B (ABI 1.4, 0.0.606): raw registers are the dwords as read */
    uint32_t uf_hubp_before, uf_optc_before, uf_hubp_settle, uf_optc_settle;   /*   0: HUBP0_DCHUBP_CNTL / ODM0_OPTC_INPUT_GLOBAL_CONTROL before the trial, after the settle */
    uint32_t uf_hubp_after, uf_optc_after, uf_hubp_or, uf_optc_or;             /*  16: after the restore verify; the OR of every sample (settle included) */
    uint32_t uf_hubp_max, uf_timeout_max, uf_samples, uf_clears;               /*  32: max HUBP UNDERFLOW_STATUS (b30:28) and TIMEOUT_STATUS (b23:20); samples taken; clear strobes written */
    uint32_t uf_clear_refused, uf_clear_stuck, uf_first_ms, reserved0;         /*  48: refused (OPTC soft reset set); strobe bit read back set; ms from the trial's first write to the first judged underflow (0 = none) */
    uint32_t rs_runs, rs_attempts, rs_rc_first, rs_rc_last;                    /*  64: resync runs (apply + restore); blank / unblank attempts; first non-OK and last N48N_MODE_SEQ_* code of the apply's resync */
    uint32_t rs_blank_us, rs_unblank_us, rs_dio_errs, rs_stream_after;         /*  80: the apply's blank and unblank durations; DIO_ERROR_COUNT growth across it (reported); DP1_DP_VID_STREAM_CNTL after the apply */
    uint32_t rs_steer_after, rs_dig_after, rs_restore_rc, rs_restore_dio_errs; /*  96: DP1_DP_STEER_FIFO / DIG1_DIG_FIFO_CTRL0 after the apply; the restore's resync code (0 ok); DIO growth across the restore's resync */
    uint32_t lk_rc, lk_wait_us, lk_pending_seen, lk_vert_max;                  /* 112: the lock sequence's N48N_MODE_SEQ_* code; time to UPDATE_LOCK_STATUS; OR of the pending bits seen (DOUBLE_BUFFER_CONTROL 0x231 mask); max VERT_COUNT of the latch witness */
    uint32_t lk_vert_samples, lk_db_before, lk_db_during, lk_db_after;         /* 128: witness samples; raw OTG0_OTG_DOUBLE_BUFFER_CONTROL before / after the apply's unlock / after the restore */
    uint32_t lk_lock_after, lk_sel_before, lk_pipe_before, lk_cleared;         /* 144: raw OTG0_OTG_MASTER_UPDATE_LOCK after the restore; GLOBAL_CONTROL2 before; PIPE_UPDATE_STATUS before; stuck-lock clears written */
    uint32_t lk_restore_rc, lk_confirmed, lk_drr_before, lk_drr_during;        /* 160: the restore's lock sequence code (0 ok); latch witnesses that confirmed (apply, restore); DRR mode (b25:24) before / during */
    uint32_t lk_drr_after, lk_expect_lines, tflags, lk_hold_max;                 /* 176: DRR mode after the restore; V_TOTAL lines the row expects; the flags word as received */
    uint32_t hold_state, hold_rc, hold_raise_ms, hold_rel_rc;                  /* 192: (0.0.607) n48dal::HoldSt after the trial; HoldRc of the raise; ms to raise; RelRc of the release (0 ok) */
    uint32_t hold_khz_raised[2], hold_khz_released[2];                         /* 208: DFS readback (kHz) [0] DISPCLK [1] DPPCLK after the raise / after the release */
    uint32_t hold_dfs_min[2], hold_samples, hold_lost;                         /* 224: lowest DFS readback seen while held; samples taken (every 250 ms); samples under the need */
    uint32_t hold_pre, hold_flags, hold_msgs, hold_release_ms;                 /* 240: HoldPre (why a hold would not start); n48dal kHf* flags; DAL messages sent by the hold; ms to release */
};

struct n48n_mode_result {          /* 512 B since ABI 1.4 (256 B in 1.3: the first 256 B are unchanged) */
    uint32_t row, verdict, latched, deny;                         /*   0 */
    uint32_t rc, flags, nwrite, nskip;                            /*  16: nwrite = registers written; nskip = registers of the row already at the row's value */
    uint32_t dwell_req_ms, dwell_done_ms, settle_ms, measure_ms;  /*  32 */
    uint32_t rate_before_mhz, rate_trial_mhz, rate_expect_mhz, rate_after_mhz;   /*  48: OTG frame rate, millihertz */
    uint32_t frames_trial, ms_trial, frames_after, ms_after;      /*  64: the two 3 s measurements */
    uint32_t fifo_before, fifo_after, errcnt_before, errcnt_after; /* 80: OTG0_PIXEL_RATE_CNTL DIO_FIFO_ERROR (bits 15:14) and DIO_ERROR_COUNT (27:16) */
    uint32_t stream_before, stream_trial, stream_after, steer_after; /* 96: raw DP1_DP_VID_STREAM_CNTL (bit 0 enable, bit 16 status) x3, raw DP1_DP_STEER_FIFO after */
    uint32_t disp_khz, dpp_khz, need_disp_khz, need_dpp_khz;      /* 112: the DFS readback and the row's DML need */
    uint32_t mismatch, bad_abs, bad_have, bad_want;               /* 128: registers not equal to the golden after the restore (masked), and the first one */
    uint32_t fail_phase, restore_tries, wd_fired, golden_n;       /* 144: fail_phase 1 pre-flight 2 write 3 settle 4 measure 5 dwell 6 restore 7 verify; golden_n = registers in the golden copy */
    uint32_t dto_phase[3], dto_modulo;                            /* 160: DP_DTO0_PHASE before / during the trial / after; the modulo */
    uint32_t vtotal_reg[3], htotal_reg;                           /* 176: OTG_V_TOTAL register before / during / after; OTG_H_TOTAL */
    uint32_t pix_khz_expect, first_write_abs, last_write_abs, hold_end;   /* 192: hold_end (ABI 1.7, was reserved0) = N48N_HOLD_END_* */
    uint32_t dio_write_errs, dio_trial_errs, dio_restore_errs, dig_fifo_after;   /* 208: DIO_ERROR_COUNT growth across the writes (reported, not judged) / during measure + dwell (judged) / across the restore (judged); raw DIG1_DIG_FIFO_CTRL0 after */
    uint32_t uf_optc_rs_dto, uf_optc_rs_latch, uf_optc_ap_trans, lk_hold_rc;   /* 224: (ABI 1.6, 0.0.608, row 120) raw ODM0_OPTC_INPUT_GLOBAL_CONTROL: right after the restore's DTO step (recorded, not cleared) / right after its V_TOTAL latch, still blanked (recorded and cleared) / after the apply's MSA, before the unblank (recorded and cleared); the apply's lock-hold witness code (N48N_MODE_SEQ_*, stored, never judged) */
    uint32_t dio_hold_errs, dio_post_unblank_errs, lk_hold_ran, hold_flags;    /* 240: hold_flags (ABI 1.7, was reserved1) = N48N_HOLD_FL_*;  DIO_ERROR_COUNT growth across the clock raise / from the raise to just after the apply's unblank (both reported, not judged); 1 = the apply's lock-hold witness ran (ext.lk_hold_max = its max VERT_COUNT); 0 */
    struct n48n_mode_ext ext;                                     /* 256: ABI 1.4 */
};

N48N_STATIC_ASSERT(sizeof(struct n48n_gem_create_in) == 32, "n48n_gem_create_in is 32 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_gem_va) == 64, "n48n_gem_va is 64 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_ctx) == 16, "n48n_ctx is 16 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_cs_ib) == 32, "n48n_cs_ib is 32 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_cs_in) == 32, "n48n_cs_in header is 32 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_info) == 192, "n48n_info is 192 B");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_gem_va, va_address) == 16, "gem_va.va_address at 16");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_gem_va, vm_timeline_point) == 40, "gem_va.vm_timeline_point at 40");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_gem_va, input_fence_syncobj_handles) == 56, "gem_va.input_fence_syncobj_handles at 56");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_cs_ib, va_start) == 8, "cs_ib.va_start at 8");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_cs_ib, ib_bytes) == 16, "cs_ib.ib_bytes at 16");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_cs_in, reserved) == 24, "cs_in.reserved at 24");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, vram_vis_total) == 32, "info.vram_vis_total at 32");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, gtt_cap) == 64, "info.gtt_cap at 64");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, seq_emitted) == 104, "info.seq_emitted at 104");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, va_low_first) == 120, "info.va_low_first at 120");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, max_bos) == 152, "info.max_bos at 152");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_info, reserved) == 160, "info.reserved at 160");
N48N_STATIC_ASSERT(sizeof(struct n48n_scan_query) == 96, "n48n_scan_query is 96 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_scan_reg) == 32, "n48n_scan_reg is 32 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_scan_slot) == 32, "n48n_scan_slot is 32 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_scan_status) == 256, "n48n_scan_status is 256 B");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_query, frame_count) == 48, "scan_query.frame_count at 48");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_query, acquired) == 80, "scan_query.acquired at 80");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_reg, offset) == 8, "scan_reg.offset at 8");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_reg, width) == 24, "scan_reg.width at 24");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, frame_count) == 16, "scan_status.frame_count at 16");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, presents) == 48, "scan_status.presents at 48");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, vupdates) == 80, "scan_status.vupdates at 80");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, first_latch_ns) == 112, "scan_status.first_latch_ns at 112");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, idle_ms) == 128, "scan_status.idle_ms at 128");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_scan_status, slot) == 160, "scan_status.slot at 160");
N48N_STATIC_ASSERT(sizeof(struct n48n_dal_result) == 256, "n48n_dal_result is 256 B");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, vco_khz) == 32, "dal_result.vco_khz at 32");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, khz_before) == 64, "dal_result.khz_before at 64");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, frame_before) == 96, "dal_result.frame_before at 96");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, underflow) == 112, "dal_result.underflow at 112");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, smu_version) == 128, "dal_result.smu_version at 128");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, dpm_count) == 144, "dal_result.dpm_count at 144");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, dc_max_mhz) == 160, "dal_result.dc_max_mhz at 160");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, dpm_mhz) == 176, "dal_result.dpm_mhz at 176");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_dal_result, deny) == 240, "dal_result.deny at 240");
N48N_STATIC_ASSERT(sizeof(struct n48n_mode_ext) == 256, "n48n_mode_ext is 256 B");
N48N_STATIC_ASSERT(sizeof(struct n48n_mode_result) == 512, "n48n_mode_result is 512 B (ABI 1.4)");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, ext) == 256, "mode_result.ext at 256");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_ext, lk_rc) == 112, "mode_ext.lk_rc at 112");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, dwell_req_ms) == 32, "mode_result.dwell_req_ms at 32");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, rate_before_mhz) == 48, "mode_result.rate_before_mhz at 48");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, fifo_before) == 80, "mode_result.fifo_before at 80");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, disp_khz) == 112, "mode_result.disp_khz at 112");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, mismatch) == 128, "mode_result.mismatch at 128");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, dto_phase) == 160, "mode_result.dto_phase at 160");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, pix_khz_expect) == 192, "mode_result.pix_khz_expect at 192");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, dio_write_errs) == 208, "mode_result.dio_write_errs at 208");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, uf_optc_rs_dto) == 224, "mode_result.uf_optc_rs_dto at 224 (ABI 1.6)");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_result, dio_hold_errs) == 240, "mode_result.dio_hold_errs at 240");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_ext, hold_state) == 192, "mode_ext.hold_state at 192");
N48N_STATIC_ASSERT(__builtin_offsetof(struct n48n_mode_ext, hold_pre) == 240, "mode_ext.hold_pre at 240");

#endif /* NAVI48_NATIVE_ABI_H */
