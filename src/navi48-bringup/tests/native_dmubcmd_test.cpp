// native_dmubcmd_test.cpp - build 0.0.625 (multi-monitor track, stages M2 / M3: the FIRST code that sends commands to the display firmware; an internal design note "M1.5 result" section 4).
// It compiles the REAL decisions of dcn/navi48_dmubcmd.h and the REAL sequences of dcn/navi48_dmubcmd_flow.h and drives them against a model of the DMUB (a register file with a firmware that completes a command
// after k RPTR reads or never, a sparse VRAM window, the REAL DCN write allowlist). The kext glue (dcn/navi48_dcn.cpp, Navi48Bringup.cpp), the CLI and the plist are checked by source pins.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn -I src/navi48-bringup/src/amd -I src/dcn41 \
//       src/navi48-bringup/tests/native_dmubcmd_test.cpp -x c++ src/dcn41/dcn41_allow.c -o /tmp/native_dmubcmd
//   /tmp/native_dmubcmd .       (run from the repo root; the argument is the repo root the source pins read from; N48_LINUX_DC = the tree holding re/linux-dc)
//   tests/native_dmubcmd_plant.sh plants breaks in the REAL code and demands a failure for each.
// Covers:
//   A  registers and constants against Linux's dcn_4_1_0_offset.h, dcn41_regs.h and the census table; the WPTR write is inside the REAL allowlist (and its neighbours the test names are what they are);
//   B  the allowlist: exhaustive over every (type, sub) header and a grid of (d1, d2) against an independent reference; every single-bit flip of every allowed header is refused; the slot bytes of each CLI command;
//   C  the pre-checks (DMCUB state, ring placement, sanity, busy, wrap) one by one;
//   D  the send flow over the fake DMUB: the happy path writes EXACTLY the slot and the three VBIOS variables and moves WPTR once; every refusal writes nothing; read-back mismatch; slot write failure; a ring that stops
//      being idle; the allowlist refusing the WPTR; the poll bound (10000 reads, 100 ms) and NO retry; the VBIOS variables (+0x3114 = 0x40, +0x3118 = old RPTR, +0x311c = new WPTR) and +0x3100 never touched;
//   E  dmubmode: the gate, save / restore / the two blocks (exact dwords), no save, the dirty rule, read-back mismatch; dmubctx: only 0x7000 / 0x7800, never 0x6800, the dword written and read back;
//   S  source pins: the OFF switch is the first test of every verb, the single WREG32 behind the allowlist, no other MM path / GPINT / ring submit, the dispatch order, the CLI, the version, the banned strings.
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
#include "navi48_dmubcmd_flow.h"
#include "amd/native_disp_pure.h"
extern "C" {
#include "dcn41_allow.h"
#include "dcn41_dmub.h"
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

static std::string linux_hdr(const char *name) {
    std::vector<std::string> roots;
    if (const char *e = std::getenv("N48_LINUX_DC")) roots.push_back(e);
    roots.push_back(g_root);
    roots.push_back("/path/to/navi48-checkout");
    for (auto &r : roots) { std::string s = slurp(r + "/re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/" + name); if (!s.empty()) return s; }
    return "";
}
static bool linux_reg(const std::string &h, const std::string &reg, uint32_t *off, uint32_t *base) {
    const std::string key = "#define reg" + reg + " ";
    size_t p = h.find(key); if (p == std::string::npos) return false;
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

// ---- the fake DMUB -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct WriteRec { uint64_t off; uint32_t n; std::vector<uint32_t> d; };
struct FakeDmub {
    // the card (the measured numbers: notes/logs/runs/m1-622/region4.txt)
    uint64_t fbBase = 0x8000000000ull, vramSize = 0x400000000ull, base = 0x3dac00000ull;   // window base (VRAM offset)
    int64_t r4Skew = 0;                 // added to the REGION4 GPU address the registers report (a window the registers place wrongly)
    uint32_t enabled = 1, softReset = 0, r4Enabled = 1, ringRegion4 = 1, ringAtBase = 1;
    uint32_t size = 0x2000, wptr = 0x700, rptr = 0x700;
    // firmware model: after a WPTR write, RPTR follows after `latency` RPTR reads (-1 = never)
    int latency = 0;
    bool fwPending = false; int fwCountdown = 0; uint32_t fwTarget = 0;
    // knobs
    bool failProbe = false, failSlotWrite = false, failAnyWrite = false, failRead = false, refuseWptr = false;
    int corruptWriteNo = -1;            // flip bit 0 of the first dword of this write (0-based)
    int corruptReadNo = -1;
    int raceAfterWrites = -1;           // after this many writes, change WPTR (someone else wrote it)
    bool failVarsWrite = false, corruptVarsRead = false;
    // the VRAM window (sparse)
    std::map<uint64_t, uint32_t> mem;
    // logs
    std::vector<WriteRec> writes;
    std::vector<uint32_t> wptrWrites;
    int probes = 0, rptrReads = 0, delays = 0, writeCalls = 0, readCalls = 0;
    uint64_t clock = 1000000; uint32_t frame = 5000;
    uint32_t wptrNow() const { return wptr; }
    bool probe(n48dm::Probe *p) {
        ++probes;
        if (failProbe) return false;
        p->pre.enabled = enabled; p->pre.soft_reset = softReset; p->pre.ring_region4 = ringRegion4; p->pre.ring_at_base = ringAtBase;
        p->pre.size = size; p->pre.wptr = wptr; p->pre.rptr = rptr;
        p->r4Enabled = r4Enabled; p->fbBaseMc = fbBase; p->vramSize = vramSize;
        const uint64_t gpu = fbBase + base + (uint64_t)r4Skew;
        p->r4Lo = (uint32_t)gpu; p->r4Hi = (uint32_t)(gpu >> 32);
        return true;
    }
    bool rd(uint64_t off, uint32_t *dst, uint32_t n) {
        ++readCalls;
        if (failRead) return false;
        for (uint32_t i = 0; i < n; i++) { auto it = mem.find(off + 4ull * i); dst[i] = it == mem.end() ? 0u : it->second; }
        if (corruptReadNo == readCalls - 1 && n) dst[0] ^= 1u;
        if (corruptVarsRead && off == base + N48DM_VAR_LASTSIZE && n == 3u) dst[2] ^= 0x10u;
        return true;
    }
    bool wr(uint64_t off, const uint32_t *src, uint32_t n) {
        const int no = writeCalls++;
        if (failAnyWrite || (failSlotWrite && n == N48DM_SLOT_DWORDS) || (failVarsWrite && off == base + N48DM_VAR_LASTSIZE)) return false;
        WriteRec r; r.off = off; r.n = n; r.d.assign(src, src + n); writes.push_back(r);
        for (uint32_t i = 0; i < n; i++) mem[off + 4ull * i] = src[i];
        if (corruptWriteNo == no && n) mem[off] ^= 1u;
        if (raceAfterWrites >= 0 && (int)writes.size() == raceAfterWrites) wptr = wptr + 0x40;
        return true;
    }
    uint32_t rptrRead() {
        ++rptrReads;
        if (fwPending) {
            if (fwCountdown <= 0) { rptr = fwTarget; fwPending = false; } else --fwCountdown;
        }
        return rptr;
    }
    uint32_t frames() { return frame += 3u; }
    bool wrWptr(uint32_t v) {
        if (refuseWptr) return false;
        wptrWrites.push_back(v);
        wptr = v;
        if (latency >= 0) { fwPending = true; fwCountdown = latency; fwTarget = v; }
        return true;
    }
    void delay_us(uint32_t us) { ++delays; clock += us; }
    uint64_t now_us() { return clock; }
};
// the flow calls e.rptr() / e.wptr(): thin adapters (a struct member and a method may not share a name)
struct Env {
    FakeDmub &f;
    bool probe(n48dm::Probe *p) { return f.probe(p); }
    bool rd(uint64_t o, uint32_t *d, uint32_t n) { return f.rd(o, d, n); }
    bool wr(uint64_t o, const uint32_t *s, uint32_t n) { return f.wr(o, s, n); }
    uint32_t rptr() { return f.rptrRead(); }
    uint32_t wptr() { return f.wptrNow(); }
    bool wrWptr(uint32_t v) { return f.wrWptr(v); }
    uint32_t frames() { return f.frames(); }
    void delay_us(uint32_t us) { f.delay_us(us); }
    uint64_t now_us() { return f.now_us(); }
};
static std::vector<uint32_t> slot_of(uint32_t h, uint32_t d1, uint32_t d2) { uint32_t s[16]; n48dm_build_slot(s, h, d1, d2); return std::vector<uint32_t>(s, s + 16); }
static bool nothing_touched(const FakeDmub &f) { return f.writes.empty() && f.wptrWrites.empty() && f.writeCalls == 0; }

static bool allowed_abs(uint32_t abs) { struct dcn41_allow_state al; std::memset(&al, 0, sizeof(al)); (void)dcn41_allow_init(&al, kSeg, 262144u); return dcn41_allow_write(&al, abs, 0u, "t"); }

int main(int argc, char **argv) {
    g_root = argc > 1 ? argv[1] : ".";
    const std::string src = g_root + "/src/navi48-bringup/src/";

    // =============================================================================================================================================================================================
    // A. registers and constants
    // =============================================================================================================================================================================================
    { const std::string h = linux_hdr("dcn_4_1_0_offset.h");
      expect(!h.empty(), "Linux's dcn_4_1_0_offset.h is readable (N48_LINUX_DC or re/linux-dc)");
      expect_reg(h, "DMCUB_INBOX1_WPTR", N48DM_REG_INBOX1_WPTR, N48DM_REG_BASE_IDX);
      expect_reg(h, "DMCUB_INBOX1_RPTR", N48DM_REG_INBOX1_RPTR, N48DM_REG_BASE_IDX);
      expect_reg(h, "OTG0_OTG_STATUS_FRAME_COUNT", N48DM_REG_OTG0_FRAMECOUNT, N48DM_REG_BASE_IDX); }
    expect(DCN41_DMCUB_INBOX1_WPTR == N48DM_REG_INBOX1_WPTR && DCN41_DMCUB_INBOX1_WPTR_BASE_IDX == N48DM_REG_BASE_IDX && DCN41_DMCUB_INBOX1_RPTR == N48DM_REG_INBOX1_RPTR && DCN41_DMCUB_INBOX1_RPTR_BASE_IDX == N48DM_REG_BASE_IDX,
           "the offsets are dcn41_regs.h's (the dmubring / dmub_probe registers)");
    expect(n48dr_census[16].off == N48DM_REG_OTG0_FRAMECOUNT && n48dr_census[16].base_idx == N48DM_REG_BASE_IDX && std::string(n48dr_census[16].name) == "OTG0_OTG_STATUS_FRAME_COUNT", "the frame counter is the census table's OTG0 entry");
    expect_u("WPTR absolute on this card = 0x34c0 + 0x1d6 = 0x3696 (the VBIOS send routine's register)", kSeg[2] + N48DM_REG_INBOX1_WPTR, 0x3696);
    expect_u("RPTR absolute = 0x3697", kSeg[2] + N48DM_REG_INBOX1_RPTR, 0x3697);
    expect(allowed_abs(kSeg[2] + N48DM_REG_INBOX1_WPTR), "the WPTR write is inside the REAL DCN write allowlist");
    { const char *nm = nullptr; expect(dcn41_allow_classify(kSeg[2] + N48DM_REG_INBOX1_WPTR, 262144u, &nm, nullptr) == DCN41_ALLOW_OK && nm && std::string(nm) == "DC..MMHUBBUB", "the range is DC..MMHUBBUB 0x35e6..0x3771"); }
    expect(N48DM_POLL_STEP_US == 10u && N48DM_POLL_MAX == 10000u && N48DM_POLL_BOUND_US == 100000u, "the poll bound is 10000 reads x 10 us = 100 ms");
    expect(N48DM_VAR_LASTSIZE == 0x3114u && N48DM_VAR_RPTR == 0x3118u && N48DM_VAR_WPTR == 0x311cu && N48DM_VAR_RPTR == N48DM_VAR_LASTSIZE + 4u && N48DM_VAR_WPTR == N48DM_VAR_RPTR + 4u && N48DM_VAR_STATUS == 0x3100u && N48DM_VAR_CMDSIZE_VALUE == 0x40u,
           "the VBIOS ring variables are contiguous dwords +0x3114 +0x3118 +0x311c; +0x3100 is the status byte");
    expect(N48DM_MODE_BLOCK == 0x4c00u && N48DM_MODE_DWORDS == 8u && N48DM_CTX_STATUS == 0x1a0u && N48DM_SLOT_BYTES == 64u && N48DM_ACT_SEND == 95u && N48DM_ACT_MODE == 96u && N48DM_ACT_CTX == 97u && N48DR_STATUS_COUNT == 39 && N48DR_HELD_REFUSED == 38, "layout constants and verb numbers");
    expect(!std::strcmp(n48dr_status_name(N48DR_CMD_OFF), n48dr_status_name(19)) && std::string(n48dr_status_name(N48DR_CMD_OFF)).find("navi48-dmubcmd") != std::string::npos, "the OFF status has its own readable name");
    // 0.0.653 (M5 review fix A): while the monitor B's plane is HELD EVERY monitor B template (ids 8..15: otg2 / phyd / digd) is refused; none of the monitor A's (3..7) or the OTG3 probe (1, 2); and every RAW kind aimed at the monitor B's context 0x7800
    { bool ok = true;
      for (uint32_t id = 0; id <= N48DM_TPL_COUNT + 2u; id++) {
          const bool monb = id >= 8u && id <= 15u;
          if ((n48dm_tpl_held_refuses(id) != 0) != monb) { ok = false; std::fprintf(stderr, "template %u held_refuses=%d want %d\n", id, n48dm_tpl_held_refuses(id), (int)monb); }
          if (monb) { const std::string nm = n48dm_tpl_get(id)->name; if (nm.find("otg2") == std::string::npos && nm.find("phyd") == std::string::npos && nm.find("digd") == std::string::npos) ok = false; }
          else if (n48dm_tpl_get(id)) { const std::string nm = n48dm_tpl_get(id)->name; if (nm.find("otg2") != std::string::npos || nm.find("phyd") != std::string::npos || nm.find("digd") != std::string::npos) ok = false; }
      }
      expect(ok, "0.0.653: templates 8..15 (by name: every otg2 / phyd / digd one) are refused while HELD; 1..7 and out-of-range ids are not");
      // raw kinds: every one of the six, on each context
      const uint32_t ctxs[] = { N48DM_CTX_DFP2, N48DM_CTX_DFP3, N48DM_CTX_DFP4 };
      bool rawok = true; unsigned monbKinds = 0, other = 0;
      for (uint32_t kind = N48DM_K_DETECT; kind <= N48DM_K_DISABLE; kind++) for (uint32_t c : ctxs) {
          uint32_t h, d1, d2; if (n48dm_spec(kind, c, &h, &d1, &d2) != 0u) { rawok = false; continue; }
          const bool wantMonB = c == N48DM_CTX_DFP4 && kind != N48DM_K_BEGIN && kind != N48DM_K_END;
          const int got = n48dm_slot_held_refuses(h, d1, d2);
          if ((got != 0) != wantMonB) { rawok = false; std::fprintf(stderr, "kind %u ctx %#x held=%d want %d\n", kind, c, got, (int)wantMonB); }
          if (wantMonB) monbKinds++; else other++;
      }
      expect(rawok && monbKinds == 4u && other == 14u, "0.0.653: the four raw kinds with a context (detect, setmode, enable, disable) on 0x7800 are refused while HELD; begin / end and every 0x7000 / 0x6800 slot are not");
      expect(n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 0u) != 0 && n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) == 0 && n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP2, 0u) == 0, "0.0.653: `dmubsend disable 0x7800` refused, 0x7000 and the DP's 0x6800 (already refused by the allowlist) not");
      expect(n48dm_slot_held_refuses(0x12345678u, N48DM_CTX_DFP4, 0u) == 0 && n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 5u) == 0, "0.0.653: a malformed slot keeps its own (allowlist) refusal, not the HELD one");
    // 0.0.655: the monitor A's twin - while ITS plane is HELD every monitor A template (ids 3..7: otg1 / phyc / digc, the teardowns 4 and 6 included) is refused, none of the monitor B's (8..15) nor the OTG3 probe; raw kinds aimed at 0x7000 only
    { bool ok = true;
      for (uint32_t id = 0; id <= N48DM_TPL_COUNT + 2u; id++) {
          const bool mona = id >= 3u && id <= 7u;
          if ((n48dm_tpl_mona_held_refuses(id) != 0) != mona) { ok = false; std::fprintf(stderr, "template %u mona_held_refuses=%d want %d\n", id, n48dm_tpl_mona_held_refuses(id), (int)mona); }
          if (mona && (n48dm_tpl_mona_held_refuses(id) != 0) == (n48dm_tpl_held_refuses(id) != 0)) ok = false;                  // never the same set as the monitor B's
      }
      expect(ok, "0.0.655: every monitor A template (3..7, teardowns 4 and 6 included) is refused while the monitor A is HELD; the monitor B's (8..15), the OTG3 probe (1, 2) and out-of-range ids are not");
      expect(n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) != 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP4, 0u) == 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP2, 0u) == 0, "0.0.655: `dmubsend disable 0x7000` refused for the monitor A, 0x7800 (the monitor B's) and 0x6800 not");
      expect(n48dm_slot_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 0u) == 0, "0.0.655: the monitor B's slot rule still does NOT refuse 0x7000 (byte-for-byte as 0.0.654)");
      expect(n48dm_slot_mona_held_refuses(0x12345678u, N48DM_CTX_DFP3, 0u) == 0 && n48dm_slot_mona_held_refuses(N48DM_HDR_DISABLE, N48DM_CTX_DFP3, 5u) == 0, "0.0.655: a malformed slot keeps its own (allowlist) refusal for the monitor A too"); }
      // when NOT held nothing is refused by the glue (the glue gates every branch on disp2Held(); the pure predicates only say WHAT a HELD plane refuses) - the glue's gating is pinned in native_disp2_test.cpp
      expect(std::string(n48dr_status_name(N48DR_HELD_REFUSED)).find("HELD") != std::string::npos && N48DR_HELD_REFUSED == 38u, "0.0.652: the refusal has its own named status (38)"); }
    { std::set<std::string> names; for (uint32_t s = 0; s < N48DR_STATUS_COUNT; s++) { std::string n = n48dr_status_name(s); expect(n != "unknown", "every status has a name"); names.insert(n); }
      expect_u("all 39 statuses have DISTINCT texts", names.size(), N48DR_STATUS_COUNT); }
    expect_u("latch: absent -> off", n48dm_latch_value(0, 1), N48DM_LATCH_OFF);
    expect_u("latch: =1 -> on", n48dm_latch_value(1, 1), N48DM_LATCH_ON);
    expect_u("latch: =0 -> off", n48dm_latch_value(1, 0), N48DM_LATCH_OFF);
    expect_u("latch: =2 -> off", n48dm_latch_value(1, 2), N48DM_LATCH_OFF);
    expect_u("latch: =0xffffffff -> off", n48dm_latch_value(1, 0xffffffffu), N48DM_LATCH_OFF);
    expect(n48dm_latch_is_on(N48DM_LATCH_ON) && !n48dm_latch_is_on(N48DM_LATCH_OFF) && !n48dm_latch_is_on(N48DM_LATCH_UNSET), "only the ON state is on (the unset state is OFF)");

    // =============================================================================================================================================================================================
    // B. the allowlist
    // =============================================================================================================================================================================================
    const uint32_t kAllowedHdr[5] = { 0x04000a80u, 0x08000680u, 0x08000580u, 0x04000780u, 0x04000880u };
    { uint32_t okCount = 0, hd[5] = { 0, 0, 0, 0, 0 }; unsigned nh = 0;
      for (uint32_t pl : { 0x00u, 0x04u, 0x08u, 0x0cu, 0x10u, 0x01u, 0x3fu })           // payload_bytes field
          for (uint32_t ty : { 0x80u, 0x00u, 0x06u, 0x81u, 0xffu })                       // type
              for (uint32_t sub = 0; sub < 256; sub++)
                  for (uint32_t ret : { 0u, 1u }) {                                       // ret_status bit
                      const uint32_t h = (pl << 24) | (ret << 16) | (sub << 8) | ty;
                      if (n48dm_hdr_ok(h)) { if (nh < 5) hd[nh] = h; ++nh; ++okCount; }
                  }
      expect_u("exactly 5 headers of 17920 candidate headers are on the allowlist", okCount, 5);
      std::set<uint32_t> got(hd, hd + std::min(nh, 5u)), want(kAllowedHdr, kAllowedHdr + 5);
      expect(got == want, "and they are exactly 04000a80 08000680 08000580 04000780 04000880");
      for (uint32_t h : kAllowedHdr) for (unsigned b = 0; b < 32; b++) { const uint32_t f = h ^ (1u << b); expect(n48dm_hdr_ok(f) == (want.count(f) != 0), "a single-bit flip of an allowed header is refused unless it IS another allowed header (detect 0xa80 / disable 0x880 differ in one bit)"); }
      for (uint32_t h : { 0x04000980u, 0x04000a00u, 0x08000780u, 0x04000680u, 0x04000580u, 0x08000a80u, 0x08000880u, 0x04000c80u, 0x04000f80u, 0u, 0xffffffffu }) expect(!n48dm_hdr_ok(h) && n48dm_slot_check(h, 0, 0) == N48DR_HDR_REFUSED, "other VBIOS-dialect headers (sub 9, 0xc, 0xf, wrong payload size ...) are refused");
      expect(n48dm_slot_check(0x04000a80u ^ 0x1u, 0x7000u, 0u) == N48DR_HDR_REFUSED, "an allowed payload with a wrong header is a HEADER refusal"); }
    // d1 / d2 against an independent reference
    { const uint32_t G[] = { 0u, 1u, 0x100u, 0x101u, 0x102u, 0x4bffu, 0x4c00u, 0x4c01u, 0x4c04u, 0x6000u, 0x67ffu, 0x6800u, 0x6801u, 0x6c00u, 0x7000u, 0x7001u, 0x7400u, 0x7800u, 0x7fffu, 0x8000u, 0xffffu, 0x10000u, 0x00007000u | 0x10000u, 0x80000000u, 0xffffffffu };
      auto ref = [](uint32_t h, uint32_t d1, uint32_t d2) -> bool {
          const bool ctx = d1 == 0x6800u || d1 == 0x7000u || d1 == 0x7800u, ctx2 = d2 == 0x6800u || d2 == 0x7000u || d2 == 0x7800u;
          if (h == 0x04000a80u || h == 0x04000780u) return ctx && d2 == 0u;
          if (h == 0x04000880u) return (d1 == 0x7000u || d1 == 0x7800u) && d2 == 0u;      // 0.0.626 (F5): disable only on the HDMI contexts, never 0x6800
          if (h == 0x08000680u) return d1 == 0u && (d2 == 0x101u || d2 == 0u);
          if (h == 0x08000580u) return d1 == 0x4c00u && ctx2;
          return false; };
      unsigned acc = 0;
      for (uint32_t h : kAllowedHdr) for (uint32_t d1 : G) for (uint32_t d2 : G) {
          const bool want = ref(h, d1, d2), got = n48dm_slot_check(h, d1, d2) == 0u;
          if (want != got) { ++gFail; std::printf("FAIL: allowlist %08x %#x %#x: want %d got %d\n", h, d1, d2, want, got); }
          ++gRun; acc += want;
          if (!want) expect(n48dm_slot_check(h, d1, d2) == N48DR_PAYLOAD_REFUSED, "a refused payload has its own status"); }
      expect_u("the grid accepts 3 (detect) + 2 (begin / end) + 3 (setmode) + 3 (enable) + 2 (disable, 0.0.626: not 0x6800) = 13 payloads", acc, 13); }
    expect(n48dm_slot_check(0x04000a80u, 0x6800u, 0u) == 0u && n48dm_slot_check(0x04000780u, 0x6800u, 0u) == 0u && n48dm_slot_check(0x08000580u, 0x4c00u, 0x6800u) == 0u, "ctx 0x6800 IS allowed for detect / enable / setmode (the rollback replays the DP sequence) - but NOT for disable (0.0.626, F5)");
    expect(n48dm_slot_check(0x04000a80u, 0x7000u, 0x1u) == N48DR_PAYLOAD_REFUSED && n48dm_slot_check(0x08000580u, 0x4c20u, 0x7000u) == N48DR_PAYLOAD_REFUSED && n48dm_slot_check(0x08000680u, 0x1u, 0u) == N48DR_PAYLOAD_REFUSED, "d2 on a detect, a wrong mode-block offset and a begin with d1 are refused");
    // the slot bytes of each CLI command
    { struct C { uint32_t kind, ctx; uint32_t h, d1, d2; const char *name; };
      const C cmds[] = { { N48DM_K_DETECT, 0x7000, 0x04000a80u, 0x7000u, 0u, "detect 0x7000" }, { N48DM_K_DETECT, 0x7800, 0x04000a80u, 0x7800u, 0u, "detect 0x7800" },
                         { N48DM_K_BEGIN, 0, 0x08000680u, 0u, 0x101u, "begin" }, { N48DM_K_END, 0, 0x08000680u, 0u, 0u, "end" },
                         { N48DM_K_SETMODE, 0x7000, 0x08000580u, 0x4c00u, 0x7000u, "setmode 0x7000" }, { N48DM_K_SETMODE, 0x6800, 0x08000580u, 0x4c00u, 0x6800u, "setmode 0x6800" },
                         { N48DM_K_ENABLE, 0x7000, 0x04000780u, 0x7000u, 0u, "enable 0x7000" }, { N48DM_K_ENABLE, 0x6800, 0x04000780u, 0x6800u, 0u, "enable 0x6800" },
                         { N48DM_K_DISABLE, 0x7000, 0x04000880u, 0x7000u, 0u, "disable 0x7000" } };
      for (const C &c : cmds) {
          uint32_t h = 1, d1 = 1, d2 = 1;
          expect(n48dm_spec(c.kind, c.ctx, &h, &d1, &d2) == 0u && h == c.h && d1 == c.d1 && d2 == c.d2, (std::string("the spec of ") + c.name).c_str());
          expect(n48dm_slot_check(h, d1, d2) == 0u && n48dm_send_encodable(d1, d2), (std::string("the spec of ") + c.name + " is on the allowlist and encodable").c_str());
          uint32_t sl[16]; n48dm_build_slot(sl, h, d1, d2);
          expect(sl[0] == c.h && sl[1] == c.d1 && sl[2] == c.d2, (std::string("slot dwords 0..2 of ") + c.name).c_str());
          bool z = true; for (int i = 3; i < 16; i++) z = z && sl[i] == 0u; expect(z, (std::string("slot dwords 3..15 are zero for ") + c.name).c_str());
          uint32_t h2, a, b; n48dm_send_unarg(n48dm_send_arg(h, d1, d2), &h2, &a, &b); expect(h2 == h && a == d1 && b == d2, (std::string("the argument round trip of ") + c.name).c_str()); }
      uint32_t h, d1, d2; expect(n48dm_spec(0, 0, &h, &d1, &d2) == N48DR_BAD_ARG && n48dm_spec(7, 0, &h, &d1, &d2) == N48DR_BAD_ARG, "an unknown command kind is a bad argument");
      // the byte image of each slot (little-endian), against the design's text
      uint32_t sl[16]; n48dm_build_slot(sl, 0x04000a80u, 0x00007000u, 0u); const uint8_t *b = (const uint8_t *)sl;
      expect(b[0] == 0x80 && b[1] == 0x0a && b[2] == 0x00 && b[3] == 0x04 && b[4] == 0x00 && b[5] == 0x70 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[63] == 0, "detect 0x7000 byte image: 80 0a 00 04 | 00 70 00 00 | zeros"); }
    expect(!n48dm_send_encodable(0x10000u, 0u) && !n48dm_send_encodable(0u, 0x10000u) && n48dm_send_encodable(0xffffu, 0xffffu), "dmubsend's argument encodes 16-bit d1 / d2 only");

    // =============================================================================================================================================================================================
    // C. the pre-checks, one by one
    // =============================================================================================================================================================================================
    { n48dm_pre ok = { 1, 0, 1, 1, 0x2000, 0x700, 0x700 };
      expect_u("good: DMCUB on, REGION4 ring, idle, room", n48dm_precheck(&ok), 0);
      n48dm_pre p = ok; p.enabled = 0; expect_u("DMCUB not enabled", n48dm_precheck(&p), N48DR_DMUB_NOT_READY);
      p = ok; p.soft_reset = 1; expect_u("DMCUB in soft reset", n48dm_precheck(&p), N48DR_DMUB_NOT_READY);
      p = ok; p.ring_region4 = 0; expect_u("ring not mapped through REGION4", n48dm_precheck(&p), N48DR_RING_NOT_REGION4);
      p = ok; p.ring_at_base = 0; expect_u("ring byte 0 is not the window base", n48dm_precheck(&p), N48DR_RING_NOT_REGION4);
      p = ok; p.size = 0; expect_u("ring size 0", n48dm_precheck(&p), N48DR_NO_RING);
      p = ok; p.size = 0x2001; expect_u("ring size not a multiple of 64", n48dm_precheck(&p), N48DR_NO_RING);
      p = ok; p.size = 0x10040; expect_u("ring size above 64 KiB", n48dm_precheck(&p), N48DR_NO_RING);
      p = ok; p.wptr = p.rptr = 0x710; expect_u("pointers not 64-aligned", n48dm_precheck(&p), N48DR_NO_RING);
      p = ok; p.wptr = 0x2000; p.rptr = 0x2000; expect_u("pointers at the ring size", n48dm_precheck(&p), N48DR_NO_RING);
      p = ok; p.wptr = 0x740; expect_u("WPTR != RPTR: busy", n48dm_precheck(&p), N48DR_RING_BUSY);
      p = ok; p.rptr = 0x6c0; expect_u("RPTR behind WPTR: busy", n48dm_precheck(&p), N48DR_RING_BUSY);
      p = ok; p.wptr = p.rptr = 0x1fc0; expect_u("WPTR + 0x40 == the ring size: the ring wraps", n48dm_precheck(&p), N48DR_RING_WRAP);
      p = ok; p.wptr = p.rptr = 0x1f80; expect_u("WPTR + 0x40 < the ring size: still fits", n48dm_precheck(&p), 0);
      p = ok; p.wptr = p.rptr = 0; expect_u("WPTR 0 fits", n48dm_precheck(&p), 0);
      p = ok; p.size = 0x40; p.wptr = p.rptr = 0; expect_u("a one-slot ring is refused: the ring size must be the measured 0x2000 (0.0.626, F7)", n48dm_precheck(&p), N48DR_RING_SIZE_REFUSED);
      p = ok; p.enabled = 0; p.wptr = 0x740; expect_u("order: DMCUB state is judged before the ring", n48dm_precheck(&p), N48DR_DMUB_NOT_READY); }

    // =============================================================================================================================================================================================
    // D. the send flow over the fake DMUB
    // =============================================================================================================================================================================================
    const uint32_t kStatusSentinel = 0x00000009u;       // the VBIOS handshake byte at +0x3100: must stay
    auto fresh = [&](FakeDmub &f) { f.mem[f.base + N48DM_VAR_STATUS] = kStatusSentinel; f.mem[f.base + 0x3104] = 0; f.mem[f.base + 0x3110] = 0x40; f.mem[f.base + 0x3120] = 0x2000; f.mem[f.base + 0x3118] = 0x6c0; f.mem[f.base + 0x311c] = 0x700; f.mem[f.base + 0x3114] = 0x40; };
    // D1 happy path for each CLI command
    { struct C { uint32_t kind, ctx; uint32_t h, d1, d2; const char *name; };
      const C cmds[] = { { N48DM_K_DETECT, 0x7000, 0x04000a80u, 0x7000u, 0u, "detect 0x7000" }, { N48DM_K_BEGIN, 0, 0x08000680u, 0u, 0x101u, "begin" }, { N48DM_K_END, 0, 0x08000680u, 0u, 0u, "end" },
                         { N48DM_K_SETMODE, 0x7000, 0x08000580u, 0x4c00u, 0x7000u, "setmode 0x7000" }, { N48DM_K_ENABLE, 0x7000, 0x04000780u, 0x7000u, 0u, "enable 0x7000" }, { N48DM_K_DISABLE, 0x7800, 0x04000880u, 0x7800u, 0u, "disable 0x7800" } };
      for (const C &c : cmds) {
          FakeDmub f; fresh(f); f.latency = 3; Env e{f};
          uint32_t h, d1, d2; (void)n48dm_spec(c.kind, c.ctx, &h, &d1, &d2);
          const n48dm_send_out o = n48dm::send(e, h, d1, d2);
          const std::string nm = c.name;
          expect_u((nm + ": status OK").c_str(), o.status, N48DR_OK);
          expect(o.sent == 1 && o.vars == 1 && o.old_wptr == 0x700 && o.new_wptr == 0x740 && o.rptr_start == 0x700 && o.rptr_end == 0x740 && o.base == f.base, (nm + ": sent, WPTR 0x700 -> 0x740, RPTR 0x740").c_str());
          expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740, (nm + ": the WPTR register was written exactly once, with 0x740").c_str());
          expect(f.writes.size() == 2, (nm + ": exactly TWO VRAM writes (the slot, the VBIOS variables)").c_str());
          if (f.writes.size() == 2) {
              expect(f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == slot_of(c.h, c.d1, c.d2), (nm + ": the slot is at base + WPTR with the exact 64 bytes").c_str());
              expect(f.writes[1].off == f.base + 0x3114 && f.writes[1].n == 3 && f.writes[1].d == std::vector<uint32_t>({ 0x40u, 0x700u, 0x740u }), (nm + ": the variables +0x3114 = 0x40, +0x3118 = old RPTR 0x700, +0x311c = new WPTR 0x740").c_str()); }
          expect(f.mem[f.base + N48DM_VAR_STATUS] == kStatusSentinel && f.mem[f.base + 0x3104] == 0 && f.mem[f.base + 0x3110] == 0x40 && f.mem[f.base + 0x3120] == 0x2000, (nm + ": +0x3100 / 0x3104 / 0x3110 / 0x3120 untouched").c_str());
          expect(o.frames1 > o.frames0 && o.polls == 4 && o.elapsed_us == 30, (nm + ": OTG0 frames before < after, 4 polls, 30 us of waiting").c_str());
          expect(f.probes == 1, (nm + ": one probe").c_str()); } }
    // D2 other WPTR positions: the variables follow the real pointers
    { FakeDmub f; fresh(f); f.wptr = f.rptr = 0x1f40; f.latency = 0; Env e{f};
      const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_OK && o.old_wptr == 0x1f40 && o.new_wptr == 0x1f80 && f.writes.size() == 2 && f.writes[0].off == f.base + 0x1f40 && f.writes[1].d == std::vector<uint32_t>({ 0x40u, 0x1f40u, 0x1f80u }), "at WPTR 0x1f40 the slot and the variables follow it"); }
    // D3 refusals BEFORE any write
    { struct R { const char *name; uint32_t h, d1, d2; uint32_t want; };
      const R rs[] = { { "header not allowlisted (sub 9)", 0x04000980u, 0x7000u, 0u, N48DR_HDR_REFUSED }, { "header zero", 0u, 0u, 0u, N48DR_HDR_REFUSED }, { "detect with d2 != 0", 0x04000a80u, 0x7000u, 1u, N48DR_PAYLOAD_REFUSED },
                       { "detect on a context outside the list", 0x04000a80u, 0x6000u, 0u, N48DR_PAYLOAD_REFUSED }, { "setmode with the wrong block", 0x08000580u, 0x4c20u, 0x7000u, N48DR_PAYLOAD_REFUSED },
                       { "begin with d2 = 2", 0x08000680u, 0u, 2u, N48DR_PAYLOAD_REFUSED }, { "enable with d2", 0x04000780u, 0x7000u, 0x100u, N48DR_PAYLOAD_REFUSED } };
      for (const R &r : rs) { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, r.h, r.d1, r.d2);
          expect(o.status == r.want, (std::string("refusal: ") + r.name).c_str());
          expect(nothing_touched(f) && f.probes == 0 && f.readCalls == 0 && f.rptrReads == 0 && o.sent == 0, (std::string("refusal touches NOTHING (not even a read): ") + r.name).c_str()); } }
    // D4 live pre-check refusals: nothing written, WPTR not moved
    { struct R { const char *name; uint32_t want; void (*mod)(FakeDmub &); };
      const R rs[] = { { "DMCUB disabled", N48DR_DMUB_NOT_READY, [](FakeDmub &f) { f.enabled = 0; } }, { "DMCUB in soft reset", N48DR_DMUB_NOT_READY, [](FakeDmub &f) { f.softReset = 1; } },
                       { "REGION4 window disabled", N48DR_REGION4_BAD, [](FakeDmub &f) { f.r4Enabled = 0; } }, { "window below the FB base", N48DR_REGION4_BAD, [](FakeDmub &f) { f.r4Skew = -(int64_t)f.base - 1; } },
                       { "window outside VRAM", N48DR_REGION4_BAD, [](FakeDmub &f) { f.vramSize = 0x3dac00000ull; } }, { "window not 4 KiB aligned", N48DR_REGION4_BAD, [](FakeDmub &f) { f.base = 0x3dac00200ull; } },
                       { "ring not through REGION4", N48DR_RING_NOT_REGION4, [](FakeDmub &f) { f.ringRegion4 = 0; } }, { "ring not at the window base", N48DR_RING_NOT_REGION4, [](FakeDmub &f) { f.ringAtBase = 0; } },
                       { "ring busy", N48DR_RING_BUSY, [](FakeDmub &f) { f.rptr = 0x6c0; } }, { "ring busy the other way", N48DR_RING_BUSY, [](FakeDmub &f) { f.wptr = 0x740; } },
                       { "would wrap", N48DR_RING_WRAP, [](FakeDmub &f) { f.wptr = f.rptr = 0x1fc0; } }, { "ring insane", N48DR_NO_RING, [](FakeDmub &f) { f.size = 0x30; } },
                       { "no device", N48DR_NO_DEVICE, [](FakeDmub &f) { f.failProbe = true; } } };
      for (const R &r : rs) { FakeDmub f; fresh(f); r.mod(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
          expect(o.status == r.want, (std::string("pre-check: ") + r.name).c_str());
          expect(nothing_touched(f) && o.sent == 0 && f.rptrReads == 0, (std::string("pre-check refusal writes nothing and reads no pointer: ") + r.name).c_str()); } }
    // D5 slot write failure, read-back mismatch, a ring that stops being idle, the allowlist refusing the WPTR
    { FakeDmub f; fresh(f); f.failSlotWrite = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_SLOT_WRITE_FAILED && f.wptrWrites.empty() && o.sent == 0 && f.writes.empty(), "a refused slot write: its own status, WPTR not moved"); }
    for (int bit = 0; bit < 2; bit++) {     // mismatch in the slot dword 0 (the write path), or the read path
      FakeDmub f; fresh(f); if (bit == 0) f.corruptWriteNo = 0; else f.corruptReadNo = 0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x08000580u, 0x4c00u, 0x7000u);
      expect(o.status == N48DR_READBACK_MISMATCH && f.wptrWrites.empty() && o.sent == 0 && f.writes.size() == 1, bit == 0 ? "read-back mismatch (written dword differs): WPTR NOT moved, nothing else written" : "read-back mismatch (read path): WPTR NOT moved"); }
    { FakeDmub f; fresh(f);
      // a mismatch in the LAST dword only
      struct E2 : Env { FakeDmub &g; E2(FakeDmub &x) : Env{ x }, g(x) {} bool wr(uint64_t o, const uint32_t *s, uint32_t n) { bool r = g.wr(o, s, n); if (n == 16) g.mem[o + 60] ^= 0x80000000u; return r; } } e2(f);
      const n48dm_send_out o = n48dm::send(e2, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_READBACK_MISMATCH && f.wptrWrites.empty(), "a mismatch in the LAST dword of the slot is caught too"); }
    { FakeDmub f; fresh(f); f.failRead = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_VRAM_READ_FAILED && f.wptrWrites.empty(), "an unreadable read-back: no WPTR"); }
    { FakeDmub f; fresh(f); f.raceAfterWrites = 1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_RING_BUSY && f.wptrWrites.empty() && o.sent == 0, "WPTR changed under us between the slot write and the bump: refused, WPTR not written"); }
    { FakeDmub f; fresh(f); f.refuseWptr = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_WPTR_REFUSED && o.sent == 0 && f.rptrReads == 1 && f.writes.size() == 1 && f.delays == 0, "the allowlist refusing the WPTR: nothing sent, no poll, no variables"); }
    // D6 the poll bound and NO retry
    { FakeDmub f; fresh(f); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_POLL_TIMEOUT, "a firmware that never answers: POLL_TIMEOUT");
      expect_u("exactly 10000 polls", o.polls, 10000);
      expect_u("exactly 10000 x 10 us of waiting = 100 ms", f.clock - 1000000, 100000);
      expect_u("elapsed_us reports 100000", o.elapsed_us, 100000);
      expect_u("the WPTR was written EXACTLY ONCE (no retry)", f.wptrWrites.size(), 1);
      expect(f.writes.size() == 1 && o.sent == 1 && o.vars == 0 && o.rptr_end == 0x700, "on timeout nothing else is written (no variables), the command counts as sent");
      expect(f.mem[f.base + 0x3118] == 0x6c0 && f.mem[f.base + 0x311c] == 0x700, "the VBIOS variables are left exactly as they were"); }
    { FakeDmub f; fresh(f); f.latency = 9998; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_OK && o.polls == 9999, "RPTR arriving on the 9999th read is accepted"); }
    { FakeDmub f; fresh(f); f.latency = 9999; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_OK && o.polls == 10000 && f.delays == 9999, "RPTR arriving on the 10000th (last) read is accepted"); }
    { FakeDmub f; fresh(f); f.latency = 10000; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 10000 && f.wptrWrites.size() == 1, "RPTR arriving on the 10001st read is too late"); }
    // =============================================================================================================================================================================================
    // T (0.0.630, M4c). The mainline VBIOS templates: sub 2 SET_PIXEL_CLOCK (P1..P4) and sub 1 TRANSMITTER_CONTROL (T1, T2), each an EXACT 16-dword slot named by ID; a 2 s poll bound; nothing else of sub 1 / 2.
    // =============================================================================================================================================================================================
    {
      // T1. the layout, decoded field by field against the Linux structs (dmub_cmd.h dmub_cmd_header / set_pixel_clock_parameter_v1_7 / dmub_dig_transmitter_control_data_v1_7)
      auto b8 = [](uint32_t d, unsigned k) { return (uint32_t)((d >> (8u * k)) & 0xFFu); };
      auto hdr_type = [&](uint32_t h) { return h & 0xFFu; }; auto hdr_sub = [&](uint32_t h) { return (h >> 8) & 0xFFu; }; auto hdr_flags = [&](uint32_t h) { return (h >> 16) & 0xFFu; }; auto hdr_payload = [&](uint32_t h) { return (h >> 24) & 0x3Fu; }; auto hdr_res1 = [&](uint32_t h) { return h >> 30; };
      expect_u("T: fifteen templates (0.0.632 added digc-setup-dvi, 0.0.633 the monitor B's five, 0.0.634 the three 1440p ones)", N48DM_TPL_COUNT, 15);
      expect(N48DM_HDR_PCLK == 0x10000280u && N48DM_HDR_XMIT == 0x3C000180u, "T: the two headers are 0x10000280 (sub 2) and 0x3C000180 (sub 1)");
      for (unsigned i = 0; i < 6; i++) { const n48dm_tpl &t = n48dm_tpls[i]; const bool pclk = i < 4;
          const std::string nm = t.name;
          expect(hdr_type(t.dw[0]) == 0x80u && hdr_sub(t.dw[0]) == (pclk ? 2u : 1u) && hdr_flags(t.dw[0]) == 0u && hdr_payload(t.dw[0]) == (pclk ? 16u : 60u) && hdr_res1(t.dw[0]) == 0u, (nm + ": header = type 0x80 (DMUB_CMD__VBIOS), sub, payload_bytes 16 (pixel clock) / 60 (transmitter), no flag bits").c_str());
          expect_u((nm + ": dword 4 (reserved2 / reserved1) is zero").c_str(), t.dw[4], 0); }
      // P1..P4 : pixclk_100hz | pll_id, encoderobjid, encoder_mode, miscinfo | crtc_id, deep_color_ratio, reserved1[2] | reserved2
      { const uint32_t pll[4] = { 0x17, 0x17, 0x16, 0x16 }, crtc[4] = { 3, 3, 1, 1 }, clk[4] = { 1485000u, 0, 1485000u, 0 };
        for (unsigned i = 0; i < 4; i++) { const n48dm_tpl &t = n48dm_tpls[i]; const std::string nm = t.name;
            expect(t.dw[1] == clk[i] && b8(t.dw[2], 0) == pll[i] && b8(t.dw[2], 1) == 0x20u && b8(t.dw[2], 2) == 2u && b8(t.dw[2], 3) == 0u && b8(t.dw[3], 0) == crtc[i] && b8(t.dw[3], 1) == 0u && b8(t.dw[3], 2) == 0u && b8(t.dw[3], 3) == 0u,
                   (nm + ": pixclk_100hz, pll_id 0x17/0x16 (ATOM_COMBOPHY_PLL3 / PLL2), encoderobjid 0x20, encoder_mode 2 (DVI), miscinfo 0, crtc_id 3 / 1 (0-based), deep colour 0, reserved zero").c_str()); }
        expect(n48dm_tpls[0].dw[1] == 0x0016A8C8u && n48dm_tpls[2].dw[1] == 0x0016A8C8u && 0x0016A8C8u == 148500000u / 100u, "P: 0x0016A8C8 is 148.5 MHz in units of 100 Hz"); }
      // T1 / T2 : phyid, action, digmode, lanenum | symclk_10khz | hpdsel, digfe_sel, connobj_id, HPO_instance | TxFFELaneSel, skip_phy_ssc_reduction, reserved2[2]
      for (unsigned i = 4; i < 6; i++) { const n48dm_tpl &t = n48dm_tpls[i]; const std::string nm = t.name;
          expect(b8(t.dw[1], 0) == 2u && b8(t.dw[1], 1) == (i == 4 ? 1u : 0u) && b8(t.dw[1], 2) == 2u && b8(t.dw[1], 3) == 4u && t.dw[2] == 14850u && b8(t.dw[3], 0) == 3u && b8(t.dw[3], 1) == 0u && b8(t.dw[3], 2) == 0x0Cu && b8(t.dw[3], 3) == 0u,
                 (nm + ": phyid 2 (UNIPHYC), action 1 enable / 0 disable (TRANSMITTER_CONTROL_ENABLE / DISABLE), digmode 2 (DVI), lanenum 4, symclk_10khz 14850, hpdsel 3, digfe_sel 0, connobj_id 0x0C, HPO_instance 0").c_str()); }
      // T1b. the Linux sources the sizes come from (read live): the 60-byte transmitter union and the payload_bytes expression of both senders
      { std::string dm, ct; for (const char *r : { "/re/linux-dc", "" }) { if (dm.empty()) dm = slurp(g_root + r + "/drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h"); if (ct.empty()) ct = slurp(g_root + r + "/drivers/gpu/drm/amd/display/dc/bios/command_table2.c"); }
        if (dm.empty()) { dm = slurp("/path/to/navi48-checkout/re/linux-dc/drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h"); ct = slurp("/path/to/navi48-checkout/re/linux-dc/drivers/gpu/drm/amd/display/dc/bios/command_table2.c"); }
        const size_t v7 = dm.find("struct dmub_dig_transmitter_control_data_v1_7 {");
        expect(v7 != std::string::npos && dm.find("uint32_t reserved3[11]; /**< For future use */", v7) != std::string::npos && dm.find("uint8_t reserved2[2]; /**< For future use */", v7) != std::string::npos, "T: Linux dmub_dig_transmitter_control_data_v1_7 ends with reserved2[2] and reserved3[11] (4+4+4+4 + 44 = 60 bytes)");
        expect(count_of(ct, "header.payload_bytes =\n\t\tsizeof(cmd.dig1_transmitter_control) -\n\t\tsizeof(cmd.dig1_transmitter_control.header);") == 2 && count_of(ct, "header.payload_bytes =\n\t\tsizeof(cmd.set_pixel_clock) -\n\t\tsizeof(cmd.set_pixel_clock.header);") == 1, "T: command_table2.c computes payload_bytes as sizeof(cmd.<x>) - sizeof(header) for both transmitter senders and the pixel clock"); }
      // T2. the slot of every template: five dwords then eleven zeros
      for (unsigned i = 0; i < 6; i++) { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[i]); bool z = true; for (unsigned k = 5; k < 16; k++) z = z && sl[k] == 0u;
          expect(z && sl[0] == n48dm_tpls[i].dw[0] && sl[1] == n48dm_tpls[i].dw[1] && sl[4] == n48dm_tpls[i].dw[4] && n48dm_tpl_slot_check(sl) == 0u, (std::string(n48dm_tpls[i].name) + ": the 64-byte slot is the five dwords + 11 zeros and passes the exact-slot check").c_str()); }
      // T3. refusal of ANY other payload of sub 1 / sub 2: every single-bit flip of every dword of every template slot, plus a grid of other values and the old 16-byte transmitter header
      { size_t flips = 0, accepted = 0; bool all = true;
        for (unsigned i = 0; i < 6; i++) for (unsigned k = 0; k < 16; k++) for (unsigned bit = 0; bit < 32; bit++) { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[i]); sl[k] ^= 1u << bit; ++flips; if (n48dm_tpl_slot_check(sl) == 0u) { ++accepted; if (!((i >= 4 && k == 1 && bit == 8) || (i < 2 && k == 3 && bit == 0))) all = false; } }
        // the legitimate coincidences among these six: T1 <-> T2 differ in the single action bit (d1 bit 8), and (0.0.633) pclk-otg3-on / -off differ from pclk-otg2-on / -off in the single crtc_id bit 0 (d3 3 vs 2): each such flip is the OTHER template, exactly (section V proves it)
        expect(all && accepted == 4, "T: every single-bit flip of every dword of the first six template slots is refused (6 x 16 x 32 flips), except the T1 <-> T2 action bit and the otg3 -> otg2 crtc bit, which are other templates"); expect_u("T: flips counted", flips, 6 * 16 * 32); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[4]); sl[0] = 0x10000180u; expect(n48dm_tpl_slot_check(sl) != 0u, "T: the transmitter payload with the 16-byte header 0x10000180 is NOT a template"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[4]); sl[3] = 0x000C0004u; expect(n48dm_tpl_slot_check(sl) != 0u, "T: HPD select 4 is refused"); sl[3] = 0x000C0003u; sl[1] = 0x04020103u; expect(n48dm_tpl_slot_check(sl) != 0u, "T: transmitter action 3 is refused"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[0]); sl[2] = 0x00022014u; expect(n48dm_tpl_slot_check(sl) != 0u, "T: PLL id 0x14 (PLL0) is refused"); sl[2] = 0x00022017u; sl[3] = 0x0000000Cu; expect(n48dm_tpl_slot_check(sl) != 0u, "T: crtc 0xC is refused"); sl[3] = 3u; sl[1] = 0x0016A8C9u; expect(n48dm_tpl_slot_check(sl) != 0u, "T: another clock is refused"); }
      // the old allowlist never admits sub 1 / sub 2 headers (any payload_bytes): ordinary and replay paths
      { bool none = true; for (uint32_t pb = 0; pb < 64; pb++) for (uint32_t sub = 1; sub <= 2; sub++) { const uint32_t h = 0x80u | (sub << 8) | (pb << 24); if (n48dm_slot_check(h, 0u, 0u) == 0u || n48dm_slot_check(h, 0x7000u, 0u) == 0u || n48dm_slot_check(h, 0x4c00u, 0x7000u) == 0u) none = false; }
        expect(none, "T: n48dm_slot_check refuses every payload_bytes of sub 1 and sub 2 (the ordinary dmubsend and replay paths cannot carry them)"); }
      // the argument: only the tag + ID 1..6; nothing else
      { uint32_t id = 99;
        for (uint32_t k = 1; k <= 15; k++) expect(n48dm_tpl_unarg(n48dm_tpl_arg(k), &id) == 0u && id == k, "T: tpl arg round-trip");
        expect(n48dm_tpl_unarg(n48dm_tpl_arg(0), &id) != 0u && id == 0 && n48dm_tpl_unarg(n48dm_tpl_arg(16), &id) != 0u && n48dm_tpl_unarg(n48dm_tpl_arg(0xFF), &id) != 0u, "T: ID 0 / 16 / 255 refused");
        expect(n48dm_tpl_unarg(n48dm_tpl_arg(1) | (1ull << 40), &id) != 0u && n48dm_tpl_unarg(n48dm_tpl_arg(1) | (1ull << 63), &id) != 0u && n48dm_tpl_unarg(0x10000280ull | (1ull << 32), &id) != 0u, "T: stray high bits and a raw header are refused");
        expect(!n48dm_hdr_ok(N48DM_TPL_TAG) && N48DM_TPL_TAG != N48DM_REPLAY_TAG && !n48dm_is_replay_arg(n48dm_tpl_arg(1)) && !n48dm_is_tpl_arg(n48dm_replay_arg(0x100)) && !n48dm_is_tpl_arg(n48dm_send_arg(0x04000a80u, 0x7000u, 0u)), "T: the tag is not an allowlisted header and does not collide with the replay tag"); }
      // T4. the poll bound per sub-type
      expect(N48DM_POLL_MAX_VBIOS == 200000u && N48DM_POLL_BOUND_VBIOS_US == 2000000u && n48dm_poll_max_for(N48DM_HDR_PCLK) == 200000u && n48dm_poll_max_for(N48DM_HDR_XMIT) == 200000u && n48dm_poll_max_for(0x04000a80u) == 10000u && n48dm_poll_max_for(0x08000680u) == 10000u && n48dm_poll_max_for(0x04000780u) == 10000u && n48dm_poll_max_for(0u) == 10000u,
             "T: poll bound 200000 x 10 us = 2 s for sub 1 / sub 2; 10000 x 10 us for every old header");
      // T5. the flow over the fake DMUB: each template, exact bytes, one WPTR write, vars, 2 s bound, no retry
      for (unsigned i = 1; i <= 6; i++) { FakeDmub f; fresh(f); f.latency = 3; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, i);
          const std::string nm = n48dm_tpls[i - 1].name; uint32_t want[16]; n48dm_tpl_slot(want, &n48dm_tpls[i - 1]);
          expect(o.status == N48DR_OK && o.sent == 1 && o.vars == 1 && o.hdr == want[0] && o.d1 == want[1] && o.d2 == want[2] && o.polls == 4 && o.new_wptr == 0x740, (nm + ": send by template ID: OK, sent, 4 polls, header/d1/d2 reported").c_str());
          expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740 && f.writes.size() == 2 && f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == std::vector<uint32_t>(want, want + 16) && f.writes[1].off == f.base + 0x3114, (nm + ": the exact 64-byte slot at base + WPTR, one WPTR write, then the variables").c_str()); }
      { FakeDmub f; fresh(f); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_PCLK_OTG3_ON);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 200000u && f.clock - 1000000 == 2000000u && o.elapsed_us == 2000000u && f.wptrWrites.size() == 1 && f.writes.size() == 1 && o.vars == 0, "T: a sub-2 template that never completes: exactly 200000 polls = 2 s, ONE WPTR write (no retry), no variables"); }
      { FakeDmub f; fresh(f); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_PHYC_ENABLE_DVI);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 200000u && o.elapsed_us == 2000000u && f.wptrWrites.size() == 1, "T: a sub-1 template that never completes: 200000 polls = 2 s, no retry"); }
      { FakeDmub f; fresh(f); f.latency = 199999; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_PHYC_DISABLE);
        expect(o.status == N48DR_OK && o.polls == 200000u, "T: RPTR arriving on the 200000th read is accepted"); }
      { FakeDmub f; fresh(f); f.latency = 200000; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_PHYC_DISABLE);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 200000u, "T: RPTR arriving on the 200001st read is too late"); }
      { FakeDmub f; fresh(f); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 10000u && o.elapsed_us == 100000u, "T: an OLD sub-type keeps the 100 ms bound"); }
      // refusals touch nothing: ID 0 is the ordinary path; an unknown ID; a template with replay; the old paths on sub 1 / 2
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 16u); expect(o.status == N48DR_PAYLOAD_REFUSED && nothing_touched(f) && f.probes == 0, "T: template ID 16: refused before anything is read or written"); }
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, 0x100u, 1u); expect(o.status == N48DR_PAYLOAD_REFUSED && nothing_touched(f), "T: a template together with replay: refused"); }
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x3C000180u, 0x04020102u & 0xFFFFu, 0x3A02u); expect(o.status == N48DR_HDR_REFUSED && nothing_touched(f), "T: an ordinary send with the transmitter header: refused, nothing touched"); }
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x10000280u, 0u, 0u); expect(o.status == N48DR_HDR_REFUSED && nothing_touched(f), "T: an ordinary send with the pixel-clock header: refused, nothing touched"); }
      { FakeDmub f; fresh(f); Env e{f}; f.mem[f.base + 0x6c0] = 0x3C000180u; f.mem[f.base + 0x6c4] = 0x04020102u; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, 0x6c0u); expect(o.status == N48DR_HDR_REFUSED && f.writes.empty() && f.wptrWrites.empty(), "T: replay of a ring slot that holds a sub-1 header: refused, nothing written"); }
      { FakeDmub f; fresh(f); f.ringAtBase = 0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 1u); expect(o.status == N48DR_RING_NOT_REGION4 && nothing_touched(f), "T: a template still passes the pre-checks (ring placement): refused, nothing touched"); }
      { FakeDmub f; fresh(f); f.rptr = 0x6c0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 5u); expect(o.status == N48DR_RING_BUSY && nothing_touched(f), "T: a busy ring refuses a template"); }
      { FakeDmub f; fresh(f); f.refuseWptr = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 5u); expect(o.status == N48DR_WPTR_REFUSED && o.sent == 0 && f.rptrReads == 1, "T: the allowlist refusing the WPTR: nothing sent for a template either"); }
    }
    // =============================================================================================================================================================================================
    // U (0.0.632). Template 7, the mainline DIGX_ENCODER_CONTROL (sub 0): the DIGC DVI stream setup exactly as Linux's dcn401 sends it. Every byte derived from the Linux tree (read live), nothing else of sub 0.
    // =============================================================================================================================================================================================
    {
      auto b8 = [](uint32_t d, unsigned k) { return (uint32_t)((d >> (8u * k)) & 0xFFu); };
      const n48dm_tpl &t = n48dm_tpls[N48DM_TPL_DIGC_SETUP_DVI - 1u];
      expect(N48DM_TPL_DIGC_SETUP_DVI == 7u && std::string(t.name) == "digc-setup-dvi" && N48DM_HDR_DIGENC == 0x0C000080u && t.dw[0] == N48DM_HDR_DIGENC, "U: template 7 is digc-setup-dvi with header 0x0C000080");
      // U1. the header, field by field: type 0x80 (DMUB_CMD__VBIOS), sub_type 0 (DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL), flag bits 0, payload_bytes 12, reserved1 0
      expect((t.dw[0] & 0xFFu) == 0x80u && ((t.dw[0] >> 8) & 0xFFu) == 0u && ((t.dw[0] >> 16) & 0xFFu) == 0u && ((t.dw[0] >> 24) & 0x3Fu) == 12u && (t.dw[0] >> 30) == 0u, "U: header = type 0x80, sub_type 0, no flag bits, payload_bytes 12, reserved1 0");
      // U2. the payload, field by field (dig_encoder_stream_setup_parameters_v1_5): digid, action, digmode, lanenum | pclk_10khz | bitpercolor, dplinkrate_270mhz, reserved[2]
      expect(b8(t.dw[1], 0) == 2u && b8(t.dw[1], 1) == 0x0Fu && b8(t.dw[1], 2) == 2u && b8(t.dw[1], 3) == 4u && t.dw[2] == 14850u && b8(t.dw[3], 0) == 0u && b8(t.dw[3], 1) == 0u && b8(t.dw[3], 2) == 0u && b8(t.dw[3], 3) == 0u && t.dw[4] == 0u,
             "U: digid 2 (DIGC), action 0x0F (ATOM_ENCODER_CMD_STREAM_SETUP), digmode 2 (DVI), lanenum 4, pclk_10khz 14850, bitpercolor 0, dplinkrate 0, reserved 0, slot dword 4 zero");
      expect(t.dw[1] == 0x04020F02u && t.dw[2] == 0x00003A02u && 14850u == (1485000u / 10u) / 10u, "U: the dwords 0x04020F02 / 0x00003A02 / 0 (pix_clk_100hz 1485000 -> kHz 148500 -> 10 kHz units 14850)");
      // U3. the Linux sources the bytes come from (read live)
      { const std::string base = "/drivers/gpu/drm/amd/";
        auto rd = [&](const std::string &rel) { std::string r; for (const char *root : { "/re/linux-dc", "" }) if (r.empty()) r = slurp(g_root + root + base + rel);
            if (r.empty()) r = slurp(std::string("/path/to/navi48-checkout/re/linux-dc") + base + rel); return r; };
        const std::string atom = rd("include/atomfirmware.h"), ct = rd("display/dc/bios/command_table2.c"), dio = rd("display/dc/dio/dcn401/dcn401_dio_stream_encoder.c"), hlp = rd("display/dc/bios/dce112/command_table_helper2_dce112.c"),
                          hlp2 = rd("display/dc/bios/command_table_helper2.c"), dm = rd("display/dmub/inc/dmub_cmd.h"), bpt = rd("display/include/bios_parser_types.h"), dp = rd("display/dc/dc_dp_types.h"), gro = rd("display/include/grph_object_id.h");
        expect(!atom.empty() && !ct.empty() && !dio.empty() && !hlp.empty() && !hlp2.empty() && !dm.empty() && !bpt.empty() && !dp.empty() && !gro.empty(), "U: all nine Linux sources were found");
        // sizes: the member lists of the four union arms, parsed from atomfirmware.h (u8 / u32 members, natural alignment)
        auto structSize = [&](const std::string &name, uint32_t *align) -> uint32_t {
            const size_t a = atom.find("struct " + name); if (a == std::string::npos) return 0; const size_t o = atom.find('{', a), c = atom.find("};", o); if (o == std::string::npos || c == std::string::npos) return 0;
            uint32_t off = 0, al = 1; size_t pos = o;
            while (true) { const size_t nl = atom.find('\n', pos + 1); if (nl == std::string::npos || nl > c) break; std::string line = atom.substr(pos + 1, nl - pos - 1); pos = nl;
                const size_t k8 = line.find("uint8_t "), k32 = line.find("uint32_t "); if (k8 == std::string::npos && k32 == std::string::npos) continue;
                const uint32_t sz = (k32 != std::string::npos && (k8 == std::string::npos || k32 < k8)) ? 4u : 1u; uint32_t n = 1; const size_t br = line.find('[', 0), semi = line.find(';');
                if (br != std::string::npos && semi != std::string::npos && br < semi) n = (uint32_t)std::strtoul(line.c_str() + br + 1, nullptr, 0);
                off = (off + sz - 1) / sz * sz; off += sz * n; if (sz > al) al = sz; }
            if (align) *align = al; return (off + al - 1) / al * al; };
        uint32_t al = 0; const uint32_t sSetup = structSize("dig_encoder_stream_setup_parameters_v1_5", &al), sLink = structSize("dig_encoder_link_setup_parameters_v1_5", nullptr), sPanel = structSize("dp_panel_mode_set_parameters_v1_5", nullptr), sGen = structSize("dig_encoder_generic_cmd_parameters_v1_5", nullptr);
        uint32_t uni = sSetup; for (uint32_t x : { sLink, sPanel, sGen }) if (x > uni) uni = x;
        expect(sSetup == 12u && sLink == 9u + 0u && sPanel == 12u && sGen == 12u && al == 4u && uni == 12u, "U: Linux's union dig_encoder_control_parameters_v1_5 is 12 bytes (stream_setup 12, link_setup 9, dppanel 12, generic 12; align 4)");
        expect(dm.find("struct dmub_cmd_digx_encoder_control_data {\n\tunion dig_encoder_control_parameters_v1_5 dig; /**< payload */\n};") != std::string::npos && dm.find("struct dmub_rb_cmd_digx_encoder_control {\n\tstruct dmub_cmd_header header;  /**< header */\n\tstruct dmub_cmd_digx_encoder_control_data encoder_control; /**< payload */\n};") != std::string::npos,
               "U: dmub_rb_cmd_digx_encoder_control = dmub_cmd_header + dmub_cmd_digx_encoder_control_data { union dig_encoder_control_parameters_v1_5 dig } -> sizeof 4 + 12 = 16, payload_bytes 12");
        expect(((4u + uni) - 4u) == ((t.dw[0] >> 24) & 0x3Fu), "U: payload_bytes = sizeof(cmd.digx_encoder_control) - sizeof(header) = 12 equals the template header's");
        expect(dm.find("DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL = 0,") != std::string::npos && ct.find("cmd.digx_encoder_control.header.sub_type =\n\t\tDMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL;") != std::string::npos && ct.find("cmd.digx_encoder_control.header.type = DMUB_CMD__VBIOS;") != std::string::npos &&
               ct.find("cmd.digx_encoder_control.header.payload_bytes =\n\t\tsizeof(cmd.digx_encoder_control) -\n\t\tsizeof(cmd.digx_encoder_control.header);") != std::string::npos && ct.find("cmd.digx_encoder_control.encoder_control.dig.stream_param = *dig;") != std::string::npos && ct.find("memset(&cmd, 0, sizeof(cmd));") != std::string::npos,
               "U: command_table2.c encoder_control_dmcub: memset 0, type DMUB_CMD__VBIOS, sub_type DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL (= 0), payload_bytes = sizeof - sizeof(header), stream_param = *dig");
        expect(dm.find("DMUB_CMD__VBIOS = 128,") != std::string::npos || dm.find("DMUB_CMD__VBIOS = 0x80") != std::string::npos, "U: DMUB_CMD__VBIOS is 128 (0x80)");
        expect(ct.find("params.digid = (uint8_t)(cntl->engine_id);") != std::string::npos && ct.find("params.action = bp->cmd_helper->encoder_action_to_atom(cntl->action);") != std::string::npos && ct.find("params.pclk_10khz = cntl->pixel_clock / 10;") != std::string::npos &&
               ct.find("(uint8_t)(bp->cmd_helper->encoder_mode_bp_to_atom(\n\t\t\t\t\tcntl->signal,\n\t\t\t\t\tcntl->enable_dp_audio));") != std::string::npos && ct.find("params.lanenum = (uint8_t)(cntl->lanes_number);") != std::string::npos &&
               ct.find("switch (cntl->color_depth) {\n\tcase COLOR_DEPTH_888:\n\t\tparams.bitpercolor = PANEL_8BIT_PER_COLOR;") != std::string::npos && ct.find("\tdefault:\n\t\tbreak;\n\t}\n\n\tif (cntl->signal == SIGNAL_TYPE_HDMI_TYPE_A)") != std::string::npos,
               "U: encoder_control_digx_v1_5 fills digid / action / pclk_10khz = pixel_clock / 10 / digmode / lanenum from the bp_encoder_control, bitpercolor only for a DEFINED colour depth (default: break), the HDMI scaling only for HDMI");
        const size_t dv = dio.find("void enc401_stream_encoder_dvi_set_stream_attribute("), dve = dio.find("\n}\n", dv == std::string::npos ? 0 : dv);
        const std::string dvi = (dv == std::string::npos || dve == std::string::npos) ? std::string() : dio.substr(dv, dve - dv);
        expect(!dvi.empty() && dvi.find("struct bp_encoder_control cntl = {0};") != std::string::npos && dvi.find("cntl.action = ENCODER_CONTROL_SETUP;") != std::string::npos && dvi.find("cntl.engine_id = enc1->base.id;") != std::string::npos &&
               dvi.find("SIGNAL_TYPE_DVI_DUAL_LINK : SIGNAL_TYPE_DVI_SINGLE_LINK;") != std::string::npos && dvi.find("cntl.enable_dp_audio = false;") != std::string::npos && dvi.find("cntl.pixel_clock = crtc_timing->pix_clk_100hz / 10;") != std::string::npos &&
               dvi.find("cntl.lanes_number = (is_dual_link) ? LANE_COUNT_EIGHT : LANE_COUNT_FOUR;") != std::string::npos && dvi.find("avoid_vbios_exec_table") != std::string::npos,
               "U: enc401_stream_encoder_dvi_set_stream_attribute: ENCODER_CONTROL_SETUP, engine_id = the stream encoder id, single link, no DP audio, pixel_clock = pix_clk_100hz / 10 (kHz), LANE_COUNT_FOUR");
        expect(!dvi.empty() && dvi.find("cntl.color_depth") == std::string::npos && dvi.find("ASSERT(crtc_timing->display_color_depth == COLOR_DEPTH_888);") != std::string::npos,
               "U: that function NEVER sets cntl.color_depth (the only mention of a depth is an ASSERT on the timing), so bitpercolor stays 0 - not PANEL_8BIT_PER_COLOR (2)") ;
        expect(bpt.find("enum bp_encoder_control_action {\n\t/* direct VBIOS translation! Just to simplify the translation */\n\tENCODER_CONTROL_DISABLE = 0,\n\tENCODER_CONTROL_ENABLE,\n\tENCODER_CONTROL_SETUP,") != std::string::npos &&
               hlp.find("\tcase ENCODER_CONTROL_SETUP:\n\t\tatom_action = ATOM_ENCODER_CMD_STREAM_SETUP;") != std::string::npos && atom.find("ATOM_ENCODER_CMD_STREAM_SETUP                 = 0x0F,") != std::string::npos &&
               hlp2.find("case DCN_VERSION_4_01:") != std::string::npos && hlp2.find("*h = dal_cmd_tbl_helper_dce112_get_table2();") != std::string::npos,
               "U: ENCODER_CONTROL_SETUP -> ATOM_ENCODER_CMD_STREAM_SETUP = 0x0F through the dce112 table, which DCN 4.01 uses");
        expect(hlp2.find("case SIGNAL_TYPE_DVI_SINGLE_LINK:\n\tcase SIGNAL_TYPE_DVI_DUAL_LINK:\n\t\treturn ATOM_ENCODER_MODE_DVI;") != std::string::npos && atom.find("ATOM_ENCODER_MODE_DVI         =2,") != std::string::npos && dp.find("LANE_COUNT_FOUR = 4,") != std::string::npos,
               "U: SIGNAL_TYPE_DVI_SINGLE_LINK -> ATOM_ENCODER_MODE_DVI = 2; LANE_COUNT_FOUR = 4");
        expect(gro.find("ENGINE_ID_DIGA,\n\tENGINE_ID_DIGB,\n\tENGINE_ID_DIGC,") != std::string::npos, "U: ENGINE_ID_DIGC is the third engine id (DIGA = 0): 2");
        // Linux has no encoder-control DISABLE on the dcn401 TMDS path: ENCODER_CONTROL_DISABLE is used only by the DAC path (dce_link_encoder.c) in the whole display tree
        { const std::string dce = rd("display/dc/dce/dce_link_encoder.c"); expect(!dce.empty() && dce.find("ENCODER_CONTROL_DISABLE") != std::string::npos && dio.find("ENCODER_CONTROL_DISABLE") == std::string::npos && rd("display/dc/dio/dcn401/dcn401_dio_link_encoder.c").find("ENCODER_CONTROL_DISABLE") == std::string::npos,
               "U: ENCODER_CONTROL_DISABLE appears only in the DAC path (dce_link_encoder.c), never in the dcn401 stream / link encoder: there is no template 8"); } }
      // U4. the exact slot, and the flow over the fake DMUB
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &t); bool z = true; for (unsigned k = 5; k < 16; k++) z = z && sl[k] == 0u;
        expect(z && sl[0] == 0x0C000080u && sl[1] == 0x04020F02u && sl[2] == 0x3A02u && sl[3] == 0u && sl[4] == 0u && n48dm_tpl_slot_check(sl) == 0u, "U: the 64-byte slot is 0x0C000080 0x04020F02 0x00003A02 0 0 + 11 zeros and passes the exact-slot check"); }
      // U5. refusal of ANY other sub-0 payload
      { size_t flips = 0, accepted = 0;
        for (unsigned k = 0; k < 16; k++) for (unsigned bit = 0; bit < 32; bit++) { uint32_t sl[16]; n48dm_tpl_slot(sl, &t); sl[k] ^= 1u << bit; ++flips; if (n48dm_tpl_slot_check(sl) == 0u) ++accepted; }
        expect(flips == 16 * 32 && accepted == 1, "U: every single-bit flip of every dword of the template-7 slot (512 flips) is refused, except the one that is template 12 (digd-setup-dvi: digid bit 0, d1 0x04020F02 -> 0x04020F03; section V)"); }
      { struct V { const char *what; unsigned dw; uint32_t val; };
        const V vs[] = { { "digid 0 (DIGA)", 1, 0x04020F00u }, { "digid 1 (DIGB, the live DP's encoder)", 1, 0x04020F01u }, { "digid 4", 1, 0x04020F04u }, { "action 1 (ENABLE)", 1, 0x04020102u }, { "action 0 (DISABLE)", 1, 0x04020002u }, { "action 0x11 (LINK_SETUP)", 1, 0x04021102u },
                         { "action 0x10 (SETUP_PANEL_MODE)", 1, 0x04021002u }, { "digmode 3 (HDMI)", 1, 0x04030F02u }, { "digmode 5 (DP)", 1, 0x04000F02u | (5u << 16) }, { "lanenum 8", 1, 0x08020F02u }, { "lanenum 2", 1, 0x02020F02u },
                         { "pclk 14851", 2, 14851u }, { "pclk 0", 2, 0u }, { "pclk 29700 (297 MHz)", 2, 29700u }, { "bitpercolor 2 (PANEL_8BIT_PER_COLOR)", 3, 0x00000002u }, { "dplinkrate 6", 3, 0x00000600u }, { "reserved[0] 1", 3, 0x00010000u }, { "reserved[1] 1", 3, 0x01000000u },
                         { "dword 4 nonzero", 4, 1u }, { "dword 5 nonzero", 5, 1u }, { "dword 15 nonzero", 15, 0x80000000u } };
        for (const V &v : vs) { uint32_t sl[16]; n48dm_tpl_slot(sl, &t); sl[v.dw] = v.val; expect(n48dm_tpl_slot_check(sl) != 0u, (std::string("U: refused: ") + v.what).c_str()); } }
      { bool none = true; for (uint32_t pb = 0; pb < 64; pb++) { const uint32_t h = 0x80u | (pb << 24); if (n48dm_slot_check(h, 0u, 0u) == 0u || n48dm_slot_check(h, 0x7000u, 0u) == 0u || n48dm_slot_check(h, 0x04020F02u & 0xFFFFu, 0x3A02u) == 0u) none = false; }
        expect(none, "U: n48dm_slot_check refuses every payload_bytes of sub 0 (the ordinary dmubsend and replay paths cannot carry a DIGX_ENCODER_CONTROL)"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &t); sl[0] = 0x04000080u; expect(n48dm_tpl_slot_check(sl) != 0u, "U: the template payload under a 4-byte-payload sub-0 header is refused"); sl[0] = 0x3C000080u; expect(n48dm_tpl_slot_check(sl) != 0u, "U: ... and under a 60-byte one"); sl[0] = 0x0C000180u; expect(n48dm_tpl_slot_check(sl) != 0u, "U: ... and under the sub-1 header with payload_bytes 12"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[4]); sl[0] = N48DM_HDR_DIGENC; expect(n48dm_tpl_slot_check(sl) != 0u, "U: a transmitter payload under the sub-0 header is refused"); }
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x0C000080u, 0x04020F02u & 0xFFFFu, 0x3A02u); expect(o.status == N48DR_HDR_REFUSED && nothing_touched(f), "U: an ordinary send with the sub-0 header: refused, nothing touched"); }
      { FakeDmub f; fresh(f); Env e{f}; f.mem[f.base + 0x6c0] = 0x0C000080u; f.mem[f.base + 0x6c4] = 0x04020F02u; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, 0x6c0u); expect(o.status == N48DR_HDR_REFUSED && f.writes.empty() && f.wptrWrites.empty(), "U: replay of a ring slot that holds a sub-0 header: refused, nothing written"); }
      // U6. the poll bound and the flow
      expect(n48dm_poll_max_for(N48DM_HDR_DIGENC) == 200000u && n48dm_hdr_is_vbios_new(N48DM_HDR_DIGENC) && !n48dm_hdr_is_vbios_new(0x04000080u) && !n48dm_hdr_is_vbios_new(0x0C000081u), "U: the sub-0 header gets the 2 s bound; no other sub-0 header does");
      { FakeDmub f; fresh(f); f.latency = 3; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_DIGC_SETUP_DVI); uint32_t want[16]; n48dm_tpl_slot(want, &t);
        expect(o.status == N48DR_OK && o.sent == 1 && o.vars == 1 && o.hdr == 0x0C000080u && o.d1 == 0x04020F02u && o.d2 == 0x3A02u && o.polls == 4 && o.new_wptr == 0x740, "U: send by template ID 7: OK, sent, 4 polls, header/d1/d2 reported");
        expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740 && f.writes.size() == 2 && f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == std::vector<uint32_t>(want, want + 16) && f.writes[1].off == f.base + 0x3114, "U: the exact 64-byte slot at base + WPTR, one WPTR write, then the variables"); }
      { FakeDmub f; fresh(f); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, N48DM_TPL_DIGC_SETUP_DVI);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 200000u && o.elapsed_us == 2000000u && f.wptrWrites.size() == 1 && o.vars == 0, "U: template 7 that never completes: 200000 polls = 2 s, ONE WPTR write (no retry), no variables"); }
      { FakeDmub f; fresh(f); f.latency = 199999; Env e{f}; expect(n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 7u).status == N48DR_OK, "U: RPTR on the 200000th read is accepted"); }
      { FakeDmub f; fresh(f); f.latency = 200000; Env e{f}; expect(n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 7u).status == N48DR_POLL_TIMEOUT, "U: RPTR on the 200001st read is too late"); }
      { FakeDmub f; fresh(f); f.rptr = 0x6c0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 7u); expect(o.status == N48DR_RING_BUSY && nothing_touched(f), "U: a busy ring refuses template 7"); }
      // U7. the CLI: `digc setup` -> template 7 by ID; `digc disable` has no template and is refused by the CLI itself
      { const std::string cli = slurp(g_root + "/tools/pc/navi48test.c");
        expect(cli.find("!strcmp(a1, \"digc\")") != std::string::npos && cli.find("id = !strcmp(a2, \"setup\") ? N48DM_TPL_DIGC_SETUP_DVI : N48DM_TPL_NONE;") != std::string::npos && cli.find("if (!strcmp(a2, \"disable\")) { fprintf(stderr, \"accel dmubsend digc disable: refused") != std::string::npos && cli.find("digc setup") != std::string::npos,
               "U: the CLI maps `digc setup` to template 7 by ID and refuses `digc disable` (no Linux disable exists)"); }
    }
    // =============================================================================================================================================================================================
    // V (0.0.633). The monitor B's five templates (8..12: pclk-otg2-on / -off, phyd-enable-dvi / phyd-disable, digd-setup-dvi) = templates 3 / 4 / 5 / 6 / 7 with ONLY the instance fields changed. Every byte derived from the
    // Linux structs and helper tables (read live); the rest of the slot is the monitor A's, byte for byte.
    // =============================================================================================================================================================================================
    {
      auto b8 = [](uint32_t d, unsigned k) { return (uint32_t)((d >> (8u * k)) & 0xFFu); };
      const n48dm_tpl &p_on = n48dm_tpls[N48DM_TPL_PCLK_OTG2_ON - 1u], &p_off = n48dm_tpls[N48DM_TPL_PCLK_OTG2_OFF - 1u], &x_en = n48dm_tpls[N48DM_TPL_PHYD_ENABLE_DVI - 1u], &x_dis = n48dm_tpls[N48DM_TPL_PHYD_DISABLE - 1u], &e_set = n48dm_tpls[N48DM_TPL_DIGD_SETUP_DVI - 1u];
      // V1. identity
      expect(N48DM_TPL_PCLK_OTG2_ON == 8u && N48DM_TPL_PCLK_OTG2_OFF == 9u && N48DM_TPL_PHYD_ENABLE_DVI == 10u && N48DM_TPL_PHYD_DISABLE == 11u && N48DM_TPL_DIGD_SETUP_DVI == 12u && N48DM_TPL_COUNT == 15u, "V: ids 8..12 (and fifteen templates since 0.0.634)");
      expect(std::string(p_on.name) == "pclk-otg2-on" && std::string(p_off.name) == "pclk-otg2-off" && std::string(x_en.name) == "phyd-enable-dvi" && std::string(x_dis.name) == "phyd-disable" && std::string(e_set.name) == "digd-setup-dvi", "V: the five names");
      // V2. headers: the same three as the monitor A's (type 0x80, sub, no flag bits, payload_bytes 16 / 60 / 12)
      expect(p_on.dw[0] == N48DM_HDR_PCLK && p_off.dw[0] == N48DM_HDR_PCLK && x_en.dw[0] == N48DM_HDR_XMIT && x_dis.dw[0] == N48DM_HDR_XMIT && e_set.dw[0] == N48DM_HDR_DIGENC, "V: headers 0x10000280 (sub 2, 16 bytes) / 0x3C000180 (sub 1, 60 bytes) / 0x0C000080 (sub 0, 12 bytes)");
      // V3. set_pixel_clock_parameter_v1_7: pixclk_100hz | pll_id, encoderobjid, encoder_mode, miscinfo | crtc_id, deep_color_ratio, reserved1[2] | reserved2
      expect(p_on.dw[1] == 1485000u && p_off.dw[1] == 0u && b8(p_on.dw[2], 0) == 0x17u && b8(p_on.dw[2], 1) == 0x20u && b8(p_on.dw[2], 2) == 2u && b8(p_on.dw[2], 3) == 0u && b8(p_on.dw[3], 0) == 2u && b8(p_on.dw[3], 1) == 0u && b8(p_on.dw[3], 2) == 0u && b8(p_on.dw[3], 3) == 0u && p_on.dw[4] == 0u &&
             p_off.dw[2] == p_on.dw[2] && p_off.dw[3] == p_on.dw[3] && p_off.dw[4] == 0u, "V: pclk-otg2: pixclk_100hz 1485000 (148.5 MHz) / 0 (off), pll_id 0x17 (ATOM_COMBOPHY_PLL3), encoderobjid 0x20, encoder_mode 2 (DVI), miscinfo 0, crtc_id 2 (ATOM_CRTC3), deep colour 0, reserved zero");
      // V4. dmub_dig_transmitter_control_data_v1_7: phyid, action, digmode, lanenum | symclk_10khz | hpdsel, digfe_sel, connobj_id, HPO_instance | TxFFELaneSel, skip_phy_ssc_reduction, reserved2[2]
      expect(b8(x_en.dw[1], 0) == 3u && b8(x_en.dw[1], 1) == 1u && b8(x_en.dw[1], 2) == 2u && b8(x_en.dw[1], 3) == 4u && x_en.dw[2] == 14850u && b8(x_en.dw[3], 0) == 4u && b8(x_en.dw[3], 1) == 0u && b8(x_en.dw[3], 2) == 0x0Cu && b8(x_en.dw[3], 3) == 0u && x_en.dw[4] == 0u, "V: phyd-enable-dvi: phyid 3 (UNIPHYD), action 1 (ENABLE), digmode 2 (DVI), lanenum 4, symclk_10khz 14850, hpdsel 4, digfe_sel 0, connobj_id 0x0C, HPO_instance 0, reserved zero");
      expect(b8(x_dis.dw[1], 0) == 3u && b8(x_dis.dw[1], 1) == 0u && b8(x_dis.dw[1], 2) == 2u && b8(x_dis.dw[1], 3) == 4u && x_dis.dw[2] == 14850u && x_dis.dw[3] == x_en.dw[3] && x_dis.dw[4] == 0u, "V: phyd-disable: the same with action 0 (DISABLE)");
      // V5. dig_encoder_stream_setup_parameters_v1_5: digid, action, digmode, lanenum | pclk_10khz | bitpercolor, dplinkrate_270mhz, reserved[2]
      expect(b8(e_set.dw[1], 0) == 3u && b8(e_set.dw[1], 1) == 0x0Fu && b8(e_set.dw[1], 2) == 2u && b8(e_set.dw[1], 3) == 4u && e_set.dw[2] == 14850u && e_set.dw[3] == 0u && e_set.dw[4] == 0u, "V: digd-setup-dvi: digid 3 (DIGD), action 0x0F (STREAM_SETUP), digmode 2 (DVI), lanenum 4, pclk_10khz 14850, bitpercolor 0, dplinkrate 0, reserved 0");
      // V6. ONLY the instance fields changed: each monitor B template against its monitor A source, dword by dword
      { const n48dm_tpl &a_on = n48dm_tpls[N48DM_TPL_PCLK_OTG1_ON - 1u], &a_off = n48dm_tpls[N48DM_TPL_PCLK_OTG1_OFF - 1u], &c_en = n48dm_tpls[N48DM_TPL_PHYC_ENABLE_DVI - 1u], &c_dis = n48dm_tpls[N48DM_TPL_PHYC_DISABLE - 1u], &g_set = n48dm_tpls[N48DM_TPL_DIGC_SETUP_DVI - 1u];
        expect(p_on.dw[0] == a_on.dw[0] && p_on.dw[1] == a_on.dw[1] && (p_on.dw[2] ^ a_on.dw[2]) == 0x01u && (p_on.dw[3] ^ a_on.dw[3]) == 0x03u && p_on.dw[4] == a_on.dw[4], "V: pclk-otg2-on vs pclk-otg1-on: ONLY pll_id (0x16 -> 0x17) and crtc_id (1 -> 2) differ");
        expect(p_off.dw[0] == a_off.dw[0] && p_off.dw[1] == a_off.dw[1] && (p_off.dw[2] ^ a_off.dw[2]) == 0x01u && (p_off.dw[3] ^ a_off.dw[3]) == 0x03u && p_off.dw[4] == a_off.dw[4], "V: pclk-otg2-off vs pclk-otg1-off: ONLY pll_id and crtc_id differ");
        expect(x_en.dw[0] == c_en.dw[0] && (x_en.dw[1] ^ c_en.dw[1]) == 0x01u && x_en.dw[2] == c_en.dw[2] && (x_en.dw[3] ^ c_en.dw[3]) == 0x07u && x_en.dw[4] == c_en.dw[4], "V: phyd-enable vs phyc-enable: ONLY phyid (2 -> 3) and hpdsel (3 -> 4) differ");
        expect(x_dis.dw[0] == c_dis.dw[0] && (x_dis.dw[1] ^ c_dis.dw[1]) == 0x01u && x_dis.dw[2] == c_dis.dw[2] && (x_dis.dw[3] ^ c_dis.dw[3]) == 0x07u && x_dis.dw[4] == c_dis.dw[4], "V: phyd-disable vs phyc-disable: ONLY phyid and hpdsel differ");
        expect(e_set.dw[0] == g_set.dw[0] && (e_set.dw[1] ^ g_set.dw[1]) == 0x01u && e_set.dw[2] == g_set.dw[2] && e_set.dw[3] == g_set.dw[3] && e_set.dw[4] == g_set.dw[4], "V: digd-setup vs digc-setup: ONLY digid (2 -> 3) differs");
        // and the pll_id / crtc pairing: OTG2's PLL is neither OTG1's (0x16) nor Linux's first free (0x14); the OTG3 probe shares the PLL (0x17) with a different crtc
        expect(b8(p_on.dw[2], 0) != b8(a_on.dw[2], 0) && b8(p_on.dw[2], 0) == b8(n48dm_tpls[N48DM_TPL_PCLK_OTG3_ON - 1u].dw[2], 0) && b8(p_on.dw[3], 0) != b8(n48dm_tpls[N48DM_TPL_PCLK_OTG3_ON - 1u].dw[3], 0), "V: pll 0x17 is the OTG3 probe's PLL too (never run pclk-otg3-on and pclk-otg2-on together); the crtc ids differ"); }
      // V7. the Linux sources the new bytes come from (read live)
      { const std::string base = "/drivers/gpu/drm/amd/";
        auto rd = [&](const std::string &rel) { std::string r; for (const char *root : { "/re/linux-dc", "" }) if (r.empty()) r = slurp(g_root + root + base + rel);
            if (r.empty()) r = slurp(std::string("/path/to/navi48-checkout/re/linux-dc") + base + rel); return r; };
        const std::string atom = rd("include/atomfirmware.h"), hlp = rd("display/dc/bios/dce112/command_table_helper2_dce112.c"), hlp2 = rd("display/dc/bios/command_table2.c"), hlpc = rd("display/dc/bios/command_table_helper2.c"), dm = rd("display/dmub/inc/dmub_cmd.h"), gro = rd("display/include/grph_object_id.h"), ct = hlp2;
        expect(!atom.empty() && !hlp.empty() && !hlp2.empty() && !hlpc.empty() && !dm.empty() && !gro.empty(), "V: the Linux sources were found");
        auto enumVal = [&](const std::string &src, const char *name) -> long { const size_t a = src.find(std::string(name)); if (a == std::string::npos) return -1; size_t q = a + std::strlen(name); while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) ++q; if (q >= src.size() || src[q] != '=') return -2; ++q; while (src[q] == ' ') ++q; return std::strtol(src.c_str() + q, nullptr, 0); };
        expect(enumVal(atom, "ATOM_CRTC3") == 2 && hlpc.find("case CONTROLLER_ID_D2:\n\t\t*atom_id = ATOM_CRTC3;") != std::string::npos, "V: controller_id_to_atom(CONTROLLER_ID_D2) = ATOM_CRTC3 = 2 (OTG2 is controller D2; OTG1 = D1 = ATOM_CRTC2 = 1 is the monitor A's crtc_id 1)");
        expect(enumVal(atom, "ATOM_COMBOPHY_PLL3") == 23 && enumVal(atom, "ATOM_COMBOPHY_PLL2") == 22 && hlp.find("case CLOCK_SOURCE_COMBO_PHY_PLL3:\n\t\t\t*atom_pll_id = ATOM_COMBOPHY_PLL3;") != std::string::npos, "V: clock_source_id_to_atom(CLOCK_SOURCE_COMBO_PHY_PLL3) = ATOM_COMBOPHY_PLL3 = 23 = 0x17 (PLL2 = 22 = 0x16 is the monitor A's)");
        expect(enumVal(atom, "ATOM_TRANSMITTER_V6_HPD4_SEL") == 4 && hlp.find("case HPD_SOURCEID4:\n\t\tatom_hpd_sel = ATOM_TRANSMITTER_V6_HPD4_SEL;") != std::string::npos, "V: hpd_sel_to_atom(HPD_SOURCEID4) = ATOM_TRANSMITTER_V6_HPD4_SEL = 4 (1-based: the monitor B's connector list entry HDMI-A/ddc3/hpd4)");
        expect(dm.find("uint8_t phyid; /**< 0=UNIPHYA, 1=UNIPHYB, 2=UNIPHYC, 3=UNIPHYD, 4=UNIPHYE, 5=UNIPHYF */") != std::string::npos, "V: dmub_dig_transmitter_control_data_v1_7.phyid: 3 = UNIPHYD");
        expect(hlp.find("static uint8_t dig_encoder_sel_to_atom(enum engine_id id)\n{\n\t(void)id;") != std::string::npos && hlp.find("return 0;") != std::string::npos, "V: dig_encoder_sel_to_atom returns 0 for every engine (digfe_sel 0 for DIGD too)");
        expect(enumVal(gro, "CONNECTOR_ID_HDMI_TYPE_A") == 12 && hlp2.find("dig_v1_7.connobj_id = (uint8_t)cntl->connector_obj_id.id;") != std::string::npos, "V: transmitter_control_v1_7 sends connector_obj_id.id, and CONNECTOR_ID_HDMI_TYPE_A = 12 = 0x0C");
        { const size_t e0 = gro.find("enum engine_id {"), e1 = gro.find("ENGINE_ID_DIGD", e0 == std::string::npos ? 0 : e0);
          size_t n = 0, pos = e0; while (e0 != std::string::npos && e1 != std::string::npos && (pos = gro.find("ENGINE_ID_", pos)) != std::string::npos && pos < e1) { ++n; pos += 10; }
          expect(e0 != std::string::npos && e1 != std::string::npos && n == 3u && ct.find("params.digid = (uint8_t)(cntl->engine_id);") != std::string::npos, "V: enum engine_id lists DIGA, DIGB, DIGC before DIGD (DIGD = 3); encoder_control_digx_v1_5 sends digid = (uint8_t)engine_id"); }
        { const std::string notes = slurp(g_root + "/notes/DISPLAY-DESIGN.md"), notes2 = slurp("/path/to/navi48-checkout/notes/DISPLAY-DESIGN.md"); const std::string& nn = notes.empty() ? notes2 : notes;
          expect(nn.find("path 3: display_objid 0x340c (HDMI) encoderobjid 0x2220  device_tag 0x0400   -> link 3, UNIPHY_D, DIGD") != std::string::npos && (0x340cu & 0xFFu) == 0x0Cu && ((0x2220u & 0xFFu) == 0x20u) && ((0x2120u & 0xFFu) == 0x20u),
                 "V: the monitor B's ROM path (notes/DISPLAY-DESIGN.md 3.5): display_objid 0x340c -> connector object id 0x0C (HDMI type A, as the monitor A's 0x330c), encoder object 0x2220 -> id 0x20 (as the monitor A's 0x2120), link 3 / UNIPHY_D / DIGD"); } }
      // V8. the slots and the exact check; every single-bit flip of every dword of every one of the fifteen templates is refused unless it IS another template
      for (unsigned i = 7; i < 12; i++) { uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[i]); bool z = true; for (unsigned k = 5; k < 16; k++) z = z && sl[k] == 0u;
          expect(z && sl[0] == n48dm_tpls[i].dw[0] && sl[1] == n48dm_tpls[i].dw[1] && sl[4] == n48dm_tpls[i].dw[4] && n48dm_tpl_slot_check(sl) == 0u, (std::string(n48dm_tpls[i].name) + ": the 64-byte slot is the five dwords + 11 zeros and passes the exact-slot check").c_str()); }
      { size_t flips = 0, accepted = 0; bool allOther = true; std::set<std::pair<unsigned, unsigned>> pairs;
        for (unsigned i = 0; i < N48DM_TPL_COUNT; i++) for (unsigned k = 0; k < 16; k++) for (unsigned bit = 0; bit < 32; bit++) {
            uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[i]); sl[k] ^= 1u << bit; ++flips;
            if (n48dm_tpl_slot_check(sl) == 0u) { ++accepted; int to = -1; for (unsigned t = 0; t < N48DM_TPL_COUNT; t++) { uint32_t w[16]; n48dm_tpl_slot(w, &n48dm_tpls[t]); if (std::memcmp(w, sl, sizeof w) == 0) to = (int)t; }
                if (to < 0 || (unsigned)to == i) allOther = false; else pairs.insert({ i, (unsigned)to }); } }
        const std::set<std::pair<unsigned, unsigned>> want = { { 0, 7 }, { 7, 0 }, { 1, 8 }, { 8, 1 }, { 4, 5 }, { 5, 4 }, { 9, 10 }, { 10, 9 }, { 6, 11 }, { 11, 6 } };
        expect(flips == 15 * 16 * 32 && allOther && accepted == 10 && pairs == want, "V: of 7680 single-bit flips (15 templates) only 10 are accepted and each IS another template: otg3 <-> otg2 (crtc bit 0) on/off, phyc enable <-> disable, phyd enable <-> disable, digc <-> digd (digid bit 0); the three 1440p templates have NO single-bit neighbour"); }
      // V9. refusals around the monitor B's slots: the monitor A's PLL with the monitor B's crtc, the monitor B's PHY with the monitor A's HPD, a digid that is neither, a crtc that is neither
      { struct R { const char *what; unsigned tpl; unsigned dw; uint32_t val; };
        const R rs[] = { { "pclk crtc 2 with the monitor A's pll 0x16", 7, 2, 0x00022016u }, { "pclk crtc 2 with pll 0x14 (Linux's first free PLL)", 7, 2, 0x00022014u }, { "pclk crtc 2 with pll 0x15", 7, 2, 0x00022015u }, { "pclk crtc 2 with pll 0x18", 7, 2, 0x00022018u }, { "pclk pll 0x17 with crtc 4", 7, 3, 4u }, { "pclk pll 0x17 with crtc 0xC", 7, 3, 0xCu },
                         { "pclk another clock", 7, 1, 0x0016A8C9u }, { "pclk encoder_mode 3 (HDMI)", 7, 2, 0x00032017u }, { "pclk deep colour 1", 7, 3, 0x00000102u }, { "phyd with the monitor A's hpdsel 3", 9, 3, 0x000C0003u }, { "phyd with hpdsel 5", 9, 3, 0x000C0005u }, { "phyd with connobj 0x0B", 9, 3, 0x000B0004u },
                         { "phyd with digfe_sel 4", 9, 3, 0x000C0404u }, { "phyd with phyid 2 (the monitor A's PHY C) and hpdsel 4", 9, 1, 0x04020102u }, { "phyd with phyid 1 (PHY B, the live DP)", 9, 1, 0x04020101u }, { "phyd action 3", 9, 1, 0x04020303u }, { "phyd digmode 3 (HDMI)", 9, 1, 0x04030103u },
                         { "phyd lanenum 2", 9, 1, 0x02020103u }, { "phyd symclk 29700", 9, 2, 29700u }, { "digd digid 2 with the setup of digd's other fields (that is digc: valid, so skipped)", 11, 1, 0x04020F04u }, { "digd digid 1 (DIGB, the live DP's encoder)", 11, 1, 0x04020F01u }, { "digd action 1 (ENABLE)", 11, 1, 0x04020103u } };
        for (const R &r : rs) { if (std::string(r.what).find("(that is digc") != std::string::npos) continue; uint32_t sl[16]; n48dm_tpl_slot(sl, &n48dm_tpls[r.tpl]); sl[r.dw] = r.val; expect(n48dm_tpl_slot_check(sl) != 0u, (std::string("V: refused: ") + r.what).c_str()); } }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &x_en); sl[0] = N48DM_HDR_DIGENC; expect(n48dm_tpl_slot_check(sl) != 0u, "V: a monitor B transmitter payload under the sub-0 header is refused"); sl[0] = N48DM_HDR_PCLK; expect(n48dm_tpl_slot_check(sl) != 0u, "V: ... and under the pixel-clock header"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &e_set); sl[0] = N48DM_HDR_XMIT; expect(n48dm_tpl_slot_check(sl) != 0u, "V: the digd payload under the transmitter header is refused"); }
      { bool none = true; for (unsigned t = 7; t < 12; t++) for (uint32_t pb = 0; pb < 64; pb++) for (uint32_t sub = 0; sub <= 2; sub++) { const uint32_t h = 0x80u | (sub << 8) | (pb << 24); if (n48dm_slot_check(h, n48dm_tpls[t].dw[1] & 0xFFFFu, n48dm_tpls[t].dw[2] & 0xFFFFu) == 0u) none = false; }
        expect(none, "V: n48dm_slot_check refuses every sub 0 / 1 / 2 header with the monitor B's payloads (the ordinary dmubsend and replay paths cannot carry them)"); }
      // V10. the flow over the fake DMUB: each monitor B template, exact bytes, one WPTR write, the 2 s bound, no retry
      for (unsigned i = 8; i <= 12; i++) { FakeDmub f; fresh(f); f.latency = 3; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, i);
          const std::string nm = n48dm_tpls[i - 1].name; uint32_t want[16]; n48dm_tpl_slot(want, &n48dm_tpls[i - 1]);
          expect(o.status == N48DR_OK && o.sent == 1 && o.vars == 1 && o.hdr == want[0] && o.d1 == want[1] && o.d2 == want[2] && o.polls == 4 && o.new_wptr == 0x740, (nm + ": send by template ID: OK, sent, 4 polls, header/d1/d2 reported").c_str());
          expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740 && f.writes.size() == 2 && f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == std::vector<uint32_t>(want, want + 16) && f.writes[1].off == f.base + 0x3114, (nm + ": the exact 64-byte slot at base + WPTR, one WPTR write, then the variables").c_str());
          FakeDmub g; fresh(g); g.latency = -1; Env e2{g}; const n48dm_send_out o2 = n48dm::send(e2, 0u, 0u, 0u, N48DM_NO_REPLAY, i);
          expect(o2.status == N48DR_POLL_TIMEOUT && o2.polls == 200000u && o2.elapsed_us == 2000000u && g.wptrWrites.size() == 1 && o2.vars == 0, (nm + ": a template that never completes: 200000 polls = 2 s, ONE WPTR write (no retry), no variables").c_str());
          FakeDmub b; fresh(b); b.rptr = 0x6c0; Env e3{b}; expect(n48dm::send(e3, 0u, 0u, 0u, N48DM_NO_REPLAY, i).status == N48DR_RING_BUSY && nothing_touched(b), (nm + ": a busy ring refuses it").c_str()); }
      { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, 16u); expect(o.status == N48DR_PAYLOAD_REFUSED && nothing_touched(f), "V: template ID 16 is refused, nothing touched"); }
      // V11. the CLI maps the new words to their own IDs, by ID only
      { const std::string cli = slurp(g_root + "/tools/pc/navi48test.c");
        expect(cli.find("!strcmp(a1, \"phyd\")") != std::string::npos && cli.find("!strcmp(a1, \"digd\")") != std::string::npos && cli.find("!strcmp(a2, \"otg2-on\") ? N48DM_TPL_PCLK_OTG2_ON : !strcmp(a2, \"otg2-off\") ? N48DM_TPL_PCLK_OTG2_OFF : !strcmp(a2, \"otg2-1440-on\") ? N48DM_TPL_PCLK_OTG2_1440_ON : N48DM_TPL_NONE;") != std::string::npos &&
               cli.find("else if (!strcmp(a1, \"phyd\")) id = !strcmp(a2, \"enable\") ? N48DM_TPL_PHYD_ENABLE_DVI : !strcmp(a2, \"disable\") ? N48DM_TPL_PHYD_DISABLE : !strcmp(a2, \"enable-1440\") ? N48DM_TPL_PHYD_ENABLE_DVI_1440 : N48DM_TPL_NONE;") != std::string::npos &&
               cli.find("id = !strcmp(a2, \"setup\") ? N48DM_TPL_DIGD_SETUP_DVI : !strcmp(a2, \"setup-1440\") ? N48DM_TPL_DIGD_SETUP_DVI_1440 : N48DM_TPL_NONE;") != std::string::npos && cli.find("if (!strcmp(a2, \"disable\")) { fprintf(stderr, \"accel dmubsend digd disable: refused") != std::string::npos && cli.find("|| !strcmp(a1, \"phyd\") || !strcmp(a1, \"digd\")))") != std::string::npos,
               "V: the CLI has `pclk otg2-on|otg2-off`, `phyd enable|disable`, `digd setup` (no digd disable), each sent by template ID");
        expect(cli.find("otg2-on|otg2-off|phyc enable|disable|phyd enable|disable|digc setup|digd setup") != std::string::npos, "V: the usage line lists the new words"); }
    }
    // =============================================================================================================================================================================================
    // W (0.0.634, stage M4b). The monitor B at 2560x1440@60 (241.50 MHz): templates 13 / 14 / 15 = templates 8 / 10 / 12 with ONLY the clock field changed. The arithmetic, the field decode of the 1080p twins (lane count,
    // digmode, link) against Linux's structs and the Linux code that fills them, the twin identity, the flow, the CLI words. A change of the lane / link field would have STOPPED the build; the decode shows none.
    // =============================================================================================================================================================================================
    {
      auto b8 = [](uint32_t d, unsigned k) { return (uint32_t)((d >> (8u * k)) & 0xFFu); };
      auto linux_src = [&](const char *rel) { std::vector<std::string> roots; if (const char *e = std::getenv("N48_LINUX_DC")) roots.push_back(e); roots.push_back(g_root); roots.push_back("/path/to/navi48-checkout");
          for (auto &r : roots) { std::string t = slurp(r + "/re/linux-dc/drivers/gpu/drm/amd/display/" + rel); if (!t.empty()) return t; } return std::string(); };
      const n48dm_tpl &p_on = n48dm_tpls[N48DM_TPL_PCLK_OTG2_ON - 1u], &x_en = n48dm_tpls[N48DM_TPL_PHYD_ENABLE_DVI - 1u], &x_dis = n48dm_tpls[N48DM_TPL_PHYD_DISABLE - 1u], &e_set = n48dm_tpls[N48DM_TPL_DIGD_SETUP_DVI - 1u];
      const n48dm_tpl &p14 = n48dm_tpls[N48DM_TPL_PCLK_OTG2_1440_ON - 1u], &x14 = n48dm_tpls[N48DM_TPL_PHYD_ENABLE_DVI_1440 - 1u], &e14 = n48dm_tpls[N48DM_TPL_DIGD_SETUP_DVI_1440 - 1u];
      // W1. identity
      expect(N48DM_TPL_PCLK_OTG2_1440_ON == 13u && N48DM_TPL_PHYD_ENABLE_DVI_1440 == 14u && N48DM_TPL_DIGD_SETUP_DVI_1440 == 15u && N48DM_TPL_COUNT == 15u, "W: ids 13 / 14 / 15, fifteen templates");
      expect(std::string(p14.name) == "pclk-otg2-1440-on" && std::string(x14.name) == "phyd-enable-dvi-1440" && std::string(e14.name) == "digd-setup-dvi-1440", "W: the three names");
      // W2. the arithmetic, from the EDID descriptor's own clock: the CTA DTD bytes `56 5e` are 0x5e56 = 24150 in units of 10 kHz = 241.50 MHz
      { const uint32_t dtd10khz = 0x56u | (0x5eu << 8), hz = dtd10khz * 10000u;
        expect(dtd10khz == 24150u && hz == 241500000u, "W: the DTD clock `56 5e` = 0x5e56 = 24150 x 10 kHz = 241.5 MHz");
        expect(p14.dw[1] == 0x0024D998u && p14.dw[1] == hz / 100u && hz / 100u == 2415000u, "W: pclk d1 = 241.5 MHz / 100 Hz = 2415000 = 0x0024D998 (CONFIRMED; the 1080p twin: 1485000 = 0x0016A8C8)");
        expect(x14.dw[2] == 0x5E56u && x14.dw[2] == hz / 10000u && e14.dw[2] == 0x5E56u && e14.dw[2] == hz / 10000u && hz / 10000u == 24150u, "W: symclk_10khz = 241.5 MHz / 10 kHz = 24150 = 0x5E56 in phyd-enable and digd-setup (CONFIRMED; the 1080p twins: 14850 = 0x3A02)");
        expect(p_on.dw[1] == 148500000u / 100u && x_en.dw[2] == 148500000u / 10000u && e_set.dw[2] == 148500000u / 10000u, "W: the same arithmetic reproduces the 1080p twins' 0x0016A8C8 and 0x3A02"); }
      // W3. twin identity: every dword except the clock field is the 1080p twin's
      expect(p14.dw[0] == p_on.dw[0] && p14.dw[2] == p_on.dw[2] && p14.dw[3] == p_on.dw[3] && p14.dw[4] == p_on.dw[4] && p14.dw[1] != p_on.dw[1], "W: pclk-otg2-1440-on = pclk-otg2-on with ONLY d1 (pixclk_100hz) changed");
      expect(x14.dw[0] == x_en.dw[0] && x14.dw[1] == x_en.dw[1] && x14.dw[3] == x_en.dw[3] && x14.dw[4] == x_en.dw[4] && x14.dw[2] != x_en.dw[2], "W: phyd-enable-dvi-1440 = phyd-enable-dvi with ONLY d2 (symclk_10khz) changed");
      expect(e14.dw[0] == e_set.dw[0] && e14.dw[1] == e_set.dw[1] && e14.dw[3] == e_set.dw[3] && e14.dw[4] == e_set.dw[4] && e14.dw[2] != e_set.dw[2], "W: digd-setup-dvi-1440 = digd-setup-dvi with ONLY d2 (pclk_10khz) changed");
      // W4. the field decode of the transmitter payload d1 = 0x04020103 (the brief's item 2a) against dmub_dig_transmitter_control_data_v1_7: byte 0 phyid, byte 1 action, byte 2 mode_laneset.digmode, byte 3 lanenum
      expect(x_en.dw[1] == 0x04020103u && b8(x_en.dw[1], 0) == 3u && b8(x_en.dw[1], 1) == 1u && b8(x_en.dw[1], 2) == 2u && b8(x_en.dw[1], 3) == 4u, "W: d1 0x04020103 = phyid 3 (UNIPHYD), action 1 (ENABLE), digmode 2 (ATOM_ENCODER_MODE_DVI), lanenum 4");
      expect(b8(x_en.dw[3], 0) == 4u && b8(x_en.dw[3], 1) == 0u && b8(x_en.dw[3], 2) == 0x0Cu && b8(x_en.dw[3], 3) == 0u && x14.dw[3] == x_en.dw[3], "W: d3 0x000C0004 = hpdsel 4, digfe_sel 0, connobj_id 0x0C (HDMI type A), HPO_instance 0 - unchanged at 1440p");
      expect(x14.dw[1] == x_en.dw[1] && b8(x14.dw[1], 3) == 4u, "W: the 1440p transmitter carries lanenum 4 (single link) exactly as 1080p: the lane / link field does NOT change with the clock");
      expect(b8(e14.dw[1], 0) == 3u && b8(e14.dw[1], 1) == 0x0Fu && b8(e14.dw[1], 2) == 2u && b8(e14.dw[1], 3) == 4u && e14.dw[1] == e_set.dw[1], "W: digd-setup d1 0x04020F03 = digid 3, action 0x0F (STREAM_SETUP), digmode 2, lanenum 4 - unchanged at 1440p");
      // W5. what Linux does (read live): lanenum 4 for every non-dual-link TMDS enable; symclk_10khz = pixel_clock / 10 (pixel_clock in kHz); no dual link on dcn401; the disable carries no pixel clock
      { const std::string enc = linux_src("dc/dio/dcn10/dcn10_link_encoder.c"), ct = linux_src("dc/bios/command_table2.c"), st = linux_src("dc/core/dc_stream.c"), lk = linux_src("dc/link/link_detection.c"), rs = linux_src("dc/resource/dcn401/dcn401_resource.c");
        const size_t en = enc.find("void dcn10_link_encoder_enable_tmds_output(");
        expect(en != std::string::npos && enc.find("if (cntl.signal == SIGNAL_TYPE_DVI_DUAL_LINK)\n\t\tcntl.lanes_number = 8;\n\telse\n\t\tcntl.lanes_number = 4;", en) != std::string::npos, "W: Linux dcn10_link_encoder_enable_tmds_output: lanes_number = 8 only for SIGNAL_TYPE_DVI_DUAL_LINK, else 4");
        expect(ct.find("dig_v1_7.lanenum = (uint8_t)cntl->lanes_number;") != std::string::npos && ct.find("dig_v1_7.symclk_units.symclk_10khz = cntl->pixel_clock/10;") != std::string::npos, "W: Linux transmitter_control_v1_7: lanenum = lanes_number, symclk_10khz = pixel_clock / 10 (kHz -> 10 kHz)");
        expect(ct.find("clk.pixclk_100hz = cpu_to_le32(bp_params->target_pixel_clock_100hz);") != std::string::npos, "W: Linux set_pixel_clock_v7: pixclk_100hz = target_pixel_clock_100hz");
        const size_t di = enc.find("void dcn10_link_encoder_disable_output(");
        const size_t de = di == std::string::npos ? di : enc.find("link_transmitter_control(enc10, &cntl);", di);
        expect(di != std::string::npos && de != std::string::npos && enc.substr(di, de - di).find("pixel_clock") == std::string::npos && enc.substr(di, de - di).find("struct bp_transmitter_control cntl = { 0 };") != std::string::npos,
               "W: Linux dcn10_link_encoder_disable_output sends a zero-initialised control with NO pixel_clock: the disable template's symclk never has to match the enable's, so templates 6 / 11 are reused unchanged");
        expect(!rs.empty() && rs.find("dual_link_dvi") == std::string::npos && st.find("stream->ctx->dc->caps.dual_link_dvi &&") != std::string::npos && lk.find("case CONNECTOR_ID_HDMI_TYPE_A:\n\t\t\treturn SIGNAL_TYPE_HDMI_TYPE_A;") != std::string::npos,
               "W: Linux picks DVI dual link only when caps.dual_link_dvi (never set for dcn401: resource/dcn401 has no dual_link_dvi) and an HDMI connector is SIGNAL_TYPE_HDMI_TYPE_A (single link): 241.5 MHz needs no second link");
        expect(x_dis.dw[2] == 0x3A02u && n48dm_tpls[N48DM_TPL_PHYC_DISABLE - 1u].dw[2] == 0x3A02u, "W: the reused disable templates keep their 1080p symclk 14850 (harmless: Linux's disable carries none)"); }
      // W6. refusals: the 1440p clock with the wrong instance fields, an off-by-one clock, the 1080p clock with the 1440p twin's neighbours
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &p14); sl[2] = 0x00022016u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: pclk 1440 with the monitor A's PLL 0x16: refused"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &p14); sl[1] = 0x0024D999u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: pclk 241.5001 MHz: refused"); sl[1] = 0x0024D997u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: pclk 241.4999 MHz: refused"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &x14); sl[2] = 24151u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: phyd 1440 symclk 24151: refused"); sl[2] = 24149u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: phyd 1440 symclk 24149: refused"); sl[2] = 14850u; sl[3] = 0x000C0003u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: phyd with the monitor A's hpdsel: refused"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &e14); sl[2] = 24151u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: digd 1440 pclk 24151: refused"); sl[2] = 0x5E56u; sl[1] = 0x04020F02u; expect(n48dm_tpl_slot_check(sl) != 0u, "W: digc (digid 2) with the 1440p clock: refused (no digc-1440 exists)"); }
      { uint32_t sl[16]; n48dm_tpl_slot(sl, &x14); sl[0] = N48DM_HDR_DIGENC; expect(n48dm_tpl_slot_check(sl) != 0u, "W: the 1440p transmitter payload under the sub-0 header: refused"); }
      { bool none = true; for (unsigned t = 12; t < 15; t++) for (uint32_t pb = 0; pb < 64; pb++) for (uint32_t sub = 0; sub <= 2; sub++) { const uint32_t h = 0x80u | (sub << 8) | (pb << 24); if (n48dm_slot_check(h, n48dm_tpls[t].dw[1] & 0xFFFFu, n48dm_tpls[t].dw[2] & 0xFFFFu) == 0u) none = false; }
        expect(none, "W: n48dm_slot_check refuses every sub 0 / 1 / 2 header with the 1440p payloads (the ordinary dmubsend and replay paths cannot carry them)"); }
      // W7. the flow over the fake DMUB: each 1440p template, exact bytes, one WPTR write, the 2 s bound, no retry (REACHABILITY: the real n48dm::send with the new IDs)
      for (unsigned i = 13; i <= 15; i++) { FakeDmub f; fresh(f); f.latency = 3; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0u, 0u, 0u, N48DM_NO_REPLAY, i);
          const std::string nm = n48dm_tpls[i - 1].name; uint32_t want[16]; n48dm_tpl_slot(want, &n48dm_tpls[i - 1]);
          expect(o.status == N48DR_OK && o.sent == 1 && o.vars == 1 && o.hdr == want[0] && o.d1 == want[1] && o.d2 == want[2] && o.polls == 4 && o.new_wptr == 0x740, (nm + ": send by template ID: OK, sent, 4 polls, header/d1/d2 reported").c_str());
          expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740 && f.writes.size() == 2 && f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == std::vector<uint32_t>(want, want + 16), (nm + ": the exact 64-byte slot at base + WPTR, one WPTR write").c_str());
          FakeDmub g; fresh(g); g.latency = -1; Env e2{g}; const n48dm_send_out o2 = n48dm::send(e2, 0u, 0u, 0u, N48DM_NO_REPLAY, i);
          expect(o2.status == N48DR_POLL_TIMEOUT && o2.polls == 200000u && o2.elapsed_us == 2000000u && g.wptrWrites.size() == 1, (nm + ": a template that never completes: 2 s, ONE WPTR write (no retry)").c_str()); }
      // W8. the CLI words, mapped to their own IDs, appended after the 1080p words (so the 1080p mapping is byte-identical)
      { const std::string cli = slurp(g_root + "/tools/pc/navi48test.c");
        expect(cli.find("!strcmp(a2, \"otg2-1440-on\") ? N48DM_TPL_PCLK_OTG2_1440_ON") != std::string::npos && cli.find("!strcmp(a2, \"enable-1440\") ? N48DM_TPL_PHYD_ENABLE_DVI_1440") != std::string::npos && cli.find("!strcmp(a2, \"setup-1440\") ? N48DM_TPL_DIGD_SETUP_DVI_1440") != std::string::npos,
               "W: the CLI words `pclk otg2-1440-on`, `phyd enable-1440`, `digd setup-1440` map to templates 13 / 14 / 15");
        expect(cli.find("1440 words: pclk otg2-1440-on, phyd enable-1440, digd setup-1440") != std::string::npos, "W: the usage line lists the three 1440p words");
        { const size_t t0 = cli.find("const struct n48dm_tpl *tp = n48dm_tpl_get(id);"), t1 = t0 == std::string::npos ? t0 : cli.find("if (a1 && !strcmp(a1, \"detect\")) kind = N48DM_K_DETECT;", t0);
          const std::string br = (t0 == std::string::npos || t1 == std::string::npos) ? std::string() : cli.substr(t0, t1 - t0);
          const size_t r0 = br.find("if (dp_watch_read(dpw0) != 0) { printf(\"accel dmubsend %s %s: REFUSED by the CLI: the DP watch (dispcensus page 10) could not be read; nothing was sent\\n\", a1, a2); return 1; }"),
                       sd = br.find("if (dr_call(N48DM_ACT_SEND, n48dm_tpl_arg(id), o) != 0) return 1;"), r1 = br.find("if (dp_watch_read(dpw1) != 0)"), rp = br.find("(void)dp_watch_report(\"dmubsend template\", dpw0, dpw1);");
          expect(!br.empty() && r0 != std::string::npos && sd != std::string::npos && r1 != std::string::npos && rp != std::string::npos && r0 < sd && sd < r1 && r1 < rp,
                 "W: REACHABILITY (item 4): every template send reads the DP watch BEFORE (and refuses to send if it cannot), sends, reads it AFTER and compares - in that order"); }
        { const size_t u0 = cli.find("static unsigned dp_watch_report("); const std::string body = u0 == std::string::npos ? "" : cli.substr(u0, 1400);
          expect(body.find("if (changed) printf(\"  *** DP DISTURBED (%s): %u of %u DP plane registers changed; STOP, run the ROLLBACK, do not retry this boot ***\\n\"") != std::string::npos && body.find("  *** CHANGED ***") != std::string::npos && body.find("underflow 30:28") != std::string::npos, "W: the DP watch report says DP DISTURBED, marks every CHANGED register and prints HUBP0's underflow bits 30:28"); }
        expect(cli.find("dp_watch_read(dpw0)") != std::string::npos && cli.find("dp_watch_report(\"dmubsend template\", dpw0, dpw1)") != std::string::npos, "W: the template branch compares the DP watch before and after (item 4)"); }
    }
    // D7 the variables
    { FakeDmub f; fresh(f); f.failVarsWrite = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_VARS_FAILED && o.sent == 1 && o.vars == 0 && o.rptr_end == 0x740 && f.wptrWrites.size() == 1, "a failed variables write: its own status, the command had completed, no resend"); }
    { FakeDmub f; fresh(f); f.corruptVarsRead = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
      expect(o.status == N48DR_VARS_FAILED && o.vars == 0, "a variables read-back mismatch is reported"); }
    // D8 only the named VRAM may be written
    { for (uint32_t startPtr : { 0u, 0x40u, 0x700u, 0x1f80u }) { FakeDmub f; fresh(f); f.wptr = f.rptr = startPtr; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x08000680u, 0u, 0x101u);
          expect(o.status == N48DR_OK, "ok at each start pointer");
          for (const WriteRec &w : f.writes) { const bool slot = w.off == f.base + startPtr && w.n == 16, vars = w.off == f.base + 0x3114 && w.n == 3; expect(slot || vars, "every VRAM write is the slot at base + WPTR or the three variables"); }
          for (const WriteRec &w : f.writes) expect(!(w.off <= f.base + 0x3100 && f.base + 0x3100 < w.off + 4ull * w.n), "+0x3100 is never inside a write"); } }
    // D9 the result layout round trip
    { n48dm_send_out o; std::memset(&o, 0, sizeof(o)); o.status = 29; o.sent = 1; o.vars = 0; o.polls = 10000; o.old_wptr = 0x700; o.new_wptr = 0x740; o.rptr_end = 0x700; o.elapsed_us = 100000; o.frames0 = 7; o.frames1 = 9; o.hdr = 0x04000a80u; o.d1 = 0x7000u; o.d2 = 5; o.rptr_start = 0x700; o.base = 0x3dac00000ull;
      uint64_t v[13]; n48dm_send_pack(v, &o); n48dm_send_out p; std::memset(&p, 0, sizeof(p)); n48dm_send_unpack(v, &p);
      expect(!std::memcmp(&o, &p, sizeof(o)) || (p.status == o.status && p.sent == o.sent && p.vars == o.vars && p.polls == o.polls && p.old_wptr == o.old_wptr && p.new_wptr == o.new_wptr && p.rptr_end == o.rptr_end && p.elapsed_us == o.elapsed_us && p.frames0 == o.frames0 && p.frames1 == o.frames1 && p.hdr == o.hdr && p.d1 == o.d1 && p.d2 == o.d2 && p.rptr_start == o.rptr_start && p.base == o.base), "dmubsend's result packs and unpacks field for field"); }

    // =============================================================================================================================================================================================
    // E. dmubmode and dmubctx
    // =============================================================================================================================================================================================
    // E1 the gate: exhaustive over small ops and some large ones
    { n48dm_mode_state s; std::memset(&s, 0, sizeof(s)); s.saved = 1;
      unsigned okOps = 0; std::set<uint64_t> got;
      for (uint64_t op = 0; op < 0x2000; op++) if (n48dm_mode_op_ok(op)) { ++okOps; got.insert(op); }
      for (uint64_t op : { 0x100000000ull + 1, 0x100000000ull + 2, 0x100000000ull + 0x1a6, 0xffffffffffffffffull, 0x8000000000000000ull, 0x41a6ull, 0x1a60ull }) expect(!n48dm_mode_op_ok(op), "a large or shifted op is refused");
      expect(okOps == 4 && got == std::set<uint64_t>({ 1, 2, 0x1a6, 0x1d4 }), "exactly 1 (save), 2 (restore), 0x1a6 and 0x1d4 are accepted");
      for (uint64_t op : { 0ull, 3ull, 0x1a5ull, 0x1a7ull, 0x1d9ull, 0x1d3ull, 0x70ull, 0x100ull }) expect(n48dm_mode_gate(op, &s) == N48DR_MODE_REFUSED, "any other VBE mode is refused with its own status"); }
    // E2 save / write / restore flow
    { const uint32_t dpBlock[8] = { 0x05a00a00u, 0x00020a00u, 0x41d40001u, 0x08081008u, 0x00000008u, 0x000000e2u, 0u, 0u };
      FakeDmub f; fresh(f); for (int i = 0; i < 8; i++) f.mem[f.base + 0x4c00 + 4 * i] = dpBlock[i]; f.mem[f.base + 0x4c20] = 0x05a00a00u;     // the VBE scratch copy next to it must stay
      Env e{f}; n48dm_mode_state s; std::memset(&s, 0, sizeof(s));
      n48dm::ModeResult r = n48dm::mode(e, N48DM_VBE_1080P, s);
      expect(r.status == N48DR_NO_SAVE && nothing_touched(f) && f.probes == 0, "dmubmode 0x1a6 without a prior save: NO_SAVE, nothing read or written");
      r = n48dm::mode(e, N48DM_VBE_DP, s); expect(r.status == N48DR_NO_SAVE && nothing_touched(f), "dmubmode 0x1d4 without a prior save: NO_SAVE");
      r = n48dm::mode(e, N48DM_MODE_RESTORE, s); expect(r.status == N48DR_NO_SAVE && nothing_touched(f), "dmubmode restore without a prior save: NO_SAVE");
      r = n48dm::mode(e, N48DM_MODE_SAVE, s);
      expect(r.status == N48DR_OK && s.saved && !s.dirty && f.writes.empty() && f.writeCalls == 0 && !std::memcmp(s.buf, dpBlock, 32) && !std::memcmp(r.block, dpBlock, 32), "save copies +0x4c00..+0x4c1f into the kext, returns it, writes NOTHING");
      expect(f.readCalls == 1 && r.base == f.base, "save is one 8-dword read at base + 0x4c00");
      r = n48dm::mode(e, N48DM_VBE_1080P, s);
      static const uint32_t b1a6[8] = { 0x04380780u, 0x00020780u, 0x41a60001u, 0x08081008u, 0x00000008u, 0x3a02007fu, 0u, 0u };
      expect(r.status == N48DR_OK && s.dirty && f.writes.size() == 1 && f.writes[0].off == f.base + 0x4c00 && f.writes[0].n == 8 && !std::memcmp(f.writes[0].d.data(), b1a6, 32) && !std::memcmp(r.block, b1a6, 32), "0x1a6 writes exactly the plan's 1080p block at +0x4c00 and reads it back");
      expect(!std::memcmp(r.before, dpBlock, 32), "the block read before the write is reported");
      expect(f.mem[f.base + 0x4c20] == 0x05a00a00u, "+0x4c20 (the VBE scratch copy) is not touched");
      r = n48dm::mode(e, N48DM_MODE_SAVE, s); expect(r.status == N48DR_SAVE_DIRTY && s.saved && !std::memcmp(s.buf, dpBlock, 32), "a second save while our block is in the window is refused: the original stays");
      r = n48dm::mode(e, N48DM_VBE_DP, s);
      expect(r.status == N48DR_OK && f.writes.size() == 2 && !std::memcmp(f.writes[1].d.data(), dpBlock, 32), "0x1d4 writes exactly the measured DP block");
      r = n48dm::mode(e, N48DM_VBE_1080P, s); expect(r.status == N48DR_OK, "and back to 1080p");
      r = n48dm::mode(e, N48DM_MODE_RESTORE, s);
      expect(r.status == N48DR_OK && !s.dirty && f.writes.size() == 4 && !std::memcmp(f.writes[3].d.data(), dpBlock, 32) && !std::memcmp(r.block, dpBlock, 32), "restore writes the saved copy back, verified, and clears the dirty flag");
      r = n48dm::mode(e, N48DM_MODE_SAVE, s); expect(r.status == N48DR_OK, "after a restore a save is allowed again");
      for (const WriteRec &w : f.writes) expect(w.off == f.base + 0x4c00 && w.n == 8, "every mode write is the 8 dwords at +0x4c00"); }
    // E3 mode refusals and failures
    { FakeDmub f; fresh(f); Env e{f}; n48dm_mode_state s; std::memset(&s, 0, sizeof(s));
      for (uint64_t op : { 0ull, 3ull, 0x1a5ull, 0x1d9ull, 0x4c00ull }) { const n48dm::ModeResult r = n48dm::mode(e, op, s); expect(r.status == N48DR_MODE_REFUSED && nothing_touched(f) && f.probes == 0, "refused ops touch nothing"); }
      f.enabled = 0; expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_DMUB_NOT_READY && f.readCalls == 0, "DMCUB not enabled: refused before any VRAM access");
      f.enabled = 1; f.r4Enabled = 0; expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_REGION4_BAD && f.readCalls == 0, "REGION4 disabled: refused before any VRAM access");
      f.r4Enabled = 1; f.failProbe = true; expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_NO_DEVICE, "no device"); }
    { FakeDmub f; fresh(f); Env e{f}; n48dm_mode_state s; std::memset(&s, 0, sizeof(s)); (void)n48dm::mode(e, N48DM_MODE_SAVE, s);
      f.corruptWriteNo = f.writeCalls; const n48dm::ModeResult r = n48dm::mode(e, N48DM_VBE_1080P, s);
      expect(r.status == N48DR_READBACK_MISMATCH && s.dirty, "a mode write that does not read back: its own status, and the original is flagged as not in the window");
      f.corruptWriteNo = -1; f.failAnyWrite = true; expect(n48dm::mode(e, N48DM_VBE_DP, s).status == N48DR_SLOT_WRITE_FAILED, "a refused write has its own status");
      f.failAnyWrite = false; f.corruptWriteNo = f.writeCalls; const n48dm::ModeResult q = n48dm::mode(e, N48DM_MODE_RESTORE, s);
      expect(q.status == N48DR_READBACK_MISMATCH && s.dirty, "a restore that does not read back stays dirty"); }
    // E4 dmubctx
    { for (uint32_t ctx : { 0x6800u, 0u, 0x6000u, 0x7001u, 0x7400u, 0x7c00u, 0x8000u, 0xffffu }) {
          FakeDmub f; fresh(f); Env e{f}; const n48dm::CtxResult r = n48dm::ctx(e, n48dm_ctx_arg(ctx, 0x01020006u));
          expect(r.status == N48DR_CTX_REFUSED && nothing_touched(f) && f.probes == 0 && f.readCalls == 0, "dmubctx refuses a context outside {0x7000, 0x7800} (0x6800 included), touching nothing"); }
      for (uint64_t a : { 0x17000ull, 0x27800ull, 0x10000ull | 0x7000ull, 0x00080000ull | 0x7800ull }) { FakeDmub f; fresh(f); Env e{f}; expect(n48dm::ctx(e, a).status == N48DR_CTX_REFUSED && nothing_touched(f), "bits 16..31 of the argument must be zero"); }
      expect(n48dm_ctx_gate(n48dm_ctx_arg(0x6800u, 0)) == N48DR_CTX_REFUSED && n48dm_ctx_gate(n48dm_ctx_arg(0x7000u, 0)) == 0u && n48dm_ctx_gate(n48dm_ctx_arg(0x7800u, 0xffffffffu)) == 0u, "ctx gate: 0x6800 refused, 0x7000 / 0x7800 allowed with any value");
      for (uint32_t ctx : { 0x7000u, 0x7800u }) for (uint32_t val : { 0u, 0x01020006u, 0xffffffffu }) {
          FakeDmub f; fresh(f); f.mem[f.base + ctx + 0x1a0] = 0x01020006u; Env e{f}; const n48dm::CtxResult r = n48dm::ctx(e, n48dm_ctx_arg(ctx, val));
          expect(r.status == N48DR_OK && r.before == 0x01020006u && r.after == val && r.base == f.base, "dmubctx writes the dword and reads it back");
          expect(f.writes.size() == 1 && f.writes[0].off == f.base + ctx + 0x1a0 && f.writes[0].n == 1 && f.writes[0].d[0] == val, "exactly ONE VRAM dword at base + ctx + 0x1a0 is written");
          expect(f.wptrWrites.empty(), "no register is written"); }
      FakeDmub f; fresh(f); f.corruptWriteNo = 0; Env e{f}; expect(n48dm::ctx(e, n48dm_ctx_arg(0x7000u, 0)).status == N48DR_READBACK_MISMATCH, "a marker that does not read back: its own status");
      FakeDmub g; fresh(g); g.enabled = 0; Env e2{g}; expect(n48dm::ctx(e2, n48dm_ctx_arg(0x7000u, 0)).status == N48DR_DMUB_NOT_READY && g.writeCalls == 0, "DMCUB not enabled: refused"); }

    // =============================================================================================================================================================================================
    // F. 0.0.626: the fixes the 0.0.625 review required (F5 sub 8, F6 idle ring for mode / ctx, F7 ring size, F8 replay)
    // =============================================================================================================================================================================================
    // F5 sub 8 (disable) only on 0x7000 / 0x7800, never 0x6800
    { expect(n48dm_slot_check(0x04000880u, 0x6800u, 0u) == N48DR_PAYLOAD_REFUSED, "F5: disable on 0x6800 (the live DP) is refused by the allowlist");
      expect(n48dm_slot_check(0x04000880u, 0x7000u, 0u) == 0u && n48dm_slot_check(0x04000880u, 0x7800u, 0u) == 0u, "F5: disable on 0x7000 / 0x7800 stays allowed");
      expect(n48dm_slot_check(0x04000880u, 0x7000u, 1u) == N48DR_PAYLOAD_REFUSED && n48dm_slot_check(0x04000880u, 0x7400u, 0u) == N48DR_PAYLOAD_REFUSED, "F5: disable still needs d2 == 0 and a listed context");
      expect(n48dm_slot_check(0x04000780u, 0x6800u, 0u) == 0u && n48dm_slot_check(0x04000a80u, 0x6800u, 0u) == 0u, "F5: detect / enable on 0x6800 are unchanged (the rollback)");
      uint32_t h = 0, d1 = 0, d2 = 0; (void)n48dm_spec(N48DM_K_DISABLE, 0x6800, &h, &d1, &d2);
      expect(n48dm_slot_check(h, d1, d2) == N48DR_PAYLOAD_REFUSED, "F5: the CLI spec of `disable 0x6800` is refused by the same check the CLI runs");
      FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000880u, 0x6800u, 0u);
      expect(o.status == N48DR_PAYLOAD_REFUSED && nothing_touched(f) && f.probes == 0 && f.readCalls == 0 && o.sent == 0, "F5: the flow refuses disable 0x6800 before touching anything");
      FakeDmub g; fresh(g); g.latency = 1; Env e2{g}; expect(n48dm::send(e2, 0x04000880u, 0x7000u, 0u).status == N48DR_OK && g.wptrWrites.size() == 1, "F5: disable 0x7000 still sends"); }
    // F6 dmubmode (write ops) and dmubctx need an idle ring
    { for (int way = 0; way < 2; way++) {     // WPTR ahead of RPTR, RPTR ahead of WPTR
          FakeDmub f; fresh(f); Env e{f}; n48dm_mode_state s; std::memset(&s, 0, sizeof(s));
          expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_OK, "F6: save works on the idle ring");
          if (way == 0) f.wptr = 0x740; else f.rptr = 0x740;
          const int w0 = f.writeCalls;
          for (uint64_t op : { (uint64_t)N48DM_VBE_1080P, (uint64_t)N48DM_VBE_DP, (uint64_t)N48DM_MODE_RESTORE }) {
              const n48dm::ModeResult r = n48dm::mode(e, op, s);
              expect(r.status == N48DR_RING_NOT_IDLE && f.writeCalls == w0 && !s.dirty, "F6: a mode WRITE on a busy ring is refused with RING_NOT_IDLE, nothing written, not marked dirty"); }
          expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_OK, "F6: save is read-only and stays allowed on a busy ring");
          FakeDmub g; fresh(g); if (way == 0) g.wptr = 0x740; else g.rptr = 0x740; Env e2{g};
          const n48dm::CtxResult c = n48dm::ctx(e2, n48dm_ctx_arg(0x7000u, 0x01020006u));
          expect(c.status == N48DR_RING_NOT_IDLE && g.writeCalls == 0 && g.readCalls == 0 && g.wptrWrites.empty(), "F6: dmubctx on a busy ring: RING_NOT_IDLE, no VRAM access at all"); }
      // after a send that timed out the ring stays busy: every follow-up write is refused
      FakeDmub f; fresh(f); f.latency = -1; Env e{f}; n48dm_mode_state s; std::memset(&s, 0, sizeof(s));
      expect(n48dm::mode(e, N48DM_MODE_SAVE, s).status == N48DR_OK, "F6: save before the timeout");
      expect(n48dm::send(e, 0x04000a80u, 0x7000u, 0u).status == N48DR_POLL_TIMEOUT, "F6: a send that never completes");
      const int w0 = f.writeCalls;
      expect(n48dm::mode(e, N48DM_MODE_RESTORE, s).status == N48DR_RING_NOT_IDLE && n48dm::ctx(e, n48dm_ctx_arg(0x7000u, 0u)).status == N48DR_RING_NOT_IDLE && f.writeCalls == w0, "F6: after a timed-out send neither `dmubmode restore` nor `dmubctx` writes");
      // the idle gate is judged AFTER the DMCUB-state check
      FakeDmub g; fresh(g); g.enabled = 0; g.wptr = 0x740; Env e2{g}; expect(n48dm::ctx(e2, n48dm_ctx_arg(0x7000u, 0u)).status == N48DR_DMUB_NOT_READY, "F6: DMCUB state is judged before the idle gate");
      n48dm_pre idle = { 1, 0, 1, 1, 0x2000, 0x700, 0x700 }; expect(n48dm_idle_gate(&idle) == 0u, "F6: the gate passes WPTR == RPTR"); }
    // F7 the ring size must be the measured 0x2000
    { n48dm_pre ok = { 1, 0, 1, 1, 0x2000, 0x700, 0x700 };
      expect_u("F7: size 0x2000 is accepted", n48dm_precheck(&ok), 0);
      for (uint32_t sz : { 0x40u, 0x1000u, 0x1fc0u, 0x2040u, 0x3140u, 0x4000u, 0x8000u, 0x10000u }) {
          n48dm_pre p = ok; p.size = sz; if (sz < 0x740) { p.wptr = p.rptr = 0; } const uint32_t r = n48dm_precheck(&p);
          expect(r == N48DR_RING_SIZE_REFUSED, (std::string("F7: ring size ") + std::to_string(sz) + " is refused with RING_SIZE_REFUSED").c_str());
          FakeDmub f; fresh(f); f.size = sz; if (sz < 0x740) { f.wptr = f.rptr = 0; } Env e{f}; const n48dm_send_out o = n48dm::send(e, 0x04000a80u, 0x7000u, 0u);
          expect(o.status == N48DR_RING_SIZE_REFUSED && nothing_touched(f) && f.rptrReads == 0 && o.sent == 0, (std::string("F7: the flow refuses ring size ") + std::to_string(sz) + " before any write").c_str()); }
      expect(N48DM_RING_SIZE == 0x2000u && N48DM_RING_SIZE <= N48DM_VAR_STATUS, "F7: the only accepted ring ends below +0x3100, so no slot can reach the VBIOS variables");
      bool below = true; for (uint32_t w = 0; w + N48DM_SLOT_BYTES < N48DM_RING_SIZE; w += N48DM_SLOT_BYTES) { n48dm_pre p = ok; p.wptr = p.rptr = w; if (n48dm_precheck(&p) != 0u || w + N48DM_SLOT_BYTES > N48DM_VAR_STATUS) below = false; }
      expect(below, "F7: every WPTR the pre-check admits puts the 64-byte slot wholly below +0x3100"); }
    // F8 dmubsend replay <slot_off>: a byte-exact copy of an existing slot
    { uint32_t off = 0;
      expect(n48dm_replay_unarg(n48dm_replay_arg(0x100u), &off) == 0u && off == 0x100u, "F8: the replay argument round-trips (0x100)");
      expect(n48dm_replay_unarg(n48dm_replay_arg(0u), &off) == 0u && off == 0u && n48dm_replay_unarg(n48dm_replay_arg(0x1fc0u), &off) == 0u && off == 0x1fc0u, "F8: offsets 0 and 0x1fc0 are well formed");
      for (uint32_t bad : { 0x20u, 0x101u, 0x13fu, 0x2000u, 0x2040u, 0x8000u }) expect(n48dm_replay_unarg(n48dm_replay_arg(bad), &off) == N48DR_REPLAY_REFUSED && off == N48DM_NO_REPLAY, "F8: an unaligned or out-of-ring offset is refused");
      expect(n48dm_replay_unarg(n48dm_replay_arg(0x100u) | (1ull << 48), &off) == N48DR_REPLAY_REFUSED, "F8: bits 48..63 must be zero");
      expect(!n48dm_is_replay_arg(n48dm_send_arg(0x04000a80u, 0x7000u, 0u)) && !n48dm_hdr_ok(N48DM_REPLAY_TAG) && n48dm_slot_check(N48DM_REPLAY_TAG, 0u, 0u) == N48DR_HDR_REFUSED, "F8: the replay tag is not an allowlisted header, so no ordinary send can be mistaken for it");
      for (uint32_t k = 1; k <= 6; k++) { uint32_t h, a, b; (void)n48dm_spec(k, 0x7000, &h, &a, &b); expect(!n48dm_is_replay_arg(n48dm_send_arg(h, a, b)), "F8: no CLI command encodes to a replay"); } }
    { auto plant_src = [&](FakeDmub &f, uint32_t at, uint32_t h, uint32_t d1, uint32_t d2) { uint32_t s[16] = { h, d1, d2, 0xdeadbee3u, 0xdeadbee4u, 0x00007000u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xcafef00du }; for (int i = 0; i < 16; i++) f.mem[f.base + at + 4u * i] = s[i]; };
      std::vector<uint32_t> want = { 0x04000a80u, 0x7000u, 0u, 0xdeadbee3u, 0xdeadbee4u, 0x00007000u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xcafef00du };
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.latency = 3; Env e{f};
        const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u);
        expect(o.status == N48DR_OK && o.sent == 1 && o.vars == 1 && o.old_wptr == 0x700 && o.new_wptr == 0x740 && o.rptr_end == 0x740, "F8: replay 0x100: sent, WPTR 0x700 -> 0x740, completed");
        expect(f.writes.size() == 2 && f.writes[0].off == f.base + 0x700 && f.writes[0].n == 16 && f.writes[0].d == want, "F8: the new slot is the source's 64 bytes BYTE FOR BYTE (d3..d5 and the tail included), not zero-filled");
        expect(f.writes.size() == 2 && f.writes[1].off == f.base + 0x3114 && f.writes[1].d == std::vector<uint32_t>({ 0x40u, 0x700u, 0x740u }), "F8: the VBIOS variables follow as for any send");
        expect(f.wptrWrites.size() == 1 && f.wptrWrites[0] == 0x740, "F8: the WPTR register is written once");
        expect(o.hdr == 0x04000a80u && o.d1 == 0x7000u && o.d2 == 0u, "F8: the reported header / d1 / d2 are the source's");
        expect(f.mem[f.base + 0x100] == 0x04000a80u && f.mem[f.base + 0x13c] == 0xcafef00du, "F8: the source slot itself is not modified"); }
      for (uint32_t bad : { 0x700u, 0x740u, 0x6c1u, 0x1fc0u }) { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, bad);
          expect(o.status == N48DR_REPLAY_REFUSED && f.writeCalls == 0 && f.wptrWrites.empty() && o.sent == 0, "F8: a source at / above WPTR (or unaligned) is refused, nothing written"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x6c0, 0x04000a80u, 0x7000u, 0u); Env e{f}; expect(n48dm::send(e, 0, 0, 0, 0x6c0u).status == N48DR_OK, "F8: the slot just below WPTR (0x6c0) is a valid source"); }
      for (uint32_t bad : { 0x20u, 0x2000u, 0x4000u }) { FakeDmub f; fresh(f); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, bad);
          expect(o.status == N48DR_REPLAY_REFUSED && nothing_touched(f) && f.probes == 0 && f.readCalls == 0, "F8: a malformed offset touches nothing (not even a read)"); }
      struct S { const char *name; uint32_t h, d1, d2, want; };
      const S ss[] = { { "header off the allowlist (sub 9)", 0x04000980u, 0x7000u, 0u, N48DR_HDR_REFUSED }, { "header zero", 0u, 0u, 0u, N48DR_HDR_REFUSED }, { "detect with d1 outside the contexts", 0x04000a80u, 0x6000u, 0u, N48DR_PAYLOAD_REFUSED },
                       { "detect with d2 != 0", 0x04000a80u, 0x7000u, 1u, N48DR_PAYLOAD_REFUSED }, { "setmode with the wrong block", 0x08000580u, 0x4c20u, 0x7000u, N48DR_PAYLOAD_REFUSED },
                       { "disable on 0x6800 (F5 applies to a replay too)", 0x04000880u, 0x6800u, 0u, N48DR_PAYLOAD_REFUSED } };
      for (const S &s : ss) { FakeDmub f; fresh(f); plant_src(f, 0x100, s.h, s.d1, s.d2); Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u);
          expect(o.status == s.want && f.writeCalls == 0 && f.wptrWrites.empty() && o.sent == 0, (std::string("F8: a source with a ") + s.name + " is refused by the same allowlist, nothing written").c_str()); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.enabled = 0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u); expect(o.status == N48DR_DMUB_NOT_READY && f.readCalls == 0 && f.writeCalls == 0, "F8: DMCUB not enabled: refused before the source is read"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.rptr = 0x6c0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u); expect(o.status == N48DR_RING_BUSY && f.writeCalls == 0 && f.wptrWrites.empty(), "F8: a busy ring refuses a replay"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.size = 0x4000; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u); expect(o.status == N48DR_RING_SIZE_REFUSED && f.writeCalls == 0, "F8/F7: a replay on a ring of the wrong size is refused"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.corruptWriteNo = 0; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u);
        expect(o.status == N48DR_READBACK_MISMATCH && f.wptrWrites.empty() && o.sent == 0, "F8: a replayed slot that does not read back equal is refused, WPTR not moved"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); struct E3 : Env { FakeDmub &g; E3(FakeDmub &x) : Env{ x }, g(x) {} bool wr(uint64_t o, const uint32_t *s, uint32_t n) { bool r = g.wr(o, s, n); if (n == 16) g.mem[o + 60] ^= 0x80000000u; return r; } } e3(f);
        const n48dm_send_out o = n48dm::send(e3, 0, 0, 0, 0x100u); expect(o.status == N48DR_READBACK_MISMATCH && f.wptrWrites.empty(), "F8: a mismatch in the LAST dword (the tail the zero-fill used to hide) is caught"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.raceAfterWrites = 1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u);
        expect(o.status == N48DR_RING_BUSY && f.wptrWrites.empty(), "F8: WPTR changing under a replay is refused"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.latency = -1; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u);
        expect(o.status == N48DR_POLL_TIMEOUT && o.polls == 10000 && f.wptrWrites.size() == 1 && f.writes.size() == 1 && o.vars == 0, "F8: a replay that never completes: the 10000-poll bound, ONE WPTR write, nothing else written (no retry)"); }
      { FakeDmub f; fresh(f); plant_src(f, 0x100, 0x04000a80u, 0x7000u, 0u); f.failRead = true; Env e{f}; const n48dm_send_out o = n48dm::send(e, 0, 0, 0, 0x100u); expect(o.status == N48DR_VRAM_READ_FAILED && f.writeCalls == 0, "F8: an unreadable source: nothing written"); }
      { FakeDmub f; fresh(f); f.latency = 0; Env e{f}; (void)n48dm::send(e, 0x04000a80u, 0x7000u, 0u); expect(f.writes.size() == 2 && f.writes[0].d == slot_of(0x04000a80u, 0x7000u, 0u), "F8: the ordinary send still writes the zero-filled slot"); } }

    // =============================================================================================================================================================================================
    // S. source pins
    // =============================================================================================================================================================================================
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), hpp = slurp(src + "dcn/navi48_dcn.hpp"), brg = slurp(src + "Navi48Bringup.cpp"), pure = slurp(src + "amd/native_disp_pure.h"), ucl = slurp(src + "Navi48UserClient.cpp"),
                      flowS = slurp(src + "dcn/navi48_dmubcmd_flow.h"), hdrS = slurp(src + "dcn/navi48_dmubcmd.h"), drh = slurp(src + "dcn/navi48_dispread.h"), cli = slurp(g_root + "/tools/pc/navi48test.c"),
                      plist = slurp(g_root + "/src/navi48-bringup/Info.plist"), s1c = slurp(src + "amd/native_s1c.h"), mk = slurp(g_root + "/src/navi48-bringup/Makefile");
    expect(!dcn.empty() && !hpp.empty() && !brg.empty() && !pure.empty() && !flowS.empty() && !hdrS.empty() && !cli.empty() && !plist.empty(), "the sources are readable");
    const size_t g0 = dcn.find("build 0.0.625 (stages M2 / M3; an internal design note");
    expect(g0 != std::string::npos, "the command glue block is present");
    const size_t g1 = dcn.find("build 0.0.631 (multi-monitor stage M4d;", g0 == std::string::npos ? 0 : g0);   // 0.0.631: the disp2 glue follows; it has its own pins (tests/native_disp2_test.cpp)
    const std::string glue = g0 == std::string::npos ? "" : strip_line_comments(dcn.substr(g0, g1 == std::string::npos ? std::string::npos : g1 - g0));
    const std::string flowCode = strip_line_comments(flowS), hdrCode = strip_line_comments(hdrS);
    // 1. the OFF switch is the first test, and the latch is exactly the boot-arg
    { const size_t e0 = glue.find("uint32_t dm_enter(uint64_t *out, unsigned outCount, bool needBound) {");
      expect(e0 != std::string::npos, "dm_enter exists");
      const std::string en = e0 == std::string::npos ? "" : glue.substr(e0, 1100);
      const size_t z = en.find("out[i] = 0u;"), l = en.find("if (!dm_latched_on()) { out[0] = N48DR_CMD_OFF; return N48DR_CMD_OFF; }"), st = en.find("gLr.state != N48LR_READY"), bz = en.find("__atomic_exchange_n(&gDmBusy, 1u, __ATOMIC_ACQ_REL)");
      expect(z != std::string::npos && l != std::string::npos && st != std::string::npos && bz != std::string::npos && z < l && l < st && st < bz, "REACHABILITY: zero the output, then the OFF switch, THEN the device and the busy flag (the switch is the first real test)");
      for (const char *fn : { "uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount) {", "uint32_t dmubMode(uint64_t arg, uint64_t *out, unsigned outCount) {", "uint32_t dmubCtx(uint64_t arg, uint64_t *out, unsigned outCount) {" }) {
          expect(glue.find(std::string(fn) + "\n\tconst uint32_t early = dm_enter(out, outCount, ") != std::string::npos, (std::string("the first statement of ") + fn + " is dm_enter and nothing runs before it").c_str()); }
      expect(glue.find("dm_enter(out, outCount, true)") != std::string::npos && count_of(glue, "dm_enter(out, outCount, false)") == 2, "only dmubSend needs the bound allowlist layer");
      expect(dcn.find("PE_parse_boot_argn(\"navi48-dmubcmd\", &v, sizeof(v))") != std::string::npos && dcn.find("n48dm_latch_value(present ? 1 : 0, v)") != std::string::npos && dcn.find("volatile uint32_t gDmLatch = N48DM_LATCH_UNSET;") != std::string::npos, "the latch is boot-arg navi48-dmubcmd, default unset = OFF");
      expect(dcn.find("(void)dm_latched_on();                                   // 0.0.625: latch navi48-dmubcmd at start") != std::string::npos, "attach() (called from start()) latches it");
      expect(glue.find("__atomic_store_n(&gDmBusy, 0u, __ATOMIC_RELEASE)") != std::string::npos && count_of(glue, "__atomic_store_n(&gDmBusy, 0u, __ATOMIC_RELEASE)") == 3, "every verb releases the single-flight flag"); }
    // 2. the register write: ONE WREG32, behind the allowlist, tagged dmubsend
    expect_u("the new glue writes through exactly one WREG32", count_of(glue, "WREG32("), 1);
    { const size_t w = glue.find("amdgpu::WREG32(*gDcn.dev, a, v);"), a = glue.find("dcn41_allow_write(&gDcn.allow, a, v, \"dmubsend\")"); expect(w != std::string::npos && a != std::string::npos && a < w, "the WREG32 follows the allowlist test in the same function"); }
    expect(glue.find("N48DM_REG_INBOX1_WPTR, N48DM_REG_BASE_IDX);\n\t\tif (a == DCN41_BAD_OFFSET || !dcn41_allow_write") != std::string::npos, "the written register is DMCUB_INBOX1_WPTR");
    for (const char *bad : { "MM_INDEX", "WVRAM32_via_mm", "RVRAM32_via_mm", "WBAR0_", "bar0_memcpy_to_vram", "dcn41_dmub_gpint", "dcn41_dmub_ring_submit", "dcn41_dmub_ring_attach", "dcn41_dmub_build_", "dcn41_dmub_ring_wait", "dcn41_write(", "regWrite32", "navi48_reg_write32", "IODelay", "IOSleep" })
        expect(glue.find(bad) == std::string::npos, (std::string("the command glue never uses ") + bad).c_str());
    expect_u("the only VRAM writer is navi48_vram_write_mm (its declaration and ONE call site)", count_of(glue, "navi48_vram_write_mm("), 2);
    expect(glue.find("bool wr(uint64_t off, const uint32_t *src, uint32_t n) { return navi48_vram_write_mm(off, src, n); }") != std::string::npos && glue.find("bool rd(uint64_t off, uint32_t *dst, uint32_t n) { return navi48_vram_read_mm(off, dst, n); }") != std::string::npos, "the env's memory is the kext's existing locked MM readers / writer");
    expect(dcn.find("bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords);") != std::string::npos && brg.find("bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {") != std::string::npos, "the writer is the one defined in Navi48Bringup.cpp");
    expect(glue.find("void delay_us(uint32_t us) { dcn_udelay(nullptr, us); }") != std::string::npos, "the wait is the file's ONE delay glue");
    expect(glue.find("p->r4Enabled = (st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u ? 1u : 0u;") != std::string::npos && dcn.find("const uint32_t enabled = (st.region4_top & DCN41_DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK) != 0u ? 1u : 0u;") != std::string::npos &&
           flowCode.find("return n48dr_region4_base(p.r4Lo, p.r4Hi, p.r4Enabled, p.fbBaseMc, p.vramSize, base);") != std::string::npos, "the REGION4 window base is computed exactly as region4read does (same enable expression, same n48dr_region4_base)");
    expect(glue.find("dcn41_dmub_probe(&gLr.d, &st) != DCN41_OK") != std::string::npos && glue.find("p->pre.ring_region4 = st.ring_map == DCN41_DMUB_MAP_REGION4 ? 1u : 0u;") != std::string::npos && glue.find("st.ring_addr == (((uint64_t)st.region4_offset_high << 32) | st.region4_offset)") != std::string::npos, "the ring placement comes from the probe");
    // 3. the flow: the exact write sites and the order
    expect_u("the flow has exactly 4 VRAM write sites (slot, variables, mode block, marker)", count_of(flowCode, "e.wr("), 4);
    expect_u("the flow writes the WPTR register at exactly ONE site", count_of(flowCode, "e.wrWptr("), 1);
    { const size_t f0 = flowCode.find("inline n48dm_send_out send("), f1 = flowCode.find("struct ModeResult");
      expect(f0 != std::string::npos && f1 != std::string::npos, "send() is in the flow");
      const std::string sd = f0 == std::string::npos ? "" : flowCode.substr(f0, f1 - f0);
      const size_t a = sd.find("n48dm_slot_check(hdr, d1, d2)"), p = sd.find("e.probe(&p)"), pc = sd.find("n48dm_precheck(&p.pre)"), w = sd.find("e.wr(base + wptr0, slot, N48DM_SLOT_DWORDS)"), r = sd.find("e.rd(base + wptr0, back, N48DM_SLOT_DWORDS)"), c = sd.find("if (back[i] != slot[i])"),
                   rc = sd.find("if (e.wptr() != wptr0)"), fr = sd.find("o.frames0 = e.frames();"), b = sd.find("e.wrWptr(next)"), lp = sd.find("for (uint32_t n = 0; n < pollMax; n++) {"), dl = sd.find("e.delay_us(N48DM_POLL_STEP_US);"), tm = sd.find("if (!done) { o.status = N48DR_POLL_TIMEOUT; return o; }"), vr = sd.find("e.wr(base + N48DM_VAR_LASTSIZE, vars, 3u)");
      expect(a != std::string::npos && p != std::string::npos && pc != std::string::npos && w != std::string::npos && r != std::string::npos && c != std::string::npos && rc != std::string::npos && fr != std::string::npos && b != std::string::npos && lp != std::string::npos && dl != std::string::npos && tm != std::string::npos && vr != std::string::npos &&
             a < p && p < pc && pc < w && w < r && r < c && c < rc && rc < fr && fr < b && b < lp && lp < dl && dl < tm && tm < vr, "REACHABILITY/ORDER: allowlist, probe, pre-checks, slot write, read-back, COMPARE, re-check idle, frame witness, WPTR bump, bounded poll, timeout, variables");
      expect(lp != std::string::npos && tm != std::string::npos && sd.substr(lp, tm - lp).find("e.wrWptr(") == std::string::npos && sd.substr(lp, tm - lp).find("e.wr(") == std::string::npos, "NO RETRY: neither the WPTR nor any memory is written inside the poll loop or on the timeout path");
      expect(sd.find("const uint32_t vars[3] = { N48DM_VAR_CMDSIZE_VALUE, rptr0, next };") != std::string::npos, "the variables are { 0x40, the old RPTR, the new WPTR }");
      expect(sd.find("N48DM_VAR_STATUS") == std::string::npos, "+0x3100 is never named by the send flow"); }
    expect(hdrCode.find("if (p->wptr != p->rptr) return N48DR_RING_BUSY;") != std::string::npos && hdrCode.find("if (p->wptr + N48DM_SLOT_BYTES >= p->size) return N48DR_RING_WRAP;") != std::string::npos && hdrCode.find("if (!p->enabled || p->soft_reset) return N48DR_DMUB_NOT_READY;") != std::string::npos, "the pre-checks are in the pure header");
    // 4. the dispatch, the bound, the exemption
    { const size_t ex = brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)"), adm = brg.find("n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;"), br = brg.find("if (action == N48DM_ACT_SEND || action == N48DM_ACT_MODE || action == N48DM_ACT_CTX) {"), a88 = brg.find("	if (action == 88) {");
      expect(ex != std::string::npos && adm != std::string::npos && br != std::string::npos && a88 != std::string::npos && ex < adm && adm < br && br < a88, "REACHABILITY: the exemption, the latch admission, then the 95..97 branch (before action 88's)");
      std::string b = br == std::string::npos ? "" : brg.substr(br, 1400);
      { const size_t cut = b.find("	if (action == 88) {"); if (cut != std::string::npos) b = b.substr(0, cut); }
      expect(b.find("if (action == N48DM_ACT_SEND && n48dcn::dmubCmdLatchedOn()) (void)n48dcn::bind(this);") != std::string::npos && count_of(b, "bind(this)") == 1 && b.find("n48dcn::dmubSend(argScalar, v, 13)") != std::string::npos && b.find("n48dcn::dmubMode(argScalar, v, 13)") != std::string::npos && b.find("n48dcn::dmubCtx(argScalar, v, 13)") != std::string::npos, "the branch binds only for dmubsend and calls the three");
      expect(b.find("verb_args_ok") == std::string::npos, "the branch does not turn a refusal into BAD_ARG: the verbs judge their argument"); }
    expect(pure.find("constexpr uint32_t kLastAction = 99u;") != std::string::npos && pure.find("(action >= kActDmubSend && action <= kActDmubCtx) ||") != std::string::npos && pure.find("kActDmubSend = N48DM_ACT_SEND;") != std::string::npos, "the action bound is 99 (0.0.633: scdcread; 0.0.631: disp2) and 95..97 are exempt (latch ON)");
    expect(ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "the user client's bound goes through action_admitted (site 5)");
    expect(hpp.find("uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount);") != std::string::npos && hpp.find("uint32_t dmubMode(") != std::string::npos && hpp.find("uint32_t dmubCtx(") != std::string::npos, "navi48_dcn.hpp declares the three");
    // 5. the CLI
    for (const char *t : { "in = 95;", "in = 96;", "in = 97;", "!strcmp(what, \"dmubsend\")) in = 95;", "!strcmp(what, \"dmubmode\")) in = 96;", "!strcmp(what, \"dmubctx\")) in = 97;", "if (in == 95) return cmd_dmubsend(arg, arg2);", "if (in == 96) return cmd_dmubmode(arg);", "if (in == 97) return cmd_dmubctx(arg, arg2, arg3);",
                           "navi48_dmubcmd.h", "region4read region4dump dmubsend dmubmode dmubctx", "in == 97 ? \"dmubctx\" : in == 96 ? \"dmubmode\" : in == 95 ? \"dmubsend\"", "n48dm_spec(kind, ctx, &h, &d1, &d2)", "n48dm_slot_check(h, d1, d2)",
                           "static const char dm_scripts[]", "M2 - replay ONE command", "M3 - light the SINK-B", "M3 rollback", "accel dmubsend detect 0x7000", "accel dmubctx 0x7000 status 0", "accel dmubctx 0x7000 status 0x01020006",
                           "accel dmubmode save", "accel dmubmode 0x1a6", "accel dmubsend begin", "accel dmubsend setmode 0x7000", "accel region4read 0x4c08 1", "accel dmubsend enable 0x7000", "accel dmubsend end", "accel dmubmode restore", "accel dmubsend setmode 0x6800 ; accel dmubsend enable 0x6800",
                           "dmubsend detect <ctx>/begin/end/setmode <ctx>/enable <ctx>/disable <0x7000|0x7800>/replay <slot_off>/pclk otg3-on|otg3-off|otg1-on|otg1-off/phyc enable|disable/digc setup|dmubmode save/restore/0x1a6/0x1d4|dmubctx <ctx> status <v>" })
        expect(cli.find(t) != std::string::npos, (std::string("the CLI carries ") + t).c_str());
    expect(cli.find("static int cmd_dmubctx(const char *a1, const char *a2, const char *a3) {") != std::string::npos && cli.find("strcmp(a2, \"status\") != 0") != std::string::npos, "dmubctx takes `<ctx> status <value>`");
    // 6. the version, the project rules
    expect(count_of(plist, "0.0.664") == 2 && plist.find("0.0.625") == std::string::npos, "Info.plist is 0.0.664, carried twice");
    expect(s1c.find("kN1cKextBuild = 664;") != std::string::npos, "the native client reports build 660");
    expect(drh.find("N48DR_STATUS_COUNT = 39") != std::string::npos, "the status enum ends at 39 (0.0.652: N48DR_HELD_REFUSED = 38)");
    expect(mk.find("$(wildcard src/dcn/*.cpp)") != std::string::npos, "the Makefile builds src/dcn/*.cpp");
    const std::vector<std::string> bannedStrings = { std::string("pipe") + "+0x" + "280", std::string("+0x") + "282", std::string("+0x") + "299" };   // spelled in pieces so this file itself never carries them
    for (const std::string *f : { &hdrS, &flowS }) for (const std::string &ban : bannedStrings) expect(f->find(ban) == std::string::npos, (std::string("banned string absent: ") + ban).c_str());
    for (const std::string &ban : bannedStrings) expect(glue.find(ban) == std::string::npos, (std::string("banned string absent from the glue: ") + ban).c_str());
    // 0.0.626 source pins (F4 latch before bind, F5 CLI text, F8 glue + CLI replay, F6/F7 in the flow / header)
    { const size_t br = brg.find("if (action == N48DM_ACT_SEND || action == N48DM_ACT_MODE || action == N48DM_ACT_CTX) {");
      const std::string b = br == std::string::npos ? "" : brg.substr(br, 900);
      const size_t la = b.find("n48dcn::dmubCmdLatchedOn()"), bi = b.find("n48dcn::bind(this)");
      expect(la != std::string::npos && bi != std::string::npos && la < bi && b.find("if (action == N48DM_ACT_SEND && n48dcn::dmubCmdLatchedOn()) (void)n48dcn::bind(this);") != std::string::npos, "F4: the dispatcher asks the navi48-dmubcmd latch BEFORE bind() (OFF binds nothing)");
      expect(glue.find("bool dmubCmdLatchedOn() { return dm_latched_on(); }") != std::string::npos && hpp.find("bool dmubCmdLatchedOn();") != std::string::npos, "F4: the latch is exported from the glue and declared"); }
    expect(glue.find("n48dm_is_replay_arg(arg)") != std::string::npos && glue.find("n48dm_replay_unarg(arg, &replaySrc)") != std::string::npos && glue.find("n48dm::send(env, 0u, 0u, 0u, replaySrc)") != std::string::npos, "F8: the glue decodes the replay argument and calls send with the source offset");
    expect(cli.find("accel dmubsend replay 0x100") != std::string::npos && cli.find("n48dm_replay_arg(srcOff)") != std::string::npos && cli.find("!strcmp(a1, \"replay\")") != std::string::npos, "F8: the CLI has `dmubsend replay <slot_off>`");
    expect(cli.find("allowed ONLY on 0x7000 / 0x7800, never 0x6800") != std::string::npos && cli.find("never: sub 8") == std::string::npos && cli.find("disable <ctx>") == std::string::npos, "F5: the CLI text says disable is for 0x7000 / 0x7800 only (usage, scripts and help agree)");
    expect(hdrS.find("case 0x08u:") != std::string::npos && hdrS.find("return (n48dm_ctx_ok(d1) && d2 == 0u) ? 0u") != std::string::npos, "F5: sub 8 has its own case using the HDMI-only context test");
    expect(hdrS.find("if (p->size != N48DM_RING_SIZE) return N48DR_RING_SIZE_REFUSED;") != std::string::npos && hdrS.find("#define N48DM_RING_SIZE    0x2000u") != std::string::npos, "F7: the pre-check requires the measured ring size");
    expect(count_of(flowS, "n48dm_idle_gate(&p.pre)") == 2, "F6: dmubmode and dmubctx both call the idle gate");
    // 0.0.630 source pins (M4c): the glue, the CLI, the flow
    expect(glue.find("} else if (n48dm_is_tpl_arg(arg)) {") != std::string::npos && glue.find("n48dm_tpl_unarg(arg, &tplId)") != std::string::npos && glue.find("n48dm::send(env, 0u, 0u, 0u, N48DM_NO_REPLAY, tplId)") != std::string::npos, "M4c: the glue decodes the template argument and calls send with the ID (no raw dwords from userland)");
    expect(cli.find("n48dm_tpl_arg(id)") != std::string::npos && cli.find("!strcmp(a1, \"pclk\")") != std::string::npos && cli.find("!strcmp(a1, \"phyc\")") != std::string::npos && cli.find("\"otg3-on\"") != std::string::npos && cli.find("\"otg1-off\"") != std::string::npos && cli.find("\"enable\" ) ? N48DM_TPL_PHYC_ENABLE_DVI") == std::string::npos, "M4c: the CLI has `dmubsend pclk otg3-on|otg3-off|otg1-on|otg1-off` and `phyc enable|disable`, sent by template ID");
    expect(cli.find("id = !strcmp(a2, \"otg3-on\") ? N48DM_TPL_PCLK_OTG3_ON : !strcmp(a2, \"otg3-off\") ? N48DM_TPL_PCLK_OTG3_OFF : !strcmp(a2, \"otg1-on\") ? N48DM_TPL_PCLK_OTG1_ON : !strcmp(a2, \"otg1-off\") ? N48DM_TPL_PCLK_OTG1_OFF :") != std::string::npos &&
           cli.find("else id = !strcmp(a2, \"enable\") ? N48DM_TPL_PHYC_ENABLE_DVI : !strcmp(a2, \"disable\") ? N48DM_TPL_PHYC_DISABLE : N48DM_TPL_NONE;") != std::string::npos, "M4c: the CLI maps each word to its own template ID");
    expect(flowS.find("const uint32_t pollMax = n48dm_poll_max_for(slot[0]);") != std::string::npos && flowS.find("n48dm_tpl_slot_check(slot)") != std::string::npos, "M4c: the flow takes the poll bound from the header it writes and checks the exact slot of a template");
    std::printf("native_dmubcmd: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
