/* linuxshim: amdgpu/atom.h is not in the sparse clone; prototypes only (the harness defines them).
 * The DCE112 command-table helper takes ATOM_* names through atom.h: atomfirmware.h (re/linux-dc) FIRST, so its enums
 * are declared before atombios.h (re/m2/linux 5878583, searched last) #defines some of the same names. */
#ifndef DCN41_SHIM_ATOM_H
#define DCN41_SHIM_ATOM_H
#include <linux/types.h>
#include "atomfirmware.h"
#include "atom-types.h"
#include "atombios.h"
#include "ObjectID.h"        /* PLACEHOLDER values: linuxshim/include/ObjectID.h */
struct atom_context;
int amdgpu_atom_execute_table(struct atom_context *ctx, int index, uint32_t *params, int params_size);
bool amdgpu_atom_parse_cmd_header(struct atom_context *ctx, int index, uint8_t *frev, uint8_t *crev);
#endif
