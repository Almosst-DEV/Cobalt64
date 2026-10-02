#ifndef DCN41_SHIM_LINUX_SLAB_H
#define DCN41_SHIM_LINUX_SLAB_H
#include <linux/types.h>
#define kzalloc(n, f) calloc(1, (n))
#define kmalloc(n, f) malloc(n)
#define kcalloc(n, s, f) calloc((n), (s))
#define kmalloc_array(n, s, f) calloc((n), (s))
#define kfree(p) free(p)
#define kvfree(p) free(p)
#define vzalloc(n) calloc(1, (n))
#define vfree(p) free(p)
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define kmemdup(p, n, f) memcpy(malloc(n), (p), (n))
#endif
