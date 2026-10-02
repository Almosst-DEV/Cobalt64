//
//  regs.hpp — Navi 48 (gfx1201) register constants used by the bring-up kext.
//
//  Byte/dword offsets and semantics are cross-checked against the Linux amdgpu
//  driver headers (GPL-2.0, used as documentation only) and the register bases
//  are resolved at runtime from the on-die IP discovery table rather than
//  hardcoded, so these are segment-relative dword offsets plus the few
//  absolute BAR5 bytes AMD fixes across the family.
//
#ifndef Navi48_regs_hpp
#define Navi48_regs_hpp

#include <stdint.h>

namespace n48 {

// --- Absolute BAR5 dword indices (fixed across AMD dGPUs since Bonaire) -----
// BIF_BX_PF0 indirect VRAM window (amdgpu_device_mm_access):
//   MM_INDEX at byte 0x0, MM_DATA at byte 0x4, MM_INDEX_HI at byte 0x18.
static constexpr uint32_t kMMIndexDword    = 0x00 / 4;   // rmmio[0]
static constexpr uint32_t kMMDataDword     = 0x04 / 4;   // rmmio[1]
static constexpr uint32_t kMMIndexHiDword  = 0x18 / 4;   // rmmio[6]

// NBIF RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE — VRAM size in MiB.
// Discovery: HwNbif, instance 0, segment 2, dword 0x00c3. This card's table
// places it at BAR5 byte 0x378c; used as the fallback if discovery is absent.
static constexpr uint8_t  kNbifMemsizeSeg   = 2;
static constexpr uint32_t kNbifMemsizeDword = 0x00c3;
static constexpr uint32_t kNbifMemsizeByteFallback = 0x378c;

// --- MP0 / PSP scratch mailbox (HwMp0, instance 0, segment 0) ----------------
// C2PMSG registers (mp_14_0_2_offset.h). Read-only status we survey:
static constexpr uint32_t kPspC2PMsg35 = 0x0063; // bit31 = bootloader ready
static constexpr uint32_t kPspC2PMsg64 = 0x0080; // GPCOM ring cmd/response (bit31 = response flag, low16 = status)
static constexpr uint32_t kPspC2PMsg81 = 0x0091; // sOS alive (build stamp, !=0)
static constexpr uint32_t kPspRing69   = 0x0085; // ring base/size regs
static constexpr uint32_t kPspRing70   = 0x0086;
static constexpr uint32_t kPspRing71   = 0x0087;
static constexpr uint32_t kPspFwVer58  = 0x007a;
static constexpr uint32_t kPspFwVer59  = 0x007b;
static constexpr uint32_t kPspBootReadyBit = 0x80000000u;
static constexpr uint32_t kPspRespFlagBit  = 0x80000000u; // C2PMSG_64 response flag (0x80c00000 seen right after SOS boot)

// --- MP1 / SMU mailbox (HwMp1, instance 0, segment 1) ------------------------
// smu_v14_0_send_msg_with_param protocol. base_idx MUST be 1 (base 0 routes
// writes to a different physical register and the SMU never answers).
static constexpr uint8_t  kSmuSeg      = 1;
static constexpr uint32_t kSmuMsgDword   = 0x0082; // regMP1_SMN_C2PMSG_66
static constexpr uint32_t kSmuParamDword = 0x0092; // regMP1_SMN_C2PMSG_82
static constexpr uint32_t kSmuRespDword  = 0x009a; // regMP1_SMN_C2PMSG_90
// PPSMC message ids (smu_v14_0_2_ppsmc.h)
static constexpr uint32_t kSmuMsgTestMessage      = 0x1;
static constexpr uint32_t kSmuMsgGetSmuVersion    = 0x2;
static constexpr uint32_t kSmuMsgGetDriverIfVer   = 0x3;
// Response codes: 1 OK, 0xFF failed, 0xFE unknown, 0xFD prereq, 0xFC busy.
static constexpr uint32_t kSmuRespOK = 0x1;

// --- on-die discovery TMR (top of VRAM) --------------------------------------
static constexpr uint32_t kDiscTmrSize   = 10u << 10; // 10 KiB
static constexpr uint32_t kDiscTmrOffset = 64u << 10; // 64 KiB below VRAM end

} // namespace n48

#endif /* Navi48_regs_hpp */
