/*
 * dcn41_io.h - internal register primitives of the dcn41 layer (not part of the public API).
 *
 * Each primitive reproduces the register-level behaviour of the Linux DC helper it names (dc/dc_helper.c,
 * dc/dm_services.h, 238650ef6c7c), because the trace harness compares our reads and writes one for one:
 *   dcn41_update  REG_UPDATE*  -> generic_reg_update_ex: one read, then one write of (v & ~mask) | bits
 *   dcn41_set     REG_SET*     -> generic_reg_set_ex:    one write of (init & ~mask) | bits, no read
 *   dcn41_get     REG_GET*     -> generic_reg_get:       one read, (v & mask) >> shift
 *   dcn41_wait    REG_WAIT     -> generic_reg_wait:      read; compare; between tries msleep(d/1000) if d >= 1000
 *                                                        else udelay(d); tries = max_try + 1 reads at most
 * `bits` is always the field value already shifted and masked: DCN41_FV(REG, FIELD, value).
 * Every address passes through dcn41_abs(), which returns DCN41_BAD_OFFSET for an unknown offset or base; the public
 * functions validate instances before the first primitive, so a BAD address here is a programming error and is
 * refused without I/O.
 */
#ifndef N48_DCN41_IO_H
#define N48_DCN41_IO_H

#include "dcn41.h"

#define DCN41_FV(REG, FIELD, value) \
    ((((uint32_t)(value)) << REG##__##FIELD##__SHIFT) & REG##__##FIELD##_MASK)
#define DCN41_FG(REG, FIELD, regval) \
    ((((uint32_t)(regval)) & REG##__##FIELD##_MASK) >> REG##__##FIELD##__SHIFT)
#define DCN41_M(REG, FIELD) (REG##__##FIELD##_MASK)

static inline bool dcn41_ready(const struct dcn41_dev *dev)
{
    return dev && dev->magic == DCN41_DEV_MAGIC && dev->rreg && dev->wreg && dev->udelay;
}

static inline int dcn41_read(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t *v)
{
    if (abs_dword == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    *v = dev->rreg(dev->cookie, abs_dword);
    return DCN41_OK;
}

static inline int dcn41_write(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t v)
{
    if (abs_dword == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    dev->wreg(dev->cookie, abs_dword, v);
    return DCN41_OK;
}

static inline int dcn41_update(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t mask, uint32_t bits)
{
    uint32_t v = 0;
    int rc = dcn41_read(dev, abs_dword, &v);
    if (rc)
        return rc;
    return dcn41_write(dev, abs_dword, (v & ~mask) | bits);
}

static inline int dcn41_set(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t init, uint32_t mask, uint32_t bits)
{
    return dcn41_write(dev, abs_dword, (init & ~mask) | bits);
}

static inline int dcn41_get(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t mask, uint32_t shift, uint32_t *field)
{
    uint32_t v = 0;
    int rc = dcn41_read(dev, abs_dword, &v);
    if (rc)
        return rc;
    *field = (v & mask) >> shift;
    return DCN41_OK;
}

static inline int dcn41_wait(struct dcn41_dev *dev, uint32_t abs_dword, uint32_t mask, uint32_t shift,
                             uint32_t condition, uint32_t delay_us, uint32_t max_try)
{
    uint32_t i, field = 0;
    if (abs_dword == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    for (i = 0; i <= max_try; i++) {
        int rc;
        if (i) {
            if (delay_us >= 1000)
                dev->udelay(dev->cookie, (delay_us / 1000) * 1000);
            else if (delay_us > 0)
                dev->udelay(dev->cookie, delay_us);
        }
        rc = dcn41_get(dev, abs_dword, mask, shift, &field);
        if (rc)
            return rc;
        if (field == condition)
            return DCN41_OK;
    }
    return DCN41_E_TIMEOUT;
}

/* absolute address of an instanced register: R is the generated name without its (i) */
#define DCN41_ADDR_I(dev, R, i) dcn41_abs((dev), R(i), R##_BASE_IDX)
#define DCN41_ADDR(dev, R) dcn41_abs((dev), R, R##_BASE_IDX)

#endif /* N48_DCN41_IO_H */
