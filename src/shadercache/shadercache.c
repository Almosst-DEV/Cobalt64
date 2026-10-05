/*
 * shadercache.c - see shadercache.h and README.md.
 *
 * Every function here is reachable from the kext: no heap, no floating point, no libc calls
 * (byte loops only, so -fno-builtin keeps the object free of memcpy/memset imports; the
 * `make kextcheck` gate proves it). The blob is untrusted until sc_open() has validated all of
 * it, and the lookup paths still bounds-check what they read.
 */
#include "shadercache.h"

/* ---- little-endian reads -------------------------------------------------------------- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* header field offsets */
enum {
    H_MAGIC = 0x00, H_VMAJ = 0x08, H_VMIN = 0x0a, H_HSIZE = 0x0c, H_TOTAL = 0x10, H_FLAGS = 0x14,
    H_HASH = 0x18, H_TERM = 0x1c, H_GRID = 0x20, H_MAXKEY = 0x24, H_MAXAPPLE = 0x28, H_NENT = 0x2c,
    H_IDXOFF = 0x30, H_IDXCNT = 0x34, H_ENTOFF = 0x38, H_ENTSIZE = 0x3c, H_MASKOFF = 0x40,
    H_MASKCNT = 0x44, H_WORDOFF = 0x48, H_WORDSIZE = 0x4c, H_STROFF = 0x50, H_STRSIZE = 0x54,
    H_CHECKSUM = 0x58
};
/* entry field offsets */
enum {
    E_SIZE = 0x00, E_STAGE = 0x04, E_FLAGS = 0x06, E_KEY = 0x08, E_MASK = 0x10, E_KEYDW = 0x12,
    E_APPLEDW = 0x14, E_APPLEOFF = 0x18, E_CAP = 0x1c, E_SUBDW = 0x20, E_SUBOFF = 0x24,
    E_ADJCNT = 0x28, E_ADJOFF = 0x2c, E_NAMEOFF = 0x30, E_NAMELEN = 0x34, E_SUBHASH = 0x38,
    E_APPLESG = 0x40, E_APPLEVG = 0x42, E_SUBSG = 0x44, E_SUBVG = 0x46, E_ORIGIN = 0x48,
    E_RESERVED = 0x4c
};

static const uint8_t kMagic[8] = { 'N', '4', '8', 'S', 'C', 'A', 'C', 'H' };

uint64_t sc_fnv1a64(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= SC_FNV64_PRIME;
    }
    return h;
}

static uint64_t fnv_zero4(uint64_t h)
{
    unsigned i;
    for (i = 0; i < 4; i++) h *= SC_FNV64_PRIME;   /* h ^= 0 is a no-op */
    return h;
}

static uint64_t key_start(uint32_t key_dwords)
{
    uint8_t len[4];
    len[0] = (uint8_t)key_dwords; len[1] = (uint8_t)(key_dwords >> 8);
    len[2] = (uint8_t)(key_dwords >> 16); len[3] = (uint8_t)(key_dwords >> 24);
    return sc_fnv1a64(SC_FNV64_BASIS, len, 4);
}

uint64_t sc_key(const uint8_t *code, uint32_t key_dwords, const uint32_t *mask_idx, uint32_t mask_n)
{
    uint64_t h = key_start(key_dwords);
    uint32_t i, j = 0;
    if (!code) return h;
    for (i = 0; i < key_dwords; i++) {
        while (j < mask_n && mask_idx[j] < i) j++;
        if (j < mask_n && mask_idx[j] == i) h = fnv_zero4(h);
        else h = sc_fnv1a64(h, code + (size_t)i * 4u, 4);
    }
    return h;
}

/* the same key with the mask indices read from the blob's words pool */
static uint64_t key_blobmask(const uint8_t *code, uint32_t key_dwords, const uint8_t *idx, uint32_t n)
{
    uint64_t h = key_start(key_dwords);
    uint32_t i, j = 0;
    for (i = 0; i < key_dwords; i++) {
        while (j < n && rd32(idx + (size_t)j * 4u) < i) j++;
        if (j < n && rd32(idx + (size_t)j * 4u) == i) h = fnv_zero4(h);
        else h = sc_fnv1a64(h, code + (size_t)i * 4u, 4);
    }
    return h;
}

int sc_extent(const uint8_t *buf, size_t avail, uint32_t max_dwords, uint32_t *key_dwords)
{
    uint32_t i;
    if (!buf || !key_dwords) return SC_E_ARG;
    *key_dwords = 0;
    if (max_dwords > SC_MAX_KEY_DWORDS) max_dwords = SC_MAX_KEY_DWORDS;
    for (i = 0; i < max_dwords && (size_t)i < avail / 4u; i++) {
        if (rd32(buf + (size_t)i * 4u) == SC_TERMINATOR) {
            *key_dwords = i + 1;
            return SC_OK;
        }
    }
    return SC_MISS;
}

/* ---- validation helpers --------------------------------------------------------------- */
static void zero_cache(sc_cache *c)
{
    c->base = 0; c->size = 0; c->grid = 0; c->max_key_dwords = 0; c->max_apple_dwords = 0;
    c->entry_count = 0; c->index_off = 0; c->entries_off = 0; c->entries_size = 0;
    c->masks_off = 0; c->masks_count = 0; c->words_off = 0; c->words_size = 0;
    c->strings_off = 0; c->strings_size = 0;
}

static void zero_match(sc_match *m)
{
    m->key = 0; m->entry_off = 0; m->flags = 0; m->stage = 0; m->mask_set = 0; m->key_dwords = 0;
    m->apple_dwords = 0; m->capacity_bytes = 0; m->subst_dwords = 0; m->adj_count = 0;
    m->verified = 0; m->miss_reason = SC_MISS_NONE;
}

/* [off, off+size) inside [lo, hi), 4-aligned, without overflow */
static int in_range(uint64_t off, uint64_t size, uint64_t lo, uint64_t hi)
{
    if (off < lo || off > hi) return 0;
    if (size > hi - off) return 0;
    return 1;
}

static int region_ok(uint32_t off, uint64_t size, uint32_t total)
{
    if ((off & 3u) || (size & 3u)) return 0;
    return in_range(off, size, SC_HEADER_SIZE, total);
}

/* mask set `id` (1-based) -> pointer to its u32 indices and count; id 0 = no mask */
static int maskset(const sc_cache *c, uint32_t id, const uint8_t **idx, uint32_t *n)
{
    const uint8_t *rec;
    uint32_t cnt, first;
    *idx = 0; *n = 0;
    if (id == 0) return 1;
    if (id > c->masks_count) return 0;
    rec = c->base + c->masks_off + (size_t)(id - 1) * SC_MASKSET_REC_SIZE;
    cnt = rd32(rec); first = rd32(rec + 4);
    if (cnt == 0 || cnt > SC_MAX_MASK_INDICES) return 0;
    if (!in_range((uint64_t)first * 4u, (uint64_t)cnt * 4u, 0, c->words_size)) return 0;
    *idx = c->base + c->words_off + (size_t)first * 4u;
    *n = cnt;
    return 1;
}

static int masked(const uint8_t *idx, uint32_t n, uint32_t *j, uint32_t i)
{
    while (*j < n && rd32(idx + (size_t)*j * 4u) < i) (*j)++;
    return *j < n && rd32(idx + (size_t)*j * 4u) == i;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* validate one entry record; fill *m on success */
static int check_entry(const sc_cache *c, uint32_t eoff, sc_match *m)
{
    const uint8_t *e;
    uint64_t end = (uint64_t)c->entries_off + c->entries_size;
    uint32_t esize, stage, flags, ms, kd, adw, aoff, cap, sdw, soff, acnt, aoffs, noff, nlen;
    const uint8_t *idx;
    uint32_t n, i, j, limit;

    if ((eoff & 3u) || !in_range(eoff, SC_ENTRY_FIXED_SIZE, c->entries_off, end)) return SC_E_BOUNDS;
    e = c->base + eoff;
    esize = rd32(e + E_SIZE);
    if (esize < SC_ENTRY_FIXED_SIZE || (esize & 3u) || !in_range(eoff, esize, c->entries_off, end))
        return SC_E_ENTRY;
    stage = rd16(e + E_STAGE);
    flags = rd16(e + E_FLAGS);
    ms = rd16(e + E_MASK);
    kd = rd16(e + E_KEYDW);
    adw = rd32(e + E_APPLEDW);
    aoff = rd32(e + E_APPLEOFF);
    cap = rd32(e + E_CAP);
    sdw = rd32(e + E_SUBDW);
    soff = rd32(e + E_SUBOFF);
    acnt = rd32(e + E_ADJCNT);
    aoffs = rd32(e + E_ADJOFF);
    noff = rd32(e + E_NAMEOFF);
    nlen = rd32(e + E_NAMELEN);
    if (stage > SC_STAGE_FRAGMENT || (flags & ~SC_F_ALL) || rd32(e + E_RESERVED) != 0) return SC_E_ENTRY;
    if (ms > c->masks_count) return SC_E_MASK;
    if (kd == 0 || kd > c->max_key_dwords) return SC_E_ENTRY;
    if ((cap & 3u) || cap > SC_MAX_APPLE_DWORDS * 4u) return SC_E_ENTRY;
    if (flags & SC_F_HASH_ONLY) {
        if (adw != 0 || aoff != 0 || (flags & (SC_F_SUBSTITUTE | SC_F_RELOCATE))) return SC_E_ENTRY;
    } else {
        if (adw < kd || adw > c->max_apple_dwords) return SC_E_ENTRY;
        if ((aoff & 3u) || !in_range(aoff, (uint64_t)adw * 4u, SC_ENTRY_FIXED_SIZE, esize)) return SC_E_BOUNDS;
        if (cap < adw * 4u) return SC_E_ENTRY;
    }
    if (flags & (SC_F_SUBSTITUTE | SC_F_RELOCATE)) {
        /* 0.0.311 (notes §565): both flags mean "an image is stored here", and they are ALTERNATIVES - an entry that
         * claimed both would be saying the same bytes may and may not be written over Apple's program. The capacity
         * clause belongs to SUBSTITUTE alone: a relocated image goes into our arena, not into Apple's allocation, and
         * bounding it by that allocation is exactly the rule that kept RectPosTexFast_VS out of the blob (§564). */
        if ((flags & SC_F_SUBSTITUTE) && (flags & SC_F_RELOCATE)) return SC_E_ENTRY;
        if (flags & (SC_F_CONFLICT | SC_F_HASH_ONLY)) return SC_E_ENTRY;
        if (sdw == 0) return SC_E_ENTRY;
        if ((flags & SC_F_SUBSTITUTE) && (uint64_t)sdw * 4u > cap) return SC_E_ENTRY;
        if ((soff & 3u) || !in_range(soff, (uint64_t)sdw * 4u, SC_ENTRY_FIXED_SIZE, esize)) return SC_E_BOUNDS;
        if (sc_fnv1a64(SC_FNV64_BASIS, e + soff, (size_t)sdw * 4u) != rd64(e + E_SUBHASH)) return SC_E_ENTRY;
    } else if (sdw != 0 || soff != 0) {
        return SC_E_ENTRY;
    }
    if (acnt > SC_MAX_ADJUST || ((acnt != 0) != ((flags & SC_F_HAS_ADJUST) != 0))) return SC_E_ENTRY;
    if (acnt && ((aoffs & 3u) || !in_range(aoffs, (uint64_t)acnt * SC_ADJ_REC_SIZE, SC_ENTRY_FIXED_SIZE, esize)))
        return SC_E_BOUNDS;
    if (nlen > SC_MAX_NAME || !in_range(noff, (uint64_t)nlen + 1u, 0, c->strings_size) ||
        c->base[c->strings_off + noff + nlen] != 0)
        return SC_E_STRING;
    for (i = 0; i < nlen; i++) if (c->base[c->strings_off + noff + i] == 0) return SC_E_STRING;
    if (!maskset(c, ms, &idx, &n)) return SC_E_MASK;
    limit = adw > kd ? adw : kd;
    for (i = 0; i < n; i++) {
        uint32_t v = rd32(idx + (size_t)i * 4u);
        if (v >= limit || (i && v <= rd32(idx + (size_t)(i - 1) * 4u))) return SC_E_MASK;
    }
    if (!(flags & SC_F_HASH_ONLY)) {
        const uint8_t *a = e + aoff;
        /* the key prefix must be exactly "through the first terminator" of the stored bytes */
        for (i = 0; i + 1 < kd; i++) if (rd32(a + (size_t)i * 4u) == SC_TERMINATOR) return SC_E_ENTRY;
        if (rd32(a + (size_t)(kd - 1) * 4u) != SC_TERMINATOR) return SC_E_ENTRY;
        if (key_blobmask(a, kd, idx, n) != rd64(e + E_KEY)) return SC_E_KEY;
        /* masked dwords are stored as zero, so the stored bytes are the canonical form */
        j = 0;
        for (i = 0; i < adw; i++)
            if (masked(idx, n, &j, i) && rd32(a + (size_t)i * 4u) != 0) return SC_E_MASK;
    }
    m->key = rd64(e + E_KEY);
    m->entry_off = eoff;
    m->flags = (uint16_t)flags;
    m->stage = (uint16_t)stage;
    m->mask_set = (uint16_t)ms;
    m->key_dwords = (uint16_t)kd;
    m->apple_dwords = adw;
    m->capacity_bytes = cap;
    m->subst_dwords = sdw;
    m->adj_count = acnt;
    m->verified = 0;
    m->miss_reason = SC_MISS_NONE;
    return SC_OK;
}

int sc_open(sc_cache *c, const uint8_t *blob, size_t len)
{
    const uint8_t *h;
    uint64_t sum, prev_key = 0;
    uint32_t i, prev_ms = 0, prev_kd = 0, prev_eoff = 0, grid, idxcnt;
    static const uint8_t zero8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    sc_cache t;
    int st;

    if (!c) return SC_E_ARG;
    zero_cache(c);
    if (!blob) return SC_E_ARG;
    if (len < SC_HEADER_SIZE) return SC_E_SHORT;
    if (len > 0x7fffffffu) return SC_E_BOUNDS;
    h = blob;
    if (!bytes_equal(h, kMagic, 8)) return SC_E_MAGIC;
    if (rd16(h + H_VMAJ) != SC_VERSION_MAJOR) return SC_E_VERSION;
    if (rd32(h + H_HSIZE) != SC_HEADER_SIZE) return SC_E_HEADER;
    if (rd32(h + H_TOTAL) != (uint32_t)len) return SC_E_SHORT;
    if (rd32(h + H_FLAGS) != 0 || rd32(h + H_HASH) != SC_HASH_FNV1A64 || rd32(h + H_TERM) != SC_TERMINATOR)
        return SC_E_HEADER;
    sum = sc_fnv1a64(SC_FNV64_BASIS, blob, H_CHECKSUM);
    sum = sc_fnv1a64(sum, zero8, 8);
    sum = sc_fnv1a64(sum, blob + SC_HEADER_SIZE, len - SC_HEADER_SIZE);
    if (sum != rd64(h + H_CHECKSUM)) return SC_E_CHECKSUM;

    zero_cache(&t);
    t.base = blob;
    t.size = (uint32_t)len;
    grid = rd32(h + H_GRID);
    if (grid < 4u || grid > 0x10000u || (grid & (grid - 1u))) return SC_E_HEADER;
    t.grid = grid;
    t.max_key_dwords = rd32(h + H_MAXKEY);
    t.max_apple_dwords = rd32(h + H_MAXAPPLE);
    if (t.max_key_dwords == 0 || t.max_key_dwords > SC_MAX_KEY_DWORDS ||
        t.max_apple_dwords > SC_MAX_APPLE_DWORDS)
        return SC_E_HEADER;
    t.entry_count = rd32(h + H_NENT);
    idxcnt = rd32(h + H_IDXCNT);
    if (t.entry_count > SC_MAX_ENTRIES || idxcnt != t.entry_count) return SC_E_HEADER;
    t.index_off = rd32(h + H_IDXOFF);
    t.entries_off = rd32(h + H_ENTOFF);
    t.entries_size = rd32(h + H_ENTSIZE);
    t.masks_off = rd32(h + H_MASKOFF);
    t.masks_count = rd32(h + H_MASKCNT);
    t.words_off = rd32(h + H_WORDOFF);
    t.words_size = rd32(h + H_WORDSIZE);
    t.strings_off = rd32(h + H_STROFF);
    t.strings_size = rd32(h + H_STRSIZE);
    if (t.masks_count > SC_MAX_MASKSETS) return SC_E_HEADER;
    if (!region_ok(t.index_off, (uint64_t)t.entry_count * SC_INDEX_REC_SIZE, t.size) ||
        !region_ok(t.entries_off, t.entries_size, t.size) ||
        !region_ok(t.masks_off, (uint64_t)t.masks_count * SC_MASKSET_REC_SIZE, t.size) ||
        !region_ok(t.words_off, t.words_size, t.size) ||
        !in_range(t.strings_off, t.strings_size, SC_HEADER_SIZE, t.size))
        return SC_E_BOUNDS;

    for (i = 1; i <= t.masks_count; i++) {
        const uint8_t *idx;
        uint32_t n, k;
        if (!maskset(&t, i, &idx, &n)) return SC_E_MASK;
        for (k = 0; k < n; k++) {
            uint32_t v = rd32(idx + (size_t)k * 4u);
            if (v >= SC_MAX_APPLE_DWORDS || (k && v <= rd32(idx + (size_t)(k - 1) * 4u))) return SC_E_MASK;
        }
    }

    for (i = 0; i < t.entry_count; i++) {
        const uint8_t *r = blob + t.index_off + (size_t)i * SC_INDEX_REC_SIZE;
        uint64_t key = rd64(r);
        uint32_t eoff = rd32(r + 8), ms = rd16(r + 12), kd = rd16(r + 14);
        sc_match m;
        if (i) {
            if (key < prev_key) return SC_E_ORDER;
            if (key == prev_key) {
                if (ms < prev_ms) return SC_E_ORDER;
                if (ms == prev_ms) {
                    if (kd < prev_kd) return SC_E_ORDER;
                    if (kd == prev_kd && eoff <= prev_eoff) return SC_E_ORDER;
                }
            }
        }
        st = check_entry(&t, eoff, &m);
        if (st != SC_OK) return st;
        if (m.key != key || m.mask_set != ms || m.key_dwords != kd) return SC_E_ENTRY;
        prev_key = key; prev_ms = ms; prev_kd = kd; prev_eoff = eoff;
    }
    *c = t;
    return SC_OK;
}

uint32_t sc_entry_count(const sc_cache *c) { return (c && c->base) ? c->entry_count : 0; }

int sc_entry_at(const sc_cache *c, uint32_t index_pos, sc_match *m)
{
    const uint8_t *r;
    if (!c || !c->base || !m) return SC_E_ARG;
    zero_match(m);
    if (index_pos >= c->entry_count) return SC_E_ARG;
    r = c->base + c->index_off + (size_t)index_pos * SC_INDEX_REC_SIZE;
    return check_entry(c, rd32(r + 8), m);
}

static uint32_t lower_bound(const sc_cache *c, uint64_t key)
{
    uint32_t lo = 0, hi = c->entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (rd64(c->base + c->index_off + (size_t)mid * SC_INDEX_REC_SIZE) < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

int sc_lookup(const sc_cache *c, const uint8_t *buf, size_t avail, sc_match *m)
{
    uint32_t kd, ms, pos, verified = 0, saw_short = 0, saw_compare = 0, have_hashonly = 0;
    sc_match hashonly;

    if (!c || !c->base || !buf || !m) return SC_E_ARG;
    zero_match(m);
    zero_match(&hashonly);
    if (avail < 4u || rd32(buf) == 0) { m->miss_reason = SC_MISS_EMPTY; return SC_MISS; }
    if (sc_extent(buf, avail, c->max_key_dwords, &kd) != SC_OK) { m->miss_reason = SC_MISS_NOEND; return SC_MISS; }

    for (ms = 0; ms <= c->masks_count; ms++) {
        const uint8_t *idx;
        uint32_t n;
        uint64_t key;
        if (!maskset(c, ms, &idx, &n)) return SC_E_MASK;
        key = key_blobmask(buf, kd, idx, n);
        for (pos = lower_bound(c, key); pos < c->entry_count; pos++) {
            const uint8_t *r = c->base + c->index_off + (size_t)pos * SC_INDEX_REC_SIZE;
            sc_match e;
            const uint8_t *a;
            uint32_t i, j = 0, equal = 1;
            if (rd64(r) != key) break;
            if (rd16(r + 12) != ms || rd16(r + 14) != kd) continue;
            if (check_entry(c, rd32(r + 8), &e) != SC_OK) return SC_E_ENTRY;
            if (e.flags & SC_F_HASH_ONLY) {
                if (!have_hashonly) { hashonly = e; have_hashonly = 1; }
                continue;
            }
            if (avail / 4u < e.apple_dwords) { saw_short = 1; continue; }
            a = c->base + e.entry_off + rd32(c->base + e.entry_off + E_APPLEOFF);
            for (i = 0; i < e.apple_dwords; i++) {
                if (masked(idx, n, &j, i)) continue;
                if (!bytes_equal(a + (size_t)i * 4u, buf + (size_t)i * 4u, 4)) { equal = 0; break; }
            }
            if (!equal) { saw_compare = 1; continue; }
            verified++;
            if (verified == 1) { *m = e; m->verified = 1; }
        }
    }
    if (verified > 1) { zero_match(m); return SC_E_AMBIGUOUS; }
    if (verified == 1) return SC_OK;
    if (have_hashonly) { *m = hashonly; m->verified = 0; return SC_OK; }
    m->miss_reason = saw_short ? SC_MISS_SHORT : saw_compare ? SC_MISS_COMPARE : SC_MISS_NOKEY;
    return SC_MISS;
}

uint64_t sc_scan_total(const sc_scan_stats *st)
{
    if (!st) return 0;
    return (uint64_t)st->matches + st->ambiguous + st->errors + st->miss_empty +
           st->miss_noend + st->miss_nokey + st->miss_compare + st->miss_short;
}

int sc_scan_window(const sc_cache *c, const uint8_t *buf, size_t len,
                   size_t base, size_t owned, uint32_t grid, size_t *next,
                   sc_scan_fn fn, void *ctx, sc_scan_stats *st)
{
    sc_scan_stats local;
    size_t off, lookahead;
    if (!st) st = &local;
    st->candidates = st->matches = st->ambiguous = st->errors = 0;
    st->miss_empty = st->miss_noend = st->miss_nokey = st->miss_compare = st->miss_short = 0;
    if (!c || !c->base || !buf || !next) return SC_E_ARG;
    if (grid == 0) grid = c->grid;
    if (grid < 4u || (grid & (grid - 1u)) || (base & (grid - 1u)) ||
        owned > len || len > (size_t)-1 - grid || base > (size_t)-1 - len - grid || *next < base ||
        (*next & (grid - 1u)) || (owned < len && (owned & (grid - 1u)))) return SC_E_ARG;
    lookahead = (size_t)(c->max_apple_dwords > c->max_key_dwords ?
                        c->max_apple_dwords : c->max_key_dwords) * 4u;
    if (owned < len && len - owned < lookahead) return SC_E_ARG;
    off = *next - base;
    while (len >= 4u && off <= len - 4u && off < owned) {
        sc_match m;
        size_t advance = grid;
        int r = sc_lookup(c, buf + off, len - off, &m);
        st->candidates++;
        if (r == SC_OK) {
            size_t span = (size_t)(m.apple_dwords > m.key_dwords ? m.apple_dwords : m.key_dwords) * 4u;
            st->matches++;
            advance = (span + grid - 1u) & ~(size_t)(grid - 1u);
            if (!advance) advance = grid;
        } else if (r == SC_E_AMBIGUOUS) st->ambiguous++;
        else if (r == SC_MISS) {
            switch (m.miss_reason) {
            case SC_MISS_EMPTY: st->miss_empty++; break;
            case SC_MISS_NOEND: st->miss_noend++; break;
            case SC_MISS_NOKEY: st->miss_nokey++; break;
            case SC_MISS_COMPARE: st->miss_compare++; break;
            case SC_MISS_SHORT: st->miss_short++; break;
            default: st->errors++; return SC_E_ENTRY;
            }
        } else {
            st->errors++;
            return r;
        }
        *next = base + off + advance;
        if (r == SC_OK && fn && fn(ctx, off, &m)) return SC_OK;
        off += advance;
    }
    return SC_OK;
}

int sc_scan(const sc_cache *c, const uint8_t *buf, size_t len, uint32_t grid,
            sc_scan_fn fn, void *ctx, sc_scan_stats *st)
{
    size_t next = 0;
    return sc_scan_window(c, buf, len, 0, len, grid, &next, fn, ctx, st);
}

/* re-derive the entry a caller hands back, so a forged or stale sc_match cannot index outside */
static int reload(const sc_cache *c, const sc_match *m, sc_match *e)
{
    int st;
    if (!c || !c->base || !m) return SC_E_ARG;
    st = check_entry(c, m->entry_off, e);
    if (st != SC_OK) return st;
    if (e->key != m->key) return SC_E_ARG;
    return SC_OK;
}

/* The image a substitution writes, without the verified/adjustment gates: for RECOGNISING already
 * substituted code, never for deciding to write. See the header. */
int sc_subst_image(const sc_cache *c, const sc_match *m, uint8_t *out, size_t out_cap, uint32_t *nbytes)
{
    sc_match e;
    const uint8_t *s;
    uint32_t write, sbytes, i;
    int st;
    if (nbytes) *nbytes = 0;
    if (!out || !nbytes) return SC_E_ARG;
    st = reload(c, m, &e);
    if (st != SC_OK) return st;
    sbytes = e.subst_dwords * 4u;
    if (e.flags & SC_F_RELOCATE) {
        /* 0.0.311 (notes §565): a RELOCATED image is placed in our own arena, so neither of the two rules that exist for
         * writing over Apple's program applies to it. It is NOT padded to Apple's extent (there is no tail of Apple's
         * program left behind it to overwrite) and it is NOT capped by Apple's allocation (that allocation is not where
         * it goes). Only the caller's buffer bounds it. */
        if (!sbytes) return SC_E_NOSUB;
        write = sbytes;
        if (out_cap < write) return SC_E_CAPACITY;
    } else {
        if (!(e.flags & SC_F_SUBSTITUTE)) return SC_E_NOSUB;
        write = (e.subst_dwords > e.apple_dwords ? e.subst_dwords : e.apple_dwords) * 4u;
        if (write > e.capacity_bytes || out_cap < write) return SC_E_CAPACITY;
    }
    s = c->base + e.entry_off + rd32(c->base + e.entry_off + E_SUBOFF);
    for (i = 0; i < sbytes; i++) out[i] = s[i];
    for (; i < write; i++) out[i] = 0;
    *nbytes = write;
    return SC_OK;
}

int sc_subst_render(const sc_cache *c, const sc_match *m, int accept_adjust,
                    uint8_t *out, size_t out_cap, uint32_t *nbytes)
{
    sc_match e;
    const uint8_t *s;
    uint32_t write, sbytes, i;
    int st;
    if (nbytes) *nbytes = 0;
    if (!out || !nbytes) return SC_E_ARG;
    st = reload(c, m, &e);
    if (st != SC_OK) return st;
    if (!m->verified || !(e.flags & SC_F_SUBSTITUTE)) return SC_E_NOSUB;
    if ((e.flags & SC_F_HAS_ADJUST) && !accept_adjust) return SC_E_ADJUST;
    sbytes = e.subst_dwords * 4u;
    write = (e.subst_dwords > e.apple_dwords ? e.subst_dwords : e.apple_dwords) * 4u;
    if (write > e.capacity_bytes || out_cap < write) return SC_E_CAPACITY;
    s = c->base + e.entry_off + rd32(c->base + e.entry_off + E_SUBOFF);
    for (i = 0; i < sbytes; i++) out[i] = s[i];
    for (; i < write; i++) out[i] = 0;     /* no Apple gfx10 word survives past our code */
    *nbytes = write;
    return SC_OK;
}

int sc_alt_find(const sc_cache *c, const sc_match *m, sc_match *alt)
{
    sc_match base, e;
    const uint8_t *ba;
    uint32_t pos, found = 0;
    int st;
    if (!alt) return SC_E_ARG;
    zero_match(alt);
    if (!c || !c->base || !m) return SC_E_ARG;
    st = reload(c, m, &base);
    if (st != SC_OK) return st;
    if (!m->verified || !(base.flags & SC_F_SUBSTITUTE) || !base.apple_dwords) return SC_E_NOSUB;
    ba = c->base + base.entry_off + rd32(c->base + base.entry_off + E_APPLEOFF);
    for (pos = lower_bound(c, base.key); pos < c->entry_count; pos++) {
        const uint8_t *r = c->base + c->index_off + (size_t)pos * SC_INDEX_REC_SIZE;
        const uint8_t *ea;
        if (rd64(r) != base.key) break;
        if (rd32(r + 8) == base.entry_off) continue;
        if (rd16(r + 12) != base.mask_set || rd16(r + 14) != base.key_dwords) continue;
        if (check_entry(c, rd32(r + 8), &e) != SC_OK) return SC_E_ENTRY;
        if (e.flags != base.flags || e.stage != base.stage || e.capacity_bytes != base.capacity_bytes ||
            e.apple_dwords <= base.apple_dwords) continue;
        ea = c->base + e.entry_off + rd32(c->base + e.entry_off + E_APPLEOFF);
        if (!bytes_equal(ea, ba, (size_t)base.apple_dwords * 4u)) continue;
        if (++found == 1u) *alt = e;
    }
    if (found > 1u) { zero_match(alt); return SC_E_AMBIGUOUS; }
    if (!found) return SC_MISS;
    alt->verified = 1;
    return SC_OK;
}

int sc_adjust_get(const sc_cache *c, const sc_match *m, uint32_t i, sc_adjust *a)
{
    sc_match e;
    const uint8_t *r;
    int st;
    if (!a) return SC_E_ARG;
    st = reload(c, m, &e);
    if (st != SC_OK) return st;
    if (i >= e.adj_count) return SC_E_ARG;
    r = c->base + e.entry_off + rd32(c->base + e.entry_off + E_ADJOFF) + (size_t)i * SC_ADJ_REC_SIZE;
    a->reg = rd32(r);
    a->guard_mask = rd32(r + 4);
    a->guard_value = rd32(r + 8);
    a->set_mask = rd32(r + 12);
    a->set_value = rd32(r + 16);
    return SC_OK;
}

const char *sc_entry_name(const sc_cache *c, const sc_match *m, uint32_t *len)
{
    sc_match e;
    const uint8_t *p;
    if (len) *len = 0;
    if (reload(c, m, &e) != SC_OK) return 0;
    p = c->base + e.entry_off;
    if (len) *len = rd32(p + E_NAMELEN);
    return (const char *)(c->base + c->strings_off + rd32(p + E_NAMEOFF));
}

const char *sc_status_name(int status)
{
    switch (status) {
    case SC_OK: return "ok";
    case SC_MISS: return "miss";
    case SC_E_ARG: return "bad argument";
    case SC_E_SHORT: return "blob truncated";
    case SC_E_MAGIC: return "bad magic";
    case SC_E_VERSION: return "unsupported version";
    case SC_E_HEADER: return "header field out of range";
    case SC_E_BOUNDS: return "region out of bounds";
    case SC_E_ORDER: return "index not sorted";
    case SC_E_ENTRY: return "inconsistent entry";
    case SC_E_KEY: return "stored key does not match stored bytes";
    case SC_E_CHECKSUM: return "checksum mismatch";
    case SC_E_MASK: return "bad mask set";
    case SC_E_STRING: return "bad name string";
    case SC_E_AMBIGUOUS: return "ambiguous match";
    case SC_E_NOSUB: return "entry is not substitutable";
    case SC_E_CAPACITY: return "capacity exceeded";
    case SC_E_ADJUST: return "entry needs register adjustments";
    default: return "unknown status";
    }
}
