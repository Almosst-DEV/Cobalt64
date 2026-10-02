/* linuxshim: amd/include/cgs_common.h is not in the sparse clone; only the indirect-register names DC's
 * dm_services.h uses are declared (the harness never calls them). */
#ifndef DCN41_SHIM_CGS_COMMON_H
#define DCN41_SHIM_CGS_COMMON_H
#include <linux/types.h>
struct cgs_device;
enum cgs_ind_reg { CGS_IND_REG__PCIE, CGS_IND_REG__SMC, CGS_IND_REG__UVD_CTX, CGS_IND_REG__DIDT, CGS_IND_REG_GC_CAC,
	CGS_IND_REG_SE_CAC, CGS_IND_REG__AUDIO_ENDPT };
uint32_t cgs_read_ind_register(struct cgs_device *cgs_device, enum cgs_ind_reg space, unsigned index);
void cgs_write_ind_register(struct cgs_device *cgs_device, enum cgs_ind_reg space, unsigned index, uint32_t value);
uint32_t cgs_read_register(struct cgs_device *cgs_device, unsigned offset);
void cgs_write_register(struct cgs_device *cgs_device, unsigned offset, uint32_t value);
#endif
