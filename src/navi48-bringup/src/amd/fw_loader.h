//
//  fw_loader.h — FirmwareLoader implementation over the embedded gfx1201 blobs:
//  decodes each .bin into LOAD_IP_FW payloads (amdgpu_ucode_extract), stages a
//  payload into the PSP fw_buf on first request (psp_fw_buf_stage) and hands
//  psp_load_non_psp_fw the MC address. "Not present" -> kIOReturnUnsupported,
//  which the loader treats as "skip" (reference contract).
//
#pragma once
#include "amdgpu_regs.h"
#include "amdgpu_psp.h"

namespace amdgpu {

struct FwLoaderState {
    DeviceContext *dev { nullptr };
    PSPContext    *psp { nullptr };
    struct Entry {
        uint32_t       fw_type;   // psp_gfx_fw_type
        const uint8_t *src;       // bytes inside the embedded blob
        uint32_t       size;
        uint64_t       mc;        // MC address once staged
        bool           staged;
        const char    *blob;      // blob name for logging
    };
    static constexpr uint32_t kMaxEntries = 64;
    Entry    entries[kMaxEntries] {};
    uint32_t count { 0 };
    bool imuPresent { false }, rlcPresent { false }, cpPresent { false },
         sdmaPresent { false }, mesPresent { false }, smuPresent { false };
};

// Decode every embedded blob into payload entries (no hardware access).
kern_return_t  fw_loader_prepare(DeviceContext &dev, PSPContext &psp, FwLoaderState &st);
FirmwareLoader fw_loader_make(FwLoaderState &st);
const FwLoaderState::Entry *fw_loader_find(const FwLoaderState &st, uint32_t fw_type);

} // namespace amdgpu
