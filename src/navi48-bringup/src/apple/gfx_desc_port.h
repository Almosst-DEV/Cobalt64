// gfx_desc_port.h - M4-DESC-KEXT-PORT (notes/M4-DESC-KEXT-PORT.md: the KEXT side of the table / inline descriptor
// path that notes/M4-DESC-TABLE-IMPL.md built offline. Two things, both pure, host-tested by tests/gfx_desc_port_test.cpp and
// compiled unchanged by the kext (AppleHardwareHook.cpp) and by the offline replay (tools/m4-xlat/replay.c --kext-desc):
//
//   1. n48_dp_read - the snapshot read xlat12's XLAT12_EXTRA_TABLE_DESC step calls (xlat12_draw_extra.desc_read). It adds only
//      REFUSALS to the reader it is handed: the kext hands it gfxc_read over the FRAME'S OWN GfxcVm - the reader, root and guards
//      the decide path already uses for the IB and the programs (the RAM-range guard on host pages, the VRAM aperture bound on
//      device pages, and a stop at the first page that does not resolve). Here: at most N48_DP_READ_MAX dwords, a dword-aligned
//      VA below 2^48 that does not wrap, and a SHORT read is a FAILED read (gfxc_read's stop at an unresolvable page is how an
//      unmapped record reaches the translator, and it must refuse, never translate a zero-filled tail).
//
//   2. n48_dl - the PRODUCER LEDGER that answers xlat12_draw_extra.desc_tiled_ok: "was the tiled surface at `va` last written, in
//      this VM context, by a colour target we translated in gfx12 swizzle mode `mode`?" (M4-DESC-TABLE.md section 4). A tiled T#
//      translates only with a yes. The discipline, each rule a refusal-direction choice:
//        - entries are written ONLY for a frame whose COMMIT GATE answered N48_CM_OK (n48_cm_gate, gfx_commit.h) - i.e. a frame
//          that was rewritten, read back clean and will execute. A TRANSLATE verdict alone is not enough: at DECIDE, or with the
//          gate refusing, the frame is NEUTERED and writes nothing, so its target is not in the mode we would claim;
//        - entries are KEYED BY VM CONTEXT (our own creation sequence number, which Apple cannot recycle - rootwrite_guards.h's
//          rule); VA alone is ambiguous because every Apple context allocates the same VAs. ctx 0 = unknown: never
//          recorded, never proven;
//        - an unmapVA of a context drops every entry of that context (the surface may be gone or re-used). 0.0.380
//          makes that drop RANGE-ACCURATE behind a switch (n48_dl_unmap_rng, `exact`): the unmap's own [va, size) - which its
//          only caller has always had and threw away. 0.0.382 WITHDRAWS's entry extent as unsafe (the runs'
//          own unmapVA lines nest, and one colour target is covered by a 64-PAGE range) and narrows the rule to the ONE SIDE
//          it knows: KEEP only when the unmap ENDS AT OR BEFORE the entry's base VA. arm10 then killed that rule with its own
//          arithmetic - the entry survived 26 real unmaps and died on the 27th, which began 0xb7000 ABOVE the surface's end
//          and overlapped nothing. 0.0.383 gives the entry a DERIVED extent (n48_dl_extent: the output's own CB_COLOR0_ATTRIB2
//          / INFO / VIEW, through scanout_copy.h's ADDR3_64KB_2D size equation) and restores the TWO-SIDED overlap test on it.
//          An entry for which no extent can be derived keeps 0.0.382's one-sided answer - FAIL CLOSED, counted apart.
//          DEFAULT OFF = the wholesale drop;
//        - 0.0.382, behind a third switch: an unmap that finds the lock busy is DEFERRED, not turned into a
//          wholesale epoch bump - its event is already in ws_resprov.h's clear ring, and the next frame under the lock drains
//          it with the same per-entry rule (n48_dl_drain). The drain REPLACES the busy branch's epoch bump; beside it, it is
//          worthless. DEFAULT OFF = the epoch bump, unchanged;
//        - 0.0.380, behind a second switch: an entry also carries the BASE PHYSICAL PAGE its VA resolved to when it
//          was recorded, and an answer requires that page to be unchanged - CONFIRMED Apple's allocator re-maps a VA
//          mid-run. DEFAULT OFF = the (ctx, VA, mode) key, unchanged. The two are ONE unit: an exact drop without the key keeps
//          the WRONG entry, and the key without the exact drop has nothing left to answer about;
//        - 0.0.394, behind a fourth switch (`gfxneuter 29`, n48_dl.keep): a COVERING unmap no longer
//          withdraws the entry. It is KEPT and marked withdrawn-by-unmap, its recorded page untouched, and the key-on ask above
//          decides - proven only if the re-map put the SAME page back (the fill's own pixels), refused as `keyMoved` if it
//          moved. This is what makes arm19b's ledger askable: WindowServer's own unmap of the fill's whole surface range 8 s
//          after the commit emptied it 13 s before the first consumer that sampled our surface. DEFAULT OFF = the drop, and
//          the switch is only meaningful with the exact drop (17) on - with 17 off the whole context still drops wholesale;
//        - any frame handed to Apple UNTRANSLATED, a change of the arm level, and an unmap that could not take the lock all move
//          an EPOCH; a ledger whose epoch or arm differs from the caller's is cleared before it answers anything;
//        - only CB0 is modelled: a translated segment writing a non-zero CB1-7 base clears the ledger (as the replay's does).
//      NOT modelled (SUSPECTED, named in the notes): CPU and SDMA writers of a surface; a partial unmap that misses the base VA
//      is covered only by the whole-context drop - so with the exact drop ON it is NOT covered at all (n48_dl_unmap_rng's own
//      caveat); a PARTIAL re-map that leaves the base page alone and moves a later page of the surface ('s own caveat,
//      n48_dl_tiled_ok_pg).
//
// Plain C11 / C++17, no libc, no kernel types; loops, never struct assignment of the ledger (a large struct copy lowers to
// memcpy under -mkernel, notes: kextcheck's d_pair_note).
#ifndef N48_GFX_DESC_PORT_H
#define N48_GFX_DESC_PORT_H

#include <stdint.h>
#include "gfx_xlat_verdict.h"
#include "gfx_commit.h"
#include "ws_resprov.h"   /* 0.0.382: the CLEAR RING the deferred drain reads (n48_rp_clog). No new type. */
#include "scanout_copy.h" /* 0.0.383 (THE ENTRY EXTENT): n48_addr3_64kb_2d_bytes_4bpe - THIS PROJECT'S OWN ADDR3_64KB_2D size
                           * equation, already host-tested by tests/scanout_copy_test.cpp and already the one the detile path
                           * refuses to work without (n48_scanout_plan_tiled: `if (swizzle != 3u) return kScanTiledSwizzle;`).
                           * It is REUSED here, never re-derived: one equation, one self-test. No new type, no kernel header. */

/* ---- 1. the snapshot read ------------------------------------------------------------------------------------------------ */
typedef uint32_t (*n48_dp_reader)(const void *vm, uint64_t va, uint32_t *dst, uint32_t n);
#define N48_DP_READ_MAX 8u   /* the table step reads 2 (a table pointer), 4 (an S#) or 8 (a T#) dwords */

/* 1 = all `ndw` dwords were read through `rd` over `vm`; 0 = refused (out is then all zero). */
static inline int n48_dp_read(n48_dp_reader rd, const void *vm, uint64_t va, uint32_t ndw, uint32_t *out)
{
    if (!out) return 0;
    for (uint32_t k = 0; k < ndw && k < N48_DP_READ_MAX; k++) out[k] = 0u;
    if (!rd || !vm || ndw == 0u || ndw > N48_DP_READ_MAX) return 0;
    if ((va & 3ull) || va >= (1ull << 48) || va + 4ull * ndw > (1ull << 48)) return 0;
    const uint32_t got = rd(vm, va, out, ndw);
    if (got != ndw) {
        for (uint32_t k = 0; k < ndw; k++) out[k] = 0u;
        return 0;
    }
    return 1;
}

/* ---- 2. the producer ledger ---------------------------------------------------------------------------------------------- */
#define N48_DL_MAX 64u
#define N48_DL_PAGE 0x1000ull
/* `page` (0.0.380,  - THE PHYSICAL-PAGE KEY): the BASE 4 KiB PHYSICAL PAGE the entry's VA resolved to in the frame that
 * recorded it, as that frame's own walk already found it. 0 = the feed supplied none, which is not a key and proves nothing. */
/* `flags` (0.0.382) was the `pad` word. N48_DL_F_CAPPED = this entry was recorded with NO base page while the
 * feeding frame's own colour-target rows were FULL (the kext's kXdTgtHold = 8, hVa[]/hPage[] taking CB0-CB7 across every IB of
 * the frame). It exists so a run cannot misread OUR CAP as Apple: a `keyNoPage` refusal carrying this flag is a target the
 * frame named past the eighth, not a target whose walk failed. */
#define N48_DL_F_CAPPED 1u
/* `flags` (0.0.394,  item 1): N48_DL_F_WITHDRAWN = the entry's own range was COVERED by an unmapVA while
 * `gfxneuter 29` was on, so instead of dropping it the ledger KEPT it and marked it withdrawn-by-unmap, its recorded
 * physical page untouched. The key-on ask then decides: proven only if the asking frame maps the SAME base page (the
 * fill's own pixels), refused as `keyMoved` if it moved. Set only in n48_dl_unmap_rng, only while the switch is on, and
 * cleared by the next n48_dl_set of the same (ctx, VA) - a re-feed is a fresh proof. */
#define N48_DL_F_WITHDRAWN 2u
/* `flags` (build 0.0.485, switch 58 - L1 of notes/design/LOGIN-SCREEN-PATH.md): N48_DL_F_MID = this entry's base page
 * was looked up in a PER-DRAW colour-target row (n48_dl_pgx_extend below), not in one of the frame's IB-END rows (hVa/hPage).
 * It changes no answer: the ask reads `page`, never this bit. It exists so the ledger's own report can say how many proofs
 * the switch added, and so an un-feed that treated such an entry differently would be a visible, testable defect. */
#define N48_DL_F_MID 4u
/* `size` (0.0.383 - THE ENTRY EXTENT, and it is the fix for the defect arm10's own arithmetic proved). The entry's surface in
 * BYTES from its base VA, DERIVED at the feed from the registers the translated output itself writes for that target - never
 * invented. 0 = NO EXTENT COULD BE DERIVED, which is not an extent and proves nothing: the drop rule then behaves exactly as
 * 0.0.382's one-sided rule did (drop on any unmap that does not end at or below the base). See n48_dl_extent below. */
/* `tok` / `arm_ep` (0.0.390,  "Ledger additions: producer token seq + arm epoch per entry"). The COMMIT token
 * seq of the frame that PRODUCED this surface, and the ledger's arm epoch at that moment. They are RECORDED, never read by
 * n48_dl_tiled_ok_pg: an entry that is present is already of this arm and this epoch, because n48_dl_sync clears the whole
 * ledger when either moves. What they buy is that a report can NAME the producer of the surface a consumer was allowed to
 * sample -'s item (b) was "no log line prints the committing frame's ctxSeq" - and that a second committed frame can
 * be told from the first in a two-frame chain. 0 = the feed supplied none, which is not a producer identity. */
typedef struct { uint64_t ctx, va, page, size; uint32_t mode, flags, tok, arm_ep; } n48_dl_ent;
typedef struct {
    uint32_t n, arm, epoch, synced;
    uint32_t keep;                 /* 0.0.394: `gfxneuter 29` mirrored into the ledger, so BOTH the locked
                                    * drop and the deferred drain honour the keep without a new parameter at every call site.
                                    * 0 = OFF (the default, and the boot value): today's drop, byte for byte. */
    uint32_t feedTok, feedArmEp;   /* 0.0.390: the producer identity of the feed IN PROGRESS; 0 outside n48_dl_feed */
    n48_dl_ent e[N48_DL_MAX];
    /* counters, never cleared by n48_dl_clear */
    uint64_t feeds, fedOk, refusedGate, refusedCtx, added, overflow, clears, unmaps, unmapDropped, asked, proven;
    /* 0.0.380: what the two ledger-correctness switches DID, so a run can tell a narrowing that acted from one that
     * never fired. unmapKept = entries of the unmapped context the exact drop left standing; unmapNoRange = exact drops that fell
     * back to the wholesale rule because the unmap's size or the entry's extent was unknown; keyNoPage / keyMoved = asks that
     * matched (ctx, va, mode) and were still refused because a base page was missing, or had MOVED. */
    uint64_t unmapKept, unmapNoRange, keyNoPage, keyMoved;
    /* 0.0.394: what the KEEP-ACROSS-UNMAP decision did. keptAcross = entries a COVERING unmap would
     * have withdrawn that the switch KEPT and marked withdrawn (invariant 0 while the switch is off). provenRemap /
     * movedRemap = asks that hit such a marked entry: the physical-page key PROVED the same page (the fill's own pixels),
     * or REFUSED because the page MOVED after the re-map. Both are 0 while the switch is off, so the off path answers and
     * counts exactly as 0.0.393 does. */
    uint64_t keptAcross, provenRemap, movedRemap;
    /* 0.0.382: THE DEFERRED DRAIN's own scoreboard, and the cap's. drains = n48_dl_drain calls; drainEv = clear-ring
     * events actually applied PER ENTRY (the whole point: these are the unmaps that, before this, moved the epoch and wiped every
     * context); drainWiped = drains that FAILED CLOSED and wiped, split by why (wrap / torn slot / a whole-table WindowServer
     * event). pgCapped / keyCapped are OUR 8-target cap, counted apart from keyNoPage so `keyNoPage >> keyMoved` can be read. */
    uint64_t drains, drainEv, drainWiped, drainWrap, drainTorn, drainWs, pgCapped, keyCapped;
    /* 0.0.383 (THE ENTRY EXTENT): so a run can tell "kept by extent" from "dropped for want of one", which is exactly the
     * distinction arm10 could not make. entExtent / entNoExtent = entries recorded WITH and WITHOUT a derived extent (counted
     * at n48_dl_set, so they are a feed statistic). unmapKeptExtent = drops the SECOND disjunct alone saved - the unmap began
     * at or after the entry's END, which the one-sided rule could not see and which is the 27th unmap of arm10's run.
     * unmapNoExtent = entries dropped because they carried NO extent and the unmap did not end at or below their base: the
     * FAIL-CLOSED path, and the number that says how much of the run is still 0.0.382's behaviour. */
    uint64_t entExtent, entNoExtent, unmapKeptExtent, unmapNoExtent;
    /* C5 part 1 (hygiene, notes/design/C5-CONTINUOUS.md Q4) — THE UN-FEED (n48_dl_unfeed_tok). unfeedAsks = calls made
     * (one per keystone-withdrawn / token-mismatched / never-run commit); unfeedRemoved = entries actually taken back
     * out, summed across every call. unfeedRemoved can be 0 on a call whose frame fed nothing this ledger ever held
     * (e.g. `dp` was off, or every target it named already refused a key) - that is not a defect, it is the call
     * finding nothing of its own to take back. */
    uint64_t unfeedAsks, unfeedRemoved;
    /* 0.0.446 ( fix (5)) — THE UN-FEED QUEUE OVERFLOWED, SO THE LEDGER WAS CLEARED (n48_dl_unfeed_drain).
     * A token the queue could not hold is an un-feed that will never be applied: some withdrawn / mismatched / NOPed
     * frame's entries would stay standing as proof of pixels it never drew. Fail closed: the drain that finds the
     * queue lost a token clears the WHOLE ledger (n48_dl_clear) and counts it here. Never cleared by n48_dl_clear. */
    uint64_t unfeedOverflowClears;
    /* CONDUCTOR REVIEW OF 0.0.446 (item 5), build 0.0.447 — A6 FAIL-CLOSED. reFeedDropped = re-feeds of an
     * EXISTING (ctx, va) entry whose mode, page or size DIFFERED from what the entry already held: the entry is
     * DROPPED (n48_dl_set below), not kept with its fields overwritten and the first producer's `tok` left
     * attached to a shape that producer never actually wrote. A re-feed whose mode/page/size MATCH is unaffected -
     * it is not counted here, and the entry's `tok`/`arm_ep` stay exactly as 0.0.446 left them. */
    uint64_t reFeedDropped;
    /* build 0.0.485 (switch 58, notes/design/LOGIN-SCREEN-PATH.md "The ledger wall (L1, L2)").
     * `replace` - the switch mirrored into the ledger by its ONE caller (the kext's feed site, under gXdLock), exactly as
     * `keep` mirrors 29. 0 (the default and the boot value) = the A6 drop above, byte for byte. 1 = a re-feed of an existing
     * (ctx, va) whose mode, page or size DIFFERS REPLACES the entry with the re-feeding producer's (mode, page, size, flags,
     * tok, arm_ep) - see n48_dl_set. reFeedReplaced counts those. A re-feed that arrives outside a feed (feedTok 0: no
     * producer identity, so nothing could ever un-feed it) is still DROPPED and counted in reFeedDropped.
     * midRows / midPgRes / midPgUnres / midOver - n48_dl_pgx_extend's account of the PER-DRAW rows it added to a committed
     * frame's page map (L1): rows added, of which the page resolved / did NOT resolve (fed as page 0 = unprovable, never
     * guessed), and per-draw targets it could not hold (the per-draw pass's own 32-target table, or this map's room).
     * midFed / midFedKeyed - ledger entries (n48_dl_from_output) whose page lookup was answered by a PER-DRAW row, and of
     * those the ones that got a non-zero page. All eight are 0 on every boot that never throws 58. Never cleared. */
    uint32_t replace;
    uint64_t reFeedReplaced, midRows, midPgRes, midPgUnres, midOver, midFed, midFedKeyed;
} n48_dl;

static inline void n48_dl_clear(n48_dl *l) { if (l->n) l->clears++; l->n = 0u; }

/* Before the ledger is asked or fed on a frame: an arm or epoch different from the one its entries were recorded under clears it. */
static inline void n48_dl_sync(n48_dl *l, uint32_t arm, uint32_t epoch)
{
    if (!l->synced || l->arm != arm || l->epoch != epoch) { n48_dl_clear(l); l->arm = arm; l->epoch = epoch; l->synced = 1u; }
}

/* desc_tiled_ok's answer: 1 only for an entry of the SAME context, VA and gfx12 mode - and, with `key` set, the SAME BASE
 * PHYSICAL PAGE (0.0.380).
 *
 * WHY THE PAGE. CONFIRMED that Apple's own allocator re-maps a VA mid-run (on arm9 and arm8, 0x400800000 -> 0x10030000 then
 * -> 0x13aa0000): the ledger's (ctx, VA, mode) key cannot see that, so an entry recorded for the OLD page answers yes for the NEW
 * one, and a tiled T# translates against bytes a different allocation wrote. The key closes that:
 *   key 0 (DEFAULT, today's behaviour): `page` is not read at all - the answer is exactly (ctx, VA, mode) as before;
 *   key 1: the entry's base page and the ASKING frame's own base page for that VA must be equal, and a missing page on EITHER
 *          side is NOT a match (no key = no proof). Both refusals are counted apart, because `keyMoved` IS the re-map.
 * CAVEAT, and it is's own (SUSPECTED, not modelled): a PARTIAL re-map that leaves the BASE page where it is and moves a
 * later page of the surface slips through this key unchanged. The key is a base-page identity, not a surface identity; it sits
 * BESIDE the exact drop below, never instead of it. */
static inline int n48_dl_tiled_ok_pg(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t page, uint32_t key)
{
    l->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++)
        if (l->e[k].ctx == ctx && l->e[k].va == va && l->e[k].mode == mode) {
            if (key) {
                if (!page || !l->e[k].page) {                                  /* no key on one side: nothing is proven */
                    l->keyNoPage++;
                    /* 0.0.382: and WHOSE fault it is. A refusal on an entry recorded under a FULL target map is our own
                     * kXdTgtHold = 8 cap, not Apple re-mapping anything - counted apart so it cannot be read as a finding. */
                    if (l->e[k].flags & N48_DL_F_CAPPED) l->keyCapped++;
                    return 0;
                }
                if (l->e[k].page != page)   {                                  /*: the VA was re-mapped under us */
                    l->keyMoved++;
                    /* 0.0.394: and if this entry is one the keep held across a covering unmap, THIS is the
                     * answer the design is for - the re-map moved the surface off the page we recorded, so no proof. */
                    if (l->e[k].flags & N48_DL_F_WITHDRAWN) l->movedRemap++;
                    return 0;
                }
            }
            /* 0.0.394: a marked entry answered by the SAME page - the re-map put the fill's own pixels back. */
            if (l->e[k].flags & N48_DL_F_WITHDRAWN) l->provenRemap++;
            l->proven++;
            return 1;
        }
    return 0;
}
/* The un-keyed ask, bit for bit what it has always been (the offline replay calls this one). */
static inline int n48_dl_tiled_ok(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode)
{
    return n48_dl_tiled_ok_pg(l, ctx, va, mode, 0ull, 0u);
}

/* build 0.0.488 (notes/design/DCC-DESC.md Q2 option (A), switch 60) — THE ONLY PROOF A STRIPPED DCC T# MAY HAVE.
 * xlat12's XLAT12_EXTRA_DCC_STRIP admits a stripped DCC record only through ex->desc_dcc_ok; the kext's gfxsrc_desc_dcc_ok
 * answers it through THIS function and nothing else. It asks the two lists that can only hold surfaces written by OUR
 * translated colour targets (whose CB never had DCC - repack_CB_COLOR0_INFO keeps no DCC_ENABLE and CB_COLOR0_DCC_BASE is
 * ABSENT), in gfxsrc_desc_tiled_ok's own order and with its own arguments: the producer ledger (with the physical-page key
 * exactly as the caller computed it), then - only while switch 45 is on - the frame-local list (un-keyed, as there). It
 * takes NO residency-provenance table: a verified residency COPY of Apple's bytes can never vouch for a DCC surface, because
 * those bytes would be Apple's gfx10 DCC encoding. Returns N48_DL_DCC_LEDGER / N48_DL_DCC_FRAMELOCAL, or 0 (not proven). */
enum { N48_DL_DCC_NONE = 0u, N48_DL_DCC_LEDGER = 1u, N48_DL_DCC_FRAMELOCAL = 2u };
static inline uint32_t n48_dl_dcc_ok(n48_dl *led, n48_dl *fl, uint32_t flOn, uint64_t ctx, uint64_t va, uint32_t mode,
                                     uint64_t page, uint32_t key)
{
    if (led && n48_dl_tiled_ok_pg(led, ctx, va, mode, page, key)) return N48_DL_DCC_LEDGER;
    if (flOn && fl && n48_dl_tiled_ok(fl, ctx, va, mode)) return N48_DL_DCC_FRAMELOCAL;
    return N48_DL_DCC_NONE;
}
/* build 0.0.488 — switch 60's report line (the kext prints it from `gfxneuter 60`), here so tests/gfx_desc_port_test.cpp
 * can bound it under n48log's 491-byte body cap at widest numerics and longest strings. args: state (%s), how (%s), the
 * inert note (%s), then segsOn, stripped, provenLed, provenFl, unproven, refused, asks (u64). */
#define N48_DL_DCC60_REPORT_FMT \
    "dccstrip60: switch 60 (DCC T# strip, table path) %s (%s)%s; segments %llu; T# stripped %llu, proven by ledger " \
    "%llu, by frame-local %llu, REFUSED unproven %llu; DCC T# refused (shape/port) %llu; dcc asks %llu."

/* 0.0.383 — THE ENTRY EXTENT, DERIVED AT THE FEED FROM THE OUTPUT'S OWN REGISTERS.
 *
 * WHY THIS EXISTS. 0.0.382 had no surface size anywhere on this path, so it narrowed the unmap rule to the one
 * side it knew: keep only when the unmap ENDS AT OR BEFORE the entry's base VA. arm10 then proved that rule wrong with its own
 * arithmetic - the entry at (ctx 5, VA 0x400800000) survived 26 real unmaps and was killed by the 27th, `va 0x4010a0000
 * size 0x10000`, which begins 0xb7000 ABOVE the end of a 1920x1080 BGRA surface and overlaps NOTHING. The rule drops on ANY
 * higher-address unmap because the entry has no recorded size. This gives it one.
 *
 * WHERE THE NUMBERS COME FROM, and none of them is invented (CONFIRMED against notes/logs/runs/arm10/capture.bin, IB F1 #0 at
 * VA 0x4000d0000, the frame that fed the ledger): the gfx10.3 source IB writes CB_COLOR0_ATTRIB2 = 0x01dfc437 in the SAME
 * SET_CONTEXT_REG packet run as CB_COLOR0_BASE = 0x04008000, and xlat12 REPACKS it (xlat12_tables.h: gfx10.3 0x28ec0 ->
 * gfx12 0x28c78, XLAT12_CLS_FIELD_REPACK), so the TRANSLATED OUTPUT this function already parses carries it:
 *   gfx12 CB_COLOR0_ATTRIB2 0x28c78: MIP0_HEIGHT [15:0], MIP0_WIDTH [31:16], both stored as (value - 1)
 *                                    -> 1079 / 1919 -> 1080 x 1920. (xlat12_repack.h repack_CB_COLOR0_ATTRIB2.)
 *   gfx12 CB_COLOR0_INFO     0x28ec0: FORMAT [4:0] -> 10 = COLOR_8_8_8_8 -> 4 bytes per element.
 *   gfx12 CB_COLOR0_ATTRIB3  0x28c7c: COLOR_SW_MODE [17:15] -> 3 = ADDR3_64KB_2D; MIP0_DEPTH [13:0] -> 0;
 *                                     RESOURCE_TYPE [25:24] -> 1 (2D).
 *   gfx12 CB_COLOR0_VIEW     0x28c64: SLICE_START [13:0] / SLICE_MAX [27:14] -> 0 (one slice).
 * 1920 x 1080 x 4 = 0x7e9000, which is the figure arm10's analysis used; the SIZE recorded is not that, it is the TILED one:
 * n48_addr3_64kb_2d_bytes_4bpe(1920,1080) = 15 x 9 x 64 KiB = 0x870000, so the extent ends at 0x401070000 and the 27th unmap
 * begins 0x30000 above it. The padding is NOT guessed here: that function is scanout_copy.h's, it is the equation the detile
 * path already emits packets from, and it has its own host test.
 *
 * FAIL CLOSED, and deliberately narrowly: an extent is derived ONLY for the one shape this driver holds an address equation
 * for - swizzle 3 (ADDR3_64KB_2D), format 10 (8_8_8_8, the 4-bytes-per-element the equation's own name states), a single 2D
 * slice (VIEW 0, MIP0_DEPTH 0, RESOURCE_TYPE 2D) and a plausible w/h. ANYTHING else returns 0 = NO EXTENT, and an entry with
 * no extent is dropped by exactly the 0.0.382 rule. n48_scanout_plan_tiled refuses every other swizzle for the same reason
 * (`if (swizzle != 3u) return kScanTiledSwizzle;`) and this refuses in the same place rather than inventing a second rule.
 *
 * WHAT IT STILL DOES NOT SEE (SUSPECTED, and stated because the caller cannot check it): gfx10.3 CB_COLOR0_ATTRIB2.MAX_MIP is
 * DROPPED by the repack (xlat12_repack.h: "dropped (gfx10.3 only): MAX_MIP") and gfx12's own MAX_MIP is left 0, so a MIPPED
 * surface's higher levels are NOT inside this extent and an unmap of them would be judged disjoint. arm10's own frame has
 * MAX_MIP 0, so nothing in hand is mipped; a target that is would be understated. */
#define N48_DL_SW_64KB_2D  3u    /* gfx12 CB_COLOR0_ATTRIB3.COLOR_SW_MODE: ADDR3_64KB_2D, the only mode we have an equation for */
#define N48_DL_FMT_8888   10u    /* gfx12 CB_COLOR0_INFO.FORMAT: COLOR_8_8_8_8 = the 4 bytes per element the equation assumes */
#define N48_DL_RSRC_2D     1u    /* gfx12 CB_COLOR0_ATTRIB3.RESOURCE_TYPE, as arm10's own frame carries it */
#define N48_DL_DIM_MAX  0x10000u /* the 16-bit (value-1) MIP0_WIDTH/HEIGHT fields cannot name more than this */
static inline uint64_t n48_dl_extent(uint32_t at2, uint32_t at3, uint32_t info, uint32_t view)
{
    const uint32_t h = (at2 & 0xFFFFu) + 1u, w = ((at2 >> 16) & 0xFFFFu) + 1u;
    if (!at2) return 0ull;                 /* ATTRIB2 absent from the output, or literally 0 (a 1x1 target): no extent */
    if (((at3 >> 15) & 7u) != N48_DL_SW_64KB_2D) return 0ull;           /* a swizzle we have no address equation for */
    if ((info & 0x1Fu) != N48_DL_FMT_8888) return 0ull;                 /* not 4 bytes per element */
    if (((at3 >> 24) & 3u) != N48_DL_RSRC_2D) return 0ull;              /* 1D / 3D: a different equation */
    if (at3 & 0x3FFFu) return 0ull;                                     /* MIP0_DEPTH non-zero: not one 2D slice */
    if (view) return 0ull;                                              /* SLICE_START / SLICE_MAX non-zero: an array */
    if (w > N48_DL_DIM_MAX || h > N48_DL_DIM_MAX) return 0ull;
    return n48_addr3_64kb_2d_bytes_4bpe(w, h);
}

static inline void n48_dl_set(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t page, uint32_t flags, uint64_t size)
{
    if (flags & N48_DL_F_CAPPED) l->pgCapped++;          /* 0.0.382: keyless because OUR 8-target map was full */
    if (size) l->entExtent++; else l->entNoExtent++;     /* 0.0.383: the feed's own account of how many entries have an extent */
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++)
        if (l->e[k].ctx == ctx && l->e[k].va == va) {
            /* 0.0.446 ( fix (6)) — THE FIRST PRODUCER'S page, mode AND size STAY, unchanged by a
             * re-feed: 0.0.444 kept only `tok`/`arm_ep` (below) and overwrote mode/page/size with the re-feed's,
             * so the entry kept the FIRST producer's token but described the SECOND producer's surface. `mode` is
             * part of the ask's match and `page` is its key (n48_dl_tiled_ok_pg), so when that second frame was
             * later withdrawn (its un-feed by its own token finds nothing - the token is still the first's), its
             * (mode, page) stayed PROVEN for pixels it never drew.
             *
             * CONDUCTOR REVIEW OF 0.0.446 (item 5), build 0.0.447 — A6 FAIL-CLOSED, closing the hole the fix
             * above LEFT OPEN. Leaving the first producer's (mode, page, size) standing FOREVER is only safe while
             * every later re-feed of this (ctx, va) genuinely describes THE SAME surface - the moment one does not
             * (a real reallocation, a format or tiling change), the entry is now STALE: it still answers "proven"
             * for an (mode, page) an ask might legitimately present, describing content the first producer's frame
             * never actually wrote there. The FIX: a re-feed whose mode, page OR size DIFFERS from what the entry
             * already holds DROPS the entry outright (the compaction below, the SAME in-place shift
             * n48_dl_unmap_rng/n48_dl_unfeed_tok already use) rather than silently keeping the mismatch standing -
             * fail-closed: the next ask for this (ctx, va) finds no entry and refuses (not recorded = not proven),
             * exactly the ledger's own stated refusal direction. It is NOT re-inserted under the re-feed's own
             * values here: n48_dl_feed's own next real producer adds it fresh, as the first producer of a NEW
             * entry. A MATCHING re-feed (same mode/page/size - by far the common case, since d_table_desc/hook
             * feed on every committed frame whether or not the surface actually changed) is UNCHANGED: only
             * `flags` follows it, exactly as 0.0.394 documented (K10: a re-feed clears the withdrawn mark); flags
             * feed counters and the empty-why classification only, never an ask's answer. */
            if (l->e[k].mode != mode || l->e[k].page != page || l->e[k].size != size) {
                /* build 0.0.485 (switch 58, L2 of notes/design/LOGIN-SCREEN-PATH.md; run10d F26/F35: the composite
                 * re-written 8_8_8_8 -> 2_10_10_10, the drop below left it unrecorded and every later plane refused).
                 * A COMMITTED frame that re-writes this (ctx, va) in a different shape IS the surface's producer now: its
                 * values REPLACE the entry's, ALL of them, the token included - so the OLD producer's proof does not
                 * survive (an ask proves only against the new mode/page), and the new producer's own un-feed (keystone
                 * withdrawal, token mismatch, NOPed IB) finds the entry by its token and removes it, never reverting it
                 * to the old producer (whose pixels the new frame may have overwritten). Exactly as sound as a first
                 * insertion. Outside a feed (feedTok 0) nothing could un-feed it, so it is DROPPED as with the switch off. */
                if (l->replace && l->feedTok) {
                    l->reFeedReplaced++;
                    l->e[k].mode = mode; l->e[k].page = page; l->e[k].size = size; l->e[k].flags = flags;
                    l->e[k].tok = l->feedTok; l->e[k].arm_ep = l->feedArmEp;
                    return;
                }
                l->reFeedDropped++;
                for (uint32_t w = k; w + 1u < l->n && w + 1u < N48_DL_MAX; w++) l->e[w] = l->e[w + 1u];
                l->n--;
                return;
            }
            l->e[k].flags = flags;
            /* 0.0.444 (C5-RING-REVIEW.md (B) item 9c, Q7c) — KEEP THE PREVIOUS PRODUCER'S PROOF. Through 0.0.443
             * this line was `l->e[k].tok = l->feedTok; l->e[k].arm_ep = l->feedArmEp;` ("the LAST producer is the
             * one that counts", 0.0.390) — but n48_dl_feed runs for every COMMITTED frame, and a frame that
             * committed here can still be un-fed LATER by its own token if a later stage withdraws or NOPs it
             * (gfx_flightring.h's PENDING/COMMITTED -> NOT_RUN, or a keystone refusal's un-feed). If the LATER
             * feed's `tok` had overwritten an EARLIER, already-real commit's `tok` on this same (ctx, va), un-feeding
             * the later (withdrawn) frame would delete the earlier fill's ONLY proof along with it - the earlier
             * frame really did commit and its pixels really are current, but nothing says so any more. Keeping the
             * entry's EXISTING `tok`/`arm_ep` is always safe: SOME committed frame's proof is all this ledger ever
             * needs to hold, and the first one recorded is never wrong about "this content is proven" for as long as
             * it has not itself been un-fed. A re-feed with the SAME mode/page/size only refreshes `flags`; a
             * DIFFERING one is dropped above, never silently kept - only the PROVENANCE stays with its original
             * producer, for a shape that producer actually proved. */
            return;
        }
    if (l->n >= N48_DL_MAX) { l->overflow++; return; }   /* not recorded = not proven: the refusal direction */
    l->e[l->n].ctx = ctx; l->e[l->n].va = va; l->e[l->n].page = page; l->e[l->n].size = size;
    l->e[l->n].mode = mode; l->e[l->n].flags = flags;
    l->e[l->n].tok = l->feedTok; l->e[l->n].arm_ep = l->feedArmEp;     /* 0.0.390 */
    l->n++; l->added++;
}

/* The (VA -> base physical page) rows the judged frame ALREADY walked for its own colour targets (the kext's hVa[] / hPage[],
 * filled by gfxc_page over the frame's own VM in the same frame as the feed; page 0 there = it did not resolve). No new plumbing
 * and no second walk: the feed hands these rows in, and an entry whose VA is not among them gets page 0 = no key.
 * 0.0.382: `full` is 1 when the frame named MORE colour targets than these rows hold (the kext's kXdTgtHold = 8,
 * and hVa[] takes CB0-CB7 across all of the frame's IBs). It changes no decision; it only stamps N48_DL_F_CAPPED on the entries
 * that then get page 0, so a run can say "our cap" instead of guessing. */
/* build 0.0.485 (switch 58): `mid0` / `midN` - rows [mid0, mid0 + midN) are PER-DRAW rows n48_dl_pgx_extend appended
 * after the frame's own IB-end rows. midN 0 (every zero-initialised map, and every map while 58 is off) = no such rows: the
 * lookup below answers exactly as n48_dl_pg_of always has, and nothing is counted or flagged. */
typedef struct { const uint64_t *va; const uint64_t *page; uint32_t n, full, mid0, midN; } n48_dl_pgmap;
static inline uint64_t n48_dl_pg_of(const n48_dl_pgmap *m, uint64_t va)
{
    if (!m || !m->va || !m->page) return 0ull;
    for (uint32_t k = 0; k < m->n; k++)
        if ((m->va[k] & ~(N48_DL_PAGE - 1ull)) == (va & ~(N48_DL_PAGE - 1ull))) return m->page[k];
    return 0ull;
}
/* The same lookup, first match, same answer - and WHICH row answered (*ix; m->n, or 0 with no map, when none did). */
static inline uint64_t n48_dl_pg_of_ix(const n48_dl_pgmap *m, uint64_t va, uint32_t *ix)
{
    if (ix) *ix = m ? m->n : 0u;
    if (!m || !m->va || !m->page) return 0ull;
    for (uint32_t k = 0; k < m->n; k++)
        if ((m->va[k] & ~(N48_DL_PAGE - 1ull)) == (va & ~(N48_DL_PAGE - 1ull))) { if (ix) *ix = k; return m->page[k]; }
    return 0ull;
}

/* =====================================================================================================================
 * build 0.0.485 — L1 OF notes/design/LOGIN-SCREEN-PATH.md: EVERY COLOUR TARGET A COMMITTED FRAME WROTE GETS ITS OWN PAGE.
 *
 * THE DEFECT (CONFIRMED on hardware, run10c/run10d). n48_dl_from_output already enters EVERY draw's CB0 target of a committed
 * segment into the ledger, but it looks each one's base page up in the frame's IB-END rows only (the kext's hVa/hPage, filled
 * from n48_gcap_scan's N48_GCAP_CB items, which that scan pushes once per slot AFTER its walk). A surface written MID-IB -
 * run10c F48's first draw into the composite 0x406800000, then 39 more draws into five other targets, both IBs ending on
 * 0x400034000 - therefore enters with page 0, and with the physical-page key (switch 18) on, page 0 proves NOTHING: the plane
 * F51 that read the composite was refused (`refused for no page 372`).
 *
 * THE FIX. The caller hands in the frame's PER-DRAW colour targets (the kext's gfx_capture_scan.h n48_gcap_cbt_ib pass over
 * every IB of the frame: every active slot's target at every draw, plus each IB's end state; a walk that runs whenever 58 is
 * on, whatever switch 54 says) and a resolver (the kext's r5_resolve_cb: gfxc_page over THE COMMITTING FRAME'S OWN vm, the
 * walker - and the `ok ? page : 0` rule - the IB-end rows already use). This builds, in `x`, the IB-end rows FIRST AND
 * UNCHANGED, then one row per per-draw target whose 4 KiB page no earlier row names, resolved; and re-points `m` at them.
 *   - a target whose page does NOT resolve gets page 0: fed exactly as today, never guessed, never proven with the key on;
 *   - a VA an IB-end row already names keeps THAT row's answer (the first match wins in n48_dl_pg_of_ix, and those rows come
 *     first), so every entry the old map answered is answered identically;
 *   - a per-draw target the table could not hold (past N48_DL_PGX_MAX, or past the pass's own 32 - `dtotal > dn`) gets no row
 *     and therefore page 0, and marks the map `full` so those entries carry N48_DL_F_CAPPED (OUR cap, not Apple);
 *   - no pages are resolved and nothing changes unless the caller calls this (the kext: switch 58 on AND the frame committed).
 * `x` must outlive the feed that reads `m` (the kext's is a file-scope static: 640 bytes, off the submit-path stack).
 * Returns the per-draw rows added. */
#define N48_DL_PGX_MAX 40u   /* the kext's kXdTgtHold (8) IB-end rows + gfx_capture_scan.h N48_GCAP_CBT_MAX (32) */
typedef struct { uint64_t va[N48_DL_PGX_MAX]; uint64_t page[N48_DL_PGX_MAX]; uint32_t n; } n48_dl_pgx;
typedef uint32_t (*n48_dl_page_fn)(void *ud, uint64_t va, uint64_t *page);   /* 1 = resolved, *page = its base page */
static inline uint32_t n48_dl_pgx_extend(n48_dl *l, n48_dl_pgx *x, n48_dl_pgmap *m, const uint64_t *dva, uint32_t dn,
                                         uint32_t dtotal, n48_dl_page_fn fn, void *ud)
{
    if (!l || !x || !m) return 0u;
    x->n = 0u;
    if (m->va && m->page)
        for (uint32_t k = 0; k < m->n && x->n < N48_DL_PGX_MAX; k++) { x->va[x->n] = m->va[k]; x->page[x->n] = m->page[k]; x->n++; }
    const uint32_t n0 = x->n;
    uint32_t over = (dtotal > dn) ? dtotal - dn : 0u;
    for (uint32_t q = 0; dva && q < dn; q++) {
        const uint64_t va = dva[q];
        if (!va) continue;                                   /* Apple's zeroed base of an unbound slot: not a target */
        uint32_t named = 0u;
        for (uint32_t r = 0; r < x->n; r++)
            if ((x->va[r] & ~(N48_DL_PAGE - 1ull)) == (va & ~(N48_DL_PAGE - 1ull))) { named = 1u; break; }
        if (named) continue;                                 /* an earlier row already answers this VA: its answer stands */
        if (x->n >= N48_DL_PGX_MAX) { over++; continue; }    /* no room: no row, so page 0 - fail closed, and counted */
        uint64_t pg = 0ull;
        if (!fn || !fn(ud, va & ~(N48_DL_PAGE - 1ull), &pg)) pg = 0ull;   /* unresolved: page 0, never guessed */
        if (pg) l->midPgRes++; else l->midPgUnres++;
        x->va[x->n] = va; x->page[x->n] = pg; x->n++;
    }
    l->midRows += (uint64_t)(x->n - n0);
    l->midOver += over;
    m->va = x->va; m->page = x->page; m->n = x->n; m->mid0 = n0; m->midN = x->n - n0;
    if (over) m->full = 1u;
    return x->n - n0;
}

/* Every draw of a TRANSLATED segment's output that writes CB0 with CB_COLOR0_BASE, BASE_EXT, ATTRIB3 and a CB0 target mask all
 * written by that output enters the ledger with gfx12 CB_COLOR0_ATTRIB3.COLOR_SW_MODE [17:15] (gfx12.json); a non-zero CB1-7
 * base clears it. gfx12 addresses: CB_COLOR0_BASE 0x28c60, ATTRIB3 0x28c7c, BASE_EXT 0x28e40, CB_TARGET_MASK 0x28850,
 * CB_COLOR1_BASE 0x28c84 (stride 0x24 to CB7). The same parse as tools/m4-xlat/replay.c led_from_output (M4-DESC-TABLE-IMPL). */
static inline uint32_t n48_dl_from_output(n48_dl *l, uint64_t ctx, const uint32_t *o, uint32_t n, const n48_dl_pgmap *pg)
{
    uint32_t i = 0, have = 0, base = 0, ext = 0, at3 = 0, tm = 0, added = 0;
    /* 0.0.383 (THE ENTRY EXTENT): three MORE gfx12 words of the SAME CB0 state, read from the SAME packets. They are NOT part
     * of `have`, so WHICH entries get recorded is bit for bit what it was - they only decide whether the entry carries a size.
     * gfx12 CB_COLOR0_ATTRIB2 0x28c78, CB_COLOR0_INFO 0x28ec0, CB_COLOR0_VIEW 0x28c64 (xlat12_repack.h's own destinations).
     * NOTE these are GFX12 addresses in a GFX12 stream: gfx10.3 0x28ec0 is ATTRIB2 and gfx10.3 0x28c78 is DCC_CONTROL, and
     * neither reading applies here - this function has only ever parsed the TRANSLATED output. */
    uint32_t at2 = 0, info = 0, view = 0;
    while (i < n) {
        const uint32_t h = o[i];
        if (h == 0xFFFF1000u || (h >> 30) != 3u) { i++; continue; }   /* the one-dword NOP is one dword */
        const uint32_t total = 2u + ((h >> 16) & 0x3FFFu), op = (h >> 8) & 0xFFu;
        if (i + total > n) break;
        if (op == 0x69u && total >= 3u) {
            const uint32_t first = 0x28000u + ((o[i + 1u] & 0xFFFFu) << 2);
            for (uint32_t k = 0; k + 2u < total; k++) {
                const uint32_t a = first + 4u * k, v = o[i + 2u + k];
                if (a == 0x28c60u) { base = v; have |= 1u; } else if (a == 0x28e40u) { ext = v; have |= 2u; }
                else if (a == 0x28c7cu) { at3 = v; have |= 4u; } else if (a == 0x28850u) { tm = v; have |= 8u; }
                else if (a == 0x28c78u) { at2 = v; }                 /* 0.0.383: CB_COLOR0_ATTRIB2 - MIP0_WIDTH / MIP0_HEIGHT */
                else if (a == 0x28ec0u) { info = v; }                /* 0.0.383: CB_COLOR0_INFO    - FORMAT */
                else if (a == 0x28c64u) { view = v; }                /* 0.0.383: CB_COLOR0_VIEW    - SLICE_START / SLICE_MAX */
                else if (a >= 0x28c84u && a < 0x28c84u + 7u * 0x24u && (a - 0x28c84u) % 0x24u == 0u && v) n48_dl_clear(l);
            }
        }
        if ((op == 0x2Du || op == 0x27u || op == 0x35u) && have == 15u && (tm & 0xFu) && base) {
            const uint64_t va = ((uint64_t)(ext & 0xFFu) << 40) | ((uint64_t)base << 8);
            uint32_t ix = 0u;
            const uint64_t pgv = n48_dl_pg_of_ix(pg, va, &ix);
            uint32_t fl = (!pgv && pg && pg->full) ? N48_DL_F_CAPPED : 0u;   /* 0.0.382: keyless, and OUR cap */
            /* build 0.0.485 (switch 58, L1): the answer came from a PER-DRAW row. Counted and flagged only; `pgv` is the
             * same value n48_dl_pg_of gives, and with no per-draw rows (midN 0) this block never runs. */
            if (pg && pg->midN && ix >= pg->mid0 && ix < pg->mid0 + pg->midN) {
                fl |= N48_DL_F_MID; l->midFed++;
                if (pgv) l->midFedKeyed++;
            }
            n48_dl_set(l, ctx, va, (at3 >> 15) & 7u, pgv, fl, n48_dl_extent(at2, at3, info, view)); added++;
        }
        i += total;
    }
    return added;
}

/* One judged frame, as the kext has it after the COMMIT gate answered. `committed` is 1 only when the caller's commit path returned
 * commit_ok for THIS frame, and `gate` is n48_cm_gate's reason for it; both are required, because N48_CM_OK is 0 and a
 * zero-initialised frame must NOT read as a gate that said yes. seg/nseg the policy's build (gfx_commit.h n48_cm_seg); out the
 * rewritten IB the segments index. */
typedef struct {
    uint32_t committed, gate, verdict, nseg, n;
    uint64_t ctx;
    const n48_cm_seg *seg;
    const uint32_t *out;
    n48_dl_pgmap pg;   /* 0.0.380: the frame's own colour-target page walk; all-zero = no pages, so no key (fail closed) */
    uint32_t tok, arm_ep;   /* 0.0.390: this frame's COMMIT token seq and the ledger's arm epoch. Recorded only */
} n48_dl_frame;

/* 0.0.390: the producer identity the ledger holds for one surface, for the REPORT. 1 when an entry matched. */
static inline int n48_dl_producer_of(const n48_dl *l, uint64_t ctx, uint64_t va, uint32_t *tok, uint32_t *arm_ep)
{
    if (tok) *tok = 0u;
    if (arm_ep) *arm_ep = 0u;
    if (!l) return 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++)
        if (l->e[k].ctx == ctx && l->e[k].va == va) {
            if (tok) *tok = l->e[k].tok;
            if (arm_ep) *arm_ep = l->e[k].arm_ep;
            return 1;
        }
    return 0;
}

/* Returns the entries this frame added; 0 when it was refused. */
static inline uint32_t n48_dl_feed(n48_dl *l, const n48_dl_frame *f)
{
    l->feeds++;
    if (!f || f->committed != 1u || f->gate != N48_CM_OK || f->verdict != N48_XV_TRANSLATE || !f->out || !f->seg) {
        l->refusedGate++;
        return 0u;
    }
    if (!f->ctx) { l->refusedCtx++; return 0u; }
    l->fedOk++;
    uint32_t added = 0;
    l->feedTok = f->tok; l->feedArmEp = f->arm_ep;   /* 0.0.390: recorded onto every entry this feed writes */
    for (uint32_t k = 0; k < f->nseg && k < N48_XV_MAX_SEGS; k++) {
        const n48_cm_seg *s = &f->seg[k];
        if (s->status || s->end <= s->start || s->end > f->n) continue;
        added += n48_dl_from_output(l, f->ctx, &f->out[s->start], s->end - s->start, &f->pg);
    }
    l->feedTok = 0u; l->feedArmEp = 0u;              /* outside a feed there is no producer identity to record */
    return added;
}

/* 0.0.383 — THE TWO-SIDED RULE, RESTORED ON A DERIVED EXTENT (and the history below is why it had to be earned).
 *
 * answered "does this unmap cover this entry?" with ws_resprov.h's two-sided disjointness predicate over an entry extent
 * this header INVENTED: base VA to the end of its base page. That extent is CONFIRMED WRONG, from the runs' own unmapVA lines:
 *   - WindowServer's ctx (create #5) unmaps ranges that NEST - arm9 and arm8 both carry [0x400200000,+0x40000) alongside the
 *     inner [0x400235000,+0x1000) and [0x400238000,+0x8000), and [0x4000b8000,+0x8000) alongside inners at +0x3000 / +0x6000;
 *   - the one colour-target VA any unmap covers, 0x400240000, is covered by a 0x40000 range - 64 pages. A one-page extent
 *     understates that surface by 63 pages, and the second disjunct (`va + bytes <= eva`) would then KEEP an entry an unmap of
 *     a later page of the same surface really did destroy.
 * said "there is no surface SIZE anywhere on this path" - true of n48_gcap_item {kind,index,dword,want,va}, and FALSE of
 * the translated output this ledger is actually fed from, which carries CB_COLOR0_ATTRIB2 / INFO / VIEW beside the BASE it
 * already reads. n48_dl_extent above derives the size from those, for the one surface shape this driver holds an equation for.
 *
 * 0.0.383 RESTORES THE SECOND DISJUNCT, ON THAT DERIVED EXTENT AND ONLY ON IT. `vsz` is the entry's own size:
 *   vsz > 0  the two-sided test, and it is ws_resprov.h n48_rp_clr_clean's PREDICATE VERBATIM - keep when
 *            `eva + esz <= va || va + vsz <= eva`, i.e. drop when [eva, eva+esz) and [va, va+vsz) intersect;
 *   vsz == 0 NO EXTENT WAS DERIVED. Fail closed: bit for bit 0.0.382's one-sided rule, keep only when the unmap ends at or
 *            before the base. This is the path was thrown out for taking with an INVENTED extent, and the difference is
 *            that nothing is invented here - an entry either carries a size its own frame's registers gave it, or it does not.
 * Everything unknown still drops, on either side: `esz` 0 (the whole context, ws_resprov.h section 4b) and an unmap range that
 * wraps 2^64 DROP, and an entry extent that wraps 2^64 is no extent and DROPS.
 * 1 = KEEP this entry, 0 = DROP it. */
static inline int n48_dl_unmap_keeps(uint64_t eva, uint64_t esz, uint64_t va, uint64_t vsz)
{
    if (!esz) return 0;                    /* size 0 = the whole context (ws_resprov.h section 4b): scope unknown, DROP */
    if (eva + esz < eva) return 0;         /* a range that wraps 2^64 is no scope at all: DROP */
    if (eva + esz <= va) return 1;         /* the unmap ends at or before the entry's base VA: it cannot have touched it */
    if (!vsz) return 0;                    /* NO DERIVED EXTENT: fail closed, exactly as 0.0.382 did */
    if (va + vsz < va) return 0;           /* an entry extent that wraps 2^64 is no extent at all: DROP */
    return (va + vsz <= eva) ? 1 : 0;      /* the unmap begins at or after the entry's END: it cannot have touched it */
}

/* An unmapVA of context `ctx` over [va, va + size). ctx 0 (unknown context) reaches every entry.
 *   exact 0  THE DEFAULT, AND TODAY'S BEHAVIOUR: every entry of that context is dropped, whatever the unmap's range - `va` and
 *            `size` are not read at all. arm9 measured this emptying the ledger 1.32 times per judged frame.
 *   exact 1  0.0.383: an entry is KEPT when this unmap is DISJOINT from [entry VA, entry VA + entry size) - the two-sided test
 *            of n48_dl_unmap_keeps. An entry with NO derived size keeps 0.0.382's one-sided answer (keep only when the unmap
 *            ends at or before its base) and is counted in `unmapNoExtent` so a run can separate the two populations.
 *            Everything unknown still drops: a size of 0 (the whole context) or a range that wraps.
 * WHAT THIS STILL DOES NOT COVER (SUSPECTED): a MIPPED surface's higher levels are outside the derived extent (gfx12 drops
 * gfx10.3's MAX_MIP - n48_dl_extent's own caveat), and an entry that derived NO extent keeps the gap, where an unmap of
 * a LOWER range belonging to the same allocation cannot be told from an unrelated one. That is why the physical-page key above
 * must be on with it - the key re-checks the base page at every answer. */
static inline void n48_dl_unmap_rng(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) {
            const uint64_t vsz = l->e[k].size;
            if (!size) l->unmapNoRange++;                  /* counted: the exact drop fell back to the wholesale rule */
            drop = !n48_dl_unmap_keeps(va, size, l->e[k].va, vsz);
            /* 0.0.394: A COVERING UNMAP NO LONGER WITHDRAWS THE ENTRY. `n48_dl_unmap_keeps` answered 0
             * because the unmap covers the entry's range; with `l->keep` on the entry is KEPT instead, marked
             * withdrawn-by-unmap and its recorded page left untouched - the key-on ask later decides whether the re-map put
             * the SAME page back (proven) or a different one (refused as `keyMoved`). The guard `size && va + size >= va`
             * leaves the two UNKNOWN scopes dropping: a size of 0 (the whole context) and a range that wraps 2^64. `keep`
             * is read from the ledger, so the deferred drain (n48_dl_drain calls this with the same `l`) honours it too. */
            if (drop && l->keep && size && va + size >= va) {
                drop = 0;
                l->keptAcross++;
                l->e[k].flags |= N48_DL_F_WITHDRAWN;
            } else if (size && va + size > l->e[k].va && va + size >= va) {
                /* 0.0.383: WHICH side of the rule decided, so "kept by extent" and "dropped for want of an extent" are
                 * separate numbers in the run rather than one bucket. The first is the 27th unmap of arm10; the second is the
                 * population still living under 0.0.382's rule. Counted only when the one-sided answer would have differed. */
                if (!vsz) l->unmapNoExtent++;
                else if (!drop) l->unmapKeptExtent++;
            }
        }
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;                          /* the narrowing ACTED: this entry survived its context's unmap */
        if (w != k) {
            l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
            l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size;
            /* 0.0.444 (C5-RING-REVIEW.md (B) item 9b, Q7b, CONFIRMED) — `tok`/`arm_ep` MUST MOVE WITH THE ENTRY TOO.
             * Through 0.0.443 this copy dropped both, so a surviving entry compacted down from slot k to slot w kept
             * whatever `tok`/`arm_ep` slot w had held BEFORE this call (another entry's producer identity, or 0 at
             * the tail) - a drop of an unrelated entry between the feed and the un-feed made n48_dl_unfeed_tok miss
             * the withdrawn frame's own entries entirely (they no longer carried its `tok`), or, worse, un-feed a
             * DIFFERENT surviving entry that happened to inherit a matching stale `tok` by the compaction's own
             * accident of position. */
            l->e[w].tok = l->e[k].tok; l->e[w].arm_ep = l->e[k].arm_ep;
        }
        w++;
    }
    l->n = w;
}
/* C5 part 1 (hygiene, notes/design/C5-CONTINUOUS.md Q4) — THE LEDGER UN-FEED.
 *
 * n48_dl_feed is asked from ONE fact alone: the COMMIT gate answered N48_CM_OK for this frame (n48_dl_frame's own
 * `committed`/`gate`/`verdict`). That answer is recorded at the GATE, before the keystone even runs — so a frame
 * the keystone then WITHDRAWS, or whose identity does not match at the hook (TOKEN), or whose IB the ring walk's
 * exemption then NOPs (never spared, "its IB never ran"), still fed the ledger: the entries it added describe pixels
 * that were never drawn. Every one of those three outcomes is already recorded per-entry, unconditionally, at the
 * feed (`tok` = the committing frame's own token seq) — so this removes exactly the entries ONE
 * frame fed, by that same `tok`, and nothing else.
 *
 * `tok` 0 NEVER MATCHES ANYTHING: 0 is not a producer identity (n48_dl_ent's own comment: "0 = the feed supplied
 * none"), and n48_dl_feed never writes 0 into a live entry's `tok` (gXdCmGateSeq/gXdCmToken.seq are never 0 for a
 * real commit). Treating 0 as a wildcard here would let a caller that forgot to record the token wipe the whole
 * ledger instead of nothing; refusing it is the same fail-closed shape n48_dl_tiled_ok_pg's ctx-0 refusal takes.
 *
 * The compaction is n48_dl_unmap_rng's own in-place filter, walked over `tok` instead of `ctx`; it does not touch
 * `unmaps`/`unmapDropped`/`unmapKept` (this is not an unmap) and counts itself separately. */
static inline uint32_t n48_dl_unfeed_tok(n48_dl *l, uint32_t tok)
{
    if (!l || !tok) return 0u;
    l->unfeedAsks++;
    uint32_t removed = 0u, w = 0u;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        if (l->e[k].tok == tok) { removed++; continue; }
        if (w != k) {
            l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
            l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size;
            l->e[w].tok = l->e[k].tok; l->e[w].arm_ep = l->e[k].arm_ep;
        }
        w++;
    }
    l->n = w;
    l->unfeedRemoved += removed;
    return removed;
}

/* =====================================================================================================================
 * 0.0.444 (C5-RING-REVIEW.md (B) item 9a, Q7a, CONFIRMED) — THE UN-FEED QUEUE.
 *
 * All three un-feed call sites live in hook_gfxCommitIB, on Apple's submit thread, WITHOUT gXdLock — the SAME lock
 * gfxsrc_desc_unmap's locked compaction (n48_dl_unmap_rng, via xd_led_drain_locked/n48_dl_sync at the top of the
 * NEXT judged frame) and every write n48_dl_feed makes both run under. An un-feed calling n48_dl_unfeed_tok directly
 * from there could race that locked compaction — one thread mutating `l->e[]`/`l->n` while the other walks it —
 * and corrupt the ledger's array (CONFIRMED by the review, same class of hazard the lock already exists to prevent
 * for every OTHER ledger writer).
 *
 * The fix: hook_gfxCommitIB QUEUES the token (n48_dl_unfeed_queue — a plain array, no lock needed, because every
 * un-feed call site runs on the ONE submit thread and nothing else ever writes this queue); gfxsrc_decide_frame
 * DRAINS it (n48_dl_unfeed_drain) under gXdLock, at the top of the NEXT judged frame — the same "before anything in
 * this frame can ask the ledger" position n48_dl_sync already holds (AppleHardwareHook.cpp, right before it). */
#define N48_DL_UNFEED_QUEUE 8u
/* 0.0.446 ( fix (5)): `lost` = a token was refused for want of room since the last drain (the drain
 * then clears the ledger, below); `overflow` stays the boot total of such refusals. */
typedef struct { uint32_t tok[N48_DL_UNFEED_QUEUE]; uint32_t n, overflow, lost; } n48_dl_unfeed_q;

/* Called WITHOUT any lock, from hook_gfxCommitIB. `tok` 0 is refused, matching n48_dl_unfeed_tok's own guard (0 is
 * never a real producer identity). A queue already full counts the overflow rather than silently dropping it or
 * blocking; N48_DL_UNFEED_QUEUE (8) is double this build's own spend ceiling (N48_CM_SHOT_BUDGET_MAX = 4), so an
 * overflow is not observed at today's usage. 0.0.446: and marks the queue `lost`, so the next drain fails closed. */
static inline void n48_dl_unfeed_queue(n48_dl_unfeed_q *q, uint32_t tok)
{
    if (!q || !tok) return;
    const uint32_t n = __atomic_load_n(&q->n, __ATOMIC_RELAXED);   /* 0.0.523 fix pass SHOULD 2: the one writer's count */
    if (n >= N48_DL_UNFEED_QUEUE) { q->overflow++; q->lost = 1u; return; }
    q->tok[n] = tok;                                                /* the entry first ... */
    __atomic_store_n(&q->n, n + 1u, __ATOMIC_RELEASE);              /* ... then publish it */
}

/* Called UNDER the caller's own lock (gXdLock). Applies every queued token via n48_dl_unfeed_tok (which already
 * does its own compaction correctly, item 9b) and always empties the queue, even one that overflowed — an
 * overflowed token is lost (counted), never retried. Returns the number of tokens applied.
 * 0.0.446 ( fix (5)) — FAIL CLOSED ON A LOST TOKEN. 0.0.444 let a lost token's entries stand ("fail toward
 * LEAVING an entry standing a little longer"), but an entry that is never un-fed is not standing "a little longer":
 * it is a withdrawn frame's proof of pixels it never drew, kept until something else happens to drop it. When the
 * queue lost a token since the last drain, the WHOLE ledger is cleared after the queued tokens are applied (nothing
 * in it can be trusted to be free of the lost frame's entries) and `unfeedOverflowClears` counts it. Clearing only
 * removes proof - every later ask for those surfaces refuses until a committed frame feeds them again. */
static inline uint32_t n48_dl_unfeed_drain(n48_dl *l, n48_dl_unfeed_q *q)
{
    if (!q) return 0u;
    uint32_t did = 0u;
    const uint32_t qn = __atomic_load_n(&q->n, __ATOMIC_ACQUIRE);   /* 0.0.523 fix pass SHOULD 2: read once, acquire */
    for (uint32_t i = 0; i < qn && i < N48_DL_UNFEED_QUEUE; i++) { (void)n48_dl_unfeed_tok(l, q->tok[i]); did++; }
    q->n = 0u;
    if (q->lost) {
        q->lost = 0u;
        if (l) { n48_dl_clear(l); l->unfeedOverflowClears++; }
    }
    return did;
}

/* 0.0.446 ( fix (5)) — the un-feed path's report line (the kext prints it from `gfxneuter 10`), here so
 * tests/gfx_desc_port_test.cpp can bound it at widest numerics. args: unfeedAsks, unfeedRemoved (u64), queued now,
 * queue overflow (u32), unfeedOverflowClears (u64). */
#define N48_DL_UNFEED_REPORT_FMT \
    "descport: UN-FEED - un-feeds applied %llu (entries removed %llu), queued now %u, queue overflow %u, " \
    "LEDGER CLEARED ON A LOST UN-FEED %llu (fail closed: a token the queue could not hold would have left " \
    "a withdrawn frame's proof standing)."

/* build 0.0.485 — switch 58's report line (the kext prints it from `gfxneuter 58`), here so tests/gfx_desc_port_test.cpp
 * can bound it under n48log's 491-byte body cap at widest numerics and longest strings. args: state (%s), how (%s), the
 * inert note (%s), then midRows, midPgRes, midPgUnres, midOver, midFed, midFedKeyed, reFeedDropped, reFeedReplaced (u64). */
#define N48_DL_MID_REPORT_FMT \
    "ledmid485: switch 58 (per-draw target pages + re-shape REPLACE) %s (%s)%s; per-draw rows %llu (resolved %llu, " \
    "UNRESOLVED %llu = page 0), not held %llu; entries fed from a per-draw row %llu (with a page %llu); re-shaped " \
    "re-feeds DROPPED %llu, REPLACED %llu."

/* The whole-context unmap, bit for bit what it has always been (the offline replay calls this one). */
static inline void n48_dl_unmap(n48_dl *l, uint64_t ctx)
{
    n48_dl_unmap_rng(l, ctx, 0ull, 0ull, 0u);
}

/* 0.0.382 — THE DEFERRED DRAIN, AND THE ONE THING IT MUST REPLACE.
 *
 * WHY. An unmapVA that finds gXdLock busy cannot touch the ledger, so today it moves gXdDpEpoch instead - and the epoch is
 * WHOLESALE ACROSS EVERY CONTEXT and is applied by n48_dl_sync at the TOP of the next judged frame, before that frame's target
 * walk and before any ask. arm9 measured that as the dominant emptier (epoch moved 136 times against `busy 146`),
 * and its own dpled841 column shows it acting between the one feed and the first ask: f1 feeds (`fd 1/1/1 fed 1`) at `ep 2`,
 * f15 still reads `ep 2`, f22 reads `ep 4` and the first ask is f23. SO THE EXACT-RANGE DROP ALONE CHANGES NOTHING: the entry
 * is gone at f23's sync whatever the unmap rule says.
 *
 * ★ THE RULE THIS HEADER CANNOT ENFORCE AND THE CALLER MUST: THIS DRAIN **REPLACES** THE BUSY BRANCH'S EPOCH BUMP. It does not
 * sit beside it. A caller that keeps the bump has changed nothing - the bump wipes every context before this drain is ever
 * reached. The kext's own wiring test pins that (gfx_desc_port_test group C), and the planted defect "a drain that leaves the
 * epoch bump in place" is exactly this mistake.
 *
 * WHAT IT DOES. ws_resprov.h's clear ring already carries every unmap's (kind, ctxSeq, va, size) - n48_rp_clr_push is the FIRST,
 * UNCONDITIONAL statement of the kext's gfxsrc_desc_unmap, before IOLockTryLock and on BOTH branches, so the busy branch's
 * event is already recorded. A frame holding gXdLock drains from its own mark and applies each event with the SAME per-entry
 * rule the locked branch uses (n48_dl_unmap_rng), so a busy unmap of an unrelated range no longer costs every context.
 *
 * FAIL CLOSED, in the ring's own terms and in today's direction (WIPE = exactly what the epoch bump did):
 *   - no ledger / no ring, or the ring WRAPPED past the mark (`now - since > N48_RP_CLR`, the ring is 64): WIPE;
 *   - a slot whose seqlock stamp does not bracket the read (being rewritten, or overwritten): WIPE;
 *   - an event that is not an unmap (N48_RP_CLR_WS - WindowServer's binding dropped, a whole-table clear): WIPE;
 *   - an unmap with ctxSeq 0 or size 0 is NOT a wipe: n48_dl_unmap_rng already treats ctx 0 as reaching every entry and
 *     size 0 as the whole context, which is the same answer applied per entry.
 * `*markOut` is set to the position drained TO on every path, including the wiping ones, so the caller never re-drains an event
 * and never leaves a stale mark that would wrap later. `since` is the caller's previous mark; the caller must hold the ledger's
 * lock. Returns N48_DL_DRAIN_OK, or the reason it wiped. */
enum { N48_DL_DRAIN_OK = 0u, N48_DL_DRAIN_WRAP, N48_DL_DRAIN_TORN, N48_DL_DRAIN_WS };
static inline uint32_t n48_dl_drain(n48_dl *l, const n48_rp_clog *g, uint64_t since, uint64_t *markOut, uint32_t exact)
{
    if (!l) { if (markOut) *markOut = since; return N48_DL_DRAIN_WRAP; }
    l->drains++;
    const uint64_t now = g ? n48_rp_clr_mark(g) : since;
    if (markOut) *markOut = now;
    if (!g || now < since || now - since > (uint64_t)N48_RP_CLR) {
        l->drainWrap++; l->drainWiped++; n48_dl_clear(l); return N48_DL_DRAIN_WRAP;
    }
    for (uint64_t n = since; n < now; n++) {
        const n48_rp_clr *e = &g->s[n % N48_RP_CLR];
        const uint64_t s1   = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        const uint32_t kind = __atomic_load_n(&e->kind,  __ATOMIC_SEQ_CST);
        const uint32_t cs   = __atomic_load_n(&e->ctxSeq, __ATOMIC_SEQ_CST);
        const uint64_t eva  = __atomic_load_n(&e->va,    __ATOMIC_SEQ_CST);
        const uint64_t esz  = __atomic_load_n(&e->size,  __ATOMIC_SEQ_CST);
        const uint64_t s2   = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) { l->drainTorn++; l->drainWiped++; n48_dl_clear(l); return N48_DL_DRAIN_TORN; }
        if (kind != N48_RP_CLR_UNMAP)   { l->drainWs++;   l->drainWiped++; n48_dl_clear(l); return N48_DL_DRAIN_WS; }
        l->drainEv++;
        /* THE EVENT'S OWN CONTEXT. The kext keys a ledger entry by gfxsrc_desc_ctx(ctxSeq), which returns the SAME seq
         * number, and gfxsrc_desc_unmap hands n48_dl_unmap_rng that same ctxSeq - so `cs` is the entry key, not a
         * translation of it. Using anything else here (0, or the ledger's own arm/epoch) would drop other contexts. */
        n48_dl_unmap_rng(l, (uint64_t)cs, eva, esz, exact);
    }
    return N48_DL_DRAIN_OK;
}


/* ---- 3. the per-judged-frame readout (0.0.379) ----------------------------------------------------------------
 * item 7. A run scored on `gXdDpEpoch` and `clears` alone would print "the lifetime was fine" over a ledger that was
 * NEVER FED - in every carried replay `clears` is 0 and the epoch constant while the ledger is empty, because the binding term
 * is the FEED, not the lifetime - and an N=2 hardware run would then be read as a negative for the carry. These two lines of
 * pure code are the discrimination itself, so that "empty because nothing fed it" and "empty because it was cleared" are
 * different STRINGS in the log and a planted defect in the classification is caught by a host test, not by a code review.
 *
 * Every arm is decided by a counter n48_dl already keeps and n48_dl_clear never resets, and the raw counters are printed
 * beside the name so the classification can always be audited against them:
 *   feeds        0  -> n48_dl_feed was never called at all: no judged frame ever offered this ledger anything;
 *   fedOk        0  -> it was called and EVERY call refused (refusedGate vs refusedCtx says which, and both are printed);
 *   added        0  -> calls passed the gate but no segment's output wrote a CB0 that n48_dl_from_output would record;
 *   clears     > 0  -> entries WERE recorded and an arm/epoch change (or a CB1-7 base) cleared them. n48_dl_clear counts
 *                      only when `n` was non-zero, so a non-zero `clears` always means something was really thrown away;
 *   unmapDropped>0  -> entries were recorded and an unmapVA dropped them, and nothing was ever cleared.
 * When both `clears` and `unmapDropped` are non-zero the ledger keeps no ordering, so CLEARED is reported and the raw
 * `unmaps` counter on the same line is what says an unmap also happened. SUSPECTED: nothing else can empty it. */
enum {
    N48_DL_WHY_HOLDING = 0,   /* not empty - there is nothing to explain */
    N48_DL_WHY_NEVER_FED,     /* feeds == 0 */
    N48_DL_WHY_ALL_REFUSED,   /* feeds > 0, fedOk == 0 */
    N48_DL_WHY_NO_TARGET,     /* fedOk > 0, added == 0 */
    N48_DL_WHY_CLEARED,       /* added > 0, clears > 0 */
    N48_DL_WHY_UNMAPPED,      /* added > 0, clears == 0, unmapDropped > 0 */
    N48_DL_WHY_UNKNOWN,       /* recorded, gone, and neither a clear nor an unmap accounts for it */
    /* 0.0.394: NOT EMPTY, and every entry it holds was KEPT across a COVERING unmap and marked
     * withdrawn-by-unmap. The state exists only while `gfxneuter 29` is on, so this arm is unreachable on any switch-off
     * boot and the classification below is byte-for-byte 0.0.393's there. It is what makes a `led 1>1` after the fill's own
     * unmap readable as "kept for the key to judge", not as an ordinary hold. */
    N48_DL_WHY_WITHDRAWN,     /* n > 0 and EVERY entry carries N48_DL_F_WITHDRAWN */
    /* build 0.0.532 item 11 (b): empty, and the only thing that took entries back is the UN-FEED (a frame whose
     * IB never ran: keystone-withdrawn, token-mismatched or walk-NOPed): added > 0, clears == 0, unmapDropped == 0,
     * unfeedRemoved > 0. RUN AC's `led 0>0 ... why ?` was this. Asked after UNMAPPED, before UNKNOWN: every earlier answer is unchanged. */
    N48_DL_WHY_UNFED,
    N48_DL_WHY_N
};

static inline const char *n48_dl_why_name(uint32_t w)
{
    static const char *const n[N48_DL_WHY_N] = {
        "holding", "NEVER-FED", "all-refused", "no-target", "CLEARED", "unmapped", "?", "withdrawn", "un-fed" };
    return w < N48_DL_WHY_N ? n[w] : "?";
}

/* WHY the ledger is in the state it is in, from its own counters. Pure; reads, never writes. */
static inline uint32_t n48_dl_empty_why(const n48_dl *l)
{
    if (!l) return N48_DL_WHY_UNKNOWN;
    if (l->n) {
        /* 0.0.394: holding, but is EVERY held entry one the keep carried across a covering unmap? A run reads
         * this as the new state by name rather than as an ordinary hold. Off the switch no entry is ever marked, so the
         * loop below never changes the off-path answer. */
        uint32_t allw = 1u;
        for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++)
            if (!(l->e[k].flags & N48_DL_F_WITHDRAWN)) { allw = 0u; break; }
        return allw ? N48_DL_WHY_WITHDRAWN : N48_DL_WHY_HOLDING;
    }
    if (!l->feeds) return N48_DL_WHY_NEVER_FED;
    if (!l->fedOk) return N48_DL_WHY_ALL_REFUSED;
    if (!l->added) return N48_DL_WHY_NO_TARGET;
    if (l->clears) return N48_DL_WHY_CLEARED;
    if (l->unmapDropped) return N48_DL_WHY_UNMAPPED;
    if (l->unfeedRemoved) return N48_DL_WHY_UNFED;   /* build 0.0.532 item 11 (b) */
    return N48_DL_WHY_UNKNOWN;
}

/* THE LINE gfxsrc_decide_frame prints for each judged frame while the descriptor path is on. The %s, in order, are
 * n48_cm_reason_name (gfx_commit.h), n48_xv_reason_name (gfx_xlat_verdict.h), n48_dl_why_name (above) and
 * n48_dep_reason_name (gfx_dep.h - not included here; the kext supplies it, and the test's bound uses a longer filler).
 * Fields: f = the judged frame number the verdict line uses; arm = n48_cm_shot_level's answer for THIS frame, not gXdArm;
 * ep = the epoch value the sync was handed; led = gXdLed.n before>after the sync; cl = clears; fd = feeds/fedOk/added;
 * fed = 1 when n48_dl_feed was called this frame; gate/vrd/add/rg/rc = the feed's own inputs and outcome; why = the
 * classification above; dep/d/obs/ns/sn/fg = n48_dep_check's reason and detail, the world's observers, the RAW
 * gGs.neuteredSubs, the world's source_neuters and what a forgiveness subtracted - all four AS THE COMMIT GATE READ THEM.
 * `dep GATE-NOT-RUN` means gfxsrc_commit_try was not reached on this frame, so there is no gate reading to report.
 * Its worst case is bounded under n48log's 491-byte body cap by tests/gfx_desc_port_test.cpp group D. */
/* build 0.0.448 (review addition item 7) — `rf` is `reFeedDropped` (A6, above): re-feeds of an already-fed
 * surface that this build now DROPS (a mode/page/size mismatch), counted since 0.0.447 but never printed anywhere
 * until now. Measured by tests/gfx_desc_port_test.cpp group D under the SAME 491-byte body-cap bound as every
 * other field on this line. */
#define N48_DL_LINE_FMT \
    "dpled841: f%llu arm %u ep %u led %u>%u cl %llu fd %llu/%llu/%llu fed %u gate %s vrd %s add %u rg %llu rc %llu " \
    "why %s dep %s d %#llx obs %#x ns %llu sn %llu fg %llu rf %llu"

#endif /* N48_GFX_DESC_PORT_H */
