// gfx_t0src.h - build 0.0.524 (an internal design note items 1-7): WHO WROTE S's TEXTURE 0. Switch 79 (`t0src`),
// default OFF. Pure, header-only, host-tested (tests/gfx_t0src_test.cpp). READ-ONLY BY CONSTRUCTION: nothing here writes a register,
// a page table, VRAM, a translated dword, Apple's objects, or anything a rule, verdict, rung, gate, ledger or copy guard reads. Every
// function writes only its caller's own ring / table / out-parameters.
//
// WHAT IT HOLDS:
//   - a MAP RING of N48_T0_MAP_N entries: one per AMDHWVMContext::mapVA of a context we recorded (hook_mapVA, AFTER Apple's mapVA
//     returned): {seq, up_ms, frame, ctxSeq, pid, va, off, size, mem, memLen, mem0c, flags, rc8, cls};
//   - an UNMAP RING of N48_T0_UNMAP_N entries: one per unmapVA, recorded at its ENTRY: {seq, up_ms, ctxSeq, va, size};
//   - an N48_T0_CLS_N-entry interned CLASS-NAME table (the IOAccelMemory's getMetaClass()->getClassName(), read by the hook).
// Writers take an index with an atomic fetch-add, invalidate the slot (seq 0), fill it, and store `seq` LAST with release. A reader
// accepts a slot only if its seq is the same non-zero value before and after the copy and names that slot; anything else is TORN.
//
// THE PROBE (gfxsrc_c88_frame_end, switch 79, B1 frames only, its own cap of N48_T0_LINES): for S's texture 0 it resolves the first
// and last page (a system page is NEVER a VRAM key), reads the first 4 KiB through the MM window (zero count, first 4 dwords), and
// asks - read-only - the producer ledger, the R5' hazard set (n48_hz_probe / n48_hz_count: const, no counter moves; never
// n48_hz_hit), the residency-provenance table, and the two rings (newest map and unmap covering the VA, and one other context that
// mapped the same memory object).
#ifndef N48_GFX_T0SRC_H
#define N48_GFX_T0SRC_H
#include <stdint.h>
#include "gfx_hazard.h"      /* n48_hazard, n48_hz_probe, n48_hz_count */
#include "gfx_desc_port.h"   /* n48_dl (the producer ledger) */
#include "ws_resprov.h"      /* n48_rp (the residency-provenance table) */

#define N48_T0_BOOT_ON   0u          /* switch 79's boot value: OFF. `79 | 1 << 8` = 335 ON, `79 | 2 << 8` = 591 OFF */
#define N48_T0_MAP_N     1024u       /* map ring entries */
#define N48_T0_UNMAP_N   256u        /* unmap ring entries */
#define N48_T0_CLS_N     8u          /* interned class names */
#define N48_T0_CLS_LEN   64u         /* characters kept per class name (+ NUL) */
#define N48_T0_CLS_NONE  0xFEu       /* no class could be read (not a kernel pointer, no metaclass) */
#define N48_T0_CLS_FULL  0xFFu       /* a class the full table could not intern */
#define N48_T0_LINES     32u         /* probe line pairs per boot */
#define N48_T0_PAGE      0x1000ull
#define N48_T0_ZERO_UNREAD 0xFFFFFFFFu   /* the zero count when the 4 KiB could not be read (page unusable or a read failed) */
#define N48_T0_VIDMEM    "AMDRadeonX6000_AMDAccelVidMemory"   /* the one class whose +0x40 is read (DisplayPipeGuard's precedent) */

/* Host tests plant a concurrent writer here (between the copy and the second seq read). Empty in the kext. */
#ifndef N48_T0_TORN_HOOK
#define N48_T0_TORN_HOOK(slot) ((void)0)
#endif

typedef struct {
    uint64_t seq;                    /* index + 1; 0 = empty or being written. Stored LAST (release) */
    uint64_t up_ms, frame;           /* uptime ms; the judged-frame count at the map */
    uint32_t ctxSeq; int32_t pid;    /* our record of the context (create #) and its process */
    uint64_t va, off, size;          /* mapVA's va, rcx = offset (Apple stores it at ctx+0xf0), r8 = size (the bounds check) */
    uint64_t mem, memLen;            /* the IOAccelMemory; its +0x40 (only for N48_T0_VIDMEM, else 0) */
    uint32_t mem0c, flags, rc8, cls; /* its +0xc (Apple reads it at 0xbe12176); r9d VmMapFlags; rc & 0xff; interned class */
} n48_t0_map;
typedef struct {
    uint64_t seq, up_ms;
    uint32_t ctxSeq, pad;
    uint64_t va, size;
} n48_t0_unmap;
typedef struct { n48_t0_map s[N48_T0_MAP_N]; uint64_t head; } n48_t0_mapring;
typedef struct { n48_t0_unmap s[N48_T0_UNMAP_N]; uint64_t head; } n48_t0_unmapring;
typedef struct { char name[N48_T0_CLS_N][N48_T0_CLS_LEN + 1u]; uint32_t ready[N48_T0_CLS_N]; uint32_t n; } n48_t0_cls;

/* ---- the class-name table ------------------------------------------------------------------------------------------------ */
static inline uint32_t n48_t0_streq(const char *a, const char *b, uint32_t max)
{
    if (!a || !b) return 0u;
    for (uint32_t i = 0; i < max; i++) {
        if (a[i] != b[i]) return 0u;
        if (!a[i]) return 1u;
    }
    return 1u;
}
/* 1 = exactly N48_T0_VIDMEM (a prefix, a suffix or a longer name is not). */
static inline uint32_t n48_t0_is_vidmem(const char *name)
{
    return (name && n48_t0_streq(name, N48_T0_VIDMEM, 64u)) ? 1u : 0u;
}
/* The index of `name` (at most N48_T0_CLS_LEN characters compared and kept), interning it if new. N48_T0_CLS_NONE for no name,
 * N48_T0_CLS_FULL when the table is full. Lock-free: a slot is claimed by fetch-add and published by its `ready` flag (release);
 * two racing threads may intern one name twice, which costs a slot and nothing else. */
static inline uint32_t n48_t0_intern(n48_t0_cls *t, const char *name)
{
    if (!t || !name) return N48_T0_CLS_NONE;
    uint32_t n = __atomic_load_n(&t->n, __ATOMIC_ACQUIRE);
    if (n > N48_T0_CLS_N) n = N48_T0_CLS_N;
    for (uint32_t i = 0; i < n; i++)
        if (__atomic_load_n(&t->ready[i], __ATOMIC_ACQUIRE) && n48_t0_streq(t->name[i], name, N48_T0_CLS_LEN)) return i;
    const uint32_t k = __atomic_fetch_add(&t->n, 1u, __ATOMIC_ACQ_REL);
    if (k >= N48_T0_CLS_N) return N48_T0_CLS_FULL;
    uint32_t j = 0u;
    for (; j < N48_T0_CLS_LEN && name[j]; j++) t->name[k][j] = name[j];
    t->name[k][j] = '\0';
    __atomic_store_n(&t->ready[k], 1u, __ATOMIC_RELEASE);
    return k;
}
static inline const char *n48_t0_cls_name(const n48_t0_cls *t, uint32_t idx)
{
    if (idx == N48_T0_CLS_NONE) return "(no class)";
    if (!t || idx >= N48_T0_CLS_N || !__atomic_load_n(&t->ready[idx], __ATOMIC_ACQUIRE)) return "(class table full)";
    return t->name[idx];
}

/* ---- the rings: writers --------------------------------------------------------------------------------------------------- */
/* Record one map. `rc` is mapVA's full return register: only its LOW BYTE is the bool (0xbe121be `sete %r12b`, then
 * `movl %r12d,%eax`); the upper bits are flag residue and are dropped here. Returns the entry's seq. */
static inline uint64_t n48_t0_map_note(n48_t0_mapring *r, uint64_t up_ms, uint64_t frame, uint32_t ctxSeq, int32_t pid,
                                       uint64_t va, uint64_t off, uint64_t size, uint64_t mem, uint64_t memLen, uint32_t mem0c,
                                       uint32_t flags, uint64_t rc, uint32_t cls)
{
    if (!r) return 0ull;
    const uint64_t idx = __atomic_fetch_add(&r->head, 1ull, __ATOMIC_RELAXED);
    n48_t0_map *s = &r->s[idx % N48_T0_MAP_N];
    __atomic_store_n(&s->seq, 0ull, __ATOMIC_RELAXED);   /* invalidate first: a reader mid-copy sees the change */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->up_ms = up_ms; s->frame = frame; s->ctxSeq = ctxSeq; s->pid = pid;
    s->va = va; s->off = off; s->size = size; s->mem = mem; s->memLen = memLen;
    s->mem0c = mem0c; s->flags = flags; s->rc8 = (uint32_t)(rc & 0xffull); s->cls = cls;
    __atomic_store_n(&s->seq, idx + 1ull, __ATOMIC_RELEASE);   /* LAST */
    return idx + 1ull;
}
static inline uint64_t n48_t0_unmap_note(n48_t0_unmapring *r, uint64_t up_ms, uint32_t ctxSeq, uint64_t va, uint64_t size)
{
    if (!r) return 0ull;
    const uint64_t idx = __atomic_fetch_add(&r->head, 1ull, __ATOMIC_RELAXED);
    n48_t0_unmap *s = &r->s[idx % N48_T0_UNMAP_N];
    __atomic_store_n(&s->seq, 0ull, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->up_ms = up_ms; s->ctxSeq = ctxSeq; s->pad = 0u; s->va = va; s->size = size;
    __atomic_store_n(&s->seq, idx + 1ull, __ATOMIC_RELEASE);   /* LAST */
    return idx + 1ull;
}

/* ---- the rings: readers --------------------------------------------------------------------------------------------------- */
enum { N48_T0_SLOT_EMPTY = 0u, N48_T0_SLOT_OK = 1u, N48_T0_SLOT_TORN = 2u };
/* One slot's consistent copy. EMPTY: never claimed. TORN: claimed but not published (a write in flight), a seq that does not
 * name this slot, or a seq that changed during the copy. */
static inline uint32_t n48_t0_map_get(const n48_t0_mapring *r, uint32_t i, n48_t0_map *out)
{
    if (!r || !out || i >= N48_T0_MAP_N) return N48_T0_SLOT_EMPTY;
    const n48_t0_map *s = &r->s[i];
    const uint64_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (a == 0ull) return (uint64_t)i < __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) ? N48_T0_SLOT_TORN : N48_T0_SLOT_EMPTY;
    if ((a - 1ull) % N48_T0_MAP_N != i) return N48_T0_SLOT_TORN;
    out->up_ms = s->up_ms; out->frame = s->frame; out->ctxSeq = s->ctxSeq; out->pid = s->pid;
    out->va = s->va; out->off = s->off; out->size = s->size; out->mem = s->mem; out->memLen = s->memLen;
    out->mem0c = s->mem0c; out->flags = s->flags; out->rc8 = s->rc8; out->cls = s->cls;
    N48_T0_TORN_HOOK(s);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != a) return N48_T0_SLOT_TORN;
    out->seq = a;
    return N48_T0_SLOT_OK;
}
static inline uint32_t n48_t0_unmap_get(const n48_t0_unmapring *r, uint32_t i, n48_t0_unmap *out)
{
    if (!r || !out || i >= N48_T0_UNMAP_N) return N48_T0_SLOT_EMPTY;
    const n48_t0_unmap *s = &r->s[i];
    const uint64_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (a == 0ull) return (uint64_t)i < __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) ? N48_T0_SLOT_TORN : N48_T0_SLOT_EMPTY;
    if ((a - 1ull) % N48_T0_UNMAP_N != i) return N48_T0_SLOT_TORN;
    out->up_ms = s->up_ms; out->ctxSeq = s->ctxSeq; out->pad = 0u; out->va = s->va; out->size = s->size;
    N48_T0_TORN_HOOK(s);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != a) return N48_T0_SLOT_TORN;
    out->seq = a;
    return N48_T0_SLOT_OK;
}
/* [base, base + max(size, 4 KiB)) covers x. A size-0 record still names its own first page. */
static inline uint32_t n48_t0_covers(uint64_t base, uint64_t size, uint64_t x)
{
    const uint64_t len = size < N48_T0_PAGE ? N48_T0_PAGE : size;
    return (x >= base && x - base < len) ? 1u : 0u;
}
/* The NEWEST (highest seq) consistent map covering `va`: 1 = found in *out. *torn counts the torn slots seen. */
static inline uint32_t n48_t0_map_newest(const n48_t0_mapring *r, uint64_t va, n48_t0_map *out, uint32_t *torn)
{
    uint32_t found = 0u;
    n48_t0_map c;
    for (uint32_t i = 0; i < N48_T0_MAP_N; i++) {
        const uint32_t g = n48_t0_map_get(r, i, &c);
        if (g == N48_T0_SLOT_TORN) { if (torn) (*torn)++; continue; }
        if (g != N48_T0_SLOT_OK || !n48_t0_covers(c.va, c.size, va)) continue;
        if (!found || c.seq > out->seq) { *out = c; found = 1u; }
    }
    return found;
}
/* The newest consistent map of the same memory object `mem` by a DIFFERENT context than `ctxSeq`. mem 0 matches nothing. */
static inline uint32_t n48_t0_map_other(const n48_t0_mapring *r, uint64_t mem, uint32_t ctxSeq, n48_t0_map *out, uint32_t *torn)
{
    uint32_t found = 0u;
    n48_t0_map c;
    if (!mem) return 0u;
    for (uint32_t i = 0; i < N48_T0_MAP_N; i++) {
        const uint32_t g = n48_t0_map_get(r, i, &c);
        if (g == N48_T0_SLOT_TORN) { if (torn) (*torn)++; continue; }
        if (g != N48_T0_SLOT_OK || c.mem != mem || c.ctxSeq == ctxSeq) continue;
        if (!found || c.seq > out->seq) { *out = c; found = 1u; }
    }
    return found;
}
static inline uint32_t n48_t0_unmap_newest(const n48_t0_unmapring *r, uint64_t va, n48_t0_unmap *out, uint32_t *torn)
{
    uint32_t found = 0u;
    n48_t0_unmap c;
    for (uint32_t i = 0; i < N48_T0_UNMAP_N; i++) {
        const uint32_t g = n48_t0_unmap_get(r, i, &c);
        if (g == N48_T0_SLOT_TORN) { if (torn) (*torn)++; continue; }
        if (g != N48_T0_SLOT_OK || !n48_t0_covers(c.va, c.size, va)) continue;
        if (!found || c.seq > out->seq) { *out = c; found = 1u; }
    }
    return found;
}
/* Full laps of a ring (0 until it wrapped): an entry older than head - N is gone ("older than the ring"). */
static inline uint64_t n48_t0_wraps(uint64_t head, uint32_t n) { return n ? head / n : 0ull; }

/* ---- the probe's page ------------------------------------------------------------------------------------------------------ */
/* (a) What a page-table walk of the texture's page gave. VRAM is the ONLY class the probe uses as a key: a SYSTEM page is not VRAM
 * (its address is host memory, whatever the offset converter says of it) and reads as unresolved for every VRAM question. */
enum { N48_T0_PG_VRAM = 0u, N48_T0_PG_SYS = 1u, N48_T0_PG_UNRES = 2u };
static inline uint32_t n48_t0_pg_class(uint32_t walked, uint32_t sys, uint32_t offOk)
{
    if (!walked) return N48_T0_PG_UNRES;
    if (sys) return N48_T0_PG_SYS;
    if (!offOk) return N48_T0_PG_UNRES;
    return N48_T0_PG_VRAM;
}
static inline uint32_t n48_t0_pg_usable(uint32_t cls) { return cls == N48_T0_PG_VRAM ? 1u : 0u; }
static inline const char *n48_t0_pg_prefix(uint32_t cls)
{
    return cls == N48_T0_PG_VRAM ? "" : cls == N48_T0_PG_SYS ? "sys:" : "unres:";
}
/* The texture's byte size from its gfx10 T# (the clock88 field positions, n48_c88_extent's; SUSPECTED from Mesa), at least a page:
 * the last page is (va + bytes - 1) & ~0xfff. An estimate for a PROBE, never for a write. */
static inline uint64_t n48_t0_tex_bytes(const uint32_t rec[8], uint32_t elem_bytes)
{
    if (!rec) return N48_T0_PAGE;
    const uint64_t w = (uint64_t)(((rec[1] >> 30) & 3u) | ((rec[2] & 0x3FFFu) << 2)) + 1ull;
    const uint64_t h = (uint64_t)((rec[2] >> 14) & 0xFFFFu) + 1ull;
    const uint64_t b = w * h * (uint64_t)(elem_bytes ? elem_bytes : 16u);
    return b < N48_T0_PAGE ? N48_T0_PAGE : b;
}
/* (b) zero dwords of `n`. */
static inline uint32_t n48_t0_zeros(const uint32_t *w, uint32_t n)
{
    uint32_t z = 0u;
    if (!w) return 0u;
    for (uint32_t i = 0; i < n; i++) if (!w[i]) z++;
    return z;
}
/* (c) the producer ledger: entries whose recorded base page covers `page` (page <= P < page + max(size, 4 KiB)); a page-0 entry
 * (no key) never matches. Returns the count; the first match's tok, va and ctx. Read-only. */
static inline uint32_t n48_t0_led_probe(const n48_dl *l, uint64_t page, uint32_t *tok, uint64_t *va, uint64_t *ctx)
{
    uint32_t c = 0u;
    if (tok) *tok = 0u;
    if (va) *va = 0ull;
    if (ctx) *ctx = 0ull;
    if (!l || !page) return 0u;
    for (uint32_t i = 0; i < l->n && i < N48_DL_MAX; i++) {
        const n48_dl_ent *e = &l->e[i];
        if (!e->page || !n48_t0_covers(e->page, e->size, page)) continue;
        if (!c) { if (tok) *tok = e->tok; if (va) *va = e->va; if (ctx) *ctx = e->ctx; }
        c++;
    }
    return c;
}
/* (d) the R5' hazard set, READ-ONLY: n48_hz_probe (the filter's answer) and n48_hz_count (the exact row's hits) take a const set,
 * so no counter moves; plus the exact row's first namer and VA. Returns the filter answer. */
static inline uint32_t n48_t0_hz_probe(const n48_hazard *hz, uint64_t page, uint64_t *hits, uint32_t *exact, uint64_t *namer,
                                       uint64_t *va)
{
    const uint32_t filt = n48_hz_probe(hz, page);
    uint32_t ex = 0u;
    const uint64_t h = n48_hz_count(hz, page, &ex);
    if (hits) *hits = h;
    if (exact) *exact = ex;
    if (namer) *namer = 0ull;
    if (va) *va = 0ull;
    if (hz && ex) {
        const uint64_t key = page & ~0xFFFull;
        for (uint32_t i = 0; i < hz->used && i < N48_HZ_ROWS; i++)
            if (hz->row[i].page == key) { if (namer) *namer = hz->row[i].namer; if (va) *va = hz->row[i].va; break; }
    }
    return filt;
}
/* (e) the residency-provenance table: entries whose VRAM range covers `vram` (vram <= P < vram + bytes; a 0-byte entry covers
 * nothing). Returns the count and the first match's VA. Read-only. */
static inline uint32_t n48_t0_rp_probe(const n48_rp *rp, uint64_t vram, uint64_t *va)
{
    uint32_t c = 0u;
    if (va) *va = 0ull;
    if (!rp) return 0u;
    for (uint32_t i = 0; i < rp->n && i < N48_RP_MAX; i++) {
        const n48_rp_ent *e = &rp->e[i];
        if (!e->bytes || vram < e->vram || vram - e->vram >= e->bytes) continue;
        if (!c && va) *va = e->va;
        c++;
    }
    return c;
}
/* The probe's own line budget: 1..N48_T0_LINES = this pair's number (counted), 0 = spent. */
static inline uint32_t n48_t0_take_line(uint32_t *lines)
{
    if (!lines || *lines >= N48_T0_LINES) return 0u;
    return ++*lines;
}

/* THE LINES (each bounded under N48_LOG_CAP_BODY by tests/gfx_t0src_test.cpp at worst-case values, a 64-character class name
 * included: %.40s prints at most 40 of it). */
/* the page, content, ledger, hazard and resprov answers for S's texture 0 in a B1 frame */
#define N48_T0_FMT "t0src: frame %llu VA %#llx p0 %s%#llx pn %s%#llx bar %u; zero %u/1024 %08x %08x %08x %08x; led %u tok %u va %#llx; " \
                   "hz %u hits %llu exact %u namer %llu va %#llx; rp %u va %#llx (line %u of 32)"
/* the newest map / unmap covering the VA and one other context that mapped the same memory object (seq 0 = none in the ring) */
#define N48_T0_MAP_FMT "t0map: frame %llu map seq %llu ctx #%u pid %d up %llu mem %p %.40s len %#llx off %#llx size %#llx fl %#x rc %u " \
                       "m0c %#x; unmap up %llu size %#llx; other ctx #%u pid %d va %#llx; torn %u (line %u of 32)"
/* the totals, on the drawelide66 report and switch 79's own line; C1: the probe's MM-read time */
#define N48_T0_REPORT_FMT "t0src: switch 79 %s%s; maps %llu (wraps %llu), unmaps %llu (wraps %llu), torn %llu; probes %u of %u, " \
                          "MM reads %llu (failed %llu) in %llu us (max %llu us per probe); second resource class %llu"

#endif
