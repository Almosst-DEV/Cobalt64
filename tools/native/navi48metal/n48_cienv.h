// n48_cienv.h (bundle 9, app crash study item 1): the Core Image switch that keeps CIKL string kernels off the nil -[MTLDevice compiler] path.
// Pure C so it is host-testable (test-cienv.c).  an internal design note, "App crash study", item (1).
//
// Why: -[CIKernel _initWithString:...] reflects the kernel-language source through [device newLibraryWithSource:] and asks the function for its arguments;
// for a stitchable function Metal asks [device compiler], which is nil on this device, so the argument list stays empty and CIKernelReflection's consolidate()
// writes into a NULL argument array (EXC_BAD_ACCESS in Preview, Tools > Adjust Color).  CI_USE_MTL_DAG_FOR_CIKL_SRC=0 makes Core Image take its own
// kernel-language parser instead.  The variable is set ONLY when it is unset, so an operator's explicit value (the ciprobe cikl control case sets 1) always wins.
#ifndef N48_CIENV_H
#define N48_CIENV_H
#include <stddef.h>
#define N48CE_VAR   "CI_USE_MTL_DAG_FOR_CIKL_SRC"
#define N48CE_VALUE "0"
enum { N48CE_ALREADY_SET = 0, N48CE_SET = 1, N48CE_SET_FAILED = -1 };
typedef const char *(*n48ce_getenv_fn)(const char *name);
typedef int (*n48ce_setenv_fn)(const char *name, const char *value, int overwrite);
// Returns N48CE_SET when it set the variable to "0", N48CE_ALREADY_SET when a value (any value, even the empty string) was present and was left alone.
static inline int n48ce_apply(n48ce_getenv_fn get, n48ce_setenv_fn set) {
    if (get(N48CE_VAR) != NULL) return N48CE_ALREADY_SET;
    return set(N48CE_VAR, N48CE_VALUE, 0) == 0 ? N48CE_SET : N48CE_SET_FAILED;
}
#endif
