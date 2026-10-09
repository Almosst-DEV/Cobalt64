// native_sdmaorder_test.cpp - build 0.0.657 (GitHub issue #1, public fix 1a04d8e): the SDMA0_DCC_CNTL no-PTE compression clear lands BEFORE the ladder's own SDMA copies.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       src/navi48-bringup/tests/native_sdmaorder_test.cpp -o /tmp/native_sdmaorder && /tmp/native_sdmaorder .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_sdmaorder_plant.sh plants breaks)
// Covers (source pins; the kext cannot run on the host, so ORDER and REACHABILITY are pinned on the text of the SDMAInit case and of navi48_sdmadcc_default):
//   O1  the SDMAInit case calls navi48_sdmadcc_default() exactly once, AFTER sdma_init_full's failure return and BEFORE both sdma_vram_copy_test and sdma_vram_copy_sweep
//       (and each of those calls appears exactly once in the case); amdgpu_init.cpp declares the function it calls;
//   O2  navi48_sdmadcc_default: the already-done check (the "already handled earlier this boot" line + return, no write) comes BEFORE the boot-arg read and every register access;
//       gSdmaDccDefaultDone is set on the opt-out path and immediately after the single navi48_reg_write32, and ONLY there (two assignments; the 'no DeviceContext' and
//       'GC base did not resolve' skips leave it clear, so a skipped early call is retried by the runStages call); one write in the function; navi48-sdmadcc=0 still opts out;
//   O3  Navi48Bringup::runStages still calls navi48_sdmadcc_default() once, under ctx.reached >= SDMAInit.
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static std::string slurp(const std::string &path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }
static const size_t NP = std::string::npos;

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string K = root + "/src/navi48-bringup/src/";
    const std::string init = slurp(K + "amd/amdgpu_init.cpp"), brg = slurp(K + "Navi48Bringup.cpp");
    expect(!init.empty() && !brg.empty(), "the sources are readable");

    // ---- O1: the SDMAInit case ----
    const size_t cs = init.find("case BringupStage::SDMAInit: {");
    const size_t ce = cs == NP ? NP : init.find("\n    case BringupStage::", cs + 10);
    expect(cs != NP && ce != NP && ce > cs, "the SDMAInit case is found");
    const std::string c = (cs != NP && ce != NP) ? init.substr(cs, ce - cs) : std::string();
    const size_t fi = c.find("sdma_init_full(dev, ctx.psp, ctx.gmc, ctx.sdma);");
    const size_t fr = fi == NP ? NP : c.find("if (r != kIOReturnSuccess) return r;", fi);
    const size_t dc = c.find("navi48_sdmadcc_default();");
    const size_t ct = c.find("sdma_vram_copy_test(");
    const size_t sw = c.find("sdma_vram_copy_sweep(");
    expect(count_of(c, "navi48_sdmadcc_default();") == 1, "O1: the SDMAInit case calls navi48_sdmadcc_default() exactly once");
    expect(count_of(c, "sdma_init_full(") == 1 && count_of(c, "sdma_vram_copy_test(") == 1 && count_of(c, "sdma_vram_copy_sweep(") == 1, "O1: sdma_init_full, the copy test and the sweep each appear once in the case");
    expect(fi != NP && fr != NP && dc != NP && ct != NP && sw != NP, "O1: all five anchors are present");
    expect(fi != NP && fr != NP && dc != NP && fr < dc, "O1: the call comes AFTER sdma_init_full and its failure return (never on a dead SDMA)");
    expect(dc != NP && ct != NP && dc < ct, "O1: the call comes BEFORE sdma_vram_copy_test");
    expect(dc != NP && sw != NP && dc < sw, "O1: the call comes BEFORE sdma_vram_copy_sweep");
    expect(init.find("void navi48_sdmadcc_default(void);") != NP && init.find("void navi48_sdmadcc_default(void);") < cs, "O1: amdgpu_init.cpp declares navi48_sdmadcc_default before the stage code");

    // ---- O2: navi48_sdmadcc_default ----
    const size_t fs = brg.find("void navi48_sdmadcc_default(void) {");
    const size_t fe = fs == NP ? NP : brg.find("\n}\n", fs);
    expect(fs != NP && fe != NP, "O2: navi48_sdmadcc_default is found");
    const std::string f = (fs != NP && fe != NP) ? brg.substr(fs, fe - fs + 3) : std::string();
    expect(brg.find("static bool gSdmaDccDefaultDone { false };") != NP && brg.find("static bool gSdmaDccDefaultDone { false };") < fs, "O2: gSdmaDccDefaultDone is a file-scope static bool, false at load");
    const size_t ck = f.find("if (gSdmaDccDefaultDone) {");
    const size_t lg = f.find("sdmadcc: DEFAULT already handled earlier this boot");
    const size_t rt = lg == NP ? NP : f.find("return;", lg);
    const size_t pe = f.find("PE_parse_boot_argn(\"navi48-sdmadcc\"");
    const size_t wr = f.find("navi48_reg_write32(reg0, target);");
    expect(ck != NP && lg != NP && rt != NP && pe != NP && wr != NP && ck < lg && lg < rt && rt < pe && pe < wr, "O2: the already-done check logs and returns BEFORE the boot-arg read and the register write");
    expect(f.find("- no second write") != NP, "O2: the already-done line says no second write");
    expect(count_of(f, "navi48_reg_write32(") == 1 && count_of(f, "navi48_reg_read32(") == 2, "O2: one write; the boot capture and the readback are the only reads");
    expect(count_of(f, "gSdmaDccDefaultDone = true;") == 2, "O2: the flag is assigned exactly twice (opt-out path, right after the write)");
    const size_t oo = f.find("if (!n48_sdma_dcc_default_on(present, ba)) {");
    const size_t os = oo == NP ? NP : f.find("gSdmaDccDefaultDone = true;", oo);
    const size_t ol = f.find("sdmadcc: DEFAULT SKIPPED - navi48-sdmadcc=0");
    expect(oo != NP && os != NP && ol != NP && oo < os && os < ol, "O2: the opt-out path sets the flag, then logs the navi48-sdmadcc=0 skip");
    const size_t nd = f.find("sdmadcc: DEFAULT SKIPPED - no DeviceContext"), ng = f.find("sdmadcc: DEFAULT SKIPPED - GC BASE_IDX 0 did not resolve");
    const size_t f1 = f.find("gSdmaDccDefaultDone = true;"), f2 = f1 == NP ? NP : f.find("gSdmaDccDefaultDone = true;", f1 + 10);
    expect(nd != NP && ng != NP && f1 != NP && f2 != NP && f2 > ng && f2 > nd && f2 > wr, "O2: the 'no DeviceContext' and 'GC base did not resolve' skips come before the second assignment (they never set the flag)");
    expect(wr != NP && f2 != NP && f.find("navi48_reg_read32(reg0);", wr) != NP && f2 < f.find("navi48_reg_read32(reg0);", wr), "O2: the second assignment is immediately after the write, before the readback");
    expect(f.find("n48_sdma_dcc_default_on(present, ba)") != NP && f.find("gSdmaDccRestore") != NP && f.find("gSdmaDccCaptured = true;") != NP, "O2: the opt-out predicate and the restore pair are the same as before");

    // ---- O3: the runStages call ----
    expect(count_of(brg, "navi48_sdmadcc_default();") == 1, "O3: Navi48Bringup.cpp calls navi48_sdmadcc_default() once (runStages)");
    const size_t rs = brg.find("if (ctx.reached >= amdgpu::BringupStage::SDMAInit) {");
    const size_t rc = brg.find("navi48_sdmadcc_default();");
    expect(rs != NP && rc != NP && rs < rc && rc - rs < 1500, "O3: that call stays under ctx.reached >= SDMAInit");

    std::printf("native_sdmaorder: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
