// gfx_pgmid.h — 0.0.434 (notes/design/PGMID-COPYGUARD.md Part 1). PROGRAM-IDENTITY COST, THE PURE HALF.
//
// measured decide37's 41.9 ms 2-IB pass as WORK, not waiting: ~90% of it is `gfxsrc_pgm_profile` reading 512
// dwords through the MM window (~2 us/dword) on EVERY call, with no memo. Design T (this file): read the 4-dword
// head first; `n48_pgm_need` answers the LARGEST ndw among identity rows of that stage sharing that exact head, and
// that many dwords (never more) are enough to reproduce `xlat12_shader_id_match`'s answer for ANY n at or above it —
// every candidate row's own `code[0..ndw)` is already inside a buffer that long, and a row whose head differs can
// never match regardless of n. A head with NO same-head row of that stage needs no further read at all: no row
// could match no matter how many more dwords came back, so `n48_pgm_need` returning 0 is itself the honest refusal
// after 4 dwords, not a guess. THE INVARIANT this rests on (identity cannot change): `xlat12_shader_id_match` skips
// any row whose head or stage disagrees at the very first test, and needs no more than that row's own `ndw` dwords
// to decide the rest (its own FNV, over its own `ndw`) — so `n48_pgm_need`'s maximum is provably sufficient and
// never too small for any row it could possibly match.
//
// Pure C++ (static inline / a small POD row type), so the kext and its host suite (tests/gfx_pgmid_test.cpp) run the
// SAME arithmetic over the SAME kind of row. This header knows nothing of `xlat12_shader_id`'s own layout (that
// struct is private to xlat12_ib.c, per xlat12_shader_ids.h's own comment) — the caller hands it an array of
// `n48_pgmid_row`, built once per boot from the exported by-index accessor `xlat12_shader_id_row` (xlat12_ib.h), or
// (in the host test) directly from the real table via the same accessor, or from synthetic rows for a planted break.
#ifndef N48_GFX_PGMID_H
#define N48_GFX_PGMID_H

#include <stdint.h>

// The HEAD-FIRST read's own cap: no read this design asks for may ever exceed it, whatever a future identity row's
// own `ndw` claims. Through 0.0.483 it was 512, equal to AppleHardwareHook.cpp's kXdIdDwords (the OFF/SHADOW window).
// build 0.0.484 (notes/design/GLASS.md Q2 K4): 1344. glass_background_lph's gfx1201 images are 1271 (BD) and 1269
// (BE) dwords, so at 512 xlat12_shader_id_match's `s->ndw > n` skip made them unmatchable under mode T/T+M too. The two
// caps are now DIFFERENT numbers with different jobs: kXdIdDwords (gfx_subst_caps.h N48_XD_ID_DWORDS, still 512) is
// what mode OFF/SHADOW reads and what the no-identity witness is grown to; N48_PGMID_CAP bounds a head-first read of
// n48_pgm_need's answer. Every buffer a head-first read lands in is at least this wide: gXdPgm is kXdPgmDwords
// (N48_XD_PGM_DWORDS, 1344) and gXdPgmT is allocated N48_PGMID_CAP wide; gfx_subst_caps.h static_asserts
// N48_PGMID_CAP <= N48_XD_PGM_DWORDS, so raising this without raising K3 does not compile.
#define N48_PGMID_CAP 1344u

// The head-first read's own first step: 4 dwords, exactly what xlat12_shader_id_match needs to test a row's head
// before anything else (n >= 4u is its own minimum; every real row's own head[] is 4 dwords).
#define N48_PGMID_HEAD_DWORDS 4u

typedef struct {
    uint32_t stage;
    uint32_t ndw;
    uint32_t head[4];
} n48_pgmid_row;

// n48_pgm_need: over `rows[0..count)`, the largest `ndw` among rows whose `stage` and 4-dword `head` both match the
// caller's — capped at N48_PGMID_CAP so a future identity row longer than the cap can never make this function ask
// a caller for more than its own buffer holds. 0 means no row shares this (stage, head) at all: a head-only
// refusal, correct after 4 dwords because xlat12_shader_id_match could not have matched any row either (every row
// it could match must share the SAME head test this function just ran).
static inline uint32_t n48_pgm_need(uint32_t stage, const uint32_t head[4], const n48_pgmid_row *rows, uint32_t count)
{
    uint32_t best = 0u;
    if (!head || !rows) return 0u;
    for (uint32_t i = 0; i < count; i++) {
        if (rows[i].stage != stage) continue;
        if (rows[i].head[0] != head[0] || rows[i].head[1] != head[1] ||
            rows[i].head[2] != head[2] || rows[i].head[3] != head[3]) continue;
        if (rows[i].ndw > best) best = rows[i].ndw;
    }
    return best > N48_PGMID_CAP ? N48_PGMID_CAP : best;
}

// The switch's built modes (0.0.436: T+M is now built, and SHADOW-M joins it; 0xFF is OFF).
enum {
    N48_PGMID_MODE_OFF      = 0xFFu,
    N48_PGMID_MODE_T        = 1u,
    N48_PGMID_MODE_TM       = 2u,   // 0.0.436: T + the per-pass memo below
    N48_PGMID_MODE_SHADOW   = 3u,
    N48_PGMID_MODE_SHADOW_M = 4u    // 0.0.436: T+M run as a side trial against HEAD's full read, HEAD's answer used
};

// =====================================================================================================================
// 0.0.436 (notes/design/PGMID-COPYGUARD.md Part 1, design "2. M") — THE PER-PASS PROGRAM-IDENTITY MEMO.
// =====================================================================================================================
// Keyed by (stage, VA); cleared once per gfxsrc_policy pass (AppleHardwareHook.cpp, before the segment loop). A row
// also carries the copy-guard ring mark taken BEFORE the read that filled it (n48_cg_ring_mark, gfx_copyguard.h) and
// at most N48_PM_PAGES VRAM pages that read touched.
// build 0.0.484 (notes/design/GLASS.md Q2 K4) — THE PAGE PROOF, REDONE FOR THE NEW CAP. Through 0.0.483 a row held
// 2 pages, and 2 was provably enough because N48_PGMID_CAP was 512 dwords = 2048 bytes: a byte range that short crosses
// at most one 4 KiB boundary. At 1344 dwords (5376 bytes) that proof no longer holds: a 5084-byte glass read from a
// 256-byte-aligned VA at page offset 0xd00 or later ends in a THIRD page, and a 2-page row would refuse to store it
// (pm_not_stored - fail-closed, but every glass ask would then re-read ~1271 dwords through the MM window). Every read
// one head-first ask makes (the 4-dword head, the [4, L) tail, and the witness extension, which is at most kXdIdDwords)
// covers a SINGLE byte range [va, va + 4 * got) with got <= N48_PGMID_CAP, and a byte range of B bytes spans at most
// ceil((B - 1) / 4096) + 1 pages; for B <= (N48_PM_PAGES - 1) * 4096 = 8192 that is at most 3. So 3 is PROVABLY enough
// at this cap, and the static_assert below makes a cap the proof does not cover fail to compile rather than silently
// fall back to refusing. A read from system memory notes no page at all (gfxc_read_core), which stays a valid 0-page row.
#define N48_PM_PAGES 3u
static_assert(N48_PGMID_CAP * 4u <= (N48_PM_PAGES - 1u) * 4096u,
              "a head-first read of N48_PGMID_CAP dwords must span at most N48_PM_PAGES 4 KiB pages, or a memo row "
              "cannot name every page it depends on - grow N48_PM_PAGES (and redo the proof above) with the cap");
//
// A HIT is valid only when (a) the ring mark is UNCHANGED since the row was filled — any Part-2 guard event
// invalidates it, not merely an overlapping one, because this header cannot itself judge overlap against pages it
// does not track — and (b) none of the row's pages are currently poisoned. This header knows nothing of the ring or
// poison table's STORAGE (gCgRing/gCgPoison live in Navi48Bringup.cpp, like every other piece of gfx_copyguard.h's
// own state) — the caller hands it the current ring mark and answers the poison question itself; this header is
// purely the table and its (stage,VA) arithmetic, so the kext and tests/gfx_pgmid_test.cpp run the SAME code.
#define N48_PM_ROWS 32u

typedef struct {
    uint32_t stage;
    uint64_t va;
    int32_t  id;
    uint64_t fillMark;
    uint32_t npages;
    uint64_t pages[N48_PM_PAGES];
} n48_pm_row;

typedef struct {
    n48_pm_row row[N48_PM_ROWS];
    uint32_t n;
} n48_pm;

static inline void n48_pm_clear(n48_pm *m) { m->n = 0u; }

// Keyed by (stage, VA) — BOTH must match. A table keyed on VA alone would answer stage 0's ask with stage 1's row
// whenever the two happen to share an address (the design's own planted break).
static inline const n48_pm_row *n48_pm_lookup(const n48_pm *m, uint32_t stage, uint64_t va)
{
    for (uint32_t i = 0; i < m->n; i++)
        if (m->row[i].stage == stage && m->row[i].va == va) return &m->row[i];
    return 0;
}

// Fills or refreshes the (stage,va) row: if one already exists it is updated IN PLACE (a re-fill after an
// invalidation never grows the table), else a new row is appended if there is room. `npages` > N48_PM_PAGES or a
// full table both store nothing — the caller counts which. Never evicts: a full table simply stops accepting NEW keys.
// build 0.0.484: N48_PM_PAGES (3) replaces the literal 2 (see the page proof above); `pages` is read only at
// [0, npages), after the npages check, so a caller may hand it a longer array (the per-call recorder's own).
static inline int n48_pm_store(n48_pm *m, uint32_t stage, uint64_t va, int32_t id, uint64_t fillMark,
                                const uint64_t *pages, uint32_t npages)
{
    if (npages > N48_PM_PAGES) return 0;
    n48_pm_row *r = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->row[i].stage == stage && m->row[i].va == va) { r = &m->row[i]; break; }
    if (!r) {
        if (m->n >= N48_PM_ROWS) return 0;   // full: stores nothing, never evicts an existing key
        r = &m->row[m->n++];
        r->stage = stage; r->va = va;
    }
    r->id = id; r->fillMark = fillMark; r->npages = npages;
    for (uint32_t k = 0; k < N48_PM_PAGES; k++) r->pages[k] = k < npages ? pages[k] : 0ull;
    return 1;
}

// build 0.0.484 — THE MEMO FILL'S OWN RULE, one copy for AppleHardwareHook.cpp's gfxsrc_pgmid_memo_fill and
// tests/gfx_pgmid_test.cpp's model: a per-call page recorder that OVERFLOWED (it could not name every page the read
// touched) or named more than N48_PM_PAGES pages stores nothing (fail-closed: the next ask simply reads again); anything
// else goes to n48_pm_store with the recorder's own page array. 1 = stored.
static inline int n48_pm_store_rec(n48_pm *m, uint32_t stage, uint64_t va, int32_t id, uint64_t fillMark,
                                    const uint64_t *recPages, uint32_t recN, uint32_t recOverflow)
{
    if (recOverflow || recN > N48_PM_PAGES) return 0;
    return n48_pm_store(m, stage, va, id, fillMark, recPages, recN);
}

// 0.0.436 DEFECT FIX (reviewer, post-0.0.436): `n48_pm_pages_of` (a page range derived from `va`, the program's
// VMID-2 GPU VA) is DELETED. The copy guard's slots, ring events and poison rows are all VRAM OFFSETS
// (gfxc_read_core's own `voff`, after VA-to-VRAM translation) — deriving a row's pages from `va` compared them
// against the wrong coordinate space, so a poisoned or in-flight VRAM range could never be found by a lookup keyed
// on the program's VA pages, and a hit would note the WRONG pages into the segment's recorder. A row's pages must
// instead come from the per-call n48_cg_pagerec (gfx_copyguard.h) the resolving gfxc_read_rs call itself was handed
// as its recorder — see AppleHardwareHook.cpp's gfxsrc_pgmid_memo_fill, the ONLY caller this ever had.

// ---- Always-on instruments. One instance, fed on every gfxsrc_pgm_profile call whatever the switch is (mode OFF
// only ever bumps `asks` and `mm_dwords`, matching HEAD's one gfxc_read of kXdIdDwords exactly). Units: whole
// microseconds for time, dwords for MM traffic. ----
typedef struct {
    uint64_t asks;             // gfxsrc_pgm_profile calls that reached the identity read
    uint64_t pgm_us;           // wall time in the identity-read + match pair, accumulated, ALWAYS timed
    uint64_t mm_dwords;        // dwords actually pulled through the MM window for identity by this call
    uint64_t head_only;        // mode T/SHADOW's new path: L == 0, stopped after the 4-dword head (no same-head row)
    uint64_t witness_full;     // the no-identity witness's first-sighting read was grown to the full window
    // 0.0.437: a first-sighting extension was SKIPPED because the no-identity witness table
    // (gXdMissN/kXdMissRows, AppleHardwareHook.cpp) was already full - a new address could never be stored there,
    // so the 512-dword read this would otherwise have grown to was skipped rather than read and thrown away.
    uint64_t witness_skip_full;
    uint64_t shadow_compares;  // SHADOW/SHADOW-M: old and new both computed and compared
    uint64_t disagreed;        // ... of which SHADOW's two answers (matched id, or both unmatched) differed
    // 0.0.436 — mode T+M / SHADOW-M's own memo counters.
    uint64_t pm_hits;          // a VALID memo hit (ring mark unmoved, no page poisoned): the read was skipped
    uint64_t pm_misses;        // the memo had no row at all for this (stage,va)
    uint64_t pm_inval;         // a row existed but the ring mark had moved since it was filled
    uint64_t pm_poison_miss;   // a row existed, the ring mark agreed, but one of its pages is now poisoned
    uint64_t pm_not_stored;    // a miss's answer was NOT written back: too many pages, or the table is full
    uint64_t m_disagreed;      // SHADOW-M: the memo path's answer differed from HEAD's
} n48_pgmid_stats;

static inline uint32_t n48_pgmid_cap32(uint64_t v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}

// ONE line, printed on every read of the `gfxneuter` report whatever switch (38) was thrown — the instrument is
// always on; only which read path runs is gated. Measured against this real format string by
// tests/gfx_pgmid_test.cpp (not this comment), well under the logger's 512-byte body cap.
#define N48_PGMID_FMT \
    "pgmid: %s; asks %u, %u us, %u MM dwords, head-only refusals %u, witness full reads %u, witness skipped " \
    "(table full) %u, shadow compares %u, DISAGREED %u, memo hits %u misses %u invalidations %u poison-misses %u " \
    "not-stored %u, M-DISAGREED %u"

#define N48_PGMID_ARGS(mode_str, s) \
    (mode_str), \
    n48_pgmid_cap32((s)->asks), n48_pgmid_cap32((s)->pgm_us), n48_pgmid_cap32((s)->mm_dwords), \
    n48_pgmid_cap32((s)->head_only), n48_pgmid_cap32((s)->witness_full), n48_pgmid_cap32((s)->witness_skip_full), \
    n48_pgmid_cap32((s)->shadow_compares), n48_pgmid_cap32((s)->disagreed), \
    n48_pgmid_cap32((s)->pm_hits), n48_pgmid_cap32((s)->pm_misses), n48_pgmid_cap32((s)->pm_inval), \
    n48_pgmid_cap32((s)->pm_poison_miss), n48_pgmid_cap32((s)->pm_not_stored), n48_pgmid_cap32((s)->m_disagreed)

#endif /* N48_GFX_PGMID_H */
