// scanout_full_test.cpp — build 0.0.542 (apple/scanout_full.h): `accel scanout full`, the full-res scanout readback.
// Compile (from the tree root; the dcn41 sources are compiled as C++, as the kext does):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         -I src/navi48-bringup/src/dcn -I src/dcn41 -x c++ src/navi48-bringup/tests/scanout_full_test.cpp \
//         src/dcn41/dcn41_core.c src/dcn41/dcn41_otg.c -o /tmp/sftest && /tmp/sftest \
//         src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/dcn/navi48_dcn.cpp \
//         src/navi48-bringup/src/Navi48UserClient.cpp
// Covers:
//   R. the decision's refusals, in order: no device / a failed read, OTG0 not lit, dcnflip's pattern, a flip IN PROGRESS (pending, or
//      programmed != in use), a restore requested / unfinished / engaged-but-OFF, FOREIGN, the geometry and the 16 MiB cap; flip mode
//      NOT engaged reads A; B read while engaged; the flags (front differs, geometry differs, armed);
//   C. the copy: the gate (source inside the surface, destination exactly the read-back buffer, <= 1 MiB), a fence TIMEOUT ends the
//      capture after ONE submission and retires the buffer (never retried), a ring failure retires too, a refusal does not, fence
//      values exhausted, and a clean run's chunk plan (sizes, offsets, sources);
//   H. the 256-byte header: every field's offset (the decoder's struct string), fill, and the reader's check;
//   D. the read-only display read (navi48_liveraster.h n48lr_scan_surface) over a register file with a COUNTING write callback:
//      the fields decoded, ZERO writes, the offsets equal navi48_dcn.cpp's kOff*;
//   K. source pins on the kext: no DCN write, flip mode's lock TRY-locked, the bounded submission, the gate at the ring, the one caller
//      (inert unless the verb runs), the user client's read selector.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include "scanout_full.h"
#include "navi48_liveraster.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-86s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-86s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *path)
{
    std::string s;
    FILE *f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &hay, const char *needle)
{
    uint32_t c = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) c++;
    return c;
}
static std::string body_of(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

// ---- a world: the console A at vram 0x1000000 (MC 0x8000000000 + it), flip mode's B at vram 0x2000000, 1920x1080 linear ----
static const uint64_t VS = 0x8000000000ull;
static n48_sf_dcn dcn_ok_on(uint64_t mc)
{
    n48_sf_dcn d {};
    d.dcn_ok = 1u; d.lit_otg = 0u; d.pending = 0u; d.fmt = 10u; d.sw_mode = 0u; d.vp_w = 1920u; d.vp_h = 1080u; d.pitch_px = 1920u;
    d.frame_count = 100u; d.primary = mc; d.earliest = mc;
    return d;
}
static n48_sf_ctx ctx_base()
{
    n48_sf_ctx c {};
    c.con_ok = 1u; c.con_w = 1920u; c.con_h = 1080u; c.con_row_bytes = 7680u;
    c.a_off = 0x1000000ull; c.a_mc = VS + c.a_off; c.a_len = 0x7f0000ull;   // 8,323,072 >= 7680 x 1080 = 8,294,400
    c.fm_have_b = 1u; c.b_off = 0x2000000ull; c.b_mc = VS + c.b_off; c.b_len = 7680ull * 1080u;
    return c;
}

// ---- C: a fake engine ----
struct Eng {
    uint32_t fence = 0, subs = 0, retires = 0, retireWhy = 0, copies = 0, answerAt = 0, answer = N48_SF_SUB_LANDED, exhausted = 0;
    uint32_t after = N48_SF_SUB_LANDED;
    std::vector<uint64_t> src, off; std::vector<uint32_t> len;
    uint64_t lastDst = 0;
};
static uint32_t e_fence(void *c) { Eng *e = static_cast<Eng *>(c); return e->exhausted ? 0u : ++e->fence; }
static uint32_t e_submit(void *c, uint64_t s, uint64_t d, uint32_t b, uint32_t f)
{
    Eng *e = static_cast<Eng *>(c);
    (void)f; (void)b;
    e->subs++; e->src.push_back(s); e->lastDst = d;
    if (e->subs > 16u) return N48_SF_SUB_LANDED;           // a planted retry loop ends here (and is then counted)
    return e->subs == e->answerAt ? e->answer : e->after;
}
static void e_retire(void *c, uint32_t why) { Eng *e = static_cast<Eng *>(c); e->retires++; e->retireWhy = why; }
static void e_copy(void *c, uint64_t off, uint32_t b) { Eng *e = static_cast<Eng *>(c); e->copies++; e->off.push_back(off); e->len.push_back(b); }

// ---- D: a register file with a COUNTING write callback ----
struct Regs { uint32_t mem[0x10000]; uint32_t reads, writes; };
static uint32_t r_read(void *c, uint32_t a) { Regs *m = static_cast<Regs *>(c); m->reads++; return a < 0x10000u ? m->mem[a] : 0xdeadbeefu; }
static void r_write(void *c, uint32_t a, uint32_t v) { Regs *m = static_cast<Regs *>(c); m->writes++; if (a < 0x10000u) m->mem[a] = v; }
static void r_delay(void *, uint32_t) {}
static const uint32_t SEG[DCN41_NUM_SEGS] = { 0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00 };
static void light(Regs *m, uint32_t i)
{
    m->mem[0x34c0 + DCN41_OTG_OTG_CONTROL(i)] = 1u;
    m->mem[0x34c0 + DCN41_OTG_OTG_H_BLANK_START_END(i)] = (112u << 16) | 2672u;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_BLANK_START_END(i)] = (38u << 16) | 1478u;
    m->mem[0x34c0 + DCN41_OTG_OTG_H_TOTAL(i)] = 2719u;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_TOTAL(i)] = 1480u;
}

int main(int argc, char **argv)
{
    // ================================================================ R: the decision
    {
        n48_sf_ctx c = ctx_base();
        n48_sf_dcn d = dcn_ok_on(c.a_mc);
        n48_sf_plan p {};
        expect_u("R0 flip mode NOT engaged, A scanned: captured", n48_sf_decide(&d, &c, &p), N48_SF_OK);
        expect_u("R0 ... reads A", p.which, 1u);
        expect_u("R0 ... from the console's MC", p.surf_mc, c.a_mc);
        expect_u("R0 ... 1920 x 4 x 1080 bytes", p.bytes, 7680ull * 1080u);
        expect_u("R0 ... flags: none (flip mode OFF, geometry = console)", p.flags, 0u);
        n48_sf_dcn x = d; x.dcn_ok = 0u;
        expect_u("R1 a device that did not answer: NO_DCN", n48_sf_decide(&x, &c, &p), N48_SF_R_NO_DCN);
        x = d; x.lit_otg = 1u;
        expect_u("R1 OTG1 lit (HUBP0 not the plane): OTG", n48_sf_decide(&x, &c, &p), N48_SF_R_OTG);
        x = d; x.lit_otg = 0xffffffffu;
        expect_u("R1 nothing lit: OTG", n48_sf_decide(&x, &c, &p), N48_SF_R_OTG);
        n48_sf_ctx y = c; y.fm_test_held = 1u;
        expect_u("R2 dcnflip's pattern on screen: TEST_PATTERN", n48_sf_decide(&d, &y, &p), N48_SF_R_TEST_PATTERN);
        // IN PROGRESS
        x = d; x.pending = 1u;
        expect_u("R3 SURFACE_FLIP_PENDING set: FLIP_PENDING (in progress)", n48_sf_decide(&x, &c, &p), N48_SF_R_FLIP_PENDING);
        x = d; x.primary = c.b_mc;
        expect_u("R3 programmed B, still scanning A (latch not taken): FLIP_PENDING", n48_sf_decide(&x, &c, &p), N48_SF_R_FLIP_PENDING);
        x = d; x.pending = 1u; x.lit_otg = 0u; y = c; y.fm_restore_req = 1u;
        expect_u("R3 a pending flip outranks a restore request", n48_sf_decide(&x, &y, &p), N48_SF_R_FLIP_PENDING);
        // RESTORE
        y = c; y.fm_restore_req = 4u;
        expect_u("R4 a restore requested (withdrawal): RESTORE", n48_sf_decide(&d, &y, &p), N48_SF_R_RESTORE);
        y = c; y.fm_a_copy_pending = 1u;
        expect_u("R4 a failed B->A copy pending: RESTORE", n48_sf_decide(&d, &y, &p), N48_SF_R_RESTORE);
        y = c; y.fm_engaged = 1u; y.fm_on = 0u;
        expect_u("R4 engaged but OFF (a failed restore 842 retries): RESTORE", n48_sf_decide(&d, &y, &p), N48_SF_R_RESTORE);
        y = c; y.con_ok = 0u;
        expect_u("R5 no console geometry: NO_CONTEXT", n48_sf_decide(&d, &y, &p), N48_SF_R_NO_CONTEXT);
        // FOREIGN
        x = dcn_ok_on(VS + 0x4000000ull);
        expect_u("R6 scanning neither A nor B: FOREIGN", n48_sf_decide(&x, &c, &p), N48_SF_R_FOREIGN);
        x = dcn_ok_on(c.b_mc); y = c; y.fm_have_b = 0u;
        expect_u("R6 B's address but no B allocated: FOREIGN", n48_sf_decide(&x, &y, &p), N48_SF_R_FOREIGN);
        // B, engaged, flip mode ON; flip mode's front names A (the last present's pre-flip front): recorded, not refused
        x = dcn_ok_on(c.b_mc); y = c; y.fm_on = 1u; y.fm_engaged = 1u; y.fm_front = c.a_mc;
        expect_u("R7 flip mode ON, scanning B: captured", n48_sf_decide(&x, &y, &p), N48_SF_OK);
        expect_u("R7 ... reads B", p.which == 2u && p.surf_mc == c.b_mc && p.surf_off == c.b_off && p.buf_len == c.b_len, 1u);
        expect_u("R7 ... flags ON | ENGAGED | FRONT_DIFFERS (hardware wins, the difference is recorded)", p.flags,
                 N48_SF_F_FM_ON | N48_SF_F_FM_ENGAGED | N48_SF_F_FM_FRONT_DIFFERS);
        y.fm_front = c.b_mc;
        expect_u("R7 ... front agrees: no FRONT_DIFFERS", (n48_sf_decide(&x, &y, &p), p.flags & N48_SF_F_FM_FRONT_DIFFERS), 0u);
        y = c; y.fm_engaged = 0u;
        expect_u("R7 B scanned while NOT engaged: captured, flagged", (n48_sf_decide(&x, &y, &p) == N48_SF_OK && (p.flags & N48_SF_F_B_UNENGAGED)) ? 1u : 0u, 1u);
        // GEOMETRY / BOUNDS
        x = d; x.fmt = 12u;
        expect_u("R8 a 64-bit or unknown format: GEOM", n48_sf_decide(&x, &c, &p), N48_SF_R_GEOM);
        x = d; x.sw_mode = 2u;
        expect_u("R8 SW_MODE 4KB_2D (not decodable here): GEOM", n48_sf_decide(&x, &c, &p), N48_SF_R_GEOM);
        x = d; x.pitch_px = 1900u;
        expect_u("R8 pitch < width: GEOM", n48_sf_decide(&x, &c, &p), N48_SF_R_GEOM);
        x = d; x.vp_h = 1100u;
        expect_u("R8 larger than the buffer it lives in: GEOM", n48_sf_decide(&x, &c, &p), N48_SF_R_GEOM);
        x = d; x.vp_w = 0u;
        expect_u("R8 zero width: GEOM", n48_sf_decide(&x, &c, &p), N48_SF_R_GEOM);
        y = c; y.a_len = 64ull << 20; x = d; x.vp_w = 2560u; x.pitch_px = 4096u; x.vp_h = 1440u;
        expect_u("R8 over the 16 MiB cap (4096 x 4 x 1440 = 22.5 MiB): GEOM", n48_sf_decide(&x, &y, &p), N48_SF_R_GEOM);
        x.pitch_px = 2560u;
        expect_u("R8 2560x1440 (14.1 MiB) fits the cap", n48_sf_decide(&x, &y, &p), N48_SF_OK);
        expect_u("R8 ... and is flagged GEOM_DIFFERS against a 1080p console", p.flags & N48_SF_F_GEOM_DIFFERS, N48_SF_F_GEOM_DIFFERS);
        x = d; x.sw_mode = 3u; y = c; y.a_len = 64ull << 20;
        expect_u("R8 64KB_2D 1920x1080: 15 x 9 blocks of 64 KiB", (n48_sf_decide(&x, &y, &p), p.bytes), 15ull * 9u * 65536u);
        y = c; y.armed = 1u;
        expect_u("R9 an arm stands: captured, flagged ARMED", (n48_sf_decide(&d, &y, &p) == N48_SF_OK && p.flags == N48_SF_F_ARMED) ? 1u : 0u, 1u);
        expect_u("R9 payload bytes: linear pitch x 4 x h", n48_sf_payload_bytes(0u, 2560u, 2560u, 1440u), 2560ull * 4u * 1440u);
        expect_u("R9 payload bytes: SW_MODE 1 unsupported", n48_sf_payload_bytes(1u, 1920u, 1920u, 1080u), 0u);
        expect_u("R9 every reason has a name", std::strcmp(n48_sf_reason_name(N48_SF_R_COUNT - 1u), "?") != 0 &&
                 std::strcmp(n48_sf_reason_name(N48_SF_R_COUNT), "?") == 0 ? 1u : 0u, 1u);
    }
    // ================================================================ C: the copy
    {
        n48_sf_ctx c = ctx_base();
        n48_sf_dcn d = dcn_ok_on(c.a_mc);
        n48_sf_plan p {};
        (void)n48_sf_decide(&d, &c, &p);
        const uint64_t RB = 0x8400100000ull;
        // the gate
        expect_u("C0 gate: first chunk", n48_sf_kick_ok(p.surf_mc, RB, N48_SF_CHUNK_BYTES, &p, RB, N48_SF_CHUNK_BYTES), 1u);
        expect_u("C0 gate: the last byte of the surface", n48_sf_kick_ok(p.surf_mc + p.bytes - 4u, RB, 4u, &p, RB, N48_SF_CHUNK_BYTES), 1u);
        expect_u("C0 gate: one dword past the surface REFUSED", n48_sf_kick_ok(p.surf_mc + p.bytes - 4u, RB, 8u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: below the surface REFUSED", n48_sf_kick_ok(p.surf_mc - 4u, RB, 4u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: destination not the read-back buffer (e.g. the surface itself) REFUSED",
                 n48_sf_kick_ok(p.surf_mc, p.surf_mc, 4096u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: more than the buffer holds REFUSED", n48_sf_kick_ok(p.surf_mc, RB, N48_SF_CHUNK_BYTES + 4u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: a buffer claimed larger than 1 MiB REFUSED", n48_sf_kick_ok(p.surf_mc, RB, 4096u, &p, RB, N48_SF_CHUNK_BYTES * 2u), 0u);
        expect_u("C0 gate: zero / unaligned length REFUSED", n48_sf_kick_ok(p.surf_mc, RB, 0u, &p, RB, N48_SF_CHUNK_BYTES) +
                 n48_sf_kick_ok(p.surf_mc, RB, 6u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: address wrap REFUSED", n48_sf_kick_ok(~0ull - 3u, RB, 8u, &p, RB, N48_SF_CHUNK_BYTES), 0u);
        expect_u("C0 gate: no read-back buffer REFUSED", n48_sf_kick_ok(p.surf_mc, 0u, 4096u, &p, 0u, N48_SF_CHUNK_BYTES), 0u);
        // a clean run
        {
            Eng e; const n48_sf_ops o { &e, &e_fence, &e_submit, &e_retire, &e_copy };
            uint32_t ch = 0;
            expect_u("C1 a clean capture", n48_sf_run(&o, &p, RB, &ch), N48_SF_OK);
            const uint32_t want = (uint32_t)((p.bytes + N48_SF_CHUNK_BYTES - 1u) / N48_SF_CHUNK_BYTES);
            expect_u("C1 ... in ceil(bytes / 1 MiB) chunks (8 for 1080p)", ch, want);
            expect_u("C1 ... one submission and one copy-out per chunk, no retire", e.subs == want && e.copies == want && e.retires == 0u, 1u);
            uint64_t sum = 0; uint32_t order = 1u;
            for (uint32_t i = 0; i < e.copies; i++) {
                sum += e.len[i];
                if (e.off[i] != (uint64_t)i * N48_SF_CHUNK_BYTES || e.src[i] != p.surf_mc + e.off[i]) order = 0u;
            }
            expect_u("C1 ... the chunks cover the surface exactly, in order, source = surface + offset", sum == p.bytes && order ? 1u : 0u, 1u);
            expect_u("C1 ... the last chunk is the remainder", e.len.back(), (uint32_t)(p.bytes - (uint64_t)(want - 1u) * N48_SF_CHUNK_BYTES));
            expect_u("C1 ... every destination is the read-back buffer", e.lastDst, RB);
        }
        // A FENCE TIMEOUT: one submission, retired, never retried, nothing copied out of that chunk
        {
            Eng e; e.answerAt = 3u; e.answer = N48_SF_SUB_TIMEOUT;
            const n48_sf_ops o { &e, &e_fence, &e_submit, &e_retire, &e_copy };
            uint32_t ch = 0;
            expect_u("C2 a fence timeout at chunk 3: FENCE", n48_sf_run(&o, &p, RB, &ch), N48_SF_R_FENCE);
            expect_u("C2 ... exactly 3 submissions (the timed-out chunk is NOT retried)", e.subs, 3u);
            expect_u("C2 ... the buffer retired once, for the timeout", e.retires == 1u && e.retireWhy == N48_SF_SUB_TIMEOUT, 1u);
            expect_u("C2 ... chunk 3 was not copied out", e.copies == 2u && ch == 2u, 1u);
            Eng f; f.answerAt = 1u; f.answer = N48_SF_SUB_TIMEOUT; f.after = N48_SF_SUB_TIMEOUT;
            const n48_sf_ops o2 { &f, &e_fence, &e_submit, &e_retire, &e_copy };
            expect_u("C2 a timeout on the first chunk: FENCE after ONE submission", (n48_sf_run(&o2, &p, RB, &ch) == N48_SF_R_FENCE && f.subs == 1u) ? 1u : 0u, 1u);
        }
        {
            Eng e; e.answerAt = 2u; e.answer = N48_SF_SUB_RING;
            const n48_sf_ops o { &e, &e_fence, &e_submit, &e_retire, &e_copy };
            uint32_t ch = 0;
            expect_u("C3 a ring failure: RING, retired", (n48_sf_run(&o, &p, RB, &ch) == N48_SF_R_RING && e.retires == 1u && e.subs == 2u) ? 1u : 0u, 1u);
            Eng f; f.answerAt = 1u; f.answer = N48_SF_SUB_REFUSED;
            const n48_sf_ops o2 { &f, &e_fence, &e_submit, &e_retire, &e_copy };
            expect_u("C3 refused at the ring (queue busy): REFUSED, NOT retired (nothing was rung)",
                     (n48_sf_run(&o2, &p, RB, &ch) == N48_SF_R_REFUSED && f.retires == 0u && f.subs == 1u) ? 1u : 0u, 1u);
            Eng g; g.exhausted = 1u;
            const n48_sf_ops o3 { &g, &e_fence, &e_submit, &e_retire, &e_copy };
            expect_u("C3 fence values exhausted: FENCES, nothing submitted", (n48_sf_run(&o3, &p, RB, &ch) == N48_SF_R_FENCES && g.subs == 0u) ? 1u : 0u, 1u);
            Eng h;
            const n48_sf_ops o4 { &h, &e_fence, &e_submit, &e_retire, &e_copy };
            n48_sf_plan bad = p; bad.surf_mc = 0u;
            expect_u("C3 a plan the gate refuses: REFUSED before any submission", (n48_sf_run(&o4, &bad, RB, &ch) == N48_SF_R_REFUSED && h.subs == 0u) ? 1u : 0u, 1u);
        }
    }
    // ================================================================ H: the header
    {
        expect_u("H0 sizeof 256", sizeof(n48_sf_hdr), 256u);
        const size_t off[] = { offsetof(n48_sf_hdr, version), offsetof(n48_sf_hdr, hdr_bytes), offsetof(n48_sf_hdr, width),
                               offsetof(n48_sf_hdr, height), offsetof(n48_sf_hdr, pitch_bytes), offsetof(n48_sf_hdr, bpp),
                               offsetof(n48_sf_hdr, dcn_format), offsetof(n48_sf_hdr, sw_mode), offsetof(n48_sf_hdr, surface_mc),
                               offsetof(n48_sf_hdr, primary_mc), offsetof(n48_sf_hdr, payload_bytes), offsetof(n48_sf_hdr, surface_vram_off),
                               offsetof(n48_sf_hdr, which), offsetof(n48_sf_hdr, flags), offsetof(n48_sf_hdr, fm_front),
                               offsetof(n48_sf_hdr, a_mc), offsetof(n48_sf_hdr, b_mc), offsetof(n48_sf_hdr, uptime_us),
                               offsetof(n48_sf_hdr, cal_sec), offsetof(n48_sf_hdr, cal_usec), offsetof(n48_sf_hdr, copy_us),
                               offsetof(n48_sf_hdr, chunks), offsetof(n48_sf_hdr, fnv32), offsetof(n48_sf_hdr, con_w),
                               offsetof(n48_sf_hdr, con_h), offsetof(n48_sf_hdr, con_row_bytes), offsetof(n48_sf_hdr, lit_otg),
                               offsetof(n48_sf_hdr, fc_before), offsetof(n48_sf_hdr, fc_after), offsetof(n48_sf_hdr, seq),
                               offsetof(n48_sf_hdr, writers_during), offsetof(n48_sf_hdr, reserved0),
                               /* build 0.0.543 item E5 (version 2) */
                               offsetof(n48_sf_hdr, vp_x), offsetof(n48_sf_hdr, vp_y), offsetof(n48_sf_hdr, fc63_latched),
                               offsetof(n48_sf_hdr, fc63_why), offsetof(n48_sf_hdr, fc89_state), offsetof(n48_sf_hdr, fc89_mode),
                               offsetof(n48_sf_hdr, reserved) };
        const size_t want[] = { 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 76, 80, 88, 96, 104, 112, 120, 124, 128, 132, 136, 140,
                                144, 148, 152, 156, 160, 168, 172, 176, 180, 184, 188, 192, 196, 200 };
        uint32_t okAll = 1u;
        for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) if (off[i] != want[i]) { okAll = 0u; std::printf("   field %zu at %zu want %zu\n", i, off[i], want[i]); }
        expect_u("H1 every field at the offset scanout2png.py's struct string reads", okAll, 1u);
        n48_sf_ctx c = ctx_base(); c.fm_on = 1u; c.fm_engaged = 1u; c.fm_front = c.a_mc;
        c.fc63_latched = 1u; c.fc63_why = 3u; c.fc89_state = 5u; c.fc89_mode = 3u;   /* build 0.0.543 item E5 */
        n48_sf_dcn d = dcn_ok_on(c.b_mc);
        d.vp_x = 17u; d.vp_y = 29u;
        n48_sf_plan p {};
        (void)n48_sf_decide(&d, &c, &p);
        n48_sf_hdr h;
        std::memset(&h, 0xa5, sizeof h);
        n48_sf_hdr_fill(&h, &d, &c, &p);
        expect_u("H2 magic N48SCAN1", std::memcmp(h.magic, "N48SCAN1", 8) == 0, 1u);
        expect_u("H2 version / header bytes (0.0.543 E5: version 2)", ((uint64_t)h.version << 32) | h.hdr_bytes, (2ull << 32) | 256u);
        expect_u("H2 (0.0.543 E5) the viewport start and the 63/89 state are copied into the header",
                 h.vp_x == 17u && h.vp_y == 29u && h.fc63_latched == 1u && h.fc63_why == 3u && h.fc89_state == 5u && h.fc89_mode == 3u, 1u);
        expect_u("H2 geometry 1920x1080 pitch 7680 bpp 4", h.width == 1920u && h.height == 1080u && h.pitch_bytes == 7680u && h.bpp == 4u, 1u);
        expect_u("H2 format 10, SW_MODE 0", ((uint64_t)h.dcn_format << 32) | h.sw_mode, 10ull << 32);
        expect_u("H2 surface = EARLIEST_INUSE = B; which 2", h.surface_mc == c.b_mc && h.primary_mc == c.b_mc && h.which == 2u, 1u);
        expect_u("H2 flip mode's front and A/B recorded", h.fm_front == c.a_mc && h.a_mc == c.a_mc && h.b_mc == c.b_mc, 1u);
        expect_u("H2 payload bytes and VRAM offset", h.payload_bytes == p.bytes && h.surface_vram_off == c.b_off, 1u);
        expect_u("H2 reserved bytes zero", h.reserved[0] == 0u && h.reserved[55] == 0u && h.reserved0 == 0u, 1u);
        expect_u("H3 the reader accepts header + payload", n48_sf_hdr_check(&h, 256u + p.bytes), 0u);
        expect_u("H3 ... refuses a short transfer", n48_sf_hdr_check(&h, 256u + p.bytes - 4096u), 3u);
        n48_sf_hdr b = h; b.magic[0] = 'X';
        expect_u("H3 ... refuses a bad magic", n48_sf_hdr_check(&b, 256u + p.bytes), 1u);
        b = h; b.version = 3u;
        expect_u("H3 ... refuses another version", n48_sf_hdr_check(&b, 256u + p.bytes), 2u);
        b = h; b.pitch_bytes = 4096u;
        expect_u("H3 ... refuses geometry that does not make the payload", n48_sf_hdr_check(&b, 256u + p.bytes), 4u);
        const uint8_t z[3] = { 'a', 'b', 'c' };
        expect_u("H4 FNV-1a 32 of \"abc\" (the reference value)", n48_sf_fnv32(z, 3), 0x1a47e90bu);
    }
    // ================================================================ D: the read-only display read
    {
        Regs *m = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(m, 0u);
        const uint32_t H = 0x34c0u;
        m->mem[H + DCN41_HUBPREQ_DCSURF_FLIP_CONTROL(0)] = 0x00000000u;
        m->mem[H + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE(0)] = 0x01000000u;
        m->mem[H + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH(0)] = 0xffff0080u;   // high garbage above the 16-bit field
        m->mem[H + DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(0)] = 0x01000000u;
        m->mem[H + DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH(0)] = 0x00000080u;
        m->mem[H + 0x05e5u] = 0xff80u | 10u;     // SURFACE_PIXEL_FORMAT 10 (bits above 6 are other fields)
        m->mem[H + 0x05e7u] = 0xffe0u;           // SW_MODE 0
        m->mem[H + 0x05ebu] = (1080u << 16) | 1920u;
        m->mem[H + 0x05e9u] = (12u << 16) | 34u;   // build 0.0.543 item E5: PRI_VIEWPORT_START y 12, x 34
        m->mem[H + 0x0607u] = 0xffff0000u | 1919u;
        m->mem[H + DCN41_OTG_OTG_STATUS_FRAME_COUNT(0)] = 4242u;
        n48lr_ro ro; std::memset(&ro, 0, sizeof ro);
        // the kext's device has n48lr_wreg_refuse; this one COUNTS writes, so a write anywhere in the reader is seen
        ro.init_rc = dcn41_dev_init(&ro.d, m, &r_read, &r_write, &r_delay, SEG, 0x100000u, 0u);
        ro.state = ro.init_rc == DCN41_OK ? N48LR_READY : N48LR_FAILED;
        n48_sf_dcn s {};
        const Regs before = *m;
        expect_u("D1 the read answers", n48lr_scan_surface(&ro, &s), 1u);
        expect_u("D1 ZERO register writes", m->writes, 0u);
        expect_u("D1 the register file is unchanged", std::memcmp(m->mem, before.mem, sizeof m->mem) == 0, 1u);
        expect_u("D1 OTG0 lit, dcn_ok", s.lit_otg == 0u && s.dcn_ok == 1u, 1u);
        expect_u("D1 EARLIEST_INUSE (the HIGH field masked to 16 bits)", s.earliest, 0x8001000000ull);
        expect_u("D1 PRIMARY", s.primary, 0x8001000000ull);
        expect_u("D1 not pending", s.pending, 0u);
        expect_u("D1 format 10, SW_MODE 0, 1920x1080, pitch 1920", s.fmt == 10u && s.sw_mode == 0u && s.vp_w == 1920u && s.vp_h == 1080u &&
                 s.pitch_px == 1920u, 1u);
        expect_u("D1 OTG0 frame count", s.frame_count, 4242u);
        expect_u("D1 (0.0.543 E5) the viewport start read from 0x05e9 (x 0..15, y 16..31)", s.vp_x == 34u && s.vp_y == 12u &&
                 N48LR_OFF_VIEWPORT_START == 0x05e9u, 1u);
        m->mem[H + DCN41_HUBPREQ_DCSURF_FLIP_CONTROL(0)] = 0x100u;
        expect_u("D2 SURFACE_FLIP_PENDING (bit 8) read", (n48lr_scan_surface(&ro, &s), s.pending), 1u);
        Regs *n = static_cast<Regs *>(std::calloc(1, sizeof(Regs)));
        light(n, 1u);
        n48lr_ro r2; std::memset(&r2, 0, sizeof r2);
        r2.init_rc = dcn41_dev_init(&r2.d, n, &r_read, &r_write, &r_delay, SEG, 0x100000u, 0u); r2.state = N48LR_READY;
        expect_u("D3 OTG1 lit: answers lit_otg 1 and reads no HUBP register", (n48lr_scan_surface(&r2, &s), s.lit_otg == 1u && s.dcn_ok == 1u &&
                 s.earliest == 0u && n->writes == 0u) ? 1u : 0u, 1u);
        n48lr_ro none; std::memset(&none, 0, sizeof none);
        expect_u("D4 an unbuilt device: 0, dcn_ok 0 (-> NO_DCN)", (n48lr_scan_surface(&none, &s) == 0u && s.dcn_ok == 0u) ? 1u : 0u, 1u);
        std::free(n); std::free(m);
    }
    // ================================================================ K: the kext
    {
        const std::string top = slurp(argc > 1 ? argv[1] : nullptr);
        const std::string dcn = slurp(argc > 2 ? argv[2] : nullptr);
        const std::string uc = slurp(argc > 3 ? argv[3] : nullptr);
        expect_u("K0 the three kext sources were read", !top.empty() && !dcn.empty() && !uc.empty(), 1u);
        const std::string f = body_of(top, "static uint32_t navi48_scanout_full(uint32_t mode, uint64_t *v) {");
        const std::string kk = body_of(top, "static uint32_t sf_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {");
        const std::string sb = body_of(top, "static uint32_t sf_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {");
        const std::string cf = body_of(top, "static void sf_ctx_fill(n48_sf_ctx *c) {");
        const std::string rt = body_of(top, "static void sf_retire(void *, uint32_t sub) {");
        const std::string co = body_of(top, "static void sf_copy_out(void *vc, uint64_t off, uint32_t bytes) {");
        const std::string rd = body_of(top, "uint32_t navi48_scanfull_read(uint64_t off, void *dst, uint32_t max, uint64_t *total, uint64_t *seq) {");
        expect_u("K1 the verb's functions exist", !f.empty() && !kk.empty() && !sb.empty() && !cf.empty() && !rt.empty() && !co.empty() && !rd.empty(), 1u);
        // READ-ONLY toward the display: not one DCN-writing call in any of them
        const char *dcnWrites[] = { "fmFlip(", "fmSetExact(", "fmCrcBegin(", "fmCrcEnd(", "fmCrcRead(", "n48dcn::flip(", "n48dcn::vbl(",
                                    "n48dcn::mode(", "n48dcn::bind(", "dcn41_hubp_program_flip(", "dcn41_irq_set(", "dcn41_otg_lock(",
                                    "WREG32", "navi48_reg_write32(", "WBAR0_32(", "bar0_memcpy_to_vram(", "navi48_vram_write_mm(",
                                    "fm_restore_locked(", "fm_engage_locked(", "wreg" };
        uint32_t hits = 0;
        for (const char *w : dcnWrites) hits += count(f, w) + count(kk, w) + count(sb, w) + count(cf, w) + count(rt, w) + count(co, w) + count(rd, w);
        expect_u("K2 no DCN / register / VRAM write in the verb (flip, exact set, CRC, bind, WREG, BAR0, MM writes, restores)", hits, 0u);
        const std::string rs = body_of(dcn, "uint32_t roScanSurface(n48_sf_dcn *s) {");
        expect_u("K2 the display read is the read-only device's (gLr), never the bound one (gDcn)",
                 (count(rs, "return n48lr_scan_surface(&gLr, s);") == 1u && count(rs, "gDcn") == 0u) ? 1u : 0u, 1u);
        expect_u("K2 the display is read only through roScanSurface (3 reads: pre-size, the decision, after the copy)",
                 count(f, "n48dcn::roScanSurface(&d0)") == 2u && count(f, "n48dcn::roScanSurface(&d1)") == 1u ? 1u : 0u, 1u);
        expect_u("K2 the kOff geometry offsets are navi48_dcn.cpp's", (count(dcn, "kOffSurfConfig   = 0x05e5,") == 1u &&
                 count(dcn, "kOffTilingConfig = 0x05e7,") == 1u && count(dcn, "kOffViewportDim  = 0x05eb,") == 1u &&
                 count(dcn, "kOffSurfPitch    = 0x0607,") == 1u && N48LR_OFF_SURF_CONFIG == 0x05e5u && N48LR_OFF_TILING_CONFIG == 0x05e7u &&
                 N48LR_OFF_VIEWPORT_DIM == 0x05ebu && N48LR_OFF_SURF_PITCH == 0x0607u) ? 1u : 0u, 1u);
        // the in-progress refusal: flip mode's lock TRY-locked before anything is copied, held to the end
        const size_t tl = f.find("if (!IOLockTryLock(gFmLock)) { r = N48_SF_R_FLIP_BUSY; goto out; }");
        const size_t run = f.find("r = n48_sf_run(&ops, &p, gFc89RbMc, &chunks);");
        const size_t ul = f.find("if (fmHeld) IOLockUnlock(gFmLock);");
        expect_u("K3 FLIP_BUSY: gFmLock TRY-locked (never waited on), BEFORE the copy, released after it",
                 (tl != std::string::npos && run != std::string::npos && ul != std::string::npos && tl < run && run < ul &&
                  count(f, "IOLockLock(gFmLock)") == 0u) ? 1u : 0u, 1u);
        expect_u("K3 the decision that counts is re-made under gFmLock (after the try-lock)", f.find("r = n48_sf_decide(&d0, &c, &p2);") > tl ? 1u : 0u, 1u);
        expect_u("K3 the fast-copy lock is TRY-locked too", count(f, "if (!fl || !IOLockTryLock(fl))"), 1u);
        expect_u("K3 after the copy the display is re-read: moved or pending = CHANGED",
                 count(f, "if (!d1.dcn_ok || d1.pending || d1.earliest != d0.earliest || d1.primary != d0.primary) r = N48_SF_R_CHANGED;"), 1u);
        // bounded: ONE fastcopy submission per chunk, whose fence wait is n48_fc_submit_wait's (N48_FC_FENCE_TIMEOUT_US from the kick)
        expect_u("K4 each chunk is ONE n48_fc_submit_wait (the bounded fence wait), no loop around it",
                 (count(sb, "n48_fc_submit_wait(&kSfWaitOps, c, srcMc, dstMc, bytes, fence, &w);") == 1u && count(sb, "while") == 0u &&
                  count(sb, "for (") == 0u && count(sb, "goto") == 0u) ? 1u : 0u, 1u);
        expect_u("K4 the wait's callbacks: gScanoutLock, 63's fence dword, the fast copy's delay",
                 count(top, "static const n48_fc_wait_ops kSfWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &sf_kick, &fc_fence_read, &fc_delay };"), 1u);
        expect_u("K4 a timeout retires the read-back buffer and latches 63 (89's own rule)", (count(rt, "n48_fc_fence_outcome(&gFc, sub);") == 1u &&
                 count(rt, "gFc89.rb_retired = 1u;") == 1u && count(rt, "gSfDead = 1u;") == 1u) ? 1u : 0u, 1u);
        // build 0.0.543 item E2: the retirement is counted in 89's own stats and in the verb's, and named on a line
        expect_u("K4 (0.0.543 E2) the retirement counts 89's timeouts / ring failures and the verb's retires, and logs \"63 LATCHED OFF, 89 RETIRED\"",
                 (count(rt, "if (sub == N48_FC_SUB_RING) gFc89S.ring_fail++; else gFc89S.timeouts++;") == 1u && count(rt, "gSfRetires++;") == 1u &&
                  count(rt, "switch 63's fast-copy path LATCHED OFF, switch 89's read-back buffer RETIRED") == 1u) ? 1u : 0u, 1u);
        expect_u("K4 the queue-idle wait is 89's bounded one", count(kk, "n48_fc89_qwait(&kFc89QWait, N48_FC89_QWAIT_US, &w)"), 1u);
        expect_u("K5 the gate is checked AGAIN at the ring (read-back buffer only, source in the surface)",
                 count(kk, "n48_sf_kick_ok(srcMc, dstMc, bytes, c->p, gFc89RbMc, N48_FC_STAGING_BYTES)"), 1u);
        expect_u("K5 the copy-out reads only the read-back buffer", count(co, "memcpy(c->dst + N48_SF_HDR_BYTES + off, gFc89Rb.cpu, bytes);"), 1u);
        // build 0.0.543 item E1: the read-back buffer is NEVER bound or proven by this verb (it holds gFmLock): unbound = STAGING
        expect_u("K5 (0.0.543 E1) the verb never binds or proves 89's buffer (no fc89_rb_bind / fc89_control under gFmLock)",
                 (count(f, "fc89_rb_bind(") == 0u && count(f, "fc89_control(") == 0u) ? 1u : 0u, 1u);
        expect_u("K5 (0.0.543 E1) an unbound buffer is refused STAGING, after gFmLock's try-lock and before the copy",
                 (count(f, "if (!(gFc89Rb.valid() && gFc89RbMc && gFc89.rb_ok)) { r = N48_SF_R_STAGING; goto out; }") == 1u &&
                  f.find("if (!(gFc89Rb.valid() && gFc89RbMc && gFc89.rb_ok)) { r = N48_SF_R_STAGING; goto out; }") > tl &&
                  f.find("if (!(gFc89Rb.valid() && gFc89RbMc && gFc89.rb_ok)) { r = N48_SF_R_STAGING; goto out; }") < run) ? 1u : 0u, 1u);
        // build 0.0.543 item E3: the writers are counted AT THE WRITE (after gScanoutLock, right before the SDMA copy), and the
        // verb samples that counter - never gScanoutCopyCalls, which counts at entry, before the lock
        {
            static const char *const wr[] = { "uint32_t navi48_scanout_copy_vram(", "uint32_t navi48_scanout_copy_tiled(",
                                              "static uint32_t fm_copy_tiled_to(", "static uint32_t fm_copy_linear(",
                                              "uint32_t navi48_scanout_copy_staged(" };
            uint32_t good = 0u;
            for (const char *w : wr) {
                const std::string b = body_of(top, w);
                const size_t lk = b.find("IOLockLock(gScanoutLock);");
                const size_t ct = b.find("if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);");
                const size_t cp = b.find("scanout_sdma_", ct == std::string::npos ? 0 : ct);
                const size_t ul = b.find("IOLockUnlock(gScanoutLock);");
                if (lk != std::string::npos && ct != std::string::npos && cp != std::string::npos && ul != std::string::npos && lk < ct &&
                    ct < cp && cp < ul && count(b, "gScanoutWrites") == 1u) good++;
                else std::printf("   E3: %s does not count at the write\n", w);
            }
            expect_u("K7 (0.0.543 E3) every A/B writer counts gScanoutWrites under gScanoutLock, immediately before its SDMA copy", good, 5u);
            expect_u("K7 (0.0.543 E3) gScanoutWrites is written only there (5 sites)", count(top, "__atomic_fetch_add(&gScanoutWrites,"), 5u);
            expect_u("K7 (0.0.543 E3) the verb samples gScanoutWrites before and after the copy, never gScanoutCopyCalls",
                     (count(f, "w0 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);") == 1u &&
                      count(f, "w1 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);") == 1u && count(f, "gScanoutCopyCalls") == 0u &&
                      f.find("w0 = __atomic_load_n(&gScanoutWrites") < run && f.find("w1 = __atomic_load_n(&gScanoutWrites") > run) ? 1u : 0u, 1u);
            expect_u("K7 (0.0.543 E5) the 63/89 state is read while the fast-copy lock is still held (before `out:` releases it)",
                     (f.find("c.fc63_latched = gFc.latched_off;") > run && f.find("c.fc63_latched = gFc.latched_off;") < f.find("if (fcHeld) IOLockUnlock(fl);")) ? 1u : 0u, 1u);
        }
        // OFF / inert: one caller, reached only by `scanout 9` / `scanout 10`; the reader only by the user client's selector
        expect_u("K6 navi48_scanout_full has ONE call site (modes 9 and 10 of `accel scanout`)", count(top, "navi48_scanout_full(mode, v)"), 1u);
        expect_u("K6 ... and it is guarded by the mode", count(top, "if (mode == N48_SF_MODE || mode == N48_SF_MODE_RELEASE) {\n\t\tst = navi48_scanout_full(mode, v)"), 1u);
        expect_u("K6 no other file calls into the verb's internals", count(top, "sf_ctx_fill(&c)") == 2u && count(uc, "navi48_scanout_full") == 0u ? 1u : 0u, 1u);
        expect_u("K6 the user client's read selector calls only the read-only accessor",
                 (count(uc, "case kNavi48SelReadScanFull: return doReadScanFull(args);") == 1u &&
                  count(uc, "navi48_scanfull_read(args->scalarInput[0], args->structureOutput, max, &total, &seq);") == 1u) ? 1u : 0u, 1u);
        expect_u("K6 the transfer is <= NAVI48_UC_MAX_XFER per call", count(uc, "if (max > NAVI48_UC_MAX_XFER) max = NAVI48_UC_MAX_XFER;"), 3u);
        expect_u("K6 start() only allocates the capture lock", count(top, "if (!gSfLock) gSfLock = IOLockAlloc();"), 1u);
        expect_u("K6 the reader writes nothing but the caller's buffer", (count(rd, "memcpy(dst, gSfBuf + off, n);") == 1u && count(rd, "gSfBuf =") == 0u) ? 1u : 0u, 1u);
    }
    std::printf("\nscanout_full: %d of %d checks failed\n", gFail, gRun);
    return gFail ? 1 : 0;
}
