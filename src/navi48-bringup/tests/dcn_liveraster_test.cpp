// dcn_liveraster_test.cpp — build 0.0.515: the AGDC LINKCFG reply's live raster does not depend on
// n48dcn::bind (DCN verbs 74-77). Compile (from the tree root; the dcn41 sources are compiled as C++, as the kext does):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         -I src/navi48-bringup/src/dcn -I src/dcn41 -x c++ src/navi48-bringup/tests/dcn_liveraster_test.cpp \
//         src/dcn41/dcn41_core.c src/dcn41/dcn41_otg.c -o /tmp/lrtest && /tmp/lrtest \
//         src/navi48-bringup/src/dcn/navi48_dcn.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/apple/DisplayPipeGuard.cpp
// Covers:
//   A. a native-1440p boot with NO DCN verb run (nothing armed; the read-only device built as start() builds it): the FIRST
//      getLinkConfig answer is the live 2560x1440, totals 2720x1481, 241.5 MHz, porches 48/32 and 3/5 - read-only (the
//      device's write callback is the no-op; the register file is unchanged);
//   B. 1920x1080: NO register read at all and the reply byte-identical to 0.0.513's CEA 1080p;
//   C. the fail directions: an unbuilt / failed device, no lit OTG, totals with no EDID row -> 0 (the CEA fallback);
//   D. source pins that hold the kext glue to it: liveRaster is the one-line n48lr_live call over gLr and names neither
//      gDcn.armed nor lit_otg(); start() calls n48dcn::attach AFTER stagePSP and BEFORE the Phase 4 block; agdc_link_config
//      goes through n48_agdc_link_timing with the liveRaster adapter.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include "display_pipe_guard.h"
#include "navi48_liveraster.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-78s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
}

// A register file indexed by absolute BAR5 dword, counting every read.
struct Regs { uint32_t mem[0x10000]; uint32_t reads; };
static uint32_t r_read(void *c, uint32_t a)
{
    Regs *m = static_cast<Regs *>(c);
    m->reads++;
    return a < 0x10000u ? m->mem[a] : 0xdeadbeefu;
}
static const uint32_t SEG[DCN41_NUM_SEGS] = { 0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00 };   // the measured DMU bases
static const uint32_t MMIO_DWORDS = 0x100000u;

// OTG `i` lit with the SINK-A's 2560x1440@60 DTD as optc1_program_timing leaves it (dcn41_modes.h row 0).
static void light(Regs *m, uint32_t i)
{
    m->mem[0x34c0 + DCN41_OTG_OTG_CONTROL(i)] = 1u;
    m->mem[0x34c0 + DCN41_OTG_OTG_H_BLANK_START_END(i)] = (112u << 16) | 2672u;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_BLANK_START_END(i)] = (38u << 16) | 1478u;
    m->mem[0x34c0 + DCN41_OTG_OTG_H_TOTAL(i)] = 2719u;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_TOTAL(i)] = 1480u;
}

// The kext's adapter, reproduced: getLinkConfig's reader is n48dcn::liveRaster == n48lr_live over the start()-built device.
static uint32_t live_adapter(void *ctx, n48_agdc_raster *lr)
{
    return n48lr_live(static_cast<n48lr_ro *>(ctx), &lr->h_active, &lr->v_active, &lr->h_total, &lr->v_total, &lr->h_front,
                      &lr->h_sync, &lr->v_front, &lr->v_sync, &lr->pixel_clock_hz);
}

static std::string slurp(const char *path)
{
    std::string s;
    FILE *f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &hay, const char *needle)
{
    uint32_t c = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) c++;
    return c;
}
// The text of the function whose definition starts at `head` (up to the next line that is exactly "}").
static std::string body_of(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

int main(int argc, char **argv)
{
    // ---- A: native 1440p, nothing armed, first call -------------------------------------------------------------------
    {
        Regs *m = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(m, 0u);
        Regs before = *m;
        static n48lr_ro ro;   // what n48dcn::attach builds at start(): no DCN verb has run, bind() never ran
        std::memset(&ro, 0, sizeof ro);
        expect_u("A0 build performs NO register read", (n48lr_ro_build(&ro, m, &r_read, SEG, MMIO_DWORDS), m->reads), 0u);
        expect_u("A0 the read-only device is READY", ro.state, N48LR_READY);
        expect_u("A0 its write callback is the no-op (nothing can be written)", ro.d.wreg == &n48lr_wreg_refuse, 1u);
        n48_agdc_timing lt;
        std::memset(&lt, 0xa5, sizeof lt);
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &ro, &lt);
        expect_u("A1 FIRST call: live raster", lt.live, 1u);
        expect_u("A1 width 2560", lt.w, 2560u);
        expect_u("A1 height 1440", lt.h, 1440u);
        expect_u("A1 h total 2720 (blank 160)", (uint64_t)lt.w + lt.h_blank, 2720u);
        expect_u("A1 v total 1481 (blank 41)", (uint64_t)lt.h + lt.v_blank, 1481u);
        expect_u("A1 pixel clock 241.5 MHz", lt.pixel_clock, 241500000ull);
        expect_u("A1 h front / sync 48 / 32", ((uint64_t)lt.h_sync_off << 16) | lt.h_sync_w, (48ull << 16) | 32u);
        expect_u("A1 v front / sync 3 / 5", ((uint64_t)lt.v_sync_off << 16) | lt.v_sync_w, (3ull << 16) | 5u);
        expect_u("A1 registers were READ", m->reads != 0u, 1u);
        expect_u("A1 the register file is unchanged (read-only)", std::memcmp(m->mem, before.mem, sizeof m->mem) == 0, 1u);
        uint8_t rep[N48_AGDC_LINK_CONFIG_LEN];
        expect_u("A2 the reply fills", (uint64_t)n48_agdc_fill_link_config_t(rep, sizeof rep, &lt), 0u);
        uint64_t pc; std::memcpy(&pc, rep + 0x2c, 8);
        expect_u("A2 reply +0x2c PixelClock 241500000", pc, 241500000ull);
        uint32_t hb; std::memcpy(&hb, rep + 0x48, 4);
        expect_u("A2 reply +0x48 HorizontalBlanking 160 (not the CEA 280)", hb, 160u);
        // the same answer on a second call (nothing latched, nothing armed in between)
        n48_agdc_timing lt2;
        std::memset(&lt2, 0xa5, sizeof lt2);   // the same padding bytes as lt, so the comparison is of the fields
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &ro, &lt2);
        expect_u("A3 second call identical", std::memcmp(&lt, &lt2, sizeof lt) == 0, 1u);
        // the OTG that is lit is read, not assumed
        Regs *m2 = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(m2, 2u);
        static n48lr_ro ro2; std::memset(&ro2, 0, sizeof ro2);
        (void)n48lr_ro_build(&ro2, m2, &r_read, SEG, MMIO_DWORDS);
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &ro2, &lt2);
        expect_u("A4 lit OTG 2 found (OTG 0-1 dark)", lt2.live && lt2.pixel_clock == 241500000ull, 1u);
        std::free(m2);
        std::free(m);
    }
    // ---- B: 1080p reads nothing and answers 0.0.513's CEA timing -------------------------------------------------------
    {
        Regs *m = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(m, 0u);
        static n48lr_ro ro; std::memset(&ro, 0, sizeof ro);
        (void)n48lr_ro_build(&ro, m, &r_read, SEG, MMIO_DWORDS);
        m->reads = 0u;
        n48_agdc_timing lt, cea;
        std::memset(&lt, 0, sizeof lt); std::memset(&cea, 0, sizeof cea);
        n48_agdc_link_timing(1920u, 1080u, &live_adapter, &ro, &lt);
        n48_agdc_timing_for(1920u, 1080u, nullptr, &cea);
        expect_u("B1 1080p: NO register read", m->reads, 0u);
        expect_u("B1 1080p: the CEA timing, field for field", std::memcmp(&lt, &cea, sizeof lt) == 0, 1u);
        uint8_t a[N48_AGDC_LINK_CONFIG_LEN], b[N48_AGDC_LINK_CONFIG_LEN];
        (void)n48_agdc_fill_link_config_t(a, sizeof a, &lt);
        (void)n48_agdc_fill_link_config(b, sizeof b, 1920u, 1080u);
        expect_u("B2 1080p: reply byte-identical to 0.0.513's", std::memcmp(a, b, sizeof a) == 0, 1u);
        expect_u("B3 1080p: pixel clock 148.5 MHz", lt.pixel_clock, 148500000ull);
        std::free(m);
    }
    // ---- C: fail directions -> the CEA fallback -------------------------------------------------------------------------
    {
        Regs *m = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(m, 0u);
        static n48lr_ro none; std::memset(&none, 0, sizeof none);
        n48_agdc_timing lt;
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &none, &lt);
        expect_u("C1 unbuilt device -> CEA fallback, no read", lt.live == 0u && m->reads == 0u, 1u);
        static n48lr_ro bad; std::memset(&bad, 0, sizeof bad);
        uint32_t wrong[DCN41_NUM_SEGS]; std::memcpy(wrong, SEG, sizeof wrong); wrong[2] ^= 0x40u;
        expect_u("C2 bases not the measured ones -> FAILED", (n48lr_ro_build(&bad, m, &r_read, wrong, MMIO_DWORDS), bad.state), N48LR_FAILED);
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &bad, &lt);
        expect_u("C2 failed device -> CEA fallback", lt.live, 0u);
        expect_u("C2 a failed device is never rebuilt", n48lr_ro_build(&bad, m, &r_read, SEG, MMIO_DWORDS), 0u);
        Regs *dark = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        static n48lr_ro rd; std::memset(&rd, 0, sizeof rd);
        (void)n48lr_ro_build(&rd, dark, &r_read, SEG, MMIO_DWORDS);
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &rd, &lt);
        expect_u("C3 no lit OTG -> CEA fallback", lt.live, 0u);
        light(dark, 0u);
        dark->mem[0x34c0 + DCN41_OTG_OTG_V_TOTAL(0)] = 1500u;   // a re-timed raster no EDID row describes
        n48_agdc_link_timing(2560u, 1440u, &live_adapter, &rd, &lt);
        expect_u("C4 totals with no EDID row -> CEA fallback", lt.live, 0u);
        std::free(dark);
        std::free(m);
    }
    // ---- D: the kext glue ----------------------------------------------------------------------------------------------
    {
        const std::string dcn = slurp(argc > 1 ? argv[1] : nullptr);
        const std::string top = slurp(argc > 2 ? argv[2] : nullptr);
        const std::string dpg = slurp(argc > 3 ? argv[3] : nullptr);
        expect_u("D0 the three kext sources were read", !dcn.empty() && !top.empty() && !dpg.empty(), 1u);
        const std::string lr = body_of(dcn, "uint32_t liveRaster(uint32_t *hAct");
        expect_u("D1 liveRaster calls n48lr_live over gLr", count(lr, "return n48lr_live(&gLr, "), 1u);
        expect_u("D1 liveRaster names no gDcn.armed", count(lr, "gDcn.armed"), 0u);
        expect_u("D1 liveRaster calls no lit_otg()", count(lr, "lit_otg("), 0u);
        expect_u("D1 liveRaster reads nothing through gDcn", count(lr, "gDcn."), 0u);
        const std::string at = body_of(dcn, "void attach(Navi48Bringup *owner) {");
        expect_u("D2 attach builds the read-only device once", count(at, "n48lr_ro_build(&gLr, dev, lr_rreg, seg,"), 1u);
        expect_u("D2 attach arms nothing (no bind, no gDcn.armed)", count(at, "gDcn.") + count(at, "bind("), 0u);
        const std::string st = body_of(top, "bool Navi48Bringup::start(IOService *provider) {");
        const size_t psp = st.find("stagePSP();"), att = st.find("n48dcn::attach(this);"), p4 = st.find("accelExperimentArmed =");
        expect_u("D3 start() calls n48dcn::attach exactly once", count(st, "n48dcn::attach(this);"), 1u);
        expect_u("D3 ... after stagePSP (the device context exists)", psp != std::string::npos && att != std::string::npos && psp < att, 1u);
        expect_u("D3 ... before the Phase 4 block (before any hook)", att != std::string::npos && p4 != std::string::npos && att < p4, 1u);
        const std::string lc = body_of(dpg, "static __attribute__((noinline)) uint32_t agdc_link_config(");
        expect_u("D4 agdc_link_config decides through n48_agdc_link_timing", count(lc, "n48_agdc_link_timing(lw, lh, &agdc_live_raster, nullptr, &lt);"), 1u);
        expect_u("D4 ... and never calls liveRaster itself", count(lc, "liveRaster("), 0u);
        const std::string ad = body_of(dpg, "static uint32_t agdc_live_raster(void *ctx, n48_agdc_raster *lr) {");
        expect_u("D4 the adapter is n48dcn::liveRaster", count(ad, "return n48dcn::liveRaster(&lr->h_active"), 1u);
    }
    std::printf("dcn_liveraster: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
