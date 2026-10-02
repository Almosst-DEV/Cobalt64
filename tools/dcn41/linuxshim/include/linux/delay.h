#ifndef DCN41_SHIM_LINUX_DELAY_H
#define DCN41_SHIM_LINUX_DELAY_H
#include <linux/types.h>
void dcn41_shim_udelay(unsigned long us);
#define udelay(us) dcn41_shim_udelay(us)
#define msleep(ms) dcn41_shim_udelay((unsigned long)(ms) * 1000UL)
#define fsleep(us) dcn41_shim_udelay(us)
#define usleep_range(lo, hi) dcn41_shim_udelay(lo)
#endif
