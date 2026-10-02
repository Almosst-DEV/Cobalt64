#include "fw_loader.h"
#include "amdgpu_ucode_extract.h"
#include "amdgpu_log.h"
#include "../fw/fw_table.h"

namespace amdgpu {

namespace {
struct BlobPlan { FwId id; uint64_t hostType; bool *presentFlag(FwLoaderState &s) const; };
}

kern_return_t fw_loader_prepare(DeviceContext &dev, PSPContext &psp, FwLoaderState &st) {
    st.dev = &dev; st.psp = &psp; st.count = 0;
    psp.fwBufBumpOffset = 0;    // a re-run re-stages everything from the start of fw_buf
    struct Plan { FwId id; uint64_t host; const char *what; };
    const Plan plans[] = {
        { FwId::SMU,        kHostFwIPBase + PSPGfxFwType::SMU, "smu"     },
        { FwId::GC_IMU,     kHostFwFile_IMU,                   "imu"     },
        { FwId::GC_RLC,     kHostFwFile_RLC,                   "rlc"     },
        { FwId::GC_PFP,     kHostFwFile_CP_PFP,                "pfp"     },
        { FwId::GC_ME,      kHostFwFile_CP_ME,                 "me"      },
        { FwId::GC_MEC,     kHostFwFile_CP_MEC,                "mec"     },
        { FwId::GC_UNI_MES, kHostFwFile_MES_UNI,               "uni_mes" },
        { FwId::SDMA,       kHostFwFile_SDMA,                  "sdma"    },
    };
    for (const Plan &pl : plans) {
        const FwBlob *b = fw_get(pl.id);
        if (!b || !b->data || b->size == 0) { UCODE_LOG("loader: blob %s missing — skipped", pl.what); continue; }
        UcodePayload out[kMaxUcodePayloadsPerFile];
        uint32_t n = amdgpu_ucode_extract(pl.host, b->data, b->size, out);
        if (n == 0) { UCODE_LOG("loader: %s (%s, %llu B): header not understood — skipped", pl.what, b->name, (unsigned long long)b->size); continue; }
        for (uint32_t i = 0; i < n && st.count < FwLoaderState::kMaxEntries; i++) {
            if (out[i].size_bytes == 0) continue;
            if ((uint64_t)out[i].offset_bytes + out[i].size_bytes > b->size) {
                UCODE_LOG("loader: %s payload type %u out of bounds — skipped", pl.what, out[i].fw_type); continue;
            }
            FwLoaderState::Entry &e = st.entries[st.count++];
            e.fw_type = out[i].fw_type; e.src = b->data + out[i].offset_bytes; e.size = out[i].size_bytes;
            e.mc = 0; e.staged = false; e.blob = b->name;
            UCODE_LOG("loader: %-8s type %3u  %7u bytes @+%u", pl.what, e.fw_type, e.size, out[i].offset_bytes);
        }
        switch (pl.id) {
        case FwId::SMU:        st.smuPresent  = true; break;
        case FwId::GC_IMU:     st.imuPresent  = true; break;
        case FwId::GC_RLC:     st.rlcPresent  = true; break;
        case FwId::GC_PFP: case FwId::GC_ME: case FwId::GC_MEC: st.cpPresent = true; break;
        case FwId::GC_UNI_MES: st.mesPresent  = true; break;
        case FwId::SDMA:       st.sdmaPresent = true; break;
        default: break;
        }
    }
    UCODE_LOG("loader: %u payloads ready (smu %d imu %d rlc %d cp %d mes %d sdma %d)", st.count,
              st.smuPresent, st.imuPresent, st.rlcPresent, st.cpPresent, st.mesPresent, st.sdmaPresent);
    return st.count ? kIOReturnSuccess : kIOReturnNotFound;
}

const FwLoaderState::Entry *fw_loader_find(const FwLoaderState &st, uint32_t fw_type) {
    for (uint32_t i = 0; i < st.count; i++) if (st.entries[i].fw_type == fw_type) return &st.entries[i];
    return nullptr;
}

static kern_return_t get_payload(void *ctx, uint32_t fw_type, uint64_t &out_bus_addr, uint32_t &out_size) {
    FwLoaderState &st = *static_cast<FwLoaderState *>(ctx);
    FwLoaderState::Entry *e = const_cast<FwLoaderState::Entry *>(fw_loader_find(st, fw_type));
    if (!e) return kIOReturnUnsupported;                       // "not present" -> skipped by the caller
    if (!e->staged) {
        kern_return_t r = psp_fw_buf_stage(*st.dev, *st.psp, e->src, e->size, &e->mc);
        if (r != kIOReturnSuccess) { UCODE_LOG("loader: staging type %u (%u B) failed kr=%#x", fw_type, e->size, r); return r; }
        e->staged = true;
    }
    out_bus_addr = e->mc; out_size = e->size;
    return kIOReturnSuccess;
}

FirmwareLoader fw_loader_make(FwLoaderState &st) {
    FirmwareLoader l; l.fn = &get_payload; l.ctx = &st; return l;
}

} // namespace amdgpu
