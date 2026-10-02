//
//  amdgpu_psp.h — post-SOS PSP protocol (GPCOM ring, TMR, LOAD_IP_FW).
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/amdgpu_psp.h   (PSPContext, FirmwareLoader, decls)
//      dext/amdgpu/psp_v14_0.cpp  (the GFX command structs it declares
//                                  privately: rb_frame, cmd headers)
//  which in turn ports drivers/gpu/drm/amd/amdgpu/{amdgpu_psp.c,
//  psp_v14_0.c, psp_gfx_if.h} from Linux.
//
//  ============ Deviations from the reference ============
//  1. DriverKit gone. Every IOBufferMemoryDescriptor/IODMACommand pair is
//     either dropped (the buffer lives in VRAM and is reached through the
//     BAR0 aperture) or replaced by amdgpu::SysMem. Specifically dropped:
//       fwPriBuffer/fwPriDMACommand, ringBuffer/ringDMACommand,
//       cmdBuffer/cmdDMACommand, fenceBuffer/fenceDMACommand,
//       tmrBuffer/tmrDMACommand, fwBufSysmemBuffer/fwBufSysmemDMA.
//  2. GART gone. The reference's `GARTContext *gart` + three `GARTBinding`
//     members and `psp_setup_fw_buf_sysmem()` need gmc_bind_existing(),
//     which this kext does not have yet. fw_buf therefore stays VRAM-backed
//     (which is also what the reference falls back to on real hardware),
//     and `fwBufSysmem*` is not carried over. See psp_fw_buf_stage().
//  3. *CPUAddr members for VRAM-backed buffers (ring/cmd/fence/tmr/fw_pri)
//     are dropped instead of kept as permanent nullptrs — CPU access is
//     bar0_memcpy_to_vram / RBAR2_32 only. The matching *VRAMOffset member
//     (an ABSOLUTE VRAM byte offset, i.e. dev.vramBase + the reference's
//     hardcoded constant) is added next to each *BusAddr.
//  4. std::function is STL; FirmwareLoader::get_payload becomes a plain
//     function pointer + void* context, wrapped so call sites read exactly
//     like the reference (`loader.get_payload(type, bus, size)`).
//  5. Added: psp_adopt_sos_alive() — our SOS chain lives in src/psp.cpp, so
//     psp_load_sos / psp_bootloader_load_component / psp_load_sos_package are
//     deliberately NOT ported; this records the state they would have left.
//     Added: psp_fw_buf_stage() so a caller with embedded blobs can produce
//     the (bus_addr, size) pair FirmwareLoader must return.
//  6. Added: explicit psp_gfx_resp layout + kPSPGfxResp*Offset constants.
//     The reference reads resp.status at cmd_buf+864 and resp.uresp at +928
//     but reads resp.tmr_size at cmd_buf+16 (a missing `864 +`; offset 16 is
//     resp_buf_addr_hi, which we write as 0). We read tmr_size at 864+16 and
//     ALSO log the reference's +16 dword so the two can be compared on
//     hardware.
//  7. psp_parse_ta_microcode is NOT ported (it belongs to the TA package,
//     not the SOS container), so psp.asd.parsed stays false and
//     psp_asd_initialize is a logged no-op until someone ports it.
//
//  PSPContext is a POD with no constructors (kext ABI: no static
//  initialisers). Callers must zero it: `PSPContext psp = {};`.
//

#pragma once

#include <stdint.h>

#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"

namespace amdgpu {

// ============================================================
// FirmwareLoader — callback interface for psp_load_non_psp_fw.
//
// The caller implements the callback to provide firmware for each
// fw_type. It is responsible for staging the firmware bytes somewhere
// PSP can read them (psp_fw_buf_stage() does this for VRAM fw_buf) and
// returning the resulting GPU address and size.
//
// Return kIOReturnSuccess if the payload exists, kIOReturnUnsupported if
// this fw_type is not present in the current firmware version (the loader
// then skips it, exactly like the reference).
// ============================================================
struct FirmwareLoader {
    using GetPayloadFn = kern_return_t (*)(void *ctx, uint32_t fw_type,
                                           uint64_t &out_bus_addr,
                                           uint32_t &out_size);
    GetPayloadFn fn;     // may be null → every get_payload() is Unsupported
    void        *ctx;    // opaque, handed back to fn

    kern_return_t get_payload(uint32_t fw_type, uint64_t &out_bus_addr,
                              uint32_t &out_size) const {
        if (fn == nullptr) return kIOReturnUnsupported;
        return fn(ctx, fw_type, out_bus_addr, out_size);
    }
};

struct PSPContext {
    // ---- fw_pri: PSP reads each staged binary from here (VRAM) ----------
    // The reference allocated this with IOBufferMemoryDescriptor and later
    // moved it to VRAM; we are VRAM-only. fwPriVRAMOffset is an absolute
    // VRAM byte offset (dev.vramBase + kFwPriVRAMOffset), fwPriBusAddr the
    // matching MC address (dev.vramMC(fwPriVRAMOffset)).
    uint64_t  fwPriVRAMOffset;
    uint64_t  fwPriBusAddr;
    uint64_t  fwPriSize;

    // ---- PSP firmware sub-binary descriptors ----------------------------
    // Mirrors upstream `struct psp_bin_desc`. Each sub-firmware lives inside
    // the same `psp_<chip>_sos.bin`; start_addr points INTO the caller's
    // blob (for us the embedded fw_psp_14_0_3_sos[] array, which lives as
    // long as the kext). Filled by psp_parse_sos_microcode().
    struct PSPSubBin {
        const uint8_t *start_addr;
        uint64_t       size_bytes;
        uint32_t       fw_version;
    };
    PSPSubBin sos;
    PSPSubBin sys;          // upstream "sys_drv"
    PSPSubBin kdb;
    PSPSubBin toc;
    PSPSubBin spl;
    PSPSubBin rl;
    PSPSubBin soc_drv;
    PSPSubBin intf_drv;
    PSPSubBin dbg_drv;      // a.k.a. had_drv on psp_v14
    PSPSubBin ras_drv;
    PSPSubBin ipkeymgr_drv;
    PSPSubBin spdm_drv;
    PSPSubBin sys_drv_aux;  // v1.3 only
    PSPSubBin sos_aux;      // v1.3 only

    // ---- ASD (Authenticated Secure Display) TA --------------------------
    // Upstream calls psp_asd_initialize between AUTOLOAD_RLC and psp_rl_load
    // (amdgpu_psp.c:3153). Populated by psp_parse_ta_microcode in the
    // reference — NOT ported here, so `parsed` stays false and
    // psp_asd_initialize no-ops (see deviation 7 above).
    struct ASDContext {
        bool      parsed;
        uint32_t  size_bytes;
        uint32_t  fw_version;
        uint64_t  ucode_mc_addr;   // MC address of the staged ASD ucode
        uint64_t  ucode_vram_off;  // absolute VRAM byte offset of the same
        uint32_t  session_id;      // PSP-assigned, post-load
        uint32_t  resp_status;     // last LOAD_ASD response status
    };
    ASDContext asd;

    // The `_sos.bin` blob the sub-bin descriptors point into (not owned).
    const uint8_t *sos_fw_blob;
    uint64_t       sos_fw_blob_size;

    // Legacy slots the reference's psp_load_sos consumed. Kept so a future
    // caller can set them; nothing in this file reads them.
    const uint8_t *sosFirmware;
    uint64_t       sosFirmwareSize;

    bool     sosAlive;
    // Recorded by psp_adopt_sos_alive (our addition):
    uint32_t sosSignOfLife;      // MP0 C2PMSG_81 (0x0191ae05 on this card)
    uint32_t sosFwVersion;       // MP0 C2PMSG_58 (0x003a1014 on this card)
    uint32_t sosFeatureVersion;  // MP0 C2PMSG_59 (0x003a0c43 on this card)

    // ---- PSP runtime database (psp_read_runtime_db) ---------------------
    bool      runtimeDbRead;
    uint16_t  runtimeDbCookie;   // PSP_RUNTIME_DB_COOKIE_ID = 0x0ed5 expected
    uint16_t  runtimeDbVersion;
    uint32_t  bootCfgBitmask;    // BOOT_CFG_FEATURE_* flags
    uint32_t  scpmStatus;        // 0=disabled, 1=enabled, 2=enabled+err
    bool      scpmEnabled;

    // ---- PSP GPCOM command ring (km_ring), VRAM-backed ------------------
    uint64_t  ringVRAMOffset;    // absolute VRAM byte offset
    uint64_t  ringBusAddr;       // MC address written to C2PMSG_69/70
    uint64_t  ringSize;          // bytes advertised to PSP via C2PMSG_71
    bool      ringCreated;

    // ---- command buffer + fence buffer, VRAM-backed ---------------------
    uint64_t  cmdVRAMOffset;
    uint64_t  cmdBusAddr;
    uint64_t  fenceVRAMOffset;
    uint64_t  fenceBusAddr;
    uint32_t  fenceCounter;

    // ---- TMR (Trusted Memory Region) ------------------------------------
    // On psp_v14_0_2/3 SOS owns the TMR (boot_time_tmr) and SETUP_TMR is
    // skipped — the slot below is reserved but only programmed on the
    // legacy branch of psp_setup_tmr.
    uint64_t  tmrVRAMOffset;
    uint64_t  tmrBusAddr;
    uint64_t  tmrSize;
    bool      tmrSetUp;

    // ---- firmware.fw_buf equivalent -------------------------------------
    // Upstream amdgpu_ucode_create_bo allocates ONE buffer and gives EACH
    // ucode its own MC address inside it; psp_prep_load_ip_fw_cmd_buf then
    // passes that per-ucode address. Passing one shared address for every
    // LOAD_IP_FW makes PSP reject SDMA / CP_RS64 / MES with
    // TEE_BAD_PARAMETERS (0xFFFF0006). So bump-allocate per payload.
    uint64_t  fwBufVRAMOffset;   // absolute VRAM byte offset of fw_buf
    uint64_t  fwBufBaseMC;       // MC address of the same
    uint64_t  fwBufSize;         // total bytes reserved
    uint64_t  fwBufBumpOffset;   // next free offset within fw_buf

    // Navi48Bringup: TMR address the PSP reported for each LOAD_IP_FW fw_type (0 = not loaded).
    // The RS64 CP needs these for its IC/DC base registers when the PSP does not program them.
    uint64_t tmr_fw_addr_by_type[128];
};

// ============================================================
// PSP GFX command interface — upstream psp_gfx_if.h layouts.
// These were file-private in the reference's psp_v14_0.cpp; they are
// the wire format PSP reads, so they live in the header here.
// ============================================================

// `struct psp_gfx_rb_frame` (64 bytes). PSP reads our writes without
// re-interpretation, so the layout must match byte for byte.
struct PSPGfxRBFrame {
    uint32_t cmd_buf_addr_lo;
    uint32_t cmd_buf_addr_hi;
    uint32_t cmd_buf_size;
    uint32_t fence_addr_lo;
    uint32_t fence_addr_hi;
    uint32_t fence_value;
    uint32_t sid_lo;
    uint32_t sid_hi;
    uint8_t  vmid;
    uint8_t  frame_type;
    uint8_t  reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PSPGfxRBFrame) == 64, "PSPGfxRBFrame must be 64 B");

constexpr uint32_t kPSPGfxCmdRespSize = 1024;  // upstream psp_gfx_cmd_resp
constexpr uint32_t kPSPFenceBufSize   = 16384; // page-aligned (only 4 B used)
constexpr uint32_t kPSPCmdBufSize     = 16384; // page-aligned (only 1 KB used)
constexpr uint32_t kPSPTMRDefaultSize = 4 * 1024 * 1024;  // 4 MB
constexpr uint32_t kPSPGfxCmdBufVersion = 1;   // PSP_GFX_CMD_BUF_VERSION

// Leading fields of `struct psp_gfx_cmd_resp`. Explicitly __packed: clang
// on Darwin does not apply the implicit packing Linux GCC + __le32 does.
// The command union starts right after, at offset 28.
struct PSPGfxCmdRespHeader {
    uint32_t buf_size;
    uint32_t buf_version;       // PSP_GFX_CMD_BUF_VERSION = 1
    uint32_t cmd_id;
    uint32_t resp_buf_addr_lo;  // 0 for the GPCOM ring
    uint32_t resp_buf_addr_hi;  // 0
    uint32_t resp_offset;       // 0
    uint32_t resp_buf_size;     // 0
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdRespHeader) == 28,
              "PSPGfxCmdRespHeader must be 28 B (cmd union starts there)");
constexpr uint32_t kPSPGfxCmdUnionOffset = 28;

// `struct psp_gfx_cmd_setup_tmr` @ +28.
struct PSPGfxCmdSetupTmr {
    uint32_t buf_phy_addr_lo;
    uint32_t buf_phy_addr_hi;
    uint32_t buf_size;
    uint32_t tmr_flags;         // bit0=sriov, bit1=virt_phy_addr
    uint32_t system_phy_addr_lo;
    uint32_t system_phy_addr_hi;
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdSetupTmr) == 24, "psp_gfx_cmd_setup_tmr = 24 B");

// `struct psp_gfx_cmd_load_ip_fw` @ +28.
struct PSPGfxCmdLoadIpFw {
    uint32_t fw_phy_addr_lo;
    uint32_t fw_phy_addr_hi;
    uint32_t fw_size;
    uint32_t fw_type;           // enum psp_gfx_fw_type
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdLoadIpFw) == 16, "psp_gfx_cmd_load_ip_fw = 16 B");

// `struct psp_gfx_cmd_load_toc` @ +28. toc_size is the PAYLOAD size, not
// the file size. (The reference built this with a bare uint32_t[3].)
struct PSPGfxCmdLoadToc {
    uint32_t toc_phy_addr_lo;
    uint32_t toc_phy_addr_hi;
    uint32_t toc_size;
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdLoadToc) == 12, "psp_gfx_cmd_load_toc = 12 B");

// `struct psp_gfx_cmd_load_ta` @ +28 (used by LOAD_ASD). For ASD the
// shared-memory half is all zero — PSP_ASD_SHARED_MEM_SIZE == 0 upstream.
// (The reference built this with a bare uint32_t[6].)
struct PSPGfxCmdLoadTa {
    uint32_t app_phy_addr_lo;
    uint32_t app_phy_addr_hi;
    uint32_t app_len;
    uint32_t cmd_buf_phy_addr_lo;
    uint32_t cmd_buf_phy_addr_hi;
    uint32_t cmd_buf_len;
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdLoadTa) == 24, "psp_gfx_cmd_load_ta = 24 B");

// `struct psp_gfx_resp` — the response PSP writes back into the SAME
// cmd buffer, at offset kPSPGfxRespOffset. Read it with RBAR2_32.
struct psp_gfx_resp {
    uint32_t status;        // +0   0 = success (PSP convention)
    uint32_t session_id;    // +4   in response to LOAD_TA / LOAD_ASD
    uint32_t fw_addr_lo;    // +8   FW address within TMR (LOAD_IP_FW)
    uint32_t fw_addr_hi;    // +12
    uint32_t tmr_size;      // +16  TMR bytes PSP needs (LOAD_TOC)
    uint32_t reserved[11];  // +20
    uint32_t uresp[8];      // +64  union psp_gfx_uresp_*
} __attribute__((packed));
static_assert(sizeof(psp_gfx_resp) == 96, "psp_gfx_resp must be 96 B");

// Byte offsets of the response fields WITHIN the 1 KB cmd buffer.
constexpr uint32_t kPSPGfxRespOffset          = 864;
constexpr uint32_t kPSPGfxRespStatusOffset    = kPSPGfxRespOffset + 0;
constexpr uint32_t kPSPGfxRespSessionIdOffset = kPSPGfxRespOffset + 4;
constexpr uint32_t kPSPGfxRespFwAddrLoOffset  = kPSPGfxRespOffset + 8;
constexpr uint32_t kPSPGfxRespFwAddrHiOffset  = kPSPGfxRespOffset + 12;
constexpr uint32_t kPSPGfxRespTmrSizeOffset   = kPSPGfxRespOffset + 16;
// psp_gfx_uresp_fw_reserve_info: hi, then lo, then size (order is upstream's).
constexpr uint32_t kPSPGfxRespUrespOffset     = kPSPGfxRespOffset + 64;

// Subset of psp_gfx_fw_type — full enum in upstream psp_gfx_if.h:208.
namespace PSPGfxFwType {
    constexpr uint32_t SMU       = 18;   // PMFW
    // Legacy SDMA instances (sdma_v4-style packaging). RDNA4 uses
    // SDMA_UCODE_TH0 instead — see below.
    constexpr uint32_t SDMA0     = 9;
    constexpr uint32_t SDMA1     = 10;
    // RDNA4 (sdma_v7_1) packs both engines into one RS64 firmware that
    // PSP loads ONCE with TH0=71. Per upstream amdgpu_sdma_init_microcode.
    constexpr uint32_t SDMA_UCODE_TH0 = 71;
    constexpr uint32_t RLC_G     = 8;
    constexpr uint32_t CP_ME     = 1;
    constexpr uint32_t CP_PFP    = 2;
    constexpr uint32_t CP_MEC    = 4;
    // GFX12 / RDNA4 RS64 CP firmwares.
    constexpr uint32_t RS64_PFP      = 87;
    constexpr uint32_t RS64_ME       = 88;
    constexpr uint32_t RS64_MEC      = 89;
    constexpr uint32_t RS64_PFP_P0   = 90;
    constexpr uint32_t RS64_PFP_P1   = 91;
    constexpr uint32_t RS64_ME_P0    = 92;
    constexpr uint32_t RS64_ME_P1    = 93;
    constexpr uint32_t RS64_MEC_P0   = 94;
    constexpr uint32_t RS64_MEC_P1   = 95;
    constexpr uint32_t RS64_MEC_P2   = 96;
    constexpr uint32_t RS64_MEC_P3   = 97;
    constexpr uint32_t IMU_I     = 68;
    constexpr uint32_t IMU_D     = 69;
    // Standalone MES (legacy / non-uni packaging).
    constexpr uint32_t RS64_MES        = 76;
    constexpr uint32_t RS64_MES_STACK  = 77;
    constexpr uint32_t RS64_KIQ        = 78;
    constexpr uint32_t RS64_KIQ_STACK  = 79;
    // PSP-embedded Register List (REG_LIST). Loaded via psp_rl_load AFTER
    // AUTOLOAD_RLC — PSP's autoload state machine waits for this firmware
    // to arrive before completing GC bringup. psp.rl is populated from
    // the v2 SOS package by psp_parse_sos_microcode (PSP_FW_TYPE_PSP_RL).
    constexpr uint32_t REG_LIST        = 67;
    // uni_mes packaging: ucode + data loaded as two LOAD_IP_FW frames.
    constexpr uint32_t CP_MES          = 33;
    constexpr uint32_t CP_MES_DATA     = 34;
    // Note: upstream `enum psp_gfx_fw_type` calls this MES_STACK (=34);
    // we use CP_MES_DATA as a clearer name for the uni_mes data half.
    constexpr uint32_t MES_STACK       = 34;   // alias of CP_MES_DATA
    // KIQ pipe variants — psp_gfx_if.h:285-286. Same uni_mes.bin bytes
    // as CP_MES/CP_MES_DATA but tagged differently so PSP places them
    // in the KIQ TMR slot. Required on RDNA4 because enable_uni_mes=1
    // is the default and mes_v12_0_early_init registers both pipes.
    constexpr uint32_t CP_MES_KIQ      = 81;
    constexpr uint32_t MES_KIQ_STACK   = 82;

    // RLC sub-firmwares (v2.1+). All emitted from a single rlc.bin
    // file when the relevant rlc_firmware_header_v2_x has non-zero
    // size for that sub-bin. From psp_gfx_if.h.
    constexpr uint32_t RLC_RESTORE_LIST_GPM_MEM  = 20;
    constexpr uint32_t RLC_RESTORE_LIST_SRM_MEM  = 21;
    constexpr uint32_t RLC_RESTORE_LIST_SRM_CNTL = 22;
    constexpr uint32_t RLC_V                     = 7;   // psp_gfx_if.h:216
    constexpr uint32_t RLC_P                     = 25;
    constexpr uint32_t RLC_IRAM                  = 26;
    constexpr uint32_t RLC_DRAM_BOOT             = 48;
    // RLC v2.4 tap delays.
    constexpr uint32_t GLOBAL_TAP_DELAYS         = 27;
    constexpr uint32_t SE0_TAP_DELAYS            = 28;
    constexpr uint32_t SE1_TAP_DELAYS            = 29;
    constexpr uint32_t SE2_TAP_DELAYS            = 65;
    constexpr uint32_t SE3_TAP_DELAYS            = 66;
}

// GFX command IDs (subset). Full list in upstream psp_gfx_if.h.
namespace PSPGfxCmd {
    constexpr uint32_t LOAD_TA               = 1;
    constexpr uint32_t UNLOAD_TA             = 2;
    constexpr uint32_t INVOKE_CMD            = 3;
    constexpr uint32_t LOAD_ASD              = 4;   // Authenticated Secure Display
    constexpr uint32_t SETUP_TMR             = 5;
    constexpr uint32_t LOAD_IP_FW            = 6;
    constexpr uint32_t LOAD_TOC              = 0x20;
    constexpr uint32_t AUTOLOAD_RLC          = 0x21;
    constexpr uint32_t FB_FW_RESERV_ADDR     = 0x50;
    constexpr uint32_t FB_FW_RESERV_EXT_ADDR = 0x51;
}

// PSP error code we special-case: SOS too old to know a command.
constexpr uint32_t kPSPErrUnknownCommand = 0x100;

// ============================================================
// API
// ============================================================

//
// psp_init — pick the fixed VRAM slots and populate PSPContext.
// All offsets are relative to dev.vramBase (PORTING.md VRAM layout):
//   fw_pri +0x000000 (1 MiB), ring +0x100000, cmd +0x104000,
//   fence +0x108000, TMR +0x200000 (4 MiB), fw_buf +0x1000000 (8 MiB).
// Requires MP0 + MMHUB IP bases resolved and dev.vramMcBase set.
// Idempotent.
//
// psp_gfx_if.h GFX_CTRL_CMD_ID_*: the ring control commands written to
// C2PMSG_64. INIT_GPCOM_RING is what psp_ring_create sends as
// (kPSPRingTypeKM << 16); DESTROY_RINGS is its counterpart.
constexpr uint32_t kPSPGfxCtrlInitGpcomRing = 0x00020000u;
constexpr uint32_t kPSPGfxCtrlDestroyRings  = 0x00030000u;

kern_return_t psp_init(DeviceContext &dev, PSPContext &psp);

//
// psp_adopt_sos_alive — NOT in the reference. src/psp.cpp already runs the
// bootloader chain (KDB…SOS) and leaves SOS alive; this records the state
// psp_load_sos would have left: psp.sosAlive, dev.psoCAlive, and the
// version stamps in MP0 C2PMSG_58 / C2PMSG_59 / C2PMSG_81.
// Returns kIOReturnNotReady if C2PMSG_81 reads back zero (SOS not up).
//
kern_return_t psp_adopt_sos_alive(DeviceContext &dev, PSPContext &psp);

//
// psp_read_runtime_db — port of upstream psp_get_runtime_db_entry +
// psp_sw_init runtime DB read (amdgpu_psp.c:376-449, 471-503). Reads the
// PSP runtime data header at (vram_size - 0x100000) and fills in
// boot_cfg_bitmask + scpm_status if the cookie matches. Idempotent.
//
kern_return_t psp_read_runtime_db(DeviceContext &dev, PSPContext &psp,
                                  uint64_t vram_size_bytes);

//
// psp_parse_sos_microcode — port of upstream psp_init_sos_microcode.
// Walks the AMD firmware header in `fw_data` (raw bytes of
// psp_<chip>_sos.bin), auto-detects v1 vs v2 layout, and populates
// psp.sos / psp.kdb / psp.toc / psp.rl / … with pointers into fw_data.
// The caller retains ownership of fw_data.
//
kern_return_t psp_parse_sos_microcode(PSPContext &psp,
                                      const uint8_t *fw_data,
                                      uint64_t fw_size);

//
// psp_release — reset PSP state. Nothing to free (VRAM-backed buffers).
//
void psp_release(PSPContext &psp);

//
// psp_is_sos_alive — port of psp_v14_0_is_sos_alive. True when MP0
// C2PMSG_81 is non-zero (SOS has reported in).
//
bool psp_is_sos_alive(const DeviceContext &dev);

//
// psp_ring_create — port of psp_v14_0_ring_create (non-SR-IOV path).
// Zeroes the VRAM ring, programs address+size+type into C2PMSG_69..71+64,
// waits for the response flag in C2PMSG_64, then zeroes cmd + fence.
// Ring type is PSP_RING_TYPE__KM (2).
//
kern_return_t psp_ring_create(DeviceContext &dev, PSPContext &psp);

// psp_ring_destroy — port of psp_v14_0_ring_stop (non-SR-IOV path):
// C2PMSG_64 = GFX_CTRL_CMD_ID_DESTROY_RINGS (0x00030000), 20 ms settle, wait
// for the response flag. Needed on unload: the SOS keeps the GPCOM ring
// across a kext reload (we deliberately leave the PSP running), and a second
// psp_ring_create then fails with status 0x115 — which is exactly what the
// first kmutil-reload test hit. Clears psp.ringCreated on success.
kern_return_t psp_ring_destroy(DeviceContext &dev, PSPContext &psp);

//
// psp_ring_cmd_submit — port of psp_ring_cmd_submit + psp_cmd_submit_buf.
// Synchronously submits one psp_gfx_cmd_resp-sized frame and waits for the
// fence. cmdSize must be kPSPGfxCmdRespSize (1024). *outRespStatus gets the
// PSP response status (0 = success).
//
kern_return_t psp_ring_cmd_submit(DeviceContext &dev, PSPContext &psp,
                                  const void *cmd, uint32_t cmdSize,
                                  uint32_t *outRespStatus);

//
// psp_setup_tmr — port of psp_tmr_load / psp_tmr_init. On psp_v14_0_2/3
// upstream's psp_skip_tmr() is true (boot_time_tmr + autoload_supported),
// so SETUP_TMR is NOT submitted; instead the TOC sub-bin from the SOS
// package is pushed with LOAD_TOC so PSP knows the per-IP TMR slot layout.
// Required before any TMR-resident LOAD_IP_FW.
//
kern_return_t psp_setup_tmr(DeviceContext &dev, PSPContext &psp);

//
// psp_load_ip_fw — port of psp_load_ip_fw. Submits GFX_CMD_ID_LOAD_IP_FW
// for one firmware image already staged at MC address `fwBusAddr`.
// fwType is a PSPGfxFwType value.
//
kern_return_t psp_load_ip_fw(DeviceContext &dev, PSPContext &psp,
                             uint64_t fwBusAddr, uint32_t fwSize,
                             uint32_t fwType);

//
// psp_query_fw_reservation — port of psp_update_fw_reservation
// (amdgpu_psp.c:1040). Sends FB_FW_RESERV_ADDR + _EXT_ADDR right after
// ring_create and BEFORE any LOAD_IP_FW. PSP_ERR_UNKNOWN_COMMAND (0x100)
// from older SOS is swallowed.
//
kern_return_t psp_query_fw_reservation(DeviceContext &dev, PSPContext &psp);

//
// psp_load_toc — port of upstream psp_load_toc (amdgpu_psp.c:840) for a
// standalone gc_<v>_toc.bin FILE: slices the common_firmware_header and
// submits only the payload. *outTmrSize gets PSP's reported TMR size.
//
kern_return_t psp_load_toc(DeviceContext &dev, PSPContext &psp,
                           const uint8_t *tocBin, uint32_t tocSize,
                           uint32_t *outTmrSize,
                           uint32_t *outRespStatus = nullptr);

//
// psp_load_toc_subbin — same submit, but `subBin/subSize` are ALREADY the
// payload: the PSP_FW_TYPE_PSP_TOC sub-binary inside psp_<chip>_sos.bin,
// as extracted by psp_parse_sos_microcode. This is the path psp_setup_tmr
// takes on psp_v14_0_3.
//
kern_return_t psp_load_toc_subbin(DeviceContext &dev, PSPContext &psp,
                                  const uint8_t *subBin, uint32_t subSize,
                                  uint32_t *outTmrSize,
                                  uint32_t *outRespStatus = nullptr);

//
// psp_rlc_autoload_start — port of psp_rlc_autoload_start
// (amdgpu_psp.c:3434). cmd-id-only GFX_CMD_ID_AUTOLOAD_RLC frame; send it
// immediately after RLC_G loads.
//
kern_return_t psp_rlc_autoload_start(DeviceContext &dev, PSPContext &psp);

//
// psp_rl_load — port of psp_rl_load (amdgpu_psp.c:1152). Stages psp.rl in
// fw_pri and submits LOAD_IP_FW with fw_type = GFX_FW_TYPE_REG_LIST (67).
// Must run AFTER psp_rlc_autoload_start. No-op (success) if psp.rl is
// absent from the SOS package.
//
kern_return_t psp_rl_load(DeviceContext &dev, PSPContext &psp);

//
// psp_asd_initialize — port of psp_asd_initialize + psp_ta_load
// (amdgpu_psp.c:1225, 1380) for the ASD case. Submits GFX_CMD_ID_LOAD_ASD
// pointing at psp.asd.ucode_mc_addr. Because psp_parse_ta_microcode is not
// ported, psp.asd.parsed is currently always false and this logs + returns
// success as a no-op. MUST be called after psp_rlc_autoload_start and
// before psp_rl_load once a TA package is wired up.
//
kern_return_t psp_asd_initialize(DeviceContext &dev, PSPContext &psp);

//
// psp_load_non_psp_fw — port of amdgpu_psp.c:3051. Loads all non-PSP
// firmware through the ring in upstream order:
//   SMU → IMU_I/IMU_D → RLC sub-bins (tap delays → restore lists →
//   iram/dram → P/V → G) → psp_rlc_autoload_start() → CP RS64 (PFP/ME/MEC
//   + per-pipe stacks) → SDMA → MES.
// Payload bytes come from the caller through `loader`.
//
kern_return_t psp_load_non_psp_fw(DeviceContext &dev, PSPContext &psp,
                                  const FirmwareLoader &loader);

//
// psp_fw_buf_stage — NOT in the reference (there, the dext's LoadFirmware
// path staged payloads and the GART bind produced the address). Bump-
// allocates a page-aligned slot inside fw_buf, copies `bytes` into it
// through the BAR0 aperture, HDP-flushes, and returns the MC address to
// pass to psp_load_ip_fw. This is what a FirmwareLoader callback backed by
// embedded blobs should call.
//
kern_return_t psp_fw_buf_stage(DeviceContext &dev, PSPContext &psp,
                               const void *bytes, uint32_t size,
                               uint64_t *outBusAddr);

} // namespace amdgpu
