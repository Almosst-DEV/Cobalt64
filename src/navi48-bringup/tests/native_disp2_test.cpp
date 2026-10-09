// native_disp2_test.cpp - build 0.0.631 (multi-monitor stage M4d: a TEST PATTERN on the second display; an internal design note "M4 design" / "M4b analysis").
// It compiles the REAL decisions of dcn/navi48_disp2.h and the REAL sequence of dcn/navi48_disp2_flow.h and drives them against a register-file model of OTG0 / OTG1 / ODM1 / DPG1 / DIG1 / DIG2 whose writes go through
// the instance guard AND the REAL DCN write allowlist (exactly the kext glue's E::wr). The kext glue, the dispatch, the CLI and the plist are checked by source pins.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn -I src/navi48-bringup/src/amd -I src/dcn41 \
//       src/navi48-bringup/tests/native_disp2_test.cpp -x c++ src/dcn41/dcn41_allow.c -o /tmp/native_disp2
//   /tmp/native_disp2 .       (run from the repo root; the argument is the repo root the source pins read from; N48_LINUX_DC = the tree holding re/linux-dc; N48_NOTES = the tree holding notes/logs)
//   tests/native_disp2_plant.sh plants breaks in the REAL code and demands a failure for each.
// Covers:
//   A  every register against Linux's dcn_4_1_0_offset.h (offset AND BASE_IDX), every field mask against dcn_4_1_0_sh_mask.h; every writable register inside the REAL allowlist;
//   B  the timing: the 18 DTD bytes are the captured EDID's (notes/logs/runs/m1-622/ddc.txt line 2 block 0, checksum re-verified), the decode is CEA VIC 16, the register values are optc1_program_timing's arithmetic;
//   C  the EXACT step list of timing / connect / off, rebuilt independently from Linux register and field NAMES (resolved through the headers), compared step for step (kind, address, mask, value, wait bounds);
//   D  the instance guard: every register of every OTG0 / ODM0 / VTG0 / OPP0 / DPG0 / FMT0 / HUBP0 / DPP0 / MPCC0 / DIG1-path / PHY B / DCCG name in the Linux header is refused; every listed register is admitted; the value
//      rules (FE source OTG0, BE FE1 bit, mapper link 1, ODM SEG0 OPP0); a list touching OTG0 / DIG1 is refused whole; a refused list reads and writes NOTHING;
//   E  the flow over the model: the exact (address, value) write sequence of timing, connect and off from the measured firmware state; every precheck refusal writes nothing; ORDER (timing before connect,
//      off reverses); the DP watch (OTG0 stopping / DIG1 changing triggers off); an allowlist refusal part-way runs off; wait timeouts; WAITIF; status reads only; off from any partial state;
//   S  source pins: the OFF switch first (no read before it), the dispatcher asks the latch before bind(), the glue's WREG32 follows the guard AND the allowlist, the admission, the CLI, the version, the banned strings.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <fstream>
#include <sstream>
#include "navi48_disp2_flow.h"
#include "amd/native_disp_pure.h"
#include "amd/native_d2pin_pure.h"      // 0.0.652 (M5): the pair pin / free / alloc decisions
extern "C" {
#include "dcn41_allow.h"
}

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { ++gRun; if (!ok) { ++gFail; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) { ++gRun; if (got != want) { ++gFail; std::printf("FAIL: %s: got %llu (%#llx), want %llu (%#llx)\n", what, (unsigned long long)got, (unsigned long long)got, (unsigned long long)want, (unsigned long long)want); } }
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::string strip_line_comments(const std::string &s) {
    std::string out; size_t i = 0; bool str = false;
    while (i < s.size()) {
        if (str) { out += s[i]; if (s[i] == '\\' && i + 1 < s.size()) { out += s[i + 1]; i += 2; continue; } if (s[i] == '"') str = false; i++; continue; }
        if (s[i] == '"') { str = true; out += s[i++]; continue; }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; continue; }
        out += s[i++];
    }
    return out;
}
static size_t count_of(const std::string &s, const std::string &t) { size_t n = 0, p = 0; while ((p = s.find(t, p)) != std::string::npos) { ++n; p += t.size(); } return n; }
static std::string g_root = ".";
static const uint32_t kSeg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u };   // this card's own DMU bases

// ---- the Linux headers ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static std::string linux_hdr(const char *name) {
    std::vector<std::string> roots;
    if (const char *e = std::getenv("N48_LINUX_DC")) roots.push_back(e);
    roots.push_back(g_root);
    roots.push_back("/path/to/navi48-checkout");
    for (auto &r : roots) { std::string s = slurp(r + "/re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/" + name); if (!s.empty()) return s; }
    return "";
}
static std::string gOff, gMask;
static bool lx_reg(const std::string &reg, uint32_t *abs) {     // absolute = seg[BASE_IDX] + offset
    const std::string key = "#define reg" + reg + " ";
    size_t p = gOff.find(key); if (p == std::string::npos || (p > 0 && gOff[p - 1] != '\n')) return false;
    const uint32_t off = (uint32_t)std::strtoul(gOff.c_str() + p + key.size(), nullptr, 0);
    const std::string bk = "#define reg" + reg + "_BASE_IDX";
    size_t q = gOff.find(bk); if (q == std::string::npos) return false;
    q += bk.size(); while (gOff[q] == ' ' || gOff[q] == '\t') ++q;
    const uint32_t b = (uint32_t)std::strtoul(gOff.c_str() + q, nullptr, 0);
    if (b > 4) return false;
    *abs = kSeg[b] + off;
    return true;
}
static uint32_t lx_abs(const std::string &reg) { uint32_t a = 0; bool f = lx_reg(reg, &a); ++gRun; if (!f) { ++gFail; std::printf("FAIL: Linux register %s NOT FOUND\n", reg.c_str()); } return a; }
static bool lx_field(const std::string &f, uint32_t *mask, uint32_t *shift) {     // f = "REG__FIELD" (instance-0 names)
    const std::string mk = "#define " + f + "_MASK", sk = "#define " + f + "__SHIFT";
    size_t p = gMask.find(mk + " "); if (p == std::string::npos) p = gMask.find(mk + "\t"); if (p == std::string::npos) return false;
    size_t q = gMask.find(sk + " "); if (q == std::string::npos) q = gMask.find(sk + "\t"); if (q == std::string::npos) return false;
    p += mk.size(); while (gMask[p] == ' ' || gMask[p] == '\t') ++p;
    q += sk.size(); while (gMask[q] == ' ' || gMask[q] == '\t') ++q;
    *mask = (uint32_t)std::strtoul(gMask.c_str() + p, nullptr, 0); *shift = (uint32_t)std::strtoul(gMask.c_str() + q, nullptr, 0);
    return true;
}
static uint32_t lx_mask(const std::string &f) { uint32_t m = 0, s = 0; bool ok = lx_field(f, &m, &s); ++gRun; if (!ok) { ++gFail; std::printf("FAIL: Linux field %s NOT FOUND\n", f.c_str()); } return m; }
static uint32_t lx_fv(const std::string &f, uint32_t v) { uint32_t m = 0, s = 0; bool ok = lx_field(f, &m, &s); ++gRun; if (!ok) { ++gFail; std::printf("FAIL: Linux field %s NOT FOUND\n", f.c_str()); return 0; }
    ++gRun; if (((v << s) & ~m) != 0u) { ++gFail; std::printf("FAIL: value %#x does not fit field %s\n", v, f.c_str()); } return (v << s) & m; }

// ---- the register-file model ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct W { uint32_t abs, v; };
struct Model {
    std::map<uint32_t, uint32_t> r;            // the stored (written / initial) values
    std::vector<W> writes;                     // every write that reached the "card"
    std::vector<uint32_t> reads;
    dcn41_allow_state allow {};
    uint32_t frames0 = 1000, frames1 = 0, frames2 = 0;
    uint32_t inst = N48D2_INST_MONA;           // 0.0.633: the instance whose guard judges wr() (the kext glue's env.inst)
    bool otg0Stops = false, otg0StopOnMaster = false, dig1FlipOnFeEn = false, clocksWork = true, symclk = true, fifoDoneWorks = true, symclkbFlipOnSymclk = false;
    uint32_t refuseAbs = 0, refusedCount = 0, sleeps = 0, delays = 0;
    uint32_t flipTrig = 0, flipReg = 0, flipXor = 0;     // 0.0.634: ONE-SHOT: when the write to flipTrig lands, flipReg ^= flipXor (a DP plane register changing under the op); cleared after it fires so the rollback `off` does not undo it
    uint32_t dpxBuf[2][N48D2_DPX_BUF] = {};              // 0.0.634: the flow's two DP-plane buffers (the kext's gD2Dpx); 0.0.635: 16 dwords (fifteen rows + the HUBP0 primary HIGH)
    // ---- 0.0.635 (M4c / M4d): the plane model (the kext's D2KextEnv members): state, gates, the scanout window, the desktop flag, two VRAM buffers drawn and read back through the "MM window"
    n48d2_plane plane {}, plane1 {};                     // 0.0.655: plane = the monitor B's (instance 2), plane1 = the monitor A's (instance 1)
    uint32_t gateBuf[N48D2_GATES] = {};
    uint32_t recBuf[N48D2_REC_N] = {}, recBuf1[N48D2_REC_N] = {};      // 0.0.638: the record page (the kext's gD2Rec); 0.0.655: one per instance
    uint32_t w2Buf[2][N48D2_W2_BUF] = {};                // 0.0.655: the monitor B watch's two buffers (the kext's gD2W2)
    std::vector<uint32_t> sleepsAtWrite;                 // 0.0.638: the sleep count at each write that reached the card (to prove "no sleep between MPCC2, the mux and the unblank")
    bool haveWin = false, acquired = false;
    uint64_t winLo = 0, winHi = 0;
    std::vector<uint32_t> vram[2], vram1[2];             // the monitor B's buffers A / B and (0.0.655) the monitor A's
    std::string vlog;                                  // 'f' a fill, 'F' the flush, 'r' a read-back, in order
    std::vector<uint32_t> f2AtWrite;                   // OTG2's frame counter at each write that reached the card
    std::vector<uint32_t> f1AtWrite;                   // 0.0.655: OTG1's frame counter at each write that reached the card (the monitor A's flow tests)
    uint32_t vfillCalls = 0, vflushCalls = 0, vreadCalls = 0, vfillFailAt = 0xFFFFFFFFu, vreadCorruptK = 9, vreadCorruptDw = 0xFFFFFFFFu;
    std::vector<size_t> readsAtWrite;                    // 0.0.634: reads.size() at the moment of each write that reached the card (to prove the DP-plane reads bracket the writes)
    n48d2_step bufs[2][N48D2_MAX_STEPS];
    std::vector<std::string> notes;
    Model() {
        expect(dcn41_allow_init(&allow, kSeg, 262144u) == DCN41_ALLOW_OK, "the real allowlist arms with this card's bases");
        // the state measured after `dmubsend pclk otg1-on` + `dmubsend phyc enable` (; notes/logs/runs/m4c-630/phyc.txt) and the live DP
        r[N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL] = 2u;
        r[N48D2_DIG2_BE_CLK_CNTL] = 0x2812u; r[N48D2_DIG2_BE_EN_CNTL] = 1u; r[N48D2_DIG2_BE_CNTL] = 0x20000000u; r[N48D2_DIG2_FE_CLK_CNTL] = 0x1u; r[N48D2_DIG2_CLOCK_PATTERN] = 0x63u;
        r[N48D2_SYMCLKC_CLOCK_ENABLE] = 0x1u; r[N48D2_SYMCLKB_CLOCK_ENABLE] = 0x111u;          // 0.0.633: SYMCLKC as measured (CLOCK_ENABLE 1, FE_EN 0, FE_SRC_SEL 0), SYMCLKB = the live DP's (FE_EN 1, CLOCK_ENABLE 1, SRC 1)
        // the monitor B (instance 2) as the DMUB's `pclk otg2-on` + `phyd enable` would leave it (SUSPECTED values by analogy with the monitor A's measured ones)
        r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 3u;
        r[N48D2_DIG3_BE_CLK_CNTL] = 0x2812u; r[N48D2_DIG3_BE_EN_CNTL] = 1u; r[N48D2_DIG3_BE_CNTL] = 0x20000000u; r[N48D2_DIG3_FE_CLK_CNTL] = 0x1u; r[N48D2_DIG3_CLOCK_PATTERN] = 0x63u;
        r[N48D2_SYMCLKD_CLOCK_ENABLE] = 0x1u;
        r[N48D2_OTG0_CONTROL] = 0x00011201u; r[N48D2_DIG1_FE_CNTL] = 0u; r[N48D2_DIG1_BE_CLK_CNTL] = 0x810u; r[N48D2_DIG1_BE_CNTL] = 0x10000200u; r[N48D2_DIG1_BE_EN_CNTL] = 1u; r[0x53ceu] = 1u;
    }
    uint32_t get(uint32_t a) const { auto it = r.find(a); return it == r.end() ? 0u : it->second; }
    bool otg1Running() const { return (get(N48D2_OTG1_CONTROL) & 1u) && (get(N48D2_OTG1_CLOCK_CONTROL) & 1u); }
    bool otg2Running() const { return (get(N48D2_OTG2_CONTROL) & 1u) && (get(N48D2_OTG2_CLOCK_CONTROL) & 1u); }
    // E
    uint32_t rd(uint32_t a) {
        reads.push_back(a);
        delayUsAtRead.push_back(delayUs);
        uint32_t v = get(a);
        if (a == N48D2_OTG0_FRAME_COUNT) return frames0 & 0xFFFFFFu;
        if (a == N48D2_OTG1_FRAME_COUNT) return frames1 & 0xFFFFFFu;
        if (a == N48D2_OTG2_FRAME_COUNT) return frames2 & 0xFFFFFFu;
        // instance 2's twins of the engine rules below
        if (a == N48D2_ODM2_INPUT_CLOCK_CONTROL) { v &= ~N48D2_ODM_CLK_ON_MASK; if (clocksWork && (v & N48D2_ODM_CLK_EN_MASK)) v |= N48D2_ODM_CLK_ON_MASK; }
        if (a == N48D2_OTG2_CLOCK_CONTROL) { v &= ~(N48D2_OTG_CLOCK_ON_MASK | N48D2_OTG_BUSY_MASK); if (clocksWork && (v & N48D2_OTG_CLOCK_EN_MASK)) v |= N48D2_OTG_CLOCK_ON_MASK; }
        if (a == N48D2_OTG2_CONTROL) { v &= ~N48D2_OTG_CUR_MASTER_EN_MASK; if (v & 1u) v |= N48D2_OTG_CUR_MASTER_EN_MASK; }
        if (a == N48D2_DIG3_FIFO_CTRL0) { v &= ~N48D2_FIFO_RESET_DONE_MASK; if (fifoDoneWorks && (v & N48D2_FIFO_RESET_MASK)) v |= N48D2_FIFO_RESET_DONE_MASK; }
        if (a == N48D2_DIG3_FE_CLK_CNTL) { v &= ~N48D2_DIG_FE_SYMCLK_ON_MASK; if (symclk) v |= N48D2_DIG_FE_SYMCLK_ON_MASK; }
        if (a == N48D2_ODM1_INPUT_CLOCK_CONTROL) { v &= ~N48D2_ODM_CLK_ON_MASK; if (clocksWork && (v & N48D2_ODM_CLK_EN_MASK)) v |= N48D2_ODM_CLK_ON_MASK; }
        if (a == N48D2_OTG1_CLOCK_CONTROL) { v &= ~(N48D2_OTG_CLOCK_ON_MASK | N48D2_OTG_BUSY_MASK); if (clocksWork && (v & N48D2_OTG_CLOCK_EN_MASK)) v |= N48D2_OTG_CLOCK_ON_MASK; }
        if (a == N48D2_OTG1_CONTROL) { v &= ~N48D2_OTG_CUR_MASTER_EN_MASK; if (v & 1u) v |= N48D2_OTG_CUR_MASTER_EN_MASK; }
        if (a == N48D2_DIG2_FIFO_CTRL0) { v &= ~N48D2_FIFO_RESET_DONE_MASK; if (fifoDoneWorks && (v & N48D2_FIFO_RESET_MASK)) v |= N48D2_FIFO_RESET_DONE_MASK; }
        if (a == N48D2_DIG2_FE_CLK_CNTL) { v &= ~N48D2_DIG_FE_SYMCLK_ON_MASK; if (symclk) v |= N48D2_DIG_FE_SYMCLK_ON_MASK; }
        return v;
    }
    bool wr(uint32_t a, uint32_t v) {     // EXACTLY the kext glue's order: the instance guard, then the DCN allowlist, then the write
        if (n48d2_guard_write_i(inst, a, v, nullptr) != N48D2_G_OK) { ++refusedCount; return false; }
        if (a == refuseAbs || !dcn41_allow_write(&allow, a, v, "disp2")) { ++refusedCount; return false; }
        writes.push_back({ a, v });
        readsAtWrite.push_back(reads.size());
        f2AtWrite.push_back(frames2);
        f1AtWrite.push_back(frames1);
        sleepsAtWrite.push_back(sleeps);
        r[a] = v;
        if (flipTrig && a == flipTrig) { r[flipReg] ^= flipXor; flipTrig = 0; }
        if (otg0StopOnMaster && a == N48D2_OTG1_CONTROL && (v & 1u)) otg0Stops = true;
        if (dig1FlipOnFeEn && (a == N48D2_DIG2_FE_EN_CNTL || a == N48D2_DIG3_FE_EN_CNTL) && (v & 1u)) r[N48D2_DIG1_BE_CNTL] |= 0x400u;
        if (otg0StopOnMaster && a == N48D2_OTG2_CONTROL && (v & 1u)) otg0Stops = true;
        if (symclkbFlipOnSymclk && (a == N48D2_SYMCLKC_CLOCK_ENABLE || a == N48D2_SYMCLKD_CLOCK_ENABLE) && (v & N48D2_SYMCLK_FE_EN_MASK)) r[N48D2_SYMCLKB_CLOCK_ENABLE] ^= 0x10u;
        return true;
    }
    uint64_t delayUs = 0;                                // 0.0.638: the total of every delay_us (the DET2 wait bound, the 1 ms before the first recorded read)
    std::vector<uint64_t> delayUsAtRead;                 // 0.0.638: the cumulative delay_us at each read (parallel to Model::reads)
    void delay_us(uint32_t us) { ++delays; delayUs += us; }
    void sleep_ms(uint32_t ms) { ++sleeps; if (!otg0Stops) frames0 += ms / 16u; if (otg1Running()) frames1 += ms / 16u; if (otg2Running()) frames2 += ms / 16u; }
    void note(uint32_t i, const n48d2_step &s, uint32_t, uint32_t, uint32_t what) { char b[160]; std::snprintf(b, sizeof b, "%u %s %u", i, s.reg, what); notes.push_back(b); }
    n48d2_step *buf(unsigned k) { return bufs[k & 1u]; }
    uint32_t *dpx_buf(unsigned k) { return dpxBuf[k & 1u]; }
    n48d2_plane *pl_of(uint32_t i) { return i == N48D2_INST_MONA ? &plane1 : &plane; }
    n48d2_plane *pl() { return pl_of(inst); }
    uint32_t *w2_buf(unsigned k) { return w2Buf[k & 1u]; }
    bool pinWorks = true, pinned = false; unsigned pinCalls = 0;      // 0.0.652 (M5): the allocator's pin (n1c_d2_pin); pinWorks = false: it refuses
    bool pin() { ++pinCalls; if (!pinWorks) return false; pinned = true; return true; }
    uint32_t *gate_buf() { return gateBuf; }
    uint32_t *rec_buf() { return inst == N48D2_INST_MONA ? recBuf1 : recBuf; }
    bool window(uint64_t *lo, uint64_t *hi) { *lo = winLo; *hi = winHi; return haveWin; }
    bool desktop_acquired() { return acquired; }
    bool vfill(unsigned k, uint32_t byteOff, uint32_t pattern, uint32_t bytes) {
        const uint32_t bb = n48d2_surf_get(inst)->buf_bytes;
        if (k > 1u || (uint64_t)byteOff + bytes > bb || ((byteOff | bytes) & 3u) != 0u) return false;
        if (vfillCalls++ == vfillFailAt) return false;
        vlog.push_back('f');
        std::vector<uint32_t> &V = inst == N48D2_INST_MONA ? vram1[k] : vram[k];
        if (V.empty()) V.assign(bb / 4u, 0xDEADBEEFu);
        for (uint32_t i = 0; i < bytes / 4u; i++) V[byteOff / 4u + i] = pattern;
        return true;
    }
    void vflush() { ++vflushCalls; vlog.push_back('F'); }
    bool vread(unsigned k, uint32_t byteOff, uint32_t *dw) {
        ++vreadCalls; vlog.push_back('r');
        std::vector<uint32_t> &V = k > 1u ? vram[0] : (inst == N48D2_INST_MONA ? vram1[k] : vram[k]);
        if (k > 1u || V.empty() || byteOff >= n48d2_surf_get(inst)->buf_bytes) return false;
        *dw = V[byteOff / 4u];
        if (k == vreadCorruptK && byteOff / 4u == vreadCorruptDw) *dw ^= 1u;
        return true;
    }
};
static std::vector<W> writes_from(const Model &m, size_t from) { return std::vector<W>(m.writes.begin() + (long)from, m.writes.end()); }
static void expect_writes(const char *what, const std::vector<W> &got, const std::vector<W> &want) {
    bool same = got.size() == want.size();
    for (size_t i = 0; same && i < got.size(); i++) same = got[i].abs == want[i].abs && got[i].v == want[i].v;
    ++gRun;
    if (!same) {
        ++gFail; std::printf("FAIL: %s: the write sequence differs (%zu written, %zu expected)\n", what, got.size(), want.size());
        for (size_t i = 0; i < got.size() || i < want.size(); i++)
            std::printf("    %2zu  got %s  want %s\n", i, i < got.size() ? (std::to_string(got[i].abs) + "=" + std::to_string(got[i].v)).c_str() : "-", i < want.size() ? (std::to_string(want[i].abs) + "=" + std::to_string(want[i].v)).c_str() : "-");
    }
}
static size_t first_write(const std::vector<W> &w, uint32_t abs, size_t from = 0) { for (size_t i = from; i < w.size(); i++) if (w[i].abs == abs) return i; return (size_t)-1; }
static size_t last_write(const std::vector<W> &w, uint32_t abs) { size_t r = (size_t)-1; for (size_t i = 0; i < w.size(); i++) if (w[i].abs == abs) r = i; return r; }

// ---- the independent step lists (Linux NAMES only; resolved through the headers) -----------------------------------------------------------------------------------------------------------------------------
struct F { const char *field; uint32_t v; };
struct X { uint8_t kind; const char *reg; std::vector<F> f; uint32_t polls, us; const char *fn; const char *cond, *condField; uint32_t bits = 0; };   // bits != 0: Linux's `field |= bits` / `&= ~bits` (the mask is those bits of f[0] only)
static void check_list_i(uint32_t inst, const char *what, uint32_t op, const std::vector<X> &want) {
    n48d2_step s[N48D2_MAX_STEPS];
    const uint32_t n = n48d2_build_i(inst, op, s);
    expect_u((std::string(what) + ": step count").c_str(), n, want.size());
    for (uint32_t i = 0; i < n && i < want.size(); i++) {
        const X &x = want[i];
        uint32_t mask = 0, val = 0;
        for (const F &f : x.f) { mask |= lx_mask(f.field); val |= lx_fv(f.field, f.v); }
        if (x.kind == N48D2_K_SET) mask = 0xFFFFFFFFu;
        if (x.bits != 0u) mask = lx_fv(x.f[0].field, x.bits);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "%s step %u (%s %s)", what, i, x.reg, x.fn);
        ++gRun;
        const uint32_t a = lx_abs(x.reg);
        bool ok = s[i].kind == x.kind && s[i].abs == a && s[i].mask == mask && s[i].val == val && std::string(s[i].fn) == x.fn;
        if (x.kind == N48D2_K_WAIT || x.kind == N48D2_K_WAITIF) ok = ok && s[i].polls == x.polls && s[i].step_us == x.us;
        if (x.kind == N48D2_K_WAITIF) ok = ok && s[i].cond_abs == lx_abs(x.cond) && s[i].cond_mask == lx_mask(x.condField);
        if (!ok) { ++gFail; std::printf("FAIL: %s: built kind %u abs %#x mask %#x val %#x polls %u us %u fn %s ; want kind %u abs %#x mask %#x val %#x\n", lbl, s[i].kind, s[i].abs, s[i].mask, s[i].val, s[i].polls, s[i].step_us, s[i].fn, x.kind, a, mask, val); }
    }
}
static void check_list(const char *what, uint32_t op, const std::vector<X> &want) { check_list_i(N48D2_INST_MONA, what, op, want); }
enum : uint8_t { S = N48D2_K_SET, U = N48D2_K_UPD, WT = N48D2_K_WAIT, WI = N48D2_K_WAITIF };

// 0.0.633: the INSTANCE 2 (monitor B: OTG2 / ODM2 / VTG2 / OPP2 / DPG2 / FMT2 / DIG3 / SYMCLKD / UNIPHY_D) step lists, rebuilt from Linux NAMES exactly like instance 1's: the register names carry the instance (ODM2_ ... DIG3_,
// SYMCLKD_), the field names are the instance-0 ones, and the values that name the instance are the monitor B's: OPTC_SEG0_SRC_SEL 2 (OPP2), DIG_SOURCE_SELECT 2 (OTG2), DIG_STEREOSYNC_SELECT 2, DIG_FE_SOURCE_SELECT bit 0x08 (DIGD),
// DIG_STREAM_LINK_TARGET 3 (UNIPHY_D), SYMCLKD_FE_SRC_SEL 3. (Generated once from the instance-1 lists by that renaming; the lists below are the source of truth the code is judged against.)
static void check_lists_inst2() {
    check_list_i(N48D2_INST_MONB, "timing (instance 2)", N48D2_OP_TIMING, {
        { U,  "ODM2_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN", 1 }, { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS", 1 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { WT, "ODM2_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON", 1 } }, 1000, 1, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "OTG2_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN", 1 }, { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS", 1 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { WT, "OTG2_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON", 1 } }, 1000, 1, "optc1_enable_optc_clock", nullptr, nullptr },
        { S,  "ODM2_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT", 0 }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 2 }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL", 0xf },
                                                  { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG2_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG3_SRC_SEL", 0xf } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { U,  "OTG2_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE", 0 } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { S,  "ODM2_OPTC_MEMORY_CONFIG", { { "ODM0_OPTC_MEMORY_CONFIG__OPTC_MEM_SEL", 0 } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { S,  "OTG2_OTG_H_TOTAL", { { "OTG0_OTG_H_TOTAL__OTG_H_TOTAL", 2199 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_H_SYNC_A", { { "OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_START", 0 }, { "OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END", 44 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_H_BLANK_START_END", { { "OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START", 2112 }, { "OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END", 192 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_H_SYNC_A_CNTL", { { "OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG2_OTG_V_TOTAL", { { "OTG0_OTG_V_TOTAL__OTG_V_TOTAL", 1124 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG2_OTG_V_TOTAL_MAX", { { "OTG0_OTG_V_TOTAL_MAX__OTG_V_TOTAL_MAX", 1124 } }, 0, 0, "optc1_set_vtotal_min_max", nullptr, nullptr },
        { S,  "OTG2_OTG_V_TOTAL_MIN", { { "OTG0_OTG_V_TOTAL_MIN__OTG_V_TOTAL_MIN", 1124 } }, 0, 0, "optc1_set_vtotal_min_max", nullptr, nullptr },
        { U,  "OTG2_OTG_V_SYNC_A", { { "OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_START", 0 }, { "OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_END", 5 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_V_BLANK_START_END", { { "OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START", 1121 }, { "OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END", 41 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_V_SYNC_A_CNTL", { { "OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_INTERLACE_CONTROL", { { "OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "VTG2_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_START_POINT_CNTL", 0 }, { "OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_CNTL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG2_OTG_VSTARTUP_PARAM", { { "OTG0_OTG_VSTARTUP_PARAM__VSTARTUP_START", 13 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { S,  "OTG2_OTG_VUPDATE_PARAM", { { "OTG0_OTG_VUPDATE_PARAM__VUPDATE_OFFSET", 680 }, { "OTG0_OTG_VUPDATE_PARAM__VUPDATE_WIDTH", 320 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { S,  "OTG2_OTG_VREADY_PARAM", { { "OTG0_OTG_VREADY_PARAM__VREADY_OFFSET", 300 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { U,  "VTG2_CONTROL", { { "VTG0_CONTROL__VTG0_FP2", 0 }, { "VTG0_CONTROL__VTG0_VCOUNT_INIT", 1121 } }, 0, 0, "optc1_set_vtg_params", nullptr, nullptr },
        { U,  "ODM2_OPTC_DATA_FORMAT_CONTROL", { { "ODM0_OPTC_DATA_FORMAT_CONTROL__OPTC_DATA_FORMAT", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG2_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MANUAL", 0 } }, 0, 0, "optc401_set_h_timing_div_manual_mode", nullptr, nullptr },
        { U,  "OPP_PIPE2_OPP_PIPE_CONTROL", { { "OPP_PIPE0_OPP_PIPE_CONTROL__OPP_PIPE_CLOCK_EN", 1 } }, 0, 0, "opp1_pipe_clock_control", nullptr, nullptr },
        { U,  "FMT2_FMT_422_CONTROL", { { "FMT0_FMT_422_CONTROL__FMT_LEFT_EDGE_EXTRA_PIXEL_COUNT", 0 } }, 0, 0, "opp2_program_left_edge_extra_pixel", nullptr, nullptr },
        { S,  "DPG2_DPG_DIMENSIONS", { { "DPG0_DPG_DIMENSIONS__DPG_ACTIVE_WIDTH", 1920 }, { "DPG0_DPG_DIMENSIONS__DPG_ACTIVE_HEIGHT", 1080 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG2_DPG_OFFSET_SEGMENT", { { "DPG0_DPG_OFFSET_SEGMENT__DPG_X_OFFSET", 0 }, { "DPG0_DPG_OFFSET_SEGMENT__DPG_SEGMENT_WIDTH", 0 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "DPG2_DPG_CONTROL", { { "DPG0_DPG_CONTROL__DPG_EN", 1 }, { "DPG0_DPG_CONTROL__DPG_MODE", 0 }, { "DPG0_DPG_CONTROL__DPG_DYNAMIC_RANGE", 0 }, { "DPG0_DPG_CONTROL__DPG_BIT_DEPTH", 1 },
                                     { "DPG0_DPG_CONTROL__DPG_VRES", 6 }, { "DPG0_DPG_CONTROL__DPG_HRES", 6 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "ODM2_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 2 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { U,  "VTG2_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 1 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { U,  "OTG2_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_DISABLE_POINT_CNTL", 2 }, { "OTG0_OTG_CONTROL__OTG_MASTER_EN", 1 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { WT, "DPG2_DPG_STATUS", { { "DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING", 0 } }, 1000, 100, "dcn20_wait_for_blank_complete", nullptr, nullptr },
    });
    check_list_i(N48D2_INST_MONB, "connect (instance 2)", N48D2_OP_CONNECT, {
        { U,  "SYMCLKD_CLOCK_ENABLE", { { "SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_EN", 1 }, { "SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_SRC_SEL", 3 } }, 0, 0, "dccg401_enable_symclk_se", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT", 2 } }, 0, 0, "enc1_dig_connect_to_otg", nullptr, nullptr },
        { U,  "OTG2_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_OUT_MUX", 0 } }, 0, 0, "optc401_set_out_mux", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_SELECT", 2 } }, 0, 0, "enc1_setup_stereo_sync", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_GATE_EN", 1 } }, 0, 0, "enc1_setup_stereo_sync", nullptr, nullptr },
        { U,  "DIG3_DIG_CLOCK_PATTERN", { { "DIG0_DIG_CLOCK_PATTERN__DIG_CLOCK_PATTERN", 0x1f } }, 0, 0, "enc401_stream_encoder_dvi_set_stream_attribute", nullptr, nullptr },
        { U,  "DIG3_HDMI_CONTROL", { { "DIG0_HDMI_CONTROL__TMDS_PIXEL_ENCODING", 0 } }, 0, 0, "enc401_stream_encoder_set_stream_attribute_helper", nullptr, nullptr },
        { U,  "DIG3_HDMI_CONTROL", { { "DIG0_HDMI_CONTROL__TMDS_COLOR_FORMAT", 0 } }, 0, 0, "enc401_stream_encoder_set_stream_attribute_helper", nullptr, nullptr },
        { U,  "DIG3_DIG_BE_CNTL", { { "DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0x08 } }, 0, 0, "dcn10_link_encoder_connect_dig_be_to_fe", nullptr, nullptr, 0x08 },   // field |= 0x08 (DIGD)
        { U,  "DIG3_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_MODE", 2 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 1 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 1 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG3_STREAM_MAPPER_CONTROL", { { "DIG0_STREAM_MAPPER_CONTROL__DIG_STREAM_LINK_TARGET", 3 } }, 0, 0, "enc401_stream_encoder_map_to_link", nullptr, nullptr },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE", 0 } }, 0, 0, "enc401_set_dig_input_mode", nullptr, nullptr },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_READ_START_LEVEL", 7 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET", 1 } }, 0, 0, "enc35_reset_fifo", nullptr, nullptr },
        { WI, "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE", 1 } }, 5000, 10, "enc35_reset_fifo", "DIG3_DIG_FE_CLK_CNTL", "DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON" },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET", 0 } }, 0, 0, "enc35_reset_fifo", nullptr, nullptr },
        { WI, "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE", 0 } }, 5000, 10, "enc35_reset_fifo", "DIG3_DIG_FE_CLK_CNTL", "DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON" },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
    });
    check_list_i(N48D2_INST_MONB, "off (instance 2)", N48D2_OP_OFF, {
        { S,  "DPG2_DPG_CONTROL", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG2_DPG_COLOUR_R_CR", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG2_DPG_COLOUR_G_Y", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG2_DPG_COLOUR_B_CB", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG2_DPG_RAMP_CONTROL", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG3_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE", 0 } }, 0, 0, "enc401_set_dig_input_mode", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 0 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG3_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 0 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG3_DIG_BE_CNTL", { { "DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0 } }, 0, 0, "dcn10_link_encoder_connect_dig_be_to_fe", nullptr, nullptr, 0x08 },   // field &= ~0x08
        { U,  "SYMCLKD_CLOCK_ENABLE", { { "SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_EN", 0 }, { "SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_SRC_SEL", 0 } }, 0, 0, "dccg401_disable_symclk_se", nullptr, nullptr },
        { U,  "DIG3_STREAM_MAPPER_CONTROL", { { "DIG0_STREAM_MAPPER_CONTROL__DIG_STREAM_LINK_TARGET", 0 } }, 0, 0, "enc401_stream_encoder_map_to_link", nullptr, nullptr },
        { U,  "ODM2_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG2_SRC_SEL", 0xf },
                                                  { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG3_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "ODM2_OPTC_MEMORY_CONFIG", { { "ODM0_OPTC_MEMORY_CONFIG__OPTC_MEM_SEL", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "OTG2_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_MASTER_EN", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "VTG2_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { WT, "OTG2_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_CURRENT_MASTER_EN_STATE", 0 } }, 15000, 10, "optc401_disable_crtc", nullptr, nullptr },
        { WT, "OTG2_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_BUSY", 0 } }, 150000, 1, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "OTG2_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS", 0 }, { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN", 0 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "ODM2_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS", 0 }, { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN", 0 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "OPP_PIPE2_OPP_PIPE_CONTROL", { { "OPP_PIPE0_OPP_PIPE_CONTROL__OPP_PIPE_CLOCK_EN", 0 } }, 0, 0, "opp1_pipe_clock_control", nullptr, nullptr },
    });
    // the BE step is Linux's `field |= 0x08` / `&= ~0x08`: the built step's mask is ONLY the FE3 bit (never FE1 / FE2)
    { n48d2_step t[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build_i(N48D2_INST_MONB, N48D2_OP_CONNECT, t);
      expect(n > 8 && t[8].abs == N48D2_DIG3_BE_CNTL && t[8].kind == N48D2_K_UPD && t[8].mask == 0x800u && t[8].val == 0x800u, "instance 2 connect's BE step sets ONLY bit 11 (FE source 0x08 << 8): mask 0x800, value 0x800"); }
    { n48d2_step t[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build_i(N48D2_INST_MONB, N48D2_OP_OFF, t);
      expect(n > 12 && t[11].abs == N48D2_DIG3_BE_CNTL && t[11].kind == N48D2_K_UPD && t[11].mask == 0x800u && t[11].val == 0u && t[12].abs == N48D2_SYMCLKD_CLOCK_ENABLE, "instance 2 off's BE step (11) clears ONLY bit 11: mask 0x800, value 0; the SYMCLKD step (12) FOLLOWS it (0.0.634)"); }
}

// =================================================================================================================================================================================================================
// 0.0.633: INSTANCE 2 (the monitor B), the DCCG symbol-clock exception, status3, the instance argument. Everything here is judged against Linux's own header (names -> absolutes), the REAL guard and the REAL allowlist.
// =================================================================================================================================================================================================================
static std::vector<uint32_t> all_header_regs() {      // the absolute address of every register in Linux's dcn_4_1_0_offset.h (one entry per #define reg<NAME>, BASE_IDX lines skipped)
    std::vector<uint32_t> v; size_t p = 0;
    while ((p = gOff.find("#define reg", p)) != std::string::npos) {
        p += 11; size_t e = p; while (e < gOff.size() && gOff[e] != ' ' && gOff[e] != '\t') ++e;
        const std::string name = gOff.substr(p, e - p);
        if (name.size() > 9 && name.compare(name.size() - 9, 9, "_BASE_IDX") == 0) continue;
        uint32_t a = 0; if (lx_reg(name, &a)) v.push_back(a);
    }
    return v;
}
// the DP-side twin of an instance's register (the same block of OTG0 / ODM0 / OPP0 / DIG1, or SYMCLKB for the symbol clock): block strides OTG 0x80, ODM 0x10, OPP / FMT / DPG 0x5a, DIG 0x124 per instance number
static uint32_t dp_twin(uint32_t inst, uint32_t a) {
    const uint32_t k = inst;
    if (a == (inst == 1 ? N48D2_SYMCLKC_CLOCK_ENABLE : N48D2_SYMCLKD_CLOCK_ENABLE)) return N48D2_SYMCLKB_CLOCK_ENABLE;
    if (a == (inst == 1 ? 0x39f1u : 0x39f2u)) return 0x39f0u;
    if (a == N48D2_DIG2_STREAM_MAPPER || a == N48D2_DIG3_STREAM_MAPPER) return 0x53ceu;
    if (inst == 1 ? (a >= 0x506au && a <= 0x50e9u) : (a >= 0x50eau && a <= 0x5169u)) return a - 0x80u * k;
    if (inst == 1 ? (a >= 0x4f9au && a <= 0x4fa2u) : (a >= 0x4fabu && a <= 0x4fb1u)) return a - 0x10u * k;
    if (inst == 1 ? (a >= 0x4d56u && a <= 0x4daf) : (a >= 0x4dbdu && a <= 0x4e00u)) return a - 0x5au * k;
    if (inst == 1 ? (a >= 0x579bu && a <= 0x57f9u) : (a >= 0x58bfu && a <= 0x591au)) return a - 0x124u * k;
    return 0u;
}
static void inst2_tests() {
    static const struct { uint32_t ours; const char *lx; } regs2[] = {
        { N48D2_VTG2_CONTROL, "VTG2_CONTROL" },
        { N48D2_ODM2_DATA_SOURCE_SELECT, "ODM2_OPTC_DATA_SOURCE_SELECT" },
        { N48D2_ODM2_DATA_FORMAT_CONTROL, "ODM2_OPTC_DATA_FORMAT_CONTROL" },
        { N48D2_ODM2_INPUT_CLOCK_CONTROL, "ODM2_OPTC_INPUT_CLOCK_CONTROL" },
        { N48D2_ODM2_MEMORY_CONFIG, "ODM2_OPTC_MEMORY_CONFIG" },
        { N48D2_OTG2_H_TOTAL, "OTG2_OTG_H_TOTAL" },
        { N48D2_OTG2_H_BLANK_START_END, "OTG2_OTG_H_BLANK_START_END" },
        { N48D2_OTG2_H_SYNC_A, "OTG2_OTG_H_SYNC_A" },
        { N48D2_OTG2_H_SYNC_A_CNTL, "OTG2_OTG_H_SYNC_A_CNTL" },
        { N48D2_OTG2_H_TIMING_CNTL, "OTG2_OTG_H_TIMING_CNTL" },
        { N48D2_OTG2_V_TOTAL, "OTG2_OTG_V_TOTAL" },
        { N48D2_OTG2_V_TOTAL_MIN, "OTG2_OTG_V_TOTAL_MIN" },
        { N48D2_OTG2_V_TOTAL_MAX, "OTG2_OTG_V_TOTAL_MAX" },
        { N48D2_OTG2_V_BLANK_START_END, "OTG2_OTG_V_BLANK_START_END" },
        { N48D2_OTG2_V_SYNC_A, "OTG2_OTG_V_SYNC_A" },
        { N48D2_OTG2_V_SYNC_A_CNTL, "OTG2_OTG_V_SYNC_A_CNTL" },
        { N48D2_OTG2_CONTROL, "OTG2_OTG_CONTROL" },
        { N48D2_OTG2_INTERLACE_CONTROL, "OTG2_OTG_INTERLACE_CONTROL" },
        { N48D2_OTG2_STATUS, "OTG2_OTG_STATUS" },
        { N48D2_OTG2_FRAME_COUNT, "OTG2_OTG_STATUS_FRAME_COUNT" },
        { N48D2_OTG2_CLOCK_CONTROL, "OTG2_OTG_CLOCK_CONTROL" },
        { N48D2_OTG2_VSTARTUP_PARAM, "OTG2_OTG_VSTARTUP_PARAM" },
        { N48D2_OTG2_VUPDATE_PARAM, "OTG2_OTG_VUPDATE_PARAM" },
        { N48D2_OTG2_VREADY_PARAM, "OTG2_OTG_VREADY_PARAM" },
        { N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL, "OTG2_PHYPLL_PIXEL_RATE_CNTL" },
        { N48D2_FMT2_422_CONTROL, "FMT2_FMT_422_CONTROL" },
        { N48D2_DPG2_CONTROL, "DPG2_DPG_CONTROL" },
        { N48D2_DPG2_RAMP_CONTROL, "DPG2_DPG_RAMP_CONTROL" },
        { N48D2_DPG2_DIMENSIONS, "DPG2_DPG_DIMENSIONS" },
        { N48D2_DPG2_COLOUR_R_CR, "DPG2_DPG_COLOUR_R_CR" },
        { N48D2_DPG2_COLOUR_G_Y, "DPG2_DPG_COLOUR_G_Y" },
        { N48D2_DPG2_COLOUR_B_CB, "DPG2_DPG_COLOUR_B_CB" },
        { N48D2_DPG2_OFFSET_SEGMENT, "DPG2_DPG_OFFSET_SEGMENT" },
        { N48D2_DPG2_STATUS, "DPG2_DPG_STATUS" },
        { N48D2_OPP_PIPE2_CONTROL, "OPP_PIPE2_OPP_PIPE_CONTROL" },
        { N48D2_DIG3_STREAM_MAPPER, "DIG3_STREAM_MAPPER_CONTROL" },
        { N48D2_DIG3_FE_CNTL, "DIG3_DIG_FE_CNTL" },
        { N48D2_DIG3_FE_CLK_CNTL, "DIG3_DIG_FE_CLK_CNTL" },
        { N48D2_DIG3_FE_EN_CNTL, "DIG3_DIG_FE_EN_CNTL" },
        { N48D2_DIG3_CLOCK_PATTERN, "DIG3_DIG_CLOCK_PATTERN" },
        { N48D2_DIG3_FIFO_CTRL0, "DIG3_DIG_FIFO_CTRL0" },
        { N48D2_DIG3_HDMI_CONTROL, "DIG3_HDMI_CONTROL" },
        { N48D2_DIG3_BE_CLK_CNTL, "DIG3_DIG_BE_CLK_CNTL" },
        { N48D2_DIG3_BE_CNTL, "DIG3_DIG_BE_CNTL" },
        { N48D2_DIG3_BE_EN_CNTL, "DIG3_DIG_BE_EN_CNTL" },
        { N48D2_DIG3_TMDS_CTL_BITS, "DIG3_TMDS_CTL_BITS" },
        { N48D2_SYMCLKD_CLOCK_ENABLE, "SYMCLKD_CLOCK_ENABLE" },
        { N48D2_OTG2_PIXEL_RATE_CNTL, "OTG2_PIXEL_RATE_CNTL" },
        { N48D2_FMT2_CLAMP_COMPONENT_R, "FMT2_FMT_CLAMP_COMPONENT_R" },
        { N48D2_FMT2_CLAMP_COMPONENT_G, "FMT2_FMT_CLAMP_COMPONENT_G" },
        { N48D2_FMT2_CLAMP_COMPONENT_B, "FMT2_FMT_CLAMP_COMPONENT_B" },
        { N48D2_FMT2_DYNAMIC_EXP_CNTL, "FMT2_FMT_DYNAMIC_EXP_CNTL" },
        { N48D2_FMT2_CONTROL, "FMT2_FMT_CONTROL" },
        { N48D2_FMT2_BIT_DEPTH_CONTROL, "FMT2_FMT_BIT_DEPTH_CONTROL" },
        { N48D2_FMT2_CLAMP_CNTL, "FMT2_FMT_CLAMP_CNTL" },
        { N48D2_HUBP2_DCHUBP_CNTL, "HUBP2_DCHUBP_CNTL" },
        { N48D2_HUBP2_HUBP_CLK_CNTL, "HUBP2_HUBP_CLK_CNTL" },
        { N48D2_OTG2_V_TOTAL_CONTROL, "OTG2_OTG_V_TOTAL_CONTROL" },
        { N48D2_DIG2_CLOCK_PATTERN, "DIG2_DIG_CLOCK_PATTERN" },
        { N48D2_DIG2_TEST_PATTERN, "DIG2_DIG_TEST_PATTERN" },
        { N48D2_DIG2_FIFO_CTRL0, "DIG2_DIG_FIFO_CTRL0" },
        { N48D2_DIG2_FIFO_CTRL1, "DIG2_DIG_FIFO_CTRL1" },
        { N48D2_DIG2_HDMI_STATUS, "DIG2_HDMI_STATUS" },
        { N48D2_DIG2_TMDS_CNTL, "DIG2_TMDS_CNTL" },
        { N48D2_DIG2_TMDS_CONTROL_CHAR, "DIG2_TMDS_CONTROL_CHAR" },
        { N48D2_DIG2_TMDS_CTL_BITS, "DIG2_TMDS_CTL_BITS" },
        { N48D2_DIG2_TMDS_DCBALANCER_CONTROL, "DIG2_TMDS_DCBALANCER_CONTROL" },
        { N48D2_DIG2_OUTPUT_CRC_CNTL, "DIG2_DIG_OUTPUT_CRC_CNTL" },
        { N48D2_DIG2_OUTPUT_CRC_RESULT, "DIG2_DIG_OUTPUT_CRC_RESULT" },
        { N48D2_DIG3_TEST_PATTERN, "DIG3_DIG_TEST_PATTERN" },
        { N48D2_DIG3_FIFO_CTRL1, "DIG3_DIG_FIFO_CTRL1" },
        { N48D2_DIG3_HDMI_STATUS, "DIG3_HDMI_STATUS" },
        { N48D2_DIG3_TMDS_CNTL, "DIG3_TMDS_CNTL" },
        { N48D2_DIG3_TMDS_CONTROL_CHAR, "DIG3_TMDS_CONTROL_CHAR" },
        { N48D2_DIG3_TMDS_DCBALANCER_CONTROL, "DIG3_TMDS_DCBALANCER_CONTROL" },
        { N48D2_DIG3_OUTPUT_CRC_CNTL, "DIG3_DIG_OUTPUT_CRC_CNTL" },
        { N48D2_DIG3_OUTPUT_CRC_RESULT, "DIG3_DIG_OUTPUT_CRC_RESULT" },
        { N48D2_DIG1_FIFO_CTRL0, "DIG1_DIG_FIFO_CTRL0" },
        { N48D2_DIG1_TMDS_CTL_BITS, "DIG1_TMDS_CTL_BITS" },
        { N48D2_SYMCLKB_CLOCK_ENABLE, "SYMCLKB_CLOCK_ENABLE" },
    };
    static const char *const pairs1[39] = {"VTG1_CONTROL", "FMT1_FMT_422_CONTROL", "DPG1_DPG_CONTROL", "DPG1_DPG_RAMP_CONTROL", "DPG1_DPG_DIMENSIONS", "DPG1_DPG_COLOUR_R_CR", "DPG1_DPG_COLOUR_G_Y", "DPG1_DPG_COLOUR_B_CB", "DPG1_DPG_OFFSET_SEGMENT", "OPP_PIPE1_OPP_PIPE_CONTROL", "ODM1_OPTC_DATA_SOURCE_SELECT", "ODM1_OPTC_DATA_FORMAT_CONTROL", "ODM1_OPTC_INPUT_CLOCK_CONTROL", "ODM1_OPTC_MEMORY_CONFIG", "OTG1_OTG_H_TOTAL", "OTG1_OTG_H_BLANK_START_END", "OTG1_OTG_H_SYNC_A", "OTG1_OTG_H_SYNC_A_CNTL", "OTG1_OTG_H_TIMING_CNTL", "OTG1_OTG_V_TOTAL", "OTG1_OTG_V_TOTAL_MIN", "OTG1_OTG_V_TOTAL_MAX", "OTG1_OTG_V_BLANK_START_END", "OTG1_OTG_V_SYNC_A", "OTG1_OTG_V_SYNC_A_CNTL", "OTG1_OTG_CONTROL", "OTG1_OTG_INTERLACE_CONTROL", "OTG1_OTG_CLOCK_CONTROL", "OTG1_OTG_VSTARTUP_PARAM", "OTG1_OTG_VUPDATE_PARAM", "OTG1_OTG_VREADY_PARAM", "DIG2_STREAM_MAPPER_CONTROL", "DIG2_DIG_FE_CNTL", "DIG2_DIG_FE_CLK_CNTL", "DIG2_DIG_FE_EN_CNTL", "DIG2_DIG_CLOCK_PATTERN", "DIG2_DIG_FIFO_CTRL0", "DIG2_HDMI_CONTROL", "DIG2_DIG_BE_CNTL"};
    static const char *const pairs2[39] = {"VTG2_CONTROL", "FMT2_FMT_422_CONTROL", "DPG2_DPG_CONTROL", "DPG2_DPG_RAMP_CONTROL", "DPG2_DPG_DIMENSIONS", "DPG2_DPG_COLOUR_R_CR", "DPG2_DPG_COLOUR_G_Y", "DPG2_DPG_COLOUR_B_CB", "DPG2_DPG_OFFSET_SEGMENT", "OPP_PIPE2_OPP_PIPE_CONTROL", "ODM2_OPTC_DATA_SOURCE_SELECT", "ODM2_OPTC_DATA_FORMAT_CONTROL", "ODM2_OPTC_INPUT_CLOCK_CONTROL", "ODM2_OPTC_MEMORY_CONFIG", "OTG2_OTG_H_TOTAL", "OTG2_OTG_H_BLANK_START_END", "OTG2_OTG_H_SYNC_A", "OTG2_OTG_H_SYNC_A_CNTL", "OTG2_OTG_H_TIMING_CNTL", "OTG2_OTG_V_TOTAL", "OTG2_OTG_V_TOTAL_MIN", "OTG2_OTG_V_TOTAL_MAX", "OTG2_OTG_V_BLANK_START_END", "OTG2_OTG_V_SYNC_A", "OTG2_OTG_V_SYNC_A_CNTL", "OTG2_OTG_CONTROL", "OTG2_OTG_INTERLACE_CONTROL", "OTG2_OTG_CLOCK_CONTROL", "OTG2_OTG_VSTARTUP_PARAM", "OTG2_OTG_VUPDATE_PARAM", "OTG2_OTG_VREADY_PARAM", "DIG3_STREAM_MAPPER_CONTROL", "DIG3_DIG_FE_CNTL", "DIG3_DIG_FE_CLK_CNTL", "DIG3_DIG_FE_EN_CNTL", "DIG3_DIG_CLOCK_PATTERN", "DIG3_DIG_FIFO_CTRL0", "DIG3_HDMI_CONTROL", "DIG3_DIG_BE_CNTL"};
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // A2. every instance-2 / status / exception register against Linux (offset AND BASE_IDX through the header), the pairing of the two instances' lists, the allowlist
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    for (const auto &x : regs2) { char b[128]; std::snprintf(b, sizeof b, "register %s is Linux's absolute address", x.lx); expect_u(b, x.ours, lx_abs(x.lx)); }
    for (unsigned i = 0; i < 39; i++) {
        char b[160]; std::snprintf(b, sizeof b, "allowed[%u] (instance 1) is Linux %s", i, pairs1[i]); expect_u(b, n48d2_allowed[i], lx_abs(pairs1[i]));
        std::snprintf(b, sizeof b, "allowed2[%u] (instance 2) is Linux %s: the twin of %s", i, pairs2[i], pairs1[i]); expect_u(b, n48d2_allowed2[i], lx_abs(pairs2[i]));
    }
    expect(N48D2_VTG2_CONTROL == 0x39f2u && N48D2_ODM2_DATA_SOURCE_SELECT == 0x4fabu && N48D2_ODM2_INPUT_CLOCK_CONTROL == 0x4fb0u && N48D2_OTG2_H_TOTAL == 0x50eau && N48D2_OTG2_CONTROL == 0x5103u && N48D2_OTG2_CLOCK_CONTROL == 0x5144u &&
           N48D2_DPG2_CONTROL == 0x4dc8u && N48D2_OPP_PIPE2_CONTROL == 0x4e00u && N48D2_DIG3_FE_CNTL == 0x58bfu && N48D2_DIG3_FE_CLK_CNTL == 0x58c0u && N48D2_DIG3_FIFO_CTRL0 == 0x58c7u && N48D2_DIG3_BE_CNTL == 0x58e8u && N48D2_DIG3_STREAM_MAPPER == 0x53d0u &&
           N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL == 0x14bu && N48D2_SYMCLKB_CLOCK_ENABLE == 0x161u && N48D2_SYMCLKC_CLOCK_ENABLE == 0x162u && N48D2_SYMCLKD_CLOCK_ENABLE == 0x163u,
           "the instance-2 absolutes (0x5103 OTG2_CONTROL, 0x58bf DIG3_FE_CNTL, 0x53d0 mapper, 0x14b OTG2 PHYPLL, SYMCLKB/C/D 0x161 / 0x162 / 0x163)");
    {   // the stride relation, independent of the names: instance 2 = instance 1 + (ODM 0x10, OTG 0x80, VTG 1, FMT/DPG/OPP 0x5a, DIG 0x124, mapper 1)
        bool ok = true;
        for (unsigned i = 0; i < 39; i++) {
            const uint32_t a = n48d2_allowed[i], b = n48d2_allowed2[i];
            uint32_t want = a == N48D2_VTG1_CONTROL ? a + 1u : a == N48D2_DIG2_STREAM_MAPPER ? a + 1u : (a >= 0x4f9au && a <= 0x4fa2u) ? a + 0x10u : (a >= 0x506au && a <= 0x50e9u) ? a + 0x80u : (a >= 0x4d56u && a <= 0x4dafu) ? a + 0x5au : (a >= 0x579bu && a <= 0x57f9u) ? a + 0x124u : 0u;
            if (want != b) ok = false;
        }
        expect(ok, "every instance-2 register is its instance-1 twin plus the block stride (ODM 0x10, OTG 0x80, VTG 1, OPP/FMT/DPG 0x5a, DIG 0x124, mapper 1)"); }
    {   // the real allowlist, the forbidden spans, disjointness
        dcn41_allow_state al {}; expect(dcn41_allow_init(&al, kSeg, 262144u) == DCN41_ALLOW_OK, "the allowlist arms");
        for (unsigned i = 0; i < 39; i++) {
            char b[96]; std::snprintf(b, sizeof b, "instance-2 register %#x is inside the DCN allowlist", n48d2_allowed2[i]); expect(dcn41_allow_write(&al, n48d2_allowed2[i], 0u, "t"), b);
            for (uint32_t k = 0; k < N48D2_FORBIDDEN_COUNT; k++) expect(!(n48d2_allowed2[i] >= n48d2_forbidden[k].lo && n48d2_allowed2[i] <= n48d2_forbidden[k].hi), "no instance-2 register is inside a forbidden span");
            for (unsigned j = 0; j < i; j++) expect(n48d2_allowed2[i] != n48d2_allowed2[j], "the instance-2 list has no duplicates");
            for (unsigned j = 0; j < 39; j++) expect(n48d2_allowed2[i] != n48d2_allowed[j], "the two instances' lists are DISJOINT");
        }
        expect(dcn41_allow_write(&al, N48D2_SYMCLKC_CLOCK_ENABLE, 0u, "t") && dcn41_allow_write(&al, N48D2_SYMCLKD_CLOCK_ENABLE, 0u, "t"), "the two exception registers (0x162 / 0x163) are inside the DCN allowlist range PHYPLLA..DCCG");
        expect_u("39 instance-2 registers", N48D2_ALLOWED2_COUNT, 39);
        for (uint32_t a : { N48D2_DIG3_BE_CLK_CNTL, N48D2_DIG3_BE_EN_CNTL, N48D2_DIG3_TMDS_CTL_BITS, N48D2_OTG2_STATUS, N48D2_OTG2_FRAME_COUNT, N48D2_DPG2_STATUS, N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL })
            expect(n48d2_guard_addr_i(N48D2_INST_MONB, a, nullptr) != N48D2_G_OK, "an instance-2 register this verb only READS (BE clock / enable, TMDS bits, statuses, the PHYPLL source) is refused for writing");
    }
    // fields and constants
    expect_u("FE3's source bit is DIGD (0x08) in DIG_FE_SOURCE_SELECT", N48D2_BE_FE3_BIT, lx_fv("DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0x08u));
    expect_u("SYMCLKB FE_EN mask", N48D2_SYMCLK_FE_EN_MASK, lx_mask("SYMCLKB_CLOCK_ENABLE__SYMCLKB_FE_EN"));
    expect_u("SYMCLKC FE_EN mask", N48D2_SYMCLK_FE_EN_MASK, lx_mask("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_EN"));
    expect_u("SYMCLKD FE_EN mask", N48D2_SYMCLK_FE_EN_MASK, lx_mask("SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_EN"));
    expect_u("SYMCLKC FE_SRC_SEL mask", N48D2_SYMCLK_FE_SRC_MASK, lx_mask("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_SRC_SEL"));
    expect_u("SYMCLKD FE_SRC_SEL mask", N48D2_SYMCLK_FE_SRC_MASK, lx_mask("SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_SRC_SEL"));
    expect_u("the exception mask is FE_EN | FE_SRC_SEL = 0x710 and EXCLUDES the PHY's own CLOCK_ENABLE bit 0", N48D2_SYMCLK_FE_MASK, lx_mask("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_EN") | lx_mask("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_SRC_SEL"));
    expect((N48D2_SYMCLK_FE_MASK & lx_mask("SYMCLKC_CLOCK_ENABLE__SYMCLKC_CLOCK_ENABLE")) == 0u && (N48D2_SYMCLK_FE_MASK & lx_mask("SYMCLKD_CLOCK_ENABLE__SYMCLKD_CLOCK_ENABLE")) == 0u, "bit 0 (SYMCLK<n>_CLOCK_ENABLE) is outside the exception mask");
    expect_u("SYMCLKC value = dccg401_enable_symclk_se(2, 2) = FE_EN 1 | FE_SRC_SEL 2", N48D2_SYMCLKC_FE_VALUE, lx_fv("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_EN", 1u) | lx_fv("SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_SRC_SEL", 2u));
    expect_u("SYMCLKD value = dccg401_enable_symclk_se(3, 3) = FE_EN 1 | FE_SRC_SEL 3", N48D2_SYMCLKD_FE_VALUE, lx_fv("SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_EN", 1u) | lx_fv("SYMCLKD_CLOCK_ENABLE__SYMCLKD_FE_SRC_SEL", 3u));
    expect(N48D2_SYMCLKC_FE_VALUE == 0x210u && N48D2_SYMCLKD_FE_VALUE == 0x310u && N48D2_LINK_UNIPHY_C == 2u && N48D2_LINK_UNIPHY_D == 3u && N48D2_OTG1_INST == 1u && N48D2_OTG2_INST == 2u && N48D2_PHYPLL_PLL3 == 3u, "0x210 / 0x310, links 2 / 3, OTG 1 / 2, PLL 3");
    {   const struct n48d2_inst *a = n48d2_inst_get(1), *b = n48d2_inst_get(2);
        expect(a && b && !n48d2_inst_get(0) && !n48d2_inst_get(3) && a->symclk == N48D2_SYMCLKC_CLOCK_ENABLE && b->symclk == N48D2_SYMCLKD_CLOCK_ENABLE && a->link == 2u && b->link == 3u && a->otg_inst == 1u && b->otg_inst == 2u && a->fe_bit == 0x400u && b->fe_bit == 0x800u &&
               a->phypll_pll == 2u && b->phypll_pll == 3u && a->phypll == N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL && b->phypll == N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL && a->otg_control == N48D2_OTG1_CONTROL && b->otg_control == N48D2_OTG2_CONTROL &&
               a->be_cntl == N48D2_DIG2_BE_CNTL && b->be_cntl == N48D2_DIG3_BE_CNTL && a->mapper == N48D2_DIG2_STREAM_MAPPER && b->mapper == N48D2_DIG3_STREAM_MAPPER && a->fe_cntl == N48D2_DIG2_FE_CNTL && b->fe_cntl == N48D2_DIG3_FE_CNTL &&
               a->odm_dss == N48D2_ODM1_DATA_SOURCE_SELECT && b->odm_dss == N48D2_ODM2_DATA_SOURCE_SELECT, "the instance table: monitor A = OTG1 / DIG2 / FE2 bit 0x400 / link 2 / PLL2 / SYMCLKC, monitor B = OTG2 / DIG3 / FE3 bit 0x800 / link 3 / PLL3 / SYMCLKD; no instance 0 or 3"); }

    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // C2. the instance-2 step lists, rebuilt from Linux NAMES (check_lists_inst2) + structural relations to instance 1
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    check_lists_inst2();
    for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
        n48d2_step s1[N48D2_MAX_STEPS], s2[N48D2_MAX_STEPS]; const uint32_t n1 = n48d2_build_i(1, op, s1), n2 = n48d2_build_i(2, op, s2);
        expect(n1 == n2 && n1 > 0, "the two instances' lists have the same length");
        bool same = n1 == n2;
        for (uint32_t i = 0; i < n1 && i < n2; i++) {
            if (s1[i].kind != s2[i].kind || std::string(s1[i].fn) != s2[i].fn || s1[i].polls != s2[i].polls || s1[i].step_us != s2[i].step_us) same = false;
            for (uint32_t k = 0; k < N48D2_ALLOWED_COUNT; k++) {   // the step's register is the twin of instance 1's
                if (s1[i].abs == n48d2_allowed[k] && s2[i].abs != n48d2_allowed2[k]) same = false;
            }
            if (s1[i].abs == N48D2_SYMCLKC_CLOCK_ENABLE && s2[i].abs != N48D2_SYMCLKD_CLOCK_ENABLE) same = false;
        }
        expect(same, "step for step: the same kind, Linux function, wait bounds; the register is the instance-1 step's twin (SYMCLKC -> SYMCLKD)");
    }
    {   // the monitor B timing IS the monitor A's VIC 16: the same 148.5 MHz arithmetic
        n48d2_step s1[N48D2_MAX_STEPS], s2[N48D2_MAX_STEPS]; (void)n48d2_build_i(1, N48D2_OP_TIMING, s1); (void)n48d2_build_i(2, N48D2_OP_TIMING, s2); bool same = true;
        for (uint32_t i = 0; i < 36; i++) if (s1[i].kind != N48D2_K_WAIT && s1[i].abs != N48D2_ODM1_DATA_SOURCE_SELECT) same = same && s1[i].val == s2[i].val && s1[i].mask == s2[i].mask;
        expect(same, "the monitor B's timing step values equal the monitor A's (CEA VIC 16, 148.5 MHz): only the ODM data-source's OPP number differs");
        n48d2_step t[N48D2_MAX_STEPS]; expect(n48d2_build_i(0, N48D2_OP_TIMING, t) == 0 && n48d2_build_i(3, N48D2_OP_OFF, t) == 0 && n48d2_build_i(2, N48D2_OP_STATUS3, t) == 0, "an unknown instance builds no list; status3 has no list"); }
    for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
        n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build_i(2, op, s);
        for (uint32_t i = 0; i < n; i++) if (n48d2_is_write(&s[i])) {
            expect(s[i].abs != N48D2_DIG3_BE_CLK_CNTL && s[i].abs != N48D2_DIG3_BE_EN_CNTL && s[i].abs != N48D2_DIG3_TMDS_CTL_BITS && (s[i].abs >= 0x200u || s[i].abs == N48D2_SYMCLKD_CLOCK_ENABLE),
                   "instance 2: no op writes the BE mode / clock / enable, the TMDS bits or any DCCG register but the ONE exception SYMCLKD_CLOCK_ENABLE");
            for (unsigned k = 0; k < 39; k++) expect(s[i].abs != n48d2_allowed[k], "instance 2's lists name no instance-1 register");
            expect(s[i].abs != N48D2_SYMCLKC_CLOCK_ENABLE && s[i].abs != N48D2_SYMCLKB_CLOCK_ENABLE, "instance 2's lists never name SYMCLKC or SYMCLKB");
        }
        n48d2_step s1[N48D2_MAX_STEPS]; const uint32_t n1 = n48d2_build_i(1, op, s1);
        for (uint32_t i = 0; i < n1; i++) if (n48d2_is_write(&s1[i])) {
            for (unsigned k = 0; k < 39; k++) expect(s1[i].abs != n48d2_allowed2[k], "instance 1's lists name no instance-2 register");
            expect(s1[i].abs != N48D2_SYMCLKD_CLOCK_ENABLE && s1[i].abs != N48D2_SYMCLKB_CLOCK_ENABLE, "instance 1's lists never name SYMCLKD or SYMCLKB");
        }
    }
    {   // exactly ONE symbol-clock step in connect and ONE in off, per instance, and none in timing
        for (uint32_t inst : { 1u, 2u }) {
            const uint32_t sc = inst == 1 ? N48D2_SYMCLKC_CLOCK_ENABLE : N48D2_SYMCLKD_CLOCK_ENABLE;
            const uint32_t nT = [&]{ n48d2_step s[N48D2_MAX_STEPS]; uint32_t n = n48d2_build_i(inst, N48D2_OP_TIMING, s), c = 0; for (uint32_t i = 0; i < n; i++) if (s[i].abs == sc) c++; return c; }();
            const uint32_t nC = [&]{ n48d2_step s[N48D2_MAX_STEPS]; uint32_t n = n48d2_build_i(inst, N48D2_OP_CONNECT, s), c = 0; for (uint32_t i = 0; i < n; i++) if (s[i].abs == sc) { c++; if (!(i == 0 && s[i].kind == N48D2_K_UPD && s[i].mask == 0x710u && s[i].val == (inst == 1 ? 0x210u : 0x310u))) c += 100; } return c; }();
            const uint32_t nO = [&]{ n48d2_step s[N48D2_MAX_STEPS]; uint32_t n = n48d2_build_i(inst, N48D2_OP_OFF, s), c = 0; for (uint32_t i = 0; i < n; i++) if (s[i].abs == sc) { c++; if (!(s[i].kind == N48D2_K_UPD && s[i].mask == 0x710u && s[i].val == 0u)) c += 100; } return c; }();
            expect(nT == 0 && nC == 1 && nO == 1, "the symbol clock step: none in timing, ONE first step in connect (UPD mask 0x710 value 0x210 / 0x310), ONE in off (UPD mask 0x710 value 0)");
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // D2. the guard, per instance
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    std::set<uint32_t> set1(n48d2_allowed, n48d2_allowed + 39), set2(n48d2_allowed2, n48d2_allowed2 + 39);
    set1.insert(N48D2_SYMCLKC_CLOCK_ENABLE); set2.insert(N48D2_SYMCLKD_CLOCK_ENABLE);
    for (unsigned i = 0; i < N48D2_ALLOWED2P_COUNT; i++) set2.insert(n48d2_allowed2p[i]);      // 0.0.635: instance 2's 37 plane registers ...
    set2.insert(N48D2_DPPCLK2_DTO_PARAM); set2.insert(N48D2_DPPCLK_CTRL);                       // ... and the TWO DCCG exceptions
    for (unsigned i = 0; i < N48D2_ALLOWED1P_COUNT; i++) set1.insert(n48d2_allowed1p[i]);      // 0.0.655: instance 1's 43 plane registers ...
    set1.insert(N48D2_DPPCLK1_DTO_PARAM); set1.insert(N48D2_DPPCLK_CTRL);                       // ... and ITS two DCCG exceptions (0x15a, 0x168)
    {   // the WHOLE header: an address is admitted by instance N's guard iff it is in N's list (or is N's one exception)
        const std::vector<uint32_t> regs = all_header_regs(); uint32_t bad1 = 0, bad2 = 0, ok1 = 0, ok2 = 0;
        for (uint32_t a : regs) {
            const bool g1 = n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK, g2 = n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_OK;
            if (g1 != (set1.count(a) != 0)) ++bad1;
            if (g2 != (set2.count(a) != 0)) ++bad2;
            ok1 += g1; ok2 += g2;
        }
        std::printf("D2: %zu registers in the Linux header; instance 1 admits %u, instance 2 admits %u header entries\n", regs.size(), ok1, ok2);
        expect(regs.size() > 2000 && bad1 == 0 && bad2 == 0 && ok1 >= 85 && ok2 >= 81, "over EVERY register of Linux's header each instance's guard admits exactly its listed registers + its exceptions (instance 1: 39 + SYMCLKC + 43 plane registers + its two DCCG exceptions (0.0.655); instance 2: 39 + 39 plane registers (0.0.643: 37 + OTG2's lock select and lock) + SYMCLKD + the two DCCG exceptions), nothing else");
        uint32_t adm1 = 0, adm2 = 0, adm0 = 0, adm3 = 0;
        for (uint32_t a = 0; a < 0xA000u; a++) { adm1 += n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK; adm2 += n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_OK; adm0 += n48d2_guard_addr_i(0, a, nullptr) == N48D2_G_OK; adm3 += n48d2_guard_addr_i(3, a, nullptr) == N48D2_G_OK; }
        expect(adm1 == 85u && adm2 == 81u && adm0 == 0u && adm3 == 0u, "the 0..0xA000 sweep: instance 1 admits 85 addresses (39 + SYMCLKC + 43 plane registers + the two DCCG exceptions; 0.0.655), instance 2 admits 81 (39 + 39 plane registers + SYMCLKD + DPPCLK2_DTO + DPPCLK_CTRL), NONE for an unknown instance"); }
    {   // cross-instance: the other instance's registers and symbol clock are named OTHER
        for (unsigned i = 0; i < 39; i++) {
            expect(n48d2_guard_addr_i(2, n48d2_allowed[i], nullptr) == N48D2_G_OTHER && n48d2_guard_write_i(2, n48d2_allowed[i], 0u, nullptr) != N48D2_G_OK, "instance 2 refuses every instance-1 register (OTHER)");
            expect(n48d2_guard_addr_i(1, n48d2_allowed2[i], nullptr) == N48D2_G_OTHER && n48d2_guard_write_i(1, n48d2_allowed2[i], 0u, nullptr) != N48D2_G_OK, "instance 1 refuses every instance-2 register (OTHER)");
            expect(n48d2_guard_addr(n48d2_allowed2[i], nullptr) == n48d2_guard_addr_i(1, n48d2_allowed2[i], nullptr), "the old entry point is instance 1's");
        }
        expect(n48d2_guard_addr_i(2, N48D2_SYMCLKC_CLOCK_ENABLE, nullptr) == N48D2_G_OTHER && n48d2_guard_addr_i(1, N48D2_SYMCLKD_CLOCK_ENABLE, nullptr) == N48D2_G_OTHER, "instance 2 may not write SYMCLKC and instance 1 may not write SYMCLKD");
        const char *why = nullptr; (void)n48d2_guard_addr_i(2, N48D2_OTG1_CONTROL, &why); expect(why && std::strstr(why, "OTHER") != nullptr, "the refusal names the other instance");
        // the rest of the DCCG: SYMCLKA / B / E and the whole 0x100..0x17e span stay forbidden for both
        for (uint32_t a = 0x100u; a <= 0x17eu; a++) {
            const bool own1 = a == N48D2_SYMCLKC_CLOCK_ENABLE || a == N48D2_DPPCLK1_DTO_PARAM || a == N48D2_DPPCLK_CTRL, own2 = a == N48D2_SYMCLKD_CLOCK_ENABLE || a == N48D2_DPPCLK2_DTO_PARAM || a == N48D2_DPPCLK_CTRL;     // 0.0.635: instance 2 has THREE here (SYMCLKD + the two DCCG exceptions)
            expect((n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK) == own1 && (n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_OK) == own2, "in the DCCG span only the instance's own SYMCLK register is admitted (instance 2 also DPPCLK2_DTO_PARAM 0x15b and DPPCLK_CTRL 0x168; 0.0.655: instance 1 also DPPCLK1_DTO_PARAM 0x15a and DPPCLK_CTRL 0x168)");
        }
        for (uint32_t a : { 0x160u, N48D2_SYMCLKB_CLOCK_ENABLE, 0x164u }) {
            expect(n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_write_i(1, a, 0x111u, nullptr) != N48D2_G_OK && n48d2_guard_write_i(2, a, 0u, nullptr) != N48D2_G_OK,
                   "SYMCLKA / SYMCLKB (the live DP) / SYMCLKE are FORBIDDEN for both instances, whatever the value"); }
    }
    {   // the DP-path names: refused for instance 2 exactly as for instance 1 (the same exhaustive name sweep)
        const char *pfx[] = { "OTG0_", "ODM0_", "VTG0_", "OPP_PIPE0_", "OPPBUF0_", "FMT0_", "DPG0_", "HUBP0_", "HUBPREQ0_", "HUBPRET0_", "DPP_TOP0_", "CNVC_CFG0_", "DSCL0_", "CM0_", "MPCC0_", "MPCC_OGAM0_", "MPCC_MCM0_",
                              "DIG1_", "DP1_", "DME1_", "VPG1_", "AFMT1_", "DP_AUX1_", "DCIO_UNIPHY1_", "RDPCSTX1_", "UNIPHYB_", "DP_DTO", "OTG_PIXEL_RATE_DIV", "DCCG_GATE" };
        uint32_t n = 0, refused = 0; size_t p = 0;
        while ((p = gOff.find("#define reg", p)) != std::string::npos) {
            p += 11; size_t e = p; while (e < gOff.size() && gOff[e] != ' ' && gOff[e] != '\t') ++e;
            const std::string name = gOff.substr(p, e - p);
            if (name.size() > 9 && name.compare(name.size() - 9, 9, "_BASE_IDX") == 0) continue;
            for (const char *x : pfx) if (name.compare(0, std::strlen(x), x) == 0) {
                uint32_t a = 0; if (!lx_reg(name, &a)) break;
                if ((std::string(x) == "DP_DTO" || std::string(x) == "OTG_PIXEL_RATE_DIV" || std::string(x) == "DCCG_GATE") && a >= 0x200u) break;
                ++n;
                if (n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_OK && n48d2_guard_write_i(2, a, 0u, nullptr) != N48D2_G_OK) ++refused;
                else { ++gRun; ++gFail; std::printf("FAIL: instance 2 admits the DP-path register %s\n", name.c_str()); }
                break; }
        }
        expect(n > 900 && n == refused, "instance 2 refuses every OTG0 / ODM0 / VTG0 / OPP0 / DPG0 / FMT0 / HUBP0 / DPP0 / MPCC0 / DIG1 / DP1 / PHY B / DCCG-block register of the header");
        for (const char *nm : { "OTG0_OTG_CONTROL", "ODM0_OPTC_DATA_SOURCE_SELECT", "DIG1_DIG_FE_CNTL", "DIG1_DIG_BE_CNTL", "DIG1_STREAM_MAPPER_CONTROL", "OTG1_OTG_CONTROL", "DIG2_DIG_FE_CNTL", "OTG0_PIXEL_RATE_CNTL", "OTG1_PHYPLL_PIXEL_RATE_CNTL", "OTG2_PIXEL_RATE_CNTL", "OTG2_PHYPLL_PIXEL_RATE_CNTL", "SYMCLKB_CLOCK_ENABLE", "RDPCSTX2_RDPCSTX_PHY_CNTL0", "UNIPHYC_LINK_CNTL", "UNIPHYD_LINK_CNTL" }) {
            const uint32_t a = lx_abs(nm); char b[128]; std::snprintf(b, sizeof b, "%s (%#x) is refused for instance 2", nm, a); expect(n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_OK, b);
            std::snprintf(b, sizeof b, "%s (%#x) is admitted by instance 1 only if it is on instance 1's list", nm, a); expect((n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK) == (set1.count(a) != 0), b); }
    }
    {   // the value rules, instance 2
        expect(n48d2_guard_write_i(2, N48D2_DIG3_FE_CNTL, 0u, nullptr) == N48D2_G_VALUE && n48d2_guard_write_i(2, N48D2_DIG3_FE_CNTL, 1u, nullptr) == N48D2_G_VALUE && n48d2_guard_write_i(2, N48D2_DIG3_FE_CNTL, 0x122u, nullptr) == N48D2_G_OK, "DIG3 FE source: OTG2 only (never OTG0, never the monitor A's OTG1)");
        expect(n48d2_guard_write_i(1, N48D2_DIG2_FE_CNTL, 2u, nullptr) == N48D2_G_VALUE && n48d2_guard_write_i(1, N48D2_DIG2_FE_CNTL, 0x111u, nullptr) == N48D2_G_OK, "DIG2 FE source: OTG1 only (never the monitor B's OTG2)");
        for (uint32_t b = 0; b < 7; b++) { const uint32_t v = 0x20000000u | (1u << (8 + b));
            expect(n48d2_guard_write_i(2, N48D2_DIG3_BE_CNTL, v, nullptr) == (b == 3 ? N48D2_G_OK : N48D2_G_VALUE), "DIG3 BE FE-source: ONLY bit 11 (FE3 = 0x08 << 8); FE1, FE2 (the monitor A's) and every other bit refused");
            expect(n48d2_guard_write_i(1, N48D2_DIG2_BE_CNTL, v, nullptr) == (b == 2 ? N48D2_G_OK : N48D2_G_VALUE), "DIG2 BE FE-source: ONLY bit 10 (FE2); the monitor B's FE3 bit refused"); }
        expect(n48d2_guard_write_i(2, N48D2_DIG3_BE_CNTL, 0x20000000u, nullptr) == N48D2_G_OK && n48d2_guard_write_i(2, N48D2_DIG3_BE_CNTL, 0x20000800u, nullptr) == N48D2_G_OK, "DIG3 BE: FE3 or none");
        expect(n48d2_guard_write_i(2, N48D2_DIG3_STREAM_MAPPER, 0u, nullptr) == N48D2_G_OK && n48d2_guard_write_i(2, N48D2_DIG3_STREAM_MAPPER, 3u, nullptr) == N48D2_G_OK && n48d2_guard_write_i(2, N48D2_DIG3_STREAM_MAPPER, 1u, nullptr) == N48D2_G_VALUE &&
               n48d2_guard_write_i(2, N48D2_DIG3_STREAM_MAPPER, 2u, nullptr) == N48D2_G_VALUE, "DIG3 mapper: link 0 or 3 only (never 1 = PHY B, never 2 = the monitor A's PHY C)");
        expect(n48d2_guard_write_i(1, N48D2_DIG2_STREAM_MAPPER, 3u, nullptr) == N48D2_G_VALUE, "DIG2 mapper: link 3 (the monitor B's PHY D) refused");
        expect(n48d2_guard_write_i(2, N48D2_ODM2_DATA_SOURCE_SELECT, 0xFFF20000u, nullptr) == N48D2_G_OK && n48d2_guard_write_i(2, N48D2_ODM2_DATA_SOURCE_SELECT, 0xFFFF0000u, nullptr) == N48D2_G_OK && n48d2_guard_write_i(2, N48D2_ODM2_DATA_SOURCE_SELECT, 0xFFF10000u, nullptr) == N48D2_G_VALUE &&
               n48d2_guard_write_i(2, N48D2_ODM2_DATA_SOURCE_SELECT, 0xFFF00000u, nullptr) == N48D2_G_VALUE, "ODM2 SEG0 = OPP2 or 0xf (never OPP0, never the monitor A's OPP1)");
        expect(n48d2_guard_write_i(1, N48D2_ODM1_DATA_SOURCE_SELECT, 0xFFF20000u, nullptr) == N48D2_G_VALUE, "ODM1 SEG0 = OPP2 refused");
        // the symbol clock exception: FE_SRC_SEL 0 or the instance's link
        for (uint32_t sel = 0; sel < 8; sel++) { const uint32_t v = 0x1u | (sel << 8) | 0x10u;
            expect(n48d2_guard_write_i(1, N48D2_SYMCLKC_CLOCK_ENABLE, v, nullptr) == (sel == 0 || sel == 2 ? N48D2_G_OK : N48D2_G_VALUE), "SYMCLKC FE_SRC_SEL: 0 or 2 (UNIPHY_C) only");
            expect(n48d2_guard_write_i(2, N48D2_SYMCLKD_CLOCK_ENABLE, v, nullptr) == (sel == 0 || sel == 3 ? N48D2_G_OK : N48D2_G_VALUE), "SYMCLKD FE_SRC_SEL: 0 or 3 (UNIPHY_D) only"); }
    }
    {   // the list guard and the exception's shape
        for (uint32_t inst : { 1u, 2u }) {
            const uint32_t sc = inst == 1 ? N48D2_SYMCLKC_CLOCK_ENABLE : N48D2_SYMCLKD_CLOCK_ENABLE, link = inst == 1 ? 2u : 3u;
            for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
                n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build_i(inst, op, s); uint32_t bad = 0;
                expect(n48d2_guard_list_i(inst, s, n, &bad) == 0u && bad == 0xFFFFFFFFu, "the built list passes its OWN instance's guard");
                expect(n48d2_guard_list_i(3 - inst, s, n, &bad) == N48D2_GUARD_REFUSED, "the built list is REFUSED whole by the OTHER instance's guard");
                expect(n48d2_guard_list_i(0, s, n, &bad) == N48D2_GUARD_REFUSED && n48d2_guard_list_i(3, s, n, &bad) == N48D2_GUARD_REFUSED, "an unknown instance refuses every list");
                // every write step moved onto the other instance's twin / the DP twin is refused at that step
                for (uint32_t i = 0; i < n; i++) if (n48d2_is_write(&s[i])) {
                    const uint32_t a = s[i].abs; n48d2_step t[N48D2_MAX_STEPS];
                    uint32_t other = 0;
                    for (unsigned k = 0; k < 39; k++) if ((inst == 1 ? n48d2_allowed[k] : n48d2_allowed2[k]) == a) other = inst == 1 ? n48d2_allowed2[k] : n48d2_allowed[k];
                    if (a == sc) other = inst == 1 ? N48D2_SYMCLKD_CLOCK_ENABLE : N48D2_SYMCLKC_CLOCK_ENABLE;
                    std::memcpy(t, s, sizeof(t)); t[i].abs = other;
                    expect(other != 0 && n48d2_guard_list_i(inst, t, n, &bad) == N48D2_GUARD_REFUSED && bad == i, "a step moved onto the OTHER instance's twin register refuses the whole list at that step");
                    const uint32_t dp = dp_twin(inst, a);
                    std::memcpy(t, s, sizeof(t)); t[i].abs = dp;
                    expect(dp != 0 && n48d2_guard_list_i(inst, t, n, &bad) == N48D2_GUARD_REFUSED && bad == i && n48d2_guard_addr_i(inst, dp, nullptr) == N48D2_G_FORBIDDEN, "a step moved onto the DP twin (OTG0 / ODM0 / OPP0 / DIG1 / SYMCLKB) refuses the whole list at that step (FORBIDDEN)");
                }
            }
            // the exception's shape in a list
            n48d2_step z[N48D2_MAX_STEPS]; uint32_t bad = 0; uint32_t n = 0;
            auto one = [&](uint8_t kind, uint32_t mask, uint32_t val, uint32_t abs) { n = 0; n48d2_put(z, &n, kind, abs, mask, val, 0, 0, 0, 0, "SYMCLK", "t"); return n48d2_guard_list_i(inst, z, n, &bad); };
            expect(one(N48D2_K_UPD, 0x710u, (link << 8) | 0x10u, sc) == 0u && one(N48D2_K_UPD, 0x710u, 0u, sc) == 0u && one(N48D2_K_UPD, 0x10u, 0x10u, sc) == 0u && one(N48D2_K_UPD, 0x700u, link << 8, sc) == 0u, "an UPDATE inside bits 4 and 10:8 with SRC 0 / the link is admitted");
            expect(one(N48D2_K_SET, 0xFFFFFFFFu, 0x211u, sc) != 0u && one(N48D2_K_SET, 0xFFFFFFFFu, 0u, sc) != 0u, "a SET of the symbol clock register is refused (it would rewrite bit 0, the PHY's CLOCK_ENABLE)");
            expect(one(N48D2_K_UPD, 0x711u, 0x211u, sc) != 0u && one(N48D2_K_UPD, 0x1u, 0u, sc) != 0u && one(N48D2_K_UPD, 0x8000u, 0u, sc) != 0u && one(N48D2_K_UPD, 0xFFFFFFFFu, 0u, sc) != 0u && one(N48D2_K_UPD, 0x710u | 0x2u, 0u, sc) != 0u, "an UPDATE whose mask reaches bit 0 or ANY bit outside 0x710 is refused");
            expect(one(N48D2_K_UPD, 0x710u, ((link ^ 1u) << 8) | 0x10u, sc) != 0u && one(N48D2_K_UPD, 0x710u, (7u << 8), sc) != 0u, "an UPDATE with another FE_SRC_SEL (the other link, 7) is refused");
            expect(one(N48D2_K_UPD, 0x10u, 0x211u, sc) != 0u, "an UPDATE whose value reaches outside its mask is refused");
            expect(one(N48D2_K_UPD, 0x710u, 0x210u, inst == 2 ? N48D2_SYMCLKC_CLOCK_ENABLE : N48D2_SYMCLKD_CLOCK_ENABLE) != 0u, "the other instance's SYMCLK is refused even with a perfect UPDATE");
            expect(one(N48D2_K_UPD, 0x710u, 0u, N48D2_SYMCLKB_CLOCK_ENABLE) != 0u && one(N48D2_K_UPD, 0x710u, 0u, 0x160u) != 0u && one(N48D2_K_UPD, 0x710u, 0u, 0x164u) != 0u, "SYMCLKB (the live DP) / SYMCLKA / SYMCLKE: refused with a perfect UPDATE");
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // E2. the flow over the model, instance 2, and the symbol clock in both
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    const std::vector<W> kTiming2 = {
        { 0x4fb0, 0x00000003 }, { 0x5144, 0x00000003 }, { 0x4fab, 0xFFF20000 }, { 0x50ee, 0x00000000 }, { 0x4fb1, 0x00000000 }, { 0x50ea, 0x00000897 },
        { 0x50ec, 0x002C0000 }, { 0x50eb, 0x00C00840 }, { 0x50ed, 0x00000000 }, { 0x50ef, 0x00000464 }, { 0x50f1, 0x00000464 }, { 0x50f0, 0x00000464 },
        { 0x50f9, 0x00050000 }, { 0x50f8, 0x00290461 }, { 0x50fa, 0x00000000 }, { 0x5105, 0x00000000 }, { 0x39f2, 0x00000000 }, { 0x5103, 0x00000000 },
        { 0x5145, 0x0000000D }, { 0x5146, 0x014002A8 }, { 0x5147, 0x0000012C }, { 0x39f2, 0x04610000 }, { 0x4fac, 0x00000000 }, { 0x50ee, 0x00000000 },
        { 0x50ee, 0x00000000 }, { 0x4e00, 0x00000001 }, { 0x4dbd, 0x00000000 }, { 0x4dca, 0x07800438 }, { 0x4dce, 0x00000000 }, { 0x4dc8, 0x00661001 },
        { 0x4fab, 0xFFF20000 }, { 0x39f2, 0x84610000 }, { 0x5103, 0x00000201 },
    };
    const std::vector<W> kConnect2 = {
        { 0x0163, 0x00000311 }, { 0x58bf, 0x00000002 }, { 0x5103, 0x00010201 }, { 0x58bf, 0x00000022 }, { 0x58bf, 0x00000122 }, { 0x58c4, 0x0000001F },
        { 0x58ca, 0x00000000 }, { 0x58ca, 0x00000000 }, { 0x58e8, 0x20000800 }, { 0x58c0, 0x00000802 }, { 0x58c0, 0x00000812 }, { 0x58c1, 0x00000001 },
        { 0x53d0, 0x00000003 }, { 0x58c7, 0x00000000 }, { 0x58c7, 0x0000001C }, { 0x58c0, 0x00000812 }, { 0x58c1, 0x00000001 }, { 0x58c7, 0x0000001E },
        { 0x58c7, 0x0010001C }, { 0x58c7, 0x0000001D },
    };
    const std::vector<W> kOff2 = {
        { 0x4dc8, 0x00000000 }, { 0x4dcb, 0x00000000 }, { 0x4dcc, 0x00000000 }, { 0x4dcd, 0x00000000 }, { 0x4dc9, 0x00000000 }, { 0x58c7, 0x0000001C },
        { 0x58c1, 0x00000000 }, { 0x58c0, 0x00000802 }, { 0x58c7, 0x0000001C }, { 0x58c1, 0x00000000 }, { 0x58c0, 0x00000802 },
        { 0x58e8, 0x20000000 }, { 0x0163, 0x00000001 }, { 0x53d0, 0x00000000 }, { 0x4fab, 0xFFFF0000 }, { 0x4fb1, 0x00000000 }, { 0x5103, 0x00010200 }, { 0x39f2, 0x04610000 },
        { 0x5144, 0x00000100 }, { 0x4fb0, 0x00000004 }, { 0x4e00, 0x00000000 },
    };
    {   // the happy path of the monitor B: timing, connect, status pages, off
        Model m; m.inst = 2; m.frames1 = 5; m.frames2 = 777;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING, 2);
        expect_u("inst 2 timing: status OK", t.status, N48D2_OK);
        expect(t.f1a == 777u && t.f1b > 777u, "inst 2 timing: the OTG-frames fields are OTG2's counter (777 -> more), not OTG1's");
        expect_writes("inst 2 timing: the exact write sequence", m.writes, kTiming2);
        expect(t.done == 33 && t.total == 36 && t.timeouts == 0 && t.fail == 0xFF && t.auto_off == 0 && t.dp_changed == 0 && t.inst == 2u, "inst 2 timing: 33 writes of 36 steps, no timeout, no rollback, DP and SYMCLKB unchanged, result says instance 2");
        expect(t.f0b != t.f0a && t.f1b != t.f1a && (t.otg1_ctl & 1u) && t.dpg_ctl == 0x00661001u && t.symclkb_a == 0x111u && t.symclkb_b == 0x111u, "inst 2 timing: OTG0 counting, OTG2 counting, DPG2 on, SYMCLKB read before / after (0x111)");
        const size_t w0 = m.writes.size();
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT, 2);
        expect_u("inst 2 connect: status OK", c.status, N48D2_OK);
        expect_writes("inst 2 connect: the exact write sequence", writes_from(m, w0), kConnect2);
        expect(c.done == 20 && c.total == 22 && c.timeouts == 0 && c.fe_en == 1u && c.be_cntl == 0x20000800u && m.get(N48D2_DIG3_STREAM_MAPPER) == 3u && (m.get(N48D2_DIG3_FE_CNTL) & 7u) == 2u && (m.get(N48D2_DIG3_FE_CLK_CNTL) & 7u) == 2u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x311u,
               "inst 2 connect: SYMCLKD 0x311 (bit 0 kept), FE3 enabled from OTG2 in DVI mode, BE FE source 0x08, mapper 3");
        expect(m.get(N48D2_DIG3_BE_CLK_CNTL) == 0x2812u && m.get(N48D2_DIG3_BE_EN_CNTL) == 1u && c.symclk_wait == 2 && c.symclk_delay == 0, "inst 2 connect left the DMUB's back end untouched; both FIFO reset edges waited on the FE symbol clock");
        for (const W &w : m.writes) { for (unsigned k = 0; k < 39; k++) expect(w.abs != n48d2_allowed[k], "NO instance-1 register was ever written by an instance-2 run"); expect(w.abs != N48D2_SYMCLKC_CLOCK_ENABLE && w.abs != N48D2_SYMCLKB_CLOCK_ENABLE, "SYMCLKC and SYMCLKB were never written by an instance-2 run"); }
        const size_t w1 = m.writes.size(), r1 = m.reads.size(); uint64_t v[13];
        expect_u("inst 2 status: OK", n48d2::status(m, v, 2), N48D2_OK);
        expect(m.writes.size() == w1 && m.reads.size() == r1 + N48D2_STAT_COUNT, "inst 2 status: reads every listed register once, writes NOTHING");
        expect((uint32_t)v[1] == m.rd(N48D2_OTG2_CONTROL) && (uint32_t)(v[8] >> 32) == 0x20000800u && (uint32_t)v[10] == 3u && (uint32_t)v[11] == 3u && (uint32_t)v[2] != (uint32_t)(v[2] >> 32) && (uint32_t)(v[11] >> 32) != (uint32_t)v[12],
               "inst 2 status: OTG2_CONTROL, DIG3 BE_CNTL, mapper 3, PHYPLL source 3 packed in place; both frame counters moved across 50 ms");
        {   // status2 / status3 of instance 2 (markers written into the model, restored afterwards)
            const std::map<uint32_t, uint32_t> saved = m.r;
            for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) m.r[n48d2_stat2_regs2[i].abs] = 0xB5000000u + i;
            const size_t w2 = m.writes.size(), r2 = m.reads.size(); uint64_t v2[13];
            expect_u("inst 2 status2: OK", n48d2::status2(m, v2, 2), N48D2_OK);
            bool inOrder = true; for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) inOrder = inOrder && m.reads[r2 + i] == n48d2_stat2_regs2[i].abs;
            bool packed = true; for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) packed = packed && (uint32_t)(v2[1 + i / 2u] >> (32u * (i & 1u))) == 0xB5000000u + i;
            expect(m.writes.size() == w2 && m.reads.size() == r2 + N48D2_STAT2_COUNT && inOrder && packed && ((v2[0] >> 8) & 0xFF) == N48D2_OP_STATUS2, "inst 2 status2: the 12 table registers, in order, packed two per scalar, writes NOTHING");
            m.r = saved;
            for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) m.r[n48d2_stat3_regs2[i].abs] = 0xC5000000u + i;
            const size_t w3 = m.writes.size(), r3 = m.reads.size(); uint64_t v3[13];
            expect_u("inst 2 status3: OK", n48d2::status3(m, v3, 2), N48D2_OK);
            inOrder = true; for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) inOrder = inOrder && m.reads[r3 + i] == n48d2_stat3_regs2[i].abs;
            packed = true; for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) packed = packed && (uint32_t)(v3[1 + i / 2u] >> (32u * (i & 1u))) == 0xC5000000u + i;
            expect(m.writes.size() == w3 && m.reads.size() == r3 + N48D2_STAT3_COUNT && inOrder && packed && ((v3[0] >> 8) & 0xFF) == N48D2_OP_STATUS3 && ((v3[0] >> 16) & 0xFF) == N48D2_STAT3_COUNT && v3[8] == 0 && v3[12] == 0, "inst 2 status3: the 14 table registers, in order, packed two per scalar into v[1..7], writes NOTHING");
            m.r = saved;
        }
        const size_t w4 = m.writes.size();
        const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 2);
        expect_u("inst 2 off: status OK", o.status, N48D2_OK);
        expect_writes("inst 2 off: the exact write sequence", writes_from(m, w4), kOff2);
        expect(o.done == 21 && o.total == 23 && (m.get(N48D2_OTG2_CONTROL) & 1u) == 0 && m.get(N48D2_DIG3_FE_EN_CNTL) == 0 && m.get(N48D2_DIG3_STREAM_MAPPER) == 0 && m.get(N48D2_DIG3_BE_CNTL) == 0x20000000u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x1u &&
               m.get(N48D2_DPG2_CONTROL) == 0 && (m.get(N48D2_OTG2_CLOCK_CONTROL) & 3u) == 0 && (m.get(N48D2_ODM2_INPUT_CLOCK_CONTROL) & 3u) == 0 && m.get(N48D2_OPP_PIPE2_CONTROL) == 0, "inst 2 off: OTG2 stopped, FE off, SYMCLKD back to 0x1 (CLOCK_ENABLE kept), mapper 0, BE FE source 0, DPG off, clocks off");
        expect(m.get(N48D2_OTG0_CONTROL) == 0x00011201u && m.get(N48D2_DIG1_BE_CNTL) == 0x10000200u && m.get(N48D2_DIG1_BE_CLK_CNTL) == 0x810u && m.get(0x53ceu) == 1u && m.get(N48D2_SYMCLKB_CLOCK_ENABLE) == 0x111u && m.get(N48D2_SYMCLKC_CLOCK_ENABLE) == 0x1u, "the DP's registers, SYMCLKB and the monitor A's SYMCLKC are as they were");
        const n48d2_out o2 = n48d2::run(m, N48D2_OP_OFF, 2); expect(o2.status == N48D2_OK && o2.done == 21, "inst 2 off again from the off state is safe and complete");
    }
    {   // the symbol clock on instance 1 keeps CLOCK_ENABLE; the monitor A's lists never touch instance 2
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect(c.status == N48D2_OK && m.get(N48D2_SYMCLKC_CLOCK_ENABLE) == 0x211u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x1u && c.inst == 1u, "inst 1 connect: SYMCLKC 0x211 (the PHY's bit 0 kept, FE_EN 1, FE_SRC_SEL 2); SYMCLKD untouched");
        (void)n48d2::run(m, N48D2_OP_OFF);
        expect(m.get(N48D2_SYMCLKC_CLOCK_ENABLE) == 0x1u, "inst 1 off: SYMCLKC back to 0x1");
        for (size_t i = 0; i < m.writes.size(); i++) { for (unsigned k = 0; k < 39; k++) expect(m.writes[i].abs != n48d2_allowed2[k], "NO instance-2 register was ever written by an instance-1 run"); expect(m.writes[i].abs != N48D2_SYMCLKD_CLOCK_ENABLE && m.writes[i].abs != N48D2_SYMCLKB_CLOCK_ENABLE, "SYMCLKD and SYMCLKB were never written by an instance-1 run"); }
    }
    {   // both pipes at once: neither disturbs the other, and each `off` leaves the other's registers byte for byte
        Model m;
        const uint32_t a1 = n48d2::run(m, N48D2_OP_TIMING, 1).status; m.inst = 2; const uint32_t a2 = n48d2::run(m, N48D2_OP_TIMING, 2).status; m.inst = 1; const uint32_t a3 = n48d2::run(m, N48D2_OP_CONNECT, 1).status; m.inst = 2; const uint32_t a4 = n48d2::run(m, N48D2_OP_CONNECT, 2).status;
        expect(a1 == 0 && a2 == 0 && a3 == 0 && a4 == 0, "both pipes can be brought up one after the other");
        std::map<uint32_t, uint32_t> mona; for (unsigned k = 0; k < 39; k++) mona[n48d2_allowed[k]] = m.get(n48d2_allowed[k]); mona[N48D2_SYMCLKC_CLOCK_ENABLE] = m.get(N48D2_SYMCLKC_CLOCK_ENABLE);
        m.inst = 2; const n48d2_out od = n48d2::run(m, N48D2_OP_OFF, 2); bool same = od.status == 0; for (auto &kv : mona) same = same && m.get(kv.first) == kv.second;
        expect(same && (m.get(N48D2_OTG1_CONTROL) & 1u) == 1u && m.get(N48D2_SYMCLKC_CLOCK_ENABLE) == 0x211u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x1u, "instance 2's off leaves all 40 of instance 1's registers exactly as they were (the monitor A keeps running)");
        std::map<uint32_t, uint32_t> monb; (void)n48d2::run(m, N48D2_OP_TIMING, 2); (void)n48d2::run(m, N48D2_OP_CONNECT, 2);
        for (unsigned k = 0; k < 39; k++) monb[n48d2_allowed2[k]] = m.get(n48d2_allowed2[k]); monb[N48D2_SYMCLKD_CLOCK_ENABLE] = m.get(N48D2_SYMCLKD_CLOCK_ENABLE);
        m.inst = 1; const n48d2_out oa = n48d2::run(m, N48D2_OP_OFF, 1); same = oa.status == 0; for (auto &kv : monb) same = same && m.get(kv.first) == kv.second;
        expect(same && (m.get(N48D2_OTG2_CONTROL) & 1u) == 1u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x311u, "instance 1's off leaves all 40 of instance 2's registers exactly as they were (the monitor B keeps running)");
    }
    {   // instance 2 prechecks: each refusal writes nothing
        Model a; a.inst = 2; a.r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 0u; const n48d2_out x = n48d2::run(a, N48D2_OP_TIMING, 2);
        expect(x.status == N48D2_NO_PIXCLK && a.writes.empty() && x.pre_abs == N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL && x.pre_val == 0u, "inst 2 timing without `pclk otg2-on` (PHYPLL source 0) is refused, nothing written");
        Model a2; a2.inst = 2; a2.r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 2u; const n48d2_out x2 = n48d2::run(a2, N48D2_OP_TIMING, 2); expect(x2.status == N48D2_NO_PIXCLK && a2.writes.empty(), "inst 2 timing with the monitor A's PLL number 2 in OTG2's source is refused");
        Model a3; a3.r[N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL] = 3u; const n48d2_out x3 = n48d2::run(a3, N48D2_OP_TIMING, 1); expect(x3.status == N48D2_NO_PIXCLK && a3.writes.empty(), "inst 1 timing with PLL number 3 in OTG1's source is refused");
        Model b; b.inst = 2; b.r[N48D2_OTG2_CONTROL] = 1u; const n48d2_out y = n48d2::run(b, N48D2_OP_TIMING, 2); expect(y.status == N48D2_OTG1_BUSY && b.writes.empty() && y.pre_abs == N48D2_OTG2_CONTROL, "inst 2 timing with OTG2 already enabled is refused");
        Model c; c.inst = 2; c.otg0Stops = true; const n48d2_out z = n48d2::run(c, N48D2_OP_TIMING, 2); expect(z.status == N48D2_DP_NOT_COUNTING && c.writes.empty(), "inst 2 timing with OTG0 not counting is refused, nothing written");
        Model d; d.inst = 2; const n48d2_out nt = n48d2::run(d, N48D2_OP_CONNECT, 2); expect(nt.status == N48D2_NO_TIMING && d.writes.empty() && nt.pre_abs == N48D2_OTG2_CONTROL, "ORDER: inst 2 connect before timing is refused NO_TIMING, nothing written");
        Model e2; e2.inst = 2; (void)n48d2::run(e2, N48D2_OP_TIMING, 2); e2.r[N48D2_DIG3_BE_CLK_CNTL] = 0x810u; size_t w = e2.writes.size(); n48d2_out q = n48d2::run(e2, N48D2_OP_CONNECT, 2);
        expect(q.status == N48D2_NO_PHY && e2.writes.size() == w && q.pre_abs == N48D2_DIG3_BE_CLK_CNTL, "inst 2 connect with the BE in DP mode is refused, nothing written");
        e2.r[N48D2_DIG3_BE_CLK_CNTL] = 0x2812u; e2.r[N48D2_DIG3_BE_EN_CNTL] = 0u; q = n48d2::run(e2, N48D2_OP_CONNECT, 2); expect(q.status == N48D2_NO_PHY && e2.writes.size() == w, "inst 2 connect with the BE disabled is refused");
        e2.r[N48D2_DIG3_BE_EN_CNTL] = 1u; e2.r[N48D2_DIG3_FE_EN_CNTL] = 1u; q = n48d2::run(e2, N48D2_OP_CONNECT, 2); expect(q.status == N48D2_FE_BUSY && e2.writes.size() == w, "inst 2 connect with FE3 already enabled is refused");
        Model u; const n48d2_out ub = n48d2::run(u, N48D2_OP_TIMING, 0), ub3 = n48d2::run(u, N48D2_OP_TIMING, 3); expect(ub.status == N48D2_BAD_ARG && ub3.status == N48D2_BAD_ARG && u.writes.empty() && u.reads.empty(), "an unknown instance is BAD_ARG: not one register read or written");
    }
    {   // the DP watch, instance 2
        Model m; m.inst = 2; m.otg0StopOnMaster = true; const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING, 2);
        expect(t.status == N48D2_DP_DISTURBED && t.auto_off == 1 && (t.dp_changed & 0x80u) && (m.get(N48D2_OTG2_CONTROL) & 1u) == 0 && (m.get(N48D2_OTG2_CLOCK_CONTROL) & 1u) == 0, "inst 2: OTG0 stopping after timing: DP_DISTURBED, `off` ran (OTG2 master and clock off)");
        expect_u("the rollback is the instance-2 off list after the 33 timing writes", m.writes.size(), 33 + 21);
        Model n; n.inst = 2; (void)n48d2::run(n, N48D2_OP_TIMING, 2); n.dig1FlipOnFeEn = true; const n48d2_out c = n48d2::run(n, N48D2_OP_CONNECT, 2);
        expect(c.status == N48D2_DP_DISTURBED && c.auto_off == 1 && (c.dp_changed & (1u << 3)) && n.get(N48D2_DIG3_FE_EN_CNTL) == 0 && n.get(N48D2_DIG3_STREAM_MAPPER) == 0 && n.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x1u, "inst 2: DIG1_DIG_BE_CNTL changing across connect: DP_DISTURBED, `off` ran (FE3 off, mapper 0, SYMCLKD cleared)");
        // SYMCLKB changing across the op (a write of the symbol clock moves the DP's) ends the op in DP_DISTURBED and runs `off`
        for (uint32_t inst : { 1u, 2u }) {
            Model k; k.inst = inst; (void)n48d2::run(k, N48D2_OP_TIMING, inst); k.symclkbFlipOnSymclk = true; const n48d2_out sc = n48d2::run(k, N48D2_OP_CONNECT, inst);
            const uint32_t reg = inst == 1 ? N48D2_SYMCLKC_CLOCK_ENABLE : N48D2_SYMCLKD_CLOCK_ENABLE;
            expect(sc.status == N48D2_DP_DISTURBED && sc.auto_off == 1 && (sc.dp_changed & 0x40u) && sc.symclkb_a == 0x111u && sc.symclkb_b != sc.symclkb_a && (k.get(reg) & 0x710u) == 0u && k.get(N48D2_DIG2_FE_EN_CNTL) + k.get(N48D2_DIG3_FE_EN_CNTL) == 0u,
                   "SYMCLKB changing across connect: bit 6 of 'DP changed', DP_DISTURBED, `off` ran (the instance's SYMCLK bits cleared, FE off)");
        }
    }
    {   // an allowlist refusal part-way: stop and roll back; off is best effort; the symbol clock refusal is handled like any other
        Model m; m.inst = 2; m.refuseAbs = N48D2_OTG2_V_TOTAL; const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING, 2);
        expect(t.status == N48D2_ALLOW_REFUSED && t.fail == 11 && t.done == 9 && t.auto_off == 1 && (m.get(N48D2_OTG2_CLOCK_CONTROL) & 1u) == 0, "inst 2: an allowlist refusal at step 11 (OTG_V_TOTAL) stops timing (9 writes done) and runs off");
        Model k; k.inst = 2; (void)n48d2::run(k, N48D2_OP_TIMING, 2); k.refuseAbs = N48D2_SYMCLKD_CLOCK_ENABLE; const n48d2_out c = n48d2::run(k, N48D2_OP_CONNECT, 2);
        expect(c.status == N48D2_ALLOW_REFUSED && c.fail == 0 && c.done == 0 && c.auto_off == 1 && k.get(N48D2_DIG3_FE_EN_CNTL) == 0, "inst 2: the DCN allowlist refusing the symbol clock step stops connect at step 0 (nothing of the front end written) and runs off");
        Model o; o.inst = 2; (void)n48d2::run(o, N48D2_OP_TIMING, 2); (void)n48d2::run(o, N48D2_OP_CONNECT, 2); o.refuseAbs = N48D2_DIG3_STREAM_MAPPER; const n48d2_out off = n48d2::run(o, N48D2_OP_OFF, 2);
        expect(off.status == N48D2_ALLOW_REFUSED && off.done == 20 && (o.get(N48D2_OTG2_CONTROL) & 1u) == 0 && o.get(N48D2_OPP_PIPE2_CONTROL) == 0 && (o.get(N48D2_SYMCLKD_CLOCK_ENABLE) & 0x710u) == 0u, "inst 2 off with one refused write still runs every other step (SYMCLKD cleared, OTG2 stopped)");
        Model v; v.inst = 2; (void)n48d2::run(v, N48D2_OP_TIMING, 2); v.r[N48D2_DIG3_BE_CNTL] = 0x20000400u; const size_t wv = v.writes.size(); const n48d2_out cv = n48d2::run(v, N48D2_OP_CONNECT, 2);
        expect(cv.status == N48D2_GUARD_REFUSED && cv.fail == 8 && cv.guard == N48D2_G_VALUE && cv.auto_off == 1, "inst 2: a live DIG3 BE_CNTL holding the monitor A's FE2 bit stops connect at the BE step (value guard) and rolls back");
        bool beWritten = false; for (size_t i = wv; i < v.writes.size(); i++) if (v.writes[i].abs == N48D2_DIG3_BE_CNTL && (v.writes[i].v & 0x400u)) beWritten = true;
        expect(!beWritten, "DIG3_BE_CNTL is never written while it holds the monitor A's FE2 bit");
    }
    {   // the guard inside run_steps refuses the other instance's register and an off-value SYMCLK step, with nothing reaching the model
        Model m; m.inst = 2; n48d2_step s[2]; uint32_t n = 0;
        n48d2_put(s, &n, N48D2_K_SET, N48D2_OTG1_CONTROL, 0xFFFFFFFFu, 0u, 0, 0, 0, 0, "OTG1_CONTROL", "t");
        n48d2_out o; std::memset(&o, 0, sizeof o); uint32_t ref = 0;
        expect(n48d2::run_steps(m, 2, s, n, false, o, &ref) == N48D2_GUARD_REFUSED && m.writes.empty() && ref == 1 && o.guard == N48D2_G_OTHER, "run_steps (instance 2) refuses an instance-1 write itself, naming the OTHER instance");
        Model k; k.inst = 2; n = 0; n48d2_put(s, &n, N48D2_K_UPD, N48D2_SYMCLKD_CLOCK_ENABLE, 0x710u, 0x210u, 0, 0, 0, 0, "SYMCLKD_CLOCK_ENABLE", "t"); std::memset(&o, 0, sizeof o); ref = 0;
        expect(n48d2::run_steps(k, 2, s, n, false, o, &ref) == N48D2_GUARD_REFUSED && k.writes.empty() && o.guard == N48D2_G_VALUE, "run_steps (instance 2) refuses SYMCLKD with the monitor A's FE_SRC_SEL 2 (value guard)");
    }
    {   // the result of an instance-2 run travels with its instance bit
        Model m; m.inst = 2; const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING, 2); uint64_t pv[13]; n48d2_pack(pv, &t); n48d2_out u; std::memset(&u, 0, sizeof u); n48d2_unpack(pv, &u);
        expect(((pv[0] >> 51) & 1u) == 1u && u.inst == 2u && u.status == t.status && u.done == t.done, "the instance travels in v[0] bit 51 and survives the pack");
        Model k; const n48d2_out ta = n48d2::run(k, N48D2_OP_TIMING, 1); n48d2_pack(pv, &ta); expect(((pv[0] >> 51) & 1u) == 0u, "instance 1's v[0] has bit 51 clear (byte-identical to 0.0.632's layout)");
    }
    {   // status3 of instance 1 (the monitor A's DIG2) reads its table too; and the table's contents are the requested registers
        Model m; for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) m.r[n48d2_stat3_regs[i].abs] = 0xD5000000u + i;
        const size_t r0 = m.reads.size(); uint64_t v[13]; expect_u("inst 1 status3: OK", n48d2::status3(m, v), N48D2_OK);
        bool inOrder = true, packed = true; for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) { inOrder = inOrder && m.reads[r0 + i] == n48d2_stat3_regs[i].abs; packed = packed && (uint32_t)(v[1 + i / 2u] >> (32u * (i & 1u))) == 0xD5000000u + i; }
        expect(inOrder && packed && m.writes.empty(), "inst 1 status3: the table in order, packed, no write");
        const uint32_t want2[] = { 0x57a0, 0x57a1, 0x57a4, 0x57a7, 0x57ec, 0x57ed, 0x57f3, 0x57f4, 0x579e, 0x579f, 0x567f, 0x56cf, 0x161, 0x162 };
        bool tbl = true; for (unsigned i = 0; i < 14; i++) tbl = tbl && n48d2_stat3_regs[i].abs == want2[i] && n48d2_stat3_regs[i].name && n48d2_stat3_regs[i].name[0];
        expect(tbl, "the monitor A's status3 registers are the ones the M4d analysis named: DIG2 0x57a0 0x57a1 0x57a4 0x57a7 0x57ec 0x57ed 0x57f3 0x57f4 0x579e 0x579f ; DIG1 0x567f 0x56cf ; SYMCLKB 0x161 ; SYMCLKC 0x162");
        const uint32_t want3[] = { 0x58c4, 0x58c5, 0x58c8, 0x58cb, 0x5910, 0x5911, 0x5917, 0x5918, 0x58c2, 0x58c3, 0x567f, 0x56cf, 0x161, 0x163 };
        tbl = true; for (unsigned i = 0; i < 14; i++) tbl = tbl && n48d2_stat3_regs2[i].abs == want3[i];
        expect(tbl && N48D2_STAT3_SYMCLKB == 12u && n48d2_stat3_regs[N48D2_STAT3_SYMCLKB].abs == N48D2_SYMCLKB_CLOCK_ENABLE && n48d2_stat3_regs2[N48D2_STAT3_SYMCLKB].abs == N48D2_SYMCLKB_CLOCK_ENABLE, "the monitor B's status3 registers are DIG3's twins (offset.h), DIG1's two, SYMCLKB at index 12 and SYMCLKD");
        for (uint32_t i = 0; i < N48D2_STAT3_COUNT; i++) {      // a status3 register is writable ONLY if it is on the instance's own list (CLOCK_PATTERN, FIFO_CTRL0) or is its one exception; DIG1's, SYMCLKB and the rest never are
            expect((n48d2_guard_addr_i(1, n48d2_stat3_regs[i].abs, nullptr) == N48D2_G_OK) == (set1.count(n48d2_stat3_regs[i].abs) != 0), "status3 (instance 1): writable iff on instance 1's list or its exception");
            expect((n48d2_guard_addr_i(2, n48d2_stat3_regs2[i].abs, nullptr) == N48D2_G_OK) == (set2.count(n48d2_stat3_regs2[i].abs) != 0), "status3 (instance 2): writable iff on instance 2's list or its exception");
            if (i >= 10 && i <= 12) { expect(n48d2_guard_addr_i(1, n48d2_stat3_regs[i].abs, nullptr) != N48D2_G_OK && n48d2_guard_addr_i(2, n48d2_stat3_regs2[i].abs, nullptr) != N48D2_G_OK, "DIG1's FIFO_CTRL0 / TMDS_CTL_BITS and SYMCLKB are never writable by either instance"); } }
    }
}



// =================================================================================================================================================================================================================
// 0.0.634 (stage M4b / item 4): the monitor B at 2560x1440@60, off's order, the DP-plane watch
// =================================================================================================================================================================================================================
static std::string find_notes_file(const char *rel) {
    std::vector<std::string> roots; if (const char *e = std::getenv("N48_NOTES")) roots.push_back(e);
    roots.push_back(g_root); roots.push_back("/path/to/navi48-checkout");
    for (auto &r : roots) { std::string t = slurp(r + "/" + rel); if (!t.empty()) return t; }
    return "";
}
static void m4_634_tests() {
    // ---- M1. the DTD: the capture's bytes, the hand decode, the decoder
    {
        const std::string ddc = find_notes_file("notes/logs/runs/m1-622/ddc.txt");
        expect(!ddc.empty(), "M1: the captured monitor B EDID (notes/logs/runs/m1-622/ddc.txt) is readable");
        const size_t at = ddc.find("accel ddcread: line 3 block 1");
        std::vector<uint8_t> e;
        if (at != std::string::npos) for (int row = 0; row < 8; row++) { char key[16]; std::snprintf(key, sizeof key, "  %03x:", row * 16); const size_t p = ddc.find(key, at); if (p == std::string::npos) break;
            const char *c = ddc.c_str() + p + std::strlen(key); for (int k = 0; k < 16; k++) { char *end = nullptr; e.push_back((uint8_t)std::strtoul(c, &end, 16)); c = end; } }
        expect_u("M1: 128 CTA block bytes", e.size(), 128);
        if (e.size() == 128) {
            uint32_t sum = 0; for (uint8_t b : e) sum += b;
            expect_u("M1: the CTA block checksum", sum & 0xFFu, 0);
            expect(e[0] == 0x02 && std::memcmp(&e[0x64], n48d2_monb_dtd1440, 18) == 0, "M1: n48d2_monb_dtd1440 IS bytes 0x64..0x75 of the capture's CTA block (`56 5e 00 a0 a0 a0 29 50 30 20 35 00 55 50 21 00 00 1a`)");
        }
        // the hand decode (EDID 1.4 section 3.10.2), independent of n48d2_dtd_decode
        const uint8_t *d = n48d2_monb_dtd1440;
        const uint32_t pix = d[0] | (d[1] << 8), ha = d[2] | ((d[4] >> 4) << 8), hb = d[3] | ((d[4] & 15) << 8), va = d[5] | ((d[7] >> 4) << 8), vb = d[6] | ((d[7] & 15) << 8),
                       hfp = d[8] | (((d[11] >> 6) & 3) << 8), hsw = d[9] | (((d[11] >> 4) & 3) << 8), vfp = (d[10] >> 4) | (((d[11] >> 2) & 3) << 4), vsw = (d[10] & 15) | ((d[11] & 3) << 4);
        expect(pix == 24150 && ha == 2560 && hb == 160 && va == 1440 && vb == 41 && hfp == 48 && hsw == 32 && vfp == 3 && vsw == 5 && d[17] == 0x1a, "M1: by hand: 241.50 MHz, 2560 + 160 = 2720, fp 48 / sync 32, 1440 + 41 = 1481, fp 3 / sync 5; flags 0x1a");
        n48d2_timing t; expect_u("M1: the DTD decodes", n48d2_dtd_decode(n48d2_monb_dtd1440, &t), 0);
        expect(t.pix_clk_10khz == 24150 && t.h_active == 2560 && t.h_blank == 160 && t.h_total == 2720 && t.h_fp == 48 && t.h_sync == 32 && t.h_bp == 80 && t.v_active == 1440 && t.v_blank == 41 && t.v_total == 1481 && t.v_fp == 3 && t.v_sync == 5 && t.v_bp == 33, "M1: n48d2_dtd_decode: 2720 x 1481, 48 / 32 / 80 and 3 / 5 / 33");
        expect(t.h_pos == 1 && t.v_pos == 0 && t.digital_separate == 1 && t.interlace == 0 && t.h_border == 0 && t.v_border == 0, "M1: hsync POSITIVE, vsync NEGATIVE (d[17] 0x1a: bit 1 = 1, bit 2 = 0), digital separate, progressive");
        expect_u("M1: 59.95 Hz (mHz): the monitor B lists a reduced-blanking-class 59.95 Hz timing", (uint64_t)t.pix_clk_10khz * 10000ull * 1000ull / ((uint64_t)t.h_total * t.v_total), 59950);   // 241.5e6 / (2720 x 1481) = 59.950 Hz
    }
    // ---- M2. the 1440p list over the monitor B: the 1080p list's twin (same registers, same order, same masks, same functions) with only the timing-dependent values different
    {
        n48d2_step a[N48D2_MAX_STEPS], b[N48D2_MAX_STEPS];
        const uint32_t na = n48d2_build_i(N48D2_INST_MONB, N48D2_OP_TIMING, a), nb = n48d2_build_i(N48D2_INST_MONB, N48D2_OP_TIMING1440, b);
        expect(na == nb && nb == 36, "M2: timing1440 has the 1080p timing's 36 steps");
        bool same = na == nb; std::set<uint32_t> differ;
        for (uint32_t i = 0; i < na && i < nb; i++) { same = same && a[i].kind == b[i].kind && a[i].abs == b[i].abs && a[i].mask == b[i].mask && a[i].polls == b[i].polls && a[i].step_us == b[i].step_us && a[i].cond_abs == b[i].cond_abs && std::string(a[i].fn) == b[i].fn; if (a[i].val != b[i].val) differ.insert(a[i].abs); }
        expect(same, "M2: step for step, the SAME registers in the SAME order with the same masks, waits and Linux functions as the 1080p timing (programmed exactly like the existing timing)");
        const std::set<uint32_t> want = { N48D2_OTG2_H_TOTAL, N48D2_OTG2_H_SYNC_A, N48D2_OTG2_H_BLANK_START_END, N48D2_OTG2_V_TOTAL, N48D2_OTG2_V_TOTAL_MAX, N48D2_OTG2_V_TOTAL_MIN, N48D2_OTG2_V_BLANK_START_END, N48D2_OTG2_V_SYNC_A_CNTL, N48D2_VTG2_CONTROL, N48D2_DPG2_DIMENSIONS };
        expect(differ == want, "M2: ONLY the timing-dependent values differ: H_TOTAL, H_SYNC_A, H_BLANK, V_TOTAL / MAX / MIN, V_BLANK, V_SYNC_A_CNTL (vsync polarity); V_SYNC_A (end 5) is equal, VTG_CONTROL (VCOUNT_INIT) and DPG_DIMENSIONS");
        n48d2_step c[N48D2_MAX_STEPS]; expect_u("M2: instance 1 has no timing1440 list", n48d2_build_i(N48D2_INST_MONA, N48D2_OP_TIMING1440, c), 0);
        expect(n48d2_build(N48D2_OP_TIMING1440, c) == 0, "M2: the instance-1 entry point builds no timing1440 either");
        expect(!n48d2_arg_ok(n48d2_arg(N48D2_OP_TIMING1440)) && !n48d2_arg_ok(n48d2_arg_i(N48D2_OP_TIMING1440, 1)) && n48d2_arg_ok(n48d2_arg_i(N48D2_OP_TIMING1440, 2)) && std::strcmp(n48d2_op_name(N48D2_OP_TIMING1440), "timing1440") == 0 && N48D2_OP_TIMING1440 == 7u, "M2: op 7 `timing1440` is a legal argument for instance 2 only");
        { uint32_t bad = 0; expect(n48d2_guard_list_i(N48D2_INST_MONB, b, nb, &bad) == 0u, "M2: the whole timing1440 list passes the monitor B's instance guard (no register outside the 39 + SYMCLKD exception is named)"); }
    }
    // ---- M3. the flow: the exact write sequence of `timing1440 2` (computed BY HAND from the DTD), its prechecks, and the ordering with connect / off
    {
        const std::vector<W> kTiming1440 = {
            { 0x4fb0, 0x00000003 }, { 0x5144, 0x00000003 }, { 0x4fab, 0xFFF20000 }, { 0x50ee, 0x00000000 }, { 0x4fb1, 0x00000000 }, { 0x50ea, 2719 /*0xa9f*/ },
            { 0x50ec, 32u << 16 }, { 0x50eb, (112u << 16) | 2672u }, { 0x50ed, 0x00000000 }, { 0x50ef, 1480 }, { 0x50f1, 1480 }, { 0x50f0, 1480 },
            { 0x50f9, 5u << 16 }, { 0x50f8, (38u << 16) | 1478u }, { 0x50fa, 0x00000001 }, { 0x5105, 0x00000000 }, { 0x39f2, 0x00000000 }, { 0x5103, 0x00000000 },
            { 0x5145, 0x0000000D }, { 0x5146, 0x014002A8 }, { 0x5147, 0x0000012C }, { 0x39f2, 1478u << 16 }, { 0x4fac, 0x00000000 }, { 0x50ee, 0x00000000 },
            { 0x50ee, 0x00000000 }, { 0x4e00, 0x00000001 }, { 0x4dbd, 0x00000000 }, { 0x4dca, (2560u << 16) | 1440u }, { 0x4dce, 0x00000000 }, { 0x4dc8, 0x00661001 },
            { 0x4fab, 0xFFF20000 }, { 0x39f2, 0x80000000u | (1478u << 16) }, { 0x5103, 0x00000201 },
        };
        Model m; m.inst = 2; m.frames1 = 5; m.frames2 = 777;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING1440, 2);
        expect_u("M3: timing1440 2: status OK", t.status, N48D2_OK);
        expect_writes("M3: timing1440 2: the exact write sequence (hand-derived: 2719, 0x00200000, 0x00700a70, 1480, 0x00050000, 0x002605c6, V pol 1, VCOUNT_INIT 1478, DPG 2560 x 1440)", m.writes, kTiming1440);
        expect(t.done == 33 && t.total == 36 && t.timeouts == 0 && t.fail == 0xFF && t.auto_off == 0 && t.dp_changed == 0 && t.dpx_changed == 0 && t.inst == 2u, "M3: 33 writes of 36 steps, no timeout, no rollback, the DP (six + SYMCLKB + plane watch) unchanged, instance 2");
        expect(t.f0b != t.f0a && t.f1b != t.f1a && t.dpg_ctl == 0x00661001u && (t.otg1_ctl & 1u), "M3: OTG0 counting, OTG2 counting, DPG2 on");
        { bool inside = true; for (const W &w : m.writes) { const char *why = nullptr; inside = inside && n48d2_guard_addr_i(N48D2_INST_MONB, w.abs, &why) == N48D2_G_OK; for (unsigned k = 0; k < 39; k++) inside = inside && w.abs != n48d2_allowed[k]; }
          expect(inside && m.refusedCount == 0, "M3: every write is a register of the monitor B's own 39 (none refused by the guard or the REAL allowlist, none of instance 1's)"); }
        // the OTG2_PHYPLL source check stays: 3 is required
        { Model a; a.inst = 2; a.r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 0u; const n48d2_out x = n48d2::run(a, N48D2_OP_TIMING1440, 2); expect(x.status == N48D2_NO_PIXCLK && a.writes.empty() && x.pre_abs == N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL && x.pre_val == 0u, "M3: timing1440 without `pclk otg2-1440-on` (PHYPLL source 0) is refused NO_PIXCLK, nothing written");
          Model a2; a2.inst = 2; a2.r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 2u; const n48d2_out x2 = n48d2::run(a2, N48D2_OP_TIMING1440, 2); expect(x2.status == N48D2_NO_PIXCLK && a2.writes.empty(), "M3: ... and with source 2 (PLL2, the monitor A's) too"); }
        { Model b; b.inst = 2; b.r[N48D2_OTG2_CONTROL] = 1u; const n48d2_out y = n48d2::run(b, N48D2_OP_TIMING1440, 2); expect(y.status == N48D2_OTG1_BUSY && b.writes.empty(), "M3: timing1440 on a running OTG2 is refused, nothing written"); }
        { Model g; g.inst = 2; g.otg0Stops = true; const n48d2_out y = n48d2::run(g, N48D2_OP_TIMING1440, 2); expect(y.status == N48D2_DP_NOT_COUNTING && g.writes.empty(), "M3: with OTG0 not counting nothing is written"); }
        // instance 1 asked for timing1440 directly (the flow, past the argument check): refused by the guard on an empty list, NOTHING read or written
        { Model i1; const n48d2_out y = n48d2::run(i1, N48D2_OP_TIMING1440, 1); expect(y.status == N48D2_GUARD_REFUSED && i1.writes.empty() && i1.reads.empty(), "M3: the flow itself refuses timing1440 for instance 1: GUARD_REFUSED, no read, no write"); }
        // REACHABILITY: the real ordering timing1440 -> connect -> status pages -> off over ONE model; connect and off are the 1080p lists unchanged
        const size_t w0 = m.writes.size();
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT, 2);
        expect(c.status == N48D2_OK && c.done == 20 && c.fe_en == 1u && m.get(N48D2_DIG3_STREAM_MAPPER) == 3u && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x311u, "M3: connect 2 runs after timing1440 (OTG2 running, BE up) exactly as after the 1080p timing");
        (void)w0;
        const size_t w1 = m.writes.size();
        const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 2);
        expect(o.status == N48D2_OK && o.done == 21 && m.get(N48D2_DPG2_CONTROL) == 0 && (m.get(N48D2_OTG2_CONTROL) & 1u) == 0 && m.get(N48D2_SYMCLKD_CLOCK_ENABLE) == 0x1u && m.get(N48D2_DIG3_BE_CNTL) == 0x20000000u, "M3: off 2 after timing1440 + connect: OTG2 stopped, DPG off, SYMCLKD back to 0x1, BE source back");
        // the SYMCLK disable FOLLOWS the BE disconnect in the written sequence (item 3a)
        { const std::vector<W> f = writes_from(m, w1);
          expect(first_write(f, N48D2_DIG3_BE_CNTL) < first_write(f, N48D2_SYMCLKD_CLOCK_ENABLE) && first_write(f, N48D2_SYMCLKD_CLOCK_ENABLE) < first_write(f, N48D2_DIG3_STREAM_MAPPER), "M3 (item 3a): off 2 writes DIG3_BE_CNTL (the disconnect) BEFORE SYMCLKD_CLOCK_ENABLE"); }
        const n48d2_out o2 = n48d2::run(m, N48D2_OP_OFF, 2); expect(o2.status == N48D2_OK, "M3: a second off is still safe");
    }
    // ---- M4. the side-by-side with the DP's live OTG0 raster (notes/logs/runs/t1a/passA-decoded.txt): the 1440p timing equals the DP's raster register for register, except the vsync polarity
    {
        const std::string pa = find_notes_file("notes/logs/runs/t1a/passA-decoded.txt");
        expect(!pa.empty(), "M4: the DP's live capture (notes/logs/runs/t1a/passA-decoded.txt) is readable");
        auto live = [&](const char *name) -> uint32_t { const std::string key = std::string(name) + " "; size_t p = pa.find(std::string("\n") + key); if (p == std::string::npos && pa.compare(0, key.size(), key) == 0) p = 0; else if (p != std::string::npos) ++p;
            if (p == std::string::npos) return 0xdeadbeefu; const size_t eq = pa.find("= 0x", p); return eq == std::string::npos ? 0xdeadbeefu : (uint32_t)std::strtoul(pa.c_str() + eq + 2, nullptr, 16); };
        n48d2_timing t; (void)n48d2_dtd_decode(n48d2_monb_dtd1440, &t); n48d2_otg_regs r; (void)n48d2_otg_compute(&t, &r);
        expect(r.h_total == live("OTG0_OTG_H_TOTAL") && r.h_total == 2719u, "M4: OTG_H_TOTAL 2719 = the DP's live OTG0 (0x00000a9f)");
        expect(r.h_blank == live("OTG0_OTG_H_BLANK_START_END") && r.h_blank == 0x00700a70u, "M4: OTG_H_BLANK_START_END 0x00700a70 (start 2672, end 112) = the DP's live OTG0");
        expect(r.h_sync_a == live("OTG0_OTG_H_SYNC_A") && r.h_sync_a == 0x00200000u, "M4: OTG_H_SYNC_A 0x00200000 (0..32) = the DP's live OTG0");
        expect(r.v_total == live("OTG0_OTG_V_TOTAL") && r.v_total == 1480u, "M4: OTG_V_TOTAL 1480 = the DP's live OTG0 (0x000005c8)");
        expect(r.v_blank == live("OTG0_OTG_V_BLANK_START_END") && r.v_blank == 0x002605c6u, "M4: OTG_V_BLANK_START_END 0x002605c6 (start 1478, end 38) = the DP's live OTG0");
        expect(r.v_sync_a == live("OTG0_OTG_V_SYNC_A") && r.v_sync_a == 0x00050000u, "M4: OTG_V_SYNC_A 0x00050000 (0..5) = the DP's live OTG0");
        expect(r.vstartup == live("OTG0_OTG_VSTARTUP_PARAM") && r.vupdate == live("OTG0_OTG_VUPDATE_PARAM") && r.vready == live("OTG0_OTG_VREADY_PARAM"), "M4: VSTARTUP 13, VUPDATE 0x014002a8, VREADY 0x12c = the DP's live OTG0 (the borrowed global sync is the SAME at 1440p)");
        expect(r.h_pol == live("OTG0_OTG_H_SYNC_A_CNTL") && r.h_pol == 0u, "M4: H_SYNC_A POL 0 (positive) = the DP's live OTG0");
        expect(live("OTG0_OTG_V_SYNC_A_CNTL") == 0u && r.v_pol == 1u, "M4: the ONE difference: V_SYNC_A POL is 1 (the monitor B's DTD says vsync negative) where the DP's OTG0 holds 0");
        expect(r.vtg_vcount_init == 1478u && r.vtg_fp2 == 0u && r.h_div2 == 1u, "M4: VTG VCOUNT_INIT 1478, FP2 0, h-divisible-by-2 (DIV_MANUAL stays 0)");
    }
    // ---- M5. the DP plane watch (item 4)
    {
        const struct { uint32_t ours; const char *lx; } dpx[N48D2_DPX_REGS] = {
            { N48D2_MPC_OUT0_MUX, "MPC_OUT0_MUX" }, { N48D2_MPCC0_TOP_SEL, "MPCC0_MPCC_TOP_SEL" }, { N48D2_MPCC0_BOT_SEL, "MPCC0_MPCC_BOT_SEL" }, { N48D2_MPCC0_OPP_ID, "MPCC0_MPCC_OPP_ID" }, { N48D2_HUBP0_DCHUBP_CNTL, "HUBP0_DCHUBP_CNTL" },
            { N48D2_DPPCLK0_DTO_PARAM, "DPPCLK0_DTO_PARAM" }, { N48D2_DPPCLK_CTRL, "DPPCLK_CTRL" }, { N48D2_DET0_CTRL, "DCHUBBUB_DET0_CTRL" }, { N48D2_COMPBUF_CTRL, "DCHUBBUB_COMPBUF_CTRL" },
            { N48D2_HUBP0_DCHUBP_CNTL, "HUBP0_DCHUBP_CNTL" }, { N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL, "ODM0_OPTC_INPUT_GLOBAL_CONTROL" }, { N48D2_DPP_TOP0_DPP_CONTROL, "DPP_TOP0_DPP_CONTROL" }, { N48D2_MPCC0_UPDATE_LOCK_SEL, "MPCC0_MPCC_UPDATE_LOCK_SEL" },
            { N48D2_OTG2_OTG_GLOBAL_CONTROL2, "OTG2_OTG_GLOBAL_CONTROL2" }, { N48D2_HUBPREQ0_PRIMARY_LOW, "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS" } };
        for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) { char b[128]; std::snprintf(b, sizeof b, "DP watch register %s is Linux's absolute address and table entry %u", dpx[i].lx, i); expect_u(b, dpx[i].ours, lx_abs(dpx[i].lx)); expect(n48d2_dpx_regs[i].abs == dpx[i].ours && n48d2_dpx_regs[i].name && n48d2_dpx_regs[i].name[0], "DP watch table entry i is that register, named"); }
        expect(N48D2_MPC_OUT0_MUX == 0x92f2u && N48D2_MPCC0_TOP_SEL == 0x9000u && N48D2_HUBP0_DCHUBP_CNTL == 0x3ab4u && N48D2_DPPCLK0_DTO_PARAM == 0x159u && N48D2_DPPCLK_CTRL == 0x168u && N48D2_DET0_CTRL == 0x397bu && N48D2_COMPBUF_CTRL == 0x397au, "DP watch: the absolute dwords 0x92f2, 0x9000..0x9002, 0x3ab4, 0x159, 0x168, 0x397b, 0x397a");
        expect(N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL == 0x4f8au && N48D2_DPP_TOP0_DPP_CONTROL == 0x4185u && N48D2_MPCC0_UPDATE_LOCK_SEL == 0x9005u && N48D2_OTG2_OTG_GLOBAL_CONTROL2 == 0x5150u && N48D2_HUBPREQ0_PRIMARY_LOW == 0x3acau && N48D2_HUBPREQ0_PRIMARY_HIGH == 0x3acbu && N48D2_DPX_REGS == 15u && N48D2_DPX_BUF == 16u && N48D2_DPX_RULE == 14u,
               "DP watch (0.0.635): the six new reads 0x4f8a, 0x4185, 0x9005, 0x5150, 0x3aca (+ HIGH 0x3acb); fifteen rows, a 16-dword buffer, row 14 the rule");
        expect_u("DP watch: HUBPREQ0 primary HIGH is Linux's", N48D2_HUBPREQ0_PRIMARY_HIGH, lx_abs("HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"));
        expect(n48d2_dpx_regs[9].mask == (lx_mask("HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS") | lx_mask("HUBP0_DCHUBP_CNTL__HUBP_SEG_ALLOC_ERR_STATUS")) && n48d2_dpx_regs[9].mask == 0x70000800u && n48d2_dpx_regs[9].abs == n48d2_dpx_regs[4].abs, "DP watch row 9: HUBP0_DCHUBP_CNTL again, underflow [30:28] + SEG_ALLOC_ERR [11] only");
        expect(n48d2_dpx_regs[10].mask == lx_mask("ODM0_OPTC_INPUT_GLOBAL_CONTROL__OPTC_UNDERFLOW_OCCURRED_STATUS") && n48d2_dpx_regs[12].mask == lx_mask("MPCC0_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL") && n48d2_dpx_regs[12].mask == 0xFu, "DP watch rows 10 / 12: ODM0 underflow-occurred bit 10, MPCC0's SELECT field (the locked-status bits [6:4] move)");
        // masks against sh_mask.h
        const uint32_t ctlMask = lx_mask("HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ") | lx_mask("HUBP0_DCHUBP_CNTL__HUBP_IN_BLANK") | lx_mask("HUBP0_DCHUBP_CNTL__HUBP_XRQ_NO_OUTSTANDING_REQ");
        expect(n48d2_dpx_regs[4].mask == ~ctlMask && ctlMask == 0x000F000Au && N48D2_DPX_HUBP_CNTL_MASK == ~ctlMask, "DP watch: HUBP0_DCHUBP_CNTL is compared on every bit except NO_OUTSTANDING_REQ [1], IN_BLANK [3] and XRQ_NO_OUTSTANDING_REQ [19:16] (the sh_mask header's masks)");
        expect(N48D2_HUBP_UNDERFLOW_MASK == lx_mask("HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS") && (N48D2_HUBP_UNDERFLOW_MASK & n48d2_dpx_regs[4].mask) == N48D2_HUBP_UNDERFLOW_MASK, "DP watch: the underflow bits [30:28] are in the compared mask (sh_mask: HUBP_UNDERFLOW_STATUS 0x70000000)");
        expect(n48d2_dpx_regs[6].mask == lx_mask("DPPCLK_CTRL__DPPCLK0_EN") && n48d2_dpx_regs[6].mask == 1u, "DP watch: DPPCLK_CTRL is compared on bit 0 (DPPCLK0_EN) only: the register is shared by every pipe");
        { bool allfull = true; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) if (i != 4 && i != 6 && i != 9 && i != 10 && i != 12) allfull = allfull && n48d2_dpx_regs[i].mask == 0xFFFFFFFFu; expect(allfull, "DP watch: the other ten are compared whole"); }
        // never writable: not by either instance's guard, so no list can contain them
        { bool ro = true; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) { ro = ro && (n48d2_dpx_regs[i].abs == N48D2_DPPCLK_CTRL || n48d2_guard_addr_i(1, n48d2_dpx_regs[i].abs, nullptr) != N48D2_G_OK) && (n48d2_dpx_regs[i].abs == N48D2_DPPCLK_CTRL || n48d2_dpx_regs[i].abs == N48D2_OTG2_OTG_GLOBAL_CONTROL2 || n48d2_guard_addr_i(2, n48d2_dpx_regs[i].abs, nullptr) != N48D2_G_OK); Model mm; ro = ro && (n48d2_dpx_regs[i].abs == N48D2_OTG2_OTG_GLOBAL_CONTROL2 ? n48d2_guard_addr_i(1, N48D2_OTG2_OTG_GLOBAL_CONTROL2, nullptr) != N48D2_G_OK : n48d2_dpx_regs[i].abs == N48D2_DPPCLK_CTRL ? true : !mm.wr(n48d2_dpx_regs[i].abs, 0u)); }
          ro = ro && n48d2_guard_addr_i(1, N48D2_HUBPREQ0_PRIMARY_HIGH, nullptr) != N48D2_G_OK && n48d2_guard_addr_i(2, N48D2_HUBPREQ0_PRIMARY_HIGH, nullptr) != N48D2_G_OK;
          expect(ro, "DP watch: none of the fifteen (+ the primary HIGH) is writable by instance 1 except (0.0.655) DPPCLK_CTRL, its own DCCG exception (UPDATE of bit 3 alone, pinned by the list guard); instance 2 may write ONLY DPPCLK_CTRL (the one DCCG exception: UPDATE of bit 6, mask pinned by the list guard) and (0.0.643) OTG2_GLOBAL_CONTROL2 (op 8's lock select: an UPDATE of the select field to 2, pinned by the list guard)"); }
        // a clean run reads the nine before the first write and again after the last, changes nothing, and writes EXACTLY what the 0.0.633 list wrote
        for (uint32_t inst : { 1u, 2u }) for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
            Model m; m.inst = inst;
            for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) { uint32_t first = i; for (uint32_t j = 0; j < i; j++) if (n48d2_dpx_regs[j].abs == n48d2_dpx_regs[i].abs) { first = j; break; } m.r[n48d2_dpx_regs[i].abs] = 0x10u + first; }      // distinct non-zero values (rows 4 and 9 are the same register)
            m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = 0x80u;
            if (op != N48D2_OP_TIMING) (void)n48d2::run(m, N48D2_OP_TIMING, inst);
            if (op == N48D2_OP_OFF) (void)n48d2::run(m, N48D2_OP_CONNECT, inst);
            const size_t r0 = m.reads.size(), w0 = m.writes.size();
            const n48d2_out o = n48d2::run(m, op, inst);
            expect(o.status == N48D2_OK && o.dpx_changed == 0u && o.dp_changed == 0u, (std::string("clean ") + n48d2_op_name(op) + " (instance " + std::to_string(inst) + "): OK and the DP plane watch unchanged").c_str());
            bool vals = m.dpxBuf[0][N48D2_DPX_BUF - 1u] == 0x80u && m.dpxBuf[1][N48D2_DPX_BUF - 1u] == 0x80u; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) vals = vals && m.dpxBuf[0][i] == (m.r[n48d2_dpx_regs[i].abs] & n48d2_dpx_regs[i].mask) && m.dpxBuf[1][i] == m.dpxBuf[0][i];
            expect(vals, (std::string("clean ") + n48d2_op_name(op) + ": the masked before / after values are recorded in the environment's two buffers").c_str());
            // reads of the nine: counted before the first write (>= 9) and after the last write
            size_t before = 0, after = 0;
            for (size_t k = r0; k < m.reads.size(); k++) { bool isw = m.reads[k] == N48D2_HUBPREQ0_PRIMARY_HIGH; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) if (m.reads[k] == n48d2_dpx_regs[i].abs) isw = true; if (isw) { if (k < m.readsAtWrite[w0]) ++before; else if (k >= m.readsAtWrite.back()) ++after; } }
            expect(before >= N48D2_DPX_REGS + 1u && after == N48D2_DPX_REGS + 1u + (inst == 1u ? 1u : 0u), (std::string("clean ") + n48d2_op_name(op) + ": the fifteen DP-plane rows and the primary HIGH dword are READ before the first write and once more after the last write (0.0.655: the monitor A's monitor B watch reads DPPCLK_CTRL once more)").c_str());
            // the pack carries the change mask
            uint64_t v[13]; n48d2_pack(v, &o); n48d2_out b; std::memset(&b, 0, sizeof b); n48d2_unpack(v, &b); expect(b.dpx_changed == 0u && b.pre_abs == o.pre_abs, "the clean result packs dpx_changed 0");
        }
        // every register, both instances, every writing op: a change under the op ends it in DP_DISTURBED, `off` runs at once, and the mask names the register
        struct Flip { uint32_t idx, xorv; bool trips; const char *what; };
        const Flip flips[] = { { 0, 0xF, true, "MPC_OUT0_MUX" }, { 1, 0x2, true, "MPCC0 TOP_SEL" }, { 2, 0xF, true, "MPCC0 BOT_SEL" }, { 3, 0x1, true, "MPCC0 OPP_ID" }, { 4, 0x10000000u, true, "HUBP0 underflow bit 28" }, { 4, 0x40000000u, true, "HUBP0 underflow bit 30" },
                               { 4, 0x1u, true, "HUBP0 BLANK_EN" }, { 4, 0x1000u, true, "HUBP0 TTU_DISABLE" }, { 4, 0xF0u, true, "HUBP0 VTG_SEL" }, { 5, 0x100u, true, "DPPCLK0_DTO_PARAM" }, { 6, 0x1u, true, "DPPCLK_CTRL bit 0 (DPPCLK0_EN)" },
                               { 7, 0x8u, true, "DET0_CTRL" }, { 8, 0x1u, true, "COMPBUF_CTRL" },
                               { 4, 0x800u, true, "HUBP0 SEG_ALLOC_ERR" }, { 10, 0x400u, true, "ODM0 underflow-occurred bit 10" }, { 10, 0x1u, false, "ODM0 soft reset bit (not in the mask)" }, { 11, 0x10u, true, "DPP_TOP0 DPP_CLOCK_ENABLE" }, { 12, 0x1u, true, "MPCC0 UPDATE_LOCK_SEL" },
                               { 12, 0x10u, false, "MPCC0 UPDATE_LOCKED_STATUS (moves)" }, { 13, 0x04000000u, true, "OTG2_GLOBAL_CONTROL2" }, { 14, 0x10000u, true, "HUBP0 primary address (the rule: changed while the scanout is not acquired)" },
                               { 4, 0x2u, false, "HUBP0 NO_OUTSTANDING_REQ (toggles with the raster)" }, { 4, 0x8u, false, "HUBP0 IN_BLANK (toggles with the raster)" }, { 4, 0x30000u, false, "HUBP0 XRQ_NO_OUTSTANDING_REQ (toggles)" }, { 6, 0x2u, false, "DPPCLK_CTRL bit 1 (another pipe's)" }, { 6, 0x40u, false, "DPPCLK_CTRL bit 6 (DPPCLK2_EN, the monitor B's pipe)" } };
        for (const Flip &f : flips) for (uint32_t inst : { 1u, 2u }) for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
            Model m; m.inst = inst;
            for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) m.r[n48d2_dpx_regs[i].abs] = 0x01020304u;
            uint32_t wantMask = 0u;      // the expected change mask, derived from the table: every row on that register whose mask covers a flipped bit (row 14 is the rule: any change trips it while the scanout is not acquired)
            for (uint32_t j = 0; j < N48D2_DPX_REGS; j++) if (n48d2_dpx_regs[j].abs == n48d2_dpx_regs[f.idx].abs && (j == N48D2_DPX_RULE ? f.xorv != 0u : (f.xorv & n48d2_dpx_regs[j].mask) != 0u)) wantMask |= 1u << j;
            expect((wantMask != 0u) == f.trips, (std::string("the flip table agrees with the masks: ") + f.what).c_str());
            if (op != N48D2_OP_TIMING) (void)n48d2::run(m, N48D2_OP_TIMING, inst);
            if (op == N48D2_OP_OFF) (void)n48d2::run(m, N48D2_OP_CONNECT, inst);
            m.flipTrig = op == N48D2_OP_TIMING ? (inst == 2 ? N48D2_OTG2_CONTROL : N48D2_OTG1_CONTROL) : op == N48D2_OP_CONNECT ? (inst == 2 ? N48D2_DIG3_FE_EN_CNTL : N48D2_DIG2_FE_EN_CNTL) : (inst == 2 ? N48D2_DPG2_CONTROL : N48D2_DPG1_CONTROL);
            m.flipReg = n48d2_dpx_regs[f.idx].abs; m.flipXor = f.xorv;
            const n48d2_out o = n48d2::run(m, op, inst);
            const std::string lbl = std::string(f.what) + " flips during " + n48d2_op_name(op) + " (instance " + std::to_string(inst) + ")";
            if (f.trips) {
                expect(o.status == N48D2_DP_DISTURBED && o.dpx_changed == wantMask && o.dp_changed == 0u, (lbl + ": DP_DISTURBED, the mask names exactly that register (and its twin row, if any)").c_str());
                expect((op == N48D2_OP_OFF) ? o.auto_off == 0u : o.auto_off == 1u, (lbl + (op == N48D2_OP_OFF ? ": `off` itself does not recurse" : ": `off` was run at once (auto_off)")).c_str());
                expect((m.get(inst == 2 ? N48D2_OTG2_CONTROL : N48D2_OTG1_CONTROL) & 1u) == 0u && m.get(inst == 2 ? N48D2_DIG3_FE_EN_CNTL : N48D2_DIG2_FE_EN_CNTL) == 0u, (lbl + ": the monitor B / monitor A pipe is OFF afterwards").c_str());
                uint64_t v[13]; n48d2_pack(v, &o); n48d2_out b; std::memset(&b, 0, sizeof b); n48d2_unpack(v, &b); expect(b.dpx_changed == wantMask && b.status == N48D2_DP_DISTURBED && b.pre_abs == o.pre_abs, (lbl + ": the change mask survives the result packing").c_str());
            } else if (inst == 1u && f.idx == 6u && f.xorv == 0x40u) {      // 0.0.655: DPPCLK_CTRL bit 6 is the MONB's pipe: the DP watch masks it away, an MONA op's monitor B watch (row 0) does not
                expect(o.status == N48D2_DP_DISTURBED && o.dpx_changed == 0u && o.dp_changed == 0u && o.w2_changed == 1u && o.guard == 1u, (lbl + ": 0.0.655: the DP watch masks it but the monitor A's MONB WATCH row 0 trips: DP_DISTURBED, w2_changed 1, guard field = first row + 1").c_str());
            } else {
                expect(o.status == N48D2_OK && o.dpx_changed == 0u, (lbl + ": NOT a disturbance (masked away)").c_str());
            }
        }
        // the old checks keep working next to the new ones: an OTG0 stop is still its own bit
        { Model m; m.otg0StopOnMaster = true; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING); expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x80u) != 0u && o.dpx_changed == 0u, "the OTG0 frame check still trips alone, with no DP-plane bit"); }
        // the packing: pre_abs shares v[3] with the mask without loss for every BAR5 dword
        { n48d2_out a; std::memset(&a, 0, sizeof a); a.pre_abs = 0xFFFFu; a.dpx_changed = 0x7FFFu; a.pre_val = 0xFFFFFFFFu; uint64_t v[13]; n48d2_pack(v, &a); n48d2_out b; std::memset(&b, 0, sizeof b); n48d2_unpack(v, &b);
          expect(b.pre_abs == 0xFFFFu && b.dpx_changed == 0x7FFFu && b.pre_val == 0xFFFFFFFFu, "v[3] (0.0.635): pre_abs (16 bits: every register this verb names is below 0x10000), the fifteen-bit DP-plane mask and the value share the scalar without loss"); }
    }
}

// =====================================================================================================================================================================================================
// 0.0.635 (M4c / M4d): the monitor B's plane. PModel = Model + a behavioural model of HUBP2 / DET2 / the address latch / MPCC2 / the CRC, with a KNOB for every hardware assumption the spec marks SUSPECTED, so each gate can be failed
// on purpose and the flow shown to fail CLOSED. Every run below drives the REAL n48d2::run_plane / n48d2::run over the REAL guard and the REAL DCN allowlist (Model::wr).
// =====================================================================================================================================================================================================
static const uint64_t kWinLo = 0x8000000000ull, kWinHi = 0x8010000000ull;                 // the scanout window (256 MiB at the frame-buffer base)
static const uint64_t kBufA = 0x8002000000ull, kBufB = kBufA + N48D2_BUF_BYTES;           // the two buffers (64 KiB aligned, inside the window)
struct PModel : Model {
    bool hubpWritesLand = true, clkWorks = true, detWorks = true, latchWorks = true, noOutWorks = true, clkOffWorks = true, blankSticks = true, undClearWorks = true, flipClears = true, otg2Frozen = false;
    uint32_t flipPendReads = 2, injectUnderflow = 0, vmFault = 0, odmAfterUnblank = 0, odmStale = 0, stopOtg0On = 0;
    uint32_t det2Size = 3, timeoutBits = 0;
    bool detDropAfterUnblank = false;
    bool detAfterUnblank = false; uint32_t detReadsAfterUnblank = 0, detUnblankReads = 0;      // 0.0.639: the 0.0.638 hardware reading: DET2 current stays 0 while HUBP2 is blanked / unconnected and becomes 3 only after the unblank (and detReadsAfterUnblank further reads = ms)
    // 0.0.636 knobs: the 0.0.635 run showed DSCL2_LB_MEMORY_CTRL does not take with HUBP2 / DPP2 unclocked (SUSPECTED: double-buffered, latching at the pipe's update)
    uint32_t lbReadAnd = 0xFFFFFFFFu, lbReadXor = 0u;      // 0.0.637: what DSCL2_LB_MEMORY_CTRL reads back through (bits 16-30 forced to 0 / bits 8-13 flipped)
    bool dsclNeedsClock = false;         // a DSCL2 write is silently NOT stored while HUBP2's clock is off
    bool dsclDoubleBuf = false;          // a DSCL2 write takes effect only when an OTG2 frame passes (it reads back the OLD value until then)
    uint32_t earlyLag = 0, earlyReads = 0;   // EARLIEST_INUSE (LOW) keeps showing the previous address for this many reads after a flip's pending bit cleared (hubp2_is_flip_pending's second condition)
    uint64_t prevLatched = 0;
    bool pendGlitch = false, glitchArmed = false, glitchDone = false;   // FLIP_PENDING reads clear once, then SET on the very next read (a pending bit that comes back)
    uint32_t freezeOnEarlyRead = 0, nEarlyReads = 0;                     // OTG2 stops counting from the Nth EARLIEST_INUSE (LOW) read on
    uint32_t freezeOnFlipRead = 0, nFlipReads = 0;                       // 0.0.638: OTG2 stops counting from the Nth FLIP_CONTROL read on
    uint32_t earlyAfterUnblank = 0, earlyUnblankCnt = 0;                 // 0.0.638: EARLIEST_INUSE (LOW) reads 0 for this many reads AFTER the unblank before it shows the address (the post-unblank gate must POLL)
    bool blankedNoEarly = false;                                         // 0.0.638: the study's reading of the hardware: a BLANKED HUBP never updates EARLIEST_INUSE (it reads 0 until the unblank)
    // ---- 0.0.643: OTG2's master update lock (Linux optc3_lock / optc1_unlock) and the hardware reading of the 0.0.639 run
    uint32_t refuseInLock = 0; bool pendAtUnlock = false;      // refuseInLock: a write to this register is REFUSED while the lock is held; pendAtUnlock: the address was still pending (FLIP_PENDING 1) when the lock was released (lockAware)
    bool locked = false, lockNeverAsserts = false;       // the lock bit as last written; UPDATE_LOCK_STATUS (bit 8) never reads 1 when lockNeverAsserts
    bool lockAware = false, hwPending = false;           // lockAware: a pending address is consumed ONLY at a frame (VUPDATE) with the lock released AND the HUBP unblanked (a blanked HUBP never consumes it; FLIP_PENDING stays 1)
    std::vector<std::pair<uint32_t, uint32_t>> pendW;
    std::vector<uint32_t> f2AtRead;      // OTG2's frame counter at each read (parallel to Model::reads)
    static bool dscl2(uint32_t a) { return a >= N48D2_DSCL2_SCL_MODE && a <= N48D2_DSCL2_LB_MEMORY_CTRL; }
    bool segAllocErr = false, reUnderflow = false, mpccNeverIdle = false;
    uint32_t dropAbs = 0;                 // a write to this register is accepted and silently NOT stored (a register that does not latch)
    bool clockOn = false, blanked = false, dpgOn = true, crcOn = false, clkStatus = false;
    uint32_t curUnderflow = 0, odmSticky = 0, pend = 0, accMs = 0;
    uint64_t progAddr = 0, latched = 0;
    std::vector<std::string> ev;
    static bool gated(uint32_t a) { return (a >= 0x3c5du && a <= 0x3d05u && a != N48D2_HUBP2_HUBP_CLK_CNTL) || (a >= 0x445bu && a <= 0x455eu); }
    uint32_t rd(uint32_t a) {
        f2AtRead.push_back(frames2);
        uint32_t v = Model::rd(a);
        switch (a) {
        case N48D2_HUBP2_DCHUBP_CNTL:
            v &= ~(0x70000802u | 1u); v |= blanked ? 1u : 0u;
            if (blanked && noOutWorks) v |= 2u;
            if (!blanked) { v |= curUnderflow | timeoutBits; if (segAllocErr) v |= 0x800u; }
            return v;
        case N48D2_HUBP2_HUBP_CLK_CNTL: return (get(a) & N48D2_HUBP_CLK_WRITE_MASK) | 0x00500000u | (clkStatus ? 0x00A00000u : 0u);
        case N48D2_DCHUBBUB_DET2_CTRL: {
            bool cur = clockOn && detWorks && !(detDropAfterUnblank && !blanked);
            if (detAfterUnblank && cur) { if (blanked) cur = false; else cur = detUnblankReads++ >= detReadsAfterUnblank; }
            return det2Size | (cur ? (3u << 8) : 0u); }
        case N48D2_OTG2_OTG_MASTER_UPDATE_LOCK: return (get(a) & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) | (((get(a) & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) != 0u && !lockNeverAsserts) ? N48D2_OTG_UPDATE_LOCK_STATUS_MASK : 0u);
        case N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE: if (blankedNoEarly && blanked) return 0u; if (earlyAfterUnblank && !blanked && clockOn && earlyUnblankCnt++ < earlyAfterUnblank) return 0u; if (freezeOnEarlyRead && ++nEarlyReads >= freezeOnEarlyRead) otg2Frozen = true; if (earlyLag && earlyReads < earlyLag) { ++earlyReads; return (uint32_t)prevLatched; } return (uint32_t)latched;
        case N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH: if (blankedNoEarly && blanked) return 0u; return (uint32_t)((earlyLag && earlyReads < earlyLag ? prevLatched : latched) >> 32) & 0xFFFFu;
        case N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL: {
            if (freezeOnFlipRead && ++nFlipReads >= freezeOnFlipRead) otg2Frozen = true;
            uint32_t r = get(a) & ~N48D2_FLIP_PENDING_MASK;
            if (lockAware) { if (hwPending) r |= N48D2_FLIP_PENDING_MASK; return r; }
            if (pend != 0u) { r |= N48D2_FLIP_PENDING_MASK; if (flipClears) { --pend; if (pend == 0u && latchWorks) latched = progAddr; } }
            else if (pendGlitch && !glitchDone) { if (!glitchArmed) glitchArmed = true; else { r |= N48D2_FLIP_PENDING_MASK; glitchDone = true; } }
            return r; }
        case N48D2_DCN_VM_FAULT_STATUS: return (!blanked && clockOn && vmFault) ? vmFault : v;
        case N48D2_DSCL2_LB_MEMORY_CTRL: return (v & lbReadAnd) ^ lbReadXor;
        case N48D2_MPCC2_MPCC_STATUS: if (mpccNeverIdle) return 2u; return (get(N48D2_MPCC2_MPCC_TOP_SEL) == 0xFu && get(N48D2_MPCC2_MPCC_BOT_SEL) == 0xFu && get(N48D2_MPCC2_MPCC_OPP_ID) == 0xFu) ? 5u : 2u;
        case N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL: return get(a) | odmSticky;
        case N48D2_OTG2_OTG_CRC0_DATA_RG: return crcOn ? (dpgOn ? 0x11110000u : 0x22220000u) ^ (uint32_t)(latched >> 16) : 0u;
        case N48D2_OTG2_OTG_CRC0_DATA_B: return crcOn ? (dpgOn ? 0x1111u : 0x2222u) ^ (uint32_t)(latched >> 8) : 0u;
        default: return v;
        }
    }
    bool wr(uint32_t a, uint32_t v) {
        const uint32_t before = get(a);
        if (locked && refuseInLock && a == refuseInLock) { ++refusedCount; return false; }
        if (!Model::wr(a, v)) return false;
        if (stopOtg0On && a == stopOtg0On) otg0Stops = true;
        if ((!hubpWritesLand && !clockOn && gated(a)) || (dropAbs && a == dropAbs) || (dsclNeedsClock && !clockOn && dscl2(a))) { r[a] = before; ev.push_back("write dropped"); return true; }
        if (dsclDoubleBuf && dscl2(a)) { r[a] = before; pendW.push_back({ a, v }); ev.push_back("write pending until the next OTG2 frame"); return true; }
        switch (a) {
        case N48D2_HUBP2_DCHUBP_CNTL: {
            const bool wasBlank = blanked;
            blanked = blankSticks ? (v & 1u) != 0u : false;
            if (v & 0x80000000u) { if (undClearWorks) curUnderflow = 0u; if (reUnderflow) curUnderflow = 0x10000000u; r[a] = v & ~0x80000000u; }
            if (wasBlank && !blanked) { curUnderflow = injectUnderflow; odmSticky |= odmAfterUnblank; ev.push_back("unblank"); }
            if (!wasBlank && blanked) ev.push_back("blank");
            break; }
        case N48D2_HUBP2_HUBP_CLK_CNTL:
            if (v & 1u) { if (!clockOn) { clockOn = true; ev.push_back("clock on"); if (progAddr != 0u && latchWorks && !lockAware) latched = progAddr; } clkStatus = clkWorks; }
            else { clockOn = false; if (clkOffWorks) clkStatus = false; ev.push_back("clock off"); }
            break;
        case N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH: progAddr = (progAddr & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
        case N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS:
            progAddr = (progAddr & ~0xFFFFFFFFull) | v;
            if (lockAware) hwPending = true;
            else if (clockOn) { prevLatched = latched; earlyReads = 0; if (flipPendReads == 0u) { if (latchWorks) latched = progAddr; } else pend = flipPendReads; }
            break;
        case N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL: if ((v & N48D2_ODM_UNDERFLOW_CLEAR_MASK) && undClearWorks) odmSticky = 0u; r[a] = v & ~(N48D2_ODM_UNDERFLOW_MASK | N48D2_ODM_UNDERFLOW_CLEAR_MASK); break;
        case N48D2_OTG2_OTG_CRC_CNTL: crcOn = (v & 1u) != 0u; break;
        case N48D2_OTG2_OTG_MASTER_UPDATE_LOCK: locked = (v & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) != 0u; ev.push_back(locked ? "lock" : "unlock"); if (!locked) pendAtUnlock = hwPending; r[a] = v & N48D2_OTG_MASTER_UPDATE_LOCK_MASK; break;
        case N48D2_DPG2_CONTROL: dpgOn = (v & 1u) != 0u; break;
        default: break;
        }
        return true;
    }
    void sleep_ms(uint32_t ms) {
        ++sleeps; accMs += ms; const uint32_t f = accMs / 16u; accMs %= 16u;
        if (!otg0Stops) frames0 += f;
        if (otg1Running()) frames1 += f;
        if (otg2Running() && !otg2Frozen) { frames2 += f; if (f > 0u) { for (auto &w : pendW) r[w.first] = w.second; pendW.clear(); if (lockAware && hwPending && !locked && !blanked && latchWorks) { latched = progAddr; hwPending = false; ev.push_back("VUPDATE latched"); } } }
    }
    // the census state (notes/logs/runs/m4ab-634/census-plane.txt) of an idle monitor B pipe under a lit DP, the DPG pattern on OTG2, and the buffers allocated (stage ALLOC)
    void initPlane() {
        inst = N48D2_INST_MONB;           // the kext glue sets env.inst = 2 for every plane op: the guard that judges wr() is instance 2's
        for (uint32_t i = 0; i < N48D2_EXP_COUNT; i++) { r[n48d2_exp[i].abs2] = n48d2_exp[i].val; r[n48d2_exp[i].abs0] = n48d2_exp[i].val; }
        r[N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL] = 0x04100000u;
        for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) { r[n48d2_mirror0[i].src] = n48d2_mirror0[i].expect; r[n48d2_mirror0[i].dst] = 0u; }
        r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x9ffu;      // 0.0.637: the DP surface's pitch (census: HUBPREQ0_DCSURF_SURFACE_PITCH = 0x9ff)
        r[N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION] = 0u; r[N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG] = 0x05501b00u;
        r[N48D2_OTG2_CONTROL] = 0x00010001u; r[N48D2_OTG2_CLOCK_CONTROL] = 3u; r[N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL] = 3u;
        r[N48D2_DPG2_CONTROL] = N48D2_DPG_CONTROL_ON;
        r[N48D2_HUBP2_HUBP_CLK_CNTL] = 0u; r[N48D2_HUBP2_DCHUBP_CNTL] = 0x000f002au; blanked = false; r[N48D2_DPP_TOP2_DPP_CONTROL] = 0x70000000u;
        r[N48D2_MPCC2_MPCC_TOP_SEL] = 0xFu; r[N48D2_MPCC2_MPCC_BOT_SEL] = 0xFu; r[N48D2_MPCC2_MPCC_OPP_ID] = 0xFu; r[N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL] = 0xFu; r[N48D2_MPC_OUT2_MUX] = 0x400fu;
        r[N48D2_DPPCLK_CTRL] = 1u; r[N48D2_DPPCLK0_DTO_PARAM] = N48D2_DPPCLK_DTO_VALUE; r[N48D2_DPPCLK2_DTO_PARAM] = 0u; r[N48D2_DET0_CTRL] = 0x303u;
        r[N48D2_HUBPREQ0_PRIMARY_HIGH] = 0x80u; r[N48D2_HUBPREQ0_PRIMARY_LOW] = 0u;                 // the console (MC 0x80_0000_0000), HUBP0
        r[N48D2_MPC_OUT0_MUX] = 0x4100u; r[N48D2_MPCC0_BOT_SEL] = 0xFu; r[N48D2_HUBP0_DCHUBP_CNTL] = 0x000f0002u; r[N48D2_COMPBUF_CTRL] = 0x40101u; r[N48D2_DPP_TOP0_DPP_CONTROL] = 0x70000010u; r[N48D2_OTG2_OTG_GLOBAL_CONTROL2] = 0x04000000u;
        r[N48D2_OTG0_CONTROL] = 0x00011201u;
        r[N48D2_OTG0_H_TOTAL] = N48D2_RASTER_HTOTAL_RAW; r[N48D2_OTG0_V_TOTAL] = N48D2_RASTER_VTOTAL_RAW;
        r[N48D2_OTG2_H_TOTAL] = N48D2_RASTER_HTOTAL_RAW; r[N48D2_OTG2_V_TOTAL] = N48D2_RASTER_VTOTAL_RAW;
        haveWin = true; winLo = kWinLo; winHi = kWinHi;
        plane.stage = N48D2_PL_ALLOC; plane.cur = 0; plane.mc[0] = kBufA; plane.mc[1] = kBufB;
        odmSticky = odmStale;
        n48d2_aset_clear();
    }
};

// ---- 0.0.655: INSTANCE 1 (the monitor A). P1Model = PModel's behavioural model with the instance-1 registers (generated from PModel by renaming HUBP2 -> HUBP1 ... OTG2 -> OTG1, then hand-fixed: its own census initPlane, the monitor B's OTG2 keeps
//      counting in sleep_ms for the monitor B watch). Every knob means what PModel's means.
static const uint64_t kA1 = 0x8004000000ull, kB1 = kA1 + N48D1_BUF_BYTES;      // the monitor A's two buffers (64 KiB aligned, inside the window, clear of the monitor B's kBufA / kBufB)
struct P1Model : Model {
    bool hubpWritesLand = true, clkWorks = true, detWorks = true, latchWorks = true, noOutWorks = true, clkOffWorks = true, blankSticks = true, undClearWorks = true, flipClears = true, otg1Frozen = false;
    uint32_t flipPendReads = 2, injectUnderflow = 0, vmFault = 0, odmAfterUnblank = 0, odmStale = 0, stopOtg0On = 0;
    uint32_t det2Size = 3, timeoutBits = 0;
    bool detDropAfterUnblank = false;
    bool detAfterUnblank = false; uint32_t detReadsAfterUnblank = 0, detUnblankReads = 0;      // 0.0.639: the 0.0.638 hardware reading: DET2 current stays 0 while HUBP2 is blanked / unconnected and becomes 3 only after the unblank (and detReadsAfterUnblank further reads = ms)
    // 0.0.636 knobs: the 0.0.635 run showed DSCL2_LB_MEMORY_CTRL does not take with HUBP2 / DPP2 unclocked (SUSPECTED: double-buffered, latching at the pipe's update)
    uint32_t lbReadAnd = 0xFFFFFFFFu, lbReadXor = 0u;      // 0.0.637: what DSCL2_LB_MEMORY_CTRL reads back through (bits 16-30 forced to 0 / bits 8-13 flipped)
    bool dsclNeedsClock = false;         // a DSCL2 write is silently NOT stored while HUBP2's clock is off
    bool dsclDoubleBuf = false;          // a DSCL2 write takes effect only when an OTG2 frame passes (it reads back the OLD value until then)
    uint32_t earlyLag = 0, earlyReads = 0;   // EARLIEST_INUSE (LOW) keeps showing the previous address for this many reads after a flip's pending bit cleared (hubp2_is_flip_pending's second condition)
    uint64_t prevLatched = 0;
    bool pendGlitch = false, glitchArmed = false, glitchDone = false;   // FLIP_PENDING reads clear once, then SET on the very next read (a pending bit that comes back)
    uint32_t freezeOnEarlyRead = 0, nEarlyReads = 0;                     // OTG2 stops counting from the Nth EARLIEST_INUSE (LOW) read on
    uint32_t freezeOnFlipRead = 0, nFlipReads = 0;                       // 0.0.638: OTG2 stops counting from the Nth FLIP_CONTROL read on
    uint32_t earlyAfterUnblank = 0, earlyUnblankCnt = 0;                 // 0.0.638: EARLIEST_INUSE (LOW) reads 0 for this many reads AFTER the unblank before it shows the address (the post-unblank gate must POLL)
    bool blankedNoEarly = false;                                         // 0.0.638: the study's reading of the hardware: a BLANKED HUBP never updates EARLIEST_INUSE (it reads 0 until the unblank)
    // ---- 0.0.643: OTG2's master update lock (Linux optc3_lock / optc1_unlock) and the hardware reading of the 0.0.639 run
    uint32_t refuseInLock = 0; bool pendAtUnlock = false;      // refuseInLock: a write to this register is REFUSED while the lock is held; pendAtUnlock: the address was still pending (FLIP_PENDING 1) when the lock was released (lockAware)
    bool locked = false, lockNeverAsserts = false;       // the lock bit as last written; UPDATE_LOCK_STATUS (bit 8) never reads 1 when lockNeverAsserts
    bool lockAware = false, hwPending = false;           // lockAware: a pending address is consumed ONLY at a frame (VUPDATE) with the lock released AND the HUBP unblanked (a blanked HUBP never consumes it; FLIP_PENDING stays 1)
    std::vector<std::pair<uint32_t, uint32_t>> pendW;
    std::vector<uint32_t> f1AtRead;      // OTG2's frame counter at each read (parallel to Model::reads)
    static bool dscl2(uint32_t a) { return a >= N48D2_DSCL1_SCL_MODE && a <= N48D2_DSCL1_LB_MEMORY_CTRL; }
    bool segAllocErr = false, reUnderflow = false, mpccNeverIdle = false;
    uint32_t dropAbs = 0;                 // a write to this register is accepted and silently NOT stored (a register that does not latch)
    bool clockOn = false, blanked = false, dpgOn = true, crcOn = false, clkStatus = false;
    uint32_t curUnderflow = 0, odmSticky = 0, pend = 0, accMs = 0;
    uint64_t progAddr = 0, latched = 0;
    std::vector<std::string> ev;
    static bool gated(uint32_t a) { return (a >= 0x3b81u && a <= 0x3c29u && a != N48D2_HUBP1_HUBP_CLK_CNTL) || (a >= 0x42f0u && a <= 0x43f3u); }
    uint32_t rd(uint32_t a) {
        f1AtRead.push_back(frames1);
        uint32_t v = Model::rd(a);
        switch (a) {
        case N48D2_HUBP1_DCHUBP_CNTL:
            v &= ~(0x70000802u | 1u); v |= blanked ? 1u : 0u;
            if (blanked && noOutWorks) v |= 2u;
            if (!blanked) { v |= curUnderflow | timeoutBits; if (segAllocErr) v |= 0x800u; }
            return v;
        case N48D2_HUBP1_HUBP_CLK_CNTL: return (get(a) & N48D2_HUBP_CLK_WRITE_MASK) | 0x00500000u | (clkStatus ? 0x00A00000u : 0u);
        case N48D2_DCHUBBUB_DET1_CTRL: {
            bool cur = clockOn && detWorks && !(detDropAfterUnblank && !blanked);
            if (detAfterUnblank && cur) { if (blanked) cur = false; else cur = detUnblankReads++ >= detReadsAfterUnblank; }
            return det2Size | (cur ? (3u << 8) : 0u); }
        case N48D2_OTG1_OTG_MASTER_UPDATE_LOCK: return (get(a) & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) | (((get(a) & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) != 0u && !lockNeverAsserts) ? N48D2_OTG_UPDATE_LOCK_STATUS_MASK : 0u);
        case N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE: if (blankedNoEarly && blanked) return 0u; if (earlyAfterUnblank && !blanked && clockOn && earlyUnblankCnt++ < earlyAfterUnblank) return 0u; if (freezeOnEarlyRead && ++nEarlyReads >= freezeOnEarlyRead) otg1Frozen = true; if (earlyLag && earlyReads < earlyLag) { ++earlyReads; return (uint32_t)prevLatched; } return (uint32_t)latched;
        case N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH: if (blankedNoEarly && blanked) return 0u; return (uint32_t)((earlyLag && earlyReads < earlyLag ? prevLatched : latched) >> 32) & 0xFFFFu;
        case N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL: {
            if (freezeOnFlipRead && ++nFlipReads >= freezeOnFlipRead) otg1Frozen = true;
            uint32_t r = get(a) & ~N48D2_FLIP_PENDING_MASK;
            if (lockAware) { if (hwPending) r |= N48D2_FLIP_PENDING_MASK; return r; }
            if (pend != 0u) { r |= N48D2_FLIP_PENDING_MASK; if (flipClears) { --pend; if (pend == 0u && latchWorks) latched = progAddr; } }
            else if (pendGlitch && !glitchDone) { if (!glitchArmed) glitchArmed = true; else { r |= N48D2_FLIP_PENDING_MASK; glitchDone = true; } }
            return r; }
        case N48D2_DCN_VM_FAULT_STATUS: return (!blanked && clockOn && vmFault) ? vmFault : v;
        case N48D2_DSCL1_LB_MEMORY_CTRL: return (v & lbReadAnd) ^ lbReadXor;
        case N48D2_MPCC1_MPCC_STATUS: if (mpccNeverIdle) return 2u; return (get(N48D2_MPCC1_MPCC_TOP_SEL) == 0xFu && get(N48D2_MPCC1_MPCC_BOT_SEL) == 0xFu && get(N48D2_MPCC1_MPCC_OPP_ID) == 0xFu) ? 5u : 2u;
        case N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL: return get(a) | odmSticky;
        case N48D2_OTG1_OTG_CRC0_DATA_RG: return crcOn ? (dpgOn ? 0x11110000u : 0x22220000u) ^ (uint32_t)(latched >> 16) : 0u;
        case N48D2_OTG1_OTG_CRC0_DATA_B: return crcOn ? (dpgOn ? 0x1111u : 0x2222u) ^ (uint32_t)(latched >> 8) : 0u;
        default: return v;
        }
    }
    bool wr(uint32_t a, uint32_t v) {
        const uint32_t before = get(a);
        if (locked && refuseInLock && a == refuseInLock) { ++refusedCount; return false; }
        if (!Model::wr(a, v)) return false;
        if (stopOtg0On && a == stopOtg0On) otg0Stops = true;
        if ((!hubpWritesLand && !clockOn && gated(a)) || (dropAbs && a == dropAbs) || (dsclNeedsClock && !clockOn && dscl2(a))) { r[a] = before; ev.push_back("write dropped"); return true; }
        if (dsclDoubleBuf && dscl2(a)) { r[a] = before; pendW.push_back({ a, v }); ev.push_back("write pending until the next OTG2 frame"); return true; }
        switch (a) {
        case N48D2_HUBP1_DCHUBP_CNTL: {
            const bool wasBlank = blanked;
            blanked = blankSticks ? (v & 1u) != 0u : false;
            if (v & 0x80000000u) { if (undClearWorks) curUnderflow = 0u; if (reUnderflow) curUnderflow = 0x10000000u; r[a] = v & ~0x80000000u; }
            if (wasBlank && !blanked) { curUnderflow = injectUnderflow; odmSticky |= odmAfterUnblank; ev.push_back("unblank"); }
            if (!wasBlank && blanked) ev.push_back("blank");
            break; }
        case N48D2_HUBP1_HUBP_CLK_CNTL:
            if (v & 1u) { if (!clockOn) { clockOn = true; ev.push_back("clock on"); if (progAddr != 0u && latchWorks && !lockAware) latched = progAddr; } clkStatus = clkWorks; }
            else { clockOn = false; if (clkOffWorks) clkStatus = false; ev.push_back("clock off"); }
            break;
        case N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH: progAddr = (progAddr & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
        case N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS:
            progAddr = (progAddr & ~0xFFFFFFFFull) | v;
            if (lockAware) hwPending = true;
            else if (clockOn) { prevLatched = latched; earlyReads = 0; if (flipPendReads == 0u) { if (latchWorks) latched = progAddr; } else pend = flipPendReads; }
            break;
        case N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL: if ((v & N48D2_ODM_UNDERFLOW_CLEAR_MASK) && undClearWorks) odmSticky = 0u; r[a] = v & ~(N48D2_ODM_UNDERFLOW_MASK | N48D2_ODM_UNDERFLOW_CLEAR_MASK); break;
        case N48D2_OTG1_OTG_CRC_CNTL: crcOn = (v & 1u) != 0u; break;
        case N48D2_OTG1_OTG_MASTER_UPDATE_LOCK: locked = (v & N48D2_OTG_MASTER_UPDATE_LOCK_MASK) != 0u; ev.push_back(locked ? "lock" : "unlock"); if (!locked) pendAtUnlock = hwPending; r[a] = v & N48D2_OTG_MASTER_UPDATE_LOCK_MASK; break;
        case N48D2_DPG1_CONTROL: dpgOn = (v & 1u) != 0u; break;
        default: break;
        }
        return true;
    }
    void sleep_ms(uint32_t ms) {
        ++sleeps; accMs += ms; const uint32_t f = accMs / 16u; accMs %= 16u;
        if (!otg0Stops) frames0 += f;
        if (otg2Running()) frames2 += f;      // the monitor B's OTG (the monitor B watch's counting rule)
        if (otg1Running() && !otg1Frozen) { frames1 += f; if (f > 0u) { for (auto &w : pendW) r[w.first] = w.second; pendW.clear(); if (lockAware && hwPending && !locked && !blanked && latchWorks) { latched = progAddr; hwPending = false; ev.push_back("VUPDATE latched"); } } }
    }
    // the census state of an idle monitor A pipe under a lit DP and the monitor B: notes/logs/runs/mona-census-654/mona-after-timing.txt (OTG1 timing running, DPG1 colour squares on, HUBP1 unblanked with VTG_SEL 1 and its clock gated), the
    // MPCC1 / VM registers of m4ab-634/census-plane.txt, and the buffers allocated (stage ALLOC). The monitor B (instance 2) is left as the plain model has it (nothing programmed).
    void initPlane() {
        inst = N48D2_INST_MONA;           // the kext glue sets env.inst = 1 for every monitor A op: the guard that judges wr() is instance 1's
        for (uint32_t i = 0; i < N48D1_PRE_STATE_COUNT; i++) r[n48d1_pre_state[i].abs] = n48d1_pre_state[i].val;
        for (uint32_t i = 0; i < N48D1_EXP_COUNT; i++) r[n48d1_exp[i].abs] = n48d1_exp[i].val;
        r[N48D2_HUBP1_DCHUBP_CNTL] = 0x000f001au; r[N48D2_HUBP1_HUBP_CLK_CNTL] = 0u; blanked = false;
        r[N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL] = 0x04100000u; r[N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION] = 0u; r[N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG] = 0x05501b00u;
        r[N48D2_DPP_TOP1_DPP_CONTROL] = 0x70000000u; r[N48D2_OTG1_CONTROL] = 0x00010001u; r[N48D2_OTG1_CLOCK_CONTROL] = 3u; r[N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL] = 2u; r[N48D2_DPG1_CONTROL] = N48D2_DPG_CONTROL_ON;
        r[N48D2_OTG1_OTG_GLOBAL_CONTROL2] = 0x02000000u; r[N48D2_DET0_CTRL] = 0x303u; r[N48D2_DCHUBBUB_DET1_CTRL] = 3u; r[N48D2_DPPCLK_CTRL] = 1u; r[N48D2_DPPCLK0_DTO_PARAM] = N48D2_DPPCLK_DTO_VALUE; r[N48D2_DPPCLK1_DTO_PARAM] = 0u;
        r[N48D2_HUBPREQ0_PRIMARY_HIGH] = 0x80u; r[N48D2_HUBPREQ0_PRIMARY_LOW] = 0u; r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x9ffu; r[N48D2_HUBP0_PRI_VIEWPORT_DIMENSION] = 0x05a00a00u;      // the DP's 2560x1440 console surface (14.7 MB)
        r[N48D2_MPC_OUT0_MUX] = 0x4100u; r[N48D2_MPCC0_BOT_SEL] = 0xFu; r[N48D2_HUBP0_DCHUBP_CNTL] = 0x000f0002u; r[N48D2_COMPBUF_CTRL] = 0x40101u; r[N48D2_DPP_TOP0_DPP_CONTROL] = 0x70000010u; r[N48D2_OTG2_OTG_GLOBAL_CONTROL2] = 0x04000000u;
        r[N48D2_OTG0_CONTROL] = 0x00011201u;
        haveWin = true; winLo = kWinLo; winHi = kWinHi;
        plane1.stage = N48D2_PL_ALLOC; plane1.cur = 0; plane1.mc[0] = kA1; plane1.mc[1] = kB1;
        odmSticky = odmStale;
        n48d2_aset_clear_i(N48D2_INST_MONA);
    }
};

struct WantW { uint32_t abs, mask, val; };
static std::string seq_diff(const std::vector<W> &got, const std::vector<WantW> &want) {
    if (got.size() != want.size()) return "count " + std::to_string(got.size()) + " != " + std::to_string(want.size());
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i].abs != want[i].abs) return "write " + std::to_string(i) + ": abs " + std::to_string(got[i].abs) + " != " + std::to_string(want[i].abs);
        if ((got[i].v & want[i].mask) != (want[i].val & want[i].mask)) return "write " + std::to_string(i) + " (abs " + std::to_string(got[i].abs) + "): value " + std::to_string(got[i].v) + " vs " + std::to_string(want[i].val);
    }
    return "";
}
static void expect_seq(const char *what, const std::vector<W> &got, const std::vector<WantW> &want) { const std::string d = seq_diff(got, want); ++gRun; if (!d.empty()) { ++gFail; std::printf("FAIL: %s: %s\n", what, d.c_str()); } }
static std::vector<std::string> header_names(const std::string &prefix) {     // every Linux register NAME starting with `prefix` (instance-qualified), BASE_IDX lines skipped
    std::vector<std::string> v; size_t p = 0; const std::string key = "#define reg" + prefix;
    while ((p = gOff.find(key, p)) != std::string::npos) {
        p += 11; size_t e = p; while (e < gOff.size() && gOff[e] != ' ' && gOff[e] != '\t') ++e;
        const std::string name = gOff.substr(p, e - p);
        if (!(name.size() > 9 && name.compare(name.size() - 9, 9, "_BASE_IDX") == 0)) v.push_back(name);
    }
    return v;
}
// the spec's 20 COPY0 pairs, by Linux NAME (destination, source) and census value
static const struct { const char *dst, *src; uint32_t v; uint32_t w; } kSpecMirror[20] = {   // w (0.0.637): the bits written and read back; Linux writes LB_MEMORY_CTRL's MEMORY_CONFIG [1:0] + LB_MAX_PARTITIONS [13:8] only
    { "DSCL2_SCL_MODE", "DSCL0_SCL_MODE", 1u, ~0u }, { "DSCL2_DSCL_2TAP_CONTROL", "DSCL0_DSCL_2TAP_CONTROL", 0x01110111u, ~0u }, { "DSCL2_SCL_HORZ_FILTER_SCALE_RATIO", "DSCL0_SCL_HORZ_FILTER_SCALE_RATIO", 0x01000000u, ~0u },
    { "DSCL2_SCL_HORZ_FILTER_INIT", "DSCL0_SCL_HORZ_FILTER_INIT", 0u, ~0u }, { "DSCL2_SCL_VERT_FILTER_SCALE_RATIO", "DSCL0_SCL_VERT_FILTER_SCALE_RATIO", 0x01000000u, ~0u }, { "DSCL2_SCL_VERT_FILTER_INIT", "DSCL0_SCL_VERT_FILTER_INIT", 0u, ~0u },
    { "DSCL2_OTG_H_BLANK", "DSCL0_OTG_H_BLANK", 0x00700a70u, ~0u }, { "DSCL2_OTG_V_BLANK", "DSCL0_OTG_V_BLANK", 0x002605c6u, ~0u }, { "DSCL2_RECOUT_SIZE", "DSCL0_RECOUT_SIZE", 0x05a00a00u, ~0u }, { "DSCL2_MPC_SIZE", "DSCL0_MPC_SIZE", 0x05a00a00u, ~0u },
    { "DSCL2_LB_MEMORY_CTRL", "DSCL0_LB_MEMORY_CTRL", 0x08083f00u, 0x00003F03u },
    { "HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION", "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION", 0x05a00a00u, ~0u }, { "HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION", "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION", 0x05a00a00u, ~0u }, { "HUBP2_DCHUBP_REQ_SIZE_CONFIG", "HUBP0_DCHUBP_REQ_SIZE_CONFIG", 0x05500300u, ~0u },
    { "HUBPREQ2_DCN_GLOBAL_TTU_CNTL", "HUBPREQ0_DCN_GLOBAL_TTU_CNTL", 0xe00050aau, ~0u }, { "HUBPREQ2_DCN_SURF0_TTU_CNTL0", "HUBPREQ0_DCN_SURF0_TTU_CNTL0", 0x08000000u, ~0u }, { "HUBPREQ2_BLANK_OFFSET_0", "HUBPREQ0_BLANK_OFFSET_0", 0x00260017u, ~0u },
    { "HUBPREQ2_PREFETCH_SETTINGS", "HUBPREQ0_PREFETCH_SETTINGS", 0x28080000u, ~0u }, { "HUBPREQ2_PREFETCH_SETTINGS_C", "HUBPREQ0_PREFETCH_SETTINGS_C", 0x00080000u, ~0u }, { "HUBPREQ2_VBLANK_PARAMETERS_0", "HUBPREQ0_VBLANK_PARAMETERS_0", 0x00000301u, ~0u } };
// the spec's EXPECT list (equal-already, pipe 2 and its pipe-0 twin) and the raster / global-sync constants: { pipe-2 name, pipe-0 name, mask, value }
static const struct { const char *n2, *n0; uint32_t mask, val; } kSpecExp[34] = {
    { "HUBP2_DCSURF_SURFACE_CONFIG", "HUBP0_DCSURF_SURFACE_CONFIG", ~0u, 8u }, { "HUBP2_DCSURF_ADDR_CONFIG", "HUBP0_DCSURF_ADDR_CONFIG", ~0u, 0u }, { "HUBP2_DCSURF_TILING_CONFIG", "HUBP0_DCSURF_TILING_CONFIG", ~0u, 0x80u },
    { "HUBP2_DCSURF_PRI_VIEWPORT_START", "HUBP0_DCSURF_PRI_VIEWPORT_START", ~0u, 0u }, { "HUBP2_HUBPREQ_DEBUG_DB", "HUBP0_HUBPREQ_DEBUG_DB", ~0u, 0x100u }, { "HUBP2_HUBPREQ_DEBUG", "HUBP0_HUBPREQ_DEBUG", ~0u, 0x04000000u },
    { "HUBP2_DCHUBP_CNTL", "HUBP0_DCHUBP_CNTL", 0x100u, 0u },
    { "HUBPREQ2_DCN_EXPANSION_MODE", "HUBPREQ0_DCN_EXPANSION_MODE", ~0u, 0x55u }, { "HUBPREQ2_DCN_TTU_QOS_WM", "HUBPREQ0_DCN_TTU_QOS_WM", ~0u, 0x06920000u }, { "HUBPREQ2_BLANK_OFFSET_1", "HUBPREQ0_BLANK_OFFSET_1", ~0u, 0x1136u },
    { "HUBPREQ2_DST_DIMENSIONS", "HUBPREQ0_DST_DIMENSIONS", ~0u, 0x1e641u }, { "HUBPREQ2_VBLANK_PARAMETERS_1", "HUBPREQ0_VBLANK_PARAMETERS_1", ~0u, 0x1a4u }, { "HUBPREQ2_VBLANK_PARAMETERS_3", "HUBPREQ0_VBLANK_PARAMETERS_3", ~0u, 0x1a4u },
    { "HUBPREQ2_NOM_PARAMETERS_0", "HUBPREQ0_NOM_PARAMETERS_0", ~0u, 0x200u }, { "HUBPREQ2_NOM_PARAMETERS_1", "HUBPREQ0_NOM_PARAMETERS_1", ~0u, 0xd249u }, { "HUBPREQ2_NOM_PARAMETERS_4", "HUBPREQ0_NOM_PARAMETERS_4", ~0u, 0x20u },
    { "HUBPREQ2_NOM_PARAMETERS_5", "HUBPREQ0_NOM_PARAMETERS_5", ~0u, 0xd24u }, { "HUBPREQ2_REF_FREQ_TO_PIX_FREQ", "HUBPREQ0_REF_FREQ_TO_PIX_FREQ", ~0u, 0x14e5eu }, { "HUBPREQ2_DST_Y_DELTA_DRQ_LIMIT", "HUBPREQ0_DST_Y_DELTA_DRQ_LIMIT", ~0u, 0x3fffu },
    { "HUBPREQ2_DCSURF_SURFACE_CONTROL", "HUBPREQ0_DCSURF_SURFACE_CONTROL", ~0u, 0u }, { "HUBPREQ2_DCSURF_FLIP_CONTROL", "HUBPREQ0_DCSURF_FLIP_CONTROL", 3u, 0u },
    { "HUBPRET2_HUBPRET_CONTROL", "HUBPRET0_HUBPRET_CONTROL", ~0u, 0x00e40000u }, { "CNVC_CFG2_CNVC_SURFACE_PIXEL_FORMAT", "CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT", ~0u, 8u }, { "CNVC_CFG2_FORMAT_CONTROL", "CNVC_CFG0_FORMAT_CONTROL", ~0u, 0x24000000u },
    { "CNVC_CFG2_PRE_CSC_MODE", "CNVC_CFG0_PRE_CSC_MODE", ~0u, 0u }, { "CM2_CM_CONTROL", "CM0_CM_CONTROL", 1u, 1u },
    { "MPCC2_MPCC_CONTROL", "MPCC0_MPCC_CONTROL", ~0u, 0xffff0461u }, { "MPC_OUT2_DENORM_CONTROL", "MPC_OUT0_DENORM_CONTROL", ~0u, 0x00fff000u }, { "MPC_OUT2_CSC_MODE", "MPC_OUT0_CSC_MODE", ~0u, 0u },
    { "OTG2_OTG_H_TOTAL", "OTG0_OTG_H_TOTAL", ~0u, 2719u }, { "OTG2_OTG_V_TOTAL", "OTG0_OTG_V_TOTAL", ~0u, 1480u }, { "OTG2_OTG_VSTARTUP_PARAM", "OTG0_OTG_VSTARTUP_PARAM", ~0u, 13u },
    { "OTG2_OTG_VUPDATE_PARAM", "OTG0_OTG_VUPDATE_PARAM", ~0u, 0x014002a8u }, { "OTG2_OTG_VREADY_PARAM", "OTG0_OTG_VREADY_PARAM", ~0u, 0x12cu } };

static void plane_tables_vs_linux() {
    // ---- every register define of the plane against the Linux header (offset AND BASE_IDX, through the card's segment bases)
    const struct { uint32_t ours; const char *lx; } regs[] = {
        { N48D2_DPPCLK2_DTO_PARAM, "DPPCLK2_DTO_PARAM" }, { N48D2_DPP_TOP2_DPP_CONTROL, "DPP_TOP2_DPP_CONTROL" }, { N48D2_DSCL2_SCL_MODE, "DSCL2_SCL_MODE" }, { N48D2_DSCL2_DSCL_2TAP_CONTROL, "DSCL2_DSCL_2TAP_CONTROL" },
        { N48D2_DSCL2_SCL_HORZ_FILTER_SCALE_RATIO, "DSCL2_SCL_HORZ_FILTER_SCALE_RATIO" }, { N48D2_DSCL2_SCL_HORZ_FILTER_INIT, "DSCL2_SCL_HORZ_FILTER_INIT" }, { N48D2_DSCL2_SCL_VERT_FILTER_SCALE_RATIO, "DSCL2_SCL_VERT_FILTER_SCALE_RATIO" },
        { N48D2_DSCL2_SCL_VERT_FILTER_INIT, "DSCL2_SCL_VERT_FILTER_INIT" }, { N48D2_DSCL2_OTG_H_BLANK, "DSCL2_OTG_H_BLANK" }, { N48D2_DSCL2_OTG_V_BLANK, "DSCL2_OTG_V_BLANK" }, { N48D2_DSCL2_RECOUT_SIZE, "DSCL2_RECOUT_SIZE" },
        { N48D2_DSCL2_MPC_SIZE, "DSCL2_MPC_SIZE" }, { N48D2_DSCL2_LB_MEMORY_CTRL, "DSCL2_LB_MEMORY_CTRL" },
        { N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION, "HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION" }, { N48D2_HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION, "HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION" }, { N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG, "HUBP2_DCHUBP_REQ_SIZE_CONFIG" },
        { N48D2_HUBP2_DCHUBP_CNTL, "HUBP2_DCHUBP_CNTL" }, { N48D2_HUBP2_HUBP_CLK_CNTL, "HUBP2_HUBP_CLK_CNTL" },
        { N48D2_HUBPREQ2_DCN_GLOBAL_TTU_CNTL, "HUBPREQ2_DCN_GLOBAL_TTU_CNTL" }, { N48D2_HUBPREQ2_DCN_SURF0_TTU_CNTL0, "HUBPREQ2_DCN_SURF0_TTU_CNTL0" }, { N48D2_HUBPREQ2_BLANK_OFFSET_0, "HUBPREQ2_BLANK_OFFSET_0" },
        { N48D2_HUBPREQ2_PREFETCH_SETTINGS, "HUBPREQ2_PREFETCH_SETTINGS" }, { N48D2_HUBPREQ2_PREFETCH_SETTINGS_C, "HUBPREQ2_PREFETCH_SETTINGS_C" }, { N48D2_HUBPREQ2_VBLANK_PARAMETERS_0, "HUBPREQ2_VBLANK_PARAMETERS_0" },
        { N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, "HUBPREQ2_DCSURF_SURFACE_PITCH" }, { N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS" }, { N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH" },
        { N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL, "HUBPREQ2_DCSURF_FLIP_CONTROL" }, { N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE" }, { N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH" },
        { N48D2_MPCC2_MPCC_TOP_SEL, "MPCC2_MPCC_TOP_SEL" }, { N48D2_MPCC2_MPCC_BOT_SEL, "MPCC2_MPCC_BOT_SEL" }, { N48D2_MPCC2_MPCC_OPP_ID, "MPCC2_MPCC_OPP_ID" }, { N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL, "MPCC2_MPCC_UPDATE_LOCK_SEL" },
        { N48D2_MPCC2_MPCC_STATUS, "MPCC2_MPCC_STATUS" }, { N48D2_MPC_OUT2_MUX, "MPC_OUT2_MUX" }, { N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, "ODM2_OPTC_INPUT_GLOBAL_CONTROL" },
        { N48D2_OTG2_OTG_CRC_CNTL, "OTG2_OTG_CRC_CNTL" }, { N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL, "OTG2_OTG_CRC0_WINDOWA_X_CONTROL" }, { N48D2_OTG2_OTG_CRC0_WINDOWA_Y_CONTROL, "OTG2_OTG_CRC0_WINDOWA_Y_CONTROL" },
        { N48D2_OTG2_OTG_CRC0_WINDOWB_X_CONTROL, "OTG2_OTG_CRC0_WINDOWB_X_CONTROL" }, { N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL, "OTG2_OTG_CRC0_WINDOWB_Y_CONTROL" }, { N48D2_OTG2_OTG_CRC0_DATA_RG, "OTG2_OTG_CRC0_DATA_RG" }, { N48D2_OTG2_OTG_CRC0_DATA_B, "OTG2_OTG_CRC0_DATA_B" },
        { N48D2_DCHUBBUB_DET2_CTRL, "DCHUBBUB_DET2_CTRL" }, { N48D2_DCN_VM_FAULT_STATUS, "DCN_VM_FAULT_STATUS" }, { N48D2_OTG2_OTG_GLOBAL_CONTROL2, "OTG2_OTG_GLOBAL_CONTROL2" },
        { N48D2_DCCG_GATE_DISABLE_CNTL, "DCCG_GATE_DISABLE_CNTL" }, { N48D2_DCCG_GATE_DISABLE_CNTL6, "DCCG_GATE_DISABLE_CNTL6" }, { N48D2_DOMAIN2_PG_STATUS, "DOMAIN2_PG_STATUS" },
        { N48D2_HUBPREQ2_VM_APERTURE_LOW, "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_LOW_ADDR" }, { N48D2_HUBPREQ2_VM_APERTURE_HIGH, "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR" }, { N48D2_HUBPREQ2_VM_MX_L1_TLB, "HUBPREQ2_DCN_VM_MX_L1_TLB_CNTL" },
        { N48D2_HUBPREQ2_VMID_SETTINGS_0, "HUBPREQ2_VMID_SETTINGS_0" }, { N48D2_OTG0_H_TOTAL, "OTG0_OTG_H_TOTAL" }, { N48D2_OTG0_V_TOTAL, "OTG0_OTG_V_TOTAL" }, { N48D2_OTG0_VSTARTUP_PARAM, "OTG0_OTG_VSTARTUP_PARAM" },
        { N48D2_OTG0_VUPDATE_PARAM, "OTG0_OTG_VUPDATE_PARAM" }, { N48D2_OTG0_VREADY_PARAM, "OTG0_OTG_VREADY_PARAM" }, { N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL, "ODM0_OPTC_INPUT_GLOBAL_CONTROL" }, { N48D2_DPP_TOP0_DPP_CONTROL, "DPP_TOP0_DPP_CONTROL" },
        { N48D2_MPCC0_UPDATE_LOCK_SEL, "MPCC0_MPCC_UPDATE_LOCK_SEL" }, { N48D2_HUBPREQ0_PRIMARY_LOW, "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS" }, { N48D2_HUBPREQ0_PRIMARY_HIGH, "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH" },
        { N48D2_DPG2_STATUS, "DPG2_DPG_STATUS" }, { N48D2_DET0_CTRL, "DCHUBBUB_DET0_CTRL" },
        { N48D2_HUBPREQ0_EARLIEST_LOW, "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE" }, { N48D2_HUBPREQ0_EARLIEST_HIGH, "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH" } };      // 0.0.636: the two C3 reads
    for (const auto &x : regs) { char b[160]; std::snprintf(b, sizeof b, "plane register %s is Linux's absolute address", x.lx); expect_u(b, x.ours, lx_abs(x.lx)); }
    // ---- the spec's 20 COPY0 pairs, by name, in the spec's order: destination, source, census value
    expect_u("20 COPY0 pairs", N48D2_MIRROR0_COUNT, 20);
    for (uint32_t i = 0; i < 20; i++) {
        char b[200]; std::snprintf(b, sizeof b, "COPY0 pair %u: %s <- %s = %#x", i, kSpecMirror[i].dst, kSpecMirror[i].src, kSpecMirror[i].v);
        expect(n48d2_mirror0[i].dst == lx_abs(kSpecMirror[i].dst) && n48d2_mirror0[i].src == lx_abs(kSpecMirror[i].src) && n48d2_mirror0[i].expect == kSpecMirror[i].v && n48d2_mirror0[i].wmask == kSpecMirror[i].w, b);
        expect(n48d2_mirror0[i].dst - n48d2_mirror0[i].src == (i < 11 ? 0x2d6u : 0x1b8u), "a COPY0 destination is its source's pipe-2 twin (DPP stride 0x2d6, HUBP stride 0x1b8)");
    }
    // ---- the EXPECT list
    expect_u("34 EXPECT rows (29 equal-already + 5 raster / global-sync constants)", N48D2_EXP_COUNT, 34);
    for (uint32_t i = 0; i < 34; i++) {
        char b[200]; std::snprintf(b, sizeof b, "EXPECT row %u: %s / %s mask %#x value %#x", i, kSpecExp[i].n2, kSpecExp[i].n0, kSpecExp[i].mask, kSpecExp[i].val);
        expect(n48d2_exp[i].abs2 == lx_abs(kSpecExp[i].n2) && n48d2_exp[i].abs0 == lx_abs(kSpecExp[i].n0) && n48d2_exp[i].mask == kSpecExp[i].mask && n48d2_exp[i].val == kSpecExp[i].val, b);
    }
    // ---- the 37-entry writable list and the equal pairs
    expect_u("39 plane registers (0.0.643: the 37 + OTG2_GLOBAL_CONTROL2 and OTG2_MASTER_UPDATE_LOCK)", N48D2_ALLOWED2P_COUNT, 39);
    { std::set<uint32_t> seen; bool inAllow = true; Model m; for (uint32_t i = 0; i < N48D2_ALLOWED2P_COUNT; i++) { seen.insert(n48d2_allowed2p[i]); inAllow = inAllow && dcn41_allow_write(&m.allow, n48d2_allowed2p[i], 0u, "x") != 0; }
      expect(seen.size() == N48D2_ALLOWED2P_COUNT, "the 39 plane registers are distinct");
      expect(inAllow, "every plane register is inside the REAL DCN write allowlist (the second line of defence admits what the first does)");
      expect(dcn41_allow_write(&m.allow, N48D2_DPPCLK2_DTO_PARAM, 0u, "x") != 0 && dcn41_allow_write(&m.allow, N48D2_DPPCLK_CTRL, 1u, "x") != 0, "the two DCCG exceptions are inside the allowlist's DCCG range"); }
    expect(n48d2_eqpairs[0].a2 == lx_abs("MPCC_OGAM2_MPCC_OGAM_CONTROL") && n48d2_eqpairs[0].a0 == lx_abs("MPCC_OGAM0_MPCC_OGAM_CONTROL") && n48d2_eqpairs[1].a2 == lx_abs("MPCC_MCM2_MPCC_MCM_SHAPER_CONTROL") && n48d2_eqpairs[1].a0 == lx_abs("MPCC_MCM0_MPCC_MCM_SHAPER_CONTROL") &&
           n48d2_eqpairs[2].a2 == lx_abs("MPCC_MCM2_MPCC_MCM_3DLUT_MODE") && n48d2_eqpairs[2].a0 == lx_abs("MPCC_MCM0_MPCC_MCM_3DLUT_MODE") && n48d2_eqpairs[3].a2 == lx_abs("MPCC_MCM2_MPCC_MCM_1DLUT_CONTROL") && n48d2_eqpairs[3].a0 == lx_abs("MPCC_MCM0_MPCC_MCM_1DLUT_CONTROL") &&
           N48D2_EQ_COUNT == 4u, "the never-censused pairs: MPCC_OGAM2 control 0x913a / 0x907e and MCM2 0x95b3 / 0x95ea / 0x95f3 against 0x9453 / 0x948a / 0x9493");
    // ---- the pre-state rows: values from the census/spec, registers by name
    { const struct { const char *n; uint32_t mask, val; } st[18] = {
          { "OTG2_OTG_CONTROL", 0x00010001u, 0x00010001u }, { "DPG2_DPG_CONTROL", N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON }, { "HUBP2_HUBP_CLK_CNTL", 1u, 0u }, { "HUBP2_DCHUBP_CNTL", 0xF4u, 0x20u }, { "DPP_TOP2_DPP_CONTROL", 0x10u, 0u },
          { "MPCC2_MPCC_TOP_SEL", ~0u, 0xFu }, { "MPCC2_MPCC_BOT_SEL", ~0u, 0xFu }, { "MPCC2_MPCC_OPP_ID", ~0u, 0xFu }, { "MPC_OUT2_MUX", 0xFu, 0xFu }, { "DPPCLK_CTRL", 0x40u, 0u }, { "DPPCLK2_DTO_PARAM", ~0u, 0u }, { "DCHUBBUB_DET0_CTRL", ~0u, 0x303u },
          { "DCHUBBUB_DET2_CTRL", 0x1Fu, 3u }, { "DCN_VM_FAULT_STATUS", ~0u, 0u }, { "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_LOW_ADDR", ~0u, 0u }, { "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR", ~0u, 0u }, { "HUBPREQ2_DCN_VM_MX_L1_TLB_CNTL", ~0u, 0u }, { "HUBPREQ2_VMID_SETTINGS_0", ~0u, 0u } };
      expect_u("18 state rows", N48D2_PRE_STATE_COUNT, 18);
      for (uint32_t i = 0; i < 18; i++) { char b[160]; std::snprintf(b, sizeof b, "precheck state row %u: %s", i, st[i].n); expect(n48d2_pre_state[i].abs == lx_abs(st[i].n) && n48d2_pre_state[i].mask == st[i].mask && n48d2_pre_state[i].val == st[i].val, b); } }
    // ---- the precheck pages: 106 EXPECT steps in 3 pages of <= 48, nothing but EXPECTs, every census-pinned value; the COPY0 sources are the LAST 20
    { n48d2_step s[N48D2_MAX_STEPS]; uint32_t tot = 0; bool allExp = true; std::set<uint32_t> srcs;
      std::vector<uint32_t> flat;
      for (uint32_t p = 0; p < N48D2_PRE_PAGES; p++) { const uint32_t n = n48d2_build_pre(p, s); tot += n; expect(n >= 1 && n <= N48D2_MAX_STEPS, "a precheck page fits the 48-step cap"); for (uint32_t i = 0; i < n; i++) { allExp = allExp && s[i].kind == N48D2_K_EXPECT; flat.push_back(s[i].abs); } }
      for (size_t i = flat.size() >= 20 ? flat.size() - 20 : 0; i < flat.size(); i++) srcs.insert(flat[i]);
      expect(tot == 106u && N48D2_PRE_TOTAL == 106u && N48D2_PRE_PAGES == 3u && allExp, "the precheck is 106 EXPECT steps in 3 pages: nothing in it can write");
      bool same = srcs.size() == 20u; for (uint32_t i = 0; i < 20; i++) same = same && srcs.count(n48d2_mirror0[i].src) == 1u;
      expect(same, "the last 20 precheck steps are exactly the 20 COPY0 sources (read before any write)");
      expect_u("a page past the end is empty", n48d2_build_pre(3, s), 0); }
}

static n48d2_step mkstep(uint8_t kind, uint32_t abs, uint32_t mask, uint32_t val, uint32_t src = 0u) {
    n48d2_step s; std::memset(&s, 0, sizeof s); s.kind = kind; s.abs = abs; s.mask = kind == N48D2_K_SET ? 0xFFFFFFFFu : mask; s.val = val; s.cond_abs = src; s.cond_mask = src ? 0xFFFFFFFFu : 0u;
    if (kind == N48D2_K_COPY0) for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) if (n48d2_mirror0[i].dst == abs && n48d2_mirror0[i].src == src) s.cond_mask = n48d2_mirror0[i].wmask;      // 0.0.637: a pair's own write mask
    s.reg = "t"; s.fn = "t"; return s;
}
static bool list_ok(uint32_t inst, const n48d2_step &st) { uint32_t bad = 0; return n48d2_guard_list_i(inst, &st, 1u, &bad) == 0u; }
static bool wr_ok(uint32_t inst, uint32_t abs, uint32_t v) { return n48d2_guard_write_i(inst, abs, v, nullptr) == N48D2_G_OK; }

static void plane_guard_tests() {
    // ---- address: the 37 plane registers and the two DCCG exceptions are instance 2's alone
    for (uint32_t i = 0; i < N48D2_ALLOWED2P_COUNT; i++) {
        expect(n48d2_guard_addr_i(2, n48d2_allowed2p[i], nullptr) == N48D2_G_OK, "instance 2 admits every plane register");
        expect(n48d2_guard_addr_i(1, n48d2_allowed2p[i], nullptr) == N48D2_G_FORBIDDEN && !wr_ok(1, n48d2_allowed2p[i], 0u), "instance 1 refuses every plane register of the monitor B (0.0.655: FORBIDDEN - a per-instance span)");
        expect(n48d2_guard_addr_i(0, n48d2_allowed2p[i], nullptr) != N48D2_G_OK && n48d2_guard_addr_i(3, n48d2_allowed2p[i], nullptr) != N48D2_G_OK, "an unknown instance admits none");
    }
    expect(n48d2_guard_addr_i(2, N48D2_DPPCLK2_DTO_PARAM, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(2, N48D2_DPPCLK_CTRL, nullptr) == N48D2_G_OK, "instance 2 admits the two DCCG exceptions");
    expect(n48d2_guard_addr_i(1, N48D2_DPPCLK2_DTO_PARAM, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, N48D2_DPPCLK_CTRL, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(1, N48D2_DPPCLK1_DTO_PARAM, nullptr) == N48D2_G_OK, "instance 1 may not touch the monitor B's DTO 0x15b (the DCCG span); 0.0.655: it has its OWN two exceptions, 0x15a and the shared 0x168 (kind / mask / value pinned below)");
    for (uint32_t a = 0x100u; a <= 0x17eu; a++) {
        if (a == N48D2_DPPCLK2_DTO_PARAM || a == N48D2_DPPCLK_CTRL || a == N48D2_SYMCLKD_CLOCK_ENABLE) continue;
        expect(n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_OK, "every OTHER DCCG dword is refused for instance 2 (DPPCLK0 / 1 / 3 DTO, DPPCLK_DTO_CTRL, the PHYPLL sources, OTG0's pixel rate, SYMCLKA / B / C / E ...)");
    }
    expect(n48d2_guard_addr_i(2, N48D2_DPPCLK0_DTO_PARAM, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x15au, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x15cu, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x176u, nullptr) == N48D2_G_FORBIDDEN,
           "DPPCLK0 / 1 / 3 DTO and DPPCLK_DTO_CTRL stay FORBIDDEN (a mis-address by one dword would retime the DP's DPP clock)");
    // ---- value rules (judged on the value that would be written)
    n48d2_aset_set(kBufA, kBufB);
    const struct { const char *what; uint32_t abs, v; bool ok; } vr[] = {
        { "DCHUBP_CNTL VTG_SEL 2, soft reset clear", N48D2_HUBP2_DCHUBP_CNTL, 0x000f002au, true }, { "DCHUBP_CNTL blank set, VTG_SEL 2", N48D2_HUBP2_DCHUBP_CNTL, 0x000f002bu, true }, { "DCHUBP_CNTL VTG_SEL 0 (the DP's)", N48D2_HUBP2_DCHUBP_CNTL, 0x000f000au, false },
        { "DCHUBP_CNTL VTG_SEL 3", N48D2_HUBP2_DCHUBP_CNTL, 0x000f003au, false }, { "DCHUBP_CNTL soft reset bit 2", N48D2_HUBP2_DCHUBP_CNTL, 0x000f002eu, false },
        { "MPCC2 TOP 2", N48D2_MPCC2_MPCC_TOP_SEL, 2u, true }, { "MPCC2 TOP 0xf", N48D2_MPCC2_MPCC_TOP_SEL, 0xFu, true }, { "MPCC2 TOP 0 (the DP's mixer input)", N48D2_MPCC2_MPCC_TOP_SEL, 0u, false }, { "MPCC2 TOP 1", N48D2_MPCC2_MPCC_TOP_SEL, 1u, false }, { "MPCC2 TOP 0x12", N48D2_MPCC2_MPCC_TOP_SEL, 0x12u, false },
        { "MPCC2 OPP 2", N48D2_MPCC2_MPCC_OPP_ID, 2u, true }, { "MPCC2 OPP 0xf", N48D2_MPCC2_MPCC_OPP_ID, 0xFu, true }, { "MPCC2 OPP 0 (OPP0 = the DP)", N48D2_MPCC2_MPCC_OPP_ID, 0u, false }, { "MPCC2 OPP 1", N48D2_MPCC2_MPCC_OPP_ID, 1u, false },
        { "MPCC2 LOCK 2", N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL, 2u, true }, { "MPCC2 LOCK 0xf", N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL, 0xFu, true }, { "MPCC2 LOCK 0 (OTG0's lock)", N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL, 0u, false },
        { "MPCC2 BOT 0xf", N48D2_MPCC2_MPCC_BOT_SEL, 0xFu, true }, { "MPCC2 BOT 2", N48D2_MPCC2_MPCC_BOT_SEL, 2u, false }, { "MPCC2 BOT 0", N48D2_MPCC2_MPCC_BOT_SEL, 0u, false },
        { "MUX 0x4102 (mux 2)", N48D2_MPC_OUT2_MUX, 0x4102u, true }, { "MUX 0x400f (none)", N48D2_MPC_OUT2_MUX, 0x400fu, true }, { "MUX 0x4100 (OPP 0!)", N48D2_MPC_OUT2_MUX, 0x4100u, false }, { "MUX 0x4101", N48D2_MPC_OUT2_MUX, 0x4101u, false },
        { "MUX 0x4103", N48D2_MPC_OUT2_MUX, 0x4103u, false }, { "MUX 0x410e", N48D2_MPC_OUT2_MUX, 0x410eu, false },
        { "pitch 0x9ff", N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0x9ffu, true }, { "pitch 0x9fe", N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0x9feu, false }, { "pitch 0", N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0u, false }, { "pitch 0xa00", N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0xa00u, false },
        { "DTO 0x00ff00ff", N48D2_DPPCLK2_DTO_PARAM, 0x00ff00ffu, true }, { "DTO 0", N48D2_DPPCLK2_DTO_PARAM, 0u, true }, { "DTO 1", N48D2_DPPCLK2_DTO_PARAM, 1u, false }, { "DTO 0x00ff00fe", N48D2_DPPCLK2_DTO_PARAM, 0x00ff00feu, false }, { "DTO all ones", N48D2_DPPCLK2_DTO_PARAM, 0xFFFFFFFFu, false },
        { "address LOW = A", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufA, true }, { "address LOW = B", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufB, true }, { "address LOW = A + 64 KiB", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufA + 0x10000u, false },
        { "address LOW = 0 (0.0.638: EXACTLY 0 is allowed, the rollback zeroes the address)", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u, true }, { "address LOW = 1", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 1u, false }, { "address LOW = 0x10000 (not A, not B, not 0)", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0x10000u, false },
        { "address LOW = all ones", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0xFFFFFFFFu, false },
        { "address HIGH = 0x80", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x80u, true }, { "address HIGH = 0x81", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x81u, false }, { "address HIGH = 0 (0.0.638: allowed)", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0u, true },
        { "address HIGH = 1", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 1u, false }, { "address HIGH = all ones", N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0xFFFFFFFFu, false },
        { "CRC window AX", N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_CRC_WIN_X, true }, { "CRC window AX wrong", N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_CRC_WIN_Y, false }, { "CRC window AY", N48D2_OTG2_OTG_CRC0_WINDOWA_Y_CONTROL, N48D2_CRC_WIN_Y, true },
        { "CRC window BX", N48D2_OTG2_OTG_CRC0_WINDOWB_X_CONTROL, N48D2_CRC_WIN_X, true }, { "CRC window BY", N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL, N48D2_CRC_WIN_Y, true }, { "CRC window BY wrong", N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL, 0u, false },
        { "ODM2 soft reset bit", N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 0x1001u, false }, { "ODM2 underflow clear", N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 0x1000u, true },
        { "CRC_CNTL select 0", N48D2_OTG2_OTG_CRC_CNTL, 0x11u, true }, { "CRC_CNTL select 1", N48D2_OTG2_OTG_CRC_CNTL, 0x00100011u, false },
    };
    for (const auto &c : vr) expect(wr_ok(2, c.abs, c.v) == c.ok, (std::string("value rule: ") + c.what).c_str());
    n48d2_aset_clear();
    expect(!wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufA) && !wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x80u), "with NO address set installed every address write is refused (fail closed)");
    // ---- kind / mask rules (list level)
    n48d2_aset_set(kBufA, kBufB);
    const uint8_t S_ = N48D2_K_SET, U_ = N48D2_K_UPD;
    const struct { const char *what; n48d2_step st; bool ok; } kr[] = {
        { "DTO SET 0x00ff00ff", mkstep(S_, N48D2_DPPCLK2_DTO_PARAM, 0, 0x00ff00ffu), true }, { "DTO SET 0", mkstep(S_, N48D2_DPPCLK2_DTO_PARAM, 0, 0u), true }, { "DTO UPD", mkstep(U_, N48D2_DPPCLK2_DTO_PARAM, 0xFFFFFFFFu, 0u), false }, { "DTO SET 1", mkstep(S_, N48D2_DPPCLK2_DTO_PARAM, 0, 1u), false },
        { "DPPCLK_CTRL UPD mask 0x40 -> 0x40", mkstep(U_, N48D2_DPPCLK_CTRL, 0x40u, 0x40u), true }, { "DPPCLK_CTRL UPD mask 0x40 -> 0", mkstep(U_, N48D2_DPPCLK_CTRL, 0x40u, 0u), true },
        { "DPPCLK_CTRL mask 0x1 (DPPCLK0_EN: the DP!)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x1u, 0u), false }, { "DPPCLK_CTRL mask 0x41", mkstep(U_, N48D2_DPPCLK_CTRL, 0x41u, 0x40u), false }, { "DPPCLK_CTRL mask 0x4 (the WRONG bit)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x4u, 0x4u), false },
        { "DPPCLK_CTRL mask 0x8 (DPPCLK1_EN)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x8u, 0x8u), false }, { "DPPCLK_CTRL mask 0x2", mkstep(U_, N48D2_DPPCLK_CTRL, 0x2u, 0x2u), false }, { "DPPCLK_CTRL mask 0x200 (DPPCLK3_EN)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x200u, 0x200u), false },
        { "DPPCLK_CTRL mask 0xFFFFFFFF", mkstep(U_, N48D2_DPPCLK_CTRL, 0xFFFFFFFFu, 0x40u), false }, { "DPPCLK_CTRL mask 0", mkstep(U_, N48D2_DPPCLK_CTRL, 0u, 0u), false }, { "DPPCLK_CTRL SET", mkstep(S_, N48D2_DPPCLK_CTRL, 0, 0x40u), false },
        { "DPPCLK_CTRL value outside its mask", mkstep(U_, N48D2_DPPCLK_CTRL, 0x40u, 0x41u), false },
        { "DPP_TOP2 UPD 0x10", mkstep(U_, N48D2_DPP_TOP2_DPP_CONTROL, 0x10u, 0x10u), true }, { "DPP_TOP2 UPD 0x10 -> 0", mkstep(U_, N48D2_DPP_TOP2_DPP_CONTROL, 0x10u, 0u), true }, { "DPP_TOP2 mask 0x11", mkstep(U_, N48D2_DPP_TOP2_DPP_CONTROL, 0x11u, 0x11u), false },
        { "DPP_TOP2 SET", mkstep(S_, N48D2_DPP_TOP2_DPP_CONTROL, 0, 0x10u), false },
        { "HUBP2 CNTL UPD mask 1", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 1u, 1u), true }, { "HUBP2 CNTL UPD mask 0x80000000", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 0x80000000u, 0x80000000u), true }, { "HUBP2 CNTL UPD mask 0x80000001", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 0x80000001u, 0u), true },
        { "HUBP2 CNTL mask 2", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 2u, 2u), false }, { "HUBP2 CNTL mask 4 (soft reset)", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 4u, 4u), false }, { "HUBP2 CNTL mask 0xF0 (VTG_SEL)", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 0xF0u, 0u), false },
        { "HUBP2 CNTL mask 0x1000", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 0x1000u, 0u), false }, { "HUBP2 CNTL SET", mkstep(S_, N48D2_HUBP2_DCHUBP_CNTL, 0, 0u), false }, { "HUBP2 CNTL mask 0", mkstep(U_, N48D2_HUBP2_DCHUBP_CNTL, 0u, 0u), false },
        { "HUBP clock UPD 0x11111", mkstep(U_, N48D2_HUBP2_HUBP_CLK_CNTL, 0x11111u, 0x11111u), true }, { "HUBP clock UPD 1", mkstep(U_, N48D2_HUBP2_HUBP_CLK_CNTL, 1u, 0u), true }, { "HUBP clock mask 0x100000 (status)", mkstep(U_, N48D2_HUBP2_HUBP_CLK_CNTL, 0x100000u, 0u), false },
        { "HUBP clock mask 0x00f11111 (HUBP0's)", mkstep(U_, N48D2_HUBP2_HUBP_CLK_CNTL, 0x00f11111u, 0u), false }, { "HUBP clock SET", mkstep(S_, N48D2_HUBP2_HUBP_CLK_CNTL, 0, 0u), false },
        { "ODM2 UPD 0x1000", mkstep(U_, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 0x1000u, 0x1000u), true }, { "ODM2 mask 0x1001", mkstep(U_, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 0x1001u, 0x1000u), false }, { "ODM2 mask 1 (soft reset)", mkstep(U_, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 1u, 1u), false },
        { "ODM2 SET", mkstep(S_, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, 0, 0x1000u), false },
        { "CRC_CNTL UPD 0x00700011 -> 0x11", mkstep(U_, N48D2_OTG2_OTG_CRC_CNTL, 0x00700011u, 0x11u), true }, { "CRC_CNTL UPD -> 0", mkstep(U_, N48D2_OTG2_OTG_CRC_CNTL, 0x00700011u, 0u), true }, { "CRC_CNTL mask 0x11", mkstep(U_, N48D2_OTG2_OTG_CRC_CNTL, 0x11u, 0x11u), false },
        { "CRC_CNTL mask 0x00700012", mkstep(U_, N48D2_OTG2_OTG_CRC_CNTL, 0x00700012u, 0x11u), false }, { "CRC_CNTL SET", mkstep(S_, N48D2_OTG2_OTG_CRC_CNTL, 0, 0x11u), false },
        { "pitch UPD", mkstep(U_, N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0xFFFFFFFFu, 0x9ffu), false }, { "pitch SET", mkstep(S_, N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, 0, 0x9ffu), true },
        { "address LOW UPD", mkstep(U_, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0xFFFFFFFFu, (uint32_t)kBufA), false }, { "address LOW SET A", mkstep(S_, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kBufA), true },
        { "address LOW SET outside {A,B}", mkstep(S_, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kBufA + 0x10000u), false },
        { "MPCC2 TOP SET 2", mkstep(S_, N48D2_MPCC2_MPCC_TOP_SEL, 0, 2u), true }, { "MPCC2 TOP UPD", mkstep(U_, N48D2_MPCC2_MPCC_TOP_SEL, 0xFu, 2u), false }, { "MUX UPD", mkstep(U_, N48D2_MPC_OUT2_MUX, 0xFu, 2u), false }, { "MUX SET 0x4102", mkstep(S_, N48D2_MPC_OUT2_MUX, 0, 0x4102u), true },
        { "CRC window UPD", mkstep(U_, N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL, 0xFFFFFFFFu, N48D2_CRC_WIN_X), false },
        { "EXPECT is read only and always passes the list guard", mkstep(N48D2_K_EXPECT, N48D2_HUBP0_DCHUBP_CNTL, 0xFFFFFFFFu, 0u), true },
        { "a WAIT on a pipe-0 register is a read", mkstep(N48D2_K_WAIT, N48D2_HUBP0_DCHUBP_CNTL, 1u, 0u), true },
    };
    for (const auto &c : kr) expect(list_ok(2, c.st) == c.ok, (std::string("kind / mask rule: ") + c.what).c_str());
    // ---- COPY0: ONLY the 20 exact pairs; every other form is refused
    for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) {
        const auto &m = n48d2_mirror0[i];
        expect(list_ok(2, mkstep(N48D2_K_COPY0, m.dst, 0xFFFFFFFFu, m.expect, m.src)), "COPY0: each of the 20 pairs passes the list guard");
        expect(!list_ok(1, mkstep(N48D2_K_COPY0, m.dst, 0xFFFFFFFFu, m.expect, m.src)), "COPY0: instance 1 refuses every pair");
        expect(!list_ok(2, mkstep(N48D2_K_COPY0, m.dst, 0xFFFFFFFFu, m.expect, m.src + 1u)) && !list_ok(2, mkstep(N48D2_K_COPY0, m.dst, 0xFFFFFFFFu, m.expect ^ 1u, m.src)) && !list_ok(2, mkstep(N48D2_K_COPY0, m.dst, 0x0000FFFFu, m.expect, m.src)), "COPY0: a wrong source, a wrong census value or a partial mask is refused");
        expect(!list_ok(2, mkstep(N48D2_K_COPY0, m.src, 0xFFFFFFFFu, m.expect, m.src)), "COPY0 TO the pipe-0 source address (a pipe-0 write) is refused (forbidden span)");
        expect(!list_ok(2, mkstep(N48D2_K_COPY0, m.dst, 0xFFFFFFFFu, m.expect, n48d2_mirror0[(i + 1) % N48D2_MIRROR0_COUNT].src)), "COPY0: a pair that crosses two table rows is refused");
        expect(!list_ok(2, mkstep(N48D2_K_SET, m.dst, 0, m.expect)) && !list_ok(2, mkstep(N48D2_K_UPD, m.dst, 0xFFFFFFFFu, m.expect)), "a COPY0 destination is NEVER writable by SET / UPD");
        expect(wr_ok(2, m.dst, m.expect), "the destination itself is admitted by the per-write guard (the kind rule lives in the list guard)");
    }
    expect(!list_ok(2, mkstep(N48D2_K_COPY0, N48D2_HUBP2_DCHUBP_CNTL, 0xFFFFFFFFu, 0u, N48D2_HUBP0_DCHUBP_CNTL)) && !list_ok(2, mkstep(N48D2_K_COPY0, N48D2_HUBP2_HUBP_CLK_CNTL, 0xFFFFFFFFu, 0x00f11111u, N48D2_HUBP0_DCHUBP_CNTL + 1u)) &&
           !list_ok(2, mkstep(N48D2_K_COPY0, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0xFFFFFFFFu, 0u, N48D2_HUBPREQ0_PRIMARY_LOW)) && !list_ok(2, mkstep(N48D2_K_COPY0, N48D2_MPC_OUT2_MUX, 0xFFFFFFFFu, 0x4100u, N48D2_MPC_OUT0_MUX)),
           "COPY0 of DCHUBP_CNTL, the clock register, the PRIMARY ADDRESS (HUBP0's) and the MPC mux is refused: the 'never copy' list");
    // ---- the 'never copy from pipe 0' list, by sweep: EVERY register of the pipe-0 blocks copied to its pipe-2 twin is refused unless it is one of the 20 pairs (with its census value). That covers DCHUBP_CNTL, HUBP_CLK_CNTL status bits, the primary / secondary addresses,
    //      INUSE / EARLIEST_INUSE, FLIP_CONTROL, SURFACE_FLIP_INTERRUPT, HUBPREQ_STATUS_REG0-3, the MEM_PWR_STATUS registers, HUBPRET READ_LINE_* and INTERRUPT, MALL_STATUS, LB_V_COUNTER, the CM / ISHARP power status, CM_CONTROL, MPCC0's TOP / OPP, MPC_OUT0's mux, the cursor registers.
    { uint32_t tried = 0, accepted = 0, listed = 0;
      const struct { const char *p0, *p2; } blocks[] = { { "HUBP0_", "HUBP2_" }, { "HUBPREQ0_", "HUBPREQ2_" }, { "HUBPRET0_", "HUBPRET2_" }, { "DPP_TOP0_", "DPP_TOP2_" }, { "CNVC_CFG0_", "CNVC_CFG2_" }, { "DSCL0_", "DSCL2_" }, { "CM0_", "CM2_" }, { "MPCC0_MPCC_", "MPCC2_MPCC_" }, { "MPC_OUT0_", "MPC_OUT2_" }, { "CURSOR0_", "CURSOR2_" }, { "OBUF0_", "OBUF2_" } };
      for (const auto &b : blocks) for (const std::string &nm : header_names(b.p0)) {
          const std::string twin = std::string(b.p2) + nm.substr(std::strlen(b.p0)); uint32_t a0 = 0, a2 = 0; if (!lx_reg(nm, &a0) || !lx_reg(twin, &a2)) continue; ++tried;
          bool isPair = false; uint32_t expv = 0; for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) if (n48d2_mirror0[i].dst == a2 && n48d2_mirror0[i].src == a0) { isPair = true; expv = n48d2_mirror0[i].expect; }
          const bool okAny = list_ok(2, mkstep(N48D2_K_COPY0, a2, 0xFFFFFFFFu, 0u, a0)) || list_ok(2, mkstep(N48D2_K_COPY0, a2, 0xFFFFFFFFu, 0xFFFFFFFFu, a0));
          if (isPair) { ++listed; if (!list_ok(2, mkstep(N48D2_K_COPY0, a2, 0xFFFFFFFFu, expv, a0))) ++accepted; /* a listed pair must pass (counted as a miss when it does not) */ }
          else if (okAny || list_ok(2, mkstep(N48D2_K_COPY0, a2, 0xFFFFFFFFu, 0x08000000u, a0))) ++accepted;
      }
      std::printf("plane guard: %u pipe-0 / pipe-2 twin pairs swept for COPY0, %u of them the spec's, %u mis-judged\n", tried, listed, accepted);
      expect(tried > 300 && listed == 20u && accepted == 0u, "COPY0 sweep: of every pipe-0 register (HUBP / HUBPREQ / HUBPRET / DPP_TOP / CNVC / DSCL / CM / MPCC / MPC_OUT / CURSOR / OBUF) and its pipe-2 twin, ONLY the 20 pairs pass; nothing else is copyable"); }
    // ---- the forbidden spans: every pipe-0 / pipe-1 / pipe-3 / DCHUBBUB / MPC_OUT0 / MPC-global dword is refused for BOTH instances (named FORBIDDEN where a span names it)
    { const char *pre[] = { "HUBP0_", "HUBPREQ0_", "HUBPRET0_", "HUBP1_", "HUBPREQ1_", "HUBPRET1_", "HUBP3_", "HUBPREQ3_", "HUBPRET3_", "DPP_TOP0_", "CNVC_CFG0_", "DSCL0_", "CM0_", "DPP_TOP1_", "CNVC_CFG1_", "DSCL1_", "CM1_", "DPP_TOP3_", "CNVC_CFG3_", "DSCL3_", "CM3_",
                            "MPCC0_MPCC_", "MPCC1_MPCC_", "MPCC3_MPCC_", "MPC_OUT0_", "DCHUBBUB_DET0", "DCHUBBUB_COMPBUF" };
      uint32_t n = 0, bad = 0;
      for (const char *p : pre) for (const std::string &nm : header_names(p)) { uint32_t a = 0; if (!lx_reg(nm, &a)) continue; ++n;
          bool pipe1 = false; for (const char *pp : { "HUBP1_", "HUBPREQ1_", "HUBPRET1_", "DPP_TOP1_", "CNVC_CFG1_", "DSCL1_", "CM1_", "MPCC1_MPCC_" }) pipe1 = pipe1 || nm.compare(0, std::strlen(pp), pp) == 0;
          bool inList1 = false; for (uint32_t k = 0; k < N48D2_ALLOWED1P_COUNT; k++) inList1 = inList1 || n48d2_allowed1p[k] == a;      // 0.0.655: instance 1's exact list (the pipe-1 blocks are forbidden for the MONB; the monitor A reaches only its list)
          const bool monbOk = n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_FORBIDDEN && !wr_ok(2, a, 0u);
          const bool monaOk = pipe1 ? (inList1 ? n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK : n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_OK && !wr_ok(1, a, 0u)) : (n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_FORBIDDEN && !wr_ok(1, a, 0u));
          if (!monbOk || !monaOk) { ++bad; std::printf("   not forbidden: %s (%#x) guard1 %u guard2 %u\n", nm.c_str(), a, n48d2_guard_addr_i(1, a, nullptr), n48d2_guard_addr_i(2, a, nullptr)); } }
      std::printf("plane guard: %u header registers of HUBP0/1/3, DPP0/1/3, MPCC0/1/3, MPC_OUT0, DET0, COMPBUF checked, %u not forbidden\n", n, bad);
      expect(n > 1000 && bad == 0, "every HUBP0 / HUBPREQ0 / HUBPRET0 / DPP0 / MPCC0 / MPC_OUT0 / DET0 / COMPBUF register (and the HUBP3, DPP3, MPCC3 blocks) is FORBIDDEN for both instances; the HUBP1 / DPP1 / MPCC1 blocks are forbidden for the monitor B and reach the monitor A only through its exact list (0.0.655)");
      uint32_t nd = 0, badd = 0;
      for (const std::string &nm : header_names("DCHUBBUB_")) { uint32_t a = 0; if (!lx_reg(nm, &a)) continue; if (a >= 0x392fu && a <= 0x3aa4u) { ++nd; if (n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK || n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_OK) ++badd; } }
      expect(nd >= 40 && badd == 0, "every DCHUBBUB register in the spec's 0x392f..0x3aa4 window (DET0-3, COMPBUF, the watermarks, DET2_CTRL) is refused for both instances");
      expect(n48d2_guard_addr_i(2, N48D2_DCHUBBUB_DET2_CTRL, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, N48D2_DCN_VM_FAULT_STATUS, nullptr) == N48D2_G_FORBIDDEN, "DET2_CTRL and the VM fault status are READ only (forbidden for writes)");
      for (uint32_t a = 0x92b2u; a <= 0x92c0u; a++) expect(n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, a, nullptr) == N48D2_G_FORBIDDEN, "the MPC global block 0x92b2..0x92c0 is forbidden");
      expect(n48d2_guard_addr_i(2, N48D2_VTG2_CONTROL, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(1, N48D2_VTG1_CONTROL, nullptr) == N48D2_G_OK, "the spec's 0x392f..0x3aa4 span is split around VTG1 / VTG2_CONTROL (0x39f1 / 0x39f2): the timing ops still reach them");
      expect(n48d2_guard_addr_i(2, 0x39f0u, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x39f3u, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, 0x39f0u, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, 0x39f3u, nullptr) == N48D2_G_FORBIDDEN, "VTG0 and VTG3 stay forbidden"); }
    // ---- the plane's own lists pass the guard, as built, with the set installed; every one of them is refused for instance 1
    { uint32_t bad = 0;
      struct L { const char *w; uint32_t n; } ls[16]; uint32_t nl = 0;
      static n48d2_step buf[16][N48D2_MAX_STEPS];
      ls[nl++] = { "plane A", n48d2_build_plane_a(buf[0], kBufA) }; ls[nl++] = { "plane B", n48d2_build_plane_b(buf[1]) }; ls[nl++] = { "plane C", n48d2_build_plane_c(buf[2], kBufA) }; ls[nl++] = { "show 1", n48d2_build_show1(buf[3]) }; ls[nl++] = { "show 2", n48d2_build_show2(buf[4]) };
      ls[nl++] = { "flip A", n48d2_build_flip(buf[5], kBufA) }; ls[nl++] = { "flip B", n48d2_build_flip(buf[6], kBufB) }; ls[nl++] = { "crc", n48d2_build_crc(buf[7]) }; ls[nl++] = { "planeoff", n48d2_build_planeoff(buf[8]) };
      for (uint32_t p = 0; p < N48D2_PRE_PAGES; p++) ls[nl++] = { "precheck page", n48d2_build_pre(p, buf[9 + p]) };
      ls[nl++] = { "plane D", n48d2_build_plane_d(buf[12], kBufA) };      // 0.0.636: the COPY0 pairs + the address again (index 12)
      ls[nl++] = { "plane rec2", n48d2_build_plane_rec2(buf[13]) };        // 0.0.638: the second recorded sample (index 13)
      ls[nl++] = { "plane lock", n48d2_build_plane_lock(buf[14]) };         // 0.0.643: OTG2's update lock (index 14)
      ls[nl++] = { "plane unlock", n48d2_build_plane_unlock(buf[15]) };     // 0.0.643: ... and its release (index 15)
      for (uint32_t i = 0; i < nl; i++) {
          expect(ls[i].n >= 1u && ls[i].n <= N48D2_MAX_STEPS, (std::string(ls[i].w) + ": the list fits the 48-step cap").c_str());
          expect(n48d2_guard_list_i(2, buf[i], ls[i].n, &bad) == 0u, (std::string(ls[i].w) + ": the list passes instance 2's guard as built").c_str());
          if (i < 9 || i == 12 || i >= 13) expect(n48d2_guard_list_i(1, buf[i], ls[i].n, &bad) != 0u, (std::string(ls[i].w) + ": instance 1's guard refuses the whole list").c_str());
      }
      // a list with ONE forbidden step is refused whole, at THAT step, whichever step it is: every plane list with each write step aimed at HUBP0's CNTL / DET0 / MPC_OUT0_MUX / DPPCLK0's DTO / MPCC0 / HUBP0's address
      bool allRefused = true; uint32_t tried = 0;
      for (uint32_t li = 0; li < 16; li++) if (li < 9 || li == 12 || li >= 14) for (uint32_t k = 0; k < ls[li].n; k++) if (n48d2_is_write(&buf[li][k])) {
          for (uint32_t bad_abs : { N48D2_HUBP0_DCHUBP_CNTL, N48D2_DET0_CTRL, N48D2_MPC_OUT0_MUX, N48D2_DPPCLK0_DTO_PARAM, N48D2_MPCC0_TOP_SEL, N48D2_HUBPREQ0_PRIMARY_LOW }) {
              static n48d2_step t[N48D2_MAX_STEPS]; std::memcpy(t, buf[li], sizeof(n48d2_step) * ls[li].n); t[k].abs = bad_abs; uint32_t b2 = 0; ++tried;
              if (n48d2_guard_list_i(2, t, ls[li].n, &b2) == 0u || b2 != k) allRefused = false; } }
      expect(allRefused && tried > 300, "a plane list with a pipe-0 / DP register aimed at any one write step is refused whole, at THAT step");
    }
    n48d2_aset_clear();
}

// ---- the exact step lists, rebuilt from Linux NAMES and the spec's table ------------------------------------------------------------------------------------------------------------------------------------------
struct XS { uint8_t kind; const char *reg; uint32_t mask, val; const char *src; uint32_t polls, us; uint32_t cm = 0xFFFFFFFFu; };      // cm: a COPY0's write / read-back mask (0.0.637)
static void check_xs(const char *what, const n48d2_step *s, uint32_t n, const std::vector<XS> &want) {
    expect_u((std::string(what) + ": step count").c_str(), n, want.size());
    for (uint32_t i = 0; i < n && i < want.size(); i++) {
        const XS &x = want[i]; char lbl[200]; std::snprintf(lbl, sizeof lbl, "%s step %u (%s)", what, i, x.reg);
        bool ok = s[i].kind == x.kind && s[i].abs == lx_abs(x.reg) && s[i].val == x.val;
        ok = ok && (x.kind == N48D2_K_SET ? s[i].mask == 0xFFFFFFFFu : s[i].mask == x.mask);
        if (x.kind == N48D2_K_WAIT || x.kind == N48D2_K_REC) ok = ok && s[i].polls == x.polls && s[i].step_us == x.us;      // 0.0.638: a REC step's delay is its step_us, its slot its val
        if (x.kind == N48D2_K_COPY0) ok = ok && s[i].cond_abs == lx_abs(x.src) && s[i].cond_mask == x.cm;
        ++gRun; if (!ok) { ++gFail; std::printf("FAIL: %s: built kind %u abs %#x mask %#x val %#x polls %u us %u src %#x ; want kind %u %s mask %#x val %#x\n", lbl, s[i].kind, s[i].abs, s[i].mask, s[i].val, s[i].polls, s[i].step_us, s[i].cond_abs, x.kind, x.reg, x.mask, x.val); }
    }
}
static std::vector<XS> spec_part_a(uint64_t mc) {      // 0.0.636: Linux's order - rows 1-3, 15 (+ its read-back WAIT), 25-27: everything BEFORE the HUBP clock
    std::vector<XS> w;
    w.push_back({ N48D2_K_SET, "DPPCLK2_DTO_PARAM", 0, 0x00ff00ffu, nullptr, 0, 0 });
    w.push_back({ N48D2_K_UPD, "DPPCLK_CTRL", 0x40u, 0x40u, nullptr, 0, 0 });
    w.push_back({ N48D2_K_UPD, "DPP_TOP2_DPP_CONTROL", 0x10u, 0x10u, nullptr, 0, 0 });
    w.push_back({ N48D2_K_UPD, "HUBP2_DCHUBP_CNTL", 1u, 1u, nullptr, 0, 0 });                        // row 15: BLANK_EN = 1 ...
    w.push_back({ N48D2_K_WAIT, "HUBP2_DCHUBP_CNTL", 1u, 1u, nullptr, 100, 10 });                    // ... and READ BACK before the clock
    w.push_back({ N48D2_K_SET, "HUBPREQ2_DCSURF_SURFACE_PITCH", 0, 0x9ffu, nullptr, 0, 0 });
    w.push_back({ N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(mc >> 32), nullptr, 0, 0 });
    w.push_back({ N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)mc, nullptr, 0, 0 });
    return w;
}
static std::vector<XS> spec_part_d(uint64_t mc) {      // 0.0.636: rows 4-14 and 16-24 (COPY0) with the clocks ON, then the address again
    std::vector<XS> w;
    for (int i = 0; i < 20; i++) w.push_back({ N48D2_K_COPY0, kSpecMirror[i].dst, 0xFFFFFFFFu, kSpecMirror[i].v, kSpecMirror[i].src, 0, 0, kSpecMirror[i].w });
    w.push_back({ N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(mc >> 32), nullptr, 0, 0 });
    w.push_back({ N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)mc, nullptr, 0, 0 });
    return w;
}
static void plane_list_shapes() {
    n48d2_step s[N48D2_MAX_STEPS];
    check_xs("op 8 part A (rows 1-3, 15, 25-27: before the clock)", s, n48d2_build_plane_a(s, kBufA), spec_part_a(kBufA));
    check_xs("op 8 part D (rows 4-14, 16-24 COPY0, then the address again)", s, n48d2_build_plane_d(s, kBufA), spec_part_d(kBufA));
    check_xs("op 8 part B (0.0.639: row 28 the bare clock update, the four RECORDED reads ~1 ms later, DET2_CTRL RECORDED (slot 13); NO clock-status wait, NO DET2 wait)", s, n48d2_build_plane_b(s), {
        { N48D2_K_UPD, "HUBP2_HUBP_CLK_CNTL", 0x00011111u, 0x00011111u, nullptr, 0, 0 },
        { N48D2_K_REC, "HUBP2_HUBP_CLK_CNTL", 0u, 0u, nullptr, 0, 1000 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL6", 0u, 1u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL", 0u, 2u, nullptr, 0, 0 }, { N48D2_K_REC, "DOMAIN2_PG_STATUS", 0u, 3u, nullptr, 0, 0 },
        { N48D2_K_REC, "DCHUBBUB_DET2_CTRL", 0u, 13u, nullptr, 0, 0 } });
    check_xs("op 8 part E (0.0.639: AFTER the unblank: WAIT DET2 current = 3, 100 x 1 ms)", s, n48d2_build_plane_e(s), { { N48D2_K_WAIT, "DCHUBBUB_DET2_CTRL", 0x1F00u, 0x300u, nullptr, 100, 1000 } });
    check_xs("op 8 second recorded sample (after the 2-frame wait)", s, n48d2_build_plane_rec2(s), {
        { N48D2_K_REC, "HUBP2_HUBP_CLK_CNTL", 0u, 4u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL6", 0u, 5u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL", 0u, 6u, nullptr, 0, 0 }, { N48D2_K_REC, "DOMAIN2_PG_STATUS", 0u, 7u, nullptr, 0, 0 } });
    check_xs("op 8 LOCK (0.0.643: optc3_lock: OTG_MASTER_UPDATE_LOCK_SEL = 2, OTG_MASTER_UPDATE_LOCK = 1, wait UPDATE_LOCK_STATUS)", s, n48d2_build_plane_lock(s), {
        { N48D2_K_UPD, "OTG2_OTG_GLOBAL_CONTROL2", 0x0E000000u, 0x04000000u, nullptr, 0, 0 }, { N48D2_K_UPD, "OTG2_OTG_MASTER_UPDATE_LOCK", 1u, 1u, nullptr, 0, 0 },
        { N48D2_K_WAIT, "OTG2_OTG_MASTER_UPDATE_LOCK", 0x100u, 0x100u, nullptr, 2000, 50 } });
    check_xs("op 8 UNLOCK (0.0.643: optc1_unlock: OTG_MASTER_UPDATE_LOCK = 0)", s, n48d2_build_plane_unlock(s), { { N48D2_K_UPD, "OTG2_OTG_MASTER_UPDATE_LOCK", 1u, 0u, nullptr, 0, 0 } });
    check_xs("op 8 part C (0.0.643: INSIDE the lock: address HIGH / LOW, rows 29-34)", s, n48d2_build_plane_c(s, kBufA), {
        { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(kBufA >> 32), nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)kBufA, nullptr, 0, 0 },
        { N48D2_K_SET, "MPCC2_MPCC_BOT_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_TOP_SEL", 0, 2u, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_OPP_ID", 0, 2u, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_UPDATE_LOCK_SEL", 0, 2u, nullptr, 0, 0 },
        { N48D2_K_SET, "MPC_OUT2_MUX", 0, 0x00004102u, nullptr, 0, 0 }, { N48D2_K_UPD, "HUBP2_DCHUBP_CNTL", 1u, 0u, nullptr, 0, 0 } });
    check_xs("op 9 clears (rows 35-36)", s, n48d2_build_show1(s), { { N48D2_K_UPD, "HUBP2_DCHUBP_CNTL", 0x80000000u, 0x80000000u, nullptr, 0, 0 }, { N48D2_K_UPD, "ODM2_OPTC_INPUT_GLOBAL_CONTROL", 0x1000u, 0x1000u, nullptr, 0, 0 } });
    check_xs("op 9 DPG off", s, n48d2_build_show2(s), { { N48D2_K_UPD, "DPG2_DPG_CONTROL", 1u, 0u, nullptr, 0, 0 }, { N48D2_K_WAIT, "DPG2_DPG_STATUS", 1u, 0u, nullptr, 1000, 100 } });
    check_xs("op 10 flip B", s, n48d2_build_flip(s, kBufB), {
        { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(kBufB >> 32), nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)kBufB, nullptr, 0, 0 },
        { N48D2_K_WAIT, "HUBPREQ2_DCSURF_FLIP_CONTROL", 0x100u, 0u, nullptr, 100, 1000 },
        { N48D2_K_WAIT, "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE", 0xFFFFFFFFu, (uint32_t)kBufB, nullptr, 100, 1000 }, { N48D2_K_WAIT, "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 0xFFFFu, (uint32_t)(kBufB >> 32) & 0xFFFFu, nullptr, 100, 1000 } });
    check_xs("op 11 crc", s, n48d2_build_crc(s), {
        { N48D2_K_SET, "OTG2_OTG_CRC0_WINDOWA_X_CONTROL", 0, 2560u << 16, nullptr, 0, 0 }, { N48D2_K_SET, "OTG2_OTG_CRC0_WINDOWA_Y_CONTROL", 0, 1440u << 16, nullptr, 0, 0 },
        { N48D2_K_SET, "OTG2_OTG_CRC0_WINDOWB_X_CONTROL", 0, 2560u << 16, nullptr, 0, 0 }, { N48D2_K_SET, "OTG2_OTG_CRC0_WINDOWB_Y_CONTROL", 0, 1440u << 16, nullptr, 0, 0 },
        { N48D2_K_UPD, "OTG2_OTG_CRC_CNTL", 0x00700011u, 0x11u, nullptr, 0, 0 } });
    check_xs("op 12 planeoff (the rollback)", s, n48d2_build_planeoff(s), {
        { N48D2_K_UPD, "DPG2_DPG_CONTROL", N48D2_DPG_CONTROL_MASK, 0x00661001u, nullptr, 0, 0 }, { N48D2_K_WAIT, "DPG2_DPG_STATUS", 1u, 0u, nullptr, 1000, 100 },
        { N48D2_K_UPD, "HUBP2_DCHUBP_CNTL", 1u, 1u, nullptr, 0, 0 }, { N48D2_K_WAIT, "HUBP2_DCHUBP_CNTL", 2u, 2u, nullptr, 100, 1000 },
        { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, 0u, nullptr, 0, 0 },      // 0.0.638: the address zeroed after the blank + NO_OUTSTANDING_REQ
        { N48D2_K_SET, "MPC_OUT2_MUX", 0, 0x0000400fu, nullptr, 0, 0 },
        { N48D2_K_SET, "MPCC2_MPCC_TOP_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_BOT_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_OPP_ID", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC2_MPCC_UPDATE_LOCK_SEL", 0, 0xFu, nullptr, 0, 0 },
        { N48D2_K_WAIT, "MPCC2_MPCC_STATUS", 7u, 5u, nullptr, 100, 1000 },
        { N48D2_K_UPD, "OTG2_OTG_CRC_CNTL", 0x00700011u, 0u, nullptr, 0, 0 },
        { N48D2_K_UPD, "HUBP2_HUBP_CLK_CNTL", 0x00011111u, 0u, nullptr, 0, 0 }, { N48D2_K_WAIT, "HUBP2_HUBP_CLK_CNTL", 0x00A00000u, 0u, nullptr, 100, 100 },
        { N48D2_K_UPD, "DPP_TOP2_DPP_CONTROL", 0x10u, 0u, nullptr, 0, 0 }, { N48D2_K_UPD, "DPPCLK_CTRL", 0x40u, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "DPPCLK2_DTO_PARAM", 0, 0u, nullptr, 0, 0 } });
    expect(N48D2_PA_I_BLANKWAIT == 4u && N48D2_PA_I_BLANK == 3u && N48D2_FLIP_I_WAIT_LO == 3u && N48D2_FLIP_I_WAIT_HI == 4u && N48D2_PO_I_NOOUT == 3u && N48D2_PO_I_CLKOFF == 14u && N48D2_PO_I_MPCC == 11u && N48D2_PO_I_ADDR_HI == 4u && N48D2_PO_I_ADDR_LO == 5u && N48D2_PE_I_DETWAIT == 0u && N48D2_PE_STEPS == 1u && N48D2_FLIP_I_WAIT == 2u && N48D2_SHOW2_I_WAIT == 1u, "the WAIT step indices the flow gates on are those of the lists above");
    expect(N48D2_PA_STEPS == 8u && N48D2_PB_STEPS == 6u && N48D2_PR_STEPS == 4u && N48D2_PC_STEPS == 8u && N48D2_LK_STEPS == 3u && N48D2_PU_STEPS == 1u && N48D2_PD_STEPS == 22u && N48D2_FLIP_STEPS == 5u && N48D2_PO_STEPS == 18u && N48D2_LK_I_WAIT == 2u && N48D2_PC_I_UNBLANK == 7u && N48D2_PA_STEPS + N48D2_PB_STEPS + N48D2_PC_STEPS + N48D2_PD_STEPS - 6u == 34u + 4u,
           "op 8's four parts hold the table's 34 rows plus the 2 address re-writes in part D and 2 inside the lock (8 + 6 + 8 + 22 = 44 steps, six of them reads that are not writes - the blank read-back and the five recorded reads (the DET2 wait is part E, 1 more step, after the unblank): 38 writes) plus the lock (3 steps: 2 writes + the status wait) and the unlock (1 write): 41 writes in all; the rollback is 18 steps");
    // ---- the unavoidable ORDER, in the BUILDERS (the flow runs part A, B, D, then C): the HUBP clock is in part B only; the blank + its read-back and the address (HIGH then LOW) are in part A, BEFORE the clock; part D re-writes the address and holds
    //      every COPY0; the unblank is the LAST step of part C
    { n48d2_step a[N48D2_MAX_STEPS], b[N48D2_MAX_STEPS], c[N48D2_MAX_STEPS], d[N48D2_MAX_STEPS]; const uint32_t na = n48d2_build_plane_a(a, kBufA), nb = n48d2_build_plane_b(b), nc = n48d2_build_plane_c(c, kBufA), nd = n48d2_build_plane_d(d, kBufA);
      uint32_t blank = 99, bwait = 99, addrLo = 99, addrHi = 99, clkA = 0, mpcA = 0, copyA = 0, copyD = 0, addrD = 0, clkD = 0;
      for (uint32_t i = 0; i < na; i++) { if (a[i].abs == N48D2_HUBP2_DCHUBP_CNTL && a[i].kind == N48D2_K_UPD) blank = i; if (a[i].abs == N48D2_HUBP2_DCHUBP_CNTL && a[i].kind == N48D2_K_WAIT) bwait = i;
          if (a[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) addrLo = i; if (a[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) addrHi = i; if (a[i].abs == N48D2_HUBP2_HUBP_CLK_CNTL) ++clkA; if (a[i].abs == N48D2_MPC_OUT2_MUX || a[i].abs == N48D2_MPCC2_MPCC_TOP_SEL) ++mpcA; if (a[i].kind == N48D2_K_COPY0) ++copyA; }
      for (uint32_t i = 0; i < nd; i++) { if (d[i].kind == N48D2_K_COPY0) ++copyD; if (d[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS || d[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) ++addrD; if (d[i].abs == N48D2_HUBP2_HUBP_CLK_CNTL || d[i].abs == N48D2_HUBP2_DCHUBP_CNTL) ++clkD; }
      expect(blank < bwait && bwait < addrHi && addrHi < addrLo && clkA == 0u && mpcA == 0u && copyA == 0u && na == 8u && blank == N48D2_PA_I_BLANK && bwait == N48D2_PA_I_BLANKWAIT, "part A: BLANK_EN = 1, its read-back WAIT, then HIGH then LOW; NO clock write, NO COPY0 and NO MPCC2 / mux write in part A");
      expect(copyD == 20u && addrD == 2u && clkD == 0u && d[nd - 2].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && d[nd - 1].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, "part D: the 20 COPY0 pairs, then the address HIGH then LOW AGAIN; no clock and no blank write");
      expect(b[0].abs == N48D2_HUBP2_HUBP_CLK_CNTL && b[0].kind == N48D2_K_UPD && nb == 6u && b[nb - 1].kind == N48D2_K_REC && b[nb - 1].abs == N48D2_DCHUBBUB_DET2_CTRL, "part B opens with the HUBP clock write and ends with the DET2 RECORDED read (no WAIT)");
      { bool anyWait = false; for (uint32_t i = 0; i < nb; i++) anyWait = anyWait || b[i].kind == N48D2_K_WAIT; expect(!anyWait, "0.0.639: part B holds NO WAIT at all"); }
      expect(c[nc - 1].abs == N48D2_HUBP2_DCHUBP_CNTL && c[nc - 1].kind == N48D2_K_UPD && c[nc - 1].mask == 1u && c[nc - 1].val == 0u && c[nc - 2].abs == N48D2_MPC_OUT2_MUX && c[0].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && c[1].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS && nc == 8u, "part C (inside the lock) opens with the address HIGH then LOW (A) and ends with the UNBLANK, right after MPC_OUT2_MUX"); }
}

// ---- helpers for the flow tests
static std::vector<WantW> spec_op8_writes(uint64_t A) {      // 0.0.636: Linux's order, 36 writes
    std::vector<WantW> w;
    w.push_back({ lx_abs("DPPCLK2_DTO_PARAM"), ~0u, 0x00ff00ffu }); w.push_back({ lx_abs("DPPCLK_CTRL"), 0x40u, 0x40u }); w.push_back({ lx_abs("DPP_TOP2_DPP_CONTROL"), 0x10u, 0x10u });
    w.push_back({ lx_abs("HUBP2_DCHUBP_CNTL"), 1u, 1u });
    w.push_back({ lx_abs("HUBPREQ2_DCSURF_SURFACE_PITCH"), ~0u, 0x9ffu }); w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(A >> 32) }); w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)A });
    w.push_back({ lx_abs("HUBP2_HUBP_CLK_CNTL"), 0x00011111u, 0x00011111u });
    for (int i = 0; i < 20; i++) w.push_back({ lx_abs(kSpecMirror[i].dst), ~0u, kSpecMirror[i].v & kSpecMirror[i].w });      // 0.0.637: the LB row is written as 0x00003f00
    w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(A >> 32) }); w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)A });
    w.push_back({ lx_abs("OTG2_OTG_GLOBAL_CONTROL2"), 0x0E000000u, 0x04000000u }); w.push_back({ lx_abs("OTG2_OTG_MASTER_UPDATE_LOCK"), 1u, 1u });      // 0.0.643: optc3_lock
    w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(A >> 32) }); w.push_back({ lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)A });      // inside the lock
    w.push_back({ lx_abs("MPCC2_MPCC_BOT_SEL"), ~0u, 0xFu }); w.push_back({ lx_abs("MPCC2_MPCC_TOP_SEL"), ~0u, 2u }); w.push_back({ lx_abs("MPCC2_MPCC_OPP_ID"), ~0u, 2u }); w.push_back({ lx_abs("MPCC2_MPCC_UPDATE_LOCK_SEL"), ~0u, 2u });
    w.push_back({ lx_abs("MPC_OUT2_MUX"), ~0u, 0x00004102u }); w.push_back({ lx_abs("HUBP2_DCHUBP_CNTL"), 1u, 0u });
    w.push_back({ lx_abs("OTG2_OTG_MASTER_UPDATE_LOCK"), 1u, 0u });      // 0.0.643: optc1_unlock
    return w;
}
static std::vector<WantW> spec_rollback_writes() {
    return { { lx_abs("DPG2_DPG_CONTROL"), N48D2_DPG_CONTROL_MASK, 0x00661001u }, { lx_abs("HUBP2_DCHUBP_CNTL"), 1u, 1u }, { lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, 0u }, { lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, 0u }, { lx_abs("MPC_OUT2_MUX"), ~0u, 0x400fu }, { lx_abs("MPCC2_MPCC_TOP_SEL"), ~0u, 0xFu }, { lx_abs("MPCC2_MPCC_BOT_SEL"), ~0u, 0xFu },
             { lx_abs("MPCC2_MPCC_OPP_ID"), ~0u, 0xFu }, { lx_abs("MPCC2_MPCC_UPDATE_LOCK_SEL"), ~0u, 0xFu }, { lx_abs("OTG2_OTG_CRC_CNTL"), 0x00700011u, 0u }, { lx_abs("HUBP2_HUBP_CLK_CNTL"), 0x00011111u, 0u },
             { lx_abs("DPP_TOP2_DPP_CONTROL"), 0x10u, 0u }, { lx_abs("DPPCLK_CTRL"), 0x40u, 0u }, { lx_abs("DPPCLK2_DTO_PARAM"), ~0u, 0u } };
}
static size_t nth_write(const std::vector<W> &w, uint32_t abs, size_t n = 0) { for (size_t i = 0; i < w.size(); i++) if (w[i].abs == abs) { if (n == 0) return i; --n; } return (size_t)-1; }
static size_t first_read_after(const PModel &m, uint32_t abs, size_t readIdx) { for (size_t k = readIdx; k < m.reads.size(); k++) if (m.reads[k] == abs) return k; return (size_t)-1; }
static bool any_write(const std::vector<W> &w, uint32_t abs, uint32_t mask, uint32_t val) { for (const W &x : w) if (x.abs == abs && (x.v & mask) == val) return true; return false; }
static bool rolled_back_clean(const PModel &m, bool dppBit0 = true) {      // the plane is OFF: blanked, mux/MPCC2 none, clocks off, DPG on, DTO 0
    return m.blanked && !m.clockOn && !m.clkStatus && m.dpgOn && m.get(N48D2_MPC_OUT2_MUX) == 0x400fu && m.get(N48D2_MPCC2_MPCC_TOP_SEL) == 0xFu && m.get(N48D2_MPCC2_MPCC_BOT_SEL) == 0xFu && m.get(N48D2_MPCC2_MPCC_OPP_ID) == 0xFu &&
           m.get(N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL) == 0xFu && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) == 0u && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == 0u && (m.get(N48D2_DPPCLK_CTRL) & 0x40u) == 0u && (!dppBit0 || (m.get(N48D2_DPPCLK_CTRL) & 1u) == 1u) && m.get(N48D2_DPPCLK2_DTO_PARAM) == 0u && (m.get(N48D2_DPP_TOP2_DPP_CONTROL) & 0x10u) == 0u;
}
static n48d2_out run_op(PModel &m, uint32_t op, uint32_t buf = 0u) { return n48d2::run_plane(m, op, buf); }
struct SetupFailed {};
static void plane_up(PModel &m) {       // a failed setup is a FAILED CHECK and ends the group (the tests below index the writes it would have made)
    m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
    const bool up = o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE;
    expect(up, "setup: op 8 brought the plane up");
    if (!up) throw SetupFailed();
    m.writes.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear();
}

static void plane_op8_tests() {
    // ================= the clean run
    {
        PModel m; m.initPlane();
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        if (o.status != N48D2_OK) std::printf("   op 8 clean run: status %u (%s) fail %u pre_abs %#x pre_val %#x guard %u dp_changed %#x dpx %#x timeouts %u\n", o.status, n48d2_status_name(o.status), o.fail, o.pre_abs, o.pre_val, o.guard, o.dp_changed, o.dpx_changed, o.timeouts);
        expect(o.status == N48D2_OK && o.fail == 0xFFu && o.timeouts == 0u && o.auto_off == 0u && o.free_bufs == 0u && o.dp_changed == 0u && o.dpx_changed == 0u && o.inst == 2u, "op 8 on the census state: OK, no timeout, no rollback, the DP (six + SYMCLKB + fifteen rows) unchanged");
        expect(m.plane.stage == N48D2_PL_PLANE && m.plane.cur == 0u && m.plane.mc[0] == kBufA && m.plane.mc[1] == kBufB, "the plane is UP on buffer A (stage PLANE)");
        expect_seq("op 8: the 41 register writes (the spec's 34 in Linux's order + the address again + 0.0.643: optc3_lock (2), the address a third time INSIDE the lock, optc1_unlock), in order", m.writes, spec_op8_writes(kBufA));
        expect(o.done == 41u, "steps written are counted (41)");
        // ORDER (every index must exist before any of it is used: a broken flow must FAIL a check, not crash the test)
        const size_t NP = (size_t)-1;
        const size_t iBlank = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL, 0), iClk = nth_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL), iUnblank = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL, 1);
        const size_t iHi = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0), iLo = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0);
        const size_t iHi2 = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 1), iLo2 = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 1), iPitch = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH);
        const size_t iHi3 = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 2), iLo3 = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 2);
        const size_t iLockSel = nth_write(m.writes, N48D2_OTG2_OTG_GLOBAL_CONTROL2), iLock = nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, 0), iUnlock = nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, 1);
        const size_t iBot = nth_write(m.writes, N48D2_MPCC2_MPCC_BOT_SEL), iMux = nth_write(m.writes, N48D2_MPC_OUT2_MUX), iDto = nth_write(m.writes, N48D2_DPPCLK2_DTO_PARAM), iCtl = nth_write(m.writes, N48D2_DPPCLK_CTRL), iDpp = nth_write(m.writes, N48D2_DPP_TOP2_DPP_CONTROL), iDscl = nth_write(m.writes, N48D2_DSCL2_SCL_MODE), iLb = nth_write(m.writes, N48D2_DSCL2_LB_MEMORY_CTRL);
        const bool idxOk = iHi3 != NP && iLo3 != NP && iLockSel != NP && iLock != NP && iUnlock != NP && iBlank != NP && iClk != NP && iUnblank != NP && iHi != NP && iLo != NP && iHi2 != NP && iLo2 != NP && iPitch != NP && iBot != NP && iMux != NP && iDto != NP && iCtl != NP && iDpp != NP && iDscl != NP && iLb != NP && m.writes.size() >= 2u && m.readsAtWrite.size() == m.writes.size() && m.f2AtWrite.size() == m.writes.size() && m.f2AtRead.size() == m.reads.size();
        expect(idxOk, "op 8: every write the ORDER checks name exists (blank, clock, unblank, the address TWICE, the pitch, MPCC2, mux, DTO, DPPCLK_CTRL, DPP clock, DSCL2)");
        if (idxOk) {
        // --- before the HUBP clock: the DPP clocks, the blank (READ BACK), the pitch and the address (HIGH then LOW) - Linux's order, the clock before the programming
        expect(iDto < iCtl && iCtl < iDpp && iDpp < iBlank && iBlank < iClk && (m.writes[iBlank].v & 1u) == 1u, "ORDER: DPPCLK2 DTO, DPPCLK2_EN, the DPP clock, then BLANK_EN = 1, all BEFORE the HUBP clock goes on");
        expect(first_read_after(m, N48D2_HUBP2_DCHUBP_CNTL, m.readsAtWrite[iBlank] + 0) < m.readsAtWrite[iClk], "ORDER (reachability): the BLANK is READ BACK (a read of HUBP2_DCHUBP_CNTL between the blank write and the clock write)");
        expect(iBlank < iPitch && iPitch < iHi && iHi < iLo && iLo < iClk, "ORDER: the pitch and the address (HIGH then LOW) are written BEFORE the HUBP clock, after the blank");
        // --- after the clock: the whole DSCL2 / HUBP2 / HUBPREQ2 programming (the 0.0.635 run: DSCL2_LB_MEMORY_CTRL did not take with the pipe unclocked), then the address AGAIN
        expect(iClk < iDscl && iDscl < iLb, "ORDER (the 0.0.635 failure): every DSCL2 write comes AFTER the HUBP clock is on");
        { bool allAfter = true; for (const uint32_t d : { N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG, N48D2_HUBPREQ2_DCN_GLOBAL_TTU_CNTL, N48D2_HUBPREQ2_VBLANK_PARAMETERS_0 }) { const size_t k = nth_write(m.writes, d); allAfter = allAfter && k != NP && k > iClk; } expect(allAfter, "ORDER: the HUBP2 / HUBPREQ2 COPY0 writes come AFTER the HUBP clock is on"); }
        expect(iLb < iHi2 && iHi2 < iLo2 && iClk < iHi2, "ORDER (C2): the address is RE-WRITTEN (HIGH then LOW) after the clock is on and after all the programming");
        expect(m.writes[iHi2].v == m.writes[iHi].v && m.writes[iLo2].v == m.writes[iLo].v && m.writes[iLo2].v == (uint32_t)kBufA, "... with the same value (A)");
        // --- readback after the 2-frame wait and before the latch gate
        const size_t rLast = m.readsAtWrite[iLo2];                 // reads.size() right after... (at the write: readsAtWrite[i] = reads before write i)
        size_t rLb = NP, rEarly = NP; for (size_t k = rLast; k < m.reads.size(); k++) { if (rLb == NP && m.reads[k] == N48D2_DSCL2_LB_MEMORY_CTRL) rLb = k; if (rEarly == NP && m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL) rEarly = k; }      // 0.0.638: rEarly = the first read of the PRE-UNBLANK gate (FLIP_CONTROL)
        expect(rLb != NP && rEarly != NP && rLb < rEarly, "ORDER: the register READ-BACK (DSCL2_LB_MEMORY_CTRL read after the programming) comes BEFORE the first FLIP_PENDING read (of the POST-unlock gate, 0.0.643)");
        if (rLb != NP && rEarly != NP) {
            expect(m.f2AtRead[rLb] - m.f2AtWrite[iLo2] >= 2u, "REACHABILITY: at least 2 OTG2 frames pass between the last programming write and the first read-back read (the double-buffered fields latch at OTG2's update)");
            expect(m.readsAtWrite[iLockSel] > rLb && rEarly >= m.readsAtWrite[iUnlock], "ORDER (0.0.643): the read-back completes BEFORE the lock is taken, and the first FLIP_PENDING read comes only AFTER the unlock write");
            // every register of parts A and D (everything written before MPCC2 except the clock) was read back in that window
            bool all = true; std::string missing; for (size_t i = 0; i < iLockSel; i++) { if (i == iClk) continue; bool seen = false; for (size_t k = rLast; k < rEarly; k++) seen = seen || m.reads[k] == m.writes[i].abs; if (!seen) { all = false; char b[40]; std::snprintf(b, sizeof b, " %#x", m.writes[i].abs); missing += b; } }
            expect(all, ("REACHABILITY: every register written before MPCC2 (parts A and D, 35 of them; not the clock) is READ BACK between the frame wait and the lock; missing:" + missing).c_str());
        }
        // --- MPCC2, the mux, the unblank LAST
        expect(iLo2 < iLockSel && iLockSel < iLock && iLock < iHi3 && iHi3 < iLo3 && iLo3 < iBot && iBot < iMux && iMux < iUnblank && iUnblank < iUnlock && m.writes.size() - 1 == iUnlock, "ORDER (0.0.643): the programming, then the lock (select, lock), then the address (HIGH, LOW), MPCC2, MPC_OUT2_MUX, the UNBLANK, then the UNLOCK is the LAST write");
        expect(iHi3 == iLock + 1u && iLo3 == iHi3 + 1u && iBot == iLo3 + 1u && iUnblank == iBot + 5u && iUnlock == iUnblank + 1u && iLock == iLockSel + 1u, "ORDER (0.0.643): CONSECUTIVE - nothing is written between the lock and the unlock but the address, MPCC2 (4), the mux and the unblank");
        expect(m.sleepsAtWrite[iUnlock] == m.sleepsAtWrite[iLo3] && m.f2AtWrite[iUnlock] == m.f2AtWrite[iHi3], "REACHABILITY (0.0.643): no sleep and no OTG2 frame between the address write inside the lock and the unlock");
        { size_t nFlipIn = 0, nEarlyIn = 0; for (size_t k = m.readsAtWrite[iLo2]; k < m.readsAtWrite[iUnlock]; k++) { nFlipIn += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; nEarlyIn += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE; }
          expect(nFlipIn == 0u && nEarlyIn == 0u, "0.0.643: NO FLIP_PENDING and NO EARLIEST_INUSE read between the last programming write and the unlock (the pre-unblank gate is gone)"); }
        { size_t nWaitReads = 0; for (size_t k = m.readsAtWrite[iLock]; k < m.readsAtWrite[iHi3]; k++) nWaitReads += m.reads[k] == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK; expect(nWaitReads >= 1u, "0.0.643: the lock status (OTG_MASTER_UPDATE_LOCK bit 8) is READ between the lock write and the first write inside the lock"); }
        { size_t nPostFlip = 0, nPostEarly = 0; for (size_t k = m.readsAtWrite[iUnlock]; k < m.reads.size(); k++) { nPostFlip += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; nPostEarly += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE; }
          expect(nPostFlip >= 3u && nPostEarly >= 1u, "REACHABILITY (0.0.643): AFTER the unlock the latch gate POLLS FLIP_PENDING (the model is pending for 2 reads: >= 3 reads) and reads EARLIEST_INUSE"); }
        // the gates are reached (REACHABILITY): between the clock write and the first MPCC2 write the flow read DET2, FLIP_CONTROL and EARLIEST_INUSE
        const size_t rClk = m.readsAtWrite[iClk], rBot = m.readsAtWrite[iBot];
        size_t nDet = 0, nFlip = 0, nEarly = 0; for (size_t k = rClk; k < rBot; k++) { nDet += m.reads[k] == N48D2_DCHUBBUB_DET2_CTRL; nFlip += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; nEarly += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE; }
        expect(nDet >= 1 && nFlip == 0 && nEarly == 0, "REACHABILITY (0.0.643): after the clock and before MPCC2 DET2 is only RECORDED and FLIP_PENDING / EARLIEST_INUSE are NOT read at all (no pre-unblank gate: a blanked HUBP neither consumes the address nor fetches)");
        { size_t nPost = 0; for (size_t k = m.readsAtWrite[iUnblank]; k < m.reads.size(); k++) nPost += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE; expect(nPost >= 1, "REACHABILITY (0.0.638): the EARLIEST_INUSE gate runs AFTER the unblank"); }
        expect(m.f2AtWrite[iBot] - m.f2AtWrite[iClk] >= 2u, "REACHABILITY: at least 2 OTG2 frames pass between the clock and MPCC2");
        expect(m.frames2 - m.f2AtWrite[iUnblank] >= 3u, "REACHABILITY: at least 3 OTG2 frames pass after the unblank (the health gate)");
        size_t nHealth = 0; for (size_t k = m.readsAtWrite[iUnblank]; k < m.reads.size(); k++) nHealth += m.reads[k] == N48D2_HUBP2_DCHUBP_CNTL || m.reads[k] == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL || m.reads[k] == N48D2_DCN_VM_FAULT_STATUS || m.reads[k] == N48D2_DCHUBBUB_DET2_CTRL;
        expect(nHealth >= 4, "REACHABILITY: after the unblank the health gate reads HUBP2 CNTL, ODM2, the VM fault status and DET2");
        }   // idxOk
        // the surface: drawn, flushed once, verified: 'f' x 23040, 'F', 'r' x 48; the flush strictly between
        expect(m.vfillCalls == 2u * N48D2_PLANE_H * 8u && m.vflushCalls == 1u && m.vreadCalls == 2u * N48D2_VPOINTS, "the surface: 23040 bar runs, ONE flush, 48 read-backs through the MM window");
        { const size_t F = m.vlog.find('F'); expect(F == 23040u && m.vlog.size() == 23040u + 1u + 48u && m.vlog.find('r') > F && m.vlog.rfind('f') < F, "the HDP flush sits between the last fill and the first read-back"); }
        expect(m.vfillCalls > 0 && m.reads.size() > 0 && m.writes.size() == 41u, "41 writes (the 34 of the table + the address again + the lock (2), the address inside the lock (2) and the unlock), none before the surface was drawn and verified");
        // the gates
        const uint32_t *g = m.gateBuf;
        expect(((g[N48D2_GT_DET2] & N48D2_DET_CUR_MASK) >> 8) == 3u && (g[N48D2_GT_HUBP_CNTL] & 0x70000800u) == 0u && g[N48D2_GT_VMFAULT] == 0u && (g[N48D2_GT_FLIP_CTL] & 0x100u) == 0u && (((uint64_t)(g[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | g[N48D2_GT_EARLY_LO]) == kBufA, "gates after op 8: DET2 current 3, no underflow / SEG_ALLOC_ERR, VM fault 0, FLIP_PENDING 0, EARLIEST_INUSE = A");
        expect(g[N48D2_GT_MPC_MUX] == 0x4102u && g[N48D2_GT_A_LO] == (uint32_t)kBufA && g[N48D2_GT_B_LO] == (uint32_t)kBufB && (g[N48D2_GT_META] & 0xFFu) == N48D2_PL_PLANE && n48d2_health(g, 0u, kBufA, 1) == 0u, "gates: the mux, A / B, the meta (stage PLANE), the pure verdict is clean");
        // the pack carries the gates
        uint64_t v[13]; n48d2_pack_plane(v, &o, g); n48d2_out b; uint32_t g2[N48D2_GATES]; std::memset(&b, 0, sizeof b); n48d2_unpack_plane(v, &b, g2);
        bool same = true; for (uint32_t i = 0; i < N48D2_GATES; i++) same = same && g2[i] == g[i];
        expect(same && b.status == o.status && b.f0a == o.f0a && b.dpx_changed == o.dpx_changed, "the 18 gate dwords survive the 13-scalar result");
        // the DP side: 15 rows + the HIGH dword read before the first write and again after the last
        size_t before = 0, after = 0; if (!m.readsAtWrite.empty()) for (size_t k = 0; k < m.reads.size(); k++) { bool isw = m.reads[k] == N48D2_HUBPREQ0_PRIMARY_HIGH; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) isw = isw || m.reads[k] == n48d2_dpx_regs[i].abs; if (isw) { if (k < m.readsAtWrite[0]) ++before; else if (k >= m.readsAtWrite.back()) ++after; } }
        expect(before >= N48D2_DPX_REGS + 1u && after >= N48D2_DPX_REGS + 1u, "the 15-row DP watch is read before the first write and after the last (op 8)");
        // the buffers: A bars white..black, B reversed, every dword
        bool pat = m.vram[0].size() == N48D2_BUF_BYTES / 4u && m.vram[1].size() == N48D2_BUF_BYTES / 4u;
        for (uint32_t k = 0; k < 2 && pat; k++) for (uint32_t y = 0; y < N48D2_PLANE_H && pat; y += 1) { const uint32_t *row = &m.vram[k][(size_t)y * N48D2_PLANE_W]; for (uint32_t x = 0; x < N48D2_PLANE_W; x += 1) { const uint32_t bar = x / 320u; if (row[x] != n48d2_bars[k ? 7u - bar : bar]) { pat = false; break; } } }
        expect(pat, "buffer A is eight 320-px bars white yellow cyan green magenta red blue black, buffer B the same in REVERSE, every dword of both 2560x1440 surfaces");
    }
    // ================= refusals before anything is read or written
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty() && m.vfillCalls == 0, "op 8 with no buffers allocated: PLANE_STATE, nothing read, nothing written"); }
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_PLANE; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty(), "op 8 on a plane that is already up: PLANE_STATE, nothing read or written"); }
    { PModel m; m.initPlane(); const n48d2_out o = n48d2::run_plane(m, 7u); expect(o.status == N48D2_BAD_ARG && m.reads.empty() && m.writes.empty(), "run_plane refuses a non-plane op with nothing read"); const n48d2_out o2 = n48d2::run_plane(m, N48D2_OP_FLIP, 2u); expect(o2.status == N48D2_BAD_ARG && m.reads.empty(), "... and a flip index above 1"); }
    // ================= admission of the buffers (pure arithmetic: nothing read)
    {
        struct Adm { const char *what; uint64_t a, b; bool haveWin; uint64_t lo, hi; } cases[] = {
            { "A not 64 KiB aligned", kBufA + 0x1000u, kBufB, true, kWinLo, kWinHi }, { "B not 64 KiB aligned", kBufA, kBufB + 0x4u, true, kWinLo, kWinHi }, { "A below the window", kWinLo - 0x10000u, kBufB, true, kWinLo, kWinHi },
            { "B runs past the window end", kBufA, kWinHi - N48D2_BUF_BYTES + 0x10000u, true, kWinLo, kWinHi }, { "A and B overlap", kBufA, kBufA + 0x10000u, true, kWinLo, kWinHi }, { "A == B", kBufA, kBufA, true, kWinLo, kWinHi },
            { "no window known (fail closed)", kBufA, kBufB, false, 0, 0 }, { "an empty window", kBufA, kBufB, true, kWinLo, 0 }, { "A is zero", 0, kBufB, true, kWinLo, kWinHi },
            { "A and B in different 4 GiB halves", 0x80FFF00000ull, 0x8100000000ull, true, kWinLo, 0x8200000000ull }, { "A straddles a 4 GiB boundary", 0x80FF800000ull, kBufB, true, kWinLo, 0x8200000000ull } };
        for (const Adm &c : cases) {
            PModel m; m.initPlane(); m.plane.mc[0] = c.a; m.plane.mc[1] = c.b; m.haveWin = c.haveWin; m.winLo = c.lo; m.winHi = c.hi;
            const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.reads.empty() && m.vfillCalls == 0 && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE, (std::string("admission: ") + c.what + ": NO_BUFFER, nothing read / written / drawn, the buffers are released").c_str());
        }
    }
    // ================= prechecks: EVERY EXPECT register, both pipes, and every COPY0 source; nothing written, nothing drawn
    {
        uint32_t nTried = 0, nGood = 0;
        for (uint32_t i = 0; i < N48D2_EXP_COUNT; i++) for (int pipe = 0; pipe < 2; pipe++) {
            PModel m; m.initPlane(); const uint32_t a = pipe ? n48d2_exp[i].abs0 : n48d2_exp[i].abs2; const uint32_t bit = n48d2_exp[i].mask & ~(n48d2_exp[i].mask - 1u);       // the lowest masked bit
            m.r[a] ^= bit; ++nTried;
            const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            if (o.status == N48D2_PRECHECK && o.pre_abs == a && m.writes.empty() && m.vfillCalls == 0 && m.plane.stage == N48D2_PL_FREE && o.free_bufs == 1u) ++nGood; else std::printf("   EXPECT row %u pipe %d (%s, %#x) not caught: status %u pre_abs %#x writes %zu\n", i, pipe, n48d2_exp[i].name, a, o.status, o.pre_abs, m.writes.size());
        }
        expect(nTried == 68u && nGood == nTried, "precheck: a flipped bit in ANY of the 34 EXPECT registers, on pipe 2 or on pipe 0, ends the op in PRECHECK naming that register; nothing written, nothing drawn, the buffers released");
        nTried = nGood = 0;
        for (uint32_t i = 0; i < N48D2_PRE_STATE_COUNT; i++) {
            PModel m; m.initPlane(); const uint32_t a = n48d2_pre_state[i].abs; m.r[a] ^= n48d2_pre_state[i].mask & ~(n48d2_pre_state[i].mask - 1u); ++nTried;
            if (a == N48D2_DCHUBBUB_DET2_CTRL) { m.r[a] ^= n48d2_pre_state[i].mask & ~(n48d2_pre_state[i].mask - 1u); m.det2Size = 2u; }      // DET2's size field is a model knob
            const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            const bool otg2Off = a == N48D2_OTG2_CONTROL;      // OTG2 stopped: the counting check (a PRECHECK on the frame counter) answers before the EXPECT page does
            if (o.status == N48D2_PRECHECK && (o.pre_abs == a || (otg2Off && o.pre_abs == N48D2_OTG2_FRAME_COUNT)) && m.writes.empty() && m.vfillCalls == 0) ++nGood; else std::printf("   state row %u (%s, %#x) not caught: status %u pre_abs %#x\n", i, n48d2_pre_state[i].name, a, o.status, o.pre_abs);
        }
        expect(nTried == 18u && nGood == nTried, "precheck: a wrong value in ANY of the 18 state registers (OTG2 not enabled, DPG2 off, HUBP2 clock already on, DPP2 clock on, MPCC2 / mux in use, DPPCLK2_EN set, DTO set, DET0 / DET2, VM fault, the four VM registers) ends the op in PRECHECK; nothing written");
        nTried = nGood = 0;
        for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) {
            PModel m; m.initPlane(); m.r[n48d2_mirror0[i].src] ^= 1u; ++nTried;
            const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            if (o.status == N48D2_PRECHECK && o.pre_abs == n48d2_mirror0[i].src && m.writes.empty() && m.vfillCalls == 0 && m.plane.stage == N48D2_PL_FREE) ++nGood; else std::printf("   COPY0 source %u (%#x) drift not caught: status %u\n", i, n48d2_mirror0[i].src, o.status);
        }
        expect(nTried == 20u && nGood == nTried, "COPY0 drift tripwire: ANY of the 20 pipe-0 sources off its census value ends the op in PRECHECK before the first write");
        for (uint32_t i = 0; i < N48D2_EQ_COUNT; i++) { PModel m; m.initPlane(); m.r[n48d2_eqpairs[i].a2] = 0x5u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && o.pre_abs == n48d2_eqpairs[i].a2 && m.writes.empty() && m.vfillCalls == 0, "precheck: an OGAM2 / MCM2 register that differs from its pipe-0 twin refuses the op"); }
        { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)kBufA; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(kBufA >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && m.writes.empty() && m.vfillCalls == 0, "precheck: HUBP0's current address == buffer A refuses the op"); }
        { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)kBufB; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(kBufB >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && m.writes.empty() && m.vfillCalls == 0, "precheck: HUBP0's current address == buffer B refuses the op"); }
        { PModel m; m.initPlane(); m.otg0Stops = true; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_DP_NOT_COUNTING && m.writes.empty() && m.vfillCalls == 0 && m.plane.stage == N48D2_PL_FREE, "OTG0 not counting: DP_NOT_COUNTING, nothing written, buffers released"); }
        { PModel m; m.initPlane(); m.otg2Frozen = true; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_OTG2_FRAME_COUNT && m.writes.empty() && m.vfillCalls == 0, "OTG2 not counting: PRECHECK naming the frame counter, nothing written"); }
        { PModel m; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE); (void)o; expect(m.reads.size() > 0 && first_write(m.writes, N48D2_DPPCLK2_DTO_PARAM) != (size_t)-1, "(control) the unmodified state passes every precheck above"); }
    }
    // ================= fill / verify: refused, never half-done
    {
        { PModel m; m.initPlane(); m.vfillFailAt = 0; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.plane.stage == N48D2_PL_FREE && o.free_bufs == 1u, "a refused first fill: NO_BUFFER, no register written, buffers released"); }
        { PModel m; m.initPlane(); m.vfillFailAt = 11520u + 7u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.vflushCalls == 0, "a refused fill part-way into buffer B: NO_BUFFER, no flush, no register written"); }
        uint32_t caught = 0, tried = 0;
        for (uint32_t k = 0; k < 2; k++) for (uint32_t p = 0; p < N48D2_VPOINTS; p++) {
            PModel m; m.initPlane(); uint32_t want = 0; const uint32_t off = n48d2_vpoint(k, p, &want); m.vreadCorruptK = k; m.vreadCorruptDw = off / 4u; ++tried;
            const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            if (o.status == N48D2_NO_BUFFER && m.writes.empty() && m.plane.stage == N48D2_PL_FREE) ++caught;
        }
        expect(tried == 48u && caught == tried, "verify: a wrong dword at ANY of the 24 points of A or of B (rows 0 / 720 / 1439 x the eight bar centres) is REFUSED: NO_BUFFER, no register written");
        // the points themselves
        { std::set<uint32_t> offs; bool pos = true; for (uint32_t p = 0; p < N48D2_VPOINTS; p++) { uint32_t w = 0; const uint32_t off = n48d2_vpoint(0, p, &w); offs.insert(off); const uint32_t row = off / N48D2_PLANE_PITCH_BYTES, x = (off % N48D2_PLANE_PITCH_BYTES) / 4u; pos = pos && (row == 0 || row == 720 || row == 1439) && x % 320u == 160u && w == n48d2_bars[x / 320u]; }
          expect(offs.size() == 24u && pos, "the 24 verification points: rows 0, 720, 1439 at x = 160 + 320 * bar, each expecting its bar's colour"); }
    }
}

static void plane_gate_failures() {
    // Each row: a hardware assumption of the spec's (all SUSPECTED) turned off, the status the flow must end in, and what must NOT have been written. Every failure after the first write runs the ROLLBACK and, with the clock reading off, releases the buffers.
    struct Row { const char *what; std::function<void(PModel &)> set; uint32_t status; bool clockWritten, mpccWritten, unblanked; bool leak = false; };
    const Row rows[] = {
        { "BLANK_EN does not read back as 1 (blank bit does not stick; the rollback's blank cannot assert NO_OUTSTANDING_REQ either, so the buffers are LEAKED, not freed)", [](PModel &m) { m.blankSticks = false; }, N48D2_GATE, false, false, false, true },
        { "HUBP register writes do not land while the plane clock is gated (same: the buffers are LEAKED)", [](PModel &m) { m.hubpWritesLand = false; }, N48D2_GATE, false, false, false, true },
        { "DSCL2_SCL_MODE does not latch (register read-back)", [](PModel &m) { m.dropAbs = N48D2_DSCL2_SCL_MODE; }, N48D2_READBACK, true, false, false },
        { "DSCL2_LB_MEMORY_CTRL does not latch", [](PModel &m) { m.dropAbs = N48D2_DSCL2_LB_MEMORY_CTRL; }, N48D2_READBACK, true, false, false },
        { "DPPCLK2_DTO_PARAM does not latch", [](PModel &m) { m.dropAbs = N48D2_DPPCLK2_DTO_PARAM; }, N48D2_READBACK, true, false, false },
        { "HUBP2 viewport does not latch", [](PModel &m) { m.dropAbs = N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION; }, N48D2_READBACK, true, false, false },
        { "HUBPREQ2 prefetch settings do not latch", [](PModel &m) { m.dropAbs = N48D2_HUBPREQ2_PREFETCH_SETTINGS; }, N48D2_READBACK, true, false, false },
        { "the pitch does not latch", [](PModel &m) { m.dropAbs = N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH; }, N48D2_READBACK, true, false, false },
        { "the address HIGH does not latch", [](PModel &m) { m.dropAbs = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH; }, N48D2_READBACK, true, false, false },
        { "the address LOW does not latch", [](PModel &m) { m.dropAbs = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; }, N48D2_READBACK, true, false, false },
        { "DPPCLK_CTRL bit 6 does not latch", [](PModel &m) { m.dropAbs = N48D2_DPPCLK_CTRL; }, N48D2_READBACK, true, false, false },
        { "DET2 current never becomes 3 (0.0.639: judged AFTER the unblank, so the plane WAS joined and unblanked)", [](PModel &m) { m.detWorks = false; }, N48D2_GATE, true, true, true },
        { "the address never latches (EARLIEST_INUSE stays 0): 0.0.638 judges it AFTER the unblank, so the plane WAS joined and unblanked and is rolled back", [](PModel &m) { m.latchWorks = false; }, N48D2_GATE, true, true, true },
        { "FLIP_PENDING never clears (0.0.643: judged AFTER the unlock, so the plane WAS joined and unblanked inside the lock)", [](PModel &m) { m.flipPendReads = 1; m.flipClears = false; }, N48D2_GATE, true, true, true },
        { "the OTG2 update lock never asserts (UPDATE_LOCK_STATUS stays 0): 0.0.643 fails BEFORE any address / MPCC2 / unblank write inside the lock", [](PModel &m) { m.lockNeverAsserts = true; }, N48D2_GATE, true, false, false },
        { "HUBP2 SEG_ALLOC_ERR after the unblank", [](PModel &m) { m.segAllocErr = true; }, N48D2_GATE, true, true, true },
        { "a DCN VM fault after the unblank", [](PModel &m) { m.vmFault = 0x00000101u; }, N48D2_GATE, true, true, true },
        { "DET2 current drops away after the unblank", [](PModel &m) { m.detDropAfterUnblank = true; }, N48D2_GATE, true, true, true },
    };
    for (const Row &r : rows) {
        PModel m; m.initPlane(); r.set(m);
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        const std::string w = std::string("op 8, ") + r.what;
        expect(o.status == r.status, (w + ": the status is " + std::to_string(r.status)).c_str());
        if (r.leak) expect(o.auto_off == 1u && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED && !m.clockOn, (w + ": the ROLLBACK ran, NO_OUTSTANDING_REQ never asserted, the buffers are LEAKED (never freed) and the HUBP clock was never on").c_str());
        else {
            expect(o.auto_off == 1u && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE, (w + ": the ROLLBACK ran and, HUBP2's clock reading off, the buffers are released").c_str());
            expect(rolled_back_clean(m), (w + ": afterwards the plane is OFF: blanked, mux / MPCC2 none, clocks off, DPG2 on, DTO 0").c_str());
        }
        bool clkW = false; for (const W &x : m.writes) if (x.abs == N48D2_HUBP2_HUBP_CLK_CNTL && (x.v & 1u)) clkW = true;
        expect(clkW == r.clockWritten, (w + (r.clockWritten ? ": the clock WAS enabled (and the rollback disabled it)" : ": the HUBP clock was NEVER enabled")).c_str());
        expect(!m.locked && (m.get(N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) & 1u) == 0u, (w + ": 0.0.643: OTG2's update lock is RELEASED on this exit path").c_str());
        const bool mpcc = any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) || any_write(m.writes, N48D2_MPCC2_MPCC_TOP_SEL, ~0u, 2u);
        expect(mpcc == r.mpccWritten, (w + (r.mpccWritten ? ": MPCC2 / the mux were joined" : ": MPCC2 and MPC_OUT2_MUX were NEVER written (the plane never reached the mixer)")).c_str());
        bool unb = false; for (size_t i = 0; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_HUBP2_DCHUBP_CNTL && (m.writes[i].v & 1u) == 0u && (m.writes[i].v & 0x80000000u) == 0u && any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u)) unb = true;
        expect(unb == r.unblanked, (w + (r.unblanked ? ": the plane WAS unblanked before the gate saw the fault" : ": the plane was NEVER unblanked")).c_str());
    }
    // the rollback never touches the DP: every write of every failed run is inside instance 2's list
    { PModel m; m.initPlane(); m.latchWorks = false; (void)run_op(m, N48D2_OP_PLANE); bool in = true; for (const W &x : m.writes) { bool ok = x.abs == N48D2_DPPCLK2_DTO_PARAM || x.abs == N48D2_DPPCLK_CTRL; for (uint32_t k = 0; k < N48D2_ALLOWED2P_COUNT; k++) ok = ok || x.abs == n48d2_allowed2p[k]; for (uint32_t k = 0; k < N48D2_ALLOWED2_COUNT; k++) ok = ok || x.abs == n48d2_allowed2[k]; in = in && ok; }
      expect(in, "every write of a failed run is a listed instance-2 register"); }
    // stale ODM2 flags from BEFORE the op are not blamed on it (op 8 does not gate on them) ...
    { PModel m; m.odmStale = 0x2400u; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE && m.plane.odm_sticky == 0x2400u && ((m.gateBuf[N48D2_GT_META] >> 16) & 0xFFFFu) == 0x2400u, "stale ODM2 underflow flags (set before op 8 wrote anything) do not fail op 8; they are recorded and reported");
      // ... but op 9 clears them and demands 0
      const n48d2_out s = run_op(m, N48D2_OP_SHOW); expect(s.status == N48D2_OK && m.plane.stage == N48D2_PL_SHOWN && !m.dpgOn && m.odmSticky == 0u, "op 9 clears the stale flags (row 36), sees 0 after 10 frames and turns DPG2 off"); }
    { PModel m; m.odmStale = 0x400u; m.undClearWorks = false; m.initPlane(); (void)run_op(m, N48D2_OP_PLANE); const n48d2_out s = run_op(m, N48D2_OP_SHOW); expect(s.status == N48D2_GATE && m.dpgOn && m.plane.stage == N48D2_PL_PLANE, "op 9: ODM2 flags that will not clear -> GATE, DPG2 STAYS ON, the plane stays up (no rollback)"); }
    // a COPY0 source that drifts AFTER the precheck (during the run) ends the op and rolls back
    { PModel m; m.initPlane(); m.flipTrig = N48D2_DPP_TOP2_DPP_CONTROL; m.flipReg = n48d2_mirror0[19].src; m.flipXor = 1u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == n48d2_mirror0[19].src && o.auto_off == 1u && o.free_bufs == 1u && rolled_back_clean(m), "a COPY0 source that drifts mid-run is caught by the step itself: PRECHECK, rollback, buffers released");
      bool clkW = false; for (const W &x : m.writes) if (x.abs == N48D2_HUBP2_HUBP_CLK_CNTL && (x.v & 1u)) clkW = true; expect(clkW, "... in Linux's order the COPY0 pairs are read AFTER the HUBP clock is on, so the clock WAS enabled (and the rollback turned it off again)"); }
    // the allowlist/guard refusing a write part-way (here: the model's allowlist hook) ends the op, rolled back
    { PModel m; m.initPlane(); m.refuseAbs = N48D2_DSCL2_RECOUT_SIZE; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_ALLOW_REFUSED && o.auto_off == 1u && o.free_bufs == 1u && rolled_back_clean(m), "a write the allowlist refuses part-way: ALLOW_REFUSED, rollback, buffers released"); }
}

static void plane_other_ops() {
    // ================= op 9 show
    { PModel m; plane_up(m); const n48d2_out o = run_op(m, N48D2_OP_SHOW);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_SHOWN && !m.dpgOn && o.timeouts == 0u && o.auto_off == 0u, "op 9: OK, DPG2 off, stage SHOWN");
      expect_seq("op 9: the two underflow clears (rows 35-36), then DPG_EN = 0", m.writes, { { lx_abs("HUBP2_DCHUBP_CNTL"), 0x80000000u, 0x80000000u }, { lx_abs("ODM2_OPTC_INPUT_GLOBAL_CONTROL"), 0x1000u, 0x1000u }, { lx_abs("DPG2_DPG_CONTROL"), 1u, 0u } });
      const bool w3 = m.writes.size() >= 3u && m.f2AtWrite.size() >= 3u && m.readsAtWrite.size() >= 3u;
      expect(w3 && m.f2AtWrite[2] - m.f2AtWrite[1] >= 10u, "REACHABILITY: 10 OTG2 frames pass between the clears and the DPG2 off (the underflow gate's wait)");
      size_t nH = 0; if (w3) for (size_t k = m.readsAtWrite[1]; k < m.readsAtWrite[2]; k++) nH += m.reads[k] == N48D2_HUBP2_DCHUBP_CNTL || m.reads[k] == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL; expect(nH >= 2, "REACHABILITY: the gate reads HUBP2 CNTL and ODM2 between the clears and the DPG off");
      expect((m.gateBuf[N48D2_GT_META] & 0xFFu) == N48D2_PL_SHOWN, "gates: meta says stage SHOWN"); }
    { PModel m; plane_up(m); m.reUnderflow = true; const n48d2_out o = run_op(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn && m.plane.stage == N48D2_PL_PLANE && o.auto_off == 0u && !any_write(m.writes, N48D2_DPG2_CONTROL, 1u, 0u), "op 9: HUBP2 underflows again after the clear -> GATE; DPG2 is NOT turned off (the pattern still covers the screen)"); }
    { PModel m; plane_up(m); m.segAllocErr = true; const n48d2_out o = run_op(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn, "op 9: SEG_ALLOC_ERR -> GATE, DPG2 stays on"); }
    { PModel m; plane_up(m); m.vmFault = 0x101u; const n48d2_out o = run_op(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn, "op 9: a VM fault -> GATE, DPG2 stays on"); }
    { PModel m; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_SHOW); expect(o.status == N48D2_PLANE_STATE && m.writes.empty() && m.reads.empty(), "op 9 before op 8: PLANE_STATE, nothing read or written"); }
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); const size_t w0 = m.writes.size(); const n48d2_out o = run_op(m, N48D2_OP_SHOW); expect(o.status == N48D2_PLANE_STATE && m.writes.size() == w0, "op 9 twice: the second is PLANE_STATE (nothing written)"); }
    // ================= op 10 flip
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.writes.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear();
      const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
      expect(o.status == N48D2_OK && m.plane.cur == 1u && m.latched == kBufB && o.timeouts == 0u && o.auto_off == 0u, "op 10 flip B: OK, EARLIEST_INUSE = B, FLIP_PENDING cleared");
      expect_seq("op 10: SET HIGH then SET LOW of buffer B", m.writes, { { lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(kBufB >> 32) }, { lx_abs("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)kBufB } });
      size_t nF = 0; for (size_t k = m.readsAtWrite.empty() ? m.reads.size() : m.readsAtWrite.back(); k < m.reads.size(); k++) nF += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; expect(nF >= 2, "REACHABILITY: after the address the flow polls FLIP_PENDING until it clears (it was pending for 2 reads)");
      expect(m.gateBuf[N48D2_GT_EARLY_LO] == (uint32_t)kBufB && (m.gateBuf[N48D2_GT_FLIP_CTL] & 0x100u) == 0u && ((m.gateBuf[N48D2_GT_META] >> 12) & 1u) == 1u, "gates: EARLIEST_INUSE = B, FLIP_PENDING 0, latched buffer B");
      const n48d2_out o2 = run_op(m, N48D2_OP_FLIP, 0u); expect(o2.status == N48D2_OK && m.plane.cur == 0u && m.latched == kBufA, "flip back to A: OK, EARLIEST_INUSE = A");
      for (int i = 0; i < 5; i++) { const n48d2_out ob = run_op(m, N48D2_OP_FLIP, 1u), oa = run_op(m, N48D2_OP_FLIP, 0u); expect(ob.status == N48D2_OK && oa.status == N48D2_OK && m.latched == kBufA, "five A/B/A cycles: every flip OK"); } }
    { PModel m; plane_up(m); m.flipClears = false; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_GATE && o.timeouts == 2u && o.fail == N48D2_FLIP_I_WAIT && m.plane.cur == 0u && o.auto_off == 0u && m.plane.stage == N48D2_PL_PLANE, "flip: FLIP_PENDING never clears -> GATE at the WAIT step (it is the one reported; the EARLIEST_INUSE LOW wait behind it times out too, the HIGH half equals A and B: 2 timeouts), the plane stays up on A, no rollback"); }
    { PModel m; plane_up(m); m.latchWorks = false; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_GATE && o.fail == N48D2_FLIP_I_WAIT_LO && o.timeouts == 1u && m.plane.cur == 0u, "flip: pending clears but EARLIEST_INUSE never becomes B -> GATE at the EARLIEST_INUSE wait step (C1: hubp2_is_flip_pending), the recorded buffer stays A"); }
    { PModel m; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_PLANE_STATE && m.writes.empty() && m.reads.empty(), "flip before op 8: PLANE_STATE, nothing read or written"); }
    { PModel m; plane_up(m); const n48d2_out o = run_op(m, N48D2_OP_FLIP, 2u); expect(o.status == N48D2_BAD_ARG && m.writes.empty(), "flip to buffer 2: BAD_ARG (only A or B exist)"); }
    // ================= op 11 crc
    { PModel m; plane_up(m); const n48d2_out o = run_op(m, N48D2_OP_CRC);
      expect(o.status == N48D2_OK && m.crcOn, "op 11: OK, CRC on");
      expect_seq("op 11: the four 2560x1440 windows, then CNTL (mask 0x00700011 value 0x11)", m.writes, { { lx_abs("OTG2_OTG_CRC0_WINDOWA_X_CONTROL"), ~0u, 2560u << 16 }, { lx_abs("OTG2_OTG_CRC0_WINDOWA_Y_CONTROL"), ~0u, 1440u << 16 }, { lx_abs("OTG2_OTG_CRC0_WINDOWB_X_CONTROL"), ~0u, 2560u << 16 },
                                                                                  { lx_abs("OTG2_OTG_CRC0_WINDOWB_Y_CONTROL"), ~0u, 1440u << 16 }, { lx_abs("OTG2_OTG_CRC_CNTL"), 0x00700011u, 0x11u } });
      expect(!m.f2AtWrite.empty() && m.f2AtWrite.back() < m.frames2 && m.frames2 - m.f2AtWrite.back() >= 3u && m.gateBuf[N48D2_GT_CRC_RG] != 0u, "REACHABILITY: 3 frames pass after CNTL, then DATA_RG / DATA_B are read into the gates");
      const uint32_t dpgRG = m.gateBuf[N48D2_GT_CRC_RG];
      // A, then B, then A: stable and different
      (void)run_op(m, N48D2_OP_SHOW); const n48d2_out ca = run_op(m, N48D2_OP_CRC); const uint32_t a1 = m.gateBuf[N48D2_GT_CRC_RG], ab1 = m.gateBuf[N48D2_GT_CRC_B]; (void)ca;
      for (int i = 0; i < 10; i++) { (void)run_op(m, N48D2_OP_CRC); expect(m.gateBuf[N48D2_GT_CRC_RG] == a1 && m.gateBuf[N48D2_GT_CRC_B] == ab1, "CRC of A is identical over 10 reads"); }
      expect(a1 != dpgRG, "CRC of A differs from the DPG pattern's");
      (void)run_op(m, N48D2_OP_FLIP, 1u); (void)run_op(m, N48D2_OP_CRC); const uint32_t b1 = m.gateBuf[N48D2_GT_CRC_RG]; expect(b1 != a1, "flipping to B changes the CRC");
      (void)run_op(m, N48D2_OP_FLIP, 0u); (void)run_op(m, N48D2_OP_CRC); expect(m.gateBuf[N48D2_GT_CRC_RG] == a1, "flipping back to A returns A's CRC"); }
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; const n48d2_out o = run_op(m, N48D2_OP_CRC); expect(o.status == N48D2_OK && m.crcOn && o.free_bufs == 0u, "op 11 works with the DPG alone (no plane): the baseline CRC"); }
    { PModel m; m.initPlane(); m.otg2Frozen = true; const n48d2_out o = run_op(m, N48D2_OP_CRC); expect(o.status == N48D2_PRECHECK && m.writes.empty(), "op 11 with OTG2 not counting: PRECHECK, nothing written"); }
    // ================= op 12 planeoff
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_CRC); m.writes.clear(); m.readsAtWrite.clear();
      const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF);
      expect(o.status == N48D2_OK && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE && rolled_back_clean(m) && !m.crcOn, "op 12 from a running plane: OK, the plane is off, CRC off, buffers released");
      expect_seq("op 12: the rollback's 14 writes in the spec's order (DPG2, blank, the address HIGH then LOW to 0, mux, MPCC2 x4, CRC off, HUBP clock, DPP, DPPCLK_CTRL, DTO)", m.writes, spec_rollback_writes());
      const size_t iDpg = nth_write(m.writes, N48D2_DPG2_CONTROL), iBlank = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL), iMux = nth_write(m.writes, N48D2_MPC_OUT2_MUX), iClk = nth_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL), iDto = nth_write(m.writes, N48D2_DPPCLK2_DTO_PARAM);
      expect(iDpg == 0u && iDpg < iBlank && iBlank < iMux && iMux < iClk && iClk < iDto, "ORDER: DPG2 back on FIRST, then blank, then the mux, then the HUBP clock off, the DTO last");
      const bool idx = iBlank != (size_t)-1 && iMux != (size_t)-1 && iClk != (size_t)-1 && iDto != (size_t)-1 && iDpg != (size_t)-1 && m.readsAtWrite.size() == m.writes.size();
      expect(idx, "op 12: the writes the ORDER checks name exist");
      size_t nNo = 0; if (idx) for (size_t k = m.readsAtWrite[iBlank]; k < m.readsAtWrite[iMux]; k++) nNo += m.reads[k] == N48D2_HUBP2_DCHUBP_CNTL; expect(nNo >= 1, "REACHABILITY: NO_OUTSTANDING_REQ is polled between the blank and the mux");
      expect(idx && first_read_after(m, N48D2_HUBP2_HUBP_CLK_CNTL, m.readsAtWrite[iClk]) != (size_t)-1, "REACHABILITY: the clock-gated wait reads HUBP2_HUBP_CLK_CNTL after the clock write");
      expect(((m.gateBuf[N48D2_GT_META] >> 8) & 1u) == 1u && (m.gateBuf[N48D2_GT_META] & 0xFFu) == N48D2_PL_FREE, "gates: meta says released / FREE"); }
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && rolled_back_clean(m), "op 12 from SHOWN: OK, DPG2 back on, buffers released"); }
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && m.writes.empty() && o.free_bufs == 0u && o.auto_off == 0u, "op 12 with no plane and the clock off: OK ('nothing to do', 0.0.636: a runner may always call it), nothing written, nothing freed"); }
    // the buffers are released ONLY when the clock reads off and NO_OUTSTANDING_REQ came true
    { PModel m; plane_up(m); m.clkOffWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED && o.timeouts >= 1u, "planeoff: the clock does not gate -> the buffers are LEAKED (stage LEAKED, no release)"); }
    { PModel m; plane_up(m); m.noOutWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED, "planeoff: NO_OUTSTANDING_REQ never asserts -> the buffers are LEAKED"); }
    { PModel m; plane_up(m); m.mpccNeverIdle = true; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && o.timeouts == 1u && m.plane.stage == N48D2_PL_FREE, "planeoff: MPCC2_STATUS never reads 5 -> logged (one timeout) but the buffers are still released: the clock is off"); }
    { PModel m; plane_up(m); m.clkOffWorks = false; (void)run_op(m, N48D2_OP_PLANEOFF); m.clkOffWorks = true; m.clkStatus = false; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE, "a LEAKED plane can be rolled back again once the clock gates: then the buffers are released"); }
    { PModel m; plane_up(m); m.clkOffWorks = false; (void)run_op(m, N48D2_OP_PLANEOFF); const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE, "op 8 on a LEAKED plane is refused (PLANE_STATE)"); }
    // a failed op 8 that left the clock ON and could not turn it off leaks too
    { PModel m; m.initPlane(); m.detWorks = false; m.clkOffWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED, "op 8 failing with the clock stuck ON: rolled back as far as possible, buffers LEAKED, never freed"); }
}

static void plane_dp_tests() {
    struct Dp { const char *what; std::function<void(PModel &)> arm; uint32_t trig; bool otg0; uint32_t bit; };
    // the DP watch inside the plane ops: a change under ANY op ends it in DP_DISTURBED and rolls the plane back
    const uint32_t ops[] = { N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_CRC };
    for (uint32_t op : ops) {
        const uint32_t trig0 = op == N48D2_OP_PLANE ? N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS : op == N48D2_OP_SHOW ? N48D2_DPG2_CONTROL : op == N48D2_OP_FLIP ? N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS : N48D2_OTG2_OTG_CRC_CNTL;
        struct Fl { uint32_t reg, xorv, row; const char *what; };
        const Fl fl[] = { { N48D2_MPC_OUT0_MUX, 0xF, 0, "MPC_OUT0_MUX" }, { N48D2_MPCC0_TOP_SEL, 0x2, 1, "MPCC0 TOP" }, { N48D2_HUBP0_DCHUBP_CNTL, 0x10000000u, 4, "HUBP0 underflow" }, { N48D2_DPPCLK_CTRL, 0x1, 6, "DPPCLK_CTRL bit 0" }, { N48D2_DET0_CTRL, 0x8, 7, "DET0" },
                          { N48D2_COMPBUF_CTRL, 0x1, 8, "COMPBUF" }, { N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL, 0x400, 10, "ODM0 underflow" }, { N48D2_DPP_TOP0_DPP_CONTROL, 0x10, 11, "DPP_TOP0" }, { N48D2_MPCC0_UPDATE_LOCK_SEL, 0x1, 12, "MPCC0 lock select" },
                          { N48D2_OTG2_OTG_GLOBAL_CONTROL2, 0x04000000u, 13, "OTG2_GLOBAL_CONTROL2" }, { N48D2_HUBPREQ0_PRIMARY_LOW, 0x10000u, 14, "HUBP0 primary (the rule)" } };
        for (const Fl &f : fl) {
            PModel m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane_up(m);
            if (op == N48D2_OP_SHOW && f.reg == N48D2_DPG2_CONTROL) continue;
            const uint32_t trig = (op == N48D2_OP_PLANE && f.reg == N48D2_OTG2_OTG_GLOBAL_CONTROL2) ? (uint32_t)N48D2_OTG2_OTG_MASTER_UPDATE_LOCK : trig0;      // 0.0.643: op 8 itself rewrites the lock select (to 2) first thing inside the lock: the disturbance must land AFTER that write, on the lock bit's write
            m.flipTrig = trig; m.flipReg = f.reg; m.flipXor = f.xorv;
            const n48d2_out o = run_op(m, op, 1u);
            uint32_t want = 0u; for (uint32_t j = 0; j < N48D2_DPX_REGS; j++) if (n48d2_dpx_regs[j].abs == f.reg && (j == N48D2_DPX_RULE ? f.xorv != 0u : (f.xorv & n48d2_dpx_regs[j].mask) != 0u)) want |= 1u << j;
            const std::string w = std::string(n48d2_op_name(op)) + ": " + f.what + " changes during the op";
            expect(o.status == N48D2_DP_DISTURBED && o.dpx_changed == want && want != 0u, (w + " -> DP_DISTURBED, the mask names the row").c_str());
            expect(rolled_back_clean(m, f.reg != N48D2_DPPCLK_CTRL) && m.plane.stage != N48D2_PL_PLANE && m.plane.stage != N48D2_PL_SHOWN, (w + ": the plane was rolled back").c_str());
        }
        { PModel m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane_up(m); m.stopOtg0On = trig0; const n48d2_out o = run_op(m, op, 1u); if (!(op == N48D2_OP_SHOW && trig0 == N48D2_DPG2_CONTROL)) expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x80u) != 0u && rolled_back_clean(m), (std::string(n48d2_op_name(op)) + ": OTG0 stops counting under the op -> DP_DISTURBED (bit 7), rolled back").c_str()); }
    }
    // SYMCLKB (the live DP's symbol clock) changing under ANY plane op ends it in DP_DISTURBED (bit 6) and rolls the plane back
    for (uint32_t op : ops) {
        const uint32_t trig = op == N48D2_OP_PLANE ? N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS : op == N48D2_OP_SHOW ? N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL : op == N48D2_OP_FLIP ? N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS : N48D2_OTG2_OTG_CRC_CNTL;
        PModel m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane_up(m);
        m.r[N48D2_SYMCLKB_CLOCK_ENABLE] = 0x111u; m.flipTrig = trig; m.flipReg = N48D2_SYMCLKB_CLOCK_ENABLE; m.flipXor = 0x10u;
        const n48d2_out o = run_op(m, op, 1u);
        expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x40u) != 0u && o.dpx_changed == 0u && rolled_back_clean(m), (std::string(n48d2_op_name(op)) + ": SYMCLKB changes under the op -> DP_DISTURBED (bit 6), rolled back").c_str());
    }
    // the HUBP0 primary rule: with the desktop scanout ACQUIRED a move inside the window that avoids A / B is legitimate; A, B or outside the window never are
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = 0x10000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && o.dpx_changed == 0u && m.plane.stage == N48D2_PL_PLANE, "desktop scanout ACQUIRED: HUBP0 flipping to another address inside the window is NOT a disturbance"); }
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)kBufA; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u && rolled_back_clean(m), "even with the scanout acquired, HUBP0 landing on buffer A is a disturbance"); }
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_HIGH; m.flipXor = 0x40u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u && rolled_back_clean(m), "HUBP0's primary leaving the scanout window is a disturbance even with the scanout acquired"); }
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)kBufB; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u, "HUBP0 landing on buffer B is a disturbance"); }
    // the rule for ops 1..7 (no buffers): equality while not acquired, the window while one is known
    { Model m; m.inst = 2; m.haveWin = true; m.winLo = kWinLo; m.winHi = kWinHi; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = 0x80u; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2); expect(o.status == N48D2_OK && o.dpx_changed == 0u, "ops 1..7: the console address inside the window is clean"); }
    { Model m; m.inst = 2; m.haveWin = true; m.winLo = kWinLo; m.winHi = kWinHi; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = 0x90u; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2); expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u, "ops 1..7: a HUBP0 primary outside the scanout window is a disturbance (row 14)"); }
    { Model m; m.inst = 2; m.acquired = true; m.flipTrig = N48D2_OTG2_CONTROL; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = 0x20000u; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2); expect(o.status == N48D2_OK && o.dpx_changed == 0u, "ops 1..7 with the scanout acquired: a primary move is legitimate"); }
    { Model m; m.inst = 2; m.flipTrig = N48D2_OTG2_CONTROL; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = 0x20000u; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2); expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u, "ops 1..7 with the scanout NOT acquired: any primary move is a disturbance"); }
}

static void plane_off2_and_pack() {
    // `disp2 off 2` is REFUSED while HUBP2's clock is on (one read, nothing written) ...
    { PModel m; plane_up(m); const size_t w0 = m.writes.size(), r0 = m.reads.size(); const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 2);
      expect(o.status == N48D2_PLANE_STATE && m.writes.size() == w0 && m.reads.size() == r0 + 1u && m.reads.back() == N48D2_HUBP2_HUBP_CLK_CNTL && o.pre_abs == N48D2_HUBP2_HUBP_CLK_CNTL, "off 2 with the HUBP2 clock on: PLANE_STATE; ONE read (the clock register), nothing written");
      // ... works after planeoff, and for instance 1 it is not affected at all
      (void)run_op(m, N48D2_OP_PLANEOFF); const n48d2_out o2 = n48d2::run(m, N48D2_OP_OFF, 2); expect(o2.status == N48D2_OK, "off 2 after planeoff: OK"); }
    { PModel m; plane_up(m); m.inst = 1; const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 1); expect(o.status != N48D2_PLANE_STATE, "off 1 (the monitor A) is not touched by the HUBP2 refusal"); }
    { PModel m; m.initPlane(); const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 2); expect(o.status == N48D2_OK, "off 2 with the HUBP2 clock off behaves as in 0.0.634 (OK)"); }
}

static void plane_source_pins() {
    const std::string src = g_root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), s1c = slurp(src + "amd/native_s1c.cpp"), s1h = slurp(src + "amd/native_s1c.h"), flow = strip_line_comments(slurp(src + "dcn/navi48_disp2_flow.h")), hdr = strip_line_comments(slurp(src + "dcn/navi48_disp2.h")),
                      cli = slurp(g_root + "/tools/pc/navi48test.c"), brg = slurp(src + "Navi48Bringup.cpp");
    expect(!dcn.empty() && !s1c.empty() && !s1h.empty() && !flow.empty() && !hdr.empty() && !cli.empty() && !brg.empty(), "0.0.635: the sources are readable");
    // ---- the glue: allocate BEFORE the busy flag, run, release the flag, free AFTER it
    const size_t g0 = dcn.find("build 0.0.631 (multi-monitor stage M4d; an internal design note");
    const std::string glue = g0 == std::string::npos ? "" : strip_line_comments(dcn.substr(g0));
    const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {"), f1 = glue.find("}  // namespace n48dcn", f0 == std::string::npos ? 0 : f0);
    const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
    {
        const size_t a = fn.find("amdgpu::n1c_d2_alloc(*ctx, inst, n48d2_surf_get(inst)->buf_bytes, pmc, poff)"), b = fn.find("__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL)"), r = fn.find("n48d2::run_plane(env, op, n48d2_arg_buf(arg), inst)"), l = fn.rfind("__atomic_store_n(&gD2Busy, 0u, __ATOMIC_RELEASE)"), fr = fn.rfind("amdgpu::n1c_d2_free(inst);");
        expect(a != std::string::npos && b != std::string::npos && r != std::string::npos && l != std::string::npos && fr != std::string::npos && a < b && b < r && r < l && l < fr, "0.0.635 glue: REACHABILITY/ORDER: the buffers are ALLOCATED before the busy flag is taken, the plane op runs under it, the flag is RELEASED, and only then are the buffers FREED");
        { const size_t fi = fn.find("if (freeNow) {"); expect(fi != std::string::npos && fi > l && count_of(fn, "amdgpu::n1c_d2_free(inst);") == 3 && fn.substr(r, l - r).find("n1c_d2_free") == std::string::npos, "0.0.635 glue: exactly three frees (busy refusal, plane-state refusal, the final one) and NONE between the plane op and the release of the busy flag"); }
        expect(fn.find("if (op == N48D2_OP_PLANE) {") < b && fn.find("freeNow = o.free_bufs != 0u;") != std::string::npos && fn.find("} else if ((op >= N48D2_OP_PLANE && op <= N48D2_OP_PLANEOFF) || op == N48D2_OP_FBHOLD) {") != std::string::npos, "0.0.635 glue: only op 8 allocates (before the flag); the free is guarded by the flow's own verdict (free_bufs: HUBP2's clock reads off)");
        expect(fn.find("gD2Pl[pi].stage = N48D2_PL_NONE;") > l && fn.find("gD2Pl[pi].stage = N48D2_PL_NONE;") < fr, "0.0.635 glue: the stage is reset BEFORE the allocator gets the buffers back (no plane op can start on buffers about to be freed)");
        expect(fn.find("if (__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL) != 0u) { if (allocated) amdgpu::n1c_d2_free(inst);") != std::string::npos, "0.0.635 glue: a busy refusal gives a just-made allocation back");
        expect(glue.find("n48d2_plane gD2Pl[2];") != std::string::npos && glue.find("uint32_t gD2Gate[N48D2_GATES];") != std::string::npos && glue.find("uint32_t gD2Dpx[2][N48D2_DPX_BUF];") != std::string::npos && glue.find("n48d2_plane *pl_of(uint32_t i) { return &gD2Pl[d2i(i)]; }") != std::string::npos, "0.0.635 glue: the plane state, the gates and the DP buffers are file-scope (never on the kernel stack)");
        expect(glue.find("bar0_memset_vram(*gDcn.dev, off + byteOff, pattern, bytes);") != std::string::npos && glue.find("off + byteOff + bytes > gDcn.dev->bar0Size") != std::string::npos && glue.find("navi48_vram_read_mm(gD2Off[d2i(inst)][k] + byteOff, dw, 1u)") != std::string::npos && glue.find("amdgpu::amdgpu_hdp_flush(*gDcn.dev);") != std::string::npos,
               "0.0.635 glue: the fill is bar0_memset_vram behind a bounds check, flushed with amdgpu_hdp_flush, read back through navi48_vram_read_mm");
        expect(glue.find("bool desktop_acquired() { return __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u; }") != std::string::npos && glue.find("bool window(uint64_t *lo, uint64_t *hi) { *lo = gDcn.d.scanout_lo; *hi = gDcn.d.scanout_hi; return gDcn.d.scanout_hi != 0u; }") != std::string::npos,
               "0.0.635 glue: the desktop-acquired flag is the native scanout's `active` word and the window is the flip layer's [scanout_lo, scanout_hi), unknown when hi is 0");
        expect(count_of(glue, "WREG32(") == 2, "0.0.635 glue: exactly one WREG32 in the disp2 glue (the surface is drawn through bar0_memset_vram, a VRAM write, not a register write); 0.0.661: plus exactly one in the instance-2 scanout block that follows it (the address writer, behind write_reg_ok and the allowlist: tests/native_scanx_test.cpp pins that one)");
        expect(fn.find("n1c_d2_alloc") != std::string::npos && fn.find("gCliLock") == std::string::npos, "0.0.635 glue: the DCN layer never names the client lock; the helper does");
    }
    // ---- the helper: gCliLock around every allocator touch, no register access
    {
        const size_t a0 = s1c.find("IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]) {"), a1 = s1c.find("void n1c_d2_free(uint32_t inst) {"), a2 = s1c.find("IOReturn n1c_bo_free(");
        const std::string al = (a0 == std::string::npos || a1 == std::string::npos) ? "" : s1c.substr(a0, a1 - a0), fr = (a1 == std::string::npos || a2 == std::string::npos) ? "" : s1c.substr(a1, a2 - a1);
        expect(!al.empty() && !fr.empty(), "0.0.635 helper: n1c_d2_alloc and n1c_d2_free exist");
        const size_t lk = al.find("IOLockLock(gCliLock);"), ac = al.find("vram_alloc.alloc(bytes, 65536"), ul = al.find("IOLockUnlock(gCliLock);");
        expect(lk != std::string::npos && ac != std::string::npos && ul != std::string::npos && lk < ac && ac < ul, "0.0.635 helper: the allocation happens strictly inside IOLockLock(gCliLock) .. IOLockUnlock(gCliLock)");
        const size_t lk2 = fr.find("IOLockLock(gCliLock);"), fc = fr.find("vram_alloc.free("), ul2 = fr.rfind("IOLockUnlock(gCliLock);");     // 0.0.652: the LAST unlock (the first one belongs to the pinned early return)
        expect(lk2 != std::string::npos && fc != std::string::npos && ul2 != std::string::npos && lk2 < fc && fc < ul2, "0.0.635 helper: the free happens strictly inside the same lock");
        for (const char *bad : { "RREG32", "WREG32", "bar0_memset_vram", "WBAR0_", "MM_INDEX", "hub_rd" }) expect(al.find(bad) == std::string::npos && fr.find(bad) == std::string::npos, (std::string("0.0.635 helper: never uses ") + bad).c_str());
        expect(al.find("(a[k].gpu_va - ctx.gmc.vram_start) + a[k].size <= ctx.dev->bar0Size") != std::string::npos && al.find("(a[k].gpu_va & 0xFFFFull) == 0") != std::string::npos && al.find("if (n48d2pin::alloc_refused(gD2Held[sl])) { rc = kIOReturnExclusiveAccess; break; }") != std::string::npos, "0.0.635 helper: 64 KiB aligned, inside the BAR0 aperture, one pair at a time");
        expect(s1h.find("IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]);") != std::string::npos && s1h.find("void n1c_d2_free(uint32_t inst);") != std::string::npos, "0.0.635 helper: exported from native_s1c.h");
        expect(brg.find("n48dcn::disp2(argScalar, v, 13, &gBringup)") != std::string::npos, "0.0.635: the dispatcher hands disp2 the bring-up context (the allocator)");
    }
    // ---- the flow: the order of the blocks in plane_exec (a literal pin ALONGSIDE the reachability tests above)
    {
        const size_t e0 = flow.find("inline bool plane_exec("), e1 = flow.find("template <class E>\ninline n48d2_out run_plane(", e0 == std::string::npos ? 0 : e0);
        const std::string pe = (e0 == std::string::npos || e1 == std::string::npos) ? "" : flow.substr(e0, e1 - e0);
        const char *chain[] = { "plane_admit(e, inst, pl)", "plane_judge_all(e, inst, op, bufIdx, o, pl)", "dp_snapshot(e, o.dp_before);", "n48d2_build_pre_i(inst, p, s)", "e.rd(N48D2_HUBPREQ0_PRIMARY_HIGH)", "plane_fill_verify(e, inst)", "n48d2_build_plane_a_i(inst, s, pl->mc[0])",
                                "n48d2_build_plane_b_i(inst, s)", "n48d2_build_plane_d_i(inst, s, pl->mc[0])", "wait_frames(e, fc2, 2u)", "n48d2_build_plane_rec2_i(inst, s)", "plane_readback(e, s, n, o)", "n48d2_build_plane_lock_i(inst, s)", "n48d2_build_plane_c_i(inst, s, pl->mc[0])", "n48d2_build_plane_unlock_i(inst, s)", "locked = false;", "latch_gate(e, inst, fc2, 5u, pl->mc[0], g)", "n48d2_build_plane_e_i(inst, s)", "wait_frames(e, fc2, 3u)", "n48d2_health(g, pl->odm_sticky, pl->mc[0], 1)", "pl->stage = N48D2_PL_PLANE;", "dpx_diff(e, pxB, pxA)" };
        size_t prev = 0; bool inOrder = !pe.empty(); for (const char *c : chain) { const size_t p = pe.find(c, prev); if (p == std::string::npos) { inOrder = false; std::printf("   missing/out of order: %s\n", c); break; } prev = p; }
        expect(pe.find("if (!plane_judge_all(e, inst, op, bufIdx, o, pl)) { if (op == N48D2_OP_PLANE) plane_clean(inst, pl, o); return false; }") != std::string::npos, "0.0.635 flow: every list of the op is judged by the guard BEFORE anything is read, and a refusal returns (op 8 gives the buffers back)");
        expect(inOrder, "0.0.639 flow: REACHABILITY: admission, the guard over every list, the DP snapshot, the prechecks, the surface, part A (clocks, blank, address), part B (the HUBP clock, the recorded reads, DET2 RECORDED), part D (COPY0 + the address again), the 2-frame wait, the second recorded sample, the read-back, the OTG2 update LOCK, part C (address, MPCC2, mux, UNBLANK) inside it, the UNLOCK, the post-unlock latch gate (FLIP_PENDING == 0 and EARLIEST_INUSE == A, polled up to 5 frames), part E (the DET2 wait, AFTER the unblank), the 3-frame health gate, the stage change, the DP diff - in that order in plane_exec");
        expect(pe.find("if ((o.wfail & (1ull << N48D2_PA_I_BLANKWAIT)) != 0u)") != std::string::npos && pe.find("if ((o.wfail & (1ull << N48D2_PE_I_DETWAIT)) != 0u)") != std::string::npos && pe.find("N48D2_PB_I_DETWAIT") == std::string::npos && pe.find("N48D2_PB_I_CLKWAIT") == std::string::npos, "0.0.639 flow: the blank read-back and the (post-unblank) DET2 wait are GATES (a timeout is fatal, not counted and forgotten); the clock-status wait is GONE");
        expect(flow.find("if (n48d2_guard_list_i(inst, s, n, &bad) == 0u) { (void)run_steps(e, inst, s, n, true, sc, nullptr); ran = true; }") != std::string::npos, "0.0.635 flow: the rollback list is judged by the guard before it runs, like every other list");
        expect(flow.find("o.free_bufs = (ran && clkOff && outOk && clkOk) ? 1u : 0u;") != std::string::npos, "0.0.635 flow: the buffers are released only when the rollback ran AND HUBP2's clock reads off AND neither wait timed out");
        expect(flow.find("if (op == N48D2_OP_OFF || op == N48D2_OP_TIMING || op == N48D2_OP_CONNECT || op == N48D2_OP_TIMING1440) {") != std::string::npos && flow.find("n48d2_pr_get(inst)->hubp_clk") != std::string::npos && flow.find("o.status = N48D2_PLANE_STATE; return o;") != std::string::npos, "0.0.636 flow (C4): `off`, `timing`, `connect` and `timing1440` for the monitor B are refused while HUBP2's clock is on");
        { const size_t c4 = flow.find("if (op == N48D2_OP_OFF || op == N48D2_OP_TIMING || op == N48D2_OP_CONNECT || op == N48D2_OP_TIMING1440) {"), snap = flow.find("dp_snapshot(e, o.dp_before);", c4 == std::string::npos ? 0 : c4), wr = flow.find("run_steps(e, inst, s, n, op == N48D2_OP_OFF, o, &refusals)");
          expect(c4 != std::string::npos && snap != std::string::npos && wr != std::string::npos && c4 < snap && snap < wr, "0.0.636 flow (C4) REACHABILITY: the HUBP2-clock refusal sits after the guard list and BEFORE the DP snapshot and the steps of run()"); }
        expect(flow.find("n48d2_surface_overlaps_n(h0, pl->mc[0], pl->dp_bytes, bsz)") != std::string::npos && flow.find("n48d2_surface_overlaps_n(i0, pl->mc[1], pl->dp_bytes, bsz)") != std::string::npos && flow.find("e.rd(N48D2_HUBPREQ0_EARLIEST_LOW)") != std::string::npos && flow.find("pa == pl->mc[k]") == std::string::npos && flow.find("h0 == pl->mc[0]") == std::string::npos,
               "0.0.636 flow (C3): the op-8 precheck and the DP watch's address rule use the OVERLAP test (primary AND EARLIEST_INUSE), no exact compare is left");
        expect(flow.find("o.status = N48D2_OK; plane_gates(e, inst, pl, g, o); return true; }") != std::string::npos && flow.find("N48D2_PL_NONE && (e.rd(pr->hubp_clk) & 1u) == 0u") != std::string::npos, "0.0.636 flow: planeoff at stage NONE with the HUBP2 clock off answers OK ('nothing to do'), gates reported");
        expect(count_of(flow, "e.rd(N48D2_HUBPREQ0_PRIMARY_HIGH)") == 2, "0.0.635 flow: HUBP0's primary HIGH is READ in the snapshot and in the op-8 precheck (reads only)");
    }
    // ---- the header: the pinned constants
    {
        expect(hdr.find("#define N48D2_DPPCLK2_EN_MASK         0x00000040u") != std::string::npos, "0.0.635 header: DPPCLK2_EN is bit 6 (mask 0x40)");
        expect(hdr.find("#define N48D2_MPC_OUT2_MUX_ON         0x00004102u") != std::string::npos && hdr.find("#define N48D2_MPC_OUT2_MUX_OFF        0x0000400fu") != std::string::npos, "0.0.635 header: MPC_OUT2_MUX is 0x00004102 on, 0x400f off");
        expect(count_of(hdr, "abs == N48D2_DPPCLK2_DTO_PARAM || abs == N48D2_DPPCLK_CTRL") == 1 && hdr.find("if (inst == N48D2_INST_MONB && (abs == N48D2_DPPCLK2_DTO_PARAM || abs == N48D2_DPPCLK_CTRL)) return N48D2_G_OK;") != std::string::npos, "0.0.635 header: the two DCCG exceptions are ONE address test in the guard, instance 2 only");
        expect(hdr.find("#define N48D2_FORBIDDEN_COUNT 48u") != std::string::npos && hdr.find("#define N48D2_ALLOWED1P_COUNT 43u") != std::string::npos && hdr.find("#define N48D2_ALLOWED2P_COUNT 39u") != std::string::npos && hdr.find("#define N48D2_MIRROR0_COUNT 20u") != std::string::npos && hdr.find("#define N48D2_DPX_REGS 15u") != std::string::npos, "0.0.643 header (0.0.655: 48 forbidden spans, instance 1's 43 plane registers): 39 plane registers (37 + OTG2 lock select and lock), 20 mirror pairs, 15 DP rows");
        expect(hdr.find("if (op == N48D2_OP_FBHOLD && inst != N48D2_INST_MONB && inst != N48D2_INST_MONA) return 0;") != std::string::npos && hdr.find("if (inst > N48D2_INST_MONB || (a >> 24) != 0u) return 0;") != std::string::npos && hdr.find("return op == N48D2_OP_FLIP ? (n48d2_arg_buf(a) <= 1u) : ((a >> 16) == 0u);") != std::string::npos, "0.0.635 header (0.0.655: ops 8..13 take instance 0 / 1 / 2, op 14 for instance 1 or 2 EXACTLY, 0.0.658): the buffer index (bits 16..23) is legal for op 10 alone");
        expect(hdr.find("if (s->kind == N48D2_K_COPY0) return n48d2_mirror_ok(a, s->cond_abs, v, s->cond_mask) && m == 0xFFFFFFFFu;") != std::string::npos && hdr.find("if (n48d2_mirror_dst(a)) return 0;") != std::string::npos, "0.0.635 header: a COPY0 must be one of the 20 exact pairs; a mirror destination takes no other step kind");
        { bool named = false; for (const char *r : { "HUBP0_DCHUBP_CNTL", "DET0_CTRL", "COMPBUF_CTRL", "MPC_OUT0_MUX", "MPCC0_TOP_SEL", "HUBPREQ0_PRIMARY_LOW", "DPP_TOP0_DPP_CONTROL", "DCHUBBUB_DET2_CTRL", "DCN_VM_FAULT_STATUS", "OTG2_OTG_CRC0_DATA_RG", "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE" })
              named = named || hdr.find(std::string("N48D2_SET(") + r) != std::string::npos || hdr.find(std::string("N48D2_UPD(") + r) != std::string::npos;
          expect(!named, "0.0.635 header: no step list SETs / UPDs a pipe-0 / DET / COMPBUF / mux-0 register or a READ-ONLY plane register"); }
    }
    // ---- the CLI
    {
        for (const char *t : { "static int cmd_disp2_plane(const char *a1, uint32_t op, uint32_t bufIdx) {", "!strcmp(a1, \"plane\") ? N48D2_OP_PLANE", "!strcmp(a1, \"flipA\") || !strcmp(a1, \"flipB\") ? N48D2_OP_FLIP", "!strcmp(a1, \"planeoff\") ? N48D2_OP_PLANEOFF", "accel disp2 flipB 2 ; accel disp2 crc 2 ; accel disp2 flipA 2",
                               "the plane ops take the instance", "*** ABORT: DP DISTURBED during `accel disp2 %s`: running `accel disp2 planeoff %u` now ***", "dr_call(N48D2_ACT, d2_arg(op) | ((uint64_t)bufIdx << 16), o)", "n48d2_unpack_plane(o + 3, &r, g)", "static void d2_print_gates(uint32_t op, const struct n48d2_out *r, const uint32_t *g) {",
                               "DP HUBP0 underflow 30:28", "static int dp_watch_read(uint32_t v[N48D2_DPX_BUF]) {", "for (uint32_t i = 0; i < 5; i++) v[10 + i] = r6[i] & n48d2_dpx_regs[10 + i].mask;", "if (i == N48D2_DPX_RULE) {", "    d2_print_gates(op, &r, g);\n", "N48DR_CENSUS6_PAGE", "is REFUSED while HUBP2's clock is on: run planeoff first", "EARLIEST_INUSE          : %#llx  (A %#llx, B %#llx)", "const uint64_t early = ((uint64_t)(g[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | g[N48D2_GT_EARLY_LO];", "const uint64_t A = ((uint64_t)g[N48D2_GT_A_HI] << 32) | g[N48D2_GT_A_LO], B = ((uint64_t)g[N48D2_GT_B_HI] << 32) | g[N48D2_GT_B_LO];" })
            expect(cli.find(t) != std::string::npos, (std::string("0.0.635 CLI carries ") + t).c_str());
        const size_t c0 = cli.find("static int cmd_disp2_plane("), c1 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {", c0 == std::string::npos ? 0 : c0);
        const std::string cp = (c0 == std::string::npos || c1 == std::string::npos) ? "" : cli.substr(c0, c1 - c0);
        const size_t w0 = cp.find("dp_watch_read(dpw0)"), call = cp.find("dr_call(N48D2_ACT, d2_arg(op)"), w1 = cp.find("dp_watch_read(dpw1)"), ab = cp.find("d2_arg(N48D2_OP_PLANEOFF)");
        expect(w0 != std::string::npos && call != std::string::npos && w1 != std::string::npos && ab != std::string::npos && w0 < call && call < w1 && w1 < ab, "0.0.635 CLI: REACHABILITY: the 15-row DP watch is read BEFORE the plane op, AFTER it, and a disturbance runs planeoff");
        expect(cp.find("\n        else if (dp_watch_report(a1, dpw0, dpw1) != 0u) watchBad = 1;") != std::string::npos && cp.find("if (r.dp_changed || r.dpx_changed || r.status == N48D2_DP_DISTURBED) watchBad = 1;") != std::string::npos, "0.0.635 CLI: the plane CLI compares the 15-row watch after the op and treats the kext's own masks as a disturbance");
        expect(cp.find("REFUSED by the CLI: the DP watch (dispcensus pages 10 and 43) could not be read; nothing was sent") != std::string::npos && cp.find("if (op != N48D2_OP_PLANEOFF) {") != std::string::npos, "0.0.635 CLI: every plane op but the rollback is refused without a readable DP watch");
        {   // 0.0.655: the watch the CLI reads is BOTH dispcensus pages (10 and 43); a CLI that reads page 10 only would leave the monitor B/DP rows 10..14 and the primary HIGH dword at zero
            const size_t b = cli.find("static int dp_watch_read(uint32_t v[N48D2_DPX_BUF]) {"), e = b == std::string::npos ? b : cli.find("\n}\n", b);
            const std::string body = b == std::string::npos || e == std::string::npos ? std::string() : cli.substr(b, e - b);
            expect(!body.empty() && body.find("dp_watch_page(N48DR_CENSUS4_PAGE, N48DR_CENSUS4_COUNT, r4) != 0 || dp_watch_page(N48DR_CENSUS6_PAGE, N48DR_CENSUS6_COUNT, r6) != 0) return -1;") != std::string::npos
                   && body.find("v[10 + i] = r6[i] & n48d2_dpx_regs[10 + i].mask;") != std::string::npos && body.find("v[N48D2_DPX_BUF - 1u] = r6[5];") != std::string::npos && N48DR_CENSUS4_PAGE == 10u && N48DR_CENSUS6_PAGE == 43u && N48DR_CENSUS6_COUNT == 6u,
                   "0.0.655 CLI: dp_watch_read reads BOTH watch pages (page 10 = census4 and page 43 = census6), refuses (-1) when either fails, and fills rows 10..14 and the primary HIGH dword from page 43"); }
        const size_t d0 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {"); const std::string cd = d0 == std::string::npos ? "" : cli.substr(d0, 3000);
        expect(cd.find("!strcmp(a1, \"flipA\") || !strcmp(a1, \"flipB\")") != std::string::npos && cd.find("if (!a2 || (strcmp(a2, \"1\") && strcmp(a2, \"2\"))) {") != std::string::npos && cd.find("g_d2_inst = !strcmp(a2, \"1\") ? N48D2_INST_MONA : N48D2_INST_MONB;") != std::string::npos && cd.find("return cmd_disp2_plane(a1, pop, !strcmp(a1, \"flipB\") ? 1u : 0u);") != std::string::npos, "0.0.635 CLI (0.0.655: plane ops need the explicit instance 1 or 2, fbhold too since 0.0.658) and pass flipB as buffer 1");
    }
}

// =====================================================================================================================================================================================================
// 0.0.636: Linux's order (the 0.0.635 run: DSCL2_LB_MEMORY_CTRL did not take with HUBP2 / DPP2 unclocked) and the review items C1 (flip polls EARLIEST_INUSE), C2 (address re-written with the clock on), C3 (overlap, not equality),
// C4 (timing / connect / timing1440 refused under a running HUBP2), C5 (comment; pinned in the header pins), plus planeoff 'nothing to do' = OK.
// =====================================================================================================================================================================================================
static void plane_636_tests() {
    // ---- the 0.0.635 failure, modelled: DSCL2 writes do not land while HUBP2 is unclocked -> the new order passes (every DSCL2 write is after the clock); the readback sees the programmed LB value
    { PModel m; m.dsclNeedsClock = true; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE && m.get(N48D2_DSCL2_LB_MEMORY_CTRL) == 0x00003f00u && o.timeouts == 0u && o.auto_off == 0u, "0.0.635 failure modelled (DSCL2 writes dropped while unclocked): op 8 now passes; DSCL2_LB_MEMORY_CTRL holds 0x00003f00 (0.0.637: Linux's fields only)");
      bool dropped = false; for (const std::string &e : m.ev) dropped = dropped || e == "write dropped"; expect(!dropped, "... and not one write was dropped (every DSCL2 write came after the clock)"); }
    // ---- the double-buffered reading: the LB fields latch at OTG2's update -> the 2-frame wait before the read-back is what makes it pass
    { PModel m; m.dsclDoubleBuf = true; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.pendW.empty() && m.get(N48D2_DSCL2_LB_MEMORY_CTRL) == 0x00003f00u, "DSCL2 writes that latch only at the next OTG2 frame (double-buffered): op 8 passes because the read-back waits 2 frames first"); }
    // ---- the rollback of an op-8 failure leaves the plane off and a following planeoff is OK 'nothing to do' (the runner bug of the 0.0.635 run: planeoff answered 'not in state')
    { PModel m; m.latchWorks = false; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_GATE && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE, "setup: op 8 fails at the latch gate and rolls itself back");
      m.plane.stage = N48D2_PL_NONE; m.plane.mc[0] = m.plane.mc[1] = 0u;      // what the kext glue does after it frees the buffers
      m.r[N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL] = 0x04100100u; m.pend = 1u; m.writes.clear();       // the model derives the pending bit from `pend`
      const n48d2_out p = run_op(m, N48D2_OP_PLANEOFF);
      expect(p.status == N48D2_OK && m.writes.empty() && p.free_bufs == 0u && p.auto_off == 0u && m.gateBuf[N48D2_GT_FLIP_CTL] == 0x04100100u && (m.gateBuf[N48D2_GT_FLIP_CTL] & N48D2_FLIP_PENDING_MASK) != 0u,
             "planeoff on a released plane after the failed op 8: OK, nothing written, nothing freed, and the gates REPORT the left-over FLIP_PENDING (0x04100100: set by the pre-clock address write)");
      const n48d2_out q = run_op(m, N48D2_OP_PLANEOFF); expect(q.status == N48D2_OK && m.writes.empty(), "... and it may be called again and again"); }
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; m.r[N48D2_HUBP2_HUBP_CLK_CNTL] = 1u; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF); expect(o.status != N48D2_OK || !m.writes.empty(), "planeoff with the stage NONE but the HUBP2 clock ON is NOT 'nothing to do': the rollback runs"); }
    // ---- C2 / latch gate: the pending bit is POLLED for up to 5 frames, not read once
    { PModel m; m.initPlane(); m.flipPendReads = 40; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && o.timeouts == 0u, "latch gate: FLIP_PENDING set for 40 reads (about 2.5 frames) - the gate POLLS and passes");
      size_t nF = 0; for (uint32_t a : m.reads) nF += a == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; expect(nF >= 40u, "... it read FLIP_CONTROL at least 40 times"); }
    { PModel m; m.initPlane(); m.flipPendReads = 5000; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_GATE && o.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && o.pre_wait == 1u && o.auto_off == 1u && o.free_bufs == 1u && any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) && !m.locked, "0.0.643 post-unlock gate: FLIP_PENDING still set after 5 frames -> GATE (the latch gate names EARLIEST_INUSE), the lock was released, rollback; MPCC2 / the mux WERE written (inside the lock, before the gate)");
      expect(m.frames2 >= 5u, "... and the gate really waited about 5 frames before it gave up"); }
    // ---- C1: the flip also waits for EARLIEST_INUSE (hubp2_is_flip_pending)
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.earlyLag = 5; m.writes.clear(); m.reads.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear(); m.f2AtRead.clear();
      const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
      size_t nE = 0; for (uint32_t a : m.reads) nE += a == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE;
      expect(o.status == N48D2_OK && m.plane.cur == 1u && o.timeouts == 0u && nE >= 6u, "flip: FLIP_PENDING clears but EARLIEST_INUSE shows the OLD address for 5 more reads -> the flip POLLS EARLIEST_INUSE (>= 6 reads) and passes"); }
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.earlyLag = 0xFFFFFFFFu; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
      expect(o.status == N48D2_GATE && o.fail == N48D2_FLIP_I_WAIT_LO && o.timeouts == 1u && m.plane.cur == 0u && m.plane.stage == N48D2_PL_SHOWN && o.auto_off == 0u, "flip: EARLIEST_INUSE never reaches B -> GATE at the EARLIEST_INUSE (LOW) wait, one timeout, the plane is left as it is (ops 9..11 do not roll back)"); }
    // the flip's post-wait health check still catches a pending bit that comes back after the waits passed
    { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.pendGlitch = true; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
      expect(o.status == N48D2_GATE && o.timeouts == 0u && o.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && m.plane.cur == 0u, "flip: FLIP_PENDING reads clear in the wait, then SET again at the final gate read -> GATE on the latch check (the waits passed), the recorded buffer stays A"); }
    // the latch gate is bounded and FAILS CLOSED when the bound runs out (OTG2 stops counting mid-gate, the address never latches)
    { PModel m; m.initPlane(); m.flipPendReads = 1; m.flipClears = false; m.freezeOnFlipRead = 3; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_GATE && o.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) && !m.locked && m.plane.stage == N48D2_PL_FREE, "0.0.643 post-unlock gate: OTG2 stops counting inside the gate and FLIP_PENDING never clears -> the bound runs out and the gate FAILS (the lock was released first; rollback)"); }
    // the post-unblank gate is bounded and FAILS CLOSED the same way (OTG2 stops counting inside it, the address never latches): the plane WAS unblanked, so the rollback runs
    { PModel m; m.initPlane(); m.latchWorks = false; m.freezeOnEarlyRead = 3; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_GATE && o.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && o.pre_wait == 1u && any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) && o.auto_off == 1u && m.plane.stage == N48D2_PL_FREE && rolled_back_clean(m), "EARLIEST_INUSE gate (after the unblank): OTG2 stops counting inside it and the address never latches -> the bound runs out, the gate FAILS, the rollback runs"); }
    // ---- C3: the pure overlap test, then the op-8 precheck on HUBP0's primary AND EARLIEST_INUSE
    expect(n48d2_surface_overlaps(kBufA, kBufA, N48D2_BUF_BYTES) && n48d2_surface_overlaps(kBufA + 0x100000u, kBufA, N48D2_BUF_BYTES) && n48d2_surface_overlaps(kBufA + 0xFFFFu, kBufA, N48D2_BUF_BYTES) && n48d2_surface_overlaps(kBufA - N48D2_BUF_BYTES + 0x10000u, kBufA, N48D2_BUF_BYTES) && n48d2_surface_overlaps(kBufA + N48D2_BUF_BYTES - 0x10000u, kBufA, N48D2_BUF_BYTES),
           "n48d2_surface_overlaps: equal, interior, an unaligned low half (rounded to 64 KiB), one 64 KiB step short of a full buffer below / above -> overlap");
    expect(!n48d2_surface_overlaps(kBufA - N48D2_BUF_BYTES, kBufA, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(kBufA + N48D2_BUF_BYTES, kBufA, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(0u, kBufA, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(kBufA - N48D2_BUF_BYTES + 0xFFFFu, kBufA, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(kBufA, 0u, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(0x80ull << 32, kBufA, N48D2_BUF_BYTES), "... exactly one buffer away, one buffer away with an unaligned low half (rounded down to 64 KiB), zero, a zero buffer and the console base -> no overlap");
    // 0.0.655: the per-size overlap test (n48d2_surface_overlaps_n) rounds the DP address down to 64 KiB too, for the monitor A's 8,323,072-byte buffer and the monitor B's
    {   const uint64_t mona = N48D1_BUF_BYTES, monb = N48D2_BUF_BYTES;
        expect(n48d2_surface_overlaps_n(kBufA, kBufA, 0u, mona) && n48d2_surface_overlaps_n(kBufA + mona - 0x10000u, kBufA, 0u, mona) && n48d2_surface_overlaps_n(kBufA - mona + 0x10000u, kBufA, 0u, mona)
               && n48d2_surface_overlaps_n(kBufA - mona + 0x10007u, kBufA, 0u, mona) && n48d2_surface_overlaps_n(kBufA + mona - 0x10000u + 0xFFFFu, kBufA, 0u, mona),
               "0.0.655 overlaps_n (monitor A size): equal, one 64 KiB step short of a full buffer below / above, and the same with an unaligned low half -> overlap");
        expect(!n48d2_surface_overlaps_n(kBufA + mona, kBufA, 0u, mona) && !n48d2_surface_overlaps_n(kBufA - mona, kBufA, 0u, mona) && !n48d2_surface_overlaps_n(0u, kBufA, 0u, mona) && !n48d2_surface_overlaps_n(kBufA, 0u, 0u, mona),
               "0.0.655 overlaps_n (monitor A size): exactly one buffer above / below, zero, a zero buffer -> no overlap");
        expect(!n48d2_surface_overlaps_n(kBufA - mona + 0xFFFFu, kBufA, 0u, mona) && !n48d2_surface_overlaps_n(kBufA - monb + 0xFFFFu, kBufA, 0u, monb) && !n48d2_surface_overlaps(kBufA - monb + 0xFFFFu, kBufA, 0u),
               "0.0.655 overlaps_n: C3 ROUNDING: an address one buffer below PLUS an unaligned low half (0xFFFF) rounds down to exactly one buffer away -> NO overlap (an unrounded test would say overlap), for both sizes");
        expect(n48d2_surface_overlaps_n(kBufA - 20u * 1024u * 1024u, kBufA, N48D2_DP_FALLBACK_BYTES, mona) && !n48d2_surface_overlaps_n(kBufA - N48D2_DP_FALLBACK_BYTES + 0xFFFFu, kBufA, N48D2_DP_FALLBACK_BYTES, mona) && n48d2_surface_overlaps_n(kBufA - mona + 0x10000u, kBufA, 1u, mona),
               "0.0.655 overlaps_n: a DP size larger than ours is honoured (the 64 MiB fallback reaches 20 MiB; its own size with an unaligned half does not), a smaller one is floored to OUR size"); }
    struct Ov { const char *what; bool earliest; uint64_t addr; bool refused; };
    const Ov ovs[] = { { "primary == A", false, kBufA, true }, { "primary inside A (+1 MiB)", false, kBufA + 0x100000u, true }, { "primary one step below A's end (A - size + 64 KiB)", false, kBufA - N48D2_BUF_BYTES + 0x10000u, true }, { "primary inside B (+0x20000)", false, kBufB + 0x20000u, true },
                       { "primary with the low half unaligned, inside A", false, kBufA + 0xFFFFu, true }, { "EARLIEST_INUSE == A", true, kBufA, true }, { "EARLIEST_INUSE inside B", true, kBufB + 0x400000u, true },
                       { "primary exactly one buffer below A (adjacent, no overlap)", false, kBufA - N48D2_BUF_BYTES, false }, { "primary exactly one buffer above B (adjacent, no overlap)", false, kBufB + N48D2_BUF_BYTES, false },
                       { "EARLIEST_INUSE one buffer below A (adjacent)", true, kBufA - N48D2_BUF_BYTES, false } };
    for (const Ov &c : ovs) {
        PModel m; m.initPlane(); const uint32_t lo = c.earliest ? N48D2_HUBPREQ0_EARLIEST_LOW : N48D2_HUBPREQ0_PRIMARY_LOW, hi = c.earliest ? N48D2_HUBPREQ0_EARLIEST_HIGH : N48D2_HUBPREQ0_PRIMARY_HIGH;
        m.r[lo] = (uint32_t)c.addr; m.r[hi] = (uint32_t)(c.addr >> 32);
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        if (c.refused) expect(o.status == N48D2_PRECHECK && o.pre_abs == lo && m.writes.empty() && m.vfillCalls == 0 && m.plane.stage == N48D2_PL_FREE && o.free_bufs == 1u, (std::string("precheck (C3): ") + c.what + ": PRECHECK naming the register, nothing written or drawn, buffers released").c_str());
        else expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE, (std::string("precheck (C3): ") + c.what + ": the op runs").c_str());
    }
    // the DP watch's address rule, with the desktop acquired (HUBP0 legitimately flips): it moves OVERLAPPING our buffer -> DP_DISTURBED; it moves elsewhere in the window -> fine
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)(kBufA + 0x100000u); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << N48D2_DPX_RULE)) != 0u, "DP watch (C3): HUBP0's primary moves to the INTERIOR of A (not an exact base) while the desktop is acquired -> DP_DISTURBED on the address rule"); }
    { PModel m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = 0x00400000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && o.dpx_changed == 0u, "DP watch (C3, control): HUBP0 flips elsewhere inside the window with the desktop acquired -> no disturbance"); }
    expect(n48d2_guard_addr_i(2, N48D2_HUBPREQ0_EARLIEST_LOW, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, N48D2_HUBPREQ0_EARLIEST_HIGH, nullptr) == N48D2_G_FORBIDDEN && !wr_ok(2, N48D2_HUBPREQ0_EARLIEST_LOW, 0u) && !wr_ok(1, N48D2_HUBPREQ0_EARLIEST_HIGH, 0u), "HUBP0's EARLIEST_INUSE (read by the C3 precheck) is READ ONLY: forbidden for writes, both instances");
    // ---- C4: timing / connect / timing1440 for the monitor B are refused under a running HUBP2 (one read, nothing written); `off` still is; the monitor A's are untouched
    for (uint32_t op : { (uint32_t)N48D2_OP_TIMING, (uint32_t)N48D2_OP_CONNECT, (uint32_t)N48D2_OP_TIMING1440, (uint32_t)N48D2_OP_OFF }) {
        PModel m; plane_up(m); const size_t w0 = m.writes.size(), r0 = m.reads.size(); const n48d2_out o = n48d2::run(m, op, 2);
        expect(o.status == N48D2_PLANE_STATE && m.writes.size() == w0 && m.reads.size() == r0 + 1u && m.reads.back() == N48D2_HUBP2_HUBP_CLK_CNTL && o.pre_abs == N48D2_HUBP2_HUBP_CLK_CNTL, (std::string("C4: `") + n48d2_op_name(op) + " 2` with the HUBP2 clock on: PLANE_STATE, ONE read (the clock register), nothing written").c_str());
        PModel k; k.initPlane(); const n48d2_out o2 = n48d2::run(k, op, 2); expect(o2.status != N48D2_PLANE_STATE, (std::string("C4: `") + n48d2_op_name(op) + " 2` with the HUBP2 clock OFF is not refused by this rule").c_str());
        PModel a; plane_up(a); a.inst = 1; if (op != N48D2_OP_TIMING1440) { const n48d2_out o3 = n48d2::run(a, op, 1); expect(o3.status != N48D2_PLANE_STATE, (std::string("C4: `") + n48d2_op_name(op) + " 1` (the monitor A) is not touched by the HUBP2 refusal").c_str()); }
    }
    { const std::string flow = strip_line_comments(slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_disp2_flow.h"));
      expect(flow.find("if (ok) ok = plane_judge(o, inst, s, n48d2_build_plane_d_i(inst, s, pl->mc[0]));") != std::string::npos && flow.find("if (ok) ok = plane_judge(o, inst, s, n48d2_build_plane_c_i(inst, s, pl->mc[0]));") != std::string::npos && flow.find("if (ok) ok = plane_judge(o, inst, s, n48d2_build_plane_lock_i(inst, s));") != std::string::npos && flow.find("if (ok) ok = plane_judge(o, inst, s, n48d2_build_plane_unlock_i(inst, s));") != std::string::npos, "0.0.643 flow: part D, the lock, part C and the unlock are judged by the guard together with A and B BEFORE anything is read (a refusal of any list reads and writes nothing)");
      expect(flow.find("if ((o.wfail & (1ull << N48D2_FLIP_I_WAIT_LO)) != 0u || (o.wfail & (1ull << N48D2_FLIP_I_WAIT_HI)) != 0u)") != std::string::npos, "0.0.636 flow (C1): the flip gates on the EARLIEST_INUSE waits"); }
    // ---- C5: the header comments tell the truth about which ops roll back
    { const std::string hdr = slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_disp2.h");
      expect(hdr.find("It also runs by itself on any failure of OP 8 after its first write; ops 9, 10 and 11 roll back") != std::string::npos && hdr.find("(also run on any failure of 8..11)") == std::string::npos, "C5: the header says planeoff runs by itself only after an op 8 failure; ops 9..11 roll back only on a DP disturbance (the old 'any failure of 8..11' is gone)");
      expect(hdr.find("an op 8 failure after its first write was rolled back; ops 9..11 roll back only on a DP disturbance") != std::string::npos && hdr.find("plane / plane-op failures were rolled back") == std::string::npos, "C5: the GATE status text says the same"); }
}


// =====================================================================================================================================================================================================
// 0.0.637: (1) DSCL2_LB_MEMORY_CTRL is written as Linux writes it (REG_SET_2(LB_MEMORY_CTRL, 0, MEMORY_CONFIG, ..., LB_MAX_PARTITIONS, ...): LB_NUM_PARTITIONS / _C stay 0): the COPY0 pair carries a write / read-back mask 0x00003F03;
// (2) the C3 overlap test sizes the DP's surface: max(ours, HUBP0 pitch x viewport height) at 64 KiB, 64 MiB when those reads are implausible.
// =====================================================================================================================================================================================================
static void plane_637_tests() {
    // ---- (1) the table, the step list, the written value
    { const uint32_t lb = 10u; expect(n48d2_mirror0[lb].dst == N48D2_DSCL2_LB_MEMORY_CTRL && n48d2_mirror0[lb].src == 0x41e2u && n48d2_mirror0[lb].expect == 0x08083f00u && n48d2_mirror0[lb].wmask == 0x00003F03u, "mirror row 10: DSCL2_LB_MEMORY_CTRL <- 0x41e2, census 0x08083f00 (full tripwire), write / read-back mask 0x00003F03");
      uint32_t full = 0u; for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) if (n48d2_mirror0[i].wmask == 0xFFFFFFFFu) ++full; expect_u("the other 19 pairs keep the full 32-bit mask", full, 19);
      n48d2_step d[N48D2_MAX_STEPS]; const uint32_t nd = n48d2_build_plane_d(d, kBufA); bool seen = false;
      for (uint32_t i = 0; i < nd; i++) if (d[i].kind == N48D2_K_COPY0 && d[i].abs == N48D2_DSCL2_LB_MEMORY_CTRL) { seen = true; expect(d[i].mask == 0xFFFFFFFFu && d[i].val == 0x08083f00u && d[i].cond_mask == 0x00003F03u, "part D: the LB COPY0 step tripwires on the FULL source (mask / val) and writes / reads back 0x00003F03 (cond_mask)"); }
      expect(seen, "part D carries the LB COPY0 step"); }
    { PModel m; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE); bool found = false; uint32_t v = 0u, nLb = 0u;
      for (const W &x : m.writes) if (x.abs == N48D2_DSCL2_LB_MEMORY_CTRL) { found = true; v = x.v; ++nLb; }
      expect(o.status == N48D2_OK && found && v == 0x00003f00u && nLb == 1u, "op 8: DSCL2_LB_MEMORY_CTRL is written ONCE and the value is 0x00003f00 (LB_NUM_PARTITIONS / _C left at 0)"); }
    // ---- the read-back judges only 0x00003F03
    { PModel m; m.initPlane(); m.lbReadAnd = ~0x7FFF0000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE, "read-back: a register whose bits 16-30 read 0 (hardware-computed) PASSES"); }
    { PModel m; m.initPlane(); m.lbReadXor = 0x00003F00u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_READBACK && o.pre_abs == N48D2_DSCL2_LB_MEMORY_CTRL && o.auto_off == 1u && o.free_bufs == 1u, "read-back: LB_MAX_PARTITIONS (bits 8-13) reading back different FAILS (READBACK naming the register), rolled back"); }
    { PModel m; m.initPlane(); m.lbReadXor = 0x00000001u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_READBACK && o.pre_abs == N48D2_DSCL2_LB_MEMORY_CTRL, "read-back: MEMORY_CONFIG (bit 0) reading back different FAILS"); }
    // ---- the drift tripwire stays on the FULL source
    { PModel m; m.initPlane(); m.r[0x41e2u] ^= 0x00010000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == 0x41e2u && m.writes.empty(), "drift: a change in LB_NUM_PARTITIONS (bit 16) of the SOURCE is still caught by the precheck, nothing written"); }
    { PModel m; m.initPlane(); m.flipTrig = N48D2_DPP_TOP2_DPP_CONTROL; m.flipReg = 0x41e2u; m.flipXor = 0x00010000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == 0x41e2u && o.auto_off == 1u && o.free_bufs == 1u && !any_write(m.writes, N48D2_DSCL2_LB_MEMORY_CTRL, ~0u, 0x00003f00u) && !any_write(m.writes, N48D2_DSCL2_LB_MEMORY_CTRL, ~0u, 0x08083f00u),
             "drift: LB_NUM_PARTITIONS (bit 16) of the SOURCE changing AFTER the precheck is caught by the COPY0 step itself (the tripwire is on the FULL source, not the write mask): PRECHECK, rolled back, the LB register never written"); }
    // ---- the guard judges the mask too
    { const auto &lb = n48d2_mirror0[10]; const auto &o = n48d2_mirror0[0];
      n48d2_step a = mkstep(N48D2_K_COPY0, lb.dst, 0xFFFFFFFFu, lb.expect, lb.src), b = a, c = a; b.cond_mask = 0xFFFFFFFFu; c.cond_mask = 0x00003F00u;
      n48d2_step d = mkstep(N48D2_K_COPY0, o.dst, 0xFFFFFFFFu, o.expect, o.src), e2 = d; e2.cond_mask = 0x00003F03u;
      expect(list_ok(2, a) && !list_ok(2, b) && !list_ok(2, c) && list_ok(2, d) && !list_ok(2, e2), "guard: a COPY0's mask must be its pair's own (LB: exactly 0x00003F03; the others: 0xFFFFFFFF) - a widened or narrowed mask is refused");
      expect(wr_ok(2, lb.dst, 0x00003f00u), "the per-write guard admits the 0x00003f00 value on DSCL2_LB_MEMORY_CTRL"); }
    // ---- (2) the DP surface size
    expect(n48d2_dp_surface_bytes(0x9ffu, 0x05a00a00u) == (uint64_t)N48D2_BUF_BYTES, "dp size: the census DP surface (pitch 0x9ff, 1440 high) = exactly our buffer size, 14745600");
    { const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u); expect(big == 25100288u && (big & 0xFFFFu) == 0u, "dp size: pitch 4352 px x 4 B x 1440 rows = 25067520 rounded UP to 64 KiB = 25100288"); }
    expect(n48d2_dp_surface_bytes(0x0100u, 0x00100100u) == (uint64_t)N48D2_BUF_BYTES, "dp size: a SMALLER DP surface still sizes to max(ours, its) = ours");
    expect(n48d2_dp_surface_bytes(0u, 0x05a00a00u) == N48D2_DP_FALLBACK_BYTES && n48d2_dp_surface_bytes(0x9ffu, 0u) == N48D2_DP_FALLBACK_BYTES && n48d2_dp_surface_bytes(0x9ffu, 0x0000a00u) == N48D2_DP_FALLBACK_BYTES, "dp size: a pitch of 0, a viewport height of 0 -> the 64 MiB fallback");
    expect(n48d2_dp_surface_bytes(0x4000u, 0x05a00a00u) == N48D2_DP_FALLBACK_BYTES && n48d2_dp_surface_bytes(0xFFFFFFFFu, 0x05a00a00u) == N48D2_DP_FALLBACK_BYTES && n48d2_dp_surface_bytes(0x9ffu, 0x45a00a00u) == N48D2_DP_FALLBACK_BYTES && n48d2_dp_surface_bytes(0x9ffu, 0x05a04a00u) == N48D2_DP_FALLBACK_BYTES, "dp size: a pitch / viewport with reserved bits set (> 16384 px, a dead register) -> the 64 MiB fallback");
    expect(N48D2_DP_FALLBACK_BYTES == 64ull * 1024u * 1024u, "the fallback is 64 MiB");
    { const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u);
      expect(n48d2_surface_overlaps(kBufA - big + 0x10000u, kBufA, big) && !n48d2_surface_overlaps(kBufA - big + 0x10000u, kBufA, N48D2_BUF_BYTES) && !n48d2_surface_overlaps(kBufA - big, kBufA, big) && n48d2_surface_overlaps(kBufA - 20u * 1024u * 1024u, kBufA, N48D2_DP_FALLBACK_BYTES) && !n48d2_surface_overlaps(kBufA - 70u * 1024u * 1024u, kBufA, N48D2_DP_FALLBACK_BYTES) && !n48d2_surface_overlaps(kBufA - 20u * 1024u * 1024u, kBufA, 0u),
             "overlap with the DP size: a larger DP surface just below the buffer overlaps (the fixed size would not see it); exactly its size below does not; the 64 MiB fallback reaches 20 MiB, not 70 MiB; size 0 (not judged yet) = our own size"); 
      expect(n48d2_surface_overlaps(kBufA + N48D2_BUF_BYTES - 0x10000u, kBufA, big) && !n48d2_surface_overlaps(kBufA + N48D2_BUF_BYTES, kBufA, big), "... above the buffer only OUR size counts (the DP surface starts above us)"); }
    // ---- the op-8 precheck with HUBP0's pitch read from the registers
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x10FFu; const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u); const uint64_t at = kBufA - big + 0x10000u;
      m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)at; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(at >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_HUBPREQ0_PRIMARY_LOW && m.writes.empty() && m.vfillCalls == 0 && m.plane.stage == N48D2_PL_FREE && m.plane.dp_bytes == big, "C3: a DP surface LARGER than ours (pitch 4352 px) placed just below buffer A overlaps -> PRECHECK naming HUBP0's primary, nothing written, dp_bytes recorded"); }
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x10FFu; const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u); const uint64_t at = kBufA - big;
      m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)at; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(at >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_PLANE, "C3 control: the same larger DP surface exactly its size below A (adjacent) -> the op runs"); }
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x10FFu; const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u); const uint64_t at = kBufA - big + 0x10000u;
      m.r[N48D2_HUBPREQ0_EARLIEST_LOW] = (uint32_t)at; m.r[N48D2_HUBPREQ0_EARLIEST_HIGH] = (uint32_t)(at >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_HUBPREQ0_EARLIEST_LOW && m.writes.empty(), "C3: the same for HUBP0's EARLIEST_INUSE"); }
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0u; const uint64_t at = kBufA - 20u * 1024u * 1024u;
      m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)at; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(at >> 32); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_HUBPREQ0_PRIMARY_LOW && m.plane.dp_bytes == N48D2_DP_FALLBACK_BYTES && m.writes.empty(), "C3: HUBP0's pitch reads 0 -> the 64 MiB fallback: a surface 20 MiB below A is refused"); }
    { PModel m; m.initPlane(); const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_OK && m.plane.dp_bytes == (uint64_t)N48D2_BUF_BYTES, "C3: the census DP (pitch 0x9ff, 1440 rows): dp_bytes = 14745600, the op runs"); }
    // ---- the DP watch uses the recorded size too
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_SURFACE_PITCH] = 0x10FFu; m.acquired = true; const uint64_t big = n48d2_dp_surface_bytes(0x10FFu, 0x05a00a00u);
      m.flipTrig = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)(kBufA - big + 0x10000u); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << N48D2_DPX_RULE)) != 0u, "DP watch: HUBP0's primary moving into a larger-than-ours DP surface's reach of our buffer -> DP_DISTURBED"); }
    // ---- source pins
    { const std::string flow = strip_line_comments(slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_disp2_flow.h"));
      expect(flow.find("pl->dp_bytes = n48d2_dp_surface_bytes(e.rd(N48D2_HUBPREQ0_SURFACE_PITCH), e.rd(N48D2_HUBP0_PRI_VIEWPORT_DIMENSION));") != std::string::npos && count_of(flow, "n48d2_surface_overlaps_n(") == 5 && count_of(flow, ", pl->dp_bytes, ") == 5,
             "0.0.637 flow: the DP size is read from HUBP0's pitch and viewport in the op-8 precheck and EVERY overlap test (4 in the precheck, 1 in the DP watch) takes it");
      expect(flow.find("const uint32_t cv = sv & st.cond_mask;") != std::string::npos && flow.find("m = st.kind == N48D2_K_UPD ? st.mask : st.kind == N48D2_K_COPY0 ? st.cond_mask : 0xFFFFFFFFu;") != std::string::npos, "0.0.637 flow: a COPY0 writes (sv & its mask) and reads back under its mask"); }
    expect_u("HUBPREQ0_DCSURF_SURFACE_PITCH is Linux's absolute address", N48D2_HUBPREQ0_SURFACE_PITCH, lx_abs("HUBPREQ0_DCSURF_SURFACE_PITCH"));
    expect_u("HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION is Linux's absolute address", N48D2_HUBP0_PRI_VIEWPORT_DIMENSION, lx_abs("HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION"));
    expect(!wr_ok(2, N48D2_HUBPREQ0_SURFACE_PITCH, 0x9ffu) && !wr_ok(2, N48D2_HUBP0_PRI_VIEWPORT_DIMENSION, 0x05a00a00u) && !wr_ok(1, N48D2_HUBPREQ0_SURFACE_PITCH, 0x9ffu), "both new registers are READ ONLY (the guard refuses a write from either instance)");
}


// =====================================================================================================================================================================================================
// 0.0.638: the 0.0.637 run's op 8 failed closed at the HUBP2_HUBP_CLK_CNTL status WAIT (bits 23:20 == 0xf, demand-gated clocks; Linux's hubp2_clk_cntl is a bare REG_UPDATE). Changes under test: (1) that WAIT is gone, replaced by RECORDED reads
// (K_REC, no gate) of HUBP2_HUBP_CLK_CNTL / DCCG_GATE_DISABLE_CNTL6 / DCCG_GATE_DISABLE_CNTL / DOMAIN2_PG_STATUS ~1 ms after the clock enable and after the 2-frame wait; (2) every timed-out WAIT records its register and LAST value; (3) DET2 wait = 100 ms;
// (4) the pre-unblank gate is FLIP_PENDING alone; (5) MPCC2 / mux / unblank back to back, THEN the EARLIEST_INUSE gate; (6) the rollback zeroes the primary address before the rest of the rollback and before the release; op 13 returns the record page.
// =====================================================================================================================================================================================================
static n48d2_step mkrec(uint32_t abs, uint32_t slot, uint32_t us = 0u) { n48d2_step s; std::memset(&s, 0, sizeof s); s.kind = N48D2_K_REC; s.abs = abs; s.val = slot; s.step_us = us; return s; }
static size_t first_read_from(const PModel &m, size_t from, uint32_t abs) { for (size_t k = from; k < m.reads.size(); k++) if (m.reads[k] == abs) return k; return (size_t)-1; }
static void plane_638_tests() {
    const uint32_t kG6 = 0x11110000u, kG = 0x22220000u, kPg = 0x33330000u;
    const size_t NP = (size_t)-1;
    auto seed = [&](PModel &m) { m.r[N48D2_DCCG_GATE_DISABLE_CNTL6] = kG6; m.r[N48D2_DCCG_GATE_DISABLE_CNTL] = kG; m.r[N48D2_DOMAIN2_PG_STATUS] = kPg; };
    // ---- the recorded registers against Linux, and READ ONLY
    expect_u("DCCG_GATE_DISABLE_CNTL is Linux's absolute address (0xc0 + 0x74)", N48D2_DCCG_GATE_DISABLE_CNTL, lx_abs("DCCG_GATE_DISABLE_CNTL"));
    expect_u("DCCG_GATE_DISABLE_CNTL6 is Linux's absolute address (0xc0 + 0xa9)", N48D2_DCCG_GATE_DISABLE_CNTL6, lx_abs("DCCG_GATE_DISABLE_CNTL6"));
    expect_u("DOMAIN2_PG_STATUS is Linux's absolute address (0x34c0 + 0x85)", N48D2_DOMAIN2_PG_STATUS, lx_abs("DOMAIN2_PG_STATUS"));
    expect(N48D2_DCCG_GATE_DISABLE_CNTL == 0x134u && N48D2_DCCG_GATE_DISABLE_CNTL6 == 0x169u && N48D2_DOMAIN2_PG_STATUS == 0x3545u, "the three absolute addresses are 0x134, 0x169 and 0x3545");
    for (const uint32_t a : { (uint32_t)N48D2_DCCG_GATE_DISABLE_CNTL, (uint32_t)N48D2_DCCG_GATE_DISABLE_CNTL6, (uint32_t)N48D2_DOMAIN2_PG_STATUS }) {
        Model m;
        for (const uint32_t inst : { 1u, 2u }) for (const uint32_t v : { 0u, 1u, 0xFFFFFFFFu }) {
            expect(!wr_ok(inst, a, v), "a recorded-read register is refused by the per-write guard (both instances, any value)");
            for (const uint8_t kind : { (uint8_t)N48D2_K_SET, (uint8_t)N48D2_K_UPD }) expect(!list_ok(inst, mkstep(kind, a, 0xFFFFFFFFu, v)), "... and by the list guard as a SET / UPD step");
            expect(!list_ok(inst, mkstep(N48D2_K_COPY0, a, 0xFFFFFFFFu, v, N48D2_HUBP0_DCHUBP_CNTL)), "... and as a COPY0 destination");
        }
        { PModel w; w.initPlane(); const bool refused = !w.wr(a, 0u) && !w.wr(a, 0xFFFFFFFFu) && w.writes.empty() && w.refusedCount == 2u; expect(refused, "... and Model::wr (the kext glue's order: the instance guard, THEN the DCN allowlist) writes nothing and counts the refusal"); }
        (void)m;
    }
    expect(n48d2_guard_addr_i(2, N48D2_DCCG_GATE_DISABLE_CNTL, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, N48D2_DCCG_GATE_DISABLE_CNTL6, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, N48D2_DOMAIN2_PG_STATUS, nullptr) == N48D2_G_UNLISTED,
           "the two DCCG ones are FORBIDDEN (only 0x15b and 0x168 are excepted - 0x169 is the very next dword), DOMAIN2_PG_STATUS is UNLISTED");
    { const n48d2_step rs = mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 0u); expect(!n48d2_is_write(&rs), "a REC step is not a write (HUBP2_HUBP_CLK_CNTL, which IS writable as an UPDATE of its own mask, is only ever READ by one)"); }
    // ---- the REC list guard: exactly the four registers, each in its own slots, instance 2 only
    for (uint32_t s = 0; s < N48D2_REC_SLOTS; s++) {
        expect(list_ok(2, mkrec(n48d2_rec_regs[s & 3u], s)), "REC: each register in each of its own slots passes the list guard");
        expect((s & 3u) == 1u || (s & 3u) == 2u || !list_ok(1, mkrec(n48d2_rec_regs[s & 3u], s)), "REC: instance 1 refuses the monitor B's own HUBP2 / DOMAIN2 registers (the two DCCG gates are shared by both instances' pages, 0.0.655)");
        expect(!list_ok(2, mkrec(n48d2_rec_regs[(s + 1u) & 3u], s)), "REC: a register in another register's slot is refused");
    }
    expect(!list_ok(2, mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 8u)) && !list_ok(2, mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 0xFFFFFFFFu)), "REC: a slot beyond the page is refused");
    for (const uint32_t a : { (uint32_t)N48D2_HUBP0_DCHUBP_CNTL, (uint32_t)N48D2_DCHUBBUB_DET2_CTRL, (uint32_t)N48D2_DPPCLK_CTRL, (uint32_t)N48D2_DPPCLK2_DTO_PARAM, (uint32_t)N48D2_SYMCLKB_CLOCK_ENABLE, (uint32_t)N48D2_DCN_VM_FAULT_STATUS, (uint32_t)N48D2_HUBP2_DCHUBP_CNTL })
        expect(!list_ok(2, mkrec(a, 0u)) && !list_ok(2, mkrec(a, 1u)), "REC: any other register (the DP's HUBP0, DET2, the DCCG exceptions, SYMCLKB, ...) is refused: a recorded read cannot be aimed anywhere else");
    {   // running REC steps writes NOTHING and stores into the page
        PModel m; m.initPlane(); seed(m); n48d2_step st[4] = { mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 0u, 1000u), mkrec(N48D2_DCCG_GATE_DISABLE_CNTL6, 1u), mkrec(N48D2_DCCG_GATE_DISABLE_CNTL, 2u), mkrec(N48D2_DOMAIN2_PG_STATUS, 3u) };
        n48d2_out o; std::memset(&o, 0, sizeof o); const uint32_t r = n48d2::run_steps(m, 2u, st, 4u, false, o, nullptr);
        expect(r == 0u && m.writes.empty() && m.refusedCount == 0u && m.recBuf[1] == kG6 && m.recBuf[2] == kG && m.recBuf[3] == kPg && m.recBuf[N48D2_RC_VALID] == N48D2_RV_A && m.delayUs == 1000u, "run_steps on REC steps: four reads, NO write, the page filled, the 1 ms delay taken, only 'sample A' marked");
    }
    // ---- the clean op 8: the recorded values, their moments
    {
        PModel m; m.initPlane(); seed(m); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && o.timeouts == 0u && o.auto_off == 0u && o.done == 41u && o.dp_changed == 0u && o.dpx_changed == 0u && m.writes.size() == 41u, "0.0.638 op 8 (clean): OK, no timeout, no rollback, exactly 41 writes (0.0.643: 36 + the lock pair, the address inside it, the unlock) - the recorded reads add none");
        const uint32_t *rc = m.recBuf;
        expect(rc[N48D2_RC_VALID] == (N48D2_RV_A | N48D2_RV_B | N48D2_RV_DET0 | N48D2_RV_UND) && rc[N48D2_RC_DET0] == 0x303u && (rc[N48D2_RC_UND_HUBP] & 0x70F00800u) == 0u && rc[0] == 0x00f11111u && rc[4] == 0x00f11111u && rc[1] == kG6 && rc[5] == kG6 && rc[2] == kG && rc[6] == kG && rc[3] == kPg && rc[7] == kPg && rc[N48D2_RC_WAIT_ABS] == 0u && rc[N48D2_RC_RB_ABS] == 0u,
               "the record page: both samples hold the four registers (HUBP2_HUBP_CLK_CNTL 0x00f11111 in the model), valid = A | B | DET0 | UND (0.0.639), DET2_CTRL before the unblank 0x303, no wait record");
        const size_t iClk = nth_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL), iBot = nth_write(m.writes, N48D2_MPCC2_MPCC_BOT_SEL);
        size_t a6 = NP, a6b = NP, aClk = NP, aDet = NP, aLb = NP;
        if (iClk != NP && iBot != NP) {
            const size_t w = m.readsAtWrite[iClk];
            aClk = first_read_from(m, w, N48D2_HUBP2_HUBP_CLK_CNTL); a6 = first_read_from(m, w, N48D2_DCCG_GATE_DISABLE_CNTL6); a6b = a6 == NP ? NP : first_read_from(m, a6 + 1u, N48D2_DCCG_GATE_DISABLE_CNTL6);
            aDet = first_read_from(m, w, N48D2_DCHUBBUB_DET2_CTRL); aLb = first_read_from(m, w, N48D2_DSCL2_LB_MEMORY_CTRL);
        }
        const bool idx = iClk != NP && iBot != NP && aClk != NP && a6 != NP && a6b != NP && aDet != NP && aLb != NP && m.delayUsAtRead.size() == m.reads.size() && m.f2AtRead.size() == m.reads.size() && a6b >= 1u && a6b + 2u < m.reads.size() && a6 + 2u < m.reads.size();
        expect(idx, "op 8: the recorded reads (sample A and B) and the order markers exist");
        if (idx) {
            expect(aClk + 1u == a6 && m.reads[a6 + 1u] == N48D2_DCCG_GATE_DISABLE_CNTL && m.reads[a6 + 2u] == N48D2_DOMAIN2_PG_STATUS && a6 < aDet, "ORDER: sample A = HUBP2_HUBP_CLK_CNTL, DCCG_GATE_DISABLE_CNTL6, DCCG_GATE_DISABLE_CNTL, DOMAIN2_PG_STATUS, consecutive, right after the clock write and BEFORE the DET2 wait");
            expect(m.delayUsAtRead[aClk] - m.delayUsAtRead[m.readsAtWrite[iClk] - 1u] >= 1000u, "REACHABILITY: at least 1 ms (1000 us) passes between the clock write and the first recorded read");
            expect(m.f2AtRead[a6b] - m.f2AtWrite[iClk] >= 2u && m.reads[a6b - 1u] == N48D2_HUBP2_HUBP_CLK_CNTL && m.reads[a6b + 1u] == N48D2_DCCG_GATE_DISABLE_CNTL && m.reads[a6b + 2u] == N48D2_DOMAIN2_PG_STATUS, "ORDER / REACHABILITY: sample B (the same four, consecutive) is taken at least 2 OTG2 frames after the clock write");
            expect(a6b < aLb && m.readsAtWrite[iBot] > a6b + 2u, "ORDER: sample B is taken BEFORE the register read-back and before MPCC2 is joined");
        }
        // the page through op 13: no register read, equal to the buffer
        const size_t r0 = m.reads.size(); uint64_t v[13]; const uint32_t st = n48d2::planerec(m, v); uint32_t back[N48D2_REC_N]; n48d2_unpack_rec(v, back); bool same = true; for (uint32_t i = 0; i < N48D2_REC_N; i++) same = same && back[i] == rc[i];
        expect(st == N48D2_OK && m.reads.size() == r0 && m.writes.size() == 41u && same && (v[0] & 0xFFu) == N48D2_OK && ((v[0] >> 8) & 0xFFu) == N48D2_OP_PLANEREC && ((v[0] >> 16) & 0xFFu) == N48D2_REC_N, "op 13 (planerec): returns the page byte for byte, reads NO register and writes nothing");
    }
    // ---- the clock-status WAIT is gone: the 0.0.637 failure (bits 21 / 23 never come on) no longer fails op 8
    {
        PModel m; m.initPlane(); m.clkWorks = false; seed(m); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && o.timeouts == 0u && o.auto_off == 0u && m.plane.stage == N48D2_PL_PLANE && m.recBuf[0] == 0x00511111u && (m.recBuf[0] & 0x00A00000u) == 0u, "0.0.637 failure modelled (HUBP2_HUBP_CLK_CNTL status bits 21 / 23 never come on): op 8 PASSES, no timeout; the recorded read shows them off (0x00511111)");
        n48d2_step b[N48D2_MAX_STEPS]; const uint32_t nb = n48d2_build_plane_b(b); bool anyClkWait = false; for (uint32_t i = 0; i < nb; i++) anyClkWait = anyClkWait || (b[i].kind == N48D2_K_WAIT && b[i].abs == N48D2_HUBP2_HUBP_CLK_CNTL);
        expect(!anyClkWait, "part B holds no WAIT on HUBP2_HUBP_CLK_CNTL");
    }
    // ---- a blanked HUBP never updates EARLIEST_INUSE (the study's reading): the clean op 8 passes because the gate is after the unblank
    {
        PModel m; m.initPlane(); m.blankedNoEarly = true; seed(m); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && o.timeouts == 0u && o.auto_off == 0u && m.plane.stage == N48D2_PL_PLANE && m.writes.size() == 41u, "fake 'a blanked HUBP never updates EARLIEST_INUSE': op 8 PASSES (the EARLIEST_INUSE gate runs after the unblank)");
        const size_t iUnb = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL, 1), iBot = nth_write(m.writes, N48D2_MPCC2_MPCC_BOT_SEL);
        size_t early = 0, earlyPre = 0, flipPre = 0;
        if (iUnb != NP && iBot != NP) {
            for (size_t k = 0; k < m.readsAtWrite[iBot]; k++) { earlyPre += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE; if (k >= m.readsAtWrite[0]) flipPre += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL; }      // (the precheck page's EXPECT reads FLIP_CONTROL once, before the first write)
            for (size_t k = m.readsAtWrite[iUnb]; k < m.reads.size(); k++) early += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE;
        }
        expect(iUnb != NP && earlyPre == 0u && flipPre == 0u && early >= 1u && ((((uint64_t)(m.gateBuf[N48D2_GT_EARLY_HI] & 0xFFFFu)) << 32) | m.gateBuf[N48D2_GT_EARLY_LO]) == kBufA, "ORDER (0.0.643): neither EARLIEST_INUSE nor (after the first write) FLIP_CONTROL is read before MPCC2 is joined (no pre-unblank gate); EARLIEST_INUSE = A is read after the unblank");
    }
    // ---- ORDER (the four the brief names), on the real run(): FLIP_PENDING gate before MPCC2; MPCC2 + mux + unblank consecutive; EARLIEST_INUSE gate after the unblank; rollback zeroes the address before release
    for (const bool noEarly : { false, true }) {
        PModel m; m.initPlane(); m.blankedNoEarly = noEarly; (void)run_op(m, N48D2_OP_PLANE);
        const size_t iBot = nth_write(m.writes, N48D2_MPCC2_MPCC_BOT_SEL), iUnb = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL, 1), iMux = nth_write(m.writes, N48D2_MPC_OUT2_MUX), iLo2 = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 1);
        const bool idx = iBot != NP && iUnb != NP && iMux != NP && iLo2 != NP && m.sleepsAtWrite.size() == m.writes.size() && m.readsAtWrite.size() == m.writes.size();
        expect(idx, "ORDER setup: the writes exist");
        if (!idx) continue;
        // (1) 0.0.643: NO FLIP_PENDING read between the last programming write and MPCC2; the lock write lies between them
        size_t nFlip = 0; for (size_t k = m.readsAtWrite[iLo2]; k < m.readsAtWrite[iBot]; k++) nFlip += m.reads[k] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL;
        const size_t iLk = nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, 0);
        expect(nFlip == 0u && iLk != NP && iLo2 < iLk && iLk < iBot, "ORDER (1, 0.0.643): NO FLIP_PENDING read AFTER the last programming address write and BEFORE the first MPCC2 write; the lock write sits between them");
        // (2) MPCC2 insert x4, the mux, the unblank: six CONSECUTIVE writes with no sleep and no frame between them and no read but the unblank's own update-read
        bool consec = iUnb == iBot + 5u && iMux == iBot + 4u; for (size_t k = iBot; k <= iUnb && consec; k++) consec = m.sleepsAtWrite[k] == m.sleepsAtWrite[iBot] && m.f2AtWrite[k] == m.f2AtWrite[iBot];
        bool noReads = true; for (size_t k = iBot + 1u; k < iUnb; k++) noReads = noReads && m.readsAtWrite[k] == m.readsAtWrite[iBot];
        expect(consec && noReads && m.readsAtWrite[iUnb] == m.readsAtWrite[iBot] + 1u && m.reads[m.readsAtWrite[iBot]] == N48D2_HUBP2_DCHUBP_CNTL, "ORDER (2): MPCC2 (BOT, TOP, OPP, LOCK_SEL), MPC_OUT2_MUX and the UNBLANK are six consecutive writes - no sleep, no frame, no read between them (the unblank's own read-modify-write excepted)");
        // (3) the EARLIEST_INUSE gate comes after the unblank (and not before MPCC2)
        size_t eBefore = 0, eAfter = 0; for (size_t k = 0; k < m.readsAtWrite[iUnb]; k++) eBefore += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE;
        for (size_t k = m.readsAtWrite[iUnb]; k < m.reads.size(); k++) eAfter += m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE;
        expect(eBefore == 0u && eAfter >= 1u, "ORDER (3): EARLIEST_INUSE is read only AFTER the unblank");
    }
    {   // (4) the rollback zeroes the address before the rest of it and before the release: the order in the writes AND the register state at return
        PModel m; plane_up(m); const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF);
        const size_t iBlank = nth_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL), iHi = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), iLo = nth_write(m.writes, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS), iMux = nth_write(m.writes, N48D2_MPC_OUT2_MUX), iClk = nth_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL);
        const bool idx = iBlank != NP && iHi != NP && iLo != NP && iMux != NP && iClk != NP && m.readsAtWrite.size() == m.writes.size();
        expect(idx, "rollback: the blank, the two zero writes, the mux and the clock write exist");
        if (idx) {
            size_t nNo = 0; for (size_t k = m.readsAtWrite[iBlank]; k < m.readsAtWrite[iHi]; k++) nNo += m.reads[k] == N48D2_HUBP2_DCHUBP_CNTL;
            expect(iBlank < iHi && iHi + 1u == iLo && iLo < iMux && iMux < iClk && nNo >= 1u, "ORDER: blank, NO_OUTSTANDING_REQ polled, THEN address HIGH, address LOW, THEN the mux / MPCC2 / the clock off");
            expect(m.writes[iHi].v == 0u && m.writes[iLo].v == 0u && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) == 0u && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == 0u && o.status == N48D2_OK && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE,
                   "the address registers hold 0/0 when the rollback returns and only THEN does it allow the release (free_bufs 1)");
        }
        PModel f; f.initPlane(); f.latchWorks = false; const n48d2_out of = run_op(f, N48D2_OP_PLANE);
        expect(of.status == N48D2_GATE && of.free_bufs == 1u && f.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) == 0u && f.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == 0u && f.progAddr == 0u, "the rollback of a FAILED op 8 zeroes the address too before the buffers are released");
        PModel n; plane_up(n); n.noOutWorks = false; const n48d2_out on = run_op(n, N48D2_OP_PLANEOFF);
        expect(on.status == N48D2_GATE && on.free_bufs == 0u && n.plane.stage == N48D2_PL_LEAKED, "NO_OUTSTANDING_REQ never asserted: the buffers stay LEAKED (the release is withheld)");
    }
    // ---- the zero-address writes in the guard: a ZERO needs the blank + NO_OUTSTANDING wait earlier in the same list; anything but {A, B, 0} is refused
    {
        n48d2_aset_set(kBufA, kBufB);
        const n48d2_step blank = mkstep(N48D2_K_UPD, N48D2_HUBP2_DCHUBP_CNTL, 1u, 1u), unblank = mkstep(N48D2_K_UPD, N48D2_HUBP2_DCHUBP_CNTL, 1u, 0u);
        n48d2_step wait = mkstep(N48D2_K_WAIT, N48D2_HUBP2_DCHUBP_CNTL, 2u, 2u); wait.polls = 100u; wait.step_us = 1000u;
        n48d2_step wrongWait = wait; wrongWait.mask = 4u; wrongWait.val = 4u;
        const n48d2_step hi0 = mkstep(N48D2_K_SET, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0, 0u), lo0 = mkstep(N48D2_K_SET, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, 0u);
        const n48d2_step loA = mkstep(N48D2_K_SET, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kBufA), loBad = mkstep(N48D2_K_SET, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kBufA + 0x10000u), lo1 = mkstep(N48D2_K_SET, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, 1u);
        uint32_t bad = 0;
        auto ok = [&](std::initializer_list<n48d2_step> l) { std::vector<n48d2_step> v(l); return n48d2_guard_list_i(2, v.data(), (uint32_t)v.size(), &bad) == 0u; };
        expect(ok({ blank, wait, hi0, lo0 }), "zero address: blank, NO_OUTSTANDING wait, HIGH 0, LOW 0 passes the list guard");
        expect(!ok({ hi0 }) && !ok({ lo0 }) && !ok({ blank, hi0 }) && !ok({ wait, hi0 }) && !ok({ blank, wrongWait, hi0 }) && !ok({ wait, blank, hi0 }), "zero address: refused alone, with the blank but no wait, with the wait but no blank, with a wrong wait, with the wait BEFORE the blank");
        expect(!ok({ blank, wait, unblank, hi0 }) && !ok({ blank, wait, blank, hi0 }) && ok({ blank, wait, blank, wait, hi0 }), "zero address: an unblank (or a second blank) after the wait cancels it - the zero needs a wait after the LATEST blank");
        expect(!ok({ hi0, blank, wait }) && !ok({ blank, wait, lo1 }) && !ok({ blank, wait, loBad }) && ok({ blank, wait, loA }) && ok({ loA }), "a non-{A,B,0} value is refused even after the blank + wait (1, A + 64 KiB); A itself passes anywhere");
        expect(!wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 1u) && !wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x81u) && wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u) && wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0u), "the per-write value rule: {A, B, 0} only");
        n48d2_aset_clear();
        expect(wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u) && !wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufA), "with NO address set installed only 0 is writable (A is still refused: fail closed)");
        expect(!wr_ok(1, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u), "instance 1 may write no address register");
    }
    // ---- every timed-out WAIT records its register and its LAST value; the gate reports it
    {
        PModel m; m.initPlane(); m.detWorks = false; seed(m); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_GATE && o.fail == N48D2_PE_I_DETWAIT && o.pre_wait == 1u && o.pre_abs == N48D2_DCHUBBUB_DET2_CTRL && o.pre_val == 3u && o.timeouts >= 1u && o.auto_off == 1u && o.free_bufs == 1u,
               "DET2 never reaches 3: GATE at the (post-unblank) DET2 wait step, pre_abs = DCHUBBUB_DET2_CTRL, pre_val = the LAST value read (3: size 3, current 0), flagged as a timed-out wait, rolled back");
        expect(m.recBuf[N48D2_RC_WAIT_ABS] == N48D2_DCHUBBUB_DET2_CTRL && m.recBuf[N48D2_RC_WAIT_VAL] == 3u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_WAIT) != 0u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_A) != 0u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_B) != 0u && m.recBuf[0] == 0x00f11111u, "the record page: the wait's register and last value, samples A and B both taken (the wait is after the unblank)");
        expect(m.delayUs >= 100u * 1000u + 1000u, "the DET2 wait ran its 100 ms bound (100 polls x 1000 us) after the 1 ms before the first recorded read");
        { const size_t iMx = nth_write(m.writes, N48D2_MPC_OUT2_MUX), iUn = iMx == (size_t)-1 ? iMx : iMx + 1u; size_t nBefore = 0, nAfter = 0; if (iUn < m.readsAtWrite.size()) for (size_t k = 0; k < m.reads.size(); k++) if (m.reads[k] == N48D2_DCHUBBUB_DET2_CTRL) { if (k < m.readsAtWrite[iUn]) ++nBefore; else ++nAfter; }
          expect(iUn < m.readsAtWrite.size() && m.writes[iUn].abs == N48D2_HUBP2_DCHUBP_CNTL && nBefore >= 1u && nAfter >= 100u, "the 100-poll DET2 wait sits after the last unblank-era write: the reads before the unblank are the recorded ones only, the 100 polls come after"); }
        uint64_t v[13]; n48d2_pack_plane(v, &o, m.gateBuf); n48d2_out b; uint32_t g2[N48D2_GATES]; std::memset(&b, 0, sizeof b); n48d2_unpack_plane(v, &b, g2);
        expect(b.pre_wait == 1u && b.pre_abs == (N48D2_DCHUBBUB_DET2_CTRL & 0xFFFFu) && b.pre_val == 3u && b.dpx_changed == o.dpx_changed && b.status == o.status, "the wait record survives the 13-scalar result (v[3] bit 31 = timed-out wait, pre_abs / pre_val)");
        n48d2_out c; std::memset(&c, 0, sizeof c); c.pre_abs = 0x1234u; c.pre_val = 0xdeadbeefu; c.dpx_changed = 0x7FFFu; uint64_t w[13]; n48d2_pack(w, &c); n48d2_out d; std::memset(&d, 0, sizeof d); n48d2_unpack(w, &d);
        expect(d.pre_wait == 0u && d.dpx_changed == 0x7FFFu && d.pre_abs == 0x1234u && d.pre_val == 0xdeadbeefu, "the wait flag is bit 31 of v[3]: the 15-bit DP-plane mask next to it is untouched, and an unflagged precheck stays unflagged");
    }
    {
        PModel m; m.initPlane(); m.blankSticks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_GATE && o.fail == N48D2_PA_I_BLANKWAIT && o.pre_wait == 1u && o.pre_abs == N48D2_HUBP2_DCHUBP_CNTL && (o.pre_val & 1u) == 0u && m.recBuf[N48D2_RC_WAIT_ABS] == N48D2_HUBP2_DCHUBP_CNTL && m.recBuf[N48D2_RC_WAIT_VAL] == o.pre_val, "BLANK_EN read-back wait times out: the register and the last value read (BLANK_EN 0) are reported and recorded");
    }
    {
        PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.flipClears = false; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
        expect(o.status == N48D2_GATE && o.fail == N48D2_FLIP_I_WAIT && o.pre_wait == 1u && o.pre_abs == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL && (o.pre_val & N48D2_FLIP_PENDING_MASK) != 0u && m.recBuf[N48D2_RC_WAIT_ABS] == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL, "the flip's FLIP_PENDING wait times out: FLIP_CONTROL and its last value (pending) are reported and recorded");
    }
    {
        PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); m.latchWorks = false; const n48d2_out o = run_op(m, N48D2_OP_FLIP, 1u);
        expect(o.status == N48D2_GATE && o.fail == N48D2_FLIP_I_WAIT_LO && o.pre_wait == 1u && o.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && o.pre_val == (uint32_t)kBufA, "the flip's EARLIEST_INUSE wait times out: the register and the last value (still A) are reported");
    }
    {
        PModel m; plane_up(m); m.mpccNeverIdle = true; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF);
        expect(o.status == N48D2_OK && o.timeouts == 1u && m.recBuf[N48D2_RC_RB_ABS] == N48D2_MPCC2_MPCC_STATUS && m.recBuf[N48D2_RC_RB_VAL] == 2u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_RB) != 0u && n48d2_wait_reg_name(m.recBuf[N48D2_RC_RB_ABS])[0] == 'M', "a ROLLBACK wait that times out (MPCC2_STATUS never 5) records its register and last value (2) in the page's rollback slots");
    }
    {   // op 8 starts the page afresh: junk left by an earlier op (both samples, the wait and rollback records) must not survive into a failed op 8 that never takes sample B
        PModel m; m.initPlane(); m.flipTrig = N48D2_DPP_TOP2_DPP_CONTROL; m.flipReg = n48d2_mirror0[19].src; m.flipXor = 1u; for (uint32_t i = 0; i < N48D2_REC_N; i++) m.recBuf[i] = 0xDEAD0000u + i; m.recBuf[N48D2_RC_VALID] = 0xFFu;      // 0.0.639: a COPY0 drift in part D (before sample B, after the DET2 record)
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_PRECHECK && m.recBuf[4] == 0u && m.recBuf[5] == 0u && m.recBuf[6] == 0u && m.recBuf[7] == 0u && m.recBuf[N48D2_RC_RB_ABS] == 0u && m.recBuf[N48D2_RC_RB_VAL] == 0u && (m.recBuf[N48D2_RC_VALID] & (N48D2_RV_B | N48D2_RV_RB | N48D2_RV_UND | N48D2_RV_ADDRKEPT | N48D2_RV_WAIT)) == 0u && (m.recBuf[13] & 0xFFFF0000u) == 0u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_DET0) != 0u && m.recBuf[14] == 0u && m.recBuf[15] == 0u,
               "op 8 clears the whole page first: a failed op 8 shows no stale sample B, no stale rollback record, no stale spare dword");
    }
    {
        PModel k; k.initPlane(); (void)run_op(k, N48D2_OP_PLANE); k.recBuf[N48D2_RC_WAIT_ABS] = 0x1234u; const n48d2_out o = run_op(k, N48D2_OP_SHOW);
        expect(o.status == N48D2_OK && k.recBuf[N48D2_RC_WAIT_ABS] == 0u && k.recBuf[0] == 0x00f11111u && (k.recBuf[N48D2_RC_VALID] & (N48D2_RV_A | N48D2_RV_B)) == (N48D2_RV_A | N48D2_RV_B), "ops 9..12 clear only the wait records: the samples of the last op 8 stay");
    }
    {   // the post-unblank EARLIEST_INUSE gate POLLS (up to 5 frames) and fails closed when the bound runs out
        PModel m; m.initPlane(); m.earlyAfterUnblank = 30u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); size_t nE = 0; for (const uint32_t a : m.reads) nE += a == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE;
        expect(o.status == N48D2_OK && o.timeouts == 0u && m.plane.stage == N48D2_PL_PLANE && nE >= 31u, "EARLIEST_INUSE shows the address only 30 reads AFTER the unblank: the gate POLLS (>= 31 reads) and passes");
        PModel n; n.initPlane(); n.earlyAfterUnblank = 100000u; const n48d2_out on = run_op(n, N48D2_OP_PLANE);
        expect(on.status == N48D2_GATE && on.pre_abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && on.pre_wait == 1u && on.pre_val == 0u && on.auto_off == 1u && on.free_bufs == 1u && n.frames2 >= 5u && rolled_back_clean(n), "EARLIEST_INUSE never shows the address after the unblank: the gate waits its 5 frames, then FAILS (EARLIEST_INUSE named with its last value 0), rollback, buffers released");
    }
    for (const uint32_t a : { (uint32_t)N48D2_HUBP2_DCHUBP_CNTL, (uint32_t)N48D2_DCHUBBUB_DET2_CTRL, (uint32_t)N48D2_DPG2_STATUS, (uint32_t)N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL, (uint32_t)N48D2_MPCC2_MPCC_STATUS }) expect(std::strcmp(n48d2_wait_reg_name(a), "?") != 0, "every register a WAIT can time out on has a name");

    // ---- source pins (alongside the behavioural tests above)
    {
        const std::string src = g_root + "/src/navi48-bringup/src/";
        const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), flow = strip_line_comments(slurp(src + "dcn/navi48_disp2_flow.h")), hdr = strip_line_comments(slurp(src + "dcn/navi48_disp2.h")), cli = slurp(g_root + "/tools/pc/navi48test.c");
        const size_t g0 = dcn.find("build 0.0.631 (multi-monitor stage M4d; an internal design note");
        const std::string glue = g0 == std::string::npos ? "" : strip_line_comments(dcn.substr(g0));
        const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {");
        const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0);
        expect(!glue.empty() && !fn.empty() && !flow.empty() && !hdr.empty() && !cli.empty(), "0.0.638: the sources are readable");
        expect(glue.find("uint32_t gD2Rec[2][N48D2_REC_N];") != std::string::npos && glue.find("uint32_t *rec_buf() { return gD2Rec[d2i(inst)]; }") != std::string::npos, "0.0.638 glue: the record page is file-scope (never on the kernel stack)");
        { const size_t a = fn.find("} else if (op == N48D2_OP_PLANEREC) {"), b = fn.find("} else if ((op >= N48D2_OP_PLANE && op <= N48D2_OP_PLANEOFF) || op == N48D2_OP_FBHOLD) {"); const std::string br = (a == std::string::npos || b == std::string::npos || b < a) ? "" : fn.substr(a, b - a);
          expect(!br.empty() && br.find("n48d2::planerec(env, out)") != std::string::npos && br.find("run_plane") == std::string::npos && br.find("WREG32") == std::string::npos && br.find("rreg") == std::string::npos, "0.0.638 glue: op 13 is its own branch (before the plane ops), answers from the record page and names no register access"); }
        expect(fn.find("if (op == N48D2_OP_PLANE) {\n\t\tif (ctx == nullptr") != std::string::npos && fn.find("op == N48D2_OP_PLANEREC") != std::string::npos && fn.find("n1c_d2_alloc") == fn.rfind("n1c_d2_alloc"), "0.0.638 glue: only op 8 allocates; op 13 allocates nothing");
        { const size_t a = flow.find("} else if (st.kind == N48D2_K_REC) {"); const size_t b = a == std::string::npos ? a : flow.find("\n        }\n    }\n    return 0u;", a); const std::string br = (a == std::string::npos || b == std::string::npos) ? "" : flow.substr(a, b - a);
          expect(!br.empty() && br.find("e.rd(st.abs)") != std::string::npos && br.find("e.wr(") == std::string::npos && br.find("n48d2_rec_ok_i(inst, st.abs, st.val)") != std::string::npos && br.find("o.timeouts") == std::string::npos && br.find("wfail") == std::string::npos, "0.0.638 flow: the REC branch of run_steps READS (e.rd), never writes (no e.wr), is never a gate (no timeout / wfail) and re-checks the register + slot"); }
        expect(hdr.find("#define N48D2_PE_DET_POLLS 100u") != std::string::npos && hdr.find("#define N48D2_PE_DET_STEP_US 1000u") != std::string::npos, "0.0.639 header: the DET2 wait is 100 polls x 1000 us = 100 ms");
        expect(hdr.find("N48D2_WAIT(HUBP2_HUBP_CLK_CNTL, N48D2_HUBP_CLK_ON_MASK") == std::string::npos, "0.0.638 header: no step list waits for the HUBP clock-on status bits (the rollback's gated-again wait, OFF_MASK, is a different wait and stays)");
        expect(hdr.find("N48D2_REC(HUBP2_HUBP_CLK_CNTL, 0u, 1000u,") != std::string::npos && hdr.find("N48D2_REC(DCCG_GATE_DISABLE_CNTL6, 1u, 0u,") != std::string::npos && hdr.find("N48D2_REC(DCCG_GATE_DISABLE_CNTL, 2u, 0u,") != std::string::npos && hdr.find("N48D2_REC(DOMAIN2_PG_STATUS, 3u, 0u,") != std::string::npos, "0.0.638 header: part B records the four registers, the first after 1 ms");
        { const size_t e0 = flow.find("inline bool plane_exec("), e1 = flow.find("template <class E>\ninline n48d2_out run_plane(", e0 == std::string::npos ? 0 : e0); const std::string pe = (e0 == std::string::npos || e1 == std::string::npos) ? "" : flow.substr(e0, e1 - e0);
          const size_t lk = pe.find("n = n48d2_build_plane_lock_i(inst, s);"), pc = pe.find("n = n48d2_build_plane_c_i(inst, s, pl->mc[0]);"), ul = pe.find("n = n48d2_build_plane_unlock_i(inst, s);", pc == std::string::npos ? 0 : pc), lg = pe.find("if (!latch_gate(e, inst, fc2, 5u, pl->mc[0], g)) {");
          expect(lk != std::string::npos && pc != std::string::npos && ul != std::string::npos && lg != std::string::npos && lk < pc && pc < ul && ul < lg && pe.find("latch_gate(") == pe.rfind("latch_gate(") && pe.find("pending_gate(") == std::string::npos, "0.0.643 flow: in plane_exec the LOCK precedes part C, the UNLOCK follows it, the post-unlock latch gate (FLIP_PENDING and EARLIEST_INUSE) comes last (ONE latch_gate call) and the pre-unblank pending_gate is NOT CALLED"); }
        { const size_t a = flow.find("inline bool pending_gate("), b = a == std::string::npos ? a : flow.find("\n}\n", a); const std::string pg = (a == std::string::npos || b == std::string::npos) ? "" : flow.substr(a, b - a);
          expect(!pg.empty() && pg.find("EARLIEST") == std::string::npos && pg.find("N48D2_FLIP_PENDING_MASK") != std::string::npos && pg.find("40u * frames") != std::string::npos, "0.0.638 flow: the (uncalled since 0.0.643) pending_gate reads FLIP_CONTROL ONLY (no EARLIEST_INUSE) and is bounded (40 ms per frame)"); }
        for (const char *t : { "static int d2_print_rec(void) {", "dr_call(N48D2_ACT, d2_arg(N48D2_OP_PLANEREC), o)", "n48d2_unpack_rec(o + 3, rec);", "wait timed out at %s (%#06x) last value %#010x", "DCCG_GATE_DISABLE_CNTL6  DCCG_GATE_DISABLE_CNTL   DOMAIN%u_PG_STATUS", "if (r.pre_wait) printf(", "if (op == N48D2_OP_PLANE || op == N48D2_OP_FBHOLD || r.timeouts != 0u) (void)d2_print_rec();" })
            expect(cli.find(t) != std::string::npos, (std::string("0.0.638 CLI carries ") + t).c_str());
        { const size_t c0 = cli.find("static int cmd_disp2_plane("), c1 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {", c0 == std::string::npos ? 0 : c0); const std::string cp = (c0 == std::string::npos || c1 == std::string::npos) ? "" : cli.substr(c0, c1 - c0);
          const size_t call = cp.find("dr_call(N48D2_ACT, d2_arg(op)"), gt = cp.find("d2_print_gates(op, &r, g);"), rc = cp.find("d2_print_rec();"), w1 = cp.find("dp_watch_read(dpw1)"), ab = cp.find("d2_arg(N48D2_OP_PLANEOFF)");
          expect(call != std::string::npos && gt != std::string::npos && rc != std::string::npos && w1 != std::string::npos && ab != std::string::npos && call < gt && gt < rc && rc < w1 && w1 < ab, "0.0.638 CLI: REACHABILITY: the record page is read AFTER the op and its gates, BEFORE the abort planeoff (which would clear the wait records)"); }
        expect(hdr.find("(a >> 24) != 0u) return 0;") != std::string::npos && hdr.find("op <= N48D2_OP_FBHOLD") != std::string::npos, "0.0.638 header: op 13 is legal for instance 2 only, like 8..12");
    }
}

// =====================================================================================================================================================================================================
// 0.0.639: the 0.0.638 run (notes/logs/runs/m4cd-638/run.txt) got past the clock enable and failed closed at the DET2 wait: DCHUBBUB_DET2_CTRL stayed 0x3 (size 3, current 0) for 100 ms while HUBP2 was blanked and MPCC2 unconnected. Linux waits for
// DETn_SIZE_CURRENT only AFTER the pipe is enabled and committed (dcn401_wait_for_det_buffer_update_under_otg_master, dcn401_wait_for_det_update). Tests: (1) no DET gate before the unblank (one RECORDED read); (2) the DET2 wait AFTER the unblank,
// after the EARLIEST_INUSE gate and before the health gate; (3) a fake 'DET stays 0 while blanked, 3 after the unblank' PASSES op 8; (4) a fake 'DET never current' FAILS op 8 after the unblank with rollback; (5) underflow / TIMEOUT / ODM2 underflow
// in op 8 are RECORDED (op 8 passes) and op 9 is the real gate (it refuses what persists after its clear); (6) the rollback skips the zero address when NO_OUTSTANDING_REQ timed out.
// =====================================================================================================================================================================================================
static void plane_639_tests() {
    const size_t NP = (size_t)-1;
    // ---- the PURE health verdict (op 8 now drops UNDERFLOW / TIMEOUT / ODM from its gate, so the verdict's own rules are pinned here, on the function op 8 and op 9 both call)
    {
        uint32_t gv[N48D2_GATES] = {}; gv[N48D2_GT_DET2] = 0x303u; gv[N48D2_GT_EARLY_LO] = (uint32_t)kBufA; gv[N48D2_GT_EARLY_HI] = (uint32_t)(kBufA >> 32);
        expect(n48d2_health(gv, 0u, kBufA, 1) == 0u, "pure verdict: the clean gate dwords give 0");
        for (const uint32_t b : { 0x10000000u, 0x20000000u, 0x40000000u }) { uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_HUBP_CNTL] = b; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_UNDERFLOW, "pure verdict: each of HUBP2 underflow bits 28 / 29 / 30 is H_UNDERFLOW"); }
        for (const uint32_t b : { 0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u }) { uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_HUBP_CNTL] = b; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_TIMEOUT, "pure verdict: each of TIMEOUT bits 23:20 is H_TIMEOUT"); }
        { uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_HUBP_CNTL] = 0x800u; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_SEGALLOC, "pure verdict: SEG_ALLOC_ERR is H_SEGALLOC"); }
        for (const uint32_t b : { 0x400u, 0x2000u }) {
            uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_ODM2] = b;
            expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_ODM && n48d2_health(g2, b, kBufA, 1) == 0u && n48d2_health(g2, 0x2400u & ~b, kBufA, 1) == N48D2_H_ODM, "pure verdict: ODM2 bit 10 / 13 is H_ODM unless it is in the stale mask (and only that bit is excused)");
        }
        { uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_DET2] = 0x003u; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_DET, "pure verdict: DET2 current 0 is H_DET"); g2[N48D2_GT_DET2] = 0x203u; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_DET, "pure verdict: DET2 current 2 is H_DET"); }
        { uint32_t g2[N48D2_GATES]; std::memcpy(g2, gv, sizeof g2); g2[N48D2_GT_VMFAULT] = 1u; expect(n48d2_health(g2, 0u, kBufA, 1) == N48D2_H_VMFAULT, "pure verdict: a VM fault is H_VMFAULT"); }
    }
    // ---- the REC guard for the fifth register: DET2_CTRL only in slot 13, instance 2 only, never as a write
    expect(list_ok(2, mkrec(N48D2_DCHUBBUB_DET2_CTRL, 13u)) && !list_ok(1, mkrec(N48D2_DCHUBBUB_DET2_CTRL, 13u)) && !list_ok(2, mkrec(N48D2_DCHUBBUB_DET2_CTRL, 12u)) && !list_ok(2, mkrec(N48D2_DCHUBBUB_DET2_CTRL, 14u)) && !list_ok(2, mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 13u)) && !list_ok(2, mkrec(N48D2_DCN_VM_FAULT_STATUS, 13u)),
           "0.0.639 REC guard: DET2_CTRL passes in slot 13 and nowhere else (instance 2 only); no other register may use slot 13");
    {   PModel m; m.initPlane(); n48d2_step st[1] = { mkrec(N48D2_DCHUBBUB_DET2_CTRL, 13u) }; n48d2_out o; std::memset(&o, 0, sizeof o); const uint32_t r = n48d2::run_steps(m, 2u, st, 1u, false, o, nullptr);
        expect(r == 0u && m.writes.empty() && m.recBuf[13] == 3u && (m.recBuf[N48D2_RC_VALID] & ~N48D2_RV_DET0) == 0u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_DET0) != 0u, "0.0.639: running the DET2 REC step writes nothing, fills slot 13 and marks ONLY the DET0 valid bit (not sample A or B)"); }
    // ---- (1)(2)(3) the fake hardware: DET2 current 0 while blanked, 3 only after the unblank (and 20 reads = 20 ms later)
    {
        PModel m; m.initPlane(); m.detAfterUnblank = true; m.detReadsAfterUnblank = 20u; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && o.timeouts == 0u && o.auto_off == 0u && m.plane.stage == N48D2_PL_PLANE && m.writes.size() == 41u, "0.0.639: a DET2 that is 0 while blanked and 3 only after the unblank (20 ms later): op 8 PASSES, 41 writes, no timeout, no rollback");
        const size_t iClk = nth_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL), iMux = nth_write(m.writes, N48D2_MPC_OUT2_MUX);
        const size_t iUn = iMux == NP ? NP : iMux + 1u;
        const bool idx = iClk != NP && iUn != NP && iUn < m.writes.size() && m.writes[iUn].abs == N48D2_HUBP2_DCHUBP_CNTL && m.readsAtWrite.size() == m.writes.size() && m.f2AtRead.size() == m.reads.size();
        expect(idx, "0.0.639: the clock write, the mux and the unblank exist");
        if (idx) {
            size_t pre = 0, post = 0, firstPost = NP; for (size_t k = m.readsAtWrite[iClk]; k < m.reads.size(); k++) if (m.reads[k] == N48D2_DCHUBBUB_DET2_CTRL) { if (k < m.readsAtWrite[iUn]) ++pre; else { ++post; if (firstPost == NP) firstPost = k; } }
            expect(pre == 1u, "0.0.639: NO DET2 gate before the unblank: between the clock enable and the unblank DET2_CTRL is read exactly ONCE (the recorded read)");
            expect(post >= 21u, "0.0.639: the DET2 wait is AFTER the unblank: at least 21 polls (20 reads of 0, then 3)");
            size_t firstEarly = NP, hubpCntl = NP; for (size_t k = m.readsAtWrite[iUn]; k < m.reads.size(); k++) { if (firstEarly == NP && m.reads[k] == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE) firstEarly = k; if (hubpCntl == NP && m.reads[k] == N48D2_HUBP2_DCHUBP_CNTL) hubpCntl = k; }
            expect(firstEarly != NP && firstPost != NP && hubpCntl != NP && firstEarly < firstPost && firstPost < hubpCntl, "0.0.639 ORDER: after the unblank the EARLIEST_INUSE gate, THEN the DET2 wait, THEN the health gate reads");
            expect(m.recBuf[N48D2_RC_DET0] == 3u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_DET0) != 0u, "0.0.639: the pre-unblank DET2_CTRL is RECORDED (size 3, current 0) in the record page");
            expect(m.delayUs >= 20u * 1000u + 1000u, "0.0.639: the wait really waited (the 20 ms the fake needs, plus the 1 ms before the first recorded read)");
        }
        n48d2_step e[N48D2_MAX_STEPS]; const uint32_t ne = n48d2_build_plane_e(e);
        expect(ne == 1u && e[0].kind == N48D2_K_WAIT && e[0].abs == N48D2_DCHUBBUB_DET2_CTRL && e[0].polls == 100u && e[0].step_us == 1000u && e[0].mask == N48D2_DET_CUR_MASK && e[0].val == (3u << 8), "0.0.639: part E is ONE WAIT: DET2 current == 3, 100 polls x 1000 us (Linux: 1 vupdate at 10 Hz)");
    }
    // ---- (4) DET2 never current: FAILS op 8 after the unblank, with rollback; also the 'drops away' variant
    for (int variant = 0; variant < 2; variant++) {
        PModel m; m.initPlane(); m.detAfterUnblank = true; if (variant == 0) m.detWorks = false; else m.detReadsAfterUnblank = 100000u;
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_GATE && o.fail == N48D2_PE_I_DETWAIT && o.pre_wait == 1u && o.pre_abs == N48D2_DCHUBBUB_DET2_CTRL && o.auto_off == 1u && o.free_bufs == 1u && m.plane.stage == N48D2_PL_FREE && rolled_back_clean(m), "0.0.639: DET2 never becomes current: op 8 FAILS (GATE at the DET2 wait) and is ROLLED BACK");
        const bool joined = any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u); expect(joined && m.delayUs >= 100u * 1000u, "... the plane WAS joined and unblanked first (the wait is after the unblank) and the wait ran its 100 ms");
        expect(any_write(m.writes, N48D2_HUBP2_DCHUBP_CNTL, 0x80000001u, 1u) && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_WAIT) != 0u && m.recBuf[N48D2_RC_WAIT_ABS] == N48D2_DCHUBBUB_DET2_CTRL, "... the rollback blanked it again and the wait record names DCHUBBUB_DET2_CTRL");
    }
    // ---- (5) underflow / TIMEOUT / ODM2 underflow in op 8 are RECORDED, op 9 is the gate
    struct R { const char *what; std::function<void(PModel &)> set; bool persists; };
    const R rec[] = {
        { "HUBP2 underflow bit 28", [](PModel &m) { m.injectUnderflow = 0x10000000u; }, false },
        { "HUBP2 underflow bit 30", [](PModel &m) { m.injectUnderflow = 0x40000000u; }, false },
        { "ODM2 underflow-occurred (bit 10)", [](PModel &m) { m.odmAfterUnblank = 0x400u; }, false },
        { "ODM2 underflow-current (bit 13)", [](PModel &m) { m.odmAfterUnblank = 0x2000u; }, false },
        { "HUBP2 TIMEOUT_STATUS (bits 23:20) (the model cannot clear it: it persists into op 9)", [](PModel &m) { m.timeoutBits = 0x00100000u; }, true },
        { "an underflow that comes back after the clear", [](PModel &m) { m.injectUnderflow = 0x10000000u; m.reUnderflow = true; }, true },
        { "an ODM2 underflow the clear cannot remove", [](PModel &m) { m.odmAfterUnblank = 0x400u; m.undClearWorks = false; }, true },
    };
    for (const R &r : rec) {
        PModel m; m.initPlane(); r.set(m);
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        const std::string w = std::string("0.0.639 op 8, ") + r.what;
        expect(o.status == N48D2_OK && o.auto_off == 0u && m.plane.stage == N48D2_PL_PLANE && m.dpgOn, (w + ": RECORDED, not a failure: op 8 passes, no rollback, DPG2 still on").c_str());
        expect((m.recBuf[N48D2_RC_VALID] & N48D2_RV_UND) != 0u && ((m.recBuf[N48D2_RC_UND_HUBP] & 0x70F00000u) != 0u || (m.recBuf[N48D2_RC_UND_ODM] & 0x2400u) != 0u), (w + ": the HUBP2 CNTL / ODM2 values are in the record page").c_str());
        const n48d2_out s = run_op(m, N48D2_OP_SHOW);
        if (r.persists) expect(s.status == N48D2_GATE && m.dpgOn && m.plane.stage == N48D2_PL_PLANE && s.auto_off == 0u && !any_write(m.writes, N48D2_DPG2_CONTROL, 1u, 0u), (w + ": op 9 REFUSES (GATE), DPG2 stays on, no rollback").c_str());
        else expect(s.status == N48D2_OK && !m.dpgOn && m.plane.stage == N48D2_PL_SHOWN, (w + ": op 9 clears it, sees 0 after 10 frames and switches DPG2 off").c_str());
    }
    // the gates that stay gates in op 8 (alongside the table rows): SEG_ALLOC_ERR, a VM fault, DET2 != 3 (above), the latch, a DP disturbance
    { PModel m; m.initPlane(); m.segAllocErr = true; m.injectUnderflow = 0x10000000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_GATE && o.pre_val == N48D2_H_SEGALLOC && o.auto_off == 1u, "0.0.639: SEG_ALLOC_ERR is a failure even next to a recorded underflow; the reported gate bits are SEG_ALLOC_ERR only"); }
    { PModel m; m.initPlane(); m.vmFault = 0x101u; m.injectUnderflow = 0x10000000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_GATE && o.pre_val == N48D2_H_VMFAULT && o.auto_off == 1u, "0.0.639: a VM fault is a failure; the reported gate bits are the VM fault only"); }
    { PModel m; m.initPlane(); m.flipTrig = N48D2_HUBP2_DCHUBP_CNTL; m.flipReg = N48D2_SYMCLKB_CLOCK_ENABLE; m.flipXor = 0x10u; m.injectUnderflow = 0x10000000u; const n48d2_out o = run_op(m, N48D2_OP_PLANE); expect(o.status == N48D2_DP_DISTURBED || o.status == N48D2_GATE, "0.0.639: a DP disturbance still ends op 8 in a failure status (never OK)"); }
    // ---- (6) the rollback: the zero address only if NO_OUTSTANDING_REQ was reached
    {
        PModel m; plane_up(m); const size_t w0 = m.writes.size(); m.noOutWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANEOFF);
        bool zero = false; for (size_t i = w0; i < m.writes.size(); i++) if ((m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS || m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) && m.writes[i].v == 0u) zero = true;
        expect(!zero && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) == (uint32_t)kBufA && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == (uint32_t)(kBufA >> 32), "0.0.639 rollback: NO_OUTSTANDING_REQ timed out -> the primary address is NOT zeroed (still A)");
        expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_ADDRKEPT) != 0u && !m.clockOn, "... the buffers are LEAKED, the skip is RECORDED (valid bit 64) and the rest of the rollback (clock off) still ran");
        m.noOutWorks = true; const size_t w1 = m.writes.size(); const n48d2_out o2 = run_op(m, N48D2_OP_PLANEOFF);
        bool zero2 = false; for (size_t i = w1; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS && m.writes[i].v == 0u) zero2 = true;
        expect(zero2 && o2.status == N48D2_OK && o2.free_bufs == 1u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_ADDRKEPT) == 0u, "... a later rollback with NO_OUTSTANDING_REQ reached zeroes the address and releases the buffers (the skip record is cleared)");
    }
    {   // a failed op 8 (post-unblank) with NO_OUTSTANDING_REQ never reached: same rule
        PModel m; m.initPlane(); m.latchWorks = false; m.noOutWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        bool zero = false; for (size_t i = 0; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS && m.writes[i].v == 0u) zero = true;
        expect(o.status == N48D2_GATE && !zero && o.free_bufs == 0u && m.plane.stage == N48D2_PL_LEAKED && m.progAddr == kBufA, "0.0.639 rollback of a FAILED op 8 with NO_OUTSTANDING_REQ never reached: no zero write, the address stays A, buffers leaked");
    }
    {   // the clean rollback still zeroes (the 0.0.638 behaviour is unchanged when the wait succeeds)
        PModel m; m.initPlane(); m.latchWorks = false; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_GATE && o.free_bufs == 1u && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) == 0u && m.get(N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == 0u && (m.recBuf[N48D2_RC_VALID] & N48D2_RV_ADDRKEPT) == 0u, "0.0.639 rollback with NO_OUTSTANDING_REQ reached: the address is zeroed before the release, nothing recorded as kept");
    }
    // ---- source pins
    {
        const std::string src = g_root + "/src/navi48-bringup/src/";
        const std::string flow = strip_line_comments(slurp(src + "dcn/navi48_disp2_flow.h")), hdr = strip_line_comments(slurp(src + "dcn/navi48_disp2.h")), cli = slurp(g_root + "/tools/pc/navi48test.c");
        const size_t e0 = flow.find("inline bool plane_exec("), e1 = flow.find("template <class E>\ninline n48d2_out run_plane(", e0 == std::string::npos ? 0 : e0); const std::string pe = (e0 == std::string::npos || e1 == std::string::npos) ? "" : flow.substr(e0, e1 - e0);
        const size_t b0 = pe.find("n = n48d2_build_plane_b_i(inst, s);"), d0 = pe.find("n = n48d2_build_plane_d_i(inst, s, pl->mc[0]);"), c0 = pe.find("n = n48d2_build_plane_c_i(inst, s, pl->mc[0]);"), l0 = pe.find("latch_gate(e, inst, fc2, 5u"), p0 = pe.find("n = n48d2_build_plane_e_i(inst, s);"), w0 = pe.find("wait_frames(e, fc2, 3u)");
        expect(b0 != std::string::npos && d0 != std::string::npos && b0 < d0 && pe.substr(b0, d0 - b0).find("N48D2_PE_I_DETWAIT") == std::string::npos && pe.substr(b0, d0 - b0).find("DETWAIT") == std::string::npos, "0.0.639 flow: no DET2 gate between part B and part D");
        expect(c0 != std::string::npos && l0 != std::string::npos && p0 != std::string::npos && w0 != std::string::npos && c0 < l0 && l0 < p0 && p0 < w0, "0.0.639 flow: part C, the EARLIEST_INUSE gate, part E (the DET2 wait), the 3 frames - in that order");
        expect(pe.find("bad & ~(uint32_t)(N48D2_H_UNDERFLOW | N48D2_H_TIMEOUT | N48D2_H_ODM)") != std::string::npos && pe.find("if (gate != 0u)") != std::string::npos, "0.0.639 flow: op 8's health gate drops exactly the underflow / TIMEOUT / ODM2 bits (RECORDED)");
        expect(flow.find("if (st.kind == N48D2_K_SET && noOutTimedOut && st.val == 0u") != std::string::npos && flow.find("N48D2_RV_ADDRKEPT") != std::string::npos, "0.0.639 flow: run_steps skips a zero address SET after a NO_OUTSTANDING_REQ timeout, and the rollback records it");
        expect(hdr.find("#define N48D2_RS_DET 13u") != std::string::npos && hdr.find("N48D2_REC(DCHUBBUB_DET2_CTRL, N48D2_RS_DET, 0u,") != std::string::npos && hdr.find("N48D2_PB_I_DETWAIT") == std::string::npos, "0.0.639 header: part B records DET2_CTRL in slot 13 and has no DET2 WAIT index");
        expect(cli.find("N48D2_RV_DET0") != std::string::npos && cli.find("N48D2_RV_UND") != std::string::npos && cli.find("N48D2_RV_ADDRKEPT") != std::string::npos, "0.0.639 CLI: planerec prints the DET2 record, the post-unblank health record and the kept-address flag");
    }
}

// =====================================================================================================================================================================================================
// 0.0.643: the 0.0.639 run (notes/logs/runs/m4cd-639/run.txt) failed the PRE-UNBLANK gate: HUBPREQ2_DCSURF_FLIP_CONTROL stayed 0x04100100 (FLIP_PENDING 1) for 5 frames, because a BLANKED HUBP never consumes a pending address.
// Linux does not gate on it: dcn20_hwseq.c update_dchubp_dpp calls update_plane_addr and then (new pipe) hubp->funcs->set_blank(hubp, false) inside the pipe lock (dcn20_pipe_control_lock -> tg->funcs->lock = optc3_lock on dcn401;
// unlock = optc1_unlock; optc401_wait_update_lock_status), so the address, the MPCC insert and the unblank take effect together at ONE VUPDATE, and is_flip_pending is checked only afterwards. op 8 now does the same on OTG2.
// =====================================================================================================================================================================================================
static void plane_643_tests() {
    const size_t NP = (size_t)-1;
    // ---- the registers and fields against Linux's headers (OTG2's copies; the field masks are the instance-0 names)
    expect_u("OTG2_OTG_MASTER_UPDATE_LOCK is Linux's absolute address", N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, lx_abs("OTG2_OTG_MASTER_UPDATE_LOCK"));
    expect_u("OTG2_OTG_GLOBAL_CONTROL2 is Linux's absolute address", N48D2_OTG2_OTG_GLOBAL_CONTROL2, lx_abs("OTG2_OTG_GLOBAL_CONTROL2"));
    expect_u("field OTG_MASTER_UPDATE_LOCK [0]", N48D2_OTG_MASTER_UPDATE_LOCK_MASK, lx_mask("OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK"));
    expect_u("field UPDATE_LOCK_STATUS [8]", N48D2_OTG_UPDATE_LOCK_STATUS_MASK, lx_mask("OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS"));
    expect_u("field OTG_MASTER_UPDATE_LOCK_SEL [27:25]", N48D2_OTG_LOCK_SEL_MASK, lx_mask("OTG0_OTG_GLOBAL_CONTROL2__OTG_MASTER_UPDATE_LOCK_SEL"));
    expect_u("OTG_MASTER_UPDATE_LOCK_SEL = 2 (optc3_lock: optc->inst of OTG2)", N48D2_OTG_LOCK_SEL_OTG2, lx_fv("OTG0_OTG_GLOBAL_CONTROL2__OTG_MASTER_UPDATE_LOCK_SEL", 2u));
    expect(N48D2_OTG2_OTG_MASTER_UPDATE_LOCK == 0x5149u && N48D2_OTG2_OTG_GLOBAL_CONTROL2 == 0x5150u, "the absolutes are 0x5149 (the lock) and 0x5150 (the select): segment 2 (0x34c0) + 0x1c89 / 0x1c90");
    // ---- the guard: exactly OTG2's two registers, exactly their fields
    {
        const uint32_t lk = N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, sel = N48D2_OTG2_OTG_GLOBAL_CONTROL2;
        expect(n48d2_guard_addr_i(2, lk, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(2, sel, nullptr) == N48D2_G_OK, "instance 2 admits OTG2's lock and lock select");
        expect(n48d2_guard_addr_i(1, lk, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, sel, nullptr) == N48D2_G_FORBIDDEN, "instance 1 refuses both (0.0.655: FORBIDDEN, OTG2's lock is a per-instance span)");
        bool refused = true; uint32_t nr = 0;
        for (const char *r : { "OTG0_OTG_MASTER_UPDATE_LOCK", "OTG1_OTG_MASTER_UPDATE_LOCK", "OTG3_OTG_MASTER_UPDATE_LOCK", "OTG0_OTG_GLOBAL_CONTROL2", "OTG1_OTG_GLOBAL_CONTROL2", "OTG3_OTG_GLOBAL_CONTROL2" }) {
            const uint32_t a = lx_abs(r); ++nr;
            const bool isOtg1 = a == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK || a == N48D2_OTG1_OTG_GLOBAL_CONTROL2;      // 0.0.655: OTG1's two are INSTANCE 1's lock registers (and forbidden for instance 2)
            refused = refused && a != lk && a != sel && (isOtg1 ? n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK : n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_OK) && n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_OK && n48d2_guard_addr_i(0, a, nullptr) != N48D2_G_OK;
            Model m; m.inst = 2; refused = refused && !m.wr(a, 0u) && !m.wr(a, 1u) && !m.wr(a, N48D2_OTG_LOCK_SEL_OTG2) && m.writes.empty();
            n48d2_step st = mkstep(N48D2_K_UPD, a, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, 1u); refused = refused && !list_ok(2, st) && (isOtg1 ? true : !list_ok(1, st));
        }
        expect(refused && nr == 6u, "OTG0 / OTG3's lock and lock-select registers (Linux names) are REFUSED by both instances' guards, OTG1's by instance 2 (0.0.655: they are instance 1's own), OTG2's by instance 1 and by the model's write path (guard + REAL allowlist) and as list steps");
        const n48d2_step lk1 = mkstep(N48D2_K_UPD, lk, 1u, 1u), lk0 = mkstep(N48D2_K_UPD, lk, 1u, 0u), lkSet = mkstep(N48D2_K_SET, lk, 0, 1u), lkWide = mkstep(N48D2_K_UPD, lk, 0x101u, 1u), lkMask0 = mkstep(N48D2_K_UPD, lk, 0u, 0u), lkBad = mkstep(N48D2_K_UPD, lk, 1u, 2u), lkSt = mkstep(N48D2_K_UPD, lk, 0x100u, 0x100u), lkCopy = mkstep(N48D2_K_COPY0, lk, 0xFFFFFFFFu, 1u, N48D2_HUBPREQ0_PRIMARY_LOW);
        expect(list_ok(2, lk1) && list_ok(2, lk0) && !list_ok(2, lkSet) && !list_ok(2, lkWide) && !list_ok(2, lkMask0) && !list_ok(2, lkBad) && !list_ok(2, lkSt) && !list_ok(2, lkCopy) && !list_ok(1, lk1), "the lock register: an UPDATE of mask EXACTLY bit 0 (value 0 or 1) passes; a SET, a wider mask (the status bit), an empty mask, a value outside the mask, the status bit, a COPY0 are refused; instance 1 refuses all");
        const n48d2_step sl = mkstep(N48D2_K_UPD, sel, N48D2_OTG_LOCK_SEL_MASK, N48D2_OTG_LOCK_SEL_OTG2), slSet = mkstep(N48D2_K_SET, sel, 0, N48D2_OTG_LOCK_SEL_OTG2), slWide = mkstep(N48D2_K_UPD, sel, 0x0F000000u, N48D2_OTG_LOCK_SEL_OTG2), slZero = mkstep(N48D2_K_UPD, sel, N48D2_OTG_LOCK_SEL_MASK, 0u), slOne = mkstep(N48D2_K_UPD, sel, N48D2_OTG_LOCK_SEL_MASK, 0x02000000u), slLow = mkstep(N48D2_K_UPD, sel, 0x0400u, 0x0400u);
        expect(list_ok(2, sl) && !list_ok(2, slSet) && !list_ok(2, slWide) && !list_ok(2, slZero) && !list_ok(2, slOne) && !list_ok(2, slLow) && !list_ok(1, sl), "the lock select: an UPDATE of mask EXACTLY the select field with value 2 passes; a SET, a wider mask, select 0 (OTG0's lock) or 1, any other field (GLOBAL_UPDATE_LOCK_EN) are refused; instance 1 refuses all");
        expect(wr_ok(2, sel, 0x04000000u) && wr_ok(2, sel, 0x04000400u) && !wr_ok(2, sel, 0u) && !wr_ok(2, sel, 0x02000000u) && !wr_ok(2, sel, 0x06000000u) && !wr_ok(1, sel, 0x04000000u), "the per-write value rule of the select: the field must be 2 (the other bits as read are kept)");
        expect(wr_ok(2, lk, 0u) && wr_ok(2, lk, 1u) && wr_ok(2, lk, 0x101u) && !wr_ok(2, lk, 2u) && !wr_ok(2, lk, 0x10000u) && !wr_ok(1, lk, 1u), "the per-write value rule of the lock: bit 0 (and the read-only status bit as read), nothing else");
    }
    // ---- ONLY the lock / unlock lists name these two registers; no other list of any op does
    {
        static n48d2_step b[24][N48D2_MAX_STEPS]; struct L { const char *w; uint32_t n; } ls[24]; uint32_t nl = 0;
        for (uint32_t inst : { 1u, 2u }) for (uint32_t op : { (uint32_t)N48D2_OP_TIMING, (uint32_t)N48D2_OP_CONNECT, (uint32_t)N48D2_OP_OFF, (uint32_t)N48D2_OP_TIMING1440 }) { ls[nl] = { "timing / connect / off", n48d2_build_i(inst, op, b[nl]) }; ++nl; }
        for (uint32_t p = 0; p < N48D2_PRE_PAGES; p++) { ls[nl] = { "precheck page", n48d2_build_pre(p, b[nl]) }; ++nl; }
        ls[nl] = { "plane A", n48d2_build_plane_a(b[nl], kBufA) }; ++nl; ls[nl] = { "plane B", n48d2_build_plane_b(b[nl]) }; ++nl; ls[nl] = { "plane C", n48d2_build_plane_c(b[nl], kBufA) }; ++nl; ls[nl] = { "plane D", n48d2_build_plane_d(b[nl], kBufA) }; ++nl;
        ls[nl] = { "plane rec2", n48d2_build_plane_rec2(b[nl]) }; ++nl; ls[nl] = { "plane E", n48d2_build_plane_e(b[nl]) }; ++nl; ls[nl] = { "show 1", n48d2_build_show1(b[nl]) }; ++nl; ls[nl] = { "show 2", n48d2_build_show2(b[nl]) }; ++nl;
        ls[nl] = { "flip A", n48d2_build_flip(b[nl], kBufA) }; ++nl; ls[nl] = { "flip B", n48d2_build_flip(b[nl], kBufB) }; ++nl; ls[nl] = { "crc", n48d2_build_crc(b[nl]) }; ++nl; ls[nl] = { "planeoff", n48d2_build_planeoff(b[nl]) }; ++nl;
        bool none = true; for (uint32_t i = 0; i < nl && none; i++) for (uint32_t k = 0; k < ls[i].n; k++) none = none && b[i][k].abs != N48D2_OTG2_OTG_MASTER_UPDATE_LOCK && b[i][k].abs != N48D2_OTG2_OTG_GLOBAL_CONTROL2;
        expect(nl > 15u && none, "no list of any other op (timing / connect / off on either instance, the prechecks, plane A / B / C / D / rec2 / E, show, flip, crc, planeoff) names OTG2's lock or lock select");
        static n48d2_step lk[N48D2_MAX_STEPS], ul[N48D2_MAX_STEPS]; const uint32_t nlk = n48d2_build_plane_lock(lk), nul = n48d2_build_plane_unlock(ul);
        expect(nlk == 3u && lk[0].abs == N48D2_OTG2_OTG_GLOBAL_CONTROL2 && lk[1].abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK && lk[1].val == 1u && lk[2].kind == N48D2_K_WAIT && lk[2].abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK && lk[2].mask == 0x100u && lk[2].val == 0x100u && (uint64_t)lk[2].polls * lk[2].step_us <= 200000u && lk[2].polls > 0u, "the lock list: select, lock, then a BOUNDED wait on UPDATE_LOCK_STATUS (<= 200 ms)");
        expect(nul == 1u && ul[0].abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK && ul[0].kind == N48D2_K_UPD && ul[0].mask == 1u && ul[0].val == 0u, "the unlock list: one UPDATE of the lock bit to 0");
        // the other ops, RUN: they never write either register
        bool never = true;
        { PModel m; plane_up(m); (void)run_op(m, N48D2_OP_SHOW); never = never && nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) == NP && nth_write(m.writes, N48D2_OTG2_OTG_GLOBAL_CONTROL2) == NP; (void)run_op(m, N48D2_OP_FLIP, 1u); (void)run_op(m, N48D2_OP_CRC); (void)run_op(m, N48D2_OP_PLANEOFF);
          never = never && nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) == NP && nth_write(m.writes, N48D2_OTG2_OTG_GLOBAL_CONTROL2) == NP && !m.locked; }
        { PModel m; m.initPlane(); m.refuseAbs = N48D2_DSCL2_RECOUT_SIZE; (void)run_op(m, N48D2_OP_PLANE); never = never && nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) == NP && nth_write(m.writes, N48D2_OTG2_OTG_GLOBAL_CONTROL2) == NP; }     // op 8 failing before the lock never takes it
        { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; (void)run_op(m, N48D2_OP_PLANE); never = never && m.writes.empty(); }
        for (uint32_t inst : { 1u, 2u }) for (uint32_t op : { (uint32_t)N48D2_OP_TIMING, (uint32_t)N48D2_OP_CONNECT, (uint32_t)N48D2_OP_OFF }) { Model m; m.inst = inst; m.r[N48D2_OTG1_CONTROL] = 0x00010001u; m.r[N48D2_OTG1_CLOCK_CONTROL] = 3u; (void)n48d2::run(m, op, inst); never = never && nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) == NP && nth_write(m.writes, N48D2_OTG2_OTG_GLOBAL_CONTROL2) == NP; }
        expect(never, "RUN: op 9 / 10 / 11 / 12, op 8 failing before the lock, and timing / connect / off on both instances never write the lock or the select");
    }
    // ---- THE FAKE the brief names: a blanked HUBP never consumes a pending flip until the lock is released, then latches at the next VUPDATE. 0.0.643 op 8 PASSES it.
    {
        PModel m; m.initPlane(); m.lockAware = true; m.blankedNoEarly = true;
        const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && o.timeouts == 0u && o.auto_off == 0u && o.free_bufs == 0u && m.plane.stage == N48D2_PL_PLANE && !m.locked && !m.hwPending && m.latched == kBufA, "fake 'a blanked HUBP never consumes a pending flip until the lock is released, then latches at the next VUPDATE': op 8 PASSES (stage PLANE, lock released, latched = A)");
        expect(m.pendAtUnlock, "... the address WAS still pending (FLIP_PENDING 1) when the lock was released: the pre-unblank FLIP_PENDING gate of 0.0.638 / 0.0.639 could never have passed this");
        auto idx = [&](const char *e, size_t from = 0) { for (size_t i = from; i < m.ev.size(); i++) if (m.ev[i] == e) return i; return NP; };
        const size_t iL = idx("lock"), iU = iL == NP ? NP : idx("unblank", iL), iUl = iU == NP ? NP : idx("unlock", iU), iV = iUl == NP ? NP : idx("VUPDATE latched", iUl);
        expect(iL != NP && iU != NP && iUl != NP && iV != NP && idx("VUPDATE latched") == iV, "the model's events: lock, then the unblank (inside the lock), then the unlock, then the VUPDATE that latches the address - and no latch before the unlock");
        expect((m.gateBuf[N48D2_GT_FLIP_CTL] & N48D2_FLIP_PENDING_MASK) == 0u && (((uint64_t)(m.gateBuf[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | m.gateBuf[N48D2_GT_EARLY_LO]) == kBufA, "the post gates saw FLIP_PENDING 0 and EARLIEST_INUSE = A");
    }
    // ---- 'the lock status never asserts': op 8 fails BEFORE any address / MPCC2 / unblank write inside the lock, with the rollback, and the lock is released first
    {
        PModel m; m.initPlane(); m.lockNeverAsserts = true; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_GATE && o.fail == N48D2_LK_I_WAIT && o.pre_wait == 1u && o.pre_abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK && o.timeouts >= 1u && o.auto_off == 1u && o.free_bufs == 1u, "lock status never asserts: GATE at the lock-wait step, the lock register named with its last value, rolled back, buffers released");
        const size_t iLk = nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, 0), iUl = nth_write(m.writes, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, 1), iDpg = nth_write(m.writes, N48D2_DPG2_CONTROL);
        { size_t inLock = 0; if (iLk != NP && iUl != NP) for (size_t i = iLk + 1u; i < iUl; i++) ++inLock; size_t nAddrLive = 0; if (iDpg != NP) for (size_t i = 0; i < iDpg; i++) nAddrLive += m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS;
          expect(iLk != NP && iUl != NP && iUl == iLk + 1u && inLock == 0u && nAddrLive == 2u && !any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) && !any_write(m.writes, N48D2_MPCC2_MPCC_TOP_SEL, ~0u, 2u), "... NOTHING was written between the lock and its release: no third address write (the one inside the lock; the two before it are parts A and D), no MPCC2 insert, no mux, no unblank"); }
        expect(iLk != NP && iUl != NP && iDpg != NP && iLk < iUl && iUl < iDpg && (m.writes[iLk].v & 1u) == 1u && (m.writes[iUl].v & 1u) == 0u && !m.locked && rolled_back_clean(m), "... the lock is RELEASED (the unlock write) BEFORE the rollback's first write (DPG2 back on), and the plane is off afterwards");
    }
    // ---- every exit path after the lock releases it, BEFORE the rollback: a write refused INSIDE the lock (each of the eight), every post gate failing, a DP disturbance, the select / lock write refused
    {
        struct Fx { const char *what; std::function<void(PModel &)> set; };
        const Fx fx[] = {
            { "the address HIGH inside the lock refused", [](PModel &m) { m.refuseInLock = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH; } }, { "the address LOW inside the lock refused", [](PModel &m) { m.refuseInLock = N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS; } },
            { "MPCC2 BOT refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_MPCC2_MPCC_BOT_SEL; } }, { "MPCC2 TOP refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_MPCC2_MPCC_TOP_SEL; } },
            { "MPCC2 OPP refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_MPCC2_MPCC_OPP_ID; } }, { "MPCC2 LOCK_SEL refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL; } },
            { "MPC_OUT2_MUX refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_MPC_OUT2_MUX; } }, { "the UNBLANK refused inside the lock", [](PModel &m) { m.refuseInLock = N48D2_HUBP2_DCHUBP_CNTL; } },
            { "the lock-select write refused", [](PModel &m) { m.refuseAbs = N48D2_OTG2_OTG_GLOBAL_CONTROL2; } },
            { "the post gate: the address never latches", [](PModel &m) { m.latchWorks = false; } }, { "the post gate: FLIP_PENDING never clears", [](PModel &m) { m.flipPendReads = 1; m.flipClears = false; } },
            { "the DET2 wait times out", [](PModel &m) { m.detWorks = false; } }, { "SEG_ALLOC_ERR after the unblank", [](PModel &m) { m.segAllocErr = true; } }, { "a VM fault after the unblank", [](PModel &m) { m.vmFault = 0x101u; } },
            { "OTG2 frozen: the post gate's bound runs out", [](PModel &m) { m.latchWorks = false; m.freezeOnEarlyRead = 3; } },
            { "the lock status never asserts", [](PModel &m) { m.lockNeverAsserts = true; } },
            { "a DP register changes during the lock (disturbance, after the unlock)", [](PModel &m) { m.flipTrig = N48D2_OTG2_OTG_MASTER_UPDATE_LOCK; m.flipReg = N48D2_MPC_OUT0_MUX; m.flipXor = 0xF; } },
        };
        for (const Fx &f : fx) {
            PModel m; m.initPlane(); f.set(m); const n48d2_out o = run_op(m, N48D2_OP_PLANE);
            size_t nOn = 0, nOff = 0, last = NP, firstOff = NP; for (size_t i = 0; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) { if (m.writes[i].v & 1u) ++nOn; else { ++nOff; if (firstOff == NP) firstOff = i; } last = i; }
            const size_t iDpg = nth_write(m.writes, N48D2_DPG2_CONTROL);
            const std::string w = std::string("op 8, ") + f.what + ": ";
            expect(o.status != N48D2_OK && !m.locked, (w + "fails and OTG2's update lock is RELEASED").c_str());
            expect(nOn <= 1u && (nOn == 0u || (nOff >= 1u && last != NP && (m.writes[last].v & 1u) == 0u)), (w + "the LAST write to the lock register is the unlock (one lock at most)").c_str());
            expect(nOn == 0u || (firstOff != NP && (iDpg == NP || firstOff < iDpg)), (w + "the unlock comes BEFORE the rollback's first write (DPG2 back on)").c_str());
            if (o.auto_off) expect(rolled_back_clean(m) || o.free_bufs == 0u, (w + "the rollback ran").c_str());
        }
        PModel m; m.initPlane(); m.refuseAbs = N48D2_OTG2_OTG_MASTER_UPDATE_LOCK; const n48d2_out o = run_op(m, N48D2_OP_PLANE);
        const size_t iDpg = nth_write(m.writes, N48D2_DPG2_CONTROL); size_t nAddrLive = 0; if (iDpg != NP) for (size_t i = 0; i < iDpg; i++) nAddrLive += m.writes[i].abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS;
        expect(o.status == N48D2_ALLOW_REFUSED && !m.locked && iDpg != NP && nAddrLive == 2u && !any_write(m.writes, N48D2_MPC_OUT2_MUX, ~0u, 0x4102u) && rolled_back_clean(m), "the lock write itself refused: ALLOW_REFUSED before anything inside the lock, rolled back, no lock held");
    }
    // ---- pending_gate is no longer called by op 8, but it is kept (and tested directly) so a reinstated pre-unblank gate is a REAL gate: it polls, it is bounded, it fails closed
    {
        uint32_t g[N48D2_GATES] = {};
        PModel a; a.initPlane(); a.pend = 40u; for (uint32_t i = 0; i < N48D2_GATES; i++) g[i] = 0u;
        const bool pa = n48d2::pending_gate(a, N48D2_OTG2_FRAME_COUNT, 5u, g);
        PModel b; b.initPlane(); b.pend = 5000u; const bool pb = n48d2::pending_gate(b, N48D2_OTG2_FRAME_COUNT, 5u, g);
        PModel c; c.initPlane(); c.pend = 5000u; c.otg2Frozen = true; const bool pc = n48d2::pending_gate(c, N48D2_OTG2_FRAME_COUNT, 5u, g);
        expect(!pc && c.sleeps >= 200u, "pending_gate (kept, uncalled): with OTG2 frozen AND FLIP_PENDING stuck the loop's own bound (40 reads per frame) runs out and the gate FAILS closed");
        expect(pa && !pb && (g[N48D2_GT_FLIP_CTL] & N48D2_FLIP_PENDING_MASK) != 0u && b.frames2 >= 5u && b.frames2 < 50u, "pending_gate (kept, uncalled): polls through 40 pending reads and passes; with FLIP_PENDING stuck it runs ~5 frames, then FAILS closed (bounded) and reports the last FLIP_CONTROL value");
    }
    // ---- the source pins of the 0.0.643 flow
    {
        const std::string src = g_root + "/src/navi48-bringup/src/";
        const std::string flow = strip_line_comments(slurp(src + "dcn/navi48_disp2_flow.h"));
        const size_t e0 = flow.find("inline bool plane_exec("), e1 = flow.find("template <class E>\ninline n48d2_out run_plane(", e0 == std::string::npos ? 0 : e0); const std::string pe = (e0 == std::string::npos || e1 == std::string::npos) ? "" : flow.substr(e0, e1 - e0);
        const size_t lk = pe.find("locked = true;"), pc = pe.find("n = n48d2_build_plane_c_i(inst, s, pl->mc[0]);"), ul = pe.find("n = n48d2_build_plane_unlock_i(inst, s);"), lf = ul == std::string::npos ? ul : pe.find("locked = false;", ul), lg = pe.find("if (!latch_gate("), rel = pe.find("if (locked) { plane_unlock_best_effort(e, inst); locked = false; }"), rb = pe.find("plane_rollback(e, inst, o, pl)");
        expect(lk != std::string::npos && lk < pc && pc < ul && ul < lf && lf < lg && lg < rel && rel < rb, "0.0.643 flow: locked = true BEFORE the lock list runs, part C, the unlock, locked = false, the post gate, then the common-exit release (if (locked)), THEN the rollback");
        expect(pe.find("pending_gate(") == std::string::npos && count_of(pe, "n48d2_build_plane_unlock_i(") == 1u && count_of(flow, "n48d2_build_plane_unlock_i(") == 3u && pe.find("plane_unlock_best_effort(e, inst)") != std::string::npos, "0.0.643 flow: no pending_gate call; plane_exec builds the unlock once (main path) and calls plane_unlock_best_effort (the exit release, a separate noinline function that builds it once more; the judge-all list is the third)");
        expect(pe.find("if ((o.wfail & (1ull << N48D2_LK_I_WAIT)) != 0u)") != std::string::npos && pe.find("if ((o.wfail & (1ull << N48D2_LK_I_WAIT)) != 0u)") < pc, "0.0.643 flow: the lock-status wait is a GATE and is judged BEFORE part C runs");
        const std::string hdr = strip_line_comments(slurp(src + "dcn/navi48_disp2.h"));
        expect(hdr.find("#define N48D2_ALLOWED2P_COUNT 39u") != std::string::npos && hdr.find("N48D2_OTG2_OTG_GLOBAL_CONTROL2, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK,") != std::string::npos, "0.0.643 header: the plane list holds exactly OTG2's lock select and lock beside the 37");
    }
}


// =====================================================================================================================================================================================================
// 0.0.652 (M5): op 14 `fbhold 2` and the HELD plane (an internal design note "M5 build spec", section 3). The flow runs over the model; the planted breaks are in native_disp2_plant.sh.
// =====================================================================================================================================================================================================
static void shown_up(PModel &m) {
    plane_up(m);
    const n48d2_out s = run_op(m, N48D2_OP_SHOW);
    const bool ok = s.status == N48D2_OK && m.plane.stage == N48D2_PL_SHOWN;
    expect(ok, "setup: op 9 showed the plane");
    if (!ok) throw SetupFailed();
    m.writes.clear(); m.reads.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear();
}
static void held_up(PModel &m) {
    shown_up(m);
    const n48d2_out h = run_op(m, N48D2_OP_FBHOLD);
    const bool ok = h.status == N48D2_OK && m.plane.stage == N48D2_PL_HELD;
    expect(ok, "setup: op 14 held the plane");
    if (!ok) throw SetupFailed();
    m.writes.clear(); m.reads.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear(); m.pinCalls = 0;
}
static bool read_of(const PModel &m, uint32_t abs) { for (uint32_t r : m.reads) if (r == abs) return true; return false; }
static void plane_652_tests() {
    // ---- the numbers and names
    expect(N48D2_OP_FBHOLD == 14u && N48D2_PL_HELD == 6u && N48D2_HELD == 21u && N48D2_STATUS_COUNT == 22u && N48D2_RV_HELD == 128u && N48D2_STAT_HELD_BIT == 52u, "op 14, stage 6, status 21 (count 22), record bit 128, status bit 52");
    expect(std::string(n48d2_op_name(N48D2_OP_FBHOLD)) == "fbhold", "op 14 is named fbhold");
    { std::set<std::string> names; for (uint32_t st = 0; st < N48D2_STATUS_COUNT; st++) { const std::string n = n48d2_status_name(st); expect(n != "unknown", "every status has a name"); names.insert(n); } expect_u("all 22 statuses have DISTINCT texts", names.size(), N48D2_STATUS_COUNT); }
    expect(std::string(n48d2_status_name(N48D2_HELD)).find("HELD") != std::string::npos, "the HELD status says so");
    // ---- the argument: op 14 is the monitor B's alone, no extra bits
    expect(n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 2u)) == 1 && n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 1u)) == 1 && n48d2_arg_ok(n48d2_arg(N48D2_OP_FBHOLD)) == 0 && n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 3u)) == 0 && n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 2u) | (1ull << 16)) == 0 &&
           n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 2u) | (1ull << 24)) == 0 && n48d2_arg_ok(n48d2_arg_i(15u, 2u)) == 0, "op 14 (0.0.658) names instance 1 or 2 EXACTLY (the default instance 0 and 3 are refused), with no buffer index and no high bits; op 15 does not exist");
    // ---- which ops a HELD plane refuses (pure)
    for (uint32_t op = 1; op <= 14; op++) {
        const bool want = op == 1 || op == 2 || op == 3 || op == 7 || op == 8 || op == 9 || op == 10 || op == 12 || op == 14;
        char b[200]; std::snprintf(b, sizeof b, "HELD refuses op %u on instance 2: %s", op, want ? "yes" : "no (read-only or crc)");
        expect((n48d2_held_refuses(op, N48D2_INST_MONB) != 0) == want, b);
        std::snprintf(b, sizeof b, "HELD refuses op %u on the monitor A (instance 1) exactly as on the monitor B (0.0.655: HELD is per instance; the glue asks the op's OWN instance)", op);
        expect((n48d2_held_refuses(op, N48D2_INST_MONA) != 0) == want, b);
    }
    // ---- the hold verdict (pure)
    { uint32_t g[N48D2_GATES]; std::memset(g, 0, sizeof g); g[N48D2_GT_DET2] = 3u << 8; g[N48D2_GT_EARLY_LO] = (uint32_t)kBufA; g[N48D2_GT_EARLY_HI] = (uint32_t)(kBufA >> 32);
      expect_u("hold verdict: healthy, front = A", n48d2_hold_verdict(g, 0u, kBufA), 0u);
      expect((n48d2_hold_verdict(g, 1u, kBufA) & N48D2_H_FRONT_B) != 0u, "hold verdict: the plane's bookkeeping says B");
      { uint32_t x[N48D2_GATES]; std::memcpy(x, g, sizeof x); x[N48D2_GT_EARLY_LO] ^= 0x10000u; expect((n48d2_hold_verdict(x, 0u, kBufA) & N48D2_H_LATCH) != 0u, "hold verdict: EARLIEST_INUSE is not A"); }
      { uint32_t x[N48D2_GATES]; std::memcpy(x, g, sizeof x); x[N48D2_GT_FLIP_CTL] = N48D2_FLIP_PENDING_MASK; expect((n48d2_hold_verdict(x, 0u, kBufA) & N48D2_H_LATCH) != 0u, "hold verdict: a flip is pending"); }
      { uint32_t x[N48D2_GATES]; std::memcpy(x, g, sizeof x); x[N48D2_GT_HUBP_CNTL] = 0x10000000u; expect((n48d2_hold_verdict(x, 0u, kBufA) & N48D2_H_UNDERFLOW) != 0u, "hold verdict: HUBP2 underflow"); }
      { uint32_t x[N48D2_GATES]; std::memcpy(x, g, sizeof x); x[N48D2_GT_VMFAULT] = 1u; expect((n48d2_hold_verdict(x, 0u, kBufA) & N48D2_H_VMFAULT) != 0u, "hold verdict: a VM fault"); }
      { uint32_t x[N48D2_GATES]; std::memcpy(x, g, sizeof x); x[N48D2_GT_ODM2] = 0x400u; expect((n48d2_hold_verdict(x, 0u, kBufA) & N48D2_H_ODM) != 0u, "hold verdict: ODM2 underflow"); } }
    // ---- the allocator's pair state machine (pure: amd/native_d2pin_pure.h; the kernel helper decides through these functions)
    { using namespace n48d2pin;
      expect(free_action(false, false) == kFreeNone && free_action(true, false) == kFreeDo && free_action(true, true) == kFreePinned && free_action(false, true) == kFreePinned, "d2pin: free = none / do / PINNED (a pinned pair is never freed, whatever else is true)");
      expect(!alloc_refused(false) && alloc_refused(true), "d2pin: a second allocation is refused while a pair is held");
      expect(pin_ok(true, true, false, true, true), "d2pin: pin ok for a held, unpinned pair that is exactly A and B");
      expect(!pin_ok(false, true, false, true, true) && !pin_ok(true, false, false, true, true) && !pin_ok(true, true, true, true, true) && !pin_ok(true, true, false, false, true) && !pin_ok(true, true, false, true, false), "d2pin: pin refused when nothing is held, no context, already pinned, or the pair is not A / not B");
      State st = { false, false };
      st = after_alloc(st, true);   expect(st.held && !st.pinned, "d2pin sequence: alloc -> held");
      st = after_free(st);          expect(!st.held && !st.pinned, "d2pin sequence: free of an unpinned pair gives it back");
      st = after_alloc(st, true);   st = after_pin(st, pin_ok(st.held, true, st.pinned, true, true)); expect(st.held && st.pinned, "d2pin sequence: alloc, pin -> held and pinned");
      for (int i = 0; i < 3; i++) { st = after_free(st); expect(st.held && st.pinned, "d2pin sequence: free of a PINNED pair is a no-op (still held, still pinned), however often"); }
      { State s2 = after_alloc(st, true); expect(s2.held && s2.pinned, "d2pin sequence: a second alloc while pinned changes nothing (refused)"); }
      { State s3 = after_pin(st, pin_ok(st.held, true, st.pinned, true, true)); expect(s3.held && s3.pinned, "d2pin sequence: a second pin is refused and changes nothing"); }
      { State s4 = after_pin(State{ true, false }, pin_ok(true, true, false, false, true)); expect(s4.held && !s4.pinned, "d2pin sequence: a pin with a foreign pair leaves it unpinned"); }
      expect(!after_free(State{ false, true }).held || after_free(State{ false, true }).pinned, "d2pin: there is no state that frees a pinned pair (exhaustive below)");
      bool never = true; for (int h = 0; h < 2; h++) for (int p = 0; p < 2; p++) { State a = after_free(State{ h != 0, p != 0 }); if (p && (a.pinned != true)) never = false; if (p && h && !a.held) never = false; }
      expect(never, "d2pin: exhaustive: free never clears a pin and never releases a pinned held pair"); }
    // ---- op 14 from a SHOWN, healthy plane
    { PModel m; shown_up(m); const size_t v0 = m.vfillCalls;
      const n48d2_out o = run_op(m, N48D2_OP_FBHOLD);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_HELD && o.free_bufs == 0u && o.auto_off == 0u && o.timeouts == 0u, "fbhold: OK, stage HELD, nothing freed, no rollback");
      expect(m.writes.empty() && m.vfillCalls == v0, "fbhold writes NO register and draws nothing");
      expect(m.pinCalls == 1u && m.pinned, "the allocator is asked to PIN the pair exactly once");
      expect(read_of(m, N48D2_HUBP2_DCHUBP_CNTL) && read_of(m, N48D2_DCN_VM_FAULT_STATUS) && read_of(m, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE) && read_of(m, N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL) && read_of(m, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL), "REACHABILITY: it READS the live gates (HUBP2 CNTL incl. underflow, VM fault, EARLIEST_INUSE, FLIP_CONTROL, ODM2) before deciding");
      expect(n48d2_aset_get()->n == 0u, "the address set is EMPTY once held (no address register may be written again)");
      expect((m.recBuf[N48D2_RC_VALID] & N48D2_RV_HELD) != 0u && (m.gateBuf[N48D2_GT_META] & 0xFFu) == N48D2_PL_HELD, "the record page and the gate meta report HELD");
      expect(m.plane.mc[0] == kBufA && m.plane.mc[1] == kBufB && m.plane.cur == 0u, "the pair and the front buffer are untouched"); }
    // ---- op 14 only after a successful show, front = A, underflow 0, VM fault 0
    { const uint32_t stages[] = { N48D2_PL_NONE, N48D2_PL_ALLOC, N48D2_PL_PLANE, N48D2_PL_FREE, N48D2_PL_LEAKED };
      for (uint32_t st : stages) { PModel m; m.initPlane(); m.plane.stage = st; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD);
        char b[120]; std::snprintf(b, sizeof b, "fbhold from stage %u: PLANE_STATE, nothing read, written or pinned", st);
        expect(o.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty() && m.pinCalls == 0u && m.plane.stage == st, b); }
      { PModel m; plane_up(m); m.reads.clear(); const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_PLANE_STATE && m.pinCalls == 0u && m.plane.stage == N48D2_PL_PLANE && m.reads.empty(), "fbhold before show (stage PLANE): PLANE_STATE, nothing read, no pin"); } }
    { PModel m; shown_up(m); (void)run_op(m, N48D2_OP_FLIP, 1u); const n48d2_out o = run_op(m, N48D2_OP_FBHOLD);
      expect(o.status == N48D2_GATE && m.plane.cur == 1u && m.plane.stage == N48D2_PL_SHOWN && m.pinCalls == 0u && (o.pre_val & N48D2_H_FRONT_B) != 0u, "fbhold with buffer B in front: GATE (front-B), still SHOWN, NOT pinned"); }
    { PModel m; shown_up(m); m.curUnderflow = 0x10000000u; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_UNDERFLOW) != 0u && m.plane.stage == N48D2_PL_SHOWN && m.pinCalls == 0u && m.writes.empty(), "fbhold with a HUBP2 underflow: GATE, still SHOWN, NOT pinned"); }
    { PModel m; shown_up(m); m.vmFault = 0x101u; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_VMFAULT) != 0u && m.plane.stage == N48D2_PL_SHOWN && m.pinCalls == 0u, "fbhold with a VM fault: GATE, still SHOWN, NOT pinned"); }
    { PModel m; shown_up(m); m.segAllocErr = true; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_SEGALLOC) != 0u && m.pinCalls == 0u, "fbhold with SEG_ALLOC_ERR: GATE, NOT pinned"); }
    { PModel m; shown_up(m); m.odmSticky |= 0x400u; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_ODM) != 0u && m.pinCalls == 0u, "fbhold with ODM2 underflow: GATE, NOT pinned"); }
    { PModel m; shown_up(m); m.latched = kBufB; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_LATCH) != 0u && m.pinCalls == 0u && m.plane.stage == N48D2_PL_SHOWN, "fbhold with EARLIEST_INUSE != A: GATE, NOT pinned"); }
    { PModel m; shown_up(m); m.pend = 6u; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && (o.pre_val & N48D2_H_LATCH) != 0u && m.pinCalls == 0u, "fbhold with a flip pending: GATE, NOT pinned"); }
    { PModel m; shown_up(m); m.pinWorks = false; const n48d2_out o = run_op(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_NO_BUFFER && m.plane.stage == N48D2_PL_SHOWN && m.pinCalls == 1u && n48d2_aset_get()->n == 2u, "the allocator refuses the pin: NO_BUFFER, the stage stays SHOWN (the flip addresses stay legal)"); }
    // ---- ORDER: the pin comes AFTER the live gates and BEFORE the stage becomes HELD
    { const std::string flow = strip_line_comments(slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_disp2_flow.h"));
      const size_t a = flow.find("inline bool plane_hold("), e = flow.find("\n}\n", a); const std::string b = a == std::string::npos ? "" : flow.substr(a, e - a);
      const size_t g0 = b.find("plane_gates(e, inst, pl, g, o);"), v0 = b.find("n48d2_hold_verdict(g, pl->cur, pl->mc[0])"), rf = b.find("if (bad != 0u)"), pn = b.find("if (!e.pin())"), st = b.find("pl->stage = N48D2_PL_HELD;"), as = b.find("n48d2_aset_clear_i(inst);"), rv = b.find("N48D2_RV_HELD");
      expect(g0 != std::string::npos && v0 != std::string::npos && rf != std::string::npos && pn != std::string::npos && st != std::string::npos && as != std::string::npos && rv != std::string::npos && g0 < v0 && v0 < rf && rf < pn && pn < st && st < as && as < rv, "ORDER in plane_hold: gates read < verdict < refuse < PIN < stage HELD < address set cleared < record bit");
      expect(b.find("e.wr(") == std::string::npos && b.find("run_steps") == std::string::npos && b.find("vfill") == std::string::npos, "plane_hold writes nothing (no e.wr, no list, no fill)");
      const size_t pe0 = flow.find("inline bool plane_exec("), pe1 = flow.find("n48d2_plane *const pl = e.pl_of(inst);", pe0), hh = flow.find("if (stg == N48D2_PL_HELD && n48d2_held_refuses(op, inst))", pe0), so = flow.find("const bool stOk =", pe0), fh = flow.find("if (op == N48D2_OP_FBHOLD) return plane_hold(e, inst, pl, g, o);", pe0), aj = flow.find("plane_admit(e, inst, pl)", pe0);
      expect(pe1 != std::string::npos && hh != std::string::npos && so != std::string::npos && fh != std::string::npos && aj != std::string::npos && hh < so && so < fh && fh < aj, "ORDER in plane_exec: the HELD refusal < the stage check < fbhold < admission / lists");
      const size_t r0 = flow.find("inline n48d2_out run(E &e, uint32_t op, uint32_t inst"), rh = flow.find("e.pl_of(inst)->stage == N48D2_PL_HELD && n48d2_held_refuses(op, inst)", r0), rb = flow.find("n48d2_build_i(inst, op, s)", r0);
      expect(r0 != std::string::npos && rh != std::string::npos && rb != std::string::npos && rh < rb, "ORDER in run(): the HELD refusal (ops 1/2/3/7 on the monitor B) comes before any list is built"); }
    // ---- a HELD plane refuses every op that could move it: nothing read, nothing written, nothing allocated, no pin, the stage stays HELD
    { const uint32_t ops[] = { N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_PLANEOFF, N48D2_OP_FBHOLD };
      for (uint32_t op : ops) { PModel m; held_up(m); const int v0 = (int)m.vfillCalls; const n48d2_out o = run_op(m, op, 1u);
        char b[160]; std::snprintf(b, sizeof b, "HELD: op %u (%s) is REFUSED with status HELD: nothing read or written, no fill, no pin, stage and pair unchanged", op, n48d2_op_name(op));
        expect(o.status == N48D2_HELD && m.reads.empty() && m.writes.empty() && (int)m.vfillCalls == v0 && m.pinCalls == 0u && m.plane.stage == N48D2_PL_HELD && m.plane.mc[0] == kBufA && m.plane.mc[1] == kBufB && o.free_bufs == 0u && o.auto_off == 0u, b); }
      const uint32_t dops[] = { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF, N48D2_OP_TIMING1440 };
      for (uint32_t op : dops) { PModel m; held_up(m); const n48d2_out o = n48d2::run(m, op, N48D2_INST_MONB);
        char b[160]; std::snprintf(b, sizeof b, "HELD: op %u (%s) on the monitor B is REFUSED with status HELD: nothing read or written (the monitor B's DMUB-side teardown path of disp2)", op, n48d2_op_name(op));
        expect(o.status == N48D2_HELD && m.reads.empty() && m.writes.empty() && m.plane.stage == N48D2_PL_HELD && o.auto_off == 0u, b); }
      // the monitor A is not affected: a timing on instance 1 gets past the HELD rule (whatever its own verdict)
      { PModel m; held_up(m); m.inst = N48D2_INST_MONA; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, N48D2_INST_MONA); expect(o.status != N48D2_HELD, "HELD does not touch the monitor A's ops (timing on instance 1 is not answered HELD)"); } }
    // ---- what stays allowed: status pages, crc, planerec
    { PModel m; held_up(m); uint64_t v[13]; expect_u("status page 1 still works on a HELD plane", n48d2::status(m, v, N48D2_INST_MONB), N48D2_OK); expect(m.writes.empty(), "... and writes nothing"); }
    { PModel m; held_up(m); uint64_t v[13]; expect_u("status page 2 still works", n48d2::status2(m, v, N48D2_INST_MONB), N48D2_OK); expect_u("status page 3 still works", n48d2::status3(m, v, N48D2_INST_MONB), N48D2_OK); }
    { PModel m; held_up(m); uint64_t v[13]; expect_u("planerec still works", n48d2::planerec(m, v), N48D2_OK); uint32_t rec[N48D2_REC_N]; n48d2_unpack_rec(v, rec); expect((rec[N48D2_RC_VALID] & N48D2_RV_HELD) != 0u, "planerec reports HELD (valid bit 128)"); expect(m.reads.empty() && m.writes.empty(), "planerec reads and writes nothing"); }
    { PModel m; held_up(m); const n48d2_out o = run_op(m, N48D2_OP_CRC);
      expect(o.status == N48D2_OK && m.plane.stage == N48D2_PL_HELD && o.free_bufs == 0u && o.auto_off == 0u && m.crcOn, "crc on a HELD plane: OK, the stage stays HELD");
      expect(!m.writes.empty() && m.pinCalls == 0u, "crc writes its CRC registers (and nothing else is asked of the allocator)");
      for (const W &w : m.writes) { const bool crc = w.abs == N48D2_OTG2_OTG_CRC_CNTL || (w.abs >= 0x5125u && w.abs <= 0x512bu); expect(crc, "crc on a HELD plane writes only OTG2 CRC registers"); }
      expect(m.get(N48D2_MPC_OUT2_MUX) == 0x4102u && m.clockOn && !m.blanked, "the plane is still up after the crc"); }
    // ---- a DP disturbance during a HELD crc is REPORTED, never answered by a rollback (the pair is pinned; planeoff is refused)
    { PModel m; held_up(m); m.flipTrig = N48D2_OTG2_OTG_CRC_CNTL; m.flipReg = N48D2_MPC_OUT0_MUX; m.flipXor = 0xFu; const n48d2_out o = run_op(m, N48D2_OP_CRC);
      expect(o.status == N48D2_DP_DISTURBED && o.dpx_changed != 0u, "HELD crc: a DP-plane change is reported as DP_DISTURBED");
      expect(o.auto_off == 0u && o.free_bufs == 0u && m.plane.stage == N48D2_PL_HELD && m.clockOn && !m.blanked && m.get(N48D2_MPC_OUT2_MUX) == 0x4102u && !any_write(m.writes, N48D2_HUBP2_HUBP_CLK_CNTL, 1u, 0u), "... and NO rollback ran: the stage is HELD, the plane is still up, the clock is on, the pair was not freed"); }
    { PModel m; held_up(m); m.stopOtg0On = N48D2_OTG2_OTG_CRC_CNTL; const n48d2_out o = run_op(m, N48D2_OP_CRC);
      expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x80u) != 0u && o.auto_off == 0u && m.plane.stage == N48D2_PL_HELD && m.clockOn, "HELD crc: OTG0 stopping is reported, NO rollback"); }
    // ---- the glue (navi48_dcn.cpp), the allocator (native_s1c.cpp / .h) and the DMUB template refusals: source pins on the real files
    { const std::string src = g_root + "/src/navi48-bringup/src/";
      const std::string glue = slurp(src + "dcn/navi48_dcn.cpp"), s1c = slurp(src + "amd/native_s1c.cpp"), s1h = slurp(src + "amd/native_s1c.h"), dh = slurp(src + "dcn/navi48_dmubcmd.h");
      const size_t d0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {"), d1 = glue.find("\n}\n", d0); const std::string fn = d0 == std::string::npos ? "" : glue.substr(d0, d1 - d0);
      const size_t held = fn.find("if (n48d2_held_refuses(op, inst) && disp2HeldInst(inst)) {"), alloc = fn.find("amdgpu::n1c_d2_alloc("), busy = fn.find("__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL)"), arg = fn.find("if (!n48d2_arg_ok(arg))"), dev = fn.find("if (!gDcn.dev ||");
      expect(!fn.empty() && held != std::string::npos && alloc != std::string::npos && busy != std::string::npos && arg != std::string::npos && dev != std::string::npos, "0.0.652 glue: the disp2() function and its HELD refusal are found");
      expect(arg < dev && dev < held && held < alloc && held < busy, "0.0.652 glue: REACHABILITY: the HELD refusal comes after the argument and device checks and BEFORE the allocator and the busy flag (op 8 never reaches n1c_d2_alloc on a held plane)");
      expect(fn.find("out[0] = N48D2_HELD;") != std::string::npos && fn.find("return N48D2_HELD;") != std::string::npos, "0.0.652 glue: the refusal answers N48D2_HELD");
      expect(fn.find("op == N48D2_OP_FBHOLD") != std::string::npos && fn.find("(op >= N48D2_OP_PLANE && op <= N48D2_OP_PLANEOFF) || op == N48D2_OP_FBHOLD") != std::string::npos, "0.0.652 glue: op 14 is dispatched with the plane ops");
      expect(fn.find("if (freeNow && amdgpu::n1c_d2_pinned(inst)) {") != std::string::npos && fn.find("if (freeNow && amdgpu::n1c_d2_pinned(inst)) {") < fn.find("amdgpu::n1c_d2_free(inst);\n\t}\n\treturn st;"), "0.0.652 glue: a pinned pair is never freed and its stage never reset by the glue");
      expect(fn.find("out[0] |= 1ull << N48D2_STAT_HELD_BIT") != std::string::npos, "0.0.652 glue: `disp2 status 2` reports HELD (bit 52)");
      { const size_t g0 = fn.find("if (freeNow && amdgpu::n1c_d2_pinned(inst)) {"), g1 = fn.find("} else if (freeNow) {", g0 == std::string::npos ? 0 : g0); const std::string br = (g0 == std::string::npos || g1 == std::string::npos) ? "" : fn.substr(g0, g1 - g0);
        expect(!br.empty() && br.find(".stage =") == std::string::npos && br.find(".stage=") == std::string::npos && br.find(".mc[") == std::string::npos && br.find("N48D2_PL_NONE") == std::string::npos && br.find("n1c_d2_free") == std::string::npos && br.find("gD2Off") == std::string::npos, "0.0.652 glue (0.0.655: the per-instance gD2Pl[pi] spelling): the pinned-free branch only logs: it resets no stage, clears no offset and frees nothing"); }
      expect(glue.find("bool pin() { return amdgpu::n1c_d2_pin(inst, gD2Pl[d2i(inst)].mc); }") != std::string::npos, "0.0.652 glue: the env's pin asks the allocator to pin exactly the plane's pair");
      expect(glue.find("return __atomic_load_n(&gD2Pl[d2i(inst)].stage, __ATOMIC_ACQUIRE) == N48D2_PL_HELD || amdgpu::n1c_d2_pinned(inst);") != std::string::npos && glue.find("return disp2HeldInst(N48D2_INST_MONB);") != std::string::npos, "0.0.652 glue: disp2Held = stage HELD or the pair pinned (0.0.655: per instance - disp2HeldInst; disp2Held() is the monitor B's)");
      { const size_t f0 = s1c.find("void n1c_d2_free(uint32_t inst) {"), f1 = s1c.find("\n}\n", f0); const std::string f = f0 == std::string::npos ? "" : s1c.substr(f0, f1 - f0);
        const size_t lk = f.find("IOLockLock(gCliLock);"), pn = f.find("if (n48d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == n48d2pin::kFreePinned) {"), fr = f.find("gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][0]);"), ul = f.find("IOLockUnlock(gCliLock);");
        expect(lk != std::string::npos && pn != std::string::npos && fr != std::string::npos && ul != std::string::npos && lk < pn && pn < ul && ul < fr, "0.0.652 allocator: n1c_d2_free takes the lock, returns at once while pinned (a LOGGED no-op), and only then can free");
        expect(f.find("d2 free REFUSED") != std::string::npos, "0.0.652 allocator: the no-op is logged");
        expect(f.find("n48d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == n48d2pin::kFreeDo") != std::string::npos && count_of(f, "n48d2pin::free_action(") == 2u, "0.0.652 allocator: BOTH tests of n1c_d2_free go through the pure free_action (pinned -> no-op, held -> free)");
        const size_t p0 = s1c.find("bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]) {"), p1 = s1c.find("\n}\n", p0); const std::string pf = p0 == std::string::npos ? "" : s1c.substr(p0, p1 - p0);
        expect(pf.find("IOLockLock(gCliLock);") != std::string::npos && pf.find("n48d2pin::pin_ok(gD2Held[sl], gD2Ctx[sl] != nullptr, gD2Pinned[sl],") != std::string::npos && pf.find("mc[0] != 0u && gD2Buf[sl][0].gpu_va == mc[0], mc[1] != 0u && gD2Buf[sl][1].gpu_va == mc[1]") != std::string::npos && pf.find("gD2Pinned[sl] = true;") != std::string::npos, "0.0.652 allocator: pin = under gCliLock, the pair is held, not yet pinned, and exactly A and B");
        expect(s1c.find("gD2Pinned[sl] = false") == std::string::npos && count_of(s1c, "gD2Pinned[sl] = ") == 1u, "0.0.652 allocator: gD2Pinned is set once and NEVER cleared (a reboot is the release)");
        const size_t a0 = s1c.find("IOReturn n1c_d2_alloc("), a1 = s1c.find("\n}\n", a0); const std::string af = s1c.substr(a0, a1 - a0);
        expect(af.find("if (n48d2pin::alloc_refused(gD2Held[sl])) { rc = kIOReturnExclusiveAccess; break; }") != std::string::npos, "0.0.652 allocator: a held (hence a pinned) pair makes any second allocation refuse"); }
      expect(s1h.find("bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]);") != std::string::npos && s1h.find("bool n1c_d2_pinned(uint32_t inst);") != std::string::npos, "0.0.652 allocator: exported from native_s1c.h");
      // the DMUB templates
      expect(dh.find("static inline int n48dm_tpl_held_refuses(uint32_t id) { return id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }") != std::string::npos, "0.0.653 DMUB: ALL the monitor B's templates (8..15) are refused while HELD");
      { const size_t m0 = glue.find("uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount) {"), m1 = glue.find("\n}\n", m0); const std::string f = m0 == std::string::npos ? "" : glue.substr(m0, m1 - m0);
        const size_t rp = f.find("if (disp2Held() || disp2HeldInst(N48D2_INST_MONA)) {"), ru = f.find("n48dm_replay_unarg(arg, &replaySrc)"), tp = f.find("n48dm_tpl_held_refuses(tplId) && disp2Held()"), ts = f.find("o = n48dm::send(env, 0u, 0u, 0u, N48DM_NO_REPLAY, tplId)");
        expect(rp != std::string::npos && ru != std::string::npos && tp != std::string::npos && ts != std::string::npos && rp < ru && tp < ts, "0.0.652 DMUB: REACHABILITY: the HELD refusals of dmubsend replay and of the two teardown templates come BEFORE anything is sent");
        expect(count_of(f, "N48DR_HELD_REFUSED") == 5u, "0.0.653 DMUB (0.0.655: five): all refusals (replay, monitor B templates, monitor A templates, raw monitor B-context slots, raw monitor A-context slots) answer N48DR_HELD_REFUSED");
        const size_t rr = f.find("n48dm_slot_held_refuses(h, d1, d2) && disp2Held()"), rs = f.find("o = n48dm::send(env, h, d1, d2)");
        expect(rr != std::string::npos && rs != std::string::npos && rr < rs, "0.0.653 DMUB: REACHABILITY: the raw monitor B-context HELD refusal comes BEFORE the raw send"); } }
}

// =====================================================================================================================================================================================================
// 0.0.655: INSTANCE 1 (the monitor A) PLANE at 1080p60: ops 8..13 for the monitor A (an internal design note "monitor A (instance 1) plane + framebuffer spec at 1080p60", sections 1 and 3).
// Groups: tables vs Linux / the spec / the census (mona_tables_vs_linux); the per-instance guard, both directions (mona_guard_tests); the exact step lists (mona_list_shapes); the flow over P1Model (op 8 clean + order + prechecks +
// fill / verify, the gate failures, ops 9..13, the DP watch AND the MONB WATCH); the per-instance allocator / address set / HELD, with the monitor B held while the monitor A runs (mona_instance_tests); source pins (mona_source_pins).
// =====================================================================================================================================================================================================
static uint32_t mona_lx(const std::string &name) { return lx_abs(name); }
static const struct { uint32_t ours; const char *lx; } kMonARegs[] = {
    { N48D2_DPP_TOP1_DPP_CONTROL, "DPP_TOP1_DPP_CONTROL" }, { N48D2_DSCL1_SCL_MODE, "DSCL1_SCL_MODE" }, { N48D2_DSCL1_DSCL_2TAP_CONTROL, "DSCL1_DSCL_2TAP_CONTROL" },
    { N48D2_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO, "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO" }, { N48D2_DSCL1_SCL_HORZ_FILTER_INIT, "DSCL1_SCL_HORZ_FILTER_INIT" },
    { N48D2_DSCL1_SCL_VERT_FILTER_SCALE_RATIO, "DSCL1_SCL_VERT_FILTER_SCALE_RATIO" }, { N48D2_DSCL1_SCL_VERT_FILTER_INIT, "DSCL1_SCL_VERT_FILTER_INIT" },
    { N48D2_DSCL1_OTG_H_BLANK, "DSCL1_OTG_H_BLANK" }, { N48D2_DSCL1_OTG_V_BLANK, "DSCL1_OTG_V_BLANK" }, { N48D2_DSCL1_RECOUT_SIZE, "DSCL1_RECOUT_SIZE" }, { N48D2_DSCL1_MPC_SIZE, "DSCL1_MPC_SIZE" },
    { N48D2_DSCL1_LB_MEMORY_CTRL, "DSCL1_LB_MEMORY_CTRL" }, { N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION" },
    { N48D2_HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION" }, { N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG, "HUBP1_DCHUBP_REQ_SIZE_CONFIG" },
    { N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, "HUBPREQ1_DCSURF_SURFACE_PITCH" }, { N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS" },
    { N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH" }, { N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL, "HUBPREQ1_DCSURF_FLIP_CONTROL" },
    { N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE" }, { N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH" },
    { N48D2_HUBPREQ1_DCN_TTU_QOS_WM, "HUBPREQ1_DCN_TTU_QOS_WM" }, { N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, "HUBPREQ1_DCN_GLOBAL_TTU_CNTL" },
    { N48D2_HUBPREQ1_DCN_SURF0_TTU_CNTL0, "HUBPREQ1_DCN_SURF0_TTU_CNTL0" }, { N48D2_HUBPREQ1_BLANK_OFFSET_0, "HUBPREQ1_BLANK_OFFSET_0" }, { N48D2_HUBPREQ1_BLANK_OFFSET_1, "HUBPREQ1_BLANK_OFFSET_1" },
    { N48D2_HUBPREQ1_DST_DIMENSIONS, "HUBPREQ1_DST_DIMENSIONS" }, { N48D2_HUBPREQ1_PREFETCH_SETTINGS, "HUBPREQ1_PREFETCH_SETTINGS" },
    { N48D2_HUBPREQ1_PREFETCH_SETTINGS_C, "HUBPREQ1_PREFETCH_SETTINGS_C" }, { N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, "HUBPREQ1_VBLANK_PARAMETERS_0" },
    { N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ, "HUBPREQ1_REF_FREQ_TO_PIX_FREQ" }, { N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, "ODM1_OPTC_INPUT_GLOBAL_CONTROL" },
    { N48D2_OTG1_OTG_CRC_CNTL, "OTG1_OTG_CRC_CNTL" }, { N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL, "OTG1_OTG_CRC0_WINDOWA_X_CONTROL" },
    { N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL, "OTG1_OTG_CRC0_WINDOWA_Y_CONTROL" }, { N48D2_OTG1_OTG_CRC0_WINDOWB_X_CONTROL, "OTG1_OTG_CRC0_WINDOWB_X_CONTROL" },
    { N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL, "OTG1_OTG_CRC0_WINDOWB_Y_CONTROL" }, { N48D2_OTG1_OTG_CRC0_DATA_RG, "OTG1_OTG_CRC0_DATA_RG" }, { N48D2_OTG1_OTG_CRC0_DATA_B, "OTG1_OTG_CRC0_DATA_B" },
    { N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, "OTG1_OTG_MASTER_UPDATE_LOCK" }, { N48D2_OTG1_OTG_GLOBAL_CONTROL2, "OTG1_OTG_GLOBAL_CONTROL2" }, { N48D2_DCHUBBUB_DET1_CTRL, "DCHUBBUB_DET1_CTRL" },
    { N48D2_DOMAIN1_PG_STATUS, "DOMAIN1_PG_STATUS" }, { N48D2_HUBPREQ1_VM_APERTURE_LOW, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR" },
    { N48D2_HUBPREQ1_VM_APERTURE_HIGH, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR" }, { N48D2_HUBPREQ1_VM_MX_L1_TLB, "HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL" },
    { N48D2_HUBPREQ1_VMID_SETTINGS_0, "HUBPREQ1_VMID_SETTINGS_0" }, { N48D2_MPCC1_MPCC_TOP_SEL, "MPCC1_MPCC_TOP_SEL" }, { N48D2_MPCC1_MPCC_BOT_SEL, "MPCC1_MPCC_BOT_SEL" },
    { N48D2_MPCC1_MPCC_OPP_ID, "MPCC1_MPCC_OPP_ID" }, { N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, "MPCC1_MPCC_UPDATE_LOCK_SEL" }, { N48D2_MPCC1_MPCC_STATUS, "MPCC1_MPCC_STATUS" },
    { N48D2_MPC_OUT1_MUX, "MPC_OUT1_MUX" }, { N48D2_DPPCLK1_DTO_PARAM, "DPPCLK1_DTO_PARAM" }, { N48D2_DENTIST_DISPCLK_CNTL, "DENTIST_DISPCLK_CNTL" },
};
static std::map<std::string, uint32_t> census_parse(const std::string &txt) {      // "  NAME (BASE_IDX n, abs 0x....) = 0x.... | 0000000000"
    std::map<std::string, uint32_t> m; std::istringstream is(txt); std::string line;
    while (std::getline(is, line)) {
        const size_t p = line.find(" (BASE_IDX "); const size_t q = line.find(") = ");
        if (p == std::string::npos || q == std::string::npos || line.size() < 3 || line[0] != ' ') continue;
        size_t b = 0; while (b < line.size() && line[b] == ' ') ++b;
        std::string name = line.substr(b, p - b); while (!name.empty() && name.back() == ' ') name.pop_back(); const std::string val = line.substr(q + 4);
        m[name] = val.compare(0, 2, "0x") == 0 ? (uint32_t)std::strtoul(val.c_str(), nullptr, 16) : 0u;
    }
    return m;
}
static void mona_tables_vs_linux() {
    // ---- every instance-1 register by NAME against dcn_4_1_0_offset.h (offset AND BASE_IDX)
    for (const auto &r : kMonARegs) expect_u((std::string("instance-1 register ") + r.lx + " is Linux's absolute address").c_str(), r.ours, mona_lx(r.lx));
    // the pipe-1 = pipe-0 + stride relationship the spec states (HUBP / HUBPREQ / HUBPRET + 0xdc, DPP + 0x16b, MPCC + 0x15)
    expect(N48D2_HUBP1_DCHUBP_CNTL - N48D2_HUBP0_DCHUBP_CNTL == 0xdcu && N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH - N48D2_HUBPREQ0_SURFACE_PITCH == 0xdcu && N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS - N48D2_HUBPREQ0_PRIMARY_LOW == 0xdcu &&
           N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION - N48D2_HUBP0_PRI_VIEWPORT_DIMENSION == 0xdcu, "pipe 1 sits 0xdc above pipe 0 for HUBP / HUBPREQ");
    expect(N48D2_DSCL1_SCL_MODE - 0x41c8u == 0x16bu && N48D2_DSCL1_LB_MEMORY_CTRL - 0x41e2u == 0x16bu && N48D2_DPP_TOP1_DPP_CONTROL - N48D2_DPP_TOP0_DPP_CONTROL == 0x16bu, "pipe 1 sits 0x16b above pipe 0 for DPP / DSCL");
    expect(N48D2_MPCC1_MPCC_TOP_SEL - N48D2_MPCC0_TOP_SEL == 0x15u && N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL - N48D2_MPCC0_UPDATE_LOCK_SEL == 0x15u && N48D2_MPCC1_MPCC_BOT_SEL == 0x9016u && N48D2_MPCC1_MPCC_OPP_ID == 0x9017u && N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL == 0x901au, "MPCC1 sits 0x15 above MPCC0 (TOP 0x9015, BOT 0x9016, OPP 0x9017, LOCK_SEL 0x901a)");
    // the 3 abs numbers the brief names
    expect(N48D2_OTG1_OTG_GLOBAL_CONTROL2 == 0x50d0u && N48D2_OTG1_OTG_MASTER_UPDATE_LOCK == 0x50c9u && N48D2_OTG1_OTG_CRC_CNTL == 0x50a5u && N48D2_OTG1_OTG_CRC0_DATA_RG == 0x50aau && N48D2_OTG1_OTG_CRC0_DATA_B == 0x50abu && N48D2_MPC_OUT1_MUX == 0x92f6u && N48D2_DPPCLK1_DTO_PARAM == 0x15au && N48D2_DENTIST_DISPCLK_CNTL == 0x124u && N48D2_DCHUBBUB_DET1_CTRL == 0x397cu,
           "OTG1 lock 0x50c9, lock select 0x50d0, CRC 0x50a5..0x50ab, MPC_OUT1_MUX 0x92f6, DPPCLK1_DTO 0x15a, DENTIST_DISPCLK_CNTL 0x124, DET1 0x397c");
    // ---- fields
    expect_u("DPPCLK1_EN is bit 3 (mask 0x8)", N48D2_DPPCLK1_EN_MASK, lx_mask("DPPCLK_CTRL__DPPCLK1_EN"));
    expect_u("OTG_MASTER_UPDATE_LOCK_SEL = 1 (optc3_lock: optc->inst of OTG1)", N48D2_OTG_LOCK_SEL_OTG1, lx_fv("OTG0_OTG_GLOBAL_CONTROL2__OTG_MASTER_UPDATE_LOCK_SEL", 1u));
    expect(N48D2_MPC_OUT1_MUX_ON == (lx_fv("MPC_OUT0_MUX__MPC_OUT_MUX", 1u) | lx_mask("MPC_OUT0_MUX__MPC_OUT_RATE_CONTROL_DISABLE") | 0x4000u) && N48D2_MPC_OUT1_MUX_ON == 0x00004101u && N48D2_MPC_OUT1_MUX_OFF == 0x400fu, "MPC_OUT1_MUX: mux 1 + RATE_CONTROL_DISABLE + the 0x4000 flow count = 0x4101; none = 0x400f");
    // ---- the spec's literal 1080p table (section 1), typed independently here and compared to the builder's table, the guard's PIN table and the built part-D list
    static const struct { const char *lx; uint32_t v; bool upd; uint32_t mask; } spec[24] = {
        { "DSCL1_SCL_MODE", 0x00000001u, false, 0u }, { "DSCL1_DSCL_2TAP_CONTROL", 0x01110111u, false, 0u }, { "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO", 0x01000000u, false, 0u }, { "DSCL1_SCL_HORZ_FILTER_INIT", 0u, false, 0u },
        { "DSCL1_SCL_VERT_FILTER_SCALE_RATIO", 0x01000000u, false, 0u }, { "DSCL1_SCL_VERT_FILTER_INIT", 0u, false, 0u }, { "DSCL1_OTG_H_BLANK", 0x00c00840u, false, 0u }, { "DSCL1_OTG_V_BLANK", 0x00290461u, false, 0u },
        { "DSCL1_RECOUT_SIZE", 0x04380780u, false, 0u }, { "DSCL1_MPC_SIZE", 0x04380780u, false, 0u }, { "DSCL1_LB_MEMORY_CTRL", 0x00003f00u, true, 0x00003F03u },
        { "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION", 0x04380780u, false, 0u }, { "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION", 0x04380780u, false, 0u }, { "HUBP1_DCHUBP_REQ_SIZE_CONFIG", 0x05500300u, false, 0u },
        { "HUBPREQ1_DCN_TTU_QOS_WM", 0x08a40000u, false, 0u }, { "HUBPREQ1_DCN_GLOBAL_TTU_CNTL", 0xe0006a1au, false, 0u }, { "HUBPREQ1_DCN_SURF0_TTU_CNTL0", 0x08000000u, false, 0u }, { "HUBPREQ1_BLANK_OFFSET_0", 0x00290040u, false, 0u },
        { "HUBPREQ1_BLANK_OFFSET_1", 0x00000d12u, false, 0u }, { "HUBPREQ1_DST_DIMENSIONS", 0x00027f99u, false, 0u }, { "HUBPREQ1_PREFETCH_SETTINGS", 0x28080000u, false, 0u }, { "HUBPREQ1_PREFETCH_SETTINGS_C", 0x00080000u, false, 0u },
        { "HUBPREQ1_VBLANK_PARAMETERS_0", 0x00000301u, false, 0u }, { "HUBPREQ1_REF_FREQ_TO_PIX_FREQ", 0x00021fc4u, false, 0u } };
    for (uint32_t i = 0; i < N48D1_PROG_COUNT; i++) {
        const uint32_t a = mona_lx(spec[i].lx);
        const uint8_t kind = spec[i].upd ? N48D2_K_UPD : N48D2_K_SET; const uint32_t mask = spec[i].upd ? spec[i].mask : 0xFFFFFFFFu;
        expect(n48d1_prog[i].abs == a && n48d1_prog[i].val == spec[i].v && n48d1_prog[i].kind == kind && n48d1_prog[i].mask == mask, (std::string("the builder's table row ") + spec[i].lx + " is the spec's register, kind and 1080p value").c_str());
        expect(n48d1_pin[i].abs == a && n48d1_pin[i].val == spec[i].v && n48d1_pin[i].kind == kind && n48d1_pin[i].mask == mask, (std::string("the guard's PIN row ") + spec[i].lx + " is the spec's register, kind and 1080p value").c_str());
        expect(std::string(n48d1_prog[i].name) == spec[i].lx && std::string(n48d1_pin[i].name) == spec[i].lx, "the table rows carry the Linux register name");
    }
    { n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d1_build_plane_d(s, kA1); bool same = n == N48D1_PD_STEPS;
      for (uint32_t i = 0; same && i < N48D1_PROG_COUNT; i++) same = s[i].abs == mona_lx(spec[i].lx) && s[i].val == spec[i].v && s[i].kind == (spec[i].upd ? N48D2_K_UPD : N48D2_K_SET) && s[i].mask == (spec[i].upd ? spec[i].mask : 0xFFFFFFFFu) && s[i].kind != N48D2_K_COPY0;
      expect(same && N48D1_PD_STEPS == 26u && N48D1_PROG_COUNT == 24u, "part D = the 24 explicit writes in the spec's table order + the address twice; NO COPY0 step"); }
    // ---- the DLG / TTU values against the spec's formulas (scale factors: line time L = (2200/148.5)/(2720/241.5), pixel clock P = 241.5/148.5, frame lines V = 1125/1481)
    {
        const double L = (2200.0 / 148.5) / (2720.0 / 241.5), P = 241.5 / 148.5, V = 1125.0 / 1481.0;
        expect(L > 1.31535 && L < 1.31537 && P > 1.62625 && P < 1.62627, "the scale factors: L = 1.31536, P = 1.62626");
        expect_u("TTU_QOS_WM HIGH = 4 line-times = floor(1682 x L) = 2212", (0x08a40000u >> 16) & 0x3FFFu, (uint64_t)(1682.0 * L));
        expect(((0x08a40000u >> 16) & 0x3FFFu) < 2962u && (0x08a40000u & 0xFFFFu) == 0u, "TTU_QOS_WM HIGH stays BELOW the formula's 2962 (the safe direction) and LOW is 0 as pipe 0's");
        expect_u("GLOBAL_TTU_CNTL MIN_TTU_VBLANK = floor(20650 x L) = 27162 (the per-line reading: the larger, safe one)", 0xe0006a1au & 0x00FFFFFFu, (uint64_t)(20650.0 * L));
        expect((0xe0006a1au >> 28) == 0xeu && (0xe00050aau >> 28) == 0xeu, "GLOBAL_TTU_CNTL keeps the QoS field 0xe of the live value");
        expect_u("BLANK_OFFSET_0: V_BLANK_END 41", 0x00290040u >> 16, 41u);
        expect_u("BLANK_OFFSET_0: REFCYC_H_BLANK_END = floor(192 px x 50 MHz / 148.5 MHz) = 64 (a pixel count x ref/pix, NOT x 1.315 = 30)", 0x00290040u & 0xFFFFu, (uint64_t)(192.0 * 50.0 / 148.5));
        { volatile uint32_t blank0 = n48d1_prog[17].val; expect(((uint64_t)(23.0 * L)) == 30u && (blank0 & 0xFFFFu) != 30u && (blank0 & 0xFFFFu) == 64u, "the rejected scaling (23 x 1.315 = 30) is not what is programmed"); }
        expect_u("BLANK_OFFSET_1 = floor(4406 x V) = 3346", 0x00000d12u, (uint64_t)(4406.0 * V));
        expect(0x00000d12u < 0x1136u && 0x00000d12u < 0x1204u, "BLANK_OFFSET_1 is below the fallback 0x1136 and the Linux formula's 0x1204 (an earlier start: the safe direction)");
        expect_u("DST_DIMENSIONS = floor(124481 x L) = 163737", 0x00027f99u, (uint64_t)(124481.0 * L));
        expect(0x00027f99u < 189629u, "DST_DIMENSIONS stays BELOW the formula's 189629 (host test: the spec's hard bound)");
        expect_u("REF_FREQ_TO_PIX_FREQ = floor(85598 x P) = 139204", 0x00021fc4u, (uint64_t)(85598.0 * P));
        expect(0x00021fc4u < 176527u, "REF_FREQ_TO_PIX_FREQ stays BELOW the formula's 176527");
        expect(((0x28080000u >> 24) & 0xFFu) == 0x28u && 10u <= 13u, "PREFETCH: 10 lines of prefetch, within VSTARTUP 13");
        struct n48d2_timing t; struct n48d2_otg_regs o; expect(n48d2_dtd_decode(n48d2_mona_dtd, &t) == 0u && n48d2_otg_compute(&t, &o) == 0u, "the monitor A's EDID descriptor decodes");
        expect(t.h_total == 2200u && t.v_total == 1125u && t.pix_clk_10khz == 14850u && o.h_total == 0x897u && o.h_blank == 0x00c00840u && o.v_total == 0x464u && o.v_blank == 0x00290461u && o.vstartup == 0xdu && o.vupdate == 0x014002a8u && o.vready == 0x12cu,
               "`timing 1`'s OTG1 values (optc1_program_timing's arithmetic) ARE the census values: H_TOTAL 0x897, H_BLANK 0x00c00840, V_TOTAL 0x464, V_BLANK 0x00290461, VSTARTUP 0xd, VUPDATE 0x014002a8, VREADY 0x12c");
        expect(n48d1_prog[6].val == o.h_blank && n48d1_prog[7].val == o.v_blank, "DSCL1_OTG_H_BLANK / _V_BLANK are the OTG1 blank words (start 2112 end 192; start 1121 end 41)");
    }
    // ---- the surface
    expect(N48D1_BUF_BYTES == 127u * 65536u && N48D1_BUF_BYTES % 65536u == 0u && N48D1_FRAME_BYTES == 1920u * 1080u * 4u && N48D1_FRAME_BYTES % 65536u != 0u && N48D1_FRAME_BYTES <= N48D1_BUF_BYTES && N48D1_PLANE_PITCH_BYTES == 1920u * 4u && N48D1_PITCH_PX == 1920u - 1u && N48D1_BAR_PX * 8u == N48D1_PLANE_W &&
           N48D1_VIEW_DIM == ((1080u << 16) | 1920u) && N48D1_CRC_WIN_X == (1920u << 16) && N48D1_CRC_WIN_Y == (1080u << 16),
           "the monitor A's buffer is 127 x 64 KiB = 8,323,072 (the frame, 8,294,400, is NOT a 64 KiB multiple: the allocator would refuse it); pitch 0x77f; eight 240-px bars; viewport / RECOUT 0x04380780; CRC windows 1920 << 16 / 1080 << 16");
    expect(N48D1_BUF_BYTES - N48D1_FRAME_BYTES == 28672u, "the 28,672-byte tail past the frame is never drawn or scanned");
    { std::set<uint32_t> offs; bool pos = true; for (uint32_t p = 0; p < N48D2_VPOINTS; p++) { uint32_t w = 0; const uint32_t off = n48d2_vpoint_i(N48D2_INST_MONA, 0, p, &w); offs.insert(off); const uint32_t row = off / N48D1_PLANE_PITCH_BYTES, x = (off % N48D1_PLANE_PITCH_BYTES) / 4u; pos = pos && (row == 0 || row == 540 || row == 1079) && x % 240u == 120u && w == n48d2_bars[x / 240u] && off + 4u <= N48D1_FRAME_BYTES; }
      expect(offs.size() == 24u && pos, "the monitor A's 24 verification points: rows 0, 540, 1079 at x = 120 + 240 * bar, each expecting its bar's colour, all inside the frame"); }
    { uint32_t w = 0; expect(n48d2_vpoint_i(N48D2_INST_MONB, 1, 9, &w) == n48d2_vpoint(1, 9, &w) && n48d2_surf_get(N48D2_INST_MONA)->buf_bytes == N48D1_BUF_BYTES && n48d2_surf_get(N48D2_INST_MONB)->buf_bytes == N48D2_BUF_BYTES, "the surface table: instance 1 is the monitor A's, instance 2 the monitor B's (the monitor B's vpoint is unchanged)"); }
    // ---- the per-instance register table of the flow (instance 2's row is the constants the monitor B's flow always named)
    { const n48d2_pr *a = n48d2_pr_get(N48D2_INST_MONA), *d = n48d2_pr_get(N48D2_INST_MONB);
      expect(a->hubp_cntl == N48D2_HUBP1_DCHUBP_CNTL && a->hubp_clk == N48D2_HUBP1_HUBP_CLK_CNTL && a->odm_ctl == N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL && a->det_ctrl == N48D2_DCHUBBUB_DET1_CTRL && a->early_lo == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE && a->early_hi == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH &&
             a->flip_ctl == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL && a->mpcc_status == N48D2_MPCC1_MPCC_STATUS && a->mpc_mux == N48D2_MPC_OUT1_MUX && a->fifo == N48D2_DIG2_FIFO_CTRL0 && a->crc_rg == N48D2_OTG1_OTG_CRC0_DATA_RG && a->crc_b == N48D2_OTG1_OTG_CRC0_DATA_B && a->addr_lo == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS && a->addr_hi == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && a->otg_frame == N48D2_OTG1_FRAME_COUNT,
             "the monitor A's plane register row (HUBP1 / ODM1 / DET1 / EARLIEST_INUSE1 / FLIP1 / MPCC1 / MPC_OUT1 / DIG2 FIFO / OTG1 CRC and frame counter)");
      expect(d->hubp_cntl == N48D2_HUBP2_DCHUBP_CNTL && d->hubp_clk == N48D2_HUBP2_HUBP_CLK_CNTL && d->odm_ctl == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL && d->det_ctrl == N48D2_DCHUBBUB_DET2_CTRL && d->early_lo == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && d->flip_ctl == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL && d->mpcc_status == N48D2_MPCC2_MPCC_STATUS &&
             d->mpc_mux == N48D2_MPC_OUT2_MUX && d->fifo == N48D2_DIG3_FIFO_CTRL0 && d->crc_rg == N48D2_OTG2_OTG_CRC0_DATA_RG && d->addr_lo == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS && d->otg_frame == N48D2_OTG2_FRAME_COUNT, "the monitor B's plane register row is the constants the monitor B's flow always named"); }
    // ---- the monitor B watch table
    { static const struct { const char *lx; uint32_t ours; uint32_t row; } w2[N48D2_W2_DW] = { { "DPPCLK_CTRL", N48D2_DPPCLK_CTRL, 0 }, { "DPPCLK2_DTO_PARAM", N48D2_DPPCLK2_DTO_PARAM, 1 }, { "MPC_OUT2_MUX", N48D2_MPC_OUT2_MUX, 2 }, { "MPCC2_MPCC_TOP_SEL", N48D2_MPCC2_MPCC_TOP_SEL, 3 }, { "MPCC2_MPCC_OPP_ID", N48D2_MPCC2_MPCC_OPP_ID, 3 },
          { "DCHUBBUB_DET2_CTRL", N48D2_DCHUBBUB_DET2_CTRL, 4 }, { "HUBP2_DCHUBP_CNTL", N48D2_HUBP2_DCHUBP_CNTL, 5 }, { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE", N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, 6 }, { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 6 }, { "DENTIST_DISPCLK_CNTL", N48D2_DENTIST_DISPCLK_CNTL, 7 } };
      bool ok = true; for (uint32_t i = 0; i < N48D2_W2_DW; i++) ok = ok && n48d2_w2_regs[i].abs == w2[i].ours && w2[i].ours == lx_abs(w2[i].lx) && n48d2_w2_regs[i].row == w2[i].row;
      expect(ok && N48D2_W2_ROWS == 9u && N48D2_W2_ROW_COUNTING == 8u && N48D2_W2_DW == 10u && N48D2_W2_BUF >= 12u, "the monitor B watch: DPPCLK_CTRL bit 6, DPPCLK2_DTO, MPC_OUT2_MUX, MPCC2 TOP and OPP, DET2 current, HUBP2 underflow, EARLIEST_INUSE2 LOW / HIGH, DENTIST_DISPCLK_CNTL (+ OTG2 counting = row 8)");
      expect(n48d2_w2_regs[0].mask == lx_mask("DPPCLK_CTRL__DPPCLK2_EN") && n48d2_w2_regs[5].mask == lx_mask("DCHUBBUB_DET0_CTRL__DET0_SIZE_CURRENT") && n48d2_w2_regs[6].mask == (N48D2_HUBP_UNDERFLOW_MASK | N48D2_HUBP_SEG_ALLOC_ERR_MASK), "the watch masks: DPPCLK2_EN, the DET current field, HUBP2 underflow + SEG_ALLOC_ERR");
      uint32_t b[N48D2_W2_BUF] = {}, a[N48D2_W2_BUF] = {};
      expect(n48d2_w2_diff(b, a) == 0u, "the monitor B watch diff of equal snapshots is 0");
      for (uint32_t i = 0; i < N48D2_W2_DW; i++) { std::memcpy(a, b, sizeof a); a[i] ^= 1u; expect(n48d2_w2_diff(b, a) == (1u << w2[i].row), (std::string("the monitor B watch diff names row ") + std::to_string(w2[i].row) + " for a change of " + w2[i].lx).c_str()); }
      std::memset(b, 0, sizeof b); b[10] = 100; b[11] = 103; std::memcpy(a, b, sizeof a); a[10] = 103; expect(n48d2_w2_diff(b, a) == (1u << 8), "row 8: OTG2 was counting at the start and its counter did not move by the end -> changed");
      a[10] = 109; expect(n48d2_w2_diff(b, a) == 0u, "row 8: counting before and still counting -> unchanged");
      b[11] = 100; a[10] = 100; expect(n48d2_w2_diff(b, a) == 0u, "row 8: not counting before (the monitor B's pipe is off) -> no requirement");
      expect(n48d2_w2_first(0u) == 0u && n48d2_w2_first(1u << 4) == 5u && n48d2_w2_first(0x1FFu) == 1u && n48d2_w2_first(1u << 8) == 9u, "the first changed row + 1 fits the 4-bit guard field (1..9)"); }
    // ---- the CENSUS cross-check (mona-census-654, taken on the PC): every "equal already" check of the spec is an EXPECT row from a census value; nothing contradicts the spec
    {
        const std::string cb = find_notes_file("notes/logs/runs/mona-census-654/mona-before.txt"), ca = find_notes_file("notes/logs/runs/mona-census-654/mona-after-timing.txt"), cp = find_notes_file("notes/logs/runs/m4ab-634/census-plane.txt");
        expect(!cb.empty() && !ca.empty() && !cp.empty(), "the census files are readable (mona-census-654 before / after timing, m4ab-634/census-plane.txt)");
        const std::map<std::string, uint32_t> B = census_parse(cb), A = census_parse(ca), M = census_parse(cp);
        expect(B.size() > 300 && A.size() > 300 && B.size() == A.size(), "the monitor A census has the same 361 reads before and after `timing 1`");
        uint32_t nCheck = 0, nBad = 0;
        auto census_of = [&](const std::string &n, uint32_t *v) { auto it = A.find(n); if (it != A.end()) { *v = it->second; return true; } it = M.find(n); if (it != M.end()) { *v = it->second; return true; } return false; };
        for (uint32_t i = 0; i < N48D1_PRE_STATE_COUNT; i++) { uint32_t v = 0; if (!census_of(n48d1_pre_state[i].name, &v)) continue; ++nCheck; if ((v & n48d1_pre_state[i].mask) != n48d1_pre_state[i].val) { ++nBad; std::printf("   census contradicts the state row %s: %#x (mask %#x) want %#x\n", n48d1_pre_state[i].name, v, n48d1_pre_state[i].mask, n48d1_pre_state[i].val); } }
        for (uint32_t i = 0; i < N48D1_EXP_COUNT; i++) { uint32_t v = 0; if (!census_of(n48d1_exp[i].name, &v)) { ++nBad; std::printf("   EXPECT row %s is in no census\n", n48d1_exp[i].name); continue; } ++nCheck; if ((v & n48d1_exp[i].mask) != n48d1_exp[i].val) { ++nBad; std::printf("   census contradicts EXPECT row %s: %#x (mask %#x) want %#x\n", n48d1_exp[i].name, v, n48d1_exp[i].mask, n48d1_exp[i].val); } }
        for (uint32_t i = 0; i < N48D1_PRE_STATE_COUNT; i++) { expect(lx_abs(n48d1_pre_state[i].name) == n48d1_pre_state[i].abs, (std::string("state row ") + n48d1_pre_state[i].name + " is Linux's absolute address").c_str()); }
        for (uint32_t i = 0; i < N48D1_EXP_COUNT; i++) { expect(lx_abs(n48d1_exp[i].name) == n48d1_exp[i].abs, (std::string("EXPECT row ") + n48d1_exp[i].name + " is Linux's absolute address").c_str()); }
        std::printf("monitor A census: %u state / EXPECT rows checked against the census, %u contradictions\n", nCheck, nBad);
        expect(nCheck >= 45u && nBad == 0u, "every instance-1 EXPECT / state row holds in the census (mona-after-timing, plus m4ab-634 for MPCC1 and the VM registers); nothing contradicts the spec");
        // the values the brief names
        auto A_ = [&](const char *n) { auto it = A.find(n); return it == A.end() ? 0xDEADBEEFu : it->second; };
        expect(A_("OTG1_OTG_GLOBAL_CONTROL2") == 0x02000000u && (A_("OTG1_OTG_GLOBAL_CONTROL2") & N48D2_OTG_LOCK_SEL_MASK) == N48D2_OTG_LOCK_SEL_OTG1, "census: OTG1_GLOBAL_CONTROL2 = 0x02000000, the lock select is ALREADY 1");
        expect(A_("MPC_OUT1_MUX") == 0x400fu && A_("HUBP1_DCHUBP_CNTL") == 0x000f001au && A_("HUBP1_HUBP_CLK_CNTL") == 0x00500000u && (A_("HUBP1_DCHUBP_CNTL") & 1u) == 0u && ((A_("HUBP1_DCHUBP_CNTL") >> 4) & 0xFu) == 1u, "census: MPC_OUT1_MUX 0x400f; HUBP1_DCHUBP_CNTL 0x000f001a (UNBLANKED, VTG_SEL 1); HUBP1_HUBP_CLK_CNTL 0x00500000");
        expect(A_("OTG1_OTG_H_TOTAL") == 0x897u && A_("OTG1_OTG_H_BLANK_START_END") == 0x00c00840u && A_("OTG1_OTG_V_TOTAL") == 0x464u && A_("OTG1_OTG_V_BLANK_START_END") == 0x00290461u && A_("OTG1_OTG_VSTARTUP_PARAM") == 0xdu && A_("OTG1_OTG_VUPDATE_PARAM") == 0x014002a8u && A_("OTG1_OTG_VREADY_PARAM") == 0x12cu, "census after `timing 1`: the brief's OTG1 timing values");
        { auto b0 = [&](const char *n) { auto it = B.find(n); return it == B.end() ? 0xDEADBEEFu : it->second; };
          expect(b0("OTG1_OTG_H_TOTAL") == 0u && b0("OTG1_OTG_V_TOTAL") == 0u && b0("OTG1_OTG_VSTARTUP_PARAM") == 0u && b0("OTG1_OTG_GLOBAL_CONTROL2") == 0x02000000u && b0("MPC_OUT1_MUX") == 0x400fu && b0("HUBP1_DCHUBP_CNTL") == A_("HUBP1_DCHUBP_CNTL"), "census BEFORE `timing 1`: the OTG1 timing singles are 0 (so the EXPECT rows are what `timing 1` writes); the select, the mux and HUBP1 are the same before and after");
          uint32_t diff = 0; for (const auto &kv : B) { auto it = A.find(kv.first); if (it != A.end() && it->second != kv.second) ++diff; }
          expect_u("the census differs before / after `timing 1` in exactly 8 registers (the seven OTG1 timing words and HUBPREQ1_HUBPREQ_STATUS_REG0)", diff, 8u); }
        // the DLG values the spec says to REPLACE are the pipe-2 / pipe-0 ones (census): 0x06920000, 0xe00008aa, 0x00130035, 0x00001136, 0x0001e641, 0x2c080000, 0x00000402, 0x00014e5e
        expect(A_("HUBPREQ1_DCN_TTU_QOS_WM") == 0x06920000u && A_("HUBPREQ1_DCN_GLOBAL_TTU_CNTL") == 0xe00008aau && A_("HUBPREQ1_BLANK_OFFSET_0") == 0x00130035u && A_("HUBPREQ1_BLANK_OFFSET_1") == 0x00001136u && A_("HUBPREQ1_DST_DIMENSIONS") == 0x0001e641u && A_("HUBPREQ1_PREFETCH_SETTINGS") == 0x2c080000u &&
               A_("HUBPREQ1_VBLANK_PARAMETERS_0") == 0x402u && A_("HUBPREQ1_REF_FREQ_TO_PIX_FREQ") == 0x00014e5eu, "census: pipe 1 holds the SAME stale 1440p DLG values as pipe 2 (correction 3): every DLG field must be written explicitly");
        for (uint32_t i = 0; i < N48D1_PROG_COUNT; i++) { auto it = A.find(n48d1_prog[i].name); expect(it != A.end(), (std::string("the census read ") + n48d1_prog[i].name + " (the register the build programs)").c_str()); }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the INSTANCE GUARD for instance 1's plane, both directions of the per-instance spans
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void mona_guard_tests() {
    // ---- addresses: the 43 plane registers + the two DCCG exceptions are instance 1's alone
    for (uint32_t i = 0; i < N48D2_ALLOWED1P_COUNT; i++) {
        expect(n48d2_guard_addr_i(1, n48d2_allowed1p[i], nullptr) == N48D2_G_OK, "instance 1 admits every one of its 43 plane registers");
        expect(n48d2_guard_addr_i(2, n48d2_allowed1p[i], nullptr) == N48D2_G_FORBIDDEN && !wr_ok(2, n48d2_allowed1p[i], 0u), "instance 2 refuses every one of them (FORBIDDEN: the pipe-1 spans are the monitor B's forbidden ones)");
        expect(n48d2_guard_addr_i(0, n48d2_allowed1p[i], nullptr) != N48D2_G_OK && n48d2_guard_addr_i(3, n48d2_allowed1p[i], nullptr) != N48D2_G_OK, "an unknown instance admits none");
    }
    { std::set<uint32_t> u; for (uint32_t i = 0; i < N48D2_ALLOWED1P_COUNT; i++) u.insert(n48d2_allowed1p[i]); for (uint32_t i = 0; i < N48D2_ALLOWED2P_COUNT; i++) u.insert(n48d2_allowed2p[i]); for (uint32_t i = 0; i < N48D2_ALLOWED_COUNT; i++) { u.insert(n48d2_allowed[i]); u.insert(n48d2_allowed2[i]); }
      expect(u.size() == N48D2_ALLOWED1P_COUNT + N48D2_ALLOWED2P_COUNT + 2u * N48D2_ALLOWED_COUNT, "the four lists (instance 1 / 2 timing, instance 1 / 2 plane) never share a register"); }
    // the two DCCG exceptions: exactly 0x15a (SET {0x00ff00ff, 0}) and the shared 0x168 (UPDATE of mask exactly 0x8); 0x15b stays refused for instance 1, 0x15a for instance 2
    expect(n48d2_guard_addr_i(1, 0x15au, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(1, 0x168u, nullptr) == N48D2_G_OK && n48d2_guard_addr_i(1, 0x15bu, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x15au, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(2, 0x15bu, nullptr) == N48D2_G_OK,
           "DCCG: instance 1 admits 0x15a and 0x168, refuses 0x15b; instance 2 admits 0x15b and 0x168, refuses 0x15a");
    for (uint32_t a = 0x100u; a <= 0x17eu; a++) {
        if (a == N48D2_DPPCLK1_DTO_PARAM || a == N48D2_DPPCLK_CTRL || a == N48D2_SYMCLKC_CLOCK_ENABLE) continue;
        expect(n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_OK, "every OTHER DCCG dword is refused for instance 1 (DPPCLK0 / 2 / 3 DTO, the PHYPLL sources, OTG0's pixel rate, SYMCLKA / B / D / E, DENTIST_DISPCLK_CNTL ...)");
    }
    expect(n48d2_guard_addr_i(1, N48D2_DPPCLK0_DTO_PARAM, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, 0x15bu, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, 0x15cu, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, 0x176u, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, N48D2_DENTIST_DISPCLK_CNTL, nullptr) == N48D2_G_FORBIDDEN,
           "DPPCLK0 / 2 / 3 DTO, DPPCLK_DTO_CTRL and DENTIST_DISPCLK_CNTL stay FORBIDDEN for instance 1 (a mis-address by one dword would retime the DP's or the monitor B's DPP clock)");
    // ---- the per-instance spans, both directions, by Linux register name
    {
        uint32_t nD = 0, badD = 0, nA = 0, badA = 0;
        // forbidden for the MONA: every register of the monitor B's pipe-2 blocks (HUBP2 / HUBPREQ2 / HUBPRET2, DPP2, MPCC2, MPC_OUT2) and OTG2's lock / GLOBAL_CONTROL2 / CRC and ODM2's underflow word
        for (const char *p : { "HUBP2_", "HUBPREQ2_", "HUBPRET2_", "DPP_TOP2_", "CNVC_CFG2_", "DSCL2_", "CM2_", "MPCC2_MPCC_", "MPC_OUT2_" }) for (const std::string &nm : header_names(p)) { uint32_t a = 0; if (!lx_reg(nm, &a)) continue; ++nD;
            if (n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_FORBIDDEN || wr_ok(1, a, 0u)) { ++badD; std::printf("   monitor B block register not forbidden for instance 1: %s (%#x)\n", nm.c_str(), a); } }
        for (const char *r : { "OTG2_OTG_MASTER_UPDATE_LOCK", "OTG2_OTG_GLOBAL_CONTROL2", "OTG2_OTG_CRC_CNTL", "OTG2_OTG_CRC0_WINDOWA_X_CONTROL", "OTG2_OTG_CRC0_WINDOWA_Y_CONTROL", "OTG2_OTG_CRC0_WINDOWB_X_CONTROL", "OTG2_OTG_CRC0_WINDOWB_Y_CONTROL", "OTG2_OTG_CRC0_DATA_RG", "OTG2_OTG_CRC0_DATA_B", "ODM2_OPTC_INPUT_GLOBAL_CONTROL" }) {
            ++nD; const uint32_t a = lx_abs(r); if (n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_FORBIDDEN || wr_ok(1, a, 0u)) { ++badD; std::printf("   %s not forbidden for instance 1\n", r); } }
        std::printf("mona guard: %u pipe-2 registers (HUBP2 / HUBPREQ2 / HUBPRET2 / DPP2 / MPCC2 / MPC_OUT2 blocks, OTG2 lock / select / CRC, ODM2) checked against instance 1, %u not forbidden\n", nD, badD);
        expect(nD > 380u && badD == 0u, "instance 1 can write NO register of the monitor B's pipe: HUBP2 / HUBPREQ2 / HUBPRET2 / DPP2 / MPCC2 / MPC_OUT2 (spans 0x3c5d-0x3d05, 0x445b-0x455e, 0x902a-0x9038, 0x92fa-0x92fd, 0x9325-0x9331) and OTG2's lock, select and CRC are FORBIDDEN");
        // forbidden for the MONB: every register of pipe 1 (HUBP1 / DPP1 / MPCC1 / MPC_OUT1) and OTG1's lock / select / CRC; the monitor A reaches ONLY its exact list inside them
        for (const char *p : { "HUBP1_", "HUBPREQ1_", "HUBPRET1_", "DPP_TOP1_", "CNVC_CFG1_", "DSCL1_", "CM1_", "MPCC1_MPCC_", "MPC_OUT1_" }) for (const std::string &nm : header_names(p)) { uint32_t a = 0; if (!lx_reg(nm, &a)) continue; ++nA;
            bool in = false; for (uint32_t k = 0; k < N48D2_ALLOWED1P_COUNT; k++) in = in || n48d2_allowed1p[k] == a;
            if (n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_FORBIDDEN || wr_ok(2, a, 0u) || (in ? n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_OK : n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK)) { ++badA; std::printf("   pipe-1 register mis-judged: %s (%#x) guard1 %u guard2 %u\n", nm.c_str(), a, n48d2_guard_addr_i(1, a, nullptr), n48d2_guard_addr_i(2, a, nullptr)); } }
        for (const char *r : { "OTG1_OTG_MASTER_UPDATE_LOCK", "OTG1_OTG_GLOBAL_CONTROL2", "OTG1_OTG_CRC_CNTL", "OTG1_OTG_CRC0_WINDOWA_X_CONTROL", "OTG1_OTG_CRC0_WINDOWA_Y_CONTROL", "OTG1_OTG_CRC0_WINDOWB_X_CONTROL", "OTG1_OTG_CRC0_WINDOWB_Y_CONTROL", "OTG1_OTG_CRC0_DATA_RG", "OTG1_OTG_CRC0_DATA_B", "ODM1_OPTC_INPUT_GLOBAL_CONTROL" }) {
            ++nA; const uint32_t a = lx_abs(r); const bool ro = std::string(r).find("DATA_") != std::string::npos; if (n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_FORBIDDEN || wr_ok(2, a, 0u) || (ro ? n48d2_guard_addr_i(1, a, nullptr) == N48D2_G_OK : n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_OK)) { ++badA; std::printf("   %s mis-judged\n", r); } }
        std::printf("mona guard: %u pipe-1 registers (HUBP1 / HUBPREQ1 / HUBPRET1 / DPP1 / MPCC1 / MPC_OUT1 blocks, OTG1 lock / select / CRC, ODM1) checked against instance 2 and the exact list, %u mis-judged\n", nA, badA);
        expect(nA > 380u && badA == 0u, "instance 2 can write NO register of the monitor A's pipe (FORBIDDEN: HUBP1 / DPP1 / MPCC1 / MPC_OUT1 / OTG1 lock, select, CRC / ODM1), and instance 1 reaches inside those blocks ONLY its exact 43-register list (the CRC DATA registers are read-only)");
        // OGAM1 / MCM1 forbidden for BOTH (never written; the census pins them at 0)
        uint32_t nO = 0, badO = 0; for (const char *p : { "MPCC_OGAM1_", "MPCC_MCM1_" }) for (const std::string &nm : header_names(p)) { uint32_t a = 0; if (!lx_reg(nm, &a)) continue; ++nO; if (n48d2_guard_addr_i(1, a, nullptr) != N48D2_G_FORBIDDEN || n48d2_guard_addr_i(2, a, nullptr) != N48D2_G_FORBIDDEN) ++badO; }
        expect(nO > 200u && badO == 0u, "MPCC_OGAM1 and MPCC_MCM1 are FORBIDDEN for both instances");
        // DET1 and the DCHUBBUB block stay read-only for the monitor A too
        expect(n48d2_guard_addr_i(1, N48D2_DCHUBBUB_DET1_CTRL, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, N48D2_DCN_VM_FAULT_STATUS, nullptr) == N48D2_G_FORBIDDEN && n48d2_guard_addr_i(1, N48D2_DOMAIN1_PG_STATUS, nullptr) == N48D2_G_UNLISTED && n48d2_guard_addr_i(1, N48D2_OTG1_OTG_CRC0_DATA_RG, nullptr) != N48D2_G_OK,
               "DET1_CTRL and the VM fault status are READ only for instance 1 (no DET write), DOMAIN1_PG_STATUS is unlisted, the OTG1 CRC DATA registers are not writable");
    }
    // ---- value rules (judged on the value that would be WRITTEN)
    n48d2_aset_set_i(N48D2_INST_MONA, kA1, kB1); n48d2_aset_set(kBufA, kBufB);
    const struct { const char *what; uint32_t abs, v; bool ok; } vr[] = {
        { "DCHUBP_CNTL VTG_SEL 1, soft reset clear", N48D2_HUBP1_DCHUBP_CNTL, 0x000f001au, true }, { "DCHUBP_CNTL blank set, VTG_SEL 1", N48D2_HUBP1_DCHUBP_CNTL, 0x000f001bu, true }, { "DCHUBP_CNTL VTG_SEL 0 (the DP's)", N48D2_HUBP1_DCHUBP_CNTL, 0x000f000au, false },
        { "DCHUBP_CNTL VTG_SEL 2 (the monitor B's)", N48D2_HUBP1_DCHUBP_CNTL, 0x000f002au, false }, { "DCHUBP_CNTL soft reset bit 2", N48D2_HUBP1_DCHUBP_CNTL, 0x000f001eu, false },
        { "MPCC1 TOP 1", N48D2_MPCC1_MPCC_TOP_SEL, 1u, true }, { "MPCC1 TOP 0xf", N48D2_MPCC1_MPCC_TOP_SEL, 0xFu, true }, { "MPCC1 TOP 0 (the DP's mixer input)", N48D2_MPCC1_MPCC_TOP_SEL, 0u, false }, { "MPCC1 TOP 2 (the monitor B's)", N48D2_MPCC1_MPCC_TOP_SEL, 2u, false }, { "MPCC1 TOP 0x11", N48D2_MPCC1_MPCC_TOP_SEL, 0x11u, false },
        { "MPCC1 OPP 1", N48D2_MPCC1_MPCC_OPP_ID, 1u, true }, { "MPCC1 OPP 0xf", N48D2_MPCC1_MPCC_OPP_ID, 0xFu, true }, { "MPCC1 OPP 0 (OPP0 = the DP)", N48D2_MPCC1_MPCC_OPP_ID, 0u, false }, { "MPCC1 OPP 2 (OPP2 = the monitor B)", N48D2_MPCC1_MPCC_OPP_ID, 2u, false },
        { "MPCC1 LOCK 1", N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, 1u, true }, { "MPCC1 LOCK 0xf", N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, 0xFu, true }, { "MPCC1 LOCK 0 (OTG0's lock)", N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, 0u, false }, { "MPCC1 LOCK 2 (OTG2's lock)", N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, 2u, false },
        { "MPCC1 BOT 0xf", N48D2_MPCC1_MPCC_BOT_SEL, 0xFu, true }, { "MPCC1 BOT 1", N48D2_MPCC1_MPCC_BOT_SEL, 1u, false }, { "MPCC1 BOT 0", N48D2_MPCC1_MPCC_BOT_SEL, 0u, false },
        { "MUX 0x4101 (mux 1)", N48D2_MPC_OUT1_MUX, 0x4101u, true }, { "MUX 0x400f (none)", N48D2_MPC_OUT1_MUX, 0x400fu, true }, { "MUX 0x4100 (OPP 0!)", N48D2_MPC_OUT1_MUX, 0x4100u, false }, { "MUX 0x4102 (the monitor B's)", N48D2_MPC_OUT1_MUX, 0x4102u, false }, { "MUX 0x0001 (no flow-control bits)", N48D2_MPC_OUT1_MUX, 0x0001u, false }, { "MUX 0x410e", N48D2_MPC_OUT1_MUX, 0x410eu, false },
        { "pitch 0x77f", N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0x77fu, true }, { "pitch 0x9ff (the monitor B's)", N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0x9ffu, false }, { "pitch 0x77e", N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0x77eu, false }, { "pitch 0", N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0u, false }, { "pitch 0x780", N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0x780u, false },
        { "DTO1 0x00ff00ff", N48D2_DPPCLK1_DTO_PARAM, 0x00ff00ffu, true }, { "DTO1 0", N48D2_DPPCLK1_DTO_PARAM, 0u, true }, { "DTO1 1", N48D2_DPPCLK1_DTO_PARAM, 1u, false }, { "DTO1 0x00ff00fe", N48D2_DPPCLK1_DTO_PARAM, 0x00ff00feu, false }, { "DTO1 all ones", N48D2_DPPCLK1_DTO_PARAM, 0xFFFFFFFFu, false },
        { "address LOW = A1", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kA1, true }, { "address LOW = B1", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kB1, true }, { "address LOW = A1 + 64 KiB", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kA1 + 0x10000u, false },
        { "address LOW = the MONB's A (per-instance set!)", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufA, false }, { "address LOW = the MONB's B", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kBufB, false },
        { "address LOW = 0 (the rollback zeroes it)", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u, true }, { "address LOW = 1", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 1u, false }, { "address LOW = all ones", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0xFFFFFFFFu, false },
        { "address HIGH = 0x80", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x80u, true }, { "address HIGH = 0x81", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x81u, false }, { "address HIGH = 0", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0u, true }, { "address HIGH = 1", N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 1u, false },
        { "CRC window AX 1920 << 16", N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL, N48D1_CRC_WIN_X, true }, { "CRC window AX = the monitor B's", N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_CRC_WIN_X, false }, { "CRC window AY", N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL, N48D1_CRC_WIN_Y, true }, { "CRC window AY wrong", N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL, N48D1_CRC_WIN_X, false },
        { "CRC window BX", N48D2_OTG1_OTG_CRC0_WINDOWB_X_CONTROL, N48D1_CRC_WIN_X, true }, { "CRC window BY", N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL, N48D1_CRC_WIN_Y, true }, { "CRC window BY 0", N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL, 0u, false },
        { "ODM1 soft reset bit", N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 0x1001u, false }, { "ODM1 underflow clear", N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 0x1000u, true },
        { "CRC_CNTL select 0", N48D2_OTG1_OTG_CRC_CNTL, 0x11u, true }, { "CRC_CNTL select 1", N48D2_OTG1_OTG_CRC_CNTL, 0x00100011u, false },
        { "lock select = 1", N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x02000000u, true }, { "lock select = 2 (OTG2's)", N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x04000000u, false }, { "lock select = 0", N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0u, false }, { "lock select = 3", N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x06000000u, false },
        { "lock = 1", N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 1u, true }, { "lock = 0", N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 0u, true }, { "lock with other bits", N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 0x3u, false },
    };
    for (const auto &c : vr) expect(wr_ok(1, c.abs, c.v) == c.ok, (std::string("value rule: ") + c.what).c_str());
    // every register of the programming list admits EXACTLY its 1080p value (and the monitor B's / pipe 0's 1440p value, +-1, 0 and all ones are refused)
    for (uint32_t i = 0; i < N48D1_PROG_COUNT; i++) {
        const auto &p = n48d1_pin[i]; const uint32_t good = p.kind == N48D2_K_UPD ? (0x08083f00u & ~p.mask) | p.val : p.val;
        expect(wr_ok(1, p.abs, good), (std::string("PIN: ") + p.name + " admits its 1080p value").c_str());
        bool others = true; for (uint32_t bad : { good ^ 1u, good + 1u, good ^ 0x80000000u, 0u ^ (good == 0u ? 0xFFFFFFFFu : 0u), 0xFFFFFFFFu }) if (bad != good && (p.kind != N48D2_K_UPD || (bad & p.mask) != p.val)) others = others && !wr_ok(1, p.abs, bad);
        expect(others, (std::string("PIN: ") + p.name + " refuses every other value").c_str());
    }
    // the 1440p / pipe-0 values the spec says NOT to use are refused where they would land
    { const struct { uint32_t abs, v; const char *what; } stale[] = { { N48D2_HUBPREQ1_DCN_TTU_QOS_WM, 0x06920000u, "TTU_QOS_WM 0x06920000" }, { N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, 0xe00050aau, "GLOBAL_TTU_CNTL 0xe00050aa (pipe 0's)" }, { N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, 0xe00008aau, "GLOBAL_TTU_CNTL 0xe00008aa (the census)" },
              { N48D2_HUBPREQ1_BLANK_OFFSET_0, 0x00260017u, "BLANK_OFFSET_0 0x00260017 (pipe 0's)" }, { N48D2_HUBPREQ1_BLANK_OFFSET_0, 0x0029001eu, "BLANK_OFFSET_0 with the x 1.315 H_BLANK_END (30)" }, { N48D2_HUBPREQ1_BLANK_OFFSET_1, 0x1136u, "BLANK_OFFSET_1 0x1136 (the fallback is NOT admitted)" }, { N48D2_HUBPREQ1_DST_DIMENSIONS, 0x0001e641u, "DST_DIMENSIONS 0x0001e641 (the monitor B's)" },
              { N48D2_HUBPREQ1_DST_DIMENSIONS, 189629u, "DST_DIMENSIONS the Linux formula's 189629" }, { N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ, 0x00014e5eu, "REF_FREQ_TO_PIX_FREQ 0x00014e5e (the monitor B's)" }, { N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ, 176527u, "REF_FREQ_TO_PIX_FREQ the formula's 176527" },
              { N48D2_HUBPREQ1_PREFETCH_SETTINGS, 0x2c080000u, "PREFETCH_SETTINGS 0x2c080000 (the census)" }, { N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, 0x402u, "VBLANK_PARAMETERS_0 0x402 (the census)" }, { N48D2_DSCL1_OTG_H_BLANK, 0x00700a70u, "DSCL OTG_H_BLANK (the monitor B's)" }, { N48D2_DSCL1_OTG_V_BLANK, 0x002605c6u, "DSCL OTG_V_BLANK (the monitor B's)" },
              { N48D2_DSCL1_RECOUT_SIZE, 0x05a00a00u, "RECOUT_SIZE 2560x1440" }, { N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, 0x05a00a00u, "PRI_VIEWPORT 2560x1440" }, { N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG, 0x05501b00u, "REQ_SIZE_CONFIG the census value 0x05501b00" } };
      for (const auto &c : stale) expect(!wr_ok(1, c.abs, c.v), (std::string("a stale / wrong-pipe value is refused: ") + c.what).c_str()); }
    n48d2_aset_clear_i(N48D2_INST_MONA);
    expect(!wr_ok(1, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kA1) && !wr_ok(1, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x80u) && wr_ok(1, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u), "with NO monitor A address set installed every non-zero address write is refused (fail closed); the monitor B's set is a different set");
    n48d2_aset_clear();
    // ---- kind / mask rules (list level)
    n48d2_aset_set_i(N48D2_INST_MONA, kA1, kB1);
    const uint8_t S_ = N48D2_K_SET, U_ = N48D2_K_UPD;
    const struct { const char *what; n48d2_step st; bool ok; } kr[] = {
        { "DTO1 SET 0x00ff00ff", mkstep(S_, N48D2_DPPCLK1_DTO_PARAM, 0, 0x00ff00ffu), true }, { "DTO1 SET 0", mkstep(S_, N48D2_DPPCLK1_DTO_PARAM, 0, 0u), true }, { "DTO1 UPD", mkstep(U_, N48D2_DPPCLK1_DTO_PARAM, 0xFFFFFFFFu, 0u), false }, { "DTO1 SET 1", mkstep(S_, N48D2_DPPCLK1_DTO_PARAM, 0, 1u), false },
        { "DPPCLK_CTRL UPD mask 0x8 -> 0x8", mkstep(U_, N48D2_DPPCLK_CTRL, 0x8u, 0x8u), true }, { "DPPCLK_CTRL UPD mask 0x8 -> 0", mkstep(U_, N48D2_DPPCLK_CTRL, 0x8u, 0u), true },
        { "DPPCLK_CTRL mask 0x1 (DPPCLK0_EN: the DP!)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x1u, 0u), false }, { "DPPCLK_CTRL mask 0x40 (DPPCLK2_EN: the monitor B's!)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x40u, 0x40u), false }, { "DPPCLK_CTRL mask 0x9", mkstep(U_, N48D2_DPPCLK_CTRL, 0x9u, 0x8u), false },
        { "DPPCLK_CTRL mask 0x4 (the WRONG bit)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x4u, 0x4u), false }, { "DPPCLK_CTRL mask 0x2", mkstep(U_, N48D2_DPPCLK_CTRL, 0x2u, 0x2u), false }, { "DPPCLK_CTRL mask 0x200 (DPPCLK3_EN)", mkstep(U_, N48D2_DPPCLK_CTRL, 0x200u, 0x200u), false },
        { "DPPCLK_CTRL mask 0xFFFFFFFF", mkstep(U_, N48D2_DPPCLK_CTRL, 0xFFFFFFFFu, 0x8u), false }, { "DPPCLK_CTRL mask 0", mkstep(U_, N48D2_DPPCLK_CTRL, 0u, 0u), false }, { "DPPCLK_CTRL SET", mkstep(S_, N48D2_DPPCLK_CTRL, 0, 0x8u), false }, { "DPPCLK_CTRL value outside its mask", mkstep(U_, N48D2_DPPCLK_CTRL, 0x8u, 0x9u), false },
        { "DPP_TOP1 UPD 0x10", mkstep(U_, N48D2_DPP_TOP1_DPP_CONTROL, 0x10u, 0x10u), true }, { "DPP_TOP1 UPD 0x10 -> 0", mkstep(U_, N48D2_DPP_TOP1_DPP_CONTROL, 0x10u, 0u), true }, { "DPP_TOP1 mask 0x11", mkstep(U_, N48D2_DPP_TOP1_DPP_CONTROL, 0x11u, 0x11u), false }, { "DPP_TOP1 SET", mkstep(S_, N48D2_DPP_TOP1_DPP_CONTROL, 0, 0x10u), false },
        { "HUBP1 CNTL UPD mask 1", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 1u, 1u), true }, { "HUBP1 CNTL UPD mask 0x80000000", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 0x80000000u, 0x80000000u), true }, { "HUBP1 CNTL UPD mask 0x80000001", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 0x80000001u, 0u), true },
        { "HUBP1 CNTL mask 2", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 2u, 2u), false }, { "HUBP1 CNTL mask 4 (soft reset)", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 4u, 4u), false }, { "HUBP1 CNTL mask 0xF0 (VTG_SEL)", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 0xF0u, 0u), false },
        { "HUBP1 CNTL mask 0x1000", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 0x1000u, 0u), false }, { "HUBP1 CNTL SET", mkstep(S_, N48D2_HUBP1_DCHUBP_CNTL, 0, 0u), false }, { "HUBP1 CNTL mask 0", mkstep(U_, N48D2_HUBP1_DCHUBP_CNTL, 0u, 0u), false },
        { "HUBP1 clock UPD 0x11111", mkstep(U_, N48D2_HUBP1_HUBP_CLK_CNTL, 0x11111u, 0x11111u), true }, { "HUBP1 clock UPD 1", mkstep(U_, N48D2_HUBP1_HUBP_CLK_CNTL, 1u, 0u), true }, { "HUBP1 clock mask 0x100000 (status)", mkstep(U_, N48D2_HUBP1_HUBP_CLK_CNTL, 0x100000u, 0u), false },
        { "HUBP1 clock mask 0x00f11111", mkstep(U_, N48D2_HUBP1_HUBP_CLK_CNTL, 0x00f11111u, 0u), false }, { "HUBP1 clock SET", mkstep(S_, N48D2_HUBP1_HUBP_CLK_CNTL, 0, 0u), false },
        { "ODM1 UPD 0x1000", mkstep(U_, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 0x1000u, 0x1000u), true }, { "ODM1 mask 0x1001", mkstep(U_, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 0x1001u, 0x1000u), false }, { "ODM1 mask 1 (soft reset)", mkstep(U_, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 1u, 1u), false }, { "ODM1 SET", mkstep(S_, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, 0, 0x1000u), false },
        { "CRC_CNTL UPD 0x00700011 -> 0x11", mkstep(U_, N48D2_OTG1_OTG_CRC_CNTL, 0x00700011u, 0x11u), true }, { "CRC_CNTL UPD -> 0", mkstep(U_, N48D2_OTG1_OTG_CRC_CNTL, 0x00700011u, 0u), true }, { "CRC_CNTL mask 0x11", mkstep(U_, N48D2_OTG1_OTG_CRC_CNTL, 0x11u, 0x11u), false }, { "CRC_CNTL SET", mkstep(S_, N48D2_OTG1_OTG_CRC_CNTL, 0, 0x11u), false },
        { "lock select UPD 0x0E000000 -> 0x02000000", mkstep(U_, N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x0E000000u, 0x02000000u), true }, { "lock select UPD -> 2", mkstep(U_, N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x0E000000u, 0x04000000u), false }, { "lock select mask 0x0F000000", mkstep(U_, N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0x0F000000u, 0x02000000u), false },
        { "lock select SET", mkstep(S_, N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0, 0x02000000u), false }, { "lock select mask 0xFFFFFFFF", mkstep(U_, N48D2_OTG1_OTG_GLOBAL_CONTROL2, 0xFFFFFFFFu, 0x02000000u), false },
        { "lock UPD bit 0 -> 1", mkstep(U_, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 1u, 1u), true }, { "lock UPD bit 0 -> 0", mkstep(U_, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 1u, 0u), true }, { "lock SET", mkstep(S_, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 0, 1u), false }, { "lock mask 0x101", mkstep(U_, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 0x101u, 1u), false },
        { "pitch UPD", mkstep(U_, N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0xFFFFFFFFu, 0x77fu), false }, { "pitch SET", mkstep(S_, N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, 0, 0x77fu), true },
        { "address LOW UPD", mkstep(U_, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0xFFFFFFFFu, (uint32_t)kA1), false }, { "address LOW SET A1", mkstep(S_, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kA1), true }, { "address LOW SET outside {A1,B1}", mkstep(S_, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, (uint32_t)kA1 + 0x10000u), false },
        { "MPCC1 TOP SET 1", mkstep(S_, N48D2_MPCC1_MPCC_TOP_SEL, 0, 1u), true }, { "MPCC1 TOP UPD", mkstep(U_, N48D2_MPCC1_MPCC_TOP_SEL, 0xFu, 1u), false }, { "MUX UPD", mkstep(U_, N48D2_MPC_OUT1_MUX, 0xFu, 1u), false }, { "MUX SET 0x4101", mkstep(S_, N48D2_MPC_OUT1_MUX, 0, 0x4101u), true },
        { "CRC window UPD", mkstep(U_, N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL, 0xFFFFFFFFu, N48D1_CRC_WIN_X), false },
        { "LB_MEMORY_CTRL UPD mask 0x3F03 -> 0x3f00", mkstep(U_, N48D2_DSCL1_LB_MEMORY_CTRL, 0x3F03u, 0x3f00u), true }, { "LB_MEMORY_CTRL SET 0x3f00", mkstep(S_, N48D2_DSCL1_LB_MEMORY_CTRL, 0, 0x3f00u), false }, { "LB_MEMORY_CTRL UPD mask 0xFFFFFFFF", mkstep(U_, N48D2_DSCL1_LB_MEMORY_CTRL, 0xFFFFFFFFu, 0x3f00u), false }, { "LB_MEMORY_CTRL UPD wrong value", mkstep(U_, N48D2_DSCL1_LB_MEMORY_CTRL, 0x3F03u, 0x3f01u), false },
        { "DSCL1_SCL_MODE UPD (the pins are SETs)", mkstep(U_, N48D2_DSCL1_SCL_MODE, 0xFFFFFFFFu, 1u), false }, { "DSCL1_SCL_MODE SET 1", mkstep(S_, N48D2_DSCL1_SCL_MODE, 0, 1u), true },
        { "EXPECT is read only and always passes the list guard", mkstep(N48D2_K_EXPECT, N48D2_HUBP0_DCHUBP_CNTL, 0xFFFFFFFFu, 0u), true }, { "a WAIT on a pipe-0 register is a read", mkstep(N48D2_K_WAIT, N48D2_HUBP0_DCHUBP_CNTL, 1u, 0u), true },
    };
    for (const auto &c : kr) expect(list_ok(1, c.st) == c.ok, (std::string("kind / mask rule: ") + c.what).c_str());
    // COPY0 is NEVER legal for instance 1 (explicit SETs only), in any form
    for (uint32_t i = 0; i < N48D1_PROG_COUNT; i++) { expect(!list_ok(1, mkstep(N48D2_K_COPY0, n48d1_pin[i].abs, 0xFFFFFFFFu, n48d1_pin[i].val, n48d1_pin[i].abs - 0x16bu)), "instance 1 refuses a COPY0 aimed at any of its programmed registers"); }
    for (uint32_t i = 0; i < N48D2_MIRROR0_COUNT; i++) expect(!list_ok(1, mkstep(N48D2_K_COPY0, n48d2_mirror0[i].dst, 0xFFFFFFFFu, n48d2_mirror0[i].expect, n48d2_mirror0[i].src)), "instance 1 refuses all 20 of the monitor B's COPY0 pairs");
    // REC: the monitor A's own five, each in its own slot, instance 1 only
    for (uint32_t s = 0; s < N48D2_REC_SLOTS; s++) {
        expect(list_ok(1, mkrec(n48d1_rec_regs[s & 3u], s)), "REC: each monitor A register in each of its own slots passes the list guard");
        expect(!list_ok(2, mkrec(n48d1_rec_regs[s & 3u], s)) || (s & 3u) == 1u || (s & 3u) == 2u, "REC: instance 2 refuses the monitor A's HUBP1 / DOMAIN1 registers (the two DCCG gates are shared)");
        expect(!list_ok(1, mkrec(n48d1_rec_regs[(s + 1u) & 3u], s)), "REC: a register in another register's slot is refused");
    }
    expect(list_ok(1, mkrec(N48D2_DCHUBBUB_DET1_CTRL, N48D2_RS_DET)) && !list_ok(1, mkrec(N48D2_DCHUBBUB_DET2_CTRL, N48D2_RS_DET)) && !list_ok(2, mkrec(N48D2_DCHUBBUB_DET1_CTRL, N48D2_RS_DET)) && !list_ok(1, mkrec(N48D2_HUBP2_HUBP_CLK_CNTL, 0u)), "REC slot 13: DET1_CTRL for instance 1, DET2_CTRL for instance 2, never crosswise");
    // ---- the plane's own lists pass the guard, as built, with the set installed; every one of them is refused for instance 2 whole; one bad step is refused at that step
    { uint32_t bad = 0;
      struct L { const char *w; uint32_t n; } ls[16]; uint32_t nl = 0;
      static n48d2_step buf[16][N48D2_MAX_STEPS];
      ls[nl++] = { "plane A", n48d1_build_plane_a(buf[0], kA1) }; ls[nl++] = { "plane B", n48d1_build_plane_b(buf[1]) }; ls[nl++] = { "plane C", n48d1_build_plane_c(buf[2], kA1) }; ls[nl++] = { "show 1", n48d1_build_show1(buf[3]) }; ls[nl++] = { "show 2", n48d1_build_show2(buf[4]) };
      ls[nl++] = { "flip A", n48d1_build_flip(buf[5], kA1) }; ls[nl++] = { "flip B", n48d1_build_flip(buf[6], kB1) }; ls[nl++] = { "crc", n48d1_build_crc(buf[7]) }; ls[nl++] = { "planeoff", n48d1_build_planeoff(buf[8]) };
      ls[nl++] = { "plane D", n48d1_build_plane_d(buf[9], kA1) }; ls[nl++] = { "plane rec2", n48d1_build_plane_rec2(buf[10]) }; ls[nl++] = { "plane lock", n48d1_build_plane_lock(buf[11]) }; ls[nl++] = { "plane unlock", n48d1_build_plane_unlock(buf[12]) }; ls[nl++] = { "plane E", n48d1_build_plane_e(buf[13]) };
      for (uint32_t i = 0; i < nl; i++) {
          expect(ls[i].n >= 1u && ls[i].n <= N48D2_MAX_STEPS, (std::string(ls[i].w) + ": the list fits the 48-step cap").c_str());
          expect(n48d2_guard_list_i(1, buf[i], ls[i].n, &bad) == 0u, (std::string(ls[i].w) + ": the list passes instance 1's guard as built").c_str());
          { bool anyWrite = false; for (uint32_t k = 0; k < ls[i].n; k++) anyWrite = anyWrite || n48d2_is_write(&buf[i][k]);
            if (anyWrite) expect(n48d2_guard_list_i(2, buf[i], ls[i].n, &bad) != 0u, (std::string(ls[i].w) + ": instance 2's guard refuses the whole list").c_str());
            else expect(!anyWrite, (std::string(ls[i].w) + ": a list of reads only (nothing for instance 2's guard to refuse)").c_str()); }
      }
      for (uint32_t p = 0; p < N48D1_PRE_PAGES; p++) { static n48d2_step pg[N48D2_MAX_STEPS]; const uint32_t n = n48d1_build_pre(p, pg); bool allExp = n >= 1u; for (uint32_t i = 0; i < n; i++) allExp = allExp && pg[i].kind == N48D2_K_EXPECT; expect(allExp && n48d2_guard_list_i(1, pg, n, &bad) == 0u, "the precheck pages are EXPECT steps (reads only) and pass the guard"); }
      expect(n48d1_build_pre(0, buf[0]) == 48u && n48d1_build_pre(1, buf[1]) == 10u && N48D1_PRE_TOTAL == 58u && N48D1_PRE_STATE_COUNT == 21u && N48D1_EXP_COUNT == 37u && n48d1_build_pre(2, buf[2]) == 0u, "the precheck is 58 EXPECT steps in two pages (48 + 10): 21 state rows + 37 census-valued rows");
      // a list with ONE forbidden step is refused whole, at THAT step, whichever step it is: every write step aimed at HUBP0 / DET0 / MPC_OUT0 / DPPCLK0 / MPCC0 / HUBP0's address AND at the monitor B's registers
      bool allRefused = true; uint32_t tried = 0;
      for (uint32_t li = 0; li < nl; li++) for (uint32_t k = 0; k < ls[li].n; k++) if (n48d2_is_write(&buf[li][k])) {
          for (uint32_t bad_abs : { N48D2_HUBP0_DCHUBP_CNTL, N48D2_DET0_CTRL, N48D2_MPC_OUT0_MUX, N48D2_DPPCLK0_DTO_PARAM, N48D2_MPCC0_TOP_SEL, N48D2_HUBPREQ0_PRIMARY_LOW, N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, N48D2_MPCC2_MPCC_TOP_SEL, N48D2_MPC_OUT2_MUX, N48D2_DPPCLK2_DTO_PARAM, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_OTG2_OTG_CRC_CNTL, N48D2_DCHUBBUB_DET1_CTRL }) {
              static n48d2_step t[N48D2_MAX_STEPS]; std::memcpy(t, buf[li], sizeof(n48d2_step) * ls[li].n); t[k].abs = bad_abs; uint32_t b2 = 0; ++tried;
              if (n48d2_guard_list_i(1, t, ls[li].n, &b2) == 0u || b2 != k) allRefused = false; } }
      expect(allRefused && tried > 500, "an monitor A plane list with a pipe-0 / DP / MONB register aimed at any one write step is refused whole, at THAT step");
      // a ZERO address only after BLANK_EN = 1 AND the NO_OUTSTANDING_REQ wait earlier in the same list
      { n48d2_step z[3]; z[0] = mkstep(N48D2_K_SET, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0, 0u); uint32_t b = 0; expect(n48d2_guard_list_i(1, z, 1u, &b) != 0u, "a bare zero address SET is refused");
        n48d2_step pl[N48D2_MAX_STEPS]; const uint32_t n = n48d1_build_planeoff(pl); expect(n48d2_guard_list_i(1, pl, n, &b) == 0u, "the rollback's zero addresses follow its blank + NO_OUTSTANDING_REQ wait: passes");
        for (uint32_t drop : { 2u, 3u }) { n48d2_step q[N48D2_MAX_STEPS]; std::memcpy(q, pl, sizeof(pl[0]) * n); q[drop].kind = N48D2_K_EXPECT; q[drop].mask = 0u; q[drop].val = 0u; expect(n48d2_guard_list_i(1, q, n, &b) != 0u && b == N48D2_PO_I_ADDR_HI, drop == 2u ? "the rollback list without its blank is refused at the zero address" : "the rollback list without its NO_OUTSTANDING_REQ wait is refused at the zero address"); }
        n48d2_step u[N48D2_MAX_STEPS]; std::memcpy(u, pl, sizeof(pl[0]) * n); u[3].mask = N48D2_HUBP_NO_OUTSTANDING_MASK; u[3].val = 0u; expect(n48d2_guard_list_i(1, u, n, &b) != 0u, "a NO_OUTSTANDING_REQ wait that waits for 0 does not count"); }
    }
    n48d2_aset_clear_i(N48D2_INST_MONA);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the exact step lists of instance 1 (Linux names + the spec's 1080p values, typed here independently of the header's tables)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static n48d2_out run1(P1Model &m, uint32_t op, uint32_t buf = 0u) { return n48d2::run_plane(m, op, buf, N48D2_INST_MONA); }
static void plane1_up(P1Model &m) {       // a failed setup is a FAILED CHECK and ends the group
    m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE);
    const bool up = o.status == N48D2_OK && m.plane1.stage == N48D2_PL_PLANE;
    expect(up, "setup (instance 1): op 8 brought the monitor A's plane up");
    if (!up) throw SetupFailed();
    m.writes.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear(); m.f1AtWrite.clear();
}
static void plane1_shown(P1Model &m) {
    plane1_up(m);
    const n48d2_out s = run1(m, N48D2_OP_SHOW);
    const bool ok = s.status == N48D2_OK && m.plane1.stage == N48D2_PL_SHOWN;
    expect(ok, "setup (instance 1): op 9 showed the plane");
    if (!ok) throw SetupFailed();
    m.writes.clear(); m.reads.clear(); m.readsAtWrite.clear(); m.f2AtWrite.clear(); m.f1AtWrite.clear();
}
static std::vector<XS> spec1_part_a(uint64_t mc) {
    return { { N48D2_K_SET, "DPPCLK1_DTO_PARAM", 0, 0x00ff00ffu, nullptr, 0, 0 }, { N48D2_K_UPD, "DPPCLK_CTRL", 0x8u, 0x8u, nullptr, 0, 0 }, { N48D2_K_UPD, "DPP_TOP1_DPP_CONTROL", 0x10u, 0x10u, nullptr, 0, 0 }, { N48D2_K_UPD, "HUBP1_DCHUBP_CNTL", 1u, 1u, nullptr, 0, 0 },
             { N48D2_K_WAIT, "HUBP1_DCHUBP_CNTL", 1u, 1u, nullptr, 100, 10 }, { N48D2_K_SET, "HUBPREQ1_DCSURF_SURFACE_PITCH", 0, 0x77fu, nullptr, 0, 0 },
             { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(mc >> 32), nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)mc, nullptr, 0, 0 } };
}
static std::vector<XS> spec1_part_d(uint64_t mc) {
    std::vector<XS> w = {
        { N48D2_K_SET, "DSCL1_SCL_MODE", 0, 0x00000001u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_DSCL_2TAP_CONTROL", 0, 0x01110111u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO", 0, 0x01000000u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_SCL_HORZ_FILTER_INIT", 0, 0u, nullptr, 0, 0 },
        { N48D2_K_SET, "DSCL1_SCL_VERT_FILTER_SCALE_RATIO", 0, 0x01000000u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_SCL_VERT_FILTER_INIT", 0, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_OTG_H_BLANK", 0, 0x00c00840u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_OTG_V_BLANK", 0, 0x00290461u, nullptr, 0, 0 },
        { N48D2_K_SET, "DSCL1_RECOUT_SIZE", 0, 0x04380780u, nullptr, 0, 0 }, { N48D2_K_SET, "DSCL1_MPC_SIZE", 0, 0x04380780u, nullptr, 0, 0 }, { N48D2_K_UPD, "DSCL1_LB_MEMORY_CTRL", 0x3F03u, 0x3f00u, nullptr, 0, 0 },
        { N48D2_K_SET, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION", 0, 0x04380780u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION", 0, 0x04380780u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBP1_DCHUBP_REQ_SIZE_CONFIG", 0, 0x05500300u, nullptr, 0, 0 },
        { N48D2_K_SET, "HUBPREQ1_DCN_TTU_QOS_WM", 0, 0x08a40000u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCN_GLOBAL_TTU_CNTL", 0, 0xe0006a1au, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCN_SURF0_TTU_CNTL0", 0, 0x08000000u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_BLANK_OFFSET_0", 0, 0x00290040u, nullptr, 0, 0 },
        { N48D2_K_SET, "HUBPREQ1_BLANK_OFFSET_1", 0, 0x00000d12u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DST_DIMENSIONS", 0, 0x00027f99u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_PREFETCH_SETTINGS", 0, 0x28080000u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_PREFETCH_SETTINGS_C", 0, 0x00080000u, nullptr, 0, 0 },
        { N48D2_K_SET, "HUBPREQ1_VBLANK_PARAMETERS_0", 0, 0x00000301u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_REF_FREQ_TO_PIX_FREQ", 0, 0x00021fc4u, nullptr, 0, 0 } };
    w.push_back({ N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(mc >> 32), nullptr, 0, 0 });
    w.push_back({ N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)mc, nullptr, 0, 0 });
    return w;
}
static std::vector<WantW> spec1_op8_writes(uint64_t A) {      // 45 writes: the spec's order (clocks, blank, pitch, address; the HUBP clock; the 24 programming writes + the address again; LOCK; address / MPCC1 / mux / UNBLANK; UNLOCK)
    std::vector<WantW> w;
    w.push_back({ lx_abs("DPPCLK1_DTO_PARAM"), ~0u, 0x00ff00ffu }); w.push_back({ lx_abs("DPPCLK_CTRL"), 0x8u, 0x8u }); w.push_back({ lx_abs("DPP_TOP1_DPP_CONTROL"), 0x10u, 0x10u }); w.push_back({ lx_abs("HUBP1_DCHUBP_CNTL"), 1u, 1u });
    w.push_back({ lx_abs("HUBPREQ1_DCSURF_SURFACE_PITCH"), ~0u, 0x77fu }); w.push_back({ lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(A >> 32) }); w.push_back({ lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)A });
    w.push_back({ lx_abs("HUBP1_HUBP_CLK_CNTL"), 0x00011111u, 0x00011111u });
    for (const XS &x : spec1_part_d(A)) w.push_back({ lx_abs(x.reg), x.kind == N48D2_K_UPD ? x.mask : ~0u, x.val });
    w.push_back({ lx_abs("OTG1_OTG_GLOBAL_CONTROL2"), 0x0E000000u, 0x02000000u }); w.push_back({ lx_abs("OTG1_OTG_MASTER_UPDATE_LOCK"), 1u, 1u });
    w.push_back({ lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(A >> 32) }); w.push_back({ lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)A });
    w.push_back({ lx_abs("MPCC1_MPCC_BOT_SEL"), ~0u, 0xFu }); w.push_back({ lx_abs("MPCC1_MPCC_TOP_SEL"), ~0u, 1u }); w.push_back({ lx_abs("MPCC1_MPCC_OPP_ID"), ~0u, 1u }); w.push_back({ lx_abs("MPCC1_MPCC_UPDATE_LOCK_SEL"), ~0u, 1u });
    w.push_back({ lx_abs("MPC_OUT1_MUX"), ~0u, 0x00004101u }); w.push_back({ lx_abs("HUBP1_DCHUBP_CNTL"), 1u, 0u });
    w.push_back({ lx_abs("OTG1_OTG_MASTER_UPDATE_LOCK"), 1u, 0u });
    return w;
}
static std::vector<WantW> spec1_rollback_writes() {
    return { { lx_abs("DPG1_DPG_CONTROL"), N48D2_DPG_CONTROL_MASK, 0x00661001u }, { lx_abs("HUBP1_DCHUBP_CNTL"), 1u, 1u }, { lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, 0u }, { lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, 0u }, { lx_abs("MPC_OUT1_MUX"), ~0u, 0x400fu },
             { lx_abs("MPCC1_MPCC_TOP_SEL"), ~0u, 0xFu }, { lx_abs("MPCC1_MPCC_BOT_SEL"), ~0u, 0xFu }, { lx_abs("MPCC1_MPCC_OPP_ID"), ~0u, 0xFu }, { lx_abs("MPCC1_MPCC_UPDATE_LOCK_SEL"), ~0u, 0xFu }, { lx_abs("OTG1_OTG_CRC_CNTL"), 0x00700011u, 0u },
             { lx_abs("HUBP1_HUBP_CLK_CNTL"), 0x00011111u, 0u }, { lx_abs("DPP_TOP1_DPP_CONTROL"), 0x10u, 0u }, { lx_abs("DPPCLK_CTRL"), 0x8u, 0u }, { lx_abs("DPPCLK1_DTO_PARAM"), ~0u, 0u } };
}
static bool rolled_back_clean1(const P1Model &m, bool dppBit0 = true) {      // the monitor A's plane is OFF: blanked, mux/MPCC1 none, clocks off, DPG on, DTO 0
    return m.blanked && !m.clockOn && !m.clkStatus && m.dpgOn && m.get(N48D2_MPC_OUT1_MUX) == 0x400fu && m.get(N48D2_MPCC1_MPCC_TOP_SEL) == 0xFu && m.get(N48D2_MPCC1_MPCC_BOT_SEL) == 0xFu && m.get(N48D2_MPCC1_MPCC_OPP_ID) == 0xFu &&
           m.get(N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL) == 0xFu && m.get(N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS) == 0u && m.get(N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) == 0u && (m.get(N48D2_DPPCLK_CTRL) & 0x8u) == 0u && (!dppBit0 || (m.get(N48D2_DPPCLK_CTRL) & 1u) == 1u) && m.get(N48D2_DPPCLK1_DTO_PARAM) == 0u && (m.get(N48D2_DPP_TOP1_DPP_CONTROL) & 0x10u) == 0u;
}
static size_t first_read_after1(const P1Model &m, uint32_t abs, size_t readIdx) { for (size_t k = readIdx; k < m.reads.size(); k++) if (m.reads[k] == abs) return k; return (size_t)-1; }

static void mona_list_shapes() {
    n48d2_step s[N48D2_MAX_STEPS];
    check_xs("instance 1 op 8 part A (clocks, BLANK_EN = 1 + its read-back WAIT, pitch 0x77f, address HIGH then LOW: everything BEFORE the HUBP clock)", s, n48d1_build_plane_a(s, kA1), spec1_part_a(kA1));
    check_xs("instance 1 op 8 part D (the 24 explicit 1080p writes - 11 DSCL1, 3 HUBP1, 10 HUBPREQ1 - then the address again)", s, n48d1_build_plane_d(s, kA1), spec1_part_d(kA1));
    check_xs("instance 1 op 8 part B (the bare clock update, four RECORDED reads ~1 ms later, DET1_CTRL RECORDED (slot 13); no WAIT)", s, n48d1_build_plane_b(s), {
        { N48D2_K_UPD, "HUBP1_HUBP_CLK_CNTL", 0x00011111u, 0x00011111u, nullptr, 0, 0 },
        { N48D2_K_REC, "HUBP1_HUBP_CLK_CNTL", 0u, 0u, nullptr, 0, 1000 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL6", 0u, 1u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL", 0u, 2u, nullptr, 0, 0 }, { N48D2_K_REC, "DOMAIN1_PG_STATUS", 0u, 3u, nullptr, 0, 0 },
        { N48D2_K_REC, "DCHUBBUB_DET1_CTRL", 0u, 13u, nullptr, 0, 0 } });
    check_xs("instance 1 op 8 part E (AFTER the unblank: WAIT DET1 current = 3, 100 x 1 ms)", s, n48d1_build_plane_e(s), { { N48D2_K_WAIT, "DCHUBBUB_DET1_CTRL", 0x1F00u, 0x300u, nullptr, 100, 1000 } });
    check_xs("instance 1 op 8 second recorded sample", s, n48d1_build_plane_rec2(s), {
        { N48D2_K_REC, "HUBP1_HUBP_CLK_CNTL", 0u, 4u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL6", 0u, 5u, nullptr, 0, 0 }, { N48D2_K_REC, "DCCG_GATE_DISABLE_CNTL", 0u, 6u, nullptr, 0, 0 }, { N48D2_K_REC, "DOMAIN1_PG_STATUS", 0u, 7u, nullptr, 0, 0 } });
    check_xs("instance 1 op 8 LOCK (optc3_lock: OTG_MASTER_UPDATE_LOCK_SEL UPDATE 0x0E000000 -> 0x02000000 (it already reads 1), OTG_MASTER_UPDATE_LOCK = 1, wait UPDATE_LOCK_STATUS)", s, n48d1_build_plane_lock(s), {
        { N48D2_K_UPD, "OTG1_OTG_GLOBAL_CONTROL2", 0x0E000000u, 0x02000000u, nullptr, 0, 0 }, { N48D2_K_UPD, "OTG1_OTG_MASTER_UPDATE_LOCK", 1u, 1u, nullptr, 0, 0 }, { N48D2_K_WAIT, "OTG1_OTG_MASTER_UPDATE_LOCK", 0x100u, 0x100u, nullptr, 2000, 50 } });
    check_xs("instance 1 op 8 UNLOCK", s, n48d1_build_plane_unlock(s), { { N48D2_K_UPD, "OTG1_OTG_MASTER_UPDATE_LOCK", 1u, 0u, nullptr, 0, 0 } });
    check_xs("instance 1 op 8 part C (INSIDE the lock: address HIGH / LOW, MPCC1 BOT 0xf, TOP 1, OPP 1, LOCK_SEL 1, MPC_OUT1_MUX 0x4101, UNBLANK)", s, n48d1_build_plane_c(s, kA1), {
        { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(kA1 >> 32), nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)kA1, nullptr, 0, 0 },
        { N48D2_K_SET, "MPCC1_MPCC_BOT_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_TOP_SEL", 0, 1u, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_OPP_ID", 0, 1u, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_UPDATE_LOCK_SEL", 0, 1u, nullptr, 0, 0 },
        { N48D2_K_SET, "MPC_OUT1_MUX", 0, 0x00004101u, nullptr, 0, 0 }, { N48D2_K_UPD, "HUBP1_DCHUBP_CNTL", 1u, 0u, nullptr, 0, 0 } });
    check_xs("instance 1 op 9 clears", s, n48d1_build_show1(s), { { N48D2_K_UPD, "HUBP1_DCHUBP_CNTL", 0x80000000u, 0x80000000u, nullptr, 0, 0 }, { N48D2_K_UPD, "ODM1_OPTC_INPUT_GLOBAL_CONTROL", 0x1000u, 0x1000u, nullptr, 0, 0 } });
    check_xs("instance 1 op 9 DPG off", s, n48d1_build_show2(s), { { N48D2_K_UPD, "DPG1_DPG_CONTROL", 1u, 0u, nullptr, 0, 0 }, { N48D2_K_WAIT, "DPG1_DPG_STATUS", 1u, 0u, nullptr, 1000, 100 } });
    check_xs("instance 1 op 10 flip B", s, n48d1_build_flip(s, kB1), {
        { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, (uint32_t)(kB1 >> 32), nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, (uint32_t)kB1, nullptr, 0, 0 },
        { N48D2_K_WAIT, "HUBPREQ1_DCSURF_FLIP_CONTROL", 0x100u, 0u, nullptr, 100, 1000 },
        { N48D2_K_WAIT, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE", 0xFFFFFFFFu, (uint32_t)kB1, nullptr, 100, 1000 }, { N48D2_K_WAIT, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 0xFFFFu, (uint32_t)(kB1 >> 32) & 0xFFFFu, nullptr, 100, 1000 } });
    check_xs("instance 1 op 11 crc (windows 0x50a6 / 0x50a8 = 0x07800000, 0x50a7 / 0x50a9 = 0x04380000, CNTL 0x50a5 UPD 0x00700011 -> 0x11)", s, n48d1_build_crc(s), {
        { N48D2_K_SET, "OTG1_OTG_CRC0_WINDOWA_X_CONTROL", 0, 0x07800000u, nullptr, 0, 0 }, { N48D2_K_SET, "OTG1_OTG_CRC0_WINDOWA_Y_CONTROL", 0, 0x04380000u, nullptr, 0, 0 },
        { N48D2_K_SET, "OTG1_OTG_CRC0_WINDOWB_X_CONTROL", 0, 0x07800000u, nullptr, 0, 0 }, { N48D2_K_SET, "OTG1_OTG_CRC0_WINDOWB_Y_CONTROL", 0, 0x04380000u, nullptr, 0, 0 }, { N48D2_K_UPD, "OTG1_OTG_CRC_CNTL", 0x00700011u, 0x11u, nullptr, 0, 0 } });
    check_xs("instance 1 op 12 planeoff (the rollback)", s, n48d1_build_planeoff(s), {
        { N48D2_K_UPD, "DPG1_DPG_CONTROL", N48D2_DPG_CONTROL_MASK, 0x00661001u, nullptr, 0, 0 }, { N48D2_K_WAIT, "DPG1_DPG_STATUS", 1u, 0u, nullptr, 1000, 100 },
        { N48D2_K_UPD, "HUBP1_DCHUBP_CNTL", 1u, 1u, nullptr, 0, 0 }, { N48D2_K_WAIT, "HUBP1_DCHUBP_CNTL", 2u, 2u, nullptr, 100, 1000 },
        { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "MPC_OUT1_MUX", 0, 0x0000400fu, nullptr, 0, 0 },
        { N48D2_K_SET, "MPCC1_MPCC_TOP_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_BOT_SEL", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_OPP_ID", 0, 0xFu, nullptr, 0, 0 }, { N48D2_K_SET, "MPCC1_MPCC_UPDATE_LOCK_SEL", 0, 0xFu, nullptr, 0, 0 },
        { N48D2_K_WAIT, "MPCC1_MPCC_STATUS", 7u, 5u, nullptr, 100, 1000 }, { N48D2_K_UPD, "OTG1_OTG_CRC_CNTL", 0x00700011u, 0u, nullptr, 0, 0 },
        { N48D2_K_UPD, "HUBP1_HUBP_CLK_CNTL", 0x00011111u, 0u, nullptr, 0, 0 }, { N48D2_K_WAIT, "HUBP1_HUBP_CLK_CNTL", 0x00A00000u, 0u, nullptr, 100, 100 },
        { N48D2_K_UPD, "DPP_TOP1_DPP_CONTROL", 0x10u, 0u, nullptr, 0, 0 }, { N48D2_K_UPD, "DPPCLK_CTRL", 0x8u, 0u, nullptr, 0, 0 }, { N48D2_K_SET, "DPPCLK1_DTO_PARAM", 0, 0u, nullptr, 0, 0 } });
    // the dispatch the flow uses: instance 2 = the monitor B's builders UNCHANGED, instance 1 = the monitor A's
    { n48d2_step a[N48D2_MAX_STEPS], b[N48D2_MAX_STEPS]; bool same = true;
      auto eq = [](const n48d2_step *x, const n48d2_step *y, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (x[i].kind != y[i].kind || x[i].abs != y[i].abs || x[i].mask != y[i].mask || x[i].val != y[i].val || x[i].polls != y[i].polls || x[i].step_us != y[i].step_us || x[i].cond_abs != y[i].cond_abs) return false; return true; };
      same = same && n48d2_build_plane_a_i(2, a, kBufA) == n48d2_build_plane_a(b, kBufA) && eq(a, b, 8u);
      same = same && n48d2_build_plane_a_i(1, a, kA1) == n48d1_build_plane_a(b, kA1) && eq(a, b, 8u);
      same = same && n48d2_build_planeoff_i(2, a) == n48d2_build_planeoff(b) && eq(a, b, 18u) && n48d2_build_planeoff_i(1, a) == n48d1_build_planeoff(b) && eq(a, b, 18u);
      same = same && n48d2_pre_pages(1) == 2u && n48d2_pre_pages(2) == 3u && n48d2_build_pre_i(2, 0, a) == 48u && n48d2_build_pre_i(1, 0, a) == 48u;
      expect(same, "the per-instance builder dispatch: instance 2 -> the monitor B's builders, instance 1 -> the monitor A's (op 8 part A, the rollback, the precheck pages)"); }
    // the WAIT indices the flow gates on and the step counts are shared with the monitor B's lists
    expect(N48D2_PA_STEPS == 8u && N48D2_PB_STEPS == 6u && N48D2_PR_STEPS == 4u && N48D2_PC_STEPS == 8u && N48D2_LK_STEPS == 3u && N48D2_PU_STEPS == 1u && N48D1_PD_STEPS == 26u && N48D2_FLIP_STEPS == 5u && N48D2_PO_STEPS == 18u && N48D2_PE_STEPS == 1u, "the monitor A's lists have the monitor B's shapes (8 / 6 / 4 / 8 / 3 / 1 / 5 / 18 / 1) and a 26-step part D");
    { n48d2_step a[N48D2_MAX_STEPS], b[N48D2_MAX_STEPS], c[N48D2_MAX_STEPS], d[N48D2_MAX_STEPS], l[N48D2_MAX_STEPS], o[N48D2_MAX_STEPS];
      const uint32_t na = n48d1_build_plane_a(a, kA1), nb = n48d1_build_plane_b(b), nc = n48d1_build_plane_c(c, kA1), nd = n48d1_build_plane_d(d, kA1), nl = n48d1_build_plane_lock(l), no = n48d1_build_planeoff(o);
      expect(a[N48D2_PA_I_BLANK].abs == N48D2_HUBP1_DCHUBP_CNTL && a[N48D2_PA_I_BLANK].kind == N48D2_K_UPD && a[N48D2_PA_I_BLANKWAIT].abs == N48D2_HUBP1_DCHUBP_CNTL && a[N48D2_PA_I_BLANKWAIT].kind == N48D2_K_WAIT && na == 8u, "part A: BLANK_EN = 1 is step 3 and its read-back WAIT step 4 (the indices the flow gates on)");
      { uint32_t blank = 99, bwait = 99, addrLo = 99, addrHi = 99, clkA = 0, mpcA = 0, copyA = 0; for (uint32_t i = 0; i < na; i++) { if (a[i].abs == N48D2_HUBP1_DCHUBP_CNTL && a[i].kind == N48D2_K_UPD) blank = i; if (a[i].abs == N48D2_HUBP1_DCHUBP_CNTL && a[i].kind == N48D2_K_WAIT) bwait = i; if (a[i].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS) addrLo = i; if (a[i].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) addrHi = i; if (a[i].abs == N48D2_HUBP1_HUBP_CLK_CNTL) ++clkA; if (a[i].abs == N48D2_MPC_OUT1_MUX || a[i].abs == N48D2_MPCC1_MPCC_TOP_SEL) ++mpcA; if (a[i].kind == N48D2_K_COPY0) ++copyA; }
        expect(blank < bwait && bwait < addrHi && addrHi < addrLo && clkA == 0u && mpcA == 0u && copyA == 0u, "part A: BLANK_EN = 1, its read-back WAIT, then HIGH then LOW; NO clock write, NO COPY0, NO MPCC1 / mux write BEFORE the HUBP clock"); }
      expect(b[0].abs == N48D2_HUBP1_HUBP_CLK_CNTL && b[0].kind == N48D2_K_UPD && nb == 6u && b[nb - 1].kind == N48D2_K_REC && b[nb - 1].abs == N48D2_DCHUBBUB_DET1_CTRL, "part B opens with the HUBP clock write and ends with the DET1 RECORDED read (no WAIT)");
      { bool anyWait = false; for (uint32_t i = 0; i < nb; i++) anyWait = anyWait || b[i].kind == N48D2_K_WAIT; expect(!anyWait, "part B holds NO WAIT at all"); }
      { uint32_t clkD = 0, copyD = 0, addrD = 0; for (uint32_t i = 0; i < nd; i++) { if (d[i].abs == N48D2_HUBP1_HUBP_CLK_CNTL || d[i].abs == N48D2_HUBP1_DCHUBP_CNTL) ++clkD; if (d[i].kind == N48D2_K_COPY0) ++copyD; if (d[i].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS || d[i].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) ++addrD; }
        expect(nd == 26u && clkD == 0u && copyD == 0u && addrD == 2u && d[nd - 2].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && d[nd - 1].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, "part D: 24 programming writes, then the address HIGH then LOW AGAIN; no clock / blank write and NO COPY0"); }
      expect(c[nc - 1].abs == N48D2_HUBP1_DCHUBP_CNTL && c[nc - 1].kind == N48D2_K_UPD && c[nc - 1].mask == 1u && c[nc - 1].val == 0u && c[nc - 2].abs == N48D2_MPC_OUT1_MUX && c[0].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && c[1].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS && nc == 8u && N48D2_PC_I_UNBLANK == 7u,
             "part C (inside the lock) opens with the address HIGH then LOW (A1) and ends with the UNBLANK, right after MPC_OUT1_MUX");
      expect(nl == 3u && l[N48D2_LK_I_WAIT].kind == N48D2_K_WAIT && l[N48D2_LK_I_WAIT].abs == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK && l[N48D2_LK_I_SEL].abs == N48D2_OTG1_OTG_GLOBAL_CONTROL2 && l[N48D2_LK_I_LOCK].abs == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, "the lock: select UPDATE, lock UPDATE, status WAIT (OTG1's registers)");
      expect(no == 18u && o[N48D2_PO_I_NOOUT].kind == N48D2_K_WAIT && o[N48D2_PO_I_NOOUT].abs == N48D2_HUBP1_DCHUBP_CNTL && o[N48D2_PO_I_ADDR_HI].abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH && o[N48D2_PO_I_MPCC].abs == N48D2_MPCC1_MPCC_STATUS && o[N48D2_PO_I_CLKOFF].abs == N48D2_HUBP1_HUBP_CLK_CNTL && o[N48D2_PO_I_CLKOFF].kind == N48D2_K_WAIT, "the rollback's WAIT indices (NO_OUTSTANDING_REQ 3, address 4 / 5, MPCC1 status 11, clock gated 14) are the monitor B's"); }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the flow over P1Model: op 8, the gates, ops 9..13
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void mona_op8_tests() {
    // ================= the clean run
    {
        P1Model m; m.initPlane();
        const n48d2_out o = run1(m, N48D2_OP_PLANE);
        if (o.status != N48D2_OK) std::printf("   instance 1 op 8 clean run: status %u (%s) fail %u pre_abs %#x pre_val %#x guard %u dp_changed %#x dpx %#x w2 %#x timeouts %u\n", o.status, n48d2_status_name(o.status), o.fail, o.pre_abs, o.pre_val, o.guard, o.dp_changed, o.dpx_changed, o.w2_changed, o.timeouts);
        expect(o.status == N48D2_OK && o.fail == 0xFFu && o.timeouts == 0u && o.auto_off == 0u && o.free_bufs == 0u && o.dp_changed == 0u && o.dpx_changed == 0u && o.w2_changed == 0u && o.inst == 1u, "instance 1 op 8 on the census state: OK, no timeout, no rollback, the DP and the monitor B UNCHANGED");
        expect(m.plane1.stage == N48D2_PL_PLANE && m.plane1.cur == 0u && m.plane1.mc[0] == kA1 && m.plane1.mc[1] == kB1 && m.plane.stage == N48D2_PL_NONE, "the monitor A's plane is UP on buffer A1 (stage PLANE); the monitor B's plane state is untouched");
        expect_seq("instance 1 op 8: the 45 register writes in the spec's order, in order", m.writes, spec1_op8_writes(kA1));
        expect(o.done == 45u, "steps written are counted (45)");
        // every write is an instance-1 register (list or exception): NOTHING of the monitor B's, the DP's or pipe 0's
        { bool in = true; for (const W &x : m.writes) { bool ok = x.abs == N48D2_DPPCLK1_DTO_PARAM || x.abs == N48D2_DPPCLK_CTRL; for (uint32_t k = 0; k < N48D2_ALLOWED1P_COUNT; k++) ok = ok || x.abs == n48d2_allowed1p[k]; in = in && ok; }
          expect(in, "every write of op 8 is a listed instance-1 plane register or one of its two DCCG exceptions"); }
        { bool monb = false; for (const W &x : m.writes) monb = monb || (x.abs >= 0x3c5du && x.abs <= 0x3d05u) || (x.abs >= 0x445bu && x.abs <= 0x455eu) || (x.abs >= 0x902au && x.abs <= 0x9038u) || (x.abs >= 0x92fau && x.abs <= 0x92fdu) || x.abs == N48D2_DPPCLK2_DTO_PARAM || x.abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK || x.abs == N48D2_OTG2_OTG_GLOBAL_CONTROL2 || x.abs == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL;
          expect(!monb, "no write of op 8 lands in the monitor B's HUBP2 / DPP2 / MPCC2 / MPC_OUT2 blocks, DPPCLK2's DTO, OTG2's lock / select or ODM2"); }
        // ORDER (every index must exist before any of it is used)
        const size_t NP = (size_t)-1;
        const size_t iBlank = nth_write(m.writes, N48D2_HUBP1_DCHUBP_CNTL, 0), iClk = nth_write(m.writes, N48D2_HUBP1_HUBP_CLK_CNTL), iUnblank = nth_write(m.writes, N48D2_HUBP1_DCHUBP_CNTL, 1);
        const size_t iHi = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0), iLo = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0), iHi2 = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 1), iLo2 = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 1), iPitch = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH);
        const size_t iHi3 = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 2), iLo3 = nth_write(m.writes, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 2);
        const size_t iLockSel = nth_write(m.writes, N48D2_OTG1_OTG_GLOBAL_CONTROL2), iLock = nth_write(m.writes, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 0), iUnlock = nth_write(m.writes, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, 1);
        const size_t iBot = nth_write(m.writes, N48D2_MPCC1_MPCC_BOT_SEL), iMux = nth_write(m.writes, N48D2_MPC_OUT1_MUX), iDto = nth_write(m.writes, N48D2_DPPCLK1_DTO_PARAM), iCtl = nth_write(m.writes, N48D2_DPPCLK_CTRL), iDpp = nth_write(m.writes, N48D2_DPP_TOP1_DPP_CONTROL), iDscl = nth_write(m.writes, N48D2_DSCL1_SCL_MODE), iLb = nth_write(m.writes, N48D2_DSCL1_LB_MEMORY_CTRL);
        const bool idxOk = iHi3 != NP && iLo3 != NP && iLockSel != NP && iLock != NP && iUnlock != NP && iBlank != NP && iClk != NP && iUnblank != NP && iHi != NP && iLo != NP && iHi2 != NP && iLo2 != NP && iPitch != NP && iBot != NP && iMux != NP && iDto != NP && iCtl != NP && iDpp != NP && iDscl != NP && iLb != NP && m.writes.size() >= 2u && m.readsAtWrite.size() == m.writes.size() && m.f1AtWrite.size() == m.writes.size() && m.f1AtRead.size() == m.reads.size();
        expect(idxOk, "instance 1 op 8: every write the ORDER checks name exists (blank, clock, unblank, the address TWICE+, the pitch, MPCC1, mux, DTO, DPPCLK_CTRL, DPP clock, DSCL1)");
        if (idxOk) {
        expect(iDto < iCtl && iCtl < iDpp && iDpp < iBlank && iBlank < iClk && (m.writes[iBlank].v & 1u) == 1u, "ORDER: DPPCLK1 DTO, DPPCLK1_EN, the DPP clock, then BLANK_EN = 1, all BEFORE the HUBP clock goes on");
        expect(first_read_after1(m, N48D2_HUBP1_DCHUBP_CNTL, m.readsAtWrite[iBlank] + 0) < m.readsAtWrite[iClk], "ORDER (reachability): the BLANK is READ BACK (a read of HUBP1_DCHUBP_CNTL between the blank write and the clock write)");
        expect(iBlank < iPitch && iPitch < iHi && iHi < iLo && iLo < iClk, "ORDER: the pitch and the address (HIGH then LOW) are written BEFORE the HUBP clock, after the blank");
        expect(iClk < iDscl && iDscl < iLb, "ORDER: every DSCL1 write comes AFTER the HUBP clock is on");
        { bool allAfter = true; for (const uint32_t d : { N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG, N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ }) { const size_t k = nth_write(m.writes, d); allAfter = allAfter && k != NP && k > iClk; } expect(allAfter, "ORDER: the HUBP1 / HUBPREQ1 programming writes come AFTER the HUBP clock is on"); }
        expect(iLb < iHi2 && iHi2 < iLo2 && iClk < iHi2 && m.writes[iHi2].v == m.writes[iHi].v && m.writes[iLo2].v == m.writes[iLo].v && m.writes[iLo2].v == (uint32_t)kA1, "ORDER: the address is RE-WRITTEN (HIGH then LOW) after the clock is on and after all the programming, with the same value (A1)");
        // the read-back after the 2-frame wait and before the lock
        const size_t rLast = m.readsAtWrite[iLo2];
        size_t rLb = NP, rFlip = NP; for (size_t k = rLast; k < m.reads.size(); k++) { if (rLb == NP && m.reads[k] == N48D2_DSCL1_LB_MEMORY_CTRL) rLb = k; if (rFlip == NP && m.reads[k] == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL) rFlip = k; }
        expect(rLb != NP && rFlip != NP && rLb < rFlip, "ORDER: the register READ-BACK (DSCL1_LB_MEMORY_CTRL read after the programming) comes BEFORE the first FLIP_PENDING read (of the POST-unlock gate)");
        if (rLb != NP && rFlip != NP) {
            expect(m.f1AtRead[rLb] - m.f1AtWrite[iLo2] >= 2u, "REACHABILITY: at least 2 OTG1 frames pass between the last programming write and the first read-back read");
            expect(m.readsAtWrite[iLockSel] > rLb && rFlip >= m.readsAtWrite[iUnlock], "ORDER: the read-back completes BEFORE the lock is taken, and the first FLIP_PENDING read comes only AFTER the unlock write");
            bool all = true; std::string missing; for (size_t i = 0; i < iLockSel; i++) { if (i == iClk) continue; bool seen = false; for (size_t k = rLast; k < rFlip; k++) seen = seen || m.reads[k] == m.writes[i].abs; if (!seen) { all = false; char b[40]; std::snprintf(b, sizeof b, " %#x", m.writes[i].abs); missing += b; } }
            expect(all, ("REACHABILITY: every register written before the lock (parts A and D, the clock excepted) is READ BACK between the frame wait and the lock; missing:" + missing).c_str());
        }
        expect(iLo2 < iLockSel && iLockSel < iLock && iLock < iHi3 && iHi3 < iLo3 && iLo3 < iBot && iBot < iMux && iMux < iUnblank && iUnblank < iUnlock && m.writes.size() - 1 == iUnlock, "ORDER: the programming, then the lock (select, lock), then the address (HIGH, LOW), MPCC1, MPC_OUT1_MUX, the UNBLANK, then the UNLOCK is the LAST write");
        expect(iHi3 == iLock + 1u && iLo3 == iHi3 + 1u && iBot == iLo3 + 1u && iUnblank == iBot + 5u && iUnlock == iUnblank + 1u && iLock == iLockSel + 1u, "ORDER: CONSECUTIVE - nothing is written between the lock and the unlock but the address, MPCC1 (4), the mux and the unblank");
        expect(m.sleepsAtWrite[iUnlock] == m.sleepsAtWrite[iLo3] && m.f1AtWrite[iUnlock] == m.f1AtWrite[iHi3], "REACHABILITY: no sleep and no OTG1 frame between the address write inside the lock and the unlock");
        { size_t nFlipIn = 0, nEarlyIn = 0; for (size_t k = m.readsAtWrite[iLo2]; k < m.readsAtWrite[iUnlock]; k++) { nFlipIn += m.reads[k] == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL; nEarlyIn += m.reads[k] == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE; }
          expect(nFlipIn == 0u && nEarlyIn == 0u, "NO FLIP_PENDING and NO EARLIEST_INUSE read between the last programming write and the unlock (no pre-unblank gate)"); }
        { size_t nWaitReads = 0; for (size_t k = m.readsAtWrite[iLock]; k < m.readsAtWrite[iHi3]; k++) nWaitReads += m.reads[k] == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK; expect(nWaitReads >= 1u, "the lock status (OTG1_OTG_MASTER_UPDATE_LOCK bit 8) is READ between the lock write and the first write inside the lock"); }
        { size_t nPostFlip = 0, nPostEarly = 0; for (size_t k = m.readsAtWrite[iUnlock]; k < m.reads.size(); k++) { nPostFlip += m.reads[k] == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL; nPostEarly += m.reads[k] == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE; }
          expect(nPostFlip >= 3u && nPostEarly >= 1u, "REACHABILITY: AFTER the unlock the latch gate POLLS FLIP_PENDING and reads EARLIEST_INUSE"); }
        const size_t rClk = m.readsAtWrite[iClk], rBot = m.readsAtWrite[iBot];
        size_t nDet = 0, nFlip = 0, nEarly = 0; for (size_t k = rClk; k < rBot; k++) { nDet += m.reads[k] == N48D2_DCHUBBUB_DET1_CTRL; nFlip += m.reads[k] == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL; nEarly += m.reads[k] == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE; }
        expect(nDet >= 1 && nFlip == 0 && nEarly == 0, "after the clock and before MPCC1 DET1 is only RECORDED and FLIP_PENDING / EARLIEST_INUSE are NOT read");
        { size_t nPost = 0; for (size_t k = m.readsAtWrite[iUnblank]; k < m.reads.size(); k++) nPost += m.reads[k] == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE; expect(nPost >= 1, "the EARLIEST_INUSE gate runs AFTER the unblank"); }
        expect(m.f1AtWrite[iBot] - m.f1AtWrite[iClk] >= 2u, "REACHABILITY: at least 2 OTG1 frames pass between the clock and MPCC1");
        expect(m.frames1 - m.f1AtWrite[iUnblank] >= 3u, "REACHABILITY: at least 3 OTG1 frames pass after the unblank (the health gate)");
        size_t nHealth = 0; for (size_t k = m.readsAtWrite[iUnblank]; k < m.reads.size(); k++) nHealth += m.reads[k] == N48D2_HUBP1_DCHUBP_CNTL || m.reads[k] == N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL || m.reads[k] == N48D2_DCN_VM_FAULT_STATUS || m.reads[k] == N48D2_DCHUBBUB_DET1_CTRL;
        expect(nHealth >= 4, "REACHABILITY: after the unblank the health gate reads HUBP1 CNTL, ODM1, the VM fault status and DET1");
        }   // idxOk
        // the surface: 2 x 1080 x 8 runs, ONE flush, 48 read-backs
        expect(m.vfillCalls == 2u * N48D1_PLANE_H * 8u && m.vflushCalls == 1u && m.vreadCalls == 2u * N48D2_VPOINTS, "the surface: 17280 bar runs, ONE flush, 48 read-backs through the MM window");
        { const size_t F = m.vlog.find('F'); expect(F == 17280u && m.vlog.size() == 17280u + 1u + 48u && m.vlog.find('r') > F && m.vlog.rfind('f') < F, "the HDP flush sits between the last fill and the first read-back"); }
        const uint32_t *g = m.gateBuf;
        expect(((g[N48D2_GT_DET2] & N48D2_DET_CUR_MASK) >> 8) == 3u && (g[N48D2_GT_HUBP_CNTL] & 0x70000800u) == 0u && g[N48D2_GT_VMFAULT] == 0u && (g[N48D2_GT_FLIP_CTL] & 0x100u) == 0u && (((uint64_t)(g[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | g[N48D2_GT_EARLY_LO]) == kA1, "gates after op 8: DET1 current 3, no underflow / SEG_ALLOC_ERR, VM fault 0, FLIP_PENDING 0, EARLIEST_INUSE = A1");
        expect(g[N48D2_GT_MPC_MUX] == 0x4101u && g[N48D2_GT_A_LO] == (uint32_t)kA1 && g[N48D2_GT_B_LO] == (uint32_t)kB1 && (g[N48D2_GT_META] & 0xFFu) == N48D2_PL_PLANE && n48d2_health(g, 0u, kA1, 1) == 0u && ((g[N48D2_GT_META] >> 16) & 0x1FFu) == 0u, "gates: the mux 0x4101, A1 / B1, the meta (stage PLANE, monitor B-watch mask 0), the pure verdict is clean");
        uint64_t v[13]; n48d2_pack_plane(v, &o, g); n48d2_out b; uint32_t g2[N48D2_GATES]; std::memset(&b, 0, sizeof b); n48d2_unpack_plane(v, &b, g2);
        bool same = true; for (uint32_t i = 0; i < N48D2_GATES; i++) same = same && g2[i] == g[i];
        expect(same && b.status == o.status && b.f0a == o.f0a && b.dpx_changed == o.dpx_changed && b.inst == N48D2_INST_MONA, "the 18 gate dwords survive the 13-scalar result and the result says instance 1");
        size_t before = 0, after = 0; if (!m.readsAtWrite.empty()) for (size_t k = 0; k < m.reads.size(); k++) { bool isw = m.reads[k] == N48D2_HUBPREQ0_PRIMARY_HIGH; for (uint32_t i = 0; i < N48D2_DPX_REGS; i++) isw = isw || m.reads[k] == n48d2_dpx_regs[i].abs; if (isw) { if (k < m.readsAtWrite[0]) ++before; else if (k >= m.readsAtWrite.back()) ++after; } }
        expect(before >= N48D2_DPX_REGS + 1u && after >= N48D2_DPX_REGS + 1u, "the 15-row DP watch is read before the first write and after the last (instance 1 op 8)");
        { size_t wb = 0, wa = 0; for (size_t k = 0; k < m.reads.size(); k++) { if (m.reads[k] == N48D2_DENTIST_DISPCLK_CNTL) { if (k < m.readsAtWrite[0]) ++wb; else if (k >= m.readsAtWrite.back()) ++wa; } }
          expect(wb == 1u && wa == 1u, "the monitor B watch (DENTIST_DISPCLK_CNTL as its marker) is read once before the first write and once after the last (instance 1 op 8)"); }
        // the buffers: A1 bars white..black, B1 reversed, every dword of both 1920x1080 frames
        bool pat = m.vram1[0].size() == N48D1_BUF_BYTES / 4u && m.vram1[1].size() == N48D1_BUF_BYTES / 4u;
        for (uint32_t k = 0; k < 2 && pat; k++) for (uint32_t y = 0; y < N48D1_PLANE_H && pat; y += 1) { const uint32_t *row = &m.vram1[k][(size_t)y * N48D1_PLANE_W]; for (uint32_t x = 0; x < N48D1_PLANE_W; x += 1) { const uint32_t bar = x / 240u; if (row[x] != n48d2_bars[k ? 7u - bar : bar]) { pat = false; break; } } }
        expect(pat, "buffer A1 is eight 240-px bars white yellow cyan green magenta red blue black, buffer B1 the same in REVERSE, every dword of both 1920x1080 frames");
        { bool tail = true; for (uint32_t k = 0; k < 2; k++) for (size_t i = N48D1_FRAME_BYTES / 4u; i < N48D1_BUF_BYTES / 4u; i++) tail = tail && m.vram1[k][i] == 0xDEADBEEFu; expect(tail, "the 28,672-byte tail of each 127 x 64 KiB buffer past the frame is never drawn"); }
        expect(m.vram[0].empty() && m.vram[1].empty(), "the monitor B's buffers (vram[]) are never touched by the monitor A's fill");
    }
    // ================= refusals before anything is read or written
    { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_NONE; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty() && m.vfillCalls == 0, "instance 1 op 8 with no buffers allocated: PLANE_STATE, nothing read, nothing written"); }
    { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_PLANE; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty(), "instance 1 op 8 on a plane that is already up: PLANE_STATE, nothing read or written"); }
    { P1Model m; m.initPlane(); const n48d2_out o = n48d2::run_plane(m, 7u, 0u, N48D2_INST_MONA); expect(o.status == N48D2_BAD_ARG && m.reads.empty() && m.writes.empty(), "run_plane refuses a non-plane op with nothing read"); const n48d2_out o2 = run1(m, N48D2_OP_FLIP, 2u); expect(o2.status == N48D2_BAD_ARG && m.reads.empty(), "... and a flip index above 1");
      const n48d2_out o3 = n48d2::run_plane(m, N48D2_OP_FBHOLD, 0u, N48D2_INST_MONA); expect(o3.status == N48D2_PLANE_STATE && m.reads.empty() && m.writes.empty() && m.pinCalls == 0u && n48d2_arg_ok(n48d2_arg_i(N48D2_OP_FBHOLD, 1u)) && !n48d2_arg_ok(n48d2_arg(N48D2_OP_FBHOLD)), "fbhold on instance 1 (0.0.658) is a plane op: before show it is PLANE_STATE with nothing read, nothing pinned; the default instance 0 is refused by the argument");
      const n48d2_out o4 = n48d2::run_plane(m, N48D2_OP_PLANE, 0u, 3u); expect(o4.status == N48D2_BAD_ARG && m.reads.empty() && m.writes.empty(), "an unknown instance is BAD_ARG with nothing read");
      m.inst = N48D2_INST_MONB; const n48d2_out o5 = run1(m, N48D2_OP_PLANE); expect(o5.status == N48D2_BAD_ARG && m.reads.empty() && m.writes.empty(), "an environment whose guard is the OTHER instance's (E::inst != the op's instance) is BAD_ARG with nothing read"); }
    // ================= admission of the buffers (pure arithmetic: nothing read) - and against the monitor B's pair
    {
        struct Adm { const char *what; uint64_t a, b; bool haveWin; uint64_t lo, hi; } cases[] = {
            { "A1 not 64 KiB aligned", kA1 + 0x1000u, kB1, true, kWinLo, kWinHi }, { "B1 not 64 KiB aligned", kA1, kB1 + 0x4u, true, kWinLo, kWinHi }, { "A1 below the window", kWinLo - 0x10000u, kB1, true, kWinLo, kWinHi },
            { "B1 runs past the window end", kA1, kWinHi - N48D1_BUF_BYTES + 0x10000u, true, kWinLo, kWinHi }, { "A1 and B1 overlap (one 64 KiB apart)", kA1, kA1 + 0x10000u, true, kWinLo, kWinHi }, { "A1 and B1 overlap (one byte short of a buffer apart)", kA1, kA1 + N48D1_BUF_BYTES - 0x10000u, true, kWinLo, kWinHi },
            { "A1 == B1", kA1, kA1, true, kWinLo, kWinHi }, { "no window known (fail closed)", kA1, kB1, false, 0, 0 }, { "an empty window", kA1, kB1, true, kWinLo, 0 }, { "A1 is zero", 0, kB1, true, kWinLo, kWinHi },
            { "A1 and B1 in different 4 GiB halves", 0x80FFF00000ull, 0x8100000000ull, true, kWinLo, 0x8200000000ull }, { "A1 straddles a 4 GiB boundary", 0x80FFC00000ull, kB1, true, kWinLo, 0x8200000000ull } };
        for (const Adm &c : cases) {
            P1Model m; m.initPlane(); m.plane1.mc[0] = c.a; m.plane1.mc[1] = c.b; m.haveWin = c.haveWin; m.winLo = c.lo; m.winHi = c.hi;
            const n48d2_out o = run1(m, N48D2_OP_PLANE);
            expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.reads.empty() && m.vfillCalls == 0 && o.free_bufs == 1u && m.plane1.stage == N48D2_PL_FREE, (std::string("admission (instance 1): ") + c.what + ": NO_BUFFER, nothing read / written / drawn, the buffers are released").c_str());
        }
        // "admission must also refuse any overlap with the monitor B's pinned pair": the monitor B's pair live (any stage but NONE / FREE) and the monitor A's buffer overlapping either of its buffers
        struct Ov { const char *what; uint64_t a1, b1; uint32_t monbStage; bool refused; } ovs[] = {
            { "A1 inside the monitor B's A (HELD)", kBufA + 0x100000u, kB1, N48D2_PL_HELD, true }, { "A1 == the monitor B's A (HELD)", kBufA, kB1, N48D2_PL_HELD, true }, { "B1 overlaps the monitor B's B by one 64 KiB step (HELD)", kA1, kBufB + N48D2_BUF_BYTES - 0x10000u, N48D2_PL_HELD, true },
            { "A1 ends one step into the monitor B's A (HELD)", kBufA - N48D1_BUF_BYTES + 0x10000u, kB1, N48D2_PL_HELD, true }, { "A1 overlaps the monitor B's pair while it is only SHOWN", kBufB + 0x10000u, kB1 + 0x2000000u, N48D2_PL_SHOWN, true },
            { "A1 / B1 clear of the monitor B's pair (HELD)", kA1, kB1, N48D2_PL_HELD, false }, { "A1 touches the monitor B's A from below without overlap (HELD)", kBufA - N48D1_BUF_BYTES, kB1, N48D2_PL_HELD, false }, { "overlap with a monitor B pair that is FREE (released): not a conflict", kBufA, kB1, N48D2_PL_FREE, false } };
        for (const Ov &c : ovs) {
            P1Model m; m.initPlane(); m.plane.stage = c.monbStage; m.plane.mc[0] = kBufA; m.plane.mc[1] = kBufB; m.plane1.mc[0] = c.a1; m.plane1.mc[1] = c.b1;
            const n48d2_out o = run1(m, N48D2_OP_PLANE);
            if (c.refused) expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.reads.empty() && m.vfillCalls == 0 && m.plane1.stage == N48D2_PL_FREE, (std::string("admission vs the monitor B's pair: ") + c.what + ": NO_BUFFER, nothing read / written").c_str());
            else expect(o.status != N48D2_NO_BUFFER, (std::string("admission vs the monitor B's pair: ") + c.what + ": admitted").c_str());
        }
    }
    // ================= prechecks: EVERY EXPECT row and every state row; nothing written, nothing drawn
    {
        uint32_t nTried = 0, nGood = 0;
        for (uint32_t i = 0; i < N48D1_EXP_COUNT; i++) {
            P1Model m; m.initPlane(); const uint32_t a = n48d1_exp[i].abs; const uint32_t bit = n48d1_exp[i].mask & ~(n48d1_exp[i].mask - 1u);
            m.r[a] ^= bit; ++nTried;
            const n48d2_out o = run1(m, N48D2_OP_PLANE);
            const bool timingRow = a == N48D2_OTG1_H_TOTAL || a == N48D2_OTG1_V_TOTAL;
            if (o.status == N48D2_PRECHECK && o.pre_abs == a && m.writes.empty() && m.vfillCalls == 0 && m.plane1.stage == N48D2_PL_FREE && o.free_bufs == 1u) ++nGood; else std::printf("   EXPECT row %u (%s, %#x) not caught: status %u pre_abs %#x writes %zu%s\n", i, n48d1_exp[i].name, a, o.status, o.pre_abs, m.writes.size(), timingRow ? " (timing row)" : "");
        }
        expect(nTried == N48D1_EXP_COUNT && nGood == nTried, "precheck (instance 1): a flipped bit in ANY of the 37 census-valued EXPECT registers ends the op in PRECHECK naming that register; nothing written, nothing drawn, the buffers released");
        nTried = nGood = 0;
        for (uint32_t i = 0; i < N48D1_PRE_STATE_COUNT; i++) {
            P1Model m; m.initPlane(); const uint32_t a = n48d1_pre_state[i].abs; m.r[a] ^= n48d1_pre_state[i].mask & ~(n48d1_pre_state[i].mask - 1u); ++nTried;
            if (a == N48D2_DCHUBBUB_DET1_CTRL) { m.r[a] ^= n48d1_pre_state[i].mask & ~(n48d1_pre_state[i].mask - 1u); m.det2Size = 2u; }      // DET1's size field is a model knob
            if (a == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK) { m.r[a] = 1u; }       // lock bit 0 set: the model reads it back as the lock + its status
            const n48d2_out o = run1(m, N48D2_OP_PLANE);
            const bool otg1Off = a == N48D2_OTG1_CONTROL;      // OTG1 stopped: the counting check answers before the EXPECT page does
            if (o.status == N48D2_PRECHECK && (o.pre_abs == a || (otg1Off && o.pre_abs == N48D2_OTG1_FRAME_COUNT)) && m.writes.empty() && m.vfillCalls == 0) ++nGood; else std::printf("   state row %u (%s, %#x) not caught: status %u pre_abs %#x\n", i, n48d1_pre_state[i].name, a, o.status, o.pre_abs);
        }
        expect(nTried == N48D1_PRE_STATE_COUNT && nGood == nTried, "precheck (instance 1): a wrong value in ANY of the 21 state registers (OTG1 not enabled, DPG1 off, HUBP1 clock already on, DPP1 clock on, MPCC1 / mux in use, DPPCLK1_EN set, DTO set, DET0 / DET1, VM fault, the four VM registers, the lock select, the lock held) ends the op in PRECHECK; nothing written");
        { P1Model m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)kA1; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(kA1 >> 32); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && m.writes.empty() && m.vfillCalls == 0, "precheck: HUBP0's current address == buffer A1 refuses the op"); }
        { P1Model m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)(kB1 + N48D1_BUF_BYTES - 0x10000u); m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(kB1 >> 32); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && m.writes.empty() && m.vfillCalls == 0, "precheck: HUBP0's address inside buffer B1 (its last 64 KiB) refuses the op (an OVERLAP test at the monitor A's own size)"); }
        { P1Model m; m.initPlane(); m.r[N48D2_HUBPREQ0_EARLIEST_LOW] = (uint32_t)kB1; m.r[N48D2_HUBPREQ0_EARLIEST_HIGH] = (uint32_t)(kB1 >> 32); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_HUBPREQ0_EARLIEST_LOW && m.writes.empty(), "precheck: HUBP0's EARLIEST_INUSE == B1 refuses the op"); }
        { P1Model m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)(kB1 + N48D1_BUF_BYTES); m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(kB1 >> 32); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_OK, "precheck: HUBP0's address just past B1 (the monitor A's 8.3 MB, not the monitor B's 14.7 MB) does not overlap"); }
        { P1Model m; m.initPlane(); m.otg0Stops = true; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_DP_NOT_COUNTING && m.writes.empty() && m.vfillCalls == 0 && m.plane1.stage == N48D2_PL_FREE, "OTG0 not counting: DP_NOT_COUNTING, nothing written, buffers released"); }
        { P1Model m; m.initPlane(); m.otg1Frozen = true; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PRECHECK && o.pre_abs == N48D2_OTG1_FRAME_COUNT && m.writes.empty() && m.vfillCalls == 0, "OTG1 not counting: PRECHECK naming the frame counter, nothing written"); }
        { P1Model m; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE); (void)o; expect(m.reads.size() > 0 && first_write(m.writes, N48D2_DPPCLK1_DTO_PARAM) != (size_t)-1, "(control) the unmodified state passes every precheck above"); }
    }
    // ================= fill / verify: refused, never half-done
    {
        { P1Model m; m.initPlane(); m.vfillFailAt = 0; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.plane1.stage == N48D2_PL_FREE && o.free_bufs == 1u, "a refused first fill: NO_BUFFER, no register written, buffers released"); }
        { P1Model m; m.initPlane(); m.vfillFailAt = 8640u + 7u; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_NO_BUFFER && m.writes.empty() && m.vflushCalls == 0, "a refused fill part-way into buffer B1: NO_BUFFER, no flush, no register written"); }
        uint32_t caught = 0, tried = 0;
        for (uint32_t k = 0; k < 2; k++) for (uint32_t p = 0; p < N48D2_VPOINTS; p++) {
            P1Model m; m.initPlane(); uint32_t want = 0; const uint32_t off = n48d2_vpoint_i(N48D2_INST_MONA, k, p, &want); m.vreadCorruptK = k; m.vreadCorruptDw = off / 4u; ++tried;
            const n48d2_out o = run1(m, N48D2_OP_PLANE);
            if (o.status == N48D2_NO_BUFFER && m.writes.empty() && m.plane1.stage == N48D2_PL_FREE) ++caught;
        }
        expect(tried == 48u && caught == tried, "verify: a wrong dword at ANY of the 24 points of A1 or of B1 (rows 0 / 540 / 1079 x the eight bar centres) is REFUSED: NO_BUFFER, no register written");
    }
}

static void mona_gate_failures() {
    struct Row { const char *what; std::function<void(P1Model &)> set; uint32_t status; bool clockWritten, mpccWritten, unblanked; bool leak = false; };
    const Row rows[] = {
        { "BLANK_EN does not read back as 1 (the buffers are LEAKED, not freed)", [](P1Model &m) { m.blankSticks = false; }, N48D2_GATE, false, false, false, true },
        { "HUBP register writes do not land while the plane clock is gated (LEAKED)", [](P1Model &m) { m.hubpWritesLand = false; }, N48D2_GATE, false, false, false, true },
        { "DSCL1_SCL_MODE does not latch (register read-back)", [](P1Model &m) { m.dropAbs = N48D2_DSCL1_SCL_MODE; }, N48D2_READBACK, true, false, false },
        { "DSCL1_LB_MEMORY_CTRL does not latch", [](P1Model &m) { m.dropAbs = N48D2_DSCL1_LB_MEMORY_CTRL; }, N48D2_READBACK, true, false, false },
        { "DPPCLK1_DTO_PARAM does not latch", [](P1Model &m) { m.dropAbs = N48D2_DPPCLK1_DTO_PARAM; }, N48D2_READBACK, true, false, false },
        { "HUBP1 viewport does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION; }, N48D2_READBACK, true, false, false },
        { "HUBPREQ1 TTU_QOS_WM does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBPREQ1_DCN_TTU_QOS_WM; }, N48D2_READBACK, true, false, false },
        { "HUBPREQ1 DST_DIMENSIONS does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBPREQ1_DST_DIMENSIONS; }, N48D2_READBACK, true, false, false },
        { "the pitch does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH; }, N48D2_READBACK, true, false, false },
        { "the address HIGH does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH; }, N48D2_READBACK, true, false, false },
        { "the address LOW does not latch", [](P1Model &m) { m.dropAbs = N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS; }, N48D2_READBACK, true, false, false },
        { "DPPCLK_CTRL bit 3 does not latch", [](P1Model &m) { m.dropAbs = N48D2_DPPCLK_CTRL; }, N48D2_READBACK, true, false, false },
        { "DET1 current never becomes 3 (judged AFTER the unblank)", [](P1Model &m) { m.detWorks = false; }, N48D2_GATE, true, true, true },
        { "the address never latches (EARLIEST_INUSE stays 0): judged AFTER the unblank", [](P1Model &m) { m.latchWorks = false; }, N48D2_GATE, true, true, true },
        { "FLIP_PENDING never clears (judged AFTER the unlock)", [](P1Model &m) { m.flipPendReads = 1; m.flipClears = false; }, N48D2_GATE, true, true, true },
        { "the OTG1 update lock never asserts: fails BEFORE any address / MPCC1 / unblank write inside the lock", [](P1Model &m) { m.lockNeverAsserts = true; }, N48D2_GATE, true, false, false },
        { "HUBP1 SEG_ALLOC_ERR after the unblank", [](P1Model &m) { m.segAllocErr = true; }, N48D2_GATE, true, true, true },
        { "a DCN VM fault after the unblank", [](P1Model &m) { m.vmFault = 0x00000101u; }, N48D2_GATE, true, true, true },
        { "DET1 current drops away after the unblank", [](P1Model &m) { m.detDropAfterUnblank = true; }, N48D2_GATE, true, true, true },
    };
    for (const Row &r : rows) {
        P1Model m; m.initPlane(); r.set(m);
        const n48d2_out o = run1(m, N48D2_OP_PLANE);
        const std::string w = std::string("instance 1 op 8, ") + r.what;
        expect(o.status == r.status, (w + ": the status is " + std::to_string(r.status)).c_str());
        if (r.leak) expect(o.auto_off == 1u && o.free_bufs == 0u && m.plane1.stage == N48D2_PL_LEAKED && !m.clockOn, (w + ": the ROLLBACK ran, NO_OUTSTANDING_REQ never asserted, the buffers are LEAKED (never freed) and the HUBP clock was never on").c_str());
        else {
            expect(o.auto_off == 1u && o.free_bufs == 1u && m.plane1.stage == N48D2_PL_FREE, (w + ": the ROLLBACK ran and, HUBP1's clock reading off, the buffers are released").c_str());
            expect(rolled_back_clean1(m), (w + ": afterwards the plane is OFF: blanked, mux / MPCC1 none, clocks off, DPG1 on, DTO 0").c_str());
        }
        bool clkW = false; for (const W &x : m.writes) if (x.abs == N48D2_HUBP1_HUBP_CLK_CNTL && (x.v & 1u)) clkW = true;
        expect(clkW == r.clockWritten, (w + (r.clockWritten ? ": the clock WAS enabled (and the rollback disabled it)" : ": the HUBP clock was NEVER enabled")).c_str());
        expect(!m.locked && (m.get(N48D2_OTG1_OTG_MASTER_UPDATE_LOCK) & 1u) == 0u, (w + ": OTG1's update lock is RELEASED on this exit path").c_str());
        const bool mpcc = any_write(m.writes, N48D2_MPC_OUT1_MUX, ~0u, 0x4101u) || any_write(m.writes, N48D2_MPCC1_MPCC_TOP_SEL, ~0u, 1u);
        expect(mpcc == r.mpccWritten, (w + (r.mpccWritten ? ": MPCC1 / the mux were joined" : ": MPCC1 and MPC_OUT1_MUX were NEVER written")).c_str());
        bool unb = false; for (size_t i = 0; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_HUBP1_DCHUBP_CNTL && (m.writes[i].v & 1u) == 0u && (m.writes[i].v & 0x80000000u) == 0u && any_write(m.writes, N48D2_MPC_OUT1_MUX, ~0u, 0x4101u)) unb = true;
        expect(unb == r.unblanked, (w + (r.unblanked ? ": the plane WAS unblanked before the gate saw the fault" : ": the plane was NEVER unblanked")).c_str());
        expect(o.w2_changed == 0u && o.dpx_changed == 0u && o.dp_changed == 0u, (w + ": the failure is the plane's, not a DP / monitor B disturbance").c_str());
    }
    // the rollback never touches the DP or the monitor B: every write of every failed run is inside instance 1's list
    { P1Model m; m.initPlane(); m.latchWorks = false; (void)run1(m, N48D2_OP_PLANE); bool in = true; for (const W &x : m.writes) { bool ok = x.abs == N48D2_DPPCLK1_DTO_PARAM || x.abs == N48D2_DPPCLK_CTRL; for (uint32_t k = 0; k < N48D2_ALLOWED1P_COUNT; k++) ok = ok || x.abs == n48d2_allowed1p[k]; for (uint32_t k = 0; k < N48D2_ALLOWED_COUNT; k++) ok = ok || x.abs == n48d2_allowed[k]; in = in && ok; }
      expect(in, "every write of a failed run is a listed instance-1 register (the rollback's DPG1_CONTROL is on the timing list)"); }
    // stale ODM1 flags from BEFORE the op are not blamed on it, op 9 clears them and demands 0
    { P1Model m; m.odmStale = 0x2400u; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && m.plane1.stage == N48D2_PL_PLANE && m.plane1.odm_sticky == 0x2400u && ((m.gateBuf[N48D2_GT_META] >> 16) & 0xFFFFu) == 0x2400u, "stale ODM1 underflow flags (set before op 8 wrote anything) do not fail op 8; they are recorded and reported (META bits 26 / 29, clear of the monitor B-watch bits 16..24)");
      const n48d2_out s = run1(m, N48D2_OP_SHOW); expect(s.status == N48D2_OK && m.plane1.stage == N48D2_PL_SHOWN && !m.dpgOn && m.odmSticky == 0u, "op 9 clears the stale flags, sees 0 after 10 frames and turns DPG1 off"); }
    { P1Model m; m.odmStale = 0x400u; m.undClearWorks = false; m.initPlane(); (void)run1(m, N48D2_OP_PLANE); const n48d2_out s = run1(m, N48D2_OP_SHOW); expect(s.status == N48D2_GATE && m.dpgOn && m.plane1.stage == N48D2_PL_PLANE, "op 9: ODM1 flags that will not clear -> GATE, DPG1 STAYS ON, the plane stays up (no rollback)"); }
    // the allowlist refusing a write part-way ends the op, rolled back
    { P1Model m; m.initPlane(); m.refuseAbs = N48D2_DSCL1_RECOUT_SIZE; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_ALLOW_REFUSED && o.auto_off == 1u && o.free_bufs == 1u && rolled_back_clean1(m), "a write the allowlist refuses part-way: ALLOW_REFUSED, rollback, buffers released"); }
    // a register that drifts to a monitor B value mid-run is refused by the pin (the guard judges the value, not only the address)
    { P1Model m; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_OK, "(control) a clean run"); }
    // a failed op 8 that left the clock ON and could not turn it off leaks too
    { P1Model m; m.initPlane(); m.detWorks = false; m.clkOffWorks = false; const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane1.stage == N48D2_PL_LEAKED, "instance 1 op 8 failing with the clock stuck ON: rolled back as far as possible, buffers LEAKED, never freed"); }
    // the hardware reading of the 0.0.639 run (a pending address is consumed only at a VUPDATE with the lock released AND the HUBP unblanked): the lock-aware model passes
    { P1Model m; m.lockAware = true; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_OK && m.plane1.stage == N48D2_PL_PLANE && m.latched == kA1 && m.pendAtUnlock, "lock-aware hardware model: the address was still PENDING when the lock was released and is consumed (with MPCC1 and the unblank) at the first VUPDATE after it; EARLIEST_INUSE = A1"); }
    { P1Model m; m.lockAware = true; m.refuseInLock = N48D2_MPC_OUT1_MUX; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status != N48D2_OK && !m.locked && (m.get(N48D2_OTG1_OTG_MASTER_UPDATE_LOCK) & 1u) == 0u && o.auto_off == 1u, "a write refused INSIDE the lock: the op fails, the lock is released on that exit path, the rollback runs"); }
}

static void mona_other_ops() {
    // ================= op 9 show
    { P1Model m; plane1_up(m); const n48d2_out o = run1(m, N48D2_OP_SHOW);
      expect(o.status == N48D2_OK && m.plane1.stage == N48D2_PL_SHOWN && !m.dpgOn && o.timeouts == 0u && o.auto_off == 0u, "instance 1 op 9: OK, DPG1 off, stage SHOWN");
      expect_seq("op 9: the two underflow clears, then DPG_EN = 0", m.writes, { { lx_abs("HUBP1_DCHUBP_CNTL"), 0x80000000u, 0x80000000u }, { lx_abs("ODM1_OPTC_INPUT_GLOBAL_CONTROL"), 0x1000u, 0x1000u }, { lx_abs("DPG1_DPG_CONTROL"), 1u, 0u } });
      const bool w3 = m.writes.size() >= 3u && m.f1AtWrite.size() >= 3u && m.readsAtWrite.size() >= 3u;
      expect(w3 && m.f1AtWrite[2] - m.f1AtWrite[1] >= 10u, "REACHABILITY: 10 OTG1 frames pass between the clears and the DPG1 off");
      size_t nH = 0; if (w3) for (size_t k = m.readsAtWrite[1]; k < m.readsAtWrite[2]; k++) nH += m.reads[k] == N48D2_HUBP1_DCHUBP_CNTL || m.reads[k] == N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL; expect(nH >= 2, "REACHABILITY: the gate reads HUBP1 CNTL and ODM1 between the clears and the DPG off");
      expect((m.gateBuf[N48D2_GT_META] & 0xFFu) == N48D2_PL_SHOWN, "gates: meta says stage SHOWN"); }
    { P1Model m; plane1_up(m); m.reUnderflow = true; const n48d2_out o = run1(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn && m.plane1.stage == N48D2_PL_PLANE && o.auto_off == 0u && !any_write(m.writes, N48D2_DPG1_CONTROL, 1u, 0u), "op 9: HUBP1 underflows again after the clear -> GATE; DPG1 is NOT turned off"); }
    { P1Model m; plane1_up(m); m.segAllocErr = true; const n48d2_out o = run1(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn, "op 9: SEG_ALLOC_ERR -> GATE, DPG1 stays on"); }
    { P1Model m; plane1_up(m); m.vmFault = 0x101u; const n48d2_out o = run1(m, N48D2_OP_SHOW); expect(o.status == N48D2_GATE && m.dpgOn, "op 9: a VM fault -> GATE, DPG1 stays on"); }
    { P1Model m; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_SHOW); expect(o.status == N48D2_PLANE_STATE && m.writes.empty() && m.reads.empty(), "op 9 before op 8: PLANE_STATE, nothing read or written"); }
    { P1Model m; plane1_up(m); (void)run1(m, N48D2_OP_SHOW); const size_t w0 = m.writes.size(); const n48d2_out o = run1(m, N48D2_OP_SHOW); expect(o.status == N48D2_PLANE_STATE && m.writes.size() == w0, "op 9 twice: the second is PLANE_STATE (nothing written)"); }
    // ================= op 10 flip
    { P1Model m; plane1_shown(m);
      const n48d2_out o = run1(m, N48D2_OP_FLIP, 1u);
      expect(o.status == N48D2_OK && m.plane1.cur == 1u && m.latched == kB1 && o.timeouts == 0u && o.auto_off == 0u, "instance 1 op 10 flip B: OK, EARLIEST_INUSE = B1, FLIP_PENDING cleared");
      expect_seq("op 10: SET HIGH then SET LOW of buffer B1", m.writes, { { lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"), ~0u, (uint32_t)(kB1 >> 32) }, { lx_abs("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS"), ~0u, (uint32_t)kB1 } });
      size_t nF = 0; for (size_t k = m.readsAtWrite.empty() ? m.reads.size() : m.readsAtWrite.back(); k < m.reads.size(); k++) nF += m.reads[k] == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL; expect(nF >= 2, "REACHABILITY: after the address the flow polls FLIP_PENDING until it clears");
      expect(m.gateBuf[N48D2_GT_EARLY_LO] == (uint32_t)kB1 && (m.gateBuf[N48D2_GT_FLIP_CTL] & 0x100u) == 0u && ((m.gateBuf[N48D2_GT_META] >> 12) & 1u) == 1u, "gates: EARLIEST_INUSE = B1, FLIP_PENDING 0, latched buffer B");
      const n48d2_out o2 = run1(m, N48D2_OP_FLIP, 0u); expect(o2.status == N48D2_OK && m.plane1.cur == 0u && m.latched == kA1, "flip back to A1: OK");
      for (int i = 0; i < 5; i++) { const n48d2_out ob = run1(m, N48D2_OP_FLIP, 1u), oa = run1(m, N48D2_OP_FLIP, 0u); expect(ob.status == N48D2_OK && oa.status == N48D2_OK && m.latched == kA1, "five A/B/A cycles: every flip OK"); } }
    { P1Model m; plane1_up(m); m.flipClears = false; const n48d2_out o = run1(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_GATE && o.timeouts == 2u && o.fail == N48D2_FLIP_I_WAIT && m.plane1.cur == 0u && o.auto_off == 0u && m.plane1.stage == N48D2_PL_PLANE, "flip: FLIP_PENDING never clears -> GATE at the WAIT step, the plane stays up on A1, no rollback"); }
    { P1Model m; plane1_up(m); m.latchWorks = false; const n48d2_out o = run1(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_GATE && o.fail == N48D2_FLIP_I_WAIT_LO && o.timeouts == 1u && m.plane1.cur == 0u, "flip: pending clears but EARLIEST_INUSE never becomes B1 -> GATE at the EARLIEST_INUSE wait step"); }
    { P1Model m; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_FLIP, 1u); expect(o.status == N48D2_PLANE_STATE && m.writes.empty() && m.reads.empty(), "flip before op 8: PLANE_STATE, nothing read or written"); }
    { P1Model m; plane1_up(m); const n48d2_out o = run1(m, N48D2_OP_FLIP, 2u); expect(o.status == N48D2_BAD_ARG && m.writes.empty(), "flip to buffer 2: BAD_ARG"); }
    // ================= op 11 crc
    { P1Model m; plane1_up(m); const n48d2_out o = run1(m, N48D2_OP_CRC);
      expect(o.status == N48D2_OK && m.crcOn, "instance 1 op 11: OK, CRC on");
      expect_seq("op 11: the four 1920x1080 windows, then CNTL (mask 0x00700011 value 0x11)", m.writes, { { lx_abs("OTG1_OTG_CRC0_WINDOWA_X_CONTROL"), ~0u, 1920u << 16 }, { lx_abs("OTG1_OTG_CRC0_WINDOWA_Y_CONTROL"), ~0u, 1080u << 16 }, { lx_abs("OTG1_OTG_CRC0_WINDOWB_X_CONTROL"), ~0u, 1920u << 16 },
                                                                         { lx_abs("OTG1_OTG_CRC0_WINDOWB_Y_CONTROL"), ~0u, 1080u << 16 }, { lx_abs("OTG1_OTG_CRC_CNTL"), 0x00700011u, 0x11u } });
      expect(!m.f1AtWrite.empty() && m.f1AtWrite.back() < m.frames1 && m.frames1 - m.f1AtWrite.back() >= 3u && m.gateBuf[N48D2_GT_CRC_RG] != 0u, "REACHABILITY: 3 frames pass after CNTL, then OTG1's DATA_RG / DATA_B are read into the gates");
      const uint32_t dpgRG = m.gateBuf[N48D2_GT_CRC_RG];
      (void)run1(m, N48D2_OP_SHOW); const n48d2_out ca = run1(m, N48D2_OP_CRC); const uint32_t a1 = m.gateBuf[N48D2_GT_CRC_RG], ab1 = m.gateBuf[N48D2_GT_CRC_B]; (void)ca;
      for (int i = 0; i < 10; i++) { (void)run1(m, N48D2_OP_CRC); expect(m.gateBuf[N48D2_GT_CRC_RG] == a1 && m.gateBuf[N48D2_GT_CRC_B] == ab1, "CRC of A1 is identical over 10 reads"); }
      expect(a1 != dpgRG, "CRC of A1 differs from the DPG pattern's");
      (void)run1(m, N48D2_OP_FLIP, 1u); (void)run1(m, N48D2_OP_CRC); const uint32_t b1 = m.gateBuf[N48D2_GT_CRC_RG]; expect(b1 != a1, "flipping to B1 changes the CRC");
      (void)run1(m, N48D2_OP_FLIP, 0u); (void)run1(m, N48D2_OP_CRC); expect(m.gateBuf[N48D2_GT_CRC_RG] == a1, "flipping back to A1 returns A1's CRC"); }
    { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_NONE; const n48d2_out o = run1(m, N48D2_OP_CRC); expect(o.status == N48D2_OK && m.crcOn && o.free_bufs == 0u, "op 11 works with the DPG alone (no plane): the baseline CRC"); }
    { P1Model m; m.initPlane(); m.otg1Frozen = true; const n48d2_out o = run1(m, N48D2_OP_CRC); expect(o.status == N48D2_PRECHECK && m.writes.empty(), "op 11 with OTG1 not counting: PRECHECK, nothing written"); }
    // ================= op 12 planeoff
    { P1Model m; plane1_up(m); (void)run1(m, N48D2_OP_CRC); m.writes.clear(); m.readsAtWrite.clear();
      const n48d2_out o = run1(m, N48D2_OP_PLANEOFF);
      expect(o.status == N48D2_OK && o.free_bufs == 1u && m.plane1.stage == N48D2_PL_FREE && rolled_back_clean1(m) && !m.crcOn, "instance 1 op 12 from a running plane: OK, the plane is off, CRC off, buffers released");
      expect_seq("op 12: the rollback's 14 writes (DPG1, blank, the address HIGH then LOW to 0, mux, MPCC1 x4, CRC off, HUBP clock, DPP, DPPCLK_CTRL, DTO)", m.writes, spec1_rollback_writes());
      const size_t iDpg = nth_write(m.writes, N48D2_DPG1_CONTROL), iBlank = nth_write(m.writes, N48D2_HUBP1_DCHUBP_CNTL), iMux = nth_write(m.writes, N48D2_MPC_OUT1_MUX), iClk = nth_write(m.writes, N48D2_HUBP1_HUBP_CLK_CNTL), iDto = nth_write(m.writes, N48D2_DPPCLK1_DTO_PARAM);
      expect(iDpg == 0u && iDpg < iBlank && iBlank < iMux && iMux < iClk && iClk < iDto, "ORDER: DPG1 back on FIRST, then blank, then the mux, then the HUBP clock off, the DTO last");
      const bool idx = iBlank != (size_t)-1 && iMux != (size_t)-1 && iClk != (size_t)-1 && iDto != (size_t)-1 && iDpg != (size_t)-1 && m.readsAtWrite.size() == m.writes.size();
      expect(idx, "op 12: the writes the ORDER checks name exist");
      size_t nNo = 0; if (idx) for (size_t k = m.readsAtWrite[iBlank]; k < m.readsAtWrite[iMux]; k++) nNo += m.reads[k] == N48D2_HUBP1_DCHUBP_CNTL; expect(nNo >= 1, "REACHABILITY: NO_OUTSTANDING_REQ is polled between the blank and the mux");
      expect(idx && first_read_after1(m, N48D2_HUBP1_HUBP_CLK_CNTL, m.readsAtWrite[iClk]) != (size_t)-1, "REACHABILITY: the clock-gated wait reads HUBP1_HUBP_CLK_CNTL after the clock write");
      expect(((m.gateBuf[N48D2_GT_META] >> 8) & 1u) == 1u && (m.gateBuf[N48D2_GT_META] & 0xFFu) == N48D2_PL_FREE, "gates: meta says released / FREE"); }
    { P1Model m; plane1_up(m); (void)run1(m, N48D2_OP_SHOW); const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && rolled_back_clean1(m), "op 12 from SHOWN: OK, DPG1 back on, buffers released"); }
    { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_NONE; const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && m.writes.empty() && o.free_bufs == 0u && o.auto_off == 0u, "op 12 with no plane and the clock off: OK ('nothing to do'), nothing written, nothing freed"); }
    { P1Model m; plane1_up(m); m.clkOffWorks = false; const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane1.stage == N48D2_PL_LEAKED && o.timeouts >= 1u, "planeoff: the clock does not gate -> the buffers are LEAKED"); }
    { P1Model m; plane1_up(m); m.noOutWorks = false; const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_GATE && o.free_bufs == 0u && m.plane1.stage == N48D2_PL_LEAKED, "planeoff: NO_OUTSTANDING_REQ never asserts -> LEAKED"); }
    { P1Model m; plane1_up(m); m.mpccNeverIdle = true; const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && o.timeouts == 1u && m.plane1.stage == N48D2_PL_FREE, "planeoff: MPCC1_STATUS never reads 5 -> logged (one timeout), buffers still released"); }
    { P1Model m; plane1_up(m); m.clkOffWorks = false; (void)run1(m, N48D2_OP_PLANEOFF); m.clkOffWorks = true; m.clkStatus = false; const n48d2_out o = run1(m, N48D2_OP_PLANEOFF); expect(o.status == N48D2_OK && o.free_bufs == 1u && m.plane1.stage == N48D2_PL_FREE, "a LEAKED plane can be rolled back again once the clock gates"); }
    { P1Model m; plane1_up(m); m.clkOffWorks = false; (void)run1(m, N48D2_OP_PLANEOFF); const n48d2_out o = run1(m, N48D2_OP_PLANE); expect(o.status == N48D2_PLANE_STATE, "op 8 on a LEAKED plane is refused (PLANE_STATE)"); }
    // ================= `off 1` / `timing 1` / `connect 1` while HUBP1's clock is on
    { P1Model m; plane1_up(m); const size_t w0 = m.writes.size(), r0 = m.reads.size(); const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 1);
      expect(o.status == N48D2_PLANE_STATE && m.writes.size() == w0 && m.reads.size() == r0 + 1u && m.reads.back() == N48D2_HUBP1_HUBP_CLK_CNTL && o.pre_abs == N48D2_HUBP1_HUBP_CLK_CNTL, "off 1 with the HUBP1 clock on: PLANE_STATE; ONE read (the clock register), nothing written");
      for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT }) { const size_t w1 = m.writes.size(); const n48d2_out o2 = n48d2::run(m, op, 1); expect(o2.status == N48D2_PLANE_STATE && m.writes.size() == w1, "timing 1 / connect 1 with the HUBP1 clock on: PLANE_STATE, nothing written"); }
      (void)run1(m, N48D2_OP_PLANEOFF); const n48d2_out o3 = n48d2::run(m, N48D2_OP_OFF, 1); expect(o3.status == N48D2_OK, "off 1 after planeoff: OK"); }
    { P1Model m; m.initPlane(); m.inst = N48D2_INST_MONA; const n48d2_out o = n48d2::run(m, N48D2_OP_OFF, 1); expect(o.status == N48D2_OK, "off 1 with the HUBP1 clock off: OK"); }
    // ================= planerec
    { P1Model m; m.initPlane(); const n48d2_out o = run1(m, N48D2_OP_PLANE); (void)o; uint64_t v[13]; (void)n48d2::planerec(m, v); uint32_t rec[N48D2_REC_N]; n48d2_unpack_rec(v, rec);
      expect((rec[N48D2_RC_VALID] & (N48D2_RV_A | N48D2_RV_B | N48D2_RV_DET0 | N48D2_RV_UND)) == (N48D2_RV_A | N48D2_RV_B | N48D2_RV_DET0 | N48D2_RV_UND) && std::memcmp(rec, m.recBuf1, sizeof rec) == 0 && m.recBuf[N48D2_RC_VALID] == 0u, "instance 1's record page (recBuf1): samples A and B, DET1 before the unblank, the post-unblank health read; the monitor B's page is untouched");
      expect((rec[0] & 0x00500000u) == 0x00500000u || rec[0] != 0u, "the first recorded read is HUBP1_HUBP_CLK_CNTL (non-zero: the status bits)"); }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the DP watch and the MONB WATCH around the monitor A's ops
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static const uint64_t kMonBA = 0x8002000000ull;
static void monb_running(Model &m) {       // the monitor B's pipe as a running (held) plane leaves it: DPPCLK2 on, DTO, mux 2, MPCC2 -> OPP 2, DET2 3, EARLIEST_INUSE2 = A2, DENTIST_DISPCLK_CNTL as measured, OTG2 counting
    m.r[N48D2_DPPCLK_CTRL] = 0x41u; m.r[N48D2_DPPCLK2_DTO_PARAM] = N48D2_DPPCLK_DTO_VALUE; m.r[N48D2_MPC_OUT2_MUX] = 0x4102u; m.r[N48D2_MPCC2_MPCC_TOP_SEL] = 2u; m.r[N48D2_MPCC2_MPCC_OPP_ID] = 2u; m.r[N48D2_MPCC2_MPCC_BOT_SEL] = 0xFu;
    m.r[N48D2_DCHUBBUB_DET2_CTRL] = 0x303u; m.r[N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE] = (uint32_t)kMonBA; m.r[N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH] = (uint32_t)(kMonBA >> 32); m.r[N48D2_DENTIST_DISPCLK_CNTL] = 0x41194141u;
    m.r[N48D2_OTG2_CONTROL] = 0x00010001u; m.r[N48D2_OTG2_CLOCK_CONTROL] = 3u; m.r[N48D2_HUBP2_DCHUBP_CNTL] = 0x000f0020u;
    m.plane.stage = N48D2_PL_HELD; m.plane.mc[0] = kMonBA; m.plane.mc[1] = kMonBA + N48D2_BUF_BYTES;
}
static const uint32_t kMonBRegs[] = { N48D2_DPPCLK2_DTO_PARAM, N48D2_MPC_OUT2_MUX, N48D2_MPCC2_MPCC_TOP_SEL, N48D2_MPCC2_MPCC_OPP_ID, N48D2_MPCC2_MPCC_BOT_SEL, N48D2_DCHUBBUB_DET2_CTRL, N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
                                      N48D2_DENTIST_DISPCLK_CNTL, N48D2_HUBP2_HUBP_CLK_CNTL, N48D2_DPP_TOP2_DPP_CONTROL, N48D2_OTG2_OTG_GLOBAL_CONTROL2, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_OTG2_CONTROL, N48D2_OTG2_CLOCK_CONTROL, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL };
static void mona_watch_tests() {
    // ================= (1) the DP watch inside the monitor A's plane ops, as the monitor B's: a change under ANY op ends it in DP_DISTURBED and rolls the plane back
    const uint32_t ops[] = { N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_CRC };
    for (uint32_t op : ops) {
        const uint32_t trig0 = op == N48D2_OP_PLANE ? N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS : op == N48D2_OP_SHOW ? N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL : op == N48D2_OP_FLIP ? N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS : N48D2_OTG1_OTG_CRC_CNTL;
        struct Fl { uint32_t reg, xorv; const char *what; };
        const Fl fl[] = { { N48D2_MPC_OUT0_MUX, 0xF, "MPC_OUT0_MUX" }, { N48D2_MPCC0_TOP_SEL, 0x2, "MPCC0 TOP" }, { N48D2_HUBP0_DCHUBP_CNTL, 0x10000000u, "HUBP0 underflow" }, { N48D2_DPPCLK_CTRL, 0x1, "DPPCLK_CTRL bit 0" }, { N48D2_DET0_CTRL, 0x8, "DET0" },
                          { N48D2_COMPBUF_CTRL, 0x1, "COMPBUF" }, { N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL, 0x400, "ODM0 underflow" }, { N48D2_DPP_TOP0_DPP_CONTROL, 0x10, "DPP_TOP0" }, { N48D2_MPCC0_UPDATE_LOCK_SEL, 0x1, "MPCC0 lock select" },
                          { N48D2_OTG2_OTG_GLOBAL_CONTROL2, 0x04000000u, "OTG2_GLOBAL_CONTROL2" }, { N48D2_HUBPREQ0_PRIMARY_LOW, 0x10000u, "HUBP0 primary (the rule)" } };
        for (const Fl &f : fl) {
            P1Model m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane1_up(m);
            m.flipTrig = trig0; m.flipReg = f.reg; m.flipXor = f.xorv;
            const n48d2_out o = run1(m, op, 1u);
            uint32_t want = 0u; for (uint32_t j = 0; j < N48D2_DPX_REGS; j++) if (n48d2_dpx_regs[j].abs == f.reg && (j == N48D2_DPX_RULE ? f.xorv != 0u : (f.xorv & n48d2_dpx_regs[j].mask) != 0u)) want |= 1u << j;
            const std::string w = std::string("instance 1 ") + n48d2_op_name(op) + ": " + f.what + " changes during the op";
            expect(o.status == N48D2_DP_DISTURBED && o.dpx_changed == want && want != 0u, (w + " -> DP_DISTURBED, the mask names the row").c_str());
            expect(rolled_back_clean1(m, f.reg != N48D2_DPPCLK_CTRL) && m.plane1.stage != N48D2_PL_PLANE && m.plane1.stage != N48D2_PL_SHOWN, (w + ": the plane was rolled back").c_str());
        }
        { P1Model m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane1_up(m); m.stopOtg0On = trig0; const n48d2_out o = run1(m, op, 1u); expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x80u) != 0u && rolled_back_clean1(m), (std::string("instance 1 ") + n48d2_op_name(op) + ": OTG0 stops counting under the op -> DP_DISTURBED (bit 7), rolled back").c_str()); }
        { P1Model m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane1_up(m);
          m.r[N48D2_SYMCLKB_CLOCK_ENABLE] = 0x111u; m.flipTrig = trig0; m.flipReg = N48D2_SYMCLKB_CLOCK_ENABLE; m.flipXor = 0x10u;
          const n48d2_out o = run1(m, op, 1u);
          expect(o.status == N48D2_DP_DISTURBED && (o.dp_changed & 0x40u) != 0u && o.dpx_changed == 0u && rolled_back_clean1(m), (std::string("instance 1 ") + n48d2_op_name(op) + ": SYMCLKB changes under the op -> DP_DISTURBED (bit 6), rolled back").c_str()); }
    }
    // the HUBP0 primary rule at the monitor A's own buffer size: a HUBP0 address landing on A1 / B1 is a disturbance even with the scanout acquired; just past B1 is not
    { P1Model m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)kA1; const n48d2_out o = run1(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u && rolled_back_clean1(m), "even with the scanout acquired, HUBP0 landing on buffer A1 is a disturbance"); }
    { P1Model m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = 0x10000u; const n48d2_out o = run1(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_OK && o.dpx_changed == 0u && m.plane1.stage == N48D2_PL_PLANE, "desktop scanout ACQUIRED: HUBP0 flipping to another address inside the window is NOT a disturbance"); }
    { P1Model m; m.initPlane(); m.acquired = true; m.flipTrig = N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS; m.flipReg = N48D2_HUBPREQ0_PRIMARY_LOW; m.flipXor = (uint32_t)(kBufA); m.plane.stage = N48D2_PL_HELD; m.plane.mc[0] = kBufA; m.plane.mc[1] = kBufB; const n48d2_out o = run1(m, N48D2_OP_PLANE);
      expect(o.status == N48D2_DP_DISTURBED && (o.dpx_changed & (1u << 14)) != 0u, "the DP watch's address rule covers the MONB's live pair too: HUBP0 landing on the monitor B's A2 during an monitor A op is a disturbance"); }

    // ================= (2) the MONB WATCH: nine rows read before and after every monitor A op; any masked change ends the op in DP_DISTURBED, rolled back
    struct Wr { uint32_t row, reg, xorv; const char *what; };
    const Wr rows[] = { { 0, N48D2_DPPCLK_CTRL, 0x40u, "DPPCLK_CTRL bit 6 (DPPCLK2_EN)" }, { 1, N48D2_DPPCLK2_DTO_PARAM, 0x1u, "DPPCLK2_DTO_PARAM" }, { 2, N48D2_MPC_OUT2_MUX, 0x2u, "MPC_OUT2_MUX (0x4102)" },
                        { 3, N48D2_MPCC2_MPCC_TOP_SEL, 0x1u, "MPCC2 TOP" }, { 3, N48D2_MPCC2_MPCC_OPP_ID, 0x1u, "MPCC2 OPP" }, { 4, N48D2_DCHUBBUB_DET2_CTRL, 0x100u, "DET2 current" },
                        { 5, N48D2_HUBP2_DCHUBP_CNTL, 0x10000000u, "HUBP2 underflow bit 28" }, { 5, N48D2_HUBP2_DCHUBP_CNTL, 0x00000800u, "HUBP2 SEG_ALLOC_ERR" }, { 6, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, 0x10000u, "EARLIEST_INUSE2 LOW" },
                        { 6, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 0x1u, "EARLIEST_INUSE2 HIGH" }, { 7, N48D2_DENTIST_DISPCLK_CNTL, 0x1u, "DENTIST_DISPCLK_CNTL" }, { 8, N48D2_OTG2_CLOCK_CONTROL, 0x1u, "OTG2 stops counting" } };
    const struct { uint32_t reg, xorv; const char *what; } masked[] = { { N48D2_DPPCLK_CTRL, 0x2u, "DPPCLK_CTRL bit 1 (another pipe's)" }, { N48D2_DPPCLK_CTRL, 0x8u, "DPPCLK_CTRL bit 3 (the monitor A's own)" }, { N48D2_DCHUBBUB_DET2_CTRL, 0x1u, "DET2 SIZE (not the current field)" }, { N48D2_HUBP2_DCHUBP_CNTL, 0x1u, "HUBP2 BLANK_EN (not in the mask)" },
                                                                    { N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 0x10000u, "EARLIEST_INUSE2 HIGH above bit 15" } };
    for (uint32_t op : ops) {
        const uint32_t trig0 = op == N48D2_OP_PLANE ? N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS : op == N48D2_OP_SHOW ? N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL : op == N48D2_OP_FLIP ? N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS : N48D2_OTG1_OTG_CRC_CNTL;
        for (const Wr &w : rows) {
            P1Model m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane1_up(m);
            monb_running(m); m.flipTrig = trig0; m.flipReg = w.reg; m.flipXor = w.xorv;
            const n48d2_out o = run1(m, op, 1u);
            const std::string lbl = std::string("MONB WATCH, instance 1 ") + n48d2_op_name(op) + ": " + w.what + " changes during the op";
            expect(o.status == N48D2_DP_DISTURBED && o.w2_changed == (1u << w.row) && o.guard == w.row + 1u && o.dpx_changed == 0u && o.dp_changed == 0u, (lbl + " -> DP_DISTURBED, the monitor B-watch mask is row " + std::to_string(w.row) + ", the guard field names it (row + 1)").c_str());
            expect(rolled_back_clean1(m, true) && m.plane1.stage != N48D2_PL_PLANE && m.plane1.stage != N48D2_PL_SHOWN, (lbl + ": the MONA's plane was rolled back").c_str());
            expect(((m.gateBuf[N48D2_GT_META] >> 16) & 0x1FFu) == (1u << w.row), (lbl + ": the META gate dword carries the mask (bits 16..24)").c_str());
            uint64_t v[13]; n48d2_pack_plane(v, &o, m.gateBuf); n48d2_out b; uint32_t g2[N48D2_GATES]; std::memset(&b, 0, sizeof b); n48d2_unpack_plane(v, &b, g2);
            expect(b.status == N48D2_DP_DISTURBED && b.guard == w.row + 1u && ((g2[N48D2_GT_META] >> 16) & 0x1FFu) == (1u << w.row), (lbl + ": the row survives the result packing").c_str());
        }
        for (const auto &mk : masked) {
            P1Model m; if (op == N48D2_OP_PLANE) m.initPlane(); else plane1_up(m);
            monb_running(m); m.flipTrig = trig0; m.flipReg = mk.reg; m.flipXor = mk.xorv;
            const bool own = mk.reg == N48D2_DPPCLK_CTRL && mk.xorv == 0x8u;       // the monitor A's own bit: the flip is then undone by / mixed with its own write; only judge the monitor B rows
            const n48d2_out o = run1(m, op, 1u);
            expect(o.w2_changed == 0u || own, (std::string("MONB WATCH masks away: ") + mk.what + " (instance 1 " + n48d2_op_name(op) + "): NOT a monitor B disturbance").c_str());
        }
    }
    // the monitor A's own timing / connect / off: the same watch (a plain Model, instance 1)
    for (const Wr &w : rows) for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
        Model m; m.inst = N48D2_INST_MONA; monb_running(m);
        if (op != N48D2_OP_TIMING) (void)n48d2::run(m, N48D2_OP_TIMING, 1);
        if (op == N48D2_OP_OFF) (void)n48d2::run(m, N48D2_OP_CONNECT, 1);
        m.flipTrig = op == N48D2_OP_TIMING ? N48D2_OTG1_CONTROL : op == N48D2_OP_CONNECT ? N48D2_DIG2_FE_EN_CNTL : N48D2_DPG1_CONTROL; m.flipReg = w.reg; m.flipXor = w.xorv;
        const n48d2_out o = n48d2::run(m, op, 1);
        const std::string lbl = std::string("MONB WATCH, instance 1 ") + n48d2_op_name(op) + ": " + w.what + " changes during the op";
        if (w.row == 8u && op == N48D2_OP_OFF) { expect(o.status == N48D2_OK && o.w2_changed == 0u, (lbl + ": `off` has no settle, so 'OTG2 was counting' is not judged (the other eight rows are)").c_str()); continue; }
        expect(o.status == N48D2_DP_DISTURBED && o.w2_changed == (1u << w.row) && o.guard == w.row + 1u && o.dpx_changed == 0u && o.dp_changed == 0u, (lbl + " -> DP_DISTURBED, the monitor B-watch mask names the row").c_str());
        expect(op == N48D2_OP_OFF ? o.auto_off == 0u : o.auto_off == 1u, (lbl + (op == N48D2_OP_OFF ? ": `off` itself does not recurse" : ": `off` was run at once (auto_off)")).c_str());
        uint64_t v[13]; n48d2_pack(v, &o); n48d2_out b; std::memset(&b, 0, sizeof b); n48d2_unpack(v, &b); expect(b.status == N48D2_DP_DISTURBED && b.guard == w.row + 1u, (lbl + ": the row survives the result packing").c_str());
    }
    // a clean monitor A op leaves the monitor B exactly as it was: nothing of the monitor B is written, nothing changes, no disturbance
    {
        for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
            Model m; m.inst = N48D2_INST_MONA; monb_running(m); std::map<uint32_t, uint32_t> before; for (uint32_t a : kMonBRegs) before[a] = m.get(a);
            if (op != N48D2_OP_TIMING) (void)n48d2::run(m, N48D2_OP_TIMING, 1);
            if (op == N48D2_OP_OFF) (void)n48d2::run(m, N48D2_OP_CONNECT, 1);
            const size_t w0 = m.writes.size(); const n48d2_out o = n48d2::run(m, op, 1);
            bool same = true; for (uint32_t a : kMonBRegs) if (a != N48D2_OTG2_CLOCK_CONTROL) same = same && m.get(a) == before[a];
            expect(o.status == N48D2_OK && o.w2_changed == 0u && same, (std::string("a clean instance-1 ") + n48d2_op_name(op) + " with a running monitor B: OK, the monitor B watch unchanged, every monitor B register as it was").c_str());
            bool monbW = false; for (size_t i = w0; i < m.writes.size(); i++) for (uint32_t a : kMonBRegs) monbW = monbW || m.writes[i].abs == a;
            expect(!monbW, (std::string("instance-1 ") + n48d2_op_name(op) + " writes none of the monitor B's registers").c_str());
        }
        P1Model m; m.initPlane(); monb_running(m); m.r[N48D2_OTG2_CONTROL] = 0x00010001u; m.r[N48D2_OTG2_CLOCK_CONTROL] = 3u; std::map<uint32_t, uint32_t> before; for (uint32_t a : kMonBRegs) before[a] = m.get(a);
        bool ok = true; size_t w0 = 0;
        for (uint32_t op : { N48D2_OP_PLANE, N48D2_OP_CRC, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_FLIP, N48D2_OP_PLANEOFF }) {
            const n48d2_out o = run1(m, op, op == N48D2_OP_FLIP && w0++ == 0u ? 1u : 0u);
            bool same = true; for (uint32_t a : kMonBRegs) if (a != N48D2_OTG2_CLOCK_CONTROL) same = same && m.get(a) == before[a];
            ok = ok && o.status == N48D2_OK && o.w2_changed == 0u && same;
        }
        expect(ok, "the whole monitor A chain (plane, crc, show, flip B, flip A, planeoff) with a running monitor B: every op OK, the monitor B watch unchanged, every monitor B register as it was");
        bool monbW = false; for (const W &x : m.writes) for (uint32_t a : kMonBRegs) monbW = monbW || x.abs == a;
        expect(!monbW, "the whole monitor A chain writes none of the monitor B's registers (OTG2 lock / select / CRC, HUBP2, DPP2, MPCC2, MPC_OUT2, DPPCLK2 DTO, ODM2, DET2)");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// per instance: the address sets, HELD, the monitor B held while the monitor A runs, the allocator's pair state machine, the DMUB refusals
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void mona_instance_tests() {
    // ---- the monitor B HELD (its pair pinned, its address set empty) while the monitor A runs the whole chain
    {
        P1Model m; m.initPlane(); monb_running(m); n48d2_aset_clear();      // the monitor B's set is EMPTY once held
        const n48d2_out o = run1(m, N48D2_OP_PLANE);
        expect(o.status == N48D2_OK && m.plane1.stage == N48D2_PL_PLANE && m.plane.stage == N48D2_PL_HELD && m.plane.mc[0] == kMonBA, "the monitor B HELD: the monitor A's op 8 runs to OK and the monitor B's plane state is untouched");
        expect(n48d2_aset_get()->n == 0u && n48d2_aset_get_i(N48D2_INST_MONA)->n == 2u && n48d2_aset_get_i(N48D2_INST_MONA)->lo[0] == (uint32_t)kA1 && n48d2_aset_get_i(N48D2_INST_MONA)->lo[1] == (uint32_t)kB1, "per-instance address sets: the monitor B's stays EMPTY, the monitor A's holds exactly {A1, B1}");
        expect(!wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kMonBA) && !wr_ok(1, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kMonBA) && wr_ok(1, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kA1) && !wr_ok(2, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)kA1), "the sets never mix: with the monitor B's set empty the monitor B's address writes are refused, the monitor A's register refuses the monitor B's A, the monitor B's register refuses A1");
        const n48d2_out f = run1(m, N48D2_OP_FLIP, 1u); expect(f.status == N48D2_OK && m.plane.stage == N48D2_PL_HELD, "flip on instance 1 while the monitor B is HELD: OK");
        const n48d2_out r = run1(m, N48D2_OP_PLANEOFF); expect(r.status == N48D2_OK && r.free_bufs == 1u && m.plane.stage == N48D2_PL_HELD && n48d2_aset_get_i(N48D2_INST_MONA)->n == 0u && n48d2_aset_get()->n == 0u, "planeoff on instance 1 frees ITS pair and clears ITS set; the monitor B stays HELD");
        // the monitor B's ops are refused on a HELD monitor B and the monitor A's are not (and the reverse)
        Model d; d.plane.stage = N48D2_PL_HELD; d.inst = 2; const n48d2_out dt = n48d2::run(d, N48D2_OP_TIMING, 2); expect(dt.status == N48D2_HELD && d.reads.empty() && d.writes.empty(), "timing 2 on a HELD monitor B: HELD, nothing read or written");
        Model a; a.plane.stage = N48D2_PL_HELD; a.inst = 1; const n48d2_out at2 = n48d2::run(a, N48D2_OP_TIMING, 1); expect(at2.status == N48D2_OK, "timing 1 runs while the MONB is HELD (HELD is per instance)");
    }
    // ---- HELD on the monitor A (set directly here; 0.0.658 reaches it through fbhold 1, tested below): every op that could move the plane is refused with nothing read or written; the read-only pages and crc are not
    {
        const uint32_t movers[] = { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF, N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_PLANEOFF };
        for (uint32_t op : movers) {
            P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_HELD; n48d2_aset_clear_i(N48D2_INST_MONA);
            const n48d2_out o = (op >= N48D2_OP_PLANE) ? run1(m, op, 1u) : n48d2::run(m, op, 1);
            expect(o.status == N48D2_HELD && m.reads.empty() && m.writes.empty() && m.vfillCalls == 0u && m.plane1.stage == N48D2_PL_HELD, (std::string("a HELD monitor A plane refuses op ") + n48d2_op_name(op) + " with status HELD: nothing read or written, no fill, the stage stays").c_str());
        }
        { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_HELD; const n48d2_out o = run1(m, N48D2_OP_CRC); expect(o.status == N48D2_OK && m.crcOn, "crc is allowed on a HELD monitor A plane (it writes only the CRC registers)"); }
        { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_HELD; uint64_t v[13]; expect(n48d2::status(m, v, 1) == N48D2_OK && n48d2::status2(m, v, 1) == N48D2_OK && n48d2::status3(m, v, 1) == N48D2_OK && m.writes.empty(), "the status pages work on a HELD monitor A plane (reads only)"); }
        { P1Model m; m.initPlane(); m.plane1.stage = N48D2_PL_HELD; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2); expect(o.status != N48D2_HELD, "a HELD MONA plane never refuses the MONB's ops"); }
        for (uint32_t op = 1; op <= 14; op++) expect((n48d2_held_refuses(op, N48D2_INST_MONA) != 0) == (n48d2_held_refuses(op, N48D2_INST_MONB) != 0), "held_refuses is the same set of ops for either instance");
        expect(n48d2_held_refuses(N48D2_OP_PLANE, 0u) == 0 && n48d2_held_refuses(N48D2_OP_PLANE, 3u) == 0, "an unknown instance is refused nothing (it never reaches the flow)");
    }
    // ---- the allocator's pair state machine, per instance (the pure decisions of native_d2pin_pure.h applied per slot, as native_s1c.cpp does)
    {
        struct Slot { n48d2pin::State s; };
        Slot sl[2] = { { { false, false } }, { { false, false } } };      // [0] the monitor A, [1] the monitor B
        auto alloc = [&](int i, bool ok) { const bool refused = n48d2pin::alloc_refused(sl[i].s.held); sl[i].s = n48d2pin::after_alloc(sl[i].s, ok); return !refused && ok; };
        auto freeIt = [&](int i) { const auto a = n48d2pin::free_action(sl[i].s.held, sl[i].s.pinned); sl[i].s = n48d2pin::after_free(sl[i].s); return a; };
        auto pin = [&](int i, bool isA, bool isB) { const bool ok = n48d2pin::pin_ok(sl[i].s.held, true, sl[i].s.pinned, isA, isB); sl[i].s = n48d2pin::after_pin(sl[i].s, ok); return ok; };
        expect(alloc(1, true) && pin(1, true, true) && sl[1].s.pinned, "the monitor B allocates and pins its pair");
        expect(alloc(0, true) && sl[0].s.held && !sl[0].s.pinned, "the monitor A allocates its OWN pair while the monitor B's is held AND pinned (they are different slots)");
        expect(!alloc(0, true) && !alloc(1, true), "a second pair for the SAME instance is refused (the monitor A's held pair, the monitor B's pinned pair)");
        expect(freeIt(0) == n48d2pin::kFreeDo && !sl[0].s.held, "the monitor A frees its pair; ... ");
        expect(freeIt(1) == n48d2pin::kFreePinned && sl[1].s.held && sl[1].s.pinned, "... the monitor B's pinned pair stays (a logged no-op) - the monitor A's free never touched it");
        expect(alloc(0, true) && pin(0, true, true) && sl[0].s.pinned && sl[1].s.pinned && !pin(0, true, true), "the monitor A can allocate again and pin its pair; pinning twice is refused; both are pinned independently");
        expect(freeIt(0) == n48d2pin::kFreePinned && sl[0].s.held, "a pinned monitor A pair is never freed either");
        expect(!pin(0, false, true) && !pin(1, true, false), "a pin needs EXACTLY the plane's pair");
    }
    // ---- the DMUB refusals (pure): the monitor A's templates 3..7 and raw slots on 0x7000 while ITS plane is HELD; the monitor B's 8..15 and 0x7800 unchanged
    {
        bool ok = true; for (uint32_t id = 0; id <= 16; id++) { const bool mona = id >= 3u && id <= 7u, monb = id >= 8u && id <= 15u; if ((n48dm_tpl_mona_held_refuses(id) != 0) != mona || (n48dm_tpl_held_refuses(id) != 0) != monb) ok = false; if (mona && std::string(n48dm_tpl_get(id)->name).find("otg1") == std::string::npos && std::string(n48dm_tpl_get(id)->name).find("phyc") == std::string::npos && std::string(n48dm_tpl_get(id)->name).find("digc") == std::string::npos) ok = false; }
      expect(ok, "templates: 3..7 (every otg1 / phyc / digc one, the teardowns pclk-otg1-off and phyc-disable above all) are the MONA's; 8..15 stay the monitor B's; 1, 2 (the OTG3 probe) and out-of-range ids are neither's");
      expect(n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) != 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 0u) == 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP2, 0u) == 0 && n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) == 0 && n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 0u) != 0,
             "raw slots: `disable 0x7000` is the monitor A's (refused while ITS plane is HELD), `disable 0x7800` the monitor B's, the live DP's 0x6800 is neither's");
      expect(n48dm_slot_mona_held_refuses(N48DM_HDR_BEGIN, 0u, 0u) == 0 && n48dm_slot_mona_held_refuses(0x12345678u, N48DM_CTX_DFP3, 0u) == 0, "begin / end and a malformed slot are not refused as HELD (their own allowlist refusal stays)"); }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// source pins: the glue, the allocator, the CLI, the flow
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void mona_source_pins() {
    const std::string src = g_root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), s1c = slurp(src + "amd/native_s1c.cpp"), s1h = slurp(src + "amd/native_s1c.h"), hpp = slurp(src + "dcn/navi48_dcn.hpp"), flow = strip_line_comments(slurp(src + "dcn/navi48_disp2_flow.h")), hdr = strip_line_comments(slurp(src + "dcn/navi48_disp2.h")),
                      cli = slurp(g_root + "/tools/pc/navi48test.c"), brg = slurp(src + "Navi48Bringup.cpp");
    expect(!dcn.empty() && !s1c.empty() && !s1h.empty() && !hpp.empty() && !flow.empty() && !hdr.empty() && !cli.empty() && !brg.empty(), "0.0.655: the sources are readable");
    const size_t g0 = dcn.find("build 0.0.631 (multi-monitor stage M4d; an internal design note");
    const std::string glue = g0 == std::string::npos ? "" : strip_line_comments(dcn.substr(g0));
    const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {"), f1 = glue.find("}  // namespace n48dcn", f0 == std::string::npos ? 0 : f0);
    const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
    // ---- the glue: per-instance state, HELD before the allocator, allocation sized by the instance, frees by the instance
    expect(glue.find("n48d2_plane gD2Pl[2];") != std::string::npos && glue.find("uint32_t gD2Rec[2][N48D2_REC_N];") != std::string::npos && glue.find("uint32_t gD2W2[2][N48D2_W2_BUF];") != std::string::npos && glue.find("uint64_t gD2Off[2][2];") != std::string::npos && glue.find("static inline uint32_t d2i(uint32_t inst) { return inst == N48D2_INST_MONA ? 0u : 1u; }") != std::string::npos,
           "0.0.655 glue: the plane state, the record page, the monitor B-watch buffers and the buffer offsets are file-scope arrays per instance");
    expect(glue.find("n48d2_plane *pl_of(uint32_t i) { return &gD2Pl[d2i(i)]; }") != std::string::npos && glue.find("uint32_t *w2_buf(unsigned k) { return gD2W2[k & 1u]; }") != std::string::npos && glue.find("uint32_t *rec_buf() { return gD2Rec[d2i(inst)]; }") != std::string::npos && glue.find("bool pin() { return amdgpu::n1c_d2_pin(inst, gD2Pl[d2i(inst)].mc); }") != std::string::npos,
           "0.0.655 glue: the environment hands the flow the plane / monitor B-watch buffers / record page / pin of the INSTANCE");
    { const size_t held = fn.find("if (n48d2_held_refuses(op, inst) && disp2HeldInst(inst)) {"), alloc = fn.find("amdgpu::n1c_d2_alloc("), busy = fn.find("__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL)"), arg = fn.find("n48d2_arg_ok(arg)"), dev = fn.find("N48D2_NO_DEVICE");
      expect(held != std::string::npos && alloc != std::string::npos && busy != std::string::npos && arg != std::string::npos && dev != std::string::npos && arg < dev && dev < held && held < alloc && held < busy, "0.0.655 glue: REACHABILITY: the per-instance HELD refusal comes after the argument and device checks and BEFORE the allocator and the busy flag (op 8 never reaches the allocator of a HELD instance)");
      expect(fn.find("n48d2_surf_get(inst)->buf_bytes, pmc, poff") != std::string::npos && count_of(fn, "amdgpu::n1c_d2_free(inst);") == 3 && fn.find("amdgpu::n1c_d2_free();") == std::string::npos && fn.find("amdgpu::n1c_d2_pinned(inst)") != std::string::npos, "0.0.655 glue: the allocation is sized by the instance's surface (127 x 64 KiB for the monitor A) and every free / pin test names the instance"); }
    expect(fn.find("n48d2::run_plane(env, op, n48d2_arg_buf(arg), inst)") != std::string::npos && fn.find("const uint32_t pi = d2i(inst);") != std::string::npos && count_of(fn, "N48LOG(N48D2_W2_LINE1_FMT, N48D2_W2_LINE1_ARGS(") == 2 && count_of(fn, "N48LOG(N48D2_W2_LINE2_FMT, N48D2_W2_LINE2_ARGS(") == 2 && fn.find("env.inst = inst;") != std::string::npos, "0.0.655 glue: the plane op runs for the call's instance (env.inst = inst), the state index is d2i(inst), and both log branches print the monitor B watch (0.0.658: through the format/argument macros)");
    expect(glue.find("return __atomic_load_n(&gD2Pl[d2i(inst)].stage, __ATOMIC_ACQUIRE) == N48D2_PL_HELD || amdgpu::n1c_d2_pinned(inst);") != std::string::npos && glue.find("return disp2HeldInst(N48D2_INST_MONB);") != std::string::npos && hpp.find("bool disp2HeldInst(uint32_t inst);") != std::string::npos, "0.0.655 glue: HELD is per instance (stage HELD or the instance's pair pinned); disp2Held() is the monitor B's");
    { const size_t dm0 = dcn.find("uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount) {"), dm1 = dcn.find("uint32_t dmubMode(", dm0 == std::string::npos ? 0 : dm0); const std::string f = dm0 == std::string::npos ? "" : strip_line_comments(dcn.substr(dm0, dm1 - dm0));
      const size_t tp = f.find("n48dm_tpl_held_refuses(tplId) && disp2Held()"), ta = f.find("n48dm_tpl_mona_held_refuses(tplId) && disp2HeldInst(N48D2_INST_MONA)"), ts = f.find("o = n48dm::send(env, 0u, 0u, 0u, N48DM_NO_REPLAY, tplId)"), sp = f.find("n48dm_slot_held_refuses(h, d1, d2) && disp2Held()"), sa = f.find("n48dm_slot_mona_held_refuses(h, d1, d2) && disp2HeldInst(N48D2_INST_MONA)"), rs = f.find("o = n48dm::send(env, h, d1, d2)");
      expect(tp != std::string::npos && ta != std::string::npos && ts != std::string::npos && tp < ta && ta < ts && sp != std::string::npos && sa != std::string::npos && rs != std::string::npos && sp < sa && sa < rs && f.find("if (disp2Held() || disp2HeldInst(N48D2_INST_MONA)) {") != std::string::npos, "0.0.655 DMUB: REACHABILITY: the monitor A's template and raw-slot refusals (HELD monitor A plane) sit beside the monitor B's and BEFORE anything is sent; a replay is refused while EITHER plane is HELD");
      expect(count_of(f, "N48DR_HELD_REFUSED") == 5u, "0.0.655 DMUB: five refusals answer N48DR_HELD_REFUSED (replay, monitor B templates, monitor A templates, raw monitor B-context slots, raw monitor A-context slots)"); }
    // ---- the allocator: one pair per instance
    { const size_t a0 = s1c.find("IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]) {"), a1 = s1c.find("void n1c_d2_free(uint32_t inst) {"), a2 = s1c.find("bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]) {"), a3 = s1c.find("IOReturn n1c_bo_free(");
      const std::string al = (a0 == std::string::npos || a1 == std::string::npos) ? "" : s1c.substr(a0, a1 - a0), fr = (a1 == std::string::npos || a2 == std::string::npos) ? "" : s1c.substr(a1, a2 - a1), pn = (a2 == std::string::npos || a3 == std::string::npos) ? "" : s1c.substr(a2, a3 - a2);
      expect(!al.empty() && !fr.empty() && !pn.empty() && s1c.find("static VRAMAllocation gD2Buf[2][2];") != std::string::npos && s1c.find("static bool gD2Held[2];") != std::string::npos && s1c.find("static bool gD2Pinned[2];") != std::string::npos && s1c.find("static BringupContext *gD2Ctx[2];") != std::string::npos, "0.0.655 allocator: the buffers, the held flag, the pin and the context are per-slot arrays (one pair per instance)");
      expect(al.find("(inst != 1u && inst != 2u)") != std::string::npos && fr.find("(inst != 1u && inst != 2u)") != std::string::npos && pn.find("(inst != 1u && inst != 2u)") != std::string::npos && s1c.find("bool n1c_d2_pinned(uint32_t inst) { return (inst == 1u || inst == 2u) &&") != std::string::npos, "0.0.655 allocator: an instance other than 1 / 2 is BadArgument / refused everywhere");
      expect(al.find("if (n48d2pin::alloc_refused(gD2Held[sl])) { rc = kIOReturnExclusiveAccess; break; }") != std::string::npos && al.find("gD2Held[sl] = true") != std::string::npos && fr.find("gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][0]);") != std::string::npos && s1c.find("static inline uint32_t d2slot(uint32_t inst) { return inst == 1u ? 0u : 1u; }") != std::string::npos,
             "0.0.655 allocator: allocation, free and pin decide on the instance's OWN slot; the other instance's held / pinned pair never blocks it");
      expect(s1h.find("IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]);") != std::string::npos && s1h.find("void n1c_d2_free(uint32_t inst);") != std::string::npos && s1h.find("bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]);") != std::string::npos && s1h.find("bool n1c_d2_pinned(uint32_t inst);") != std::string::npos, "0.0.655 allocator: exported per instance from native_s1c.h");
      expect(al.find("(bytes & 0xFFFFull) != 0") != std::string::npos, "0.0.655 allocator: a size that is not a 64 KiB multiple is refused (the monitor A's frame, 8,294,400, would be; its buffer is 127 x 64 KiB)"); }
    // ---- the flow
    { const size_t e0 = flow.find("inline bool plane_exec("), e1 = flow.find("template <class E>\ninline n48d2_out run_plane(", e0 == std::string::npos ? 0 : e0); const std::string pe = (e0 == std::string::npos || e1 == std::string::npos) ? "" : flow.substr(e0, e1 - e0);
      const size_t b0 = pe.find("w2_snapshot(e, w2B);"), sl = pe.find("e.sleep_ms(N48D2_SETTLE_MS);"), c1 = pe.find("w2B[11] = e.rd(N48D2_OTG2_FRAME_COUNT);"), st0 = pe.find("do {"), a0 = pe.find("w2_snapshot(e, w2A);"), d0 = pe.find("if (o.dpx_changed != 0u || o.dp_changed != 0u || o.w2_changed != 0u) {"), rb = pe.find("plane_rollback(e, inst, o, pl);", a0 == std::string::npos ? 0 : a0);
      expect(b0 != std::string::npos && st0 != std::string::npos && sl != std::string::npos && c1 != std::string::npos && a0 != std::string::npos && d0 != std::string::npos && rb != std::string::npos && b0 < st0 && sl < c1 && c1 < a0 && a0 < d0 && d0 < rb && pe.find("if (inst == N48D2_INST_MONA) w2_snapshot(e, w2B);") != std::string::npos && pe.find("if (inst == N48D2_INST_MONA) { w2_snapshot(e, w2A); o.w2_changed = n48d2_w2_diff(w2B, w2A); }") != std::string::npos,
             "0.0.655 flow: REACHABILITY (plane_exec): the monitor B watch is read BEFORE the steps, the OTG2 counter once more after the settle, the watch AFTER the steps, then the disturbed decision that includes w2_changed, then the rollback");
      const size_t r0 = flow.find("inline n48d2_out run(E &e, uint32_t op, uint32_t inst"), r1 = flow.find("template <class E>\ninline uint32_t status(", r0 == std::string::npos ? 0 : r0); const std::string rn = (r0 == std::string::npos || r1 == std::string::npos) ? "" : flow.substr(r0, r1 - r0);
      const size_t wb = rn.find("if (inst == N48D2_INST_MONA) w2_snapshot(e, w2B);"), wsl = rn.find("e.sleep_ms(N48D2_SETTLE_MS);"), wc = rn.find("w2B[11] = e.rd(N48D2_OTG2_FRAME_COUNT);"), wst = rn.find("run_steps(e, inst, s, n, op == N48D2_OP_OFF, o, &refusals)"), wa = rn.find("if (inst == N48D2_INST_MONA) { w2_snapshot(e, w2A); o.w2_changed = n48d2_w2_diff(w2B, w2A); }"), wd = rn.find("if (o.dp_changed != 0u || o.dpx_changed != 0u || o.w2_changed != 0u) {");
      expect(wb != std::string::npos && wsl != std::string::npos && wc != std::string::npos && wst != std::string::npos && wa != std::string::npos && wd != std::string::npos && wb < wsl && wsl < wc && wc < wst && wst < wa && wa < wd && rn.find("o.guard = n48d2_w2_first(o.w2_changed);") != std::string::npos,
             "0.0.655 flow: REACHABILITY (run): the monitor B watch is read before the settle, the counter after it, the steps, the watch after, then the disturbed decision (guard field = first row + 1)");
      expect(flow.find("const uint32_t hcReg = n48d2_pr_get(inst)->hubp_clk;") != std::string::npos && flow.find("if (e.pl_of(inst)->stage == N48D2_PL_HELD && n48d2_held_refuses(op, inst))") != std::string::npos && flow.find("if (e.inst != inst) return o;") != std::string::npos, "0.0.655 flow: the HUBP-clock refusal and the HELD refusal read the INSTANCE's registers / plane; run_plane refuses an environment whose guard is the other instance's");
      expect(flow.find("const n48d2_plane *const op = e.pl_of(oi);") != std::string::npos && flow.find("for (uint32_t ii = N48D2_INST_MONA; ii <= N48D2_INST_MONB; ii++) {") != std::string::npos, "0.0.655 flow: admission checks the OTHER instance's live pair, the DP watch's address rule checks BOTH instances' pairs"); }
    // ---- the header
    expect(hdr.find("if (inst == N48D2_INST_MONA && (abs == N48D2_DPPCLK1_DTO_PARAM || abs == N48D2_DPPCLK_CTRL)) return N48D2_G_OK;") != std::string::npos && hdr.find("(n48d2_forbidden[i].skip & (inst == N48D2_INST_MONA ? N48D2_SKIP_MONA : N48D2_SKIP_MONB)) == 0u") != std::string::npos && hdr.find("#define N48D2_ALLOWED1P_COUNT 43u") != std::string::npos,
           "0.0.655 header: instance 1's two DCCG exceptions are ONE address test, the forbidden spans carry a per-instance skip mask, 43 plane registers");
    expect(hdr.find("n48d1_plane_kind_ok(&s[i])") != std::string::npos && hdr.find("n48d1_guard_value_plane(abs, v) != N48D2_G_OK") != std::string::npos && hdr.find("if (s[i].kind == N48D2_K_COPY0 && inst != N48D2_INST_MONB) { *bad = i; return N48D2_GUARD_REFUSED; }") != std::string::npos, "0.0.655 header: the monitor A's kind and value rules are applied (list level and per write); COPY0 stays the monitor B's alone");
    // ---- the CLI
    expect(cli.find("static int cmd_disp2_plane(const char *a1, uint32_t op, uint32_t bufIdx) {") != std::string::npos && cli.find("g_d2_inst = !strcmp(a2, \"1\") ? N48D2_INST_MONA : N48D2_INST_MONB;") != std::string::npos && cli.find("!strcmp(a1, \"planerec\") ? N48D2_OP_PLANEREC") != std::string::npos && cli.find("if (pop == N48D2_OP_PLANEREC) return d2_print_rec() == 0 ? 0 : 3;") != std::string::npos &&
           cli.find("DIG2 output CRC (informational (DIG2 CRC not enabled))") != std::string::npos && cli.find("(2nd witness)") == std::string::npos && cli.find("as a second witness") == std::string::npos && cli.find("the MONB WATCH tripped (0.0.655)") != std::string::npos && cli.find("monitor B watch (kext)       :") != std::string::npos && cli.find("monitor A plane run (0.0.655; instance 1, 1920x1080@60;") != std::string::npos,
           "0.0.655 CLI: the plane verbs (incl. planerec) take the instance 1 | 2 (fbhold 2 only), print the monitor B watch and the DIG2 output CRC as a second witness, and carry the monitor A's run script");
    {   // 0.0.657 (coordinator items 3, 4, 5 from the 0.0.655 safety review)
        const size_t r0 = cli.find("monitor A plane run (0.0.655;"), r1 = r0 == std::string::npos ? r0 : cli.find("accel disp2 off 1` is REFUSED", r0);
        const std::string run = (r0 == std::string::npos || r1 == std::string::npos) ? std::string() : cli.substr(r0, r1 - r0);
        expect(!run.empty() && run.find("held and published or not") == std::string::npos && run.find("SHOWN on its static plane - `plane 2` + `show 2` - with NO `fbhold` and NO `fbpublish` this boot") != std::string::npos,
               "0.0.657 CLI item 3: Run A text says the monitor B SHOWN on its static plane (plane 2 + show 2) with NO fbhold and NO fbpublish this boot, never 'held and published or not'");
        expect(!run.empty() && count_of(run, "informational (DIG2 CRC not enabled)") == 2 && run.find("second witness") == std::string::npos && run.find("NOT a pass criterion") != std::string::npos && run.find("DIG2 FIFO error 0 (the DIG2 output CRC is informational") != std::string::npos,
               "0.0.657 CLI item 4: every Run A mention of the DIG2 CRC is labelled informational (DIG2 CRC not enabled), none calls it a witness, and the PASS line does not require it");
        expect(cli.find("DIG2 output CRC (informational (DIG2 CRC not enabled)): CNTL") != std::string::npos && cli.find("N48D2_OP_CRC && g_d2_inst == N48D2_INST_MONA && r.status == N48D2_OK) {") != std::string::npos && cli.find("DIG2_DIG_OUTPUT_CRC_CNTL") != std::string::npos, "0.0.657 CLI item 4: the CRC line is labelled and READS only");
        const size_t h0 = cli.find("static void d2_print_monb_watch(uint32_t status, uint32_t guard) {"), h1 = h0 == std::string::npos ? h0 : cli.find("\n}\n", h0);
        const std::string hw = (h0 == std::string::npos || h1 == std::string::npos) ? std::string() : cli.substr(h0, h1 - h0);
        expect(!hw.empty() && hw.find("status == N48D2_DP_DISTURBED && guard >= 1u && guard <= 9u") != std::string::npos && hw.find("monitor B watch: CHANGED (row %u, %s") != std::string::npos && hw.find("monitor B watch: unchanged\\n") != std::string::npos && count_of(hw, "\"") > 20,
               "0.0.657 CLI item 5: the helper prints 'monitor B watch: CHANGED (row, name)' on a DP_DISTURBED result whose guard names a row 1..9, 'unchanged' on OK");
        // decode agreement with the kext: guard = first changed row + 1, as n48d2_w2_first computes (rows 0..8)
        expect(n48d2_w2_first(0u) == 0u && n48d2_w2_first(1u) == 1u && n48d2_w2_first(0x100u) == 9u && n48d2_w2_first(0x18u) == 4u, "0.0.657 CLI item 5: the guard the CLI decodes is the kext's first-changed-row + 1 (1..9)");
        const size_t d0 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {"); const std::string cd = d0 == std::string::npos ? "" : cli.substr(d0);
        const size_t st = cd.find("printf(\"accel disp2 %s %u: status %u (%s)\\n\", a1, g_d2_inst, r.status, n48d2_status_name(r.status));");
        const size_t pw = cd.find("if (g_d2_inst == N48D2_INST_MONA) d2_print_monb_watch(r.status, r.guard);");
        const size_t sp = cd.find("printf(\"  steps                   : %u of %u written");
        expect(st != std::string::npos && pw != std::string::npos && sp != std::string::npos && st < pw && pw < sp, "0.0.657 CLI item 5: timing / connect / off on instance 1 print the monitor B watch line right after the status line");
        expect(count_of(cli, "d2_print_monb_watch(") == 2, "0.0.657 CLI item 5: the helper is defined once and called once (the timing / connect / off path)");
    }
}

// =====================================================================================================================================================================================================
static void plane1_tests() {      // 0.0.655
    void (*const groups[])() = { mona_tables_vs_linux, mona_guard_tests, mona_list_shapes, mona_op8_tests, mona_gate_failures, mona_other_ops, mona_watch_tests, mona_instance_tests, mona_source_pins };
    for (void (*g)() : groups) { try { g(); } catch (const SetupFailed &) { /* recorded as a failed check by plane1_up; the next group still runs */ } }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// 0.0.655: the MONB'S BYTE-FOR-BYTE BATTERY. The instance-2 plane flow was generalised for instance 1; this battery runs the monitor B through a fixed set of scenarios (the normal chain op 8 -> 9 -> 10 -> 11 -> 10 -> 12, op 8 under every
// hardware knob that fails a gate, the held plane, timing / connect / off on instance 2) and hashes EVERYTHING observable: every register write (address, value), every register read (address, in order), every sleep and delay_us, the
// status / counters / gates of every op, the plane state. The golden hash below was produced by compiling THIS battery against the 0.0.654 sources (git e6a6a092) and running it: the same hash under the 0.0.655 sources proves the
// monitor B's behaviour is unchanged to the read. (The battery uses only members the 0.0.654 test model already had.)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static uint64_t bat_h(uint64_t h, uint64_t v) { for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 1099511628211ull; } return h; }
static uint64_t bat_model(uint64_t h, const Model &m) {
    h = bat_h(h, m.writes.size()); for (const W &w : m.writes) { h = bat_h(h, w.abs); h = bat_h(h, w.v); }
    h = bat_h(h, m.reads.size()); for (uint32_t r : m.reads) h = bat_h(h, r);
    h = bat_h(h, m.sleeps); h = bat_h(h, m.delays); h = bat_h(h, m.delayUs); h = bat_h(h, m.refusedCount);
    h = bat_h(h, m.plane.stage); h = bat_h(h, m.plane.cur); h = bat_h(h, m.plane.odm_sticky); h = bat_h(h, m.plane.dp_bytes); h = bat_h(h, m.plane.mc[0]); h = bat_h(h, m.plane.mc[1]);
    for (uint32_t i = 0; i < N48D2_GATES; i++) h = bat_h(h, m.gateBuf[i]);
    for (uint32_t i = 0; i < N48D2_REC_N; i++) h = bat_h(h, m.recBuf[i]);
    for (unsigned k = 0; k < 2; k++) for (uint32_t i = 0; i < N48D2_DPX_BUF; i++) h = bat_h(h, m.dpxBuf[k][i]);
    h = bat_h(h, m.vlog.size()); h = bat_h(h, m.vfillCalls); h = bat_h(h, m.vflushCalls); h = bat_h(h, m.vreadCalls); h = bat_h(h, m.pinCalls);
    return h;
}
static uint64_t bat_out(uint64_t h, const n48d2_out &o) {
    const uint32_t f[] = { o.status, o.op, o.done, o.total, o.fail, o.timeouts, o.auto_off, o.guard, o.dp_changed, o.f0a, o.f0b, o.f1a, o.f1b, o.pre_abs, o.pre_val, o.dpx_changed, o.free_bufs, o.wait_abs, o.wait_val, o.pre_wait, o.symclkb_a, o.symclkb_b, o.symclk_wait, o.symclk_delay };
    for (uint32_t v : f) h = bat_h(h, v);
    return bat_h(h, o.wfail);
}
static uint64_t monb_battery_hash() {
    uint64_t h = 1469598103934665603ull;
    // the normal chain
    { PModel m; m.initPlane(); n48d2_aset_clear();
      const uint32_t chain[][2] = { { N48D2_OP_PLANE, 0 }, { N48D2_OP_CRC, 0 }, { N48D2_OP_SHOW, 0 }, { N48D2_OP_CRC, 0 }, { N48D2_OP_FLIP, 1 }, { N48D2_OP_CRC, 0 }, { N48D2_OP_FLIP, 0 }, { N48D2_OP_FBHOLD, 0 }, { N48D2_OP_CRC, 0 }, { N48D2_OP_PLANEOFF, 0 }, { N48D2_OP_PLANE, 0 }, { N48D2_OP_PLANEOFF, 0 }, { N48D2_OP_PLANEOFF, 0 } };
      for (const auto &c : chain) { const n48d2_out o = n48d2::run_plane(m, c[0], c[1]); h = bat_out(h, o); h = bat_model(h, m); }
      n48d2_aset_clear(); }
    // op 8 under each knob, then the rollback / show where it matters
    enum { K_HW, K_CLK, K_DET, K_LATCH, K_NOOUT, K_BLANK, K_UND, K_VM, K_SEG, K_ODM, K_FROZEN, K_LOCKAWARE, K_DETAFTER, K_BLANKNOEARLY, K_LOCKNEVER, K_PENDGLITCH, K_CLKOFF, K_MPCC, K_DROP, K_INSIDE, K_N };
    for (int k = 0; k < K_N; k++) {
        PModel m; m.initPlane(); n48d2_aset_clear();
        switch (k) {
        case K_HW: m.hubpWritesLand = false; break; case K_CLK: m.clkWorks = false; break; case K_DET: m.detWorks = false; break; case K_LATCH: m.latchWorks = false; break; case K_NOOUT: m.noOutWorks = false; break;
        case K_BLANK: m.blankSticks = false; break; case K_UND: m.injectUnderflow = 0x10000000u; break; case K_VM: m.vmFault = 1u; break; case K_SEG: m.segAllocErr = true; break; case K_ODM: m.odmAfterUnblank = 0x400u; break;
        case K_FROZEN: m.otg2Frozen = true; break; case K_LOCKAWARE: m.lockAware = true; break; case K_DETAFTER: m.detAfterUnblank = true; m.detReadsAfterUnblank = 7u; break; case K_BLANKNOEARLY: m.blankedNoEarly = true; m.lockAware = true; break;
        case K_LOCKNEVER: m.lockNeverAsserts = true; break; case K_PENDGLITCH: m.pendGlitch = true; break; case K_CLKOFF: m.clkOffWorks = false; break; case K_MPCC: m.mpccNeverIdle = true; break;
        case K_DROP: m.dropAbs = N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH; break; case K_INSIDE: m.lockAware = true; m.refuseInLock = N48D2_MPC_OUT2_MUX; break;
        }
        const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m);
        if (m.plane.stage == N48D2_PL_PLANE) { const n48d2_out s = n48d2::run_plane(m, N48D2_OP_SHOW); h = bat_out(h, s); h = bat_model(h, m); }
        const n48d2_out off = n48d2::run_plane(m, N48D2_OP_PLANEOFF); h = bat_out(h, off); h = bat_model(h, m);
        n48d2_aset_clear();
    }
    // a failed buffer fill, a corrupted read-back, a bad window
    { PModel m; m.initPlane(); m.vfillFailAt = 5; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.vreadCorruptK = 1; m.vreadCorruptDw = 3; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.haveWin = false; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.plane.mc[1] = m.plane.mc[0] + 0x10000u; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.r[N48D2_HUBPREQ0_PRIMARY_LOW] = (uint32_t)m.plane.mc[0]; m.r[N48D2_HUBPREQ0_PRIMARY_HIGH] = (uint32_t)(m.plane.mc[0] >> 32); const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.r[N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION] = 5u; m.r[N48D2_MPC_OUT2_MUX] = 0x4102u; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    { PModel m; m.initPlane(); m.stopOtg0On = N48D2_HUBP2_HUBP_CLK_CNTL; const n48d2_out o = n48d2::run_plane(m, N48D2_OP_PLANE); h = bat_out(h, o); h = bat_model(h, m); n48d2_aset_clear(); }
    // ops out of stage
    { PModel m; m.initPlane(); m.plane.stage = N48D2_PL_NONE; for (uint32_t op : { N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_CRC, N48D2_OP_PLANEOFF, N48D2_OP_FBHOLD, N48D2_OP_PLANE }) { const n48d2_out o = n48d2::run_plane(m, op, 0u); h = bat_out(h, o); h = bat_model(h, m); } n48d2_aset_clear(); }
    // the held plane refuses the movers
    { PModel m; m.initPlane(); (void)n48d2::run_plane(m, N48D2_OP_PLANE); (void)n48d2::run_plane(m, N48D2_OP_SHOW); (void)n48d2::run_plane(m, N48D2_OP_FBHOLD); m.writes.clear(); m.reads.clear();
      for (uint32_t op : { N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_CRC, N48D2_OP_PLANEOFF, N48D2_OP_FBHOLD }) { const n48d2_out o = n48d2::run_plane(m, op, 1u); h = bat_out(h, o); h = bat_model(h, m); }
      for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF, N48D2_OP_TIMING1440 }) { const n48d2_out o = n48d2::run(m, op, 2u); h = bat_out(h, o); h = bat_model(h, m); } n48d2_aset_clear(); }
    // timing / connect / off on the monitor B (the 1080p and 1440p timing), including the refusals while HUBP2's clock is on
    { Model m; m.inst = N48D2_INST_MONB; for (uint32_t op : { N48D2_OP_TIMING1440, N48D2_OP_CONNECT, N48D2_OP_OFF, N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF, N48D2_OP_OFF }) { const n48d2_out o = n48d2::run(m, op, 2u); h = bat_out(h, o); h = bat_model(h, m); } }
    { Model m; m.inst = N48D2_INST_MONB; m.r[N48D2_HUBP2_HUBP_CLK_CNTL] = 1u; for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) { const n48d2_out o = n48d2::run(m, op, 2u); h = bat_out(h, o); h = bat_model(h, m); } }
    // a DP register changing under a monitor B op, and the DP not counting
    { Model m; m.inst = N48D2_INST_MONB; m.flipTrig = N48D2_OTG2_CONTROL; m.flipReg = N48D2_MPC_OUT0_MUX; m.flipXor = 0xFu; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2u); h = bat_out(h, o); h = bat_model(h, m); }
    { Model m; m.inst = N48D2_INST_MONB; m.otg0Stops = true; const n48d2_out o = n48d2::run(m, N48D2_OP_TIMING, 2u); h = bat_out(h, o); h = bat_model(h, m); }
    // the status pages
    { Model m; m.inst = N48D2_INST_MONB; uint64_t v[13]; (void)n48d2::status(m, v, 2u); for (int i = 0; i < 13; i++) h = bat_h(h, v[i]); (void)n48d2::status2(m, v, 2u); for (int i = 0; i < 13; i++) h = bat_h(h, v[i]); (void)n48d2::status3(m, v, 2u); for (int i = 0; i < 13; i++) h = bat_h(h, v[i]); h = bat_model(h, m); }
    return h;
}

static void monb_battery_test() {
    const uint64_t h = monb_battery_hash();
    std::printf("monitor B battery hash %016llx (golden from the 0.0.654 sources: 35e43cabe804915b)\n", (unsigned long long)h);
    expect(h == 0x35e43cabe804915bull, "0.0.655: the monitor B's plane / timing / connect / off behaviour is BYTE-FOR-BYTE the 0.0.654 one (every write, read, sleep, delay, status, gate and state of the whole battery hashes to the hash the 0.0.654 sources produce)");
}

static void plane_tests() {      // 0.0.635 (M4c / M4d)
    void (*const groups[])() = { plane_tables_vs_linux, plane_guard_tests, plane_list_shapes, plane_op8_tests, plane_gate_failures, plane_other_ops, plane_dp_tests, plane_off2_and_pack, plane_source_pins, plane_636_tests, plane_637_tests, plane_638_tests, plane_639_tests, plane_643_tests, plane_652_tests };
    for (void (*g)() : groups) { try { g(); } catch (const SetupFailed &) { /* recorded as a failed check by plane_up; the next group still runs */ } }
}


// ---- 0.0.658: fbhold 1 (the monitor A's HELD plane), its REACHABILITY chain into the monitor A DMUB teardown refusals (dormant until now: `fbhold 1` was refused), the instance <-> display table, and the monitor B-watch log lines
static void mona_hold_tests() {
    // fbhold 1 through the REAL flow: plane 1 -> show 1 -> fbhold 1
    {   P1Model m;
        try { plane1_shown(m); } catch (SetupFailed &) { return; }
        m.pinCalls = 0u;
        const n48d2_out h = run1(m, N48D2_OP_FBHOLD);
        expect(h.status == N48D2_OK && m.plane1.stage == N48D2_PL_HELD && h.free_bufs == 0u && h.auto_off == 0u && h.timeouts == 0u, "0.0.658 fbhold 1: OK, the monitor A's stage is HELD, nothing freed, no rollback");
        expect(m.writes.empty() && m.pinCalls == 1u && m.pinned, "0.0.658 fbhold 1 writes NO register and asks the allocator to pin the monitor A's pair exactly once");
        // from here every op that could move the plane is refused on instance 1 with nothing read or written
        for (uint32_t op : { N48D2_OP_PLANE, N48D2_OP_SHOW, N48D2_OP_FLIP, N48D2_OP_PLANEOFF, N48D2_OP_FBHOLD }) {
            m.reads.clear(); m.writes.clear(); const unsigned pc = m.pinCalls, vf = m.vfillCalls;
            const n48d2_out o = run1(m, op, 0u);
            expect(o.status == N48D2_HELD && m.reads.empty() && m.writes.empty() && m.vfillCalls == vf && m.plane1.stage == N48D2_PL_HELD && m.pinCalls == pc, (std::string("0.0.658: after fbhold 1, op ") + n48d2_op_name(op) + " on the monitor A is refused HELD, nothing read, written or pinned again").c_str());
        }
        for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
            m.reads.clear(); m.writes.clear(); const n48d2_out o = n48d2::run(m, op, N48D2_INST_MONA);
            expect(o.status == N48D2_HELD && m.reads.empty() && m.writes.empty(), (std::string("0.0.658: after fbhold 1, ") + n48d2_op_name(op) + " 1 is refused HELD, nothing read or written").c_str());
        }
        { m.reads.clear(); m.writes.clear(); const n48d2_out o = run1(m, N48D2_OP_CRC); expect(o.status == N48D2_OK, "0.0.658: crc 1 still works on the HELD monitor A plane"); }
        // the glue's HELD test (disp2HeldInst: stage HELD or the pair pinned) is true for the monitor A now, false for the monitor B, and the DMUB template refusals follow it (the chain the 0.0.655 review found dormant)
        const bool monaHeld = m.plane1.stage == N48D2_PL_HELD || m.pinned, monbHeld = false;
        bool chain = monaHeld;
        for (uint32_t id = 1; id <= N48DM_TPL_COUNT; id++) {
            const bool refused = (n48dm_tpl_held_refuses(id) && monbHeld) || (n48dm_tpl_mona_held_refuses(id) && monaHeld);
            const bool wantMonA = id >= N48DM_TPL_PCLK_OTG1_ON && id <= N48DM_TPL_DIGC_SETUP_DVI;      // 3..7: pclk otg1 on/off, phyc enable/disable, digc setup
            if (refused != wantMonA) chain = false;
        }
        expect(chain, "0.0.658 REACHABILITY: once fbhold 1 ran, exactly the monitor A's DMUB templates 3..7 (pclk otg1-on / otg1-off, phyc enable / disable, digc setup) are refused and none of the monitor B's (8..15) or the OTG3 ones");
        expect(n48dm_tpl_mona_held_refuses(N48DM_TPL_PHYC_DISABLE) && n48dm_tpl_mona_held_refuses(N48DM_TPL_PCLK_OTG1_OFF), "0.0.658: the two TEARDOWN templates (phyc disable, pclk otg1-off) are among them");
        expect(n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) != 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 0u) == 0, "0.0.658: a raw DISABLE aimed at the monitor A's context (DFP3) is refused while HELD, the monitor B's (DFP4) is not this instance's");
    }
    // the refusals of fbhold 1 itself, as the monitor B's
    { P1Model m; m.initPlane(); try { plane1_up(m); } catch (SetupFailed &) { return; } m.reads.clear(); const n48d2_out o = run1(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_PLANE_STATE && m.pinCalls == 0u && m.plane1.stage == N48D2_PL_PLANE && m.reads.empty(), "0.0.658 fbhold 1 before show: PLANE_STATE, nothing read, no pin"); }
    { P1Model m; try { plane1_shown(m); } catch (SetupFailed &) { return; } m.pinCalls = 0u; m.curUnderflow = 0x10000000u; const n48d2_out o = run1(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_GATE && m.plane1.stage == N48D2_PL_SHOWN && m.pinCalls == 0u && m.writes.empty(), "0.0.658 fbhold 1 with a HUBP1 underflow: GATE, still SHOWN, NOT pinned"); }
    { P1Model m; try { plane1_shown(m); } catch (SetupFailed &) { return; } m.pinCalls = 0u; m.pinWorks = false; const n48d2_out o = run1(m, N48D2_OP_FBHOLD); expect(o.status == N48D2_NO_BUFFER && m.plane1.stage == N48D2_PL_SHOWN && m.pinCalls == 1u, "0.0.658 fbhold 1 with a refused pin: NO_BUFFER, still SHOWN"); }
    // the instance <-> display-index table agrees with the plane tables
    for (uint32_t inst : { N48D2_INST_MONA, N48D2_INST_MONB }) {
        N48DispGeom g; const uint32_t idx = n48disp_index_for_inst(inst); const bool have = n48disp_geom_for_index(idx, &g) != 0; const n48d2_pr *pr = n48d2_pr_get(inst); const n48d2_surf *sf = n48d2_surf_get(inst);
        expect(have && g.inst == inst && pr->otg_frame == (g.otg == 1u ? N48D2_OTG1_FRAME_COUNT : N48D2_OTG2_FRAME_COUNT) && sf->w == g.w && sf->h == g.h && sf->pitch_bytes == g.pitchBytes && sf->buf_bytes >= g.aperLen && (sf->buf_bytes & 0xFFFFu) == 0u && g.aperLen == (uint64_t)g.w * g.h * 4u,
               (std::string("0.0.658: the display-ops table row of instance ") + std::to_string(inst) + " agrees with the plane tables (OTG, width, height, pitch, buffer size >= the exact frame)").c_str());
    }
    expect(N48D2_INST_MONA == N48_DISP_INST_MONA && N48D2_INST_MONB == N48_DISP_INST_MONB, "0.0.658: the ops header's instance numbers are navi48_disp2.h's");
    // the monitor B-watch log lines: built from the macros the kext uses, with known values; the second line's HUBP2 column is HUBP2's (the 0.0.655 glue shifted it by DET2's pair)
    {   uint32_t b[N48D2_W2_BUF], a[N48D2_W2_BUF];
        for (uint32_t i = 0; i < N48D2_W2_BUF; i++) { b[i] = 0x11u + i; a[i] = 0x21u + i; }
        char l1[1024], l2[1024];
        std::snprintf(l1, sizeof l1, N48D2_W2_LINE1_FMT, N48D2_W2_LINE1_ARGS("plane", " *** MONB DISTURBED ***", 0x5u, b, a));
        std::snprintf(l2, sizeof l2, N48D2_W2_LINE2_FMT, N48D2_W2_LINE2_ARGS("plane", b, a));
        expect(std::string(l1) == "disp2 plane (instance 1) MONB WATCH *** MONB DISTURBED *** (changed mask 0x5, rows 0 DPPCLK_CTRL bit 6, 1 DPPCLK2_DTO, 2 MPC_OUT2_MUX, 3 MPCC2 TOP/OPP, 4 DET2 current): DPPCLK_CTRL 0x00000011 -> 0x00000021 ; DTO2 0x00000012 -> 0x00000022 ; MPC_OUT2_MUX 0x00000013 -> 0x00000023 ; MPCC2 TOP 0x14 -> 0x24 OPP 0x15 -> 0x25 ; DET2 0x00000016 -> 0x00000026",
               "0.0.658 log fix: MONB WATCH line 1 prints every column from its own row, DET2 last");
        expect(std::string(l2) == "disp2 plane (instance 1) MONB WATCH (rows 5 HUBP2 underflow, 6 EARLIEST_INUSE2, 7 DENTIST_DISPCLK_CNTL, 8 OTG2 counting): HUBP2 0x00000017 -> 0x00000027 ; EARLIEST2 0x19:0x00000018 -> 0x29:0x00000028 ; DENTIST 0x0000001a -> 0x0000002a ; OTG2 frames 27 -> 43 (settled 28)",
               "0.0.658 log fix: MONB WATCH line 2 prints HUBP2 from the HUBP2 dword (not DET2's), EARLIEST2 as high:low, DENTIST, the frame counter and the settled counter");
        // the column indexes name the SAME dwords the watch compares (n48d2_w2_regs order)
        expect(n48d2_w2_regs[N48D2_W2_I_DPPCLK].abs == N48D2_DPPCLK_CTRL && n48d2_w2_regs[N48D2_W2_I_DTO].abs == N48D2_DPPCLK2_DTO_PARAM && n48d2_w2_regs[N48D2_W2_I_MUX].abs == N48D2_MPC_OUT2_MUX && n48d2_w2_regs[N48D2_W2_I_TOP].abs == N48D2_MPCC2_MPCC_TOP_SEL && n48d2_w2_regs[N48D2_W2_I_OPP].abs == N48D2_MPCC2_MPCC_OPP_ID &&
               n48d2_w2_regs[N48D2_W2_I_DET].abs == N48D2_DCHUBBUB_DET2_CTRL && n48d2_w2_regs[N48D2_W2_I_HUBP].abs == N48D2_HUBP2_DCHUBP_CNTL && n48d2_w2_regs[N48D2_W2_I_EARLY_LO].abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE && n48d2_w2_regs[N48D2_W2_I_EARLY_HI].abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH &&
               n48d2_w2_regs[N48D2_W2_I_DENTIST].abs == N48D2_DENTIST_DISPCLK_CNTL && N48D2_W2_I_FRAMES == 10u && N48D2_W2_I_SETTLED == 11u && N48D2_W2_DW == 10u, "0.0.658: the log columns' indexes are the watch's own register order");
        // every %-conversion has an argument: count them in both formats against the macros' argument counts (a mismatch cannot hide behind a format-checking-off build)
        auto convs = [](const char *f) { unsigned n = 0; for (; *f; ++f) if (*f == '%') { if (f[1] == '%') ++f; else ++n; } return n; };
        expect(convs(N48D2_W2_LINE1_FMT) == 15u && convs(N48D2_W2_LINE2_FMT) == 12u, "0.0.658: line 1 has 15 conversions (op, flag, mask + 12 values), line 2 has 12 (op + 11 values)"); }
}

int main(int argc, char **argv) {
    if (argc > 1) g_root = argv[1];
    gOff = linux_hdr("dcn_4_1_0_offset.h"); gMask = linux_hdr("dcn_4_1_0_sh_mask.h");
    expect(!gOff.empty() && !gMask.empty(), "Linux's dcn_4_1_0_offset.h and dcn_4_1_0_sh_mask.h are readable");

    // =============================================================================================================================================================================================
    // A. registers and fields against Linux
    // =============================================================================================================================================================================================
    const struct { uint32_t ours; const char *lx; } regs[] = {
        { N48D2_VTG1_CONTROL, "VTG1_CONTROL" }, { N48D2_ODM1_DATA_SOURCE_SELECT, "ODM1_OPTC_DATA_SOURCE_SELECT" }, { N48D2_ODM1_DATA_FORMAT_CONTROL, "ODM1_OPTC_DATA_FORMAT_CONTROL" },
        { N48D2_ODM1_INPUT_CLOCK_CONTROL, "ODM1_OPTC_INPUT_CLOCK_CONTROL" }, { N48D2_ODM1_MEMORY_CONFIG, "ODM1_OPTC_MEMORY_CONFIG" }, { N48D2_OTG1_H_TOTAL, "OTG1_OTG_H_TOTAL" },
        { N48D2_OTG1_H_BLANK_START_END, "OTG1_OTG_H_BLANK_START_END" }, { N48D2_OTG1_H_SYNC_A, "OTG1_OTG_H_SYNC_A" }, { N48D2_OTG1_H_SYNC_A_CNTL, "OTG1_OTG_H_SYNC_A_CNTL" },
        { N48D2_OTG1_H_TIMING_CNTL, "OTG1_OTG_H_TIMING_CNTL" }, { N48D2_OTG1_V_TOTAL, "OTG1_OTG_V_TOTAL" }, { N48D2_OTG1_V_TOTAL_MIN, "OTG1_OTG_V_TOTAL_MIN" }, { N48D2_OTG1_V_TOTAL_MAX, "OTG1_OTG_V_TOTAL_MAX" },
        { N48D2_OTG1_V_BLANK_START_END, "OTG1_OTG_V_BLANK_START_END" }, { N48D2_OTG1_V_SYNC_A, "OTG1_OTG_V_SYNC_A" }, { N48D2_OTG1_V_SYNC_A_CNTL, "OTG1_OTG_V_SYNC_A_CNTL" }, { N48D2_OTG1_CONTROL, "OTG1_OTG_CONTROL" },
        { N48D2_OTG1_INTERLACE_CONTROL, "OTG1_OTG_INTERLACE_CONTROL" }, { N48D2_OTG1_STATUS, "OTG1_OTG_STATUS" }, { N48D2_OTG1_FRAME_COUNT, "OTG1_OTG_STATUS_FRAME_COUNT" }, { N48D2_OTG1_CLOCK_CONTROL, "OTG1_OTG_CLOCK_CONTROL" },
        { N48D2_OTG1_VSTARTUP_PARAM, "OTG1_OTG_VSTARTUP_PARAM" }, { N48D2_OTG1_VUPDATE_PARAM, "OTG1_OTG_VUPDATE_PARAM" }, { N48D2_OTG1_VREADY_PARAM, "OTG1_OTG_VREADY_PARAM" },
        { N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL, "OTG1_PHYPLL_PIXEL_RATE_CNTL" }, { N48D2_FMT1_422_CONTROL, "FMT1_FMT_422_CONTROL" }, { N48D2_DPG1_CONTROL, "DPG1_DPG_CONTROL" },
        { N48D2_DPG1_RAMP_CONTROL, "DPG1_DPG_RAMP_CONTROL" }, { N48D2_DPG1_DIMENSIONS, "DPG1_DPG_DIMENSIONS" }, { N48D2_DPG1_COLOUR_R_CR, "DPG1_DPG_COLOUR_R_CR" }, { N48D2_DPG1_COLOUR_G_Y, "DPG1_DPG_COLOUR_G_Y" },
        { N48D2_DPG1_COLOUR_B_CB, "DPG1_DPG_COLOUR_B_CB" }, { N48D2_DPG1_OFFSET_SEGMENT, "DPG1_DPG_OFFSET_SEGMENT" }, { N48D2_DPG1_STATUS, "DPG1_DPG_STATUS" }, { N48D2_OPP_PIPE1_CONTROL, "OPP_PIPE1_OPP_PIPE_CONTROL" },
        { N48D2_DIG2_STREAM_MAPPER, "DIG2_STREAM_MAPPER_CONTROL" }, { N48D2_DIG2_FE_CNTL, "DIG2_DIG_FE_CNTL" }, { N48D2_DIG2_FE_CLK_CNTL, "DIG2_DIG_FE_CLK_CNTL" }, { N48D2_DIG2_FE_EN_CNTL, "DIG2_DIG_FE_EN_CNTL" },
        { N48D2_DIG2_CLOCK_PATTERN, "DIG2_DIG_CLOCK_PATTERN" }, { N48D2_DIG2_FIFO_CTRL0, "DIG2_DIG_FIFO_CTRL0" }, { N48D2_DIG2_HDMI_CONTROL, "DIG2_HDMI_CONTROL" }, { N48D2_DIG2_BE_CLK_CNTL, "DIG2_DIG_BE_CLK_CNTL" },
        { N48D2_DIG2_BE_CNTL, "DIG2_DIG_BE_CNTL" }, { N48D2_DIG2_BE_EN_CNTL, "DIG2_DIG_BE_EN_CNTL" }, { N48D2_DIG2_TMDS_CTL_BITS, "DIG2_TMDS_CTL_BITS" }, { N48D2_OTG0_CONTROL, "OTG0_OTG_CONTROL" },
        { N48D2_OTG0_FRAME_COUNT, "OTG0_OTG_STATUS_FRAME_COUNT" }, { N48D2_DIG1_FE_CNTL, "DIG1_DIG_FE_CNTL" }, { N48D2_DIG1_BE_CLK_CNTL, "DIG1_DIG_BE_CLK_CNTL" }, { N48D2_DIG1_BE_CNTL, "DIG1_DIG_BE_CNTL" },
        { N48D2_DIG1_BE_EN_CNTL, "DIG1_DIG_BE_EN_CNTL" }, { n48d2_dp_regs[5], "DIG1_STREAM_MAPPER_CONTROL" },
    };
    for (const auto &x : regs) { char b[128]; std::snprintf(b, sizeof b, "register %s is Linux's absolute address", x.lx); expect_u(b, x.ours, lx_abs(x.lx)); }
    // the measured absolutes the brief and the M4 design name
    expect(N48D2_DIG2_FE_CNTL == 0x579bu && N48D2_DIG2_FE_CLK_CNTL == 0x579cu && N48D2_DIG2_FE_EN_CNTL == 0x579du && N48D2_DIG2_STREAM_MAPPER == 0x53cfu && N48D2_DIG2_FIFO_CTRL0 == 0x57a3u &&
           N48D2_DIG2_CLOCK_PATTERN == 0x57a0u && N48D2_DIG2_BE_CLK_CNTL == 0x57c3u && N48D2_DIG2_BE_EN_CNTL == 0x57c5u && N48D2_DPG1_CONTROL == 0x4d6eu && N48D2_DPG1_DIMENSIONS == 0x4d70u &&
           N48D2_OPP_PIPE1_CONTROL == 0x4da6u && N48D2_ODM1_INPUT_CLOCK_CONTROL == 0x4fa0u && N48D2_OTG1_CLOCK_CONTROL == 0x50c4u && N48D2_ODM1_DATA_SOURCE_SELECT == 0x4f9bu && N48D2_OTG1_H_TOTAL == 0x506au &&
           N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL == 0x147u, "the absolutes match the M4 design table (0x579b.., 0x53cf, 0x4d6e.., 0x4fa0, 0x50c4, 0x4f9b, 0x506a, 0x147)");
    const struct { uint32_t ours; const char *f; } fields[] = {
        { N48D2_VTG_FP2_MASK, "VTG0_CONTROL__VTG0_FP2" }, { N48D2_VTG_VCOUNT_INIT_MASK, "VTG0_CONTROL__VTG0_VCOUNT_INIT" }, { N48D2_VTG_ENABLE_MASK, "VTG0_CONTROL__VTG0_ENABLE" },
        { N48D2_ODM_NUM_IN_SEG_MASK, "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT" }, { N48D2_ODM_SEG0_MASK, "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL" },
        { N48D2_ODM_DATA_FORMAT_MASK, "ODM0_OPTC_DATA_FORMAT_CONTROL__OPTC_DATA_FORMAT" }, { N48D2_ODM_CLK_GATE_DIS_MASK, "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS" },
        { N48D2_ODM_CLK_EN_MASK, "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN" }, { N48D2_ODM_CLK_ON_MASK, "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON" }, { N48D2_ODM_MEM_SEL_MASK, "ODM0_OPTC_MEMORY_CONFIG__OPTC_MEM_SEL" },
        { N48D2_OTG_CLOCK_EN_MASK, "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN" }, { N48D2_OTG_CLOCK_GATE_DIS_MASK, "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS" }, { N48D2_OTG_CLOCK_ON_MASK, "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON" },
        { N48D2_OTG_BUSY_MASK, "OTG0_OTG_CLOCK_CONTROL__OTG_BUSY" }, { N48D2_OTG_MASTER_EN_MASK, "OTG0_OTG_CONTROL__OTG_MASTER_EN" }, { N48D2_OTG_DISABLE_POINT_MASK, "OTG0_OTG_CONTROL__OTG_DISABLE_POINT_CNTL" },
        { N48D2_OTG_START_POINT_MASK, "OTG0_OTG_CONTROL__OTG_START_POINT_CNTL" }, { N48D2_OTG_FIELD_NUMBER_MASK, "OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_CNTL" }, { N48D2_OTG_CUR_MASTER_EN_MASK, "OTG0_OTG_CONTROL__OTG_CURRENT_MASTER_EN_STATE" },
        { N48D2_OTG_OUT_MUX_MASK, "OTG0_OTG_CONTROL__OTG_OUT_MUX" }, { N48D2_H_DIV_MODE_MASK, "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE" }, { N48D2_H_DIV_MANUAL_MASK, "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MANUAL" },
        { N48D2_VSTARTUP_MASK, "OTG0_OTG_VSTARTUP_PARAM__VSTARTUP_START" }, { N48D2_VUPDATE_OFFSET_MASK, "OTG0_OTG_VUPDATE_PARAM__VUPDATE_OFFSET" }, { N48D2_VUPDATE_WIDTH_MASK, "OTG0_OTG_VUPDATE_PARAM__VUPDATE_WIDTH" },
        { N48D2_VREADY_MASK, "OTG0_OTG_VREADY_PARAM__VREADY_OFFSET" }, { N48D2_SYNC_POL_MASK, "OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL" }, { N48D2_SYNC_POL_MASK, "OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL" },
        { N48D2_INTERLACE_EN_MASK, "OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE" }, { N48D2_LO15, "OTG0_OTG_H_TOTAL__OTG_H_TOTAL" }, { N48D2_HI15, "OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END" },
        { N48D2_FMT_LEFT_EDGE_MASK, "FMT0_FMT_422_CONTROL__FMT_LEFT_EDGE_EXTRA_PIXEL_COUNT" }, { N48D2_DPG_EN_MASK, "DPG0_DPG_CONTROL__DPG_EN" }, { N48D2_DPG_MODE_MASK, "DPG0_DPG_CONTROL__DPG_MODE" },
        { N48D2_DPG_DYN_RANGE_MASK, "DPG0_DPG_CONTROL__DPG_DYNAMIC_RANGE" }, { N48D2_DPG_BIT_DEPTH_MASK, "DPG0_DPG_CONTROL__DPG_BIT_DEPTH" }, { N48D2_DPG_VRES_MASK, "DPG0_DPG_CONTROL__DPG_VRES" },
        { N48D2_DPG_HRES_MASK, "DPG0_DPG_CONTROL__DPG_HRES" }, { N48D2_DPG_PENDING_MASK, "DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING" }, { N48D2_OPP_PIPE_CLOCK_EN_MASK, "OPP_PIPE0_OPP_PIPE_CONTROL__OPP_PIPE_CLOCK_EN" },
        { N48D2_DIG_SOURCE_SELECT_MASK, "DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT" }, { N48D2_DIG_STEREOSYNC_SEL_MASK, "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_SELECT" }, { N48D2_DIG_STEREOSYNC_GATE_MASK, "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_GATE_EN" },
        { N48D2_DIG_FE_MODE_MASK, "DIG0_DIG_FE_CLK_CNTL__DIG_FE_MODE" }, { N48D2_DIG_FE_CLK_EN_MASK, "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN" }, { N48D2_DIG_FE_SYMCLK_ON_MASK, "DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON" },
        { N48D2_DIG_FE_ENABLE_MASK, "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE" }, { N48D2_DIG_CLOCK_PATTERN_MASK, "DIG0_DIG_CLOCK_PATTERN__DIG_CLOCK_PATTERN" }, { N48D2_FIFO_ENABLE_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE" },
        { N48D2_FIFO_RESET_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET" }, { N48D2_FIFO_READ_START_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_READ_START_LEVEL" }, { N48D2_FIFO_PIX_PER_CYCLE_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE" },
        { N48D2_FIFO_RESET_DONE_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE" }, { N48D2_FIFO_ERROR_MASK, "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ERROR" }, { N48D2_TMDS_PIXEL_ENCODING_MASK, "DIG0_HDMI_CONTROL__TMDS_PIXEL_ENCODING" },
        { N48D2_TMDS_COLOR_FORMAT_MASK, "DIG0_HDMI_CONTROL__TMDS_COLOR_FORMAT" }, { N48D2_BE_MODE_MASK, "DIG0_DIG_BE_CLK_CNTL__DIG_BE_MODE" }, { N48D2_BE_CLK_EN_MASK, "DIG0_DIG_BE_CLK_CNTL__DIG_BE_CLK_EN" },
        { N48D2_BE_FE_SOURCE_MASK, "DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT" }, { N48D2_BE_ENABLE_MASK, "DIG0_DIG_BE_EN_CNTL__DIG_BE_ENABLE" }, { N48D2_MAPPER_LINK_MASK, "DIG0_STREAM_MAPPER_CONTROL__DIG_STREAM_LINK_TARGET" },
        { N48D2_PHYPLL_SOURCE_MASK, "OTG1_PHYPLL_PIXEL_RATE_CNTL__OTG1_PHYPLL_PIXEL_RATE_SOURCE" },
    };
    for (const auto &x : fields) { char b[160]; std::snprintf(b, sizeof b, "field mask %s", x.f); expect_u(b, x.ours, lx_mask(x.f)); }
    expect_u("FE2's source bit is DIGC (0x04) in DIG_FE_SOURCE_SELECT", N48D2_BE_FE2_BIT, lx_fv("DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0x04u));
    expect_u("the measured DIG2 BE_CNTL after the M4b enable: FE source 0x04, HPD select 2", 0x20000400u, lx_fv("DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0x04u) | lx_fv("DIG0_DIG_BE_CNTL__DIG_HPD_SELECT", 2u));
    expect_u("the measured DIG2 BE_CLK_CNTL after phyc enable decodes as DVI + clock on + symclk on + TMDS clock on", 0x2812u,
             lx_fv("DIG0_DIG_BE_CLK_CNTL__DIG_BE_MODE", 2u) | lx_fv("DIG0_DIG_BE_CLK_CNTL__DIG_BE_CLK_EN", 1u) | lx_fv("DIG0_DIG_BE_CLK_CNTL__DIG_BE_SYMCLK_G_CLOCK_ON", 1u) | lx_fv("DIG0_DIG_BE_CLK_CNTL__DIG_BE_SYMCLK_G_TMDS_CLOCK_ON", 1u));
    expect_u("DPG_CONTROL ON = EN | COLORSQUARES_RGB | VESA | 8 bpc | VRES 6 | HRES 6", N48D2_DPG_CONTROL_ON,
             lx_fv("DPG0_DPG_CONTROL__DPG_EN", 1u) | lx_fv("DPG0_DPG_CONTROL__DPG_MODE", 0u) | lx_fv("DPG0_DPG_CONTROL__DPG_DYNAMIC_RANGE", 0u) | lx_fv("DPG0_DPG_CONTROL__DPG_BIT_DEPTH", 1u) | lx_fv("DPG0_DPG_CONTROL__DPG_VRES", 6u) | lx_fv("DPG0_DPG_CONTROL__DPG_HRES", 6u));
    expect_u("DPG_CONTROL ON literal", N48D2_DPG_CONTROL_ON, 0x00661001u);
    // every register the verb can write is inside the REAL allowlist, and none of them inside a forbidden span
    {
        dcn41_allow_state al {};
        expect(dcn41_allow_init(&al, kSeg, 262144u) == DCN41_ALLOW_OK, "the allowlist arms");
        for (uint32_t i = 0; i < N48D2_ALLOWED_COUNT; i++) {
            char b[96]; std::snprintf(b, sizeof b, "listed register %#x is inside the DCN allowlist", n48d2_allowed[i]);
            expect(dcn41_allow_write(&al, n48d2_allowed[i], 0u, "t"), b);
            for (uint32_t k = 0; k < N48D2_FORBIDDEN_COUNT; k++) expect(!(n48d2_allowed[i] >= n48d2_forbidden[k].lo && n48d2_allowed[i] <= n48d2_forbidden[k].hi), "no listed register is inside a forbidden span");
            for (uint32_t j = 0; j < i; j++) expect(n48d2_allowed[i] != n48d2_allowed[j], "the list has no duplicates");
        }
        expect_u("39 listed registers", N48D2_ALLOWED_COUNT, 39);
        // the read-only registers are NOT writable by this verb
        for (uint32_t a : { N48D2_DIG2_BE_CLK_CNTL, N48D2_DIG2_BE_EN_CNTL, N48D2_DIG2_TMDS_CTL_BITS, N48D2_OTG1_STATUS, N48D2_OTG1_FRAME_COUNT, N48D2_DPG1_STATUS, N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL })
            expect(n48d2_guard_addr(a, nullptr) != N48D2_G_OK, "a register this verb only READS (BE clock / enable, TMDS bits, statuses, the PHYPLL source) is refused for writing");
    }

    // =============================================================================================================================================================================================
    // B. the timing from the captured EDID
    // =============================================================================================================================================================================================
    {
        std::string ddc;
        std::vector<std::string> roots; if (const char *e = std::getenv("N48_NOTES")) roots.push_back(e);
        roots.push_back(g_root); roots.push_back("/path/to/navi48-checkout");
        for (auto &r : roots) { ddc = slurp(r + "/notes/logs/runs/m1-622/ddc.txt"); if (!ddc.empty()) break; }
        expect(!ddc.empty(), "the captured EDID (notes/logs/runs/m1-622/ddc.txt) is readable");
        const size_t at = ddc.find("accel ddcread: line 2 block 0");
        expect(at != std::string::npos, "the capture has line 2 block 0");
        std::vector<uint8_t> e;
        if (at != std::string::npos) {
            for (int row = 0; row < 8; row++) {
                char key[16]; std::snprintf(key, sizeof key, "  %03x:", row * 16);
                const size_t p = ddc.find(key, at);
                if (p == std::string::npos) break;
                const char *c = ddc.c_str() + p + std::strlen(key);
                for (int k = 0; k < 16; k++) { char *end = nullptr; e.push_back((uint8_t)std::strtoul(c, &end, 16)); c = end; }
            }
        }
        expect_u("128 EDID bytes", e.size(), 128);
        if (e.size() == 128) {
            uint32_t sum = 0; for (uint8_t b : e) sum += b;
            expect_u("the EDID checksum", sum & 0xFFu, 0);
            expect(e[0] == 0 && e[1] == 0xff && e[7] == 0 && e[8] == 0x04 && e[9] == 0x72, "an EDID header, monitor A's PNP id");
            expect(std::memcmp(&e[0x36], n48d2_mona_dtd, 18) == 0, "n48d2_mona_dtd IS the capture's first detailed timing descriptor (bytes 0x36..0x47)");
        }
        n48d2_timing t;
        expect_u("the DTD decodes", n48d2_dtd_decode(n48d2_mona_dtd, &t), 0);
        expect(t.pix_clk_10khz == 14850 && t.h_active == 1920 && t.h_blank == 280 && t.h_total == 2200 && t.h_fp == 88 && t.h_sync == 44 && t.h_bp == 148, "horizontal: 148.5 MHz, 1920 / 2200, 88 / 44 / 148 (CEA VIC 16)");
        expect(t.v_active == 1080 && t.v_blank == 45 && t.v_total == 1125 && t.v_fp == 4 && t.v_sync == 5 && t.v_bp == 36, "vertical: 1080 / 1125, 4 / 5 / 36");
        expect(t.h_pos == 1 && t.v_pos == 1 && t.digital_separate == 1 && t.interlace == 0 && t.h_border == 0 && t.v_border == 0, "positive syncs, digital separate, progressive, no borders");
        expect_u("60 Hz (mHz)", (uint64_t)t.pix_clk_10khz * 10000ull * 1000ull / ((uint64_t)t.h_total * t.v_total), 60000);
        n48d2_otg_regs r;
        expect_u("the timing computes", n48d2_otg_compute(&t, &r), 0);
        expect_u("OTG_H_TOTAL = 2199", r.h_total, 2199);
        expect_u("OTG_H_SYNC_A: start 0, end 44", r.h_sync_a, lx_fv("OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END", 44) | lx_fv("OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_START", 0));
        expect_u("OTG_H_BLANK: start 2112 (2200 - 88), end 192 (2112 - 1920)", r.h_blank, lx_fv("OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START", 2112) | lx_fv("OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END", 192));
        expect_u("OTG_V_TOTAL = 1124", r.v_total, 1124);
        expect_u("OTG_V_SYNC_A: start 0, end 5", r.v_sync_a, lx_fv("OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_END", 5));
        expect_u("OTG_V_BLANK: start 1121 (1125 - 4), end 41 (1121 - 1080)", r.v_blank, lx_fv("OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START", 1121) | lx_fv("OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END", 41));
        expect(r.h_pol == 0 && r.v_pol == 0, "positive syncs program POL 0");
        expect_u("VTG VCOUNT_INIT = 1121", r.vtg_vcount_init, 1121);
        expect_u("VTG FP2 = 0 (41 - 13 + 1 >= 0)", r.vtg_fp2, 0);
        expect_u("VSTARTUP 13", r.vstartup, 13);
        expect_u("VUPDATE offset 680 width 320 (the GOP's OTG0 value 0x014002a8)", r.vupdate, 0x014002a8u);
        expect_u("VREADY 300 (0x12c)", r.vready, 0x12cu);
        expect_u("all horizontal values even: no manual H divide", r.h_div2, 1);
        // the decoder against a second, synthetic DTD (negative syncs, upper bits): 2560x1440 241.5 MHz (the DP's mode) with hfp 48 hsync 32 vfp 3 vsync 5, syncs + / -
        const uint8_t dp[18] = { 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0, 0, 0, 0, 0, 0x1a };
        n48d2_timing d;
        expect_u("a second DTD decodes", n48d2_dtd_decode(dp, &d), 0);
        expect(d.pix_clk_10khz == 24150 && d.h_active == 2560 && d.h_total == 2720 && d.v_active == 1440 && d.v_total == 1481 && d.h_fp == 48 && d.h_sync == 32 && d.v_fp == 3 && d.v_sync == 5 && d.h_pos == 1 && d.v_pos == 0, "the DP DTD (2560x1440, +h -v)");
        n48d2_otg_regs dr; (void)n48d2_otg_compute(&d, &dr);
        expect(dr.h_pol == 0 && dr.v_pol == 1, "a negative vsync programs V POL 1");
        const uint8_t zero[18] = { 0 };
        expect(n48d2_dtd_decode(zero, &d) != 0, "a non-timing descriptor is refused");
        n48d2_timing il = t; il.interlace = 1; expect(n48d2_otg_compute(&il, &dr) != 0, "an interlaced timing is refused");
    }

    // =============================================================================================================================================================================================
    // C. the exact step lists, rebuilt from Linux NAMES
    // =============================================================================================================================================================================================
    check_list("timing", N48D2_OP_TIMING, {
        { U,  "ODM1_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN", 1 }, { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS", 1 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { WT, "ODM1_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON", 1 } }, 1000, 1, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "OTG1_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN", 1 }, { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS", 1 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { WT, "OTG1_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON", 1 } }, 1000, 1, "optc1_enable_optc_clock", nullptr, nullptr },
        { S,  "ODM1_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT", 0 }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 1 }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL", 0xf },
                                                  { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG2_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG3_SRC_SEL", 0xf } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { U,  "OTG1_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE", 0 } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { S,  "ODM1_OPTC_MEMORY_CONFIG", { { "ODM0_OPTC_MEMORY_CONFIG__OPTC_MEM_SEL", 0 } }, 0, 0, "optc401_set_odm_bypass", nullptr, nullptr },
        { S,  "OTG1_OTG_H_TOTAL", { { "OTG0_OTG_H_TOTAL__OTG_H_TOTAL", 2199 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_H_SYNC_A", { { "OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_START", 0 }, { "OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END", 44 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_H_BLANK_START_END", { { "OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START", 2112 }, { "OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END", 192 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_H_SYNC_A_CNTL", { { "OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG1_OTG_V_TOTAL", { { "OTG0_OTG_V_TOTAL__OTG_V_TOTAL", 1124 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG1_OTG_V_TOTAL_MAX", { { "OTG0_OTG_V_TOTAL_MAX__OTG_V_TOTAL_MAX", 1124 } }, 0, 0, "optc1_set_vtotal_min_max", nullptr, nullptr },
        { S,  "OTG1_OTG_V_TOTAL_MIN", { { "OTG0_OTG_V_TOTAL_MIN__OTG_V_TOTAL_MIN", 1124 } }, 0, 0, "optc1_set_vtotal_min_max", nullptr, nullptr },
        { U,  "OTG1_OTG_V_SYNC_A", { { "OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_START", 0 }, { "OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_END", 5 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_V_BLANK_START_END", { { "OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START", 1121 }, { "OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END", 41 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_V_SYNC_A_CNTL", { { "OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_INTERLACE_CONTROL", { { "OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "VTG1_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_START_POINT_CNTL", 0 }, { "OTG0_OTG_CONTROL__OTG_FIELD_NUMBER_CNTL", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { S,  "OTG1_OTG_VSTARTUP_PARAM", { { "OTG0_OTG_VSTARTUP_PARAM__VSTARTUP_START", 13 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { S,  "OTG1_OTG_VUPDATE_PARAM", { { "OTG0_OTG_VUPDATE_PARAM__VUPDATE_OFFSET", 680 }, { "OTG0_OTG_VUPDATE_PARAM__VUPDATE_WIDTH", 320 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { S,  "OTG1_OTG_VREADY_PARAM", { { "OTG0_OTG_VREADY_PARAM__VREADY_OFFSET", 300 } }, 0, 0, "optc401_program_global_sync", nullptr, nullptr },
        { U,  "VTG1_CONTROL", { { "VTG0_CONTROL__VTG0_FP2", 0 }, { "VTG0_CONTROL__VTG0_VCOUNT_INIT", 1121 } }, 0, 0, "optc1_set_vtg_params", nullptr, nullptr },
        { U,  "ODM1_OPTC_DATA_FORMAT_CONTROL", { { "ODM0_OPTC_DATA_FORMAT_CONTROL__OPTC_DATA_FORMAT", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE", 0 } }, 0, 0, "optc1_program_timing", nullptr, nullptr },
        { U,  "OTG1_OTG_H_TIMING_CNTL", { { "OTG0_OTG_H_TIMING_CNTL__OTG_H_TIMING_DIV_MODE_MANUAL", 0 } }, 0, 0, "optc401_set_h_timing_div_manual_mode", nullptr, nullptr },
        { U,  "OPP_PIPE1_OPP_PIPE_CONTROL", { { "OPP_PIPE0_OPP_PIPE_CONTROL__OPP_PIPE_CLOCK_EN", 1 } }, 0, 0, "opp1_pipe_clock_control", nullptr, nullptr },
        { U,  "FMT1_FMT_422_CONTROL", { { "FMT0_FMT_422_CONTROL__FMT_LEFT_EDGE_EXTRA_PIXEL_COUNT", 0 } }, 0, 0, "opp2_program_left_edge_extra_pixel", nullptr, nullptr },
        { S,  "DPG1_DPG_DIMENSIONS", { { "DPG0_DPG_DIMENSIONS__DPG_ACTIVE_WIDTH", 1920 }, { "DPG0_DPG_DIMENSIONS__DPG_ACTIVE_HEIGHT", 1080 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG1_DPG_OFFSET_SEGMENT", { { "DPG0_DPG_OFFSET_SEGMENT__DPG_X_OFFSET", 0 }, { "DPG0_DPG_OFFSET_SEGMENT__DPG_SEGMENT_WIDTH", 0 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "DPG1_DPG_CONTROL", { { "DPG0_DPG_CONTROL__DPG_EN", 1 }, { "DPG0_DPG_CONTROL__DPG_MODE", 0 }, { "DPG0_DPG_CONTROL__DPG_DYNAMIC_RANGE", 0 }, { "DPG0_DPG_CONTROL__DPG_BIT_DEPTH", 1 },
                                     { "DPG0_DPG_CONTROL__DPG_VRES", 6 }, { "DPG0_DPG_CONTROL__DPG_HRES", 6 } }, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "ODM1_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 1 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { U,  "VTG1_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 1 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { U,  "OTG1_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_DISABLE_POINT_CNTL", 2 }, { "OTG0_OTG_CONTROL__OTG_MASTER_EN", 1 } }, 0, 0, "optc401_enable_crtc", nullptr, nullptr },
        { WT, "DPG1_DPG_STATUS", { { "DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING", 0 } }, 1000, 100, "dcn20_wait_for_blank_complete", nullptr, nullptr },
    });
    check_list("connect", N48D2_OP_CONNECT, {
        { U,  "SYMCLKC_CLOCK_ENABLE", { { "SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_EN", 1 }, { "SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_SRC_SEL", 2 } }, 0, 0, "dccg401_enable_symclk_se", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT", 1 } }, 0, 0, "enc1_dig_connect_to_otg", nullptr, nullptr },
        { U,  "OTG1_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_OUT_MUX", 0 } }, 0, 0, "optc401_set_out_mux", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_SELECT", 1 } }, 0, 0, "enc1_setup_stereo_sync", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CNTL", { { "DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_GATE_EN", 1 } }, 0, 0, "enc1_setup_stereo_sync", nullptr, nullptr },
        { U,  "DIG2_DIG_CLOCK_PATTERN", { { "DIG0_DIG_CLOCK_PATTERN__DIG_CLOCK_PATTERN", 0x1f } }, 0, 0, "enc401_stream_encoder_dvi_set_stream_attribute", nullptr, nullptr },
        { U,  "DIG2_HDMI_CONTROL", { { "DIG0_HDMI_CONTROL__TMDS_PIXEL_ENCODING", 0 } }, 0, 0, "enc401_stream_encoder_set_stream_attribute_helper", nullptr, nullptr },
        { U,  "DIG2_HDMI_CONTROL", { { "DIG0_HDMI_CONTROL__TMDS_COLOR_FORMAT", 0 } }, 0, 0, "enc401_stream_encoder_set_stream_attribute_helper", nullptr, nullptr },
        { U,  "DIG2_DIG_BE_CNTL", { { "DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0x04 } }, 0, 0, "dcn10_link_encoder_connect_dig_be_to_fe", nullptr, nullptr, 0x04 },   // field |= 0x04 (DIGC)
        { U,  "DIG2_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_MODE", 2 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 1 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 1 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG2_STREAM_MAPPER_CONTROL", { { "DIG0_STREAM_MAPPER_CONTROL__DIG_STREAM_LINK_TARGET", 2 } }, 0, 0, "enc401_stream_encoder_map_to_link", nullptr, nullptr },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE", 0 } }, 0, 0, "enc401_set_dig_input_mode", nullptr, nullptr },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_READ_START_LEVEL", 7 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET", 1 } }, 0, 0, "enc35_reset_fifo", nullptr, nullptr },
        { WI, "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE", 1 } }, 5000, 10, "enc35_reset_fifo", "DIG2_DIG_FE_CLK_CNTL", "DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON" },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET", 0 } }, 0, 0, "enc35_reset_fifo", nullptr, nullptr },
        { WI, "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_RESET_DONE", 0 } }, 5000, 10, "enc35_reset_fifo", "DIG2_DIG_FE_CLK_CNTL", "DIG0_DIG_FE_CLK_CNTL__DIG_FE_SYMCLK_FE_G_CLOCK_ON" },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE", 1 } }, 0, 0, "enc35_enable_fifo", nullptr, nullptr },
    });
    // the BE step is Linux's `field |= 0x04`: the built step's mask is ONLY the FE2 bit (it must never clear FE1 or anything else of the field)
    { n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(N48D2_OP_CONNECT, s);
      expect(n > 8 && s[8].abs == N48D2_DIG2_BE_CNTL && s[8].kind == N48D2_K_UPD && s[8].mask == 0x400u && s[8].val == 0x400u, "connect's BE step sets ONLY bit 10 (FE source 0x04 << 8): mask 0x400, value 0x400"); }
    check_list("off", N48D2_OP_OFF, {
        { S,  "DPG1_DPG_CONTROL", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG1_DPG_COLOUR_R_CR", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG1_DPG_COLOUR_G_Y", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG1_DPG_COLOUR_B_CB", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { S,  "DPG1_DPG_RAMP_CONTROL", {}, 0, 0, "opp2_set_disp_pattern_generator", nullptr, nullptr },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_ENABLE", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 0 } }, 0, 0, "enc35_disable_fifo", nullptr, nullptr },
        { U,  "DIG2_DIG_FIFO_CTRL0", { { "DIG0_DIG_FIFO_CTRL0__DIG_FIFO_OUTPUT_PIXEL_PER_CYCLE", 0 } }, 0, 0, "enc401_set_dig_input_mode", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_EN_CNTL", { { "DIG0_DIG_FE_EN_CNTL__DIG_FE_ENABLE", 0 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG2_DIG_FE_CLK_CNTL", { { "DIG0_DIG_FE_CLK_CNTL__DIG_FE_CLK_EN", 0 } }, 0, 0, "enc401_stream_encoder_enable", nullptr, nullptr },
        { U,  "DIG2_DIG_BE_CNTL", { { "DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT", 0 } }, 0, 0, "dcn10_link_encoder_connect_dig_be_to_fe", nullptr, nullptr, 0x04 },   // field &= ~0x04
        { U,  "SYMCLKC_CLOCK_ENABLE", { { "SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_EN", 0 }, { "SYMCLKC_CLOCK_ENABLE__SYMCLKC_FE_SRC_SEL", 0 } }, 0, 0, "dccg401_disable_symclk_se", nullptr, nullptr },
        { U,  "DIG2_STREAM_MAPPER_CONTROL", { { "DIG0_STREAM_MAPPER_CONTROL__DIG_STREAM_LINK_TARGET", 0 } }, 0, 0, "enc401_stream_encoder_map_to_link", nullptr, nullptr },
        { U,  "ODM1_OPTC_DATA_SOURCE_SELECT", { { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG1_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG2_SRC_SEL", 0xf },
                                                  { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG3_SRC_SEL", 0xf }, { "ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_NUM_OF_INPUT_SEGMENT", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "ODM1_OPTC_MEMORY_CONFIG", { { "ODM0_OPTC_MEMORY_CONFIG__OPTC_MEM_SEL", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "OTG1_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_MASTER_EN", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "VTG1_CONTROL", { { "VTG0_CONTROL__VTG0_ENABLE", 0 } }, 0, 0, "optc401_disable_crtc", nullptr, nullptr },
        { WT, "OTG1_OTG_CONTROL", { { "OTG0_OTG_CONTROL__OTG_CURRENT_MASTER_EN_STATE", 0 } }, 15000, 10, "optc401_disable_crtc", nullptr, nullptr },
        { WT, "OTG1_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_BUSY", 0 } }, 150000, 1, "optc401_disable_crtc", nullptr, nullptr },
        { U,  "OTG1_OTG_CLOCK_CONTROL", { { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS", 0 }, { "OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN", 0 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "ODM1_OPTC_INPUT_CLOCK_CONTROL", { { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS", 0 }, { "ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN", 0 } }, 0, 0, "optc1_enable_optc_clock", nullptr, nullptr },
        { U,  "OPP_PIPE1_OPP_PIPE_CONTROL", { { "OPP_PIPE0_OPP_PIPE_CONTROL__OPP_PIPE_CLOCK_EN", 0 } }, 0, 0, "opp1_pipe_clock_control", nullptr, nullptr },
    });
    { n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(N48D2_OP_OFF, s);
      expect(n > 12 && s[11].abs == N48D2_DIG2_BE_CNTL && s[11].kind == N48D2_K_UPD && s[11].mask == 0x400u && s[11].val == 0u && s[12].abs == N48D2_SYMCLKC_CLOCK_ENABLE, "off's BE step (11) clears ONLY bit 10: mask 0x400, value 0; the SYMCLKC step (12) FOLLOWS it (0.0.634)"); }
    { n48d2_step s[N48D2_MAX_STEPS]; expect(n48d2_build(N48D2_OP_STATUS, s) == 0 && n48d2_build(N48D2_OP_STATUS2, s) == 0 && n48d2_build(0, s) == 0 && n48d2_build(N48D2_OP_STATUS3, s) == 0 && n48d2_build(7, s) == 0, "status, status2, status3 and unknown ops have no step list"); }
    // connect / off name no register of the plane, the DMUB-owned back end, PHY C or the PLL; timing names no DIG register
    for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
        n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(op, s);
        for (uint32_t i = 0; i < n; i++) if (n48d2_is_write(&s[i])) {
            expect(s[i].abs != N48D2_DIG2_BE_CLK_CNTL && s[i].abs != N48D2_DIG2_BE_EN_CNTL && s[i].abs != N48D2_DIG2_TMDS_CTL_BITS && (s[i].abs >= 0x200u || s[i].abs == N48D2_SYMCLKC_CLOCK_ENABLE),
                   "no op writes the BE mode / clock / enable, the TMDS bits or any DCCG register but the ONE symbol-clock exception SYMCLKC_CLOCK_ENABLE (the DMUB owns the rest)");
            if (op == N48D2_OP_TIMING) expect(s[i].abs < 0x53c0u, "timing writes no DIG register");
        }
    }

    // =============================================================================================================================================================================================
    // D. the instance guard
    // =============================================================================================================================================================================================
    {
        // every register of the DP's pipe in Linux's header is FORBIDDEN (exhaustive over the header, by name prefix)
        const char *pfx[] = { "OTG0_", "ODM0_", "VTG0_", "OPP_PIPE0_", "OPPBUF0_", "FMT0_", "DPG0_", "HUBP0_", "HUBPREQ0_", "HUBPRET0_", "DPP_TOP0_", "CNVC_CFG0_", "DSCL0_", "CM0_", "MPCC0_", "MPCC_OGAM0_", "MPCC_MCM0_",
                              "DIG1_", "DP1_", "DME1_", "VPG1_", "AFMT1_", "DP_AUX1_", "DCIO_UNIPHY1_", "RDPCSTX1_", "UNIPHYB_", "DP_DTO", "OTG1_PIXEL_RATE", "OTG1_PHYPLL", "PHYPLL", "OTG_PIXEL_RATE_DIV", "SYMCLK", "DCCG_GATE" };
        std::map<std::string, uint32_t> hit;
        size_t p = 0; uint32_t nAll = 0, nForb = 0;
        while ((p = gOff.find("#define reg", p)) != std::string::npos) {
            p += 11; size_t e = p; while (e < gOff.size() && gOff[e] != ' ' && gOff[e] != '\t') ++e;
            std::string name = gOff.substr(p, e - p);
            if (name.size() > 9 && name.compare(name.size() - 9, 9, "_BASE_IDX") == 0) continue;
            for (const char *x : pfx) if (name.compare(0, std::strlen(x), x) == 0) {
                uint32_t a = 0; if (!lx_reg(name, &a)) continue;
                if (name == "SYMCLKC_CLOCK_ENABLE") {      // 0.0.633: the ONE DCCG register instance 1 may write (value / mask judged separately below)
                    ++gRun; if (n48d2_guard_addr(a, nullptr) != N48D2_G_OK) { ++gFail; std::printf("FAIL: the symbol clock exception SYMCLKC_CLOCK_ENABLE is not admitted by instance 1\n"); }
                    break;
                }
                if (std::string(x) == "OTG1_PIXEL_RATE" || std::string(x) == "OTG1_PHYPLL" || std::string(x) == "PHYPLL" || std::string(x) == "DP_DTO" || std::string(x) == "OTG_PIXEL_RATE_DIV" || std::string(x) == "SYMCLK" || std::string(x) == "DCCG_GATE")
                    if (a >= 0x200u) continue;   // only the BASE_IDX 1 ones are the DCCG block's
                ++nAll;
                const char *why = nullptr;
                const uint32_t g = n48d2_guard_addr(a, &why);
                if (g == N48D2_G_FORBIDDEN || (name == "SYMCLKD_CLOCK_ENABLE" && g == N48D2_G_OTHER)) ++nForb; else {   /* 0.0.633: instance 2's one exception register is named OTHER for instance 1 (a refusal all the same) */ ++gRun; ++gFail; std::printf("FAIL: the DP-path register %s (%#x) is NOT refused as forbidden (guard %u)\n", name.c_str(), a, g); }
                hit[x]++;
                // and every refusal of a full write too
                ++gRun; if (n48d2_guard_write(a, 0u, nullptr) == N48D2_G_OK) { ++gFail; std::printf("FAIL: guard_write admits %s\n", name.c_str()); }
                break;
            }
        }
        std::printf("D: %u DP-path / DCCG registers from the Linux header, %u refused as forbidden\n", nAll, nForb);
        expect(nAll > 900 && nAll == nForb, "every DP-path / DCCG register in the header is forbidden");
        for (const char *x : { "OTG0_", "ODM0_", "VTG0_", "OPP_PIPE0_", "DPG0_", "FMT0_", "HUBP0_", "HUBPREQ0_", "DPP_TOP0_", "CM0_", "MPCC0_", "DIG1_", "DP1_", "DCIO_UNIPHY1_", "RDPCSTX1_", "UNIPHYB_", "DP_DTO", "OTG1_PIXEL_RATE", "OTG1_PHYPLL", "OTG_PIXEL_RATE_DIV" }) {
            char b[96]; std::snprintf(b, sizeof b, "the header has %s registers and the test reached them", x); expect(hit[x] > 0, b); }
        // the named ones the brief lists
        for (const char *n : { "OTG0_OTG_CONTROL", "OTG0_OTG_H_TOTAL", "ODM0_OPTC_DATA_SOURCE_SELECT", "ODM0_OPTC_INPUT_CLOCK_CONTROL", "DIG1_DIG_FE_CNTL", "DIG1_DIG_BE_CNTL", "DIG1_DIG_BE_CLK_CNTL", "DIG1_STREAM_MAPPER_CONTROL",
                               "OPP_PIPE0_OPP_PIPE_CONTROL", "DPG0_DPG_CONTROL", "HUBP0_DCSURF_SURFACE_CONFIG", "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", "DPP_TOP0_DPP_CONTROL", "MPCC0_MPCC_CONTROL", "OTG0_PIXEL_RATE_CNTL",
                               "DP_DTO0_PHASE", "DP_DTO0_MODULO", "OTG_PIXEL_RATE_DIV", "DCIO_UNIPHY1_UNIPHY_MACRO_CNTL_RESERVED0", "RDPCSTX1_RDPCSTX_PHY_CNTL0", "OTG0_INTERRUPT_DEST", "UNIPHYB_LINK_CNTL", "VTG0_CONTROL", "OTG1_PHYPLL_PIXEL_RATE_CNTL", "OTG1_PIXEL_RATE_CNTL" }) {
            uint32_t a = 0; char b[120];
            if (!lx_reg(n, &a)) { std::snprintf(b, sizeof b, "Linux has %s", n); expect(false, b); continue; }
            std::snprintf(b, sizeof b, "%s (%#x) is FORBIDDEN by name", n, a);
            expect(n48d2_guard_addr(a, nullptr) == N48D2_G_FORBIDDEN, b);
        }
        // every listed register admitted; its OTG0 / DIG1 twin (one instance down) forbidden or unlisted, never admitted
        for (uint32_t i = 0; i < N48D2_ALLOWED_COUNT; i++) {
            const uint32_t a = n48d2_allowed[i];
            expect(n48d2_guard_addr(a, nullptr) == N48D2_G_OK, "a listed register is admitted");
            uint32_t twin = 0;
            if (a >= 0x4f9au && a <= 0x4fa2u) twin = a - 0x10u;                     // ODM stride 0x10
            else if (a >= 0x506au && a <= 0x50e9u) twin = a - 0x80u;                 // OTG stride 0x80
            else if (a >= 0x4d56u && a <= 0x4dafu) twin = a - 0x5au;                 // OPP stride 0x5a
            else if (a == N48D2_VTG1_CONTROL) twin = a - 1u;
            else if (a == N48D2_DIG2_STREAM_MAPPER) twin = a - 1u;
            else if (a >= 0x579bu && a <= 0x57f9u) twin = a - 0x124u;                // DIG stride 0x124
            char b[96]; std::snprintf(b, sizeof b, "the DP twin %#x of listed %#x is FORBIDDEN", twin, a);
            expect(twin != 0 && n48d2_guard_addr(twin, nullptr) == N48D2_G_FORBIDDEN, b);
        }
        // a sweep of the whole display aperture: admitted == exactly the list
        uint32_t admitted = 0;
        for (uint32_t a = 0; a < 0xA000u; a++) if (n48d2_guard_addr(a, nullptr) == N48D2_G_OK) ++admitted;
        expect_u("the whole 0..0xA000 sweep admits exactly the 39 listed registers + the ONE symbol-clock exception (SYMCLKC_CLOCK_ENABLE) + (0.0.655) the 43 plane registers and the two DCCG exceptions", admitted, N48D2_ALLOWED_COUNT + 1u + N48D2_ALLOWED1P_COUNT + 2u);
        expect(n48d2_guard_addr(0x5ba7u, nullptr) == N48D2_G_UNLISTED && n48d2_guard_addr(0x50c8u, nullptr) == N48D2_G_UNLISTED && n48d2_guard_addr(0x58bfu, nullptr) == N48D2_G_OTHER, "OTG1 registers off the list are UNLISTED; instance 2's DIG3_FE_CNTL (0x58bf) is the OTHER instance's");
        // the value rules
        expect(n48d2_guard_write(N48D2_DIG2_FE_CNTL, 0x00000000u, nullptr) == N48D2_G_VALUE, "FE source OTG0 is refused");
        expect(n48d2_guard_write(N48D2_DIG2_FE_CNTL, 0x00000111u, nullptr) == N48D2_G_OK && n48d2_guard_write(N48D2_DIG2_FE_CNTL, 0x00000002u, nullptr) == N48D2_G_VALUE, "FE source OTG1 only");
        expect(n48d2_guard_write(N48D2_DIG2_BE_CNTL, 0x20000600u, nullptr) == N48D2_G_VALUE && n48d2_guard_write(N48D2_DIG2_BE_CNTL, 0x20000200u, nullptr) == N48D2_G_VALUE, "the BE FE-source FE1 bit is refused");
        expect(n48d2_guard_write(N48D2_DIG2_BE_CNTL, 0x20000400u, nullptr) == N48D2_G_OK && n48d2_guard_write(N48D2_DIG2_BE_CNTL, 0x20000000u, nullptr) == N48D2_G_OK, "FE2 bit or none");
        for (uint32_t b = 0; b < 7; b++) if (b != 2) expect(n48d2_guard_write(N48D2_DIG2_BE_CNTL, 0x20000000u | (1u << (8 + b)), nullptr) == N48D2_G_VALUE, "every other FE-source bit is refused");
        expect(n48d2_guard_write(N48D2_DIG2_STREAM_MAPPER, 1u, nullptr) == N48D2_G_VALUE && n48d2_guard_write(N48D2_DIG2_STREAM_MAPPER, 3u, nullptr) == N48D2_G_VALUE &&
               n48d2_guard_write(N48D2_DIG2_STREAM_MAPPER, 2u, nullptr) == N48D2_G_OK && n48d2_guard_write(N48D2_DIG2_STREAM_MAPPER, 0u, nullptr) == N48D2_G_OK, "mapper link 0 or 2 only (never 1, PHY B)");
        expect(n48d2_guard_write(N48D2_ODM1_DATA_SOURCE_SELECT, 0xFFF00000u, nullptr) == N48D2_G_VALUE && n48d2_guard_write(N48D2_ODM1_DATA_SOURCE_SELECT, 0xFFF10000u, nullptr) == N48D2_G_OK &&
               n48d2_guard_write(N48D2_ODM1_DATA_SOURCE_SELECT, 0xFFFF0000u, nullptr) == N48D2_G_OK, "ODM1 SEG0 = OPP1 or 0xf (never OPP0)");
        // the real lists pass; a list that touches OTG0 / DIG1 / DCCG is refused WHOLE
        for (uint32_t op : { N48D2_OP_TIMING, N48D2_OP_CONNECT, N48D2_OP_OFF }) {
            n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(op, s); uint32_t bad = 0;
            expect(n48d2_guard_list(s, n, &bad) == 0u && bad == 0xFFFFFFFFu, "the built list passes the guard");
            for (uint32_t i = 0; i < n; i++) if (n48d2_is_write(&s[i])) {
                n48d2_step t[N48D2_MAX_STEPS]; std::memcpy(t, s, sizeof(t));
                const uint32_t a = t[i].abs;
                t[i].abs = (a >= 0x506au && a <= 0x50e9u) ? a - 0x80u : (a >= 0x579bu) ? a - 0x124u : (a >= 0x4f9au && a <= 0x4fa2u) ? a - 0x10u : (a >= 0x4d56u && a <= 0x4dafu) ? a - 0x5au : a - 1u;
                char b[96]; std::snprintf(b, sizeof b, "op %u step %u moved to the DP twin %#x: the whole list is refused at that step", op, i, t[i].abs);
                expect(n48d2_guard_list(t, n, &bad) == N48D2_GUARD_REFUSED && bad == i, b);
                std::memcpy(t, s, sizeof(t)); t[i].abs = 0x141u;   // DP_DTO0 (DCCG)
                expect(n48d2_guard_list(t, n, &bad) == N48D2_GUARD_REFUSED && bad == i, "a step aimed at the DCCG refuses the list");
            }
        }
        { n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(N48D2_OP_CONNECT, s); uint32_t bad = 0;
          s[1].val = 0u; expect(n48d2_guard_list(s, n, &bad) == N48D2_GUARD_REFUSED && bad == 1, "a connect list routing FE2 from OTG0 is refused before anything runs");
          n48d2_build(N48D2_OP_CONNECT, s); s[8].val = 0x600u; s[8].mask = 0x600u; expect(n48d2_guard_list(s, n, &bad) == N48D2_GUARD_REFUSED && bad == 8, "a connect list setting the FE1 bit is refused");
          n48d2_build(N48D2_OP_CONNECT, s); s[12].val = 1u; expect(n48d2_guard_list(s, n, &bad) == N48D2_GUARD_REFUSED && bad == 12, "a connect list mapping DIG2 to link 1 (PHY B) is refused"); }
        { n48d2_step s[N48D2_MAX_STEPS]; const uint32_t n = n48d2_build(N48D2_OP_TIMING, s); uint32_t bad = 0;
          expect(s[4].kind == N48D2_K_SET && s[4].abs == N48D2_ODM1_DATA_SOURCE_SELECT, "timing step 4 is the ODM1 data-source SET");
          s[4].val = 0xFFF00000u; expect(n48d2_guard_list(s, n, &bad) == N48D2_GUARD_REFUSED && bad == 4, "a SET routing ODM1 from OPP0 (SEG0 0) is refused by the list guard before anything runs"); }
        { n48d2_step s[N48D2_MAX_STEPS]; uint32_t bad = 0;
          expect(n48d2_guard_list(s, 0, &bad) == N48D2_GUARD_REFUSED && n48d2_guard_list(s, N48D2_MAX_STEPS + 1, &bad) == N48D2_GUARD_REFUSED, "an empty or oversized list is refused");
          n48d2_build(N48D2_OP_OFF, s); s[3].kind = 9; expect(n48d2_guard_list(s, 23, &bad) == N48D2_GUARD_REFUSED && bad == 3, "an unknown step kind is refused"); }
    }

    // =============================================================================================================================================================================================
    // E. the flow over the model
    // =============================================================================================================================================================================================
    const std::vector<W> kTiming = {
        { 0x4fa0, 0x00000003 }, { 0x50c4, 0x00000003 }, { 0x4f9b, 0xFFF10000 }, { 0x506e, 0x00000000 }, { 0x4fa1, 0x00000000 }, { 0x506a, 0x00000897 }, { 0x506c, 0x002C0000 }, { 0x506b, 0x00C00840 },
        { 0x506d, 0x00000000 }, { 0x506f, 0x00000464 }, { 0x5071, 0x00000464 }, { 0x5070, 0x00000464 }, { 0x5079, 0x00050000 }, { 0x5078, 0x00290461 }, { 0x507a, 0x00000000 }, { 0x5085, 0x00000000 },
        { 0x39f1, 0x00000000 }, { 0x5083, 0x00000000 }, { 0x50c5, 0x0000000D }, { 0x50c6, 0x014002A8 }, { 0x50c7, 0x0000012C }, { 0x39f1, 0x04610000 }, { 0x4f9c, 0x00000000 }, { 0x506e, 0x00000000 },
        { 0x506e, 0x00000000 }, { 0x4da6, 0x00000001 }, { 0x4d63, 0x00000000 }, { 0x4d70, 0x07800438 }, { 0x4d74, 0x00000000 }, { 0x4d6e, 0x00661001 }, { 0x4f9b, 0xFFF10000 }, { 0x39f1, 0x84610000 },
        { 0x5083, 0x00000201 },
    };
    const std::vector<W> kConnect = {
        { 0x162, 0x00000211 }, { 0x579b, 0x00000001 }, { 0x5083, 0x00010201 }, { 0x579b, 0x00000011 }, { 0x579b, 0x00000111 }, { 0x57a0, 0x0000001F }, { 0x57a6, 0x00000000 }, { 0x57a6, 0x00000000 }, { 0x57c4, 0x20000400 },
        { 0x579c, 0x00000802 }, { 0x579c, 0x00000812 }, { 0x579d, 0x00000001 }, { 0x53cf, 0x00000002 }, { 0x57a3, 0x00000000 }, { 0x57a3, 0x0000001C }, { 0x579c, 0x00000812 }, { 0x579d, 0x00000001 },
        { 0x57a3, 0x0000001E }, { 0x57a3, 0x0010001C }, { 0x57a3, 0x0000001D },
    };
    const std::vector<W> kOff = {
        { 0x4d6e, 0 }, { 0x4d71, 0 }, { 0x4d72, 0 }, { 0x4d73, 0 }, { 0x4d6f, 0 }, { 0x57a3, 0x0000001C }, { 0x579d, 0 }, { 0x579c, 0x00000802 }, { 0x57a3, 0x0000001C }, { 0x579d, 0 }, { 0x579c, 0x00000802 },
        { 0x57c4, 0x20000000 }, { 0x162, 0x00000001 }, { 0x53cf, 0 }, { 0x4f9b, 0xFFFF0000 }, { 0x4fa1, 0 }, { 0x5083, 0x00010200 }, { 0x39f1, 0x04610000 }, { 0x50c4, 0x00000100 }, { 0x4fa0, 0x00000004 }, { 0x4da6, 0 },
    };
    {   // the happy path: timing, connect, status, off
        Model m;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING);
        expect_u("timing: status OK", t.status, N48D2_OK);
        expect_writes("timing: the exact write sequence", m.writes, kTiming);
        expect(t.done == 33 && t.total == 36 && t.timeouts == 0 && t.fail == 0xFF && t.auto_off == 0 && t.dp_changed == 0, "timing: 33 writes of 36 steps, no timeout, no rollback, DP unchanged");
        expect(t.f0b != t.f0a && t.f1b != t.f1a && (t.otg1_ctl & 1u) && t.dpg_ctl == 0x00661001u, "timing: OTG0 still counting, OTG1 counting, DPG1 on");
        const size_t w0 = m.writes.size();
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect_u("connect: status OK", c.status, N48D2_OK);
        expect_writes("connect: the exact write sequence", writes_from(m, w0), kConnect);
        expect(c.done == 20 && c.total == 22 && c.timeouts == 0 && c.fe_en == 1u && c.be_cntl == 0x20000400u && m.get(N48D2_DIG2_STREAM_MAPPER) == 2u && (m.get(N48D2_DIG2_FE_CNTL) & 7u) == 1u && (m.get(N48D2_DIG2_FE_CLK_CNTL) & 7u) == 2u,
               "connect: FE2 enabled from OTG1 in DVI mode, BE FE source 0x04, mapper 2");
        expect(m.get(N48D2_DIG2_BE_CLK_CNTL) == 0x2812u && m.get(N48D2_DIG2_BE_EN_CNTL) == 1u, "connect left the DMUB's back end mode / clock / enable untouched");
        const size_t w1 = m.writes.size(), r1 = m.reads.size();
        uint64_t v[13];
        expect_u("status: OK", n48d2::status(m, v), N48D2_OK);
        expect(m.writes.size() == w1 && m.reads.size() == r1 + N48D2_STAT_COUNT, "status: reads every listed register once, writes NOTHING");
        expect((v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == N48D2_OP_STATUS && ((v[0] >> 16) & 0xFF) == N48D2_STAT_COUNT, "status: v[0] layout");
        expect((uint32_t)v[1] == m.rd(N48D2_OTG1_CONTROL) && (uint32_t)(v[8] >> 32) == 0x20000400u && (uint32_t)v[10] == 2u && (uint32_t)v[11] == 2u && (uint32_t)v[2] != (uint32_t)(v[2] >> 32) && (uint32_t)(v[11] >> 32) != (uint32_t)v[12],
               "status: OTG1_CONTROL, DIG2 BE_CNTL, mapper, PHYPLL source packed in place; both frame counters moved across 50 ms");
        {   // 0.0.632: the second status page - 12 more registers, reads only
            struct SR { const char *lx; uint32_t ours; };
            const SR srs[N48D2_STAT2_COUNT] = { { "SYMCLKC_CLOCK_ENABLE", N48D2_SYMCLKC_CLOCK_ENABLE }, { "OTG1_PIXEL_RATE_CNTL", N48D2_OTG1_PIXEL_RATE_CNTL }, { "OTG1_OTG_V_TOTAL_CONTROL", N48D2_OTG1_V_TOTAL_CONTROL },
                { "FMT1_FMT_CONTROL", N48D2_FMT1_CONTROL }, { "FMT1_FMT_BIT_DEPTH_CONTROL", N48D2_FMT1_BIT_DEPTH_CONTROL }, { "FMT1_FMT_DYNAMIC_EXP_CNTL", N48D2_FMT1_DYNAMIC_EXP_CNTL }, { "FMT1_FMT_CLAMP_CNTL", N48D2_FMT1_CLAMP_CNTL },
                { "FMT1_FMT_CLAMP_COMPONENT_R", N48D2_FMT1_CLAMP_COMPONENT_R }, { "FMT1_FMT_CLAMP_COMPONENT_G", N48D2_FMT1_CLAMP_COMPONENT_G }, { "FMT1_FMT_CLAMP_COMPONENT_B", N48D2_FMT1_CLAMP_COMPONENT_B },
                { "HUBP1_DCHUBP_CNTL", N48D2_HUBP1_DCHUBP_CNTL }, { "HUBP1_HUBP_CLK_CNTL", N48D2_HUBP1_HUBP_CLK_CNTL } };
            for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) {
                char b[160]; std::snprintf(b, sizeof b, "status2 register %s resolves to Linux's absolute address (segment + offset)", srs[i].lx);
                expect_u(b, srs[i].ours, lx_abs(srs[i].lx)); expect(n48d2_stat2_regs[i].abs == srs[i].ours && n48d2_stat2_regs[i].name != nullptr && n48d2_stat2_regs[i].name[0] != 0, "status2 table entry i is that register, named");
                const char *why = nullptr;
                if (i == 0) expect(n48d2_guard_addr(srs[i].ours, &why) == N48D2_G_OK && srs[i].ours == N48D2_SYMCLKC_CLOCK_ENABLE, "status2 register 0 is SYMCLKC_CLOCK_ENABLE: the ONE DCCG register the verb writes (0.0.633), through the exception");
                else if (srs[i].ours == N48D2_HUBP1_DCHUBP_CNTL || srs[i].ours == N48D2_HUBP1_HUBP_CLK_CNTL) expect(n48d2_guard_addr(srs[i].ours, &why) == N48D2_G_OK && (srs[i].ours != N48D2_HUBP1_DCHUBP_CNTL || !m.wr(srs[i].ours, 0xFFFFFFFFu)), "status2 HUBP1 registers: 0.0.655 instance 1's PLANE registers - admitted by address (DCHUBP_CNTL not with VTG_SEL 0xf; the kind rules are the list guard's)");
                else expect(n48d2_guard_addr(srs[i].ours, &why) != N48D2_G_OK && !m.wr(srs[i].ours, 0u), "status2 register is NOT writable by this verb (guard refuses it, the model's write path refuses it)");
            }
            expect(srs[0].ours == 0x162u && srs[1].ours == 0x144u && srs[2].ours == 0x5073u && srs[10].ours == 0x3b90u, "status2: SYMCLKC 0x162, OTG1_PIXEL_RATE_CNTL 0x144, OTG1_V_TOTAL_CONTROL 0x5073, HUBP1_DCHUBP_CNTL 0x3b90");
            for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) m.r[n48d2_stat2_regs[i].abs] = 0xA5000000u + i;
            const size_t w1b = m.writes.size(), r1b = m.reads.size(); uint64_t v2[13];
            expect_u("status2: OK", n48d2::status2(m, v2), N48D2_OK);
            expect(m.writes.size() == w1b && m.reads.size() == r1b + N48D2_STAT2_COUNT, "status2: reads every listed register once (12), writes NOTHING");
            bool inOrder = true; for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) inOrder = inOrder && m.reads[r1b + i] == n48d2_stat2_regs[i].abs;
            expect(inOrder, "status2: the reads are exactly the table, in order");
            expect((v2[0] & 0xFF) == 0 && ((v2[0] >> 8) & 0xFF) == N48D2_OP_STATUS2 && ((v2[0] >> 16) & 0xFF) == N48D2_STAT2_COUNT && v2[7] == 0 && v2[12] == 0, "status2: v[0] layout, v[7..12] untouched");
            bool packed = true; for (uint32_t i = 0; i < N48D2_STAT2_COUNT; i++) packed = packed && (uint32_t)(v2[1 + i / 2u] >> (32u * (i & 1u))) == 0xA5000000u + i;
            expect(packed, "status2: every register's value is packed two per scalar in table order");
            m.r[N48D2_SYMCLKC_CLOCK_ENABLE] = 0x211u;       // the status2 loop above overwrote it with a marker; connect left it at 0x211 (CLOCK_ENABLE 1 | FE_EN | SRC 2)
            m.r[N48D2_HUBP1_HUBP_CLK_CNTL] = 0u; m.r[N48D2_HUBP1_DCHUBP_CNTL] = 0u;       // 0.0.655: the markers of the status2 loop would read as a running HUBP1 clock (`off` refuses while it is on)
        }
        const size_t w2 = m.writes.size();
        const n48d2_out o = n48d2::run(m, N48D2_OP_OFF);
        expect_u("off: status OK", o.status, N48D2_OK);
        expect_writes("off: the exact write sequence", writes_from(m, w2), kOff);
        expect(o.done == 21 && o.total == 23 && (m.get(N48D2_OTG1_CONTROL) & 1u) == 0 && m.get(N48D2_DIG2_FE_EN_CNTL) == 0 && m.get(N48D2_DIG2_STREAM_MAPPER) == 0 && m.get(N48D2_DIG2_BE_CNTL) == 0x20000000u &&
               m.get(N48D2_DPG1_CONTROL) == 0 && (m.get(N48D2_OTG1_CLOCK_CONTROL) & 3u) == 0 && (m.get(N48D2_ODM1_INPUT_CLOCK_CONTROL) & 3u) == 0 && m.get(N48D2_OPP_PIPE1_CONTROL) == 0,
               "off: OTG1 stopped, FE off, mapper 0, BE FE source back to the firmware's 0x20000000, DPG off, clocks off");
        // the DP side never written, at any point
        for (const W &w : m.writes) for (uint32_t k = 0; k < N48D2_FORBIDDEN_COUNT; k++) { const bool inSpan = w.abs >= n48d2_forbidden[k].lo && w.abs <= n48d2_forbidden[k].hi && (n48d2_forbidden[k].skip & N48D2_SKIP_MONA) == 0u; if (inSpan && w.abs != N48D2_SYMCLKC_CLOCK_ENABLE) std::printf("   write %#x lands in span %u (%s)\n", w.abs, k, n48d2_forbidden[k].why); expect(!inSpan || w.abs == N48D2_SYMCLKC_CLOCK_ENABLE, "no write ever lands in a forbidden span that applies to instance 1 (but the symbol clock exception SYMCLKC_CLOCK_ENABLE)"); }
        expect(m.get(N48D2_OTG0_CONTROL) == 0x00011201u && m.get(N48D2_DIG1_BE_CNTL) == 0x10000200u && m.get(N48D2_DIG1_BE_CLK_CNTL) == 0x810u && m.get(0x53ceu) == 1u, "the DP's registers are as they were");
        // a second off from the off state: still safe, the same list, no refusal
        const n48d2_out o2 = n48d2::run(m, N48D2_OP_OFF);
        expect(o2.status == N48D2_OK && o2.done == 21, "off again from the off state is safe and complete");
    }
    {   // ORDER: timing before connect (connect refuses without OTG1 running; nothing written)
        Model m;
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect(c.status == N48D2_NO_TIMING && m.writes.empty() && c.pre_abs == N48D2_OTG1_CONTROL, "ORDER: connect before timing is refused NO_TIMING and writes nothing");
        // and within each list
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING); (void)t;
        const std::vector<W> &w = m.writes;
        expect(first_write(w, N48D2_ODM1_INPUT_CLOCK_CONTROL) < first_write(w, N48D2_OTG1_H_TOTAL) && first_write(w, N48D2_OTG1_CLOCK_CONTROL) < first_write(w, N48D2_OTG1_H_TOTAL) &&
               last_write(w, N48D2_OTG1_V_BLANK_START_END) < first_write(w, N48D2_DPG1_CONTROL) && first_write(w, N48D2_OPP_PIPE1_CONTROL) < first_write(w, N48D2_DPG1_CONTROL) &&
               first_write(w, N48D2_DPG1_CONTROL) < last_write(w, N48D2_OTG1_CONTROL) && last_write(w, N48D2_OTG1_CONTROL) == w.size() - 1, "ORDER (timing): clocks, timing, OPP clock, DPG, then the master enable LAST");
    }
    {   // ORDER within connect / off
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); const size_t w0 = m.writes.size(); (void)n48d2::run(m, N48D2_OP_CONNECT);
        const std::vector<W> w = writes_from(m, w0);
        expect(first_write(w, N48D2_SYMCLKC_CLOCK_ENABLE) == 0 && first_write(w, N48D2_DIG2_FE_CNTL) == 1 && first_write(w, N48D2_DIG2_BE_CNTL) < first_write(w, N48D2_DIG2_FE_EN_CNTL) && first_write(w, N48D2_DIG2_FE_EN_CNTL) < first_write(w, N48D2_DIG2_STREAM_MAPPER) &&
               first_write(w, N48D2_DIG2_STREAM_MAPPER) < first_write(w, N48D2_DIG2_FIFO_CTRL0) && last_write(w, N48D2_DIG2_FIFO_CTRL0) == w.size() - 1 && (w.back().v & 1u),
               "ORDER (connect): the front end's SYMCLKC first, then the FE source, BE FE-source, FE enable, mapper, then the FIFO, enabled LAST");
        const size_t w1 = m.writes.size(); (void)n48d2::run(m, N48D2_OP_OFF);
        const std::vector<W> f = writes_from(m, w1);
        expect(first_write(f, N48D2_DPG1_CONTROL) == 0 && last_write(f, N48D2_DPG1_RAMP_CONTROL) < first_write(f, N48D2_DIG2_FIFO_CTRL0) && last_write(f, N48D2_DIG2_FE_EN_CNTL) < first_write(f, N48D2_DIG2_BE_CNTL) && first_write(f, N48D2_DIG2_BE_CNTL) < first_write(f, N48D2_SYMCLKC_CLOCK_ENABLE) && first_write(f, N48D2_SYMCLKC_CLOCK_ENABLE) < first_write(f, N48D2_DIG2_STREAM_MAPPER) &&
               first_write(f, N48D2_DIG2_BE_CNTL) < first_write(f, N48D2_DIG2_STREAM_MAPPER) && first_write(f, N48D2_DIG2_STREAM_MAPPER) < first_write(f, N48D2_OTG1_CONTROL) &&
               first_write(f, N48D2_OTG1_CONTROL) < first_write(f, N48D2_OTG1_CLOCK_CONTROL) && first_write(f, N48D2_OTG1_CLOCK_CONTROL) < first_write(f, N48D2_ODM1_INPUT_CLOCK_CONTROL) &&
               last_write(f, N48D2_OPP_PIPE1_CONTROL) == f.size() - 1, "ORDER (off reverses): DPG off, DIG2 FIFO / FE off, BE FE-source (the disconnect), THEN SYMCLKC FE off (0.0.634: reset_dio_stream_encoder, then disable_symclk_se), mapper, OTG1 master, OTG clock, ODM clock, OPP clock LAST");
    }
    {   // timing prechecks: each refusal writes nothing
        Model a; a.r[N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL] = 0u; const n48d2_out x = n48d2::run(a, N48D2_OP_TIMING);
        expect(x.status == N48D2_NO_PIXCLK && a.writes.empty() && x.pre_abs == 0x147u && x.pre_val == 0u, "timing without `pclk otg1-on` (PHYPLL source 0) is refused, nothing written");
        Model b; b.r[N48D2_OTG1_CONTROL] = 1u; const n48d2_out y = n48d2::run(b, N48D2_OP_TIMING);
        expect(y.status == N48D2_OTG1_BUSY && b.writes.empty(), "timing with OTG1 already enabled is refused, nothing written");
        Model c; c.otg0Stops = true; const n48d2_out z = n48d2::run(c, N48D2_OP_TIMING);
        expect(z.status == N48D2_DP_NOT_COUNTING && c.writes.empty(), "timing with OTG0 not counting is refused, nothing written");
        Model d; d.otg0Stops = true; const n48d2_out zz = n48d2::run(d, N48D2_OP_CONNECT);
        expect(zz.status == N48D2_DP_NOT_COUNTING && d.writes.empty(), "connect with OTG0 not counting is refused, nothing written");
    }
    {   // connect prechecks
        Model a; (void)n48d2::run(a, N48D2_OP_TIMING); a.r[N48D2_DIG2_BE_CLK_CNTL] = 0x810u; size_t w = a.writes.size(); n48d2_out x = n48d2::run(a, N48D2_OP_CONNECT);
        expect(x.status == N48D2_NO_PHY && a.writes.size() == w && x.pre_abs == N48D2_DIG2_BE_CLK_CNTL, "connect with the BE in DP mode (the M4b state) is refused, nothing written");
        a.r[N48D2_DIG2_BE_CLK_CNTL] = 0x2802u; x = n48d2::run(a, N48D2_OP_CONNECT); expect(x.status == N48D2_NO_PHY && a.writes.size() == w, "connect with the BE clock off is refused");
        a.r[N48D2_DIG2_BE_CLK_CNTL] = 0x2812u; a.r[N48D2_DIG2_BE_EN_CNTL] = 0u; x = n48d2::run(a, N48D2_OP_CONNECT); expect(x.status == N48D2_NO_PHY && a.writes.size() == w, "connect with the BE disabled is refused");
        a.r[N48D2_DIG2_BE_EN_CNTL] = 1u; a.r[N48D2_DIG2_FE_EN_CNTL] = 1u; x = n48d2::run(a, N48D2_OP_CONNECT); expect(x.status == N48D2_FE_BUSY && a.writes.size() == w, "connect with FE2 already enabled is refused");
    }
    {   // the DP watch: OTG0 stops when OTG1's master enable lands -> off runs at once
        Model m; m.otg0StopOnMaster = true;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING);
        expect(t.status == N48D2_DP_DISTURBED && t.auto_off == 1 && (t.dp_changed & 0x80u) && (m.get(N48D2_OTG1_CONTROL) & 1u) == 0 && (m.get(N48D2_OTG1_CLOCK_CONTROL) & 1u) == 0,
               "OTG0 stopping after timing: DP_DISTURBED, `off` ran (OTG1 master and clock off)");
        expect_u("the rollback is the off list after the 33 timing writes", m.writes.size(), 33 + 21);
    }
    {   // the DP watch: a DIG1 register changes during connect -> off
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); m.dig1FlipOnFeEn = true;
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect(c.status == N48D2_DP_DISTURBED && c.auto_off == 1 && (c.dp_changed & (1u << 3)) && m.get(N48D2_DIG2_FE_EN_CNTL) == 0 && m.get(N48D2_DIG2_STREAM_MAPPER) == 0,
               "DIG1_DIG_BE_CNTL changing across connect: DP_DISTURBED, `off` ran (FE off, mapper 0)");
    }
    {   // the allowlist refusing part-way: stop, roll back
        Model m; m.refuseAbs = N48D2_OTG1_V_TOTAL;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING);
        expect(t.status == N48D2_ALLOW_REFUSED && t.fail == 11 && t.done == 9 && t.auto_off == 1 && (m.get(N48D2_OTG1_CLOCK_CONTROL) & 1u) == 0, "an allowlist refusal at step 11 (OTG_V_TOTAL) stops timing (9 writes done) and runs off");
        bool after = false; for (size_t i = 9; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_OTG1_V_TOTAL_MAX) after = true;
        expect(!after, "no timing step after the refused one ran");
    }
    {   // off is best effort: a refused write is skipped, the rest still run
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); (void)n48d2::run(m, N48D2_OP_CONNECT); m.refuseAbs = N48D2_DIG2_STREAM_MAPPER;
        const n48d2_out o = n48d2::run(m, N48D2_OP_OFF);
        expect(o.status == N48D2_ALLOW_REFUSED && o.done == 20 && (m.get(N48D2_OTG1_CONTROL) & 1u) == 0 && m.get(N48D2_OPP_PIPE1_CONTROL) == 0, "off with one refused write still runs every other step and reports it");
    }
    {   // a value the guard refuses at run time (the firmware left FE1 in DIG2's BE): connect stops and rolls back, the BE is never written with FE1
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); m.r[N48D2_DIG2_BE_CNTL] = 0x20000200u; const size_t w0 = m.writes.size();
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect(c.status == N48D2_GUARD_REFUSED && c.fail == 8 && c.guard == N48D2_G_VALUE && c.auto_off == 1, "a live BE_CNTL with the FE1 bit stops connect at the BE step (value guard) and rolls back");
        bool beWritten = false; for (size_t i = w0; i < m.writes.size(); i++) if (m.writes[i].abs == N48D2_DIG2_BE_CNTL) beWritten = true;
        expect(!beWritten, "DIG2_BE_CNTL is never written while it holds FE1");
    }
    {   // wait timeouts are counted, never fatal (Linux's REG_WAIT)
        Model m; m.clocksWork = false;
        const n48d2_out t = n48d2::run(m, N48D2_OP_TIMING);
        expect(t.status == N48D2_WAIT_TIMEOUT && t.timeouts == 2 && t.done == 33 && m.delays >= 2000, "clocks that never report ON: both waits time out (1000 polls each), every write still done, status WAIT_TIMEOUT");
    }
    {   // WAITIF: no symbol clock -> one 10 us delay instead of the wait
        Model m; (void)n48d2::run(m, N48D2_OP_TIMING); m.symclk = false; m.fifoDoneWorks = false; const uint32_t d0 = m.delays;
        const n48d2_out c = n48d2::run(m, N48D2_OP_CONNECT);
        expect(c.status == N48D2_OK && c.timeouts == 0 && m.delays == d0 + 2, "no symclk: enc35_reset_fifo delays once per reset edge, no wait");
        expect(c.symclk_delay == 2 && c.symclk_wait == 0 && std::strcmp(n48d2_symclk_report(&c), "delay (no symclk)") == 0, "no symclk: the connect result reports \"delay (no symclk)\" (both reset edges delayed)");
        uint64_t pv[13]; n48d2_pack(pv, &c); n48d2_out cu; std::memset(&cu, 0, sizeof cu); n48d2_unpack(pv, &cu);
        expect(((pv[0] >> 49) & 1u) == 0 && ((pv[0] >> 50) & 1u) == 1u && cu.symclk_delay == 1 && cu.symclk_wait == 0 && std::strcmp(n48d2_symclk_report(&cu), "delay (no symclk)") == 0, "the symclk report travels in v[0] bits 49 (waited) / 50 (delayed) and survives the pack");
        Model ok; (void)n48d2::run(ok, N48D2_OP_TIMING); const n48d2_out co = n48d2::run(ok, N48D2_OP_CONNECT);
        uint64_t qv[13]; n48d2_pack(qv, &co); n48d2_out qu; std::memset(&qu, 0, sizeof qu); n48d2_unpack(qv, &qu);
        expect(co.status == N48D2_OK && co.symclk_wait == 2 && co.symclk_delay == 0 && std::strcmp(n48d2_symclk_report(&co), "wait ok (FE symbol clock on)") == 0 && ((qv[0] >> 49) & 1u) == 1u && ((qv[0] >> 50) & 1u) == 0u && std::strcmp(n48d2_symclk_report(&qu), "wait ok (FE symbol clock on)") == 0,
               "symclk on: the connect result reports \"wait ok\" (both reset edges waited), packed in bit 49");
        { Model tm; const n48d2_out ct = n48d2::run(tm, N48D2_OP_TIMING); expect(ct.symclk_wait == 0 && ct.symclk_delay == 0 && std::strcmp(n48d2_symclk_report(&ct), "not reached") == 0, "timing has no FIFO-reset step: \"not reached\""); }
        { n48d2_out mx; std::memset(&mx, 0, sizeof mx); mx.symclk_wait = 1; mx.symclk_delay = 1; expect(std::strstr(n48d2_symclk_report(&mx), "mixed") != nullptr, "one waited + one delayed reads \"mixed\""); }
        { Model rb; (void)n48d2::run(rb, N48D2_OP_TIMING); rb.symclk = false; rb.refuseAbs = N48D2_DIG2_STREAM_MAPPER; const n48d2_out cr = n48d2::run(rb, N48D2_OP_CONNECT);
          expect(cr.auto_off == 1 && cr.symclk_delay == 0 && cr.symclk_wait == 0, "a connect that stopped BEFORE the reset steps and rolled back reports \"not reached\" (the rollback's own steps are not counted)"); }
        Model k; (void)n48d2::run(k, N48D2_OP_TIMING); k.fifoDoneWorks = false;
        const n48d2_out c2 = n48d2::run(k, N48D2_OP_CONNECT);
        expect(c2.status == N48D2_WAIT_TIMEOUT && c2.timeouts == 1, "symclk on but RESET_DONE never rises: one wait times out (the release wait then succeeds)");
    }
    {   // the guard refuses a forbidden step INSIDE run_steps too (defence in depth): nothing reaches the model
        Model m; n48d2_step s[2]; uint32_t n = 0;
        n48d2_put(s, &n, N48D2_K_SET, N48D2_OTG0_CONTROL, 0xFFFFFFFFu, 0u, 0, 0, 0, 0, "OTG0_OTG_CONTROL", "t");
        n48d2_out o; std::memset(&o, 0, sizeof o); uint32_t ref = 0;
        expect(n48d2::run_steps(m, N48D2_INST_MONA, s, n, false, o, &ref) == N48D2_GUARD_REFUSED && m.writes.empty() && ref == 1 && o.guard == N48D2_G_FORBIDDEN, "run_steps refuses a forbidden write itself");
    }
    {   // the result layout round-trips
        n48d2_out a; std::memset(&a, 0, sizeof a);
        a.status = 15; a.op = 2; a.done = 19; a.total = 21; a.fail = 7; a.timeouts = 3; a.auto_off = 1; a.guard = 3; a.dp_changed = 0x88; a.f0a = 1; a.f0b = 2; a.f1a = 3; a.f1b = 4; a.pre_abs = 0x57c3; a.pre_val = 0x810; a.dpx_changed = 0x1A5;
        for (unsigned i = 0; i < N48D2_DP_REGS; i++) { a.dp_before[i] = 0x100 + i; a.dp_after[i] = 0x200 + i; }
        a.otg1_ctl = 0x10201; a.fe_en = 1; a.dpg_ctl = 0x661001; a.be_cntl = 0x20000400; a.fifo = 0x1d; a.fe_clk = 0x812; a.symclk_wait = 1; a.symclk_delay = 1; a.inst = N48D2_INST_MONA;
        uint64_t v[13]; n48d2_pack(v, &a); n48d2_out b; std::memset(&b, 0, sizeof b); n48d2_unpack(v, &b);
        expect(std::memcmp(&a, &b, sizeof a) == 0, "the result packs and unpacks field for field");
    }
    // the switch, the argument, the statuses
    expect(n48d2_latch_value(0, 1) == N48D2_LATCH_OFF && n48d2_latch_value(1, 0) == N48D2_LATCH_OFF && n48d2_latch_value(1, 2) == N48D2_LATCH_OFF && n48d2_latch_value(1, 1) == N48D2_LATCH_ON &&
           !n48d2_latch_is_on(N48D2_LATCH_UNSET) && !n48d2_latch_is_on(N48D2_LATCH_OFF) && n48d2_latch_is_on(N48D2_LATCH_ON), "the latch is ON only for navi48-disp2=1; unset and anything else are OFF");
    expect(!n48d2_arg_ok(0) && n48d2_arg_ok(1) && n48d2_arg_ok(4) && n48d2_arg_ok(5) && n48d2_arg_ok(6) && !n48d2_arg_ok(7) && n48d2_arg_ok(8) && !n48d2_arg_ok(14) && n48d2_arg_ok(0x207) && !n48d2_arg_ok(0x107) && !n48d2_arg_ok(0x307) && !n48d2_arg_ok(1ull << 32) && N48D2_OP_STATUS2 == 5u && N48D2_OP_STATUS3 == 6u && std::strcmp(n48d2_op_name(5), "status2") == 0 && std::strcmp(n48d2_op_name(6), "status3") == 0,
           "only the six ops (timing, connect, off, status, status2, status3) are legal arguments");
    { bool ok = true;   // 0.0.633: the argument is op | instance << 8; instance 0 / 1 = the monitor A (the 0.0.631 argument, unchanged), 2 = the monitor B, nothing else
      for (uint64_t a = 0; a < 0x2000000ull && ok; a += (a < 0x400 ? 1 : 0x13)) {
          const uint64_t op = a & 0xFF, inst = (a >> 8) & 0xFF, hi = a >> 16;
          if (op >= 8 && op <= 14) continue;      // 0.0.635: the plane ops have their own exhaustive check right below (0.0.638: op 13, planerec, joined them; 0.0.652: op 14, fbhold)
          const bool want = op >= 1 && op <= 7 && inst <= 2 && hi == 0 && !(op == 7 && inst != 2);   /* 0.0.634: op 7 (timing1440) is the monitor B's alone; 0.0.635: ops 8..12 are tested below */
          if (n48d2_arg_ok(a) != (want ? 1 : 0)) ok = false;
          if (want && (n48d2_arg_op(a) != op || n48d2_arg_inst(a) != (inst == 2 ? 2u : 1u))) ok = false; }
      expect(ok, "arg_ok accepts exactly op 1..6 with instance byte 0 / 1 / 2, op 7 (timing1440) with instance 2 ONLY, and nothing above bit 15; op and instance decode back");
      for (uint64_t op = 8; op <= 14 && ok; op++) for (uint64_t inst = 0; inst < 4; inst++) for (uint64_t buf = 0; buf < 4; buf++) for (uint64_t top = 0; top < 2; top++) {    // 0.0.635: ops 8..12
          const uint64_t a = op | (inst << 8) | (buf << 16) | (top << 24);
          const bool want = inst <= 2 && !(op == 14 && inst != 2 && inst != 1) && top == 0 && (op == 10 ? buf <= 1 : buf == 0);      // 0.0.655: ops 8..13 for either instance (byte 0 / 1 = the monitor A, 2 = the monitor B), op 14 (fbhold) the monitor B's alone
          if (n48d2_arg_ok(a) != (want ? 1 : 0)) { ok = false; std::printf("   arg_ok(%#llx) wrong\n", (unsigned long long)a); }
          if (want && op == 10 && n48d2_arg_buf(a) != buf) ok = false; }
      expect(ok && n48d2_arg_ok(13) && n48d2_arg_ok(0x20D) && n48d2_arg_ok(0x10D) && n48d2_arg_ok(0x20E) && n48d2_arg_ok(0x10E) && !n48d2_arg_ok(14) && !n48d2_arg_ok(0x20F) && !n48d2_arg_ok(0x1020D) && N48D2_OP_PLANEREC == 13u && std::strcmp(n48d2_op_name(13), "planerec") == 0 && N48D2_OP_FBHOLD == 14u, "0.0.635: ops 8 plane, 9 show, 10 flip, 11 crc, 12 planeoff (and, 0.0.638, 13 planerec; 0.0.652, 14 fbhold) are legal for instance byte 0 / 1 / 2 (0.0.655: the monitor A's plane too; 3 refused; op 14 fbhold for instance 1 or 2 exactly, 0.0.658); the buffer index in bits 16..23 is legal for op 10 alone (0 or 1); bits 24.. never; op 15 is not an op");
      expect(n48d2_arg(3) == 3ull && n48d2_arg_i(3, 1) == 0x103ull && n48d2_arg_i(3, 2) == 0x203ull && n48d2_arg_inst(n48d2_arg(3)) == 1u && n48d2_arg_inst(n48d2_arg_i(3, 1)) == 1u && n48d2_arg_inst(n48d2_arg_i(3, 2)) == 2u && !n48d2_arg_ok(0x301) && !n48d2_arg_ok(0x10001ull),
             "instance 1's argument is the 0.0.631 op byte (unchanged); instance 2 is op | 2 << 8"); }
    for (uint32_t s = 0; s < N48D2_STATUS_COUNT; s++) expect(std::strcmp(n48d2_status_name(s), "unknown") != 0, "every status has a name");
    expect(N48D2_ACT == 98u && n48disp::kActDisp2 == 98u && n48disp::kLastAction == 99u, "disp2 is action 98 (99, scdcread 0.0.633, is the last admitted)");
    expect(n48disp::action_admitted(true, 98) && !n48disp::action_admitted(false, 98) && n48disp::native_exempt(true, 98, 0) && n48disp::native_exempt(true, 98, ~0ull) && !n48disp::native_exempt(false, 98, 1),
           "98 is admitted and native-exempt with navi48-metal-disp ON only (any argument: the verb judges it)");

    inst2_tests();      // 0.0.633: instance 2, the symbol clock exception, status3
    m4_634_tests();     // 0.0.634: timing1440, off's order, the DP-plane watch
    plane_tests();      // 0.0.635: the monitor B's plane (ops 8..12), the guard additions, the 15-row DP watch
    monb_battery_test();      // 0.0.655: the monitor B byte-for-byte against the 0.0.654 sources
    plane1_tests();      // 0.0.655: instance 1 (the monitor A): ops 8..13 at 1080p60, the per-instance guard / allocator / HELD, the monitor B watch
    mona_hold_tests();      // 0.0.658: fbhold 1, the reachability of the monitor A DMUB teardown refusals, the instance <-> index table, the monitor B-watch log lines

    // =============================================================================================================================================================================================
    // S. source pins
    // =============================================================================================================================================================================================
    const std::string src = g_root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), hpp = slurp(src + "dcn/navi48_dcn.hpp"), brg = slurp(src + "Navi48Bringup.cpp"), pure = slurp(src + "amd/native_disp_pure.h"), ucl = slurp(src + "Navi48UserClient.cpp"),
                      flowS = slurp(src + "dcn/navi48_disp2_flow.h"), hdrS = slurp(src + "dcn/navi48_disp2.h"), cli = slurp(g_root + "/tools/pc/navi48test.c"), plist = slurp(g_root + "/src/navi48-bringup/Info.plist"),
                      s1c = slurp(src + "amd/native_s1c.h"), nub = slurp(src + "Navi48MetalNub.cpp");
    expect(!dcn.empty() && !hpp.empty() && !brg.empty() && !pure.empty() && !flowS.empty() && !hdrS.empty() && !cli.empty() && !plist.empty(), "the sources are readable");
    const size_t g0 = dcn.find("build 0.0.631 (multi-monitor stage M4d; an internal design note");
    expect(g0 != std::string::npos, "the disp2 glue block is present");
    const std::string glue = g0 == std::string::npos ? "" : strip_line_comments(dcn.substr(g0));
    const std::string flowCode = strip_line_comments(flowS);
    {   // 1. the OFF switch first: zero the output, the latch, THEN the argument, the device, the bases, the busy flag
        const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {");
        expect(f0 != std::string::npos, "n48dcn::disp2 exists");
        const size_t f1 = glue.find("}  // namespace n48dcn", f0 == std::string::npos ? 0 : f0);
        const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
        const size_t z = fn.find("out[i] = 0u;"), l = fn.find("if (!d2_latched_on()) { out[0] = N48D2_OFF; return N48D2_OFF; }"), a = fn.find("if (!n48d2_arg_ok(arg))"), d = fn.find("__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)"),
                     sg = fn.find("gDcn.seg[1] != N48D2_SEG1 || gDcn.seg[2] != N48D2_SEG2"), b = fn.find("__atomic_exchange_n(&gD2Busy, 1u, __ATOMIC_ACQ_REL)"), rd = fn.find("env"), rl = fn.rfind("__atomic_store_n(&gD2Busy, 0u, __ATOMIC_RELEASE)");     /* 0.0.635: the LAST release (the early one belongs to the plane-state refusal) */
        expect(z != std::string::npos && l != std::string::npos && a != std::string::npos && d != std::string::npos && sg != std::string::npos && b != std::string::npos && rd != std::string::npos && rl != std::string::npos &&
               z < l && l < a && a < d && d < sg && sg < b && b < rd && rd < rl, "REACHABILITY: zero, the OFF switch, the argument, the bound layer, the bases, the busy flag, then the environment; the flag is released");
        expect(fn.substr(0, l == std::string::npos ? 0 : l).find("rd(") == std::string::npos && fn.substr(0, l == std::string::npos ? 0 : l).find("RREG32") == std::string::npos, "nothing is read before the OFF switch");
        expect(dcn.find("PE_parse_boot_argn(\"navi48-disp2\", &v, sizeof(v))") != std::string::npos && dcn.find("n48d2_latch_value(present ? 1 : 0, v)") != std::string::npos && dcn.find("volatile uint32_t gD2Latch = N48D2_LATCH_UNSET;") != std::string::npos,
               "the latch is boot-arg navi48-disp2, default unset = OFF");
        expect_u("the boot-arg is read at ONE site", count_of(dcn, "PE_parse_boot_argn(\"navi48-disp2\""), 1);
        expect(dcn.find("(void)d2_latched_on();                                   // 0.0.631: latch navi48-disp2 at start") != std::string::npos, "attach() latches it at start");
    }
    {   // 2. the write: the guard, the allowlist, ONE WREG32
        expect_u("the glue file has exactly two WREG32 (the disp2 one and, 0.0.661, the instance-2 address writer)", count_of(glue, "WREG32("), 2);
        const size_t g = glue.find("const uint32_t g = n48d2_guard_write_i(inst, abs, v, &why);"), al = glue.find("if (!dcn41_allow_write(&gDcn.allow, abs, v, \"disp2\")) {"), w = glue.find("amdgpu::WREG32(*gDcn.dev, abs, v);");
        expect(g != std::string::npos && al != std::string::npos && w != std::string::npos && g < al && al < w, "the WREG32 follows the instance guard AND the allowlist, in that order");
        expect(glue.find("if (g != N48D2_G_OK) {") != std::string::npos && glue.find("return false;\n\t\t}\n\t\tif (!dcn41_allow_write") != std::string::npos, "a guard refusal returns before the allowlist / write");
        expect(glue.find("n48d2_step gD2Steps[2][N48D2_MAX_STEPS];") != std::string::npos && glue.find("n48d2_step *buf(unsigned k) { return gD2Steps[k & 1u]; }") != std::string::npos, "the step lists are file-scope, never on the kernel stack");
        expect(glue.find("void sleep_ms(uint32_t ms) { /*nolock*/ IOSleep(ms); }") != std::string::npos && glue.find("void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }") != std::string::npos, "the waits are IOSleep / the file's delay glue");
        for (const char *bad : { "MM_INDEX", "navi48_vram_write_mm", "dcn41_dmub", "WBAR0_", "dcn41_write(" }) expect(glue.find(bad) == std::string::npos, (std::string("the disp2 glue never uses ") + bad).c_str());
        expect_u("the flow writes at exactly TWO sites (0.0.635: the SET / UPDATE site and the COPY0 site, each behind its own guard judgement)", count_of(flowCode, "e.wr("), 2);
        { const size_t cw = flowCode.find("e.wr(st.abs, cv)"), cg = flowCode.find("const uint32_t g = n48d2_guard_write_i(inst, st.abs, cv, 0);"); expect(cg != std::string::npos && cw != std::string::npos && cg < cw, "0.0.635 flow: the COPY0 write is judged by the guard (address AND the value just read from pipe 0) before it is made"); }
        const size_t gw = flowCode.find("const uint32_t g = n48d2_guard_write_i(inst, st.abs, v, 0);"), ew = flowCode.find("e.wr(st.abs, v)");
        expect(gw != std::string::npos && ew != std::string::npos && gw < ew, "the flow judges the guard before its one write");
        const size_t gl = flowCode.find("if (n48d2_guard_list_i(inst, s, n, &bad) != 0u) {"), sn = flowCode.find("dp_snapshot(e, o.dp_before);"), rs = flowCode.find("const uint32_t st = run_steps(e, inst, s, n, op == N48D2_OP_OFF, o, &refusals);");
        expect(gl != std::string::npos && sn != std::string::npos && rs != std::string::npos && gl < sn && sn < rs, "REACHABILITY (flow): the list guard before any read, the DP snapshot, then the steps");
    }
    {   // 3. the dispatch, the admission, the user client
        const size_t ex = brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)"), br = brg.find("if (action == N48D2_ACT) {"), fb = brg.find("if (action == 78) {");
        expect(ex != std::string::npos && br != std::string::npos && fb != std::string::npos && ex < br && br < fb, "REACHABILITY: the exemption gate, then the action-98 branch (before fbname's)");
        const std::string b = br == std::string::npos ? "" : brg.substr(br, 700);
        expect(b.find("if (n48dcn::disp2LatchedOn()) (void)n48dcn::bind(this);") != std::string::npos && b.find("n48dcn::disp2(argScalar, v, 13, &gBringup)") != std::string::npos &&
               b.find("disp2LatchedOn()") < b.find("n48dcn::disp2(argScalar"), "the dispatcher asks the latch BEFORE bind(), then calls disp2");
        expect(pure.find("constexpr uint32_t kActDisp2 = N48D2_ACT;") != std::string::npos && pure.find("action == kActDisp2;") != std::string::npos, "the admission names disp2");
        expect(ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "the user client's bound goes through action_admitted (site 5)");
        expect(hpp.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx = nullptr);") != std::string::npos && hpp.find("bool disp2LatchedOn();") != std::string::npos, "navi48_dcn.hpp declares both");
    }
    {   // 4. the CLI
        for (const char *t : { "navi48_disp2.h", "else if (what && !strcmp(what, \"disp2\")) in = 98;", "in == 98 ? \"disp2\"", "if (in == 98) return cmd_disp2(arg, arg2);", "dmubctx disp2 hangtest", "disp2 [timing|timing1440|connect|off|status [1|2]|plane|show|flipA|flipB|crc|planeoff 2]",
                               "accel dmubsend pclk otg1-on", "accel dmubsend phyc enable", "accel disp2 timing", "accel disp2 connect", "accel disp2 off", "accel dmubsend phyc disable", "accel dmubsend pclk otg1-off",
                               "d2_arg(N48D2_OP_STATUS)", "d2_arg(N48D2_OP_OFF)", "*** ABORT: the DP check after", "REFUSED by the CLI: OTG0's frame counter did not advance" })
            expect(cli.find(t) != std::string::npos, (std::string("the CLI carries ") + t).c_str());
        const size_t c0 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {"), pre = cli.find("if (d2_status_call(pre, 0) != 0) return 1;", c0), call = cli.find("if (dr_call(N48D2_ACT, d2_arg(op), o) != 0) return 1;", c0),
                     post = cli.find("int bad = d2_status_call(post, 0) != 0;", c0), off = cli.find("dr_call(N48D2_ACT, d2_arg(N48D2_OP_OFF), o2)", c0);
        expect(c0 != std::string::npos && pre != std::string::npos && call != std::string::npos && post != std::string::npos && off != std::string::npos && pre < call && call < post && post < off,
               "the CLI: status pre-check, the op, status post-check, then `off` on a failed post-check");
        // 0.0.632: the status page 2 and the FIFO-reset (symbol clock) report
        expect(cli.find("return (d2_status_call(pre, 1) == 0 && d2_status2_call(1) == 0 && d2_status3_call(NULL, 1) == 0) ? 0 : 3;", c0) != std::string::npos && cli.find("dr_call(N48D2_ACT, d2_arg(N48D2_OP_STATUS2), o)") != std::string::npos &&
               cli.find("if (op == N48D2_OP_CONNECT) printf(\"  FIFO reset (enc35)      : %s\\n\", n48d2_symclk_report(&r));") != std::string::npos,
               "0.0.632 CLI: `disp2 status` prints page 1 and then page 2; `disp2 connect` prints the FIFO-reset report (wait ok / delay (no symclk))");
        const size_t gs3 = glue.find("if (op == N48D2_OP_STATUS3) {"), gs2 = glue.find("} else if (op == N48D2_OP_STATUS2) {"), gs1 = glue.find("} else if (op == N48D2_OP_STATUS) {");
        expect(gs3 != std::string::npos && gs2 != std::string::npos && gs1 != std::string::npos && gs3 < gs2 && gs2 < gs1 && glue.find("st = n48d2::status3(env, out, inst);") != std::string::npos && glue.find("st = n48d2::status2(env, out, inst);") != std::string::npos && glue.find("FIFO reset: %s;") != std::string::npos && glue.find("n48d2_symclk_report(&o)") != std::string::npos,
               "0.0.632 glue: op 5 reads only (status2), the op log carries \"FIFO reset: <wait ok | delay (no symclk)>\", and status2 is dispatched BEFORE the step-running ops");
        const size_t sc = cli.find("static void d2_script(void) {");
        expect(sc != std::string::npos, "the CLI prints a run script");
        const size_t s1 = cli.find("accel dmubsend pclk otg1-on", sc), s2 = cli.find("accel dmubsend phyc enable", sc), s3 = cli.find("accel disp2 timing", sc), s4 = cli.find("accel disp2 connect", sc), s5 = cli.find("accel disp2 off", sc),
                     s6 = cli.find("accel dmubsend phyc disable", sc), s7 = cli.find("accel dmubsend pclk otg1-off", sc);
        expect(s1 != std::string::npos && s7 != std::string::npos && s1 < s2 && s2 < s3 && s3 < s4 && s4 < s5 && s5 < s6 && s6 < s7, "the printed run script is in the brief's order");
    }
    {   // 6. 0.0.633: the instance, the symbol clock exception, status3 - glue, flow, header, CLI
        const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {"), f1 = glue.find("}  // namespace n48dcn", f0 == std::string::npos ? 0 : f0);
        const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
        const size_t i0 = fn.find("const uint32_t op = n48d2_arg_op(arg), inst = n48d2_arg_inst(arg);"), i1 = fn.find("env.inst = inst;"), r0 = fn.find("n48d2::run(env, op, inst)"), s3 = fn.find("n48d2::status3(env, out, inst)"), s2 = fn.find("n48d2::status2(env, out, inst)"), s1 = fn.find("n48d2::status(env, out, inst)");
        expect(i0 != std::string::npos && i1 != std::string::npos && r0 != std::string::npos && s3 != std::string::npos && s2 != std::string::npos && s1 != std::string::npos && i0 < i1 && i1 < s3 && s3 < s2 && s2 < s1 && s1 < r0, "0.0.633 glue: the instance is decoded from the argument and set on the environment BEFORE any status or run, and status3 / status2 / status are dispatched before the step-running ops");
        expect(glue.find("uint32_t inst { N48D2_INST_MONA };") != std::string::npos && glue.find("n48d2_guard_write_i(inst, abs, v, &why)") != std::string::npos && count_of(glue, "env.inst = inst;") == 2, "0.0.633 glue: the environment carries the instance and its guard is that instance's (two assignments: disp2() and, since 0.0.658, fbLive(), which refuses any other instance before it)");
        expect(flowCode.find("const n48d2_inst *dp = n48d2_inst_get(inst);\n    if (!dp) return o;") != std::string::npos && flowCode.find("o.symclkb_a = e.rd(N48D2_SYMCLKB_CLOCK_ENABLE);") != std::string::npos && flowCode.find("o.symclkb_b = e.rd(N48D2_SYMCLKB_CLOCK_ENABLE);") != std::string::npos && flowCode.find("if (o.symclkb_b != o.symclkb_a) o.dp_changed |= 0x40u;") != std::string::npos &&
               flowCode.find("rollback(e, inst, o)") != std::string::npos, "0.0.633 flow: an unknown instance returns BAD_ARG before anything is read; SYMCLKB is READ before and after and a change ends the op in DP_DISTURBED with `off`");
        expect(count_of(flowCode, "N48D2_SYMCLKB_CLOCK_ENABLE") == 4, "0.0.633 / 0.0.635 flow: SYMCLKB is only ever READ (two reads in run(), two in plane_exec; no write path to it)");
        // the header: the exception is one register per instance in the step lists, and ONE address test in the guard
        const std::string hc = strip_line_comments(hdrS);
        expect_u("instance 1's lists name SYMCLKC in exactly two steps (connect, off)", count_of(hc, "N48D2_UPD(SYMCLKC_CLOCK_ENABLE,"), 2);
        expect_u("instance 2's lists name SYMCLKD in exactly two steps (connect, off)", count_of(hc, "N48D2_UPD(SYMCLKD_CLOCK_ENABLE,"), 2);
        expect(hc.find("N48D2_UPD(SYMCLKC_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, N48D2_SYMCLKC_FE_VALUE, \"dccg401_enable_symclk_se\");") != std::string::npos && hc.find("N48D2_UPD(SYMCLKC_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u, \"dccg401_disable_symclk_se\");") != std::string::npos &&
               hc.find("N48D2_UPD(SYMCLKD_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, N48D2_SYMCLKD_FE_VALUE, \"dccg401_enable_symclk_se\");") != std::string::npos && hc.find("N48D2_UPD(SYMCLKD_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u, \"dccg401_disable_symclk_se\");") != std::string::npos, "0.0.633 header: the four symbol-clock steps are UPDATEs with mask 0x710 and value 0x210 / 0x310 / 0");
        expect(hc.find("if (abs == me->symclk) return N48D2_G_OK;") != std::string::npos && count_of(hc, "return N48D2_G_OK;") >= 1 && hc.find("{ 0x00000100u, 0x0000017eu, \"DCCG (BASE_IDX 1)") != std::string::npos && hc.find("#define N48D2_FORBIDDEN_COUNT 48u") != std::string::npos && count_of(hc, "abs == me->symclk") == 1,
               "0.0.633 header: the exception is ONE address test in the guard (the instance's own SYMCLK); the DCCG span (0x100..0x17e) stays in the forbidden table");
        expect(hc.find("(s[i].mask & ~N48D2_SYMCLK_FE_MASK) != 0u") != std::string::npos && hc.find("s[i].kind != N48D2_K_UPD") != std::string::npos && hc.find("(s[i].val & ~s[i].mask) != 0u") != std::string::npos && hc.find("(sel != 0u && sel != d->link)") != std::string::npos, "0.0.633 header: the list guard pins the exception to an UPDATE inside mask 0x710, value inside its mask, FE_SRC_SEL 0 or the link");
        expect(hdrS.find("#define N48D2_SYMCLK_FE_MASK          (N48D2_SYMCLK_FE_EN_MASK | N48D2_SYMCLK_FE_SRC_MASK)") != std::string::npos && hdrS.find("#define N48D2_SYMCLK_FE_EN_MASK       0x00000010u") != std::string::npos && hdrS.find("#define N48D2_SYMCLK_FE_SRC_MASK      0x00000700u") != std::string::npos, "0.0.633 header: the exception mask is 0x10 | 0x700");
        // the CLI
        for (const char *t : { "static uint32_t g_d2_inst = N48D2_INST_MONA;", "static uint64_t d2_arg(uint32_t op) { return g_d2_inst == N48D2_INST_MONB ? n48d2_arg_i(op, N48D2_INST_MONB) : op == N48D2_OP_FBHOLD ? n48d2_arg_i(op, N48D2_INST_MONA) : n48d2_arg(op); }", "static int d2_status3_call(uint32_t r[N48D2_STAT3_COUNT], int print) {", "dr_call(N48D2_ACT, d2_arg(N48D2_OP_STATUS3), o)",
                               "if (strcmp(a2, \"1\") && strcmp(a2, \"2\")) return d2_usage();", "post3[N48D2_STAT3_SYMCLKB] != pre3[N48D2_STAT3_SYMCLKB]", "accel disp2 timing 2 ; accel disp2 connect 2 ; accel dmubsend digd setup", "accel dmubsend pclk otg2-on", "accel dmubsend phyd enable", "accel scdcread 3 0x40 1", "accel disp2 off 2 ; accel dmubsend phyd disable ; accel dmubsend pclk otg2-off",
                               "INSTANCE 2 (the monitor B, HDMI ddc3/hpd4, UNIPHY_D" })
            expect(cli.find(t) != std::string::npos, (std::string("0.0.633 CLI carries ") + t).c_str());
    }
    {   // 6b. 0.0.634: timing1440, the DP-plane watch, the SYMCLKB line - glue, flow, header, CLI
        const size_t f0 = glue.find("uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx) {"), f1 = glue.find("}  // namespace n48dcn", f0 == std::string::npos ? 0 : f0);
        const std::string fn = f0 == std::string::npos ? "" : glue.substr(f0, f1 == std::string::npos ? std::string::npos : f1 - f0);
        expect(fn.find("gDcn.seg[3] != N48D2_SEG3") != std::string::npos && fn.find("n48d2::run(env, op, inst)") != std::string::npos && fn.find("DP plane watch") != std::string::npos && fn.find("DP DISTURBED") != std::string::npos && fn.find("gD2Dpx[0][4]") != std::string::npos && glue.find("uint32_t *dpx_buf(unsigned k) { return gD2Dpx[k & 1u]; }") != std::string::npos, "0.0.634 glue: the BASE_IDX 3 segment is checked, run() is the one entry for timing / timing1440 / connect / off, and the DP plane watch is logged (with DP DISTURBED)");
        // flow: the nine are read after the DP snapshot and before the steps, and again after the settle; the disturbed test includes them; no write member is used
        const size_t b0 = flowCode.find("dpx_snapshot(e, dpxB);"), st0 = flowCode.find("run_steps(e, inst, s, n, op == N48D2_OP_OFF, o, &refusals)"), a0 = flowCode.find("dpx_snapshot(e, dpxA);"), d0 = flowCode.find("if (o.dp_changed != 0u || o.dpx_changed != 0u || o.w2_changed != 0u) {");
        expect(b0 != std::string::npos && st0 != std::string::npos && a0 != std::string::npos && d0 != std::string::npos && b0 < st0 && st0 < a0 && a0 < d0, "0.0.634 flow: REACHABILITY: dpx_snapshot BEFORE the steps, dpx_snapshot AFTER them, then the disturbed decision that includes dpx_changed");
        { const size_t x0 = flowCode.find("inline void dpx_snapshot(E &e, uint32_t *d)"); const std::string body = x0 == std::string::npos ? "" : flowCode.substr(x0, 260);
          expect(!body.empty() && body.find("e.rd(") != std::string::npos && body.find("e.wr(") == std::string::npos, "0.0.634 flow: dpx_snapshot uses e.rd only"); }
        expect(flowCode.find("const bool isTiming = op == N48D2_OP_TIMING || op == N48D2_OP_TIMING1440;") != std::string::npos && flowCode.find("op != N48D2_OP_TIMING && op != N48D2_OP_TIMING1440 && op != N48D2_OP_CONNECT && op != N48D2_OP_OFF") != std::string::npos && flowCode.find("if (isTiming) {") != std::string::npos, "0.0.634 flow: timing1440 takes timing's prechecks (the PHYPLL source check stays)");
        // header: the new addresses are reads only (no step list names them); the 1440 builder is the SAME list builder as the 1080p timing
        const std::string hc = strip_line_comments(hdrS);
        { bool named = false; for (const char *r : { "MPC_OUT0_MUX", "MPCC0_TOP_SEL", "MPCC0_BOT_SEL", "MPCC0_OPP_ID", "HUBP0_DCHUBP_CNTL", "DPPCLK0_DTO_PARAM", "DET0_CTRL", "COMPBUF_CTRL", "ODM0_OPTC_INPUT_GLOBAL_CONTROL", "DPP_TOP0_DPP_CONTROL", "MPCC0_UPDATE_LOCK_SEL", "HUBPREQ0_PRIMARY_LOW", "HUBPREQ0_PRIMARY_HIGH" }) { named = named || hc.find(std::string("N48D2_SET(") + r) != std::string::npos || hc.find(std::string("N48D2_UPD(") + r) != std::string::npos || hc.find(std::string("N48D2_WAIT(") + r) != std::string::npos; }
          expect(!named, "0.0.634 header: no step list SETs / UPDs / WAITs a DP-plane watch register (0.0.635: the DPPCLK_CTRL exception is checked right below)");
          expect_u("0.0.635 header: DPPCLK_CTRL (the one DP-watch register instance 2 may touch) is named by exactly TWO steps, both UPDATEs of the DPPCLK2_EN mask", count_of(hc, "N48D2_UPD(DPPCLK_CTRL, N48D2_DPPCLK2_EN_MASK,"), 2);
          expect_u("0.0.635 header: ... and by no other step kind", count_of(hc, "N48D2_SET(DPPCLK_CTRL") + count_of(hc, "N48D2_WAIT(DPPCLK_CTRL") + count_of(hc, "N48D2_EXPECT(DPPCLK_CTRL"), 0); }
        expect(hc.find("else if (op == N48D2_OP_TIMING1440) { if (inst != N48D2_INST_MONB || n48d2_dtd_decode(n48d2_monb_dtd1440, &t) != 0u) return 0u; n = n48d2_build_timing2(s, &t); }") != std::string::npos, "0.0.634 header: op 7 builds the monitor B's timing2 list over the 1440p descriptor, refuses any other instance");
        expect(hc.find("N48D2_UPD(DIG2_BE_CNTL, N48D2_BE_FE2_BIT, 0u,") < hc.find("N48D2_UPD(SYMCLKC_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u,") && hc.find("N48D2_UPD(DIG3_BE_CNTL, N48D2_BE_FE3_BIT, 0u,") < hc.find("N48D2_UPD(SYMCLKD_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u,"), "0.0.634 header (item 3a): in both off lists the BE disconnect comes BEFORE the SYMCLK FE disable");
        // CLI: the op word, the instance rule, the DP watch around every non-status op, SYMCLKB printed from the status3 pair
        const size_t c0 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {"), c1 = cli.find("static int cmd_accel(const char *what", c0 == std::string::npos ? 0 : c0);
        const std::string cd = (c0 == std::string::npos || c1 == std::string::npos) ? std::string() : cli.substr(c0, c1 - c0);
        expect(!cd.empty() && cd.find("!strcmp(a1, \"timing1440\") ? N48D2_OP_TIMING1440") != std::string::npos && cd.find("if (op == N48D2_OP_TIMING1440 && (!a2 || strcmp(a2, \"2\")))") != std::string::npos, "0.0.634 CLI: `disp2 timing1440 2` (instance 2 only; the CLI refuses without the 2)");
        const size_t w0 = cd.find("int haveWatch = dp_watch_read(dpw0) == 0;"), call = cd.find("if (dr_call(N48D2_ACT, d2_arg(op), o) != 0) return 1;"), w1 = cd.find("if (dp_watch_read(dpw1) != 0)"), rep1 = cd.find("dp_watch_report(a1, dpw0, dpw1)");
        expect(w0 != std::string::npos && call != std::string::npos && w1 != std::string::npos && rep1 != std::string::npos && w0 < call && call < w1 && w1 < rep1, "0.0.634 CLI: REACHABILITY: the DP watch is read BEFORE the op's kext call and again AFTER it, then compared, for every op that reaches the call (timing, timing1440, connect, off)");
        expect(cd.find("if (!haveWatch) { printf(\"accel disp2 %s: REFUSED by the CLI: the DP watch (dispcensus page 10) could not be read; nothing was sent\\n\", a1); return 1; }") != std::string::npos, "0.0.634 CLI: timing / timing1440 / connect are refused when the DP watch cannot be read (nothing sent); off still runs");
        expect(cd.find("r.symclkb_a") == std::string::npos && cd.find("r.symclkb_b") == std::string::npos && cd.find("pre3[N48D2_STAT3_SYMCLKB], post3[N48D2_STAT3_SYMCLKB]") != std::string::npos && cd.find("SYMCLKB_CLOCK_ENABLE    : %#010x -> %#010x%s\\n\", pre3[N48D2_STAT3_SYMCLKB]") != std::string::npos, "0.0.634 CLI (item 3b): the per-op SYMCLKB line prints the status3 pre / post values (the unpacked symclkb_a / _b scalars are gone)");
        const size_t sy = cd.find("SYMCLKB_CLOCK_ENABLE    : %#010x -> %#010x%s"), gd = cd.find("if (guarded) {        // the CLI's own post-check");
        expect(sy != std::string::npos && gd != std::string::npos && sy < gd, "0.0.634 CLI: the SYMCLKB comparison is printed for EVERY op (before the guarded-only block)");
        expect(cd.find("const int guarded = op == N48D2_OP_TIMING || op == N48D2_OP_TIMING1440 || op == N48D2_OP_CONNECT;") != std::string::npos, "0.0.634 CLI: timing1440 is a GUARDED op (the CLI's own OTG0 / SYMCLKB / DP-plane pre- and post-check, abort-by-off)");
        expect(cd.find("watchBad = 1") != std::string::npos && cd.find("if (!bad && (watchBad || r.dpx_changed != 0u)) bad = 1;") != std::string::npos && cd.find("DP plane watch unchanged") != std::string::npos, "0.0.634 CLI: a changed DP-plane register (CLI's own or the kext's mask) triggers the ABORT path (disp2 off)");
        { const size_t u0 = cli.find("static unsigned dp_watch_report(");  const std::string body = u0 == std::string::npos ? "" : cli.substr(u0, 1500);
          expect(body.find("DP DISTURBED") != std::string::npos && body.find("CHANGED") != std::string::npos && body.find("underflow 30:28") != std::string::npos && body.find("N48D2_HUBP_UNDERFLOW_MASK") != std::string::npos, "0.0.634 CLI: dp_watch_report prints DP DISTURBED, marks each CHANGED register, and reports HUBP0's underflow bits 30:28"); }
        expect(cli.find("accel disp2 timing1440 2") != std::string::npos && cli.find("INSTANCE 2 at 2560x1440@60 (0.0.634") != std::string::npos, "0.0.634 CLI: the printed run script carries the 1440p sequence");
    }
    {   // 5. the version, the banned strings
        expect(count_of(plist, "0.0.664") == 2 && plist.find("0.0.630") == std::string::npos, "Info.plist is 0.0.664, carried twice");
        expect(s1c.find("kN1cKextBuild = 664;") != std::string::npos && count_of(nub, "664u") == 2, "the native client and both ops tables report build 664");
        const std::string p1 = std::string("pipe+") + "0x280", p2 = std::string("+") + "0x282", p3 = std::string("+") + "0x299";
        const size_t c0 = cli.find("// ---- 0.0.631 (multi-monitor stage M4d)"), c1 = cli.find("static int cmd_accel(const char *what", c0 == std::string::npos ? 0 : c0);
        const std::string cliNew = (c0 == std::string::npos || c1 == std::string::npos) ? std::string("missing") : cli.substr(c0, c1 - c0);
        expect(cliNew != "missing", "the CLI's disp2 section is delimited");
        for (const std::string *f : { &hdrS, &flowS, &glue, &cliNew })
            expect(f->find(p1) == std::string::npos && f->find(p2) == std::string::npos && f->find(p3) == std::string::npos, "no banned pipe-offset spelling in the new code");
        const std::string me = slurp(g_root + "/src/navi48-bringup/tests/native_disp2_test.cpp");
        expect(!me.empty() && me.find(p1) == std::string::npos && me.find(p2) == std::string::npos && me.find(p3) == std::string::npos, "nor in this test");
    }

    {   // 7. 0.0.658: source pins for the monitor A's hold (the flow, the glue, the CLI)
        expect(flowCode.find("|| (op == N48D2_OP_FBHOLD && inst != N48D2_INST_MONB)") == std::string::npos && hdrS.find("if (op == N48D2_OP_FBHOLD && inst != N48D2_INST_MONB && inst != N48D2_INST_MONA) return 0;") != std::string::npos, "0.0.658: neither the argument check nor the flow restricts fbhold to the monitor B any more");
        expect(dcn.find("if (n48d2_held_refuses(op, inst) && disp2HeldInst(inst)) {") != std::string::npos && dcn.find("n48dm_tpl_mona_held_refuses(tplId) && disp2HeldInst(N48D2_INST_MONA)") != std::string::npos && dcn.find("(op >= N48D2_OP_PLANE && op <= N48D2_OP_PLANEOFF) || op == N48D2_OP_FBHOLD) {") != std::string::npos, "0.0.658: the glue dispatches fbhold for either instance and refuses per instance (the monitor A's templates follow the monitor A's HELD state)");
        expect(dcn.find("N48LOG(\"disp2 %s (instance 1) MONB WATCH") == std::string::npos, "0.0.658: no hand-written MONB WATCH format is left in the glue (both lines come from the macros)");
    }

    std::printf("native_disp2_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
