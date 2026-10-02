/* linuxshim: amdgpu/amdgpu.h is not in the sparse clone. command_table2.c only casts ctx->driver_context to
 * struct amdgpu_device to reach mode_info.atom_context inside macros; the harness never executes an ATOM table. */
#ifndef DCN41_SHIM_AMDGPU_H
#define DCN41_SHIM_AMDGPU_H
#include <linux/types.h>
struct atom_context;
struct amdgpu_device { struct { struct atom_context *atom_context; } mode_info; };
#endif
