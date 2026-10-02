// Navi48Device.m - Metal driver bundle for the Navi48 native stack (milestone #9, step 9c/9d).
// Spec: notes/design/NATIVE-S3.md section 4 and 5 (rows 9c/9d/9e); RE: NATIVE-S3-RE-KERNEL.md sections 1, 3.
//
// Superclass binding: MTLIOAccelDevice is an exported ObjC class of Metal.framework (the SDK's Metal.tbd lists
// it under objc-classes for x86_64-macos, and pvm.x86 - AppleParavirtGPUMetal - links _OBJC_CLASS_$_MTLIOAccelDevice
// the same way).  So the class is declared here WITHOUT ivars (non-fragile ABI: the subclass adds none, the real
// layout comes from Metal at load time) and linked at build time with -framework Metal.  No runtime class pair.
//
// Build-time switches (see build.sh):
//   N48_LAZY=1   supportLazyInitialization -> YES (9c/9d, default).  N48_LAZY=0 for 9e (base returns NO: CONFIRMED,
//                -[MTLIOAccelDevice supportLazyInitialization] at 0x7ff80f5a9cd3 is `xor eax,eax; ret`).
//   N48_9D=1     the 9d feature/limit queries (default).  N48_9D=0 leaves them to the MTLIOAccelDevice base.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#import <Metal/Metal.h>
#import <os/log.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#import <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "n48_spvrefl.h"
#include "n48_heapalloc.h"
#include "n48_plane.h"
#include "n48_fallback_spv.h"
#include "n48_hotswap.h"
#include "n48_scanabi.h"    // S5.2a: scanout ABI structs (verbatim copy of the Mesa header)
#include "n48_dispflip.h"   // S5.2a: the pure D-copy state machine
#include "n48_crc.h"        // native #12: opt-in per-frame CRC diagnostic (row sampling, accounting; host test: test-crc.c)
#include "n48_cblog.h"      // native #12 Stage 0b: pure cross-queue RAW-inversion bookkeeping (host test: test-cblog.c)
#include "n48_t1.h"        // native #12: T1 timing histograms + T2 pipeline-cache file format (host test: test-t1.c)
#include "n48_impcache.h"  // P4 import cache + classify cache decisions (host test: test-impcache.c)
#include "n48_pool.h"      // P1 memory pooling decisions (fence-gated deferred frees, 4 MiB slabs, recycle cache; host test: test-pool.c)
#include "n48_drawcache.h" // P5b per-draw redundancy: descriptor-set signatures, last-pipeline caches, render-pass cache (host test: test-drawcache.c)

#ifndef N48_LAZY
#define N48_LAZY 1
#endif
#ifndef N48_9D
#define N48_9D 1
#endif

// Forward declaration, deliberately without ivars.
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
- (void)lazyInitialize;
@end

// Metal's generic resource base (exported, F8). Declared without ivars for the same reason.
@interface _MTLResource : NSObject
@end

// Metal's generic heap base (exported; Apple-silicon chain AGXG16GFamilyHeap <- IOGPUMetalHeap <- _MTLHeap <- _MTLAllocation <- _MTLObjectWithLabel). No ivars declared.
@interface _MTLHeap : NSObject
@end

// Metal's generic queue / command buffer / encoder / pipeline bases (exported, F8). No ivars declared.
@interface _MTLCommandQueue : NSObject
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc;
- (void)commandBufferDidComplete:(id)cb startTime:(uint64_t)st completionTime:(uint64_t)ct error:(NSError *)err;
@end
@interface _MTLCommandBuffer : NSObject
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r;
- (void)didScheduleWithStartTime:(uint64_t)st endTime:(uint64_t)et error:(NSError *)err;
- (void)setCurrentCommandEncoder:(id)e;
- (void)commit;
@end
@interface _MTLCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end
@interface _MTLRenderPipelineState : NSObject
@end

@interface Navi48Device : MTLIOAccelDevice
@end

static void n48_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void n48_log(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    os_log(OS_LOG_DEFAULT, "Navi48Metal: %{public}s", buf);
    fprintf(stderr, "Navi48Metal: %s\n", buf);
}
#define N48LOG(fmt, ...) n48_log(fmt, ##__VA_ARGS__)
// #12 R5: per-command-buffer / per-encoder lines are rate limited per call site: the first 8, then every 1024th (errors and WARN lines stay N48LOG).
#define N48LOGR(fmt, ...) do { static _Atomic uint64_t n_; uint64_t k_ = atomic_fetch_add(&n_, 1); if (k_ < 8 || (k_ & 1023) == 0) n48_log(fmt, ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Load policy (NATIVE-S4-M11 11b, L1-L3). Every Metal-enumerating process loads this bundle; only WindowServer (or a root
// tool that opts in with N48M_ALLOW=1) may get a device. All test hooks are honoured only with N48M_ALLOW=1 as root.
//   /private/tmp/n48m-off              exists -> decline (L1 kill file)
//   /private/tmp/n48m-starts           one epoch-seconds line per WindowServer init; >= 3 within 300 s -> decline (L2)
//   nub property "Navi48,Ready" = 0    -> decline (L3); absent (old kexts) -> proceed
//   nub property "Navi48,AutoDisarmed" = 1 -> decline (#12); absent -> ignored
//   /private/tmp/n48m-headless-no      exists -> isHeadless NO (default YES); read once per process
//   /private/tmp/n48m-noplanes         exists -> P2: multi-plane IOSurfaces refused as before (default: plane p of a 2/3-plane surface is accepted); read once per process
//   /private/tmp/n48m-noflip           exists -> S5.2a D-copy present OFF for the process (the kernel v1 copy continues); read once per process
//   N48M_TEST_IGNORE_KILL=1            (root + N48M_ALLOW=1, NOT WindowServer) -> n48m-off is IGNORED in that process only (S5.2b test bypass)
//   N48M_TEST_FALLBACK_AS_WS=1         (root + N48M_ALLOW=1) -> a spvcache miss gives a FALLBACK pipeline as in WindowServer (hot-swap oracle: mtlprobe hotswap)
//   N48M_TEST_HIDE_BUNDLE_SPV=1        (root + N48M_ALLOW=1) -> the bundle's Resources/spvcache is NOT consulted (only the side directory)
//   N48M_TEST_SIDE_DIR=<dir>           (root + N48M_ALLOW=1) -> replaces the side spvcache directory /private/var/tmp/n48m-spv
//   N48M_TEST_DISPFLIP=1               (root + N48M_ALLOW=1) -> a surface may be accepted as a display surface without the CoreDisplay backtrace signal (mtlprobe dispflip sets it)
// ---------------------------------------------------------------------------------------------------------------
#define N48_KILL_FILE      "/private/tmp/n48m-off"
#define N48_STARTS_FILE    "/private/tmp/n48m-starts"
#define N48_OK_FILE        "/private/tmp/n48m-ok"
#define N48_HEADLESS_NO    "/private/tmp/n48m-headless-no"

static BOOL n48_is_ws(void) {
    static int v = -1;
    if (v < 0) { const char *p = getprogname(); v = (p && !strcmp(p, "WindowServer")) ? 1 : 0; }
    return v == 1;
}
static BOOL n48_allow(void) {
    const char *e = getenv("N48M_ALLOW");
    return e && !strcmp(e, "1") && geteuid() == 0;
}
static BOOL n48_force_fallback(void) {
    const char *e = getenv("N48M_FORCE_FALLBACK");
    return n48_allow() && e && !strcmp(e, "1");
}
// Test hook (root + N48M_ALLOW=1): this process takes the WindowServer branch of the fallback rule (a spvcache miss yields a fallback pipeline,
// not an error) while the lookup stays real, so the hot-swap can be exercised from mtlprobe.
static BOOL n48_test_fb_as_ws(void) {
    const char *e = getenv("N48M_TEST_FALLBACK_AS_WS");
    return n48_allow() && e && !strcmp(e, "1");
}
static BOOL n48_fallback_ok(void) { return n48_is_ws() || n48_force_fallback() || n48_test_fb_as_ws(); }

// Crash counter (#12 R3): counts only ABNORMAL ends. One line per instance start: "<epoch> <pid>". An instance that committed a
// first successful command buffer appends "<pid> <start epoch> <now>" to N48_OK_FILE (n48_mark_clean) and no longer counts: normal
// logouts, logins and restarts do not trip the rule; >= 3 instances within 300 s that never got clean still decline.
// Old-format lines (no pid) count as before. Paths are N48_STARTS_FILE / N48_OK_FILE, or (test hook, needs N48M_ALLOW)
// N48M_TEST_COUNTER=<path> (ok file <path>.ok), which also enables the counter outside WindowServer.
static char n48_ok_path[1100]; static long long n48_start_epoch; static _Atomic int n48_counter_on, n48_clean_done;
static BOOL n48_counter_tripped(int *countOut) {
    const char *path = N48_STARTS_FILE;
    const char *tp = getenv("N48M_TEST_COUNTER");
    if (n48_allow() && tp && tp[0]) { path = tp; snprintf(n48_ok_path, sizeof n48_ok_path, "%s.ok", tp); }
    else if (!n48_is_ws()) return NO;
    else snprintf(n48_ok_path, sizeof n48_ok_path, "%s", N48_OK_FILE);
    time_t now = time(NULL); int count = 1, cleaned = 0;
    NSMutableSet *clean = [NSMutableSet set];
    FILE *f = fopen(n48_ok_path, "r");
    if (f) { char line[96]; while (fgets(line, sizeof line, f)) { long long pid = 0, st = 0; if (sscanf(line, "%lld %lld", &pid, &st) == 2) [clean addObject:[NSString stringWithFormat:@"%lld/%lld", pid, st]]; } fclose(f); }
    f = fopen(path, "r");
    if (f) { char line[64]; while (fgets(line, sizeof line, f)) {
        long long t = 0, pid = 0; int n = sscanf(line, "%lld %lld", &t, &pid);
        if (n < 1 || t <= 0 || now - t < 0 || now - t > 300) continue;
        if (n == 2 && [clean containsObject:[NSString stringWithFormat:@"%lld/%lld", pid, t]]) { cleaned++; continue; }
        count++; }
        fclose(f); }
    n48_start_epoch = (long long)now;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { char b[48]; int n = snprintf(b, sizeof b, "%lld %d\n", (long long)now, (int)getpid()); (void)!write(fd, b, (size_t)n); close(fd); }
    else N48LOG("counter: cannot append to %s (errno %d)", path, errno);
    atomic_store(&n48_counter_on, 1);
    N48LOG("counter: %d abnormal start(s) in the last 300 s including this one (%d earlier instance(s) reached clean and are not counted)", count, cleaned);
    if (countOut) *countOut = count;
    return count >= 3;
}
// Called after the first successfully completed command buffer of an admitted instance that is counted.
static void n48_mark_clean(void) {
    if (!atomic_load(&n48_counter_on) || atomic_exchange(&n48_clean_done, 1)) return;
    int fd = open(n48_ok_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) { N48LOG("clean marker: cannot append to %s (errno %d)", n48_ok_path, errno); return; }
    char b[96]; int n = snprintf(b, sizeof b, "%d %lld %lld\n", (int)getpid(), n48_start_epoch, (long long)time(NULL)); (void)!write(fd, b, (size_t)n); close(fd);
    N48LOG("clean marker written to %s (first completed command buffer)", n48_ok_path);
}

// -1 = property absent, 0 = false/zero, 1 = true. Reads a numeric/boolean property from the accelerator's parent (the nub).
static int n48_nub_flag(uint32_t port, CFStringRef name) {
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(parent, name, kCFAllocatorDefault, 0);
    IOObjectRelease(parent);
    if (!v) return -1;
    int r = -1;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) { int x = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &x); r = x ? 1 : 0; }
    else if (CFGetTypeID(v) == CFBooleanGetTypeID()) r = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    CFRelease(v);
    return r;
}
// "Navi48,Ready": absent (old kexts) -> proceed. N48M_TEST_READY overrides.
static int n48_nub_ready(uint32_t port) {
    const char *th = getenv("N48M_TEST_READY");
    if (n48_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return n48_nub_flag(port, CFSTR("Navi48,Ready"));
}
// #12 R3: "Navi48,AutoDisarmed" = 1 (kernel disarmed the display pipe after a fault) -> decline; absent -> ignore. N48M_TEST_AUTODISARMED overrides.
static int n48_nub_autodisarmed(uint32_t port) {
    const char *th = getenv("N48M_TEST_AUTODISARMED");
    if (n48_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return n48_nub_flag(port, CFSTR("Navi48,AutoDisarmed"));
}

// NULL = admit, else the reason for declining (static string).
static const char *n48_admit(uint32_t port) {
    if (!n48_is_ws() && !n48_allow()) return "process is not WindowServer and N48M_ALLOW=1 (as root) is not set";
    struct stat st;
    if (stat(N48_KILL_FILE, &st) == 0) {
        // S5.2b root test path: ignored ONLY by a non-WindowServer root process with N48M_ALLOW=1 and N48M_TEST_IGNORE_KILL=1 (WindowServer never is).
        if (!n48df_off_file_ignored((int)geteuid(), getenv("N48M_ALLOW"), getenv("N48M_TEST_IGNORE_KILL"), n48_is_ws())) return "kill file " N48_KILL_FILE " exists";
        N48LOG("admit: kill file " N48_KILL_FILE " exists but is IGNORED (root test bypass N48M_ALLOW=1 + N48M_TEST_IGNORE_KILL=1, not WindowServer)");
    }
    if (n48_nub_ready(port) == 0) return "nub property Navi48,Ready is 0";
    if (n48_nub_autodisarmed(port) == 1) return "nub property Navi48,AutoDisarmed is 1";
    int count = 0;
    if (n48_counter_tripped(&count)) return "crash counter: >= 3 starts within 300 s";
    return NULL;
}
static BOOL n48_headless(void) {
    static BOOL v; static dispatch_once_t once;
    dispatch_once(&once, ^{ struct stat st; v = stat(N48_HEADLESS_NO, &st) != 0; });
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// Lazy in-process RADV (NATIVE-S4-M10 route a', F5). Opened on the first resource/queue request, NEVER in
// initWithAcceleratorPort: (every Metal-enumerating process loads this bundle). N48N is root-only and exclusive:
// a refusal (non-root, second client) becomes nil + NSError, never an abort.
// ---------------------------------------------------------------------------------------------------------------
#define N48_VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdDraw) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdCopyBufferToImage) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCreateComputePipelines) X(vkCmdSetBlendConstants) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkResetFences) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetImageSubresourceLayout) X(vkGetPhysicalDeviceProperties2) X(vkGetDeviceProcAddr) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData)
#define X(n) static PFN_##n n;
N48_VK_FUNCS(X)
#undef X
static PFN_vkCreateInstance n48_vkCreateInstance;
static PFN_vkGetInstanceProcAddr n48_gipa;
static PFN_vkGetMemoryHostPointerPropertiesEXT n48_vkGetMHPP;   // 11h.6: VK_EXT_external_memory_host

static struct {
    BOOL tried; BOOL ok; NSError *err;
    void *lib; VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp; pthread_mutex_t qlock; VkPhysicalDeviceLimits lim; VkCommandPool opool; VkPipelineCache pc;   // pc: T2 persisted pipeline cache (VK_NULL_HANDLE = none)
    BOOL hostExt; uint64_t hostAlign; uint64_t impTotal;   // 11h.6: external_memory_host enabled, minImportedHostPointerAlignment, bytes currently imported
} N48R;

static NSError *n48_err(NSInteger code, NSString *msg) {
    return [NSError errorWithDomain:@"Navi48Metal" code:code userInfo:@{ NSLocalizedDescriptionKey: msg }];
}

static uint64_t n48_now(void);

// ---------------------------------------------------------------------------------------------------------------
// native #12 bundle work (lag investigation).
// T1: cheap always-on timing, aggregated per process and drained every 10 s by one timer (no per-event logging). All values ns.
//   spv   = n48_spv_lookup (sha256 + spvcache file read + meta)       shm  = vkCreateShaderModule
//   gfx   = vkCreateGraphicsPipelines   cmp = vkCreateComputePipelines   imp = IOSurface / NoCopy import (vkGetMemoryHostPointerProperties..vkAllocateMemory)
//   enc   = command buffer CPU time, creation (begin) to submit entry (includes app think time between encoders)
//   gpu   = submit -> fence signalled, seen by the completion thread (a lower bound on queueing: the completion queue is SERIAL, so it also holds
//           the head-of-line wait behind earlier fences)          wake = dispatch_async enqueue -> block start on the serial completion queue
//   qlk   = time blocked acquiring N48R.qlock (every acquisition is timed, contended or not)
//   fgap  = gap between submits of consecutive "frame" command buffers (those that wrote a display surface: the D-copy identification)
//   fgpu  = submit -> fence for frame command buffers
// T2: a VkPipelineCache created at device open from /private/var/tmp (or /private/tmp) and saved there (WindowServer only).
// ---------------------------------------------------------------------------------------------------------------
static struct { n48t_hist spv, shm, gfx, cmp, imp, enc, gpu, wake, qlk, fgap, fgpu; } T1;
static _Atomic uint64_t n48_t1_frames, n48_t1_lastframe, n48_pipes_new;
static inline void n48_qlock(void) { uint64_t t_ = n48_now(); pthread_mutex_lock(&(N48R.qlock)); n48t_add(&T1.qlk, n48_now() - t_); }

static struct { char path[300]; uint8_t key[32], uuid[16]; uint32_t vendor, device; BOOL save; uint64_t lastHash, savedPipes; pthread_mutex_t mu; } N48PC = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void n48_pc_derive_key(void) {
    NSBundle *b = [NSBundle bundleForClass:[Navi48Device class]];
    NSString *ver = [b objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    NSString *lib = [[b resourcePath] stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
    struct stat st; long long sz = 0, mt = 0; if (stat(lib.UTF8String, &st) == 0) { sz = st.st_size; mt = st.st_mtimespec.tv_sec; }
    NSString *k = [NSString stringWithFormat:@"navi48metal|%@|%s %s|%lld|%lld", ver, __DATE__, __TIME__, sz, mt];
    CC_SHA256(k.UTF8String, (CC_LONG)strlen(k.UTF8String), N48PC.key);
}
// Called from n48_radv_open_once after the device exists (class lock held). Never fails the open: no cache = pipelines compile as before.
static void n48_pc_open(const VkPhysicalDeviceProperties *pp) {
    if (!vkCreatePipelineCache || !vkGetPipelineCacheData) { N48LOG("T2 pipeline cache: entry points not exposed by RADV, running without"); return; }
    if (access("/private/tmp/n48m-nopcache", F_OK) == 0 || (n48_allow() && getenv("N48M_NOPCACHE"))) { N48LOG("T2 pipeline cache: disabled (kill file /private/tmp/n48m-nopcache or N48M_NOPCACHE)"); return; }
    memcpy(N48PC.uuid, pp->pipelineCacheUUID, 16); N48PC.vendor = pp->vendorID; N48PC.device = pp->deviceID; n48_pc_derive_key();
    const char *dir = (n48_allow() && getenv("N48M_PCACHE_DIR")) ? getenv("N48M_PCACHE_DIR") : (access("/private/var/tmp", W_OK) == 0 ? "/private/var/tmp" : "/private/tmp");
    snprintf(N48PC.path, sizeof N48PC.path, "%s/n48m-pipecache.bin", dir);
    N48PC.save = n48_is_ws() || (n48_allow() && getenv("N48M_PCACHE_SAVE"));   // only WindowServer writes: a root tool's file would block WindowServer's rename in the sticky dir
    char marker[sizeof N48PC.path + 16]; snprintf(marker, sizeof marker, "%s.loading", N48PC.path);
    if (access(marker, F_OK) == 0) { unlink(N48PC.path); unlink(marker); N48LOG("T2 pipeline cache: a previous load never finished (%s present): cache file discarded", marker); }
    void *buf = NULL; size_t flen = 0; const void *payload = NULL; size_t plen = 0; int c = -1;
    int fd = open(N48PC.path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= N48PC_MAX_PAYLOAD + sizeof(n48pc_hdr) && (buf = malloc((size_t)st.st_size))) {
            size_t got = 0; while (got < (size_t)st.st_size) { ssize_t n = read(fd, (char *)buf + got, (size_t)st.st_size - got); if (n <= 0) break; got += (size_t)n; }
            flen = got;
        }
        close(fd);
        if (buf) { c = n48pc_check(buf, flen, N48PC.key, N48PC.uuid, N48PC.vendor, N48PC.device, &payload, &plen);
                   if (c) { N48LOG("T2 pipeline cache: %s ignored: %s", N48PC.path, n48pc_why(c)); payload = NULL; plen = 0; } }
        else N48LOG("T2 pipeline cache: %s unreadable/empty/oversized, ignored", N48PC.path);
    } else N48LOG("T2 pipeline cache: no file %s yet (cold start; saving %s)", N48PC.path, N48PC.save ? "ON" : "OFF in this process");
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = plen, .pInitialData = payload };
    int mk = -1; if (payload) mk = open(marker, O_WRONLY | O_CREAT, 0644);
    uint64_t t0 = n48_now();
    VkResult r = vkCreatePipelineCache(N48R.dev, &ci, NULL, &N48R.pc);
    uint64_t dt = n48_now() - t0;
    if (mk >= 0) { close(mk); unlink(marker); }
    if (r != VK_SUCCESS && payload) {   // RADV refused the data: start empty
        N48LOG("T2 pipeline cache: vkCreatePipelineCache(initialData %zu B) = %d, retrying empty", plen, r);
        ci.initialDataSize = 0; ci.pInitialData = NULL; payload = NULL; r = vkCreatePipelineCache(N48R.dev, &ci, NULL, &N48R.pc);
    }
    if (r != VK_SUCCESS) { N48R.pc = VK_NULL_HANDLE; N48LOG("T2 pipeline cache: vkCreatePipelineCache = %d, running without", r); free(buf); return; }
    if (payload) N48PC.lastHash = n48pc_fnv(payload, plen);
    N48LOG("T2 pipeline cache: %p created from %s in %.2f ms (initialData %zu B%s); key %02x%02x%02x%02x uuid %02x%02x%02x%02x", (void *)N48R.pc, N48PC.path, (double)dt / 1e6, plen, payload ? ", ACCEPTED" : ", none",
           N48PC.key[0], N48PC.key[1], N48PC.key[2], N48PC.key[3], N48PC.uuid[0], N48PC.uuid[1], N48PC.uuid[2], N48PC.uuid[3]);
    free(buf);
}
static void n48_pc_save(const char *why) {
    if (!N48R.ok || !N48R.pc || !N48PC.save || !N48PC.path[0]) return;
    uint64_t np = atomic_load(&n48_pipes_new);
    pthread_mutex_lock(&N48PC.mu);
    if (np == N48PC.savedPipes) { pthread_mutex_unlock(&N48PC.mu); return; }
    N48PC.savedPipes = np;   // retried only when another pipeline appears (no log spam on a persistent write failure)
    uint64_t t0 = n48_now(); size_t sz = 0; uint8_t *buf = NULL;
    if (vkGetPipelineCacheData(N48R.dev, N48R.pc, &sz, NULL) == VK_SUCCESS && sz >= 32 && sz <= N48PC_MAX_PAYLOAD && (buf = malloc(sizeof(n48pc_hdr) + sz))) {
        VkResult r = vkGetPipelineCacheData(N48R.dev, N48R.pc, &sz, buf + sizeof(n48pc_hdr));
        if (r == VK_SUCCESS && n48pc_vk_header_ok(buf + sizeof(n48pc_hdr), sz, N48PC.uuid, N48PC.vendor, N48PC.device)) {
            uint64_t h = n48pc_fnv(buf + sizeof(n48pc_hdr), sz);
            if (h == N48PC.lastHash) { free(buf); pthread_mutex_unlock(&N48PC.mu); return; }   // nothing new in the cache
            n48pc_hdr hd; n48pc_fill(&hd, N48PC.key, N48PC.uuid, N48PC.vendor, N48PC.device, buf + sizeof hd, sz); memcpy(buf, &hd, sizeof hd);
            char tmp[sizeof N48PC.path + 24]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", N48PC.path, (int)getpid());
            int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); BOOL ok = NO; int en = 0;
            if (fd >= 0) {
                size_t put = 0, tot = sizeof hd + sz; while (put < tot) { ssize_t n = write(fd, buf + put, tot - put); if (n <= 0) break; put += (size_t)n; }
                ok = put == tot && fsync(fd) == 0; en = errno; close(fd);
                if (ok && rename(tmp, N48PC.path) != 0) { ok = NO; en = errno; }
                if (!ok) unlink(tmp);
            } else en = errno;
            if (ok) { N48PC.lastHash = h; N48LOG("T2 pipeline cache: SAVED %zu B to %s (%s; %llu pipelines created so far) in %.2f ms", sizeof hd + sz, N48PC.path, why, (unsigned long long)np, (double)(n48_now() - t0) / 1e6); }
            else N48LOG("T2 pipeline cache: save to %s FAILED (errno %d)", N48PC.path, en);
        } else N48LOG("T2 pipeline cache: vkGetPipelineCacheData = %d / header check failed, not saved", r);
    }
    free(buf);
    pthread_mutex_unlock(&N48PC.mu);
}
static void n48_pc_atexit(void) { n48_pc_save("exit"); }

static void n48_pool_latch(void);
static void n48_imp_latch(void);
static void n48_imp_tick(void);
static void n48_imp_report(unsigned tick);
static void n48_pool_tick(void);
static void n48_pool_report(unsigned tick);
static void n48_drawopt_latch(void);
static void n48_drawopt_report(unsigned tick);
static void n48_t1_report(void) {
    static unsigned tick;
    struct { const char *n; n48t_hist *h; n48t_snap s; } m[] = { { "spvcache_read", &T1.spv, {0} }, { "shader_module", &T1.shm, {0} }, { "gfx_pipeline", &T1.gfx, {0} }, { "comp_pipeline", &T1.cmp, {0} }, { "import", &T1.imp, {0} },
        { "cb_encode", &T1.enc, {0} }, { "cb_gpu(submit->done)", &T1.gpu, {0} }, { "completion_wake", &T1.wake, {0} }, { "qlock_wait", &T1.qlk, {0} }, { "frame_gap", &T1.fgap, {0} }, { "frame_gpu", &T1.fgpu, {0} } };
    unsigned any = 0; char line[640];
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        n48t_take(m[i].h, &m[i].s);
        if (n48t_fmt(m[i].n, &m[i].s, line, sizeof line)) { any++; N48LOG("%s [%s pid %d, 10 s window]", line, getprogname(), (int)getpid()); }
    }
    uint64_t fr = atomic_exchange(&n48_t1_frames, 0);
    if (any || fr) N48LOG("T1 counts/10s [%s pid %d]: pipelines gfx=%llu comp=%llu imports=%llu command_buffers=%llu frames=%llu (frame = command buffer that wrote a display surface)", getprogname(), (int)getpid(),
        (unsigned long long)m[2].s.n, (unsigned long long)m[3].s.n, (unsigned long long)m[4].s.n, (unsigned long long)m[5].s.n, (unsigned long long)fr);
    else if (tick % 6 == 5) N48LOG("T1 idle: no pipeline/import/command buffer in the last 60 s [%s pid %d]", getprogname(), (int)getpid());
    n48_pool_report(tick);   // P1: one line when the pool is ON and something happened (nothing when OFF)
    n48_drawopt_report(tick);   // P5b: one line when the drawopt switch is ON and something happened (nothing when OFF)
    n48_pool_tick();
    n48_imp_report(tick); n48_imp_tick();   // P4
    if (++tick % 3 == 0) n48_pc_save("periodic");
}
static void n48_t1_start(void) {
    static dispatch_source_t src;
    if (src) return;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, 500 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(src, ^{ n48_t1_report(); });
    dispatch_resume(src);
    atexit(n48_pc_atexit);
    N48LOG("T1 timing: summaries every 10 s (pid %d %s)", (int)getpid(), getprogname());
}
// N48M_TEST_OPEN_REFUSE=<n> (needs N48M_ALLOW): the first n open attempts fail like a refused N48N open, without touching Vulkan.
static BOOL n48_test_refuse(void) {
    static long left = -1;
    if (left < 0) { const char *e = getenv("N48M_TEST_OPEN_REFUSE"); left = (n48_allow() && e) ? atol(e) : 0; }
    if (left > 0) { left--; return YES; }
    return NO;
}
// One open attempt (#12 R4: the retry policy is in n48_radv_open). Returns YES when the RADV device + queue are up; on failure *err
// is set. Called with the class lock held.
static BOOL n48_radv_open_once(NSError **err) {
    {
        NSString *dir = [[NSBundle bundleForClass:[Navi48Device class]] resourcePath];
        NSString *path = [dir stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
        N48LOG("radv open: dlopen %s", path.UTF8String);
        void *h = dlopen(path.UTF8String, RTLD_NOW | RTLD_LOCAL);
        if (!h) { N48R.err = n48_err(1, [NSString stringWithFormat:@"dlopen %@: %s", path, dlerror()]); goto fail; }
        N48R.lib = h;
        n48_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!n48_gipa) { N48R.err = n48_err(2, @"no vk_icdGetInstanceProcAddr in RADV"); goto fail; }
        n48_vkCreateInstance = (PFN_vkCreateInstance)n48_gipa(NULL, "vkCreateInstance");
        if (!n48_vkCreateInstance) { N48R.err = n48_err(3, @"no vkCreateInstance"); goto fail; }
        VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "navi48metal", .apiVersion = VK_API_VERSION_1_2 };
        VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
        {   // P5a: /private/tmp/n48m-nosam -> RADV_PERFTEST gains "nosam" (read by RADV at instance creation): command buffers and RADV's upload buffer go to GTT
            // (system memory, CPU-cached, GPU-snooped) instead of the BAR, whose uncached stores stalled radv_emit_* during desktop drags (P5 study, ws-p1.txt). Absent = unchanged.
            struct stat sb_;
            if (stat("/private/tmp/n48m-nosam", &sb_) == 0 || stat("/private/tmp/n48m-nosam-off", &sb_) != 0) {   //: DEFAULT ON (60 fps under drag measured); /private/tmp/n48m-nosam-off disables
                const char *pt = getenv("RADV_PERFTEST"); char v[256];
                snprintf(v, sizeof v, "%s%snosam", pt && *pt ? pt : "", pt && *pt ? "," : "");
                setenv("RADV_PERFTEST", v, 1);
                N48LOG("radv open: P5a /private/tmp/n48m-nosam present: RADV_PERFTEST=%s (command buffers in GTT)", v);
            }
        }
        VkResult r = n48_vkCreateInstance(&ici, NULL, &N48R.inst);
        if (r != VK_SUCCESS) { N48R.err = n48_err(4, [NSString stringWithFormat:@"vkCreateInstance = %d", r]); goto fail; }
#define X(n) n = (PFN_##n)n48_gipa(N48R.inst, #n); if (!n) N48LOG("radv: %s not exposed", #n);
        N48_VK_FUNCS(X)
#undef X
        uint32_t cnt = 1;
        r = vkEnumeratePhysicalDevices(N48R.inst, &cnt, &N48R.pd);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt == 0) {
            N48R.err = n48_err(5, [NSString stringWithFormat:@"vkEnumeratePhysicalDevices = %d count %u (N48N refused? root-only, exclusive)", r, cnt]);
            if (vkDestroyInstance && N48R.inst) { vkDestroyInstance(N48R.inst, NULL); N48R.inst = VK_NULL_HANDLE; }   // a retry starts from a fresh instance
            goto fail;
        }
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(N48R.pd, &pp);
        N48LOG("radv: physical device %s", pp.deviceName);
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(N48R.pd, &nq, NULL);
        VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(N48R.pd, &nq, qf);
        uint32_t qfi = 0; for (; qfi < nq; qfi++) if (qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        if (qfi == nq) { N48R.err = n48_err(6, @"no graphics queue family"); goto fail; }
        N48R.qfi = qfi;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        VkPhysicalDeviceFeatures pf; vkGetPhysicalDeviceFeatures(N48R.pd, &pf);
        // translated AIR uses Int64 (10a); 11e: storage images are translated with format Unknown + Read/WriteWithoutFormat (add-air.py)
        VkPhysicalDeviceFeatures ef = { .shaderInt64 = pf.shaderInt64, .fragmentStoresAndAtomics = pf.fragmentStoresAndAtomics, .shaderStorageImageReadWithoutFormat = pf.shaderStorageImageReadWithoutFormat,
                                        .shaderStorageImageWriteWithoutFormat = pf.shaderStorageImageWriteWithoutFormat };
        N48LOG("radv: apiVersion %u.%u shaderInt64 supported %u, storage image read/write without format %u/%u", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), pf.shaderInt64,
               pf.shaderStorageImageReadWithoutFormat, pf.shaderStorageImageWriteWithoutFormat);
        N48R.lim = pp.limits;
        // 11e-2: measure what RADV offers for framebuffer fetch (logged once per process; the design choice is recorded in NATIVE-S4-M11.md).
        {
            uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(N48R.pd, NULL, &ne, NULL);
            VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(N48R.pd, NULL, &ne, ex);
            NSMutableString *all = [NSMutableString string];
            for (uint32_t i = 0; i < ne; i++) [all appendFormat:@"%s ", ex[i].extensionName];
            N48LOG("radv: %u device extensions (listed in chunks):", ne);
            for (NSUInteger o = 0; o < all.length; o += 600) N48LOG("radv: ext[%lu]: %s", (unsigned long)o, [all substringWithRange:NSMakeRange(o, MIN((NSUInteger)600, all.length - o))].UTF8String);
            BOOL hasRO = NO, hasLR = NO, hasFL = NO, hasDR = NO;
            for (uint32_t i = 0; i < ne; i++) {
                if (!strcmp(ex[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) N48R.hostExt = YES;
                hasRO |= !strcmp(ex[i].extensionName, "VK_EXT_rasterization_order_attachment_access") || !strcmp(ex[i].extensionName, "VK_ARM_rasterization_order_attachment_access");
                hasLR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering_local_read");
                hasFL |= !strcmp(ex[i].extensionName, "VK_EXT_attachment_feedback_loop_layout");
                hasDR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering");
            }
            free(ex);
            VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT fro = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT };
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .pNext = &fro };
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &flr };
            if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(N48R.pd, &f2);
            N48LOG("radv: framebuffer-fetch support: rasterization_order_attachment_access ext %d (color feature %d, depth %d, stencil %d); dynamic_rendering_local_read ext %d (feature %d); attachment_feedback_loop_layout ext %d; dynamic_rendering ext %d; classic render-pass input attachments: core 1.0",
                   hasRO, fro.rasterizationOrderColorAttachmentAccess, fro.rasterizationOrderDepthAttachmentAccess, fro.rasterizationOrderStencilAttachmentAccess, hasLR, flr.dynamicRenderingLocalRead, hasFL, hasDR);
        }
        const char *devExts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .pEnabledFeatures = &ef,
                                   .enabledExtensionCount = N48R.hostExt ? 1 : 0, .ppEnabledExtensionNames = devExts };
        r = vkCreateDevice(N48R.pd, &dci, NULL, &N48R.dev);
        if (r != VK_SUCCESS) { N48R.err = n48_err(7, [NSString stringWithFormat:@"vkCreateDevice = %d", r]); goto fail; }
        vkGetDeviceQueue(N48R.dev, qfi, 0, &N48R.q);
        N48R.hostAlign = 4096;
        if (N48R.hostExt && vkGetDeviceProcAddr && vkGetPhysicalDeviceProperties2) {
            n48_vkGetMHPP = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(N48R.dev, "vkGetMemoryHostPointerPropertiesEXT");
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh };
            vkGetPhysicalDeviceProperties2(N48R.pd, &p2);
            if (emh.minImportedHostPointerAlignment) N48R.hostAlign = emh.minImportedHostPointerAlignment;
            N48LOG("radv: VK_EXT_external_memory_host enabled, vkGetMemoryHostPointerPropertiesEXT %s, minImportedHostPointerAlignment %llu", n48_vkGetMHPP ? "present" : "MISSING", (unsigned long long)N48R.hostAlign);
        } else N48LOG("radv: VK_EXT_external_memory_host NOT available (IOSurface-backed textures will be refused)");
        vkGetPhysicalDeviceMemoryProperties(N48R.pd, &N48R.mp);
        pthread_mutex_init(&N48R.qlock, NULL);
        VkCommandPoolCreateInfo opc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        r = vkCreateCommandPool(N48R.dev, &opc, NULL, &N48R.opool);   // one-shot uploads (replaceRegion, getBytes)
        if (r != VK_SUCCESS) { N48R.err = n48_err(8, [NSString stringWithFormat:@"vkCreateCommandPool(one-shot) = %d", r]); goto fail; }
        n48_pc_open(&pp);
        n48_pool_latch();   // P1: reads /private/tmp/n48m-pool ONCE
        n48_imp_latch();    // P4: reads /private/tmp/n48m-impcache ONCE
        n48_drawopt_latch();   // P5b: reads /private/tmp/n48m-drawopt ONCE
        N48R.ok = YES;
        n48_t1_start();
        N48LOG("radv open: OK (queue family %u)", qfi);
        return YES;
    fail:
        N48LOG("radv open: FAILED %s", N48R.err.localizedDescription.UTF8String);
        if (err) *err = N48R.err;
        return NO;
    }
}

// #12 R4: a refused N48N open (code 5: busy / not ready) is transient. The first open of the process retries with backoff
// (0.1, 0.2, 0.4, 0.8, 1.5 s, bounded to 3 s from the first attempt) before giving up, logging every attempt; after that, a later request retries ONCE if the
// last failure is >= 2 s old (so a lazy open that lost a race with a closing client is not lost for the life of the process).
// Any other failure (no dylib, vkCreateDevice, ...) is permanent for the process, as before.
static BOOL n48_radv_open(NSError **err) {
    @synchronized ([Navi48Device class]) {
        if (N48R.ok) return YES;
        static uint64_t lastFail;
        BOOL transient = N48R.tried && N48R.err.code == 5;
        if (N48R.tried && !(transient && n48_now() - lastFail >= 2000000000ULL)) { if (err) *err = N48R.err; return NO; }
        static const useconds_t backoff[] = { 100000, 200000, 400000, 800000, 1500000 };
        const uint64_t budget = 3000000000ULL;   // first open of the process: keep trying until 3 s have passed since the first attempt began (an attempt itself can take ~2 s on the PC)
        BOOL first = !N48R.tried; N48R.tried = YES;
        uint64_t t0 = n48_now();
        for (int a = 0; a < (first ? 6 : 1); a++) {
            if (a) {
                uint64_t el = n48_now() - t0;
                if (el >= budget) { N48LOG("radv open: giving up after %.2f s and %d attempt(s)", (double)el / 1e9, a); break; }
                useconds_t d = backoff[a - 1]; if ((uint64_t)d * 1000ULL > budget - el) d = (useconds_t)((budget - el) / 1000ULL);
                N48LOG("radv open: refused (%s); retry %d after %u ms (%.2f s used of 3 s)", N48R.err.localizedDescription.UTF8String, a, d / 1000, (double)el / 1e9); usleep(d);
            }
            N48LOG("radv open: attempt %d%s", a + 1, first ? "" : " (retry after an earlier refusal)");
            if (n48_test_refuse()) { N48R.err = n48_err(5, @"vkEnumeratePhysicalDevices = -3 count 0 (N48M_TEST_OPEN_REFUSE: simulated refusal)"); continue; }
            if (n48_radv_open_once(err)) { if (a || !first) N48LOG("radv open: succeeded on attempt %d", a + 1); return YES; }
            if (N48R.err.code != 5) break;
        }
        lastFail = n48_now();
        if (err) *err = N48R.err;
        return NO;
    }
}

static int n48_find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < N48R.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (N48R.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------
// P1 "memory pooling" (n48_pool.h decides, this block does the Vulkan calls). ON only when /private/tmp/n48m-pool exists, latched ONCE when the device is
// created; OFF = every call site below runs the code that was there before (the `else` branches are the old lines, unchanged).
//  * N48Buffer / N48Texture memory: <= 256 KiB from 4 MiB slabs (bound at the slab offset; host-visible slabs mapped once), 256 KiB..64 MiB from a recycle
//    cache keyed by (memory type, size), the rest (and anything VkMemoryDedicatedRequirements marks dedicated) on its own allocation as before.
//  * Nothing freed is reused until every command buffer that existed at the free has completed (device-wide fence, see N48CommandBuffer: serial taken at creation
//    in -initWithQueue:, closed in the completion block of -submitCommandBuffers:count: or at dealloc if it was never submitted).
//  * Descriptor pools of a command buffer go to a fence-gated free list (cap 32) instead of being destroyed.
// IOSurface / host-pointer imports, the scanout slots (N48S.mem) and heaps' budgets are untouched.
// ---------------------------------------------------------------------------------------------------------------
#define N48_POOL_FILE "/private/tmp/n48m-pool"
#define N48_OPT_NOZERO (1ULL << 40)   // internal marker in N48Buffer's options: the contents need not be zeroed (the upload ring)
typedef struct { int kind; VkDeviceMemory mem; uint8_t *map; uint64_t slab, off, size; uint32_t key; } N48Mem;   // kind 0 = own allocation, 1 = slab range, 2 = whole (recycled / recyclable)
static struct { BOOL on; pthread_mutex_t mu; n48p_pool p; BOOL reqOK; uint64_t lastTrim, lastSig; unsigned unchanged;
    PFN_vkGetBufferMemoryRequirements2 bmr2; PFN_vkGetImageMemoryRequirements2 imr2; PFN_vkResetDescriptorPool rdp; } N48P = { .mu = PTHREAD_MUTEX_INITIALIZER };
static void n48_pool_latch(void) {
    static int done; if (done) return; done = 1;
    N48P.on = access(N48_POOL_FILE, F_OK) == 0 || access(N48_POOL_FILE "-off", F_OK) != 0;   //: DEFAULT ON; /private/tmp/n48m-pool-off disables
    n48p_init(&N48P.p, N48P.on);
    if (!N48P.on) { N48LOG("T1 pool: OFF (no %s at device creation; latched for the life of this process)", N48_POOL_FILE); return; }
    N48P.bmr2 = (PFN_vkGetBufferMemoryRequirements2)vkGetDeviceProcAddr(N48R.dev, "vkGetBufferMemoryRequirements2");
    N48P.imr2 = (PFN_vkGetImageMemoryRequirements2)vkGetDeviceProcAddr(N48R.dev, "vkGetImageMemoryRequirements2");
    N48P.rdp = (PFN_vkResetDescriptorPool)vkGetDeviceProcAddr(N48R.dev, "vkResetDescriptorPool");
    N48P.reqOK = N48P.bmr2 && N48P.imr2;
    if (!N48P.rdp) N48LOG("T1 pool: vkResetDescriptorPool not exposed: descriptor pools will not be reused");
    N48LOG("T1 pool: ON (%s present at device creation): slabs %llu KiB (<= %llu KiB), recycle %llu KiB..%llu MiB cap %llu MiB, idle %llu ms, dedicated query %s, granularity %llu",
           N48_POOL_FILE, (unsigned long long)(N48P_SLAB_SIZE >> 10), (unsigned long long)(N48P_SUB_MAX >> 10), (unsigned long long)(N48P_SUB_MAX >> 10), (unsigned long long)(N48P_REC_MAX >> 20),
           (unsigned long long)(N48P_REC_CAP >> 20), (unsigned long long)(N48P_IDLE_NS / 1000000ULL), N48P.reqOK ? "yes" : "NO (everything own-allocation)", (unsigned long long)N48R.lim.bufferImageGranularity);
}
// Frees what the pool has given up (called with the pool mutex NOT held: vkFreeMemory is the slow kernel call).
static void n48_pool_drain(void) {
    for (;;) {
        n48p_rel o; pthread_mutex_lock(&N48P.mu); int got = n48p_pop_release(&N48P.p, &o); pthread_mutex_unlock(&N48P.mu);
        if (!got) return;
        vkFreeMemory(N48R.dev, (VkDeviceMemory)o.mem, NULL);
    }
}
static void n48_pool_tick(void) {
    if (!N48P.on) return;
    pthread_mutex_lock(&N48P.mu); n48p_trim(&N48P.p, n48_now()); pthread_mutex_unlock(&N48P.mu);
    n48_pool_drain();
}
static void n48_pool_report(unsigned tick) {
    if (!N48P.on) return;
    char line[640]; uint64_t sig;
    pthread_mutex_lock(&N48P.mu); n48p_fmt(&N48P.p, line, sizeof line); sig = N48P.p.subAlloc + N48P.p.subFree + N48P.p.recHit + N48P.p.recMiss + N48P.p.dpReuse + N48P.p.dpCreate + N48P.p.slabNew + N48P.p.slabRel + N48P.p.directAlloc; pthread_mutex_unlock(&N48P.mu);
    if (sig != N48P.lastSig || tick % 6 == 5) { N48P.lastSig = sig; N48LOG("%s [%s pid %d, cumulative]", line, getprogname(), (int)getpid()); }
}
static BOOL n48_pool_dedicated(VkBuffer b, VkImage i) {
    if (!N48P.reqOK) return YES;   // cannot ask: keep the old behaviour (own allocation)
    VkMemoryDedicatedRequirements dr = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 r2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr };
    if (b) { VkBufferMemoryRequirementsInfo2 bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = b }; N48P.bmr2(N48R.dev, &bi, &r2); }
    else { VkImageMemoryRequirementsInfo2 ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = i }; N48P.imr2(N48R.dev, &ii, &r2); }
    return dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation;
}
static VkResult n48_pool_vkalloc(uint32_t mt, uint64_t size, BOOL map, VkDeviceMemory *mem, uint8_t **mp) {
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = mt };
    VkResult r = vkAllocateMemory(N48R.dev, &ma, NULL, mem);
    if (r != VK_SUCCESS) {   // out of kernel BOs / VRAM: give back what the pool is holding that the fence has cleared, once
        pthread_mutex_lock(&N48P.mu); n48p_flush(&N48P.p, n48_now()); pthread_mutex_unlock(&N48P.mu); n48_pool_drain();
        r = vkAllocateMemory(N48R.dev, &ma, NULL, mem);
    }
    if (r != VK_SUCCESS) { *mem = VK_NULL_HANDLE; return r; }
    *mp = NULL;
    if (map) {
        void *m = NULL; r = vkMapMemory(N48R.dev, *mem, 0, VK_WHOLE_SIZE, 0, &m);
        if (r != VK_SUCCESS) { vkFreeMemory(N48R.dev, *mem, NULL); *mem = VK_NULL_HANDLE; return r; }
        *mp = (uint8_t *)m;
    }
    return VK_SUCCESS;
}
// Memory for one resource whose requirements are `mr` (memory type `mt`). Fills *m; the caller binds at m->off and, for a mapped buffer, uses m->map.
static VkResult n48_mem_alloc(uint32_t mt, VkMemoryRequirements mr, BOOL map, BOOL zero, VkBuffer b, VkImage i, N48Mem *m) {
    memset(m, 0, sizeof *m);
    uint64_t gran = N48R.lim.bufferImageGranularity ? N48R.lim.bufferImageGranularity : 1;
    uint64_t align = mr.alignment > gran ? mr.alignment : gran, sz = n48p_alup(mr.size, gran), sub = sz;
    uint32_t key = mt * 2u + (map ? 1u : 0u);
    BOOL ded = n48_pool_dedicated(b, i), newSlab = NO;
    for (int tries = 0; tries < 4 && !ded; tries++) {
        n48p_res r; pthread_mutex_lock(&N48P.mu); int k = n48p_alloc(&N48P.p, key, sz, align, n48_now(), &r);
        if (k == N48P_R_SUB && newSlab) n48p_note_new_slab_use(&N48P.p);
        pthread_mutex_unlock(&N48P.mu);
        if (k == N48P_R_SUB) {
            *m = (N48Mem){ 1, (VkDeviceMemory)r.mem, r.map ? (uint8_t *)r.map + r.off : NULL, r.slab, r.off, sub, key };
            if (zero && m->map) memset(m->map, 0, mr.size);
            return VK_SUCCESS;
        }
        if (k == N48P_R_REC) {
            *m = (N48Mem){ 2, (VkDeviceMemory)r.mem, (uint8_t *)r.map, 0, 0, mr.size, key };
            if (zero && m->map) memset(m->map, 0, mr.size);
            return VK_SUCCESS;
        }
        if (k == N48P_R_NEED_SLAB) {
            VkDeviceMemory sm; uint8_t *smap; VkResult vr = n48_pool_vkalloc(mt, N48P_SLAB_SIZE, map, &sm, &smap);
            if (vr != VK_SUCCESS) break;   // no room for a slab: try an own allocation of the exact size
            pthread_mutex_lock(&N48P.mu); n48p_add_slab(&N48P.p, key, N48P_SLAB_SIZE, (void *)sm, smap, n48_now()); pthread_mutex_unlock(&N48P.mu);
            newSlab = YES; continue;
        }
        if (k == N48P_R_NEED_WHOLE) {
            VkDeviceMemory wm; uint8_t *wmap; VkResult vr = n48_pool_vkalloc(mt, mr.size, map, &wm, &wmap);
            if (vr != VK_SUCCESS) return vr;
            *m = (N48Mem){ 2, wm, wmap, 0, 0, mr.size, key };   // fresh from the kernel: already zero
            return VK_SUCCESS;
        }
        break;   // DIRECT
    }
    VkDeviceMemory dm; uint8_t *dmap; VkResult vr = n48_pool_vkalloc(mt, mr.size, map, &dm, &dmap);
    if (vr != VK_SUCCESS) return vr;
    *m = (N48Mem){ 0, dm, dmap, 0, 0, mr.size, key };
    return VK_SUCCESS;
}
static void n48_mem_free(N48Mem *m) {
    if (!m->mem) return;
    uint64_t now = n48_now(); int own = 0;
    pthread_mutex_lock(&N48P.mu);
    if (m->kind == 1) n48p_free_sub(&N48P.p, m->slab, m->off, m->size, now);
    else if (m->kind == 2) own = !n48p_free_whole(&N48P.p, m->key, m->size, (void *)m->mem, m->map, now);
    else own = 1;
    if (now - N48P.lastTrim > 250000000ULL) { N48P.lastTrim = now; n48p_trim(&N48P.p, now); }
    pthread_mutex_unlock(&N48P.mu);
    if (own) vkFreeMemory(N48R.dev, m->mem, NULL);   // an own allocation, or one the pool declined (over the deferred-bytes bound)
    n48_pool_drain();
    memset(m, 0, sizeof *m);
}
static uint64_t n48_fence_open(void) { pthread_mutex_lock(&N48P.mu); uint64_t s = n48p_submit(&N48P.p); pthread_mutex_unlock(&N48P.mu); return s; }
static void n48_fence_close(uint64_t s) { if (!s) return; pthread_mutex_lock(&N48P.mu); n48p_done(&N48P.p, s); pthread_mutex_unlock(&N48P.mu); }
// Descriptor pools: a command buffer's pools go back here at dealloc (after its fence) instead of being destroyed.
static BOOL n48_dp_give(VkDescriptorPool dp) {
    if (!N48P.on || !N48P.rdp) return NO;
    pthread_mutex_lock(&N48P.mu); int ok = n48p_h_push(&N48P.p, (uint64_t)(uintptr_t)dp, 32); pthread_mutex_unlock(&N48P.mu); return ok ? YES : NO;
}
static void n48_dp_created(void) { if (!N48P.on) return; pthread_mutex_lock(&N48P.mu); N48P.p.dpCreate++; pthread_mutex_unlock(&N48P.mu); }
static VkDescriptorPool n48_dp_take(void) {   // VK_NULL_HANDLE: none cleared by the fence; the caller creates one
    if (!N48P.on || !N48P.rdp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&N48P.mu); VkDescriptorPool dp = (VkDescriptorPool)(uintptr_t)n48p_h_pop(&N48P.p); pthread_mutex_unlock(&N48P.mu);
    if (!dp) return VK_NULL_HANDLE;
    if (N48P.rdp(N48R.dev, dp, 0) != VK_SUCCESS) { vkDestroyDescriptorPool(N48R.dev, dp, NULL); return VK_NULL_HANDLE; }
    return dp;
}

// ---------------------------------------------------------------------------------------------------------------
// P5b "per-draw redundancy" (n48_drawcache.h decides, this block holds the latch, the device-wide render-pass cache and the counters). ON only when
// /private/tmp/n48m-drawopt exists, latched ONCE at device creation; OFF = every call site runs the code that was there before.
//  * N48RenderEncoder: re-uses a stage's VkDescriptorSet while the exact slot contents its set layout reads are unchanged; skips vkCmdBindPipeline for the pipeline
//    already bound in the current render pass; remembers the last (topology, cull, front, pso) -> VkPipeline answer.
//  * Render passes are cached device-wide by their attachment descriptions (<= 64, never destroyed while the device lives; beyond the cap created / destroyed as before).
// ---------------------------------------------------------------------------------------------------------------
#define N48_DRAWOPT_FILE "/private/tmp/n48m-drawopt"
static struct { BOOL on; pthread_mutex_t mu; n48dc_rpcache *rp; _Atomic uint64_t draws, setsReused, setsAlloc, pipesSkipped, pipesBound, pcHits, rpHits, rpCreated, rpUncached; uint64_t lastSig; } N48DO = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic int n48_dw_logged;   // P5b: how many "scanout: display write" lines n48s_disp_account has printed (it prints the pipeline-name list of the first 20 only)
static void n48_drawopt_latch(void) {
    static int done; if (done) return; done = 1;
    N48DO.on = access(N48_DRAWOPT_FILE, F_OK) == 0 || access(N48_DRAWOPT_FILE "-off", F_OK) != 0;   //: DEFAULT ON; /private/tmp/n48m-drawopt-off disables
    if (N48DO.on) { N48DO.rp = (n48dc_rpcache *)calloc(1, sizeof *N48DO.rp); if (!N48DO.rp) N48DO.on = NO; }
    if (!N48DO.on) { N48LOG("T1 drawopt: OFF (no %s at device creation; latched for the life of this process)", N48_DRAWOPT_FILE); return; }
    N48LOG("T1 drawopt: ON (%s present at device creation; latched for the life of this process): descriptor-set reuse, pipeline-bind skip, variant cache, render-pass cache (<= %d)", N48_DRAWOPT_FILE, N48DC_RP_CAP);
}
static void n48_drawopt_report(unsigned tick) {
    if (!N48DO.on) return;
    uint64_t d = atomic_load(&N48DO.draws), sr = atomic_load(&N48DO.setsReused), sa = atomic_load(&N48DO.setsAlloc), ps = atomic_load(&N48DO.pipesSkipped), pb = atomic_load(&N48DO.pipesBound),
             pc = atomic_load(&N48DO.pcHits), rh = atomic_load(&N48DO.rpHits), rc = atomic_load(&N48DO.rpCreated), ru = atomic_load(&N48DO.rpUncached), sig = d + sr + sa + ps + pb + pc + rh + rc + ru;
    if (sig != N48DO.lastSig || tick % 6 == 5) {
        N48DO.lastSig = sig;
        N48LOG("T1 drawopt: draws %llu, descriptor sets reused %llu / allocated %llu, pipeline binds skipped %llu / done %llu, pipelineForTopology cache hits %llu, render passes cached(hit) %llu / created %llu / uncached(cap) %llu [%s pid %d, cumulative]",
               (unsigned long long)d, (unsigned long long)sr, (unsigned long long)sa, (unsigned long long)ps, (unsigned long long)pb, (unsigned long long)pc, (unsigned long long)rh, (unsigned long long)rc, (unsigned long long)ru, getprogname(), (int)getpid());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// P4 import cache (n48_impcache.h decides, this block does the Vulkan / CF calls). ON only when /private/tmp/n48m-impcache exists, latched ONCE at device creation;
// OFF = -initWithDevice:descriptor:iosurface:plane: and -dealloc run the per-texture import exactly as before (`_impShared` stays NO).
//  * One VkDeviceMemory import per live IOSurface (key id + base + import size), shared by all its textures; kept 2 s after the last texture died, <= 128 MiB unused.
//  * The cache holds its own CFRetain on the IOSurface for as long as it keeps the import (NOT IOSurfaceIncrementUseCount: that stays per texture).
//  * An import given up is vkFreeMemory'd only once the P1 fence has cleared the serial taken when it was given up (stamp 0 / completed 0 when the pool is OFF = as today).
// ---------------------------------------------------------------------------------------------------------------
#define N48_IMPCACHE_FILE "/private/tmp/n48m-impcache"
static struct { BOOL on; pthread_mutex_t mu; n48ic_cache c; _Atomic uint64_t baseless; uint64_t lastSig; } N48IC = { .mu = PTHREAD_MUTEX_INITIALIZER };
static void n48_imp_latch(void) {
    static int done; if (done) return; done = 1;
    N48IC.on = access(N48_IMPCACHE_FILE, F_OK) == 0;
    n48ic_init(&N48IC.c, N48IC.on);
    if (N48IC.on) N48LOG("P4 impcache: ON (%s present at device creation; latched for the life of this process): one shared import per IOSurface, kept %llu ms after its last texture, <= %llu MiB unused, frees fence-gated (%s)",
        N48_IMPCACHE_FILE, (unsigned long long)(N48IC_IDLE_NS / 1000000ULL), (unsigned long long)(N48IC_CAP >> 20), N48P.on ? "P1 fence" : "pool OFF: kernel idle wait as today");
    else N48LOG("P4 impcache: OFF (no %s at device creation; latched for the life of this process)", N48_IMPCACHE_FILE);
}
static void n48_imp_fence(uint64_t *stamp, uint64_t *completed) {
    *stamp = 0; *completed = 0;
    if (!N48P.on) return;
    pthread_mutex_lock(&N48P.mu); *stamp = N48P.p.submitted; *completed = N48P.p.completed; pthread_mutex_unlock(&N48P.mu);
}
// Frees every released import the fence allows (vkFreeMemory is the slow kernel call: no mutex held).
static void n48_imp_drain(void) {
    if (!N48IC.on) return;
    uint64_t st, comp; n48_imp_fence(&st, &comp);
    for (;;) {
        n48ic_rel o; pthread_mutex_lock(&N48IC.mu); int got = n48ic_pop(&N48IC.c, comp, &o); pthread_mutex_unlock(&N48IC.mu);
        if (!got) return;
        vkFreeMemory(N48R.dev, (VkDeviceMemory)o.mem, NULL);
        @synchronized ([Navi48Device class]) { N48R.impTotal -= o.size; }
        if (o.owner) CFRelease((IOSurfaceRef)o.owner);
    }
}
static void n48_imp_tick(void) {
    if (!N48IC.on) return;
    uint64_t st, comp; n48_imp_fence(&st, &comp);
    pthread_mutex_lock(&N48IC.mu); n48ic_trim(&N48IC.c, n48_now(), st); pthread_mutex_unlock(&N48IC.mu);
    n48_imp_drain();
}
// The shared import of `s` (ma = the import allocate info). *hit YES = an existing import was reused.
static VkResult n48_imp_get(IOSurfaceRef s, void *base, size_t ialloc, const VkMemoryAllocateInfo *ma, VkDeviceMemory *out, BOOL *hit) {
    uint64_t st, comp; n48_imp_fence(&st, &comp); *hit = NO;
    uint32_t id = IOSurfaceGetID(s); void *m = NULL;
    pthread_mutex_lock(&N48IC.mu); int h = n48ic_acquire(&N48IC.c, id, (uint64_t)(uintptr_t)base, ialloc, n48_now(), st, &m); pthread_mutex_unlock(&N48IC.mu);
    if (h) { *out = (VkDeviceMemory)m; *hit = YES; return VK_SUCCESS; }
    VkDeviceMemory mem = VK_NULL_HANDLE; VkResult r = vkAllocateMemory(N48R.dev, ma, NULL, &mem);
    if (r != VK_SUCCESS) {   // out of kernel imports (256 MiB per client): give back the unused cached ones the fence has cleared, once
        pthread_mutex_lock(&N48IC.mu); size_t k = n48ic_flush(&N48IC.c, st); pthread_mutex_unlock(&N48IC.mu);
        if (k) { n48_imp_drain(); r = vkAllocateMemory(N48R.dev, ma, NULL, &mem); }
    }
    if (r != VK_SUCCESS) { *out = VK_NULL_HANDLE; return r; }
    pthread_mutex_lock(&N48IC.mu); n48ic_add(&N48IC.c, id, (uint64_t)(uintptr_t)base, ialloc, (void *)mem, (void *)CFRetain(s)); pthread_mutex_unlock(&N48IC.mu);
    *out = mem; return VK_SUCCESS;
}
static void n48_imp_put(VkDeviceMemory mem) {
    uint64_t st, comp; n48_imp_fence(&st, &comp);
    pthread_mutex_lock(&N48IC.mu); n48ic_unref(&N48IC.c, (void *)mem, n48_now(), st); pthread_mutex_unlock(&N48IC.mu);
    n48_imp_drain();
}
// Base-less (protected) IOSurfaces: log once per id (a ring of the last 64 ids), count every texture.
static void n48_baseless_note(uint32_t id) {
    static uint32_t ring[64]; static unsigned pos; BOOL seen = NO;
    atomic_fetch_add(&N48IC.baseless, 1);
    @synchronized ([Navi48Device class]) {
        for (unsigned i = 0; i < 64; i++) if (ring[i] == id) { seen = YES; break; }
        if (!seen) { ring[pos++ & 63] = id; }
    }
    if (!seen) N48LOG("IOSurface %u has no CPU mapping (protected): GPU-only texture; its contents are not shared with other users of the surface", id);
}
// One line per 10 s tick when something changed (and every 60 s): import cache, base-less textures, classify cache.
static void n48_scan_cc_counters(uint64_t *h, uint64_t *m, uint64_t *mm);
static void n48_imp_report(unsigned tick) {
    uint64_t ch, cm, cmm; n48_scan_cc_counters(&ch, &cm, &cmm);
    uint64_t bl = atomic_load(&N48IC.baseless), sig;
    pthread_mutex_lock(&N48IC.mu);
    n48ic_cache *c = &N48IC.c; sig = c->created + c->reused + c->evIdle + c->evCap + c->evStale + c->evFlush + bl + ch + cm;
    if (sig != N48IC.lastSig || tick % 6 == 5) {
        N48IC.lastSig = sig;
        if (N48IC.on) N48LOG("T1 impcache ON: imports created=%llu reused=%llu live=%llu KiB (%zu entries) cached_unused=%llu KiB (%zu) evicted idle/cap/stale/flush=%llu/%llu/%llu/%llu id_mismatch=%llu; baseless_textures=%llu; classify cache hit/miss/mismatch=%llu/%llu/%llu [%s pid %d, cumulative]",
            (unsigned long long)c->created, (unsigned long long)c->reused, (unsigned long long)(c->liveBytes >> 10), c->n, (unsigned long long)(c->cachedBytes >> 10), n48ic_unused(c), (unsigned long long)c->evIdle, (unsigned long long)c->evCap,
            (unsigned long long)c->evStale, (unsigned long long)c->evFlush, (unsigned long long)c->mismatches, (unsigned long long)bl, (unsigned long long)ch, (unsigned long long)cm, (unsigned long long)cmm, getprogname(), (int)getpid());
        else if (bl || ch || cm) N48LOG("T1 impcache OFF; baseless_textures=%llu; classify cache hit/miss/mismatch=%llu/%llu/%llu [%s pid %d, cumulative]", (unsigned long long)bl, (unsigned long long)ch, (unsigned long long)cm, (unsigned long long)cmm, getprogname(), (int)getpid());
    }
    pthread_mutex_unlock(&N48IC.mu);
}

// One-shot command buffer on the device queue, waited to completion (11e: replaceRegion:, getBytes:). The pool and the queue are
// externally synchronised by N48R.qlock. Returns NO with *err set on failure (a wait that times out leaks the command buffer).
static BOOL n48_oneshot(NSError **err, void (^rec)(VkCommandBuffer cmd)) {
    if (!n48_radv_open(err)) return NO;
    VkCommandBuffer c = VK_NULL_HANDLE; VkFence f = VK_NULL_HANDLE; VkResult r;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = N48R.opool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    n48_qlock();
    r = vkAllocateCommandBuffers(N48R.dev, &ai, &c);
    pthread_mutex_unlock(&N48R.qlock);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(60, [NSString stringWithFormat:@"one-shot vkAllocateCommandBuffers = %d", r]); return NO; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r = vkBeginCommandBuffer(c, &bi);
    if (r == VK_SUCCESS) { rec(c); r = vkEndCommandBuffer(c); }
    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (r == VK_SUCCESS) r = vkCreateFence(N48R.dev, &fc, NULL, &f);
    if (r == VK_SUCCESS) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
        n48_qlock();
        r = vkQueueSubmit(N48R.q, 1, &si, f);
        pthread_mutex_unlock(&N48R.qlock);
    }
    if (r == VK_SUCCESS) {
        int slices = 0; r = VK_TIMEOUT;
        while (r == VK_TIMEOUT && slices++ < 30) r = vkWaitForFences(N48R.dev, 1, &f, VK_TRUE, 2000000000ULL);
    }
    if (r != VK_SUCCESS) { if (err) *err = n48_err(61, [NSString stringWithFormat:@"one-shot submit/wait = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : r == VK_TIMEOUT ? " (TIMEOUT: HUNG?)" : ""]); return NO; }
    vkDestroyFence(N48R.dev, f, NULL);
    n48_qlock(); vkFreeCommandBuffers(N48R.dev, N48R.opool, 1, &c); pthread_mutex_unlock(&N48R.qlock);
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// S5.2a D-copy present (notes/design/NATIVE-S5-FLIP.md section 3, native #12). CoreDisplay's GPUPass renders into 3 IOSurfaces; at the end of each command buffer
// that wrote one, n48Finish appends a GPU copy (import -> a visible-VRAM scanout slot) and the completion block presents that slot (VUPDATE-latched flip) through
// the six radv_darwin_scanout_* exports of the loaded RADV (same per-process N48N connection). The state machine is the pure n48_dispflip.h (host test:
// test-dispflip.c). Default OFF where anything is missing; kill switch /private/tmp/n48m-noflip (read once per process); one mutex around every scanout call.
// ---------------------------------------------------------------------------------------------------------------
#define N48_NOFLIP_FILE "/private/tmp/n48m-noflip"
#define N48_NOPLANES_FILE "/private/tmp/n48m-noplanes"
#define N48_PRESENTALL_FILE "/private/tmp/n48m-presentall"   // native #12 P2 kill switch: present EVERY display write (the pre-final-pass-rule behaviour); polled every 1 s
#define N48_DAMAGE_FILE "/private/tmp/n48m-damage"   // native #12 D2: partial-update emulation + SkyLight composite passes count as frames; DEFAULT OFF; polled every 1 s
#define N48_HOLD_FILE "/private/tmp/n48m-hold"   // native #12 P3 present hold: absent = OFF (today's behaviour); present = ON, content "1".."8" = H in ms, else 3; polled every 1 s
static struct { _Atomic int on, ms; uint64_t held, held_sup, held_pres, hold_ns; } N48H;   // switch state (atomics); counters are written by the present queue under N48S.mu
#define N48S_FOURCC_BGRA 0x42475241u   // 'BGRA'

typedef int (*n48s_query_t)(VkDevice, struct n48n_scan_query *);
typedef int (*n48s_acquire_t)(VkDevice, uint64_t *, uint64_t *);
typedef int (*n48s_register_t)(VkDevice, VkDeviceMemory, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint64_t *);
typedef int (*n48s_present_t)(VkDevice, uint32_t, uint64_t[3]);
typedef int (*n48s_status_t)(VkDevice, struct n48n_scan_status *);
typedef int (*n48s_release_t)(VkDevice, uint64_t[2]);

static struct {
    pthread_mutex_t mu; n48df_t sm; int bound;
    n48s_query_t fq; n48s_acquire_t fa; n48s_register_t fg; n48s_present_t fp; n48s_status_t fs; n48s_release_t fr;
    uint32_t pw, ph, pitch; uint64_t slotBytes; int vtype;
    VkDeviceMemory mem[N48DF_MAX_SLOTS]; VkBuffer buf[N48DF_MAX_SLOTS]; uint32_t sid[N48DF_MAX_SLOTS];
    n48cc cc; int ndisp, ncand; dispatch_source_t timer; uint64_t tStart, tLog; int exitSet;
    struct n48n_scan_status st;
    void *map[N48DF_MAX_SLOTS];   // CRC diagnostic: persistent CPU mapping of each (host-visible) slot, made on first use
    dispatch_queue_t pq; _Atomic uint64_t seq, multi, cbno;   // ONE serial present queue; the frame sequence counter (taken under the submit lock)
} N48S = { .mu = PTHREAD_MUTEX_INITIALIZER };

static const char *n48s_reason_name(int r) {
    static const char *n[] = { "none/released-on-request", "kill-file", "missing-export", "ENOSYS", "plane-geometry", "scanout-error", "plane-lost", "slot-alloc" };
    return r >= 0 && r < (int)(sizeof n / sizeof n[0]) ? n[r] : "?";
}
static BOOL n48s_testmode(void) { const char *e = getenv("N48M_TEST_DISPFLIP"); return n48_allow() && e && !strcmp(e, "1"); }

// Release the plane (idempotent in the kernel) and stop the keep-alive. Called with the mutex held, at most once per process by the state machine's rule.
static void n48s_do_release_locked(void) {
    if (N48S.fr) { uint64_t o[2] = { 0, 0 }; int rc = N48S.fr(N48R.dev, o);
        N48LOG("scanout: release rc %d, console restore verified %llu, plane MC after 0x%llx", rc, (unsigned long long)o[0], (unsigned long long)o[1]); }
    if (N48S.timer) { dispatch_source_cancel(N48S.timer); N48S.timer = nil; }
}
// Fail closed: disable D-copy for the process and release the plane if it was taken.
static void n48s_off_locked(int reason, const char *why) {
    int rel = n48df_fail(&N48S.sm, reason);
    N48LOG("scanout: FAIL CLOSED / OFF (%s): %s", n48s_reason_name(reason), why);
    if (rel) n48s_do_release_locked();
}

// First display-surface candidate: kill switch, dlsym of the six exports, query, visible-pool memory type. Never retried.
static void n48s_bind_locked(void) {
    if (N48S.bound) return;
    N48S.bound = 1; N48S.tStart = n48_now();
    struct stat sb;
    BOOL killed = stat(N48_NOFLIP_FILE, &sb) == 0;
    n48df_init(&N48S.sm, killed);
    N48S.sm.presentall = stat(N48_PRESENTALL_FILE, &sb) == 0;
    if (N48S.sm.presentall) N48LOG("scanout: final-pass rule DISABLED at bind: " N48_PRESENTALL_FILE " exists (every display write is copied and presented)");
    if (killed) { N48LOG("scanout: D-copy OFF: kill switch " N48_NOFLIP_FILE " exists (read once per process)"); return; }
    #define SYM(f, n) do { N48S.f = (void *)dlsym(N48R.lib, "radv_darwin_scanout_" n); if (!N48S.f) { miss = YES; N48LOG("scanout: export radv_darwin_scanout_" n " NOT found in the loaded RADV"); } } while (0)
    BOOL miss = NO;
    SYM(fq, "query"); SYM(fa, "acquire"); SYM(fg, "register"); SYM(fp, "present"); SYM(fs, "status"); SYM(fr, "release");
    #undef SYM
    if (miss) { N48S.fr = NULL; n48s_off_locked(N48DF_R_NOSYM, "D-copy needs all six radv_darwin_scanout_* exports; the kernel v1 copy continues"); return; }
    struct n48n_scan_query q; memset(&q, 0, sizeof q);
    int rc = N48S.fq(N48R.dev, &q);
    if (rc) { char m[96]; snprintf(m, sizeof m, "scanout_query rc %d (-ENOSYS %d = not a native kext / ABI minor < 1)", rc, -ENOSYS); n48s_off_locked(rc == -ENOSYS ? N48DF_R_NOSYS : N48DF_R_ERROR, m); return; }
    N48LOG("scanout: query: %ux%u plane, pitch %u px, hubp_format %u sw_mode %u, refresh %u mHz, flags 0x%x (LIT %d NATIVE %d ACQUIRED %d GEOM_OK %d), console MC 0x%llx",
           q.plane_w, q.plane_h, q.pitch_px, q.hubp_format, q.sw_mode, q.refresh_mhz, q.flags, !!(q.flags & N48N_SCANQ_LIT), !!(q.flags & N48N_SCANQ_NATIVE), !!(q.flags & N48N_SCANQ_ACQUIRED),
           !!(q.flags & N48N_SCANQ_GEOM_OK), (unsigned long long)q.console_mc);
    if (!(q.flags & N48N_SCANQ_GEOM_OK) || !q.plane_w || !q.plane_h || q.pitch_px < q.plane_w) { n48s_off_locked(N48DF_R_GEOM, "the live plane is not linear ARGB8888 with a plausible pitch (GEOM_OK clear)"); return; }
    N48S.pw = q.plane_w; N48S.ph = q.plane_h; N48S.pitch = q.pitch_px * 4;
    N48S.slotBytes = ((uint64_t)N48S.pitch * N48S.ph + 65535) & ~(uint64_t)65535;
    N48S.vtype = n48_find_mem(0xFFFFFFFFu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < N48R.mp.memoryTypeCount; i++)
        N48LOG("scanout: RADV memory type %u: flags 0x%x heap %u (%llu MiB)%s", i, N48R.mp.memoryTypes[i].propertyFlags, N48R.mp.memoryTypes[i].heapIndex,
               (unsigned long long)(N48R.mp.memoryHeaps[N48R.mp.memoryTypes[i].heapIndex].size >> 20), (int)i == N48S.vtype ? "  <- visible-pool VRAM (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT), used for the scanout slots" : "");
    if (N48S.vtype < 0) { n48s_off_locked(N48DF_R_ALLOC, "no DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT memory type in RADV's list"); return; }
    N48LOG("scanout: bound: plane %ux%u pitch %u B, slot %llu B (rounded to 64 KiB), visible-pool memory type %d; visible-pool free space is NOT queryable here (VK_EXT_memory_budget not used): SUSPECTED unknown, the first slot allocation reports it by succeeding or failing", N48S.pw, N48S.ph, N48S.pitch, (unsigned long long)N48S.slotBytes, N48S.vtype);
}

// Caller backtrace test (once per texture creation): any frame resolving (dladdr) to a symbol with both "DisplaySurface" and "GetMTLTexture" (CoreDisplay's
// DisplaySurface::GetMTLTexture). The first few candidates log their frames so a miss can be diagnosed.
static BOOL n48s_bt_signal(BOOL logframes) {
    NSArray<NSNumber *> *a = [NSThread callStackReturnAddresses];
    BOOL hit = NO; char line[1600]; size_t o = 0; line[0] = 0;
    for (NSUInteger i = 0; i < a.count; i++) {
        Dl_info di; void *p = (void *)(uintptr_t)a[i].unsignedLongLongValue;
        if (!dladdr(p, &di)) continue;
        const char *sn = di.dli_sname ? di.dli_sname : "";
        if (strstr(sn, "DisplaySurface") && strstr(sn, "GetMTLTexture")) hit = YES;
        if (logframes && i < 16 && o + 200 < sizeof line) { const char *im = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL; o += (size_t)snprintf(line + o, sizeof line - o, " [%lu]%s:%.70s", (unsigned long)i, im ? im + 1 : "?", sn); }
    }
    if (logframes) N48LOG("scanout: caller backtrace:%s", line);
    return hit;
}

// Called at the end of -initWithDevice:descriptor:iosurface:plane: (RADV is open). YES = this texture is one of CoreDisplay's display surfaces.
// P4: the verdict (positive and negative) is cached per IOSurface id (key id + base + size + pitch + alloc, 64 entries, LRU) so the backtrace test (dladdr per frame) runs once per surface.
static void n48_scan_cc_counters(uint64_t *h, uint64_t *m, uint64_t *mm) {
    pthread_mutex_lock(&N48S.mu); *h = N48S.cc.hits; *m = N48S.cc.misses; *mm = N48S.cc.mismatches; pthread_mutex_unlock(&N48S.mu);
}
static BOOL n48s_classify(size_t w, size_t h, uint32_t spf, NSUInteger usage, size_t bpr, size_t alloc, unsigned sid, const void *base) {
    if (spf != N48S_FOURCC_BGRA || !(usage & MTLTextureUsageRenderTarget) || w < 1024 || h < 600) return NO;   // cheap prefilter: no lock, no log
    BOOL ok = NO;
    pthread_mutex_lock(&N48S.mu);
    n48s_bind_locked();
    if (N48S.sm.state != N48DF_OFF) {
        int cv = 0;
        if (n48cc_lookup(&N48S.cc, sid, (uint64_t)(uintptr_t)base, w, h, bpr, alloc, &cv)) ok = cv != 0;
        else {
            BOOL size = w == N48S.pw && h == N48S.ph, pitch = bpr == N48S.pitch && alloc >= (size_t)N48S.pitch * N48S.ph;
            // Reviewer: the backtrace only when the geometry can match, and only POSITIVE verdicts are cached: a display surface whose first texture came from
            // another caller must still be found when CoreDisplay's GetMTLTexture asks for it later (a cached NO would freeze the screen on that surface).
            BOOL bt = (size && pitch) ? n48s_bt_signal(N48S.ncand++ < 3) : NO, test = n48s_testmode();
            if (size && pitch) N48LOG("scanout: display-surface candidate: IOSurface id %u %zux%zu bpr %zu, signals: format_BGRA=1 size_eq_plane=%d usage_RenderTarget=1 pitch_eq_plane=%d caller_DisplaySurface_GetMTLTexture=%d%s",
                   sid, w, h, bpr, size, pitch, bt, test ? " (N48M_TEST_DISPFLIP: backtrace signal waived)" : "");
            ok = size && pitch && (bt || test);
            if (ok) n48cc_put(&N48S.cc, sid, (uint64_t)(uintptr_t)base, w, h, bpr, alloc, 1);
            if (ok) { N48S.ndisp++; N48LOG("scanout: display surface #%d identified: IOSurface id %u (expect 3 per WindowServer; counted once per surface id, later textures of the same surface hit the classify cache)", N48S.ndisp, sid); }
        }
    }
    pthread_mutex_unlock(&N48S.mu);
    return ok;
}

static BOOL n48s_status_locked(struct n48n_scan_status *st) {
    int rc = N48S.fs(N48R.dev, st);
    if (rc) { char m[64]; snprintf(m, sizeof m, "scanout_status rc %d", rc); n48s_off_locked(N48DF_R_ERROR, m); return NO; }
    if (!st->acquired || (st->flags & (N48N_SCANST_RESTORING | N48N_SCANST_STORM))) {
        char m[96]; snprintf(m, sizeof m, "the kernel ended the acquisition (acquired %u flags 0x%x watchdog_restores %llu)", st->acquired, st->flags, (unsigned long long)st->watchdog_restores);
        n48s_off_locked(N48DF_R_PLANELOST, m); return NO; }
    return YES;
}
static _Atomic uint64_t n48s_imgcopies;   // P6: display copies recorded from an image source (a display surface with no CPU mapping)
static void n48s_log_counters_locked(const struct n48n_scan_status *st) {
    N48LOG("scanout: counters: presents %llu (kernel presents %llu) latched %llu replaced %llu repeats %llu refused %llu watchdog_restores %llu drops %llu gpu_fail %llu stale_skipped %llu superseded_skipped %llu multi_disp_cbs %llu inflight %d display_surfaces %d hold %s H %d held %llu held_superseded %llu held_presented %llu hold_total_ms %.1f hold_mean_ms %.3f image_source_copies %llu",
           (unsigned long long)N48S.sm.presents, (unsigned long long)st->presents, (unsigned long long)st->latched, (unsigned long long)st->replaced, (unsigned long long)st->repeats,
           (unsigned long long)st->refused, (unsigned long long)st->watchdog_restores, (unsigned long long)N48S.sm.drops, (unsigned long long)N48S.sm.gpu_fail, (unsigned long long)N48S.sm.stale, (unsigned long long)N48S.sm.superseded, (unsigned long long)atomic_load(&N48S.multi), n48df_inflight_count(&N48S.sm), N48S.ndisp,
           atomic_load(&N48H.on) ? "ON" : "off", atomic_load(&N48H.ms), (unsigned long long)N48H.held, (unsigned long long)N48H.held_sup, (unsigned long long)N48H.held_pres, (double)N48H.hold_ns / 1e6, N48H.held ? (double)N48H.hold_ns / 1e6 / (double)N48H.held : 0.0, (unsigned long long)atomic_load(&n48s_imgcopies));
}
static BOOL n48s_crc_on(void);
static void n48s_crc_summary(void);
// native #12 P1: write-class summary of the last 10 s (interval = cumulative minus the previous snapshot), plus the cumulative nonfinal_skipped. Called with the mutex held.
static void n48s_class_summary_locked(void) {
    static uint64_t pc[N48DF_NCLS], pn[N48DF_NCLS];
    char l[512]; size_t o = 0; uint64_t tn = 0;
    for (int i = 0; i < N48DF_NCLS; i++) { o += (size_t)snprintf(l + o, sizeof l - o, " %s %llu(skipped %llu)", n48df_cls_name[i], (unsigned long long)(N48S.sm.cls[i] - pc[i]), (unsigned long long)(N48S.sm.nonfinal[i] - pn[i])); pc[i] = N48S.sm.cls[i]; pn[i] = N48S.sm.nonfinal[i]; tn += N48S.sm.nonfinal[i]; }
    N48LOG("scanout: display writes by class (10 s):%s; nonfinal_skipped total %llu; nonscanout_skipped %llu; presentall %d", l, (unsigned long long)tn, (unsigned long long)N48S.sm.nonscan_total, N48S.sm.presentall);
    for (int i = 0; i < N48DF_MAX_SURF; i++) if (N48S.sm.nonscan_skip[i]) N48LOG("scanout: nonscanout_skipped sid %u: %llu", N48S.sm.nonscan_sid[i], (unsigned long long)N48S.sm.nonscan_skip[i]);
}
// native #12 P1/P2 + D1: called from -n48Finish for the command buffer's chosen display write. Counts its class and its extents, logs the first 20 with the pipeline names and the
// first 30 extent samples, and returns whether it may take a slot.
static BOOL n48s_disp_account(uint32_t mask, uint32_t sid, const char *fns, uint64_t cbno, const n48df_wr_t *wr) {
    pthread_mutex_lock(&N48S.mu);
    int nf0 = N48S.sm.nflag_log, ok = n48df_account_sid(&N48S.sm, mask, sid), c = n48df_class(mask);
    if (N48S.sm.nflag_log > nf0) N48LOG("scanout: scanout-surface flag set #%d: surface %u (a final-GPUPass draw targeted it)", N48S.sm.nflag_log, sid);
    uint32_t W = N48S.pw, H = N48S.ph;
    if (wr) n48df_dmg_account(&N48S.sm, c, wr, W, H);
    static int logged; 
    if (logged < 20) { logged++; N48LOG("scanout: display write #%d: cb %llu surface %u class %s mask 0x%x -> %s; pipelines (vertex/fragment): %s", logged, (unsigned long long)cbno, sid, n48df_cls_name[c], mask, ok ? (c == N48DF_C_FINAL ? "FINAL: copy+present" : "scanout composite/presentall: copy+present") : "NON-FINAL: no copy, no present", fns && fns[0] ? fns : "(none)"); atomic_store(&n48_dw_logged, logged); }
    static int dlog;
    if (wr && dlog < 30 && W && H) {
        dlog++; n48df_rect_t ra, rp; int ka = n48df_dmg_resolve(&wr->all, W, H, &ra), kp = n48df_dmg_resolve(&wr->pres, W, H, &rp);
        static const char *kn[3] = { "none", "FULL", "partial" };
        N48LOG("scanout: DIRTY #%d: cb %llu surface %u class %s: all draws %u (full %u, partial %u) -> %s [%d,%d)-[%d,%d) (%lld px = %.1f%%)%s; presentable -> %s [%d,%d)-[%d,%d); first draw viewport %.0f,%.0f %.0fx%.0f scissor %lld,%lld %lldx%lld; sampled source: %s%s id %u %ux%u",
            dlog, (unsigned long long)cbno, sid, n48df_cls_name[c], wr->all.draws, wr->all.full_draws, wr->all.part_draws, kn[ka], ra.x0, ra.y0, ra.x1, ra.y1, (long long)n48df_rect_area(ra), 100.0 * (double)n48df_rect_area(ra) / ((double)W * H),
            wr->all.full ? " (a clear/blit/compute wrote the whole surface)" : "", kn[kp], rp.x0, rp.y0, rp.x1, rp.y1, wr->vp[0], wr->vp[1], wr->vp[2], wr->vp[3], (long long)wr->sc[0], (long long)wr->sc[1], (long long)wr->sc[2], (long long)wr->sc[3],
            wr->src_kind == 0 ? "none" : wr->src_kind == 1 ? "other IOSurface (not a display surface)" : wr->src_kind == 2 ? "ANOTHER display surface" : "THE TARGET ITSELF", "", wr->src_sid, wr->src_w, wr->src_h);
    }
    pthread_mutex_unlock(&N48S.mu);
    return ok != 0;
}
// D1 summary of the last 10 s: per class the cb extent kinds, plus the GPUPass source-texture kinds and the D2 counters. Mutex held.
static void n48s_dirty_summary_locked(void) {
    static uint64_t pk[N48DF_NCLS][3], pa[N48DF_NCLS], pdf[N48DF_NCLS], pdp[N48DF_NCLS], ps[4], ppc[N48DF_NCLS];
    char l[900]; size_t o = 0; double scr = (double)N48S.pw * N48S.ph;
    for (int i = 0; i < N48DF_NCLS; i++) {
        uint64_t n0 = N48S.sm.dk[i][0] - pk[i][0], n1 = N48S.sm.dk[i][1] - pk[i][1], n2 = N48S.sm.dk[i][2] - pk[i][2], ar = N48S.sm.dk_area[i] - pa[i];
        uint64_t df = N48S.sm.dd_full[i] - pdf[i], dp = N48S.sm.dd_part[i] - pdp[i], pr = N48S.sm.pres_cls[i] - ppc[i];
        if (n0 + n1 + n2 + pr) o += (size_t)snprintf(l + o, sizeof l - o, " [%s: cbs full %llu partial %llu none %llu, avg partial area %.1f%%, draws full %llu partial %llu, presented %llu]", n48df_cls_name[i], (unsigned long long)n1, (unsigned long long)n2, (unsigned long long)n0,
            n2 && scr > 0 ? 100.0 * (double)ar / (double)n2 / scr : 0.0, (unsigned long long)df, (unsigned long long)dp, (unsigned long long)pr);
        for (int k = 0; k < 3; k++) pk[i][k] = N48S.sm.dk[i][k];
        pa[i] = N48S.sm.dk_area[i]; pdf[i] = N48S.sm.dd_full[i]; pdp[i] = N48S.sm.dd_part[i]; ppc[i] = N48S.sm.pres_cls[i];
    }
    N48LOG("scanout: display extents (10 s):%s; GPUPass samples: none %llu other-IOSurface %llu another-display-surface %llu the-target-itself %llu", o ? l : " (no display writes)",
        (unsigned long long)(N48S.sm.srck[0] - ps[0]), (unsigned long long)(N48S.sm.srck[1] - ps[1]), (unsigned long long)(N48S.sm.srck[2] - ps[2]), (unsigned long long)(N48S.sm.srck[3] - ps[3]));
    for (int k = 0; k < 4; k++) ps[k] = N48S.sm.srck[k];
    if (N48S.sm.damage || N48S.sm.dm_part || N48S.sm.dm_full || N48S.sm.dm_carry)
        N48LOG("scanout: damage mode %s: partial frames %llu (empty rect %llu) full-copy frames %llu chain restarts %llu carried drops %llu poisoned %llu; region copies %llu, surface rows copied %llu px; chain head slot %d",
            N48S.sm.damage ? "ON" : "off", (unsigned long long)N48S.sm.dm_part, (unsigned long long)N48S.sm.dm_empty, (unsigned long long)N48S.sm.dm_full, (unsigned long long)N48S.sm.dm_restart, (unsigned long long)N48S.sm.dm_carry,
            (unsigned long long)N48S.sm.chain_invalid, (unsigned long long)N48S.sm.dm_regions, (unsigned long long)N48S.sm.dm_copy_px, N48S.sm.head);
}
static void n48s_tick(void) {   // 1 s keep-alive (a status call counts as activity: no 5 s idle restore); counters every 10 s
    (void)n48s_crc_on();   // keeps the 2 s file poll alive on an idle screen
    {   // P3 present hold switch (1 s tick): absent = off; present = on, H from the content (decimal 1..8 ms, else 3); a log line on every change
        int on = 0, ms = N48DF_HOLD_DEFAULT_MS; int fd = open(N48_HOLD_FILE, O_RDONLY);
        if (fd >= 0) { char b[32]; ssize_t n = read(fd, b, sizeof b); close(fd); on = 1; ms = n48df_hold_parse(b, n > 0 ? (int)n : 0); }
        int pon = atomic_load(&N48H.on), pms = atomic_load(&N48H.ms);
        if (on != pon || (on && ms != pms)) {
            if (on) atomic_store(&N48H.ms, ms);
            atomic_store(&N48H.on, on);
            N48LOG("scanout: present hold %s (" N48_HOLD_FILE " %s)%s%d%s", on ? "ENABLED" : "DISABLED", on ? "exists" : "removed", on ? ": a display present waits until its commit + " : "", on ? ms : 0, on ? " ms so a later command buffer for the same surface supersedes it" : "");
        }
    }
    pthread_mutex_lock(&N48S.mu);
    { struct stat sb; int pa = stat(N48_PRESENTALL_FILE, &sb) == 0; if (pa != N48S.sm.presentall) { N48S.sm.presentall = pa; N48LOG("scanout: final-pass rule %s (" N48_PRESENTALL_FILE " %s)", pa ? "DISABLED: every display write is copied and presented" : "ENABLED: only GPUPass frames are presented", pa ? "exists" : "removed"); } }
    { struct stat sb; int dm = stat(N48_DAMAGE_FILE, &sb) == 0; if (dm != N48S.sm.damage) { N48S.sm.damage = dm; N48S.sm.head = -1; N48LOG("scanout: partial-update emulation %s (" N48_DAMAGE_FILE " %s)", dm ? "ENABLED: SkyLight composite passes count as frames; each frame = previous frame + its dirty rectangle" : "DISABLED: full-surface copies", dm ? "exists" : "removed"); } }
    if (N48S.sm.state == N48DF_ACTIVE && n48s_status_locked(&N48S.st)) {
        uint64_t now = n48_now();
        if (now - N48S.tLog >= 10ull * 1000000000ull) { N48S.tLog = now; n48s_log_counters_locked(&N48S.st); n48s_class_summary_locked(); n48s_dirty_summary_locked(); n48s_crc_summary(); }
    }
    pthread_mutex_unlock(&N48S.mu);
}
static void n48s_atexit(void) {   // best effort; N48N close restores the console FIRST anyway
    if (pthread_mutex_trylock(&N48S.mu)) return;
    if (N48S.sm.acquired && !N48S.sm.released) { N48S.sm.released = 1; n48s_do_release_locked(); }
    pthread_mutex_unlock(&N48S.mu);
}
static void n48s_free_slots(void) {
    for (int i = 0; i < N48DF_MAX_SLOTS; i++) {
        if (N48S.buf[i]) { vkDestroyBuffer(N48R.dev, N48S.buf[i], NULL); N48S.buf[i] = VK_NULL_HANDLE; }
        if (N48S.mem[i]) { vkFreeMemory(N48R.dev, N48S.mem[i], NULL); N48S.mem[i] = VK_NULL_HANDLE; }
        N48S.map[i] = NULL;
    }
}
// Lazy acquire at the first write of a display surface: acquire the plane, 3 slots in visible VRAM, register them. Mutex held.
static BOOL n48s_acquire_locked(void) {
    if (N48S.sm.state != N48DF_UNBOUND) return N48S.sm.state == N48DF_ACTIVE;
    uint64_t cmc = 0, fc = 0;
    int rc = N48S.fa(N48R.dev, &cmc, &fc);
    if (rc) { char m[64]; snprintf(m, sizeof m, "scanout_acquire rc %d", rc); n48s_off_locked(N48DF_R_ERROR, m); return NO; }
    n48df_mark_acquired(&N48S.sm);
    int i = 0; char why[160] = "";
    for (; i < N48DF_MAX_SLOTS; i++) {
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N48S.slotBytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkResult r = vkCreateBuffer(N48R.dev, &bc, NULL, &N48S.buf[i]);
        if (r != VK_SUCCESS) { N48S.buf[i] = VK_NULL_HANDLE; snprintf(why, sizeof why, "slot %d vkCreateBuffer = %d", i, r); break; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(N48R.dev, N48S.buf[i], &mr);
        int mt = n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) { snprintf(why, sizeof why, "slot %d: no visible-VRAM type in the buffer's bits 0x%x", i, mr.memoryTypeBits); break; }
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = (mr.size + 65535) & ~(VkDeviceSize)65535, .memoryTypeIndex = (uint32_t)mt };
        r = vkAllocateMemory(N48R.dev, &ma, NULL, &N48S.mem[i]);
        if (r != VK_SUCCESS) { N48S.mem[i] = VK_NULL_HANDLE; snprintf(why, sizeof why, "slot %d vkAllocateMemory(%llu B, type %d) = %d (visible pool full?)", i, (unsigned long long)ma.allocationSize, mt, r); break; }
        r = vkBindBufferMemory(N48R.dev, N48S.buf[i], N48S.mem[i], 0);
        if (r != VK_SUCCESS) { snprintf(why, sizeof why, "slot %d vkBindBufferMemory = %d", i, r); break; }
        uint64_t mc = 0; uint32_t sl = 0;
        rc = N48S.fg(N48R.dev, N48S.mem[i], 0, N48S.pitch, N48S.pw, N48S.ph, &sl, &mc);
        if (rc) { snprintf(why, sizeof why, "slot %d scanout_register rc %d", i, rc); break; }
        if (sl >= N48N_SCAN_MAX_SLOTS) { snprintf(why, sizeof why, "slot %d: kernel slot id %u out of range", i, sl); break; }
        N48S.sid[i] = sl;
        N48LOG("scanout: slot %d: memory type %d, %llu B (buffer req %llu align %llu), kernel slot id %u, MC 0x%llx (64 KiB aligned: %d)", i, mt, (unsigned long long)ma.allocationSize,
               (unsigned long long)mr.size, (unsigned long long)mr.alignment, sl, (unsigned long long)mc, (mc & 0xFFFF) == 0);
    }
    if (i < N48DF_MAX_SLOTS) { n48s_off_locked(N48DF_R_ALLOC, why); n48s_free_slots(); return NO; }   // released first: nothing was submitted, so freeing is safe
    n48df_activate(&N48S.sm, N48DF_MAX_SLOTS);
    N48S.tLog = n48_now();
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(t, ^{ n48s_tick(); });
    N48S.timer = t; dispatch_resume(t);
    if (!N48S.exitSet) { N48S.exitSet = 1; atexit(n48s_atexit); }
    N48LOG("scanout ACQUIRED, %d slots (console MC 0x%llx, frame count %llu, %ux%u pitch %u)", N48DF_MAX_SLOTS, (unsigned long long)cmc, (unsigned long long)fc, N48S.pw, N48S.ph, N48S.pitch);
    return YES;
}
static int n48s_damage_on(void) { pthread_mutex_lock(&N48S.mu); int d = N48S.sm.damage; pthread_mutex_unlock(&N48S.mu); return d; }
// At command-buffer encode: a free slot (fresh kernel status says REUSABLE, none in flight, not pinned, not the chain head while damage is on) or -1 (drop counted, or D-copy off).
// `wr` = what this cb wrote to the display surface (for the D2 rectangle). The plan says full copy or base slot + region.
static int n48s_plan(const n48df_wr_t *wr, n48df_plan_t *pl) {
    int slot = -1; pl->slot = -1; pl->base = -1; pl->id = pl->baseid = 0; pl->kind = N48DF_K_FULL;
    pthread_mutex_lock(&N48S.mu);
    if (N48S.sm.state == N48DF_UNBOUND) n48s_acquire_locked();
    if (N48S.sm.state == N48DF_ACTIVE && n48s_status_locked(&N48S.st)) {
        uint32_t fl[N48DF_MAX_SLOTS];
        for (int i = 0; i < N48DF_MAX_SLOTS; i++) fl[i] = N48S.st.slot[N48S.sid[i]].flags;
        n48df_rect_t r = { 0, 0, (int32_t)N48S.pw, (int32_t)N48S.ph }; int k = wr ? n48df_dmg_resolve(&wr->pres, N48S.pw, N48S.ph, &r) : N48DF_K_FULL;
        if (k == N48DF_K_NONE) k = N48DF_K_FULL;   // a presentable cb with no recorded draw rectangle: unknown, copy it all
        slot = n48df_plan(&N48S.sm, fl, k, r, pl);
        if (slot >= 0 && pl->base >= 0) { N48S.sm.dm_copy_px += (uint64_t)n48df_rect_area(pl->rect); }
    }
    pthread_mutex_unlock(&N48S.mu);
    return slot;
}
static void n48s_record_copy(VkCommandBuffer cmd, VkBuffer src, const n48df_plan_t *pl) {
    int slot = pl->slot; VkDeviceSize all = (VkDeviceSize)N48S.pitch * N48S.ph;
    if (pl->base < 0) { VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, src, N48S.buf[slot], 1, &bc); return; }
    // D2: the slot first gets the previous frame (VRAM -> VRAM), then the dirty rows of the surface on top of it.
    VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, N48S.buf[pl->base], N48S.buf[slot], 1, &bc);
    static n48df_region_t rg[2048]; static VkBufferCopy vc[2048]; static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;   // recording is concurrent across command buffers: one scratch under a lock
    pthread_mutex_lock(&rmu);
    uint32_t n = n48df_regions(pl->rect, N48S.pw, N48S.ph, N48S.pitch, 4, rg, 2048);
    if (n == UINT32_MAX) { n = 1; vc[0] = (VkBufferCopy){ 0, 0, all }; }   // cannot happen with 1440 rows; fail towards the whole frame
    else for (uint32_t i = 0; i < n; i++) vc[i] = (VkBufferCopy){ rg[i].src, rg[i].dst, rg[i].size };
    if (n) { VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // WAW: the region copy must land after the base copy
        vkCmdCopyBuffer(cmd, src, N48S.buf[slot], n, vc); }
    N48S.sm.dm_regions += n;   // statistic only (a racy increment is acceptable)
    pthread_mutex_unlock(&rmu);
}
// ---- opt-in per-frame CRC diagnostic (NATIVE-S5-FLIP.md "Tearing diagnostic (CRC)"). Off unless /private/tmp/n48m-crc exists (polled every 2 s, WindowServer only; the
// file may hold a row step (default 64) and the word "noslot" = skip the slot read, which is the expensive part: the slot is write-combined BAR memory). Per frame that carries a copy: A1 = sampled CRC of the display surface's import memory after the fence, B = the same rows of the
// slot, A2 = the surface again after B, C = the surface again just before scanout_present. Reads only; nothing here changes what is copied or presented.
#define N48_CRC_FILE "/private/tmp/n48m-crc"
typedef struct { IOSurfaceRef ios; const uint8_t *base; size_t pitch, rowbytes; uint32_t h, step, sid; int slot, unst; uint64_t seq; n48crc_samp_t a1; } n48crc_rec_t;
static struct { pthread_mutex_t mu; n48crc_stats_t st; _Atomic int on; _Atomic uint32_t step; _Atomic uint64_t next; _Atomic uint64_t logged_p; _Atomic int noslot; int warmed, mapfail; struct { uint32_t sid; uint64_t seq; } surf[8]; } N48C = { .mu = PTHREAD_MUTEX_INITIALIZER };
static BOOL n48s_crc_on(void) {
    if (!n48_is_ws()) return NO;
    uint64_t now = n48_now(), nx = atomic_load(&N48C.next);
    if (now >= nx && atomic_compare_exchange_strong(&N48C.next, &nx, now + 2000000000ull)) {
        int fd = open(N48_CRC_FILE, O_RDONLY | O_CLOEXEC), was = atomic_load(&N48C.on);
        if (fd >= 0) {
            char b[48] = { 0 }; ssize_t n = read(fd, b, sizeof b - 1); close(fd); long v = n > 0 ? atol(b) : 0;
            atomic_store(&N48C.noslot, n > 0 && strstr(b, "noslot") != NULL);
            atomic_store(&N48C.step, (v >= 1 && v <= 1440) ? (uint32_t)v : N48CRC_DEFAULT_STEP);
            if (!was) { pthread_mutex_lock(&N48C.mu); if (!N48C.warmed) { N48C.warmed = 1; (void)n48crc32(0, "", 0); } memset(&N48C.st, 0, sizeof N48C.st); atomic_store(&N48C.logged_p, 0); pthread_mutex_unlock(&N48C.mu);
                atomic_store(&N48C.on, 1); N48LOG("scanout: CRC diagnostic ENABLED (" N48_CRC_FILE " exists; row step %u%s; first 20 mismatches per kind are logged, summary every 10 s)", atomic_load(&N48C.step), atomic_load(&N48C.noslot) ? ", slot read OFF (noslot)" : ""); }
        } else if (was) { atomic_store(&N48C.on, 0); N48LOG("scanout: CRC diagnostic DISABLED (" N48_CRC_FILE " removed)"); }
    }
    return atomic_load(&N48C.on) != 0;
}
// Called from the 10 s counters line: one summary line, then the interval counters restart.
static void n48s_crc_summary(void) {
    pthread_mutex_lock(&N48C.mu);
    if (N48C.st.checked || N48C.st.present_checked || atomic_load(&N48C.on)) { char l[400]; n48crc_summary(&N48C.st, l, sizeof l); N48LOG("scanout: CRC summary (10 s): %s", l); }
    uint64_t lg = N48C.st.logged; memset(&N48C.st, 0, sizeof N48C.st); N48C.st.logged = lg;
    pthread_mutex_unlock(&N48C.mu);
}
// Per display surface: the highest submission sequence of any command buffer that wrote it (called under the submit lock).. A frame is "later-overwritten" when this holds a higher sequence than its own.
static void n48s_crc_note(uint32_t sid, uint64_t seq) {
    pthread_mutex_lock(&N48C.mu); int k = -1;
    for (int i = 0; i < 8; i++) if (N48C.surf[i].sid == sid) { k = i; break; }
    if (k < 0) for (int i = 0; i < 8; i++) if (!N48C.surf[i].sid) { k = i; N48C.surf[i].sid = sid; break; }
    if (k >= 0 && seq > N48C.surf[k].seq) N48C.surf[k].seq = seq;
    pthread_mutex_unlock(&N48C.mu);
}
// C: immediately before scanout_present (the present block holds the scanout mutex).
static void n48s_crc_present(const n48crc_rec_t *r) {
    uint8_t scratch[16384]; if (r->rowbytes > sizeof scratch) return;
    n48crc_samp_t c; uint64_t t0 = n48_now(); n48crc_sample(&c, r->base, r->pitch, r->rowbytes, r->h, r->step, scratch); uint64_t dt = n48_now() - t0;
    pthread_mutex_lock(&N48C.mu);
    int later = 0; for (int i = 0; i < 8; i++) if (N48C.surf[i].sid == r->sid) { later = N48C.surf[i].seq > r->seq; break; }
    int chg = n48crc_account_present(&N48C.st, &r->a1, &c, dt, r->unst, later);
    pthread_mutex_unlock(&N48C.mu);
    if (chg && atomic_fetch_add(&N48C.logged_p, 1) < 20) {
        uint32_t d[8]; int nd = n48crc_diff(&r->a1, &c, d, 8);
        N48LOG("scanout: CRC MISMATCH src-changed-before-present: seq %llu surface %u slot %d (%s), %d sampled rows differ (step %u), first y: %u %u %u %u", (unsigned long long)r->seq, r->sid, r->slot, later ? "a LATER command buffer for this surface was submitted" : "NO later command buffer: written outside our command buffers", nd, r->step, d[0], nd > 1 ? d[1] : 0, nd > 2 ? d[2] : 0, nd > 3 ? d[3] : 0);
    }
}
static void n48s_crc_free(n48crc_rec_t *r) { if (!r) return; if (r->ios) CFRelease(r->ios); free(r); }

// The command buffer that carried the copy into `slot` ended (fence result r) or never ran: present on VK_SUCCESS only. `rec` (nil unless the CRC diagnostic runs) is consumed.

// ---------------------------------------------------------------------------------------------------------------
// native #12 Stage 0b (notes/design/NATIVE-S5-TXN.md): per-process command-buffer / present log and the cross-queue RAW-inversion counter.
// OFF unless /private/tmp/n48m-cblog exists (polled every 1 s by its own timer; removed -> the file is closed). Output goes to
// /private/var/tmp/n48m-cblog.<pid>.txt (a plain file, written with write(2) line by line: no os_log, so nothing is lost to log throttling), capped at 50 MB.
// Observation only: nothing it logs changes what is submitted or presented. All times are CLOCK_UPTIME_RAW ns (n48_now); "K" lines pair it with CLOCK_REALTIME every second.
// IOSurface ids are IOSurfaceGetID, the same ids the DTrace transaction log (tools/pc/txnlog.d) prints. Line types (key=value, one per line):
//   H header (pid, up, wall)     K clock pair + 10 s counters       C commit      S submit      F fence done      P present      X present skipped
//   I inversion case (first 20 per enable)       E end
// ---------------------------------------------------------------------------------------------------------------
#define N48_CBLOG_FLAG "/private/tmp/n48m-cblog"
#define N48_CBLOG_CAP (50ull << 20)
#define N48_CBLOG_INV_LOGGED 20
static struct { _Atomic int on; int fd; uint64_t bytes; int capped; uint64_t inv_logged; unsigned tick; pthread_mutex_t mu; n48cb_t trk; } N48CB = { .fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uint64_t n48cb_ids;
static void n48cbl_line_locked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void n48cbl_line_locked(const char *fmt, ...) {
    if (N48CB.fd < 0) return;
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (N48CB.bytes + (uint64_t)n > N48_CBLOG_CAP) {
        const char *m = "E reason=cap\n"; (void)!write(N48CB.fd, m, strlen(m));
        N48CB.capped = 1; atomic_store(&N48CB.on, 0); close(N48CB.fd); N48CB.fd = -1;
        N48LOG("cblog: 50 MB cap reached, logging stopped (remove " N48_CBLOG_FLAG " and recreate it to start a new file)");
        return;
    }
    ssize_t w = write(N48CB.fd, b, (size_t)n); if (w > 0) N48CB.bytes += (uint64_t)w;
}
static void n48cbl_sids(char *o, size_t cap, const uint32_t *a, uint32_t k) {
    size_t l = 0; o[0] = 0;
    if (!k) { snprintf(o, cap, "-"); return; }
    for (uint32_t i = 0; i < k && l + 12 < cap; i++) l += (size_t)snprintf(o + l, cap - l, i ? ",%u" : "%u", a[i]);
}
static uint64_t n48cbl_wall(void) { return clock_gettime_nsec_np(CLOCK_REALTIME); }
static void n48cbl_poll(void) {   // 1 s timer
    struct stat sb; int want = stat(N48_CBLOG_FLAG, &sb) == 0;
    pthread_mutex_lock(&N48CB.mu);
    if (want && !atomic_load(&N48CB.on) && !N48CB.capped) {
        char path[96]; snprintf(path, sizeof path, "/private/var/tmp/n48m-cblog.%d.txt", (int)getpid());
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) { static int warned; if (!warned) { warned = 1; N48LOG("cblog: cannot open %s: errno %d", path, errno); } }
        else {
            N48CB.fd = fd; N48CB.bytes = 0; N48CB.inv_logged = 0; N48CB.tick = 0; n48cb_init(&N48CB.trk);
            n48cbl_line_locked("H pid=%d up=%llu wall=%llu cap=%llu fmt=1", (int)getpid(), (unsigned long long)n48_now(), (unsigned long long)n48cbl_wall(), (unsigned long long)N48_CBLOG_CAP);
            atomic_store(&N48CB.on, 1);
            N48LOG("cblog: ENABLED (" N48_CBLOG_FLAG " exists) -> %s", path);
        }
    } else if (!want && (atomic_load(&N48CB.on) || N48CB.capped)) {
        if (atomic_load(&N48CB.on)) { n48cbl_line_locked("E reason=flag-removed"); atomic_store(&N48CB.on, 0); close(N48CB.fd); N48CB.fd = -1; N48LOG("cblog: DISABLED (flag removed), %llu bytes written", (unsigned long long)N48CB.bytes); }
        N48CB.capped = 0;
    } else if (atomic_load(&N48CB.on)) {
        n48cbl_line_locked("K up=%llu wall=%llu", (unsigned long long)n48_now(), (unsigned long long)n48cbl_wall());
        if (++N48CB.tick % 10 == 0) {
            uint64_t sub, inv; n48cb_take_interval(&N48CB.trk, &sub, &inv);
            n48cbl_line_locked("K10 submits=%llu inversions=%llu inv_total=%llu inv_cbs=%llu commits=%llu evicted=%llu no_commit=%llu bytes=%llu", (unsigned long long)sub, (unsigned long long)inv,
                (unsigned long long)N48CB.trk.inv_total, (unsigned long long)N48CB.trk.inv_cbs, (unsigned long long)N48CB.trk.commits, (unsigned long long)N48CB.trk.evicted, (unsigned long long)N48CB.trk.no_commit, (unsigned long long)N48CB.bytes);
            N48LOG("cblog (10 s): submits %llu cross-queue RAW inversions %llu (total %llu in %llu submits; evicted %llu, submitted-without-commit %llu); file %llu bytes",
                (unsigned long long)sub, (unsigned long long)inv, (unsigned long long)N48CB.trk.inv_total, (unsigned long long)N48CB.trk.inv_cbs, (unsigned long long)N48CB.trk.evicted, (unsigned long long)N48CB.trk.no_commit, (unsigned long long)N48CB.bytes);
        }
    }
    pthread_mutex_unlock(&N48CB.mu);
}
static dispatch_source_t n48cbl_timer;   // kept alive for the process (a local would be released by ARC and the timer would never fire)
static void n48cbl_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(t, ^{ n48cbl_poll(); });
        n48cbl_timer = t;
        dispatch_resume(t);
    });
}
static inline int n48cbl_on(void) { return atomic_load_explicit(&N48CB.on, memory_order_relaxed); }
static void n48cbl_present(uint64_t t0, uint64_t t1, uint32_t sid, uint64_t seq, int slot, int rc) {   // caller: the present queue, around scanout_present
    if (!n48cbl_on()) return;
    pthread_mutex_lock(&N48CB.mu);
    n48cbl_line_locked("P t0=%llu t1=%llu sid=%u seq=%llu slot=%d rc=%d", (unsigned long long)t0, (unsigned long long)t1, sid, (unsigned long long)seq, slot, rc);
    pthread_mutex_unlock(&N48CB.mu);
}
static void n48cbl_skip(uint32_t sid, uint64_t seq, int slot, const char *why, int vr) {
    if (!n48cbl_on()) return;
    pthread_mutex_lock(&N48CB.mu);
    n48cbl_line_locked("X t=%llu sid=%u seq=%llu slot=%d why=%s vk=%d", (unsigned long long)n48_now(), sid, (unsigned long long)seq, slot, why, vr);
    pthread_mutex_unlock(&N48CB.mu);
}
typedef struct { uint64_t slept; } n48h_ctx;
static void n48h_wait(void *c, uint64_t ns) {   // runs on the present queue with N48S.mu held: drop it for the sleep so the submit path (n48DispSeq) and the tick never block on the hold
    uint64_t t0 = n48_now();
    pthread_mutex_unlock(&N48S.mu);
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) }; nanosleep(&ts, NULL);
    pthread_mutex_lock(&N48S.mu);
    ((n48h_ctx *)c)->slept = n48_now() - t0;
}
static void n48s_complete(int slot, VkResult r, uint64_t seq, uint32_t sid, n48crc_rec_t *rec, int cls, uint64_t tcommit) {
    // All presents go through ONE serial queue (completions arrive from one serial queue PER Metal command queue, so across queues their order is arbitrary);
    // the sequence check under the mutex then makes the present order equal the submission order: an older frame is skipped, never shown after a newer one.
    static dispatch_once_t once; dispatch_once(&once, ^{ N48S.pq = dispatch_queue_create("navi48.present", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(N48S.pq, ^{
        pthread_mutex_lock(&N48S.mu);
        uint64_t stale0 = N48S.sm.stale, sup0 = N48S.sm.superseded, inv0 = N48S.sm.chain_invalid;
        n48h_ctx hc = { 0 }; uint64_t hw = 0;
        int hok = n48df_complete_held(&N48S.sm, slot, r == VK_SUCCESS ? 0 : 1, seq, sid, atomic_load_explicit(&N48H.on, memory_order_relaxed), atomic_load_explicit(&N48H.ms, memory_order_relaxed), tcommit, n48_now(), n48h_wait, &hc, &hw);   // P3: switch OFF = exactly n48df_complete_surf
        if (hw) { N48H.held++; N48H.hold_ns += hc.slept; if (N48S.sm.superseded != sup0) N48H.held_sup++; else if (hok) N48H.held_pres++; }
        if (hok) {
            uint64_t o[3] = { 0, 0, 0 };
            if (rec) n48s_crc_present(rec);
            uint64_t tp0_ = n48_now();
            int rc = N48S.fp(N48R.dev, N48S.sid[slot], o);
            n48cbl_present(tp0_, n48_now(), sid, seq, slot, rc);   // Stage 0b (no-op unless /private/tmp/n48m-cblog exists)
            if (rc == 0) n48df_count_present_cls(&N48S.sm, cls);
            if (n48df_present_result(&N48S.sm, rc)) {
                N48LOG("scanout: FAIL CLOSED / OFF (scanout-error): scanout_present(slot %d) rc %d", slot, rc);
                n48s_do_release_locked();
            } else N48LOGR("scanout: present slot %d seq %llu id %llu target frame %llu vupdates %llu", slot, (unsigned long long)seq, (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2]);
        } else if (n48cbl_skip(sid, seq, slot, N48S.sm.superseded != sup0 ? "superseded" : N48S.sm.chain_invalid != inv0 ? "chain-order" : N48S.sm.stale != stale0 ? "stale" : r != VK_SUCCESS ? "gpu-error" : "other", (int)r), N48S.sm.superseded != sup0) N48LOGR("scanout: superseded frame skipped (slot %d seq %llu: a later command buffer for the same surface was submitted)", slot, (unsigned long long)seq);
        else if (N48S.sm.chain_invalid != inv0) N48LOGR("scanout: frame NOT presented (slot %d seq %llu): submitted out of chain order, its base slot was not written yet", slot, (unsigned long long)seq);
        else if (N48S.sm.stale != stale0) N48LOGR("scanout: stale frame skipped (slot %d seq %llu, last presented %llu)", slot, (unsigned long long)seq, (unsigned long long)N48S.sm.last_seq);
        else if (r != VK_SUCCESS) N48LOGR("scanout: command buffer for slot %d ended with %d: not presented", slot, r);
        pthread_mutex_unlock(&N48S.mu);
        n48s_crc_free(rec);
    });
}
// Device selectors for mtlprobe dispflip (root test path; the stats read is one status call).
static NSDictionary *n48s_stats(void) {
    pthread_mutex_lock(&N48S.mu);
    struct n48n_scan_status st; memset(&st, 0, sizeof st); BOOL fresh = NO;
    if (N48S.sm.state == N48DF_ACTIVE) fresh = n48s_status_locked(&st);
    NSDictionary *d = @{ @"state": N48S.sm.state == N48DF_ACTIVE ? @"active" : N48S.sm.state == N48DF_OFF ? @"off" : (N48S.bound ? @"unbound" : @"unbound-never-seen-a-candidate"),
        @"reason": @(n48s_reason_name(N48S.sm.reason)), @"fresh": @(fresh), @"presents": @(N48S.sm.presents), @"drops": @(N48S.sm.drops), @"gpu_fail": @(N48S.sm.gpu_fail), @"stale": @(N48S.sm.stale), @"superseded": @(N48S.sm.superseded), @"multi_disp": @(atomic_load(&N48S.multi)),
        @"present_fail": @(N48S.sm.present_fail), @"latched": @(st.latched), @"replaced": @(st.replaced), @"repeats": @(st.repeats), @"refused": @(st.refused),
        @"watchdog_restores": @(st.watchdog_restores), @"frame_count": @(st.frame_count), @"vupdates": @(st.vupdates), @"disp_surfaces": @(N48S.ndisp), @"nonfinal_skipped": @(N48S.sm.nonfinal[1] + N48S.sm.nonfinal[2] + N48S.sm.nonfinal[3] + N48S.sm.nonfinal[4] + N48S.sm.nonfinal[5] + N48S.sm.nonfinal[6]), @"final_writes": @(N48S.sm.cls[0]) };
    pthread_mutex_unlock(&N48S.mu);
    return d;
}
static NSDictionary *n48s_release_request(void) {
    pthread_mutex_lock(&N48S.mu);
    uint64_t o[2] = { 0, 0 }; int rc = -1;
    if (N48S.sm.acquired && N48S.fr) { if (!N48S.sm.released) { N48S.sm.released = 1; N48S.sm.state = N48DF_OFF; }
        if (N48S.timer) { dispatch_source_cancel(N48S.timer); N48S.timer = nil; }
        rc = N48S.fr(N48R.dev, o); N48LOG("scanout: release (requested) rc %d, verified %llu, plane MC after 0x%llx", rc, (unsigned long long)o[0], (unsigned long long)o[1]); }
    NSDictionary *d = @{ @"rc": @(rc), @"verified": @(o[0]), @"plane_mc": @(o[1]) };
    pthread_mutex_unlock(&N48S.mu);
    return d;
}

// ---------------------------------------------------------------------------------------------------------------
// AIR dump for the pipeline-creation overrides (NATIVE-S4-M10 "Shaders": cache miss -> /tmp/n48m/<sha256>.air).
// ---------------------------------------------------------------------------------------------------------------
static NSString *n48_sha256hex(NSData *d) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}

// Dumps one function's bitcodeData; appends the .air path to paths, or a problem note to notes.
static void n48_dump_function(id fn, const char *role, NSMutableArray *paths, NSMutableArray *notes) {
    if (!fn) { [notes addObject:[NSString stringWithFormat:@"%s function is nil", role]]; return; }
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = nil;
    if ([fn respondsToSelector:sel]) bc = ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel);
    NSString *name = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    NSInteger stage = [fn respondsToSelector:@selector(functionType)] ? (NSInteger)[fn functionType] : -1;
    N48LOG("dump %s function '%s' class %s stage %ld bitcodeData %lu bytes", role,
           name.UTF8String, class_getName(object_getClass(fn)), (long)stage, (unsigned long)bc.length);
    if (!bc.length) { [notes addObject:[NSString stringWithFormat:@"%s '%@': no bitcodeData", role, name]]; return; }
    NSString *sha = n48_sha256hex(bc);
    mkdir("/tmp/n48m", 0777); chmod("/tmp/n48m", 0777);
    NSString *air = [NSString stringWithFormat:@"/tmp/n48m/%@.air", sha];
    NSString *side = [NSString stringWithFormat:@"/tmp/n48m/%@.%s.json", sha, role];
    BOOL ok = [bc writeToFile:air atomically:YES];
    NSDictionary *meta = @{ @"sha256": sha, @"function": name, @"role": @(role), @"stage": @(stage),
                            @"stageName": stage == 1 ? @"vertex" : stage == 2 ? @"fragment" : stage == 3 ? @"kernel" : @"other",
                            @"bytes": @(bc.length), @"air": air };
    NSData *js = [NSJSONSerialization dataWithJSONObject:meta options:NSJSONWritingPrettyPrinted error:NULL];
    ok = ok && [js writeToFile:side atomically:YES];
    chmod(air.UTF8String, 0644); chmod(side.UTF8String, 0644);
    if (!ok) { [notes addObject:[NSString stringWithFormat:@"%s '%@': write to %@ failed", role, name, air]]; return; }
    [paths addObject:[NSString stringWithFormat:@"%@ (%s '%@' stage %ld, sidecar %@)", air, role, name, (long)stage, side]];
}

static NSError *n48_dump_pipeline(MTLRenderPipelineDescriptor *d) {
    NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
    n48_dump_function(d.vertexFunction, "vertex", paths, notes);
    n48_dump_function(d.fragmentFunction, "fragment", paths, notes);
    NSString *msg = [NSString stringWithFormat:@"Navi48Metal: pipeline not in spvcache; AIR dumped for offline translation: %@%@",
                     [paths componentsJoinedByString:@"; "],
                     notes.count ? [@" | problems: " stringByAppendingString:[notes componentsJoinedByString:@"; "]] : @""];
    N48LOG("%s", msg.UTF8String);
    return n48_err(100, msg);
}


// ---------------------------------------------------------------------------------------------------------------
// Metal API gap census fixes (notes/design/NATIVE-S4-METAL-GAPS.md). Shared pieces:
//   N48_ONCE          one log line per call site per process (no-op stubs are called thousands of times a second by the compositor)
//   N48_RES_IVARS /   state + the public/SPI MTLResource surface that QuartzCore / SkyLight send to every texture and buffer
//   N48_RESOURCE_SPI  (census: -setResponsibleProcess: 7450, -protectionOptions 6741, -heap 1397, -virtualAddress 1092 in 45 s)
// ---------------------------------------------------------------------------------------------------------------
#define N48_ONCE(fmt, ...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) N48LOG(fmt, ##__VA_ARGS__); } while (0)
static _Atomic uint64_t n48_uid_ctr;
static _Atomic uint64_t n48_alloc_total;   // bytes of VkDeviceMemory held by live buffers and textures (-[MTLDevice currentAllocatedSize])
// Heap membership (m11 H1): a resource made by -[N48Heap newBuffer.../newTexture...] is a normal VkBuffer/VkImage that carries its heap, the offset the heap
// assigned, the bytes it was charged and the block id; the budget goes back to the heap at -makeAliasable or at dealloc, whichever is first.
@interface N48Heap : _MTLHeap
- (void)n48Free:(uint64_t)bid;
@end
#define N48_RES_IVARS NSUInteger _prot; int _respPid; BOOL _respSet; uint64_t _uid; N48Heap *_hpHeap; NSUInteger _hpOff, _hpSize; uint64_t _hpId; BOOL _hpAl;
#define N48_HEAP_DEALLOC if (_hpHeap && !_hpAl) { _hpAl = YES; [_hpHeap n48Free:_hpId]; }
#define N48_RESOURCE_SPI \
    - (id)heap { return _hpHeap; } \
    - (NSUInteger)heapOffset { return 0; } /* Apple-silicon: 0 for every resource of an automatic heap, although gpuAddress differs by the real offsets (measured) */ \
    - (BOOL)isAliasable { return _hpAl; } \
    - (void)makeAliasable { if (_hpHeap && !_hpAl) { _hpAl = YES; [_hpHeap n48Free:_hpId]; } } \
    - (NSUInteger)setPurgeableState:(NSUInteger)st { (void)st; return 2; } \
    - (BOOL)isPurgeable { return NO; } \
    - (int)setOwnerWithIdentity:(uint32_t)o { (void)o; return (int)0xE00002C2; } \
    - (BOOL)doesAliasResource:(id)o { (void)o; return NO; } \
    - (BOOL)doesAliasAnyResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { (void)o; (void)n; return NO; } \
    - (BOOL)doesAliasAllResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { (void)o; (void)n; return NO; } \
    - (BOOL)isComplete { return YES; } \
    - (BOOL)isWriteComplete { return YES; } \
    - (void)waitUntilComplete {} \
    - (NSUInteger)protectionOptions { return _prot; } \
    - (int)responsibleProcess { return _respSet ? _respPid : (int)getpid(); } \
    - (void)setResponsibleProcess:(int)p { _respPid = p; _respSet = YES; } \
    - (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; } \
    - (uint64_t)uniqueIdentifier { if (!_uid) _uid = atomic_fetch_add(&n48_uid_ctr, 1) + 1; return _uid; } \
    - (uint64_t)gpuAddress { N48_ONCE("gpuAddress requested: no GPU virtual address is exported (argument buffers / pointers are not implemented); returning 0"); return 0; }

// ---------------------------------------------------------------------------------------------------------------
// N48Buffer: minimal MTLBuffer (10a: Shared only) = VkBuffer + host-visible coherent VkDeviceMemory, mapped once.
// ---------------------------------------------------------------------------------------------------------------
@interface N48Buffer : _MTLResource {
    id _dev; NSUInteger _len; void *_ptr; VkBuffer _vkbuf; VkDeviceMemory _vkmem; NSString *_lbl; NSUInteger _opts; NSUInteger _asz;
    N48_RES_IVARS
    BOOL _noacct;   // heap resource: the heap's size is in n48_alloc_total, not this buffer's memory
    N48Mem _pm; BOOL _pooled;   // P1: the memory comes from the pool (slab range / recycled allocation / own allocation made through n48_mem_alloc); _vkmem is then NULL
    void (^_deallocator)(void *, NSUInteger); BOOL _imported;   // no-copy buffer: client memory imported as host memory; the block is called once, at release
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err;
- (BOOL)n48Imported;
- (void)n48AdoptHeap:(N48Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid;
- (VkBuffer)vkBuffer;
@end

@implementation N48Buffer
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _len = len; BOOL nozero = (opts & N48_OPT_NOZERO) != 0; _opts = opts & ~N48_OPT_NOZERO;   // P1: the marker never leaves this init
    opts = _opts;
    if (!n48_radv_open(err)) return nil;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
    VkResult r = vkCreateBuffer(N48R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(20, [NSString stringWithFormat:@"vkCreateBuffer = %d", r]); return nil; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(N48R.dev, _vkbuf, &mr);
    // 11e: Shared (0) and Managed (1) are host-visible coherent memory (the CPU and GPU copies are one, so didModifyRange: is a no-op);
    // Private (2) is DEVICE_LOCAL with no CPU mapping (contents = NULL). Memoryless (3) is refused.
    unsigned sm = (unsigned)((opts >> 4) & 0xF);
    if (sm > 2) { if (err) *err = n48_err(25, @"buffer storage mode Memoryless is not supported"); return nil; }
    int mt = sm == 2 ? n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                     : n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { if (err) *err = n48_err(21, sm == 2 ? @"no device-local memory type" : @"no host-visible coherent memory type"); return nil; }
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    _asz = (NSUInteger)mr.size;
    if (N48P.on) {   // P1: slab range / recycled allocation; the host-visible memory is mapped once by the pool (zeroed here unless the caller said it will overwrite it)
        r = n48_mem_alloc((uint32_t)mt, mr, sm != 2, sm != 2 && !nozero, _vkbuf, VK_NULL_HANDLE, &_pm);
        if (r != VK_SUCCESS) { if (err) *err = n48_err(22, [NSString stringWithFormat:@"vkAllocateMemory = %d (pool)", r]); return nil; }
        _pooled = YES; atomic_fetch_add(&n48_alloc_total, (uint64_t)_asz);
        r = vkBindBufferMemory(N48R.dev, _vkbuf, _pm.mem, _pm.off);
        if (r != VK_SUCCESS) { if (err) *err = n48_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
        if (sm != 2) _ptr = _pm.map;
    } else {
    r = vkAllocateMemory(N48R.dev, &ma, NULL, &_vkmem);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(22, [NSString stringWithFormat:@"vkAllocateMemory = %d", r]); return nil; }
    atomic_fetch_add(&n48_alloc_total, (uint64_t)_asz);
    r = vkBindBufferMemory(N48R.dev, _vkbuf, _vkmem, 0);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d", r]); return nil; }
    if (sm != 2) {
        r = vkMapMemory(N48R.dev, _vkmem, 0, VK_WHOLE_SIZE, 0, &_ptr);
        if (r != VK_SUCCESS) { if (err) *err = n48_err(24, [NSString stringWithFormat:@"vkMapMemory = %d", r]); return nil; }
    }
    }
    N48LOG("N48Buffer %p: %lu bytes (alloc %llu, memtype %d) mapped at %p", (__bridge void *)self, (unsigned long)len,
           (unsigned long long)mr.size, mt, _ptr);
    return self;
}
// 12 (no-copy): client memory, page-aligned and a page multiple, imported with VK_EXT_external_memory_host (the IOSurface texture path, 11h.6). The CPU pointer
// IS the client's (contents == p); GPU writes are visible to the CPU after the writing command buffer completed, CPU writes before commit are seen by the GPU
// (host-visible import: the memory type is host coherent). Anything else (unaligned pointer, length not a page multiple, Private storage, > 64 MiB) returns nil.
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define NCFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; N48LOG("newBufferWithBytesNoCopy: refused: %s", m_.UTF8String); if (err) *err = n48_err(code, m_); return nil; } while (0)
    _dev = dev; _len = len; _opts = opts;
    if (!p || !len) NCFAIL(110, @"NULL pointer or zero length");
    if (((opts >> 4) & 0xF) > 1) NCFAIL(111, @"storage mode %lu: only Shared and Managed client memory can be wrapped", (unsigned long)((opts >> 4) & 0xF));
    if (!n48_radv_open(err)) return nil;
    if (!N48R.hostExt || !n48_vkGetMHPP) NCFAIL(112, @"VK_EXT_external_memory_host is not available on this RADV");
    if (((uintptr_t)p & (N48R.hostAlign - 1)) || (len & (N48R.hostAlign - 1))) NCFAIL(113, @"pointer %p / length %lu not aligned to %llu (page-aligned page-multiple memory only)", p, (unsigned long)len, (unsigned long long)N48R.hostAlign);
    if (len > 64u << 20) NCFAIL(114, @"length %lu > 64 MiB (the N48N per-BO import limit)", (unsigned long)len);
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
    VkResult r = vkCreateBuffer(N48R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { _vkbuf = VK_NULL_HANDLE; NCFAIL(115, @"vkCreateBuffer = %d", r); }
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = n48_now();
    r = n48_vkGetMHPP(N48R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) NCFAIL(116, @"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(N48R.dev, _vkbuf, &mr);
    int mt = n48_find_mem(hp.memoryTypeBits & mr.memoryTypeBits, 0);
    if (mt < 0) NCFAIL(117, @"no memory type common to the import (0x%x) and the buffer (0x%x)", hp.memoryTypeBits, mr.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = p };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = len, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(N48R.dev, &ma, NULL, &_vkmem); n48t_add(&T1.imp, n48_now() - timp_);
    if (r != VK_SUCCESS) { _vkmem = VK_NULL_HANDLE; NCFAIL(118, @"vkAllocateMemory(import %lu B at %p) = %d", (unsigned long)len, p, r); }
    r = vkBindBufferMemory(N48R.dev, _vkbuf, _vkmem, 0);
    if (r != VK_SUCCESS) NCFAIL(119, @"vkBindBufferMemory = %d", r);
    @synchronized ([Navi48Device class]) { N48R.impTotal += len; }
    _ptr = p; _imported = YES; _asz = 0; _deallocator = [d copy];
    N48LOG("N48Buffer %p: NO-COPY %lu bytes at %p imported (host memory type %d)", (__bridge void *)self, (unsigned long)len, p, mt);
    #undef NCFAIL
    return self;
}
- (BOOL)n48Imported { return _imported; }
- (void)n48AdoptHeap:(N48Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (!_noacct && !_imported) { atomic_fetch_sub(&n48_alloc_total, (uint64_t)_asz); _noacct = YES; }
}
- (void)dealloc {
    N48_HEAP_DEALLOC
    if (N48R.ok) {
        if (_vkbuf) vkDestroyBuffer(N48R.dev, _vkbuf, NULL);
        if (_pooled) { n48_mem_free(&_pm); if (!_noacct) atomic_fetch_sub(&n48_alloc_total, (uint64_t)_asz); }   // P1
        else if (_vkmem) { vkFreeMemory(N48R.dev, _vkmem, NULL); if (_imported) { @synchronized ([Navi48Device class]) { N48R.impTotal -= _len; } } else if (!_noacct) atomic_fetch_sub(&n48_alloc_total, (uint64_t)_asz); }
    }
    if (_deallocator) { void (^d)(void *, NSUInteger) = _deallocator; _deallocator = nil; d(_ptr, _len); }
}
- (void)doesNotRecognizeSelector:(SEL)sel {
    N48LOG("UNRECOGNIZED selector %s on N48Buffer", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
- (VkBuffer)vkBuffer { return _vkbuf; }
- (void *)contents { return _ptr; }
- (NSUInteger)length { return _len; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
- (void)didModifyRange:(NSRange)r { (void)r; }   // Shared memory is coherent
// ---- gap census: MTLResource / MTLBuffer surface sent by QuartzCore, SkyLight and Metal ----
N48_RESOURCE_SPI
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_asz ? _asz : _len); }
- (void *)virtualAddress { return _ptr; }   // MTLIOAccelResource -virtualAddress is ^v (CPU pointer); QuartzCore sends it next to -contents and -didModifyRange: (SUSPECTED use)
- (void)addDebugMarker:(NSString *)m range:(NSRange)r { (void)m; (void)r; }
- (void)removeAllDebugMarkers {}
- (IOSurfaceRef)iosurface { return NULL; }
@end


// ---------------------------------------------------------------------------------------------------------------
// Common helpers for the 10b-10d classes.
// ---------------------------------------------------------------------------------------------------------------
#define N48_DNR(cls) \
    - (void)doesNotRecognizeSelector:(SEL)sel { \
        N48LOG("UNRECOGNIZED selector %s on " #cls, sel_getName(sel)); \
        [super doesNotRecognizeSelector:sel]; }

static uint64_t n48_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// flags: N48F_A8 = Metal A8Unorm stored as VK R8_UNORM; sampling goes through a view with components (0,0,0,R) (Metal returns (0,0,0,a)); a render target of this
// format is refused (a Vulkan attachment view must be identity, so a fragment's alpha cannot be steered into the R8 image).
#define N48F_A8 1u
typedef struct { MTLPixelFormat mtl; VkFormat vk; uint32_t bpp; uint32_t flags; } N48Fmt;
static const N48Fmt n48_fmts[] = {
    { MTLPixelFormatBGRA8Unorm,      VK_FORMAT_B8G8R8A8_UNORM, 4, 0 },
    { MTLPixelFormatBGRA8Unorm_sRGB, VK_FORMAT_B8G8R8A8_SRGB,  4, 0 },
    { MTLPixelFormatRGBA8Unorm,      VK_FORMAT_R8G8B8A8_UNORM, 4, 0 },
    { MTLPixelFormatRGBA8Unorm_sRGB, VK_FORMAT_R8G8B8A8_SRGB,  4, 0 },
    { MTLPixelFormatR8Unorm,         VK_FORMAT_R8_UNORM,       1, 0 },
    { MTLPixelFormatRG8Unorm,        VK_FORMAT_R8G8_UNORM,     2, 0 },
    { MTLPixelFormatRGBA16Float,     VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0 },
    { MTLPixelFormatRGBA32Float,     VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0 },
    // m11h3 format coverage (CoreDisplay's display pipe and the usual WindowServer set). Every entry is checked against RADV's format features
    // at texture / pipeline creation (n48_fmt_feats); an entry RADV cannot sample or copy is refused with a nil log, never faked.
    { MTLPixelFormatR32Float,        VK_FORMAT_R32_SFLOAT,          4, 0 },   // 55: the 16384x1 gamma/LUT array of RunFullDisplayPipe
    { MTLPixelFormatRG32Float,       VK_FORMAT_R32G32_SFLOAT,       8, 0 },
    { MTLPixelFormatR16Float,        VK_FORMAT_R16_SFLOAT,          2, 0 },
    { MTLPixelFormatRG16Float,       VK_FORMAT_R16G16_SFLOAT,       4, 0 },
    { MTLPixelFormatR16Unorm,        VK_FORMAT_R16_UNORM,           2, 0 },
    { MTLPixelFormatRG16Unorm,       VK_FORMAT_R16G16_UNORM,        4, 0 },
    { MTLPixelFormatRGBA16Unorm,     VK_FORMAT_R16G16B16A16_UNORM,  8, 0 },
    // Metal packs RGB10A2 as A2 B10 G10 R10 (red in the LOW bits) and BGR10A2 as A2 R10 G10 B10 (blue low); the Vulkan PACK32 names list the top bits first.
    { MTLPixelFormatRGB10A2Unorm,    VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0 },
    { MTLPixelFormatBGR10A2Unorm,    VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, 0 },
    { MTLPixelFormatRG11B10Float,    VK_FORMAT_B10G11R11_UFLOAT_PACK32,  4, 0 },
    // m11h9: A8Unorm (178 NIL lines in the first Metal-compositor run: glyph / mask textures, also buffer-backed)
    { MTLPixelFormatA8Unorm,         VK_FORMAT_R8_UNORM,                 1, N48F_A8 },
};
// The component mapping of a view of format f whose client swizzle is swz: client channel -> (format's own mapping of that channel). Identity for every format but A8.
static VkComponentMapping n48_view_map(const N48Fmt *f, MTLTextureSwizzleChannels swz) {
    static const VkComponentSwizzle base[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
    static const VkComponentSwizzle a8[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
    const VkComponentSwizzle *m = (f && (f->flags & N48F_A8)) ? a8 : base;
    unsigned c[4] = { swz.red, swz.green, swz.blue, swz.alpha };
    VkComponentSwizzle o[4]; for (int i = 0; i < 4; i++) o[i] = c[i] < 6 ? m[c[i]] : VK_COMPONENT_SWIZZLE_IDENTITY;
    return (VkComponentMapping){ o[0], o[1], o[2], o[3] };
}
static BOOL n48_is_identity_map(const VkComponentMapping *m) {
    return (m->r == VK_COMPONENT_SWIZZLE_R || m->r == VK_COMPONENT_SWIZZLE_IDENTITY) && (m->g == VK_COMPONENT_SWIZZLE_G || m->g == VK_COMPONENT_SWIZZLE_IDENTITY) &&
           (m->b == VK_COMPONENT_SWIZZLE_B || m->b == VK_COMPONENT_SWIZZLE_IDENTITY) && (m->a == VK_COMPONENT_SWIZZLE_A || m->a == VK_COMPONENT_SWIZZLE_IDENTITY);
}
static const N48Fmt *n48_fmt(MTLPixelFormat f) {
    for (size_t i = 0; i < sizeof n48_fmts / sizeof *n48_fmts; i++) if (n48_fmts[i].mtl == f) return &n48_fmts[i];
    return NULL;
}
// RADV's optimal-tiling features of a format (RADV must be open). 0 when the entry point is missing.
static VkFormatFeatureFlags n48_fmt_feats(const N48Fmt *f) {
    VkFormatProperties fp = {0};
    if (vkGetPhysicalDeviceFormatProperties && f) vkGetPhysicalDeviceFormatProperties(N48R.pd, f->vk, &fp);
    return fp.optimalTilingFeatures;
}

static void n48_add_protocols(Class c, const char **names, unsigned n, const char *tag) {
    for (unsigned i = 0; i < n; i++) {
        Protocol *p = objc_getProtocol(names[i]);
        BOOL added = p ? class_addProtocol(c, p) : NO;
        os_log(OS_LOG_DEFAULT, "Navi48Metal: +load %{public}s protocol %{public}s found %d added %d", tag, names[i], p != nil, added);
    }
}

@class N48CommandBuffer;
@class N48Texture;

// ---------------------------------------------------------------------------------------------------------------
// N48Texture: VkImage (optimal, DEVICE_LOCAL), always TRANSFER_SRC|DST (10c).
// ---------------------------------------------------------------------------------------------------------------
@interface N48Texture : _MTLResource {
    id _dev; NSUInteger _w, _h; MTLPixelFormat _pf; const N48Fmt *_fmt; NSUInteger _usage; NSUInteger _opts;
    VkImage _img; VkDeviceMemory _mem; VkImageView _view; VkImageLayout _layout; NSString *_lbl; NSUInteger _size;
    // 11h.6 IOSurface backing: the surface's pages are imported (VK_EXT_external_memory_host) as _imem. Path (a) _iosLinear: the VkImage is LINEAR and bound
    // to _imem itself. Path (b): the VkImage is optimal/device-local and _ibuf (a VkBuffer over _imem) is copied to/from it around each command buffer.
    IOSurfaceRef _ios; NSUInteger _iosPlane; BOOL _iosLinear; VkDeviceMemory _imem; VkBuffer _ibuf; uint8_t *_ibase; size_t _ialloc, _ibpr; NSUInteger _ioff;   // P2: _ioff = the plane's byte offset inside the imported allocation (0 for plane 0 / single-plane)
    // gap census: texture views (newTextureViewWithPixelFormat:...) share the ROOT texture's VkImage; _root is the image owner (nil for a root texture).
    N48Texture *_root; MTLTextureSwizzleChannels _swz;
    BOOL _is1D, _arr1D; NSUInteger _layers;   // m11h3: MTLTextureType1D / 1DArray (VkImageType 1D, arrayLayers = _layers); 2D textures keep _is1D NO, _layers 0
    // m11h9: mip levels (2D and 3D), 3D textures (VkImageType 3D, _depth slices) and A8 (the view carries a swizzle, so an identity view per level is made on demand for attachments / storage)
    NSUInteger _levels, _depth; BOOL _is3D, _viewSwz; NSMutableDictionary<NSNumber *, NSNumber *> *_lvViews;
    N48Buffer *_hostBuf; NSUInteger _hoff;   // no-copy: the texture is backed by this buffer's memory at _hoff (copy path b, as an IOSurface texture)
    N48_RES_IVARS
    uint64_t _acct;   // bytes added to n48_alloc_total for this texture's image memory
    N48Mem _pm; BOOL _pooled;   // P1: _mem's role is played by _pm (slab range / recycled / own allocation through n48_mem_alloc); _mem is then NULL
    BOOL _impShared, _iosNoBase;   // P4: _imem is a shared import-cache entry (give it back with n48_imp_put, never vkFreeMemory); _iosNoBase = a protected IOSurface with no CPU mapping: a GPU-only texture (no _imem/_ibuf/_ibase, n48IsIOS is NO)
    BOOL _disp; VkBuffer _dbuf;   // S5.2a: CoreDisplay display surface (D-copy source); _dbuf = a VkBuffer over _imem when path (a) has no _ibuf
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err;
- (void)n48AdoptHeap:(N48Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane error:(NSError **)err;
- (instancetype)initViewOf:(N48Texture *)root pixelFormat:(MTLPixelFormat)pf swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(N48Buffer *)b offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr error:(NSError **)err;
- (NSUInteger)n48IOSOffset;
- (BOOL)n48IsIOS;
- (BOOL)n48IsDisp;
- (VkBuffer)n48DispBuf;
- (BOOL)n48DispImgOnly;   // P6: a display surface with no CPU mapping (protected): its copy source is the VkImage, not a buffer
- (unsigned)n48DispSid;   // P6: IOSurfaceGetID of the retained surface for any display texture (n48IOSRef is nil for a base-less one)
- (const uint8_t *)n48IBase;
- (IOSurfaceRef)n48IOSRef;
- (BOOL)n48IOSLinear;
- (VkBuffer)n48IOSBuffer;
- (size_t)n48IOSBytesPerRow;
- (VkImage)vkImage;
- (VkImageView)vkView;
- (uint32_t)n48Layers;
- (uint32_t)n48Levels;
- (VkImageView)n48AttViewLevel:(uint32_t)lv;
- (VkFormat)vkFormat;
- (uint32_t)bytesPerPixel;
- (VkImageLayout)layout;
- (void)setLayout:(VkImageLayout)l;
- (NSUInteger)width;
- (NSUInteger)height;
- (NSUInteger)depth;
- (NSUInteger)mipmapLevelCount;
- (MTLTextureType)textureType;
- (MTLStorageMode)storageMode;
@end

@implementation N48Buffer (Textures)
// Linear textures over this buffer (public -newTextureWithDescriptor:offset:bytesPerRow:, SPI -newLinearTexture..., -newTiledTexture...): see N48Texture initWithDevice:descriptor:buffer:
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    NSError *e = nil; id t = [[N48Texture alloc] initWithDevice:_dev descriptor:d buffer:self offset:off bytesPerRow:bpr error:&e];
    if (!t) N48LOG("buffer newTextureWithDescriptor:offset:bytesPerRow: NIL: %s; pf %lu type %lu usage 0x%lx storage %lu %lux%lu", e.localizedDescription.UTF8String,
                   (unsigned long)d.pixelFormat, (unsigned long)d.textureType, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height);
    return t;
}
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi { (void)bpi; return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
- (id)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr { return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
@end

// Layout transition recorded into cmd; the texture object tracks the layout at RECORD time (command buffers execute in order).
static void n48_tex_to(VkCommandBuffer cmd, N48Texture *t, VkImageLayout nl) {
    if ([t layout] == nl) return;
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = [t layout], .newLayout = nl, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = [t vkImage], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, [t n48Levels], 0, [t n48Layers] } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    [t setLayout:nl];
}
static BOOL n48_region_ok(N48Texture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const char *what) {
    if (level >= [t mipmapLevelCount] || slice >= [t n48Layers]) { N48LOG("%s: mip level %lu / slice %lu: the texture has %lu level(s) and %u slice(s); refused", what, (unsigned long)level, (unsigned long)slice, (unsigned long)[t mipmapLevelCount], [t n48Layers]); return NO; }
    NSUInteger lw = MAX((NSUInteger)1, t.width >> level), lh = MAX((NSUInteger)1, t.height >> level), ld = MAX((NSUInteger)1, t.depth >> level);   // depth is 1 for everything but 3D
    if (!r.size.width || !r.size.height || !r.size.depth || r.origin.x + r.size.width > lw || r.origin.y + r.size.height > lh || r.origin.z + r.size.depth > ld) {
        N48LOG("%s: region origin %lu,%lu,%lu size %lux%lux%lu outside the %lux%lux%lu level %lu; refused", what, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
               (unsigned long)r.origin.z, (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)r.size.depth, (unsigned long)lw, (unsigned long)lh, (unsigned long)ld, (unsigned long)level); return NO; }
    return YES;
}

@implementation N48Texture
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = (MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = n48_fmt(_pf);
    if (!_fmt) { if (err) *err = n48_err(30, [NSString stringWithFormat:@"pixel format %lu not supported", (unsigned long)_pf]); return nil; }
    // m11h3: 2D, 1D and 1DArray. Mapping chosen: VkImageType 1D + VK_IMAGE_VIEW_TYPE_1D / 1D_ARRAY (not 2D with height 1), because the translated
    // AIR declares a Dim1D arrayed OpTypeImage (Sampled1D) and a descriptor view must have the matching view type; RADV supports 1D images up to 16384.
    // m11h9: mip levels (2D, 3D) and MTLTextureType3D (VkImageType 3D, view type 3D, depth slices in the extent).
    MTLTextureType tt = d.textureType;
    BOOL is1D = tt == MTLTextureType1D || tt == MTLTextureType1DArray, is3D = tt == MTLTextureType3D;
    NSUInteger levels = d.mipmapLevelCount ? d.mipmapLevelCount : 1, depth = d.depth ? d.depth : 1;
    if (!(tt == MTLTextureType2D || is1D || is3D) || (is3D ? d.arrayLength != 1 : depth != 1) || d.sampleCount != 1 ||
        (tt != MTLTextureType1DArray && d.arrayLength != 1) || d.arrayLength < 1 || (is1D && levels != 1)) {
        if (err) { *err = n48_err(31, [NSString stringWithFormat:@"only single-sample 2D (mips), 3D (mips) and single-level 1D / 1DArray textures (type %lu depth %lu mips %lu array %lu samples %lu)", (unsigned long)tt,
            (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount, (unsigned long)d.arrayLength, (unsigned long)d.sampleCount]); } return nil; }
    { NSUInteger md = MAX(MAX(_w, _h), is3D ? depth : (NSUInteger)1), maxLevels = 1; while (md > 1) { md >>= 1; maxLevels++; }
      if (levels > maxLevels) { if (err) *err = n48_err(42, [NSString stringWithFormat:@"%lu mip levels for a %lux%lux%lu texture (at most %lu)", (unsigned long)levels, (unsigned long)_w, (unsigned long)_h, (unsigned long)depth, (unsigned long)maxLevels]); return nil; } }
    if ((_fmt->flags & N48F_A8) && (d.usage & MTLTextureUsageRenderTarget)) {
        if (err) *err = n48_err(44, @"A8Unorm render targets are not supported (an R8 attachment cannot take the fragment's alpha)"); return nil; }
    if (is3D && (d.usage & MTLTextureUsageRenderTarget)) { if (err) *err = n48_err(45, @"3D render targets are not supported"); return nil; }
    if (!n48_radv_open(err)) return nil;
    _is1D = is1D; _arr1D = tt == MTLTextureType1DArray; _layers = is1D ? d.arrayLength : 0; _is3D = is3D; _levels = levels; _depth = depth;
    if (is1D && (d.height != 1 || _w > N48R.lim.maxImageDimension1D || _layers > N48R.lim.maxImageArrayLayers)) {
        if (err) { *err = n48_err(37, [NSString stringWithFormat:@"1D texture %lux%lu array %lu outside RADV limits (width <= %u, layers <= %u, height 1)", (unsigned long)_w, (unsigned long)_h,
            (unsigned long)_layers, N48R.lim.maxImageDimension1D, N48R.lim.maxImageArrayLayers]); } return nil; }
    if (is3D && (_w > N48R.lim.maxImageDimension3D || _h > N48R.lim.maxImageDimension3D || _depth > N48R.lim.maxImageDimension3D)) {
        if (err) *err = n48_err(46, [NSString stringWithFormat:@"3D texture %lux%lux%lu outside the RADV limit %u", (unsigned long)_w, (unsigned long)_h, (unsigned long)_depth, N48R.lim.maxImageDimension3D]); return nil; }
    // RADV format features decide the usage bits (before this, every format had colour-attachment and input-attachment usage).
    VkFormatFeatureFlags ff = n48_fmt_feats(_fmt);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((ff & need) != need) { if (err) *err = n48_err(38, [NSString stringWithFormat:@"pixel format %lu (vk %d): RADV features 0x%x lack sampled/transfer", (unsigned long)_pf, _fmt->vk, ff]); return nil; }
    BOOL canRT = !is1D && !is3D && !(_fmt->flags & N48F_A8) && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    if ((d.usage & MTLTextureUsageRenderTarget) && !canRT) {
        if (err) *err = n48_err(39, [NSString stringWithFormat:@"pixel format %lu (vk %d) type %lu cannot be a render target on RADV (features 0x%x)", (unsigned long)_pf, _fmt->vk, (unsigned long)tt, ff]); return nil; }
    BOOL wantSt = (d.usage & MTLTextureUsageShaderWrite) != 0;
    if (wantSt && !(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        if (err) *err = n48_err(40, [NSString stringWithFormat:@"pixel format %lu (vk %d) has no storage-image support on RADV (features 0x%x)", (unsigned long)_pf, _fmt->vk, ff]); return nil; }
    VkImageUsageFlags iu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        (canRT ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |   // 11e-2: framebuffer fetch reads the attachment as an input attachment
        (wantSt ? VK_IMAGE_USAGE_STORAGE_BIT : 0);   // 11e: storage images only when the client asks (compute writes)
    VkImageCreateFlags icf = (d.usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;   // gap census: views may reinterpret the format only when the client asked (DCC stays on otherwise)
    VkImageType vit = is1D ? VK_IMAGE_TYPE_1D : is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageFormatProperties ifp;
    VkResult qr = vkGetPhysicalDeviceImageFormatProperties ? vkGetPhysicalDeviceImageFormatProperties(N48R.pd, _fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp) : VK_SUCCESS;
    if (qr != VK_SUCCESS && canRT && !(d.usage & MTLTextureUsageRenderTarget)) {   // not asked to be a render target: drop the attachment usages rather than refuse
        iu &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        qr = vkGetPhysicalDeviceImageFormatProperties(N48R.pd, _fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp);
    }
    if (qr != VK_SUCCESS) { if (err) *err = n48_err(41, [NSString stringWithFormat:@"RADV refuses image type %s format %d usage 0x%x (vkGetPhysicalDeviceImageFormatProperties = %d)", is1D ? "1D" : is3D ? "3D" : "2D", _fmt->vk, iu, qr]); return nil; }
    if (vkGetPhysicalDeviceImageFormatProperties && levels > ifp.maxMipLevels) { if (err) *err = n48_err(42, [NSString stringWithFormat:@"%lu mip levels > RADV's %u for this format", (unsigned long)levels, ifp.maxMipLevels]); return nil; }
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = vit, .format = _fmt->vk,
        .extent = { (uint32_t)_w, (uint32_t)_h, is3D ? (uint32_t)_depth : 1 }, .mipLevels = (uint32_t)_levels, .arrayLayers = is1D ? (uint32_t)_layers : 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .flags = icf, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .usage = iu };
    VkResult r = vkCreateImage(N48R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(32, [NSString stringWithFormat:@"vkCreateImage = %d", r]); return nil; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(N48R.dev, _img, &mr);
    int mt = n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { if (err) *err = n48_err(33, @"no device-local memory type"); return nil; }
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (N48P.on) {   // P1
        r = n48_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
        if (r != VK_SUCCESS) { if (err) *err = n48_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d (pool)", r]); return nil; }
        _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
        r = vkBindImageMemory(N48R.dev, _img, _pm.mem, _pm.off);
        if (r != VK_SUCCESS) { if (err) *err = n48_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
    } else {
    r = vkAllocateMemory(N48R.dev, &ma, NULL, &_mem);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d", r]); return nil; }
    _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
    r = vkBindImageMemory(N48R.dev, _img, _mem, 0);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d", r]); return nil; }
    }
    VkComponentMapping cm = n48_view_map(_fmt, _swz); _viewSwz = !n48_is_identity_map(&cm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img,
        .viewType = is1D ? (_arr1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
        .format = _fmt->vk, .components = cm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, is1D ? (uint32_t)_layers : 1 } };
    r = vkCreateImageView(N48R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(36, [NSString stringWithFormat:@"vkCreateImageView = %d", r]); return nil; }
    N48LOG("N48Texture %p: %lux%lu%s pf %lu vkformat %d levels %lu (alloc %llu, memtype %d)%s", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, is3D ? [NSString stringWithFormat:@"x%lu (3D)", (unsigned long)_depth].UTF8String : "",
           (unsigned long)_pf, _fmt->vk, (unsigned long)_levels, (unsigned long long)mr.size, mt, _viewSwz ? " [sampled through a swizzled view]" : "");
    return self;
}
// A single-level, identity-swizzle view for attachments, input attachments and storage images. Level 0 of a one-level, unswizzled texture is the sampling view itself.
- (VkImageView)n48AttViewLevel:(uint32_t)lv {
    if (_root) return _view;
    if (lv == 0 && _levels <= 1 && !_viewSwz) return _view;
    @synchronized (self) {
        if (!_lvViews) _lvViews = [NSMutableDictionary dictionary];
        NSNumber *have = _lvViews[@(lv)]; if (have) return (VkImageView)(uintptr_t)have.unsignedLongLongValue;
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img,
            .viewType = _is1D ? (_arr1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : _is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, .format = _fmt->vk,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, 0, _is1D ? (uint32_t)_layers : 1 } };
        VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(N48R.dev, &vc, NULL, &v);
        if (r != VK_SUCCESS) { N48LOG("n48AttViewLevel %u: vkCreateImageView = %d; falling back to the sampling view", lv, r); return _view; }
        _lvViews[@(lv)] = @((unsigned long long)(uintptr_t)v); return v;
    }
}
- (void)n48AdoptHeap:(N48Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (_acct) { atomic_fetch_sub(&n48_alloc_total, _acct); _acct = 0; }   // the heap's size is what counts
}
- (void)dealloc {
    N48_HEAP_DEALLOC
    if (N48R.ok) {
        for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView(N48R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        if (_view) vkDestroyImageView(N48R.dev, _view, NULL);
        if (_img && !_root) vkDestroyImage(N48R.dev, _img, NULL);
        if (_pooled) { n48_mem_free(&_pm); atomic_fetch_sub(&n48_alloc_total, _acct); }   // P1
        else if (_mem) { vkFreeMemory(N48R.dev, _mem, NULL); atomic_fetch_sub(&n48_alloc_total, _acct); }
        if (_ibuf) vkDestroyBuffer(N48R.dev, _ibuf, NULL);
        if (_dbuf) vkDestroyBuffer(N48R.dev, _dbuf, NULL);
        if (_imem && _impShared) n48_imp_put(_imem);   // P4: the shared import goes back to the cache (kept 2 s; freed later through the fence)
        else if (_imem) { vkFreeMemory(N48R.dev, _imem, NULL); @synchronized ([Navi48Device class]) { N48R.impTotal -= _ialloc; } }
    }
    if (_ios) { IOSurfaceDecrementUseCount(_ios); CFRelease(_ios); }
}
N48_DNR(N48Texture)
// ---- 11h.6: IOSurface-backed texture ----
// Accepts a 2D, page-aligned (P2: any plane of a 2/3-plane surface, via n48_plane.h), page-multiple 32 bpp surface (BGRA8Unorm / RGBA8Unorm descriptors); anything else returns nil + log.
// The surface is retained and use-counted for the texture's lifetime; its pages are imported once (whole allocation).
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    #define IOSFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; N48LOG("newTexture(iosurface): refused: %s", m_.UTF8String); if (err) *err = n48_err(code, m_); return nil; } while (0)
    if (!s) IOSFAIL(70, @"NULL IOSurface");
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED; _iosPlane = plane;
    _swz = (MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = n48_fmt(_pf);
    // m11h9: any pixel format of the table whose bytes per pixel equal the surface's bytes per element (BGR10A2Unorm 'l10r' and RGBA16Float 'RGhA' are what the compositor's
    // display surfaces use); an A8 / render-target mismatch is refused below by the same rules as an ordinary texture.
    if (!_fmt) IOSFAIL(71, @"pixel format %lu is not in the format table", (unsigned long)_pf);
    if ((_fmt->flags & N48F_A8) && (d.usage & MTLTextureUsageRenderTarget)) IOSFAIL(71, @"A8Unorm render targets are not supported");
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72, @"only single 2D textures (no mips/array/MSAA)");
    size_t pc = IOSurfaceGetPlaneCount(s);
    // P2: a plane p < planeCount of a 2- or 3-plane surface is accepted (CoreAnimation makes one texture per plane of a bi-planar YUV surface). The kill file
    // /private/tmp/n48m-noplanes (latched once per process) restores the old refusal of every multi-plane surface.
    static int noplanes = -1; if (noplanes < 0) { noplanes = access(N48_NOPLANES_FILE, F_OK) == 0; if (noplanes) N48LOG("P2 planes: multi-plane IOSurfaces REFUSED (kill file " N48_NOPLANES_FILE " exists; latched for the life of this process)"); }
    if (pc > 1 && noplanes) IOSFAIL(73, @"IOSurface has %zu planes; single-plane surfaces only", pc);
    size_t pidx = pc > 1 ? plane : 0;   // pc <= 1: any plane != 0 is refused by n48pl_geom below, exactly as before
    size_t sw = pc ? IOSurfaceGetWidthOfPlane(s, pidx) : IOSurfaceGetWidth(s), sh = pc ? IOSurfaceGetHeightOfPlane(s, pidx) : IOSurfaceGetHeight(s);
    size_t bpe = pc ? IOSurfaceGetBytesPerElementOfPlane(s, pidx) : IOSurfaceGetBytesPerElement(s);
    size_t bpr = pc ? IOSurfaceGetBytesPerRowOfPlane(s, pidx) : IOSurfaceGetBytesPerRow(s);
    n48pl_in pin = { .pc = pc, .plane = plane, .pw = sw, .ph = sh, .pbpe = bpe, .pbpr = bpr, .poff = 0, .alloc = 0, .dw = _w, .dh = _h, .dbpp = _fmt->bpp };
    n48pl_out pout;
    if (!n48pl_geom(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    if (!n48_radv_open(err)) return nil;
    {   // the colour-attachment / sampled features of this format (RADV decides; before m11h9 every surface was 32 bpp BGRA8/RGBA8 and this was implied)
        VkFormatFeatureFlags ff = n48_fmt_feats(_fmt), nd = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            ((d.usage & MTLTextureUsageRenderTarget) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((ff & nd) != nd) IOSFAIL(92, @"pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)_pf, _fmt->vk, ff, nd & ~ff);
    }
    if (!N48R.hostExt || !n48_vkGetMHPP) IOSFAIL(78, @"VK_EXT_external_memory_host is not available on this RADV");
    IOSurfaceLock(s, kIOSurfaceLockAvoidSync, NULL);
    void *base = IOSurfaceGetBaseAddress(s);   // P2: the whole allocation is imported from the surface base; the plane is used at its offset
    void *pbase = pc ? IOSurfaceGetBaseAddressOfPlane(s, pidx) : base;
    IOSurfaceUnlock(s, kIOSurfaceLockAvoidSync, NULL);
    size_t alloc = IOSurfaceGetAllocSize(s);
    uint32_t spf = IOSurfaceGetPixelFormat(s);
    N48LOG("newTexture(iosurface): id %u %zux%zu bpr %zu allocSize %zu base %p pixelFormat 0x%08x planes %zu", IOSurfaceGetID(s), sw, sh, bpr, alloc, base, spf, pc);
    if (!pbase || !base) {   // P4: a protected IOSurface has no CPU mapping (CoreDisplay's DisplaySurface::GetMTLTexture cannot survive a nil texture). Not refused: a device-local optimal image of the descriptor's
        // size/format that is NOT backed by the surface's memory (the surface's contents are not shared). No import, no _ibuf, no _ibase: n48IsIOS is NO, so no upload/write-back copy and no CPU path touches a NULL base.
        // P6: classified like any other IOSurface texture (below, before return): with only the VkImage as D-copy source (vkCmdCopyImageToBuffer).
        n48_baseless_note(IOSurfaceGetID(s));
        if (_w != sw || _h != sh) IOSFAIL(79, @"IOSurface has no CPU mapping and the descriptor %lux%lu differs from the plane %zux%zu", (unsigned long)_w, (unsigned long)_h, sw, sh);
        VkFormatFeatureFlags nbf = n48_fmt_feats(_fmt);
        BOOL nbAtt = !(_fmt->flags & N48F_A8) && (nbf & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        VkImageUsageFlags nbu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (nbAtt ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                                ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        VkImageCreateInfo nic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = nbu, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkResult nr = vkCreateImage(N48R.dev, &nic, NULL, &_img);
        if (nr != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal, no CPU mapping) = %d", nr); }
        VkMemoryRequirements nmr; vkGetImageMemoryRequirements(N48R.dev, _img, &nmr);
        int nmt = n48_find_mem(nmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (nmt < 0) IOSFAIL(86, @"no device-local memory type");
        if (N48P.on) {
            nr = n48_mem_alloc((uint32_t)nmt, nmr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (nr != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", nr);
            _pooled = YES; _acct = nmr.size; atomic_fetch_add(&n48_alloc_total, _acct);
            nr = vkBindImageMemory(N48R.dev, _img, _pm.mem, _pm.off);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", nr, (unsigned long long)_pm.off);
        } else {
            VkMemoryAllocateInfo nma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = nmr.size, .memoryTypeIndex = (uint32_t)nmt };
            nr = vkAllocateMemory(N48R.dev, &nma, NULL, &_mem);
            if (nr != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", nr); }
            _acct = nmr.size; atomic_fetch_add(&n48_alloc_total, _acct);
            nr = vkBindImageMemory(N48R.dev, _img, _mem, 0);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", nr);
        }
        _levels = 1; _depth = 1; _size = nmr.size;
        VkComponentMapping nbm = n48_view_map(_fmt, _swz); _viewSwz = !n48_is_identity_map(&nbm);
        VkImageViewCreateInfo nvc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = _fmt->vk, .components = nbm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        nr = vkCreateImageView(N48R.dev, &nvc, NULL, &_view);
        if (nr != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", nr);
        _iosNoBase = YES; _ios = (IOSurfaceRef)CFRetain(s); IOSurfaceIncrementUseCount(s);   // retained + use-counted like any IOSurface texture; set last so a failed init above never decrements
        N48LOG("newTexture(iosurface) %p: plane %lu %zux%zu bpr %zu pf %lu -> GPU-ONLY (no CPU mapping): optimal VkImage, not backed by the surface's memory", (__bridge void *)self, (unsigned long)plane, sw, sh, bpr, (unsigned long)_pf);
        // P6: same rule as the mapped surfaces (n48s_classify: geometry/pitch vs the plane + the DisplaySurface::GetMTLTexture backtrace). Inputs: width/height/pixel format/bytesPerRow/allocSize from the IOSurface
        // object (none needs a CPU mapping); alloc 0 -> bpr*height (n48df_nobase_alloc); the cache key's base is 0, which no mapped surface has (a mapped one passed the !base test above).
        // The image is the copy source, so the format must be 4 bytes per texel (the slot's pitch is in 4-byte texels) and the image must be the plane's size.
        if (pc <= 1 && _fmt->bpp == 4 && n48s_classify(sw, sh, spf, d.usage, bpr, (size_t)n48df_nobase_alloc(alloc, bpr, sh), IOSurfaceGetID(s), NULL)) {
            _disp = YES;
            static _Atomic int nblog; if (atomic_fetch_add(&nblog, 1) < 8) N48LOG("scanout: display surface %u has no CPU mapping (protected): its copies into the scanout slot come from the VkImage (vkCmdCopyImageToBuffer, TRANSFER_SRC)", IOSurfaceGetID(s));
        }
        return self;
    }
    if (((uintptr_t)base & (N48R.hostAlign - 1)) || !alloc) IOSFAIL(80, @"base %p / allocSize %zu: base not aligned to %llu (page-aligned surfaces only)", base, alloc, (unsigned long long)N48R.hostAlign);
    pin.alloc = alloc; pin.poff = ((uintptr_t)pbase >= (uintptr_t)base) ? (uint64_t)((uintptr_t)pbase - (uintptr_t)base) : UINT64_MAX;
    if (!n48pl_bound(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    const NSUInteger poff = (NSUInteger)pout.off;
    if (pc > 1) N48LOG("newTexture(iosurface): plane %lu of %zu: offset %lu size %zux%zu bpe %zu bpr %zu (bufferRowLength %zu texels)", (unsigned long)plane, pc, (unsigned long)poff, sw, sh, bpe, bpr, pout.rowlen);
    // m11h6: IOSurface allocations are page-granular in the VM map, but IOSurfaceGetAllocSize reports the exact byte size (e.g. 5760, 303360, 1840000).
    // Import the whole pages that hold the surface: round up to the host-pointer alignment. The extra tail bytes are the surface's own page, never another object's.
    const size_t ialloc = (alloc + (size_t)N48R.hostAlign - 1) & ~((size_t)N48R.hostAlign - 1);
    if (ialloc != alloc) N48LOG("newTexture(iosurface): allocSize %zu rounded up to %zu for the page-granular import", alloc, ialloc);
    if (ialloc > 64u << 20) IOSFAIL(82, @"allocSize %zu > 64 MiB (the N48N per-BO import limit)", ialloc);
    // Import (whole allocation). The kernel refuses BAR pages, unaligned ranges and > 256 MiB per client; that arrives as a vkAllocateMemory error.
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = n48_now();
    VkResult r = n48_vkGetMHPP(N48R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) IOSFAIL(83, @"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    int hmt = n48_find_mem(hp.memoryTypeBits, 0);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = base };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = ialloc, .memoryTypeIndex = (uint32_t)hmt };
    BOOL impHit = NO;
    if (N48IC.on) { r = n48_imp_get(s, base, ialloc, &ma, &_imem, &impHit); _impShared = YES; }   // P4: shared import (a reused one is a hit)
    else r = vkAllocateMemory(N48R.dev, &ma, NULL, &_imem);
    n48t_add(&T1.imp, n48_now() - timp_);
    if (r != VK_SUCCESS) { _imem = VK_NULL_HANDLE; _impShared = NO; IOSFAIL(84, @"vkAllocateMemory(import %zu B at %p) = %d", ialloc, base, r); }
    if (impHit) { static _Atomic int nlog; if (atomic_fetch_add(&nlog, 1) < 8) N48LOG("newTexture(iosurface): id %u import REUSED from the cache (%zu B)", IOSurfaceGetID(s), ialloc); }
    else @synchronized ([Navi48Device class]) { N48R.impTotal += ialloc; N48LOG("newTexture(iosurface): imported %zu B (host memory type %d, bits 0x%x); running import total %llu B", ialloc, hmt, hp.memoryTypeBits, (unsigned long long)N48R.impTotal); }
    _ialloc = ialloc; _ibase = (uint8_t *)base + poff; _ioff = poff; _ibpr = bpr; _ios = (IOSurfaceRef)CFRetain(s); IOSurfaceIncrementUseCount(s); _size = ialloc;
    VkFormatFeatureFlags offs = n48_fmt_feats(_fmt);
    BOOL attOK = !(_fmt->flags & N48F_A8) && (offs & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (attOK ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                            ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const char *force = getenv("N48M_IOS_PATH");   // test hook: "b" forces the copy path
    BOOL tryA = !(force && force[0] == 'b');
    if (tryA) {   // path (a) needs the LINEAR-tiling features for every usage asked for (RADV does not check at vkCreateImage); otherwise the copy path (b)
        VkFormatProperties lfp = {0}; vkGetPhysicalDeviceFormatProperties(N48R.pd, _fmt->vk, &lfp);
        VkFormatFeatureFlags lneed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            (attOK ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((lfp.linearTilingFeatures & lneed) != lneed) { N48LOG("newTexture(iosurface): path (a) unavailable: LINEAR features 0x%x lack 0x%x for vk format %d", lfp.linearTilingFeatures, lneed & ~lfp.linearTilingFeatures, _fmt->vk); tryA = NO; }
        else N48LOG("newTexture(iosurface): LINEAR features 0x%x cover 0x%x for vk format %d", lfp.linearTilingFeatures, lneed, _fmt->vk);
    }
    if (tryA) {
        VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage li = VK_NULL_HANDLE; r = vkCreateImage(N48R.dev, &ic, NULL, &li);
        if (r != VK_SUCCESS) N48LOG("newTexture(iosurface): path (a) unavailable: linear vkCreateImage = %d", r);
        else {
            VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(N48R.dev, li, &sr, &sl);
            VkMemoryRequirements mr; vkGetImageMemoryRequirements(N48R.dev, li, &mr);
            BOOL pitchOK = sl.rowPitch == bpr && sl.offset == 0;
            N48LOG("newTexture(iosurface): linear layout for %zux%zu: rowPitch %llu offset %llu size %llu vs IOSurface bytesPerRow %zu -> %s; image req size %llu align %llu typeBits 0x%x (host bits 0x%x)",
                   sw, sh, (unsigned long long)sl.rowPitch, (unsigned long long)sl.offset, (unsigned long long)sl.size, bpr, pitchOK ? "MATCH" : "MISMATCH",
                   (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits, hp.memoryTypeBits);
            BOOL fits = pitchOK && n48pl_bind_ok(poff, mr.size, mr.alignment, ialloc) && (mr.alignment == 0 || ((uintptr_t)base % mr.alignment) == 0) && (mr.memoryTypeBits & (1u << hmt));
            if (pitchOK && !fits && poff) N48LOG("newTexture(iosurface): plane offset %lu: LINEAR bind at that offset not possible (image align %llu size %llu, import %zu) -> copy path (b)", (unsigned long)poff, (unsigned long long)mr.alignment, (unsigned long long)mr.size, ialloc);
            if (pitchOK && !fits) N48LOG("newTexture(iosurface): path (a) unavailable: image requirements do not fit the import (size/alignment/memory type)");
            if (fits) { r = vkBindImageMemory(N48R.dev, li, _imem, poff); if (r != VK_SUCCESS) { N48LOG("newTexture(iosurface): path (a) unavailable: vkBindImageMemory = %d", r); fits = NO; } }
            if (fits) { _img = li; _iosLinear = YES; } else vkDestroyImage(N48R.dev, li, NULL);
        }
    }
    if (!_iosLinear) {   // path (b): optimal device-local image + a VkBuffer over the import
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        r = vkCreateImage(N48R.dev, &ic, NULL, &_img);
        if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal) = %d", r); }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(N48R.dev, _img, &mr);
        int mt = n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) IOSFAIL(86, @"no device-local memory type");
        VkMemoryAllocateInfo m2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        if (N48P.on) {   // P1 (the optimal device-local image of path (b); the import stays as it was)
            r = n48_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (r != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", r);
            _pooled = YES; _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
            r = vkBindImageMemory(N48R.dev, _img, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
        } else {
        r = vkAllocateMemory(N48R.dev, &m2, NULL, &_mem);
        if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", r); }
        _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
        r = vkBindImageMemory(N48R.dev, _img, _mem, 0);
        if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", r);
        }
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = alloc, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        r = vkCreateBuffer(N48R.dev, &bc, NULL, &_ibuf);
        if (r != VK_SUCCESS) { _ibuf = VK_NULL_HANDLE; IOSFAIL(89, @"vkCreateBuffer(import) = %d", r); }
        r = vkBindBufferMemory(N48R.dev, _ibuf, _imem, 0);
        if (r != VK_SUCCESS) IOSFAIL(90, @"vkBindBufferMemory(import) = %d", r);
    }
    _levels = 1; _depth = 1;
    VkComponentMapping icm = n48_view_map(_fmt, _swz); _viewSwz = !n48_is_identity_map(&icm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = _fmt->vk, .components = icm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(N48R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", r);
    N48LOG("newTexture(iosurface) %p: plane %lu offset %lu %zux%zu bpr %zu pf %lu -> PATH (%s): %s", (__bridge void *)self, (unsigned long)plane, (unsigned long)poff, sw, sh, bpr, (unsigned long)_pf, _iosLinear ? "a" : "b",
           _iosLinear ? "LINEAR VkImage bound directly to the imported IOSurface pages" : "optimal VkImage + copies to/from a VkBuffer over the imported pages (bufferRowLength = bytesPerRow/4)");
    // S5.2a: is this one of CoreDisplay's display surfaces (D-copy source)? A buffer over the import is the copy's source (path (b) already has _ibuf).
    if (pc <= 1 && n48s_classify(sw, sh, spf, d.usage, bpr, alloc, IOSurfaceGetID(s), base)) {
        if (_ibuf) _disp = YES;
        else {
            VkBufferCreateInfo dbc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = alloc, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
            VkResult dr = vkCreateBuffer(N48R.dev, &dbc, NULL, &_dbuf);
            if (dr == VK_SUCCESS) { dr = vkBindBufferMemory(N48R.dev, _dbuf, _imem, 0); if (dr == VK_SUCCESS) _disp = YES; }
            if (dr != VK_SUCCESS) { N48LOG("scanout: display surface %u: copy-source buffer = %d; this surface will not be flipped", IOSurfaceGetID(s), dr); if (_dbuf) { vkDestroyBuffer(N48R.dev, _dbuf, NULL); _dbuf = VK_NULL_HANDLE; } }
        }
    }
    #undef IOSFAIL
    return self;
}
// A texture over a buffer (Metal's linear texture; QuartzCore's client-memory backing stores). Single-level 2D, formats of the table, Shared/Managed buffer.
// Path (b) of the IOSurface design: an optimal device-local image plus copies between it and the buffer around each command buffer (upload at the first use,
// write-back at the end), so the buffer memory (the client's memory for a no-copy buffer) is always the CPU-visible truth.
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(N48Buffer *)b offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define BFAIL(code, ...) do { if (err) *err = n48_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = (MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = n48_fmt(_pf);
    if (!_fmt) BFAIL(120, @"pixel format %lu not supported", (unsigned long)_pf);
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) BFAIL(121, @"only single 2D textures (no mips/array/MSAA)");
    if ((_fmt->flags & N48F_A8) && (d.usage & MTLTextureUsageRenderTarget)) BFAIL(121, @"A8Unorm render targets are not supported");
    if (![b isKindOfClass:[N48Buffer class]] || ![b contents]) BFAIL(122, @"the buffer has no CPU mapping (Private storage)");
    uint32_t bpp = _fmt->bpp;
    if (!_w || !_h || bpr < _w * bpp || bpr % bpp) BFAIL(123, @"bytesPerRow %lu invalid for a %lu-wide %u-byte format", (unsigned long)bpr, (unsigned long)_w, bpp);
    if (off % 4 || off % bpp) BFAIL(124, @"offset %lu must be a multiple of 4 and of the pixel size", (unsigned long)off);
    if (off + bpr * (_h - 1) + _w * bpp > [b length]) BFAIL(125, @"offset %lu + %lu rows of %lu bytes exceed the %lu-byte buffer", (unsigned long)off, (unsigned long)_h, (unsigned long)bpr, (unsigned long)[b length]);
    if (!n48_radv_open(err)) return nil;
    BOOL attOK = NO;
    {   // m11h3: this path used to ask for colour-attachment + input-attachment usage always; m11h9: only when RADV supports them (A8 never), and a render target needs them
        VkFormatFeatureFlags ff = n48_fmt_feats(_fmt), nd = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            ((d.usage & MTLTextureUsageRenderTarget) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((ff & nd) != nd) BFAIL(131, @"pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)_pf, _fmt->vk, ff, nd & ~ff);
        attOK = !(_fmt->flags & N48F_A8) && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    }
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (attOK ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                 ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0) };
    VkResult r = vkCreateImage(N48R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; BFAIL(126, @"vkCreateImage = %d", r); }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(N48R.dev, _img, &mr);
    int mt = n48_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) BFAIL(127, @"no device-local memory type");
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (N48P.on) {   // P1
        r = n48_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
        if (r != VK_SUCCESS) BFAIL(128, @"vkAllocateMemory = %d (pool)", r);
        _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
        r = vkBindImageMemory(N48R.dev, _img, _pm.mem, _pm.off);
        if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
    } else {
    r = vkAllocateMemory(N48R.dev, &ma, NULL, &_mem);
    if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; BFAIL(128, @"vkAllocateMemory = %d", r); }
    _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&n48_alloc_total, _acct);
    r = vkBindImageMemory(N48R.dev, _img, _mem, 0);
    if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d", r);
    }
    _levels = 1; _depth = 1;
    VkComponentMapping bcm = n48_view_map(_fmt, _swz); _viewSwz = !n48_is_identity_map(&bcm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = _fmt->vk, .components = bcm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(N48R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) BFAIL(130, @"vkCreateImageView = %d", r);
    _hostBuf = b; _hoff = off; _ibase = (uint8_t *)[b contents] + off; _ibpr = bpr;
    N48LOG("N48Texture %p: %lux%lu pf %lu over buffer %p offset %lu bytesPerRow %lu (%s memory; copy path b)", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, (unsigned long)_pf,
           (__bridge void *)b, (unsigned long)off, (unsigned long)bpr, [b n48Imported] ? "client" : "bundle");
    #undef BFAIL
    return self;
}
- (BOOL)n48IsDisp { return _disp; }
- (const uint8_t *)n48IBase { return _ibase; }
- (IOSurfaceRef)n48IOSRef { return _iosNoBase ? NULL : _ios; }   // P4: a base-less texture is not an IOSurface-coherent texture (sid tracking, CRC) 
- (VkBuffer)n48DispBuf { return _ibuf ? _ibuf : _dbuf; }
- (BOOL)n48DispImgOnly { return _disp && _iosNoBase && !_ibuf && !_dbuf; }
- (unsigned)n48DispSid { return (_disp && _ios) ? (unsigned)IOSurfaceGetID(_ios) : 0; }
- (BOOL)n48IsIOS { return (_ios != NULL && !_iosNoBase) || _hostBuf != nil; }   // host-backed: IOSurface memory or a buffer's memory
- (NSUInteger)n48IOSOffset { return _hostBuf ? _hoff : _ioff; }
- (BOOL)n48IOSLinear { return _iosLinear; }
- (VkBuffer)n48IOSBuffer { return _hostBuf ? [_hostBuf vkBuffer] : _ibuf; }
- (size_t)n48IOSBytesPerRow { return _ibpr; }
// 11e CPU access. Shared and Managed textures accept replaceRegion:/getBytes: (Managed has no separate CPU copy here: both are the one
// DEVICE_LOCAL image reached through a host-visible staging buffer + a one-shot copy that is WAITED, so the data is in place when the call
// returns). Private textures are REFUSED (Metal does not allow CPU access to them): there is no NSError channel in these selectors, so the
// refusal is an N48LOG line and a no-op. Rows are re-packed tightly into the staging buffer, so any bytesPerRow is accepted.
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (!bytes) { N48LOG("replaceRegion: NULL bytes; refused"); return; }
    if ([self n48IsIOS]) {   // 11h.6: the IOSurface (or no-copy buffer) memory IS the CPU copy; the GPU copy (path b) is re-uploaded at the first use in each command buffer
        if (!n48_region_ok(self, r, lvl, slice, "replaceRegion")) return;
        NSUInteger rb = r.size.width * _fmt->bpp; if (bpr < rb) { N48LOG("replaceRegion: bytesPerRow %lu < row size %lu; refused", (unsigned long)bpr, (unsigned long)rb); return; }
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy(_ibase + (r.origin.y + y) * _ibpr + r.origin.x * _fmt->bpp, (const uint8_t *)bytes + y * bpr, rb);
        N48LOG("replaceRegion: %lux%lu into the IOSurface memory", (unsigned long)r.size.width, (unsigned long)r.size.height); return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { N48LOG("replaceRegion: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    if (!n48_region_ok(self, r, lvl, slice, "replaceRegion")) return;
    uint32_t bpp = _fmt->bpp; NSUInteger rowBytes = r.size.width * bpp, nz = r.size.depth;
    if (_is1D && bpr < rowBytes) bpr = rowBytes;   // Metal ignores bytesPerRow for 1D textures (the test passes 0)
    if (bpr < rowBytes) { N48LOG("replaceRegion: bytesPerRow %lu < row size %lu; refused", (unsigned long)bpr, (unsigned long)rowBytes); return; }
    if (nz > 1 && bpi < bpr * r.size.height) { N48LOG("replaceRegion: bytesPerImage %lu < bytesPerRow*height %lu; refused", (unsigned long)bpi, (unsigned long)(bpr * r.size.height)); return; }
    NSError *err = nil;
    NSUInteger opts = MTLResourceStorageModeShared;
    N48Buffer *stg = [[N48Buffer alloc] initWithDevice:_dev length:rowBytes * r.size.height * nz options:opts error:&err];
    if (!stg) { N48LOG("replaceRegion: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)[stg contents] + (z * r.size.height + y) * rowBytes, (const uint8_t *)bytes + z * bpi + y * bpr, rowBytes);
    VkBuffer sb = [stg vkBuffer];
    BOOL ok = n48_oneshot(&err, ^(VkCommandBuffer cmd) {
        n48_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)lvl, self->_is3D ? 0 : (uint32_t)slice, 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyBufferToImage(cmd, sb, [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        n48_tex_to(cmd, self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    N48LOG("replaceRegion: %lux%lux%lu at %lu,%lu,%lu level %lu (%s, storageMode %lu): %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)r.origin.x,
           (unsigned long)r.origin.y, (unsigned long)r.origin.z, (unsigned long)lvl, ok ? "uploaded" : "FAILED", (unsigned long)[self storageMode], ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr {
    [self replaceRegion:r mipmapLevel:lvl slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:bpr * r.size.height];
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    if (!dst) { N48LOG("getBytes: NULL destination; refused"); return; }
    if ([self n48IsIOS]) {   // 11h.6: contents are current in the IOSurface / buffer memory once the writing command buffer has completed
        if (!n48_region_ok(self, r, lvl, slice, "getBytes")) return;
        NSUInteger rb = r.size.width * _fmt->bpp; if (bpr < rb) { N48LOG("getBytes: bytesPerRow %lu < row size %lu; refused", (unsigned long)bpr, (unsigned long)rb); return; }
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy((uint8_t *)dst + y * bpr, _ibase + (r.origin.y + y) * _ibpr + r.origin.x * _fmt->bpp, rb);
        return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { N48LOG("getBytes: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    if (!n48_region_ok(self, r, lvl, slice, "getBytes")) return;
    uint32_t bpp = _fmt->bpp; NSUInteger rowBytes = r.size.width * bpp, nz = r.size.depth;
    if (_is1D && bpr < rowBytes) bpr = rowBytes;
    if (bpr < rowBytes) { N48LOG("getBytes: bytesPerRow %lu < row size %lu; refused", (unsigned long)bpr, (unsigned long)rowBytes); return; }
    if (nz > 1 && bpi < bpr * r.size.height) { N48LOG("getBytes: bytesPerImage %lu < bytesPerRow*height %lu; refused", (unsigned long)bpi, (unsigned long)(bpr * r.size.height)); return; }
    NSError *err = nil;
    N48Buffer *stg = [[N48Buffer alloc] initWithDevice:_dev length:rowBytes * r.size.height * nz options:MTLResourceStorageModeShared error:&err];
    if (!stg) { N48LOG("getBytes: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    VkBuffer sb = [stg vkBuffer]; VkImageLayout was = [self layout];
    BOOL ok = n48_oneshot(&err, ^(VkCommandBuffer cmd) {
        n48_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)lvl, self->_is3D ? 0 : (uint32_t)slice, 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyImageToBuffer(cmd, [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &bic);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) n48_tex_to(cmd, self, was);
    });
    if (ok) for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)dst + z * bpi + y * bpr, (const uint8_t *)[stg contents] + (z * r.size.height + y) * rowBytes, rowBytes);
    N48LOG("getBytes: %lux%lux%lu level %lu: %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)lvl, ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl {
    [self getBytes:dst bytesPerRow:bpr bytesPerImage:bpr * r.size.height fromRegion:r mipmapLevel:lvl slice:0];
}
- (VkImage)vkImage { return _root ? [_root vkImage] : _img; }
- (VkImageView)vkView { return _view; }
- (uint32_t)n48Layers { return _root ? [_root n48Layers] : (_layers ? (uint32_t)_layers : 1); }
- (uint32_t)n48Levels { return _root ? [_root n48Levels] : (uint32_t)(_levels ? _levels : 1); }
- (VkFormat)vkFormat { return _fmt->vk; }
- (uint32_t)bytesPerPixel { return _fmt->bpp; }
- (VkImageLayout)layout { return _root ? [_root layout] : _layout; }
- (void)setLayout:(VkImageLayout)l { if (_root) [_root setLayout:l]; else _layout = l; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (MTLTextureType)textureType { return _is1D ? (_arr1D ? MTLTextureType1DArray : MTLTextureType1D) : _is3D ? MTLTextureType3D : MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return _pf; }
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (NSUInteger)depth { return _is3D ? _depth : 1; }
- (NSUInteger)mipmapLevelCount { return _root ? 1 : (_levels ? _levels : 1); }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return _is1D ? _layers : 1; }
- (NSUInteger)usage { return _usage; }
- (BOOL)isFramebufferOnly { return NO; }
- (NSUInteger)firstMipmapInTail { return 0; }
- (NSUInteger)tailSizeInBytes { return 0; }
- (BOOL)isSparse { return NO; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (id)parentTexture { return _root; }
- (NSUInteger)parentRelativeLevel { return 0; }
- (NSUInteger)parentRelativeSlice { return 0; }
- (id)buffer { return _hostBuf; }
- (NSUInteger)bufferOffset { return _hoff; }
- (NSUInteger)bufferBytesPerRow { return _hostBuf ? _ibpr : 0; }
- (IOSurfaceRef)iosurface { return _ios; }
- (NSUInteger)iosurfacePlane { return _iosPlane; }
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_root ? [_root allocatedSize] : _size); }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
// ---- gap census: MTLResource / MTLTexture surface sent by QuartzCore, SkyLight and Metal ----
N48_RESOURCE_SPI
- (id)rootResource { return _root ? _root : self; }
- (BOOL)isShareable { return NO; }
- (id)newSharedTextureHandle { N48_ONCE("newSharedTextureHandle: shared texture handles are not implemented; nil"); return nil; }
- (id)remoteStorageTexture { return nil; }
- (id)newRemoteTextureViewForDevice:(id)d { (void)d; return nil; }
- (MTLTextureSwizzleChannels)swizzle { return _swz; }
- (BOOL)isCompressed { return NO; }
- (BOOL)isDrawable { return NO; }
- (void)didModifyData {}
- (BOOL)canGenerateMipmapLevels { return _levels > 1; }
- (void)generateMipmapLevel:(NSUInteger)l slice:(NSUInteger)s { (void)l; (void)s; }
- (uint32_t)swizzleKey { return 0; }
- (NSUInteger)numFaces { return 1; }
- (NSUInteger)resourceIndex { return 0; }
- (void *)virtualAddress { return [self n48IsIOS] ? (void *)_ibase : NULL; }
// Views. Only same-size reinterpretation of a whole single-level 2D texture: a level range {0,1}, a slice range {0,1}, textureType 2D, an optional swizzle.
// The root must have been created with MTLTextureUsagePixelFormatView when the pixel format changes (that sets VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT), and must not be
// IOSurface-backed (the IOSurface upload / write-back machinery tracks the root texture object; a view would bypass it).
- (id)n48View:(MTLPixelFormat)pf type:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    if (tt != MTLTextureType2D || lv.location != 0 || lv.length != 1 || sl.location != 0 || sl.length != 1) {
        N48LOG("newTextureView: type %lu levels %lu+%lu slices %lu+%lu: only a 2D view of level 0 slice 0 exists (no mips/arrays); nil", (unsigned long)tt, (unsigned long)lv.location,
               (unsigned long)lv.length, (unsigned long)sl.location, (unsigned long)sl.length); return nil; }
    NSError *e = nil; N48Texture *v = [[N48Texture alloc] initViewOf:self pixelFormat:pf swizzle:sw error:&e];
    if (!v) N48LOG("newTextureView: refused: %s", e.localizedDescription.UTF8String);
    return v;
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf {
    return [self n48View:pf type:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl {
    return [self n48View:pf type:tt levels:lv slices:sl swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    return [self n48View:pf type:tt levels:lv slices:sl swizzle:sw];
}
- (instancetype)initViewOf:(N48Texture *)root pixelFormat:(MTLPixelFormat)pf swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define VFAIL(code, ...) do { if (err) *err = n48_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    if (!root || root->_root) VFAIL(92, @"a view needs a root texture");
    if ([root n48IsIOS]) VFAIL(93, @"views of IOSurface-backed textures are not implemented");
    if (root->_is1D) VFAIL(99, @"views of 1D / 1DArray textures are not implemented");
    const N48Fmt *f = n48_fmt(pf);
    if (!f) VFAIL(94, @"view pixel format %lu not supported", (unsigned long)pf);
    if (pf != root->_pf) {
        if (f->bpp != root->_fmt->bpp) VFAIL(95, @"view pixel format %lu has %u bytes per pixel, the texture has %u", (unsigned long)pf, f->bpp, root->_fmt->bpp);
        if (!(root->_usage & MTLTextureUsagePixelFormatView)) VFAIL(96, @"the texture was not created with MTLTextureUsagePixelFormatView");
    }
    _root = root; _dev = root->_dev; _w = root->_w; _h = root->_h; _pf = pf; _fmt = f; _usage = root->_usage; _opts = root->_opts; _swz = swz;
    if (swz.red > 5 || swz.green > 5 || swz.blue > 5 || swz.alpha > 5) VFAIL(97, @"bad swizzle");
    if (root->_is3D) VFAIL(99, @"views of 3D textures are not implemented");
    VkComponentMapping vcm = n48_view_map(f, swz);   // composes the client swizzle with the format's own (A8: (0,0,0,R))
    _levels = 1; _depth = 1; _viewSwz = YES;
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [root vkImage], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f->vk,
        .components = vcm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkResult r = vkCreateImageView(N48R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) VFAIL(98, @"vkCreateImageView = %d", r);
    N48LOG("N48Texture view %p of %p: pf %lu -> %lu (vk format %d -> %d), swizzle %u%u%u%u", (__bridge void *)self, (__bridge void *)root, (unsigned long)root->_pf, (unsigned long)pf, root->_fmt->vk, f->vk,
           swz.red, swz.green, swz.blue, swz.alpha);
    #undef VFAIL
    return self;
}
@end

// ---------------------------------------------------------------------------------------------------------------
// m11 H1: MTLHeap (automatic heaps). SkyLight's MetalCompositor sends -[MTLDevice newHeapWithDescriptor:] through MPSSupportsMTLDevice -> MPSDevice::MPSDevice
// (MPSCore 0x7ff810821b40: setSize: / setStorageMode: / setCpuCacheMode: / setResourceOptions: / setProtectionOptions: / newHeapWithDescriptor:, then
// -setPurgeableState:3 and -size on the heap; CONFIRMED against the carved MPSCore and the WindowServer crash report of an earlier run).
// Design: the heap is a BUDGET OBJECT with a first-fit offset allocator. It holds no memory. Every buffer/texture made from it is an ordinary N48Buffer /
// N48Texture (own VkBuffer / VkImage + VkDeviceMemory) whose -heap, -heapOffset, -allocatedSize and -isAliasable answer for the heap and whose release (or
// -makeAliasable) returns the budget. The heap's size is added to the device's currentAllocatedSize at creation (an Apple-silicon Mac does the same: the whole heap is
// resident) and the heap's resources are not counted again. Aliasing does not exist (placement heaps are refused), so two live resources never share bytes.
// Values below were measured on an Apple-silicon Mac first (mtlprobe heap): size rounds up to 16 KiB; usedSize = sum of the requested sizes (not rounded);
// maxAvailableSizeWithAlignment = largest free gap after aligning its start; automatic heaps return nil for the offset variants; hazardTrackingMode
// Default becomes Untracked; resourceOptions = storage << 4 | cpuCache | hazard << 8; setPurgeableState: returns the previous state (initially NonVolatile).
// Bundle-specific refusals (an Apple-silicon Mac aborts in its validation layer on these, so they are PC-only tests): resource storage/cpu-cache mode different from the
// heap's, a Tracked/Untracked request that differs from the heap's, Memoryless/placement/sparse heaps.
// ---------------------------------------------------------------------------------------------------------------
#define N48_HEAP_PAGE   16384ULL
#define N48_HEAP_ALIGN  256ULL
#define N48_HEAP_MAX    (0x300000000ULL)   /* 12 GiB, the recommended working set */
static NSUInteger n48_alup(NSUInteger v, NSUInteger a) { return (NSUInteger)n48ha_alup(v, a); }
// Size and alignment of a texture inside a heap: the tight pixel bytes (all slices), 256-aligned. Device-specific (an Apple-silicon Mac says 16512 for 64x64 RGBA8; any
// consistent value is correct for a budget).
static BOOL n48_heap_tex_sa(MTLTextureDescriptor *d, NSUInteger *size, NSUInteger *align) {
    const N48Fmt *f = n48_fmt(d.pixelFormat); if (!f) return NO;
    NSUInteger layers = d.textureType == MTLTextureType1DArray ? d.arrayLength : 1, lv = d.mipmapLevelCount ? d.mipmapLevelCount : 1, dp = d.textureType == MTLTextureType3D ? (d.depth ? d.depth : 1) : 1, tot = 0;
    for (NSUInteger L = 0; L < lv; L++) tot += MAX((NSUInteger)1, d.width >> L) * MAX((NSUInteger)1, d.height >> L) * MAX((NSUInteger)1, dp >> L) * layers * f->bpp;   // m11h9: every level, 3D depth
    *size = n48_alup(tot, N48_HEAP_ALIGN); *align = N48_HEAP_ALIGN; return YES;
}
@implementation N48Heap {
    id _dev; NSString *_lbl; NSUInteger _size, _purge; unsigned _st, _cc, _hz; N48HeapAlloc _ha;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d {
    self = [super init]; if (!self) return nil;
    NSUInteger sz = d.size; unsigned st = (unsigned)d.storageMode, cc = (unsigned)d.cpuCacheMode, hz = (unsigned)d.hazardTrackingMode;
    #define HFAIL(...) do { N48LOG("newHeapWithDescriptor: NIL: " __VA_ARGS__); return nil; } while (0)
    if (d.type != MTLHeapTypeAutomatic) HFAIL("heap type %lu: only automatic heaps exist here (no aliasing, no placement)", (unsigned long)d.type);
    if (!sz) HFAIL("size 0");
    if (st > 2) HFAIL("storage mode %u (Memoryless) not supported", st);
    if (hz > 2) HFAIL("hazard tracking mode %u", hz);
    sz = n48_alup(sz, N48_HEAP_PAGE);
    if (sz > N48_HEAP_MAX) HFAIL("size %lu > %llu", (unsigned long)sz, N48_HEAP_MAX);
    #undef HFAIL
    _dev = dev; _size = sz; _st = st; _cc = cc; _hz = hz ? hz : 1; _purge = 2; n48ha_init(&_ha, sz);
    atomic_fetch_add(&n48_alloc_total, (uint64_t)_size);
    N48LOG("N48Heap %p: %lu bytes storage %u cache %u hazard %u (budget object, no memory)", (__bridge void *)self, (unsigned long)_size, _st, _cc, _hz);
    return self;
}
- (void)dealloc { if (_size) atomic_fetch_sub(&n48_alloc_total, (uint64_t)_size); n48ha_destroy(&_ha); }
- (void)doesNotRecognizeSelector:(SEL)sel {
    N48LOG("UNRECOGNIZED selector %s on N48Heap", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
// first fit by offset (n48_heapalloc.h); returns NO when no gap holds `sz` at an `al`-aligned offset
- (BOOL)n48AllocSize:(NSUInteger)sz align:(NSUInteger)al offset:(NSUInteger *)off id:(uint64_t *)bid {
    @synchronized (self) { uint64_t o; if (!n48ha_alloc(&_ha, sz, al, &o, bid)) return NO; *off = (NSUInteger)o; return YES; }
}
- (void)n48Free:(uint64_t)bid { @synchronized (self) { n48ha_free(&_ha, bid); } }
- (BOOL)n48OptionsOK:(NSUInteger)o what:(const char *)what {
    unsigned st = (unsigned)((o >> 4) & 0xF), cc = (unsigned)(o & 0xF), hz = (unsigned)((o >> 8) & 3);
    if (st != _st || cc != _cc || (hz && hz != _hz)) {
        N48LOG("heap %s NIL: requested storage %u cache %u hazard %u does not match the heap's %u / %u / %u (Apple's validation layer aborts on this)", what, st, cc, hz, _st, _cc, _hz); return NO; }
    return YES;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    if (!len || len > _size || ![self n48OptionsOK:o what:"newBufferWithLength:options:"]) { if (!len || len > _size) N48LOG("heap newBufferWithLength: NIL: length %lu (heap %lu)", (unsigned long)len, (unsigned long)_size); return nil; }
    NSUInteger off; uint64_t bid;
    if (![self n48AllocSize:len align:N48_HEAP_ALIGN offset:&off id:&bid]) { N48LOG("heap newBufferWithLength: NIL: no gap for %lu bytes (used %lu of %lu)", (unsigned long)len, (unsigned long)_ha.used, (unsigned long)_size); return nil; }
    NSError *e = nil; NSUInteger ro = ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8);
    N48Buffer *b = [[N48Buffer alloc] initWithDevice:_dev length:len options:ro error:&e];
    if (!b) { [self n48Free:bid]; N48LOG("heap newBuffer: failed: %s", e.localizedDescription.UTF8String); return nil; }
    [b n48AdoptHeap:self offset:off size:len bid:bid];
    return b;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    NSUInteger sz, al;
    if (!n48_heap_tex_sa(td, &sz, &al)) { N48LOG("heap newTextureWithDescriptor NIL: pf %lu not supported", (unsigned long)td.pixelFormat); return nil; }
    if (![self n48OptionsOK:((NSUInteger)td.storageMode << 4) | (NSUInteger)td.cpuCacheMode | ((NSUInteger)td.hazardTrackingMode << 8) what:"newTextureWithDescriptor:"]) return nil;
    NSUInteger off; uint64_t bid;
    if (sz > _size || ![self n48AllocSize:sz align:al offset:&off id:&bid]) { N48LOG("heap newTextureWithDescriptor NIL: no gap for %lu bytes (used %lu of %lu)", (unsigned long)sz, (unsigned long)_ha.used, (unsigned long)_size); return nil; }
    MTLTextureDescriptor *c = [td copy]; c.hazardTrackingMode = (MTLHazardTrackingMode)_hz;
    NSError *e = nil; N48Texture *t = [[N48Texture alloc] initWithDevice:_dev descriptor:c error:&e];
    if (!t) { [self n48Free:bid]; N48LOG("heap newTextureWithDescriptor NIL: pf %lu %lux%lu (%s)", (unsigned long)td.pixelFormat, (unsigned long)td.width, (unsigned long)td.height, e.localizedDescription.UTF8String); return nil; }
    [t n48AdoptHeap:self offset:off size:sz bid:bid];
    return t;
}
// offset (placement) variants: an Apple-silicon Mac returns nil for them on an automatic heap
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off { (void)l; (void)o; (void)off; N48_ONCE("heap newBufferWithLength:options:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off { (void)l; (void)o; (void)off; N48_ONCE("heap newBufferWithLength:options:atOffset: on an automatic heap: nil"); return nil; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off { (void)d; (void)off; N48_ONCE("heap newTextureWithDescriptor:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off { (void)d; (void)off; N48_ONCE("heap newTextureWithDescriptor:atOffset: on an automatic heap: nil"); return nil; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al { @synchronized (self) { return (NSUInteger)n48ha_maxavail(&_ha, al); } }
- (NSUInteger)setPurgeableState:(NSUInteger)st {
    @synchronized (self) { NSUInteger prev = _purge; if (st >= 2 && st <= 4) _purge = st; N48_ONCE("heap setPurgeableState: tracked as a flag only (no memory is ever reclaimed)"); return prev; }
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { @synchronized (self) { return (NSUInteger)_ha.used; } }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)allocatedSize { return _size; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)_st; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)_cc; }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)_hz; }
- (NSUInteger)resourceOptions { return ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8); }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
- (MTLHeapType)type { return MTLHeapTypeAutomatic; }
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)gpuAddress { N48_ONCE("heap gpuAddress: no GPU virtual address is exported; 0"); return 0; }
- (uint64_t)memoryPoolId { return 0; }
// -[_MTLHeap description] calls -formattedDescription: (found on an Apple-silicon Mac: it sends -usedSize and more); a plain string here, NOT [self description] (recursion)
- (NSString *)formattedDescription:(NSUInteger)indent { (void)indent; return [NSString stringWithFormat:@"<N48Heap: %p> size %lu used %lu storage %u cache %u hazard %u label %@", (__bridge void *)self, (unsigned long)_size, (unsigned long)[self usedSize], _st, _cc, _hz, _lbl]; }
@end

// ---------------------------------------------------------------------------------------------------------------
// 11e binding model (metal2vulkan docs/REFLECTION.md, "Descriptor ABI (binding map)"; src/reflect/mod.rs constants).
// Every descriptor-backed Metal resource is a descriptor in ONE set; within the set (default ABI):
//   [[buffer(n)]]  -> binding n (0..31), storage buffer        [[texture(n)]] sampled  -> binding 32+n (32..159), sampled image
//   [[sampler(n)]] -> binding 160+n (n 0..15), sampler; a constexpr sampler ("StaticSampler") takes the first free binding in 160..191
//   writable [[texture(n)]] -> binding 480+n (480..607), storage image     synthetic (BufferAddressTable etc.) >= 640
//   compute dispatch grid -> 48-byte push-constant block (12 u32) + spec ids 0,1,2 = local size (KERNEL_LOCAL_SIZE_SPEC_IDS)
//   [[stage_in]] vertex attributes -> Vulkan vertex-input Location (= attribute index), buffer index = binding.
// Each stage is translated independently and both use set 0, so vertex buffer(0) and fragment buffer(0) would collide. Metal
// keeps the two argument tables apart, so the bundle keeps them apart too: the FRAGMENT module's DescriptorSet decorations are
// rewritten 0 -> 1 when the pipeline is built (the equivalent of TransformOptions::with_descriptor_layout(set = 1) that
// REFLECTION.md offers per independently translated stage); binding numbers inside a set are untouched. Vertex = set 0, fragment
// = set 1, compute = set 0. Static samplers and the compute dispatch contract come from spvcache/<sha>.meta.json (add-air.py).
// ---------------------------------------------------------------------------------------------------------------

// ---- samplers ----
typedef struct { int minF, magF, mipF, as, at, ar, border, cmp; BOOL unnorm; float lodMin, lodMax; } N48SampCfg;   // Metal enum values
static VkSampler n48_make_sampler(const N48SampCfg *c, NSError **err) {
    if (!n48_radv_open(err)) return VK_NULL_HANDLE;
    VkSamplerAddressMode am[3]; int in[3] = { c->as, c->at, c->ar }; BOOL zero = NO;
    for (int i = 0; i < 3; i++) switch (in[i]) {
        case 0: am[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;   // MTLSamplerAddressModeClampToEdge
        case 2: am[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 3: am[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case 4: am[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; zero = YES; break;   // ClampToZero
        case 5: am[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;                // ClampToBorderColor
        default: if (err) *err = n48_err(70, [NSString stringWithFormat:@"sampler address mode %d (MirrorClampToEdge) not supported", in[i]]); return VK_NULL_HANDLE;
    }
    VkBorderColor bc = zero || c->border == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : c->border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSamplerCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = c->magF ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = c->minF ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = c->mipF == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = am[0], .addressModeV = am[1], .addressModeW = am[2],
        .compareEnable = c->cmp > 0, .compareOp = (VkCompareOp)c->cmp,   // MTLCompareFunction 0..7 == VkCompareOp 0..7 (Never..Always); Never = no compare
        .minLod = c->mipF == 0 || c->unnorm ? 0.0f : c->lodMin, .maxLod = c->mipF == 0 || c->unnorm ? 0.0f : (c->lodMax > 1000.0f ? 1000.0f : c->lodMax),
        .borderColor = bc, .unnormalizedCoordinates = c->unnorm };
    if (c->unnorm && c->magF != c->minF) { if (err) *err = n48_err(71, @"unnormalized-coordinate sampler needs min filter == mag filter"); return VK_NULL_HANDLE; }
    VkSampler s = VK_NULL_HANDLE; VkResult r = vkCreateSampler(N48R.dev, &sc, NULL, &s);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(72, [NSString stringWithFormat:@"vkCreateSampler = %d", r]); return VK_NULL_HANDLE; }
    return s;
}

@interface N48SamplerState : NSObject { id _dev; NSString *_lbl; VkSampler _s; }
- (instancetype)initWithDevice:(id)dev descriptor:(MTLSamplerDescriptor *)d error:(NSError **)err;
- (VkSampler)vkSampler;
@end
@implementation N48SamplerState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLSamplerDescriptor *)d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lbl = [d.label copy];
    N48SampCfg c = { .minF = (int)d.minFilter, .magF = (int)d.magFilter, .mipF = (int)d.mipFilter, .as = (int)d.sAddressMode, .at = (int)d.tAddressMode,
        .ar = (int)d.rAddressMode, .border = (int)d.borderColor, .cmp = (int)d.compareFunction, .unnorm = !d.normalizedCoordinates, .lodMin = d.lodMinClamp, .lodMax = d.lodMaxClamp };
    if (d.maxAnisotropy > 1) N48LOG("sampler: maxAnisotropy %lu ignored (samplerAnisotropy not enabled)", (unsigned long)d.maxAnisotropy);
    _s = n48_make_sampler(&c, err);
    if (!_s) return nil;
    N48LOG("N48SamplerState %p: vk %p (min %d mag %d mip %d addr %d/%d/%d cmp %d %s)", (__bridge void *)self, (void *)_s, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.cmp, c.unnorm ? "unnormalized" : "normalized");
    return self;
}
- (void)dealloc { if (N48R.ok && _s) vkDestroySampler(N48R.dev, _s, NULL); }
N48_DNR(N48SamplerState)
- (VkSampler)vkSampler { return _s; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// ---- gap census: depth/stencil state and fences ----
// MTLDepthStencilState: SkyLight and QuartzCore create one each at start-up (-newDepthStencilStateWithDescriptor:, census rdt1), and the compositor sends
// -setDepthStencilState: on its encoders. The render passes here have colour attachments only (no depth/stencil exist), for which Metal makes the
// state a no-op, so this object records the descriptor and nothing else.
@interface N48DepthStencilState : NSObject { id _dev; NSString *_lbl; MTLDepthStencilDescriptor *_d; }
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d;
- (MTLDepthStencilDescriptor *)n48Descriptor;
@end
@implementation N48DepthStencilState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d {
    self = [super init]; if (!self) return nil;
    _dev = dev; _d = [d copy]; _lbl = [d.label copy];
    N48LOG("N48DepthStencilState %p: depth compare %lu write %d (recorded only: no depth/stencil attachments exist)", (__bridge void *)self, (unsigned long)d.depthCompareFunction, (int)d.depthWriteEnabled);
    return self;
}
N48_DNR(N48DepthStencilState)
- (MTLDepthStencilDescriptor *)n48Descriptor { return _d; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// MTLFence: encoders update/wait fences (-updateFence:afterStages:, -waitForFence:beforeStages:). Every encoder here ends with a full memory barrier and
// all work goes through ONE VkQueue in submission order, so a fence between encoders of one device is already satisfied by ordering; the object only
// carries identity and label.
@interface N48Fence : NSObject { id _dev; NSString *_lbl; }
- (instancetype)initWithDevice:(id)dev;
@end
@implementation N48Fence
- (instancetype)initWithDevice:(id)dev { self = [super init]; if (!self) return nil; _dev = dev; return self; }
N48_DNR(N48Fence)
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// Residency / fence calls that are satisfied by the rules above, shared by all three encoders.
#define N48_ENCODER_NOOPS \
    - (void)useResource:(id)r usage:(NSUInteger)u { (void)r; (void)u; N48_ONCE("useResource: memory is always resident; ignored"); } \
    - (void)useResource:(id)r usage:(NSUInteger)u stages:(NSUInteger)st { (void)r; (void)u; (void)st; N48_ONCE("useResource:stages: ignored"); } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u { (void)r; (void)n; (void)u; N48_ONCE("useResources: ignored"); } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u stages:(NSUInteger)st { (void)r; (void)n; (void)u; (void)st; N48_ONCE("useResources:stages: ignored"); } \
    - (void)useHeap:(id)h { (void)h; N48_ONCE("useHeap: ignored"); } \
    - (void)useHeap:(id)h stages:(NSUInteger)st { (void)h; (void)st; N48_ONCE("useHeap:stages: ignored"); } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n { (void)h; (void)n; N48_ONCE("useHeaps: ignored"); } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n stages:(NSUInteger)st { (void)h; (void)n; (void)st; N48_ONCE("useHeaps:stages: ignored"); } \
    - (void)useResourceGroup:(id)g usage:(NSUInteger)u stages:(NSUInteger)st { (void)g; (void)u; (void)st; N48_ONCE("useResourceGroup: ignored"); }

// ---- spvcache sidecars (<sha>.meta.json, add-air.py) ----
// Hot-swap H2: the lookup directories, in order: the bundle's Resources/spvcache (read-only in practice), then the side directory
// /private/var/tmp/n48m-spv (WindowServer-sandbox readable; the profile allows /private/var/tmp; the translator installs new <sha>.meta.json then
// <sha>.spv there by atomic rename, .spv LAST). Test hooks (root + N48M_ALLOW=1): N48M_TEST_SIDE_DIR replaces the side dir, N48M_TEST_HIDE_BUNDLE_SPV=1 drops the bundle dir.
static NSString *n48_side_dir(void) {
    static NSString *d; static dispatch_once_t o;
    dispatch_once(&o, ^{ const char *t = getenv("N48M_TEST_SIDE_DIR"); d = (n48_allow() && t && *t) ? @(t) : @"/private/var/tmp/n48m-spv"; });
    return d;
}
static NSArray<NSString *> *n48_spv_dirs(void) {
    static NSArray *a; static dispatch_once_t o;
    dispatch_once(&o, ^{
        NSMutableArray *m = [NSMutableArray array]; const char *h = getenv("N48M_TEST_HIDE_BUNDLE_SPV");
        if (!(n48_allow() && h && !strcmp(h, "1"))) [m addObject:[[[NSBundle bundleForClass:[Navi48Device class]] resourcePath] stringByAppendingPathComponent:@"spvcache"]];
        [m addObject:n48_side_dir()]; a = m; });
    return a;
}
static NSDictionary *n48_meta_for(NSString *sha) {
    NSData *d = nil;
    for (NSString *dir in n48_spv_dirs()) { d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:[sha stringByAppendingString:@".meta.json"]]]; if (d.length) break; }
    if (!d.length) return nil;
    id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
    return [j isKindOfClass:[NSDictionary class]] ? j : nil;
}
static int n48_idx(id s, const char *const *tbl) {
    if (![s isKindOfClass:[NSString class]]) return -1;
    for (int i = 0; tbl[i]; i++) if (!strcmp(tbl[i], [(NSString *)s UTF8String])) return i;
    return -1;
}
// Static (constexpr) sampler state from a reflection entry -> N48SampCfg (Metal enum values).
static BOOL n48_static_cfg(NSDictionary *ss, N48SampCfg *c, NSError **err) {
    static const char *const filt[] = { "Nearest", "Linear", NULL }, *const mip[] = { "None", "Nearest", "Linear", NULL };
    static const char *const addr[] = { "ClampToEdge", "MirrorClampToEdge", "Repeat", "MirroredRepeat", "ClampToZero", "ClampToBorder", NULL };   // index = MTLSamplerAddressMode
    static const char *const cmpf[] = { "None", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always", "Never", NULL };
    static const char *const bord[] = { "TransparentBlack", "OpaqueBlack", "OpaqueWhite", NULL };
    int mf = n48_idx(ss[@"min_filter"], filt), gf = n48_idx(ss[@"mag_filter"], filt), mp = n48_idx(ss[@"mip_filter"], mip), a1 = n48_idx(ss[@"address_mode_s"], addr),
        a2 = n48_idx(ss[@"address_mode_t"], addr), a3 = n48_idx(ss[@"address_mode_r"], addr), cf = n48_idx(ss[@"compare_function"], cmpf), bc = n48_idx(ss[@"border_color"], bord);
    if (mf < 0 || gf < 0 || mp < 0 || a1 < 0 || a2 < 0 || a3 < 0 || cf < 0 || bc < 0) {
        if (err) *err = n48_err(73, [NSString stringWithFormat:@"static sampler state not understood: %@", ss]); return NO; }
    *c = (N48SampCfg){ .minF = mf, .magF = gf, .mipF = mp, .as = a1, .at = a2, .ar = a3, .border = bc,
        .cmp = cf == 8 ? 0 : cf == 0 ? 0 : cf, .unnorm = [ss[@"coordinates"] isEqual:@"Pixel"], .lodMin = [ss[@"lod_min_clamp"] floatValue], .lodMax = [ss[@"lod_max_clamp"] floatValue] };
    // JSON compare index -> MTLCompareFunction: Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7 (== table index for 1..7)
    return YES;
}

// One descriptor binding of a stage, from SPIR-V reflection (+ the meta sidecar for static samplers).
typedef struct { uint32_t binding; VkDescriptorType type; uint32_t count; VkSampler stat; } N48PB;

// Rewrites every `OpDecorate <id> DescriptorSet 0` of a module to `set` (see the binding-model comment above).
static NSData *n48_spv_set(NSData *spv, uint32_t set) {
    NSMutableData *m = [spv mutableCopy]; uint32_t *w = m.mutableBytes; size_t nw = m.length / 4, p = 5;
    while (p < nw) {
        uint32_t op = w[p] & 0xffff, wc = w[p] >> 16; if (!wc || p + wc > nw) break;
        if (op == 71 && wc >= 4 && w[p + 2] == 34) w[p + 3] = set;   // OpDecorate target DescriptorSet <n>
        p += wc;
    }
    return m;
}

// One stage's descriptor-set layout for set `set` from its reflection; pb/npb = the stage's binding list (malloc'd). r may be NULL (no stage).
static BOOL n48_stage_dsl(const N48sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, N48PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, NSError **err) {
    *pbOut = NULL; *npb = 0;
    VkDescriptorSetLayoutBinding *bnd = calloc(N48S_MAXBIND, sizeof *bnd); N48PB *pb = calloc(N48S_MAXBIND, sizeof *pb); uint32_t nb = 0;
    if (r) {
        if (r->maxset > set) { free(bnd); free(pb); if (err) *err = n48_err(49, [NSString stringWithFormat:@"stage uses descriptor set %d (> %d)", r->maxset, set]); return NO; }
        for (int b = 0; b < N48S_MAXBIND; b++) {
            const N48sBind *a = &r->bind[set][b]; if (!a->used) continue;
            bnd[nb] = (VkDescriptorSetLayoutBinding){ (uint32_t)b, a->type, a->count, stage, NULL };
            pb[nb] = (N48PB){ (uint32_t)b, a->type, a->count, VK_NULL_HANDLE };
            nb++;
        }
    }
    if (meta) for (NSDictionary *bd in meta[@"bindings"]) {
        if (![bd[@"kind"] isEqual:@"StaticSampler"] || ![bd[@"static_sampler"] isKindOfClass:[NSDictionary class]]) continue;
        uint32_t bn = (uint32_t)[bd[@"descriptor"][@"binding"] unsignedIntValue];
        for (uint32_t i = 0; i < nb; i++) if (pb[i].binding == bn && pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            N48SampCfg c; NSError *e = nil;
            if (!n48_static_cfg(bd[@"static_sampler"], &c, &e)) { free(bnd); free(pb); if (err) *err = e; return NO; }
            VkSampler s = n48_make_sampler(&c, &e);
            if (!s) { free(bnd); free(pb); if (err) *err = e; return NO; }
            pb[i].stat = s; [ownedSamplers addObject:[NSValue valueWithPointer:(void *)s]];
            N48LOG("static sampler at binding %u: min %d mag %d mip %d addr %d/%d/%d %s", bn, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.unnorm ? "unnormalized (coord::pixel)" : "normalized");
        }
    }
    VkDescriptorSetLayoutCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = bnd };
    VkResult vr = vkCreateDescriptorSetLayout(N48R.dev, &dc, NULL, dsl);
    free(bnd);
    if (vr != VK_SUCCESS) { free(pb); if (err) *err = n48_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout = %d", vr]); return NO; }
    if (nb) { *pbOut = realloc(pb, nb * sizeof *pb); *npb = nb; } else free(pb);
    return YES;
}

// MTLVertexFormat -> VkFormat (0 = unsupported).
static VkFormat n48_vkvfmt(MTLVertexFormat f) {
    switch (f) {
    case MTLVertexFormatUChar: return VK_FORMAT_R8_UINT; case MTLVertexFormatUChar2: return VK_FORMAT_R8G8_UINT; case MTLVertexFormatUChar3: return VK_FORMAT_R8G8B8_UINT; case MTLVertexFormatUChar4: return VK_FORMAT_R8G8B8A8_UINT;
    case MTLVertexFormatChar: return VK_FORMAT_R8_SINT; case MTLVertexFormatChar2: return VK_FORMAT_R8G8_SINT; case MTLVertexFormatChar3: return VK_FORMAT_R8G8B8_SINT; case MTLVertexFormatChar4: return VK_FORMAT_R8G8B8A8_SINT;
    case MTLVertexFormatUCharNormalized: return VK_FORMAT_R8_UNORM; case MTLVertexFormatUChar2Normalized: return VK_FORMAT_R8G8_UNORM; case MTLVertexFormatUChar3Normalized: return VK_FORMAT_R8G8B8_UNORM; case MTLVertexFormatUChar4Normalized: return VK_FORMAT_R8G8B8A8_UNORM;
    case MTLVertexFormatCharNormalized: return VK_FORMAT_R8_SNORM; case MTLVertexFormatChar2Normalized: return VK_FORMAT_R8G8_SNORM; case MTLVertexFormatChar3Normalized: return VK_FORMAT_R8G8B8_SNORM; case MTLVertexFormatChar4Normalized: return VK_FORMAT_R8G8B8A8_SNORM;
    case MTLVertexFormatUChar4Normalized_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
    case MTLVertexFormatUShort: return VK_FORMAT_R16_UINT; case MTLVertexFormatUShort2: return VK_FORMAT_R16G16_UINT; case MTLVertexFormatUShort3: return VK_FORMAT_R16G16B16_UINT; case MTLVertexFormatUShort4: return VK_FORMAT_R16G16B16A16_UINT;
    case MTLVertexFormatShort: return VK_FORMAT_R16_SINT; case MTLVertexFormatShort2: return VK_FORMAT_R16G16_SINT; case MTLVertexFormatShort3: return VK_FORMAT_R16G16B16_SINT; case MTLVertexFormatShort4: return VK_FORMAT_R16G16B16A16_SINT;
    case MTLVertexFormatUShortNormalized: return VK_FORMAT_R16_UNORM; case MTLVertexFormatUShort2Normalized: return VK_FORMAT_R16G16_UNORM; case MTLVertexFormatUShort3Normalized: return VK_FORMAT_R16G16B16_UNORM; case MTLVertexFormatUShort4Normalized: return VK_FORMAT_R16G16B16A16_UNORM;
    case MTLVertexFormatShortNormalized: return VK_FORMAT_R16_SNORM; case MTLVertexFormatShort2Normalized: return VK_FORMAT_R16G16_SNORM; case MTLVertexFormatShort3Normalized: return VK_FORMAT_R16G16B16_SNORM; case MTLVertexFormatShort4Normalized: return VK_FORMAT_R16G16B16A16_SNORM;
    case MTLVertexFormatHalf: return VK_FORMAT_R16_SFLOAT; case MTLVertexFormatHalf2: return VK_FORMAT_R16G16_SFLOAT; case MTLVertexFormatHalf3: return VK_FORMAT_R16G16B16_SFLOAT; case MTLVertexFormatHalf4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MTLVertexFormatFloat: return VK_FORMAT_R32_SFLOAT; case MTLVertexFormatFloat2: return VK_FORMAT_R32G32_SFLOAT; case MTLVertexFormatFloat3: return VK_FORMAT_R32G32B32_SFLOAT; case MTLVertexFormatFloat4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MTLVertexFormatInt: return VK_FORMAT_R32_SINT; case MTLVertexFormatInt2: return VK_FORMAT_R32G32_SINT; case MTLVertexFormatInt3: return VK_FORMAT_R32G32B32_SINT; case MTLVertexFormatInt4: return VK_FORMAT_R32G32B32A32_SINT;
    case MTLVertexFormatUInt: return VK_FORMAT_R32_UINT; case MTLVertexFormatUInt2: return VK_FORMAT_R32G32_UINT; case MTLVertexFormatUInt3: return VK_FORMAT_R32G32B32_UINT; case MTLVertexFormatUInt4: return VK_FORMAT_R32G32B32A32_UINT;
    case MTLVertexFormatInt1010102Normalized: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case MTLVertexFormatUInt1010102Normalized: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static VkBlendFactor n48_bf(MTLBlendFactor f, BOOL *bad) {
    switch (f) {
    case MTLBlendFactorZero: return VK_BLEND_FACTOR_ZERO; case MTLBlendFactorOne: return VK_BLEND_FACTOR_ONE;
    case MTLBlendFactorSourceColor: return VK_BLEND_FACTOR_SRC_COLOR; case MTLBlendFactorOneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case MTLBlendFactorSourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA; case MTLBlendFactorOneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case MTLBlendFactorDestinationColor: return VK_BLEND_FACTOR_DST_COLOR; case MTLBlendFactorOneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case MTLBlendFactorDestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA; case MTLBlendFactorOneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case MTLBlendFactorSourceAlphaSaturated: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case MTLBlendFactorBlendColor: return VK_BLEND_FACTOR_CONSTANT_COLOR; case MTLBlendFactorOneMinusBlendColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case MTLBlendFactorBlendAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case MTLBlendFactorOneMinusBlendAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: *bad = YES; return VK_BLEND_FACTOR_ZERO;   // Source1* (dual-source blending): dualSrcBlend not enabled
    }
}
static VkBlendOp n48_bo(MTLBlendOperation o, BOOL *bad) {
    switch (o) { case MTLBlendOperationAdd: return VK_BLEND_OP_ADD; case MTLBlendOperationSubtract: return VK_BLEND_OP_SUBTRACT;
        case MTLBlendOperationReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT; case MTLBlendOperationMin: return VK_BLEND_OP_MIN;
        case MTLBlendOperationMax: return VK_BLEND_OP_MAX; default: *bad = YES; return VK_BLEND_OP_ADD; }
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline HOT-SWAP (native #12): a FALLBACK pipeline object (spvcache miss in WindowServer; render = magenta, compute = no-op placeholder) registers
// its spvcache keys here. One watcher per process (a 0.5 s dispatch timer on a private serial queue that exists only while an entry is pending)
// stats <sha>.spv / <sha>.meta.json in the lookup directories; when the state machine (n48_hotswap.h) says the files are complete and stable it
// calls the object's builder on the watcher queue (NEVER the render thread), which builds a NEW pipeline object through the normal init path and
// publishes it with one atomic store into the fallback object (n48hs_install). Encoders resolve the pointer at the start of every draw / dispatch.
// ---------------------------------------------------------------------------------------------------------------
@interface N48HSEntry : NSObject { @public __weak id obj; n48hs_entry st; NSArray<NSString *> *shas; NSString *name; BOOL (^build)(id); } @end
@implementation N48HSEntry @end
static NSMutableArray<N48HSEntry *> *n48hs_reg; static NSLock *n48hs_lock; static dispatch_queue_t n48hs_q; static dispatch_source_t n48hs_timer;

static NSString *n48_fn_sha(id fn) {   // sha256(bitcodeData) of a function, nil when it has none
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    return bc.length ? n48_sha256hex(bc) : nil;
}
static int64_t n48hs_wall_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
// First lookup directory holding a usable file (same order and sanity as the creation-time lookup).
static n48hs_obs n48hs_stat(NSString *sha, const char *ext, BOOL spv) {
    n48hs_obs o = {0};
    for (NSString *dir in n48_spv_dirs()) {
        struct stat sb; NSString *path = [dir stringByAppendingPathComponent:[sha stringByAppendingString:@(ext)]];
        if (stat(path.fileSystemRepresentation, &sb) || !S_ISREG(sb.st_mode)) continue;
        n48hs_obs c = { 1, (uint64_t)sb.st_size, (int64_t)sb.st_mtimespec.tv_sec * 1000000000LL + sb.st_mtimespec.tv_nsec };
        if (spv && !n48hs_spv_ok(&c)) continue;
        return c;
    }
    return o;
}
static void n48hs_drop(N48HSEntry *e) {
    [n48hs_lock lock]; [n48hs_reg removeObjectIdenticalTo:e];
    if (!n48hs_reg.count && n48hs_timer) { dispatch_source_cancel(n48hs_timer); n48hs_timer = nil; }
    [n48hs_lock unlock];
}
static void n48hs_tick_all(void) {
    @autoreleasepool {
        [n48hs_lock lock]; NSArray<N48HSEntry *> *snap = [n48hs_reg copy]; [n48hs_lock unlock];
        for (N48HSEntry *e in snap) {
            id o = e->obj;
            if (!o || e->st.st == N48HS_SWAPPED || e->st.st == N48HS_GAVEUP) { n48hs_drop(e); continue; }
            n48hs_key_obs cur[N48HS_MAXKEY] = {{{0},{0}}};
            for (int k = 0; k < e->st.nkeys; k++) { cur[k].spv = n48hs_stat(e->shas[(NSUInteger)k], ".spv", YES); cur[k].meta = n48hs_stat(e->shas[(NSUInteger)k], ".meta.json", NO); }
            if (n48hs_tick(&e->st, n48hs_wall_ns(), cur) != N48HS_BUILD) continue;
            N48LOG("HOT-SWAP %s: spvcache complete and stable (attempt %d), building the real pipeline off the render thread", e->name.UTF8String, e->st.attempts);
            uint64_t t0 = n48_now(); BOOL ok = e->build(o); double ms = (double)(n48_now() - t0) / 1e6;
            if (n48hs_build_done(&e->st, ok, n48hs_wall_ns())) { N48LOG("HOT-SWAP %s: fallback -> real (built in %.3f ms)", e->name.UTF8String, ms); n48hs_drop(e); }
            else if (e->st.st == N48HS_GAVEUP) { N48LOG("HOT-SWAP %s: GAVE UP after %d failed builds (the fallback stays)", e->name.UTF8String, e->st.attempts); n48hs_drop(e); }
            else N48LOG("HOT-SWAP %s: build failed (attempt %d), will retry", e->name.UTF8String, e->st.attempts);
        }
    }
}
// shas: 2 (vertex, fragment) or 1 (kernel). obj is held weakly; a deallocated object drops out at the next tick.
static void n48hs_register(id obj, NSArray<NSString *> *shas, BOOL metaRequired, NSString *name, BOOL (^build)(id)) {
    if (n48_force_fallback() || !shas.count || shas.count > N48HS_MAXKEY) return;   // forced misses (r1 test) must stay fallbacks
    static dispatch_once_t once;
    dispatch_once(&once, ^{ n48hs_lock = [NSLock new]; n48hs_reg = [NSMutableArray array]; n48hs_q = dispatch_queue_create("navi48.hotswap", DISPATCH_QUEUE_SERIAL); });
    N48HSEntry *e = [N48HSEntry new]; e->obj = obj; e->shas = [shas copy]; e->name = [name copy]; e->build = [build copy];
    n48hs_init(&e->st, (int)shas.count, metaRequired ? 1 : 0);
    [n48hs_lock lock];
    [n48hs_reg addObject:e];
    if (!n48hs_timer) {
        n48hs_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, n48hs_q);
        dispatch_source_set_timer(n48hs_timer, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), 500 * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(n48hs_timer, ^{ n48hs_tick_all(); });
        dispatch_resume(n48hs_timer);
    }
    [n48hs_lock unlock];
    N48LOG("HOT-SWAP %s: registered (%lu key(s)%s)", name.UTF8String, (unsigned long)shas.count, metaRequired ? ", meta required" : "");
}

// ---------------------------------------------------------------------------------------------------------------
// N48RenderPipelineState (10d/11e): SPIR-V from Resources/spvcache/<sha256(bitcodeData)>.spv -> VkPipeline variants.
// ---------------------------------------------------------------------------------------------------------------
@interface N48RenderPipelineState : _MTLRenderPipelineState {
    id _dev; NSString *_lbl; VkShaderModule _vs, _fs; VkPipelineLayout _pl; VkRenderPass _rp; VkDescriptorSetLayout _dsl[2];
    N48PB *_pbV, *_pbF; uint32_t _npbV, _npbF; BOOL _fetch; uint32_t _fetchMax;   // 11e-2: fragment reads [[color(n)]] (input attachments 0.._fetchMax)
    VkVertexInputBindingDescription _vib[32]; VkVertexInputAttributeDescription _via[32]; uint32_t _nvib, _nvia; uint32_t _need[32]; uint32_t _nneed;
    VkPipelineColorBlendAttachmentState _cba[8]; uint32_t _na; char _vep[128], _fep[128];
    NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _fallback;
    char _vnm[64], _fnm[64];   // native #12 P1: vertex / fragment function names (CoreDisplay's final pass = fragment "GPUPass")
    MTLRenderPipelineDescriptor *_hsDesc; _Atomic(uintptr_t) _realp;   // hot-swap: creation descriptor (fallback only); the published real object (+1, written once)
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err;
- (N48RenderPipelineState *)n48Real;   // the hot-swapped real pipeline, or nil
- (BOOL)n48HotRebuild;
- (VkPipeline)vkPipeline;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f;
- (VkPipelineLayout)layout;
- (VkDescriptorSetLayout)dsl:(int)i;
- (const N48PB *)pbV; - (uint32_t)npbV; - (const N48PB *)pbF; - (uint32_t)npbF;
- (const uint32_t *)needBindings; - (uint32_t)nNeed;
- (BOOL)fetch; - (uint32_t)fetchMax;
- (const char *)n48VName; - (const char *)n48FName;
@end
static VkRenderPass n48_mk_rp(const VkAttachmentDescription *ads, uint32_t na, BOOL fetch);

// Returns the cached SPIR-V for fn, or nil with *err code 40 (no bitcodeData) / 41 (miss, or N48M_FORCE_FALLBACK).
// *fname gets the function name for logs; *meta the <sha>.meta.json sidecar when present. Reflection reads the entry-point name out of the SPIR-V itself.
static NSData *n48_spv_lookup_impl(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    if (fname) *fname = (fn && [fn respondsToSelector:@selector(name)]) ? [fn name] : @"?";
    if (!bc.length) { if (err) *err = n48_err(40, [NSString stringWithFormat:@"%s function has no bitcodeData", role]); return nil; }
    if (n48_force_fallback()) { if (err) *err = n48_err(41, [NSString stringWithFormat:@"N48M_FORCE_FALLBACK: %s treated as a spvcache miss", role]); return nil; }
    NSString *sha = n48_sha256hex(bc);
    NSString *path = nil; NSData *spv = nil;
    for (NSString *dir in n48_spv_dirs()) {   // H2: bundle spvcache first, then the side directory
        path = [dir stringByAppendingPathComponent:[sha stringByAppendingString:@".spv"]];
        spv = [NSData dataWithContentsOfFile:path];
        if (spv.length >= 20 && !(spv.length & 3)) break;
        spv = nil;
    }
    if (!spv) { if (err) *err = n48_err(41, [NSString stringWithFormat:@"spvcache miss for %s: %@", role, [[n48_spv_dirs().firstObject stringByAppendingPathComponent:sha] stringByAppendingString:@".spv"]]); return nil; }
    if (meta) *meta = n48_meta_for(sha);
    N48LOG("spvcache hit %s: %s (%lu bytes%s)", role, path.UTF8String, (unsigned long)spv.length, (meta && *meta) ? ", meta" : ", no meta");
    return spv;
}

static NSData *n48_spv_lookup(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {
    uint64_t t_ = n48_now(); NSData *d = n48_spv_lookup_impl(fn, role, err, fname, meta); n48t_add(&T1.spv, n48_now() - t_); return d;
}

static VkShaderModule n48_spv_module(const void *code, size_t bytes, const char *role, NSError **err) {
    VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; uint64_t t_ = n48_now(); VkResult r = vkCreateShaderModule(N48R.dev, &smc, NULL, &m); n48t_add(&T1.shm, n48_now() - t_);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(42, [NSString stringWithFormat:@"vkCreateShaderModule(%s) = %d", role, r]); return VK_NULL_HANDLE; }
    return m;
}

@implementation N48RenderPipelineState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err { return [self initWithDevice:dev descriptor:d noFallback:NO error:err]; }
- (const char *)n48VName { return _vnm; }
- (const char *)n48FName { return _fnm; }
- (N48RenderPipelineState *)n48Real { uintptr_t p = n48hs_load(&_realp); return p ? (__bridge N48RenderPipelineState *)(void *)p : nil; }
// Runs on the watcher queue. Builds a complete NEW pipeline object from the recorded descriptor (no fallback allowed, no AIR dump) and publishes it.
- (BOOL)n48HotRebuild {
    if (!_hsDesc) return NO;
    NSError *e = nil;
    N48RenderPipelineState *r = [[N48RenderPipelineState alloc] initWithDevice:_dev descriptor:_hsDesc noFallback:YES error:&e];
    if (!r) { N48LOG("HOT-SWAP rebuild failed: %s", e.localizedDescription.UTF8String); return NO; }
    r->_lbl = _lbl;
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!n48hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }   // lost a race (cannot happen: one watcher): keep the first
    return YES;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    if (!n48_radv_open(err)) return nil;
    // 11e: SPIR-V from the cache; on a miss in WindowServer (or N48M_FORCE_FALLBACK with N48M_ALLOW) build BOTH stages
    // from the built-in magenta shaders instead of failing (the AIR is dumped by the caller-side n48_dump_pipeline).
    NSString *vname = nil, *fname = nil; NSError *e1 = nil, *e2 = nil; NSDictionary *mv_ = nil, *mf_ = nil;
    NSData *vsd = n48_spv_lookup(d.vertexFunction, "vertex", &e1, &vname, &mv_);
    NSData *fsd = n48_spv_lookup(d.fragmentFunction, "fragment", &e2, &fname, &mf_);
    strlcpy(_vnm, vname.UTF8String ? vname.UTF8String : "?", sizeof _vnm); strlcpy(_fnm, fname.UTF8String ? fname.UTF8String : "?", sizeof _fnm);
    BOOL fb = NO;
    if (!vsd || !fsd) {
        NSError *e = e1 ? e1 : e2;
        if (e.code == 41 && n48_fallback_ok() && !nofb) {
            fb = YES; _fallback = YES;
            N48LOG("FALLBACK %s / %s (%s)", vname.UTF8String, fname.UTF8String, e.localizedDescription.UTF8String);
            (void)n48_dump_pipeline(d);
        } else { if (err) *err = e; return nil; }
    }
    N48sRefl *rv = NULL, *rf = NULL; N48sMod mv = {0}, mf = {0}; char vep[128] = "main", fep[128] = "main";
    NSData *fsp = nil;
    if (!fb) {
        char why[256] = "";
        fsp = n48_spv_set(fsd, 1);   // fragment descriptors live in set 1 (binding-model comment above)
        rv = calloc(1, sizeof *rv); rf = calloc(1, sizeof *rf);
        BOOL bad = NO;
        if (n48s_parse(vsd.bytes, vsd.length / 4, &mv, why, sizeof why) || !mv.has_ep || mv.ep_model != 0 || n48s_reflect(&mv, 0, rv)) {
            if (err) *err = n48_err(51, [NSString stringWithFormat:@"vertex SPIR-V reflection: %s", why[0] ? why : rv->why[0] ? rv->why : "no/wrong entry point"]); bad = YES; }
        else if (n48s_parse(fsp.bytes, fsp.length / 4, &mf, why, sizeof why) || !mf.has_ep || mf.ep_model != 4 || n48s_reflect(&mf, 1, rf)) {
            if (err) *err = n48_err(52, [NSString stringWithFormat:@"fragment SPIR-V reflection: %s", why[0] ? why : rf->why[0] ? rf->why : "no/wrong entry point"]); bad = YES; }
        if (!bad) { strlcpy(vep, mv.ep_name, sizeof vep); strlcpy(fep, mf.ep_name, sizeof fep); }
        n48s_free(&mv); n48s_free(&mf);
        if (bad) { free(rv); free(rf); return nil; }
    }
    strlcpy(_vep, vep, sizeof _vep); strlcpy(_fep, fep, sizeof _fep);
    BOOL ok = NO;
    do {
        _vs = fb ? n48_spv_module(n48_fb_vs_spv, sizeof n48_fb_vs_spv, "vertex", err) : n48_spv_module(vsd.bytes, vsd.length, "vertex", err); if (!_vs) break;
        _fs = fb ? n48_spv_module(n48_fb_fs_spv, sizeof n48_fb_fs_spv, "fragment", err) : n48_spv_module(fsp.bytes, fsp.length, "fragment", err); if (!_fs) break;
        ok = YES;
    } while (0);
    if (!ok) { free(rv); free(rf); return nil; }
    VkAttachmentDescription ads[8]; VkAttachmentReference refs[8]; uint32_t na = 0;
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        if (ca.pixelFormat == MTLPixelFormatInvalid) continue;
        const N48Fmt *f = n48_fmt(ca.pixelFormat);
        if (!f) { N48LOG("pipeline: colour attachment %lu pixel format %lu has no Vulkan mapping in the format table", (unsigned long)i, (unsigned long)ca.pixelFormat);
                  if (err) *err = n48_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]); free(rv); free(rf); return nil; }
        if (f->flags & N48F_A8) { N48LOG("pipeline: colour attachment %lu is A8Unorm: not supported (the fragment's alpha cannot be written into an R8 attachment)", (unsigned long)i);
                  if (err) *err = n48_err(43, @"unsupported colour attachment format 1 (A8Unorm render targets are not supported)"); free(rv); free(rf); return nil; }
        if (!n48_radv_open(err)) { free(rv); free(rf); return nil; }
        { VkFormatFeatureFlags ff = n48_fmt_feats(f), nd = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | (ca.isBlendingEnabled ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0);
          if ((ff & nd) != nd) { N48LOG("pipeline: colour attachment %lu pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)i, (unsigned long)ca.pixelFormat, f->vk, ff, nd & ~ff);
              if (err) *err = n48_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu (RADV lacks attachment%s support)", (unsigned long)ca.pixelFormat, ca.isBlendingEnabled ? "/blend" : ""]); free(rv); free(rf); return nil; } }
        N48LOG("pipeline: colour attachment %lu pixel format %lu (vk %d)", (unsigned long)i, (unsigned long)ca.pixelFormat, f->vk);
        ads[na] = (VkAttachmentDescription){ .format = f->vk, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        refs[na] = (VkAttachmentReference){ na, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkColorComponentFlags wm = 0; MTLColorWriteMask m = ca.writeMask;
        if (m & MTLColorWriteMaskRed) wm |= VK_COLOR_COMPONENT_R_BIT; if (m & MTLColorWriteMaskGreen) wm |= VK_COLOR_COMPONENT_G_BIT;
        if (m & MTLColorWriteMaskBlue) wm |= VK_COLOR_COMPONENT_B_BIT; if (m & MTLColorWriteMaskAlpha) wm |= VK_COLOR_COMPONENT_A_BIT;
        if (rf && !(rf->outmask & (1u << na))) { wm = 0; N48LOG("pipeline: fragment stage does not write output %u: attachment write mask forced to 0 (contents kept)", na); }
        _cba[na] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = wm };
        if (ca.isBlendingEnabled) {   // 11e: blending from the descriptor (enable, factors, ops, write mask)
            BOOL bad = NO;
            _cba[na].blendEnable = VK_TRUE;
            _cba[na].srcColorBlendFactor = n48_bf(ca.sourceRGBBlendFactor, &bad); _cba[na].dstColorBlendFactor = n48_bf(ca.destinationRGBBlendFactor, &bad);
            _cba[na].srcAlphaBlendFactor = n48_bf(ca.sourceAlphaBlendFactor, &bad); _cba[na].dstAlphaBlendFactor = n48_bf(ca.destinationAlphaBlendFactor, &bad);
            _cba[na].colorBlendOp = n48_bo(ca.rgbBlendOperation, &bad); _cba[na].alphaBlendOp = n48_bo(ca.alphaBlendOperation, &bad);
            if (bad) { if (err) *err = n48_err(56, @"blend factor/operation not supported (dual-source Source1* factors)"); free(rv); free(rf); return nil; }
            N48LOG("pipeline: attachment %lu blending rgb %lu/%lu/%lu alpha %lu/%lu/%lu writeMask 0x%lx", (unsigned long)i, (unsigned long)ca.rgbBlendOperation,
                   (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor, (unsigned long)ca.alphaBlendOperation,
                   (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor, (unsigned long)m);
        }
        na++;
    }
    _na = na;
    if (d.depthAttachmentPixelFormat != MTLPixelFormatInvalid || d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid) {
        if (err) *err = n48_err(44, @"depth/stencil attachments not implemented in 10d"); free(rv); free(rf); return nil; }
    // 11e-2: a fragment stage with input attachments (ColorInput, binding 192+n, InputAttachmentIndex n) gets the "fetch" render-pass shape:
    // every colour attachment is also an input attachment (same index), layouts GENERAL; the encoder switches to a compatible pass.
    for (int b = 192; rf && b < 200; b++) if (rf->bind[1][b].used && rf->bind[1][b].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) { _fetch = YES; _fetchMax = (uint32_t)(b - 192); }
    if (_fetch && _fetchMax >= na) { if (err) *err = n48_err(59, [NSString stringWithFormat:@"fragment reads [[color(%u)]] but the pipeline has %u colour attachment(s)", _fetchMax, na]); free(rv); free(rf); return nil; }
    VkResult r;
    if (_fetch) {
        for (uint32_t i = 0; i < na; i++) ads[i].initialLayout = ads[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        _rp = n48_mk_rp(ads, na, YES);
        if (!_rp) { if (err) *err = n48_err(45, @"vkCreateRenderPass (framebuffer fetch)"); free(rv); free(rf); return nil; }
    } else {
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = na, .pColorAttachments = refs };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = ads, .subpassCount = 1, .pSubpasses = &sp };
    r = vkCreateRenderPass(N48R.dev, &rpc, NULL, &_rp);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(45, [NSString stringWithFormat:@"vkCreateRenderPass = %d", r]); free(rv); free(rf); return nil; }
    }
    // Layouts: vertex = set 0, fragment = set 1 (both always exist, possibly empty) + push constants when a stage declares a block.
    NSError *le = nil;
    if (!n48_stage_dsl(rv, 0, VK_SHADER_STAGE_VERTEX_BIT, mv_, &_dsl[0], &_pbV, &_npbV, _owned, &le) ||
        !n48_stage_dsl(rf, 1, VK_SHADER_STAGE_FRAGMENT_BIT, mf_, &_dsl[1], &_pbF, &_npbF, _owned, &le)) {
        if (err) *err = le; free(rv); free(rf); return nil; }
    uint32_t pcs = 0; VkShaderStageFlags pcf = 0;
    if (rv && rv->pc_size) { pcs = rv->pc_size; pcf |= VK_SHADER_STAGE_VERTEX_BIT; }
    if (rf && rf->pc_size) { if (rf->pc_size > pcs) pcs = rf->pc_size; pcf |= VK_SHADER_STAGE_FRAGMENT_BIT; }
    if (pcs > 256) { if (err) *err = n48_err(48, [NSString stringWithFormat:@"push constant block %u B > 256", pcs]); free(rv); free(rf); return nil; }
    VkPushConstantRange pcr = { pcf, 0, pcs };
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = _dsl,
        .pushConstantRangeCount = pcs ? 1 : 0, .pPushConstantRanges = &pcr };
    r = vkCreatePipelineLayout(N48R.dev, &plc, NULL, &_pl);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout = %d", r]); free(rv); free(rf); return nil; }
    // Vertex input: for every SPIR-V input Location, the descriptor's attribute of that index (format, offset, bufferIndex) and its
    // buffer layout (stride, step function). A Location without a descriptor attribute reads binding 31 (stride 0, a null buffer).
    MTLVertexDescriptor *vd = d.vertexDescriptor; BOOL dummy = NO;
    for (int i = 0; rv && i < rv->nva; i++) {
        uint32_t loc = rv->va[i].location;
        MTLVertexAttributeDescriptor *a = vd ? vd.attributes[loc] : nil;
        if (a && a.format != MTLVertexFormatInvalid) {
            VkFormat vf = n48_vkvfmt(a.format); NSUInteger bi = a.bufferIndex;
            if (vf == VK_FORMAT_UNDEFINED || bi >= 31 || a.offset > 2047) {
                if (err) *err = n48_err(57, [NSString stringWithFormat:@"vertex attribute %u: format %lu / bufferIndex %lu / offset %lu not supported", loc, (unsigned long)a.format, (unsigned long)bi, (unsigned long)a.offset]);
                free(rv); free(rf); return nil; }
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[bi]; VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX; uint32_t stride = (uint32_t)l.stride;
            if (l.stride == (NSUInteger)-1 || l.stride > 2048 || (l.stepFunction != MTLVertexStepFunctionConstant && l.stepFunction != MTLVertexStepFunctionPerVertex && l.stepFunction != MTLVertexStepFunctionPerInstance) ||
                (l.stepFunction == MTLVertexStepFunctionPerInstance && l.stepRate != 1)) {
                if (err) { *err = n48_err(58, [NSString stringWithFormat:@"vertex buffer layout %lu: stride %lu step function %lu rate %lu not supported (dynamic stride / step rate > 1 / patch)", (unsigned long)bi,
                                             (unsigned long)l.stride, (unsigned long)l.stepFunction, (unsigned long)l.stepRate]); }
                free(rv); free(rf); return nil; }
            if (l.stepFunction == MTLVertexStepFunctionConstant) stride = 0;
            else if (l.stepFunction == MTLVertexStepFunctionPerInstance) rate = VK_VERTEX_INPUT_RATE_INSTANCE;
            uint32_t k = 0; for (; k < _nvib; k++) if (_vib[k].binding == (uint32_t)bi) break;
            if (k == _nvib) { _vib[_nvib++] = (VkVertexInputBindingDescription){ (uint32_t)bi, stride, rate }; _need[_nneed++] = (uint32_t)bi; }
            _via[_nvia++] = (VkVertexInputAttributeDescription){ loc, (uint32_t)bi, vf, (uint32_t)a.offset };
        } else {
            N48LOG("pipeline: WARN shader input location %u has no vertex-descriptor attribute: reads a zero-stride null binding", loc);
            if (!dummy) { _vib[_nvib++] = (VkVertexInputBindingDescription){ 31, 0, VK_VERTEX_INPUT_RATE_VERTEX }; _need[_nneed++] = 31; dummy = YES; }
            _via[_nvia++] = (VkVertexInputAttributeDescription){ loc, 31, rv->va[i].format, 0 };
        }
    }
    free(rv); free(rf);
    NSError *ve = nil; VkPipeline p0 = [self n48Build:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE error:&ve];
    if (!p0) { if (err) *err = ve; return nil; }
    _variants[@((uint32_t)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST | ((uint32_t)VK_CULL_MODE_NONE << 8) | ((uint32_t)VK_FRONT_FACE_CLOCKWISE << 16))] = [NSValue valueWithPointer:(void *)p0];
    N48LOG("N48RenderPipelineState %p: pipeline %p (%u colour attachment(s), set0 %u / set1 %u bindings, %u vertex input(s), %u vertex binding(s)%s%s)", (__bridge void *)self, (void *)p0, na,
           _npbV, _npbF, _nvia, _nvib, fb ? ", FALLBACK magenta" : "", _fetch ? ", FRAMEBUFFER FETCH (input attachments)" : "");
    if (fb) {   // hot-swap: remember how to rebuild (descriptor copy: the client may reuse its own) and watch for the .spv files
        NSString *sv = n48_fn_sha(d.vertexFunction), *sf = n48_fn_sha(d.fragmentFunction);
        if (sv && sf) {
            _hsDesc = [d copy];
            n48hs_register(self, @[ sv, sf ], NO, [NSString stringWithFormat:@"%@ / %@", vname, fname], ^BOOL(id o) { return [(N48RenderPipelineState *)o n48HotRebuild]; });
        }
    }
    return self;
}
- (VkPipeline)n48Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front error:(NSError **)err {
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = _vs, .pName = _vep },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = _fs, .pName = _fep } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = _nvib, .pVertexBindingDescriptions = _vib, .vertexAttributeDescriptionCount = _nvia, .pVertexAttributeDescriptions = _via };
    BOOL strip = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;   // Metal restarts indexed strips at the max index value
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo, .primitiveRestartEnable = strip };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cull, .frontFace = front, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = _na, .pAttachments = _cba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpc = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = _pl, .renderPass = _rp };
    VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = n48_now(); VkResult r = vkCreateGraphicsPipelines(N48R.dev, N48R.pc, 1, &gpc, NULL, &p); n48t_add(&T1.gfx, n48_now() - t_); atomic_fetch_add(&n48_pipes_new, 1);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(47, [NSString stringWithFormat:@"vkCreateGraphicsPipelines = %d", r]); return VK_NULL_HANDLE; }
    return p;
}
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front {
    NSNumber *k = @((uint32_t)topo | ((uint32_t)cull << 8) | ((uint32_t)front << 16));
    [_lock lock];
    NSValue *v = _variants[k];
    if (!v) {
        NSError *e = nil; VkPipeline p = [self n48Build:topo cull:cull front:front error:&e];
        if (!p) N48LOG("pipeline variant (topology %d cull %d front %d) failed: %s", topo, cull, front, e.localizedDescription.UTF8String);
        else { v = [NSValue valueWithPointer:(void *)p]; _variants[k] = v; N48LOG("pipeline variant topology %d cull %d front %d created", topo, cull, front); }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    if (N48R.ok) {
        for (NSValue *v in _variants.allValues) vkDestroyPipeline(N48R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_pl) vkDestroyPipelineLayout(N48R.dev, _pl, NULL);
        for (int i = 0; i < 2; i++) if (_dsl[i]) vkDestroyDescriptorSetLayout(N48R.dev, _dsl[i], NULL);
        for (NSValue *v in _owned) vkDestroySampler(N48R.dev, (VkSampler)v.pointerValue, NULL);
        if (_rp) vkDestroyRenderPass(N48R.dev, _rp, NULL);
        if (_vs) vkDestroyShaderModule(N48R.dev, _vs, NULL);
        if (_fs) vkDestroyShaderModule(N48R.dev, _fs, NULL);
    }
    free(_pbV); free(_pbF);
    uintptr_t rp = n48hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);   // the published real object
}
N48_DNR(N48RenderPipelineState)
- (VkPipeline)vkPipeline { return [self pipelineForTopology:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE]; }
- (VkPipelineLayout)layout { return _pl; }
- (VkDescriptorSetLayout)dsl:(int)i { return _dsl[i]; }
- (BOOL)fetch { return _fetch; } - (uint32_t)fetchMax { return _fetchMax; }
- (const N48PB *)pbV { return _pbV; } - (uint32_t)npbV { return _npbV; } - (const N48PB *)pbF { return _pbF; } - (uint32_t)npbF { return _npbF; }
- (const uint32_t *)needBindings { return _need; } - (uint32_t)nNeed { return _nneed; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (NSUInteger)threadgroupSizeMatchesTileSize { return 0; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
@end

// ---------------------------------------------------------------------------------------------------------------
// Bound-resource state shared by the render and compute encoders, descriptor-set filling, dummy resources (11e).
// ---------------------------------------------------------------------------------------------------------------
typedef struct { __unsafe_unretained N48Buffer *b; NSUInteger off; } N48BufSlot;
typedef struct {
    N48BufSlot buf[31]; __unsafe_unretained N48Texture *tex[128]; __unsafe_unretained N48SamplerState *smp[16]; __unsafe_unretained N48Texture *att[8];   // att: the colour attachments (input attachments, 11e-2)
} N48StageState;

// Fallbacks for slots a shader reads but the client never bound. Metal reads zeros / a black texel there; a garbage descriptor could
// fault the GPU, so a zero-filled buffer, a 1x1 texel and a default sampler stand in (each use is logged as a WARN).
static N48Buffer *n48_dummy_buffer(id dev) {
    static N48Buffer *b;
    @synchronized ([Navi48Device class]) {
        if (!b) { NSError *e = nil; b = [[N48Buffer alloc] initWithDevice:dev length:65536 options:MTLResourceStorageModeShared error:&e];
                  if (b) memset([b contents], 0, 65536); else N48LOG("dummy buffer failed: %s", e.localizedDescription.UTF8String); }
    }
    return b;
}
static N48Texture *n48_dummy_texture(id dev) {
    static N48Texture *t;
    @synchronized ([Navi48Device class]) {
        if (!t) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
            NSError *e = nil; t = [[N48Texture alloc] initWithDevice:dev descriptor:td error:&e];
            if (t) { uint32_t z = 0; [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4]; }
            else N48LOG("dummy texture failed: %s", e.localizedDescription.UTF8String);
        }
    }
    return t;
}
static N48SamplerState *n48_dummy_sampler(id dev) {
    static N48SamplerState *s;
    @synchronized ([Navi48Device class]) {
        if (!s) { NSError *e = nil; s = [[N48SamplerState alloc] initWithDevice:dev descriptor:[MTLSamplerDescriptor new] error:&e];
                  if (!s) N48LOG("dummy sampler failed: %s", e.localizedDescription.UTF8String); }
    }
    return s;
}

// Visits every image a stage's bindings will read or write, with the layout the descriptor needs (SHADER_READ_ONLY for sampled images,
// GENERAL for storage images), so the encoder can transition them BEFORE the draw/dispatch (never inside a render pass).
static void n48_each_tex(const N48PB *pb, uint32_t n, const N48StageState *st, id dev, void (^fn)(N48Texture *t, VkImageLayout l)) {
    for (uint32_t i = 0; i < n; i++) {
        if (pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil;
                if (!t) t = n48_dummy_texture(dev); if (t) fn(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
        } else if (pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 480 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil; if (t) fn(t, VK_IMAGE_LAYOUT_GENERAL); }
        }
    }
}

typedef struct { uint32_t local[3], groups[3], tbase[3], gbase[3]; } N48Region;
// KernelDispatchPlan::new (metal2vulkan src/reflect/mod.rs): a dispatchThreads grid becomes <= 8 rectangular regions (full groups + boundary tails).
static int n48_plan(const uint32_t threads[3], const uint32_t nom[3], N48Region *out) {
    int n = 0;
    for (int mask = 0; mask < 8; mask++) {
        N48Region r = { { nom[0], nom[1], nom[2] }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } }; BOOL nonempty = YES;
        for (int d = 0; d < 3; d++) {
            uint32_t full = threads[d] / nom[d], tail = threads[d] % nom[d];
            if (!(mask & (1 << d))) r.groups[d] = full;
            else if (tail == 0) nonempty = NO;
            else { r.local[d] = tail; r.groups[d] = 1; r.tbase[d] = full * nom[d]; r.gbase[d] = full; }
            nonempty = nonempty && r.groups[d] != 0;
        }
        if (nonempty) out[n++] = r;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// N48ComputePipelineState (11e R2): SPIR-V + meta sidecar from spvcache; ThreadsDynamic modules take the local size as spec ids 0..2.
// ---------------------------------------------------------------------------------------------------------------
@interface N48ComputePipelineState : NSObject {
    id _dev; NSString *_lbl; VkShaderModule _sm; VkPipelineLayout _pl; VkDescriptorSetLayout _dsl; N48PB *_pb; uint32_t _npb; uint32_t _pcSize;
    uint32_t _local[3]; int _mode; uint32_t _pcOff; char _ep[128]; NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _noop;
    id _hsFn; _Atomic(uintptr_t) _realp;   // hot-swap (placeholder only): the kernel function to rebuild from; the published real object (+1, written once)
}
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err;
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn;   // #12 R1: valid state whose dispatches encode nothing (hot-swapped when the .spv + .meta.json appear)
- (N48ComputePipelineState *)n48Real;
- (BOOL)n48HotRebuild;
- (BOOL)noop;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l;
- (VkPipelineLayout)layout; - (VkDescriptorSetLayout)dsl; - (const N48PB *)pb; - (uint32_t)npb;
- (const uint32_t *)nominalLocal; - (int)mode; - (uint32_t)pcOffset; - (uint32_t)pcSize;
@end

@implementation N48ComputePipelineState
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array]; _noop = YES;
    NSString *sha = n48_fn_sha(fn);
    if (sha) {
        _hsFn = fn;
        n48hs_register(self, @[ sha ], YES, [NSString stringWithFormat:@"kernel %@", [fn respondsToSelector:@selector(name)] ? [fn name] : @"?"], ^BOOL(id o) { return [(N48ComputePipelineState *)o n48HotRebuild]; });
    }
    return self;
}
- (N48ComputePipelineState *)n48Real { uintptr_t p = n48hs_load(&_realp); return p ? (__bridge N48ComputePipelineState *)(void *)p : nil; }
// Runs on the watcher queue: a NEW pipeline object through the normal init (a miss or an unusable .meta.json fails it, nothing is published).
- (BOOL)n48HotRebuild {
    if (!_hsFn) return NO;
    NSError *e = nil;
    N48ComputePipelineState *r = [[N48ComputePipelineState alloc] initWithDevice:_dev function:_hsFn error:&e];
    if (!r) { N48LOG("HOT-SWAP kernel rebuild failed: %s", e.localizedDescription.UTF8String); return NO; }
    r->_lbl = _lbl;
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!n48hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }
    return YES;
}
- (BOOL)noop { return _noop; }
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    // #12 R1: the cache lookup comes first, so a miss (placeholder case) needs no Vulkan device.
    NSString *fname = nil; NSDictionary *meta = nil; NSError *e = nil;
    NSData *spv = n48_spv_lookup(fn, "kernel", &e, &fname, &meta);
    if (!spv) { if (err) *err = e; return nil; }
    if (!n48_radv_open(err)) return nil;
    if (!meta || !meta[@"kernel_dispatch"] || ![meta[@"local_size"] isKindOfClass:[NSArray class]] || [meta[@"local_size"] count] != 3) {
        if (err) *err = n48_err(62, [NSString stringWithFormat:@"compute function '%@': spvcache has no .meta.json with kernel_dispatch/local_size (run add-air.py)", fname]); return nil; }
    id kd = meta[@"kernel_dispatch"];
    if ([kd isEqual:@"Workgroups"]) _mode = 0;
    else if ([kd isKindOfClass:[NSDictionary class]] && kd[@"ThreadsDynamic"]) { _mode = 1; _pcOff = (uint32_t)[kd[@"ThreadsDynamic"][@"offset"] unsignedIntValue]; }
    else { if (err) *err = n48_err(63, [NSString stringWithFormat:@"compute function '%@': kernel_dispatch %@ not supported (ThreadsFixed)", fname, kd]); return nil; }
    for (int i = 0; i < 3; i++) _local[i] = (uint32_t)[meta[@"local_size"][i] unsignedIntValue];
    N48sRefl *r = calloc(1, sizeof *r); N48sMod m = {0}; char why[256] = "";
    if (n48s_parse(spv.bytes, spv.length / 4, &m, why, sizeof why) || !m.has_ep || m.ep_model != 5 || n48s_reflect(&m, 2, r)) {
        if (err) *err = n48_err(64, [NSString stringWithFormat:@"kernel SPIR-V reflection: %s", why[0] ? why : r->why[0] ? r->why : "no/wrong entry point"]); n48s_free(&m); free(r); return nil; }
    strlcpy(_ep, m.ep_name, sizeof _ep); n48s_free(&m);
    _pcSize = r->pc_size;
    BOOL ok = n48_stage_dsl(r, 0, VK_SHADER_STAGE_COMPUTE_BIT, meta, &_dsl, &_pb, &_npb, _owned, &e);
    free(r);
    if (!ok) { if (err) *err = e; return nil; }
    _sm = n48_spv_module(spv.bytes, spv.length, "kernel", err); if (!_sm) return nil;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, _pcSize };
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &_dsl,
        .pushConstantRangeCount = _pcSize ? 1 : 0, .pPushConstantRanges = &pcr };
    VkResult vr = vkCreatePipelineLayout(N48R.dev, &plc, NULL, &_pl);
    if (vr != VK_SUCCESS) { if (err) *err = n48_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(compute) = %d", vr]); return nil; }
    VkPipeline p = [self pipelineForLocal:_local];
    if (!p) { if (err) *err = n48_err(65, @"vkCreateComputePipelines failed"); return nil; }
    N48LOG("N48ComputePipelineState %p '%s': local %u,%u,%u, %s, %u bindings, push constants %u B", (__bridge void *)self, fname.UTF8String, _local[0], _local[1], _local[2],
           _mode ? "ThreadsDynamic" : "Workgroups", _npb, _pcSize);
    return self;
}
- (VkPipeline)pipelineForLocal:(const uint32_t *)l {
    uint64_t key = _mode ? ((uint64_t)l[0] | ((uint64_t)l[1] << 20) | ((uint64_t)l[2] << 40)) : 0;
    NSNumber *k = @(key);
    [_lock lock];
    NSValue *v = _variants[k];
    if (!v) {
        VkSpecializationMapEntry me[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } }; uint32_t data[3] = { l[0], l[1], l[2] };
        VkSpecializationInfo si = { 3, me, sizeof data, data };
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = _pl,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = _sm, .pName = _ep, .pSpecializationInfo = _mode ? &si : NULL } };
        VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = n48_now(); VkResult r = vkCreateComputePipelines(N48R.dev, N48R.pc, 1, &ci, NULL, &p); n48t_add(&T1.cmp, n48_now() - t_); atomic_fetch_add(&n48_pipes_new, 1);
        if (r != VK_SUCCESS) N48LOG("vkCreateComputePipelines(local %u,%u,%u) = %d", l[0], l[1], l[2], r);
        else { v = [NSValue valueWithPointer:(void *)p]; _variants[k] = v; }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    if (N48R.ok) {
        for (NSValue *v in _variants.allValues) vkDestroyPipeline(N48R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_pl) vkDestroyPipelineLayout(N48R.dev, _pl, NULL);
        if (_dsl) vkDestroyDescriptorSetLayout(N48R.dev, _dsl, NULL);
        for (NSValue *v in _owned) vkDestroySampler(N48R.dev, (VkSampler)v.pointerValue, NULL);
        if (_sm) vkDestroyShaderModule(N48R.dev, _sm, NULL);
    }
    free(_pb);
    uintptr_t rp = n48hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);
}
N48_DNR(N48ComputePipelineState)
- (VkPipelineLayout)layout { return _pl; } - (VkDescriptorSetLayout)dsl { return _dsl; } - (const N48PB *)pb { return _pb; } - (uint32_t)npb { return _npb; }
- (const uint32_t *)nominalLocal { return _local; } - (int)mode { return _mode; } - (uint32_t)pcOffset { return _pcOff; } - (uint32_t)pcSize { return _pcSize; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { (void)d; return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
@end

// ---------------------------------------------------------------------------------------------------------------
// N48CommandQueue / N48CommandBuffer (10b). Synchronisation per NATIVE-S4-M10: submitCommandBuffers:count:
// (queue thread) = vkEndCommandBuffer + vkQueueSubmit(fence) + didSchedule; a serial completion queue waits the
// fence in 2 s slices, then calls commandBufferDidComplete:startTime:completionTime:error:.
// 11e: the command buffer also owns the per-command-buffer descriptor pools, the bytes upload ring, the strong references to every
// bound resource (released when the command buffer is, i.e. after the fence) and a sticky NSError from encoding (delivered as the
// completion error instead of submitting).
// ---------------------------------------------------------------------------------------------------------------
@interface N48CommandQueue : _MTLCommandQueue {
    VkCommandPool _pool; dispatch_queue_t _cq; id _ndev;
    NSUInteger _gprio, _bprio; id _compQ;   // gap census: SkyLight's priority / completion-queue setters (recorded only)
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (VkCommandPool)pool;
@end

// CRC diagnostic, completion time (fence done, the copy ran): A1, B, A2. Returns the record the present step uses for C (nil when the check cannot run).
static n48crc_rec_t *n48s_crc_check(N48Texture *dt, int slot, uint64_t seq) {
    const uint8_t *src = [dt n48IBase]; IOSurfaceRef ios = [dt n48IOSRef];
    uint8_t scratch[16384]; if (!src || !ios || N48S.pitch > sizeof scratch || slot < 0 || slot >= N48DF_MAX_SLOTS) return NULL;
    if (!N48S.map[slot]) {
        void *m = NULL; VkResult mr = vkMapMemory(N48R.dev, N48S.mem[slot], 0, VK_WHOLE_SIZE, 0, &m);
        if (mr != VK_SUCCESS || !m) { if (!N48C.mapfail++) N48LOG("scanout: CRC diagnostic: vkMapMemory(slot %d) = %d: the slot CRC is skipped", slot, mr); return NULL; }
        N48S.map[slot] = m;
    }
    n48crc_rec_t *r = calloc(1, sizeof *r); if (!r) return NULL;
    r->ios = (IOSurfaceRef)CFRetain(ios); r->base = src; r->pitch = N48S.pitch; r->rowbytes = N48S.pitch; r->h = N48S.ph; r->step = atomic_load(&N48C.step); r->sid = IOSurfaceGetID(ios); r->slot = slot; r->seq = seq;
    n48crc_samp_t a2, b; uint64_t t0 = n48_now(), ts = 0; int doslot = !atomic_load(&N48C.noslot);
    n48crc_sample(&r->a1, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    if (doslot) { uint64_t t1 = n48_now(); n48crc_sample(&b, (const uint8_t *)N48S.map[slot], r->pitch, r->rowbytes, r->h, r->step, scratch); ts = n48_now() - t1; }
    n48crc_sample(&a2, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    uint64_t dtn = n48_now() - t0;
    pthread_mutex_lock(&N48C.mu);
    int later = 0; for (int i = 0; i < 8; i++) if (N48C.surf[i].sid == r->sid) { later = N48C.surf[i].seq > seq; break; }
    int f = n48crc_account(&N48C.st, &r->a1, &a2, doslot ? &b : NULL, dtn, ts, later); r->unst = (f & N48CRC_F_UNSTABLE) != 0;
    int lg = f && N48C.st.logged < 20 ? (int)++N48C.st.logged : 0;
    pthread_mutex_unlock(&N48C.mu);
    if (lg) {
        uint32_t d1[8] = { 0 }, d2[8] = { 0 }; int n1 = doslot ? n48crc_diff(&r->a1, &b, d1, 8) : 0, n2 = n48crc_diff(&r->a1, &a2, d2, 8);
        N48LOG("scanout: CRC MISMATCH #%d: seq %llu surface %u slot %d step %u:%s%s slot!=src rows %d (y: %u %u %u %u), src-unstable rows %d (y: %u %u %u %u), %s, check cost %.0f us (slot read %.0f us)", lg, (unsigned long long)seq, r->sid, slot, r->step,
               (f & N48CRC_F_SLOT_NE) ? " SLOT_NE_SRC" : "", (f & N48CRC_F_UNSTABLE) ? " SRC_UNSTABLE" : "", n1, d1[0], n1 > 1 ? d1[1] : 0, n1 > 2 ? d1[2] : 0, n1 > 3 ? d1[3] : 0,
               n2, d2[0], n2 > 1 ? d2[1] : 0, n2 > 2 ? d2[2] : 0, n2 > 3 ? d2[3] : 0,
               later ? "a LATER command buffer for this surface was submitted" : "NO later command buffer: written outside our command buffers", dtn / 1000.0, ts / 1000.0);
    }
    return r;
}

@interface N48CommandBuffer : _MTLCommandBuffer {
    VkCommandBuffer _vk; VkFence _fence; BOOL _ended; NSMutableArray *_rps; NSMutableArray *_fbs; N48CommandQueue *_nq;
    NSMutableArray *_refs; N48Buffer *_ring; NSUInteger _ringUsed; NSMutableArray *_pools; VkDescriptorPool _pool; NSError *_encErr;
    NSMutableArray *_iot, *_iow;   // 11h.6: IOSurface textures touched / written by this command buffer
    uint64_t _dseq;   // S5.2a/#12: frame sequence, taken at submission (under the submit lock) = GPU order
    uint32_t _dsid;   // #12: IOSurface id of the display surface this cb writes (0 = unknown)
    int _dcls; uint64_t _dcid, _dbid;   // #12 D2: write class of the presented frame; chain id of this frame and of its base (0 = full copy)
    int _dslot;   // S5.2a: the scanout slot this command buffer's appended copy targets (-1 = none)
    BOOL _dcrc; N48Texture *_dtex;   // CRC diagnostic: this frame is checked; the display texture whose import memory is sampled
    struct n48dw { __unsafe_unretained N48Texture *t; uint32_t mask; const void *lastpso; char fns[128]; n48df_wr_t wr; } _dw[4]; int _ndw;   // native #12 P1: per display surface written, how it was written
    uint64_t _tBeg; BOOL _frame;   // native #12 T1: creation time; wrote a display surface (a "frame")
    uint64_t _cbid, _tcommit; uint32_t _cbw[N48CB_MAXW], _cbr[N48CB_MAXR], _ncbw, _ncbr; int _cbcls; uint32_t _cbdsid;   // Stage 0b: log id, commit time, IOSurface ids written / read (sampled or loaded), display write class+1 and sid
    NSMutableArray *_evWait, *_evSig; NSUInteger _prot;   // gap census: encodeWaitForEvent / encodeSignalEvent (CPU emulation), protection options
    uint64_t _pser;   // P1: fence serial taken when this command buffer was created (0 = pool OFF); closed when its fence completed / it failed to submit / it was dropped unsubmitted
}
- (void)n48PoolDone;
- (BOOL)n48WaitEvents:(NSError **)err;
- (uint64_t)n48TBegin;
- (BOOL)n48IsFrame;
- (void)n48SignalEvents;
- (BOOL)n48IOFirstTouch:(N48Texture *)t;
- (void)n48IOWritten:(N48Texture *)t;
- (void)n48CBRead:(N48Texture *)t;   // Stage 0b: t's IOSurface is sampled / loaded by this command buffer
- (void)n48CBSubmit:(uint64_t)tq;    // Stage 0b: call with the submit lock held, right before vkQueueSubmit (after n48DispSeq)
- (void)n48CBDrop;
- (void)n48CBFence:(int)vkr;
- (void)n48DispDraw:(N48Texture *)t bits:(uint32_t)bits rect:(n48df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(N48Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc;   // native #12 D1: the extent of one draw
- (void)n48DispNote:(N48Texture *)t bits:(uint32_t)bits pso:(N48RenderPipelineState *)p;   // native #12 P1: a write of kind `bits` to t (ignored unless t is a display surface)
- (VkCommandBuffer)vk;
- (void)keepRenderPass:(VkRenderPass)rp framebuffer:(VkFramebuffer)fb;
- (BOOL)n48Finish:(NSError **)err;      // vkEndCommandBuffer
- (void)n48DispSeq;
- (void)n48DispDone:(VkResult)r;     // S5.2a: the appended copy ended (r) or never ran; presents on VK_SUCCESS only; idempotent
- (VkFence)fence;
- (void)setFence:(VkFence)f;
- (void)n48Retain:(id)o;
- (void)n48Fail:(NSString *)why;
- (NSError *)n48Error;
- (BOOL)n48Bytes:(const void *)p length:(NSUInteger)n buffer:(N48Buffer * __strong *)ob offset:(NSUInteger *)oo;
- (VkDescriptorSet)n48AllocSet:(VkDescriptorSetLayout)l;
- (BOOL)n48FillSet:(VkDescriptorSet)set bindings:(const N48PB *)pb count:(uint32_t)n state:(const N48StageState *)st stage:(const char *)nm;
@end

@implementation N48CommandQueue
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    if (!n48_radv_open(err)) return nil;
    id qd = desc ? [desc copy] : [[NSClassFromString(@"MTLCommandQueueDescriptor") alloc] init];
    if ([[qd valueForKey:@"maxCommandBufferCount"] unsignedIntegerValue] == 0) [qd setValue:@64 forKey:@"maxCommandBufferCount"];   // base asserts on 0
    self = [super initWithDevice:dev descriptor:qd];
    if (!self) { if (err) *err = n48_err(50, @"_MTLCommandQueue initWithDevice:descriptor: returned nil"); return nil; }
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = N48R.qfi,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkResult r = vkCreateCommandPool(N48R.dev, &cpc, NULL, &_pool);
    if (r != VK_SUCCESS) { if (err) *err = n48_err(51, [NSString stringWithFormat:@"vkCreateCommandPool = %d", r]); return nil; }
    _ndev = dev; _cq = dispatch_queue_create("navi48.completion", DISPATCH_QUEUE_SERIAL);
    N48LOG("N48CommandQueue %p: pool %p", (__bridge void *)self, (void *)_pool);
    return self;
}
- (void)dealloc { if (N48R.ok && _pool) vkDestroyCommandPool(N48R.dev, _pool, NULL); }
N48_DNR(N48CommandQueue)
- (VkCommandPool)pool { return _pool; }
- (id)device { return _ndev; }
// ---- gap census: MTLCommandQueueSPI, sent by SkyLight right after -newCommandQueue (census rdt1). Priorities are recorded; there is one hardware queue. ----
// Return values as measured on an Apple-silicon Mac (mtlprobe apigaps): setGPUPriority: YES, the background setters NO.
- (BOOL)setGPUPriority:(NSUInteger)p { _gprio = p; N48_ONCE("queue setGPUPriority: recorded only"); return YES; }
- (BOOL)setGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _gprio = p; return YES; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p { _bprio = p; return NO; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _bprio = p; return NO; }
- (BOOL)_setGPUPriority:(NSUInteger)p backgroundPriority:(NSUInteger)b { _gprio = p; _bprio = b; N48_ONCE("queue _setGPUPriority:backgroundPriority: recorded only"); return NO; }
- (NSUInteger)getGPUPriority { return _gprio; }
- (NSUInteger)getBackgroundGPUPriority { return _bprio; }
- (void)setCompletionQueue:(id)q { _compQ = q; N48_ONCE("queue setCompletionQueue: recorded only (completion handlers run on the bundle's completion thread)"); }
- (id)commandBuffer {
    N48LOGR("queue commandBuffer");
    return [[N48CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
}
- (id)commandBufferWithUnretainedReferences {
    N48LOGR("queue commandBufferWithUnretainedReferences");
    return [[N48CommandBuffer alloc] initWithQueue:self retainedReferences:NO];
}
- (id)commandBufferWithDescriptor:(id)d {
    (void)d; N48LOGR("queue commandBufferWithDescriptor:");
    return [[N48CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
}
- (void)submitCommandBuffers:(id __unsafe_unretained const *)cbs count:(NSUInteger)n {
    N48LOGR("queue submitCommandBuffers:count:%lu", (unsigned long)n);
    for (NSUInteger i = 0; i < n; i++) {
        N48CommandBuffer *cb = (N48CommandBuffer *)cbs[i];
        NSError *err = nil; uint64_t t0 = n48_now();
        n48t_add(&T1.enc, t0 - [cb n48TBegin]);
        BOOL ok = [cb n48Finish:&err];
        BOOL isFrame = [cb n48IsFrame];
        if (isFrame) { atomic_fetch_add(&n48_t1_frames, 1); uint64_t lf = atomic_exchange(&n48_t1_lastframe, t0); if (lf) n48t_add(&T1.fgap, t0 - lf); }
        if (ok) ok = [cb n48WaitEvents:&err];   // gap census: encodeWaitForEvent blocks the submission, in commit order
        VkFence fence = VK_NULL_HANDLE;
        if (ok) {
            VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkResult r = vkCreateFence(N48R.dev, &fc, NULL, &fence);
            if (r != VK_SUCCESS) { err = n48_err(52, [NSString stringWithFormat:@"vkCreateFence = %d", r]); ok = NO; }
        }
        if (ok) {
            [cb setFence:fence];
            VkCommandBuffer vcb = [cb vk];
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vcb };
            n48_qlock();
            [cb n48DispSeq];
            [cb n48CBSubmit:t0];   // Stage 0b
            VkResult r = vkQueueSubmit(N48R.q, 1, &si, fence);
            pthread_mutex_unlock(&N48R.qlock);
            N48LOGR("vkQueueSubmit = %d", r);
            if (r != VK_SUCCESS) { err = n48_err(53, [NSString stringWithFormat:@"vkQueueSubmit = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : ""]); ok = NO; }
        }
        uint64_t t1 = n48_now();
        [cb didScheduleWithStartTime:t0 endTime:t1 error:ok ? nil : err];
        N48LOGR("didSchedule sent (ok=%d)", ok);
        if (!ok) { [cb n48PoolDone]; [cb n48CBDrop]; [cb n48DispDone:VK_ERROR_UNKNOWN]; [cb n48SignalEvents]; [self commandBufferDidComplete:cb startTime:t0 completionTime:t1 error:err]; continue; }   // signal anyway: a waiting queue must not hang
        id keep = cb;
        // 10e test hook: N48M_TEST_TIMEOUT_MS=<ms> shrinks the FIRST fence wait of the process to one slice of <ms>. On timeout the
        // error is delivered as usual, but this block still waits the fence to completion before releasing `keep` (the command
        // buffer, its render passes and framebuffers), so nothing is freed while the GPU uses it.
        static _Atomic int testUsed; static long testMs = -1;
        if (testMs < 0) { const char *e = getenv("N48M_TEST_TIMEOUT_MS"); testMs = e ? atol(e) : 0; }
        BOOL testWait = testMs > 0 && atomic_exchange(&testUsed, 1) == 0;
        long tms = testMs;
        uint64_t tenq = n48_now();
        dispatch_async(_cq, ^{
            n48t_add(&T1.wake, n48_now() - tenq);
            NSError *werr = nil; VkResult r = VK_TIMEOUT; int slices = 0; BOOL delivered = NO;
            if (testWait) {
                r = vkWaitForFences(N48R.dev, 1, &fence, VK_TRUE, (uint64_t)tms * 1000000ULL); slices = 1;
                N48LOG("TEST_TIMEOUT: first fence wait of %ld ms = %d", tms, r);
                if (r == VK_TIMEOUT) {
                    werr = n48_err(54, [NSString stringWithFormat:@"vkWaitForFences = %d (TIMEOUT after %ld ms; test hook, GPU still busy)", r, tms]);
                    [keep n48SignalEvents];
                    [self commandBufferDidComplete:keep startTime:t1 completionTime:n48_now() error:werr];
                    N48LOG("TEST_TIMEOUT: commandBufferDidComplete(error) sent; still waiting the fence before release");
                    delivered = YES;
                }
            }
            while (r == VK_TIMEOUT && slices < 30) {   // 2 s slices, 60 s cap
                r = vkWaitForFences(N48R.dev, 1, &fence, VK_TRUE, 2000000000ULL); slices++;
                if (r == VK_TIMEOUT) N48LOG("vkWaitForFences: slice %d timed out", slices);
            }
            if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) [keep n48PoolDone];   // P1: the GPU is finished with (or has lost) everything this command buffer touched; a 60 s TIMEOUT leaves the fence open so nothing newer is ever reused
            if (delivered) { [keep n48DispDone:r]; N48LOG("TEST_TIMEOUT: fence finally = %d after %d slice(s)", r, slices); return; }
            if (r != VK_SUCCESS)
                werr = n48_err(54, [NSString stringWithFormat:@"vkWaitForFences = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : r == VK_TIMEOUT ? " (TIMEOUT: HUNG?)" : ""]);
            { uint64_t tg_ = n48_now() - t1; n48t_add(&T1.gpu, tg_); if (isFrame) n48t_add(&T1.fgpu, tg_); }
            N48LOGR("fence wait done = %d after %d slice(s)", r, slices);
            [keep n48CBFence:(int)r];   // Stage 0b
            [keep n48DispDone:r];   // S5.2a: present the appended copy's slot (VK_SUCCESS only)
            [keep n48SignalEvents];
            [self commandBufferDidComplete:keep startTime:t1 completionTime:n48_now() error:werr];
            N48LOGR("commandBufferDidComplete sent");
            if (!werr) n48_mark_clean();
        });
    }
}
// Base version runs right after submitCommandBuffers:count: (F4) and would complete before the GPU is done.
- (void)completeCommandBuffers:(id __unsafe_unretained *)cbs count:(NSUInteger)n {
    (void)cbs; N48LOG("queue completeCommandBuffers:count:%lu (deferred to the completion thread)", (unsigned long)n);
}
@end


// 11h.6 IOSurface coherency. Upload at the FIRST use of an IOSurface texture in a command buffer (path b: host buffer -> image; path a needs nothing:
// the GPU reads the host pages directly), recorded outside any render pass. Write-back at the END of the command buffer for every texture that was
// a render target / storage or blit destination (path b: image -> host buffer, then a host-visibility barrier; path a: the host-visibility barrier).
// Both run at GPU execution time, so CPU writes made before commit are seen and the CPU sees the result after the completion handler / waitUntilCompleted.
static void n48_full_barrier(VkCommandBuffer cb);
static void n48_ios_upload(VkCommandBuffer cmd, N48Texture *t) {
    if ([t n48IOSLinear]) { n48_full_barrier(cmd); return; }
    n48_full_barrier(cmd);
    n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = { .bufferOffset = [t n48IOSOffset], .bufferRowLength = (uint32_t)([t n48IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
    vkCmdCopyBufferToImage(cmd, [t n48IOSBuffer], [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    n48_full_barrier(cmd);
}
static void n48_ios_writeback(VkCommandBuffer cmd, N48Texture *t) {
    if (![t n48IsIOS]) return;   // P6: a base-less display texture is in _iow but has no host copy
    if (![t n48IOSLinear]) {
        n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [t n48IOSOffset], .bufferRowLength = (uint32_t)([t n48IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
        vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [t n48IOSBuffer], 1, &bic);
    }
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// P6: the D-copy of a display texture that has only a VkImage (protected IOSurface, no CPU mapping): image -> scanout slot buffer. Layout: the image is moved to TRANSFER_SRC_OPTIMAL with n48_tex_to
// (a full-memory barrier from its current tracked layout, which is what the last render pass / blit left it in; [t layout] is updated, so a later pass starts from TRANSFER_SRC like after n48_ios_writeback);
// the caller brackets this with n48_full_barrier (writes before, slot writes visible after). Same plan as n48s_record_copy: full = one region of the whole image; D2 partial = the base slot first
// (buffer -> buffer), a WAW barrier, then the rect from the image at the same byte offsets (n48df_img_region). The image is the plane's size and 4 bytes per texel (checked before the slot is taken).
static void n48s_record_copy_img(VkCommandBuffer cmd, N48Texture *t, const n48df_plan_t *pl) {
    int slot = pl->slot; VkDeviceSize all = (VkDeviceSize)N48S.pitch * N48S.ph; n48df_imgreg_t g;
    n48df_rect_t full = { 0, 0, (int32_t)N48S.pw, (int32_t)N48S.ph };
    n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (pl->base >= 0) {
        VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, N48S.buf[pl->base], N48S.buf[slot], 1, &bc);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // WAW: the region copy must land after the base copy
    }
    if (!n48df_img_region(pl->base >= 0 ? pl->rect : full, N48S.pw, N48S.ph, N48S.pitch, 4, &g)) return;   // empty partial rectangle: only the base copy (same as the buffer path); full frames were validated by n48df_img_ok
    VkBufferImageCopy bic = { .bufferOffset = g.buf_off, .bufferRowLength = g.row_len, .bufferImageHeight = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)g.x, (int32_t)g.y, 0 }, .imageExtent = { g.w, g.h, 1 } };
    vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, N48S.buf[slot], 1, &bic);
    atomic_fetch_add(&n48s_imgcopies, 1);
}
// Call OUTSIDE a render pass. needContents NO = the caller clears the whole texture, so the upload is skipped.
static void n48_ios_usek(N48CommandBuffer *cb, N48Texture *t, BOOL needContents, BOOL write, uint32_t kind) {
    if (![t n48IsIOS]) {
        if ([t n48DispImgOnly]) {   // P6: a protected display surface has no host copy to upload or write back; it only needs to be retained and recorded as written so the D-copy reads its image
            (void)[cb n48IOFirstTouch:t];
            if (write) { [cb n48IOWritten:t]; if (kind) [cb n48DispNote:t bits:kind pso:nil]; }
        }
        return;
    }
    if ([cb n48IOFirstTouch:t] && needContents) n48_ios_upload([cb vk], t);
    if (needContents) [cb n48CBRead:t];
    if (write) { [cb n48IOWritten:t]; if (kind) [cb n48DispNote:t bits:kind pso:nil]; }
}
static void n48_ios_use(N48CommandBuffer *cb, N48Texture *t, BOOL needContents, BOOL write) { n48_ios_usek(cb, t, needContents, write, 0); }

@implementation N48CommandBuffer
- (BOOL)n48IOFirstTouch:(N48Texture *)t {
    if (!_iot) { _iot = [NSMutableArray array]; _iow = [NSMutableArray array]; }
    if ([_iot indexOfObjectIdenticalTo:t] != NSNotFound) return NO;
    [_iot addObject:t]; [self n48Retain:t]; return YES;
}
- (uint64_t)n48TBegin { return _tBeg; }
- (BOOL)n48IsFrame { return _frame; }
- (void)n48IOWritten:(N48Texture *)t {
    if ([_iow indexOfObjectIdenticalTo:t] == NSNotFound) [_iow addObject:t];
    if ([t n48IsIOS] && [t n48IOSRef]) _ncbw = n48cb_addsid(_cbw, _ncbw, N48CB_MAXW, IOSurfaceGetID([t n48IOSRef]));
    else if ([t n48DispImgOnly]) _ncbw = n48cb_addsid(_cbw, _ncbw, N48CB_MAXW, [t n48DispSid]);   // P6: the transaction correlation also sees protected display surfaces
}
- (void)n48CBRead:(N48Texture *)t { if (t && [t n48IsIOS] && [t n48IOSRef]) _ncbr = n48cb_addsid(_cbr, _ncbr, N48CB_MAXR, IOSurfaceGetID([t n48IOSRef])); }
// Stage 0b. -commit is stamped, logged, then handed to Metal's own implementation unchanged.
- (void)commit {
    _tcommit = n48_now();
    if (n48cbl_on()) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        char w[160], r[256]; n48cbl_sids(w, sizeof w, _cbw, _ncbw); n48cbl_sids(r, sizeof r, _cbr, _ncbr);
        pthread_mutex_lock(&N48CB.mu);
        n48cb_commit(&N48CB.trk, _cbid, (uint64_t)(uintptr_t)(__bridge void *)_nq, _tcommit, _cbw, _ncbw, _cbr, _ncbr);
        n48cbl_line_locked("C t=%llu cb=%llu q=%#llx tid=%llu w=%s r=%s", (unsigned long long)_tcommit, (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, (unsigned long long)tid, w, r);
        pthread_mutex_unlock(&N48CB.mu);
    }
    [super commit];
}
- (void)n48CBSubmit:(uint64_t)tq {
    if (!n48cbl_on()) return;
    n48cb_inv_t inv[N48CB_MAXINV]; uint64_t sq = 0, tid = 0; pthread_threadid_np(NULL, &tid);
    char w[160], r[256]; n48cbl_sids(w, sizeof w, _cbw, _ncbw); n48cbl_sids(r, sizeof r, _cbr, _ncbr);
    uint64_t t = n48_now(), q = (uint64_t)(uintptr_t)(__bridge void *)_nq;
    pthread_mutex_lock(&N48CB.mu);
    uint32_t n = n48cb_submit(&N48CB.trk, _cbid, q, t, _cbw, _ncbw, _cbr, _ncbr, inv, N48CB_MAXINV, &sq);
    n48cbl_line_locked("S t=%llu tq=%llu cb=%llu q=%#llx tid=%llu sub=%llu dseq=%llu cmt=%llu cls=%s disp=%u slot=%d w=%s r=%s inv=%u", (unsigned long long)t, (unsigned long long)tq, (unsigned long long)_cbid, (unsigned long long)q,
        (unsigned long long)tid, (unsigned long long)sq, (unsigned long long)_dseq, (unsigned long long)_tcommit, _cbcls ? n48df_cls_name[_cbcls - 1] : "-", _cbdsid, _dslot, w, r, n);
    for (uint32_t i = 0; i < n && i < N48CB_MAXINV && N48CB.inv_logged < N48_CBLOG_INV_LOGGED; i++) {
        N48CB.inv_logged++;
        n48cbl_line_locked("I t=%llu rd_cb=%llu rd_q=%#llx rd_sub=%llu sid=%u wr_cb=%llu wr_q=%#llx wr_commit=%llu", (unsigned long long)t, (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue,
            (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue, (unsigned long long)inv[i].writer_commit);
        N48LOG("cblog: INVERSION #%llu: cb %llu (queue %#llx, submit #%llu) reads IOSurface %u whose writer cb %llu (queue %#llx, committed earlier) is not yet submitted", (unsigned long long)N48CB.inv_logged,
            (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue, (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue);
    }
    pthread_mutex_unlock(&N48CB.mu);
}
- (void)n48CBDrop {
    if (!n48cbl_on()) return;
    pthread_mutex_lock(&N48CB.mu); n48cb_drop(&N48CB.trk, _cbid); n48cbl_line_locked("D t=%llu cb=%llu", (unsigned long long)n48_now(), (unsigned long long)_cbid); pthread_mutex_unlock(&N48CB.mu);
}
- (void)n48CBFence:(int)vkr {
    if (!n48cbl_on()) return;
    pthread_mutex_lock(&N48CB.mu); n48cbl_line_locked("F t=%llu cb=%llu q=%#llx vk=%d", (unsigned long long)n48_now(), (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, vkr); pthread_mutex_unlock(&N48CB.mu);
}
// native #12 P1: records HOW a display surface is written by this command buffer (OR-ed bits per surface; the pipeline names of every draw, deduplicated).
- (void)n48DispNote:(N48Texture *)t bits:(uint32_t)bits pso:(N48RenderPipelineState *)p {
    if (![t n48IsDisp]) return;
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) { if (_ndw >= 4) return; k = _ndw++; _dw[k].t = t; _dw[k].mask = 0; _dw[k].lastpso = NULL; _dw[k].fns[0] = 0; memset(&_dw[k].wr, 0, sizeof _dw[k].wr); }
    _dw[k].mask |= bits;
    if (bits & N48DF_W_CLEAR) n48df_dmg_set_full(&_dw[k].wr.all);   // a clear load wrote the whole surface (for D2 it counts only when a presentable draw follows in the same pass: see n48DispDraw)
    if (bits & (N48DF_W_BLIT | N48DF_W_COMPUTE)) { n48df_dmg_set_full(&_dw[k].wr.all); n48df_dmg_set_full(&_dw[k].wr.pres); }   // unknown extent: the whole surface
    if (p && _dw[k].lastpso != (__bridge const void *)p) {
        _dw[k].lastpso = (__bridge const void *)p;
        if (N48DO.on && atomic_load(&n48_dw_logged) >= 20) return;   // P5b: fns is only ever printed by the first 20 "scanout: display write" lines; nothing else reads it
        char one[140]; snprintf(one, sizeof one, "%s/%s ", [p n48VName], [p n48FName]);
        if (!strstr(_dw[k].fns, one) && strlen(_dw[k].fns) + strlen(one) < sizeof _dw[k].fns) strlcat(_dw[k].fns, one, sizeof _dw[k].fns);
    }
}
// native #12 D1: one draw on display surface t. `all` collects every draw (statistics); `pres` only the presentable ones (GPUPass, SkyLight composite; never ColorFill) plus, for the first such draw
// of a pass whose load action was Clear, the whole surface. The first presentable draw's first sampled IOSurface texture is the "source" (what GPUPass reads).
- (void)n48DispDraw:(N48Texture *)t bits:(uint32_t)bits rect:(n48df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(N48Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc {
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) return;
    n48df_wr_t *wr = &_dw[k].wr;
    n48df_dmg_add_draw(&wr->all, r, w, h);
    if (!wr->vp_set) { wr->vp_set = 1; wr->vp[0] = vp.originX; wr->vp[1] = vp.originY; wr->vp[2] = vp.width; wr->vp[3] = vp.height; wr->sc[0] = (int64_t)sc.x; wr->sc[1] = (int64_t)sc.y; wr->sc[2] = (int64_t)sc.width; wr->sc[3] = (int64_t)sc.height; }
    if (bits & (N48DF_W_DRAW_FINAL | N48DF_W_DRAW_COMP)) {
        n48df_dmg_add_draw(&wr->pres, r, w, h);
        if (clr) n48df_dmg_set_full(&wr->pres);
        if (!wr->src_kind && src && [src n48IOSRef]) {
            uint32_t a = IOSurfaceGetID([src n48IOSRef]), b = [t n48IOSRef] ? IOSurfaceGetID([t n48IOSRef]) : 0;
            wr->src_sid = a; wr->src_w = (uint32_t)src.width; wr->src_h = (uint32_t)src.height;
            wr->src_kind = a == b ? 3 : [src n48IsDisp] ? 2 : 1;
        }
    }
}
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r {
    self = [super initWithQueue:q retainedReferences:r];
    if (!self) return nil;
    _cbid = atomic_fetch_add(&n48cb_ids, 1) + 1; n48cbl_start();
    _dslot = -1; _nq = q; _rps = [NSMutableArray array]; _fbs = [NSMutableArray array]; _refs = [NSMutableArray array]; _pools = [NSMutableArray array];
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = [_nq pool],
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    n48_qlock();
    VkResult vr = vkAllocateCommandBuffers(N48R.dev, &ai, &_vk);
    pthread_mutex_unlock(&N48R.qlock);
    if (vr != VK_SUCCESS) { N48LOG("vkAllocateCommandBuffers = %d", vr); return nil; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vr = vkBeginCommandBuffer(_vk, &bi);
    if (vr != VK_SUCCESS) { N48LOG("vkBeginCommandBuffer = %d", vr); return nil; }
    _tBeg = n48_now();
    if (N48P.on) _pser = n48_fence_open();   // P1: from here on nothing freed may be reused until this command buffer has completed (or is gone unsubmitted)
    N48LOGR("N48CommandBuffer %p: vk %p begun", (__bridge void *)self, (void *)_vk);
    return self;
}
- (void)dealloc {
    if (N48R.ok) {
        if (_pser && !_fence) n48_fence_close(_pser);   // P1: never submitted (a submitted one is closed by its completion block; a hung one stays open)
        for (NSValue *v in _pools) { VkDescriptorPool dp_ = (VkDescriptorPool)v.pointerValue; if (!(N48P.on && n48_dp_give(dp_))) vkDestroyDescriptorPool(N48R.dev, dp_, NULL); }   // P1: ON -> the pool goes to the fence-gated free list (cap 32)
        for (NSValue *v in _fbs) vkDestroyFramebuffer(N48R.dev, (VkFramebuffer)v.pointerValue, NULL);
        for (NSValue *v in _rps) vkDestroyRenderPass(N48R.dev, (VkRenderPass)v.pointerValue, NULL);
        if (_dslot >= 0) { int s = _dslot; _dslot = -1; n48s_complete(s, VK_ERROR_UNKNOWN, 0, 0, NULL, 0, 0); }   // never presented
        if (_fence) vkDestroyFence(N48R.dev, _fence, NULL);
        if (_vk && _nq) { n48_qlock(); VkCommandBuffer c = _vk; vkFreeCommandBuffers(N48R.dev, [_nq pool], 1, &c); pthread_mutex_unlock(&N48R.qlock); }
    }
}
N48_DNR(N48CommandBuffer)
// ---- gap census: events and MTLCommandBufferSPI protection options ----
// MTLEvent / MTLSharedEvent: -newEvent / -newSharedEvent return Metal's generic _MTLSharedEvent (CPU-side value + waiters), which works unchanged on this device.
// The GPU never touches the event: -encodeWaitForEvent:value: blocks this command buffer's SUBMISSION (queue thread, before vkQueueSubmit, in commit order) until the
// event reaches the value; -encodeSignalEvent:value: sets the value when the command buffer's GPU work has completed (completion thread, before the completed handlers).
// Granularity is the command buffer, not the position of the call inside it.
- (void)encodeSignalEvent:(id)e value:(uint64_t)v {
    if (!_evSig) _evSig = [NSMutableArray array];
    [_evSig addObject:@[ e, @(v) ]]; [self n48Retain:e];
}
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v { [self encodeWaitForEvent:e value:v timeout:60000]; }
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v timeout:(uint32_t)ms {
    if (!_evWait) _evWait = [NSMutableArray array];
    [_evWait addObject:@[ e, @(v), @(ms) ]]; [self n48Retain:e];
}
- (BOOL)n48WaitEvents:(NSError **)err {
    for (NSArray *w in _evWait) {
        id e = w[0]; uint64_t v = [w[1] unsignedLongLongValue]; uint32_t ms = [w[2] unsignedIntValue];
        if (![e respondsToSelector:@selector(waitUntilSignaledValue:timeoutMS:)]) { if (err) *err = n48_err(56, @"encodeWaitForEvent: the event cannot be waited on from the CPU"); return NO; }
        uint64_t t0 = n48_now();
        BOOL ok = [(id<MTLSharedEvent>)e waitUntilSignaledValue:v timeoutMS:ms];
        N48LOG("event wait for value %llu: %s after %.3f s", (unsigned long long)v, ok ? "signaled" : "TIMED OUT", (double)(n48_now() - t0) / 1e9);
        if (!ok) { if (err) *err = n48_err(57, [NSString stringWithFormat:@"encodeWaitForEvent: value %llu not signaled within %u ms", (unsigned long long)v, ms]); return NO; }
    }
    return YES;
}
- (void)n48SignalEvents {
    for (NSArray *w in _evSig) {
        id e = w[0];
        if ([e respondsToSelector:@selector(setSignaledValue:)]) { [(id<MTLSharedEvent>)e setSignaledValue:[w[1] unsignedLongLongValue]]; N48LOG("event signaled to %llu", [w[1] unsignedLongLongValue]); }
        else N48LOG("encodeSignalEvent: the event cannot be signaled from the CPU; skipped");
    }
    _evSig = nil;
}
- (NSUInteger)protectionOptions { return _prot; }
- (void)setProtectionOptions:(NSUInteger)o { _prot = o; N48_ONCE("cb setProtectionOptions: recorded only (no protected content)"); }
- (VkCommandBuffer)vk { return _vk; }
- (id)device { return [_nq device]; }
- (VkFence)fence { return _fence; }
- (void)setFence:(VkFence)f { _fence = f; }
- (void)keepRenderPass:(VkRenderPass)rp framebuffer:(VkFramebuffer)fb {
    [_rps addObject:[NSValue valueWithPointer:(void *)rp]]; [_fbs addObject:[NSValue valueWithPointer:(void *)fb]];
}
- (void)n48Retain:(id)o { if (o) [_refs addObject:o]; }
- (void)n48Fail:(NSString *)why {
    N48LOG("ENCODE ERROR (command buffer will complete with an error): %s", why.UTF8String);
    if (!_encErr) _encErr = n48_err(66, why);
}
- (NSError *)n48Error { return _encErr; }
- (void)n48DispDone:(VkResult)r {
    int s = _dslot; if (s < 0) return; _dslot = -1;
    n48crc_rec_t *rec = (_dcrc && r == VK_SUCCESS && _dtex) ? n48s_crc_check(_dtex, s, _dseq) : NULL;
    _dtex = nil;
    n48s_complete(s, r, _dseq, _dsid, rec, _dcls, _tcommit);
}
- (void)n48DispSeq {
    if (_dslot >= 0 || (_dcrc && _dtex)) _dseq = atomic_fetch_add(&N48S.seq, 1) + 1;
    if (_dslot >= 0 && (_dsid || _dcid)) { pthread_mutex_lock(&N48S.mu); if (_dsid) n48df_submit(&N48S.sm, _dsid, _dseq); n48df_chain_submit(&N48S.sm, _dslot, _dcid, _dbid); pthread_mutex_unlock(&N48S.mu); }   // #12: only frames WITH a slot supersede earlier ones
    if (_dcrc && _dtex && [_dtex n48IOSRef]) n48s_crc_note(IOSurfaceGetID([_dtex n48IOSRef]), _dseq);   // CRC diagnostic: which surface this submission wrote
}   // call with the submit lock held, right before vkQueueSubmit
- (BOOL)n48Finish:(NSError **)err {
    if (_ended) return _encErr == nil;
    _ended = YES;
    if (_encErr) { if (err) *err = _encErr; return NO; }   // never submit a command buffer whose encoding failed
    for (N48Texture *t in _iow) n48_ios_writeback(_vk, t);
    {   // S5.2a: append barrier + copy (display texture's import -> a free scanout slot) + barrier; the completion block presents it
        N48Texture *dt = nil; int nd = 0; uint32_t dmask = 0; const char *dfns = ""; BOOL dfinal = NO; const n48df_wr_t *dwr = NULL;
        int dmgon = n48s_damage_on();
        for (N48Texture *t in _iow) if ([t n48IsDisp]) {   // the presented surface: the last presentable one (FINAL pass; with damage on also a SkyLight composite pass) in write order, else the last display surface written
            nd++; uint32_t m = 0; const char *f = ""; const n48df_wr_t *w = NULL;
            for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { m = _dw[i].mask; f = _dw[i].fns; w = &_dw[i].wr; break; }
            BOOL fin = (m & N48DF_W_DRAW_FINAL) != 0 || (dmgon && (m & N48DF_W_DRAW_COMP) != 0);
            if (!dt || fin || !dfinal) { dt = t; dmask = m; dfns = f; dfinal = fin; dwr = w; }
        }
        if (nd > 1) { uint64_t m = atomic_fetch_add(&N48S.multi, 1) + 1; if (m <= 8) N48LOG("scanout: command buffer wrote %d display surfaces; the LAST one in write order is presented (study: frame-order ambiguity, #%llu)", nd, (unsigned long long)m); }
        if (dt) {
            _frame = YES; _cbcls = n48df_class(dmask) + 1; _cbdsid = [dt n48DispSid];
            BOOL imgsrc = [dt n48DispImgOnly];   // P6: a protected display surface: copy from its VkImage
            BOOL take = n48s_disp_account(dmask, [dt n48DispSid], dfns, atomic_fetch_add(&N48S.cbno, 1) + 1, dwr);   // native #12 P1/P2/D1: only a presentable frame takes a slot
            n48df_plan_t pl; pl.slot = -1;
            if (take && imgsrc && !(n48df_img_ok(N48S.pw, N48S.ph, N48S.pitch, 4) && [dt bytesPerPixel] == 4 && [dt width] == N48S.pw && [dt height] == N48S.ph && [dt n48Levels] == 1)) {   // refuse before a slot is taken
                static _Atomic int rl; if (atomic_fetch_add(&rl, 1) < 8) N48LOG("scanout: display surface %u has no CPU mapping and its image cannot be copied to the slot (%lux%lu bpp %lu, plane %ux%u pitch %u): frame not copied", [dt n48DispSid], (unsigned long)[dt width], (unsigned long)[dt height], (unsigned long)[dt bytesPerPixel], N48S.pw, N48S.ph, N48S.pitch);
                take = NO; }
            int sl = take ? n48s_plan(dwr, &pl) : -1;
            if (sl >= 0) { n48_full_barrier(_vk); if (imgsrc) n48s_record_copy_img(_vk, dt, &pl); else n48s_record_copy(_vk, [dt n48DispBuf], &pl); n48_full_barrier(_vk); _dslot = sl; _dcls = n48df_class(dmask); _dcid = pl.id; _dbid = pl.baseid; _dsid = [dt n48DispSid]; }
            if (n48s_crc_on()) { _dcrc = YES; _dtex = dt; }
        }
    }
    VkResult r = vkEndCommandBuffer(_vk);
    if (r != VK_SUCCESS) { [self n48DispDone:r]; if (err) *err = n48_err(55, [NSString stringWithFormat:@"vkEndCommandBuffer = %d", r]); return NO; }
    return YES;
}
- (void)n48PoolDone { if (_pser) n48_fence_close(_pser); }
// Copies bytes into the command buffer's upload ring (Shared memory, 256-byte aligned: covers every storage/uniform offset alignment).
- (BOOL)n48Bytes:(const void *)p length:(NSUInteger)n buffer:(N48Buffer * __strong *)ob offset:(NSUInteger *)oo {
    NSUInteger need = ((n ? n : 1) + 255) & ~(NSUInteger)255;
    if (!_ring || _ringUsed + need > [_ring length]) {
        NSError *e = nil; NSUInteger sz = need > 262144 ? need : 262144;
        N48Buffer *nb = [[N48Buffer alloc] initWithDevice:[_nq device] length:sz options:MTLResourceStorageModeShared | (N48P.on ? N48_OPT_NOZERO : 0) error:&e];   // P1: slab range / recycled, not a kernel BO per command buffer; every byte is written before use
        if (!nb) { [self n48Fail:[NSString stringWithFormat:@"upload ring: %@", e.localizedDescription]]; return NO; }
        _ring = nb; _ringUsed = 0; [_refs addObject:nb];
    }
    memcpy((uint8_t *)[_ring contents] + _ringUsed, p, n);
    *ob = _ring; *oo = _ringUsed; _ringUsed += need;
    return YES;
}
- (VkDescriptorSet)n48AllocSet:(VkDescriptorSetLayout)l {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!_pool) {
            VkDescriptorPoolSize sz[6] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 },
                                           { VK_DESCRIPTOR_TYPE_SAMPLER, 512 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 256 } };
            VkDescriptorPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256, .poolSizeCount = 6, .pPoolSizes = sz };
            VkDescriptorPool reuse_ = n48_dp_take();   // P1: a fence-cleared, reset pool of an earlier command buffer (VK_NULL_HANDLE when OFF)
            if (reuse_) _pool = reuse_;
            else {
            VkResult r = vkCreateDescriptorPool(N48R.dev, &pc, NULL, &_pool);
            if (r != VK_SUCCESS) { _pool = VK_NULL_HANDLE; [self n48Fail:[NSString stringWithFormat:@"vkCreateDescriptorPool = %d", r]]; return VK_NULL_HANDLE; }
            n48_dp_created();
            }
            [_pools addObject:[NSValue valueWithPointer:(void *)_pool]];
        }
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = _pool, .descriptorSetCount = 1, .pSetLayouts = &l };
        VkDescriptorSet s = VK_NULL_HANDLE; VkResult r = vkAllocateDescriptorSets(N48R.dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        _pool = VK_NULL_HANDLE;   // exhausted or fragmented: the next attempt makes a new pool
    }
    [self n48Fail:@"vkAllocateDescriptorSets failed twice (descriptor pool)"];
    return VK_NULL_HANDLE;
}
// Writes the descriptors of one stage's set from the encoder's bound state (see the binding-model comment).
- (BOOL)n48FillSet:(VkDescriptorSet)set bindings:(const N48PB *)pb count:(uint32_t)n state:(const N48StageState *)st stage:(const char *)nm {
    enum { MAXW = 96, MAXI = 640 };
    if (n > MAXW) { [self n48Fail:[NSString stringWithFormat:@"%s stage has %u descriptors (> %d)", nm, n, MAXW]]; return NO; }
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXI]; VkDescriptorImageInfo ii[MAXI]; uint32_t nbi = 0, nii = 0; id dev = [self device];
    for (uint32_t i = 0; i < n; i++) {
        const N48PB *p = &pb[i];
        if (p->count > MAXI - nbi || p->count > MAXI - nii) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: too many array descriptors", nm, p->binding]]; return NO; }
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = p->binding, .descriptorCount = p->count, .descriptorType = p->type };
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            if (p->binding >= 32) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: buffer descriptor outside the [[buffer(n)]] band 0..31 (synthetic address table? not supported)", nm, p->binding]]; return NO; }
            w[i].pBufferInfo = &bi[nbi];
            NSUInteger align = p->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? N48R.lim.minUniformBufferOffsetAlignment : N48R.lim.minStorageBufferOffsetAlignment;
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding + k; N48BufSlot s = ix < 31 ? st->buf[ix] : (N48BufSlot){ nil, 0 }; N48Buffer *b = s.b; NSUInteger off = s.off;
                if (!b) { N48LOG("WARN %s buffer(%u) is used by the shader but was never bound: zero-filled dummy", nm, ix); b = n48_dummy_buffer(dev); off = 0; }
                if (!b) { [self n48Fail:@"no dummy buffer"]; return NO; }
                if (align && off % align) { [self n48Fail:[NSString stringWithFormat:@"%s buffer(%u) offset %lu is not a multiple of %lu", nm, ix, (unsigned long)off, (unsigned long)align]]; return NO; }
                [self n48Retain:b];
                bi[nbi++] = (VkDescriptorBufferInfo){ [b vkBuffer], off, VK_WHOLE_SIZE };
            }
            break; }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            if (p->binding < 32 || p->binding >= 160) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: sampled image outside the texture band 32..159", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 32 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil;
                if (!t) { N48LOG("WARN %s texture(%u) is used by the shader but was never bound: 1x1 dummy", nm, ix); t = n48_dummy_texture(dev); }
                if (!t) { [self n48Fail:@"no dummy texture"]; return NO; }
                [self n48Retain:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t vkView], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (p->binding < 160 || p->binding >= 192) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: sampler outside the band 160..191", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                VkSampler vs = p->stat;
                if (!vs) {
                    uint32_t ix = p->binding - 160 + k; N48SamplerState *s = ix < 16 ? st->smp[ix] : nil;
                    if (!s) { N48LOG("WARN %s sampler(%u) is used by the shader but was never bound: default nearest/clamp sampler", nm, ix); s = n48_dummy_sampler(dev); }
                    if (!s) { [self n48Fail:@"no dummy sampler"]; return NO; }
                    [self n48Retain:s]; vs = [s vkSampler];
                }
                ii[nii++] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (p->binding < 480 || p->binding >= 608) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: storage image outside the band 480..607", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 480 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil;
                if (!t) { [self n48Fail:[NSString stringWithFormat:@"%s: writable texture(%u) is used by the shader but was never bound", nm, ix]]; return NO; }
                [self n48Retain:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t n48AttViewLevel:0], VK_IMAGE_LAYOUT_GENERAL };   // storage images: single-level, identity-swizzle view
            }
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:   // 11e-2: [[color(n)]] = binding 192+n = the encoder's colour attachment n, read in place (GENERAL)
            if (p->binding < 192 || p->binding >= 200) { [self n48Fail:[NSString stringWithFormat:@"%s binding %u: input attachment outside the colour band 192..199", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 192 + k; N48Texture *t = ix < 8 ? st->att[ix] : nil;
                if (!t) { [self n48Fail:[NSString stringWithFormat:@"%s: [[color(%u)]] is read by the shader but the pass has no colour attachment %u", nm, ix, ix]]; return NO; }
                [self n48Retain:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t n48AttViewLevel:0], VK_IMAGE_LAYOUT_GENERAL };
            }
            break;
        default: [self n48Fail:[NSString stringWithFormat:@"%s binding %u: descriptor type %d not supported (texel buffer)", nm, p->binding, p->type]]; return NO;
        }
    }
    vkUpdateDescriptorSets(N48R.dev, n, w, 0, NULL);
    return YES;
}
@end
@interface N48CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
- (id)blitCommandEncoder;
- (id)computeCommandEncoder;
@end

// ---------------------------------------------------------------------------------------------------------------
// N48RenderEncoder / N48ComputeEncoder / N48BlitEncoder (10c, 10d, 11e)
// ---------------------------------------------------------------------------------------------------------------
static void n48_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static VkPrimitiveTopology n48_topo(MTLPrimitiveType t) {
    switch (t) { case MTLPrimitiveTypePoint: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case MTLPrimitiveTypeLine: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case MTLPrimitiveTypeLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case MTLPrimitiveTypeTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}
// fetch = YES (11e-2): framebuffer-fetch shape. Every colour attachment i is also input attachment i (layout GENERAL for both, as Vulkan requires when
// one attachment is read and written in the same subpass), and a framebuffer-local self-dependency lets the encoder put a barrier between draws so a
// draw reads what earlier draws wrote (Metal ordering between draws). Within one draw, overlapping primitives are NOT ordered (no
// rasterization_order_attachment_access on this RADV).
static VkRenderPass n48_mk_rp(const VkAttachmentDescription *ads, uint32_t na, BOOL fetch) {
    VkAttachmentReference refs[8], ins[8];
    for (uint32_t i = 0; i < na; i++) { refs[i] = (VkAttachmentReference){ i, fetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }; ins[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL }; }
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = na, .pColorAttachments = refs,
        .inputAttachmentCount = fetch ? na : 0, .pInputAttachments = fetch ? ins : NULL };
    VkSubpassDependency deps[3] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT } };
    // deps[0] = external in, deps[1] = external out, deps[2] = the fetch self-dependency (only declared for a fetch pass)
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = ads, .subpassCount = 1,
        .pSubpasses = &sp, .dependencyCount = fetch ? 3 : 2, .pDependencies = deps };
    VkRenderPass rp = VK_NULL_HANDLE; VkResult r = vkCreateRenderPass(N48R.dev, &rpc, NULL, &rp);
    if (r != VK_SUCCESS) { N48LOG("render pass: vkCreateRenderPass = %d", r); return VK_NULL_HANDLE; }
    return rp;
}
// P5b: n48_mk_rp through the device-wide render-pass cache. *cached = YES: the pass belongs to the cache for the life of the device (the caller must NOT destroy it and hands VK_NULL_HANDLE
// to keepRenderPass:). OFF, or the cache is full: exactly n48_mk_rp (the caller owns the pass as before).
static VkRenderPass n48_mk_rp_c(const VkAttachmentDescription *ads, uint32_t na, BOOL fetch, BOOL *cached) {
    *cached = NO;
    if (!N48DO.on || na > 8) return n48_mk_rp(ads, na, fetch);
    n48dc_rpkey k; memset(&k, 0, sizeof k); k.na = na; k.fetch = fetch ? 1 : 0;
    for (uint32_t i = 0; i < na; i++)
        k.a[i] = (n48dc_att){ (uint32_t)ads[i].format, (uint32_t)ads[i].samples, (uint32_t)ads[i].loadOp, (uint32_t)ads[i].storeOp, (uint32_t)ads[i].stencilLoadOp,
                              (uint32_t)ads[i].stencilStoreOp, (uint32_t)ads[i].initialLayout, (uint32_t)ads[i].finalLayout };
    uint64_t h;
    pthread_mutex_lock(&N48DO.mu); h = n48dc_rp_find(N48DO.rp, &k); pthread_mutex_unlock(&N48DO.mu);
    if (h) { atomic_fetch_add(&N48DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    VkRenderPass rp = n48_mk_rp(ads, na, fetch); if (!rp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&N48DO.mu);
    h = n48dc_rp_find(N48DO.rp, &k);   // another thread may have entered the same key meanwhile
    if (!h && n48dc_rp_add(N48DO.rp, &k, (uint64_t)(uintptr_t)rp)) { pthread_mutex_unlock(&N48DO.mu); atomic_fetch_add(&N48DO.rpCreated, 1); *cached = YES; return rp; }
    pthread_mutex_unlock(&N48DO.mu);
    if (h) { vkDestroyRenderPass(N48R.dev, rp, NULL); atomic_fetch_add(&N48DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    atomic_fetch_add(&N48DO.rpUncached, 1);
    return rp;
}

@interface N48RenderEncoder : _MTLCommandEncoder {
    N48CommandBuffer *_cb; BOOL _ended, _inPass, _begun; uint32_t _w, _h, _na; NSString *_lbl;
    VkRenderPass _rp1; BOOL _rp1c; VkFramebuffer _fb, _fbf; VkClearValue _cvs[8]; VkAttachmentDescription _ads[8];
    VkImageView _views[8]; N48Texture *_tex[8]; VkImageLayout _curLay[8]; BOOL _fetch;   // 11e-2: _fetch = the current/next pass is the framebuffer-fetch shape
    N48RenderPipelineState *_pso; N48StageState _sv, _sf; MTLViewport _vp; MTLScissorRect _sr; float _blend[4]; MTLCullMode _cull; MTLWinding _wind;
    n48dc_enc _dc; BOOL _dcOn;   // P5b: per-draw redundancy state (_dcOn = the n48m-drawopt switch as latched at device creation)
    uint32_t _clrm; NSUInteger _dvc;   // #12 D2: attachments whose load action was Clear and not yet consumed by a presentable draw; vertex/index count of the draw being prepared (diagnostic)
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d;
@end

@implementation N48RenderEncoder
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d {
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb; _wind = MTLWindingClockwise;
    _dcOn = N48DO.on; if (_dcOn) n48dc_enc_init(&_dc);
    VkImageView views[8]; uint32_t na = 0;
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        N48Texture *t = (N48Texture *)ca.texture;
        if (!t) continue;
        if (![t isKindOfClass:[N48Texture class]]) { N48LOG("render pass: attachment %lu is not an N48Texture", (unsigned long)i); return nil; }
        // m11h9: a mip level of a 2D texture can be the attachment (the compositor renders into the base level of mipmapped textures and generates the rest)
        NSUInteger lvl = ca.level;
        if (lvl >= [t mipmapLevelCount] || ca.slice != 0 || ca.depthPlane != 0) { N48LOG("render pass: attachment %lu level %lu slice %lu depthPlane %lu outside the texture (%lu level(s)); refused", (unsigned long)i, (unsigned long)lvl, (unsigned long)ca.slice, (unsigned long)ca.depthPlane, (unsigned long)[t mipmapLevelCount]); return nil; }
        if (na == 0) { _w = (uint32_t)MAX((NSUInteger)1, t.width >> lvl); _h = (uint32_t)MAX((NSUInteger)1, t.height >> lvl); }
        n48_ios_use(_cb, t, ca.loadAction != MTLLoadActionClear, YES);   // 11h.6: IOSurface contents are never discarded (only Clear skips the upload)
        [_cb n48DispNote:t bits:(N48DF_W_PASS | (ca.loadAction == MTLLoadActionClear ? N48DF_W_CLEAR : 0)) pso:nil];   // native #12 P1
        BOOL load = (ca.loadAction == MTLLoadActionLoad || ([t n48IsIOS] && ca.loadAction != MTLLoadActionClear)) && [t layout] != VK_IMAGE_LAYOUT_UNDEFINED;
        // The layout of a texture is tracked for the WHOLE image. A render pass only transitions its attachment's level, so a multi-level texture is moved to the attachment layout
        // explicitly (outside any pass: the previous encoder has ended) and the pass then starts from that layout.
        if ([t n48Levels] > 1) n48_tex_to([_cb vk], t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // storeOp is always STORE: a pass may be split (a texture bound mid-encoder needs a layout change outside the pass) and the
        // continuation LOADs what the first part stored.
        _ads[na] = (VkAttachmentDescription){ .format = [t vkFormat], .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = ca.loadAction == MTLLoadActionClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = ([t n48Levels] > 1) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : load ? [t layout] : VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        if (ca.loadAction == MTLLoadActionClear) _clrm |= 1u << na;
        views[na] = [t n48AttViewLevel:(uint32_t)lvl]; _views[na] = views[na]; _tex[na] = t; _sf.att[na] = t;
        MTLClearColor c = ca.clearColor;
        _cvs[na] = (VkClearValue){ .color = { .float32 = { (float)c.red, (float)c.green, (float)c.blue, (float)c.alpha } } };
        [t setLayout:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL];
        [_cb n48Retain:t];
        na++;
    }
    if (!na) { N48LOG("render pass: no colour attachments"); return nil; }
    _na = na;
    for (uint32_t i = 0; i < na; i++) _curLay[i] = _ads[i].initialLayout;
    _rp1 = n48_mk_rp_c(_ads, na, NO, &_rp1c); if (!_rp1) return nil;
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = _rp1, .attachmentCount = na, .pAttachments = views, .width = _w, .height = _h, .layers = 1 };
    VkResult r = vkCreateFramebuffer(N48R.dev, &fbc, NULL, &_fb);
    if (r != VK_SUCCESS) { N48LOG("render pass: vkCreateFramebuffer = %d", r); if (!_rp1c) vkDestroyRenderPass(N48R.dev, _rp1, NULL); return nil; }
    [_cb keepRenderPass:_rp1c ? VK_NULL_HANDLE : _rp1 framebuffer:_fb];
    // Metal's default viewport = the attachment, y down; translated shaders do not flip clip Y (F6): the viewport is emitted with a negative height.
    _vp = (MTLViewport){ 0, 0, _w, _h, 0, 1 }; _sr = (MTLScissorRect){ 0, 0, _w, _h };
    N48LOGR("N48RenderEncoder %p: %u x %u, %u attachment(s); the Vulkan render pass begins at the first draw (or at endEncoding)", (__bridge void *)self, _w, _h, na);
    return self;
}
N48_DNR(N48RenderEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 1; }

// ---- pass management ----
- (void)n48BeginPass {
    if (_inPass) return;
    if (_dcOn) n48dc_enc_reset_bound(&_dc);   // P5b: a new Vulkan render pass starts: forget the bound pipeline and the cached descriptor sets
    VkRenderPass rp = _rp1; VkFramebuffer fb = _fb; uint32_t ncv = _begun ? 0 : _na;
    BOOL rpc = _rp1c;
    if (_begun || _fetch) {   // continuation and/or framebuffer-fetch pass: built on demand from the attachments' CURRENT layouts (destroyed with the command buffer)
        VkAttachmentDescription a2[8];
        for (uint32_t i = 0; i < _na; i++) {
            a2[i] = _ads[i]; a2[i].initialLayout = _curLay[i];
            if (_begun) a2[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a2[i].finalLayout = _fetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        rp = n48_mk_rp_c(a2, _na, _fetch, &rpc);
        if (!rp) { [_cb n48Fail:@"cannot create the continuation / framebuffer-fetch render pass"]; return; }
        if (_fetch) {
            if (!_fbf) {   // the fetch framebuffer: same views, a fetch-shaped (input-attachment) render pass for compatibility
                VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = _na, .pAttachments = _views, .width = _w, .height = _h, .layers = 1 };
                VkResult r = vkCreateFramebuffer(N48R.dev, &fbc, NULL, &_fbf);
                if (r != VK_SUCCESS) { _fbf = VK_NULL_HANDLE; if (!rpc) vkDestroyRenderPass(N48R.dev, rp, NULL); [_cb n48Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (fetch) = %d", r]]; return; }
                [_cb keepRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:_fbf];
            } else [_cb keepRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:VK_NULL_HANDLE];
            fb = _fbf;
        } else [_cb keepRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:VK_NULL_HANDLE];
    }
    for (uint32_t i = 0; i < _na; i++) { _curLay[i] = _fetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; [_tex[i] setLayout:_curLay[i]]; }
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { _w, _h } }, .clearValueCount = ncv, .pClearValues = _cvs };
    vkCmdBeginRenderPass([_cb vk], &rbi, VK_SUBPASS_CONTENTS_INLINE);
    _inPass = YES; _begun = YES;
}
- (void)n48Need:(N48Texture *)t layout:(VkImageLayout)l {
    if ([t layout] == l) return;
    if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; N48LOGR("render pass split: texture %p needs layout %d (was %d)", (__bridge void *)t, l, [t layout]); }
    n48_tex_to([_cb vk], t, l);
}

// ---- state ----
- (void)setRenderPipelineState:(id)pso {
    if (![pso isKindOfClass:[N48RenderPipelineState class]]) { N48LOG("setRenderPipelineState: not an N48RenderPipelineState"); return; }
    _pso = pso; [_cb n48Retain:pso];
}
- (void)setCullMode:(MTLCullMode)m { _cull = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { _wind = w; }
- (void)setViewport:(MTLViewport)v { _vp = v; }
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n { if (n) _vp = v[0]; if (n > 1) N48LOG("setViewports: %lu viewports, only the first is used", (unsigned long)n); }
- (void)setScissorRect:(MTLScissorRect)r { _sr = r; }
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n { if (n) _sr = r[0]; if (n > 1) N48LOG("setScissorRects: %lu rects, only the first is used", (unsigned long)n); }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { _blend[0] = r; _blend[1] = g; _blend[2] = b; _blend[3] = a; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { if (m != MTLTriangleFillModeFill) N48LOG("setTriangleFillMode %lu: NOT IMPLEMENTED (fill)", (unsigned long)m); }
- (void)setDepthClipMode:(NSUInteger)m { (void)m; }
- (void)setDepthBias:(float)a slopeScale:(float)b clamp:(float)c { (void)a; (void)b; (void)c; }
- (void)setStencilReferenceValue:(uint32_t)v { (void)v; }
- (void)setDepthStencilState:(id)s { if (s) N48_ONCE("setDepthStencilState: ignored (render passes have no depth/stencil attachments)"); }
// ---- gap census: barriers, fences, residency, store actions (QuartzCore sends -memoryBarrierWithScope:afterStages:beforeStages: 6369 times in 45 s) ----
// A barrier between draws of one encoder = end the Vulkan render pass (the next draw starts a LOADing continuation pass; the pass-split machinery already
// exists for layout changes) plus a full memory barrier, which orders everything before it against everything after it.
- (void)n48Barrier:(const char *)what {
    if (_ended) return;
    if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; }
    n48_full_barrier([_cb vk]);
    N48_ONCE("render encoder %s: render pass split + full barrier", what);
}
- (void)memoryBarrierWithScope:(NSUInteger)sc afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)sc; (void)a; (void)b; [self n48Barrier:"memoryBarrierWithScope:"]; }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)r; (void)n; (void)a; (void)b; [self n48Barrier:"memoryBarrierWithResources:"]; }
- (void)textureBarrier { [self n48Barrier:"textureBarrier"]; }
- (void)updateFence:(id)f afterStages:(NSUInteger)st { (void)f; (void)st; N48_ONCE("render updateFence: ordered by submission order; ignored"); }
- (void)waitForFence:(id)f beforeStages:(NSUInteger)st { (void)f; (void)st; N48_ONCE("render waitForFence: ordered by submission order; ignored"); }
- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i { (void)a; (void)i; N48_ONCE("setColorStoreAction: ignored (colour attachments are always stored)"); }
- (void)setColorStoreActionOptions:(NSUInteger)a atIndex:(NSUInteger)i { (void)a; (void)i; }
- (void)setDepthStoreAction:(NSUInteger)a { (void)a; }
- (void)setDepthStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilStoreAction:(NSUInteger)a { (void)a; }
- (void)setStencilStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { (void)f; (void)b; }
N48_ENCODER_NOOPS
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

- (N48StageState *)n48Stage:(BOOL)frag { return frag ? &_sf : &_sv; }
- (void)n48SetBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31) { [_cb n48Fail:[NSString stringWithFormat:@"buffer index %lu >= 31", (unsigned long)i]]; return; }
    if (buf && ![buf isKindOfClass:[N48Buffer class]]) { [_cb n48Fail:@"setBuffer: not an N48Buffer"]; return; }
    N48StageState *s = [self n48Stage:f]; s->buf[i].b = buf; s->buf[i].off = off; [_cb n48Retain:buf];
}
- (void)n48SetBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31 || !p) { [_cb n48Fail:[NSString stringWithFormat:@"setBytes: index %lu / bytes %p", (unsigned long)i, p]]; return; }
    N48Buffer *b = nil; NSUInteger off = 0;
    if ([_cb n48Bytes:p length:n buffer:&b offset:&off]) [self n48SetBuffer:b offset:off index:i frag:f];
}
- (void)n48SetTexture:(id)t index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 128) { [_cb n48Fail:@"texture index >= 128"]; return; }
    if (t && ![t isKindOfClass:[N48Texture class]]) { [_cb n48Fail:@"setTexture: not an N48Texture"]; return; }
    [self n48Stage:f]->tex[i] = t; [_cb n48Retain:t];
}
- (void)n48SetSampler:(id)s index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 16) { [_cb n48Fail:@"sampler index >= 16"]; return; }
    if (s && ![s isKindOfClass:[N48SamplerState class]]) { [_cb n48Fail:@"setSamplerState: not an N48SamplerState"]; return; }
    [self n48Stage:f]->smp[i] = s; [_cb n48Retain:s];
}
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self n48SetBuffer:b offset:o index:i frag:NO]; }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self n48SetBuffer:b offset:o index:i frag:YES]; }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sv.buf[i].off = o; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sf.buf[i].off = o; }
- (void)setVertexBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetBuffer:b[k] offset:o[k] index:r.location + k frag:NO]; }
- (void)setFragmentBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetBuffer:b[k] offset:o[k] index:r.location + k frag:YES]; }
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self n48SetBytes:p length:n index:i frag:NO]; }
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self n48SetBytes:p length:n index:i frag:YES]; }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { [self n48SetTexture:t index:i frag:NO]; }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { [self n48SetTexture:t index:i frag:YES]; }
- (void)setVertexTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetTexture:t[k] index:r.location + k frag:NO]; }
- (void)setFragmentTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetTexture:t[k] index:r.location + k frag:YES]; }
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { [self n48SetSampler:s index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { [self n48SetSampler:s index:i frag:YES]; }
- (void)setVertexSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { (void)a; (void)b; [self n48SetSampler:s index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { (void)a; (void)b; [self n48SetSampler:s index:i frag:YES]; }
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetSampler:s[k] index:r.location + k frag:NO]; }
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self n48SetSampler:s[k] index:r.location + k frag:YES]; }

// ---- P5b helpers (only reached when the n48m-drawopt switch is ON) ----
- (void)n48NeedTex:(N48Texture *)t layout:(VkImageLayout)l {   // the body of needTex in n48PrepareDraw, verbatim
    if ([t n48IsIOS] && [_cb n48IOFirstTouch:t]) {
        if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; N48LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); }
        n48_ios_upload([_cb vk], t);
    }
    [_cb n48CBRead:t];
    if ([t n48DispImgOnly]) (void)[_cb n48IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
    if (([t n48IsIOS] || [t n48DispImgOnly]) && l == VK_IMAGE_LAYOUT_GENERAL) { [_cb n48IOWritten:t]; [_cb n48DispNote:t bits:N48DF_W_COMPUTE pso:nil]; }
    [self n48Need:t layout:l];
}
- (void)n48EachTex:(const N48PB *)pb n:(uint32_t)n state:(const N48StageState *)st dev:(id)dev {   // n48_each_tex, with the callback inlined
    for (uint32_t i = 0; i < n; i++) {
        if (pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil;
                if (!t) t = n48_dummy_texture(dev); if (t) [self n48NeedTex:t layout:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL]; }
        } else if (pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 480 + k; N48Texture *t = ix < 128 ? st->tex[ix] : nil; if (t) [self n48NeedTex:t layout:VK_IMAGE_LAYOUT_GENERAL]; }
        }
    }
}
// One stage (idx 0 = vertex set, 1 = fragment set): signature of exactly the slots n48FillSet reads for these bindings; equal to the last bound one = no new set, no vkCmdBindDescriptorSets.
// Decision (RADV, radv_cmd_buffer.c): radv_mark_descriptors_dirty() only does `descriptors_state->dirty |= descriptors_state->valid`: bound sets stay valid across vkCmdBindPipeline and are
// re-flushed by the next draw, so a skipped vkCmdBindDescriptorSets after a pipeline bind leaves RADV with valid state (and radv_bind_descriptor_sets itself skips an already-bound valid set).
- (BOOL)n48DcStage:(int)idx pb:(const N48PB *)pb n:(uint32_t)n state:(const N48StageState *)st dsl:(VkDescriptorSetLayout)dsl name:(const char *)nm cmd:(VkCommandBuffer)cmd {
    n48dc_sig sig; n48dc_sig_begin(&sig, _dc.layout, (uint64_t)(uintptr_t)dsl);
    for (uint32_t i = 0; i < n; i++) {
        const N48PB *p = &pb[i];
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding + k; N48BufSlot sl = ix < 31 ? st->buf[ix] : (N48BufSlot){ nil, 0 };
                n48dc_sig_push(&sig, sl.b ? (uint64_t)(uintptr_t)[sl.b vkBuffer] : 0); n48dc_sig_push(&sig, sl.b ? (uint64_t)sl.off : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - 32 + k; N48Texture *t = (p->binding >= 32 && ix < 128) ? st->tex[ix] : nil;
                n48dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); n48dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)[t vkView] : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            n48dc_sig_push(&sig, (uint64_t)(uintptr_t)p->stat);
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - 160 + k; N48SamplerState *sm = (p->binding >= 160 && ix < 16) ? st->smp[ix] : nil;
                n48dc_sig_push(&sig, sm ? (uint64_t)(uintptr_t)[sm vkSampler] : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            BOOL inp = p->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - (inp ? 192 : 480) + k; N48Texture *t = (p->binding >= (inp ? 192u : 480u) && ix < (inp ? 8u : 128u)) ? (inp ? st->att[ix] : st->tex[ix]) : nil;
                n48dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); n48dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)[t n48AttViewLevel:0] : 0); }
            break; }
        default: sig.over = 1; break;   // unsupported type: let n48FillSet refuse it as before
        }
        if (sig.over) break;
    }
    uint64_t set;
    if (n48dc_stage_match(&_dc.st[idx], &sig, &set)) { _dc.setsReused++; return YES; }   // same slots, same layout: the set bound at this index is already right
    VkDescriptorSet s = [_cb n48AllocSet:dsl]; if (!s || ![_cb n48FillSet:s bindings:pb count:n state:st stage:nm]) { n48dc_stage_reset(&_dc.st[idx]); return NO; }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], (uint32_t)idx, 1, &s, 0, NULL);
    n48dc_stage_store(&_dc.st[idx], &sig, (uint64_t)(uintptr_t)s); _dc.setsAllocated++;
    return YES;
}
// ---- draws ----
- (BOOL)n48PrepareDraw:(VkPrimitiveTopology)topo {
    if (_ended) { [_cb n48Fail:@"draw after endEncoding"]; return NO; }
    if ([_cb n48Error]) return NO;
    if (!_pso) { [_cb n48Fail:@"draw without a render pipeline state"]; return NO; }
    { N48RenderPipelineState *r = [_pso n48Real];   // hot-swap: switch to the real pipeline BETWEEN draws; every accessor below then reads one coherent object
      if (r) { _pso = r; [_cb n48Retain:r]; N48LOGR("HOT-SWAP: render encoder now uses the real pipeline %p", (__bridge void *)r); } }
    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // both are window-space (y down) senses
    VkPipeline vkp;
    if (_dcOn) {   // P5b item 3: the last (topology, cull, front, pso) -> VkPipeline answer skips pipelineForTopology's NSNumber / NSLock / NSDictionary
        uint64_t pp;
        if (n48dc_enc_pc_get(&_dc, (uint32_t)topo, (uint32_t)cull, (uint32_t)front, (__bridge const void *)_pso, &pp)) vkp = (VkPipeline)(uintptr_t)pp;
        else { vkp = [_pso pipelineForTopology:topo cull:cull front:front]; n48dc_enc_pc_put(&_dc, (uint32_t)topo, (uint32_t)cull, (uint32_t)front, (__bridge const void *)_pso, (uint64_t)(uintptr_t)vkp); }
    } else vkp = [_pso pipelineForTopology:topo cull:cull front:front];
    if (!vkp) { [_cb n48Fail:@"cannot build a pipeline variant for this draw"]; return NO; }
    const uint32_t *need = [_pso needBindings]; uint32_t nn = [_pso nNeed];
    for (uint32_t i = 0; i < nn; i++) if (need[i] != 31 && !_sv.buf[need[i]].b) {
        [_cb n48Fail:[NSString stringWithFormat:@"vertex buffer at index %u is read by the vertex descriptor but never bound", need[i]]]; return NO; }
    id dev = [_cb device];
    void (^needTex)(N48Texture *, VkImageLayout) = ^(N48Texture *t, VkImageLayout l) {
        if ([t n48IsIOS] && [self->_cb n48IOFirstTouch:t]) {   // 11h.6: first use in this command buffer: upload outside the render pass (splits it if one is open)
            if (self->_inPass) { vkCmdEndRenderPass([self->_cb vk]); self->_inPass = NO; N48LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); }
            n48_ios_upload([self->_cb vk], t);
        }
        [self->_cb n48CBRead:t];   // Stage 0b: sampled (or storage-accessed) by a render draw
        if ([t n48DispImgOnly]) (void)[self->_cb n48IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
        if (([t n48IsIOS] || [t n48DispImgOnly]) && l == VK_IMAGE_LAYOUT_GENERAL) { [self->_cb n48IOWritten:t]; [self->_cb n48DispNote:t bits:N48DF_W_COMPUTE pso:nil]; }   // a storage-image write from a render pass
        [self n48Need:t layout:l];
    };
    if (_dcOn) {   // P5b item 5: the same visit as n48_each_tex, as a plain loop (no heap-copied block)
        [self n48EachTex:[_pso pbV] n:[_pso npbV] state:&_sv dev:dev];
        [self n48EachTex:[_pso pbF] n:[_pso npbF] state:&_sf dev:dev];
    } else {
    n48_each_tex([_pso pbV], [_pso npbV], &_sv, dev, needTex);
    n48_each_tex([_pso pbF], [_pso npbF], &_sf, dev, needTex);
    }
    if ([_pso fetch] != _fetch) {   // 11e-2: framebuffer-fetch draws need the input-attachment render-pass shape; a change of shape is a pass split (LOAD continuation)
        if ([_pso fetch] && [_pso fetchMax] >= _na) { [_cb n48Fail:[NSString stringWithFormat:@"pipeline reads [[color(%u)]] but the encoder has %u colour attachment(s)", [_pso fetchMax], _na]]; return NO; }
        if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; N48LOGR("render pass split: framebuffer fetch %s", [_pso fetch] ? "on" : "off"); }
        _fetch = [_pso fetch];
    }
    [self n48BeginPass];
    if ([_cb n48Error]) return NO;
    {   // native #12 P1/D1: this draw writes a display surface; GPUPass = CoreDisplay's final display pass, ColorFill = a fill, anything else a SkyLight composite
        BOOL anyd = NO; for (uint32_t i = 0; i < _na; i++) if ([_tex[i] n48IsDisp]) anyd = YES;
        if (anyd) {
            N48Texture *src = nil;   // the first IOSurface texture this draw samples in the fragment stage
            const N48PB *pbf = [_pso pbF]; uint32_t npf = [_pso npbF];
            for (uint32_t i = 0; i < npf && !src; i++) if (pbf[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && pbf[i].binding >= 32 && pbf[i].binding < 160)
                for (uint32_t k = 0; k < pbf[i].count && !src; k++) { uint32_t ix = pbf[i].binding - 32 + k; N48Texture *tt = ix < 128 ? _sf.tex[ix] : nil; if (tt && [tt n48IsIOS]) src = tt; }
            uint32_t bits = n48df_draw_bits([_pso n48FName]);
            n48df_rect_t rc = n48df_draw_rect(_vp.originX, _vp.originY, _vp.width, _vp.height, (int64_t)_sr.x, (int64_t)_sr.y, (int64_t)_sr.width, (int64_t)_sr.height, _w, _h);
            for (uint32_t i = 0; i < _na; i++) if ([_tex[i] n48IsDisp]) {
                BOOL clr = (_clrm >> i) & 1u; if (clr && (bits & (N48DF_W_DRAW_FINAL | N48DF_W_DRAW_COMP))) _clrm &= ~(1u << i); else clr = NO;
                [_cb n48DispNote:_tex[i] bits:bits pso:_pso];
                [_cb n48DispDraw:_tex[i] bits:bits rect:rc w:_w h:_h clear:clr src:src vp:_vp sc:_sr];
            }
            static _Atomic int vlog;   // D1 sample of the first 30 display draws: viewport, scissor, vertex count, and the first 32 bytes of the first bound vertex-stage buffer (the extents may live in vertex data)
            if (atomic_load(&vlog) < 30 && atomic_fetch_add(&vlog, 1) < 30) {
                const uint8_t *vb = NULL; uint32_t vbi = 0;
                for (uint32_t i = 0; i < 31 && !vb; i++) if (_sv.buf[i].b) { const uint8_t *c = (const uint8_t *)[_sv.buf[i].b contents]; if (c) { vb = c + _sv.buf[i].off; vbi = i; } }
                char hx[200]; hx[0] = 0; if (vb) for (int k = 0; k < 8; k++) { float f; uint32_t u; memcpy(&u, vb + 4 * k, 4); memcpy(&f, &u, 4); size_t l = strlen(hx); snprintf(hx + l, sizeof hx - l, "%s%08x(%g)", k ? " " : "", u, (double)f); }
                N48LOG("scanout: DRAW sample #%d: %s/%s count %lu viewport %.1f,%.1f %.1fx%.1f scissor %lu,%lu %lux%lu -> rect [%d,%d)-[%d,%d); source texture: %s id %u %lux%lu; vertex buffer %u words: %s",
                    atomic_load(&vlog), [_pso n48VName], [_pso n48FName], (unsigned long)_dvc, _vp.originX, _vp.originY, _vp.width, _vp.height, (unsigned long)_sr.x, (unsigned long)_sr.y, (unsigned long)_sr.width, (unsigned long)_sr.height,
                    rc.x0, rc.y0, rc.x1, rc.y1, src ? ([src n48IsDisp] ? "display surface" : "IOSurface") : "none", src && [src n48IOSRef] ? IOSurfaceGetID([src n48IOSRef]) : 0, src ? (unsigned long)src.width : 0UL, src ? (unsigned long)src.height : 0UL, vbi, vb ? hx : "(none or private)");
            }
        }
    }
    VkCommandBuffer cmd = [_cb vk];
    if (_fetch) {   // earlier draws' colour writes must be visible to this draw's input-attachment reads (self-dependency of the pass)
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    }
    if (_dcOn) {   // P5b item 2: RADV's radv_CmdBindPipeline re-marks descriptors dirty and re-binds dynamic state even for the same pipeline; viewport / scissor / blend are set below every draw anyway
        _dc.draws++;
        if (!n48dc_enc_pipe_same(&_dc, (uint64_t)(uintptr_t)vkp)) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    } else
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    VkViewport vp = { (float)_vp.originX, (float)(_vp.originY + _vp.height), (float)_vp.width, -(float)_vp.height, (float)_vp.znear, (float)_vp.zfar };
    VkRect2D sc = { { (int32_t)_sr.x, (int32_t)_sr.y }, { (uint32_t)_sr.width, (uint32_t)_sr.height } };
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc); vkCmdSetBlendConstants(cmd, _blend);
    for (uint32_t i = 0; i < nn; i++) {
        N48Buffer *b; NSUInteger off;
        if (need[i] == 31) { b = n48_dummy_buffer(dev); off = 0; if (!b) { [_cb n48Fail:@"no dummy buffer"]; return NO; } [_cb n48Retain:b]; }
        else { b = _sv.buf[need[i]].b; off = _sv.buf[need[i]].off; }
        VkBuffer vb = [b vkBuffer]; VkDeviceSize vo = off;
        vkCmdBindVertexBuffers(cmd, need[i], 1, &vb, &vo);
    }
    if (_dcOn) {   // P5b item 1: re-use the stage's descriptor set while the slots its layout reads are unchanged
        n48dc_enc_layout(&_dc, (uint64_t)(uintptr_t)[_pso layout]);
        if ([_pso npbV] && ![self n48DcStage:0 pb:[_pso pbV] n:[_pso npbV] state:&_sv dsl:[_pso dsl:0] name:"vertex" cmd:cmd]) return NO;
        if ([_pso npbF] && ![self n48DcStage:1 pb:[_pso pbF] n:[_pso npbF] state:&_sf dsl:[_pso dsl:1] name:"fragment" cmd:cmd]) return NO;
        return YES;
    }
    if ([_pso npbV]) {
        VkDescriptorSet s = [_cb n48AllocSet:[_pso dsl:0]]; if (!s || ![_cb n48FillSet:s bindings:[_pso pbV] count:[_pso npbV] state:&_sv stage:"vertex"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 0, 1, &s, 0, NULL);
    }
    if ([_pso npbF]) {
        VkDescriptorSet s = [_cb n48AllocSet:[_pso dsl:1]]; if (!s || ![_cb n48FillSet:s bindings:[_pso pbF] count:[_pso npbF] state:&_sf stage:"fragment"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 1, 1, &s, 0, NULL);
    }
    return YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi {
    _dvc = c * (ic ? ic : 1);
    if ([self n48PrepareDraw:n48_topo(t)]) vkCmdDraw([_cb vk], (uint32_t)c, (uint32_t)ic, (uint32_t)s, (uint32_t)bi);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:1 baseInstance:0]; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:ic baseInstance:0]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    if (![ib isKindOfClass:[N48Buffer class]]) { [_cb n48Fail:@"drawIndexedPrimitives: index buffer is not an N48Buffer"]; return; }
    NSUInteger isz = it == MTLIndexTypeUInt32 ? 4 : 2;
    if (off % isz) { [_cb n48Fail:@"index buffer offset is not a multiple of the index size"]; return; }
    _dvc = c * (ic ? ic : 1);
    if (![self n48PrepareDraw:n48_topo(t)]) return;
    [_cb n48Retain:ib];
    vkCmdBindIndexBuffer([_cb vk], [(N48Buffer *)ib vkBuffer], off, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed([_cb vk], (uint32_t)c, (uint32_t)ic, 0, (int32_t)bv, (uint32_t)bi);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:ic baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)endEncoding {
    if (_ended) return;
    if (!_begun && ![_cb n48Error]) [self n48BeginPass];   // a clear-only encoder still has to run its load action
    _ended = YES;
    if (_dcOn) {   // P5b: fold this encoder's counters into the device totals
        atomic_fetch_add(&N48DO.draws, _dc.draws); atomic_fetch_add(&N48DO.setsReused, _dc.setsReused); atomic_fetch_add(&N48DO.setsAlloc, _dc.setsAllocated);
        atomic_fetch_add(&N48DO.pipesSkipped, _dc.pipesSkipped); atomic_fetch_add(&N48DO.pipesBound, _dc.pipesBound); atomic_fetch_add(&N48DO.pcHits, _dc.pcHits);
    }
    if (_inPass) { vkCmdEndRenderPass([_cb vk]); _inPass = NO; }
    n48_full_barrier([_cb vk]);
    N48LOGR("N48RenderEncoder endEncoding (render pass ended)");
    [super endEncoding];
}
@end

// ---------------------------------------------------------------------------------------------------------------
// N48ComputeEncoder (11e R2)
// ---------------------------------------------------------------------------------------------------------------
@interface N48ComputeEncoder : _MTLCommandEncoder { N48CommandBuffer *_cb; BOOL _ended; NSString *_lbl; N48ComputePipelineState *_pso; N48StageState _st; }
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy;
@end

@implementation N48ComputeEncoder
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb]; if (!self) return nil;
    _cb = cb; N48LOGR("N48ComputeEncoder %p", (__bridge void *)self);
    return self;
}
N48_DNR(N48ComputeEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 2; }
- (NSUInteger)dispatchType { return 0; }
- (void)setComputePipelineState:(id)p {
    if (![p isKindOfClass:[N48ComputePipelineState class]]) { [_cb n48Fail:@"setComputePipelineState: not an N48ComputePipelineState"]; return; }
    _pso = p; [_cb n48Retain:p];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i >= 31 || (b && ![b isKindOfClass:[N48Buffer class]])) { [_cb n48Fail:@"compute setBuffer: bad index or class"]; return; }
    _st.buf[i].b = b; _st.buf[i].off = o; [_cb n48Retain:b];
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _st.buf[i].off = o; }
- (void)setBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:b[k] offset:o[k] atIndex:r.location + k]; }
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {
    if (i >= 31 || !p) { [_cb n48Fail:@"compute setBytes: bad index/bytes"]; return; }
    N48Buffer *b = nil; NSUInteger off = 0;
    if ([_cb n48Bytes:p length:n buffer:&b offset:&off]) [self setBuffer:b offset:off atIndex:i];
}
- (void)setTexture:(id)t atIndex:(NSUInteger)i {
    if (i >= 128 || (t && ![t isKindOfClass:[N48Texture class]])) { [_cb n48Fail:@"compute setTexture: bad index or class"]; return; }
    _st.tex[i] = t; [_cb n48Retain:t];
}
- (void)setTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setTexture:t[k] atIndex:r.location + k]; }
- (void)setSamplerState:(id)s atIndex:(NSUInteger)i {
    if (i >= 16 || (s && ![s isKindOfClass:[N48SamplerState class]])) { [_cb n48Fail:@"compute setSamplerState: bad index or class"]; return; }
    _st.smp[i] = s; [_cb n48Retain:s];
}
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { (void)a; (void)b; [self setSamplerState:s atIndex:i]; }
- (void)setSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:s[k] atIndex:r.location + k]; }
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i { N48LOG("compute setThreadgroupMemoryLength %lu at %lu: threadgroup buffers have no descriptor (ignored)", (unsigned long)n, (unsigned long)i); }
- (void)memoryBarrierWithScope:(NSUInteger)s { (void)s; n48_full_barrier([_cb vk]); }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n { (void)r; (void)n; n48_full_barrier([_cb vk]); }
- (void)updateFence:(id)f { (void)f; N48_ONCE("compute updateFence: ordered by submission order; ignored"); }
- (void)waitForFence:(id)f { (void)f; N48_ONCE("compute waitForFence: ordered by submission order; ignored"); }
N48_ENCODER_NOOPS
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

// grid: threads (dispatchThreads) or groups*tpt (dispatchThreadgroups). The Metal call's threadsPerThreadgroup must equal the local size the
// module was translated with (meta local_size); otherwise the command buffer completes with an NSError instead of dispatching.
- (void)n48Dispatch:(MTLSize)grid tpt:(MTLSize)tpt exactThreads:(BOOL)exact {
    if (_ended) { [_cb n48Fail:@"dispatch after endEncoding"]; return; }
    if ([_cb n48Error]) return;
    if (!_pso) { [_cb n48Fail:@"dispatch without a compute pipeline state"]; return; }
    { N48ComputePipelineState *r = [_pso n48Real];   // hot-swap (see the render encoder)
      if (r) { _pso = r; [_cb n48Retain:r]; N48LOGR("HOT-SWAP: compute encoder now uses the real pipeline %p", (__bridge void *)r); } }
    if ([_pso noop]) { N48LOGR("COMPUTE FALLBACK: dispatch %lux%lux%lu (tpt %lux%lux%lu) skipped, placeholder pipeline encodes nothing", (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth, (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth); return; }
    uint32_t nom[3] = { [_pso nominalLocal][0], [_pso nominalLocal][1], [_pso nominalLocal][2] };
    if ([_pso mode] == 1) {
        // #12 R2: a ThreadsDynamic module declares its local size through spec ids 0..2 (checked: every kernel in spvcache), so the
        // dispatch's own threadsPerThreadgroup is the nominal local size; a pipeline variant per (x,y,z) is built lazily and cached.
        const VkPhysicalDeviceLimits *lm = &N48R.lim;
        if (!tpt.width || !tpt.height || !tpt.depth || tpt.width > lm->maxComputeWorkGroupSize[0] || tpt.height > lm->maxComputeWorkGroupSize[1] || tpt.depth > lm->maxComputeWorkGroupSize[2]
            || tpt.width * tpt.height * tpt.depth > lm->maxComputeWorkGroupInvocations) {
            [_cb n48Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu is zero or outside the RADV limits (per axis %u,%u,%u; invocations %u)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth,
                          lm->maxComputeWorkGroupSize[0], lm->maxComputeWorkGroupSize[1], lm->maxComputeWorkGroupSize[2], lm->maxComputeWorkGroupInvocations]]; return; }
        if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2])
            N48LOGR("compute dispatch: threadsPerThreadgroup %lux%lux%lu overrides the translated local size %ux%ux%u (spec ids 0..2)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]);
        nom[0] = (uint32_t)tpt.width; nom[1] = (uint32_t)tpt.height; nom[2] = (uint32_t)tpt.depth;
    } else if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2]) {
        [_cb n48Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu != the SPIR-V local size %ux%ux%u the kernel was translated with (metal2vulkan --local; this module is Workgroups, local size is not specialisable)",
                      (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]]]; return; }
    uint32_t g[3] = { (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth };
    if (!g[0] || !g[1] || !g[2]) return;
    id dev = [_cb device];
    n48_each_tex([_pso pb], [_pso npb], &_st, dev, ^(N48Texture *t, VkImageLayout l) { n48_ios_usek(self->_cb, t, YES, l == VK_IMAGE_LAYOUT_GENERAL, N48DF_W_COMPUTE); n48_tex_to([self->_cb vk], t, l); });
    VkCommandBuffer cmd = [_cb vk];
    VkDescriptorSet s = VK_NULL_HANDLE;
    if ([_pso npb]) { s = [_cb n48AllocSet:[_pso dsl]]; if (!s || ![_cb n48FillSet:s bindings:[_pso pb] count:[_pso npb] state:&_st stage:"compute"]) return; }
    if ([_pso mode] == 0) {   // Workgroups: one pipeline, whole groups only
        if (exact && (g[0] % nom[0] || g[1] % nom[1] || g[2] % nom[2])) { [_cb n48Fail:@"dispatchThreads with a partial boundary threadgroup needs a ThreadsDynamic translation (this module is Workgroups)"]; return; }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso pipelineForLocal:nom]);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        vkCmdDispatch(cmd, exact ? g[0] / nom[0] : g[0], exact ? g[1] / nom[1] : g[1], exact ? g[2] / nom[2] : g[2]);
    } else {                  // ThreadsDynamic: the metal2vulkan region plan, one dispatch per region
        N48Region reg[8]; uint32_t threads[3], tgpg[3];
        for (int d = 0; d < 3; d++) { threads[d] = exact ? g[d] : g[d] * nom[d]; tgpg[d] = (threads[d] + nom[d] - 1) / nom[d]; }
        int nr = n48_plan(threads, nom, reg);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        for (int i = 0; i < nr; i++) {
            VkPipeline p = [_pso pipelineForLocal:reg[i].local]; if (!p) { [_cb n48Fail:@"cannot build a compute pipeline variant"]; return; }
            uint32_t words[12] = { threads[0], threads[1], threads[2], reg[i].tbase[0], reg[i].tbase[1], reg[i].tbase[2], reg[i].gbase[0], reg[i].gbase[1], reg[i].gbase[2], tgpg[0], tgpg[1], tgpg[2] };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdPushConstants(cmd, [_pso layout], VK_SHADER_STAGE_COMPUTE_BIT, [_pso pcOffset], sizeof words, words);
            vkCmdDispatch(cmd, reg[i].groups[0], reg[i].groups[1], reg[i].groups[2]);
        }
        N48LOGR("compute dispatch: threads %u,%u,%u local %u,%u,%u -> %d region(s)", threads[0], threads[1], threads[2], nom[0], nom[1], nom[2], nr);
    }
    n48_full_barrier(cmd);
}
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self n48Dispatch:g tpt:t exactThreads:NO]; }
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self n48Dispatch:g tpt:t exactThreads:YES]; }
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    n48_full_barrier([_cb vk]);
    N48LOGR("N48ComputeEncoder endEncoding");
    [super endEncoding];
}
@end

@interface N48BlitEncoder : _MTLCommandEncoder { N48CommandBuffer *_cb; BOOL _ended; NSString *_lbl; }
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy;
@end

// m11h9: one buffer<->image copy region for a level / slice (2D, 1D arrays) or a box (3D: baseArrayLayer 0, depth in the extent, bufferImageHeight from bytesPerImage).
static VkBufferImageCopy n48_bic(N48Texture *t, NSUInteger off, NSUInteger bpr, NSUInteger bpi, NSUInteger level, NSUInteger slice, MTLOrigin o, MTLSize sz) {
    BOOL is3 = [t textureType] == MTLTextureType3D; uint32_t bpp = [t bytesPerPixel];
    return (VkBufferImageCopy){ .bufferOffset = off, .bufferRowLength = (uint32_t)(bpr / bpp), .bufferImageHeight = (is3 && sz.depth > 1 && bpr) ? (uint32_t)(bpi / bpr) : 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, is3 ? 0 : (uint32_t)slice, 1 },
        .imageOffset = { (int32_t)o.x, (int32_t)o.y, is3 ? (int32_t)o.z : 0 }, .imageExtent = { (uint32_t)sz.width, (uint32_t)sz.height, is3 ? (uint32_t)sz.depth : 1 } };
}
static BOOL n48_blit_region_ok(N48Texture *t, MTLOrigin o, MTLSize sz, NSUInteger level, NSUInteger slice, const char *what) {
    return n48_region_ok(t, (MTLRegion){ o, sz }, level, slice, what);
}
@implementation N48BlitEncoder
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    N48LOGR("N48BlitEncoder %p", (__bridge void *)self);
    return self;
}
N48_DNR(N48BlitEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 3; }
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    N48LOGR("blit copyFromTexture toBuffer: slice %lu level %lu origin %lu,%lu,%lu size %lux%lux%lu offset %lu bpr %lu bpi %lu", (unsigned long)ss, (unsigned long)sl,
           (unsigned long)so.x, (unsigned long)so.y, (unsigned long)so.z, (unsigned long)sz.width, (unsigned long)sz.height, (unsigned long)sz.depth, (unsigned long)doff, (unsigned long)bpr, (unsigned long)bpi);
    if (![src isKindOfClass:[N48Texture class]] || ![dst isKindOfClass:[N48Buffer class]]) { [_cb n48Fail:@"blit copyFromTexture:toBuffer: bad classes"]; return; }
    N48Texture *t = src; N48Buffer *b = dst;
    [_cb n48Retain:t]; [_cb n48Retain:b];
    if (!n48_blit_region_ok(t, so, sz, sl, ss, "blit copyFromTexture:toBuffer:")) { [_cb n48Fail:@"blit copyFromTexture:toBuffer: region outside the texture level"]; return; }
    n48_ios_use(_cb, t, YES, NO);
    VkCommandBuffer cmd = [_cb vk];
    n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = n48_bic(t, doff, bpr, bpi, sl, ss, so, sz);
    vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [b vkBuffer], 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (![src isKindOfClass:[N48Buffer class]] || ![dst isKindOfClass:[N48Texture class]]) { [_cb n48Fail:@"blit copyFromBuffer:toTexture: bad classes"]; return; }
    N48Texture *t = dst; [_cb n48Retain:src]; [_cb n48Retain:t];
    if (bpr % [t bytesPerPixel]) { [_cb n48Fail:@"blit copyFromBuffer:toTexture: bytesPerRow is not a multiple of the pixel size"]; return; }
    if (!n48_blit_region_ok(t, dorg, sz, dl, ds, "blit copyFromBuffer:toTexture:")) { [_cb n48Fail:@"blit copyFromBuffer:toTexture: region outside the texture level"]; return; }
    n48_ios_usek(_cb, t, YES, YES, N48DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = n48_bic(t, so, bpr, bpi, dl, ds, dorg, sz);
    vkCmdCopyBufferToImage(cmd, [(N48Buffer *)src vkBuffer], [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    n48_full_barrier(cmd);
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (![src isKindOfClass:[N48Texture class]] || ![dst isKindOfClass:[N48Texture class]]) { [_cb n48Fail:@"blit copyFromTexture:toTexture: bad classes"]; return; }
    N48Texture *a = src, *b = dst; [_cb n48Retain:a]; [_cb n48Retain:b];
    if (!n48_blit_region_ok(a, so, sz, sl, ss, "blit copyFromTexture:toTexture: (source)") || !n48_blit_region_ok(b, dorg, sz, dl, ds, "blit copyFromTexture:toTexture: (destination)")) {
        [_cb n48Fail:@"blit copyFromTexture:toTexture: region outside a texture level"]; return; }
    n48_ios_use(_cb, a, YES, NO); n48_ios_usek(_cb, b, YES, YES, N48DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    BOOL a3 = [a textureType] == MTLTextureType3D, b3 = [b textureType] == MTLTextureType3D;
    n48_tex_to(cmd, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    n48_tex_to(cmd, b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)sl, a3 ? 0 : (uint32_t)ss, 1 }, .srcOffset = { (int32_t)so.x, (int32_t)so.y, a3 ? (int32_t)so.z : 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)dl, b3 ? 0 : (uint32_t)ds, 1 }, .dstOffset = { (int32_t)dorg.x, (int32_t)dorg.y, b3 ? (int32_t)dorg.z : 0 },
        .extent = { (uint32_t)sz.width, (uint32_t)sz.height, (a3 || b3) ? (uint32_t)sz.depth : 1 } };
    vkCmdCopyImage(cmd, [a vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [b vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    n48_full_barrier(cmd);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    if (![src isKindOfClass:[N48Buffer class]] || ![dst isKindOfClass:[N48Buffer class]]) { [_cb n48Fail:@"blit copyFromBuffer:toBuffer: bad classes"]; return; }
    [_cb n48Retain:src]; [_cb n48Retain:dst];
    VkBufferCopy bc = { so, doff, size };
    vkCmdCopyBuffer([_cb vk], [(N48Buffer *)src vkBuffer], [(N48Buffer *)dst vkBuffer], 1, &bc);
    n48_full_barrier([_cb vk]);
}
- (void)fillBuffer:(id)buf range:(NSRange)r value:(uint8_t)v {
    if (![buf isKindOfClass:[N48Buffer class]]) { [_cb n48Fail:@"blit fillBuffer: bad class"]; return; }
    [_cb n48Retain:buf];
    uint32_t w = v * 0x01010101u;
    vkCmdFillBuffer([_cb vk], [(N48Buffer *)buf vkBuffer], r.location, r.length, w);
    n48_full_barrier([_cb vk]);
}
- (void)updateFence:(id)f { (void)f; N48_ONCE("blit updateFence: ordered by submission order; ignored"); }
- (void)waitForFence:(id)f { (void)f; N48_ONCE("blit waitForFence: ordered by submission order; ignored"); }
N48_ENCODER_NOOPS
// m11h9: a box-filtered chain, level L-1 -> L with vkCmdBlitImage (linear filter when RADV can filter the format, else nearest) inside one GENERAL-layout image (source and
// destination are different levels, so the regions never overlap); each blit is ordered after the previous by a full memory barrier.
- (void)generateMipmapsForTexture:(id)tex {
    if (![tex isKindOfClass:[N48Texture class]]) { [_cb n48Fail:@"blit generateMipmapsForTexture: bad class"]; return; }
    N48Texture *t = tex; uint32_t nl = [t n48Levels];
    if (nl <= 1) { N48_ONCE("generateMipmapsForTexture: the texture has a single level; nothing to generate"); return; }
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(N48R.pd, [t vkFormat], &fp);
    if ((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) != (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        [_cb n48Fail:[NSString stringWithFormat:@"generateMipmapsForTexture: vk format %d cannot be blitted on RADV (features 0x%x)", [t vkFormat], fp.optimalTilingFeatures]]; return; }
    VkFilter fl = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    [_cb n48Retain:t];
    VkCommandBuffer cmd = [_cb vk];
    n48_tex_to(cmd, t, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t L = 1; L < nl; L++) {
        n48_full_barrier(cmd);
        int32_t sw = (int32_t)MAX((NSUInteger)1, [t width] >> (L - 1)), sh = (int32_t)MAX((NSUInteger)1, [t height] >> (L - 1)), sd = (int32_t)MAX((NSUInteger)1, [t depth] >> (L - 1));
        int32_t dw = (int32_t)MAX((NSUInteger)1, [t width] >> L), dh = (int32_t)MAX((NSUInteger)1, [t height] >> L), dd = (int32_t)MAX((NSUInteger)1, [t depth] >> L);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, L - 1, 0, [t n48Layers] }, .srcOffsets = { { 0, 0, 0 }, { sw, sh, sd } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, L, 0, [t n48Layers] }, .dstOffsets = { { 0, 0, 0 }, { dw, dh, dd } } };
        vkCmdBlitImage(cmd, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, fl);
    }
    n48_full_barrier(cmd);
    N48LOG("blit generateMipmapsForTexture: %u levels of %lux%lux%lu (vk format %d, %s filter)", nl, (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t depth], [t vkFormat], fl == VK_FILTER_LINEAR ? "linear" : "nearest");
}
- (void)copyFromTexture:(id)src toTexture:(id)dst {
    if (![src isKindOfClass:[N48Texture class]]) { [_cb n48Fail:@"blit copyFromTexture:toTexture: bad source class"]; return; }
    [self copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake([(N48Texture *)src width], [(N48Texture *)src height], [(N48Texture *)src depth])
                toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}
// the MTLBlitOption variants: option 0 (none) = the plain call; PVRTC row-linear options are not supported
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)o {
    if (o) { [_cb n48Fail:@"blit copyFromTexture:toBuffer:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toBuffer:dst destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb n48Fail:@"blit copyFromBuffer:toTexture:options: blit options are not supported"]; return; }
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb n48Fail:@"blit copyFromTexture:toTexture:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)synchronizeResource:(id)r { (void)r; }   // Managed resources: host-visible coherent memory, nothing to flush
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    n48_full_barrier([_cb vk]);
    N48LOG("N48BlitEncoder endEncoding");
    [super endEncoding];
}
@end

@implementation N48CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    N48LOGR("cb renderCommandEncoderWithDescriptor:");
    return [[N48RenderEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)blitCommandEncoder {
    N48LOGR("cb blitCommandEncoder");
    return [[N48BlitEncoder alloc] initWithCommandBuffer:self blit:0];
}
- (id)blitCommandEncoderWithDescriptor:(id)d { (void)d; return [self blitCommandEncoder]; }
- (id)computeCommandEncoder { N48LOGR("cb computeCommandEncoder"); return [[N48ComputeEncoder alloc] initWithCommandBuffer:self compute:0]; }
- (id)computeCommandEncoderWithDescriptor:(id)d { (void)d; return [self computeCommandEncoder]; }
- (id)computeCommandEncoderWithDispatchType:(NSUInteger)t { (void)t; return [self computeCommandEncoder]; }
@end

// MTLGPUFamily values (public enum).
enum {
    N48_Mac1 = 2001, N48_Mac2 = 2002,
    N48_Common1 = 3001, N48_Common2 = 3002, N48_Common3 = 3003,
    N48_Metal3 = 5001,
};

// Conservative limits, from our RADV gfx1201 vulkaninfo (~/navi48-native/mesa-mac-out/vulkaninfo-full.txt):
// device-local heap 16911433728 B (15.75 GiB); maxStorageBufferRange 4294967295.
#define N48_MAX_BUFFER_LENGTH        (0x100000000ULL)   /* 4 GiB */
#define N48_RECOMMENDED_WORKING_SET  (0x300000000ULL)   /* 12 GiB (< 15.75 GiB VRAM) */

@implementation Navi48Device

// S5.2a: scanout stats / release for `mtlprobe dispflip` (root test path; absent on every other Metal device).
- (NSDictionary *)n48ScanoutStats { return n48s_stats(); }
- (NSDictionary *)n48ScanoutRelease { return n48s_release_request(); }

// MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] (Metal line 1054; seen on the PC
//. Apple's AppleParavirtDevice declares MTLDeviceSPI + MTLDevice + NSObject in its class
// protocol list (pvm.x86 baseProtocols); MTLIOAccelDevice declares none. Add the same conformance at load.
+ (void)load {
    const char *names[] = { "MTLDeviceSPI", "MTLDevice" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Protocol *p = objc_getProtocol(names[i]);
        BOOL added = p ? class_addProtocol(self, p) : NO;
        os_log(OS_LOG_DEFAULT, "Navi48Metal: +load protocol %{public}s found %d added %d", names[i], p != nil, added);
    }
    const char *bnames[] = { "MTLBuffer", "MTLResource" };   // N48Buffer conformance (no compile-time adoption)
    for (unsigned i = 0; i < sizeof(bnames) / sizeof(bnames[0]); i++) {
        Protocol *p = objc_getProtocol(bnames[i]);
        BOOL added = p ? class_addProtocol([N48Buffer class], p) : NO;
        os_log(OS_LOG_DEFAULT, "Navi48Metal: +load N48Buffer protocol %{public}s found %d added %d", bnames[i], p != nil, added);
    }
    { const char *n[] = { "MTLTexture", "MTLResource" }; n48_add_protocols([N48Texture class], n, 2, "N48Texture"); }
    { const char *n[] = { "MTLRenderPipelineState" }; n48_add_protocols([N48RenderPipelineState class], n, 1, "N48RenderPipelineState"); }
    { const char *n[] = { "MTLCommandQueue" }; n48_add_protocols([N48CommandQueue class], n, 1, "N48CommandQueue"); }
    { const char *n[] = { "MTLCommandBuffer" }; n48_add_protocols([N48CommandBuffer class], n, 1, "N48CommandBuffer"); }
    { const char *n[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder" }; n48_add_protocols([N48RenderEncoder class], n, 2, "N48RenderEncoder"); }
    { const char *n[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder" }; n48_add_protocols([N48BlitEncoder class], n, 2, "N48BlitEncoder"); }
    { const char *n[] = { "MTLComputeCommandEncoder", "MTLCommandEncoder" }; n48_add_protocols([N48ComputeEncoder class], n, 2, "N48ComputeEncoder"); }
    { const char *n[] = { "MTLComputePipelineState" }; n48_add_protocols([N48ComputePipelineState class], n, 1, "N48ComputePipelineState"); }
    { const char *n[] = { "MTLSamplerState" }; n48_add_protocols([N48SamplerState class], n, 1, "N48SamplerState"); }
    { const char *n[] = { "MTLDepthStencilState" }; n48_add_protocols([N48DepthStencilState class], n, 1, "N48DepthStencilState"); }
    { const char *n[] = { "MTLFence" }; n48_add_protocols([N48Fence class], n, 1, "N48Fence"); }
    { const char *n[] = { "MTLHeap" }; n48_add_protocols([N48Heap class], n, 1, "N48Heap"); }
}

// lazyInitialize: log, then run the base (only if some superclass implements it).
- (void)lazyInitialize {
    Class sup = class_getSuperclass([Navi48Device class]);
    BOOL has = class_getInstanceMethod(sup, @selector(lazyInitialize)) != NULL;
    N48LOG("lazyInitialize (super implements: %d)", has);
    if (has) [super lazyInitialize];
}

// ---- 10a: buffers ----
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    N48LOG("newBufferWithLength:%lu options:0x%lx", (unsigned long)length, (unsigned long)options);
    if (length == 0 || length > N48_MAX_BUFFER_LENGTH) { N48LOG("newBuffer: refused length"); return nil; }
    if (((options >> 4) & 0xF) > 2) {   // 11e: Shared, Managed, Private
        N48LOG("newBuffer: storage mode %lu not supported (Memoryless)", (unsigned long)((options >> 4) & 0xF));
        return nil;
    }
    NSError *err = nil;
    N48Buffer *b = [[N48Buffer alloc] initWithDevice:self length:length options:options error:&err];
    if (!b) N48LOG("newBuffer: failed: %s", err.localizedDescription.UTF8String);
    return b;
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    N48LOG("newBufferWithBytes:length:%lu options:0x%lx", (unsigned long)length, (unsigned long)options);
    N48Buffer *b = [self newBufferWithLength:length options:options];
    if (b && [b contents] && bytes) memcpy([b contents], bytes, length);
    else if (b && bytes) N48LOG("newBufferWithBytes: buffer has no CPU mapping (Private): contents NOT copied");
    return b;
}

// ---- 11e: samplers ----
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
    N48LOG("newSamplerStateWithDescriptor:");
    NSError *err = nil;
    N48SamplerState *s = [[N48SamplerState alloc] initWithDevice:self descriptor:d error:&err];
    if (!s) N48LOG("newSamplerState: failed: %s", err.localizedDescription.UTF8String);
    return s;
}

// ---- 11e R2: compute pipelines from the SPIR-V cache (a miss dumps the AIR; in WindowServer it returns a no-op placeholder (#12 R1), elsewhere an NSError) ----
- (id)n48NewComputePipeline:(id)fn error:(NSError **)error {
    NSError *e = nil;
    N48ComputePipelineState *p = [[N48ComputePipelineState alloc] initWithDevice:self function:fn error:&e];
    if (p) return p;
    N48LOG("compute pipeline creation failed: %s", e.localizedDescription.UTF8String);
    if (e.code == 41 && n48_fallback_ok()) {
        // #12 R1: WindowServer (or N48M_FORCE_FALLBACK with N48M_ALLOW) gets a VALID pipeline whose dispatches are no-ops: MPS aborts
        // (MTLReportFailure) on a compute pipeline error, a missing desktop effect is survivable. Same rule as the render FALLBACK.
        NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
        n48_dump_function(fn, "kernel", paths, notes);
        N48LOG("COMPUTE FALLBACK %s (%s); AIR dumped: %s", ([fn respondsToSelector:@selector(name)] ? [fn name] : @"?").UTF8String, e.localizedDescription.UTF8String, [paths componentsJoinedByString:@"; "].UTF8String);
        return [[N48ComputePipelineState alloc] initPlaceholderWithDevice:self function:fn];
    }
    if (e.code == 41) {
        NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
        n48_dump_function(fn, "kernel", paths, notes);
        e = n48_err(100, [NSString stringWithFormat:@"Navi48Metal: compute pipeline not in spvcache; AIR dumped for offline translation: %@%@", [paths componentsJoinedByString:@"; "],
                          notes.count ? [@" | problems: " stringByAppendingString:[notes componentsJoinedByString:@"; "]] : @""]);
        N48LOG("%s", e.localizedDescription.UTF8String);
    }
    if (error) *error = e;
    return nil;
}
- (id)newComputePipelineStateWithFunction:(id)fn error:(NSError **)error { N48LOG("newComputePipelineStateWithFunction:error:"); return [self n48NewComputePipeline:fn error:error]; }
- (id)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o reflection:(void *)r error:(NSError **)error {
    (void)o; (void)r; N48LOG("newComputePipelineStateWithFunction:options:reflection:error:"); return [self n48NewComputePipeline:fn error:error]; }
- (void)newComputePipelineStateWithFunction:(id)fn completionHandler:(void (^)(id, NSError *))h {
    N48LOG("newComputePipelineStateWithFunction:completionHandler:"); NSError *e = nil; id p = [self n48NewComputePipeline:fn error:&e]; if (h) h(p, e); }
- (void)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h {
    (void)o; N48LOG("newComputePipelineStateWithFunction:options:completionHandler:"); NSError *e = nil; id p = [self n48NewComputePipeline:fn error:&e]; if (h) h(p, nil, e); }
// native #12 luma: QuartzCore's CA::OGL::MetalContext::get_compute_pipeline (compute_average_luma, glass backdrop tint, Reduce Transparency off) sends
// -newComputePipelineStateWithDescriptor:error: (CONFIRMED: selector reference at QuartzCore 0x7ff80d2e6c74 resolves through the cache). That selector was not
// implemented here, the inherited one returned nil although the spvcache has the kernel, and QuartzCore aborts on nil (abort_with_payload, function=compute_average_luma).
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
    N48LOG("newComputePipelineStateWithDescriptor:error:"); return [self n48NewComputePipeline:d.computeFunction error:error]; }
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h {
    N48LOG("newComputePipelineStateWithDescriptor:completionHandler:"); NSError *e = nil; id p = [self n48NewComputePipeline:d.computeFunction error:&e]; if (h) h(p, e); }
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o reflection:(void *)r error:(NSError **)error {
    (void)o; (void)r; N48LOG("newComputePipelineStateWithDescriptor:options:reflection:error:"); return [self n48NewComputePipeline:d.computeFunction error:error]; }
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h {
    (void)o; N48LOG("newComputePipelineStateWithDescriptor:options:completionHandler:"); NSError *e = nil; id p = [self n48NewComputePipeline:d.computeFunction error:&e]; if (h) h(p, nil, e); }

// ---- 10c: textures ----
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
    N48LOG("newTextureWithDescriptor: %lux%lu pf %lu usage 0x%lx", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage);
    NSError *err = nil;
    N48Texture *t = [[N48Texture alloc] initWithDevice:self descriptor:d error:&err];
    if (!t) N48LOG("newTextureWithDescriptor NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lux%lu mips %lu array %lu samples %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                   (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount,
                   (unsigned long)d.arrayLength, (unsigned long)d.sampleCount, err.localizedDescription.UTF8String);
    return t;
}

// 11h.6: IOSurface-backed texture (plane 0 of a single-plane 32 bpp surface); nil + log when the surface cannot be imported.
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane {
    N48LOG("newTextureWithDescriptor:iosurface:plane: %lux%lu pf %lu usage 0x%lx plane %lu", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage, (unsigned long)plane);
    NSError *err = nil;
    N48Texture *t = [[N48Texture alloc] initWithDevice:self descriptor:d iosurface:s plane:plane error:&err];
    if (!t) N48LOG("newTextureWithDescriptor:iosurface:plane: NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lu plane %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                   (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)plane, err.localizedDescription.UTF8String);
    return t;
}

// ---- 12: client-memory buffers and textures (QuartzCore: -newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:, 4 per second at the login screen) ----
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)len options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))d {
    N48LOG("newBufferWithBytesNoCopy:length:%lu options:0x%lx", (unsigned long)len, (unsigned long)o);
    NSError *e = nil; N48Buffer *b = [[N48Buffer alloc] initWithDevice:self bytesNoCopy:p length:len options:o deallocator:d error:&e];
    if (!b) N48LOG("newBufferWithBytesNoCopy: NIL: %s", e.localizedDescription.UTF8String);   // a refused buffer never owned the memory: the deallocator is NOT called
    return b;
}
- (id)n48NoCopyTexture:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr what:(const char *)what {
    N48Buffer *b = [self newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared deallocator:dl];
    if (!b) { N48LOG("%s: NIL (buffer over the client memory refused); pf %lu type %lu usage 0x%lx storage %lu %lux%lu", what, (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                     (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height); return nil; }
    return [b newTextureWithDescriptor:d offset:off bytesPerRow:bpr];   // the texture keeps the buffer, the buffer keeps the memory until the deallocator runs
}
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    return [self n48NoCopyTexture:p length:len deallocator:nil descriptor:d offset:off bytesPerRow:bpr what:"newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:"];
}
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    return [self n48NoCopyTexture:p length:len deallocator:dl descriptor:d offset:off bytesPerRow:bpr what:"newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:"];
}
- (id)newTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *, NSUInteger))dl {
    const N48Fmt *f = n48_fmt(d.pixelFormat);
    return [self n48NoCopyTexture:p length:len deallocator:dl descriptor:d offset:0 bytesPerRow:f ? d.width * f->bpp : 0 what:"newTextureWithBytesNoCopy:length:descriptor:deallocator:"];
}

// ---- m11 H1: heaps (see N48Heap) ----
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
    N48LOG("newHeapWithDescriptor: size %lu storage %lu cache %lu hazard %lu type %lu", (unsigned long)d.size, (unsigned long)d.storageMode, (unsigned long)d.cpuCacheMode, (unsigned long)d.hazardTrackingMode, (unsigned long)d.type);
    return [[N48Heap alloc] initWithDevice:self descriptor:d];
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)len options:(MTLResourceOptions)o { (void)o; return (MTLSizeAndAlign){ len, N48_HEAP_ALIGN }; }
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
    NSUInteger sz = 0, al = N48_HEAP_ALIGN; if (!n48_heap_tex_sa(d, &sz, &al)) { N48LOG("heapTextureSizeAndAlignWithDescriptor: pf %lu not supported: size 0", (unsigned long)d.pixelFormat); }
    return (MTLSizeAndAlign){ sz, al };
}

// ---- gap census: depth/stencil state, fences, limits ----
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
    N48LOG("newDepthStencilStateWithDescriptor:");
    return [[N48DepthStencilState alloc] initWithDevice:self descriptor:d];
}
// MTLIOAccelDevice -newFence SEGVs on this device (measured: -[MTLIOAccelDevice newFence] -> _dispatch_sync_f on a NULL queue).
- (id)newFence { N48LOG("newFence"); return [[N48Fence alloc] initWithDevice:self]; }
// MTLIOAccelDevice -currentAllocatedSize SEGVs on this device (measured: it reads the IOAccel resource pool we do not have).
- (NSUInteger)currentAllocatedSize { return (NSUInteger)atomic_load(&n48_alloc_total); }
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }   // Metal reports 32 KiB on every Mac GPU; RADV's local-memory limit is larger, so any kernel that fits here fits there
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }   // maxComputeWorkGroupSize 1024,1024,1024 (RADV gfx1201)
- (BOOL)supportsSampleCount:(NSUInteger)n { return n == 1; }   // textures refuse MSAA (initWithDevice:descriptor: sampleCount != 1)
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)pf { (void)pf; return 256; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }

// ---- 10b: queues ----
// -[_MTLCommandQueue dealloc] calls [_dev _purgeDevice] -> MTLIOAccelCommandBufferStoragePoolPurge on pools our
// device never created (SEGV in os_unfair_lock_lock, seen. We own no IOAccel pools: nothing to purge.
- (void)_purgeDevice { N48LOG("_purgeDevice (no IOAccel pools; skipped)"); }

- (id)n48NewQueue:(id)desc {
    NSError *err = nil;
    N48CommandQueue *q = [[N48CommandQueue alloc] initWithDevice:self descriptor:desc error:&err];
    if (!q) N48LOG("newCommandQueue: failed: %s", err.localizedDescription.UTF8String);
    return q;
}
- (id)newCommandQueue { N48LOG("newCommandQueue"); return [self n48NewQueue:nil]; }
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n { (void)n; N48LOG("newCommandQueueWithMaxCommandBufferCount:"); return [self n48NewQueue:nil]; }
- (id)newCommandQueueWithDescriptor:(id)d { N48LOG("newCommandQueueWithDescriptor:"); return [self n48NewQueue:d]; }

// ---- 10d: pipeline creation from the SPIR-V cache; a miss dumps the AIR and returns an NSError (all four forms;
// MTLCompiler is nil on _MTLDevice, F1) ----
- (id)n48NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    NSError *e = nil;
    N48RenderPipelineState *p = [[N48RenderPipelineState alloc] initWithDevice:self descriptor:d error:&e];
    if (p) return p;
    N48LOG("pipeline creation failed: %s", e.localizedDescription.UTF8String);
    NSError *dump = n48_dump_pipeline(d);   // cache miss: leave the AIR for the host Mac to translate
    if (error) *error = [e.code == 41 ? dump : e copy];
    return nil;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    N48LOG("newRenderPipelineStateWithDescriptor:error:");
    return [self n48NewPipeline:d error:error];
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts reflection:(void *)refl error:(NSError **)error {
    (void)opts; (void)refl;
    N48LOG("newRenderPipelineStateWithDescriptor:options:reflection:error:");
    return [self n48NewPipeline:d error:error];
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h {
    N48LOG("newRenderPipelineStateWithDescriptor:completionHandler:");
    NSError *e = nil; id p = [self n48NewPipeline:d error:&e]; if (h) h(p, e);
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts completionHandler:(void (^)(id, id, NSError *))h {
    (void)opts;
    N48LOG("newRenderPipelineStateWithDescriptor:options:completionHandler:");
    NSError *e = nil; id p = [self n48NewPipeline:d error:&e]; if (h) h(p, nil, e);
}

// Diagnostic: name any selector Metal sends that we (and our superclasses) do not implement, before the
// standard exception. Seen: -[_MTLDevice initLimits] -> doesNotRecognizeSelector.
- (void)doesNotRecognizeSelector:(SEL)sel {
    os_log(OS_LOG_DEFAULT, "Navi48Metal: UNRECOGNIZED selector %{public}s", sel_getName(sel));
    NSLog(@"Navi48Metal: UNRECOGNIZED selector %s", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port {
    os_log(OS_LOG_DEFAULT, "Navi48Metal: initWithAcceleratorPort 0x%x", port);
    NSLog(@"Navi48Metal: initWithAcceleratorPort 0x%x", port);
    const char *why = n48_admit(port);   // 11b: process filter, kill file, Ready, crash counter
    self = [super initWithAcceleratorPort:port];
    if (why) {
        // Declining BEFORE the base init crashed (measured: the un-initialised object is released by ARC and
        // -[_MTLDevice dealloc] -> dispatch_release(NULL) SEGVs), so the base init runs first and the object is then
        // dropped: ARC releases the fully initialised self and Metal sees nil.
        os_log(OS_LOG_DEFAULT, "Navi48Metal: initWithAcceleratorPort 0x%x DECLINED in %{public}s (pid %d): %{public}s", port, getprogname(), getpid(), why);
        fprintf(stderr, "Navi48Metal: declined in %s (pid %d): %s\n", getprogname(), getpid(), why);
        return nil;
    }
    os_log(OS_LOG_DEFAULT, "Navi48Metal: initWithAcceleratorPort 0x%x -> %{public}s", port, self ? "ok" : "nil");
    return self;
}

- (BOOL)supportLazyInitialization { return N48_LAZY ? YES : NO; }

// _MTLDevice -name composes vendorName/familyName/productName, skipping empty parts, joined by " "
// (SUSPECTED from -[_MTLDevice name] at 0x7ff80f49a2f6; fallback string "Unnamed_GPU") -> "AMD Radeon RX 9070 XT".
- (NSString *)vendorName  { return @"AMD"; }
- (NSString *)familyName  { return @"Radeon"; }
- (NSString *)productName { return @"RX 9070 XT"; }

// featureProfile: -[AppleParavirtDevice featureProfile] (pvm.x86 0xacc3) is `mov eax,0x2710; ret` = 10000 (CONFIRMED).
- (NSUInteger)featureProfile { return 10000; }
// llvmVersion: pvm.x86 0xb04f `mov eax,0x7d17` = 32023 (CONFIRMED).
- (int)llvmVersion { return 0x7d17; }

#if N48_9D
- (BOOL)supportsFamily:(NSInteger)family {
    switch (family) {
    case N48_Mac2: case N48_Common1: case N48_Common2: case N48_Common3: case N48_Metal3:
        return YES;
    default:            // Apple1..9 (1001..1009), Mac1, MacCatalyst, Metal4, unknown -> NO
        return NO;
    }
}
- (NSUInteger)maxBufferLength                { return (NSUInteger)N48_MAX_BUFFER_LENGTH; }
- (uint64_t)recommendedMaxWorkingSetSize     { return N48_RECOMMENDED_WORKING_SET; }
- (BOOL)hasUnifiedMemory { return NO; }
- (BOOL)isHeadless       { return n48_headless(); }   // 11b: YES unless /private/tmp/n48m-headless-no exists (read once per process)
- (BOOL)isLowPower       { return NO; }
- (BOOL)isRemovable      { return NO; }
#endif

@end
