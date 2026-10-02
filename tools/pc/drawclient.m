// drawclient.m — THE FORCED-DRAW CLIENT for the context latch's proving run (notes/M4-CONTEXT-LATCH.md, run hp3).
//
// hp2 died because a client drew while the boot chain was still arming its protections; hp1 lived because nobody did. A proof
// cannot wait for the login panel to feel like drawing, so this client MAKES the draw happen at the worst moment:
//
//   1. started BEFORE `fire` (accel-run.sh `prefire-drawclient`), it polls IOKit every 50 ms for an IOAccelerator that
//      carries a MetalPluginName, WITHOUT touching Metal (Metal may cache an empty device list for the process);
//   2. the instant one appears it asks Metal for the device and times that call: on a latched kext the call blocks in
//      createVMContext until the latch opens (or returns nil if the latch refuses, and the client says so);
//   3. from the moment it has a device, every 50 ms for DURATION s: a NEW shared buffer (a mapVA: page-table SDMA work on
//      chan 14), a blit (buffer -> buffer) and a draw (a render pass into a 64x64 texture, a triangle if the pipeline
//      compiles, clear-only if it does not). Commits never block: at most 16 command buffers in flight.
//
// Every line is stamped with mach uptime in microseconds - the SAME clock the kext stamps boot-chain and ctx-latch lines
// with (clock_get_uptime -> absolutetime_to_nanoseconds), so the two logs interleave exactly.
//
// usage: drawclient [duration_s=60] [period_ms=50] [state_file=/tmp/navi48-staging/drawclient.state]
//        [wait_max_s=180: give up if no accelerator appears]
// Build (host Mac, cross): see tools/stage-to-pc.sh. Judge-only runs: its GFX is dropped at the source or neutered like every frame.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static mach_timebase_info_data_t gTb;
static uint64_t up_us(void) { return mach_absolute_time() * gTb.numer / gTb.denom / 1000ull; }
#define SAY(fmt, ...) do { uint64_t _t = up_us(); printf("drawclient: [up %llu.%06llu s] " fmt "\n", \
    (unsigned long long)(_t / 1000000ull), (unsigned long long)(_t % 1000000ull), ##__VA_ARGS__); fflush(stdout); } while (0)

static atomic_ullong gCompleted, gErrored, gInflight;   // written by Metal's completion handlers
static const char *gState = "/tmp/navi48-staging/drawclient.state";
static void state(const char *s) { FILE *f = fopen(gState, "w"); if (f) { fprintf(f, "%s\n", s); fclose(f); } }

// Does an IOAccelerator with a Metal plugin exist? IOKit only - no Metal call, no user client opened.
static int accelerator_present(char *name, size_t n) {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return 0;
    int found = 0;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        CFTypeRef p = IORegistryEntryCreateCFProperty(o, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
        if (p) {
            found = 1;
            io_name_t cls = { 0 };
            IOObjectGetClass(o, cls);
            snprintf(name, n, "%s", cls);
            CFRelease(p);
        }
        IOObjectRelease(o);
        if (found) break;
    }
    IOObjectRelease(it);
    return found;
}

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint i [[vertex_id]]) { float2 q[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) }; V v; v.p = float4(q[i],0,1); return v; }\n"
     "fragment float4 fs(V v [[stage_in]]) { return float4(1,0,1,1); }\n";

int main(int argc, char **argv) { @autoreleasepool {
    mach_timebase_info(&gTb);
    const int duration = argc > 1 ? atoi(argv[1]) : 60;
    const int period = argc > 2 ? atoi(argv[2]) : 50;
    if (argc > 3) gState = argv[3];
    const int waitMax = argc > 4 ? atoi(argv[4]) : 180;
    if (duration <= 0 || period <= 0) { fprintf(stderr, "usage: drawclient [duration_s] [period_ms] [state] [wait_max_s]\n"); return 2; }

    state("polling");
    SAY("start (pid %d): polling IOKit every %d ms for an IOAccelerator with a MetalPluginName; NO Metal call until it appears",
        getpid(), period);
    char cls[128] = { 0 };
    const uint64_t tPoll0 = up_us();
    uint64_t polls = 0;
    while (!accelerator_present(cls, sizeof cls)) {
        polls++;
        if (up_us() - tPoll0 > (uint64_t)waitMax * 1000000ull) {
            SAY("GAVE UP: no accelerator in %d s (%llu polls) - nothing was opened", waitMax, (unsigned long long)polls);
            state("gave-up");
            return 3;
        }
        usleep((useconds_t)period * 1000u);
    }
    SAY("ACCELERATOR APPEARED: %s (after %llu polls). Asking Metal for the device NOW - on a latched kext this call waits in "
        "createVMContext until every protection is live", cls, (unsigned long long)polls);
    state("device-call");

    id<MTLDevice> d = nil;
    uint64_t nilCalls = 0;
    const uint64_t tAppear = up_us();
    while (!d) {
        const uint64_t t0 = up_us();
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        d = all.count ? all[0] : nil;
        const uint64_t t1 = up_us();
        if (d) {
            SAY("DEVICE OBTAINED: \"%s\" - the call took %llu us (%llu nil call(s) before it)", d.name.UTF8String,
                (unsigned long long)(t1 - t0), (unsigned long long)nilCalls);
            break;
        }
        nilCalls++;
        if (nilCalls <= 8 || nilCalls % 100 == 0)
            SAY("device call #%llu returned NO DEVICE after %llu us (a latch refusal returns NULL from createVMContext; "
                "Metal then has no device)", (unsigned long long)nilCalls, (unsigned long long)(t1 - t0));
        if (t1 - tAppear > (uint64_t)waitMax * 1000000ull) {
            SAY("GAVE UP: the accelerator exists but no device after %d s (%llu nil calls)", waitMax, (unsigned long long)nilCalls);
            state("no-device");
            return 4;
        }
        usleep((useconds_t)period * 1000u);
    }

    // The queue's creation is also timed: some drivers open their shared user client here rather than at device creation.
    uint64_t t0 = up_us();
    id<MTLCommandQueue> q = [d newCommandQueue];
    SAY("command queue %s in %llu us", q ? "created" : "FAILED", (unsigned long long)(up_us() - t0));
    if (!q) { state("no-queue"); return 5; }
    t0 = up_us();
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = nil;
    id<MTLLibrary> lib = [d newLibraryWithSource:kSrc options:nil error:&err];
    if (lib) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        ps = [d newRenderPipelineStateWithDescriptor:pd error:&err];
    }
    SAY("pipeline %s in %llu us%s%s", ps ? "compiled" : "NOT available - draws are clear-only render passes",
        (unsigned long long)(up_us() - t0), err ? ": " : "", err ? err.localizedDescription.UTF8String : "");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [d newTextureWithDescriptor:td];
    id<MTLBuffer> dst = [d newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    if (!rt || !dst) { SAY("setup FAILED (texture %p, buffer %p)", (__bridge void *)rt, (__bridge void *)dst); state("setup-failed"); return 6; }

    state("drawing");
    SAY("DRAWING: every %d ms for %d s - new buffer (mapVA), blit, %s; at most 16 command buffers in flight", period, duration,
        ps ? "draw" : "clear");
    atomic_ullong *pIn = &gInflight;
    uint64_t iters = 0, committed = 0, skipped = 0, allocFail = 0;
    const uint64_t tStart = up_us(), tEnd = tStart + (uint64_t)duration * 1000000ull;
    uint64_t nextReport = tStart + 1000000ull;
    while (up_us() < tEnd) {
        @autoreleasepool {
            iters++;
            if (atomic_load(pIn) >= 16) { skipped++; }
            else {
                id<MTLBuffer> src = [d newBufferWithLength:65536 options:MTLResourceStorageModeShared];
                if (!src) allocFail++;
                else {
                    memset(src.contents, (int)(iters & 0xff), 65536);
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                    [be copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:65536];
                    [be endEncoding];
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake((iters & 1) ? 1 : 0, 0.5, 0, 1);
                    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                    if (ps) { [re setRenderPipelineState:ps]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }
                    [re endEncoding];
                    atomic_fetch_add(pIn, 1);
                    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                        if (c.status == MTLCommandBufferStatusError) atomic_fetch_add(&gErrored, 1);
                        else atomic_fetch_add(&gCompleted, 1);
                        atomic_fetch_sub(pIn, 1);
                    }];
                    [cb commit];
                    committed++;
                }
            }
        }
        const uint64_t now = up_us();
        if (now >= nextReport) {
            SAY("tick: iterations %llu, committed %llu, completed %llu, errored %llu, in flight %llu, skipped (backpressure) %llu, "
                "alloc failures %llu", (unsigned long long)iters, (unsigned long long)committed,
                (unsigned long long)atomic_load(&gCompleted), (unsigned long long)atomic_load(&gErrored),
                (unsigned long long)atomic_load(pIn), (unsigned long long)skipped, (unsigned long long)allocFail);
            nextReport += 1000000ull;
        }
        usleep((useconds_t)period * 1000u);
    }
    // Let what is in flight finish before the buffers go away (: never free a buffer a ring may still reference).
    const uint64_t tDrain = up_us();
    while (atomic_load(pIn) && up_us() - tDrain < 15000000ull) usleep(50000);
    char line[256];
    snprintf(line, sizeof line, "done iterations=%llu committed=%llu completed=%llu errored=%llu inflight=%llu skipped=%llu",
             (unsigned long long)iters, (unsigned long long)committed, (unsigned long long)atomic_load(&gCompleted),
             (unsigned long long)atomic_load(&gErrored), (unsigned long long)atomic_load(pIn), (unsigned long long)skipped);
    SAY("%s", line);
    state(line);
    if (atomic_load(pIn)) {
        // Still referenced by a ring: stay alive rather than free them. The recipe ends the run; the process is left running.
        SAY("%llu command buffer(s) STILL in flight after 15 s - staying alive so their buffers stay valid", (unsigned long long)atomic_load(pIn));
        for (;;) pause();
    }
    return 0;
} }
