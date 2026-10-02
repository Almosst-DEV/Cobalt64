/*
 * g2capture — Milestone 2 G2 capture tool (design notes/MILESTONE2-DESIGN.md §Q2).
 *
 * x86_64 Objective-C command-line program. It runs LATER ON THE PC (it cannot run on
 * the arm64 host Mac — build and static-check only here). It captures Apple's gfx10.3
 * shader container and derived HW records for SkyLight's UberCompositeVertex +
 * SimpleTextureFragment, USERSPACE ONLY, with NO GPU submission.
 *
 * How it works (all confirmed against AMDRadeonX6000MTLDriver, macOS 26.6.2 build 25G83):
 *  1. Picks Apple's AMD Metal device among MTLCopyAllDevices by registryID (default
 *     0x100000695, APPLE-DRIVER-VERDICT) or by name; refuses to run on any other
 *     device (never Apple silicon).
 *  2. Swizzles -[GFX10_GfxMtlFunctionVariant
 *       initWithCompilerOutput:shaderType:device:pipelineStatisticsOutput:]
 *     (unslid 0x7ffb11191a7a) resolved through the ObjC runtime. It logs the live IMP
 *     and checks it against the design address with the shared-cache slide applied,
 *     refusing on a mismatch (override with --expect-off for a different OS build).
 *  3. Builds ONE render pipeline (or, in --triangle mode, a trivial embedded-MSL
 *     pipeline) and lets Apple's signed libSC compile it. That is the only GPU-facing
 *     work; see the README for exactly which kernel calls pipeline creation makes.
 *  4. In the swizzle it dumps compilerOutput (the dispatch_data holding
 *     GFX10_PackedBinaryRec), and after the real init returns it dumps m_members and
 *     the derived HW shader record (which begins with GFX10_HwShaderCommonRec), with a
 *     JSON manifest (sizes, offsets, function names, slide, IMP, record header fields).
 *
 * It NEVER creates a command buffer, encoder, or submits anything.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dispatch/dispatch.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <stdatomic.h>

/* ---- design constants (build 25G83) ------------------------------------- */
static const uintptr_t kTextVmaddrBase = 0x7ffb110d2000ULL; /* __TEXT vmaddr, otool -l */
static uintptr_t       gExpectInitOff   = 0x11191a7a - 0x110d2000; /* variant init - text base */
static const char     *kDriverLeaf      = "AMDRadeonX6000MTLDriver";
static const char     *kVariantClass    = "GFX10_GfxMtlFunctionVariant";
static SEL             gSel;             /* initWithCompilerOutput:shaderType:device:pipelineStatisticsOutput: */

/* GFX10_PackedBinaryRec field offsets (§B1-X/Y, notes/B1-COMPILER-BRIDGE.md) */
enum {
    PB_sizeA = 0x18, PB_offsetA = 0x20, PB_ciArrayOff = 0x70, PB_ciCount = 0x78,
    PB_sizeB = 0x80, PB_offsetB = 0x88, PB_relocOff = 0xa0, PB_relocBytes = 0xa8,
    PB_relocEnt = 0xac, PB_symOff = 0xb0, PB_symBytes = 0xb8, PB_symEnt = 0xbc,
    PB_strOff = 0xc0, PB_strSize = 0xc8, PB_flags = 0x1a0, PB_resourceUsage = 0x1a8,
    PB_ciStride = 0x3c0
};

typedef id (*init_imp_t)(id, SEL, id, unsigned int, id, id);
static init_imp_t   gOrigInit;
static NSString    *gOutDir;
static NSMutableArray *gManifest;       /* array of NSString JSON objects */
static _Atomic int  gCounter;
static NSString    *gPendingVName, *gPendingFName; /* labels for the next pipeline build */
static ptrdiff_t    gMembersOff = -1;   /* ivar offset of m_members */
static const char  *gMembersEnc;
static int          gHwInfoDumped;      /* NGG dump runs once (design §Q1 / G2 open point) */

static void note(NSString *s) { fprintf(stderr, "[g2] %s\n", s.UTF8String); }

static void write_blob(NSString *name, const void *bytes, size_t len)
{
    NSString *path = [gOutDir stringByAppendingPathComponent:name];
    NSData *d = [NSData dataWithBytesNoCopy:(void *)bytes length:len freeWhenDone:NO];
    NSError *e = nil;
    if (![d writeToFile:path options:NSDataWritingAtomic error:&e])
        note([NSString stringWithFormat:@"WRITE FAILED %@: %@", name, e]);
    else
        note([NSString stringWithFormat:@"wrote %@ (%zu bytes)", name, len]);
}

static uint64_t rd64(const uint8_t *p, size_t len, size_t off)
{ uint64_t v = 0; if (off + 8 <= len) memcpy(&v, p + off, 8); return v; }
static uint32_t rd32(const uint8_t *p, size_t len, size_t off)
{ uint32_t v = 0; if (off + 4 <= len) memcpy(&v, p + off, 4); return v; }

/* Dump the device's GFX10_DeviceMembersRec/GFX10_HwInfoRec and read the NGG-vs-legacy-VS bit that
 * the packed shader records do NOT carry (design §Q1; G2 left this UNSETTLED). The bit is
 * HwInfoRec+0xcc bit 7 — the exact byte amdMtl_GFX10_WriteRenderPipelineHwCtxRegs tests with
 * `testb $-0x80, 0xcc(%rdi)` before programming the legacy-VS slot (mtldriver.dis 0x7ffb111da955).
 * GFX10_MtlDevice.m_members is an inline GFX10_DeviceMembersRec whose first member is the HwInfoRec,
 * so HwInfoRec begins at the m_members ivar offset. Read-only; writes nothing into Apple's object. */
static void dump_hwinfo(id device)
{
    if (gHwInfoDumped) return;
    gHwInfoDumped = 1;
    Class cls = object_getClass(device);
    Ivar iv = NULL;
    for (Class c = cls; c && !iv; c = class_getSuperclass(c))
        iv = class_getInstanceVariable(c, "m_members");
    if (!iv) { note([NSString stringWithFormat:@"NGG: no m_members ivar on %s - cannot read HwInfoRec",
                     class_getName(cls)]); return; }
    ptrdiff_t off = ivar_getOffset(iv);
    const uint8_t *hw = (const uint8_t *)(__bridge const void *)device + off;
    write_blob(@"device_hwinfo.bin", hw, 0x400);
    uint8_t b = hw[0xcc];
    note([NSString stringWithFormat:@"NGG: %s m_members@%td ; HwInfoRec+0xcc = 0x%02x -> bit7 (NGG) = %d "
          "(1 = NGG vertex path, legacy-VS slot programmed; 0 = legacy VS). device_hwinfo.bin holds 0x400 bytes",
          class_getName(cls), off, b, (b >> 7) & 1]);
    NSString *e = [NSString stringWithFormat:
        @"{\"device_class\":\"%s\",\"m_members_off\":%td,\"hwinfo_cc\":%u,\"ngg_bit7\":%d,\"file\":\"device_hwinfo.bin\"}",
        class_getName(cls), off, b, (b >> 7) & 1];
    @synchronized (gManifest) { [gManifest addObject:e]; }
}

/* The swizzled method: call the real init, then dump inputs and derived records. */
static id hook_init(id self, SEL _cmd, id compilerOutput, unsigned int shaderType, id device, id pstats)
{
    int idx = atomic_fetch_add(&gCounter, 1);
    const char *stageName = (shaderType == 1) ? "vertex" : (shaderType == 0) ? "fragment" : "other";
    NSString *label = (shaderType == 1) ? (gPendingVName ?: @"vertex")
                    : (shaderType == 0) ? (gPendingFName ?: @"fragment") : @"other";

    /* 1) compilerOutput bytes = the gfx10.3 shader container (dispatch_data). */
    size_t coLen = 0; const void *coPtr = NULL; dispatch_data_t mapped = NULL;
    if (compilerOutput) {
        mapped = dispatch_data_create_map((dispatch_data_t)compilerOutput, &coPtr, &coLen);
    }
    NSString *coFile = [NSString stringWithFormat:@"%02d_%s_%@_compileroutput.bin", idx, stageName, label];
    if (coPtr && coLen) write_blob(coFile, coPtr, coLen);

    const uint8_t *pb = (const uint8_t *)coPtr;
    uint64_t sizeA = rd64(pb, coLen, PB_sizeA), offsetA = rd64(pb, coLen, PB_offsetA);
    uint64_t sizeB = rd64(pb, coLen, PB_sizeB), offsetB = rd64(pb, coLen, PB_offsetB);
    uint64_t ciOff = rd64(pb, coLen, PB_ciArrayOff), ciCnt = rd64(pb, coLen, PB_ciCount);

    /* dump the device's HwInfoRec + the NGG bit once (design §Q1, G2 open point). */
    if (device) dump_hwinfo(device);

    /* 2) call the real initializer (no submission of any kind). */
    id result = gOrigInit(self, _cmd, compilerOutput, shaderType, device, pstats);

    /* 3) after init: m_members and the derived HW shader record (GFX10_HwShaderCommonRec first). */
    size_t instSize = class_getInstanceSize(objc_getClass(kVariantClass));
    size_t memLen = (gMembersOff >= 0 && (size_t)gMembersOff < instSize) ? instSize - (size_t)gMembersOff : 0;
    const void *memPtr = (gMembersOff >= 0) ? (const void *)((__bridge const void *)result + (size_t)gMembersOff) : NULL;
    if (memPtr && memLen) {
        write_blob([NSString stringWithFormat:@"%02d_%s_%@_members.bin", idx, stageName, label], memPtr, memLen);
    }
    uintptr_t shaderPtr = (memPtr && memLen >= sizeof(void *)) ? *(const uintptr_t *)memPtr : 0;
    size_t shDump = 0x400;
    if (shaderPtr > 0x1000) {
        write_blob([NSString stringWithFormat:@"%02d_%s_%@_hwshader.bin", idx, stageName, label],
                   (const void *)shaderPtr, shDump);
    }

    NSString *entry = [NSString stringWithFormat:
        @"{\"index\":%d,\"stage\":\"%s\",\"shaderType\":%u,\"label\":\"%@\","
         "\"compilerOutput\":{\"file\":\"%@\",\"len\":%zu,"
         "\"sizeA\":%llu,\"offsetA\":%llu,\"sizeB\":%llu,\"offsetB\":%llu,"
         "\"compileInfoOffset\":%llu,\"compileInfoCount\":%llu,\"flags\":%u,\"resourceUsage\":%u},"
         "\"members\":{\"offset\":%td,\"len\":%zu,\"encoding\":\"%s\"},"
         "\"hwShaderPtr\":\"0x%lx\",\"hwShaderDump\":%zu}",
        idx, stageName, shaderType, label, coFile, coLen,
        (unsigned long long)sizeA, (unsigned long long)offsetA,
        (unsigned long long)sizeB, (unsigned long long)offsetB,
        (unsigned long long)ciOff, (unsigned long long)ciCnt,
        rd32(pb, coLen, PB_flags), rd32(pb, coLen, PB_resourceUsage),
        gMembersOff, memLen, gMembersEnc ? gMembersEnc : "?",
        (unsigned long)shaderPtr, (shaderPtr > 0x1000) ? shDump : (size_t)0];
    @synchronized (gManifest) { [gManifest addObject:entry]; }

    if (mapped) mapped = nil;   /* releases the mapping under ARC */
    return result;
}

static int install_swizzle(BOOL enforce)
{
    Class cls = objc_getClass(kVariantClass);
    if (!cls) { note(@"class GFX10_GfxMtlFunctionVariant not found — is the AMD driver loaded?"); return 1; }
    Ivar iv = class_getInstanceVariable(cls, "m_members");
    if (iv) { gMembersOff = ivar_getOffset(iv); gMembersEnc = ivar_getTypeEncoding(iv); }
    Method m = class_getInstanceMethod(cls, gSel);
    if (!m) { note(@"selector initWithCompilerOutput:… not found on the class"); return 1; }
    IMP orig = method_getImplementation(m);

    Dl_info info; memset(&info, 0, sizeof info);
    if (!dladdr((const void *)orig, &info) || !info.dli_fbase) { note(@"dladdr on the IMP failed"); return 1; }
    uintptr_t slide = (uintptr_t)info.dli_fbase - kTextVmaddrBase;
    uintptr_t expect = (uintptr_t)info.dli_fbase + gExpectInitOff;
    const char *leaf = strrchr(info.dli_fname ? info.dli_fname : "", '/');
    leaf = leaf ? leaf + 1 : (info.dli_fname ? info.dli_fname : "?");
    note([NSString stringWithFormat:@"IMP=%p image=%s fbase=%p slide=0x%lx expect=%p m_members@%td",
          (void *)orig, leaf, info.dli_fbase, (unsigned long)slide, (void *)expect, gMembersOff]);
    int okleaf = (strcmp(leaf, kDriverLeaf) == 0);
    int okaddr = ((uintptr_t)orig == expect);
    if (!okleaf || !okaddr) {
        note([NSString stringWithFormat:@"IMP CHECK %@: image %s, address %s",
              enforce ? @"FAILED (refusing; pass --expect-off <hex> for a different build)" : @"MISMATCH (continuing: --allow-imp-mismatch)",
              okleaf ? "ok" : "WRONG", okaddr ? "ok" : "WRONG"]);
        if (enforce) return 2;
    }
    gOrigInit = (init_imp_t)orig;
    method_setImplementation(m, (IMP)hook_init);
    note(@"swizzle installed on GFX10_GfxMtlFunctionVariant");
    return 0;
}

static id<MTLDevice> pick_amd_device(uint64_t wantReg, BOOL anyAMD)
{
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    note([NSString stringWithFormat:@"MTLCopyAllDevices: %lu device(s)", (unsigned long)all.count]);
    id<MTLDevice> pick = nil;
    for (id<MTLDevice> d in all) {
        NSString *nm = d.name ?: @"?";
        BOOL isApple = [nm localizedCaseInsensitiveContainsString:@"Apple"];
        BOOL isRadeon = [nm localizedCaseInsensitiveContainsString:@"Radeon"] ||
                        [nm localizedCaseInsensitiveContainsString:@"GFX10"];
        note([NSString stringWithFormat:@"  device: %@ (registryID=0x%llx headless=%d removable=%d)",
              nm, (unsigned long long)d.registryID, d.isHeadless, d.isRemovable]);
        BOOL match = (wantReg && d.registryID == wantReg) ||
                     (!wantReg && anyAMD && isRadeon && !isApple);
        if (match && !pick) pick = d;
    }
    return pick;
}

/* All bool function constants of f set to NO (SkyLight's shaders declare every constant required). */
static MTLFunctionConstantValues *bools_off(id<MTLFunction> f)
{
    MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
    for (NSString *k in f.functionConstantsDictionary) {
        MTLFunctionConstant *c = f.functionConstantsDictionary[k];
        if (c.type == MTLDataTypeBool) { BOOL no = NO; [fc setConstantValue:&no type:MTLDataTypeBool atIndex:c.index]; }
    }
    return fc;
}

/* A vertex descriptor from the specialised vertex function's own reflection: each attribute in
 * buffer 0, packed in attribute order. Returns nil when the function has no stage-in attributes. */
static MTLVertexDescriptor *reflected_vd(id<MTLFunction> v)
{
    if (v.vertexAttributes.count == 0) return nil;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    NSUInteger off = 0;
    for (MTLVertexAttribute *a in v.vertexAttributes) {
        MTLVertexFormat fmt = MTLVertexFormatFloat4; NSUInteger sz = 16;
        switch (a.attributeType) {
            case MTLDataTypeFloat:  fmt = MTLVertexFormatFloat;  sz = 4;  break;
            case MTLDataTypeFloat2: fmt = MTLVertexFormatFloat2; sz = 8;  break;
            case MTLDataTypeFloat3: fmt = MTLVertexFormatFloat3; sz = 12; break;
            case MTLDataTypeFloat4: fmt = MTLVertexFormatFloat4; sz = 16; break;
            case MTLDataTypeUInt:   fmt = MTLVertexFormatUInt;   sz = 4;  break;
            case MTLDataTypeInt:    fmt = MTLVertexFormatInt;    sz = 4;  break;
            default: break;
        }
        vd.attributes[a.attributeIndex].format = fmt;
        vd.attributes[a.attributeIndex].offset = off;
        vd.attributes[a.attributeIndex].bufferIndex = 0;
        note([NSString stringWithFormat:@"  vertex attribute %@ [%lu] type %lu at +%lu", a.name,
              (unsigned long)a.attributeIndex, (unsigned long)a.attributeType, (unsigned long)off]);
        off += sz;
    }
    vd.layouts[0].stride = off;
    return vd;
}

/* Build one SkyLight render pipeline from `vname` + `fname`. No submission.
 * : the original pairing UberCompositeVertex + SimpleTextureFragment does
 * not link ("fragment input tex was not found in vertex shader outputs", on the PC and on the host Mac); SkyLight's
 * compositor pair is UberCompositeVertex + UberCompositeFragment, and SimpleTextureFragment's partner is
 * SimpleVertex. Every bool function constant is set to NO and the vertex descriptor comes from reflection. */
static int run_skylight(id<MTLDevice> dev, NSString *metallibPath, NSString *vname, NSString *fname)
{
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:metallibPath] error:&e];
    if (!lib) { note([NSString stringWithFormat:@"newLibraryWithURL failed: %@", e]); return 1; }
    note([NSString stringWithFormat:@"loaded %@ (%lu functions)", metallibPath, (unsigned long)lib.functionNames.count]);

    gPendingVName = vname; gPendingFName = fname;
    id<MTLFunction> vbase = [lib newFunctionWithName:vname];
    id<MTLFunction> fbase = [lib newFunctionWithName:fname];
    if (!vbase || !fbase) { note([NSString stringWithFormat:@"function not found: %@ %@", vbase ? @"" : vname, fbase ? @"" : fname]); return 1; }
    id<MTLFunction> vfn = [lib newFunctionWithName:vname constantValues:bools_off(vbase) error:&e];
    if (!vfn) { note([NSString stringWithFormat:@"vertex function failed: %@", e]); return 1; }
    id<MTLFunction> ffn = [lib newFunctionWithName:fname constantValues:bools_off(fbase) error:&e];
    if (!ffn) { note([NSString stringWithFormat:@"fragment function failed: %@", e]); return 1; }
    note([NSString stringWithFormat:@"pair %@ (%lu constants) + %@ (%lu constants), all bool constants NO", vname,
          (unsigned long)vbase.functionConstantsDictionary.count, fname, (unsigned long)fbase.functionConstantsDictionary.count]);

    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = vfn; rpd.fragmentFunction = ffn;
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    rpd.vertexDescriptor = reflected_vd(vfn);

    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rpd error:&e];
    if (!ps) { note([NSString stringWithFormat:@"newRenderPipelineState failed: %@", e]); return 1; }
    note(@"render pipeline state built (no command buffer, no submission)");
    return 0;
}

/* Build one trivial triangle pipeline from embedded MSL (milestone-2 use). No submission. */
static int run_triangle(id<MTLDevice> dev)
{
    static NSString *src =
      @"#include <metal_stdlib>\n using namespace metal;\n"
      @"struct VOut { float4 pos [[position]]; float4 col; };\n"
      @"vertex VOut g2_tri_vs(uint vid [[vertex_id]]) {\n"
      @"  float2 p[3] = { float2(0,0.6), float2(-0.6,-0.6), float2(0.6,-0.6) };\n"
      @"  float3 c[3] = { float3(1,0,0), float3(0,1,0), float3(0,0,1) };\n"
      @"  VOut o; o.pos = float4(p[vid],0,1); o.col = float4(c[vid],1); return o; }\n"
      @"fragment float4 g2_tri_fs(VOut in [[stage_in]]) { return in.col; }\n";
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
    if (!lib) { note([NSString stringWithFormat:@"MSL compile failed: %@", e]); return 1; }
    gPendingVName = @"g2_tri_vs"; gPendingFName = @"g2_tri_fs";
    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [lib newFunctionWithName:@"g2_tri_vs"];
    rpd.fragmentFunction = [lib newFunctionWithName:@"g2_tri_fs"];
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rpd error:&e];
    if (!ps) { note([NSString stringWithFormat:@"newRenderPipelineState failed: %@", e]); return 1; }
    note(@"triangle pipeline state built (no submission)");
    return 0;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        gSel = @selector(initWithCompilerOutput:shaderType:device:pipelineStatisticsOutput:);
        gManifest = [NSMutableArray array];
        gOutDir = @"./g2out";
        NSString *metallib = @"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib";
        uint64_t wantReg = 0x100000695ULL; BOOL anyAMD = NO, triangle = NO, enforce = YES;
        NSString *vname = @"UberCompositeVertex", *fname = @"UberCompositeFragment";

        for (int i = 1; i < argc; i++) {
            NSString *a = @(argv[i]);
            if ([a isEqual:@"--triangle"]) triangle = YES;
            else if ([a isEqual:@"--any-amd"]) { anyAMD = YES; wantReg = 0; }
            else if ([a isEqual:@"--allow-imp-mismatch"]) enforce = NO;
            else if ([a isEqual:@"-o"] && i + 1 < argc) gOutDir = @(argv[++i]);
            else if ([a isEqual:@"--regid"] && i + 1 < argc) wantReg = strtoull(argv[++i], NULL, 0);
            else if ([a isEqual:@"--metallib"] && i + 1 < argc) metallib = @(argv[++i]);
            else if ([a isEqual:@"--expect-off"] && i + 1 < argc) gExpectInitOff = strtoull(argv[++i], NULL, 0);
            else if ([a isEqual:@"--vertex"] && i + 1 < argc) vname = @(argv[++i]);
            else if ([a isEqual:@"--fragment"] && i + 1 < argc) fname = @(argv[++i]);
            else { fprintf(stderr, "usage: g2capture [--triangle] [--any-amd|--regid 0x..] [--metallib PATH]\n"
                                   "                 [-o OUTDIR] [--allow-imp-mismatch] [--expect-off HEX]\n"
                                   "                 [--vertex NAME] [--fragment NAME]   (default UberCompositeVertex + UberCompositeFragment)\n"); return 2; }
        }

        [[NSFileManager defaultManager] createDirectoryAtPath:gOutDir withIntermediateDirectories:YES attributes:nil error:nil];

        id<MTLDevice> dev = pick_amd_device(wantReg, anyAMD);
        if (!dev) {
            note(@"no matching AMD device (this tool refuses to run on any other device, incl. Apple silicon).");
            note(@"On the arm64 host Mac there is no such device; run on the PC. Try --any-amd or --regid.");
            return 3;
        }
        note([NSString stringWithFormat:@"using device: %@ (registryID=0x%llx)", dev.name, (unsigned long long)dev.registryID]);

        int rc = install_swizzle(enforce);
        if (rc) return rc;

        rc = triangle ? run_triangle(dev) : run_skylight(dev, metallib, vname, fname);

        NSString *man = [NSString stringWithFormat:@"{\"driver_leaf\":\"%s\",\"expect_init_off\":\"0x%lx\",\"captures\":[%@]}\n",
                         kDriverLeaf, (unsigned long)gExpectInitOff, [gManifest componentsJoinedByString:@",\n"]];
        [man writeToFile:[gOutDir stringByAppendingPathComponent:@"manifest.json"]
             atomically:YES encoding:NSUTF8StringEncoding error:nil];
        note([NSString stringWithFormat:@"manifest: %lu capture(s) -> %@/manifest.json", (unsigned long)gManifest.count, gOutDir]);
        return rc;
    }
}
