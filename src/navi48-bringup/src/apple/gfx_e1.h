// gfx_e1.h — RULE E1: the ONE arm-time ring history that may let X9 v2's EARLY observer go live.
//
// THE PROBLEM. X9 v2's EARLY observer (gfx_dep.h N48_DEP_OBS_EARLY) is live only when the GFX ring held NO
// INDIRECT_BUFFER at the moment the render drain armed. On this recipe it never does: Apple's
// AMDPM4HWChannel::performClearState submits one IB before any client frame, so `pre_ibs` is 1, EARLY is not live,
// X9 v2 refuses EVERY frame, and COMMIT can never fire. hp1/hp2/hp5 all say the same sentence:
//   `ARM-TIME WALK ... 1 INDIRECT_BUFFER(s) of any VMID ... EARLY observer is NOT live - X9 v2 refuses every frame`.
//
// WHAT's RUNS ESTABLISHED. That one IB is OUR OWN 16-dword NOP clear-state page, printed verbatim:
//   `ARM-TIME IB 1 of 1 ... VA 0x840fc39000, len 16 dw, VMID 0 (control 0x80000010) ... 16 of 16 ... 0xffff1000`.
// An IB of 16 one-dword NOPs executes nothing and writes no surface, so a boot whose ring held only that one is, for
// the purpose EARLY exists to serve, a boot on which nothing ran unobserved.
//
// WHY THE FIRST DESIGN WAS UNSOUND, AND WHAT THIS ONE ADDS.'s E1 tested "VMID 0, our address, <= 16 dwords, every
// dword a NOP". EVERY ONE OF THOSE CLAUSES HELD ON hp2 — and hp2's ring was Apple's ring AFTER A GPU RESET
// (`ARM-TIME WALK ... [0..512)`, four frames; every other logged run 128). The clauses were true of a history that
// was not the history they were meant to certify. The clauses below are's, tightened by the safety review
// in the project notes ("SAFETY REVIEW — E1 / headless coverage / N1", Decision 1) and by the hp2 analysis:
//
//   1. EXACT, never "<=":   control word == 0x80000010 EXACTLY (which is VMID 0 and length 16 in one compare),
//                           dw1 & 3 == 0, VA == THIS BOOT's published clear-state MC (read from the `csb:` publish,
//                           never a constant), and 16 of 16 dwords read back == 0xffff1000.
//   2. THE PAGE IS OURS IN HARDWARE: the GART PTE for that MC, read back out of the page table, equals our own bus
//                           address plus the flags we wrote — AND `gmc_bind_at` refused ZERO overlapping Apple binds
//                           this boot. (hp1/hp2 logged 0 refusals against 12 `bind_at #` lines. A single refusal means
//                           Apple tried to map something over our bump region, so "only we write that page" is no
//                           longer an observation.)
//   3. A PACKET ALLOWLIST over the whole of [0, wptr): our IB and RELEASE_MEM, nothing else. EARLY and the
//                           look only at IBs, so a WRITE_DATA / DMA_DATA / COPY_DATA sitting DIRECTLY in the ring —
//                           which writes memory without any IB at all — passed both. It refuses here.
//   4. RING GENERATION: the GFX queue mapped EXACTLY ONCE and never unmapped, and NO reset or re-init since boot.
//                           This is the clause hp2 fails. See n48_e1_in::engine_resets / hw_reset_keys.
//   5. A NAMED VERDICT: the failing clause is returned, so the log line says WHICH one refused.
//
// DIRECTION OF ERROR. Every clause is a conjunct and every input is POSITIVELY set by the caller. A zero-initialised
// n48_e1_in refuses (N48_E1_NOT_GATHERED) — it does not read as "a clean ring nobody looked at". An input the kext
// forgets to fill refuses. A counter that cannot be read refuses. E1 can only ever make EARLY live in the one case it
// names; it can never make any other rung of X9 easier to pass, and n48_dep_fill still requires all seven observers.
//
// This header is pure: no kernel headers, no I/O, no globals. tests/gfx_e1_test.cpp runs exactly this code.
#ifndef N48_GFX_E1_H
#define N48_GFX_E1_H

#include <stdint.h>

/* Apple's 1-dword NOP padding, the same constant gfx_neuter.h calls N48_TMPL_NOP. Spelled out here so this header
 * stands alone and a change to one cannot silently move the other. */
#define N48_E1_NOP        0xFFFF1000u
/* performClearState's INDIRECT_BUFFER control dword, EXACTLY: bit 31 (VALID) | length 0x10 dwords, VMID nibble 0.
 * printed it: `len 16 dw, VMID 0 (control 0x80000010)`. */
#define N48_E1_IB_CTL     0x80000010u
#define N48_E1_IB_DWORDS  16u
/* The only two PM4 opcodes [0, wptr) may contain. */
#define N48_E1_OP_IB      0x3Fu   /* INDIRECT_BUFFER */
#define N48_E1_OP_RELMEM  0x49u   /* RELEASE_MEM */
/* How many type-3 packets the caller may record. More than this refuses (N48_E1_PACKET_OVERFLOW): an allowlist that
 * did not see every packet is not an allowlist. */
#define N48_E1_MAX_PKTS   16u

/* The verdict. 0 is the ONLY value that lets EARLY go live on a ring that held an IB. Every other value names the
 * clause that refused, in the order the clauses are tested. APPENDED, never inserted: the indices are printed. */
enum {
    N48_E1_PASS = 0,
    N48_E1_NOT_GATHERED,     /* the input was never filled from live state - a zero input is NOT a clean ring */
    N48_E1_NOT_WALKED,       /* the arm-time walk did not run, or the ring had wrapped, or the walk stopped short */
    N48_E1_NO_IBS,           /* the ring held no IB: E1 does not apply and must not be the reason EARLY is live */
    N48_E1_TOO_MANY_IBS,     /* more than one INDIRECT_BUFFER in [0, wptr) */
    N48_E1_NO_IB_RECORD,     /* the walk found an IB but the caller recorded none of its words */
    N48_E1_CSB_UNPUBLISHED,  /* our clear-state page was never published this boot: there is nothing to match against */
    N48_E1_IB_CONTROL,       /* the control dword is not 0x80000010 exactly (wrong VMID, or a length that is not 16) */
    N48_E1_IB_DW1_LOW,       /* dw1 & 3 != 0: the low bits of the address word are not the ones we published */
    N48_E1_IB_VA,            /* the IB's VA is not THIS boot's clear-state MC */
    N48_E1_IB_SIZE,          /* the size we reported through TTL slot 13 is not the 16 dwords the control word claims */
    N48_E1_IB_READ,          /* fewer than 16 dwords could be read back from the page */
    N48_E1_IB_WORDS,         /* the 16 dwords read back are not 16 of 16 Apple 1-dword NOPs */
    N48_E1_PTE_UNREAD,       /* the GART PTE for that MC could not be read out of the page table */
    N48_E1_PTE_MISMATCH,     /* ... and it does not name our own bus address plus the flags we wrote */
    N48_E1_BIND_REFUSALS,    /* gmc_bind_at refused at least one overlapping Apple bind this boot */
    N48_E1_PACKET_OVERFLOW,  /* more type-3 packets than the caller recorded: the allowlist did not see them all */
    N48_E1_PACKET_OTHER,     /* a packet in [0, wptr) is neither our IB nor a RELEASE_MEM */
    N48_E1_PACKET_IB_ALIAS,  /* an INDIRECT_BUFFER packet at a position that is not the one IB we matched */
    N48_E1_QUEUE_MAPS,       /* the GFX queue was not mapped exactly once before arming */
    N48_E1_QUEUE_UNMAPPED,   /* ... or was unmapped: the ring the walk read is not the ring that was mapped */
    N48_E1_RING_RESET,       /* an engine reset or a reset-and-replay bracket happened before arming (hp2) */
    N48_E1_CLAUSES
};

static inline const char *n48_e1_clause_name(uint32_t c)
{
    static const char *const n[N48_E1_CLAUSES] = {
        "PASS", "not-gathered", "not-walked", "no-ibs", "too-many-ibs", "no-ib-record", "csb-unpublished",
        "ib-control", "ib-dw1-low", "ib-va", "ib-size", "ib-read", "ib-words", "pte-unread", "pte-mismatch",
        "bind-refusals", "packet-overflow", "packet-other", "packet-ib-alias", "queue-maps", "queue-unmapped",
        "ring-reset" };
    return c < N48_E1_CLAUSES ? n[c] : "?";
}

/* Everything E1 judges, as the kext read it AT ARM TIME. Zero-initialised; `gathered` must be POSITIVELY set. */
typedef struct {
    uint32_t gathered;        /* 1 when render_drain_arm filled this - REQUIRED, or E1 refuses NOT_GATHERED */

    /* --- the arm-time walk of [0, wptr) ------------------------------------------------------------------- */
    uint32_t walked;          /* 1 when the walk ran */
    uint32_t wrapped;         /* 1 when the ring had wrapped, so [0, wptr) is not all it ever held */
    uint32_t stopped;         /* 1 when the walk stopped before reaching wptr */
    uint32_t ibs;             /* INDIRECT_BUFFERs of any VMID the walk found */
    uint32_t packets;         /* type-3 packets the walk passed */
    uint32_t pkt_recorded;    /* ... of which this many opcodes/positions were recorded below */
    uint32_t pkt_op[N48_E1_MAX_PKTS];   /* each recorded packet's opcode */
    uint32_t pkt_pos[N48_E1_MAX_PKTS];  /* ... and its ring dword position */

    /* --- the one IB, read through our own mapping of the page it names -------------------------------------- */
    uint32_t ib_recorded;     /* 1 when the fields below were filled from a real packet */
    uint32_t ib_pos;          /* its ring dword position, to match against pkt_pos */
    uint32_t ib_ctl;          /* the control dword, verbatim */
    uint32_t ib_dw1;          /* the low address dword, verbatim (its bottom 2 bits are the test) */
    uint64_t ib_va;           /* (hi << 32) | (dw1 & ~3) */
    uint32_t ib_read;         /* dwords actually read back from the page */
    uint32_t ib_nops;         /* ... of which this many equal N48_E1_NOP */

    /* --- our clear-state page, from THIS boot's publish (navi48_csb_peek), never a constant ------------------ */
    uint32_t csb_published;   /* 1 when navi48_csb_peek returned a page */
    uint64_t csb_mc;          /* the GART MC address we published through TTL slot 13 */
    uint32_t csb_bytes;       /* the size we reported with it (64 = 16 dwords) */

    /* --- the page's GART PTE, read back out of the page table, and the GART's own refusal count -------------- */
    uint32_t pte_read_ok;     /* 1 when the PTE for csb_mc was read */
    uint64_t pte;             /* what the page table holds */
    uint64_t pte_want;        /* (our bus address & ~0xfff) | the flags gmc_bind_existing writes */
    uint64_t bind_refusals;   /* gmc_bind_at's overlap refusals this boot - must be 0 */

    /* --- ring generation ------------------------------------------------------------------------------------ */
    uint32_t gfx_queue_maps;    /* MAP_QUEUES(engine_sel 4) seen on the KIQ ring before arming - must be exactly 1 */
    uint32_t gfx_queue_unmaps;  /* UNMAP_QUEUES(engine_sel 4) - must be 0 */
    uint64_t engine_resets;     /* TTL slot 39 resetEngineQueue calls before arming - must be 0 (hp2: 2) */
    uint64_t hw_reset_keys;     /* AccelPeer reset-hardware-and-replay brackets before arming - must be 0 (hp2: 4) */
} n48_e1_in;

/* THE RULE. Pure. Returns N48_E1_PASS, or the first clause that refused. */
static inline uint32_t n48_e1_eval(const n48_e1_in *e)
{
    if (!e || e->gathered != 1u) return N48_E1_NOT_GATHERED;

    /* The walk must have covered the whole of [0, wptr) and reached its end. A wrapped ring is a ring whose earlier
     * contents we never saw; a stopped walk is a ring we could not parse to the end. Either way there is history E1
     * cannot certify. */
    if (e->walked != 1u || e->wrapped || e->stopped) return N48_E1_NOT_WALKED;

    /* E1 exists only to answer "the ring held ONE IB, and it was ours". It must never be the reason EARLY is live on
     * a ring that held none - that case is EARLY's own rung in n48_dep_fill and is not E1's to grant. */
    if (e->ibs == 0u) return N48_E1_NO_IBS;
    if (e->ibs != 1u) return N48_E1_TOO_MANY_IBS;
    if (e->ib_recorded != 1u) return N48_E1_NO_IB_RECORD;

    /* Clause 1: EXACT, and against THIS boot's published address. */
    if (e->csb_published != 1u || e->csb_mc == 0u) return N48_E1_CSB_UNPUBLISHED;
    if (e->ib_ctl != N48_E1_IB_CTL) return N48_E1_IB_CONTROL;
    if ((e->ib_dw1 & 3u) != 0u) return N48_E1_IB_DW1_LOW;
    if (e->ib_va != e->csb_mc) return N48_E1_IB_VA;
    if (e->csb_bytes != N48_E1_IB_DWORDS * 4u) return N48_E1_IB_SIZE;
    if (e->ib_read != N48_E1_IB_DWORDS) return N48_E1_IB_READ;
    if (e->ib_nops != N48_E1_IB_DWORDS) return N48_E1_IB_WORDS;

    /* Clause 2: the page is ours in hardware. The PTE says the address the GPU will actually fetch from; the refusal
     * count says whether anything of Apple's ever tried to map over the region that PTE lives in. */
    if (e->pte_read_ok != 1u) return N48_E1_PTE_UNREAD;
    if (e->pte != e->pte_want || e->pte_want == 0u) return N48_E1_PTE_MISMATCH;
    if (e->bind_refusals != 0u) return N48_E1_BIND_REFUSALS;

    /* Clause 3: the packet allowlist over the WHOLE of [0, wptr). A WRITE_DATA, DMA_DATA or COPY_DATA sitting
     * directly in the ring writes memory with no IB at all, so neither EARLY nor's E1 would have seen it. */
    if (e->packets > N48_E1_MAX_PKTS || e->pkt_recorded != e->packets) return N48_E1_PACKET_OVERFLOW;
    {
        uint32_t ibPackets = 0u;
        for (uint32_t i = 0; i < e->pkt_recorded; i++) {
            const uint32_t op = e->pkt_op[i];
            if (op == N48_E1_OP_RELMEM) continue;
            if (op != N48_E1_OP_IB) return N48_E1_PACKET_OTHER;
            if (e->pkt_pos[i] != e->ib_pos) return N48_E1_PACKET_IB_ALIAS;
            ibPackets++;
        }
        /* Exactly one INDIRECT_BUFFER packet, and it is the one at the position we read and matched above. */
        if (ibPackets != 1u) return N48_E1_PACKET_IB_ALIAS;
    }

    /* Clause 4: ring generation. hp2 armed on a ring Apple had reset under us - every clause above held on it. */
    if (e->gfx_queue_maps != 1u) return N48_E1_QUEUE_MAPS;
    if (e->gfx_queue_unmaps != 0u) return N48_E1_QUEUE_UNMAPPED;
    if (e->engine_resets != 0u || e->hw_reset_keys != 0u) return N48_E1_RING_RESET;

    return N48_E1_PASS;
}

#endif /* N48_GFX_E1_H */
