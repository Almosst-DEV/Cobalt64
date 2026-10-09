// n48_census.h: bundle build 18 (app-fix round 2), the unimplemented-selector CENSUS (an internal design note "Unimplemented-selector policy").
//
// WHY. Every selector a client sends that none of our classes (or their Apple superclasses) implements ends in -doesNotRecognizeSelector: and the process dies (Maps: -setVisibilityResultMode:offset:). We do not
// answer "everything with zero" (zero is the wrong safe value for occlusion, and nil only moves a crash). Instead the +load protocol pass, which already makes our classes CONFORM to Apple's Metal protocols,
// now also COMPARES: for each of our classes it lists every instance method (required and optional) of the corresponding Metal protocols - and of the protocols those inherit - that the class does not respond to.
// The protocols are the RUNNING system's (Tahoe on the PC): the census is exactly the set of selectors that would crash today.
//
// WHEN / WHERE. The scan runs once per process at +load (a few hundred class_respondsToSelector calls). Nothing is printed there (every Metal-enumerating process loads this bundle) unless the environment has
// N48M_CENSUS=1 (the host Mac harness). A process that actually gets a device (initWithAcceleratorPort: not declined) logs the saved lines once, "Navi48Metal: census ...".
//
// Output line: "census: <Class> lacks <n> of <m> <Protocol> methods:" then selectors, a leading '!' marks a REQUIRED method; long lists are continued on further lines.
#ifndef N48_CENSUS_H
#define N48_CENSUS_H
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

static NSMutableArray<NSString *> *n48cen_lines;
static NSMutableSet<NSString *> *n48cen_missing_set;   // "Class selector" for the vetted-list check in the tests and the log

static void n48cen_collect(Protocol *p, NSMutableSet *seen, NSMutableArray<NSDictionary *> *out) {
    NSString *pn = NSStringFromProtocol(p);
    if ([pn isEqualToString:@"NSObject"] || [seen containsObject:pn]) return;
    [seen addObject:pn];
    for (int req = 1; req >= 0; req--) {
        unsigned n = 0; struct objc_method_description *md = protocol_copyMethodDescriptionList(p, req ? YES : NO, YES, &n);
        for (unsigned i = 0; i < n; i++) if (md[i].name) [out addObject:@{ @"sel": NSStringFromSelector(md[i].name), @"req": @(req), @"proto": pn }];
        free(md);
    }
    unsigned np = 0; Protocol * __unsafe_unretained *pl = protocol_copyProtocolList(p, &np);
    for (unsigned i = 0; i < np; i++) n48cen_collect(pl[i], seen, out);
    free(pl);
}
// One class against one top protocol (and everything it inherits). Returns the number missing.
static unsigned n48cen_scan(Class c, const char *clsName, const char *protoName) {
    Protocol *p = objc_getProtocol(protoName);
    if (!p) return 0;
    NSMutableSet *seen = [NSMutableSet set]; NSMutableArray<NSDictionary *> *all = [NSMutableArray array];
    n48cen_collect(p, seen, all);
    NSMutableArray<NSString *> *miss = [NSMutableArray array]; NSMutableSet *dedup = [NSMutableSet set];
    for (NSDictionary *d in all) {
        NSString *s = d[@"sel"]; if ([dedup containsObject:s]) continue; [dedup addObject:s];
        SEL sel = NSSelectorFromString(s);
        if (!class_respondsToSelector(c, sel)) { [miss addObject:[NSString stringWithFormat:@"%@%@", [d[@"req"] boolValue] ? @"!" : @"", s]]; [n48cen_missing_set addObject:[NSString stringWithFormat:@"%s %@", clsName, s]]; }
    }
    if (!n48cen_lines) n48cen_lines = [NSMutableArray array];
    NSMutableString *line = [NSMutableString stringWithFormat:@"census: %s lacks %lu of %lu %s methods:", clsName, (unsigned long)miss.count, (unsigned long)dedup.count, protoName];
    for (NSString *m in miss) {
        if (line.length + m.length > 700) { [n48cen_lines addObject:[line copy]]; line = [NSMutableString stringWithFormat:@"census: %s (cont.) %s:", clsName, protoName]; }
        [line appendFormat:@" %@", m];
    }
    [n48cen_lines addObject:[line copy]];
    return (unsigned)miss.count;
}
// Run the scan for the classes the bundle defines. `pairs`: { class, "ClassName", "MTLProtocol" } triples, NULL class terminates.
typedef struct { Class cls; const char *name; const char *proto; } n48cen_pair;
static unsigned n48cen_run(const n48cen_pair *pairs) {
    if (!n48cen_missing_set) n48cen_missing_set = [NSMutableSet set];
    unsigned total = 0;
    for (const n48cen_pair *q = pairs; q->cls; q++) total += n48cen_scan(q->cls, q->name, q->proto);
    return total;
}
#endif
