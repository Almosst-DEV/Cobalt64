// native_dispread_test.cpp - build 0.0.622 (multi-monitor track, stage M1: READ-ONLY instruments; an internal design note). It compiles the REAL sequences of
// dcn/navi48_dispread_flow.h and the REAL decisions of dcn/navi48_dispread.h and drives them against a model of the DC_I2C engine (a register file with an arbiter, a FIFO and a DDC bus
// holding four EDID blocks), a model of the DMUB inbox ring in BAR0-visible memory, and a census register file. The kext glue (dcn/navi48_dcn.cpp, Navi48Bringup.cpp) is checked by source pins.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn -I src/navi48-bringup/src/amd -I src/dcn41 \
//       src/navi48-bringup/tests/native_dispread_test.cpp -x c++ src/dcn41/dcn41_allow.c src/dcn41/dcn41_dmub.c src/dcn41/dcn41_core.c -o /tmp/native_dispread
//   /tmp/native_dispread .       (run from the repo root; the argument is the repo root the source pins read from; N48_LINUX_DC = the tree holding re/linux-dc, default the root and then the main checkout)
//   tests/native_dispread_plant.sh plants breaks in the REAL code and demands a failure for each.
// Covers:
//   A  the plan: line / block validation (exhaustive 0..255 x 0..255) and the EXACT transaction list, FIFO and register values of every legal (line, block); the bounded poll;
//   B  every register offset and every field bit against Linux's dcn_4_1_0_offset.h / dcn_4_1_0_sh_mask.h; every register the flow writes inside the DCN write allowlist (the real dcn41_allow code), and its range;
//   C  the flow over the engine model: all blocks of both lines return the model's bytes; the ONLY bus writes are the offset byte (and the segment pointer for block >= 2); arbitration refusals (hardware owner,
//      software owner, no response) write NOTHING; not granted withdraws only; NACK / timeout / abort / overflow / refused write each end distinctly and ALWAYS release; the poll bound; no GO after a refused write;
//   D  the EDID checksum (every byte position) and header;
//   E  DMUB: header decode, names (dcn41_dmub.h's numbers; 6/10/12/16 sub-types print unknown), the ring count (25), BAR0 reachability for this card's real numbers (unreachable) and for a reachable ring, the page
//      verdicts, the ring reader reads only [off, off + 36) of its command through the BAR0 reader, the packers;
//   F  the census table (40 registers, 24 per page), the page reads, the packing; (0.0.628, M4a) the second table n48dr_census2 (pages 2..7: DCCG clock sources, DIG0..3, RDPCSTX1/2): every register resolves in
//      Linux's header and through the REAL dcn41_abs_rd, none outside the display blocks, the read-only address twin (BASE_IDX 1 / 3 map for reads, dcn41_abs still refuses them), the glue's call sites;
//   G  (0.0.624, stage M1.5) region4read: the range rule (exhaustive over offsets and dwords against an independent reference, incl. the 0x10000 edge and 64-bit wrap), the argument, the window base from register
//      values (measured card, refusals: disabled / below the FB base / unaligned / outside VRAM / zero, other FB bases so a hard-coded base fails), the flow over a memory model (exactly ONE read at base + offset, refusals
//      read nothing, a failed read is its own status), the page layout round trip;
//   S  source pins: dispatch order, bind only for ddcread, the single register write behind the allowlist, no MM_INDEX / DMUB command / GPINT / ring submit in the new glue, the exemption and the bound, the CLI, the version, the project rule.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "navi48_dispread_flow.h"
#include "amd/native_disp_pure.h"
extern "C" {
#include "dcn41_allow.h"
#include "dcn41_dmub.h"
#include "dcn41.h"
}

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { ++gRun; if (!ok) { ++gFail; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) { ++gRun; if (got != want) { ++gFail; std::printf("FAIL: %s: got %llu (%#llx), want %llu (%#llx)\n", what, (unsigned long long)got, (unsigned long long)got, (unsigned long long)want, (unsigned long long)want); } }
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::string strip_line_comments(const std::string &s) {   // code only: pins about what the code USES must not be fooled (or tripped) by prose
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

static const uint32_t kSeg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u };   // this card's own DMU bases (notes/logs/runs/adopt1/driverlog-post.txt:1371)

// ---- the Linux register headers -----------------------------------------------------------------------------------------------------------------------------------------------------------------
static std::string linux_hdr(const char *name) {
    std::vector<std::string> roots;
    if (const char *e = std::getenv("N48_LINUX_DC")) roots.push_back(e);
    roots.push_back(g_root);
    roots.push_back("/path/to/navi48-checkout");
    for (auto &r : roots) {
        std::string s = slurp(r + "/re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/" + name);
        if (!s.empty()) return s;
    }
    return "";
}
static bool linux_reg(const std::string &h, const std::string &reg, uint32_t *off, uint32_t *base) {
    const std::string key = "#define reg" + reg + " ";
    size_t p = h.find(key); if (p == std::string::npos) return false;
    // exact name: the character before must be a newline
    if (p > 0 && h[p - 1] != '\n') return false;
    *off = (uint32_t)std::strtoul(h.c_str() + p + key.size(), nullptr, 0);
    const std::string bk = "#define reg" + reg + "_BASE_IDX";
    size_t q = h.find(bk); if (q == std::string::npos) return false;
    q += bk.size(); while (h[q] == ' ' || h[q] == '\t') ++q;
    *base = (uint32_t)std::strtoul(h.c_str() + q, nullptr, 0);
    return true;
}
static void expect_reg(const std::string &h, const char *reg, uint32_t off, uint32_t base) {
    uint32_t o = 0, b = 0;
    bool f = linux_reg(h, reg, &o, &b);
    ++gRun; if (!f || o != off || b != base) { ++gFail; std::printf("FAIL: register %s: ours %#x BASE_IDX %u, Linux %s %#x BASE_IDX %u\n", reg, off, base, f ? "has" : "NOT FOUND", o, b); }
}

// ---- the DC_I2C engine model ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct BusByte { uint8_t addr8; bool isWrite; uint8_t value; };
struct FakeI2c {
    // behaviour knobs
    int ownerAtStart = 0;               // 0 idle, 1 software, 2 hardware
    bool allOnes = false;               // the engine does not respond: every read is all ones
    bool ignoreRequest = false;         // SW_USE_I2C_REG_REQ is never granted
    bool hwGrabsAfterWake = false;      // the arbiter hands the engine to hardware once the wake writes happened
    uint32_t errBits = 0;               // SW_STATUS bits to raise instead of DONE
    bool neverDone = false;             // GO accepted, nothing ever completes
    // state
    int owner = 0;
    bool swReq = false;
    uint32_t control = 0, swStatus = 0, memPwr = 0;
    uint32_t setup[8] = {}, speed[8] = {}, txn[4] = {};
    std::vector<uint8_t> fifo; uint32_t fifoIdx = 0; bool readMode = false;
    uint32_t goCount = 0;
    int wakeSeen = 0;
    // the bus: four EDID blocks behind address 0x50, segment pointer behind 0x30
    uint8_t edid[4][128];
    uint8_t segment = 0, pointer = 0;
    uint8_t scdc[256]; uint8_t scdcPtr = 0;      // 0.0.633: the SCDC slave behind bus address 0x54 (bus bytes 0xA8 / 0xA9)
    std::vector<BusByte> bus;
    std::vector<uint32_t> softResets;   // writes carrying SOFT_RESET
    FakeI2c() {
        for (int b = 0; b < 4; b++) {
            uint32_t sum = 0;
            for (int i = 0; i < 127; i++) { edid[b][i] = (uint8_t)(0x11 * (b + 1) + 7 * i + (i >> 3)); sum += edid[b][i]; }
            if (b == 0) { const uint8_t h[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 }; sum = 0; for (int i = 0; i < 8; i++) edid[0][i] = h[i]; for (int i = 0; i < 127; i++) sum += edid[0][i]; }
            edid[b][127] = (uint8_t)(0x100 - (sum & 0xff));
        }
        for (int i = 0; i < 256; i++) scdc[i] = (uint8_t)(0xC0 ^ i);
        scdc[0x01] = 0x01; scdc[0x20] = 0x03; scdc[0x21] = 0x01; scdc[0x40] = 0x0F;
        scdc[0x50] = 0x05; scdc[0x51] = 0x80; scdc[0x52] = 0x00; scdc[0x53] = 0x00; scdc[0x54] = 0x34; scdc[0x55] = 0x92; scdc[0x56] = 0x7B;
    }
    uint32_t arbWord() const { return ((uint32_t)owner << 2) | (swReq ? N48DR_ARB_SW_USE_REQ : 0u); }
    uint32_t rd(uint32_t baseIdx, uint32_t off) {
        if (allOnes) return 0xFFFFFFFFu;
        if (baseIdx == 1u) return off == N48DR_MICROSECOND_TIME_BASE_DIV ? 0x00000064u : 0u;
        if (off == N48DR_I2C_ARBITRATION) { if (hwGrabsAfterWake && wakeSeen >= 2) owner = 2; return arbWord(); }
        if (off == N48DR_I2C_SW_STATUS) return swStatus;
        if (off == N48DR_I2C_CONTROL) return control;
        if (off == N48DR_DIO_MEM_PWR_CTRL) return memPwr;
        if (off == N48DR_DIO_MEM_PWR_STATUS) return 0;
        if (off >= N48DR_I2C_DDC1_SPEED && off < N48DR_I2C_DDC1_SPEED + 12u) return ((off - N48DR_I2C_DDC1_SPEED) & 1u) ? setup[(off - N48DR_I2C_DDC1_SPEED) / 2u] : speed[(off - N48DR_I2C_DDC1_SPEED) / 2u];
        if (off == N48DR_I2C_DATA) { uint8_t b = fifoIdx < fifo.size() ? fifo[fifoIdx] : 0xEE; fifoIdx++; return (uint32_t)b << N48DR_DATA_SHIFT; }
        return 0u;
    }
    void run() {
        const uint32_t nt = ((control >> N48DR_CTL_TXN_COUNT_SHIFT) & 3u) + 1u;
        uint32_t pos = 0;
        std::vector<uint8_t> reply;
        for (uint32_t t = 0; t < nt; t++) {
            const uint32_t reg = txn[t], cnt = (reg >> N48DR_TXN_COUNT_SHIFT) & 0x3FFu;
            const uint8_t a = pos < fifo.size() ? fifo[pos] : 0; pos++;
            if (reg & N48DR_TXN_READ) {
                for (uint32_t k = 0; k < cnt; k++) { uint8_t v = a == 0xA9 ? scdc[(uint8_t)(scdcPtr + k)] : edid[(segment * 2 + pointer / 128) & 3][(pointer + k) & 127]; bus.push_back({ a, false, v }); reply.push_back(v); }
            } else {
                for (uint32_t k = 0; k < cnt; k++) { uint8_t v = pos < fifo.size() ? fifo[pos] : 0; pos++; bus.push_back({ a, true, v }); if (a == 0x60) segment = v; if (a == 0xA0) pointer = v; if (a == 0xA8) scdcPtr = v; }
            }
        }
        // the reply lands in the FIFO right after the bytes that were pushed
        fifo.resize(pos); for (uint8_t v : reply) fifo.push_back(v);
    }
    void wr2(uint32_t off, uint32_t v) {
        if (allOnes) return;
        if (off == N48DR_I2C_CONTROL) {
            if (v & N48DR_CTL_SOFT_RESET) { softResets.push_back(v); swStatus = 0; control = v & ~N48DR_CTL_SOFT_RESET; return; }
            if (v & N48DR_CTL_SW_STATUS_RESET) swStatus = 0;
            const bool go = (v & N48DR_CTL_GO) != 0;
            control = v & ~N48DR_CTL_SW_STATUS_RESET;
            if (!go && v == 0u) wakeSeen++;
            if (go) {
                goCount++;
                if (owner != 1) return;
                if (neverDone) return;
                if (errBits) { swStatus = errBits; return; }
                run(); swStatus = N48DR_ST_DONE;
            }
            return;
        }
        if (off == N48DR_I2C_ARBITRATION) {
            if ((v & N48DR_ARB_SW_USE_REQ) && owner == 0 && !ignoreRequest) { owner = 1; swReq = true; }
            if (v & N48DR_ARB_SW_DONE_USING) { if (owner == 1) owner = 0; swReq = false; }
            return;
        }
        if (off == N48DR_DIO_MEM_PWR_CTRL) { memPwr = v; return; }
        if (off >= N48DR_I2C_DDC1_SPEED && off < N48DR_I2C_DDC1_SPEED + 12u) { if ((off - N48DR_I2C_DDC1_SPEED) & 1u) { setup[(off - N48DR_I2C_DDC1_SPEED) / 2u] = v; wakeSeen++; } else speed[(off - N48DR_I2C_DDC1_SPEED) / 2u] = v; return; }
        if (off >= N48DR_I2C_TRANSACTION0 && off < N48DR_I2C_TRANSACTION0 + 4u) { txn[off - N48DR_I2C_TRANSACTION0] = v; return; }
        if (off == N48DR_I2C_DATA) {
            if (v & N48DR_DATA_INDEX_WRITE) { fifoIdx = (v >> N48DR_DATA_INDEX_SHIFT) & 0x3FFu; readMode = (v & N48DR_DATA_READ) != 0; if (!readMode && fifoIdx == 0) fifo.clear(); if (readMode) return; }
            if (!readMode) { if (fifo.size() <= fifoIdx) fifo.resize(fifoIdx + 1); fifo[fifoIdx++] = (uint8_t)((v >> N48DR_DATA_SHIFT) & 0xFFu); }
            return;
        }
    }
};
struct TestEnv {
    FakeI2c &hw;
    std::vector<std::pair<uint32_t, uint32_t>> writes;     // every write the flow made (offset, value)
    std::set<uint32_t> refuse;                              // offsets the "allowlist" refuses
    bool refusedFlag = false;
    uint64_t usec = 0; uint32_t statusReads = 0; uint32_t goWrites = 0;
    explicit TestEnv(FakeI2c &h) : hw(h) {}
    uint32_t rd(uint32_t b, uint32_t o) { if (b == 2u && o == N48DR_I2C_SW_STATUS) statusReads++; return hw.rd(b, o); }
    void wr2(uint32_t o, uint32_t v) {
        if (refuse.count(o)) { refusedFlag = true; return; }
        writes.push_back({ o, v });
        if (o == N48DR_I2C_CONTROL && (v & N48DR_CTL_GO) && !(v & N48DR_CTL_SOFT_RESET)) goWrites++;
        hw.wr2(o, v);
    }
    bool refused() const { return refusedFlag; }
    void delay_us(uint32_t us) { usec += us; }
};
static bool wrote(const TestEnv &e, uint32_t off) { for (auto &w : e.writes) if (w.first == off) return true; return false; }
static bool allowed_abs(uint32_t abs) { struct dcn41_allow_state al; std::memset(&al, 0, sizeof(al)); (void)dcn41_allow_init(&al, kSeg, 262144u); return dcn41_allow_write(&al, abs, 0u, "t"); }
static std::set<uint32_t> gAllWritten;     // every offset any scenario wrote: section B checks them against the real allowlist

static void a_plan() {
    // exhaustive validation
    uint32_t okCount = 0;
    for (uint32_t line = 0; line < 256; line++) for (uint32_t block = 0; block < 256; block++) {
        n48dr_plan p; const uint32_t rc = n48dr_plan_ddc(line, block, &p);
        const bool want = (line == 2 || line == 3) && block <= 3;
        if ((rc == 0) != want) { expect(false, "plan: accepted/refused exactly lines 2 and 3, blocks 0..3"); return; }
        if (rc == 0) okCount++; else if (p.ntxn != 0 || p.fifo_len != 0) { expect(false, "plan: a refusal leaves an empty plan"); return; }
    }
    expect_u("plan: 8 legal (line, block) pairs", okCount, 8);
    expect(!n48dr_ddc_line_ok(0) && !n48dr_ddc_line_ok(1) && n48dr_ddc_line_ok(2) && n48dr_ddc_line_ok(3) && !n48dr_ddc_line_ok(4), "lines 0 and 1 (DP/AUX-class) are refused, 2 and 3 accepted");
    n48dr_plan p;
    for (uint32_t line = 2; line <= 3; line++) {
        expect_u("plan block 0", n48dr_plan_ddc(line, 0, &p), 0);
        expect(p.ntxn == 2 && !p.has_seg && p.t[0].addr8 == 0xA0 && p.t[0].wlen == 1 && p.t[0].wdata == 0x00 && !p.t[0].is_read && p.t[1].addr8 == 0xA1 && p.t[1].is_read && p.t[1].rlen == 128 && p.t[1].stop && !p.t[0].stop,
               "block 0: [W 0xA0 offset 0x00] [R 0xA1 128 STOP]");
        expect(p.fifo_len == 3 && p.fifo[0] == 0xA0 && p.fifo[1] == 0x00 && p.fifo[2] == 0xA1 && p.read_index == 3, "block 0: FIFO a0 00 a1, reply at index 3 (RDNA4FB's)");
        expect_u("block 0: transaction 0 register (STOP_ON_NACK | START | 1 byte)", n48dr_txn_reg(&p.t[0]), 0x100u | 0x1000u | (1u << 16));
        expect_u("block 0: transaction 1 register (STOP_ON_NACK | START | READ | STOP | 128)", n48dr_txn_reg(&p.t[1]), 0x100u | 0x1000u | 1u | 0x2000u | (128u << 16));
        expect_u("block 0: poll bound = 2 transactions x 2000 polls x 10 us = 40 ms", n48dr_poll_bound(&p), 4000);
        expect_u("plan block 1", n48dr_plan_ddc(line, 1, &p), 0);
        expect(p.ntxn == 2 && !p.has_seg && p.t[0].wdata == 0x80 && p.fifo_len == 3 && p.fifo[1] == 0x80, "block 1: offset 0x80 and NO segment pointer");
        expect_u("plan block 2", n48dr_plan_ddc(line, 2, &p), 0);
        expect(p.ntxn == 3 && p.has_seg && p.seg == 1 && p.t[0].addr8 == 0x60 && p.t[0].wlen == 1 && p.t[0].wdata == 1 && p.t[1].addr8 == 0xA0 && p.t[1].wdata == 0x00 && p.t[2].addr8 == 0xA1 && p.t[2].rlen == 128,
               "block 2: [W 0x60 segment 1] [W 0xA0 offset 0x00] [R 0xA1 128 STOP]");
        expect(p.fifo_len == 5 && p.fifo[0] == 0x60 && p.fifo[1] == 0x01 && p.fifo[2] == 0xA0 && p.fifo[3] == 0x00 && p.fifo[4] == 0xA1 && p.read_index == 5, "block 2: FIFO 60 01 a0 00 a1, reply at index 5");
        expect_u("block 2: poll bound = 3 x 20 ms", n48dr_poll_bound(&p), 6000);
        expect_u("plan block 3", n48dr_plan_ddc(line, 3, &p), 0);
        expect(p.ntxn == 3 && p.seg == 1 && p.t[0].wdata == 1 && p.t[1].wdata == 0x80, "block 3: segment 1, offset 0x80");
    }
    // the poll bound is 20 ms per transaction exactly
    expect_u("poll step x polls per transaction = 20 ms", (uint64_t)N48DR_DDC_POLL_STEP_US * N48DR_DDC_POLL_PER_TXN, 20000);
    // the arguments
    expect(n48dr_ddc_arg_ok(n48dr_ddc_arg(2, 0, 0)) && n48dr_ddc_arg_ok(n48dr_ddc_arg(3, 3, 1)) && !n48dr_ddc_arg_ok(n48dr_ddc_arg(1, 0, 0)) && !n48dr_ddc_arg_ok(n48dr_ddc_arg(2, 4, 0)) && !n48dr_ddc_arg_ok(n48dr_ddc_arg(2, 0, 2)) &&
           !n48dr_ddc_arg_ok(0) && !n48dr_ddc_arg_ok(1ull << 24) && !n48dr_ddc_arg_ok(~0ull), "ddcread arguments: block | line << 8 | page << 16, nothing else");
    // statuses: distinct, named
    std::set<std::string> names; for (uint32_t s = 0; s < N48DR_STATUS_COUNT; s++) names.insert(n48dr_status_name(s));
    expect_u("every status has its own name", names.size(), N48DR_STATUS_COUNT);
    expect(std::string(n48dr_status_name(N48DR_STATUS_COUNT)) == "unknown", "a status past the table is 'unknown'");
}

static void b_registers() {
    const std::string h = linux_hdr("dcn_4_1_0_offset.h"), m = linux_hdr("dcn_4_1_0_sh_mask.h");
    expect(!h.empty() && !m.empty(), "re/linux-dc dcn_4_1_0_offset.h and dcn_4_1_0_sh_mask.h are found (set N48_LINUX_DC)");
    if (h.empty()) return;
    expect_reg(h, "DC_I2C_CONTROL", N48DR_I2C_CONTROL, 2);
    expect_reg(h, "DC_I2C_ARBITRATION", N48DR_I2C_ARBITRATION, 2);
    expect_reg(h, "DC_I2C_SW_STATUS", N48DR_I2C_SW_STATUS, 2);
    expect_reg(h, "DC_I2C_DDC1_SPEED", N48DR_I2C_DDC1_SPEED, 2);
    expect_reg(h, "DC_I2C_DDC1_SETUP", N48DR_I2C_DDC1_SETUP, 2);
    for (uint32_t line = 2; line <= 3; line++) {      // line n here = DDC(n+1): the per-line SPEED / SETUP pairs are 2 apart
        expect_reg(h, ("DC_I2C_DDC" + std::to_string(line + 1) + "_SPEED").c_str(), N48DR_I2C_DDC1_SPEED + 2 * line, 2);
        expect_reg(h, ("DC_I2C_DDC" + std::to_string(line + 1) + "_SETUP").c_str(), N48DR_I2C_DDC1_SETUP + 2 * line, 2);
    }
    expect_reg(h, "DC_I2C_TRANSACTION0", N48DR_I2C_TRANSACTION0, 2);
    expect_reg(h, "DC_I2C_TRANSACTION1", N48DR_I2C_TRANSACTION0 + 1, 2);
    expect_reg(h, "DC_I2C_TRANSACTION2", N48DR_I2C_TRANSACTION0 + 2, 2);
    expect_reg(h, "DC_I2C_DATA", N48DR_I2C_DATA, 2);
    expect_reg(h, "DIO_MEM_PWR_STATUS", N48DR_DIO_MEM_PWR_STATUS, 2);
    expect_reg(h, "DIO_MEM_PWR_CTRL", N48DR_DIO_MEM_PWR_CTRL, 2);
    expect_reg(h, "MICROSECOND_TIME_BASE_DIV", N48DR_MICROSECOND_TIME_BASE_DIV, 1);
    for (uint32_t i = 0; i < N48DR_DMUB_SCRATCH_COUNT; i++) expect_reg(h, ("DMCUB_SCRATCH" + std::to_string(i)).c_str(), N48DR_DMUB_SCRATCH_FIRST + i, 2);
    // the census table, entry by entry
    for (uint32_t i = 0; i < N48DR_CENSUS_COUNT; i++) expect_reg(h, n48dr_census[i].name, n48dr_census[i].off, n48dr_census[i].base_idx);
    for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) { if (!n48dr_census2[i].name) { expect(false, "census2: a table slot has no name (a register was dropped)"); continue; } expect_reg(h, n48dr_census2[i].name, n48dr_census2[i].off, n48dr_census2[i].base_idx); }   // 0.0.628: the M4a table, entry by entry
    for (uint32_t i = 0; i < N48DR_CENSUS3_COUNT; i++) { if (!n48dr_census3[i].name) { expect(false, "census3: a table slot has no name (a register was dropped)"); continue; } expect_reg(h, n48dr_census3[i].name, n48dr_census3[i].off, n48dr_census3[i].base_idx); }   // 0.0.630: the M4c table
    // the field bits (sh_mask.h): the engine's control / arbitration / status / transaction / data bits
    auto mask = [&](const char *name) -> uint32_t { const std::string key = std::string("#define ") + name + " "; size_t p = m.find(key); if (p == std::string::npos) return 0xdeadbeefu; return (uint32_t)std::strtoul(m.c_str() + p + key.size(), nullptr, 0); };
    expect_u("DC_I2C_CONTROL__DC_I2C_GO_MASK", mask("DC_I2C_CONTROL__DC_I2C_GO_MASK"), N48DR_CTL_GO);
    expect_u("DC_I2C_CONTROL__DC_I2C_SOFT_RESET_MASK", mask("DC_I2C_CONTROL__DC_I2C_SOFT_RESET_MASK"), N48DR_CTL_SOFT_RESET);
    expect_u("DC_I2C_CONTROL__DC_I2C_SW_STATUS_RESET_MASK", mask("DC_I2C_CONTROL__DC_I2C_SW_STATUS_RESET_MASK"), N48DR_CTL_SW_STATUS_RESET);
    expect_u("DC_I2C_CONTROL__DC_I2C_DDC_SELECT_MASK", mask("DC_I2C_CONTROL__DC_I2C_DDC_SELECT_MASK"), 7u << N48DR_CTL_DDC_SELECT_SHIFT);
    expect_u("DC_I2C_CONTROL__DC_I2C_TRANSACTION_COUNT_MASK", mask("DC_I2C_CONTROL__DC_I2C_TRANSACTION_COUNT_MASK"), 3u << N48DR_CTL_TXN_COUNT_SHIFT);
    expect_u("DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK", mask("DC_I2C_ARBITRATION__DC_I2C_REG_RW_CNTL_STATUS_MASK"), N48DR_ARB_RW_STATUS_MASK);
    expect_u("DC_I2C_ARBITRATION__DC_I2C_NO_QUEUED_SW_GO_MASK", mask("DC_I2C_ARBITRATION__DC_I2C_NO_QUEUED_SW_GO_MASK"), N48DR_ARB_NO_QUEUED_SW_GO);
    expect_u("DC_I2C_ARBITRATION__DC_I2C_SW_USE_I2C_REG_REQ_MASK", mask("DC_I2C_ARBITRATION__DC_I2C_SW_USE_I2C_REG_REQ_MASK"), N48DR_ARB_SW_USE_REQ);
    expect_u("DC_I2C_ARBITRATION__DC_I2C_SW_DONE_USING_I2C_REG_MASK", mask("DC_I2C_ARBITRATION__DC_I2C_SW_DONE_USING_I2C_REG_MASK"), N48DR_ARB_SW_DONE_USING);
    expect_u("DC_I2C_SW_STATUS__DC_I2C_SW_DONE_MASK", mask("DC_I2C_SW_STATUS__DC_I2C_SW_DONE_MASK"), N48DR_ST_DONE);
    expect_u("DC_I2C_SW_STATUS__DC_I2C_SW_ABORTED_MASK", mask("DC_I2C_SW_STATUS__DC_I2C_SW_ABORTED_MASK"), N48DR_ST_ABORTED);
    expect_u("DC_I2C_SW_STATUS__DC_I2C_SW_TIMEOUT_MASK", mask("DC_I2C_SW_STATUS__DC_I2C_SW_TIMEOUT_MASK"), N48DR_ST_TIMEOUT);
    expect_u("DC_I2C_SW_STATUS__DC_I2C_SW_BUFFER_OVERFLOW_MASK", mask("DC_I2C_SW_STATUS__DC_I2C_SW_BUFFER_OVERFLOW_MASK"), N48DR_ST_OVERFLOW);
    expect_u("DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK", mask("DC_I2C_SW_STATUS__DC_I2C_SW_STOPPED_ON_NACK_MASK"), N48DR_ST_NACK);
    expect_u("DC_I2C_TRANSACTION0__DC_I2C_STOP_ON_NACK0_MASK", mask("DC_I2C_TRANSACTION0__DC_I2C_STOP_ON_NACK0_MASK"), N48DR_TXN_STOP_ON_NACK);
    expect_u("DC_I2C_TRANSACTION0__DC_I2C_START0_MASK", mask("DC_I2C_TRANSACTION0__DC_I2C_START0_MASK"), N48DR_TXN_START);
    expect_u("DC_I2C_TRANSACTION0__DC_I2C_STOP0_MASK", mask("DC_I2C_TRANSACTION0__DC_I2C_STOP0_MASK"), N48DR_TXN_STOP);
    expect_u("DC_I2C_TRANSACTION0__DC_I2C_RW0_MASK", mask("DC_I2C_TRANSACTION0__DC_I2C_RW0_MASK"), N48DR_TXN_READ);
    expect_u("DC_I2C_TRANSACTION0__DC_I2C_COUNT0_MASK", mask("DC_I2C_TRANSACTION0__DC_I2C_COUNT0_MASK"), 0x3FFu << N48DR_TXN_COUNT_SHIFT);
    expect_u("DC_I2C_DATA__DC_I2C_DATA_RW_MASK", mask("DC_I2C_DATA__DC_I2C_DATA_RW_MASK"), N48DR_DATA_READ);
    expect_u("DC_I2C_DATA__DC_I2C_DATA_MASK", mask("DC_I2C_DATA__DC_I2C_DATA_MASK"), 0xFFu << N48DR_DATA_SHIFT);
    expect_u("DC_I2C_DATA__DC_I2C_INDEX_MASK", mask("DC_I2C_DATA__DC_I2C_INDEX_MASK"), 0x3FFu << N48DR_DATA_INDEX_SHIFT);
    expect_u("DC_I2C_DATA__DC_I2C_INDEX_WRITE_MASK", mask("DC_I2C_DATA__DC_I2C_INDEX_WRITE_MASK"), N48DR_DATA_INDEX_WRITE);
    expect_u("DIO_MEM_PWR_CTRL bit 0 present (I2C light sleep force)", mask("DIO_MEM_PWR_CTRL__I2C_LIGHT_SLEEP_FORCE_MASK"), 1u);
    // the dcn41_dmub.h numbers the decode names
    expect(N48DR_DMUB_TYPE_QUERY_FEATURE_CAPS == DCN41_DMUB_CMD_QUERY_FEATURE_CAPS && N48DR_DMUB_TYPE_VBIOS == DCN41_DMUB_CMD_VBIOS && N48DR_DMUB_VBIOS_DIGX_ENCODER_CONTROL == DCN41_DMUB_VBIOS_DIGX_ENCODER_CONTROL &&
           N48DR_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL == DCN41_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL && N48DR_DMUB_VBIOS_SET_PIXEL_CLOCK == DCN41_DMUB_VBIOS_SET_PIXEL_CLOCK &&
           N48DR_DMUB_VBIOS_ENABLE_DISP_POWER_GATING == DCN41_DMUB_VBIOS_ENABLE_DISP_POWER_GATING && N48DR_DMUB_CMD_SIZE == DCN41_DMUB_CMD_SIZE, "the decode's type / sub-type numbers are dcn41_dmub.h's");
    expect(N48DR_DMUB_SCRATCH_FIRST == DCN41_DMCUB_SCRATCH0, "SCRATCH0 is dcn41_regs.h's");
    // every register the flow ever writes, and every DC_I2C offset the header names, inside the allowlist
    const char *rangeName = nullptr;
    for (uint32_t off : { N48DR_I2C_CONTROL, N48DR_I2C_ARBITRATION, N48DR_I2C_SW_STATUS, N48DR_I2C_DDC1_SPEED + 4u, N48DR_I2C_DDC1_SETUP + 4u, N48DR_I2C_DDC1_SPEED + 6u, N48DR_I2C_DDC1_SETUP + 6u,
                          N48DR_I2C_TRANSACTION0, N48DR_I2C_TRANSACTION0 + 1u, N48DR_I2C_TRANSACTION0 + 2u, N48DR_I2C_DATA, N48DR_DIO_MEM_PWR_CTRL }) {
        const uint32_t abs = kSeg[2] + off;
        expect(dcn41_allow_classify(abs, 262144u, &rangeName, nullptr) == DCN41_ALLOW_OK && rangeName && std::string(rangeName) == "DC..DIG6" && abs >= 0x5358u && abs <= 0x53d3u && allowed_abs(abs),
               "every register of the ddcread sequence is in the allowlist range DC..DIG6 (0x5358..0x53d3)");
    }
    expect(!allowed_abs(kSeg[2] + N48DR_I2C_DATA + 0x100u) && !allowed_abs(0x00000001u) && !allowed_abs(0x16282u), "control: a stray DC address, MM_DATA and the SMU mailbox stay refused");
}

static void c_flow() {
    for (uint32_t line = 2; line <= 3; line++) for (uint32_t block = 0; block <= 3; block++) {
        FakeI2c hw; TestEnv e(hw);
        const n48dr::DdcResult r = n48dr::ddc_read(e, line, block);
        expect_u("healthy read: status OK", r.status, N48DR_OK);
        bool same = true; for (int i = 0; i < 128; i++) same = same && r.data[i] == hw.edid[block][i];
        expect(same, "healthy read: the 128 bytes are the model's block, byte for byte");
        expect(n48dr_edid_sum(r.data) == 0u, "healthy read: the checksum is OK");
        expect(r.released && (r.arbAfter & N48DR_ARB_RW_STATUS_MASK) == 0u && hw.owner == 0 && !hw.swReq, "healthy read: the engine is released and owned by nobody");
        expect(!hw.softResets.empty() && (hw.softResets.back() & N48DR_CTL_SOFT_RESET), "healthy read: the release soft-resets the engine");
        // THE CONTAINMENT: the only bus writes
        std::vector<BusByte> w; for (auto &b : hw.bus) if (b.isWrite) w.push_back(b);
        if (block < 2) expect(w.size() == 1 && w[0].addr8 == 0xA0 && w[0].value == (uint8_t)(block * 128), "bus writes: exactly the offset byte (block * 128) to 0x50");
        else expect(w.size() == 2 && w[0].addr8 == 0x60 && w[0].value == (uint8_t)(block / 2) && w[1].addr8 == 0xA0 && w[1].value == (uint8_t)((block * 128) & 0xFF), "bus writes: exactly the segment pointer (block / 2) to 0x30, then the offset byte, to 0x50");
        size_t reads = 0; for (auto &b : hw.bus) if (!b.isWrite) reads++;
        expect_u("bus reads: 128 bytes", reads, 128);
        expect(hw.goCount == 1, "one GO per read");
        { n48dr_plan pp; (void)n48dr_plan_ddc(line, block, &pp); expect(r.polls >= 1 && r.polls <= n48dr_poll_bound(&pp), "the poll count is within the bound"); }
        for (auto &wr : e.writes) gAllWritten.insert(wr.first);
        // the DDC select and the per-line setup register
        expect(hw.setup[line] == 0u && wrote(e, N48DR_I2C_DDC1_SETUP + 2 * line) && !wrote(e, N48DR_I2C_DDC1_SETUP + 2 * (5 - line)), "only the chosen line's SETUP register is written; the release clears it");
    }
    // arbitration refusals write NOTHING
    { FakeI2c hw; hw.owner = 2; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, 0);
      expect(r.status == N48DR_HW_OWNED && e.writes.empty() && hw.bus.empty() && hw.goCount == 0 && !r.released && !r.granted, "owned by hardware / DMUB: REFUSED, no register written, no bus byte, no release"); }
    { FakeI2c hw; hw.owner = 1; hw.swReq = true; TestEnv e(hw); auto r = n48dr::ddc_read(e, 3, 1);
      expect(r.status == N48DR_SW_BUSY && e.writes.empty() && hw.bus.empty() && hw.goCount == 0, "owned by software at entry: REFUSED, nothing written"); }
    { FakeI2c hw; hw.allOnes = true; TestEnv e(hw); auto r = n48dr::ddc_read(e, 3, 0);
      expect(r.status == N48DR_ENGINE_DEAD && e.writes.empty() && hw.bus.empty(), "no response (all ones): REFUSED, nothing written"); }
    for (uint32_t line : { 0u, 1u, 4u, 255u }) { FakeI2c hw; TestEnv e(hw); auto r = n48dr::ddc_read(e, line, 0);
      expect(r.status == N48DR_BAD_ARG && e.writes.empty() && hw.bus.empty() && e.statusReads == 0, "a bad line (0, 1, 4, 255) is refused before ANY register access"); }
    for (uint32_t block : { 4u, 5u, 255u }) { FakeI2c hw; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, block);
      expect(r.status == N48DR_BAD_ARG && e.writes.empty() && hw.bus.empty(), "a bad block (4, 5, 255) is refused before ANY register access"); }
    { FakeI2c hw; hw.hwGrabsAfterWake = true; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, 0);
      expect(r.status == N48DR_HW_OWNED && !wrote(e, N48DR_I2C_ARBITRATION) && hw.bus.empty() && hw.goCount == 0 && !r.granted, "hardware takes the engine while we wake it: refused at the re-check, the arbitration register is never written"); }
    // not granted: withdraw only
    { FakeI2c hw; hw.ignoreRequest = true; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, 0);
      bool resetWritten = false; for (auto &w : e.writes) if (w.first == N48DR_I2C_CONTROL && (w.second & N48DR_CTL_SOFT_RESET)) resetWritten = true;
      expect(r.status == N48DR_NOT_GRANTED && hw.bus.empty() && hw.goCount == 0 && !resetWritten && e.writes.back().first == N48DR_I2C_ARBITRATION && (e.writes.back().second & N48DR_ARB_SW_DONE_USING),
             "request not granted: NOT_GRANTED, the request is withdrawn with DONE_USING only, no soft reset of an engine we do not own, no bus byte"); }
    // sink / engine errors: distinct statuses, ALWAYS released
    struct Err { uint32_t bits; uint32_t want; const char *name; };
    for (const Err &er : { Err{ N48DR_ST_NACK, N48DR_NACK, "NACK" }, Err{ N48DR_ST_TIMEOUT, N48DR_TIMEOUT, "engine timeout" }, Err{ N48DR_ST_ABORTED, N48DR_ABORTED, "abort" }, Err{ N48DR_ST_OVERFLOW, N48DR_OVERFLOW, "overflow" } }) {
        FakeI2c hw; hw.errBits = er.bits; TestEnv e(hw); auto r = n48dr::ddc_read(e, 3, 0);
        expect(r.status == er.want && r.released && hw.owner == 0 && !hw.softResets.empty() && wrote(e, N48DR_I2C_ARBITRATION) && (e.writes.back().second & N48DR_ARB_SW_DONE_USING) && hw.bus.empty() && r.data[0] == 0 && r.data[1] == 0,
               er.name);
        // a NACK that also reports DONE is still a NACK
    }
    { FakeI2c hw; hw.errBits = N48DR_ST_NACK | N48DR_ST_DONE; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, 0); expect(r.status == N48DR_NACK, "NACK wins over DONE"); }
    // bounded poll: never completes
    for (uint32_t block : { 0u, 2u }) { FakeI2c hw; hw.neverDone = true; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, block);
      n48dr_plan p; (void)n48dr_plan_ddc(2, block, &p);
      expect(r.status == N48DR_TIMEOUT && r.released && hw.owner == 0, "a transaction that never completes ends TIMEOUT and RELEASES the engine");
      expect_u("the poll is bounded: 2000 polls per transaction", e.statusReads, (uint64_t)p.ntxn * 2000u);
      expect(e.usec <= (uint64_t)p.ntxn * 20000u + 50u && e.usec >= (uint64_t)p.ntxn * 20000u - 50u, "the poll waits 20 ms per transaction (virtual clock)"); }
    // a refused write: abort before the GO
    for (uint32_t off : { N48DR_I2C_DDC1_SETUP + 4u, N48DR_I2C_DDC1_SPEED + 4u, N48DR_I2C_TRANSACTION0 + 1u, N48DR_I2C_DATA }) {
        FakeI2c hw; TestEnv e(hw); e.refuse.insert(off); auto r = n48dr::ddc_read(e, 2, 0);
        expect(r.status == N48DR_WRITE_REFUSED && e.goWrites == 0 && hw.goCount == 0 && hw.bus.empty() && (r.granted ? r.released : true) && hw.owner == 0, "an allowlist refusal ends WRITE_REFUSED before any GO, and releases what was taken"); }
    // the SPEED register is skipped when the time base is unprogrammed
    { FakeI2c hw; TestEnv e(hw); auto r = n48dr::ddc_read(e, 2, 0); expect(r.status == N48DR_OK && wrote(e, N48DR_I2C_DDC1_SPEED + 4u), "speed register written when the microsecond time base is programmed"); }
    // the FIFO pushes are exactly the plan
    { FakeI2c hw; TestEnv e(hw); (void)n48dr::ddc_read(e, 2, 3); std::vector<uint8_t> pushed; bool first = true;
      for (auto &w : e.writes) if (w.first == N48DR_I2C_DATA && !(w.second & N48DR_DATA_READ)) { if (first) expect((w.second & N48DR_DATA_INDEX_WRITE) != 0u, "the first FIFO push sets index 0"); first = false; pushed.push_back((uint8_t)(w.second >> 8)); }
      expect(pushed.size() == 5 && pushed[0] == 0x60 && pushed[1] == 0x01 && pushed[2] == 0xA0 && pushed[3] == 0x80 && pushed[4] == 0xA1, "the FIFO holds exactly 60 01 a0 80 a1 for block 3"); }
}

static void d_edid() {
    uint8_t b[128]; FakeI2c hw; std::memcpy(b, hw.edid[0], 128);
    expect(n48dr_edid_sum(b) == 0u && n48dr_edid_header_ok(b), "the model's block 0 is a valid EDID block");
    for (int i = 0; i < 128; i++) { uint8_t c[128]; std::memcpy(c, b, 128); c[i] = (uint8_t)(c[i] + 1); if (n48dr_edid_sum(c) != 1u) { expect(false, "checksum: changing any one byte by 1 changes the sum by 1 (position)"); return; } }
    expect(true, "checksum: every one of the 128 byte positions counts");
    { uint8_t c[128]; std::memcpy(c, b, 128); c[127] = (uint8_t)(c[127] ^ 0x80); expect(n48dr_edid_sum(c) != 0u, "a flipped bit in the checksum byte is BAD"); }
    { uint8_t c[128]; std::memcpy(c, b, 128); c[0] = 1; expect(!n48dr_edid_header_ok(c), "a bad EDID header is detected"); }
    expect(n48dr_edid_sum(hw.edid[1]) == 0u && n48dr_edid_sum(hw.edid[2]) == 0u && n48dr_edid_sum(hw.edid[3]) == 0u, "the model's extension blocks sum to 0 too");
}


// ---- scdcread (0.0.633) ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void f_scdc() {
    // the plan: exhaustive over line 0..255, offset 0..255, length 0..20
    uint32_t okCount = 0, wantCount = 0;
    for (uint32_t o = 0; o < 0x60; o++) for (uint32_t l = 1; l <= 16 && o + l <= 0x60; l++) ++wantCount;
    bool allOk = true;
    for (uint32_t line = 0; line < 256 && allOk; line++) for (uint32_t off = 0; off < 256 && allOk; off++) for (uint32_t len = 0; len <= 20; len++) {
        n48dr_plan p; const uint32_t rc = n48dr_plan_scdc(line, off, len, &p);
        const bool want = (line == 2 || line == 3) && off <= 0x5F && len >= 1 && len <= 16 && off + len <= 0x60;
        if ((rc == 0) != want || (rc == 0) != (n48dr_scdc_range_ok(off, len) && n48dr_ddc_line_ok(line)) || (rc == 0) != (n48dr_scdc_arg_ok(n48dr_scdc_arg(line, off, len)) ? true : false)) { allOk = false; break; }
        if (rc == 0) okCount++; else if (p.ntxn != 0 || p.fifo_len != 0 || rc != N48DR_BAD_ARG) { allOk = false; break; }
    }
    expect(allOk, "scdc plan: accepted/refused exactly lines 2 and 3, offsets 0..0x5f, lengths 1..16 with offset + length <= 0x60; a refusal leaves an empty plan and BAD_ARG");
    expect_u("scdc plan: 2 x (the number of legal (offset, length) pairs) plans", okCount, 2u * wantCount);
    n48dr_plan p;
    for (uint32_t line = 2; line <= 3; line++) for (uint32_t off : { 0x00u, 0x01u, 0x20u, 0x21u, 0x40u, 0x50u, 0x5Fu }) for (uint32_t len : { 1u, 2u, 7u, 16u }) {
        if (off + len > 0x60u) continue;
        expect_u("scdc plan", n48dr_plan_scdc(line, off, len, &p), 0);
        expect(p.ntxn == 2 && !p.has_seg && p.t[0].addr8 == 0xA8 && p.t[0].wlen == 1 && p.t[0].wdata == (uint8_t)off && !p.t[0].is_read && !p.t[0].stop && p.t[1].addr8 == 0xA9 && p.t[1].is_read && p.t[1].rlen == len && p.t[1].stop, "scdc plan: [W 0xA8 offset] [R 0xA9 len STOP]");
        expect(p.fifo_len == 3 && p.fifo[0] == 0xA8 && p.fifo[1] == (uint8_t)off && p.fifo[2] == 0xA9 && p.read_index == 3, "scdc plan: FIFO a8 <offset> a9, reply at index 3");
        expect_u("scdc plan: transaction 0 register (STOP_ON_NACK | START | 1 byte)", n48dr_txn_reg(&p.t[0]), 0x100u | 0x1000u | (1u << 16));
        expect_u("scdc plan: transaction 1 register (STOP_ON_NACK | START | READ | STOP | len)", n48dr_txn_reg(&p.t[1]), 0x100u | 0x1000u | 1u | 0x2000u | (len << 16));
        expect_u("scdc plan: poll bound = 2 x 20 ms", n48dr_poll_bound(&p), 4000);
    }
    expect(N48DR_SCDC_ADDR_W == (0x54u << 1) && N48DR_SCDC_ADDR_R == ((0x54u << 1) | 1u) && N48DR_ACT_SCDCREAD == 99u && N48DR_SCDC_OFF_MAX == 0x5Fu && N48DR_SCDC_END == 0x60u && N48DR_SCDC_LEN_MAX == 16u, "scdc: slave 0x54 (bus bytes 0xA8 / 0xA9), action 99, offsets <= 0x5f, end 0x60, 16 bytes");
    // the arguments
    expect(n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x40, 1)) && n48dr_scdc_arg_ok(n48dr_scdc_arg(3, 0x50, 16)) && n48dr_scdc_arg_ok(n48dr_scdc_arg(3, 0, 1)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(1, 0x40, 1)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(4, 0x40, 1)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x60, 1)) &&
           !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x51, 16)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x40, 0)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x00, 17)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0xC0, 1)) && !n48dr_scdc_arg_ok(n48dr_scdc_arg(2, 0x40, 1) | (1ull << 24)) &&
           !n48dr_scdc_arg_ok(~0ull) && !n48dr_scdc_arg_ok(0), "scdcread arguments: line | offset << 8 | len << 16, nothing else (the SCDC test registers at 0xC0 and everything above 0x5f are unreachable)");
    // the flow over the engine model: every legal (line, offset, length) returns the sink's bytes, the ONLY bus write is the offset byte to 0xA8
    for (uint32_t line = 2; line <= 3; line++) for (uint32_t off : { 0x00u, 0x01u, 0x20u, 0x21u, 0x40u, 0x50u, 0x56u, 0x5Fu }) for (uint32_t len : { 1u, 2u, 7u, 16u }) {
        if (off + len > 0x60u) continue;
        FakeI2c hw; TestEnv e(hw);
        const n48dr::DdcResult r = n48dr::scdc_read(e, line, off, len);
        expect_u("scdc read: status OK", r.status, N48DR_OK);
        bool same = true; for (uint32_t i = 0; i < len; i++) same = same && r.data[i] == hw.scdc[off + i]; for (uint32_t i = len; i < 128; i++) same = same && r.data[i] == 0;
        expect(same, "scdc read: the bytes are the sink's, byte for byte; nothing past len");
        std::vector<BusByte> w; for (auto &b : hw.bus) if (b.isWrite) w.push_back(b);
        expect(w.size() == 1 && w[0].addr8 == 0xA8 && w[0].value == (uint8_t)off, "scdc bus writes: exactly ONE byte, the register offset, to address 0x54 (bus byte 0xA8)");
        size_t reads = 0; bool allA9 = true; for (auto &b : hw.bus) if (!b.isWrite) { reads++; allA9 = allA9 && b.addr8 == 0xA9; }
        expect(reads == len && allA9, "scdc bus reads: exactly len bytes, all from 0xA9");
        expect(r.released && (r.arbAfter & N48DR_ARB_RW_STATUS_MASK) == 0u && hw.owner == 0 && !hw.swReq && hw.goCount == 1 && !hw.softResets.empty(), "scdc read: one GO, the engine is released and owned by nobody");
        expect(r.polls >= 1 && r.polls <= 4000u, "scdc read: the poll count is within the 2 x 20 ms bound");
        for (auto &wr : e.writes) gAllWritten.insert(wr.first);
        expect(hw.setup[line] == 0u && wrote(e, N48DR_I2C_DDC1_SETUP + 2 * line) && !wrote(e, N48DR_I2C_DDC1_SETUP + 2 * (5 - line)), "scdc read: only the chosen line's SETUP register is written; the release clears it");
    }
    // the SCDC registers the bus never reaches: no write transaction ever carries a data byte past the offset
    { FakeI2c hw; TestEnv e(hw); (void)n48dr::scdc_read(e, 3, 0x20, 1); for (auto &b : hw.bus) if (b.isWrite) expect(b.addr8 == 0xA8 && b.value == 0x20, "the TMDS_Config register (0x20) is READ: the only write on the bus is the offset 0x20 itself");
      std::vector<uint8_t> pushed; for (auto &w : e.writes) if (w.first == N48DR_I2C_DATA && !(w.second & N48DR_DATA_READ)) pushed.push_back((uint8_t)(w.second >> 8));
      expect(pushed.size() == 3 && pushed[0] == 0xA8 && pushed[1] == 0x20 && pushed[2] == 0xA9, "the FIFO holds exactly a8 20 a9 (write address, offset, read address)");
      expect(hw.scdc[0x20] == 0x03u && hw.scdc[0x21] == 0x01u, "the sink's TMDS_Config / Scrambler_Status are exactly as they were"); }
    // refusals write NOTHING (the argument is judged before any register)
    struct Bad { uint32_t line, off, len; };
    for (const Bad &b : { Bad{ 0, 0x40, 1 }, Bad{ 1, 0x40, 1 }, Bad{ 4, 0x40, 1 }, Bad{ 255, 0x40, 1 }, Bad{ 2, 0x60, 1 }, Bad{ 2, 0xFF, 1 }, Bad{ 2, 0x5F, 2 }, Bad{ 2, 0x51, 16 }, Bad{ 3, 0x40, 0 }, Bad{ 3, 0x40, 17 }, Bad{ 3, 0, 255 } }) {
        FakeI2c hw; TestEnv e(hw); const n48dr::DdcResult r = n48dr::scdc_read(e, b.line, b.off, b.len);
        expect(r.status == N48DR_BAD_ARG && e.writes.empty() && hw.bus.empty() && e.statusReads == 0 && !r.released && hw.goCount == 0, "scdc: a bad line / offset / length is refused before ANY register access"); }
    { FakeI2c hw; hw.owner = 2; TestEnv e(hw); auto r = n48dr::scdc_read(e, 3, 0x40, 1);
      expect(r.status == N48DR_HW_OWNED && e.writes.empty() && hw.bus.empty() && hw.goCount == 0 && !r.released && !r.granted, "scdc: owned by hardware / DMUB: REFUSED, no register written, no bus byte"); }
    { FakeI2c hw; hw.owner = 1; hw.swReq = true; TestEnv e(hw); auto r = n48dr::scdc_read(e, 3, 0x40, 1); expect(r.status == N48DR_SW_BUSY && e.writes.empty() && hw.bus.empty() && hw.goCount == 0, "scdc: owned by software at entry: REFUSED, nothing written"); }
    { FakeI2c hw; hw.allOnes = true; TestEnv e(hw); auto r = n48dr::scdc_read(e, 3, 0x40, 1); expect(r.status == N48DR_ENGINE_DEAD && e.writes.empty() && hw.bus.empty(), "scdc: no response (all ones): REFUSED, nothing written"); }
    { FakeI2c hw; hw.hwGrabsAfterWake = true; TestEnv e(hw); auto r = n48dr::scdc_read(e, 2, 0x40, 1); expect(r.status == N48DR_HW_OWNED && !wrote(e, N48DR_I2C_ARBITRATION) && hw.bus.empty() && hw.goCount == 0 && !r.granted, "scdc: hardware takes the engine while we wake it: refused at the re-check"); }
    { FakeI2c hw; hw.ignoreRequest = true; TestEnv e(hw); auto r = n48dr::scdc_read(e, 2, 0x40, 1); bool resetWritten = false; for (auto &w : e.writes) if (w.first == N48DR_I2C_CONTROL && (w.second & N48DR_CTL_SOFT_RESET)) resetWritten = true;
      expect(r.status == N48DR_NOT_GRANTED && hw.bus.empty() && hw.goCount == 0 && !resetWritten && e.writes.back().first == N48DR_I2C_ARBITRATION && (e.writes.back().second & N48DR_ARB_SW_DONE_USING), "scdc: request not granted: withdrawn with DONE_USING only, no soft reset, no bus byte"); }
    // sink / engine errors: distinct statuses, ALWAYS released, no data returned
    struct Err { uint32_t bits; uint32_t want; const char *name; };
    for (const Err &er : { Err{ N48DR_ST_NACK, N48DR_NACK, "scdc NACK (a sink without SCDC)" }, Err{ N48DR_ST_TIMEOUT, N48DR_TIMEOUT, "scdc engine timeout" }, Err{ N48DR_ST_ABORTED, N48DR_ABORTED, "scdc abort" }, Err{ N48DR_ST_OVERFLOW, N48DR_OVERFLOW, "scdc overflow" } }) {
        FakeI2c hw; hw.errBits = er.bits; TestEnv e(hw); auto r = n48dr::scdc_read(e, 3, 0x40, 1);
        expect(r.status == er.want && r.released && hw.owner == 0 && !hw.softResets.empty() && wrote(e, N48DR_I2C_ARBITRATION) && (e.writes.back().second & N48DR_ARB_SW_DONE_USING) && hw.bus.empty() && r.data[0] == 0, er.name); }
    for (uint32_t len : { 1u, 16u }) { FakeI2c hw; hw.neverDone = true; TestEnv e(hw); auto r = n48dr::scdc_read(e, 2, 0x40, len);
      expect(r.status == N48DR_TIMEOUT && r.released && hw.owner == 0, "scdc: a transaction that never completes ends TIMEOUT and RELEASES the engine");
      expect_u("scdc: the poll is bounded: 2000 polls per transaction", e.statusReads, 2u * 2000u);
      expect(e.usec <= 2u * 20000u + 50u && e.usec >= 2u * 20000u - 50u, "scdc: the poll waits 20 ms per transaction (virtual clock)"); }
    for (uint32_t off : { N48DR_I2C_DDC1_SETUP + 6u, N48DR_I2C_DDC1_SPEED + 6u, N48DR_I2C_TRANSACTION0 + 1u, N48DR_I2C_DATA }) {
        FakeI2c hw; TestEnv e(hw); e.refuse.insert(off); auto r = n48dr::scdc_read(e, 3, 0x40, 1);
        expect(r.status == N48DR_WRITE_REFUSED && e.goWrites == 0 && hw.goCount == 0 && hw.bus.empty() && (r.granted ? r.released : true) && hw.owner == 0, "scdc: an allowlist refusal ends WRITE_REFUSED before any GO, and releases what was taken"); }
    // the answer layout and the SCDC decode
    { uint8_t d[16]; for (int i = 0; i < 16; i++) d[i] = (uint8_t)(0x10 + i);
      for (uint32_t len = 1; len <= 16; len++) { uint64_t v[13]; n48dr_scdc_fill(v, N48DR_OK, len, 1, 0x5A, 0xABCD1234u, 0x11223344u, 0x55667788u, d); uint8_t o[16] = {}; n48dr_scdc_extract(v, o);
          bool ok = (v[0] & 0xFF) == N48DR_OK && ((v[0] >> 8) & 0xFF) == len && ((v[0] >> 16) & 1) == 1 && ((v[0] >> 24) & 0xFF) == 0x5A && (v[0] >> 32) == 0xABCD1234u && (uint32_t)v[1] == 0x11223344u && (v[1] >> 32) == 0x55667788u;
          for (uint32_t i = 0; i < 16; i++) ok = ok && o[i] == (i < len ? d[i] : 0); expect(ok, "scdc answer layout: v[0] = status | len << 8 | released << 16 | seq << 24 | arb_after << 32, v[1] = arb | sw << 32, v[2..3] = the bytes, zero past len"); }
      uint64_t v[13]; n48dr_scdc_fill(v, N48DR_BUSY, 4, 0, 0, 0, 0, 0, nullptr); expect(v[2] == 0 && v[3] == 0 && (v[0] & 0xFF) == N48DR_BUSY, "scdc answer layout: no data on a refusal"); }
    expect(n48dr_scdc_clock_detected(0x01) && !n48dr_scdc_clock_detected(0x0E) && n48dr_scdc_ch_locked(0x02, 0) && !n48dr_scdc_ch_locked(0x02, 1) && n48dr_scdc_ch_locked(0x04, 1) && n48dr_scdc_ch_locked(0x08, 2) && !n48dr_scdc_ch_locked(0x01, 2) && n48dr_scdc_ch_locked(0x0F, 2) && n48dr_scdc_ch_locked(0x0F, 0), "Status_Flags_0: bit 0 clock detected, bits 1 / 2 / 3 = channel 0 / 1 / 2 locked (HDMI 2.0; Linux union hdmi_scdc_status_flags_data)");
    { bool ok = true; for (unsigned b = 0; b < 256; b++) ok = ok && (n48dr_scdc_locked((uint8_t)b) != 0) == ((b & 0x0Fu) == 0x0Fu); expect(ok, "n48dr_scdc_locked: true exactly when bits 0..3 (clock, ch0, ch1, ch2) are all set (all 256 values)"); }
    expect(n48dr_scdc_scrambling_enabled(0x01) && !n48dr_scdc_scrambling_enabled(0x02) && n48dr_scdc_ratio_by_40(0x02) && !n48dr_scdc_ratio_by_40(0x01) && n48dr_scdc_ratio_by_40(0x03) && n48dr_scdc_scrambling_enabled(0x03) && n48dr_scdc_scrambling_status(0x01) && !n48dr_scdc_scrambling_status(0x00), "TMDS_Config: bit 0 scrambling, bit 1 ratio 1/40 (write_scdc_data writes 3 above 340 MHz); Scrambler_Status bit 0");
    { int valid = 7; expect(n48dr_scdc_err_count(0x34, 0x92, &valid) == 0x1234u && valid == 1 && n48dr_scdc_err_count(0xFF, 0x7F, &valid) == 0x7FFFu && valid == 0 && n48dr_scdc_err_count(0, 0x80, nullptr) == 0u, "Err_Det: 15-bit count = low | (high & 0x7f) << 8, valid = high bit 7"); }
    expect(N48DR_SCDC_SINK_VERSION == 0x01u && N48DR_SCDC_TMDS_CONFIG == 0x20u && N48DR_SCDC_SCRAMBLER_STATUS == 0x21u && N48DR_SCDC_STATUS_FLAGS == 0x40u && N48DR_SCDC_ERR_DETECT == 0x50u, "the SCDC offsets (HDMI_SCDC_* in dc_hdmi_types.h)");
    { const char *roots[] = { "/path/to/navi48-checkout/re/linux-dc", nullptr }; std::string dc; for (const char **r = roots; *r && dc.empty(); r++) dc = slurp(std::string(*r) + "/drivers/gpu/drm/amd/display/dc/dc_hdmi_types.h");
      if (const char *e = std::getenv("N48_LINUX_DC")) { std::string d2 = slurp(std::string(e) + "/re/linux-dc/drivers/gpu/drm/amd/display/dc/dc_hdmi_types.h"); if (!d2.empty()) dc = d2; }
      expect(!dc.empty() && dc.find("#define HDMI_SCDC_ADDRESS  0x54") != std::string::npos && dc.find("#define HDMI_SCDC_SINK_VERSION 0x01") != std::string::npos && dc.find("#define HDMI_SCDC_TMDS_CONFIG 0x20") != std::string::npos && dc.find("#define HDMI_SCDC_SCRAMBLER_STATUS 0x21") != std::string::npos &&
             dc.find("#define HDMI_SCDC_STATUS_FLAGS 0x40") != std::string::npos && dc.find("#define HDMI_SCDC_ERR_DETECT 0x50") != std::string::npos && dc.find("uint8_t CLOCK_DETECTED:1;\n\t\tuint8_t CH0_LOCKED:1;\n\t\tuint8_t CH1_LOCKED:1;\n\t\tuint8_t CH2_LOCKED:1;") != std::string::npos,
             "Linux's dc_hdmi_types.h: slave 0x54, the offsets and the status-flags bit order (clock, ch0, ch1, ch2) match"); }
}

// ---- DMUB ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct FakeVram { std::map<uint64_t, uint32_t> m; uint64_t lo = ~0ull, hi = 0; uint64_t reads = 0; std::vector<uint64_t> log;
    uint32_t rd32(uint64_t off) { reads++; log.push_back(off); auto it = m.find(off); return it == m.end() ? 0xBADBAD00u : it->second; } };
static void e_dmub() {
    // header fields
    { auto h = n48dr_dmub_decode(DCN41_DMUB_CMD_VBIOS | (DCN41_DMUB_VBIOS_SET_PIXEL_CLOCK << 8) | (1u << 16) | (1u << 17) | (1u << 18) | (44u << 24));
      expect(h.type == 128 && h.sub_type == 2 && h.ret_status && h.multi_cmd_pending && h.is_reg_based && h.payload_bytes == 44 && h.reserved_bits_clear, "header: type / sub_type / ret_status / multi_cmd_pending / is_reg_based / payload_bytes"); }
    expect(!n48dr_dmub_decode(0x00080000u).reserved_bits_clear && !n48dr_dmub_decode(0x80000000u).reserved_bits_clear, "header: a set reserved bit is reported");
    expect(n48dr_dmub_decode(dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, 1, 40, true)).payload_bytes == 40, "header: the decode reads what dcn41_dmub_header writes");
    expect(std::string(n48dr_dmub_type_name(128)) == "VBIOS" && std::string(n48dr_dmub_type_name(6)) == "QUERY_FEATURE_CAPS" && std::string(n48dr_dmub_type_name(0)) == "unknown" && std::string(n48dr_dmub_type_name(7)) == "unknown", "type names (only dcn41_dmub.h's)");
    expect(std::string(n48dr_dmub_subtype_name(128, 0)) == "DIGX_ENCODER_CONTROL" && std::string(n48dr_dmub_subtype_name(128, 1)) == "DIG1_TRANSMITTER_CONTROL" && std::string(n48dr_dmub_subtype_name(128, 2)) == "SET_PIXEL_CLOCK" &&
           std::string(n48dr_dmub_subtype_name(128, 3)) == "ENABLE_DISP_POWER_GATING", "VBIOS sub-type names 0..3");
    for (uint32_t s : { 4u, 5u, 6u, 10u, 12u, 16u, 255u }) expect(std::string(n48dr_dmub_subtype_name(128, s)) == "unknown", "VBIOS sub-types 6 / 10 / 12 / 16 (RDNA4FB's GOP dialect) and the rest print unknown");
    expect(std::string(n48dr_dmub_subtype_name(6, 0)) == "-" && std::string(n48dr_dmub_subtype_name(6, 3)) == "unknown", "a non-VBIOS command has no named sub-type");
    // the ring count
    uint32_t sane = 9;
    expect_u("ring: this card's 0x640 of 8192 = 25 commands", n48dr_ring_count(8192, 0x640, &sane), 25); expect_u("... and sane", sane, 1);
    expect_u("ring: WPTR 0 = no commands, still sane", n48dr_ring_count(8192, 0, &sane), 0); expect_u("... sane", sane, 1);
    expect_u("ring: 100 commands are capped at 25", n48dr_ring_count(8192, 100 * 64, &sane), 25);
    expect_u("ring: 3 commands", n48dr_ring_count(8192, 192, &sane), 3);
    n48dr_ring_count(8192, 0x41, &sane); expect_u("ring: WPTR not a multiple of 64 is insane", sane, 0);
    n48dr_ring_count(0, 0, &sane); expect_u("ring: size 0 is insane", sane, 0);
    n48dr_ring_count(100, 64, &sane); expect_u("ring: size not a multiple of 64 is insane", sane, 0);
    n48dr_ring_count(8192, 8256, &sane); expect_u("ring: WPTR past the size is insane", sane, 0);
    n48dr_ring_count(0x20000, 64, &sane); expect_u("ring: above 64 KiB is insane", sane, 0);
    // BAR0 reachability: this card's real numbers : ring at FB address 0x83dac00000, FB base 0x8000000000, aperture 256 MiB
    const uint64_t ringGpu = 0x83dac00000ull, fbBase = 0x8000000000ull, bar0 = 256ull << 20, ringOff = ringGpu - fbBase;
    expect_u("this card's ring VRAM offset", ringOff, 0x3dac00000ull);
    expect(!n48dr_ring_in_bar0(ringOff, 25 * 64, bar0), "this card's ring is BEYOND the 256 MiB BAR0 aperture: unreachable");
    expect(n48dr_ring_in_bar0(0x1000, 25 * 64, bar0) && n48dr_ring_in_bar0(bar0 - 64, 64, bar0) && !n48dr_ring_in_bar0(bar0 - 64, 65, bar0) && !n48dr_ring_in_bar0(bar0, 64, bar0) && !n48dr_ring_in_bar0(~0ull - 10, 64, bar0) && !n48dr_ring_in_bar0(0, 0, bar0) && !n48dr_ring_in_bar0(0, 64, 0),
           "BAR0 reachability: edges, overflow, empty");
    expect_u("page verdict: this card's ring is RING_UNREACHABLE", n48dr::ring_page_verdict(1, true, 8192, 0x640, ringOff, bar0), N48DR_RING_UNREACHABLE);
    expect_u("page verdict: unknown ring", n48dr::ring_page_verdict(1, false, 8192, 0x640, 0x1000, bar0), N48DR_NO_RING);
    expect_u("page verdict: insane ring", n48dr::ring_page_verdict(1, true, 8192, 0x41, 0x1000, bar0), N48DR_NO_RING);
    expect_u("page verdict: a reachable ring, command 25", n48dr::ring_page_verdict(25, true, 8192, 0x640, 0x1000, bar0), N48DR_OK);
    expect_u("page verdict: page 26 is out of range", n48dr::ring_page_verdict(26, true, 8192, 0x640, 0x1000, bar0), N48DR_PAGE_RANGE);
    expect_u("page verdict: page 0 is not a command page", n48dr::ring_page_verdict(0, true, 8192, 0x640, 0x1000, bar0), N48DR_PAGE_RANGE);
    expect_u("page verdict: only 3 commands in the ring, page 4", n48dr::ring_page_verdict(4, true, 8192, 192, 0x1000, bar0), N48DR_PAGE_RANGE);
    expect(n48dr_dmub_arg_ok(0) && n48dr_dmub_arg_ok(25) && n48dr_dmub_arg_ok(0x80) && !n48dr_dmub_arg_ok(26) && !n48dr_dmub_arg_ok(0x81) && !n48dr_dmub_arg_ok(~0ull), "dmubring arguments: 0, 1..25, 0x80");
    // the ring reader: only [off + 64 * idx, + 36) of its command, through the BAR0 reader
    FakeVram vr; const uint64_t base = 0x2000;
    for (uint32_t c = 0; c < 25; c++) for (uint32_t d = 0; d < 16; d++) vr.m[base + c * 64 + d * 4] = 0xC0000000u | (c << 8) | d;
    vr.m[base + 7 * 64] = DCN41_DMUB_CMD_VBIOS | (1u << 8) | (36u << 24);
    uint32_t hdr = 0, pl[8] = {};
    n48dr::ring_read_cmd(vr, base, 7, &hdr, pl);
    expect(hdr == (DCN41_DMUB_CMD_VBIOS | (1u << 8) | (36u << 24)) && pl[0] == (0xC0000000u | (7u << 8) | 1u) && pl[7] == (0xC0000000u | (7u << 8) | 8u), "ring reader: header and payload dwords 1..8 of command 7");
    expect(vr.reads == 9 && vr.log.front() == base + 7 * 64 && vr.log.back() == base + 7 * 64 + 32 && *std::max_element(vr.log.begin(), vr.log.end()) < base + 8 * 64 && *std::min_element(vr.log.begin(), vr.log.end()) >= base + 7 * 64, "ring reader: exactly 9 dword reads inside the command, none outside");
    // the packers
    { uint64_t v[13]; uint32_t p8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }; n48dr_dmub_cmd_pack(v, 0, 5, 0xAABBCCDDu, 25, p8);
      expect((v[0] & 0xFF) == 0 && ((v[0] >> 8) & 1) == 1 && ((v[0] >> 16) & 0xFF) == 5 && (uint32_t)v[1] == 0xAABBCCDDu && (v[1] >> 32) == 25 && v[2] == (1ull | (2ull << 32)) && v[5] == (7ull | (8ull << 32)) && v[6] == 0, "dmubring command packing"); }
    { n48dr_dmub_summary s; std::memset(&s, 0, sizeof(s)); s.status = 14; s.verdict = 0; s.ring_map = 2; s.enabled = 1; s.dal_fw = 0; s.mailbox_rdy = 1; s.ring_sane = 1; s.ncmd = 25; s.cntl = 0x10000; s.cntl2 = 0; s.sec_cntl = 7; s.scratch0 = 0x82;
      s.inbox_base = 0x80000000u; s.inbox_size = 8192; s.inbox_wptr = 0x640; s.inbox_rptr = 0x640; s.ring_addr = ringGpu; s.fb_base_mc = fbBase; s.ring_vram_off = ringOff; s.bar0_size = bar0; s.scratch7 = 9; s.scratch14 = 10; s.scratch15 = 11; s.fault_addr = 12;
      uint64_t v[13]; n48dr_dmub_summary_pack(&s, v);
      expect((v[0] & 0xFF) == 14 && ((v[0] >> 16) & 0xFF) == 2 && (((v[0] >> 24) & 0xFF) == 0x29u) && (v[0] >> 32) == 25 && v[2] == (7ull | (0x82ull << 32)) && v[3] == (0x80000000ull | (8192ull << 32)) && v[4] == (0x640ull | (0x640ull << 32)) && v[6] == ringGpu && v[8] == ringOff && v[9] == bar0 && v[10] == (9ull | (10ull << 32)) && v[11] == (11ull | (12ull << 32)),
             "dmubring summary packing (flags: enabled 1, mailbox_rdy 8, sane 32)"); }
    { uint32_t vals[3] = { 1, 2, 3 }; uint64_t v[13] = {}; n48dr_pack_regs(v, 1, vals, 3); expect(v[1] == (1ull | (2ull << 32)) && v[2] == 3ull, "register pair packing"); }
}

// ---- the census ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct CensusEnv { std::vector<std::pair<uint32_t, uint32_t>> reads;
    uint32_t rd(uint32_t b, uint32_t o) { reads.push_back({ b, o }); return (b << 28) | o | 0x01000000u; } };
// ---- (0.0.628, M4a) the second census table: pages 2..7 -------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t ro_rd(void *, uint32_t) { return 0; }
static void ro_wr(void *, uint32_t, uint32_t) {}
static void ro_dl(void *, uint32_t) {}
static bool in_block(const n48dr_reg &r) {   // the display blocks, by BASE_IDX and offset (DCN_BASE segments of dcn_4_1_0_offset.h)
    if (r.base_idx == 1) return r.off >= 0x006f && r.off <= 0x00a4;                         // DCCG clock sources in segment 1: OTG_PIXEL_RATE_DIV 0x6f .. SYMCLKE_CLOCK_ENABLE 0xa4
    if (r.base_idx != 2) return false;
    if (r.off >= 0x0052 && r.off <= 0x0056) return true;                                   // PHYASYMCLK..PHYESYMCLK_CLOCK_CNTL (segment 2)
    if (r.off >= 0x1f0d && r.off <= 0x1f10) return true;                                   // DIG0..3_STREAM_MAPPER_CONTROL
    if (r.off >= 0x2093 && r.off <= 0x2095) return true;                                   // DIG0 FE
    if (r.off >= 0x20bb && r.off <= 0x20bc) return true;                                   // DIG0 BE
    if (r.off >= 0x21b7 && r.off <= 0x21b9) return true;
    if (r.off >= 0x21df && r.off <= 0x21e0) return true;
    if (r.off >= 0x22db && r.off <= 0x22dd) return true;
    if (r.off >= 0x2303 && r.off <= 0x2304) return true;
    if (r.off >= 0x23ff && r.off <= 0x2401) return true;
    if (r.off >= 0x2427 && r.off <= 0x2428) return true;
    if (r.off >= 0x2a08 && r.off <= 0x2a36) return true;                                   // RDPCSTX1
    if (r.off >= 0x2ae0 && r.off <= 0x2b0e) return true;                                   // RDPCSTX2
    return false;
}
static void f_census2() {
    { bool named = true; for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) named = named && n48dr_census2[i].name && n48dr_census2[i].name[0] && n48dr_census2[i].off != 0;
      expect(named, "census2: every one of the 135 slots is filled (a dropped register leaves an empty slot)"); if (!named) return; }
    expect_u("census2: registers", N48DR_CENSUS2_COUNT, 135);
    expect_u("census2: pages", N48DR_CENSUS2_PAGES, 6);
    expect_u("census: all pages", N48DR_CENSUS_ALL_PAGES, 60);   /* 0.0.630: pages 8 and 9 (M4c); 0.0.634: page 10 (the DP watch) and pages 11..42 (the plane census); 0.0.635: page 43 (the rest of the DP watch, census6) */
    expect(n48dr_census_in_page(2) == 24 && n48dr_census_in_page(6) == 24 && n48dr_census_in_page(7) == 15 && n48dr_census_in_page(8) == 23 && n48dr_census_in_page(9) == 2 && n48dr_census_in_page(10) == 9 && n48dr_census_in_page(43) == 6 && n48dr_census_in_page(44) == 24 && n48dr_census_in_page(59) == 361 - 15 * 24 && n48dr_census_in_page(60) == 0 && n48dr_census_in_page(0) == 24 && n48dr_census_in_page(1) == 16, "census2: page sizes 24 24 24 24 24 15, the original 24 / 16 unchanged");
    expect_u("census2: the original table is untouched (still 40)", N48DR_CENSUS_COUNT, 40);
    std::set<std::string> names; std::set<std::pair<int, int>> addrs;
    for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) { names.insert(n48dr_census2[i].name); addrs.insert({ n48dr_census2[i].base_idx, n48dr_census2[i].off }); }
    expect(names.size() == N48DR_CENSUS2_COUNT && addrs.size() == N48DR_CENSUS2_COUNT, "census2: names and addresses unique");
    auto has = [&](const char *n) { for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) if (!std::strcmp(n48dr_census2[i].name, n)) return true; return false; };
    bool all = true;
    for (int i = 0; i < 4; i++) {
        const std::string o = std::to_string(i);
        for (const char *t : { "OTG%s_PIXEL_RATE_CNTL", "OTG%s_PHYPLL_PIXEL_RATE_CNTL", "DP_DTO%s_PHASE", "DP_DTO%s_MODULO", "DIG%s_DIG_FE_CNTL", "DIG%s_DIG_FE_CLK_CNTL", "DIG%s_DIG_BE_CNTL", "DIG%s_DIG_BE_CLK_CNTL", "DIG%s_STREAM_MAPPER_CONTROL" }) {
            std::string n = t; n.replace(n.find("%s"), 2, o); all = all && has(n.c_str());
        }
    }
    for (char c = 'A'; c <= 'E'; c++) all = all && has((std::string("SYMCLK") + c + "_CLOCK_ENABLE").c_str()) && has((std::string("PHY") + c + "SYMCLK_CLOCK_CNTL").c_str());
    all = all && has("OTG_PIXEL_RATE_DIV");
    for (int p = 1; p <= 2; p++) for (int k = 0; k <= 17; k++) all = all && has(("RDPCSTX" + std::to_string(p) + "_RDPCSTX_PHY_CNTL" + std::to_string(k)).c_str());
    expect(all, "census2: the brief's registers are all there (OTG0-3 rate / PHYPLL rate, OTG_PIXEL_RATE_DIV, DTO0-3, SYMCLKA-E, PHYA-ESYMCLK, DIG0-3 FE/BE/CLK/MAPPER, RDPCSTX1/2 PHY_CNTL0..17)");
    expect(has("RDPCSTX1_RDPCSTX_PHY_FUSE0") && has("RDPCSTX2_RDPCS_TX_SRAM_CNTL") && has("RDPCSTX2_RDPCS_TX_CR_ADDR") && !has("RDPCSTX1_RDPCS_TX_CR_DATA") && !has("RDPCSTX2_RDPCS_TX_CR_DATA"), "census2: the status registers are in; RDPCS_TX_CR_DATA (a possible read trigger) is the one left out");
    // 42 registers per PHY block: every register of dcn_4_1_0_offset.h's RDPCSTX<n> block but CR_DATA
    { const std::string h = linux_hdr("dcn_4_1_0_offset.h");
      for (int p = 1; p <= 2; p++) { size_t pos = 0, n = 0; const std::string pre = "#define regRDPCSTX" + std::to_string(p) + "_";
          while ((pos = h.find(pre, pos)) != std::string::npos) { const size_t e = h.find(' ', pos + 11); const std::string nm = h.substr(pos + 11, e - pos - 11); if (nm.size() < 9 || nm.compare(nm.size() - 9, 9, "_BASE_IDX") != 0) { ++n; if (nm.find("RDPCS_TX_CR_DATA") == std::string::npos) expect(has(nm.c_str()), (std::string("census2 carries ") + nm).c_str()); } pos = e; }
          expect_u("census2: the Linux RDPCSTX block size (incl. CR_DATA)", n, 43); } }
    // every register resolves through the REAL read-only address twin, inside the BAR5 window, and sits in a display block
    struct dcn41_dev d; expect(dcn41_dev_init(&d, nullptr, ro_rd, ro_wr, ro_dl, kSeg, 0x100000u, 0) == DCN41_OK, "census2: dcn41_dev_init on the card's own bases");
    bool res = true, blk = true, absok = true;
    for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) {
        const n48dr_reg &r = n48dr_census2[i];
        const uint32_t a = dcn41_abs_rd(&d, r.off, r.base_idx);
        res = res && a != DCN41_BAD_OFFSET && a == kSeg[r.base_idx] + r.off;
        absok = absok && a == n48dr_census_abs(r.base_idx, r.off);
        blk = blk && in_block(r);
        // the write side refuses the same registers when they are BASE_IDX 1 or 3
        if (r.base_idx != 2) expect(dcn41_abs(&d, r.off, r.base_idx) == DCN41_BAD_OFFSET, (std::string("the write-side address refuses ") + r.name).c_str());
    }
    expect(res, "census2: every register resolves through dcn41_abs_rd to seg + offset");
    expect(dcn41_abs_rd(&d, 0x10, 3) == 0x9010u && dcn41_abs_rd(&d, 0x10, 1) == 0xd0u && dcn41_abs(&d, 0x10, 1) == DCN41_BAD_OFFSET && dcn41_abs(&d, 0x10, 3) == DCN41_BAD_OFFSET, "census2: the read twin maps BASE_IDX 1 and 3, the write-side address refuses both");
    expect(absok, "census2: the CLI's absolute address (n48dr_census_abs) equals dcn41_abs_rd for every register");
    expect(blk, "census2: every register lies inside a display block (DCCG clock sources, DIG0..3, RDPCSTX1/2); none elsewhere");
    expect(N48DR_SEG1 == DCN41_SEG1_EXPECTED && N48DR_SEG2 == DCN41_SEG2_EXPECTED && N48DR_SEG3 == DCN41_SEG3_EXPECTED && kSeg[1] == N48DR_SEG1 && kSeg[2] == N48DR_SEG2 && kSeg[3] == N48DR_SEG3, "census2: the CLI's segment bases are the dcn41 layer's");
    // the design's absolute addresses (an internal design note M4 table): OTG_PIXEL_RATE_DIV 0x12f, OTG1_PIXEL_RATE_CNTL 0x144, OTG1_PHYPLL_PIXEL_RATE_CNTL 0x147, SYMCLKC 0x162, PHYCSYMCLK 0x3514, DIG2_STREAM_MAPPER 0x53cf, RDPCSTX2 0x5fa0..0x5fce
    auto absof = [&](const char *n) -> uint32_t { for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) if (!std::strcmp(n48dr_census2[i].name, n)) return n48dr_census_abs(n48dr_census2[i].base_idx, n48dr_census2[i].off); return 0xdeadbeefu; };
    expect(absof("OTG_PIXEL_RATE_DIV") == 0x12f && absof("OTG1_PIXEL_RATE_CNTL") == 0x144 && absof("OTG1_PHYPLL_PIXEL_RATE_CNTL") == 0x147 && absof("SYMCLKC_CLOCK_ENABLE") == 0x162 && absof("PHYCSYMCLK_CLOCK_CNTL") == 0x3514 &&
           absof("DIG2_STREAM_MAPPER_CONTROL") == 0x53cf && absof("RDPCSTX2_RDPCSTX_CNTL") == 0x5fa0 && absof("RDPCSTX2_RDPCS_TX_PLL_UPDATE_DATA_OVRRD") == 0x5fce && absof("DP_DTO0_PHASE") == 0x141, "census2: absolute addresses equal the design memo's");
    // the page reads: every register of the page exactly once in order, the values land in their slots (pages 2..7), nothing else
    for (uint32_t pg = 2; pg < 8; pg++) { CensusEnv ce; uint64_t v[13]; const uint32_t st = n48dr::census_page(ce, pg, v);
      const uint32_t n = n48dr_census_in_page(pg);
      expect(st == N48DR_OK && ce.reads.size() == n, "census2 page: one read per register");
      bool seq = true, pk = (v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == pg && ((v[0] >> 16) & 0xFF) == n;
      for (uint32_t i = 0; i < ce.reads.size() && i < n; i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); seq = seq && r == &n48dr_census2[(pg - 2) * 24 + i] && ce.reads[i].first == r->base_idx && ce.reads[i].second == r->off; }
      for (uint32_t i = 0; i < n; i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); const uint32_t got = (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))); pk = pk && got == ((r->base_idx << 28) | r->off | 0x01000000u); }
      expect(seq, "census2 page: the registers read are the table's, in order");
      expect(pk, "census2 page: the values land in their slots");
      expect(n48dr_census_reg(pg, n) == nullptr, "census2 page: no register past the page"); }
    // the pages together cover the table exactly once
    { std::set<const n48dr_reg *> seen; for (uint32_t pg = 2; pg < 8; pg++) for (uint32_t i = 0; i < n48dr_census_in_page(pg); i++) seen.insert(n48dr_census_reg(pg, i)); expect_u("census2: the pages cover every register exactly once", seen.size(), N48DR_CENSUS2_COUNT); }
}

// ---- (0.0.630, M4c) the third census table: pages 8 and 9 -------------------------------------------------------------------------------------------------------------------------------------------
static void f_census3() {
    { bool named = true; for (uint32_t i = 0; i < N48DR_CENSUS3_COUNT; i++) named = named && n48dr_census3[i].name && n48dr_census3[i].name[0] && n48dr_census3[i].off != 0;
      expect(named, "census3: every one of the 25 slots is filled (a dropped register leaves an empty slot)"); if (!named) return; }
    expect_u("census3: registers", N48DR_CENSUS3_COUNT, 25);
    expect_u("census3: page 8 holds 23", N48DR_CENSUS3_PAGE8_COUNT, 23);
    expect_u("census3: page 9 holds 2", N48DR_CENSUS3_PAGE9_COUNT, 2);
    // the brief's register list, by name, with the absolute BAR5 dword (segment base + Linux offset) pinned
    struct Exp { const char *n; uint32_t base, off, abs; };
    static const Exp want[25] = {
        { "PHYPLLA_PIXCLK_RESYNC_CNTL", 1, 0x0040, 0x100 }, { "PHYPLLB_PIXCLK_RESYNC_CNTL", 1, 0x0041, 0x101 }, { "PHYPLLC_PIXCLK_RESYNC_CNTL", 1, 0x0042, 0x102 }, { "PHYPLLD_PIXCLK_RESYNC_CNTL", 1, 0x0043, 0x103 },
        { "UNIPHYB_LINK_CNTL", 2, 0x286f, 0x5d2f }, { "UNIPHYB_CHANNEL_XBAR_CNTL", 2, 0x2870, 0x5d30 }, { "UNIPHYC_LINK_CNTL", 2, 0x2871, 0x5d31 }, { "UNIPHYC_CHANNEL_XBAR_CNTL", 2, 0x2872, 0x5d32 },
        { "HPD2_DC_HPD_INT_STATUS", 2, 0x176a, 0x4c2a }, { "HPD3_DC_HPD_INT_STATUS", 2, 0x1772, 0x4c32 },
        { "DIG1_HDMI_CONTROL", 2, 0x21c2, 0x5682 }, { "DIG2_HDMI_CONTROL", 2, 0x22e6, 0x57a6 }, { "DP1_DP_LINK_CNTL", 2, 0x2242, 0x5702 }, { "DP2_DP_LINK_CNTL", 2, 0x2366, 0x5826 },
        { "DIG2_DIG_BE_EN_CNTL", 2, 0x2305, 0x57c5 },
        { "OTG0_OTG_CLOCK_CONTROL", 2, 0x1b84, 0x5044 }, { "OTG1_OTG_CLOCK_CONTROL", 2, 0x1c04, 0x50c4 },
        { "ODM0_OPTC_INPUT_CLOCK_CONTROL", 2, 0x1ad0, 0x4f90 }, { "ODM1_OPTC_INPUT_CLOCK_CONTROL", 2, 0x1ae0, 0x4fa0 },
        { "OTG0_OTG_STATUS", 2, 0x1b49, 0x5009 }, { "OTG1_OTG_STATUS", 2, 0x1bc9, 0x5089 },
        { "OTG0_OTG_MASTER_UPDATE_LOCK", 2, 0x1b89, 0x5049 }, { "OTG1_OTG_MASTER_UPDATE_LOCK", 2, 0x1c09, 0x50c9 },
        { "OTG0_OTG_UPDATE_LOCK", 2, 0x1b5b, 0x501b }, { "OTG1_OTG_UPDATE_LOCK", 2, 0x1bdb, 0x509b } };
    bool same = true;
    for (uint32_t i = 0; i < 25; i++) same = same && !std::strcmp(n48dr_census3[i].name, want[i].n) && n48dr_census3[i].base_idx == want[i].base && n48dr_census3[i].off == want[i].off && n48dr_census_abs(want[i].base, want[i].off) == want[i].abs;
    expect(same, "census3: the 25 registers, their order, BASE_IDX, offset and absolute BAR5 dword are exactly the brief's");
    std::set<std::string> names; std::set<std::pair<uint32_t, uint32_t>> addrs;
    for (uint32_t i = 0; i < N48DR_CENSUS3_COUNT; i++) { names.insert(n48dr_census3[i].name); addrs.insert({ n48dr_census3[i].base_idx, n48dr_census3[i].off }); }
    expect(names.size() == 25 && addrs.size() == 25, "census3: names and addresses unique");
    // every register resolves through the READ twin to seg + offset
    
    struct dcn41_dev d; expect(dcn41_dev_init(&d, nullptr, ro_rd, ro_wr, ro_dl, kSeg, 0x100000u, 0) == DCN41_OK, "census3: dcn41_dev_init on the card's own bases");
    bool res = true; for (uint32_t i = 0; i < N48DR_CENSUS3_COUNT; i++) res = res && dcn41_abs_rd(&d, n48dr_census3[i].off, n48dr_census3[i].base_idx) == n48dr_census_abs(n48dr_census3[i].base_idx, n48dr_census3[i].off);
    expect(res, "census3: every register resolves through dcn41_abs_rd to seg + offset");
    // pages 8 and 9: one READ per register, in table order (the environment has no write member at all), the values in their slots, nothing past the page
    for (uint32_t pg = 8; pg < 10; pg++) { CensusEnv ce; uint64_t v[13]; const uint32_t st = n48dr::census_page(ce, pg, v);
      const uint32_t n = n48dr_census_in_page(pg), first = pg == 8 ? 0 : 23;
      expect(st == N48DR_OK && ce.reads.size() == n, "census3 page: one read per register");
      bool seq = true, pk = (v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == pg && ((v[0] >> 16) & 0xFF) == n;
      for (uint32_t i = 0; i < ce.reads.size() && i < n; i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); seq = seq && r == &n48dr_census3[first + i] && ce.reads[i].first == r->base_idx && ce.reads[i].second == r->off; }
      for (uint32_t i = 0; i < n; i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); const uint32_t got = (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))); pk = pk && got == ((r->base_idx << 28) | r->off | 0x01000000u); }
      expect(seq, "census3 page: the registers read are the table's, in order");
      expect(pk, "census3 page: the values land in their slots");
      expect(n48dr_census_reg(pg, n) == nullptr, "census3 page: no register past the page"); }
    { std::set<const n48dr_reg *> seen; for (uint32_t pg = 8; pg < 10; pg++) for (uint32_t i = 0; i < n48dr_census_in_page(pg); i++) seen.insert(n48dr_census_reg(pg, i)); expect_u("census3: the two pages cover every register exactly once", seen.size(), N48DR_CENSUS3_COUNT); }
    { CensusEnv ce; uint64_t v[13]; expect(n48dr::census_page(ce, 60, v) == N48DR_BAD_ARG && ce.reads.empty(), "census3: page 60 (past the last page) is refused with no read"); }
    expect(n48dr_census_arg_ok(8) && n48dr_census_arg_ok(9) && n48dr_census_arg_ok(10) && n48dr_census_arg_ok(42) && n48dr_census_arg_ok(43) && n48dr_census_arg_ok(44) && n48dr_census_arg_ok(59) && !n48dr_census_arg_ok(60), "census3: the legal pages are 0..59 (0.0.635: page 43 is the rest of the DP watch; 0.0.654: pages 44..59 are the monitor A census)");
}

static void f_census45();
static void f_census7();
static void f_census() {
    expect_u("census: 40 registers", N48DR_CENSUS_COUNT, 40);
    expect_u("census: 24 per page, 2 pages", N48DR_CENSUS_PER_PAGE * N48DR_CENSUS_PAGES, 48);
    expect(n48dr_census_in_page(0) == 24 && n48dr_census_in_page(1) == 16 && n48dr_census_in_page(N48DR_CENSUS_ALL_PAGES) == 0, "census: page sizes 24 / 16 / (past the last page) 0");
    std::set<std::string> names; std::set<std::pair<int, int>> addrs;
    for (uint32_t i = 0; i < N48DR_CENSUS_COUNT; i++) { names.insert(n48dr_census[i].name); addrs.insert({ n48dr_census[i].base_idx, n48dr_census[i].off }); }
    expect(names.size() == 40 && addrs.size() == 40, "census: names and addresses unique");
    auto has = [&](const char *n) { for (uint32_t i = 0; i < N48DR_CENSUS_COUNT; i++) if (!std::strcmp(n48dr_census[i].name, n)) return true; return false; };
    expect(has("DOMAIN0_PG_STATUS") && has("DOMAIN3_PG_STATUS") && has("DIG0_DIG_BE_EN_CNTL") && has("DIG3_DIG_BE_EN_CNTL") && has("DIG0_DIG_FE_CNTL") && has("DIG3_DIG_FE_CNTL") && has("OTG0_OTG_CONTROL") && has("OTG3_OTG_CONTROL") &&
           has("OTG0_OTG_STATUS_FRAME_COUNT") && has("OTG3_OTG_STATUS_FRAME_COUNT") && has("OTG0_PIXEL_RATE_CNTL") && has("OTG3_PIXEL_RATE_CNTL") && has("PHYPLLA_PIXCLK_RESYNC_CNTL") && has("PHYPLLG_PIXCLK_RESYNC_CNTL") && has("DC_GPIO_HPD_Y"),
           "census: the brief's registers are all there");
    // frame counter slots 16..19 (the CLI's delta reads them by index)
    expect(!std::strcmp(n48dr_census[16].name, "OTG0_OTG_STATUS_FRAME_COUNT") && !std::strcmp(n48dr_census[19].name, "OTG3_OTG_STATUS_FRAME_COUNT"), "census: the frame counters are entries 16..19 (the CLI's counting check)");
    // the page reads: every register of the page exactly once, nothing else
    for (uint32_t pg = 0; pg < 2; pg++) { CensusEnv ce; uint64_t v[13]; const uint32_t st = n48dr::census_page(ce, pg, v);
      expect(st == N48DR_OK && ce.reads.size() == n48dr_census_in_page(pg), "census page: one read per register");
      bool seq = true; for (uint32_t i = 0; i < ce.reads.size(); i++) seq = seq && ce.reads[i].first == n48dr_census[pg * 24 + i].base_idx && ce.reads[i].second == n48dr_census[pg * 24 + i].off;
      expect(seq, "census page: the registers read are the table's, in order");
      bool pk = (v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == pg && ((v[0] >> 16) & 0xFF) == n48dr_census_in_page(pg);
      for (uint32_t i = 0; i < n48dr_census_in_page(pg); i++) { const uint32_t got = (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))); pk = pk && got == ((n48dr_census[pg * 24 + i].base_idx << 28) | n48dr_census[pg * 24 + i].off | 0x01000000u); }
      expect(pk, "census page: the values land in their slots"); }
    { CensusEnv ce; uint64_t v[13]; expect(n48dr::census_page(ce, N48DR_CENSUS_ALL_PAGES, v) == N48DR_BAD_ARG && ce.reads.empty(), "census: the first page past the M4a pages is refused with no read"); }
    expect(!n48dr_census_arg_ok(N48DR_CENSUS_ALL_PAGES) && n48dr_census_arg_ok(N48DR_CENSUS_ALL_PAGES - 1) && n48dr_census_arg_ok(2) && !n48dr_census_arg_ok(~0ull) && !n48dr_census_arg_ok(1ull << 32), "census: the legal arguments are pages 0..7");
    f_census2();
    f_census3();
    f_census45();
    f_census7();
}


// ---- (0.0.634, M4a) the plane census: page 10 (the DP watch, n48dr_census4) and pages 11..42 (n48dr_census5) -------------------------------------------------------------------------------------------
static bool plane_family_ok(const std::string &n) {   // the families the M4a list names, by name prefix (the header is the authority for each entry; this is the second, independent witness that nothing else was pulled in)
    static const char *const ok[] = { "DCHUBBUB_COMPBUF_CTRL", "DCHUBBUB_DET0_CTRL", "DCHUBBUB_DET1_CTRL", "DCHUBBUB_DET2_CTRL", "DCHUBBUB_DET3_CTRL", "DCHUBBUB_GLOBAL_TIMER_CNTL", "DPPCLK_CTRL", "DPPCLK0_DTO_PARAM", "DPPCLK1_DTO_PARAM", "DPPCLK2_DTO_PARAM", "DPPCLK3_DTO_PARAM", "DPPCLK_DTO_CTRL",
        "DCN_VM_FAULT_", "HUBPREQ0_DCN_VM_", "HUBPREQ1_DCN_VM_", "HUBPREQ2_DCN_VM_", "HUBPREQ0_VMID_SETTINGS_0", "HUBPREQ1_VMID_SETTINGS_0", "HUBPREQ2_VMID_SETTINGS_0", "HUBP0_", "HUBPREQ0_", "HUBPRET0_", "HUBP2_", "HUBPREQ2_", "HUBPRET2_", "DPP_TOP0_", "CNVC_CFG0_", "DSCL0_", "CM0_", "DPP_TOP2_", "CNVC_CFG2_", "DSCL2_", "CM2_",
        "MPCC0_MPCC_", "MPCC1_MPCC_", "MPCC2_MPCC_", "MPC_OUT0_MUX", "MPC_OUT0_DENORM_", "MPC_OUT0_CSC_MODE", "MPC_OUT2_MUX", "MPC_OUT2_DENORM_", "MPC_OUT2_CSC_MODE", "OTG2_OTG_GLOBAL_CONTROL2" };
    for (const char *p : ok) if (n.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}
static void f_census45() {
    // sizes and the page map
    expect_u("census4: registers", N48DR_CENSUS4_COUNT, 9);
    expect_u("census5: registers", N48DR_CENSUS5_COUNT, 762);
    expect_u("census5: pages", N48DR_CENSUS5_PAGES, 32);
    expect(N48DR_CENSUS4_PAGE == 10 && N48DR_CENSUS5_FIRST_PAGE == 11 && N48DR_CENSUS6_PAGE == 43 && N48DR_CENSUS_ALL_PAGES == 60 && N48DR_CENSUS5_FIRST_PAGE + N48DR_CENSUS5_PAGES == N48DR_CENSUS6_PAGE && N48DR_CENSUS6_PAGE + 1u == N48DR_CENSUS7_FIRST_PAGE && N48DR_CENSUS7_FIRST_PAGE + N48DR_CENSUS7_PAGES == N48DR_CENSUS_ALL_PAGES, "census45: page 10 = the DP watch, pages 11..42 = the plane census, page 43 = the rest of the DP watch (census6), pages 44..59 = the monitor A census (census7), 60 pages in all");
    { bool sizes = n48dr_census_in_page(10) == 9 && n48dr_census_in_page(11) == 24 && n48dr_census_in_page(41) == 24 && n48dr_census_in_page(42) == 762 - 31 * 24 && n48dr_census_in_page(43) == N48DR_CENSUS6_COUNT && n48dr_census_in_page(44) == 24; for (uint32_t pg = 11; pg < 42; pg++) sizes = sizes && n48dr_census_in_page(pg) == 24;
      expect(sizes && 31 * 24 + 18 == 762, "census45: page 10 holds 9, pages 11..41 hold 24, page 42 holds the last 18"); }
    { bool named = true; for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) named = named && n48dr_census4[i].name && n48dr_census4[i].name[0];   /* offset 0 is a real register (MPCC0_MPCC_TOP_SEL, BASE_IDX 3) */
      for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) named = named && n48dr_census5[i].name && n48dr_census5[i].name[0];
      expect(named, "census45: every slot of both tables is filled (a dropped register leaves an empty slot)"); if (!named) return; }
    // unique names and addresses inside each table (a duplicate or a copy-pasted BASE_IDX is caught here and by the header check below)
    { std::set<std::string> n4, n5; std::set<std::pair<int, int>> a4, a5;
      for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) { n4.insert(n48dr_census4[i].name); a4.insert({ n48dr_census4[i].base_idx, n48dr_census4[i].off }); }
      for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) { n5.insert(n48dr_census5[i].name); a5.insert({ n48dr_census5[i].base_idx, n48dr_census5[i].off }); }
      expect(n4.size() == N48DR_CENSUS4_COUNT && a4.size() == N48DR_CENSUS4_COUNT, "census4: names and addresses unique");
      expect(n5.size() == N48DR_CENSUS5_COUNT && a5.size() == N48DR_CENSUS5_COUNT, "census5: names and addresses unique (762 of each)"); }
    // EVERY entry against Linux's header: name -> offset and BASE_IDX
    const std::string h = linux_hdr("dcn_4_1_0_offset.h");
    expect(!h.empty(), "census45: Linux's dcn_4_1_0_offset.h is readable");
    if (h.empty()) return;
    for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) expect_reg(h, n48dr_census4[i].name, n48dr_census4[i].off, n48dr_census4[i].base_idx);
    for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) expect_reg(h, n48dr_census5[i].name, n48dr_census5[i].off, n48dr_census5[i].base_idx);
    for (uint32_t i = 0; i < N48DR_CENSUS6_COUNT; i++) expect_reg(h, n48dr_census6[i].name, n48dr_census6[i].off, n48dr_census6[i].base_idx);      // 0.0.635: census6 (page 43)
    // the read allowlist: every entry resolves through the REAL read-only twin to seg + offset inside the BAR5 window, in a family the M4a list names, outside every hard-denied window
    struct dcn41_dev d; expect(dcn41_dev_init(&d, nullptr, ro_rd, ro_wr, ro_dl, kSeg, 0x100000u, 0) == DCN41_OK, "census45: dcn41_dev_init on the card's own bases");
    { bool res = true, fam = true, baseok = true, deny = true, absok = true;
      for (int t = 0; t < 2; t++) for (uint32_t i = 0; i < (t ? N48DR_CENSUS5_COUNT : N48DR_CENSUS4_COUNT); i++) {
          const n48dr_reg &r = t ? n48dr_census5[i] : n48dr_census4[i];
          const uint32_t a = dcn41_abs_rd(&d, r.off, r.base_idx);
          res = res && a != DCN41_BAD_OFFSET && a == kSeg[r.base_idx] + r.off && a < 0x100000u; absok = absok && a == n48dr_census_abs(r.base_idx, r.off);
          baseok = baseok && (r.base_idx == 1 || r.base_idx == 2 || r.base_idx == 3);
          fam = fam && plane_family_ok(r.name);
          const int why = dcn41_allow_classify(a, 0x100000u, nullptr, nullptr);
          deny = deny && why != DCN41_ALLOW_INDIRECT && why != DCN41_ALLOW_BLOCK && why != DCN41_ALLOW_WINDOW; }
      expect(res, "census45: every entry resolves through dcn41_abs_rd to seg + offset inside the BAR5 window");
      expect(absok, "census45: the CLI's absolute address (n48dr_census_abs) equals dcn41_abs_rd for every entry");
      expect(baseok, "census45: every BASE_IDX is 1 (DCCG), 2 or 3 (the read mask)");
      expect(fam, "census45: every entry belongs to a family the M4a list names (DCHUBBUB, DPPCLK, DCN_VM_FAULT, HUBP/HUBPREQ/HUBPRET 0 and 2, DPP_TOP/CNVC_CFG/DSCL/CM 0 and 2, MPCC0..2, MPC_OUT0/2, OTG2_GLOBAL_CONTROL2)");
      expect(deny, "census45: no entry lies in a hard-denied window (the indirect MM_INDEX page, SMU / PSP blocks) or beyond the BAR5 window"); }
    // the write side refuses BASE_IDX 1 and 3 entries (dcn41_abs), and NO entry is an instance-guard-writable register of either disp2 instance
    { bool wr = true, guard = true;
      for (int t = 0; t < 2; t++) for (uint32_t i = 0; i < (t ? N48DR_CENSUS5_COUNT : N48DR_CENSUS4_COUNT); i++) {
          const n48dr_reg &r = t ? n48dr_census5[i] : n48dr_census4[i];
          if (r.base_idx != 2) wr = wr && dcn41_abs(&d, r.off, r.base_idx) == DCN41_BAD_OFFSET;
          const uint32_t a = n48dr_census_abs(r.base_idx, r.off);
          // 0.0.635: instance 1 admits NO entry; instance 2 admits an entry ONLY if it is one of its listed plane registers or one of the two DCCG exceptions (the census itself writes nothing: census_page has no write member)
          bool listed2 = a == N48D2_DPPCLK2_DTO_PARAM || a == N48D2_DPPCLK_CTRL;
          for (uint32_t k = 0; k < N48D2_ALLOWED2P_COUNT; k++) listed2 = listed2 || a == n48d2_allowed2p[k];
          for (uint32_t k = 0; k < N48D2_ALLOWED2_COUNT; k++) listed2 = listed2 || a == n48d2_allowed2[k];
          bool listed1 = a == N48D2_DPPCLK1_DTO_PARAM || a == N48D2_DPPCLK_CTRL;   // 0.0.655: instance 1 now has plane registers and the two DCCG exceptions
          for (uint32_t k = 0; k < N48D2_ALLOWED1P_COUNT; k++) listed1 = listed1 || a == n48d2_allowed1p[k];
          for (uint32_t k = 0; k < N48D2_ALLOWED_COUNT; k++) listed1 = listed1 || a == n48d2_allowed[k];
          guard = guard && ((n48d2_guard_addr_i(N48D2_INST_MONA, a, nullptr) == N48D2_G_OK) == listed1) && ((n48d2_guard_addr_i(N48D2_INST_MONB, a, nullptr) == N48D2_G_OK) == listed2); }
      expect(wr, "census45: the write-side address twin refuses every BASE_IDX 1 / 3 entry");
      expect(guard, "census45: under instance 1's guard ONLY its listed registers (timing, plane, the two DCCG exceptions) are writable, and under instance 2's ONLY its listed plane registers and the two DCCG exceptions (the census itself has no write path)"); }
    // completeness: the table holds EVERY header register of each named block, except the four data ports; and the data ports are real header registers (so the exclusion is a decision, not an accident)
    { auto cnt = [&](const std::string &pre) { size_t n = 0, pos = 0; const std::string key = "#define reg" + pre; while ((pos = h.find(key, pos)) != std::string::npos) { const size_t e = h.find_first_of(" \t", pos + 11); const std::string nm = h.substr(pos + 11, e - pos - 11); if (nm.size() < 9 || nm.compare(nm.size() - 9, 9, "_BASE_IDX") != 0) ++n; pos = e; } return n; };
      auto tcnt = [&](const std::string &pre) { size_t n = 0; for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) if (std::string(n48dr_census5[i].name).compare(0, pre.size(), pre) == 0) ++n; return n; };
      for (const char *pre : { "HUBP0_", "HUBPREQ0_", "HUBPRET0_", "HUBP2_", "HUBPREQ2_", "HUBPRET2_", "DPP_TOP0_", "CNVC_CFG0_", "DPP_TOP2_", "CNVC_CFG2_", "MPCC0_MPCC_", "MPCC1_MPCC_", "MPCC2_MPCC_" })
          expect(tcnt(pre) == cnt(pre), (std::string("census5: the whole header block ") + pre + " is read, register for register (" + std::to_string(cnt(pre)) + ")").c_str());
      const char *const dports[] = { "DSCL0_SCL_COEF_RAM_TAP_DATA", "DSCL0_ISHARP_DELTA_DATA", "CM0_CM_GAMCOR_LUT_DATA", "CM0_CM_TEST_DEBUG_DATA", "DSCL2_SCL_COEF_RAM_TAP_DATA", "DSCL2_ISHARP_DELTA_DATA", "CM2_CM_GAMCOR_LUT_DATA", "CM2_CM_TEST_DEBUG_DATA" };
      for (const char *pre : { "DSCL0_", "CM0_", "DSCL2_", "CM2_" }) expect(tcnt(pre) + 2 == cnt(pre), (std::string("census5: ") + pre + " is the whole block minus its two data ports").c_str());
      for (const char *dp : dports) { uint32_t o = 0, b = 0; bool inT = false; for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) if (!std::strcmp(n48dr_census5[i].name, dp)) inT = true;
          expect(linux_reg(h, dp, &o, &b) && !inT, (std::string("census5: the data port ") + dp + " exists in the header and is NOT read").c_str()); }
      // DPP2 is DPP0's twin: same names with 0 -> 2, same order, same offset distance per block
      for (const char *pre : { "DSCL", "CM", "DPP_TOP", "CNVC_CFG" }) { std::vector<std::string> a, b2; for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) { const std::string n = n48dr_census5[i].name; if (n.compare(0, std::strlen(pre) + 2, std::string(pre) + "0_") == 0) a.push_back(n.substr(std::strlen(pre) + 2)); if (n.compare(0, std::strlen(pre) + 2, std::string(pre) + "2_") == 0) b2.push_back(n.substr(std::strlen(pre) + 2)); }
          expect(!a.empty() && a == b2, (std::string("census5: ") + pre + "0 and " + pre + "2 list the same registers in the same order (DPP2 is DPP0's twin)").c_str()); } }
    // the brief's named registers, with the design's absolute addresses (CONFIRMED against the header: 0xc0 / 0x34c0 + offset)
    { auto abs_of = [&](const char *nm) -> uint32_t { for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) if (!std::strcmp(n48dr_census5[i].name, nm)) return n48dr_census_abs(n48dr_census5[i].base_idx, n48dr_census5[i].off); return 0xdeadbeefu; };
      expect(abs_of("DCHUBBUB_COMPBUF_CTRL") == 0x397a && abs_of("DCHUBBUB_DET0_CTRL") == 0x397b && abs_of("DCHUBBUB_DET1_CTRL") == 0x397c && abs_of("DCHUBBUB_DET2_CTRL") == 0x397d && abs_of("DCHUBBUB_DET3_CTRL") == 0x397e, "census5: COMPBUF_CTRL 0x397a, DET0..3_CTRL 0x397b..0x397e (the design's addresses)");
      expect(abs_of("DCN_VM_FAULT_STATUS") == 0x3a8c && abs_of("DPPCLK_CTRL") == 0x168 && abs_of("DPPCLK0_DTO_PARAM") == 0x159 && abs_of("DPPCLK1_DTO_PARAM") == 0x15a && abs_of("DPPCLK2_DTO_PARAM") == 0x15b && abs_of("DPPCLK_DTO_CTRL") == 0x176 && abs_of("OTG2_OTG_GLOBAL_CONTROL2") == 0x5150, "census5: DCN_VM_FAULT_STATUS 0x3a8c, DPPCLK_CTRL 0x168, DPPCLK0..2_DTO_PARAM 0x159..0x15b, DPPCLK_DTO_CTRL 0x176 (the DTO control register's real name), OTG2_GLOBAL_CONTROL2 0x5150");
      expect(abs_of("HUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW_ADDR") == 0x34c0 + 0x62c && abs_of("HUBPREQ0_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR") == 0x34c0 + 0x62d && abs_of("HUBPREQ0_DCN_VM_MX_L1_TLB_CNTL") == 0x34c0 + 0x63a && abs_of("HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL") != 0xdeadbeefu && abs_of("HUBPREQ2_VMID_SETTINGS_0") != 0xdeadbeefu, "census5: HUBPREQ0 aperture low / high / MX_L1_TLB at offsets 0x62c / 0x62d / 0x63a (the design's; the header names them *_ADDR), HUBPREQ1 / 2 present");
      expect(abs_of("MPC_OUT0_MUX") == 0x92f2 && abs_of("MPC_OUT2_MUX") == 0x92fa && abs_of("MPCC2_MPCC_TOP_SEL") == 0x902a && abs_of("DCHUBBUB_GLOBAL_TIMER_CNTL") == 0x34c0 + 0x527 && abs_of("HUBP2_DCHUBP_CNTL") == 0x3c6c, "census5: MPC_OUT0_MUX 0x92f2, MPC_OUT2_MUX 0x92fa, MPCC2_TOP_SEL 0x902a, HUBP2_DCHUBP_CNTL 0x3c6c"); }
    // order: the instance-2 blocks (HUBP2 / HUBPREQ2 / HUBPRET2 / DPP2) are read LAST, after every instance-0 / MPC / global register, so a hang on a clock-gated pipe would show at the end of `dispcensus plane`
    { size_t lastOther = 0, firstTwo = N48DR_CENSUS5_COUNT; for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) { const std::string n = n48dr_census5[i].name;
          const bool two = n.compare(0, 6, "HUBP2_") == 0 || n.compare(0, 9, "HUBPREQ2_") == 0 || n.compare(0, 9, "HUBPRET2_") == 0 || n.compare(0, 8, "DPP_TOP2") == 0 || n.compare(0, 9, "CNVC_CFG2") == 0 || n.compare(0, 5, "DSCL2") == 0 || n.compare(0, 4, "CM2_") == 0;
          const bool vm2 = n.compare(0, 14, "HUBPREQ2_DCN_V") == 0 || n.compare(0, 24, "HUBPREQ2_VMID_SETTINGS_0") == 0;
          if (two && !vm2) { if (i < firstTwo) firstTwo = i; } else lastOther = i; }
      expect(firstTwo < N48DR_CENSUS5_COUNT && lastOther < firstTwo, "census5: the HUBP2 / HUBPREQ2 / HUBPRET2 / DPP2 blocks are the LAST 4 blocks of the table (after MPCC, MPC_OUT, OTG2_GLOBAL_CONTROL2)"); }
    // the DP watch: the nine registers n48d2_dpx_regs compares are page 10, in the same order, with the same absolute address (the CLI reads page 10 and uses the kext's masks by index)
    { bool same = true; for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) same = same && n48dr_census_abs(n48dr_census4[i].base_idx, n48dr_census4[i].off) == n48d2_dpx_regs[i].abs;
      expect(N48D2_DPX_REGS == 15u && N48DR_CENSUS4_COUNT == 9u && same, "census4: page 10's nine registers are rows 0..8 of n48d2_dpx_regs[], in order, same absolute dwords (one list for the kext's check and the CLI's)");
      bool same6 = n48dr_census_abs(n48dr_census4[4].base_idx, n48dr_census4[4].off) == n48d2_dpx_regs[9].abs;            // row 9 is HUBP0_DCHUBP_CNTL again (page 10's row 4)
      for (uint32_t i = 0; i < 5; i++) same6 = same6 && n48dr_census_abs(n48dr_census6[i].base_idx, n48dr_census6[i].off) == n48d2_dpx_regs[10 + i].abs;      // rows 10..14 = census6[0..4]
      same6 = same6 && n48dr_census_abs(n48dr_census6[5].base_idx, n48dr_census6[5].off) == N48D2_HUBPREQ0_PRIMARY_HIGH && N48DR_CENSUS6_COUNT == 6u;           // + the primary HIGH dword
      expect(same6, "census6: page 43 holds rows 10..14 of n48d2_dpx_regs[] in order and then the HUBP0 primary HIGH dword; row 9 is page 10's HUBP0_DCHUBP_CNTL again"); }
    // the page reads: one READ per register in table order, the values in their slots, nothing past the page; the pages cover every register once
    { std::set<const n48dr_reg *> seen;
      for (uint32_t pg = 10; pg < 44; pg++) { CensusEnv ce; uint64_t v[13]; const uint32_t st = n48dr::census_page(ce, pg, v); const uint32_t n = n48dr_census_in_page(pg);
          bool seq = st == N48DR_OK && ce.reads.size() == n, pk = (v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == pg && ((v[0] >> 16) & 0xFF) == n;
          for (uint32_t i = 0; i < n && i < ce.reads.size(); i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); seen.insert(r); seq = seq && r == (pg == 10 ? &n48dr_census4[i] : pg == 43 ? &n48dr_census6[i] : &n48dr_census5[(pg - 11) * 24 + i]) && ce.reads[i].first == r->base_idx && ce.reads[i].second == r->off;
              pk = pk && (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))) == ((r->base_idx << 28) | r->off | 0x01000000u); }
          expect(seq, (std::string("census45 page ") + std::to_string(pg) + ": one read per register, the table's, in order").c_str());
          expect(pk && n48dr_census_reg(pg, n) == nullptr, (std::string("census45 page ") + std::to_string(pg) + ": the values land in their slots, no register past the page").c_str()); }
      expect_u("census45: pages 10..43 cover every register of the three tables exactly once", seen.size(), N48DR_CENSUS4_COUNT + N48DR_CENSUS5_COUNT + N48DR_CENSUS6_COUNT);
      CensusEnv ce; uint64_t v[13]; expect(n48dr::census_page(ce, 60, v) == N48DR_BAD_ARG && ce.reads.empty(), "census45: page 60 is refused with no read"); }
    // no write path: the census sequence has no write member at all, and neither table is referenced by any file that writes a register
    { const std::string fl = slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_dispread_flow.h");
      const size_t c0 = fl.find("inline uint32_t census_page(E &e, uint32_t page, uint64_t *v)"), c1 = c0 == std::string::npos ? c0 : fl.find("// ---- region4read", c0);
      const std::string body = c0 == std::string::npos || c1 == std::string::npos ? "" : fl.substr(c0, c1 - c0);
      expect(!body.empty() && body.find("e.rd(") != std::string::npos && body.find(".wr(") == std::string::npos && body.find("write") == std::string::npos && body.find("WREG") == std::string::npos, "census45: census_page uses e.rd only (no write member exists in the environment it is given)");
      for (const char *f : { "/src/navi48-bringup/src/dcn/navi48_disp2.h", "/src/navi48-bringup/src/dcn/navi48_disp2_flow.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd_flow.h", "/src/navi48-bringup/src/dcn/navi48_dcn.cpp", "/src/navi48-bringup/src/Navi48Bringup.cpp", "/src/dcn41/dcn41_allow.c", "/src/dcn41/dcn41_hubp.c" }) {
          const std::string t = strip_line_comments(slurp(g_root + f)); expect(!t.empty() && t.find("n48dr_census4") == std::string::npos && t.find("n48dr_census5") == std::string::npos, (std::string("census45: ") + f + " never references the census tables").c_str()); } }
    // the CLI names the pages: `plane` (10..42) and `dpwatch` (10), printing name / BASE_IDX / absolute dword / value, flushing every page
    { const std::string cli = slurp(g_root + "/tools/pc/navi48test.c");
      expect(cli.find("!strcmp(a1, \"plane\") || !strcmp(a1, \"dpwatch\")") != std::string::npos && cli.find("for (uint32_t p = N48DR_CENSUS4_PAGE; p <= last && rc == 0; p++) rc = dc_print_page(p);") != std::string::npos && cli.find("const uint32_t last = !strcmp(a1, \"dpwatch\") ? N48DR_CENSUS4_PAGE : N48DR_CENSUS6_PAGE;") != std::string::npos &&
             cli.find("printf(\"  %-52s (BASE_IDX %u, abs %#07x) = %#010x\\n\", r->name, r->base_idx, n48dr_census_abs(r->base_idx, r->off)") != std::string::npos && cli.find("fflush(stdout);       /* a hang on a clock-gated pipe must show which page it was */") != std::string::npos,
             "census45: the CLI's `dispcensus plane` walks pages 10..42 in order by name, `dpwatch` is page 10, every page is flushed"); }
}

// ---- (0.0.654) the monitor A (instance 1) census: pages 44..59 (n48dr_census7) -----------------------------------------------------------------------------------------------------------------------------
static bool mona_family_ok(const std::string &n) {   // the second, independent witness: the families the spec's "Census first" part names, instance 1 only
    static const char *const ok[] = { "HUBP1_", "HUBPREQ1_", "HUBPRET1_", "DPP_TOP1_", "CNVC_CFG1_", "DSCL1_", "CM1_", "MPC_OUT1_MUX", "MPC_OUT1_DENORM_", "MPC_OUT1_CSC_MODE", "MPCC_OGAM1_MPCC_OGAM_CONTROL", "MPCC_MCM1_MPCC_MCM_SHAPER_CONTROL", "MPCC_MCM1_MPCC_MCM_3DLUT_MODE", "MPCC_MCM1_MPCC_MCM_1DLUT_CONTROL",
        "OTG1_OTG_GLOBAL_CONTROL2", "OTG1_OTG_MASTER_UPDATE_LOCK", "OTG1_OTG_VSTARTUP_PARAM", "OTG1_OTG_VUPDATE_PARAM", "OTG1_OTG_VREADY_PARAM", "OTG1_OTG_H_TOTAL", "OTG1_OTG_H_BLANK_START_END", "OTG1_OTG_V_TOTAL", "OTG1_OTG_V_BLANK_START_END", "ODM1_OPTC_INPUT_GLOBAL_CONTROL", "DIG2_DIG_OUTPUT_CRC_CNTL", "DOMAIN1_PG_STATUS", "DENTIST_DISPCLK_CNTL" };
    for (const char *p : ok) if (n.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}
static void f_census7() {
    expect_u("census7: registers", N48DR_CENSUS7_COUNT, 361);
    expect_u("census7: pages", N48DR_CENSUS7_PAGES, 16);
    { bool sizes = true; for (uint32_t pg = 44; pg < 59; pg++) sizes = sizes && n48dr_census_in_page(pg) == 24;
      expect(sizes && n48dr_census_in_page(59) == 361 - 15 * 24 && n48dr_census_in_page(60) == 0 && 15 * 24 + 1 == 361, "census7: pages 44..58 hold 24, page 59 holds the last 1, page 60 holds none"); }
    { bool named = true; for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) named = named && n48dr_census7[i].name && n48dr_census7[i].name[0] && n48dr_census7[i].off != 0;
      expect(named, "census7: every slot is filled (a dropped register leaves an empty slot)"); if (!named) return; }
    { std::set<std::string> n7; std::set<std::pair<int, int>> a7;
      for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) { n7.insert(n48dr_census7[i].name); a7.insert({ n48dr_census7[i].base_idx, n48dr_census7[i].off }); }
      expect(n7.size() == N48DR_CENSUS7_COUNT && a7.size() == N48DR_CENSUS7_COUNT, "census7: names and addresses unique (361 of each)");
      // no overlap with the other plane tables; the one register shared with the old census3 table (page 9) is OTG1_OTG_MASTER_UPDATE_LOCK, named by the spec
      size_t ov = 0; for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) ov += n7.count(n48dr_census5[i].name);
      for (uint32_t i = 0; i < N48DR_CENSUS4_COUNT; i++) ov += n7.count(n48dr_census4[i].name);
      for (uint32_t i = 0; i < N48DR_CENSUS6_COUNT; i++) ov += n7.count(n48dr_census6[i].name);
      size_t ov3 = 0; for (uint32_t i = 0; i < N48DR_CENSUS3_COUNT; i++) ov3 += n7.count(n48dr_census3[i].name);
      size_t ov2 = 0; for (uint32_t i = 0; i < N48DR_CENSUS2_COUNT; i++) ov2 += n7.count(n48dr_census2[i].name);
      size_t ov1 = 0; for (uint32_t i = 0; i < N48DR_CENSUS_COUNT; i++) ov1 += n7.count(n48dr_census[i].name);
      expect(ov == 0 && ov2 == 0 && ov3 == 1 && n7.count("OTG1_OTG_MASTER_UPDATE_LOCK") == 1 && ov1 == 1 && n7.count("DOMAIN1_PG_STATUS") == 1, "census7: no overlap with census4 / 5 / 6 / 2; only OTG1_OTG_MASTER_UPDATE_LOCK (census3) and DOMAIN1_PG_STATUS (page 0 table) are read twice, on purpose (the spec names both)"); }
    const std::string h = linux_hdr("dcn_4_1_0_offset.h");
    expect(!h.empty(), "census7: Linux's dcn_4_1_0_offset.h is readable");
    if (h.empty()) return;
    for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) expect_reg(h, n48dr_census7[i].name, n48dr_census7[i].off, n48dr_census7[i].base_idx);   // name -> offset and BASE_IDX, every entry
    struct dcn41_dev d; expect(dcn41_dev_init(&d, nullptr, ro_rd, ro_wr, ro_dl, kSeg, 0x100000u, 0) == DCN41_OK, "census7: dcn41_dev_init on the card's own bases");
    { bool res = true, fam = true, baseok = true, deny = true, absok = true, wr = true, g2 = true; std::set<std::string> g1ok;
      for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) {
          const n48dr_reg &r = n48dr_census7[i];
          const uint32_t a = dcn41_abs_rd(&d, r.off, r.base_idx);
          res = res && a != DCN41_BAD_OFFSET && a == kSeg[r.base_idx] + r.off && a < 0x100000u; absok = absok && a == n48dr_census_abs(r.base_idx, r.off);
          baseok = baseok && (r.base_idx == 1 || r.base_idx == 2 || r.base_idx == 3);
          fam = fam && mona_family_ok(r.name);
          const int why = dcn41_allow_classify(a, 0x100000u, nullptr, nullptr);
          deny = deny && why != DCN41_ALLOW_INDIRECT && why != DCN41_ALLOW_BLOCK && why != DCN41_ALLOW_WINDOW;
          if (r.base_idx != 2) wr = wr && dcn41_abs(&d, r.off, r.base_idx) == DCN41_BAD_OFFSET;       // the write-side twin refuses BASE_IDX 1 / 3
          if (n48d2_guard_addr_i(N48D2_INST_MONA, a, nullptr) == N48D2_G_OK) g1ok.insert(r.name);       // instance 1's guard: ONLY its own timing registers (written by the existing `disp2` timing op) are listed
          g2 = g2 && n48d2_guard_addr_i(N48D2_INST_MONB, a, nullptr) != N48D2_G_OK; }                 // instance 2's guard refuses EVERY entry
      expect(res, "census7: every entry resolves through dcn41_abs_rd to seg + offset inside the BAR5 window");
      expect(absok, "census7: the CLI's absolute address (n48dr_census_abs) equals dcn41_abs_rd for every entry");
      expect(baseok, "census7: every BASE_IDX is 1 (DCCG), 2 or 3 (the read mask)");
      expect(fam, "census7: every entry belongs to a family the spec's part 2 names, instance 1 only (HUBP/HUBPREQ/HUBPRET/DPP_TOP/CNVC_CFG/DSCL/CM 1, MPC_OUT1, OGAM1, MCM1, OTG1 x9, ODM1, DIG2 CRC, DOMAIN1, DENTIST)");
      expect(deny, "census7: no entry lies in a hard-denied window (the indirect MM_INDEX page, SMU / PSP blocks) or beyond the BAR5 window");
      expect(wr, "census7: the write-side address twin refuses every BASE_IDX 1 / 3 entry");
      const std::set<std::string> want1 = { "OTG1_OTG_VSTARTUP_PARAM", "OTG1_OTG_VUPDATE_PARAM", "OTG1_OTG_VREADY_PARAM", "OTG1_OTG_H_TOTAL", "OTG1_OTG_H_BLANK_START_END", "OTG1_OTG_V_TOTAL", "OTG1_OTG_V_BLANK_START_END" };
      const std::set<std::string> plane1 = { "DPP_TOP1_DPP_CONTROL", "DSCL1_DSCL_2TAP_CONTROL", "DSCL1_LB_MEMORY_CTRL", "DSCL1_MPC_SIZE", "DSCL1_OTG_H_BLANK", "DSCL1_OTG_V_BLANK", "DSCL1_RECOUT_SIZE", "DSCL1_SCL_HORZ_FILTER_INIT", "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO", "DSCL1_SCL_MODE", "DSCL1_SCL_VERT_FILTER_INIT", "DSCL1_SCL_VERT_FILTER_SCALE_RATIO",
          "HUBP1_DCHUBP_CNTL", "HUBP1_DCHUBP_REQ_SIZE_CONFIG", "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION", "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION", "HUBP1_HUBP_CLK_CNTL", "HUBPREQ1_BLANK_OFFSET_0", "HUBPREQ1_BLANK_OFFSET_1", "HUBPREQ1_DCN_GLOBAL_TTU_CNTL", "HUBPREQ1_DCN_SURF0_TTU_CNTL0", "HUBPREQ1_DCN_TTU_QOS_WM", "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", "HUBPREQ1_DCSURF_SURFACE_PITCH", "HUBPREQ1_DST_DIMENSIONS", "HUBPREQ1_PREFETCH_SETTINGS", "HUBPREQ1_PREFETCH_SETTINGS_C", "HUBPREQ1_REF_FREQ_TO_PIX_FREQ", "HUBPREQ1_VBLANK_PARAMETERS_0",
          "MPC_OUT1_MUX", "ODM1_OPTC_INPUT_GLOBAL_CONTROL", "OTG1_OTG_GLOBAL_CONTROL2", "OTG1_OTG_MASTER_UPDATE_LOCK" };
      std::set<std::string> want1b = want1; want1b.insert(plane1.begin(), plane1.end());
      expect(g1ok == want1b, "census7: under instance 1's guard (n48d2_guard_addr_i, MONA, 0.0.655) exactly the 7 OTG1 timing registers plus the spec's 34 plane registers (HUBP1/HUBPREQ1/DPP1/DSCL1, MPC_OUT1_MUX, ODM1 input, OTG1 GLOBAL_CONTROL2 and MASTER_UPDATE_LOCK) are listed; OGAM1, MCM1, the OTG1 CRC block, DIG2, DOMAIN1 and DENTIST are REFUSED");
      expect(g2, "census7: NO entry is writable under instance 2's guard (n48d2_guard_addr_i, MONB)"); }
    // completeness: every header register of each instance-1 block is read, except the four data ports and (HUBPREQ1) the four VM registers census5 already reads; the data ports are real header registers
    { auto cnt = [&](const std::string &pre) { size_t n = 0, pos = 0; const std::string key = "#define reg" + pre; while ((pos = h.find(key, pos)) != std::string::npos) { const size_t e = h.find_first_of(" \t", pos + 11); const std::string nm = h.substr(pos + 11, e - pos - 11); if (nm.size() < 9 || nm.compare(nm.size() - 9, 9, "_BASE_IDX") != 0) ++n; pos = e; } return n; };
      auto tcnt = [&](const std::string &pre) { size_t n = 0; for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) if (std::string(n48dr_census7[i].name).compare(0, pre.size(), pre) == 0) ++n; return n; };
      for (const char *pre : { "HUBP1_", "HUBPRET1_", "DPP_TOP1_", "CNVC_CFG1_" })
          expect(tcnt(pre) == cnt(pre), (std::string("census7: the whole header block ") + pre + " is read, register for register (" + std::to_string(cnt(pre)) + ")").c_str());
      expect(tcnt("HUBPREQ1_") + 4 == cnt("HUBPREQ1_"), "census7: HUBPREQ1_ is the whole block minus its four VM registers (read by census5)");
      for (const char *pre : { "DSCL1_", "CM1_" }) expect(tcnt(pre) + 2 == cnt(pre), (std::string("census7: ") + pre + " is the whole block minus its two data ports").c_str());
      const char *const dports[] = { "DSCL1_SCL_COEF_RAM_TAP_DATA", "DSCL1_ISHARP_DELTA_DATA", "CM1_CM_GAMCOR_LUT_DATA", "CM1_CM_TEST_DEBUG_DATA" };
      for (const char *dp : dports) { uint32_t o = 0, b = 0; bool inT = false; for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) if (!std::strcmp(n48dr_census7[i].name, dp)) inT = true;
          expect(linux_reg(h, dp, &o, &b) && !inT, (std::string("census7: the data port ") + dp + " exists in the header and is NOT read").c_str()); }
      for (const char *vm : { "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR", "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR", "HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL", "HUBPREQ1_VMID_SETTINGS_0" }) { bool in5 = false, in7 = false;
          for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) if (!std::strcmp(n48dr_census5[i].name, vm)) in5 = true;
          for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) if (!std::strcmp(n48dr_census7[i].name, vm)) in7 = true;
          expect(in5 && !in7, (std::string("census7: ") + vm + " is read by census5 and not repeated").c_str()); }
      // the twin: census7's HUBP/HUBPREQ/HUBPRET/DPP blocks list the same registers as census5's pipe-2 blocks (names with 1 <-> 2), in the same order, at the same offset distance (0xdc / 0x16b) except the two HUBP*_3DLUT_FL_* registers (BASE_IDX 3, stride 1)
      std::vector<std::string> t7, t5; std::vector<std::pair<uint32_t, uint32_t>> o7, o5;
      for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) { const std::string n = n48dr_census7[i].name;
          for (const char *pre : { "HUBPRET", "HUBPREQ", "HUBP", "DPP_TOP", "CNVC_CFG", "DSCL", "CM" }) { const std::string p1 = std::string(pre) + "1_"; if (n.compare(0, p1.size(), p1) == 0) { t7.push_back(std::string(pre) + "X_" + n.substr(p1.size())); o7.push_back({ n48dr_census7[i].base_idx, n48dr_census7[i].off }); break; } } }
      for (uint32_t i = 0; i < N48DR_CENSUS5_COUNT; i++) { const std::string n = n48dr_census5[i].name;
          if (n.compare(0, 14, "HUBPREQ2_DCN_V") == 0 || n.compare(0, 24, "HUBPREQ2_VMID_SETTINGS_0") == 0) continue;
          for (const char *pre : { "HUBPRET", "HUBPREQ", "HUBP", "DPP_TOP", "CNVC_CFG", "DSCL", "CM" }) { const std::string p2 = std::string(pre) + "2_"; if (n.compare(0, p2.size(), p2) == 0) { t5.push_back(std::string(pre) + "X_" + n.substr(p2.size())); o5.push_back({ n48dr_census5[i].base_idx, n48dr_census5[i].off }); break; } } }
      expect(t7.size() == 339 && t7 == t5, "census7: the 339 plane-block registers are census5's pipe-2 registers renamed 2 -> 1, same order");
      bool dist = t7.size() == t5.size(); size_t odd = 0;
      for (size_t i = 0; dist && i < t7.size(); i++) { const uint32_t dd = o5[i].second - o7[i].second; const bool hub = t7[i].compare(0, 4, "HUBP") == 0; const bool fl = t7[i].compare(0, 7, "HUBPX_3") == 0;
          if (fl) { ++odd; dist = dist && o7[i].first == 3 && o5[i].first == 3 && dd == 2; } else dist = dist && o7[i].first == o5[i].first && dd == (hub ? 0xdcu : 0x16bu); }
      expect(dist && odd == 2, "census7: every block register sits 0xdc (HUBP/HUBPREQ/HUBPRET) or 0x16b (DPP) below its pipe-2 twin, BASE_IDX equal; the two HUBP_3DLUT_FL_* registers are the header's BASE_IDX-3 stride-2 exception"); }
    // the named registers, with the spec's absolute addresses (CONFIRMED against the header: 0xc0 / 0x34c0 / 0x9000 + offset)
    { auto abs_of = [&](const char *nm) -> uint32_t { for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) if (!std::strcmp(n48dr_census7[i].name, nm)) return n48dr_census_abs(n48dr_census7[i].base_idx, n48dr_census7[i].off); return 0xdeadbeefu; };
      expect(abs_of("OTG1_OTG_GLOBAL_CONTROL2") == 0x50d0 && abs_of("OTG1_OTG_MASTER_UPDATE_LOCK") == 0x50c9 && abs_of("ODM1_OPTC_INPUT_GLOBAL_CONTROL") == 0x4f9a && abs_of("DIG2_DIG_OUTPUT_CRC_CNTL") == 0x579e, "census7: OTG1_GLOBAL_CONTROL2 0x50d0, MASTER_UPDATE_LOCK 0x50c9, ODM1 0x4f9a, DIG2_DIG_OUTPUT_CRC_CNTL 0x579e (the spec's addresses)");
      expect(abs_of("MPC_OUT1_MUX") == 0x92f6 && abs_of("MPCC_OGAM1_MPCC_OGAM_CONTROL") == 0x90dc && abs_of("MPCC_MCM1_MPCC_MCM_SHAPER_CONTROL") == 0x9503 && abs_of("MPCC_MCM1_MPCC_MCM_3DLUT_MODE") == 0x953a && abs_of("MPCC_MCM1_MPCC_MCM_1DLUT_CONTROL") == 0x9543, "census7: MPC_OUT1_MUX 0x92f6, OGAM1 0x90dc, MCM1 0x9503 / 0x953a / 0x9543 (the spec's addresses)");
      expect(abs_of("HUBP1_DCHUBP_CNTL") == 0x3b90 && abs_of("HUBPREQ1_DCSURF_SURFACE_PITCH") == 0x3ba3 && abs_of("DPP_TOP1_DPP_CONTROL") == 0x42f0 && abs_of("DSCL1_SCL_MODE") == 0x4333 && abs_of("DSCL1_OTG_H_BLANK") == 0x4347, "census7: HUBP1_DCHUBP_CNTL 0x3b90, pitch 0x3ba3, DPP_TOP1 0x42f0, DSCL1 SCL_MODE 0x4333 / OTG_H_BLANK 0x4347 (the spec's addresses)");
      expect(abs_of("OTG1_OTG_H_TOTAL") != 0xdeadbeefu && abs_of("OTG1_OTG_V_BLANK_START_END") != 0xdeadbeefu && abs_of("OTG1_OTG_VSTARTUP_PARAM") != 0xdeadbeefu && abs_of("DOMAIN1_PG_STATUS") == 0x34c0 + 0x83 && abs_of("DENTIST_DISPCLK_CNTL") == 0xc0 + 0x64, "census7: OTG1 timing registers present; DOMAIN1_PG_STATUS = seg2 + 0x83; DENTIST_DISPCLK_CNTL = seg1 0xc0 + 0x64"); }
    // order: the plane blocks (HUBP1 / HUBPREQ1 / HUBPRET1 / DPP1) come LAST (a hang on a clock-gated pipe would show at the end), after the 13 single registers and MPC_OUT1 / OGAM1 / MCM1
    { size_t lastOther = 0, firstBlk = N48DR_CENSUS7_COUNT; for (uint32_t i = 0; i < N48DR_CENSUS7_COUNT; i++) { const std::string n = n48dr_census7[i].name;
          const bool blk = n.compare(0, 6, "HUBP1_") == 0 || n.compare(0, 9, "HUBPREQ1_") == 0 || n.compare(0, 9, "HUBPRET1_") == 0 || n.compare(0, 8, "DPP_TOP1") == 0 || n.compare(0, 9, "CNVC_CFG1") == 0 || n.compare(0, 5, "DSCL1") == 0 || n.compare(0, 4, "CM1_") == 0;
          if (blk) { if (i < firstBlk) firstBlk = i; } else lastOther = i; }
      expect(firstBlk == 22 && lastOther < firstBlk, "census7: 13 singles + 5 MPC_OUT1 + OGAM1 + 3 MCM1 = 22 registers first, then the plane blocks (HUBP1 first)"); }
    // the page reads: one READ per register in table order, the values in their slots, nothing past the page; the pages cover every register once
    { std::set<const n48dr_reg *> seen;
      for (uint32_t pg = 44; pg < 60; pg++) { CensusEnv ce; uint64_t v[13]; const uint32_t st = n48dr::census_page(ce, pg, v); const uint32_t n = n48dr_census_in_page(pg);
          bool seq = st == N48DR_OK && ce.reads.size() == n, pk = (v[0] & 0xFF) == 0 && ((v[0] >> 8) & 0xFF) == pg && ((v[0] >> 16) & 0xFF) == n;
          for (uint32_t i = 0; i < n && i < ce.reads.size(); i++) { const n48dr_reg *r = n48dr_census_reg(pg, i); seen.insert(r); seq = seq && r == &n48dr_census7[(pg - 44) * 24 + i] && ce.reads[i].first == r->base_idx && ce.reads[i].second == r->off;
              pk = pk && (uint32_t)(v[1 + i / 2] >> (32 * (i & 1))) == ((r->base_idx << 28) | r->off | 0x01000000u); }
          expect(seq, (std::string("census7 page ") + std::to_string(pg) + ": one read per register, the table's, in order").c_str());
          expect(pk && n48dr_census_reg(pg, n) == nullptr, (std::string("census7 page ") + std::to_string(pg) + ": the values land in their slots, no register past the page").c_str()); }
      expect_u("census7: pages 44..59 cover every register of the table exactly once", seen.size(), N48DR_CENSUS7_COUNT);
      CensusEnv ce; uint64_t v[13]; expect(n48dr::census_page(ce, 60, v) == N48DR_BAD_ARG && ce.reads.empty(), "census7: page 60 is refused with no read"); }
    // no write path: no file that writes a register references the table, and census_page (reads only) is the single reader of it
    { const std::string fl = slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_dispread_flow.h");
      expect(fl.find("n48dr_census7") == std::string::npos, "census7: the flow never names the table (it reaches it only through n48dr_census_reg inside census_page)");
      for (const char *f : { "/src/navi48-bringup/src/dcn/navi48_disp2.h", "/src/navi48-bringup/src/dcn/navi48_disp2_flow.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd_flow.h", "/src/navi48-bringup/src/dcn/navi48_dcn.cpp", "/src/navi48-bringup/src/Navi48Bringup.cpp", "/src/dcn41/dcn41_allow.c", "/src/dcn41/dcn41_hubp.c", "/src/navi48-bringup/src/amd/native_disp.h", "/src/navi48-bringup/src/amd/native_s1c.h" }) {
          const std::string t = strip_line_comments(slurp(g_root + f)); expect(!t.empty() && t.find("n48dr_census7") == std::string::npos && t.find("N48DR_CENSUS7") == std::string::npos, (std::string("census7: ") + f + " never references the census7 table").c_str()); } }
    // the CLI names the pages: `mona` walks 44..59 by name, `plane` still stops at page 43
    { const std::string cli = slurp(g_root + "/tools/pc/navi48test.c");
      expect(cli.find("if (a1 && !strcmp(a1, \"mona\")) {") != std::string::npos && cli.find("for (uint32_t p = N48DR_CENSUS7_FIRST_PAGE; p < N48DR_CENSUS_ALL_PAGES && rc == 0; p++) rc = dc_print_page(p);") != std::string::npos && cli.find("dispcensus [0|1|2..59|plane|dpwatch|mona]") != std::string::npos &&
             cli.find("printf(\"accel dispcensus %u (%s)\\n\", pg, pg == N48DR_CENSUS4_PAGE ? \"the DP watch\" : pg >= N48DR_CENSUS7_FIRST_PAGE ? \"monitor A instance-1 census page\" : \"plane page\");") != std::string::npos,
             "census7: the CLI's `dispcensus mona` walks pages 44..59 in order by name (dc_print_page: name, BASE_IDX, absolute dword, value)"); }
}

// ---- G: region4read (0.0.624) ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct R4Model {   // a sparse 16 GiB VRAM whose every dword is a function of its address; records every read it is asked for
    std::vector<std::pair<uint64_t, uint32_t>> reads; bool fail = false;
    static uint32_t word(uint64_t addr) { return (uint32_t)(addr * 2654435761ull >> 3) ^ 0xA5000000u ^ (uint32_t)(addr >> 32); }
    bool rd(uint64_t at, uint32_t *dst, uint32_t n) { reads.push_back({ at, n }); if (fail) return false; for (uint32_t i = 0; i < n; i++) dst[i] = word(at + 4ull * i); return true; }
};
static void g_region4() {
    // the range rule against an independent reference, every offset 0..0x10100 and every dwords 0..70
    { uint64_t bad = 0;
      for (uint64_t off = 0; off <= 0x10100; off++) for (uint64_t dw = 0; dw <= 70; dw++) {
          const bool ref = (off % 4 == 0) && dw >= 1 && dw <= 64 && off + dw * 4 <= 0x10000;
          if ((n48dr_r4_range_ok(off, dw) != 0) != ref) ++bad;
      }
      expect_u("region4 range rule: 0 disagreements with the reference over every (offset, dwords)", bad, 0); }
    expect(n48dr_r4_range_ok(0xFFFC, 1) && !n48dr_r4_range_ok(0xFFFC, 2) && n48dr_r4_range_ok(0xFF00, 64) && !n48dr_r4_range_ok(0xFF04, 64) && !n48dr_r4_range_ok(0x10000, 1) && n48dr_r4_range_ok(0, 64) && !n48dr_r4_range_ok(0, 65) && !n48dr_r4_range_ok(0, 0),
           "region4 range: the 0x10000 edge is inclusive of its last dword and not past it; 1..64 dwords");
    expect(!n48dr_r4_range_ok(0xFFFFFFFFFFFFFFFCull, 64) && !n48dr_r4_range_ok(0xFFFFFFFFFFFFFFFCull, 1) && !n48dr_r4_range_ok(0xFFFFFFFFFFFFF000ull, 4) && !n48dr_r4_range_ok(0x1ull << 32, 1) && !n48dr_r4_range_ok(0x100000000ull + 0x6000, 4),
           "region4 range: huge offsets cannot wrap into the window");
    expect(n48dr_r4_range_ok(0x3100, 16) && n48dr_r4_range_ok(0x4c00, 128 / 2) && n48dr_r4_range_ok(0x6000, 64) && n48dr_r4_range_ok(0x7fc0, 64) && n48dr_r4_range_ok(0, 64) && n48dr_r4_range_ok(0x1fc0, 16), "region4 range: the ring, state, mode block and context ranges are inside");
    // the argument: offset | dwords << 32 | page << 40, nothing above bit 47, page < ceil(dwords / 22)
    expect_u("region4 pages for 1 / 22 / 23 / 44 / 45 / 64 dwords", n48dr_r4_pages_for(1) * 100000 + n48dr_r4_pages_for(22) * 10000 + n48dr_r4_pages_for(23) * 1000 + n48dr_r4_pages_for(44) * 100 + n48dr_r4_pages_for(45) * 10 + n48dr_r4_pages_for(64), 1ull * 100000 + 1 * 10000 + 2 * 1000 + 2 * 100 + 3 * 10 + 3);
    expect(N48DR_R4_PER_PAGE * N48DR_R4_PAGES >= N48DR_R4_MAX_DWORDS && N48DR_R4_PER_PAGE * (N48DR_R4_PAGES - 1) < N48DR_R4_MAX_DWORDS && N48DR_R4_PER_PAGE <= 22 && 2 + (N48DR_R4_PER_PAGE + 1) / 2 <= 13, "region4 pages: 22 dwords a page cover 64 in 3 pages and fit v[2..12]");
    { uint64_t bad = 0;
      for (uint32_t off : { 0u, 4u, 0x3100u, 0x6000u, 0xFFC0u, 0xFFFCu, 0x10000u, 0x6002u }) for (uint32_t dw = 0; dw <= 66; dw++) for (uint32_t pg = 0; pg <= 4; pg++) {
          const bool ref = n48dr_r4_range_ok(off, dw) && pg < (dw + 21) / 22;
          if ((n48dr_r4_arg_ok(n48dr_r4_arg(off, dw, pg)) != 0) != ref) ++bad;
      }
      expect_u("region4 arg: legal exactly when the range is legal and the page exists", bad, 0); }
    expect(!n48dr_r4_arg_ok(1ull << 48) && !n48dr_r4_arg_ok(n48dr_r4_arg(0x6000, 16, 0) | (1ull << 63)) && !n48dr_r4_arg_ok(n48dr_r4_arg(0x6000, 16, 0) | (1ull << 56)) && !n48dr_r4_arg_ok(0) && n48dr_r4_arg_ok(n48dr_r4_arg(0x6000, 16, 0)), "region4 arg: bits above 47 and the zero argument are refused");
    // the base from register values
    const uint64_t fb = 0x8000000000ull, vram = 0x400000000ull;   // this card: FB base 0x80_0000_0000, 16 GiB
    uint64_t b = 0;
    expect(n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, vram, &b) == N48DR_OK && b == 0x3dac00000ull, "region4 base: the measured registers (OFFSET 0xdac00000, HIGH 0x83, FB base 0x8000000000) give VRAM offset 0x3dac00000");
    expect(n48dr_region4_base(0xdac00000u, 0x83u, 0, fb, vram, &b) == N48DR_REGION4_BAD && b == ~0ull, "region4 base: refused when REGION4 is not enabled (and no base is handed out)");
    expect(n48dr_region4_base(0xdac00000u, 0x83u, 1, 0x9000000000ull, vram, &b) == N48DR_REGION4_BAD, "region4 base: refused when the address is below the FB base");
    expect(n48dr_region4_base(0xdac00800u, 0x83u, 1, fb, vram, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0xdac00004u, 0x83u, 1, fb, vram, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0xdac00fffu, 0x83u, 1, fb, vram, &b) == N48DR_REGION4_BAD, "region4 base: refused when not 4 KiB aligned");
    expect(n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, 0x3dac00000ull + 0xFFFFull, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, 0x3dac00000ull + 0x10000ull, &b) == N48DR_OK && b == 0x3dac00000ull &&
           n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, 0x3dac00000ull, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, 0x100000000ull, &b) == N48DR_REGION4_BAD, "region4 base: the whole 64 KiB window must lie inside VRAM (edge: exactly fits / one byte short)");
    expect(n48dr_region4_base(0, 0, 1, 0, vram, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0, 0, 1, fb, vram, &b) == N48DR_REGION4_BAD && n48dr_region4_base(0xFFFFFFFFu, 0xFFFFFFFFu, 1, fb, vram, &b) == N48DR_REGION4_BAD, "region4 base: unprogrammed (zero) and all-ones registers are refused");
    // not hard-coded: other FB bases and offsets give their own answers
    expect(n48dr_region4_base(0x00010000u, 0x10u, 1, 0x1000000000ull, vram, &b) == N48DR_OK && b == 0x10000ull, "region4 base: FB base 0x1000000000, address 0x1000010000 -> 0x10000");
    expect(n48dr_region4_base(0x12345000u, 0x81u, 1, 0x8000000000ull, vram, &b) == N48DR_OK && b == 0x112345000ull, "region4 base: another address gives its own base (not the measured one)");
    expect(n48dr_region4_base(0x12345000u, 0x81u, 1, 0x8100000000ull, vram, &b) == N48DR_OK && b == 0x12345000ull, "region4 base: another FB base moves it");
    expect(n48dr_region4_base(0xdac00000u, 0x83u, 1, fb, vram, nullptr) == N48DR_OK, "region4 base: a null out pointer is tolerated");
    // the flow over the memory model
    { R4Model m; uint32_t d[64]; uint64_t base = 0;
      const uint32_t st = n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0x6800, 64, &base, d);
      bool match = true; for (uint32_t i = 0; i < 64; i++) match = match && d[i] == R4Model::word(0x3dac00000ull + 0x6800 + 4ull * i);
      expect(st == N48DR_OK && base == 0x3dac00000ull && m.reads.size() == 1 && m.reads[0].first == 0x3dac06800ull && m.reads[0].second == 64 && match, "region4 flow: ONE read of 64 dwords at base + offset, and the dwords are the memory's");
      const size_t n0 = m.reads.size();
      expect(n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0xFF04, 64, &base, d) == N48DR_BAD_ARG && n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 2, 4, &base, d) == N48DR_BAD_ARG &&
             n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0, 0, &base, d) == N48DR_BAD_ARG && n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0x10000, 1, &base, d) == N48DR_BAD_ARG && m.reads.size() == n0, "region4 flow: an out-of-window range is refused with NO read");
      expect(n48dr::region4_read(m, 0xdac00000u, 0x83u, 0, fb, vram, 0, 16, &base, d) == N48DR_REGION4_BAD && base == ~0ull && n48dr::region4_read(m, 0xdac00800u, 0x83u, 1, fb, vram, 0, 16, &base, d) == N48DR_REGION4_BAD &&
             n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, 0x3dac00000ull, 0, 16, &base, d) == N48DR_REGION4_BAD && m.reads.size() == n0, "region4 flow: a bad window is refused with NO read");
      m.fail = true;
      expect(n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0, 16, &base, d) == N48DR_VRAM_READ_FAILED && m.reads.size() == n0 + 1, "region4 flow: a refused read is its own status (one attempt, no retry)");
      expect(n48dr::region4_read(m, 0xdac00000u, 0x83u, 1, fb, vram, 0, 16, &base, nullptr) == N48DR_BAD_ARG, "region4 flow: no destination is refused"); }
    // different registers -> a different read address (the base is computed, not remembered)
    { R4Model m; uint32_t d[8]; uint64_t base = 0;
      expect(n48dr::region4_read(m, 0x12345000u, 0x81u, 1, fb, vram, 0x20, 8, &base, d) == N48DR_OK && m.reads.size() == 1 && m.reads[0].first == 0x112345020ull, "region4 flow: another register value reads at ITS base + offset"); }
    // the page layout round trip, every dword count 1..64 at a few offsets
    { uint64_t bad = 0;
      for (uint32_t dw = 1; dw <= 64; dw++) {
          uint32_t d[64], got[64] = { 0 };
          for (uint32_t i = 0; i < 64; i++) d[i] = 0x9E3779B9u * (i + 1) + dw;
          for (uint32_t pg = 0; pg < n48dr_r4_pages_for(dw); pg++) {
              uint64_t v[13]; n48dr_r4_fill(v, N48DR_OK, pg, 7, 0x6000, dw, 0x3dac00000ull, d);
              const unsigned want = (dw - pg * 22) < 22 ? dw - pg * 22 : 22;
              if ((v[0] & 0xFF) != N48DR_OK || ((v[0] >> 8) & 0xFF) != pg || ((v[0] >> 16) & 0xFF) != want || ((v[0] >> 24) & 0xFF) != 7 || ((v[0] >> 32) & 0xFF) != dw || ((v[0] >> 40) & 0xFFFF) != 0x6000 || v[1] != 0x3dac00000ull) ++bad;
              if (n48dr_r4_extract(v, pg, got) != want) ++bad;
          }
          for (uint32_t i = 0; i < dw; i++) if (got[i] != d[i]) ++bad;
          for (uint32_t i = dw; i < 64; i++) if (got[i] != 0) ++bad;
      }
      expect_u("region4 pages: every dword count round-trips through fill / extract, nothing past the count", bad, 0); }
    { uint64_t v[13]; n48dr_r4_fill(v, N48DR_REGION4_BAD, 0, 0, 0x6000, 16, 0x83dac00000ull, nullptr);
      expect((v[0] & 0xFF) == N48DR_REGION4_BAD && ((v[0] >> 16) & 0xFF) == 0 && v[1] == 0x83dac00000ull && v[2] == 0 && v[12] == 0, "region4 pages: a refusal carries no data");
      uint32_t d[64] = { 0 }; n48dr_r4_fill(v, N48DR_OK, 2, 1, 0, 64, 0, d);
      expect(((v[0] >> 16) & 0xFF) == 20, "region4 pages: the last page of 64 dwords has 20"); }
    expect(N48DR_ACT_REGION4READ == 94 && N48DR_STATUS_COUNT == 39 && N48DR_REGION4_BAD == 17 && N48DR_VRAM_READ_FAILED == 18 && !std::strcmp(n48dr_status_name(N48DR_REGION4_BAD) , n48dr_status_name(17)), "region4: the verb is 94 and the two new statuses are 17 / 18");
}

// ---- source pins -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static void s_pins(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string brg = slurp(src + "Navi48Bringup.cpp"), dcn = slurp(src + "dcn/navi48_dcn.cpp"), hpp = slurp(src + "dcn/navi48_dcn.hpp"), pure = slurp(src + "amd/native_disp_pure.h"), ucl = slurp(src + "Navi48UserClient.cpp"),
                      cli = slurp(root + "/tools/pc/navi48test.c"), plist = slurp(root + "/src/navi48-bringup/Info.plist"), s1c = slurp(src + "amd/native_s1c.h"), nub = slurp(src + "Navi48MetalNub.cpp"),
                      flow = slurp(src + "dcn/navi48_dispread_flow.h"), hdr = slurp(src + "dcn/navi48_dispread.h"), mk = slurp(root + "/src/navi48-bringup/Makefile"), sh = slurp(root + "/tools/build-navi48test.sh");
    expect(!brg.empty() && !dcn.empty() && !pure.empty() && !cli.empty() && !flow.empty() && !hdr.empty(), "the sources are readable");
    // the glue region of the new file content
    const size_t g0 = dcn.find("build 0.0.622 (multi-monitor track, stage M1; an internal design note): THE THREE READ-ONLY INSTRUMENTS.");
    expect(g0 != std::string::npos, "the glue block is present");
    // 0.0.625: the DMUB command verbs' glue (dmubSend / dmubMode / dmubCtx) follows this block in the file and is pinned by tests/native_dmubcmd_test.cpp; THIS suite's pins are about the three read-only instruments + region4read
    const size_t g1 = dcn.find("build 0.0.625 (stages M2 / M3; an internal design note");
    expect(g1 != std::string::npos && g0 != std::string::npos && g1 > g0, "the 0.0.625 command glue follows the read-only glue");
    const std::string glue = (g0 == std::string::npos || g1 == std::string::npos) ? "" : strip_line_comments(dcn.substr(g0, g1 - g0));
    // 1. the dispatch: verb_args_ok first, bind only for ddcread, the other two never bind
    const size_t d0 = brg.find("if (action == N48DR_ACT_DDCREAD || action == N48DR_ACT_DMUBRING || action == N48DR_ACT_DISPCENSUS || action == N48DR_ACT_REGION4READ) {");
    expect(d0 != std::string::npos, "accelExperiment has the 91..94 branch");
    if (d0 != std::string::npos) {
        std::string br = brg.substr(d0, 1800);
        { const size_t cut = br.find("actions 95..97 (0.0.625"); if (cut != std::string::npos) br = br.substr(0, cut); }   // 0.0.625: the 95..97 arm follows; this pin is about 91..94
        const size_t va = br.find("if (!n48disp::verb_args_ok(action, argScalar)) {"), bd = br.find("(void)n48dcn::bind(this);"), dd = br.find("n48dcn::ddcRead(argScalar, v, 13)"), dm = br.find("n48dcn::dmubRing(argScalar, v, 13)"), ds = br.find("n48dcn::dispCensus(argScalar, v, 13)"), dr = br.find("n48dcn::region4Read(argScalar, v, 13)");
        expect(va != std::string::npos && bd != std::string::npos && dd != std::string::npos && dm != std::string::npos && ds != std::string::npos && dr != std::string::npos && va < bd && bd < dd && dd < dm && dm < dr && dr < ds, "the branch checks the legal argument FIRST, binds only for ddcread, then calls the four");
        expect(br.find("bind(this)") == br.rfind("bind(this)"), "exactly one bind in the branch (ddcread's)");
        expect(br.find("action == N48DR_ACT_DDCREAD) {\n\t\t\t(void)n48dcn::bind(this);") != std::string::npos, "the bind is inside ddcread's arm");
    }
    // the branch sits BEFORE the native refusal? NO: the exemption is before it; the branch is after the admission check
    const size_t ex = brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)"), adm = brg.find("n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;"), br91 = brg.find("action == N48DR_ACT_DDCREAD ||");
    expect(ex != std::string::npos && adm != std::string::npos && br91 != std::string::npos && ex < adm && adm < br91, "REACHABILITY: the exemption, then the latch admission, then the 91..93 branch");
    // 2. the bound and the exemption (native_disp_pure.h)
    expect(pure.find("constexpr uint32_t kLastAction = 99u;") != std::string::npos && pure.find("(action == kActDdcRead && n48dr_ddc_arg_ok(arg)) || (action == kActDmubRing && n48dr_dmub_arg_ok(arg)) || (action == kActDispCensus && n48dr_census_arg_ok(arg)) ||\n           (action == kActRegion4Read && n48dr_r4_arg_ok(arg)) ||") != std::string::npos,
           "the action bound is 97 (0.0.625) and the four verbs are exempt with their legal arguments");
    expect(ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "the user client's bound goes through action_admitted (site 5)");
    // 3. the glue: the single register write behind the allowlist, no MM_INDEX, no command, no GPINT
    expect_u("the glue writes through exactly one WREG32", count_of(glue, "WREG32("), 1);
    { const size_t w = glue.find("amdgpu::WREG32("), a = glue.find("dcn41_allow_write(&gDcn.allow, a, v, \"ddcread\")"); expect(w != std::string::npos && a != std::string::npos && a < w, "the WREG32 follows the allowlist test in the same function"); }
    expect(glue.find("|| !dcn41_allow_write(&gDcn.allow, a, v, \"ddcread\")) {\n\t\t\trefusedFlag = true;") != std::string::npos, "a refused write sets the flow's refusal flag and returns without touching the card");
    for (const char *bad : { "RVRAM32_via_mm", "WVRAM32_via_mm", "mmMM_INDEX", "MM_INDEX", "vramRead32", "dcn41_dmub_gpint", "dcn41_dmub_ring_submit", "dcn41_dmub_ring_attach", "dcn41_dmub_build_", "dcn41_dmub_ring_wait", "WBAR0_", "bar0_memcpy_to_vram", "regWrite32" })
        expect(glue.find(bad) == std::string::npos, (std::string("the new glue never uses ") + bad).c_str());
    expect(glue.find("dcn41_dmub_probe(&gLr.d, &st)") != std::string::npos && glue.find("amdgpu::RBAR0_32(*dev, off)") != std::string::npos, "dmubring reads registers through the read-only device and ring memory through RBAR0_32 only");
    expect(glue.find("gLr.d.rreg(gLr.d.cookie, a)") != std::string::npos && glue.find("n48dr::census_page(ro,") != std::string::npos, "dispcensus reads through the read-only device");
    // 0.0.628 (M4a): the READ-ONLY address twin has exactly three users, none a write path; the write helpers keep dcn41_abs and the write mask
    { const size_t a0 = glue.find("struct DdcKextEnv {"), a1 = glue.find("struct RoKextEnv {"), a2 = a1 == std::string::npos ? a1 : glue.find("struct Bar0Mem {", a1);
      expect(a0 != std::string::npos && a1 != std::string::npos && a2 != std::string::npos && a0 < a1 && a1 < a2, "the two kext environments are in the glue");
      const std::string dd = glue.substr(a0, a1 - a0), ro = glue.substr(a1, a2 - a1);
      const size_t w0 = dd.find("void wr2(uint32_t off, uint32_t v) {"), w1 = w0 == std::string::npos ? w0 : dd.find("bool refused() const", w0);
      const std::string wr = (w0 == std::string::npos || w1 == std::string::npos) ? "" : dd.substr(w0, w1 - w0);
      expect(!wr.empty() && wr.find("dcn41_abs(&gDcn.d, off, 2u)") != std::string::npos && wr.find("dcn41_abs_rd") == std::string::npos, "ddcread's WRITE helper resolves with dcn41_abs on BASE_IDX 2 only - never the read twin");
      expect(dd.find("const uint32_t a = dcn41_abs_rd(&gDcn.d, off, baseIdx);") != std::string::npos && count_of(dd, "dcn41_abs_rd(") == 1, "ddcread's rd() (the prescale read) uses the read twin");
      expect(ro.find("const uint32_t a = dcn41_abs_rd(&gLr.d, off, baseIdx);") != std::string::npos && count_of(ro, "dcn41_abs_rd(") == 1 && ro.find("wr") == std::string::npos && ro.find("WREG32") == std::string::npos, "the census environment reads with the twin and has no write member");
      expect_u("navi48_dcn.cpp code: dcn41_abs_rd( appears three times (two rd members, the census log)", count_of(glue, "dcn41_abs_rd("), 3);
      expect(glue.find("dcn41_abs(&gDcn.d, off, 2u)") != std::string::npos && count_of(glue, "dcn41_abs(&gDcn.d, off, baseIdx)") == 0 && count_of(glue, "dcn41_abs(&gLr.d, off, baseIdx)") == 0, "no reader still passes a caller-chosen BASE_IDX to the write-side dcn41_abs");
      // no other source file calls the twin
      size_t users = 0;
      for (const char *rel : { "/src/navi48-bringup/src/Navi48Bringup.cpp", "/src/navi48-bringup/src/dcn/navi48_liveraster.h", "/src/navi48-bringup/src/dcn/navi48_dispread_flow.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd_flow.h", "/src/navi48-bringup/src/dcn/navi48_dispread.h", "/src/navi48-bringup/src/dcn/navi48_dmubcmd.h",
                               "/src/dcn41/dcn41_otg.c", "/src/dcn41/dcn41_hubp.c", "/src/dcn41/dcn41_allow.c", "/src/dcn41/dcn41_dmub.c", "/src/dcn41/dcn41_irq.c", "/src/dcn41/dcn41_otg_timing.c", "/src/dcn41/dcn41_io.h" })
          users += count_of(slurp(root + rel), "dcn41_abs_rd");
      expect_u("no other source file uses dcn41_abs_rd", users, 0);
      const std::string regsh = slurp(root + "/src/dcn41/dcn41_regs.h"), core = slurp(root + "/src/dcn41/dcn41_core.c");
      expect(regsh.find("#define DCN41_BASE_IDX_USED_MASK 0x4u") != std::string::npos && regsh.find("dcn41_abs_rd") == std::string::npos, "the generated write mask is still BASE_IDX 2 only");
      const size_t c0 = core.find("uint32_t dcn41_abs(const struct dcn41_dev *dev"), c1 = c0 == std::string::npos ? c0 : core.find("uint32_t dcn41_abs_rd(", c0);
      expect(c0 != std::string::npos && c1 != std::string::npos && core.substr(c0, c1 - c0).find("DCN41_BASE_IDX_USED_MASK & (1u << base_idx)") != std::string::npos && core.substr(c0, c1 - c0).find("READ_MASK") == std::string::npos, "dcn41_abs still tests the write mask (USED_MASK) and never the read mask"); }
    expect(count_of(glue, "if (__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL) != 0u) { n48dr_ddc_fill") == 1 && glue.find("__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE)") != std::string::npos, "ddcread is single-flight");
    expect(count_of(glue, "if (!__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE) || !gDcn.dev) { n48dr_ddc_fill") == 1, "ddcread refuses when the allowlist layer is not armed");
    expect(hpp.find("uint32_t ddcRead(uint64_t arg, uint64_t *out, unsigned outCount);") != std::string::npos && hpp.find("uint32_t dmubRing(") != std::string::npos && hpp.find("uint32_t dispCensus(") != std::string::npos, "navi48_dcn.hpp declares the three");
    // 3b. region4read (0.0.624): the glue's ONLY VRAM access is navi48_vram_read_mm; no write, no second MM path, no hard-coded base, single-flight, the DMCUB registers are only read
    { const size_t r0 = glue.find("uint32_t region4Read(uint64_t arg, uint64_t *out, unsigned outCount) {"), r1 = r0 == std::string::npos ? r0 : glue.find("uint32_t dispCensus(", r0);
      expect(r0 != std::string::npos && r1 != std::string::npos, "region4Read is in the glue");
      const std::string r4 = (r0 == std::string::npos || r1 == std::string::npos) ? "" : glue.substr(r0, r1 - r0);
      const size_t r4m = glue.find("bool rd(uint64_t vramOff, uint32_t *dst, uint32_t n) { return navi48_vram_read_mm(vramOff, dst, n); }");
      expect(r4m != std::string::npos && count_of(glue, "navi48_vram_read_mm(") == 2 && count_of(glue, "return navi48_vram_read_mm(") == 1, "region4's memory is navi48_vram_read_mm and nothing else (its declaration and ONE call site in the glue)");
      for (const char *bad : { "navi48_vram_write_mm", "WVRAM32_via_mm", "RVRAM32_via_mm", "MM_INDEX", "WREG32", "WBAR0_", "RBAR0_", "regWrite32", "navi48_reg_write32", "dcn41_allow_write", "3dac", "0x83" })
          expect(r4.find(bad) == std::string::npos, (std::string("region4Read never uses ") + bad).c_str());
      expect(r4.find("dcn41_dmub_probe(&gLr.d, &st)") != std::string::npos && r4.find("n48dr::region4_read(mem, st.region4_offset, st.region4_offset_high, enabled, st.fb_base_mc, vramSize, off, dwords, &base, d)") != std::string::npos &&
             r4.find("(st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u") != std::string::npos, "the window base comes from the probed DMCUB registers (REGION4_OFFSET / HIGH / ENABLE, FB base) through the checked flow");
      expect(r4.find("__atomic_exchange_n(&gR4Busy, 1u, __ATOMIC_ACQ_REL)") != std::string::npos && r4.find("__atomic_store_n(&gR4Busy, 0u, __ATOMIC_RELEASE)") != std::string::npos && r4.find("if (gLr.state != N48LR_READY)") != std::string::npos &&
             r4.find("if (!n48dr_r4_arg_ok(arg))") != std::string::npos, "region4read is single-flight, needs the read-only device and re-checks its argument");
      expect(r4.find("if (page != 0u) {") != std::string::npos && r4.find("status = gR4.status;") != std::string::npos, "pages 1 and 2 return the stored dwords");
      const size_t pr = r4.find("dcn41_dmub_probe"), rd = r4.find("n48dr::region4_read(");
      expect(pr != std::string::npos && rd != std::string::npos && pr < rd, "REACHABILITY: the registers are probed BEFORE the memory read is attempted");
      expect(hpp.find("uint32_t region4Read(uint64_t arg, uint64_t *out, unsigned outCount);") != std::string::npos && dcn.find("bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords);") != std::string::npos, "navi48_dcn.hpp declares region4Read; the reader is declared where it is used");
      const size_t f0 = flow.find("inline uint32_t region4_read(");
      const std::string fr = f0 == std::string::npos ? "" : strip_line_comments(flow.substr(f0));
      expect(fr.find("m.rd(base + off, dst, dwords)") != std::string::npos && count_of(fr, "m.") == 1 && fr.find("3dac") == std::string::npos && fr.find("n48dr_region4_base(off_lo, off_hi, enabled, fb_base_mc, fb_size, &base)") != std::string::npos, "the flow reads ONCE at the computed base + offset through M::rd and has no base of its own");
      expect(hdr.find("3dac") == std::string::npos && flow.find("3dac") == std::string::npos && r4.find("3dac") == std::string::npos && cli.find("3dac") == std::string::npos, "the measured base 0x3dac00000 appears nowhere in the code (it is computed live)"); }

    // 4. the flow and the header: no other register write path
    const std::string flowCode = strip_line_comments(flow);
    expect(flowCode.find("e.wr2(") != std::string::npos && count_of(flowCode, "WREG32") == 0 && flowCode.find("MM_INDEX") == std::string::npos && flowCode.find("gpint") == std::string::npos && flowCode.find("vram") == std::string::npos, "the flow writes only through the environment's wr2");
    { size_t n = 0, p = 0; while ((p = flowCode.find("e.wr2(", p)) != std::string::npos) { ++n; p += 6; } expect_u("the flow has 17 write sites", n, 17); }
    // 5. the CLI
    for (const char *t : { "in = 91;", "in = 92;", "in = 93;", "in = 94;", "else if (what && (!strcmp(what, \"region4read\") || !strcmp(what, \"region4dump\"))) in = 94;", "\"region4read\"", "\"region4dump\"", "return !strcmp(what, \"region4dump\") ? cmd_region4dump() : cmd_region4read(arg, arg2);", "ddcread dmubring dispcensus region4read region4dump",
                            "r4_range(\"VBIOS ring state\", 0x3100, 16, vb, &base)", "r4_range(\"mode block\", 0x4c00, 128, mb, &base)", "r4_range(\"display contexts\", 0x6000, 0x2000 / 4, ctx, &base)", "const uint32_t cb = 0x6000 + c * 0x800;",
                            "\"ddcread\"", "\"dmubring\"", "\"dispcensus\"", "ddcread dmubring dispcensus", "ddcread <line 2|3> <block 0..3>|dmubring [0|1..25|0x80]|dispcensus [0|1|2..9]", "navi48_dispread.h", "return cmd_ddcread(arg, arg2);" })
        expect(cli.find(t) != std::string::npos, (std::string("the CLI carries ") + t).c_str());
    expect(cli.find("const uint32_t sum = n48dr_edid_sum(edid);") != std::string::npos && cli.find("return sum == 0 ? 0 : 3;") != std::string::npos && cli.find("checksum                : %s (sum of the 128 bytes mod 256 = %u; the kext says %s)") != std::string::npos,
           "the CLI computes the EDID checksum itself, prints OK / BAD with the sum, and exits non-zero on BAD");
    expect(sh.find("-I \"$P/src/navi48-bringup/src/apple\"") != std::string::npos && sh.find("P=\"$(cd \"$(dirname \"$0\")/..\" && pwd)\"") != std::string::npos, "build-navi48test.sh builds the tree it lives in");
    // 6. the Makefile compiles dcn/*.cpp and the kext links dcn41_dmub.c (the probe)
    expect(mk.find("$(wildcard src/dcn/*.cpp)") != std::string::npos && mk.find("DCN41_SRCS := $(wildcard $(DCN41_DIR)/*.c)") != std::string::npos, "the Makefile builds src/dcn/*.cpp and src/dcn41/*.c");
    // 6b. scdcread (0.0.633): the dispatch, the glue, the flow, the header, the CLI, the admission
    {
      const size_t s0 = brg.find("if (action == N48DR_ACT_SCDCREAD) {"), s1 = brg.find("// action 98 (0.0.631, stage M4d;"), s95 = brg.find("// actions 95..97 (0.0.625, stages M2 / M3;"), s91 = brg.find("if (action == N48DR_ACT_DDCREAD || action == N48DR_ACT_DMUBRING");
      expect(s0 != std::string::npos && s1 != std::string::npos && s0 < s1 && s91 < s95 && s95 < s0, "scdcread: the action-99 branch follows the 91..94 and 95..97 branches and precedes disp2's");
      const std::string b = s0 == std::string::npos ? "" : brg.substr(s0, s1 - s0);
      const size_t va = b.find("if (!n48disp::verb_args_ok(action, argScalar)) {"), bd = b.find("(void)n48dcn::bind(this);"), sc = b.find("st = n48dcn::scdcRead(argScalar, v, 13);");
      expect(va != std::string::npos && bd != std::string::npos && sc != std::string::npos && va < bd && bd < sc && b.find("bind(this)") == b.rfind("bind(this)"), "scdcread: the legal argument is judged FIRST, then exactly one bind, then scdcRead");
      const size_t ex = brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)");
      expect(ex != std::string::npos && ex < s0, "REACHABILITY: the native exemption gate comes before the action-99 branch");
      expect(pure.find("constexpr uint32_t kActScdcRead = N48DR_ACT_SCDCREAD;") != std::string::npos && pure.find("(action == kActScdcRead && n48dr_scdc_arg_ok(arg))") != std::string::npos && pure.find("constexpr uint32_t kLastAction = 99u;") != std::string::npos, "scdcread: admitted past 82 only with the latch (kLastAction 99) and exempt on a native boot with a LEGAL argument only");
      expect(ucl.find("99 = scdcread") != std::string::npos && ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "scdcread: the user client's bound goes through action_admitted (site 5)");
      expect(hpp.find("uint32_t scdcRead(uint64_t arg, uint64_t *out, unsigned outCount);") != std::string::npos, "scdcread: navi48_dcn.hpp declares scdcRead");
      const size_t f0 = dcn.find("uint32_t scdcRead(uint64_t arg, uint64_t *out, unsigned outCount) {"), f1 = dcn.find("uint32_t dmubRing(uint64_t arg, uint64_t *out, unsigned outCount) {", f0 == std::string::npos ? 0 : f0);
      const std::string fn = (f0 == std::string::npos || f1 == std::string::npos) ? "" : strip_line_comments(dcn.substr(f0, f1 - f0));
      const size_t a1 = fn.find("if (!n48dr_scdc_arg_ok(arg))"), a2 = fn.find("__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)"), a3 = fn.find("__atomic_exchange_n(&gDdcBusy, 1u, __ATOMIC_ACQ_REL)"), a4 = fn.find("n48dr::scdc_read(env, line, off, len)"), a5 = fn.find("__atomic_store_n(&gDdcBusy, 0u, __ATOMIC_RELEASE)");
      expect(!fn.empty() && a1 != std::string::npos && a2 != std::string::npos && a3 != std::string::npos && a4 != std::string::npos && a5 != std::string::npos && a1 < a2 && a2 < a3 && a3 < a4 && a4 < a5, "scdcread glue: the argument, the bound layer, the SHARED engine busy flag (gDdcBusy, one engine), the read through the allowlist environment, then the flag released");
      expect(fn.find("DdcKextEnv env;") != std::string::npos && fn.find("WREG32") == std::string::npos && fn.find("wr2(") == std::string::npos && fn.find("dcn41_write") == std::string::npos && fn.find("MM_INDEX") == std::string::npos, "scdcread glue: its environment is ddcread's (writes ONLY through the DCN allowlist); the function itself writes nothing");
      const size_t x0 = flow.find("inline DdcResult i2c_xfer("), d0 = flow.find("inline DdcResult ddc_read("), c0 = flow.find("inline DdcResult scdc_read(");
      expect(x0 != std::string::npos && d0 != std::string::npos && c0 != std::string::npos && x0 < d0 && d0 < c0, "scdcread flow: i2c_xfer, then ddc_read and scdc_read after it");
      const std::string dBody = strip_line_comments(flow.substr(d0, c0 - d0)), cBody = strip_line_comments(flow.substr(c0));
      expect(dBody.find("e.wr2(") == std::string::npos && cBody.find("e.wr2(") == std::string::npos && dBody.find("return i2c_xfer(e, line, p, N48DR_EDID_BLOCK);") != std::string::npos && cBody.find("return i2c_xfer(e, line, p, len);") != std::string::npos && cBody.find("n48dr_plan_scdc(line, off, len, &p) != 0u) return r;") != std::string::npos,
             "scdcread flow: ddc_read and scdc_read are plan + i2c_xfer and NOTHING else (no register write of their own): ONE engine sequence, 17 write sites");
      const std::string xBody = strip_line_comments(flow.substr(x0, d0 - x0));
      expect(xBody.find("n48dr_arb_entry(arb)") != std::string::npos && xBody.find("n48dr_poll_bound(&p)") != std::string::npos && xBody.find("N48DR_ARB_SW_DONE_USING") != std::string::npos && xBody.find("i < nread && i < N48DR_EDID_BLOCK") != std::string::npos, "scdcread flow: the shared sequence keeps the arbitration verdict, the bounded poll, the release, and reads nread (<= 128) reply bytes");
      const size_t h0 = hdr.find("static inline uint32_t n48dr_plan_scdc("), h1 = hdr.find("// argument: line | off << 8 | len << 16", h0 == std::string::npos ? 0 : h0);
      const std::string pl = (h0 == std::string::npos || h1 == std::string::npos) ? "" : strip_line_comments(hdr.substr(h0, h1 - h0));
      expect(!pl.empty() && count_of(pl, "p->t[k].addr8 = ") == 2 && pl.find("p->t[k].addr8 = N48DR_SCDC_ADDR_W; p->t[k].wlen = 1u; p->t[k].wdata = (uint8_t)off; k++;") != std::string::npos && pl.find("p->t[k].addr8 = N48DR_SCDC_ADDR_R; p->t[k].is_read = 1u; p->t[k].rlen = (uint16_t)len; p->t[k].stop = 1u; k++;") != std::string::npos && pl.find("n48dr_scdc_range_ok(off, len)") != std::string::npos,
             "scdcread plan: exactly two transactions - one 1-byte write of the register offset to 0xA8, one read from 0xA9 - behind the line and range checks");
      expect(hdr.find("return off <= N48DR_SCDC_OFF_MAX && len >= 1u && len <= N48DR_SCDC_LEN_MAX && off + len <= N48DR_SCDC_END;") != std::string::npos, "scdcread range: offset <= 0x5f, 1..16 bytes, offset + length <= 0x60");
      for (const char *t : { "static int cmd_scdcread(const char *a1, const char *a2, const char *a3) {", "else if (what && !strcmp(what, \"scdcread\")) in = 99;", "in == 99 ? \"scdcread\"", "if (in == 99) return cmd_scdcread(arg, arg2, arg3);", "hangstat sessstat scdcread appallow fbpublish m6stat m6xstat vramstat\\n", "scdcread <line 2|3> <off 0..0x5f> [len 1..16]|appallow add <name>/remove <name>/list/strikes]|fbpublish 2|1|m6stat [0|1|2|3]|m6xstat [0|1|2]|vramstat|capstream", "dr_call(N48DR_ACT_SCDCREAD, n48dr_scdc_arg(line, off, len), o)",
                             "n48dr_scdc_range_ok(off, len)", "Status_Flags_0", "the sink LOCKS onto our TMDS", "Err_Det_Checksum", "scrambling %s, TMDS bit clock ratio %s\\n\", b[i], n48dr_scdc_scrambling_enabled(b[i]) ? \"ENABLED\" : \"off\", n48dr_scdc_ratio_by_40(b[i]) ? \"1/40\" : \"1/10\");" })
        expect(cli.find(t) != std::string::npos, (std::string("the CLI carries ") + t).c_str());
      expect(cli.find("verbs 91..99") != std::string::npos, "the CLI's admission hint names verbs 91..99");
    }
    // 7. the version
    expect(count_of(plist, "0.0.664") == 2 && plist.find("0.0.622") == std::string::npos, "Info.plist is 0.0.664, carried twice");
    expect(s1c.find("kN1cKextBuild = 664;") != std::string::npos, "the native client reports build 660");
    expect(count_of(nub, "664u") == 2 && nub.find("622u") == std::string::npos, "both ops tables carry the build 664");
    // 8. the project rule: the banned strings appear in none of the new files
    const std::string banned[3] = { std::string("pipe") + "+0x2" + "80", std::string("+0x2") + "82", std::string("+0x2") + "99" };
    for (const std::string &b : banned) for (const std::string *f : { &flow, &hdr, &glue }) expect(f->find(b) == std::string::npos, "the project rule: no forbidden pipe-offset spelling in the new code");
    expect(slurp(root + "/src/navi48-bringup/tests/native_dispread_test.cpp").find(banned[0]) == std::string::npos, "... nor in this test");
    // 9. attribution of the port
    expect(flow.find("RDNA4FB") != std::string::npos && flow.find("BSD-3-Clause") != std::string::npos && hdr.find("BSD-3-Clause") != std::string::npos, "the port of readEDIDI2C carries its attribution");
}

int main(int argc, char **argv) {
    if (argc > 1) g_root = argv[1];
    a_plan();
    b_registers();
    c_flow();
    f_scdc();
    d_edid();
    e_dmub();
    f_census();
    g_region4();
    // every offset ANY scenario wrote went through the real allowlist
    for (uint32_t off : gAllWritten) { const uint32_t abs = kSeg[2] + off; expect(allowed_abs(abs), "every register the flow wrote is inside the real DCN allowlist"); }
    expect(gAllWritten.size() >= 10, "the scenarios exercised the sequence's registers");
    s_pins(g_root);
    std::printf("native_dispread: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
