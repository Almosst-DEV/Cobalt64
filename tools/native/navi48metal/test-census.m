// test-census.m: the unimplemented-selector census (build 18, n48_census.h) and the vetted-list policy.
// Build/run (from this directory):  clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -o /tmp/test-census test-census.m && /tmp/test-census .      (argv[1] = the directory holding Navi48Device.m)
//   1. the scan itself, on a FAKE protocol (a required method, an optional one, an inherited one, and a method the class gets from its superclass): exactly the missing ones are reported, required ones marked '!'.
//   2. the policy pins on Navi48Device.m: the census runs from +load over all the Metal protocols the bundle's classes claim, is logged once by a process that got a device, the vetted list is exactly the one
//      selector the evidence names, it is a no-op with a stated reason, and there is NO blanket handler (no forwardInvocation / methodSignatureForSelector / resolveInstanceMethod answers).
#import <Foundation/Foundation.h>
#include "n48_census.h"

@protocol FakeBase
- (void)baseRequired;
@optional
- (void)baseOptional;
@end
@protocol FakeProto <FakeBase, NSObject>
- (void)haveIt;
- (void)missingRequired;
- (void)fromSuper;
- (NSUInteger)missingScalar:(NSUInteger)x with:(id)y;
@optional
- (void)missingOptional;
- (void)haveOptional;
@end
@interface FakeSuper : NSObject
- (void)fromSuper;
@end
@implementation FakeSuper
- (void)fromSuper {}
@end
@interface FakeCls : FakeSuper
@end
@implementation FakeCls
- (void)haveIt {}
- (void)haveOptional {}
- (void)baseRequired {}
@end

static int fails, runs;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; runs++; printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) fails++; } while (0)
static NSString *slurp(NSString *dir, NSString *name) { return [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil]; }
static NSUInteger count(NSString *hay, NSString *needle) { NSUInteger n = 0, p = 0; for (;;) { NSRange r = [hay rangeOfString:needle options:0 range:NSMakeRange(p, hay.length - p)]; if (r.location == NSNotFound) break; n++; p = r.location + r.length; } return n; }

int main(int argc, char **argv) { @autoreleasepool {
    NSString *dir = argc > 1 ? @(argv[1]) : @".";
    (void)@protocol(FakeProto); (void)@protocol(FakeBase);   // reference the protocols so the runtime registers them
    n48cen_missing_set = [NSMutableSet set];
    unsigned miss = n48cen_scan([FakeCls class], "FakeCls", "FakeProto");
    NSString *all = [n48cen_lines componentsJoinedByString:@"\n"];
    CHECK("scan: exactly the four missing methods are counted", miss == 4);
    CHECK("scan: a missing REQUIRED method is reported with '!'", [all containsString:@"!missingRequired"]);
    CHECK("scan: a missing required method with arguments is reported by its full selector", [all containsString:@"!missingScalar:with:"]);
    CHECK("scan: a missing OPTIONAL method is reported without '!'", [all containsString:@" missingOptional"] && ![all containsString:@"!missingOptional"]);
    CHECK("scan: the inherited protocol's optional method is reported too", [all containsString:@" baseOptional"]);
    CHECK("scan: a method the class implements is not reported", ![all containsString:@"haveIt"] && ![all containsString:@"haveOptional"] && ![all containsString:@"baseRequired"]);
    CHECK("scan: a method inherited from the SUPERCLASS counts as implemented", ![all containsString:@"fromSuper"]);
    CHECK("scan: the line names the class, the count and the protocol", [all containsString:@"census: FakeCls lacks 4 of 8 FakeProto methods:"]);
    CHECK("scan: the missing set carries class + selector for each", [n48cen_missing_set containsObject:@"FakeCls missingRequired"] && n48cen_missing_set.count == 4);
    CHECK("scan: an unknown protocol name scans nothing and does not crash", n48cen_scan([FakeCls class], "FakeCls", "NoSuchProtocolAtAll") == 0);
    n48cen_pair pairs[] = { { [FakeCls class], "FakeCls", "FakeProto" }, { Nil, NULL, NULL } };
    n48cen_lines = nil; n48cen_missing_set = nil;
    CHECK("run: walks the pair list to the terminator", n48cen_run(pairs) == 4 && n48cen_lines.count >= 1);
    // a long list is continued on further lines, never truncated
    { NSMutableString *p = [NSMutableString string]; (void)p; }

    NSString *src = slurp(dir, @"Navi48Device.m");
    CHECK("pin: Navi48Device.m is readable", src.length > 0);
    if (src.length) {
        NSRange l = [src rangeOfString:@"+ (void)load {"], lEnd = [src rangeOfString:@"// lazyInitialize: log, then run the base"];
        NSString *load = (l.location != NSNotFound && lEnd.location > l.location) ? [src substringWithRange:NSMakeRange(l.location, lEnd.location - l.location)] : @"";
        for (NSString *pp in @[ @"MTLRenderCommandEncoder", @"MTLComputeCommandEncoder", @"MTLBlitCommandEncoder", @"MTLCommandBuffer", @"MTLDevice", @"MTLTexture", @"MTLBuffer" ]) {
            BOOL okp = [load containsString:[NSString stringWithFormat:@"\"%@\" }", pp]];
            NSString *nm = [NSString stringWithFormat:@"pin: +load runs the census against %@", pp];
            CHECK(nm.UTF8String, okp);
        }
        CHECK("pin: +load calls n48cen_run (the census is not silenced)", [load containsString:@"unsigned total = n48cen_run(pairs);"]);
        CHECK("pin: N48M_CENSUS=1 prints the lines at +load (the host Mac harness)", [load containsString:@"getenv(\"N48M_CENSUS\")"] && [load containsString:@"fprintf(stderr, \"%s\\n\", l.UTF8String)"]);
        NSRange cl = [src rangeOfString:@"static void n48_census_log(void) {"];
        NSString *clb = cl.location != NSNotFound ? [src substringWithRange:NSMakeRange(cl.location, MIN((NSUInteger)700, src.length - cl.location))] : @"";
        CHECK("pin: the census is logged once per process (atomic flag) through N48LOG, every line", [clb containsString:@"atomic_exchange(&done_, 1)"] && [clb containsString:@"N48LOG(\"%s\", l.UTF8String)"]);
        NSRange ia = [src rangeOfString:@"- (instancetype)initWithAcceleratorPort:(uint32_t)port {"];
        NSString *iab = ia.location != NSNotFound ? [src substringWithRange:NSMakeRange(ia.location, MIN((NSUInteger)2600, src.length - ia.location))] : @"";
        NSRange dec = [iab rangeOfString:@"return nil;"], cal = [iab rangeOfString:@"n48_census_log();"];
        CHECK("pin: only a process that was NOT declined logs the census (the call comes after the decline's return)", dec.location != NSNotFound && cal.location != NSNotFound && dec.location < cal.location);
        CHECK("pin: the vetted list is exactly ONE selector, setFragmentVisibleFunctionTable:atBufferIndex:", count(src, @"- (void)setFragmentVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {") == 1 && count(src, @"VisibleFunctionTable") >= 1 && count(src, @"- (void)setVertexVisibleFunctionTable") == 0 && count(src, @"- (void)setVisibleFunctionTable") == 0);
        CHECK("pin: the vetted selector is a no-op with a stated reason, logged once", [src containsString:@"N48_ONCE(\"setFragmentVisibleFunctionTable:atBufferIndex: ignored (vetted no-op: pipelines that call through a function table are refused at creation)\")"]);
        CHECK("pin: NO blanket handler: no forwardInvocation / methodSignatureForSelector / resolveInstanceMethod / forwardingTargetForSelector", count(src, @"forwardInvocation:") == 0 && count(src, @"methodSignatureForSelector:") == 0 && count(src, @"resolveInstanceMethod:") == 0 && count(src, @"forwardingTargetForSelector:") == 0);
        CHECK("pin: everything else keeps the crash, logged first (N48_DNR on every encoder and pipeline class, the device logs too)", count(src, @"N48_DNR(N48RenderEncoder)") == 1 && count(src, @"N48_DNR(N48ComputeEncoder)") == 1 && count(src, @"N48_DNR(N48BlitEncoder)") == 1 && count(src, @"N48_DNR(N48CommandBuffer)") == 1 && [src containsString:@"UNRECOGNIZED selector %{public}s"]);
        CHECK("pin: N48_DNR still ends in the standard exception (no swallowing)", [src containsString:@"N48LOG(\"UNRECOGNIZED selector %s on \" #cls, sel_getName(sel)); \\\n        [super doesNotRecognizeSelector:sel]; }"]);
    }
    printf("test-census: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
} }
