// ptrmove.c — A BOUNDED POINTER-MOVER for unattended PC runs (pointer movement woke the
// lock-screen clock and brought back the capsule squares; unattended runs must exercise the same path).
//
// It ONLY moves the pointer. It never clicks, types, scrolls or focuses:
//   - default mode: CGWarpMouseCursorPosition only (no event is posted; the WindowServer moves the cursor);
//   - `--post-moved` (only in the `ptrmove-ev` build, -DPTRMOVE_EVENTS): after each warp, one kCGEventMouseMoved
//     event created by CGEventCreateMouseEvent with the COMPILE-TIME CONSTANT type kCGEventMouseMoved and posted at
//     kCGHIDEventTap. No other event type exists anywhere in this file;
//   - `--declare-activity`: IOPMAssertionDeclareUserActivity(kIOPMUserActiveLocal) at start and every 10 s, released
//     at exit (the in-process equivalent of `caffeinate -u`), to wake the display/UI.
// tools/pc/build-ptrmove.sh builds both binaries and FAILS if `nm -u` shows any keyboard, button, scroll, text-input,
// event-tap, event-retyping or accessibility-action symbol, or any symbol outside its exact allow-list.
//
// BOUNDS, enforced here and not only by flags:
//   - the box (default x 200..700, y 250..700, global display points) is clamped to the main display's bounds shrunk
//     by EDGE_MARGIN on every side (no hot corners, no menu-bar extras); it must stay >= MIN_BOX on both axes;
//   - moves <= HARD_MAX_MOVES, seconds <= HARD_MAX_SECONDS, period >= MIN_PERIOD_MS (flags above these are REFUSED);
//   - the loop also stops on wall clock at the seconds bound, after MAX_CONSEC_ERR failed warps, and on SIGINT/SIGTERM;
//   - every target is re-checked against the clamped box immediately before the warp (refuse + exit 3 if outside).
//
// Every move is logged to stdout: `move <i>/<n> abs=<mach_absolute_time> us=<uptime us> x=<x> y=<y> rc=<CGError>`,
// where us = mach_absolute_time scaled by the timebase = the clock the kext stamps its lines with.
//
// usage: ptrmove [--dry-run] [--box X0,Y0,X1,Y1] [--moves N] [--seconds S] [--pattern circle|zigzag] [--laps L]
//                [--declare-activity] [--post-moved (ptrmove-ev only)] [--screen W,H (dry-run only)]
// Build: tools/pc/build-ptrmove.sh (host Mac, cross x86_64). Usage and PC launch commands: tools/runkit/ptrmove.md.

#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PTRMOVE_VERSION   "ptrmove-1"
#define HARD_MAX_MOVES    1000
#define HARD_MAX_SECONDS  120
#define MIN_PERIOD_MS     20
#define EDGE_MARGIN       64.0
#define MIN_BOX           40.0
#define MAX_CONSEC_ERR    5
#define ACTIVITY_EVERY_S  10.0

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static mach_timebase_info_data_t g_tb;
static uint64_t abs_to_us(uint64_t a) { return (a * g_tb.numer / g_tb.denom) / 1000ull; }

typedef struct { double x0, y0, x1, y1; } box_t;

static void usage(void) {
    fprintf(stderr,
        "usage: ptrmove [--dry-run] [--box X0,Y0,X1,Y1] [--moves N<=%d] [--seconds S<=%d] [--pattern circle|zigzag]\n"
        "               [--laps L] [--declare-activity] [--post-moved (ptrmove-ev only)] [--screen W,H (dry-run only)]\n",
        HARD_MAX_MOVES, HARD_MAX_SECONDS);
}

static int parse4(const char *s, double v[4]) { return sscanf(s, "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]) == 4; }

// Target i of n on the pattern, inside box b.
static CGPoint target(const char *pattern, int i, int n, double laps, box_t b) {
    double cx = (b.x0 + b.x1) / 2.0, cy = (b.y0 + b.y1) / 2.0;
    double rx = (b.x1 - b.x0) / 2.0, ry = (b.y1 - b.y0) / 2.0;
    double f = (n > 1) ? (double)i / (double)(n - 1) : 0.0;          // 0..1
    CGPoint p;
    if (strcmp(pattern, "zigzag") == 0) {
        // `laps` horizontal sweeps, alternating direction, stepping down; y covers the box once.
        double t = f * laps;
        int row = (int)floor(t); if (row >= (int)laps) row = (int)laps - 1; if (row < 0) row = 0;
        double u = t - row; if (u > 1.0) u = 1.0;
        double xf = (row % 2 == 0) ? u : 1.0 - u;
        double yf = (laps > 1.0) ? (double)row / (laps - 1.0) : 0.5;
        p.x = b.x0 + xf * (b.x1 - b.x0);
        p.y = b.y0 + yf * (b.y1 - b.y0);
    } else {
        double a = 2.0 * M_PI * laps * f;                              // ellipse inscribed in the box
        p.x = cx + rx * cos(a);
        p.y = cy + ry * sin(a);
    }
    p.x = round(p.x); p.y = round(p.y);
    return p;
}

static int inside(CGPoint p, box_t b) { return p.x >= b.x0 && p.x <= b.x1 && p.y >= b.y0 && p.y <= b.y1; }

int main(int argc, char **argv) {
    int dry = 0, moves = 400, seconds = 40, activity = 0, post_moved = 0;
    double laps = 4.0, boxv[4] = {200, 250, 700, 700}, screen_w = 0, screen_h = 0;
    const char *pattern = "circle";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dry-run")) dry = 1;
        else if (!strcmp(a, "--declare-activity")) activity = 1;
        else if (!strcmp(a, "--post-moved")) post_moved = 1;
        else if (!strcmp(a, "--box") && v) { if (!parse4(v, boxv)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--moves") && v) { moves = atoi(v); i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atoi(v); i++; }
        else if (!strcmp(a, "--laps") && v) { laps = atof(v); i++; }
        else if (!strcmp(a, "--pattern") && v) { pattern = v; i++; }
        else if (!strcmp(a, "--screen") && v) { if (sscanf(v, "%lf,%lf", &screen_w, &screen_h) != 2) { usage(); return 2; } i++; }
        else { usage(); return 2; }
    }
#ifndef PTRMOVE_EVENTS
    if (post_moved) { fprintf(stderr, "ptrmove: --post-moved is not compiled into this binary (use ptrmove-ev)\n"); return 2; }
#endif
    if (strcmp(pattern, "circle") && strcmp(pattern, "zigzag")) { fprintf(stderr, "ptrmove: pattern must be circle or zigzag\n"); return 2; }
    if (moves < 1 || moves > HARD_MAX_MOVES) { fprintf(stderr, "ptrmove: REFUSED --moves %d (1..%d)\n", moves, HARD_MAX_MOVES); return 2; }
    if (seconds < 1 || seconds > HARD_MAX_SECONDS) { fprintf(stderr, "ptrmove: REFUSED --seconds %d (1..%d)\n", seconds, HARD_MAX_SECONDS); return 2; }
    if (!(laps >= 1.0 && laps <= 50.0)) { fprintf(stderr, "ptrmove: REFUSED --laps (1..50)\n"); return 2; }
    laps = floor(laps);
    double period_ms = (double)seconds * 1000.0 / (double)moves;
    if (period_ms < MIN_PERIOD_MS) { fprintf(stderr, "ptrmove: REFUSED %d moves in %d s = %.1f ms/move < %d ms\n", moves, seconds, period_ms, MIN_PERIOD_MS); return 2; }
    if (!dry && (screen_w > 0 || screen_h > 0)) { fprintf(stderr, "ptrmove: --screen is for --dry-run only (live runs read the display)\n"); return 2; }

    mach_timebase_info(&g_tb);

    // The screen: live runs read the main display (a read, no side effect); dry runs may state it.
    CGRect scr;
    if (screen_w > 0 && screen_h > 0) scr = CGRectMake(0, 0, screen_w, screen_h);
    else scr = CGDisplayBounds(CGMainDisplayID());
    if (scr.size.width < 2 * EDGE_MARGIN + MIN_BOX || scr.size.height < 2 * EDGE_MARGIN + MIN_BOX) {
        fprintf(stderr, "ptrmove: REFUSED screen %.0fx%.0f too small or unreadable (no WindowServer?)\n", scr.size.width, scr.size.height);
        return 3;
    }
    box_t lim = { scr.origin.x + EDGE_MARGIN, scr.origin.y + EDGE_MARGIN,
                  scr.origin.x + scr.size.width - EDGE_MARGIN, scr.origin.y + scr.size.height - EDGE_MARGIN };
    box_t b = { fmin(boxv[0], boxv[2]), fmin(boxv[1], boxv[3]), fmax(boxv[0], boxv[2]), fmax(boxv[1], boxv[3]) };
    b.x0 = fmax(b.x0, lim.x0); b.y0 = fmax(b.y0, lim.y0); b.x1 = fmin(b.x1, lim.x1); b.y1 = fmin(b.y1, lim.y1);
    if (b.x1 - b.x0 < MIN_BOX || b.y1 - b.y0 < MIN_BOX) {
        fprintf(stderr, "ptrmove: REFUSED box after clamp %.0f,%.0f-%.0f,%.0f (< %.0f pt)\n", b.x0, b.y0, b.x1, b.y1, MIN_BOX);
        return 3;
    }

    printf("%s start abs=%llu us=%llu mode=%s%s%s pattern=%s laps=%.0f moves=%d seconds=%d period_ms=%.1f "
           "screen=%.0f,%.0f,%.0fx%.0f box=%.0f,%.0f-%.0f,%.0f uid=%d\n",
           PTRMOVE_VERSION, (unsigned long long)mach_absolute_time(), (unsigned long long)abs_to_us(mach_absolute_time()),
           dry ? "DRY-RUN" : "warp", post_moved ? "+post-moved" : "", activity ? "+declare-activity" : "",
           pattern, laps, moves, seconds, period_ms, scr.origin.x, scr.origin.y, scr.size.width, scr.size.height,
           b.x0, b.y0, b.x1, b.y1, (int)getuid());
    if (!dry) {
        // Read-only preflight: whether this process may post events (the Accessibility / PostEvent TCC grant).
        // Never CGRequestPostEventAccess: no prompt, no settings change.
        printf("preflight post-event-access=%d\n", CGPreflightPostEventAccess() ? 1 : 0);
    }
    fflush(stdout);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    IOPMAssertionID act = kIOPMNullAssertionID;
    uint64_t t0 = mach_absolute_time(), last_act = 0;
    uint64_t limit_us = (uint64_t)seconds * 1000000ull;
    int consec_err = 0, done = 0, rc_exit = 0;

    for (int i = 0; i < moves && !g_stop; i++) {
        // Pace: move i is due at t0 + i*period.
        uint64_t due_us = (uint64_t)(i * period_ms * 1000.0);
        uint64_t now_us = abs_to_us(mach_absolute_time() - t0);
        if (due_us >= limit_us) break;
        if (due_us > now_us) usleep((useconds_t)(due_us - now_us));
        if (abs_to_us(mach_absolute_time() - t0) >= limit_us) break;

        CGPoint p = target(pattern, i, moves, laps, b);
        if (!inside(p, b)) {
            fprintf(stderr, "ptrmove: INTERNAL target %.0f,%.0f outside box - stopping\n", p.x, p.y);
            rc_exit = 3; break;
        }

        if (!dry && activity && (last_act == 0 || abs_to_us(mach_absolute_time() - last_act) >= (uint64_t)(ACTIVITY_EVERY_S * 1e6))) {
            IOReturn ar = IOPMAssertionDeclareUserActivity(CFSTR("navi48 ptrmove"), kIOPMUserActiveLocal, &act);
            last_act = mach_absolute_time();
            printf("activity abs=%llu us=%llu ret=0x%x id=%u\n", (unsigned long long)last_act,
                   (unsigned long long)abs_to_us(last_act), ar, (unsigned)act);
        }

        uint64_t ta = mach_absolute_time();
        int rc = 0, prc = -1;
        if (!dry) {
            rc = (int)CGWarpMouseCursorPosition(p);
#ifdef PTRMOVE_EVENTS
            if (post_moved && rc == 0) {
                // The ONLY event this program can create: type fixed at compile time to kCGEventMouseMoved.
                CGEventRef ev = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
                if (ev) { CGEventPost(kCGHIDEventTap, ev); CFRelease(ev); prc = 0; } else prc = 1;
            }
#endif
        }
        if (prc < 0)
            printf("move %d/%d abs=%llu us=%llu x=%.0f y=%.0f rc=%d\n", i + 1, moves, (unsigned long long)ta,
                   (unsigned long long)abs_to_us(ta), p.x, p.y, rc);
        else
            printf("move %d/%d abs=%llu us=%llu x=%.0f y=%.0f rc=%d post=%d\n", i + 1, moves, (unsigned long long)ta,
                   (unsigned long long)abs_to_us(ta), p.x, p.y, rc, prc);
        fflush(stdout);
        done++;
        if (rc != 0) { if (++consec_err >= MAX_CONSEC_ERR) { fprintf(stderr, "ptrmove: %d consecutive warp errors - stopping\n", consec_err); rc_exit = 4; break; } }
        else consec_err = 0;
    }

    if (act != kIOPMNullAssertionID) IOPMAssertionRelease(act);
    uint64_t te = mach_absolute_time();
    printf("%s end abs=%llu us=%llu moves_done=%d elapsed_ms=%llu stopped_by=%s\n", PTRMOVE_VERSION,
           (unsigned long long)te, (unsigned long long)abs_to_us(te), done, (unsigned long long)(abs_to_us(te - t0) / 1000ull),
           g_stop ? "signal" : (rc_exit ? "error" : "bound"));
    return rc_exit;
}
