// native_fb_monb_battery.h - build 0.0.658: the MONB'S PUBLISH PATH, byte for byte. The pure half of `fbpublish 2` (amd/native_fb_pure.h) and the shared snapshot / ops checks (Navi48DisplayOps.h)
// were generalised for the monitor A (instance 1 -> display index 2). This battery runs the monitor B (instance 2 -> index 1) through a fixed set of scenarios and hashes every verdict, status number, built snapshot byte and
// validator answer. The golden hash was produced by compiling THIS FILE against the 0.0.657 sources (git f38ba015: `git show f38ba015:src/navi48-bringup/src/amd/native_fb_pure.h` and Navi48DisplayOps.h) and running it:
// the same hash under the 0.0.658 sources proves the monitor B's publish decisions are unchanged. It uses ONLY members the 0.0.657 sources already had (Pre / Data with their 0.0.657 fields, snap_build, aperture_ok, n48disp_ops_check,
// n48disp_snap_valid, the monitor B constants); the argument values it feeds are the monitor B's (2) and values neither version accepts, never 1 (the monitor A's, new in 0.0.658).
#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "amd/native_fb_pure.h"

namespace fbbat {
struct H { uint64_t h = 0xcbf29ce484222325ull; void u(uint64_t v) { for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 0x100000001b3ull; } } void b(const void *p, size_t n) { const uint8_t *q = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= q[i]; h *= 0x100000001b3ull; } } };

static inline uint64_t monb_publish_hash() {
    H h;
    // the argument gate (the monitor B's 2 and values both versions refuse)
    const uint64_t args[] = { 0ull, 2ull, 3ull, 4ull, 0x102ull, 0x10002ull, 0x100000002ull, ~0ull };
    for (uint64_t a : args) h.u(n48fb::arg_ok(a) ? 1u : 0u);
    for (uint32_t v = 0; v < 4; v++) for (int p = 0; p < 2; p++) h.u(n48fb::fb2_on(p != 0, v) ? 1u : 0u);
    // phase 1: every combination of the nine software facts
    for (uint32_t m = 0; m < 512u; m++) {
        n48fb::Pre p;
        p.argOk = m & 1; p.latched = (m >> 1) & 1; p.nubExists = (m >> 2) & 1; p.held = (m >> 3) & 1; p.pinned = (m >> 4) & 1; p.devOk = (m >> 5) & 1; p.pipeAdopted = (m >> 6) & 1; p.wsOpen = (m >> 7) & 1; p.metalNub = (m >> 8) & 1;
        h.u(n48fb::pre_verdict(p));
    }
    // phase 2: the live verdict, every EDID/registry combination, the aperture grid
    const uint32_t holds[] = { 0u, 1u, 4u, 0x80u, 0xFFFFFFFFu };
    for (uint32_t x : holds) h.u(n48fb::live_verdict(x));
    const uint32_t sts[] = { 0u, 1u, 7u, 9u };
    for (uint32_t es : sts) for (uint32_t m = 0; m < 64u; m++) {
        n48fb::Data d;
        d.edidStatus = es; d.block0Sum = m & 1; d.block0Hdr = (m >> 1) & 1; d.block1Sum = (m >> 2) & 1; d.propPresent = (m >> 3) & 1; d.block0EqProp = (m >> 4) & 1; d.apertureOk = (m >> 5) & 1;
        h.u(n48fb::data_verdict(d));
        n48fb::Post po; po.holdBad = es; po.d = d; h.u(n48fb::post_verdict(po));
    }
    const uint64_t bases[] = { 0ull, 0x80000000ull, 0x80001000ull, 0xFFFFFFFFFFFF0000ull };
    const uint64_t sizes[] = { 0ull, N48_DISP_BYTES - 1u, N48_DISP_BYTES, 0x10000000ull };
    const uint64_t offs[] = { 0ull, 0x1000ull, 0x10000ull, 0x01800000ull, 0x0FFFFFFFull, 0x10000000ull - N48_DISP_BYTES, 0x10000000ull - N48_DISP_BYTES + 0x10000ull };
    for (uint64_t b : bases) for (uint64_t sz : sizes) for (uint64_t o : offs) h.u(n48fb::aperture_ok(b, sz, o) ? 1u : 0u);
    // the snapshot: built from valid and from damaged facts, then judged by the shared validator
    uint8_t e[256];
    std::memset(e, 0, sizeof e); e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF;
    { uint32_t sm = 0; for (int i = 0; i < 127; i++) sm += e[i]; e[127] = (uint8_t)(0u - sm); }
    e[128] = 2;
    { uint32_t sm = 0; for (int i = 128; i < 255; i++) sm += e[i]; e[255] = (uint8_t)(0u - sm); }
    const uint64_t offA[] = { 0x01800000ull, 0x01801000ull, 0ull };
    const uint64_t mcs[][2] = { { 0x8001800000ull, 0x8002600000ull }, { 0ull, 0x8002600000ull }, { 0x8001800000ull, 0x8001800000ull }, { 0x8001801000ull, 0x8002600000ull } };
    for (uint64_t off : offA) for (const auto &mc : mcs) for (int bad = 0; bad < 3; bad++) {
        uint8_t ee[256]; std::memcpy(ee, e, sizeof ee); if (bad == 1) ee[20] ^= 1; if (bad == 2) ee[200] ^= 1;
        N48DispSnap s; n48fb::snap_build(&s, 0x80000000ull, off, mc[0], mc[1], ee);
        h.b(&s, sizeof s); h.u(n48disp_snap_valid(&s));
    }
    { N48DispSnap g; n48fb::snap_build(&g, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e);
      // one field at a time (the monitor B's geometry; index 2 / 1920x1080 are NOT fed here: 0.0.658 accepts index 2, 0.0.657 did not)
      for (int f = 0; f < 14; f++) {
          N48DispSnap x = g;
          switch (f) {
          case 0: x.otg = 1; break; case 1: x.ddcLine = 2; break; case 2: x.w = 1919; break; case 3: x.h = 1081; break; case 4: x.pitchBytes = 7680; break; case 5: x.fmt = 0; break; case 6: x.refresh1616 = 60u << 16; break;
          case 7: x.pixHz = 148500000ull; break; case 8: x.aperLen -= 0x10000; break; case 9: x.aperPhys = 0; break; case 10: x.mcB = 0; break; case 11: x.edidLen = 128; break; case 12: x.edid[0] = 1; break; default: x.index = 0; break;
          }
          h.u(n48disp_snap_valid(&x));
      }
      h.u(n48disp_snap_valid(nullptr)); }
    // the ops-table check
    for (uint32_t abi = 0; abi < 3; abi++) for (uint32_t sz : { 48u, 71u, 72u, 96u }) for (int miss = 0; miss < 3; miss++) {
        N48DispOps o; std::memset(&o, 0, sizeof o); o.magic = N48_DISP_OPS_MAGIC; o.abi = abi; o.size = sz;
        o.snapshot = (miss == 1) ? nullptr : [](void *, N48DispSnap *) { return 0; }; o.power = (miss == 2) ? nullptr : [](void *, uint32_t) { return 0; };
        h.u(n48disp_ops_check(&o));
    }
    h.u(n48disp_ops_check(nullptr));
    // the monitor B's constants
    h.u(N48_DISP_INDEX); h.u(N48_DISP_OTG); h.u(N48_DISP_DDC_LINE); h.u(N48_DISP_W); h.u(N48_DISP_H); h.u(N48_DISP_PITCH); h.u(N48_DISP_FMT_ARGB8888); h.u(N48_DISP_REFRESH1616); h.u(N48_DISP_PIXHZ); h.u(N48_DISP_BYTES);
    h.u(sizeof(N48DispSnap)); h.u(sizeof(N48DispOps)); h.u(N48_FBP_COUNT);
    return h.h;
}
}  // namespace fbbat
