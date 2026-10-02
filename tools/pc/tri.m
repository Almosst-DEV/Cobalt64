// tri.m — the milestone-2 render test client (notes/MILESTONE2-DESIGN.md Q4).
//
// Like blit2.m: commit, then STAY ALIVE and POLL (rule 21: never waitUntilCompleted; one Metal
// probe per boot; state in ~/navi48-staging). It renders into a small offscreen texture and reads
// the pixels back with an exact verdict on ONE `TRI:` line.
//
//   tri clear                 a render pass with a clear load action and NO draw (the draw-less
//                             context experiment): every pixel must be the clear colour.
//   tri tri                   one constant-colour triangle over a known clear colour: the centre
//                             pixel must be the triangle colour and the corners the clear colour.
//   tri draw                  0.0.212 (the milestone-2 draw): MTLLoadActionDontCare
//                             (no UBM clear stream, review report 7), the target PREFILLED ON THE CPU
//                             with blue, and one draw of the m2tri pair (tools/b1-tests/m2tri/m2tri.metal:
//                             positions from vertex buffer 0, float2, stride 8; colour (1, 0.25, 0, 1)).
//                             Verdict: centre = the triangle colour, corners = the prefill, nothing else.
//   tri <mode> --inject DIR   hand Apple our packed gfx1201 binaries instead of what libSC compiled,
//                             by swizzling -[GFX10_GfxMtlFunctionVariant initWithCompilerOutput:...]
//                             (the g2capture mechanism, design route U). DIR/manifest.json names the
//                             vertex and fragment packed-binary files (raw GFX10_PackedBinaryRec, as
//                             g2capture dumps *_compileroutput.bin, as tools/gfx-pack.py emits).
//   tri draw --ring           0.0.217 (review report 9): also allocate the gfx12 NGG rings (attribute,
//                             position, primitive) as ONE Shared buffer of XLAT12_GE_RING_TOTAL + 64 KiB, zero-filled,
//                             declared on the render encoder with useResource Read|Write so Apple maps it in VMID 2 with
//                             the submission. Prints `[tri] RING buffer gpuAddress ... base 0x...` (base = gpuAddress
//                             aligned UP to 64 KiB) BEFORE commit, for `accel renderxlat <base|0x200|mode>`; after the
//                             TRI: line, `TRI-RING:` nonzero-byte counts per ring from this process's own mapping and
//                             tri-ring-{attr,pos,prim}.bin (the first 64 KiB of each ring).
//   tri <mode> --dump DIR     0.0.212 (review report 7, recommendation 1): build the encoder exactly as
//                             <mode> would, `endEncoding`, and NEVER commit. After each encoding phase
//                             it scans this process's own writable memory in place for PM4 streams
//                             (the kext renderib's criteria, xlat12_ib's walk and census) and writes
//                             each one to DIR (raw .bin, pg[] hex .txt, a 3-page context .bin) with a
//                             `[tri] DUMP` line per stream and a region census whose counts add up.
//                             No command buffer is committed, so the GPU does no work.
//
// x86_64, ad-hoc signed, NOT hardened / NOT library-validated (so it may swizzle). Cross-built on
// the host Mac by tools/stage-to-pc.sh (with src/xlat12/xlat12.c + xlat12_ib.c linked in) and staged; it
// cannot run on the arm64 host Mac (bad CPU type).
//
// Readback path (0.0.209, an earlier analysis): NO GPU kernel. The render target is a texture on a
// Shared MTLBuffer (host pages the GPU writes directly; section 337), prefilled on the CPU, read
// directly. The storage used is printed on the TRI: line. Failure signatures (design Q4):
//   a wrong VS (bad position)      -> the triangle misses its pixels: centre stays the prefill/clear.
//   a wrong PS (bad colour/export) -> the covered pixels are flat/wrong, not the triangle colour.
//   a wrong render-state xlat      -> clears wrong, or writes nowhere (the prefill survives).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dispatch/dispatch.h>
#import <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>     // 0.0.235: gettimeofday, for --nowait's handler timing
#include "xlat12_ib.h"

static const char *kStateDir = "/tmp/navi48-staging";
enum { W = 64, H = 64 };

// Colours in BGRA8Unorm memory order {B,G,R,A}. 0.0->0, 1.0->255 exactly, so compares are exact.
static const uint32_t kClearBGRA = 0xFF0000FFu;   // blue  (R0 G0 B255 A255)
static const uint32_t kTriBGRA   = 0xFFFF0000u;   // red   (R255 G0 B0 A255)
// draw mode: m2_tri_fs returns (1, 0.25, 0, 1) -> R 0xff, G round(63.75) = 0x40, B 0, A 0xff.
// The green channel is accepted at 0x3f..0x41 (unorm rounding); everything else must be exact.
static int is_draw_colour(uint32_t v) {
    uint32_t b = v & 0xff, g = (v >> 8) & 0xff, r = (v >> 16) & 0xff, a = v >> 24;
    return b == 0 && r == 0xff && a == 0xff && g >= 0x3f && g <= 0x41;
}

// ---- optional shader injection (design route U) ---------------------------
// Unslid variant-init offset for build 25G83 (g2capture / MILESTONE2-DESIGN §Q2); the live IMP is
// checked against dli_fbase + this, logged, and used even on a mismatch (a warning, not a refusal).
static uintptr_t       gExpectInitOff  = 0x11191a7a - 0x110d2000;
static SEL             gSel;
typedef id (*init_imp_t)(id, SEL, id, unsigned int, id, id);
static init_imp_t gOrigInit;
static NSData *gVertexPB, *gFragmentPB;   // our packed binaries, or nil
static int gInjected;

static id hook_init(id self, SEL _cmd, id compilerOutput, unsigned int shaderType, id device, id pstats) {
    NSData *sub = (shaderType == 1) ? gVertexPB : (shaderType == 0) ? gFragmentPB : nil;
    if (sub) {
        dispatch_data_t dd = dispatch_data_create(sub.bytes, sub.length,
                                dispatch_get_global_queue(0, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        fprintf(stderr, "[tri] INJECT: %s stage gets our %lu-byte packed binary\n",
                shaderType == 1 ? "vertex" : "fragment", (unsigned long)sub.length);
        gInjected++;
        id r = gOrigInit(self, _cmd, (id)dd, shaderType, device, pstats);
        fprintf(stderr, "[tri] INJECT: %s variant init returned %p\n",
                shaderType == 1 ? "vertex" : "fragment", (__bridge void *)r);
        return r;
    }
    return gOrigInit(self, _cmd, compilerOutput, shaderType, device, pstats);
}

static int install_swizzle(void);
static int install_inject(NSString *dir) {
    NSString *mpath = [dir stringByAppendingPathComponent:@"manifest.json"];
    NSData *mj = [NSData dataWithContentsOfFile:mpath];
    if (!mj) { fprintf(stderr, "[tri] --inject: no manifest.json in %s\n", dir.UTF8String); return 1; }
    NSError *e = nil;
    NSDictionary *m = [NSJSONSerialization JSONObjectWithData:mj options:0 error:&e];
    if (![m isKindOfClass:NSDictionary.class]) {
        fprintf(stderr, "[tri] --inject: manifest.json not an object: %s\n", e.localizedDescription.UTF8String);
        return 1;
    }
    NSString *vf = m[@"vertex"], *ff = m[@"fragment"];
    if (vf) gVertexPB   = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:vf]];
    if (ff) gFragmentPB = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:ff]];
    fprintf(stderr, "[tri] --inject: vertex=%s (%lu B) fragment=%s (%lu B)\n",
            vf.UTF8String ?: "-", (unsigned long)gVertexPB.length,
            ff.UTF8String ?: "-", (unsigned long)gFragmentPB.length);
    if (!gVertexPB && !gFragmentPB) { fprintf(stderr, "[tri] --inject: neither file loaded\n"); return 1; }
    return install_swizzle();
}

// The swizzle on -[GFX10_GfxMtlFunctionVariant initWithCompilerOutput:...]: while gVertexPB / gFragmentPB are set, the
// matching stage gets our packed binary. Installed once per process (suite mode sets the pair per pipeline).
static int gSwizzleInstalled;
static int install_swizzle(void) {
    if (gSwizzleInstalled) return 0;
    Class cls = objc_getClass("GFX10_GfxMtlFunctionVariant");
    if (!cls) { fprintf(stderr, "[tri] --inject: GFX10_GfxMtlFunctionVariant not found\n"); return 1; }
    Method mth = class_getInstanceMethod(cls, gSel);
    if (!mth) { fprintf(stderr, "[tri] --inject: init selector not found\n"); return 1; }
    IMP orig = method_getImplementation(mth);
    Dl_info di; memset(&di, 0, sizeof di);
    if (dladdr((const void *)orig, &di) && di.dli_fbase) {
        uintptr_t expect = (uintptr_t)di.dli_fbase + gExpectInitOff;
        const char *leaf = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        fprintf(stderr, "[tri] --inject: IMP=%p image=%s expect=%p %s\n", (void *)orig, leaf,
                (void *)expect, (uintptr_t)orig == expect ? "ok" : "MISMATCH (continuing)");
    }
    gOrigInit = (init_imp_t)orig;
    method_setImplementation(mth, (IMP)hook_init);
    gSwizzleInstalled = 1;
    fprintf(stderr, "[tri] --inject: swizzle installed on GFX10_GfxMtlFunctionVariant\n");
    return 0;
}

// ---- --dump: the userspace PM4 census (no commit) ----------------------------------------------
// Scans every readable+writable, non-submap region of this process IN PLACE (no copies, so a found
// stream is never found again in a scan buffer), with a SIGSEGV/SIGBUS guard for pages that cannot
// be read. A stream = the kext renderib's rule (AppleHardwareHook.cpp render_plausible_header, then
// xlat12_ib_walk >= 16 dwords, >= 4 packets, holding a SET, draw, dispatch, CONTEXT_CONTROL or
// memory-loaded packet). Candidates are recorded during the scan and written only after it.
static pthread_t   gScanThread;
static sigjmp_buf *gGuard;
static void guard_handler(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    if (gGuard && pthread_equal(pthread_self(), gScanThread)) siglongjmp(*gGuard, 1);
    signal(sig, SIG_DFL);   // not ours: re-execute the faulting instruction with the default action
}
static void guard_install(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = guard_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    gScanThread = pthread_self();
}
static int page_readable(const uint8_t *p) {
    sigjmp_buf jb; volatile int ok = 0;
    gGuard = &jb;
    if (sigsetjmp(jb, 1) == 0) { volatile uint8_t x = p[0]; (void)x; ok = 1; }
    gGuard = NULL;
    return ok;
}

// The kext's render_plausible_header (AppleHardwareHook.cpp, 0.0.211), kept byte-for-byte in meaning.
static int plausible_header(uint32_t h) {
    if ((h >> 30) != 3u) return 0;
    const uint32_t cnt = (h >> 16) & 0x3FFFu, op = (h >> 8) & 0xFFu;
    switch (op) {
    case 0x28: return cnt == 1u;
    case 0x69: case 0x76: case 0x79: return cnt >= 1u && cnt <= 0x100u;
    case 0x9B: case 0x7A: return cnt >= 1u && cnt <= 0x40u;
    case 0x12: return cnt <= 1u;
    case 0x5E: case 0x5F: case 0x61: case 0x63: case 0x9F: return cnt >= 2u && cnt <= 0x100u;
    case 0x22: return cnt == 3u;
    case 0x2D: case 0x27: case 0x35: return cnt >= 1u && cnt <= 8u;
    case 0x15: return cnt == 3u;
    case 0x37: return cnt >= 2u && cnt <= 0x100u;
    case 0x49: case 0x58: return cnt == 6u;
    default:   return 0;
    }
}

typedef struct {
    uint64_t addr, rstart, rend;
    uint32_t walk, tag, prot, hash;
    xlat12_ib_census c;
} DumpCand;
enum { kDumpMaxCands = 64 };
static DumpCand gCands[kDumpMaxCands];
static uint32_t gNCands;
static const char *gDumpDir;
static uint64_t gSelfLo, gSelfHi;   // our candidate table's own pages (never scanned)

static uint32_t fnv32(const uint32_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// Scan [s, e) in place (page-aligned, every page readable a moment ago). Returns dwords scanned.
static uint64_t scan_range(uint64_t s, uint64_t e, uint64_t rs, uint64_t re, uint32_t tag, uint32_t prot,
                           uint32_t *streams, uint32_t *faults) {
    sigjmp_buf jb; volatile uint64_t d = s;
    gGuard = &jb;
    if (sigsetjmp(jb, 1) != 0) {   // a page vanished under us: stop this range, count it
        gGuard = NULL; (*faults)++;
        return (d - s) / 4;
    }
    while (d + 4 <= e) {
        const uint32_t *p = (const uint32_t *)(uintptr_t)d;
        if (!plausible_header(p[0])) { d += 4; continue; }
        const uint32_t n = (uint32_t)(((e - d) / 4) > 0x100000u ? 0x100000u : (e - d) / 4);
        const uint32_t w = xlat12_ib_walk(p, n);
        xlat12_ib_census c;
        xlat12_ib_census_run(p, w, &c);
        const uint32_t sig = c.set_ctx + c.set_sh_gfx + c.set_sh_cs + c.set_ucfg + c.set_index + c.draws +
                             c.dispatches + c.ctxctl_proven + c.ctxctl_other + c.clear_state + c.load_reg;
        if (c.packets < 4u || w < 16u || sig == 0u) { d += 4; continue; }
        if (gNCands < kDumpMaxCands) {
            DumpCand *k = &gCands[gNCands];
            k->addr = d; k->rstart = rs; k->rend = re; k->walk = w; k->tag = tag; k->prot = prot;
            k->hash = fnv32(p, w); k->c = c;
        }
        gNCands++; (*streams)++;
        d += (uint64_t)w * 4u;
    }
    gGuard = NULL;
    return (d - s) / 4;
}

static void write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) { ssize_t k = write(fd, p, len); if (k <= 0) break; p += k; len -= (size_t)k; }
}

static void dump_phase(const char *phase) {
    gNCands = 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/regions_%s.txt", gDumpDir, phase);
    int rfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint32_t nReg = 0, nScan = 0, nSub = 0, nNoRead = 0, nNoWrite = 0, nLarge = 0, nSelf = 0;
    uint64_t pages = 0, unreadable = 0, nonres = 0, dwords = 0;
    uint32_t streams = 0, faults = 0;
    mach_vm_address_t a = 0;
    for (;;) {
        mach_vm_size_t sz = 0; natural_t depth = 0;
        vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
        nReg++;
        const char *why = NULL;
        if (info.is_submap) { nSub++; why = "submap"; }
        else if (!(info.protection & VM_PROT_READ)) { nNoRead++; why = "no-read"; }
        else if (!(info.protection & VM_PROT_WRITE)) { nNoWrite++; why = "read-only"; }
        else if (a < gSelfHi && a + sz > gSelfLo) { nSelf++; why = "self (candidate table)"; }
        else if (sz > (64ull << 30)) { nLarge++; why = "larger than 64 GiB"; }
        uint32_t st0 = streams; uint64_t pr0 = pages, un0 = unreadable, nr0 = nonres;
        if (!why) {
            nScan++;
            const uint64_t pg = (uint64_t)vm_page_size;
            const int big = sz > (256ull << 20);   // big regions: resident pages only (mincore)
            uint64_t runS = 0; int inRun = 0;
            for (uint64_t off = 0; off <= sz; off += pg) {
                int ok = 0;
                if (off < sz) {
                    const uint64_t va = a + off;
                    int resident = 1;
                    if (big) { char v = 0; resident = (mincore((void *)(uintptr_t)va, pg, &v) == 0) && (v & MINCORE_INCORE); }
                    if (!resident) nonres++;
                    else { pages++; ok = page_readable((const uint8_t *)(uintptr_t)va); if (!ok) unreadable++; }
                }
                if (ok && !inRun) { runS = a + off; inRun = 1; }
                if (!ok && inRun) {
                    dwords += scan_range(runS, a + off, a, a + sz, info.user_tag, info.protection, &streams, &faults);
                    inRun = 0;
                }
            }
        }
        if (rfd >= 0) {
            char line[512];
            int n = snprintf(line, sizeof line, "%016llx-%016llx %10llu KiB prot %c%c%c max %c%c%c tag %3u share %u depth %u %s"
                             " pages %llu unreadable %llu nonresident %llu streams %u\n",
                             (unsigned long long)a, (unsigned long long)(a + sz), (unsigned long long)(sz >> 10),
                             (info.protection & 1) ? 'r' : '-', (info.protection & 2) ? 'w' : '-', (info.protection & 4) ? 'x' : '-',
                             (info.max_protection & 1) ? 'r' : '-', (info.max_protection & 2) ? 'w' : '-', (info.max_protection & 4) ? 'x' : '-',
                             info.user_tag, info.share_mode, depth, why ? why : "SCANNED",
                             (unsigned long long)(pages - pr0), (unsigned long long)(unreadable - un0),
                             (unsigned long long)(nonres - nr0), streams - st0);
            write_all(rfd, line, (size_t)n);
        }
        a += sz;
    }
    if (rfd >= 0) close(rfd);
    fprintf(stderr, "[tri] DUMP-REGIONS %s: %u region(s) = scanned %u + submap %u + no-read %u + read-only %u + self %u + "
            "over-64GiB %u (sum %u); pages read-probed %llu (unreadable %llu), non-resident skipped %llu; dwords scanned %llu; "
            "range faults %u; PM4 streams %u (recorded %u)\n", phase, nReg, nScan, nSub, nNoRead, nNoWrite, nSelf, nLarge,
            nScan + nSub + nNoRead + nNoWrite + nSelf + nLarge, (unsigned long long)pages, (unsigned long long)unreadable,
            (unsigned long long)nonres, (unsigned long long)dwords, faults, streams,
            streams < kDumpMaxCands ? streams : kDumpMaxCands);

    const uint32_t nw = gNCands < kDumpMaxCands ? gNCands : kDumpMaxCands;
    for (uint32_t i = 0; i < nw; i++) {
        const DumpCand *k = &gCands[i];
        const xlat12_ib_census *c = &k->c;
        fprintf(stderr, "[tri] DUMP %s s%02u VA 0x%llx (+0x%llx in region 0x%llx-0x%llx tag %u prot %u) walk %u dw hash %08x; "
                "packets %u ctxctl-first %u; SET ctx %u sh-gfx %u sh-cs %u ucfg %u index %u; regs %u: identical %u moved %u "
                "repack %u absent %u legacy-vs %u UNKNOWN %u cs-proven %u; CLEAR_STATE %u LOAD_* %u CONTEXT_CONTROL proven %u "
                "other %u; COND_EXEC %u nested-IB %u DMA_DATA %u COPY_DATA %u bad-reg-operand %u; draws %u dispatches %u "
                "unlisted %u (first op 0x%x); first UNKNOWN reg 0x%x; first memory-loaded op 0x%x\n",
                phase, i, (unsigned long long)k->addr, (unsigned long long)(k->addr - k->rstart),
                (unsigned long long)k->rstart, (unsigned long long)k->rend, k->tag, k->prot, k->walk, k->hash,
                c->packets, c->first_is_ctxctl, c->set_ctx, c->set_sh_gfx, c->set_sh_cs, c->set_ucfg, c->set_index,
                c->regs, c->reg_cls[0], c->reg_cls[1], c->reg_cls[2], c->reg_cls[3], c->reg_cls[4], c->reg_cls[5],
                c->cs_proven_regs, c->clear_state, c->load_reg, c->ctxctl_proven, c->ctxctl_other, c->cond_exec,
                c->nested_ib, c->dma_data, c->copy_data, c->reg_operand_bad, c->draws, c->dispatches, c->unlisted,
                c->first_unlisted_op, c->first_unknown_reg, c->first_memloaded_op);
        const uint32_t *p = (const uint32_t *)(uintptr_t)k->addr;
        snprintf(path, sizeof path, "%s/%s_s%02u_%llx.bin", gDumpDir, phase, i, (unsigned long long)k->addr);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write_all(fd, p, (size_t)k->walk * 4u); close(fd); }
        snprintf(path, sizeof path, "%s/%s_s%02u_%llx.txt", gDumpDir, phase, i, (unsigned long long)k->addr);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            for (uint32_t q = 0; q < k->walk; q += 8) {
                char line[128]; int n = snprintf(line, sizeof line, "pg[%04x]", q);
                for (uint32_t z = 0; z < 8 && q + z < k->walk; z++) n += snprintf(line + n, sizeof line - (size_t)n, " %08x", p[q + z]);
                line[n++] = '\n';
                write_all(fd, line, (size_t)n);
            }
            close(fd);
        }
        // context: the page before the stream's first page through the page after its last, if readable.
        const uint64_t pg = (uint64_t)vm_page_size;
        uint64_t cs = (k->addr & ~(pg - 1)); if (cs >= k->rstart + pg) cs -= pg;
        uint64_t ce = ((k->addr + (uint64_t)k->walk * 4u + pg - 1) & ~(pg - 1)) + pg; if (ce > k->rend) ce = k->rend;
        snprintf(path, sizeof path, "%s/%s_s%02u_ctx_%llx.bin", gDumpDir, phase, i, (unsigned long long)cs);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            for (uint64_t v = cs; v < ce; v += pg) {
                if (page_readable((const uint8_t *)(uintptr_t)v)) write_all(fd, (const void *)(uintptr_t)v, (size_t)pg);
                else { static const uint8_t z[16384]; write_all(fd, z, (size_t)pg); }
            }
            close(fd);
        }
    }
}

// ---- device pick ----------------------------------------------------------
static id<MTLDevice> pick_device(void) {
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    for (id<MTLDevice> d in all) {
        NSString *n = d.name ?: @"?";
        if ([n localizedCaseInsensitiveContainsString:@"Apple"]) continue;
        if ([n localizedCaseInsensitiveContainsString:@"Radeon"] ||
            [n localizedCaseInsensitiveContainsString:@"GFX10"]) return d;
    }
    return MTLCreateSystemDefaultDevice();
}

static void state(const char *s) {
    char p[512]; snprintf(p, sizeof p, "%s/tri.state", kStateDir);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

// ---- suite mode (0.0.224, an earlier analysis): the standing milestone-2 regression ------------------------------------------
// `tri suite [--cases a,b,...] [--inject DIR] [--ring] [--dump DIR]`. ONE command buffer, one render pass per case, each pass
// ONE draw into its own 64x64 target of the draw mode's kind (a texture on a Shared MTLBuffer), its own CPU prefill, DontCare
// load, and its own `TRI-CASE:` verdict line; the closing `TRI: mode=suite` line passes only when every case passes.
// One command buffer, not several in sequence: the render takeover is one-shot per boot (xlatregs rewrites only the SDMA IBs
// present when it runs, renderdraw the render streams pending before sdmamap, and after sdmamap nothing holds a new frame
// back), so every case's stream must be pending in the ONE takeover; a command buffer committed later would reach the CP as
// an untranslated gfx10 stream. Register state written by an earlier pass persists into the later passes (nothing clears it
// between encoders); whether each pass's segment re-emits all of its own state is the userspace census's question
// (`tridump-suite`, an earlier analysis).
// --inject DIR: DIR/manifest.json {"suite": {"<pipeline key>": {"vertex": file, "fragment": file}}}; the swizzle substitutes
// the key's pair while that pipeline is built. Cases (NDC; viewport 64x64: pixel x = (x+1)*32, y = (1-y)*32):
//   base    m2tri pair: (0,.7) (-.7,-.7) (.7,-.7), vertexStart 0 - the milestone-2 draw; must also draw exactly 968 pixels
//   two     m2tri pair: two disjoint triangles in ONE draw (vertexCount 6, np 2)
//   vstart  m2tri pair: vertexStart 3 of 6; vertices 0-2 are a decoy over the upper-left half, 3-5 the apex-down triangle
//   mvp     m2tri_mvp pair: the base triangle through a float4x4 at buffer(1): x' = .5x + .3, y' = -.5y - .2
//   col     m2col pair: per-vertex red/green/blue (stride 20) interpolated, checked against the barycentric colour
//   tex     SkyLight SimpleVertex + SimpleTextureFragment: identity mvp_matrix, a 64x64 four-quadrant checker on a Shared
//           texture (VidMemory: residency copy), nearest/clamp sampler, the quad -.75..+.75 from two triangles
//   texb    tex with the checker on a Shared MTLBuffer (a host page)
//   blend   m2half pair (m2_tri_vs + constant (0,1,0,.5)): source-over blending onto a left-red / right-blue prefill
typedef struct { double x, y; } SP;
enum { kSuiteMaxCases = 8, kSuiteMaxFloats = 64 };
typedef struct {
    const char *name, *pipe;
    NSUInteger vstart, vcount, stride, nfloat;
    float vdata[kSuiteMaxFloats];
    int nshape, shapeN[2];
    double shape[2][6][2];   // convex coverage polygons, NDC
    int exp;                 // 0 constant draw colour, 1 barycentric colour, 2 checker texel, 3 blend over the prefill
    int prefill;             // 0 blue, 1 left half red / right half blue
    int regress968;          // the r58-r60 readback: exactly 968 drawn pixels
    int mvp;                 // 0 none, 1 the mvp case's matrix at buffer(1), 2 identity at buffer(1)
    int tex;                 // 0 none, 1 Shared texture, 2 texture on a Shared MTLBuffer
    double col[3][3];        // exp 1: RGB of shape 0's vertices
} SuiteCase;

static NSDictionary<NSString *, NSArray<NSData *> *> *gSuitePairs;
static NSMutableArray *gSuiteKeep;   // every Metal object the passes use, alive until exit
static NSString *const kSkyLightMetallib =
    @"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib";
// MSL identical in interface to tools/b1-tests/m2tri/{m2tri,m2tri_mvp,m2col,m2half}.metal (the compiler's sources)
static NSString *const kSrcM2Tri =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct M2VIn  { float2 pos [[attribute(0)]]; };\n"
    @"struct M2VOut { float4 pos [[position]]; };\n"
    @"vertex M2VOut m2_tri_vs(M2VIn in [[stage_in]]) { M2VOut o; o.pos = float4(in.pos, 0.0, 1.0); return o; }\n"
    @"fragment float4 m2_tri_fs() { return float4(1.0, 0.25, 0.0, 1.0); }\n";
static NSString *const kSrcM2Mvp =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct M2VIn  { float2 pos [[attribute(0)]]; };\n"
    @"struct M2VOut { float4 pos [[position]]; };\n"
    @"vertex M2VOut m2_mvp_vs(M2VIn in [[stage_in]], constant float4x4 &mvp [[buffer(1)]]) {\n"
    @"  M2VOut o; o.pos = mvp * float4(in.pos, 0.0, 1.0); return o; }\n"
    @"fragment float4 m2_mvp_fs() { return float4(1.0, 0.25, 0.0, 1.0); }\n";
static NSString *const kSrcM2Col =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct M2CIn  { float2 pos [[attribute(0)]]; float3 col [[attribute(1)]]; };\n"
    @"struct M2COut { float4 pos [[position]]; float4 col; };\n"
    @"vertex M2COut m2_col_vs(M2CIn in [[stage_in]]) { M2COut o; o.pos = float4(in.pos, 0.0, 1.0); o.col = float4(in.col, 1.0); return o; }\n"
    @"fragment float4 m2_col_fs(M2COut in [[stage_in]]) { return in.col; }\n";
static NSString *const kSrcM2Half =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct M2VIn  { float2 pos [[attribute(0)]]; };\n"
    @"struct M2VOut { float4 pos [[position]]; };\n"
    @"vertex M2VOut m2_tri_vs(M2VIn in [[stage_in]]) { M2VOut o; o.pos = float4(in.pos, 0.0, 1.0); return o; }\n"
    @"fragment float4 m2_half_fs() { return float4(0.0, 1.0, 0.0, 0.5); }\n";
// Checker quadrants in BGRA8 memory order: TL red, TR green, BL yellow, BR white (none equals the blue prefill).
static const uint32_t kChecker[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u, 0xFFFFFFFFu };

static void sc_tri(SuiteCase *c, int s, double ax, double ay, double bx, double by, double cx, double cy) {
    c->shapeN[s] = 3;
    c->shape[s][0][0] = ax; c->shape[s][0][1] = ay; c->shape[s][1][0] = bx; c->shape[s][1][1] = by;
    c->shape[s][2][0] = cx; c->shape[s][2][1] = cy;
    if (c->nshape < s + 1) c->nshape = s + 1;
}
static void sc_floats(SuiteCase *c, const float *f, NSUInteger n) { memcpy(c->vdata, f, n * sizeof(float)); c->nfloat = n; }

static int suite_case(const char *name, SuiteCase *c) {
    memset(c, 0, sizeof *c);
    c->name = name; c->stride = 8; c->vcount = 3;
    static const float kTri[6] = { 0.0f, 0.7f, -0.7f, -0.7f, 0.7f, -0.7f };
    if (!strcmp(name, "base")) {
        c->pipe = "m2tri"; sc_floats(c, kTri, 6); sc_tri(c, 0, 0, .7, -.7, -.7, .7, -.7); c->regress968 = 1; return 0;
    }
    if (!strcmp(name, "two")) {
        static const float f[12] = { -0.9f, 0.8f, -0.9f, -0.8f, -0.1f, -0.8f, 0.1f, 0.8f, 0.9f, 0.8f, 0.9f, -0.8f };
        c->pipe = "m2tri"; sc_floats(c, f, 12); c->vcount = 6;
        sc_tri(c, 0, -.9, .8, -.9, -.8, -.1, -.8); sc_tri(c, 1, .1, .8, .9, .8, .9, -.8); return 0;
    }
    if (!strcmp(name, "vstart")) {
        static const float f[12] = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, -0.6f, 0.6f, 0.6f, 0.6f, 0.0f, -0.6f };
        c->pipe = "m2tri"; sc_floats(c, f, 12); c->vstart = 3; sc_tri(c, 0, -.6, .6, .6, .6, 0, -.6); return 0;
    }
    if (!strcmp(name, "mvp")) {
        c->pipe = "m2mvp"; sc_floats(c, kTri, 6); c->mvp = 1; sc_tri(c, 0, .3, -.55, -.05, .15, .65, .15); return 0;
    }
    if (!strcmp(name, "col")) {
        static const float f[15] = { 0.0f, 0.7f, 1, 0, 0, -0.7f, -0.7f, 0, 1, 0, 0.7f, -0.7f, 0, 0, 1 };
        c->pipe = "m2col"; sc_floats(c, f, 15); c->stride = 20; c->exp = 1; sc_tri(c, 0, 0, .7, -.7, -.7, .7, -.7);
        c->col[0][0] = 1; c->col[1][1] = 1; c->col[2][2] = 1; return 0;
    }
    if (!strcmp(name, "tex") || !strcmp(name, "texb")) {
        static const float f[24] = { -0.75f, 0.75f, 0, 0,  -0.75f, -0.75f, 0, 1,  0.75f, 0.75f, 1, 0,
                                      0.75f, 0.75f, 1, 0,  -0.75f, -0.75f, 0, 1,  0.75f, -0.75f, 1, 1 };
        c->pipe = "simple"; sc_floats(c, f, 24); c->stride = 16; c->vcount = 6; c->exp = 2; c->mvp = 2;
        c->tex = name[3] == 'b' ? 2 : 1;
        c->nshape = 1; c->shapeN[0] = 4;   // TL, TR, BR, BL
        const double q[4][2] = { { -.75, .75 }, { .75, .75 }, { .75, -.75 }, { -.75, -.75 } };
        for (int i = 0; i < 4; i++) { c->shape[0][i][0] = q[i][0]; c->shape[0][i][1] = q[i][1]; }
        return 0;
    }
    if (!strcmp(name, "blend")) {
        c->pipe = "m2half"; sc_floats(c, kTri, 6); c->exp = 3; c->prefill = 1; sc_tri(c, 0, 0, .7, -.7, -.7, .7, -.7); return 0;
    }
    return 1;
}

static SP sc_px(const SuiteCase *c, int s, int i) {
    SP p = { (c->shape[s][i][0] + 1.0) * W / 2.0, (1.0 - c->shape[s][i][1]) * H / 2.0 };
    return p;
}
// Signed distance in pixels from p to the nearest edge of convex polygon s: positive inside.
static double sc_min_dist(const SuiteCase *c, int s, SP p) {
    const int n = c->shapeN[s];
    double area2 = 0, md = 1e30;
    for (int i = 0; i < n; i++) { SP a = sc_px(c, s, i), b = sc_px(c, s, (i + 1) % n); area2 += a.x * b.y - b.x * a.y; }
    const double sg = area2 >= 0 ? 1.0 : -1.0;
    for (int i = 0; i < n; i++) {
        SP a = sc_px(c, s, i), b = sc_px(c, s, (i + 1) % n);
        const double ex = b.x - a.x, ey = b.y - a.y, len = sqrt(ex * ex + ey * ey);
        const double d = sg * (ex * (p.y - a.y) - ey * (p.x - a.x)) / len;
        if (d < md) md = d;
    }
    return md;
}
static uint32_t sc_bgra(int r, int g, int b, int a) { return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24); }
static int sc_u8(double v) { long k = lround(v * 255.0); return k < 0 ? 0 : k > 255 ? 255 : (int)k; }
static int sc_close(uint32_t v, uint32_t w, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        const int d = (int)((v >> sh) & 0xff) - (int)((w >> sh) & 0xff);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}
static uint32_t sc_prefill(const SuiteCase *c, int x) { return (c->prefill == 1 && x < W / 2) ? 0xFFFF0000u : kClearBGRA; }
// 1 when v is an acceptable drawn colour at pixel (x, y); *want = the nominal expected value.
static int sc_expect(const SuiteCase *c, int x, int y, uint32_t v, uint32_t *want) {
    const SP p = { x + 0.5, y + 0.5 };
    if (c->exp == 0) { *want = 0xFFFF4000u; return is_draw_colour(v); }
    if (c->exp == 1) {
        const SP a = sc_px(c, 0, 0), b = sc_px(c, 0, 1), d = sc_px(c, 0, 2);
        const double den = (b.y - d.y) * (a.x - d.x) + (d.x - b.x) * (a.y - d.y);
        const double w0 = ((b.y - d.y) * (p.x - d.x) + (d.x - b.x) * (p.y - d.y)) / den;
        const double w1 = ((d.y - a.y) * (p.x - d.x) + (a.x - d.x) * (p.y - d.y)) / den;
        const double w2 = 1.0 - w0 - w1;
        double rgb[3];
        for (int k = 0; k < 3; k++) rgb[k] = w0 * c->col[0][k] + w1 * c->col[1][k] + w2 * c->col[2][k];
        *want = sc_bgra(sc_u8(rgb[0]), sc_u8(rgb[1]), sc_u8(rgb[2]), 255);
        return sc_close(v, *want, 8);
    }
    if (c->exp == 2) {
        const SP tl = sc_px(c, 0, 0), br = sc_px(c, 0, 2);
        const double mx = (tl.x + br.x) / 2.0, my = (tl.y + br.y) / 2.0;
        const int qx = p.x < mx ? 0 : 1, qy = p.y < my ? 0 : 1;
        const int ax = fabs(p.x - mx) < 1.0, ay = fabs(p.y - my) < 1.0;   // on a texel midline: either neighbour
        *want = kChecker[qy * 2 + qx];
        for (int jy = 0; jy < 2; jy++)
            for (int jx = 0; jx < 2; jx++) {
                if ((jx != qx && !ax) || (jy != qy && !ay)) continue;
                if (sc_close(v, kChecker[jy * 2 + jx], 2)) return 1;
            }
        return 0;
    }
    const uint32_t d = sc_prefill(c, x);   // blend: 0.5 * (0, 1, 0) + 0.5 * dst, alpha 0.5 * 1 + 0.5 * 1
    *want = sc_bgra(sc_u8(0.5 * ((d >> 16) & 0xff) / 255.0), sc_u8(0.5 + 0.5 * ((d >> 8) & 0xff) / 255.0),
                    sc_u8(0.5 * (d & 0xff) / 255.0), 255);
    return sc_close(v, *want, 3);
}

// One case's verdict over its readback: sure-in pixels (>= 1 px inside a coverage polygon) must be the expected colour,
// sure-out pixels (>= 1 px outside every polygon) the prefill, the 2-px edge band either; plus base's 968-pixel regression.
static int suite_verdict(const SuiteCase *c, const uint32_t *px, int injected) {
    uint32_t nIn = 0, okIn = 0, nOut = 0, okOut = 0, nBand = 0, okBand = 0, drawn = 0, shown = 0;
    int maxErr = 0, bl = 0;
    char bad[720]; bad[0] = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const SP p = { x + 0.5, y + 0.5 };
            double md = -1e30;
            for (int s = 0; s < c->nshape; s++) { const double d = sc_min_dist(c, s, p); if (d > md) md = d; }
            const uint32_t v = px[y * W + x], pre = sc_prefill(c, x);
            uint32_t want = 0;
            const int ok = sc_expect(c, x, y, v, &want);
            if (v != pre) drawn++;
            int good;
            if (md <= -1.0) { nOut++; good = v == pre; okOut += (uint32_t)good; }
            else if (md >= 1.0) {
                nIn++; good = ok; okIn += (uint32_t)good;
                for (int sh = 0; sh < 24; sh += 8) {
                    const int e = abs((int)((v >> sh) & 0xff) - (int)((want >> sh) & 0xff));
                    if (e > maxErr) maxErr = e;
                }
            } else { nBand++; good = (v == pre) || ok; okBand += (uint32_t)good; }
            if (!good && shown < 4) {
                bl += snprintf(bad + bl, sizeof bad - (size_t)bl, " (%d,%d)=%08x want %08x edge-dist %.1f", x, y, v,
                               md <= -1.0 ? pre : want, md);
                shown++;
            }
        }
    const int pass = okIn == nIn && okOut == nOut && okBand == nBand && nIn > 0 && drawn > 0 && (!c->regress968 || drawn == 968u);
    printf("TRI-CASE: case=%s pipe=%s injected=%d vstart=%lu vcount=%lu drawn=%u%s sure-in %u/%u sure-out %u/%u band %u/%u "
           "max-in-err %d centre=0x%08x verdict=%s\n", c->name, c->pipe, injected, (unsigned long)c->vstart,
           (unsigned long)c->vcount, drawn, c->regress968 ? (drawn == 968u ? " (=r58-r60's 968)" : " (NOT r58-r60's 968)") : "",
           okIn, nIn, okOut, nOut, okBand, nBand, maxErr, px[(H / 2) * W + W / 2], pass ? "PASS" : "FAIL");
    if (shown) printf("TRI-CASE-BAD: case=%s%s\n", c->name, bad);
    if (c->exp == 1) {   // the interpolation evidence: a pixel near each vertex and the centroid
        char s[480]; int n = 0;
        SP cen = { 0, 0 };
        for (int i = 0; i < 3; i++) { const SP a = sc_px(c, 0, i); cen.x += a.x / 3.0; cen.y += a.y / 3.0; }
        for (int i = 0; i <= 3; i++) {
            SP q = cen;
            if (i < 3) { const SP a = sc_px(c, 0, i); q.x = a.x + 0.25 * (cen.x - a.x); q.y = a.y + 0.25 * (cen.y - a.y); }
            const int xi = (int)floor(q.x), yi = (int)floor(q.y);
            if (xi < 0 || yi < 0 || xi >= W || yi >= H) continue;
            uint32_t w = 0;
            const int ok = sc_expect(c, xi, yi, px[yi * W + xi], &w);
            n += snprintf(s + n, sizeof s - (size_t)n, " %s(%d,%d)=%08x want %08x %s", i == 3 ? "centroid" : i == 0 ? "near-v0" :
                          i == 1 ? "near-v1" : "near-v2", xi, yi, px[yi * W + xi], w, ok ? "ok" : "BAD");
        }
        printf("TRI-CASE-COL: case=%s%s\n", c->name, s);
    }
    return pass;
}

static MTLFunctionConstantValues *suite_bools_off(id<MTLFunction> f) {   // g2capture.m bools_off
    MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
    for (NSString *k in f.functionConstantsDictionary) {
        MTLFunctionConstant *cst = f.functionConstantsDictionary[k];
        if (cst.type == MTLDataTypeBool) { BOOL no = NO; [fc setConstantValue:&no type:MTLDataTypeBool atIndex:cst.index]; }
    }
    return fc;
}
static MTLVertexDescriptor *suite_reflected_vd(id<MTLFunction> v) {   // g2capture.m reflected_vd
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    NSUInteger off = 0;
    for (MTLVertexAttribute *a in v.vertexAttributes) {
        MTLVertexFormat fmt = MTLVertexFormatFloat4; NSUInteger sz = 16;
        switch (a.attributeType) {
            case MTLDataTypeFloat:  fmt = MTLVertexFormatFloat;  sz = 4;  break;
            case MTLDataTypeFloat2: fmt = MTLVertexFormatFloat2; sz = 8;  break;
            case MTLDataTypeFloat3: fmt = MTLVertexFormatFloat3; sz = 12; break;
            default: break;
        }
        vd.attributes[a.attributeIndex].format = fmt;
        vd.attributes[a.attributeIndex].offset = off;
        vd.attributes[a.attributeIndex].bufferIndex = 0;
        off += sz;
    }
    vd.layouts[0].stride = off;
    return vd;
}

static int load_suite_manifest(NSString *dir) {
    NSData *mj = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"manifest.json"]];
    if (!mj) { fprintf(stderr, "[tri] suite --inject: no manifest.json in %s\n", dir.UTF8String); return 1; }
    NSError *e = nil;
    NSDictionary *m = [NSJSONSerialization JSONObjectWithData:mj options:0 error:&e];
    NSDictionary *s = [m isKindOfClass:NSDictionary.class] ? m[@"suite"] : nil;
    if (![s isKindOfClass:NSDictionary.class]) { fprintf(stderr, "[tri] suite --inject: manifest.json has no \"suite\" object\n"); return 1; }
    NSMutableDictionary *out = [NSMutableDictionary new];
    for (NSString *k in s) {
        NSDictionary *p = s[k];
        if (![p isKindOfClass:NSDictionary.class]) continue;
        NSData *vd = [p[@"vertex"] isKindOfClass:NSString.class] ? [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:p[@"vertex"]]] : nil;
        NSData *fd = [p[@"fragment"] isKindOfClass:NSString.class] ? [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:p[@"fragment"]]] : nil;
        fprintf(stderr, "[tri] suite --inject: pair %s vertex %lu B fragment %lu B%s\n", k.UTF8String, (unsigned long)vd.length,
                (unsigned long)fd.length, (vd && fd) ? "" : " - INCOMPLETE, not injected");
        if (vd && fd) out[k] = @[vd, fd];
    }
    gSuitePairs = out;
    return out.count ? 0 : 1;
}

static id<MTLRenderPipelineState> suite_pipeline(id<MTLDevice> dev, const char *key, int *injected) {
    NSError *e = nil;
    NSArray<NSData *> *pair = gSuitePairs[@(key)];
    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (!strcmp(key, "simple")) {
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:kSkyLightMetallib] error:&e];
        id<MTLFunction> vb = [lib newFunctionWithName:@"SimpleVertex"], fb = [lib newFunctionWithName:@"SimpleTextureFragment"];
        if (vb) rpd.vertexFunction = [lib newFunctionWithName:@"SimpleVertex" constantValues:suite_bools_off(vb) error:&e];
        if (fb) rpd.fragmentFunction = [lib newFunctionWithName:@"SimpleTextureFragment" constantValues:suite_bools_off(fb) error:&e];
        if (rpd.vertexFunction) rpd.vertexDescriptor = suite_reflected_vd(rpd.vertexFunction);
    } else {
        NSString *src = !strcmp(key, "m2tri") ? kSrcM2Tri : !strcmp(key, "m2mvp") ? kSrcM2Mvp :
                        !strcmp(key, "m2col") ? kSrcM2Col : !strcmp(key, "m2half") ? kSrcM2Half : nil;
        NSString *vn = !strcmp(key, "m2mvp") ? @"m2_mvp_vs" : !strcmp(key, "m2col") ? @"m2_col_vs" : @"m2_tri_vs";
        NSString *fn = !strcmp(key, "m2mvp") ? @"m2_mvp_fs" : !strcmp(key, "m2col") ? @"m2_col_fs" :
                       !strcmp(key, "m2half") ? @"m2_half_fs" : @"m2_tri_fs";
        id<MTLLibrary> lib = src ? [dev newLibraryWithSource:src options:nil error:&e] : nil;
        rpd.vertexFunction = [lib newFunctionWithName:vn];
        rpd.fragmentFunction = [lib newFunctionWithName:fn];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 8;
        if (!strcmp(key, "m2col")) {
            vd.attributes[1].format = MTLVertexFormatFloat3; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
            vd.layouts[0].stride = 20;
        }
        rpd.vertexDescriptor = vd;
        if (!strcmp(key, "m2half")) {
            MTLRenderPipelineColorAttachmentDescriptor *ca = rpd.colorAttachments[0];
            ca.blendingEnabled = YES;
            ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    if (!rpd.vertexFunction || !rpd.fragmentFunction) {
        fprintf(stderr, "[tri] SUITE pipeline %s: functions not found (%s)\n", key, e.localizedDescription.UTF8String ?: "-");
        *injected = 0;
        return nil;
    }
    gVertexPB = pair ? pair[0] : nil; gFragmentPB = pair ? pair[1] : nil;
    const int before = gInjected;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rpd error:&e];
    gVertexPB = nil; gFragmentPB = nil;
    *injected = gInjected - before;
    fprintf(stderr, "[tri] SUITE pipeline %s: %s%s, injected %d stage(s) (%s)\n", key, ps ? "built" : "FAILED: ",
            ps ? "" : e.localizedDescription.UTF8String, *injected, pair ? "pair from the manifest" : "NO pair: Apple's own shaders");
    return ps;
}

// census1 : a 16x16 texture on a 1024-byte buffer (bytesPerRow 64) FAILED to create; the render target's
// buffer-backed kind works at 64x64 with bytesPerRow 256, so the checker is 64x64 (quadrants 32x32) for both kinds.
static id<MTLTexture> suite_checker(id<MTLDevice> dev, int kind, const char **storage) {
    enum { T = 64 };
    static uint32_t pix[T * T];
    for (int y = 0; y < T; y++)
        for (int x = 0; x < T; x++) pix[y * T + x] = kChecker[(y >= T / 2 ? 2 : 0) + (x >= T / 2 ? 1 : 0)];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:T height:T mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = nil;
    if (kind == 2) {
        id<MTLBuffer> b = [dev newBufferWithLength:T * T * 4 options:MTLResourceStorageModeShared];
        if (b) { memcpy(b.contents, pix, sizeof pix); [gSuiteKeep addObject:b]; t = [b newTextureWithDescriptor:td offset:0 bytesPerRow:T * 4]; }
        *storage = t ? "texture on a Shared MTLBuffer (host page)" : "texture on a Shared MTLBuffer FAILED";
    } else {
        t = [dev newTextureWithDescriptor:td];
        if (t) [t replaceRegion:MTLRegionMake2D(0, 0, T, T) mipmapLevel:0 withBytes:pix bytesPerRow:T * 4];
        *storage = t ? "Shared texture (VidMemory, residency copy)" : "Shared texture FAILED";
    }
    return t;
}

static void write_case_files(const char *name, const uint32_t *px) {
    char path[512];
    snprintf(path, sizeof path, "%s/tri-case-%s.ppm", kStateDir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", W, H);
        for (uint32_t i = 0; i < W * H; i++) {
            const uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/tri-case-%s.bgra", kStateDir, name);
    f = fopen(path, "wb");
    if (f) { fwrite(px, 4, W * H, f); fclose(f); }
}

// 0.0.235 (milestone 3 step 1, an earlier analysis): --nowait. The client commits and does NOT poll
// cb.status; it installs an addCompletedHandler first and then waits for THAT to fire. The handler is
// driven by Apple's asynchronous retire (timeStampInterruptCallback -> timestampUpdated -> advance),
// which on our stack only runs if the kext's IH -> checkTimestamps bridge calls it, because we own the
// MSI and the IH ring (an earlier analysis addendum 2). So a handler that fires with no blocking wait
// anywhere is the proof the bridge works; a handler that never fires while the readback is correct
// means the GPU finished and only Apple's ACCOUNTING is stuck.
static int gNoWait = 0;
static const double kNoWaitSeconds = 120.0;

static double now_seconds(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int run_suite(id<MTLDevice> dev, id<MTLCommandQueue> q, NSString *injectDir, NSString *casesArg, int doRing, int dump) {
    gSuiteKeep = [NSMutableArray new];
    static SuiteCase cases[kSuiteMaxCases];
    int nc = 0;
    for (NSString *nm in [(casesArg ?: @"base,two,vstart,mvp") componentsSeparatedByString:@","]) {
        if (!nm.length) continue;
        if (nc >= kSuiteMaxCases) { printf("TRI: mode=suite more than %d cases - refused\n", kSuiteMaxCases); return 2; }
        if (suite_case(strdup(nm.UTF8String), &cases[nc]) != 0) { printf("TRI: mode=suite unknown case %s - refused\n", nm.UTF8String); return 2; }
        nc++;
    }
    if (nc == 0) { printf("TRI: mode=suite no case - refused\n"); return 2; }
    fprintf(stderr, "[tri] SUITE %d case(s) in ONE command buffer, one render pass each:", nc);
    for (int i = 0; i < nc; i++) fprintf(stderr, " %s(%s)", cases[i].name, cases[i].pipe);
    fprintf(stderr, "\n");
    if (injectDir) {
        if (load_suite_manifest(injectDir) == 0) install_swizzle();
        else fprintf(stderr, "[tri] suite --inject failed; every pipeline keeps Apple's compiled shaders\n");
    }

    NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *pipes = [NSMutableDictionary new];
    NSMutableDictionary<NSString *, NSNumber *> *injCount = [NSMutableDictionary new];
    int usable[kSuiteMaxCases];
    for (int i = 0; i < nc; i++) {
        NSString *k = @(cases[i].pipe);
        if (!pipes[k] && !injCount[k]) {
            int n = 0;
            id<MTLRenderPipelineState> ps = suite_pipeline(dev, cases[i].pipe, &n);
            injCount[k] = @(n);
            if (ps) pipes[k] = ps;
        }
        usable[i] = pipes[k] != nil;
        if (!usable[i] && !dump) { printf("TRI: mode=suite pipeline %s for case %s failed - nothing committed\n", cases[i].pipe, cases[i].name); return 1; }
    }

    id<MTLBuffer> ringBuf = nil;
    uint64_t ringBase = 0, ringOff = 0;
    if (doRing) {
        // 0.0.226: + one page past the rings for RADV's ring-offsets table (XLAT12_GE_RING_DESC_OFF), whose attribute-ring
        // descriptor at +0xa0 the merged vertex stage loads through s0:s1; the kext writes its VA into PGM_LO/HI_GS only with bit 0x8000
        const NSUInteger rlen = (NSUInteger)XLAT12_GE_RING_TOTAL + (NSUInteger)XLAT12_GE_RING_ALIGN + (NSUInteger)XLAT12_GE_RING_DESC_PAGE;
        ringBuf = [dev newBufferWithLength:rlen options:MTLResourceStorageModeShared];
        if (!ringBuf || !ringBuf.contents) { printf("TRI: ring buffer of %lu bytes failed\n", (unsigned long)rlen); return 1; }
        memset(ringBuf.contents, 0, rlen);
        uint64_t ga = 0;
        if (@available(macOS 13.0, *)) ga = ringBuf.gpuAddress;
        ringBase = (ga + XLAT12_GE_RING_ALIGN - 1u) & ~(uint64_t)(XLAT12_GE_RING_ALIGN - 1u);
        ringOff = ringBase - ga;
        if (!ga || ringOff + (uint64_t)XLAT12_GE_RING_TOTAL + (uint64_t)XLAT12_GE_RING_DESC_PAGE > (uint64_t)rlen) {
            printf("TRI: ring base 0x%llx does not fit the buffer at 0x%llx\n", (unsigned long long)ringBase, (unsigned long long)ga);
            return 1;
        }
        fprintf(stderr, "[tri] RING buffer gpuAddress 0x%llx length 0x%lx contents %p; base 0x%llx (+0x%llx, 64 KiB aligned) "
                "attr 0x%llx pos 0x%llx prim 0x%llx end 0x%llx\n", (unsigned long long)ga, (unsigned long)rlen, ringBuf.contents,
                (unsigned long long)ringBase, (unsigned long long)ringOff, (unsigned long long)ringBase,
                (unsigned long long)(ringBase + XLAT12_GE_RING_POS_OFF), (unsigned long long)(ringBase + XLAT12_GE_RING_PRIM_OFF),
                (unsigned long long)(ringBase + XLAT12_GE_RING_TOTAL));
        uint32_t desc[4];
        xlat12_attr_ring_descriptor(ringBase, XLAT12_GE_RING_TOTAL, desc);
        uint32_t *table = (uint32_t *)((uint8_t *)ringBuf.contents + ringOff + XLAT12_GE_RING_DESC_OFF);
        for (int k = 0; k < 4; k++) table[XLAT12_RING_PS_ATTR_OFF / 4u + (uint32_t)k] = desc[k];
        fprintf(stderr, "[tri] RING ring-offsets table VA 0x%llx: attribute-ring descriptor at +0xa0 = %08x %08x %08x %08x\n",
                (unsigned long long)(ringBase + XLAT12_GE_RING_DESC_OFF), desc[0], desc[1], desc[2], desc[3]);
    }

    NSMutableArray<id<MTLBuffer>> *targets = [NSMutableArray new];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest; sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLBuffer> baseVb = nil;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int i = 0; i < nc; i++) {
        SuiteCase *c = &cases[i];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLBuffer> rb = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        id<MTLTexture> tgt = rb ? [rb newTextureWithDescriptor:td offset:0 bytesPerRow:W * 4] : nil;
        id<MTLBuffer> vb = [dev newBufferWithBytes:c->vdata length:c->nfloat * sizeof(float) options:MTLResourceStorageModeShared];
        if (!rb || !tgt || !vb) { printf("TRI: mode=suite case %s: target or vertex buffer failed - nothing committed\n", c->name); return 1; }
        uint32_t *t32 = (uint32_t *)rb.contents;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) t32[y * W + x] = sc_prefill(c, x);
        [targets addObject:rb];
        [gSuiteKeep addObjectsFromArray:@[ tgt, vb ]];
        if (!strcmp(c->name, "base")) baseVb = vb;
        id<MTLBuffer> mb = nil;
        if (c->mvp) {
            static const float kMvp[16] = { 0.5f, 0, 0, 0,  0, -0.5f, 0, 0,  0, 0, 1, 0,  0.3f, -0.2f, 0, 1 };
            static const float kIdent[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
            mb = [dev newBufferWithBytes:(c->mvp == 1 ? kMvp : kIdent) length:64 options:MTLResourceStorageModeShared];
            if (!mb) { printf("TRI: mode=suite case %s: matrix buffer failed - nothing committed\n", c->name); return 1; }
            [gSuiteKeep addObject:mb];
        }
        id<MTLTexture> ctex = nil;
        const char *tstore = "-";
        if (c->tex) {
            ctex = suite_checker(dev, c->tex, &tstore);
            if (!ctex && !dump) { printf("TRI: mode=suite case %s: %s - nothing committed\n", c->name, tstore); return 1; }
            if (ctex) [gSuiteKeep addObject:ctex];
        }
        uint64_t gaT = 0, gaV = 0, gaM = 0;
        if (@available(macOS 13.0, *)) { gaT = rb.gpuAddress; gaV = vb.gpuAddress; gaM = mb ? mb.gpuAddress : 0; }
        fprintf(stderr, "[tri] SUITE case %s: target gpuAddress 0x%llx, vertex buffer gpuAddress 0x%llx (%lu floats, stride %lu), "
                "matrix 0x%llx, texture %s\n", c->name, (unsigned long long)gaT, (unsigned long long)gaV,
                (unsigned long)c->nfloat, (unsigned long)c->stride, (unsigned long long)gaM, tstore);
        if (!usable[i]) { fprintf(stderr, "[tri] SUITE case %s: no pipeline - pass NOT encoded\n", c->name); continue; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tgt;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        if (!re) { printf("TRI: mode=suite case %s: renderCommandEncoderWithDescriptor failed - nothing committed\n", c->name); return 1; }
        [re setRenderPipelineState:pipes[@(c->pipe)]];
        [re setVertexBuffer:vb offset:0 atIndex:0];
        if (mb) [re setVertexBuffer:mb offset:0 atIndex:1];
        if (ctex) { [re setFragmentTexture:ctex atIndex:0]; [re setFragmentSamplerState:samp atIndex:0]; }
        if (ringBuf) {
            if (@available(macOS 13.0, *))
                [re useResource:ringBuf usage:(MTLResourceUsageRead | MTLResourceUsageWrite)
                         stages:(MTLRenderStageVertex | MTLRenderStageFragment)];
            else
                [re useResource:ringBuf usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
        }
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:c->vstart vertexCount:c->vcount];
        [re endEncoding];
    }
    if (dump) {
        dump_phase("end");
        state([[NSString stringWithFormat:@"dumped mode=suite cases=%d (never committed)", nc] UTF8String]);
        printf("TRI-DUMP: mode=suite cases=%d inject=%d done (never committed)\n", nc, gInjected);
        return 0;
    }
    printf("committing (buffers stay alive; poll %s/tri.state)\n", kStateDir); fflush(stdout);
    // --nowait: register the completion handler BEFORE commit, so nothing can race it.
    __block volatile int handlerFired = 0;
    __block double handlerAt = -1.0;
    double t0 = now_seconds();
    if (gNoWait) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            handlerAt = now_seconds() - t0;
            handlerFired = 1;
            (void)b;
        }];
        printf("[tri] NOWAIT: completion handler registered; this client will NOT poll cb.status. "
               "Only Apple's asynchronous retire can wake it.\n");
        fflush(stdout);
    }
    state("committed");
    [cb commit];
    if (gNoWait) {
        // Wait on the HANDLER, never on cb.status: reading cb.status in a loop is itself a
        // wait path on this stack and would hide the very thing being measured.
        while (!handlerFired && now_seconds() - t0 < kNoWaitSeconds) usleep(100000);
        printf("TRI-ASYNC: handler=%s after=%.2fs waited=%.2fs status-at-handler-deadline=%ld verdict=%s\n",
               handlerFired ? "FIRED" : "NEVER", handlerFired ? handlerAt : -1.0,
               now_seconds() - t0, (long)cb.status, handlerFired ? "PASS" : "FAIL");
        fflush(stdout);
    }
    // In --nowait we have ALREADY waited kNoWaitSeconds on the handler, so this fallback poll is
    // capped hard: triwait gives the client 330 s total, and 120 s + 300 s would overrun it and
    // strand tri running into the next boot. Capping here keeps the measurement inside one run.
    const int pollIters = gNoWait ? 50 : 3000;
    for (int i = 0; i < pollIters; i++) {
        if (cb.status == MTLCommandBufferStatusCompleted || cb.status == MTLCommandBufferStatusError) break;
        usleep(100000);
    }
    const long st = (long)cb.status;
    printf("status: %ld\n", st);
    if (cb.error) printf("error: %s\n", cb.error.localizedDescription.UTF8String);
    int npass = 0;
    static uint32_t readback[W * H];
    for (int i = 0; i < nc; i++) {
        memcpy(readback, targets[(NSUInteger)i].contents, sizeof readback);
        npass += suite_verdict(&cases[i], readback, injCount[@(cases[i].pipe)].intValue);
        write_case_files(cases[i].name, readback);
    }
    printf("TRI: mode=suite storage=shared-buffer status=%ld inject=%d cases=%d pass=%d fail=%d verdict=%s\n", st, gInjected, nc,
           npass, nc - npass, npass == nc ? "PASS" : "FAIL");
    printf("TRI-FILES: tri-case-<name>.ppm tri-case-<name>.bgra%s in %s\n", baseVb ? " tri-vb-page.bin" : "", kStateDir);
    if (baseVb && ((uintptr_t)baseVb.contents & 0xfffu) == 0u) {
        char path[512]; snprintf(path, sizeof path, "%s/tri-vb-page.bin", kStateDir);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(baseVb.contents, 1, 4096, f); fclose(f); }
    }
    if (ringBuf) {
        const uint8_t *rp8 = (const uint8_t *)ringBuf.contents;
        const uint64_t lim[4] = { ringOff, ringOff + XLAT12_GE_RING_POS_OFF, ringOff + XLAT12_GE_RING_PRIM_OFF, ringOff + XLAT12_GE_RING_TOTAL };
        uint64_t nz[3] = { 0, 0, 0 };
        for (int r = 0; r < 3; r++) for (uint64_t k = lim[r]; k < lim[r + 1]; k++) if (rp8[k]) nz[r]++;
        printf("TRI-RING: base 0x%llx nonzero bytes attr %llu pos %llu prim %llu\n", (unsigned long long)ringBase,
               (unsigned long long)nz[0], (unsigned long long)nz[1], (unsigned long long)nz[2]);
        static const char *rn[3] = { "tri-ring-attr.bin", "tri-ring-pos.bin", "tri-ring-prim.bin" };
        for (int r = 0; r < 3; r++) {
            char path[512]; snprintf(path, sizeof path, "%s/%s", kStateDir, rn[r]);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(rp8 + lim[r], 1, 0x10000, f); fclose(f); }
        }
    }
    char sbuf[96]; snprintf(sbuf, sizeof sbuf, "done verdict=%s mode=suite", npass == nc ? "PASS" : "FAIL");
    state(sbuf);
    return 0;
}

int main(int argc, const char **argv) { @autoreleasepool {
    gSel = @selector(initWithCompilerOutput:shaderType:device:pipelineStatisticsOutput:);
    NSString *mode = @"clear", *injectDir = nil, *dumpDir = nil, *casesArg = nil;
    int doRing = 0;
    for (int i = 1; i < argc; i++) {
        NSString *a = @(argv[i]);
        if ([a isEqual:@"clear"] || [a isEqual:@"tri"] || [a isEqual:@"draw"] || [a isEqual:@"suite"]) mode = a;
        else if ([a isEqual:@"--inject"] && i + 1 < argc) injectDir = @(argv[++i]);
        else if ([a isEqual:@"--dump"] && i + 1 < argc) dumpDir = @(argv[++i]);
        else if ([a isEqual:@"--cases"] && i + 1 < argc) casesArg = @(argv[++i]);
        else if ([a isEqual:@"--ring"]) doRing = 1;
        else if ([a isEqual:@"--nowait"]) gNoWait = 1;   // 0.0.235 
        else { fprintf(stderr, "usage: tri [clear|tri|draw|suite] [--inject DIR] [--dump DIR] [--ring] [--cases a,b,...] [--nowait]\n"); return 2; }
    }
    int doTri = [mode isEqual:@"tri"], doDraw = [mode isEqual:@"draw"], doSuite = [mode isEqual:@"suite"];
    if (doRing && !doDraw && !doSuite) { printf("TRI: --ring needs draw or suite mode\n"); return 2; }
    if (casesArg && !doSuite) { printf("TRI: --cases needs suite mode\n"); return 2; }
    if (dumpDir) {
        gDumpDir = strdup(dumpDir.UTF8String);
        mkdir(gDumpDir, 0755);
        guard_install();
        gSelfLo = (uint64_t)(uintptr_t)gCands & ~(uint64_t)(vm_page_size - 1);
        gSelfHi = ((uint64_t)(uintptr_t)gCands + sizeof gCands + vm_page_size - 1) & ~(uint64_t)(vm_page_size - 1);
        fprintf(stderr, "[tri] DUMP mode: encoder built, endEncoding, NEVER committed; output %s\n", gDumpDir);
    }

    id<MTLDevice> dev = pick_device();
    if (!dev) { printf("TRI: no device\n"); return 1; }
    fprintf(stderr, "[tri] device: %s regID=0x%llx mode=%s\n", dev.name.UTF8String,
            (unsigned long long)dev.registryID, mode.UTF8String);

    if (injectDir && !doSuite && install_inject(injectDir) != 0)   // suite mode loads its per-pipeline manifest itself
        fprintf(stderr, "[tri] --inject failed; continuing with Apple's compiled shaders\n");

    NSError *e = nil;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (doSuite) {   // 0.0.224 
        if (!q) { printf("TRI: mode=suite no command queue\n"); return 1; }
        return run_suite(dev, q, injectDir, casesArg, doRing, dumpDir != nil);
    }

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // 0.0.211 : a texture on a Shared MTLBuffer FIRST. r40/r41 showed a Shared texture is
    // an AMDAccelVidMemory resource on this stack: Apple residency-copies its backing into VRAM and renders
    // there, and page-out is skipped, so its CPU copy never sees a GPU write. A buffer is a host page the GPU
    // writes directly (milestone 1's blit2 buffers), so a buffer-backed target is CPU-readable for real.
    const char *storage = "shared-buffer";
    td.storageMode = MTLStorageModeShared;
    id<MTLBuffer> rb = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLTexture> tex = rb ? [rb newTextureWithDescriptor:td offset:0 bytesPerRow:W * 4] : nil;
    if (!tex) {
        storage = "shared-texture(VRAM copy: CPU readback is NOT evidence)"; rb = nil;
        tex = [dev newTextureWithDescriptor:td];
    }
    if (!tex) {
        storage = "managed(UNREADABLE)"; rb = nil;
        td.storageMode = MTLStorageModeManaged;
        tex = [dev newTextureWithDescriptor:td];
    }
    if (rb) {
        if (@available(macOS 13.0, *))
            fprintf(stderr, "[tri] render target buffer gpuAddress 0x%llx contents %p\n",
                    (unsigned long long)rb.gpuAddress, rb.contents);
    }
    if (!q || !tex) { printf("TRI: setup failed (queue %p texture %p)\n", (__bridge void *)q, (__bridge void *)tex); return 1; }
    fprintf(stderr, "[tri] render target storage: %s\n", storage);
    static uint32_t fill[W * H];
    const uint32_t prefill = doDraw ? kClearBGRA : 0x77777777u;   // draw: blue prefill, DontCare load
    for (uint32_t i = 0; i < W * H; i++) fill[i] = prefill;
    if (rb) memcpy(rb.contents, fill, sizeof fill);
    else [tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:fill bytesPerRow:W * 4];

    id<MTLRenderPipelineState> ps = nil;
    id<MTLBuffer> vb = nil;
    if (doTri) {
        // VS puts a fixed clip-space triangle; PS returns red.
        static NSString *src =
          @"#include <metal_stdlib>\n using namespace metal;\n"
          @"struct VOut { float4 pos [[position]]; };\n"
          @"vertex VOut tri_vs(uint vid [[vertex_id]]) {\n"
          @"  float2 p[3] = { float2(0.0,0.7), float2(-0.7,-0.7), float2(0.7,-0.7) };\n"
          @"  VOut o; o.pos = float4(p[vid],0,1); return o; }\n"
          @"fragment float4 tri_fs() { return float4(1.0,0.0,0.0,1.0); }\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("TRI: MSL compile failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"tri_vs"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        ps = [dev newRenderPipelineStateWithDescriptor:rpd error:&e];
        if (!ps) { printf("TRI: newRenderPipelineState failed: %s\n", e.localizedDescription.UTF8String); return 1; }
    }
    if (doDraw) {
        // The m2tri pair, source identical to tools/b1-tests/m2tri/m2tri.metal (re/graphics/g2/tri/manifest.json).
        static NSString *src =
          @"#include <metal_stdlib>\nusing namespace metal;\n"
          @"struct M2VIn  { float2 pos [[attribute(0)]]; };\n"
          @"struct M2VOut { float4 pos [[position]]; };\n"
          @"vertex M2VOut m2_tri_vs(M2VIn in [[stage_in]]) { M2VOut o; o.pos = float4(in.pos, 0.0, 1.0); return o; }\n"
          @"fragment float4 m2_tri_fs() { return float4(1.0, 0.25, 0.0, 1.0); }\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { printf("TRI: MSL compile failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 8;
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"m2_tri_vs"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"m2_tri_fs"];
        rpd.vertexDescriptor = vd;
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        ps = [dev newRenderPipelineStateWithDescriptor:rpd error:&e];
        if (!ps) { printf("TRI: newRenderPipelineState failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        static const float pos[6] = { 0.0f, 0.7f, -0.7f, -0.7f, 0.7f, -0.7f };
        vb = [dev newBufferWithBytes:pos length:sizeof pos options:MTLResourceStorageModeShared];
        if (!vb) { printf("TRI: vertex buffer failed\n"); return 1; }
        if (@available(macOS 13.0, *))
            fprintf(stderr, "[tri] vertex buffer gpuAddress 0x%llx contents %p; shaders injected: %d stage(s)\n",
                    (unsigned long long)vb.gpuAddress, vb.contents, gInjected);
    }

    // 0.0.217 : the gfx12 NGG rings, one Shared buffer (see the header). The ring BASE registers hold
    // VA >> 16, so the base is the gpuAddress aligned UP to 64 KiB, and the buffer carries 64 KiB of slack for that.
    id<MTLBuffer> ringBuf = nil;
    uint64_t ringBase = 0, ringOff = 0;
    if (doRing) {
        const NSUInteger rlen = (NSUInteger)XLAT12_GE_RING_TOTAL + (NSUInteger)XLAT12_GE_RING_ALIGN;
        ringBuf = [dev newBufferWithLength:rlen options:MTLResourceStorageModeShared];
        if (!ringBuf || !ringBuf.contents) { printf("TRI: ring buffer of %lu bytes failed\n", (unsigned long)rlen); return 1; }
        memset(ringBuf.contents, 0, rlen);   // touch every page: resident and zero before Apple wires them
        uint64_t ga = 0;
        if (@available(macOS 13.0, *)) ga = ringBuf.gpuAddress;
        ringBase = (ga + XLAT12_GE_RING_ALIGN - 1u) & ~(uint64_t)(XLAT12_GE_RING_ALIGN - 1u);
        ringOff = ringBase - ga;
        if (!ga || ringOff + (uint64_t)XLAT12_GE_RING_TOTAL > (uint64_t)rlen) {
            printf("TRI: ring base 0x%llx does not fit the buffer at 0x%llx + 0x%lx\n", (unsigned long long)ringBase,
                   (unsigned long long)ga, (unsigned long)rlen);
            return 1;
        }
        fprintf(stderr, "[tri] RING buffer gpuAddress 0x%llx length 0x%lx contents %p; base 0x%llx (+0x%llx, 64 KiB aligned) "
                "attr 0x%llx pos 0x%llx prim 0x%llx end 0x%llx\n", (unsigned long long)ga, (unsigned long)rlen, ringBuf.contents,
                (unsigned long long)ringBase, (unsigned long long)ringOff, (unsigned long long)ringBase,
                (unsigned long long)(ringBase + XLAT12_GE_RING_POS_OFF), (unsigned long long)(ringBase + XLAT12_GE_RING_PRIM_OFF),
                (unsigned long long)(ringBase + XLAT12_GE_RING_TOTAL));
    }

    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = doDraw ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);  // blue = kClearBGRA
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    if (!re) { printf("TRI: renderCommandEncoderWithDescriptor failed\n"); return 1; }
    if (dumpDir) dump_phase("init");
    if (doTri) {
        [re setRenderPipelineState:ps];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (doDraw) {
        [re setRenderPipelineState:ps];
        [re setVertexBuffer:vb offset:0 atIndex:0];
        if (ringBuf) {
            if (@available(macOS 13.0, *))
                [re useResource:ringBuf usage:(MTLResourceUsageRead | MTLResourceUsageWrite)
                         stages:(MTLRenderStageVertex | MTLRenderStageFragment)];
            else
                [re useResource:ringBuf usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
            fprintf(stderr, "[tri] RING useResource Read|Write (vertex|fragment) declared on the render encoder\n");
        }
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (dumpDir) dump_phase("draw");
    }
    [re endEncoding];
    if (dumpDir) {
        dump_phase("end");
        state([[NSString stringWithFormat:@"dumped mode=%@ (never committed)", mode] UTF8String]);
        printf("TRI-DUMP: mode=%s inject=%d done (never committed)\n", mode.UTF8String, gInjected);
        return 0;   // cb is released uncommitted: no submission
    }
    printf("committing (buffers stay alive; poll %s/tri.state)\n", kStateDir); fflush(stdout);
    state("committed");
    [cb commit];

    for (int i = 0; i < 3000; i++) {
        if (cb.status == MTLCommandBufferStatusCompleted || cb.status == MTLCommandBufferStatusError) break;
        usleep(100000);
    }
    long st = (long)cb.status;
    printf("status: %ld\n", st);
    if (cb.error) printf("error: %s\n", cb.error.localizedDescription.UTF8String);

    // ---- the verdict, one TRI: line -------------------------------------
    static uint32_t readback[W * H];
    if (rb) memcpy(readback, rb.contents, sizeof readback);
    else [tex getBytes:readback bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    const uint32_t *px = readback;
    #define AT(x,y) px[(y) * W + (x)]
    uint32_t centre = AT(W/2, H/2);
    uint32_t c00 = AT(2,2), c10 = AT(W-3,2), c01 = AT(2,H-3), c11 = AT(W-3,H-3);
    uint32_t nClear = 0, nTri = 0, nSentinel = 0, nOther = 0;
    for (uint32_t i = 0; i < W*H; i++) {
        uint32_t v = px[i];
        if (v == kClearBGRA) nClear++;
        else if (doDraw ? is_draw_colour(v) : (v == kTriBGRA)) nTri++;
        else if (v == 0x77777777u) nSentinel++;
        else nOther++;
    }

    if (!doTri && !doDraw) {
        int pass = (nClear == W*H);
        printf("TRI: mode=clear storage=%s status=%ld all=0x%08x clear=%u/%u sentinel=%u other=%u verdict=%s\n",
               storage, st, centre, nClear, W*H, nSentinel, nOther, pass ? "PASS" : "FAIL");
        state(pass ? "done verdict=PASS mode=clear" : "done verdict=FAIL mode=clear");
        return 0;
    }
    int inside  = doDraw ? is_draw_colour(centre) : (centre == kTriBGRA);
    int outside = (c00 == kClearBGRA && c10 == kClearBGRA && c01 == kClearBGRA && c11 == kClearBGRA);
    int pass = inside && outside && nTri > 0 && nClear > 0 && nOther == 0 && nSentinel == 0;
    printf("TRI: mode=%s storage=%s status=%ld inject=%d centre=0x%08x corners=%08x/%08x/%08x/%08x inside=%s outside=%s "
           "counts %s=%u tri=%u sentinel=%u other=%u verdict=%s\n",
           mode.UTF8String, storage, st, gInjected, centre, c00, c10, c01, c11, inside ? "OK" : "MISS",
           outside ? "OK" : "BAD", doDraw ? "prefill" : "blue", nClear, nTri, nSentinel, nOther, pass ? "PASS" : "FAIL");
    if (nOther) {   // the first few foreign colours, for the failure signature
        uint32_t shown = 0;
        for (uint32_t i = 0; i < W*H && shown < 6; i++) {
            uint32_t v = px[i];
            if (v == kClearBGRA || v == 0x77777777u || (doDraw ? is_draw_colour(v) : v == kTriBGRA)) continue;
            printf("TRI-OTHER: pixel (%u,%u) = 0x%08x\n", i % W, i / W, v); shown++;
        }
    }
    // 0.0.215: the evidence files, written AFTER the TRI: line so a write failure cannot hide the verdict.
    // tri-target.ppm = the 64x64 readback as binary PPM (RGB from the BGRA8 memory order); tri-target.bgra = the
    // raw 16384 bytes; tri-vb-page.bin = the vertex buffer's first 4 KiB host page (draw mode, page-aligned only:
    // the instrument VS route of review report 8 stores at +0x800..+0xfff of that page).
    {
        const uint8_t *cb8 = (const uint8_t *)&px[W/2 + (H/2) * W];
        printf("TRI-PIXEL: centre (%u,%u) bytes %02x %02x %02x %02x (B G R A)\n", W/2, H/2, cb8[0], cb8[1], cb8[2], cb8[3]);
        char path[512];
        snprintf(path, sizeof path, "%s/tri-target.ppm", kStateDir);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", W, H);
            for (uint32_t i = 0; i < W * H; i++) {
                const uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/tri-target.bgra", kStateDir);
        f = fopen(path, "wb");
        if (f) { fwrite(px, 4, W * H, f); fclose(f); }
        int vbDumped = 0;
        if (doDraw && vb && ((uintptr_t)vb.contents & 0xfffu) == 0u) {
            snprintf(path, sizeof path, "%s/tri-vb-page.bin", kStateDir);
            f = fopen(path, "wb");
            if (f) { fwrite(vb.contents, 1, 4096, f); fclose(f); vbDumped = 1; }
        }
        printf("TRI-FILES: tri-target.ppm tri-target.bgra%s in %s\n", vbDumped ? " tri-vb-page.bin" : "", kStateDir);
    }
    // 0.0.217 : did the geometry engine write its rings? Counted from this process's own mapping of the
    // Shared ring buffer (zero-filled before commit), with the first 64 KiB of each ring saved.
    if (ringBuf) {
        const uint8_t *rp8 = (const uint8_t *)ringBuf.contents;
        const uint64_t lim[4] = { ringOff, ringOff + XLAT12_GE_RING_POS_OFF, ringOff + XLAT12_GE_RING_PRIM_OFF,
                                  ringOff + XLAT12_GE_RING_TOTAL };
        uint64_t nz[3] = { 0, 0, 0 }, first[3] = { ~0ull, ~0ull, ~0ull }, nzAll = 0;
        for (int r = 0; r < 3; r++)
            for (uint64_t i = lim[r]; i < lim[r + 1]; i++)
                if (rp8[i]) { nz[r]++; if (first[r] == ~0ull) first[r] = i - lim[r]; }
        for (uint64_t i = 0; i < (uint64_t)ringBuf.length; i++) if (rp8[i]) nzAll++;
        printf("TRI-RING: base 0x%llx nonzero bytes attr %llu pos %llu prim %llu (whole buffer %llu of %lu); first nonzero "
               "offset attr 0x%llx pos 0x%llx prim 0x%llx (0xffffffffffffffff = none)\n", (unsigned long long)ringBase,
               (unsigned long long)nz[0], (unsigned long long)nz[1], (unsigned long long)nz[2], (unsigned long long)nzAll,
               (unsigned long)ringBuf.length, (unsigned long long)first[0], (unsigned long long)first[1],
               (unsigned long long)first[2]);
        static const char *rn[3] = { "tri-ring-attr.bin", "tri-ring-pos.bin", "tri-ring-prim.bin" };
        for (int r = 0; r < 3; r++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", kStateDir, rn[r]);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(rp8 + lim[r], 1, 0x10000, f); fclose(f); }
        }
    }
    char sbuf[96]; snprintf(sbuf, sizeof sbuf, "done verdict=%s mode=%s", pass ? "PASS" : "FAIL", mode.UTF8String);
    state(sbuf);
    return 0;
} }
