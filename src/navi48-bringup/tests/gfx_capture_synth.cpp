// gfx_capture_synth.cpp — writes a synthetic capture stream with the kext's exact record framing (amd/n48cap.cpp: 24-byte header,
// 4-byte padding) and the gfx_capture_scan.h body structs, so tools/m4-xlat/capdecode.py is checked against the C layouts, not against
// its own assumptions (0.0.282).
//     clang++ -std=c++17 -Wall -Wextra -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_capture_synth.cpp -o /tmp/gcapsynth \
//       && /tmp/gcapsynth /tmp/synth.bin && python3 tools/m4-xlat/capdecode.py --synth-check /tmp/synth.bin
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "gfx_capture_scan.h"

static std::vector<uint8_t> gOut;
static uint32_t gSeq = 0;

// The same framing n48_cap_append writes.
static void append(uint32_t type, const void *hdr, uint32_t hdrLen, const void *body, uint32_t bodyLen)
{
    const uint64_t raw = 24u + hdrLen + bodyLen, total = (raw + 3u) & ~3ull;
    const size_t at = gOut.size();
    gOut.resize(at + total, 0);
    const uint32_t h[4] = { 0x5243344eu, type, (uint32_t)total, ++gSeq };
    uint64_t now = 1000u + gSeq;
    std::memcpy(&gOut[at], h, 16);
    std::memcpy(&gOut[at + 16], &now, 8);
    if (hdrLen) std::memcpy(&gOut[at + 24], hdr, hdrLen);
    if (bodyLen) std::memcpy(&gOut[at + 24 + hdrLen], body, bodyLen);
}

static uint32_t fnv32(const uint32_t *d, uint32_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) for (unsigned b = 0; b < 4; b++) { h ^= (d[i] >> (8u * b)) & 0xffu; h *= 0x01000193u; }
    return h;
}
static uint64_t sckey(const uint32_t *d, uint32_t L)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned b = 0; b < 4; b++) { h ^= (L >> (8u * b)) & 0xffu; h *= 0x100000001b3ull; }
    for (uint32_t i = 0; i < L; i++) for (unsigned b = 0; b < 4; b++) { h ^= (d[i] >> (8u * b)) & 0xffu; h *= 0x100000001b3ull; }
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.bin\n", argv[0]); return 2; }
    // ARM with two context rows.
    n48_gcap_arm_hdr ah { 1u, 32u << 20, 2u, 0u };
    n48_gcap_arm_row rows[2] {};
    rows[0].seq = 6; rows[0].pid = 1034; std::strcpy(rows[0].name, "WindowServer"); rows[0].state = 1; rows[0].ctx = 0xffffff8b71c83280ull;
    rows[0].root = 0x3d6400000ull;
    rows[1].seq = 7; rows[1].pid = 1091; std::strcpy(rows[1].name, "SecurityAgent"); rows[1].state = 1; rows[1].ctx = 0xffffff8b71c83880ull;
    rows[1].root = 0x3d6c00000ull;
    append(N48_GCAP_REC_ARM, &ah, sizeof(ah), rows, sizeof(rows));
    // FRAME 7 with two probes and one IB entry of 3744 dwords at 0x400190000.
    n48_gcap_frame_hdr fh {};
    fh.frame = 7; fh.srcCheck = 0; fh.flags = 0; fh.vmidField = 2; fh.count = 1; fh.stamp = 0x1234;
    fh.ctx2Root = 0x3d6c00000ull; fh.startVa = 0x400000000ull; fh.readRoot = 0x3d6c00000ull;
    fh.ownerPid = 861; fh.ownerSeq = 7; std::strcpy(fh.ownerName, "SecurityAgent");
    fh.threadPid = 0; std::strcpy(fh.threadName, "kernel_task");
    fh.rptr = 0x80; fh.wptr = 0x100; fh.nprobe = 2; fh.infoBytes = 0x4c + 0x28; fh.chanId = 26; fh.readerWhy = 1;
    n48_gcap_probe pr[2] {};
    pr[0].pid = 1034; pr[0].seq = 6; pr[0].root = 0x3d6400000ull; pr[0].got = 2; pr[0].dw0 = 0; pr[0].dw1 = 0;
    pr[1].pid = 1091; pr[1].seq = 7; pr[1].root = 0x3d6c00000ull; pr[1].got = 2; pr[1].dw0 = 0xc0004600u; pr[1].dw1 = 0x16u;
    uint8_t info[0x4c + 0x28] {};
    const uint32_t len = 3744; const uint64_t va = 0x400190000ull;
    std::memcpy(info + 0x04, &fh.vmidField, 4); std::memcpy(info + 0x14, &fh.count, 4); std::memcpy(info + 0x18, &fh.stamp, 4);
    std::memcpy(info + 0x4c, &len, 4); std::memcpy(info + 0x4c + 0x0c, &va, 8);
    std::vector<uint8_t> body(sizeof(pr) + sizeof(info));
    std::memcpy(body.data(), pr, sizeof(pr)); std::memcpy(body.data() + sizeof(pr), info, sizeof(info));
    append(N48_GCAP_REC_FRAME, &fh, sizeof(fh), body.data(), (uint32_t)body.size());
    // IB with a 4-dword body.
    const uint32_t ibd[4] = { 0xc0004600u, 0x16u, 0xc0027600u, 0x8u };
    n48_gcap_ib_hdr ih { 7, 0, va, len, 4, 1, 4, fnv32(ibd, 4), 4, 0x3d6c00000ull };
    append(N48_GCAP_REC_IB, &ih, sizeof(ih), ibd, sizeof(ibd));
    // A program (3 dwords through s_endpgm), then a REF to it, then a CB0 region on a host page.
    const uint32_t pg[3] = { 0x7e000280u, 0xf8001890u, 0xbf810000u };
    n48_gcap_region_hdr rh {};
    rh.frame = 7; rh.kind = N48_GCAP_PGM + 0; rh.va = 0x400008000ull; rh.ib = 0; rh.dword = 2; rh.want = 1024; rh.got = 3; rh.isSys = 0;
    rh.firstPage = 0x3d7008000ull; rh.fnv = fnv32(pg, 3); rh.extent = 3; rh.key = sckey(pg, 3);
    append(N48_GCAP_REC_REGION, &rh, sizeof(rh), pg, sizeof(pg));
    rh.ref = 1;
    append(N48_GCAP_REC_REGION, &rh, sizeof(rh), nullptr, 0);
    const uint32_t cb[2] = { 0, 0 };
    n48_gcap_region_hdr cbh {};
    cbh.frame = 7; cbh.kind = N48_GCAP_CB | (0u << 8); cbh.va = 0x400340000ull; cbh.want = 16; cbh.got = 2; cbh.isSys = 1;
    cbh.firstPage = 0x12345000ull; cbh.fnv = fnv32(cb, 2);
    append(N48_GCAP_REC_REGION, &cbh, sizeof(cbh), cb, sizeof(cb));
    // build 0.0.450 item 4: one N48_GCAP_CENSUS row - a register fact (SGPR 4 held index 25 at IB dword 9,
    // s0:1 = the table VA 0x400300000), no body (fnv is gcap_fnv32's own seed for an empty body, 0x811c9dc5).
    n48_gcap_region_hdr ch {};
    ch.frame = 7; ch.kind = N48_GCAP_CENSUS; ch.parentVa = 0x400300000ull; ch.ib = 0; ch.dword = 9;
    ch.extent = (4u << 16) | 25u; ch.fnv = 0x811c9dc5u;
    append(N48_GCAP_REC_REGION, &ch, sizeof(ch), nullptr, 0);
    FILE *f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(gOut.data(), 1, gOut.size(), f);
    std::fclose(f);
    std::printf("wrote %zu bytes, %u records\n", gOut.size(), gSeq);
    return 0;
}
