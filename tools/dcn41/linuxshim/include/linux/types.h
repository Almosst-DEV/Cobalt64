/* linuxshim: the few Linux kernel type names Linux DC uses, for a host (macOS) build of selected DC files.
 * Not Linux code; written for tools/dcn41's register-trace harness only. */
#ifndef DCN41_SHIM_LINUX_TYPES_H
#define DCN41_SHIM_LINUX_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;   typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8;    typedef int16_t s16;  typedef int32_t s32;  typedef int64_t s64;
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef int8_t __s8;  typedef int16_t __s16; typedef int32_t __s32; typedef int64_t __s64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;
typedef uint16_t __be16; typedef uint32_t __be32; typedef uint64_t __be64;
typedef int64_t ktime_t;
typedef unsigned int gfp_t;
typedef struct { int counter; } atomic_t;
typedef struct { int64_t counter; } atomic64_t;
typedef int spinlock_t;
#ifndef __KERNEL__
#define __KERNEL__ 1
#endif
#define __packed __attribute__((packed))
#define __always_inline inline __attribute__((always_inline))
#define __maybe_unused __attribute__((unused))
#define __must_check
#define noinline __attribute__((noinline))
#define fallthrough __attribute__((fallthrough))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define EXPORT_IF_KUNIT(x)
#define STATIC_IFN_KUNIT static
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define swap(a, b) do { __typeof__(a) __t = (a); (a) = (b); (b) = __t; } while (0)
#define div_u64(a, b) ((uint64_t)(a) / (uint32_t)(b))
#define div64_u64(a, b) ((uint64_t)(a) / (uint64_t)(b))
#define div_s64(a, b) ((int64_t)(a) / (int32_t)(b))
#define div64_s64(a, b) ((int64_t)(a) / (int64_t)(b))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))
#define WARN_ON(c) ({ int __c = !!(c); if (__c) dcn41_shim_warn(__FILE__, __LINE__); __c; })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, ...) WARN_ON(c)
#define BUG_ON(c) do { if (c) abort(); } while (0)
void dcn41_shim_warn(const char *file, int line);
#define READ_ONCE(x) (x)
#ifndef static_assert
#define static_assert _Static_assert
#endif
#define pr_debug(...) do {} while (0)
#define pr_info(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define le16_add_cpu(p, v) (*(p) = (uint16_t)(*(p) + (v)))
#define le32_add_cpu(p, v) (*(p) = (uint32_t)(*(p) + (v)))
#define BUILD_BUG_ON(c) ((void)sizeof(char[1 - 2 * !!(c)]))
static inline int ilog2(uint64_t v) { return 63 - __builtin_clzll(v); }
#define WRITE_ONCE(x, v) ((x) = (v))
struct kref { int refcount; };
static inline uint64_t div_u64_rem(uint64_t a, uint32_t b, uint32_t *r) { *r = (uint32_t)(a % b); return a / b; }
static inline uint64_t div64_u64_rem(uint64_t a, uint64_t b, uint64_t *r) { *r = a % b; return a / b; }
static inline int64_t div_s64_rem(int64_t a, int32_t b, int32_t *r) { *r = (int32_t)(a % b); return a / b; }
static inline int64_t ktime_get_raw_ns(void) { return 0; }
static inline int64_t ktime_get_ns(void) { return 0; }

#endif
