//
//  cp_pm4_gfx12.h — PACKET3 (PM4 type-3) opcodes for GFX12, taken from the
//  Linux kernel's own opcode header rather than re-derived.
//
//  Why this file exists (and why it is not amdgpu_pm4.h):
//    amdgpu_pm4.h defines `kPM4OpNop = 0x00`. The PM4 IT_NOP opcode is
//    **0x10**, not 0x00 — see drivers/gpu/drm/amd/amdgpu/nvd.h (gfx10/11/12)
//    and soc15d.h (gfx9), both of which spell it
//        #define PACKET3_NOP  0x10
//    and gfx_v12_0.c:5358 / 5406 / 5444, which set every GFX12 ring's
//    `.nop = PACKET3(PACKET3_NOP, 0x3FFF)`. A header dword built with
//    opcode 0x00 is a reserved type-3 opcode: the CP raises a bad-opcode
//    fault instead of skipping the dword. Fixing amdgpu_pm4.h is outside
//    this change's file ownership, so the CP module carries its own correct
//    table and uses it for everything it emits. See the report.
//
//  Header encoding (identical to amdgpu_pm4.h's pm4_header(), which is
//  correct): PACKET3(op, n) = (3 << 30) | ((n & 0x3FFF) << 16) |
//                             ((op & 0xFF) << 8)
//  where `n` is (payload dwords - 1), so a packet is n + 2 dwords total.
//
//  Opcode values below are verbatim from nvd.h (Navi/RDNA PACKET3 list,
//  unchanged through gfx12).
//
#pragma once

#include <stdint.h>

namespace amdgpu {

// ---- PACKET3 opcodes (nvd.h) ----
constexpr uint32_t kP3_NOP              = 0x10;
constexpr uint32_t kP3_CLEAR_STATE      = 0x12;
constexpr uint32_t kP3_CONTEXT_CONTROL  = 0x28;
constexpr uint32_t kP3_WRITE_DATA       = 0x37;
constexpr uint32_t kP3_INDIRECT_BUFFER  = 0x3F;
// nvd.h:225 — INDIRECT_BUFFER's control dword. The COMPUTE ring requires it
// (gfx_v12_0_ring_emit_ib_compute); the GFX ring does not set it
// (gfx_v12_0_ring_emit_ib_gfx). Leaving it off a compute submission is why the
// MEC accepted the doorbell, marked the queue non-empty, and then never
// advanced its read pointer.
constexpr uint32_t kP3_INDIRECT_BUFFER_VALID = (1u << 23);
constexpr uint32_t kP3_RELEASE_MEM      = 0x49;
constexpr uint32_t kP3_PREAMBLE_CNTL    = 0x4A;
constexpr uint32_t kP3_SET_CONTEXT_REG  = 0x69;
constexpr uint32_t kP3_SET_SH_REG       = 0x76;
constexpr uint32_t kP3_SET_UCONFIG_REG  = 0x79;

// PREAMBLE_CNTL payload selectors (nvd.h).
constexpr uint32_t kP3_PREAMBLE_BEGIN_CLEAR_STATE = (2u << 28);
constexpr uint32_t kP3_PREAMBLE_END_CLEAR_STATE   = (3u << 28);

// SET_*_REG offset bases (nvd.h): the packet carries
// (absolute_dword_offset - START) in its first payload dword.
constexpr uint32_t kP3_SET_CONTEXT_REG_START = 0x0000A000;
constexpr uint32_t kP3_SET_UCONFIG_REG_START = 0x0000C000;
constexpr uint32_t kP3_SET_SH_REG_START      = 0x00002C00;

// Build a PACKET3 header. `count_minus_1` = payload dwords - 1.
static inline uint32_t cp_p3(uint32_t op, uint32_t count_minus_1)
{
    return (3u << 30)
         | ((count_minus_1 & 0x3FFFu) << 16)
         | ((op & 0xFFu) << 8);
}

// Single-dword filler NOP. COUNT == 0x3FFF is the PM4 special case that
// makes IT_NOP a one-dword packet; this is byte-for-byte the dword
// gfx_v12_0.c installs as `.nop` (0xFFFF1000).
static inline uint32_t cp_p3_nop1(void) { return cp_p3(kP3_NOP, 0x3FFF); }

} // namespace amdgpu
