// n48_dispflip.h: the pure state machine of the S5.2a D-copy present (an internal design note section 3), no Vulkan, no ObjC, no locking.
// The bundle (Navi48Device.m) keeps ONE n48df_t under its scanout mutex and drives it; test-dispflip.c exercises it on the host.
//
//   UNBOUND --activate--> ACTIVE --fail--> OFF          (OFF is final for the process: kill switch, missing exports, ENOSYS, any scanout error)
//   pick(): a slot that is registered, NOT in flight, and marked REUSABLE by a FRESH kernel status; marks it in flight, or counts a drop.
//   complete_seq(slot, vkresult, seq): clears the in-flight mark; asks for a present ONLY when the GPU work succeeded, the machine is ACTIVE and seq (submission order) is newer than the last presented.
//   present_result(rc): rc 0 counts a present; any other value fails the machine closed (the caller then releases the plane).
#ifndef N48_DISPFLIP_H
#define N48_DISPFLIP_H
#include <stdint.h>
#include <string.h>

#define N48DF_MAX_SLOTS 3
#define N48DF_MAX_SURF 8                         // the scanout-flag table (proxy flag for tentative / latch-off frames) and the nonscanout counters (bundle 20: NOT the supersede bookkeeping any more, which is per in-flight slot)
#define N48DF_SLOT_REUSABLE (1u << 2)   // == N48N_SCANSLOT_REUSABLE (n48_scanabi.h); the host test checks the equality

// Write classes of a command buffer's writes to ONE display surface (bits, OR-ed while encoding), and the class names derived from them.
enum { N48DF_W_PASS = 1, N48DF_W_CLEAR = 2, N48DF_W_DRAW_FINAL = 4, N48DF_W_DRAW_OTHER = 8, N48DF_W_BLIT = 16, N48DF_W_COMPUTE = 32,
       N48DF_W_DRAW_FILL = 64,    // a draw whose fragment function is ColorFill* (CoreDisplay's surface fill: never a frame by itself)
       N48DF_W_DRAW_COMP = 128 }; // a draw of any other non-GPUPass pipeline (SkyLight UberComposite/Vfx*...): a frame only when the damage switch is on
enum { N48DF_C_FINAL = 0, N48DF_C_RENDER_OTHER, N48DF_C_CLEAR_ONLY, N48DF_C_PASS_NODRAW, N48DF_C_BLIT, N48DF_C_COMPUTE, N48DF_C_UNKNOWN, N48DF_C_RENDER_COMP, N48DF_NCLS };
static const char *const n48df_cls_name[N48DF_NCLS] = { "final-GPUPass", "render-other", "clear-only", "pass-nodraw", "blit", "compute", "unknown", "render-composite" };
enum { N48DF_UNBOUND = 0, N48DF_ACTIVE = 1, N48DF_OFF = 2 };
enum { N48DF_R_NONE = 0, N48DF_R_KILL, N48DF_R_NOSYM, N48DF_R_NOSYS, N48DF_R_GEOM, N48DF_R_ERROR, N48DF_R_PLANELOST, N48DF_R_ALLOC };

typedef struct {
    int state, reason, nslots, last;                 // last: the slot picked most recently (rotation start), -1 at the start
    int acquired, released;                          // the plane was taken / the release was already requested
    uint8_t inflight[N48DF_MAX_SLOTS];
    uint64_t last_seq;                               // the highest frame sequence (submission order) ever handed to present; an older one is never presented
    uint64_t picks, drops, presents, present_fail, gpu_fail, errors, stale, superseded;
    uint64_t m6_skip;                                // bundle 12 (M6, queue 290 S3): frames whose copy was made but whose surface the kernel maps to another display (or does not know): slot released, never presented
    uint32_t sub_sid[N48DF_MAX_SLOTS];               // bundle 20 (F2): per IN-FLIGHT slot, the surface id and seq it was SUBMITTED with (0 = not submitted / untracked); cleared when the slot is released
    uint64_t sub_seq[N48DF_MAX_SLOTS];
    uint8_t sup_n[N48DF_MAX_SLOTS];                  // bundle 20 (F2): per in-flight slot, how many (not failed) frames submitted LATER for the same surface supersede it; > 0 = superseded. Bounded: only 3 slots exist
    uint8_t made[N48DF_MAX_SLOTS];                   // bundle 20 (F2): bit j = the in-flight frame in this slot added one to sup_n[j]; used only to take it back if this frame fails on the GPU (cleared when the slot is released)
    uint64_t scan_full_refused, surf_untracked;      // bundle 20 (F4): proxy flags refused because scan_id[] was full; submissions that could not be tracked (no slot in flight)
    uint64_t cls[N48DF_NCLS], nonfinal[N48DF_NCLS];  // native #12 P1/P2: command buffers that wrote a display surface, by write class; those skipped as non-final
    int damage;                                      // native #12 D2: partial-update emulation on (/private/tmp/n48m-damage); also lets SkyLight composite passes count as frames
    int head;                                        // slot holding the content of the latest CHAINED frame (-1 none): the base of the next partial frame
    uint64_t chain_id, head_id, last_sub_id; int last_sub_valid;   // chain ids are taken at encode; last_sub_* is the latest SUBMITTED chained frame (validity check under the submit lock)
    uint64_t fid[N48DF_MAX_SLOTS];                   // chain id of the frame that owns the slot (0 = none/full-chain-less)
    uint8_t pin[N48DF_MAX_SLOTS], poison[N48DF_MAX_SLOTS]; int8_t basep[N48DF_MAX_SLOTS];   // pin: slot is the base of a not yet completed frame; poison: submitted out of chain order, never present; basep: the base this slot's frame reads
    int carry_full; int32_t carry[4];                // damage of presentable frames that got no slot (dropped): added to the next frame's rectangle
    uint64_t dm_part, dm_full, dm_restart, dm_empty, dm_carry, chain_invalid, dm_copy_px, dm_regions;   // D2 counters
    uint64_t pres_cls[N48DF_NCLS];                   // presents by write class
    uint64_t dk[N48DF_NCLS][3], dk_area[N48DF_NCLS], dd_full[N48DF_NCLS], dd_part[N48DF_NCLS], srck[4];   // D1: cb extent kind (none/full/partial) by class, partial area (px), draw-level full/partial, GPUPass source texture kind
    uint32_t scan_id[N48DF_MAX_SURF]; int nscan;     // native #12 scanout flag: surfaces a final-GPUPass draw has targeted (CoreDisplay's display surfaces); composite writes present only into these
    uint64_t nonscan_skip[N48DF_MAX_SURF]; uint32_t nonscan_sid[N48DF_MAX_SURF]; uint64_t nonscan_total;   // "nonscanout_skipped" by sid (table of 8; overflow counted in the total only)
    uint32_t flag_log[10]; int nflag_log;            // the first 10 flag-set events (sid)
    int presentall;                                  // kill switch of the final-pass rule (/private/tmp/n48m-presentall): present every display write as before
} n48df_t;

static inline void n48df_init(n48df_t *s, int killed) {
    memset(s, 0, sizeof *s); s->last = -1; s->head = -1; for (int i = 0; i < N48DF_MAX_SLOTS; i++) s->basep[i] = -1;
    if (killed) { s->state = N48DF_OFF; s->reason = N48DF_R_KILL; }
}
// Kill-file bypass for the ROOT TEST path only: the L1 kill file /private/tmp/n48m-off is ignored by a process that is root, has N48M_ALLOW=1 and
// N48M_TEST_IGNORE_KILL=1 and is NOT WindowServer. Every other process (WindowServer included) always honours it.
static inline int n48df_off_file_ignored(int euid, const char *allow, const char *ignore, int is_windowserver) {
    return !is_windowserver && euid == 0 && allow && !strcmp(allow, "1") && ignore && !strcmp(ignore, "1");
}
// All slots registered and the plane taken. Only an UNBOUND machine can become ACTIVE.
static inline int n48df_activate(n48df_t *s, int nslots) {
    if (s->state != N48DF_UNBOUND || nslots < 1 || nslots > N48DF_MAX_SLOTS) return 0;
    s->nslots = nslots; s->acquired = 1; s->state = N48DF_ACTIVE; return 1;
}
// Marks the plane taken without activating (acquire succeeded, a later step failed: the caller must release).
static inline void n48df_mark_acquired(n48df_t *s) { s->acquired = 1; }
// Fail closed. Returns 1 exactly once when the caller must now call release (the plane is taken and no release was requested yet).
static inline int n48df_fail(n48df_t *s, int reason) {
    if (s->state != N48DF_OFF) { s->state = N48DF_OFF; s->reason = reason; }
    if (reason == N48DF_R_ERROR || reason == N48DF_R_PLANELOST || reason == N48DF_R_ALLOC) s->errors++;
    if (s->acquired && !s->released) { s->released = 1; return 1; }
    return 0;
}
// flags[i] = the kernel status flags of the slot registered as local index i (taken from a status read made just now). Returns the slot or -1.
// -1 with the machine not ACTIVE is not a drop. -1 while ACTIVE counts a drop (no slot free: the next frame catches up).
// `avoid` (-1 none) is the chain head: a new frame must not overwrite the slot it is about to read as its base. A pinned slot (base of a frame not yet completed) is never picked.
static inline int n48df_pick_avoid(n48df_t *s, const uint32_t flags[N48DF_MAX_SLOTS], int avoid) {
    if (s->state != N48DF_ACTIVE) return -1;
    for (int k = 1; k <= s->nslots; k++) {
        int i = (s->last + k) % s->nslots;
        if (i < 0) i += s->nslots;
        if (!s->inflight[i] && !s->pin[i] && i != avoid && (flags[i] & N48DF_SLOT_REUSABLE)) { s->inflight[i] = 1; s->last = i; s->picks++; return i; }
    }
    s->drops++; return -1;
}
static inline int n48df_pick(n48df_t *s, const uint32_t flags[N48DF_MAX_SLOTS]) { return n48df_pick_avoid(s, flags, -1); }
// The frame in `slot` is over (completed, failed, skipped or cancelled): the slot is free again and its base is unpinned.
static inline void n48df_release_slot(n48df_t *s, int slot) {
    s->inflight[slot] = 0; s->poison[slot] = 0; s->fid[slot] = 0;
    s->sub_sid[slot] = 0; s->sub_seq[slot] = 0; s->sup_n[slot] = 0; s->made[slot] = 0;   // F2: this slot's own count goes with it; the marks it MADE on older slots stay (made[] is dropped: it can no longer fail)
    for (int k = 0; k < N48DF_MAX_SLOTS; k++) s->made[k] &= (uint8_t)~(1u << slot);
    if (s->basep[slot] >= 0) { if (s->pin[s->basep[slot]]) s->pin[s->basep[slot]]--; s->basep[slot] = -1; }
}
// The command buffer that carried the copy into `slot` ended. vkres 0 = fence VK_SUCCESS. `seq` = its frame sequence, assigned at SUBMISSION (GPU) order.
// Returns 1 when the caller must present the slot now. A frame whose seq is not newer than the last one presented is STALE: the slot is released,
// nothing is presented (an older frame shown after a newer one is the rubber-band) and stale++ counts it.
static inline int n48df_complete_seq(n48df_t *s, int slot, int vkres, uint64_t seq) {
    if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) return 0;
    int poisoned = s->poison[slot]; uint64_t myid = s->fid[slot];
    n48df_release_slot(s, slot);
    if (vkres != 0) {   // the slot content is unknown: every later chained frame that has not completed read (or will read) it through its base -> never present them; the next frame restarts with a full copy
        if (myid) for (int j = 0; j < N48DF_MAX_SLOTS; j++) if (s->inflight[j] && s->fid[j] > myid) s->poison[j] = 1;
        s->gpu_fail++; s->head = -1; return 0;
    }   // the slot content is unknown: the next frame restarts the chain with a full copy
    if (poisoned) { s->chain_invalid++; return 0; }          // fence error / DEVICE_LOST: never present; the kernel's 5 s watchdog restores the console
    if (s->state != N48DF_ACTIVE) return 0;
    if (seq <= s->last_seq) { s->stale++; return 0; }
    s->last_seq = seq; return 1;
}
// bundle 12 (M6 Stage 1a, queue 290 S3): the frame in `slot` copied a surface that is NOT this display's (the kernel's table maps it to another display, or it stayed unknown): the slot is released and the frame is never
// presented, WITHOUT touching last_seq (a skipped frame must not make an older real frame stale) and WITHOUT a GPU-failure count. The slot now holds content that is not the DP's, so the chain restarts exactly as after a
// failed frame: every later chained frame still in flight is poisoned (it read this slot as its base) and the next frame starts with a full copy. The caller decides BEFORE n48df_complete_held, so the slot is still in flight here.
static inline void n48df_skip_content(n48df_t *s, int slot) {
    if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) return;
    const uint64_t myid = s->fid[slot];
    n48df_release_slot(s, slot);
    if (myid) for (int j = 0; j < N48DF_MAX_SLOTS; j++) if (s->inflight[j] && s->fid[j] > myid) s->poison[j] = 1;
    s->m6_skip++; s->head = -1;
}
// #12 superseded skip, bundle 20 (F2). Submission (under the submit lock) of the command buffer that carries the slot copy in `slot` for display surface `sid` (IOSurface id, 0 = unknown: untracked), with sequence `seq`.
// Every OTHER slot that is in flight, was submitted for the same surface and has a LOWER seq is marked as superseded by `seq` (a later cb that writes surface S makes every earlier, not yet presented, frame of S unsafe
// to show: S may be mid-update). The reverse also holds (a lower seq submitted after a higher one is itself superseded). The marks live per in-flight slot (3 x 3), so nothing saturates: the old 8-entry surface table
// filled up after a display rearrangement and from then on nothing was ever superseded. A submission with no slot in flight cannot be tracked (surf_untracked++).
static inline void n48df_unsupersede(n48df_t *s, int slot);
static inline void n48df_submit(n48df_t *s, int slot, uint32_t sid, uint64_t seq) {
    if (!sid) return;
    if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) { s->surf_untracked++; return; }
    n48df_unsupersede(s, slot);                                  // a second submit of the same slot replaces the first (never happens in the bundle; keeps the counts exact)
    s->sub_sid[slot] = sid; s->sub_seq[slot] = seq;
    for (int j = 0; j < N48DF_MAX_SLOTS; j++) {
        if (j == slot || !s->inflight[j] || s->sub_sid[j] != sid || !s->sub_seq[j]) continue;
        if (s->sub_seq[j] < seq) { s->sup_n[j]++; s->made[slot] |= (uint8_t)(1u << j); }       // slot's frame supersedes j's
        else if (s->sub_seq[j] > seq) { s->sup_n[slot]++; s->made[j] |= (uint8_t)(1u << slot); }  // j's frame (already submitted, higher seq) supersedes this one
    }
}
static inline int n48df_is_superseded(const n48df_t *s, int slot) { return slot >= 0 && slot < N48DF_MAX_SLOTS && s->sup_n[slot] > 0; }
// The frame in `slot` failed on the GPU (or is submitted again): every mark it made is taken back (a failed frame shows nothing, so it supersedes nothing); frames already skipped stay skipped.
static inline void n48df_unsupersede(n48df_t *s, int slot) {
    for (int j = 0; j < N48DF_MAX_SLOTS; j++) if ((s->made[slot] >> j) & 1u) { if (s->sup_n[j]) s->sup_n[j]--; }
    s->made[slot] = 0;
}
// Occupancy for the 10 s line: slots carrying a supersede mark, nonscan[] entries in use (scan_id[] is nscan).
static inline int n48df_marked_count(const n48df_t *s) { int n = 0; for (int j = 0; j < N48DF_MAX_SLOTS; j++) n += n48df_is_superseded(s, j); return n; }
static inline int n48df_nonscan_used(const n48df_t *s) { int n = 0; for (int i = 0; i < N48DF_MAX_SURF; i++) n += s->nonscan_skip[i] != 0; return n; }
// Like complete_seq, plus the superseded rule: a frame that is marked superseded (a later frame of the same surface was submitted while it was in flight) is not presented (superseded++, slot released).
// A GPU failure of a frame takes back the marks it made (n48df_unsupersede): frames of that surface still pending then present (the failed later frame never will); frames already skipped stay skipped and the
// screen keeps the last presented frame until the next frame of that surface completes. `sid` is kept for the callers' signature: the marks already carry the surface identity.
static inline int n48df_complete_surf(n48df_t *s, int slot, int vkres, uint64_t seq, uint32_t sid) {
    (void)sid;
    if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) return 0;
    if (vkres != 0) { n48df_unsupersede(s, slot); return n48df_complete_seq(s, slot, vkres, seq); }   // marks taken back BEFORE the slot is released
    if (s->state == N48DF_ACTIVE && seq > s->last_seq && n48df_is_superseded(s, slot)) { n48df_release_slot(s, slot); s->superseded++; return 0; }
    return n48df_complete_seq(s, slot, vkres, seq);
}
// native #12 P3 "present hold". Switch file /private/tmp/n48m-hold (absent = OFF, today's behaviour exactly). A display-surface present waits on the present queue until
// (the command buffer's commit time + H ms) BEFORE the superseded/stale rules above run, so a later command buffer for the same surface (CoreDisplay's G then C) that was
// SUBMITTED within H supersedes it by the existing rule (n48df_complete_surf: seq vs surf_hi[sid], filled by n48df_submit at submission). No second supersede mechanism.
#define N48DF_HOLD_DEFAULT_MS 3
#define N48DF_HOLD_MAX_MS 8
// File content -> H in ms: a decimal integer 1..8 (surrounding white space allowed) is H, anything else (empty, text, 0, 9, negative, 1.5) is the default 3.
static inline int n48df_hold_parse(const char *buf, int n) {
    int i = 0, v = 0, nd = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
    while (i < n && buf[i] >= '0' && buf[i] <= '9' && nd < 4) { v = v * 10 + (buf[i] - '0'); i++; nd++; }
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0)) i++;
    if (nd == 0 || i != n || v < 1 || v > N48DF_HOLD_MAX_MS) return N48DF_HOLD_DEFAULT_MS;
    return v;
}
// How long to wait now (ns): 0 when off or the deadline commit+H has passed; never more than H (a commit time in the future cannot lengthen it).
static inline uint64_t n48df_hold_ns(int on, int hms, uint64_t tcommit, uint64_t now) {
    if (!on || !tcommit) return 0;
    if (hms < 1) hms = N48DF_HOLD_DEFAULT_MS;
    if (hms > N48DF_HOLD_MAX_MS) hms = N48DF_HOLD_MAX_MS;
    uint64_t h = (uint64_t)hms * 1000000ull, dl = tcommit + h;
    if (now >= dl) return 0;
    uint64_t w = dl - now; return w > h ? h : w;
}
// The present decision with the hold. wait(ctx, ns) must return only after ns have passed (the bundle drops its mutex around the sleep). Held only when the frame could
// still present: GPU success, ACTIVE, display surface known, newer than the last presented, and not ALREADY superseded. *waited = ns asked of wait (0 = not held).
// OFF (on == 0) is exactly n48df_complete_surf.
static inline int n48df_complete_held(n48df_t *s, int slot, int vkres, uint64_t seq, uint32_t sid, int on, int hms, uint64_t tcommit, uint64_t now,
                                      void (*wait)(void *, uint64_t), void *ctx, uint64_t *waited) {
    uint64_t w = 0; if (waited) *waited = 0;
    if (on && sid && vkres == 0 && slot >= 0 && slot < N48DF_MAX_SLOTS && s->inflight[slot] && s->state == N48DF_ACTIVE && seq > s->last_seq) {
        if (!n48df_is_superseded(s, slot)) w = n48df_hold_ns(on, hms, tcommit, now);
    }
    if (w && wait) { wait(ctx, w); if (waited) *waited = w; }
    return n48df_complete_surf(s, slot, vkres, seq, sid);
}
// In-order caller (host tests of the plain machine): the next sequence number is implied.
static inline int n48df_complete(n48df_t *s, int slot, int vkres) { return n48df_complete_seq(s, slot, vkres, s->last_seq + 1); }
// CoreDisplay's final display pass is the render pass whose FRAGMENT function is GPUPass (any specialisation keeps the function name; an internal design note).
static inline int n48df_fn_is_final(const char *fragment_name) { return fragment_name && strstr(fragment_name, "GPUPass") != 0; }
static inline int n48df_class(uint32_t m) {
    if (m & N48DF_W_DRAW_FINAL) return N48DF_C_FINAL;
    if (m & N48DF_W_DRAW_COMP) return N48DF_C_RENDER_COMP;
    if (m & N48DF_W_DRAW_OTHER) return N48DF_C_RENDER_OTHER;
    if (m & N48DF_W_BLIT) return N48DF_C_BLIT;
    if (m & N48DF_W_COMPUTE) return N48DF_C_COMPUTE;
    if (m & N48DF_W_PASS) return (m & N48DF_W_CLEAR) ? N48DF_C_CLEAR_ONLY : N48DF_C_PASS_NODRAW;
    return N48DF_C_UNKNOWN;
}
// The rule: a display write is a frame to show ONLY when it contains a render pass on the surface that drew with GPUPass (or the kill switch presentall is on).
// `damage` (D2 switch): a pass that drew with any non-ColorFill, non-GPUPass pipeline (SkyLight composite) is a frame too.
// native #12 scanout rule (damage on or off): final-GPUPass, or a render-composite into a surface whose scanout flag is set (`scan`).
static inline int n48df_should_present(uint32_t m, int presentall, int scan) { return presentall || (m & N48DF_W_DRAW_FINAL) != 0 || (scan && (m & N48DF_W_DRAW_COMP) != 0); }
static inline int n48df_is_scan(const n48df_t *s, uint32_t sid) { if (!sid) return 0; for (int i = 0; i < s->nscan; i++) if (s->scan_id[i] == sid) return 1; return 0; }
// Sets the flag (returns 1 when newly set). Table full: not flagged (never presented as composite); scan_full_refused counts it (bundle 20, F4).
static inline int n48df_scan_set(n48df_t *s, uint32_t sid) {
    if (!sid || n48df_is_scan(s, sid)) return 0;
    if (s->nscan >= N48DF_MAX_SURF) { s->scan_full_refused++; return 0; }
    s->scan_id[s->nscan++] = sid; if (s->nflag_log < 10) s->flag_log[s->nflag_log++] = sid; return 1;
}
static inline int n48df_fn_is_fill(const char *fragment_name) { return fragment_name && strstr(fragment_name, "ColorFill") != 0; }
// The draw bits of one draw by its fragment function name.
static inline uint32_t n48df_draw_bits(const char *fragment_name) {
    if (n48df_fn_is_final(fragment_name)) return N48DF_W_DRAW_FINAL;
    if (n48df_fn_is_fill(fragment_name)) return N48DF_W_DRAW_OTHER | N48DF_W_DRAW_FILL;
    return N48DF_W_DRAW_OTHER | N48DF_W_DRAW_COMP;
}
// Counts the write (P1) and returns whether the caller may take a slot and copy (P2). A refused one is nonfinal[class]++.
// Bundle 20 (F1): `known` = the routing latch is on and the surface was planned NON-TENTATIVELY to this instance, i.e. the kernel's table maps it to this display's pipe: it IS a scanout surface, so a composite write
// into it presents without any table entry (the 8-entry scan_id[] filled after a display rearrangement and from then on every composite write was refused, so the unfinished GPUPass image was shown alone).
// The GPUPass-proxy flag (a final-GPUPass draw targeted it) stays for tentative / latch-off frames (known = 0). Unknown / ambiguous surfaces never reach here with known = 1: they stay non-presentable.
static inline int n48df_account_sid(n48df_t *s, uint32_t m, uint32_t sid, int known) {
    int c = n48df_class(m); s->cls[c]++;
    if ((m & N48DF_W_DRAW_FINAL) && !known) n48df_scan_set(s, sid);
    if (n48df_should_present(m, s->presentall, (known && sid) || n48df_is_scan(s, sid))) return 1;
    s->nonfinal[c]++;
    if (c == N48DF_C_RENDER_COMP && !s->presentall) {      // a composite write into a surface that was never a scanout surface
        s->nonscan_total++; int k = -1;
        for (int i = 0; i < N48DF_MAX_SURF; i++) if (s->nonscan_sid[i] == sid && s->nonscan_skip[i]) { k = i; break; }
        if (k < 0) for (int i = 0; i < N48DF_MAX_SURF; i++) if (!s->nonscan_skip[i]) { k = i; s->nonscan_sid[i] = sid; break; }
        if (k >= 0) s->nonscan_skip[k]++;
    }
    return 0;
}
static inline int n48df_account(n48df_t *s, uint32_t m) { return n48df_account_sid(s, m, 0, 0); }
// Result of the present call that complete() asked for. Returns 1 when the caller must release (fail closed).
static inline int n48df_present_result(n48df_t *s, int rc) {
    if (rc == 0) { s->presents++; return 0; }
    s->present_fail++; return n48df_fail(s, N48DF_R_ERROR);
}
static inline int n48df_inflight_count(const n48df_t *s) { int n = 0; for (int i = 0; i < N48DF_MAX_SLOTS; i++) n += s->inflight[i]; return n; }

// ================================================================ native #12 D1 / D2: dirty rectangles and partial-update emulation ================================================================
// Hypothesis (NATIVE-S5-FLIP.md "Partial-update emulation"): CoreDisplay/SkyLight redraw only a dirty region of a display surface S and rely on the pipe to keep showing the rest
// from the previous scanout. D1 measures the extents; D2 (switch /private/tmp/n48m-damage, default OFF) rebuilds the full frame as  front-slot content + region R of S.
typedef struct { int32_t x0, y0, x1, y1; } n48df_rect_t;     // half-open pixel rectangle, y down (row 0 = top); empty when x1 <= x0 or y1 <= y0
static inline int n48df_rect_empty(n48df_rect_t r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline n48df_rect_t n48df_rect_make(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {   // saturating narrowing so absurd client values cannot wrap
    #define N48DF_SAT(v) ((v) < -(1LL << 30) ? -(1 << 30) : (v) > (1LL << 30) ? (1 << 30) : (int32_t)(v))
    n48df_rect_t r = { N48DF_SAT(x0), N48DF_SAT(y0), N48DF_SAT(x1), N48DF_SAT(y1) };
    #undef N48DF_SAT
    return r;
}
static inline n48df_rect_t n48df_rect_isect(n48df_rect_t a, n48df_rect_t b) {
    n48df_rect_t r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (n48df_rect_empty(r)) r = (n48df_rect_t){ 0, 0, 0, 0 };
    return r;
}
static inline n48df_rect_t n48df_rect_union(n48df_rect_t a, n48df_rect_t b) {   // an empty operand is ignored
    if (n48df_rect_empty(a)) return n48df_rect_empty(b) ? (n48df_rect_t){ 0, 0, 0, 0 } : b;
    if (n48df_rect_empty(b)) return a;
    n48df_rect_t r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
static inline n48df_rect_t n48df_rect_clip(n48df_rect_t r, uint32_t w, uint32_t h) { return n48df_rect_isect(r, (n48df_rect_t){ 0, 0, (int32_t)w, (int32_t)h }); }
static inline int n48df_rect_is_full(n48df_rect_t r, uint32_t w, uint32_t h) { return r.x0 <= 0 && r.y0 <= 0 && r.x1 >= (int32_t)w && r.y1 >= (int32_t)h && w && h; }
static inline int64_t n48df_rect_area(n48df_rect_t r) { return n48df_rect_empty(r) ? 0 : (int64_t)(r.x1 - r.x0) * (r.y1 - r.y0); }
// Pixels a draw can touch: viewport rect (outward rounded) ∩ scissor ∩ the surface. A degenerate viewport/scissor gives an empty rectangle.
static inline n48df_rect_t n48df_draw_rect(double vx, double vy, double vw, double vh, int64_t sx, int64_t sy, int64_t sw, int64_t sh, uint32_t w, uint32_t h) {
    double big = 1e9; if (!(vx > -big && vx < big && vy > -big && vy < big && vw > -big && vw < big && vh > -big && vh < big)) return (n48df_rect_t){ 0, 0, 0, 0 };
    n48df_rect_t v = n48df_rect_make((int64_t)(vx < 0 ? (int64_t)vx - 1 : (int64_t)vx), (int64_t)(vy < 0 ? (int64_t)vy - 1 : (int64_t)vy),
                                      (int64_t)(vx + vw) + 1, (int64_t)(vy + vh) + 1);      // floor / ceil margin: one pixel outward is safe (a bigger R only costs bytes)
    if (vw <= 0 || vh <= 0) return (n48df_rect_t){ 0, 0, 0, 0 };
    if (sw < 0 || sh < 0) return (n48df_rect_t){ 0, 0, 0, 0 };
    n48df_rect_t sc = n48df_rect_make(sx, sy, sx + sw, sy + sh);
    return n48df_rect_clip(n48df_rect_isect(v, sc), w, h);
}
// Per display surface and command buffer: the union of the draw rectangles. `full` = something wrote the whole surface or unknown extents (clear load, blit, compute).
typedef struct { n48df_rect_t r; int full; uint32_t draws, full_draws, part_draws; } n48df_dmg_t;
static inline void n48df_dmg_add_draw(n48df_dmg_t *d, n48df_rect_t r, uint32_t w, uint32_t h) {
    d->draws++; if (n48df_rect_is_full(r, w, h)) d->full_draws++; else d->part_draws++;
    d->r = n48df_rect_union(d->r, r);
}
static inline void n48df_dmg_set_full(n48df_dmg_t *d) { d->full = 1; }
enum { N48DF_K_NONE = 0, N48DF_K_FULL = 1, N48DF_K_PART = 2 };      // NONE: no draw recorded and not full (nothing to say)
// Resolves the accumulator: FULL (also the answer for "unknown": w/h 0), PART with *out the clipped union, or NONE for no draws. A non-empty-draw set that clips to nothing is PART with an empty rectangle.
static inline int n48df_dmg_resolve(const n48df_dmg_t *d, uint32_t w, uint32_t h, n48df_rect_t *out) {
    *out = (n48df_rect_t){ 0, 0, (int32_t)w, (int32_t)h };
    if (!w || !h || d->full) return N48DF_K_FULL;
    if (!d->draws) return N48DF_K_NONE;
    n48df_rect_t c = n48df_rect_clip(d->r, w, h);
    if (n48df_rect_is_full(c, w, h)) return N48DF_K_FULL;
    *out = c; return N48DF_K_PART;
}
// What was written to ONE display surface by one command buffer, besides the class mask.
typedef struct {
    n48df_dmg_t all;          // every draw on the surface (D1 statistics)
    n48df_dmg_t pres;         // the presentable draws only (GPUPass, SkyLight composite; ColorFill excluded) plus any blit/compute/clear-with-draw: the D2 rectangle
    int src_kind; uint32_t src_sid, src_w, src_h;   // the first IOSurface texture a presentable draw sampled: 0 none, 1 other IOSurface (not a display surface), 2 another display surface, 3 the target itself
    int vp_set; double vp[4]; int64_t sc[4];        // viewport x y w h and scissor x y w h of the first draw (diagnostic sample)
} n48df_wr_t;

// ---- the copy-region builder: rows of R clipped to the surface -> VkBufferCopy-shaped regions (same offsets in source and destination, pitch in bytes) ----
typedef struct { uint64_t src, dst, size; } n48df_region_t;
// Returns the number of regions written to out[] (0 for an empty rectangle). One region when the rows are contiguous (full width and pitch == w*bpp), else one per row.
// Returns UINT32_MAX when out[] (max entries) is too small: the caller then copies the whole frame.
static inline uint32_t n48df_regions(n48df_rect_t r, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, n48df_region_t *out, uint32_t max) {
    r = n48df_rect_clip(r, w, h);
    if (n48df_rect_empty(r) || !bpp || (uint64_t)w * bpp > pitch) return 0;
    if (r.x0 == 0 && r.x1 == (int32_t)w && (uint64_t)w * bpp == pitch) {
        if (max < 1) return UINT32_MAX;
        out[0].src = out[0].dst = (uint64_t)r.y0 * pitch; out[0].size = (uint64_t)(r.y1 - r.y0) * pitch; return 1;
    }
    uint32_t n = (uint32_t)(r.y1 - r.y0);
    if (n > max) return UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        out[i].src = out[i].dst = (uint64_t)(r.y0 + (int32_t)i) * pitch + (uint64_t)r.x0 * bpp;
        out[i].size = (uint64_t)(r.x1 - r.x0) * bpp;
    }
    return n;
}

// ---- P6 protected display surfaces: a display texture with no CPU mapping has only a VkImage (no VkBuffer over host memory), so the copy into the scanout slot is vkCmdCopyImageToBuffer ----
// One VkBufferImageCopy-shaped region for rect R of an image of w x h texels into a slot buffer of pitch bytes per row (same byte offset in the slot as the buffer path: y*pitch + x*bpp, so a
// partial image copy lands exactly where the buffer path's region copy does). bufferRowLength = pitch / bpp texels. Returns 1, or 0 when nothing may be copied (empty / off-surface rectangle, or a
// pitch that is not a whole number of texels or is narrower than a row: the caller copies nothing and, for a full frame, refuses the frame beforehand with n48df_img_ok).
typedef struct { uint64_t buf_off; uint32_t row_len, x, y, w, h; } n48df_imgreg_t;
static inline int n48df_img_ok(uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp) { return bpp && w && h && pitch % bpp == 0 && (uint64_t)w * bpp <= pitch; }
static inline int n48df_img_region(n48df_rect_t r, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, n48df_imgreg_t *o) {
    if (!n48df_img_ok(w, h, pitch, bpp)) return 0;
    r = n48df_rect_clip(r, w, h);
    if (n48df_rect_empty(r)) return 0;
    o->buf_off = (uint64_t)r.y0 * pitch + (uint64_t)r.x0 * bpp; o->row_len = pitch / bpp;
    o->x = (uint32_t)r.x0; o->y = (uint32_t)r.y0; o->w = (uint32_t)(r.x1 - r.x0); o->h = (uint32_t)(r.y1 - r.y0);
    return 1;
}
// The allocation size used by the display-surface classifier for a surface with no CPU mapping. IOSurfaceGetAllocSize is a property of the IOSurface object (it needs no mapping) and is used when
// it reports a size; when it reports 0 the rows the plane claims (bpr * height) stand in, so the pitch-vs-plane test (alloc >= pitch * height) judges bytesPerRow alone.
static inline uint64_t n48df_nobase_alloc(uint64_t alloc, uint64_t bpr, uint64_t h) { return alloc ? alloc : bpr * h; }

// ---- D1 accounting: called once per command buffer that wrote a display surface (any class) ----
static inline void n48df_dmg_account(n48df_t *s, int cls, const n48df_wr_t *wr, uint32_t w, uint32_t h) {
    n48df_rect_t r; int k = n48df_dmg_resolve(&wr->all, w, h, &r);
    s->dk[cls][k]++; if (k == N48DF_K_PART) s->dk_area[cls] += (uint64_t)n48df_rect_area(r);
    s->dd_full[cls] += wr->all.full_draws; s->dd_part[cls] += wr->all.part_draws;
    if (cls == N48DF_C_FINAL) s->srck[wr->src_kind & 3]++;
}

// ---- D2: plan one frame (called with the encode-time slot pick; the caller holds the scanout mutex) ----
typedef struct { int slot, base; uint64_t id, baseid; int kind; n48df_rect_t rect; int inst; int tent; } n48df_plan_t;   // inst (bundle 13): 0 = the DP's slot, 2 = the monitor B's (set by the caller); tent: the frame was planned TENTATIVELY (unknown surface)   // base -1: full copy of the surface. kind: FULL, or PART (rect = the region copied from the surface)
static inline void n48df_carry_add(n48df_t *s, int kind, n48df_rect_t r) {
    if (kind != N48DF_K_PART) { s->carry_full = 1; return; }
    n48df_rect_t c = { s->carry[0], s->carry[1], s->carry[2], s->carry[3] };
    c = n48df_rect_union(c, r); s->carry[0] = c.x0; s->carry[1] = c.y0; s->carry[2] = c.x1; s->carry[3] = c.y1;
}
// kind/rect: from n48df_dmg_resolve of the presentable draws. Picks a slot (never the chain head while damage is on) and decides full or partial. Returns the slot or -1 (drop: the damage is carried to the next frame).
// Bundle 13 (R6): `tentative` = the surface was UNKNOWN at encode (planned to instance 0 only tentatively): the frame is a FULL copy (never partial) and NEVER the chain head - a later frame must not chain onto a slot whose content may turn out to be the
// monitor B's. 0 = n48df_plan exactly as before.
static inline int n48df_plan_ex(n48df_t *s, const uint32_t flags[N48DF_MAX_SLOTS], int kind, n48df_rect_t rect, int tentative, n48df_plan_t *p) {
    p->slot = -1; p->base = -1; p->id = 0; p->baseid = 0; p->kind = N48DF_K_FULL; p->rect = rect; p->tent = tentative ? 1 : 0; p->inst = 0;
    if (tentative) kind = N48DF_K_FULL;
    int slot = n48df_pick_avoid(s, flags, s->damage ? s->head : -1);
    if (slot < 0) { if (s->damage && s->state == N48DF_ACTIVE) { n48df_carry_add(s, kind, rect); s->dm_carry++; } return -1; }
    p->slot = slot;
    if (!s->damage) { s->head = -1; s->carry_full = 0; s->carry[0] = s->carry[1] = s->carry[2] = s->carry[3] = 0; return slot; }
    int full = (kind != N48DF_K_PART) || s->carry_full;
    if (!full) { n48df_rect_t c = { s->carry[0], s->carry[1], s->carry[2], s->carry[3] }; rect = n48df_rect_union(rect, c); }
    s->carry_full = 0; s->carry[0] = s->carry[1] = s->carry[2] = s->carry[3] = 0;
    s->chain_id++; p->id = s->chain_id; s->fid[slot] = p->id;
    if (!full && s->head >= 0 && s->head != slot) {
        p->base = s->head; p->baseid = s->head_id; p->kind = N48DF_K_PART; p->rect = rect;
        s->pin[p->base]++; s->basep[slot] = (int8_t)p->base;
        if (n48df_rect_empty(rect)) s->dm_empty++; else s->dm_part++;
    } else { s->basep[slot] = -1; if (full) s->dm_full++; else s->dm_restart++; }
    if (tentative) { s->head = -1; s->head_id = 0; } else { s->head = slot; s->head_id = p->id; }   // R6: a tentative frame is never the base of the next one
    return slot;
}
static inline int n48df_plan(n48df_t *s, const uint32_t flags[N48DF_MAX_SLOTS], int kind, n48df_rect_t rect, n48df_plan_t *p) { return n48df_plan_ex(s, flags, kind, rect, 0, p); }
// Under the submit lock, right before vkQueueSubmit of a frame that carries a chained copy. A frame whose base was not the latest SUBMITTED chained frame (the cb were
// submitted in a different order than they were encoded, or the base was never submitted) read garbage: it is poisoned (never presented) and the chain restarts.
static inline void n48df_chain_submit(n48df_t *s, int slot, uint64_t id, uint64_t baseid) {
    if (!id || slot < 0 || slot >= N48DF_MAX_SLOTS) return;
    int valid = baseid == 0 || (baseid == s->last_sub_id && s->last_sub_valid);
    s->last_sub_id = id; s->last_sub_valid = valid;
    if (!valid) { s->poison[slot] = 1; s->head = -1; }
}
static inline void n48df_count_present_cls(n48df_t *s, int cls) { if (cls >= 0 && cls < N48DF_NCLS) s->pres_cls[cls]++; }
#endif
