//
//  hosttest/main.cpp — host-side (macOS userspace) test harness for
//  amdgpu::amdgpu_ucode_extract() (src/amd/amdgpu_ucode_extract.cpp).
//
//  Loads each firmware/*.bin, calls amdgpu_ucode_extract() with the host
//  fw-type id the real kext would use for that file (per
//  src/amd/amdgpu_ucode_extract.h), and prints a table of every LOAD_IP_FW
//  payload it produces: fw_type (number + name), offset, size, the first
//  8 bytes at that offset, and whether [offset, offset+size) actually fits
//  inside the file. Also decodes+prints the file's common_firmware_header
//  (including ucode_version), flags any 0-size / out-of-bounds / misaligned
//  payload, and diffs the emitted fw_type sequence against a hand-written
//  expectation table taken from the reference driver's firmware mapping.
//
//  This file (and everything under hosttest/) is new — nothing under src/
//  is modified. See hosttest/shim/IOKit/{IOLib,IOReturn}.h for the minimal
//  kernel-KPI stand-ins that let amdgpu_ucode_extract.cpp's real #include
//  chain compile with a plain host clang++.
//

#include "amdgpu_ucode_extract.h"
#include "amdgpu_ucode_psp.h"
#include "amdgpu_psp.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace amdgpu;

namespace {

// ---------------------------------------------------------------------
// fw_type -> name, built from the real PSPGfxFwType constants (not
// hand-copied numbers) so it can't silently drift from amdgpu_psp.h.
// ---------------------------------------------------------------------
struct FwName { uint32_t value; const char *name; };
const FwName kFwNames[] = {
    {PSPGfxFwType::CP_ME, "CP_ME"},
    {PSPGfxFwType::CP_PFP, "CP_PFP"},
    {PSPGfxFwType::RLC_V, "RLC_V"},
    {PSPGfxFwType::RLC_G, "RLC_G"},
    {PSPGfxFwType::CP_MEC, "CP_MEC"},
    {PSPGfxFwType::SDMA0, "SDMA0"},
    {PSPGfxFwType::SDMA1, "SDMA1"},
    {PSPGfxFwType::SMU, "SMU"},
    {PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,  "RLC_RESTORE_LIST_GPM_MEM"},
    {PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,  "RLC_RESTORE_LIST_SRM_MEM"},
    {PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL, "RLC_RESTORE_LIST_SRM_CNTL"},
    {PSPGfxFwType::RLC_P, "RLC_P"},
    {PSPGfxFwType::RLC_IRAM, "RLC_IRAM"},
    {PSPGfxFwType::GLOBAL_TAP_DELAYS, "GLOBAL_TAP_DELAYS"},
    {PSPGfxFwType::SE0_TAP_DELAYS, "SE0_TAP_DELAYS"},
    {PSPGfxFwType::SE1_TAP_DELAYS, "SE1_TAP_DELAYS"},
    {PSPGfxFwType::REG_LIST, "REG_LIST"},
    {PSPGfxFwType::IMU_I, "IMU_I"},
    {PSPGfxFwType::IMU_D, "IMU_D"},
    {PSPGfxFwType::SE2_TAP_DELAYS, "SE2_TAP_DELAYS"},
    {PSPGfxFwType::SE3_TAP_DELAYS, "SE3_TAP_DELAYS"},
    {PSPGfxFwType::SDMA_UCODE_TH0, "SDMA_UCODE_TH0"},
    {PSPGfxFwType::RS64_MES, "RS64_MES"},
    {PSPGfxFwType::RS64_MES_STACK, "RS64_MES_STACK"},
    {PSPGfxFwType::RS64_KIQ, "RS64_KIQ"},
    {PSPGfxFwType::RS64_KIQ_STACK, "RS64_KIQ_STACK"},
    {PSPGfxFwType::RS64_PFP, "RS64_PFP"},
    {PSPGfxFwType::RS64_ME, "RS64_ME"},
    {PSPGfxFwType::RS64_MEC, "RS64_MEC"},
    {PSPGfxFwType::RS64_PFP_P0, "RS64_PFP_P0"},
    {PSPGfxFwType::RS64_PFP_P1, "RS64_PFP_P1"},
    {PSPGfxFwType::RS64_ME_P0, "RS64_ME_P0"},
    {PSPGfxFwType::RS64_ME_P1, "RS64_ME_P1"},
    {PSPGfxFwType::RS64_MEC_P0, "RS64_MEC_P0"},
    {PSPGfxFwType::RS64_MEC_P1, "RS64_MEC_P1"},
    {PSPGfxFwType::RS64_MEC_P2, "RS64_MEC_P2"},
    {PSPGfxFwType::RS64_MEC_P3, "RS64_MEC_P3"},
    {PSPGfxFwType::CP_MES, "CP_MES"},
    {PSPGfxFwType::CP_MES_DATA, "CP_MES_DATA"},   // == MES_STACK, alias of the same value (34)
    {PSPGfxFwType::CP_MES_KIQ, "CP_MES_KIQ"},
    {PSPGfxFwType::MES_KIQ_STACK, "MES_KIQ_STACK"},
    {PSPGfxFwType::RLC_DRAM_BOOT, "RLC_DRAM_BOOT"},
};

std::string fwName(uint32_t v) {
    for (const auto &e : kFwNames) if (e.value == v) return e.name;
    return "UNKNOWN";
}

// ---------------------------------------------------------------------
// file IO helpers
// ---------------------------------------------------------------------
bool readFile(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

// First up-to-8 bytes at `offset`, as hex, or a note if it doesn't fit.
std::string hex8At(const std::vector<uint8_t> &data, uint64_t offset) {
    if (offset >= data.size()) return "<out of range>";
    char buf[96]; size_t p = 0;
    uint64_t n = data.size() - offset; if (n > 8) n = 8;
    for (uint64_t i = 0; i < n; i++) {
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%02x ", data[offset + i]);
    }
    if (n < 8) {
        snprintf(buf + p, sizeof(buf) - p, "(only %llu byte%s available)",
                 (unsigned long long)n, n == 1 ? "" : "s");
    }
    return std::string(buf);
}

// ---------------------------------------------------------------------
// per-blob expectation: host fw id to call with, and the fw_type sequence
// amdgpu_ucode_extract() should emit, in order (taken from the task's
// reference-driver firmware mapping, cross-checked against what
// amdgpu_ucode_extract.cpp's extract_*() functions actually do for
// gfx_v12_0 / gc_12_0_1 / psp_v14_0 — e.g. RDNA4 registers only P0 for
// PFP/ME and P0+P1 for MEC, and extract_mes() unconditionally emits both
// the SCHED and KIQ pipe payloads from the same uni_mes.bin bytes).
// ---------------------------------------------------------------------
struct BlobSpec {
    const char *filename;
    uint64_t hostFwType;
    const char *label;
    std::vector<uint32_t> expected;
};

int g_failures = 0;

void checkBlob(const std::string &fwDir, const BlobSpec &spec) {
    printf("\n================ %s (%s) ================\n", spec.label, spec.filename);
    std::string path = fwDir + "/" + spec.filename;
    std::vector<uint8_t> data;
    if (!readFile(path, data)) {
        printf("%s: FAIL (could not read %s)\n", spec.label, path.c_str());
        g_failures++;
        return;
    }
    printf("file size: %zu bytes\n", data.size());

    if (data.size() >= sizeof(common_firmware_header)) {
        const auto *ch = reinterpret_cast<const common_firmware_header *>(data.data());
        printf("common_firmware_header: size_bytes=%u header_size_bytes=%u "
               "version=%u.%u ip_version=%u.%u ucode_version=0x%08x (%u) "
               "ucode_size_bytes=%u ucode_array_offset_bytes=%u crc32=0x%08x\n",
               ch->size_bytes, ch->header_size_bytes,
               ch->header_version_major, ch->header_version_minor,
               ch->ip_version_major, ch->ip_version_minor,
               ch->ucode_version, ch->ucode_version,
               ch->ucode_size_bytes, ch->ucode_array_offset_bytes, ch->crc32);
    } else {
        printf("WARNING: file smaller than common_firmware_header (%zu < %zu)\n",
               data.size(), sizeof(common_firmware_header));
    }

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t n = amdgpu_ucode_extract(spec.hostFwType, data.data(), data.size(), out);
    printf("amdgpu_ucode_extract(hostFwType=0x%llx) -> %u payload(s)\n",
           (unsigned long long)spec.hostFwType, n);

    std::vector<uint32_t> gotTypes;
    bool blobOk = true;

    for (uint32_t i = 0; i < n; i++) {
        const UcodePayload &p = out[i];
        gotTypes.push_back(p.fw_type);

        bool sizeZero    = (p.size_bytes == 0);
        bool exceeds      = (uint64_t)p.offset_bytes + (uint64_t)p.size_bytes > data.size();
        bool misaligned   = (p.offset_bytes % 4) != 0;

        printf("  [%u] fw_type=%-3u (%-26s) offset=0x%06x (%8u)  size=0x%06x (%8u)  "
               "fits_in_file=%-3s  4B_aligned=%-3s  first8=%s\n",
               i, p.fw_type, fwName(p.fw_type).c_str(),
               p.offset_bytes, p.offset_bytes, p.size_bytes, p.size_bytes,
               exceeds ? "NO" : "yes", misaligned ? "NO" : "yes",
               hex8At(data, p.offset_bytes).c_str());

        if (sizeZero)    { printf("    FLAG: size_bytes == 0\n"); blobOk = false; }
        if (exceeds)     { printf("    FLAG: offset+size exceeds file size\n"); blobOk = false; }
        if (misaligned)  { printf("    FLAG: offset_bytes is not 4-byte aligned\n"); blobOk = false; }
    }

    bool seqMatch = (gotTypes == spec.expected);
    if (!seqMatch) {
        blobOk = false;
        printf("  MISMATCH vs expected sequence:\n    expected:");
        for (auto t : spec.expected) printf(" %u(%s)", t, fwName(t).c_str());
        printf("\n    got:     ");
        for (auto t : gotTypes) printf(" %u(%s)", t, fwName(t).c_str());
        printf("\n");
    }

    printf("%s: %s\n", spec.label, blobOk ? "PASS" : "FAIL");
    if (!blobOk) g_failures++;
}

} // namespace

int main(int argc, char **argv) {
    std::string fwDir = (argc > 1) ? argv[1] : "firmware";

    // RDNA4 / gfx_v12_0 registers only P0 for PFP+ME (num_pipe_per_me=1) and
    // P0+P1 for MEC (num_pipe_per_mec=2) — see extract_cp_rs64() in
    // amdgpu_ucode_extract.cpp. extract_mes() always emits both the SCHED
    // (CP_MES/CP_MES_DATA) and KIQ (CP_MES_KIQ/MES_KIQ_STACK) pipe payloads
    // unconditionally from the same uni_mes.bin bytes.
    std::vector<BlobSpec> blobs = {
        {"sdma_7_0_1.bin",        kHostFwFile_SDMA,   "sdma",
         {PSPGfxFwType::SDMA_UCODE_TH0}},
        {"gc_12_0_1_rlc.bin",     kHostFwFile_RLC,     "rlc",
         {PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,
          PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,
          PSPGfxFwType::RLC_IRAM,
          PSPGfxFwType::RLC_DRAM_BOOT,
          PSPGfxFwType::RLC_G}},
        {"gc_12_0_1_imu.bin",     kHostFwFile_IMU,     "imu",
         {PSPGfxFwType::IMU_I, PSPGfxFwType::IMU_D}},
        {"gc_12_0_1_uni_mes.bin", kHostFwFile_MES_UNI, "uni_mes",
         {PSPGfxFwType::CP_MES, PSPGfxFwType::CP_MES_DATA,
          PSPGfxFwType::CP_MES_KIQ, PSPGfxFwType::MES_KIQ_STACK}},
        {"gc_12_0_1_pfp.bin",     kHostFwFile_CP_PFP,  "pfp",
         {PSPGfxFwType::RS64_PFP, PSPGfxFwType::RS64_PFP_P0}},
        {"gc_12_0_1_me.bin",      kHostFwFile_CP_ME,   "me",
         {PSPGfxFwType::RS64_ME, PSPGfxFwType::RS64_ME_P0}},
        {"gc_12_0_1_mec.bin",     kHostFwFile_CP_MEC,  "mec",
         {PSPGfxFwType::RS64_MEC, PSPGfxFwType::RS64_MEC_P0, PSPGfxFwType::RS64_MEC_P1}},
        {"smu_14_0_3.bin",        kHostFwIPBase + PSPGfxFwType::SMU, "smu",
         {PSPGfxFwType::SMU}},
    };

    printf("hosttest/main.cpp -- amdgpu_ucode_extract() host-side test\n");
    printf("firmware dir: %s\n", fwDir.c_str());
    printf("sizeof(UcodePayload)=%zu kMaxUcodePayloadsPerFile=%u\n",
           sizeof(UcodePayload), (unsigned)kMaxUcodePayloadsPerFile);

    for (const auto &b : blobs) checkBlob(fwDir, b);

    printf("\n================ SUMMARY ================\n");
    printf("%d blob(s) FAILED out of %zu\n", g_failures, blobs.size());
    return g_failures == 0 ? 0 : 1;
}
