// n48_t1.h: pure helpers for native #12 "bundle work" (no Vulkan, no Foundation; host test: test-t1.c).
//  (a) T1 timing aggregation: lock-free count/sum/min/max + a fixed 8-bucket histogram, drained once per 10 s window.
//  (b) T2 persisted pipeline cache file: our header (magic, version, key, RADV UUID/vendor/device, payload length, FNV-1a) around the raw
//      vkGetPipelineCacheData blob, and the check that decides whether a file may be handed to vkCreatePipelineCache as initialData.
#ifndef N48_T1_H
#define N48_T1_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

// ---- (a) histograms ------------------------------------------------------------------------------------------------
#define N48T_NB 8
// Bucket upper bounds in ns: 0.1, 0.4, 1.6, 6.4, 25.6, 102.4, 409.6 ms; the last bucket is everything above.
static inline int n48t_bucket(uint64_t ns) {
    uint64_t lim = 100000ULL;
    for (int i = 0; i < N48T_NB - 1; i++, lim *= 4) if (ns < lim) return i;
    return N48T_NB - 1;
}
typedef struct { _Atomic uint64_t n, sum, nmin /* stores ~min so that zero means "no sample" */, max, b[N48T_NB]; } n48t_hist;
typedef struct { uint64_t n, sum, min, max, b[N48T_NB]; } n48t_snap;
static inline void n48t_add(n48t_hist *h, uint64_t ns) {
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->b[n48t_bucket(ns)], 1, memory_order_relaxed);
    uint64_t cur = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (ns > cur && !atomic_compare_exchange_weak_explicit(&h->max, &cur, ns, memory_order_relaxed, memory_order_relaxed)) {}
    uint64_t inv = ~ns; cur = atomic_load_explicit(&h->nmin, memory_order_relaxed);
    while (inv > cur && !atomic_compare_exchange_weak_explicit(&h->nmin, &cur, inv, memory_order_relaxed, memory_order_relaxed)) {}
}
// Drains h into s (each field exchanged to zero; a sample landing between two exchanges is attributed to this or the next window, never lost).
static inline void n48t_take(n48t_hist *h, n48t_snap *s) {
    s->n = atomic_exchange(&h->n, 0); s->sum = atomic_exchange(&h->sum, 0); s->max = atomic_exchange(&h->max, 0);
    uint64_t nm = atomic_exchange(&h->nmin, 0); s->min = nm ? ~nm : 0;
    for (int i = 0; i < N48T_NB; i++) s->b[i] = atomic_exchange(&h->b[i], 0);
}
// One summary line (no newline); returns the length written (0 when the window had no samples).
static inline int n48t_fmt(const char *name, const n48t_snap *s, char *out, size_t cap) {
    if (!s->n) return 0;
    return snprintf(out, cap, "T1 %s n=%llu avg=%.3fms min=%.3fms max=%.3fms hist(<0.1,<0.4,<1.6,<6.4,<25.6,<102,<410,>=410ms)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        name, (unsigned long long)s->n, (double)s->sum / (double)s->n / 1e6, (double)s->min / 1e6, (double)s->max / 1e6,
        (unsigned long long)s->b[0], (unsigned long long)s->b[1], (unsigned long long)s->b[2], (unsigned long long)s->b[3],
        (unsigned long long)s->b[4], (unsigned long long)s->b[5], (unsigned long long)s->b[6], (unsigned long long)s->b[7]);
}

// ---- (b) pipeline-cache file ---------------------------------------------------------------------------------------
#define N48PC_VERSION 1u
#define N48PC_MAX_PAYLOAD (256ULL << 20)
typedef struct {
    char magic[8];            // "N48PCv1\0"
    uint32_t version;         // N48PC_VERSION
    uint32_t hdr_size;        // sizeof(n48pc_hdr)
    uint8_t key[32];          // sha256(bundle build | dylib identity)
    uint8_t uuid[16];         // VkPhysicalDeviceProperties::pipelineCacheUUID
    uint32_t vendor, device;  // VkPhysicalDeviceProperties vendorID / deviceID
    uint64_t payload_len;     // bytes of the vkGetPipelineCacheData blob that follow
    uint64_t payload_fnv;     // FNV-1a 64 of the blob
} n48pc_hdr;
static inline uint64_t n48pc_fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static inline void n48pc_fill(n48pc_hdr *h, const uint8_t key[32], const uint8_t uuid[16], uint32_t vendor, uint32_t device, const void *payload, size_t len) {
    memset(h, 0, sizeof *h); memcpy(h->magic, "N48PCv1", 8); h->version = N48PC_VERSION; h->hdr_size = (uint32_t)sizeof *h;
    memcpy(h->key, key, 32); memcpy(h->uuid, uuid, 16); h->vendor = vendor; h->device = device; h->payload_len = len; h->payload_fnv = n48pc_fnv(payload, len);
}
// 0 = usable; the payload pointer/length are returned. Otherwise a reason code (n48pc_why).
enum { N48PC_OK = 0, N48PC_SHORT, N48PC_MAGIC, N48PC_VERSION_BAD, N48PC_KEY, N48PC_UUID, N48PC_LEN, N48PC_HASH, N48PC_VKHDR };
static inline const char *n48pc_why(int c) {
    static const char *w[] = { "ok", "file shorter than header", "bad magic", "bad version/header size", "key mismatch (other bundle build or RADV library)",
        "RADV pipelineCacheUUID/vendor/device mismatch", "payload length mismatch", "payload hash mismatch (corrupt)", "Vulkan cache header invalid" };
    return c >= 0 && c <= N48PC_VKHDR ? w[c] : "?";
}
// The Vulkan-defined first 32 bytes of the blob (VkPipelineCacheHeaderVersionOne): headerSize, headerVersion(1), vendorID, deviceID, pipelineCacheUUID[16].
static inline int n48pc_vk_header_ok(const void *payload, size_t len, const uint8_t uuid[16], uint32_t vendor, uint32_t device) {
    uint32_t w[4]; if (len < 32) return 0; memcpy(w, payload, 16);
    return w[0] >= 32 && w[0] <= len && w[1] == 1 && w[2] == vendor && w[3] == device && !memcmp((const uint8_t *)payload + 16, uuid, 16);
}
static inline int n48pc_check(const void *file, size_t flen, const uint8_t key[32], const uint8_t uuid[16], uint32_t vendor, uint32_t device, const void **payload, size_t *plen) {
    n48pc_hdr h; if (flen < sizeof h) return N48PC_SHORT;
    memcpy(&h, file, sizeof h);
    if (memcmp(h.magic, "N48PCv1", 8)) return N48PC_MAGIC;
    if (h.version != N48PC_VERSION || h.hdr_size != sizeof h) return N48PC_VERSION_BAD;
    if (memcmp(h.key, key, 32)) return N48PC_KEY;
    if (memcmp(h.uuid, uuid, 16) || h.vendor != vendor || h.device != device) return N48PC_UUID;
    if (h.payload_len > N48PC_MAX_PAYLOAD || h.payload_len != flen - sizeof h) return N48PC_LEN;
    const uint8_t *p = (const uint8_t *)file + sizeof h;
    if (n48pc_fnv(p, (size_t)h.payload_len) != h.payload_fnv) return N48PC_HASH;
    if (!n48pc_vk_header_ok(p, (size_t)h.payload_len, uuid, vendor, device)) return N48PC_VKHDR;
    *payload = p; *plen = (size_t)h.payload_len; return N48PC_OK;
}
#endif
