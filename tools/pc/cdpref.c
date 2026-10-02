//
//  cdpref — read and write com.apple.CoreDisplay preferences through the EXACT CFPreferences call
//           CoreDisplay itself makes, rather than through `defaults` and a guessed path.
//
//  WHY THIS EXISTS (notes/APPLE-DRIVER-VERDICT.md)
//
//  CoreDisplay::GetBoolCFPreference (0x7ff8053645f0) calls
//
//      CFPreferencesCopyValue(key, CFSTR("com.apple.CoreDisplay"),
//                             kCFPreferencesAnyUser, kCFPreferencesCurrentHost)
//
//  with both scope constants resolved from its GOT slots. That is the machine-wide, per-host store.
//  Getting a value into it is not as simple as it looks, and the obvious attempt FAILS SILENTLY WHILE
//  APPEARING TO WORK:
//
//      sudo defaults -currentHost write /Library/Preferences/com.apple.CoreDisplay useIOP_onAMD -bool true
//      sudo defaults -currentHost read  /Library/Preferences/com.apple.CoreDisplay
//        { "useIOP_onAMD" = 1; }          <- looks right
//      ls /Library/Preferences/ByHost/com.apple.CoreDisplay.*
//        no matches                       <- but nothing landed in the per-host store
//
//  Given an ABSOLUTE PATH, `defaults` treats it as a literal plist and ignores -currentHost: the value
//  goes to /Library/Preferences/com.apple.CoreDisplay.plist, the AnyUser + ANYHOST domain, which the
//  call above never reads. The read-back consults the same wrong file and returns true. Trusting it
//  would produce a null result that looks like a confirmed-shut gate but is really a misdirected
//  write - a FALSE CONFIRMATION of the model, which is worse than no result at all.
//
//  So this tool calls the API with the same two constants, and its `get` is a read through the
//  identical call CoreDisplay makes. "A file appeared" is a weaker check than "the call CoreDisplay
//  makes returns true", and this does both.
//
//  Build (on the host Mac, for the PC):  see tools/build-cdpref.sh
//  Usage (on the PC, as root):
//      sudo ./cdpref get  <key>
//      sudo ./cdpref set  <key> <0|1>
//      sudo ./cdpref del  <key>
//      sudo ./cdpref dump            (the four flags names, in one shot)
//
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

static CFStringRef kDomain = CFSTR("com.apple.CoreDisplay");

// The four flags _CGXMappedDisplayStart reads, all through the same domain and scope.
static const char *const kFlags[] = {
    "useIOP", "useIOP_onAMD", "displaySurfaceUsesIOSurface", "useIOP_gamma_onAMD",
};

static void show(const char *key) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    // EXACTLY CoreDisplay's call: AnyUser + CurrentHost.
    CFPropertyListRef v = CFPreferencesCopyValue(k, kDomain, kCFPreferencesAnyUser,
                                                 kCFPreferencesCurrentHost);
    if (!v) {
        printf("  %-30s <absent>            (AnyUser+CurrentHost)\n", key);
    } else {
        CFTypeID t = CFGetTypeID(v);
        if (t == CFBooleanGetTypeID())
            printf("  %-30s %-19s (AnyUser+CurrentHost, CFBoolean)\n", key,
                   CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false");
        else if (t == CFNumberGetTypeID()) {
            long long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
            printf("  %-30s %-19lld (AnyUser+CurrentHost, CFNumber)\n", key, n);
        } else
            printf("  %-30s <non-boolean>       (AnyUser+CurrentHost)\n", key);
        CFRelease(v);
    }
    // For contrast, the domain the obvious `defaults` invocation actually writes: AnyUser + ANYHOST.
    CFPropertyListRef w = CFPreferencesCopyValue(k, kDomain, kCFPreferencesAnyUser,
                                                 kCFPreferencesAnyHost);
    if (w) {
        printf("  %-30s %-19s *** ALSO SET IN AnyUser+AnyHost - CoreDisplay does NOT read that ***\n",
               "", CFGetTypeID(w) == CFBooleanGetTypeID()
                   ? (CFBooleanGetValue((CFBooleanRef)w) ? "true" : "false") : "<set>");
        CFRelease(w);
    }
    CFRelease(k);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cdpref get <key> | set <key> <0|1> | del <key> | dump\n");
        return 2;
    }
    if (!strcmp(argv[1], "dump")) {
        printf("com.apple.CoreDisplay, read exactly as CoreDisplay::GetBoolCFPreference reads it:\n");
        for (unsigned i = 0; i < sizeof(kFlags) / sizeof(kFlags[0]); i++) show(kFlags[i]);
        return 0;
    }
    if (!strcmp(argv[1], "get") && argc > 2) { show(argv[2]); return 0; }

    if (!strcmp(argv[1], "set") && argc > 3) {
        CFStringRef k = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
        CFBooleanRef v = (argv[3][0] == '0') ? kCFBooleanFalse : kCFBooleanTrue;
        CFPreferencesSetValue(k, v, kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        Boolean ok = CFPreferencesSynchronize(kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        printf("set %s = %s in AnyUser+CurrentHost; synchronize %s\n", argv[2],
               v == kCFBooleanTrue ? "true" : "false", ok ? "OK" : "FAILED");
        CFRelease(k);
        printf("read back through CoreDisplay's own call:\n");
        show(argv[2]);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[1], "del") && argc > 2) {
        CFStringRef k = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
        CFPreferencesSetValue(k, NULL, kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        Boolean ok = CFPreferencesSynchronize(kDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        printf("deleted %s from AnyUser+CurrentHost; synchronize %s\n", argv[2], ok ? "OK" : "FAILED");
        CFRelease(k);
        printf("read back through CoreDisplay's own call:\n");
        show(argv[2]);
        return ok ? 0 : 1;
    }
    fprintf(stderr, "usage: cdpref get <key> | set <key> <0|1> | del <key> | dump\n");
    return 2;
}
