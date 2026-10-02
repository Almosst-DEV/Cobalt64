// ws_ident_test.cpp — WindowServer's identity by the OWNER of its VM context (ws_ident.h, 0.0.362), offline.
// The property under test:
//
//     "a frame is judged as WindowServer's only when EXACTLY ONE live context was created by a process named WindowServer
//      that is alive now under that name, whose object re-validates, and the hardware page-table base of the frame's VMID
//      is that context's root, on the VMID recorded for it - never by the VMID's number, never by who came first or last,
//      never with two candidates, and never from a record that outlived its process or its root."
//
// Every scenario runs against an OPS TABLE, so the same assertions judge the real header and each planted defect. The real
// table must pass every check; each mutant must be CAUGHT by at least one named check. The mutants are the plausible ways to
// write this wrong: identity by VMID number (the 0.0.361 bug), identity by timing (the newest context with a root), two
// candidates accepted (take the first), a stale record surviving a restart (the creator's liveness not required; and a
// binding kept once made), no re-validation of the object, the explicit drop ignored, and the VMID never recorded.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/ws_ident_test.cpp -o /tmp/wsidtest && /tmp/wsidtest
#include <cstdio>
#include <cstdint>
#include <vector>
#include "ws_ident.h"

static int gFail = 0, gRun = 0, gQuiet = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-92s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-92s %#llx\n", what, (unsigned long long)got);
    }
}

struct Ops {
    const char *name;
    uint32_t (*update)(n48_ws *, const n48_ws_rec *, uint32_t);
    uint32_t (*frame)(n48_ws *, uint32_t, uint32_t, uint64_t);
    uint32_t (*gone)(n48_ws *, uint32_t, uint64_t);
};

// ---------------------------------------------------------------------------------------------------------------------
// The mutants. Each is the real function with ONE plausible mistake.
// ---------------------------------------------------------------------------------------------------------------------
// M1 IDENTITY BY VMID NUMBER - 0.0.358..0.0.361 as they ran: "VMID 2 is WindowServer's".
static uint32_t m1_frame_vmid2(n48_ws *w, uint32_t vmid, uint32_t, uint64_t)
{
    const uint32_t v = vmid == 2u ? (uint32_t)N48_WSF_JUDGE : (uint32_t)N48_WSF_OTHER;
    w->frames[v]++;
    return v;
}
// A bind helper for the selection mutants: the real update's binding rules over a caller-chosen index.
static uint32_t bind_index(n48_ws *w, const n48_ws_rec *r, uint32_t idx, uint32_t c)
{
    const uint32_t was = w->state;
    w->ncand = c;
    if (idx == 0xFFFFFFFFu) {
        n48_ws_clear_binding(w); w->state = N48_WS_NONE;
        if (was == N48_WS_BOUND) { w->unbinds++; w->gen++; return N48_WSU_UNBIND_NONE; }
        return N48_WSU_STILL_NONE;
    }
    const n48_ws_rec *k = &r[idx];
    const uint64_t root = k->root & N48_WS_ROOT_MASK;
    if (was == N48_WS_BOUND && w->seq == k->seq && w->ctx == k->ctx && w->task == k->task && w->pid == k->pid && w->root == root)
        return N48_WSU_KEEP;
    n48_ws_clear_binding(w);
    w->state = N48_WS_BOUND; w->seq = k->seq; w->ctx = k->ctx; w->task = k->task; w->pid = k->pid; w->root = root;
    w->binds++; w->gen++;
    if (was == N48_WS_BOUND) { w->rebinds++; return N48_WSU_REBIND; }
    return N48_WSU_BIND;
}
// M2 IDENTITY BY TIMING: "WindowServer is the newest context that has a root" (after wskill it is created last - usually).
static uint32_t m2_update_newest(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    uint32_t best = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++)
        if (r[i].live && r[i].id_ok && r[i].root && (best == 0xFFFFFFFFu || r[i].seq > r[best].seq)) best = i;
    return bind_index(w, r, best, best == 0xFFFFFFFFu ? 0u : 1u);
}
// M3 TWO CANDIDATES ACCEPTED: the first candidate wins however many there are.
static uint32_t m3_update_first(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    uint32_t idx = 0xFFFFFFFFu;
    const uint32_t c = n48_ws_select(r, n, &idx);
    return bind_index(w, r, c ? idx : 0xFFFFFFFFu, c);
}
// M4 A STALE RECORD SURVIVES A RESTART (a): the creator's liveness is not required - "it was WindowServer's at create".
static uint32_t m4_update_no_liveness(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    std::vector<n48_ws_rec> c(r, r + n);
    for (auto &e : c) e.ws_now = e.ws_at_create;
    return n48_ws_update(w, c.data(), n);
}
// M5 A STALE RECORD SURVIVES A RESTART (b): a binding, once made, is kept while its record is live (no re-selection).
static uint32_t m5_update_sticky(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    if (w->state == N48_WS_BOUND)
        for (uint32_t i = 0; i < n; i++)
            if (r[i].live && r[i].seq == w->seq && r[i].ctx == w->ctx) return N48_WSU_KEEP;
    return n48_ws_update(w, r, n);
}
// M6 NO RE-VALIDATION: a recycled object (vtable or task changed) still counts.
static uint32_t m6_update_no_revalidate(n48_ws *w, const n48_ws_rec *r, uint32_t n)
{
    std::vector<n48_ws_rec> c(r, r + n);
    for (auto &e : c) e.id_ok = 1u;
    return n48_ws_update(w, c.data(), n);
}
// M7 THE EXPLICIT DROP IGNORED (unmapVA freed the root / the context was released; the binding stays until the next frame).
static uint32_t m7_gone_ignored(n48_ws *, uint32_t, uint64_t) { return 0u; }
// M8 THE VMID NEVER RECORDED: WindowServer's root on any VMID is judged.
static uint32_t m8_frame_no_latch(n48_ws *w, uint32_t vmid, uint32_t hw_ok, uint64_t hw_root)
{
    const uint32_t saved = w->vmid;
    w->vmid = 0u;
    const uint32_t v = n48_ws_frame(w, vmid, hw_ok, hw_root);
    w->vmid = saved;
    return v;
}

static const Ops kReal = { "(the real header)", n48_ws_update, n48_ws_frame, n48_ws_gone };
static const Ops kMut[] = {
    { "M1 identity by VMID number (VMID 2 is WindowServer)", n48_ws_update, m1_frame_vmid2, n48_ws_gone },
    { "M2 identity by timing (the newest context with a root)", m2_update_newest, n48_ws_frame, n48_ws_gone },
    { "M3 two candidates accepted (the first wins)", m3_update_first, n48_ws_frame, n48_ws_gone },
    { "M4 stale record survives a restart (creator liveness not required)", m4_update_no_liveness, n48_ws_frame, n48_ws_gone },
    { "M5 stale record survives a restart (binding kept once made)", m5_update_sticky, n48_ws_frame, n48_ws_gone },
    { "M6 no re-validation of the context object", m6_update_no_revalidate, n48_ws_frame, n48_ws_gone },
    { "M7 the explicit drop (unmapVA / release) ignored", n48_ws_update, n48_ws_frame, m7_gone_ignored },
    { "M8 the VMID never recorded (any VMID naming the root is judged)", n48_ws_update, m8_frame_no_latch, n48_ws_gone },
};

// ---------------------------------------------------------------------------------------------------------------------
// The world: recorded contexts and the hardware's VMID -> page-table base map.
// ---------------------------------------------------------------------------------------------------------------------
struct World {
    std::vector<n48_ws_rec> recs;
    uint64_t hw[16] = { 0 };
    uint32_t hwOk[16] = { 0 };
    void map(uint32_t vmid, uint64_t root) { hw[vmid] = root; hwOk[vmid] = 1u; }
    n48_ws_rec *by_seq(uint32_t seq) { for (auto &r : recs) if (r.seq == seq) return &r; return nullptr; }
};
static n48_ws_rec rec(uint32_t seq, int32_t pid, bool wsCreate, bool wsNow, uint64_t root)
{
    n48_ws_rec r {};
    r.live = 1u; r.seq = seq; r.ctx = 0xffffff86a9870000ull + 0x80ull * seq; r.task = 0xffffff8b76260000ull + 0x400ull * seq;
    r.pid = pid; r.ws_at_create = wsCreate ? 1u : 0u; r.ws_now = wsNow ? 1u : 0u; r.id_ok = 1u; r.root = root;
    return r;
}
// One frame, as the kext runs it: re-select, then judge on the hardware of the frame's VMID.
static uint32_t frame(const Ops &o, n48_ws &w, const World &W, uint32_t vmid)
{
    (void)o.update(&w, W.recs.data(), (uint32_t)W.recs.size());
    const uint32_t v = vmid & 0xFu;
    return o.frame(&w, vmid, v < 16u ? W.hwOk[v] : 0u, v < 16u ? W.hw[v] : 0u);
}

static const uint64_t Rk = 0x3d6c14000ull, Rd = 0x3d6c00000ull, Rw = 0x3d6c0a000ull, Rs = 0x3d6c1f000ull,
                      Ro = 0x3d6c05000ull, Rn = 0x3d6c2a000ull;

static int scenarios(const Ops &o)
{
    const int before = gFail;
    char b[200];
    // ---- S1: hp3, as measured. The draw client's root appears first -> VMID 2; WindowServer 1142 -> VMID 3;
    //      SecurityAgent 1263 -> VMID 4. The boot-time WindowServer 201 never had a root and dies at wskill.
    {
        World W;
        W.recs = { rec(1, 0, false, false, Rk), rec(2, 410, false, false, 0), rec(3, 206, false, false, 0),
                   rec(4, 201, true, true, 0), rec(5, 128, false, false, 0), rec(6, 496, false, false, Rd) };
        W.recs[4].live = 0u;                                          // Installer Progress: created and released at once
        W.map(1, Rk); W.map(2, Rd);
        n48_ws w {}; w.pid = -1;
        std::snprintf(b, sizeof b, "S1 hp3 before wskill: the draw client's VMID-2 frame is NOT judged (no WS root) %s", o.name);
        expect_u(b, frame(o, w, W, 2), N48_WSF_NONE);
        expect_u("S1   ... and WindowServer is not bound", w.state, N48_WS_NONE);
        // wskill: 201 dies (its record is released at +7 s in hp3), 1142 and 1263 are created, roots appear.
        W.by_seq(4)->ws_now = 0u; W.by_seq(4)->live = 0u;
        W.recs.push_back(rec(7, 1142, true, true, Rw));
        W.recs.push_back(rec(8, 1263, false, false, Rs));
        W.map(3, Rw); W.map(4, Rs);
        std::snprintf(b, sizeof b, "S1 hp3 after wskill: WindowServer 1142's VMID-3 frame is JUDGED             %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_JUDGE);
        expect_u("S1   ... bound to create #7 pid 1142, root 0x3d6c0a000", w.state == N48_WS_BOUND && w.seq == 7u && w.pid == 1142 &&
                 w.root == Rw, 1u);
        expect_u("S1   ... and its VMID RECORDED from the hardware as 3", w.vmid, 3u);
        std::snprintf(b, sizeof b, "S1 hp3: the draw client's VMID-2 frame is DROPPED as another client's        %s", o.name);
        expect_u(b, frame(o, w, W, 2), N48_WSF_OTHER);
        std::snprintf(b, sizeof b, "S1 hp3: SecurityAgent 1263's VMID-4 frame is DROPPED as another client's     %s", o.name);
        expect_u(b, frame(o, w, W, 4), N48_WSF_OTHER);
        std::snprintf(b, sizeof b, "S1 hp3: a VMID-0 (kernel) frame is DROPPED                                   %s", o.name);
        expect_u(b, frame(o, w, W, 0), N48_WSF_VMID);
        expect_u("S1 hp3: WindowServer's next frame on VMID 3 is judged again", frame(o, w, W, 3), N48_WSF_JUDGE);
        // the draw client exits: its root is freed and VMID 2 may be handed to someone else - still not WindowServer.
        W.by_seq(6)->root = 0u; W.by_seq(6)->live = 0u; W.map(2, Rs);
        expect_u("S1 hp3: VMID 2 re-used by SecurityAgent's root -> still DROPPED", frame(o, w, W, 2), N48_WSF_OTHER);
    }
    // ---- S2: hp1's order - the restarted WindowServer was the first GFX client, so it WAS on VMID 2. Owner identity must
    //      agree with the old rule where the old rule happened to be right.
    {
        World W;
        W.recs = { rec(1, 0, false, false, Rk), rec(5, 700, true, true, Rw), rec(6, 720, false, false, Rs) };
        W.map(1, Rk); W.map(2, Rw); W.map(3, Rs);
        n48_ws w {}; w.pid = -1;
        std::snprintf(b, sizeof b, "S2 hp1 order: WindowServer on VMID 2 is JUDGED                             %s", o.name);
        expect_u(b, frame(o, w, W, 2), N48_WSF_JUDGE);
        std::snprintf(b, sizeof b, "S2 hp1 order: SecurityAgent on VMID 3 is DROPPED                           %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_OTHER);
    }
    // ---- S3: rw1 - a killed WindowServer's VM context is NEVER torn down: its record stays live with a root on
    //      VMID 2 after the process is gone. The restarted WindowServer's root lands on VMID 3.
    {
        World W;
        W.recs = { rec(1, 0, false, false, Rk), rec(4, 201, true, true, Ro) };
        W.map(1, Rk); W.map(2, Ro);
        n48_ws w {}; w.pid = -1;
        expect_u("S3 rw1: the boot WindowServer 201 on VMID 2 is judged while it lives", frame(o, w, W, 2), N48_WSF_JUDGE);
        W.by_seq(4)->ws_now = 0u;                                     // wskill: pid 201 is gone; its context is NOT released
        std::snprintf(b, sizeof b, "S3 rw1: after the kill, the dead WindowServer's VMID-2 frame is DROPPED       %s", o.name);
        expect_u(b, frame(o, w, W, 2), N48_WSF_NONE);
        W.recs.push_back(rec(7, 1284, true, true, 0));                // the new WindowServer: created, no root yet
        expect_u("S3 rw1: new WindowServer without a root -> still nothing judged", frame(o, w, W, 2), N48_WSF_NONE);
        W.by_seq(7)->root = Rn; W.map(3, Rn);
        std::snprintf(b, sizeof b, "S3 rw1: the NEW WindowServer 1284's VMID-3 frame is JUDGED                   %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_JUDGE);
        expect_u("S3   ... bound FRESH to create #7 pid 1284 with its VMID recorded as 3", w.seq == 7u && w.pid == 1284 &&
                 w.vmid == 3u, 1u);
        std::snprintf(b, sizeof b, "S3 rw1: the OLD record's VMID-2 frame is still DROPPED                       %s", o.name);
        expect_u(b, frame(o, w, W, 2), N48_WSF_OTHER);
    }
    // ---- S3b: the restart inside one binding - the old WindowServer is bound, killed, and the new one's frame arrives
    //      BEFORE the old record has been re-examined by any other frame. Only re-selection per frame catches it.
    {
        World W;
        W.recs = { rec(4, 201, true, true, Ro) };
        W.map(2, Ro);
        n48_ws w {}; w.pid = -1;
        (void)frame(o, w, W, 2);
        W.by_seq(4)->ws_now = 0u;
        W.recs.push_back(rec(7, 1284, true, true, Rn)); W.map(3, Rn);
        std::snprintf(b, sizeof b, "S3b the first frame after a restart: the new WindowServer's is JUDGED        %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_JUDGE);
        std::snprintf(b, sizeof b, "S3b ... and the dead one's is DROPPED                                         %s", o.name);
        expect_u(b, frame(o, w, W, 2) != N48_WSF_JUDGE, 1u);
    }
    // ---- S4: TWO candidates - two live WindowServer contexts with roots (a second device open, or two processes named so).
    {
        World W;
        W.recs = { rec(3, 300, true, true, Rw), rec(9, 300, true, true, Rn) };
        W.map(3, Rw); W.map(5, Rn);
        n48_ws w {}; w.pid = -1;
        std::snprintf(b, sizeof b, "S4 two candidates: the first one's frame is REFUSED (ambiguous)               %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_AMBIGUOUS);
        std::snprintf(b, sizeof b, "S4 two candidates: the second one's frame is REFUSED (ambiguous)              %s", o.name);
        expect_u(b, frame(o, w, W, 5), N48_WSF_AMBIGUOUS);
        W.by_seq(9)->live = 0u;                                       // one of them is released: exactly one again
        expect_u("S4 ... one released -> the remaining one is judged", frame(o, w, W, 3), N48_WSF_JUDGE);
    }
    // ---- S5: re-validation - the bound object is recycled (task changed at the same address).
    {
        World W;
        W.recs = { rec(7, 1142, true, true, Rw) };
        W.map(3, Rw);
        n48_ws w {}; w.pid = -1;
        (void)frame(o, w, W, 3);
        W.by_seq(7)->id_ok = 0u;
        std::snprintf(b, sizeof b, "S5 the bound object fails re-validation -> its frame is DROPPED              %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_NONE);
    }
    // ---- S6: the explicit drop - unmapVA freed WindowServer's root. Between that call and the next frame nothing stale is
    //      left bound; the next frame's selection sees no root.
    {
        World W;
        W.recs = { rec(7, 1142, true, true, Rw) };
        W.map(3, Rw);
        n48_ws w {}; w.pid = -1;
        (void)frame(o, w, W, 3);
        const uint32_t g = o.gone(&w, 7u, W.recs[0].ctx);
        std::snprintf(b, sizeof b, "S6 unmapVA freed the bound root: the record is DROPPED at once             %s", o.name);
        expect_u(b, g == 1u && w.state == N48_WS_NONE, 1u);
        expect_u("S6 ... a drop naming another context is ignored", n48_ws_gone(&w, 8u, 0x1234u), 0u);
        W.by_seq(7)->root = 0u;
        expect_u("S6 ... and with the root gone the next frame is DROPPED", frame(o, w, W, 3), N48_WSF_NONE);
        W.by_seq(7)->root = Rn; W.map(3, Rn);                         // the same context maps again: a NEW root
        expect_u("S6 ... a new root on the same context is bound FRESH and judged", frame(o, w, W, 3), N48_WSF_JUDGE);
        expect_u("S6 ... with the new root", w.root, Rn);
    }
    // ---- S7: WindowServer's root appears on a SECOND VMID after its VMID was recorded.
    {
        World W;
        W.recs = { rec(7, 1142, true, true, Rw) };
        W.map(3, Rw); W.map(5, Rw);
        n48_ws w {}; w.pid = -1;
        expect_u("S7 recorded on VMID 3", frame(o, w, W, 3) == N48_WSF_JUDGE && w.vmid == 3u, 1u);
        std::snprintf(b, sizeof b, "S7 the same root on VMID 5 -> DROPPED (moved)                              %s", o.name);
        expect_u(b, frame(o, w, W, 5), N48_WSF_MOVED);
    }
    // ---- S8: the hardware - an unopenable context and an out-of-range VMID refuse.
    {
        World W;
        W.recs = { rec(7, 1142, true, true, Rw) };
        W.map(3, Rw);
        n48_ws w {}; w.pid = -1;
        std::snprintf(b, sizeof b, "S8 VMID 6's context unreadable -> DROPPED                                 %s", o.name);
        expect_u(b, frame(o, w, W, 6), N48_WSF_HW);
        expect_u("S8 VMID 16 (out of range) -> DROPPED", n48_ws_frame(&w, 16u, 1u, Rw), N48_WSF_VMID);
    }
    // ---- S8b: the kext's context table overflowed - "exactly one" cannot be proven, so nothing is judged.
    {
        World W;
        W.recs = { rec(7, 1142, true, true, Rw) };
        W.map(3, Rw);
        n48_ws w {}; w.pid = -1;
        (void)frame(o, w, W, 3);
        const uint32_t u = n48_ws_update_table(&w, W.recs.data(), (uint32_t)W.recs.size(), 0u);
        expect_u("S8b an incomplete context table -> UNBOUND as ambiguous", u == N48_WSU_UNBIND_AMBIG && w.state == N48_WS_AMBIGUOUS, 1u);
        expect_u("S8b ... and the frame is REFUSED", n48_ws_frame(&w, 3u, 1u, Rw), N48_WSF_AMBIGUOUS);
        expect_u("S8b ... a complete table binds again", n48_ws_update_table(&w, W.recs.data(), (uint32_t)W.recs.size(), 1u), N48_WSU_BIND);
    }
    // ---- S9: a process RENAMED or a pid RE-USED is not WindowServer now.
    {
        World W;
        W.recs = { rec(7, 1142, true, false, Rw), rec(8, 1150, false, true, Rn) };
        W.map(3, Rw); W.map(4, Rn);
        n48_ws w {}; w.pid = -1;
        std::snprintf(b, sizeof b, "S9 created by WindowServer, not WindowServer now -> DROPPED                %s", o.name);
        expect_u(b, frame(o, w, W, 3), N48_WSF_NONE);
        std::snprintf(b, sizeof b, "S9 WindowServer now, not at create -> DROPPED                              %s", o.name);
        expect_u(b, frame(o, w, W, 4), N48_WSF_NONE);
    }
    return gFail - before;
}

// THE PROPERTY, against an oracle written from the rule rather than from the code: over generated worlds and frame sequences,
// a frame is judged iff exactly one candidate exists (as the rule defines one) and the hardware of the frame's VMID names that
// candidate's root and (after the first judged frame of that same binding) the VMID is the recorded one. Never judged in any
// other case - in particular never when there are zero or two candidates.
static int property(const Ops &o, bool print)
{
    const int before = gFail;
    uint64_t x = 0x9E3779B97F4A7C15ull, frames = 0, judged = 0, wrong = 0, falseJudge = 0;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    const uint64_t roots[] = { 0, Rk, Rd, Rw, Rs, Ro, Rn };
    for (uint32_t world = 0; world < 20000u; world++) {
        World W;
        const uint32_t nrec = 1u + (uint32_t)(rnd() % 6u);
        for (uint32_t i = 0; i < nrec; i++) {
            n48_ws_rec r = rec(i + 1u, (int32_t)(100u + rnd() % 4u), (rnd() % 3u) == 0u, (rnd() % 3u) == 0u, roots[rnd() % 7u]);
            r.live = (rnd() % 5u) != 0u; r.id_ok = (rnd() % 6u) != 0u;
            W.recs.push_back(r);
        }
        for (uint32_t v = 0; v < 8u; v++) if (rnd() % 3u) W.map(v, roots[rnd() % 7u]);
        n48_ws w {}; w.pid = -1;
        // oracle state: the binding it expects and the VMID it recorded
        uint32_t oSeq = 0; uint64_t oRoot = 0, oCtx = 0; uint32_t oVmid = 0; bool oBound = false;
        for (uint32_t f = 0; f < 12u; f++) {
            // perturb the world a little between frames
            if (rnd() % 4u == 0u) { n48_ws_rec &r = W.recs[rnd() % W.recs.size()]; r.ws_now ^= 1u; }
            if (rnd() % 6u == 0u) { n48_ws_rec &r = W.recs[rnd() % W.recs.size()]; r.root = roots[rnd() % 7u]; }
            const uint32_t vmid = (uint32_t)(rnd() % 9u);
            uint32_t cands = 0, ci = 0;
            for (uint32_t i = 0; i < W.recs.size(); i++) {
                const n48_ws_rec &r = W.recs[i];
                if (r.live && r.ws_at_create && r.ws_now && r.id_ok && r.root && r.pid > 0) { if (!cands) ci = i; cands++; }
            }
            if (cands == 1u) {
                const n48_ws_rec &c = W.recs[ci];
                if (!oBound || oSeq != c.seq || oCtx != c.ctx || oRoot != c.root) { oBound = true; oSeq = c.seq; oCtx = c.ctx; oRoot = c.root; oVmid = 0; }
            } else { oBound = false; oVmid = 0; }
            bool want = false;
            if (oBound && vmid >= 1u && vmid <= 15u && W.hwOk[vmid] && W.hw[vmid] && W.hw[vmid] == oRoot && (!oVmid || oVmid == vmid)) {
                want = true;
                if (!oVmid) oVmid = vmid;
            }
            const uint32_t v = frame(o, w, W, vmid);
            frames++;
            if (v == N48_WSF_JUDGE) judged++;
            if ((v == N48_WSF_JUDGE) != want) wrong++;
            if (v == N48_WSF_JUDGE && cands != 1u) falseJudge++;
        }
    }
    if (print)
        std::printf("property: %llu generated frames, %llu judged; %llu disagree with the oracle; %llu judged with 0 or 2+ candidates\n",
                    (unsigned long long)frames, (unsigned long long)judged, (unsigned long long)wrong,
                    (unsigned long long)falseJudge);
    expect_u("property: every verdict equals the oracle's", wrong, 0u);
    expect_u("property: nothing is ever judged without EXACTLY ONE candidate", falseJudge, 0u);
    expect_u("property: not vacuous (frames were judged)", judged > 1000u, 1u);
    return gFail - before;
}

int main()
{
    std::printf("== ws_ident.h: WindowServer's identity by the OWNER of its VM context ==\n");
    const int real = scenarios(kReal) + property(kReal, true);
    const int realChecks = gRun;
    std::printf("\n== planted defects: each must be CAUGHT ==\n");
    int caught = 0;
    const int nm = (int)(sizeof(kMut) / sizeof(kMut[0]));
    for (int m = 0; m < nm; m++) {
        const int f0 = gFail, r0 = gRun;
        gQuiet = 1;
        const int f = scenarios(kMut[m]) + property(kMut[m], false);
        gQuiet = 0;
        gFail = f0; gRun = r0;                                        // mutant runs are not counted as checks
        if (f > 0) caught++;
        std::printf("  %-72s %s (%d check(s) fail)\n", kMut[m].name, f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    std::printf("\nws_ident: %d check(s) on the real header, %d failed; %d of %d planted defects caught.\n", realChecks, real,
                caught, nm);
    const bool pass = real == 0 && caught == nm;
    std::printf("%s\n", pass ? "N48-WSID-TEST-PASS" : "N48-WSID-TEST-FAIL");
    return pass ? 0 : 1;
}
