/*
 * dcn41_hubp.c - DCN 4.1 HUBP plane address program (the page flip), flip-pending readout, flip interrupt enable.
 *
 * Mirrors Linux 238650ef6c7c dc/hubp/dcn401/dcn401_hubp.c hubp401_program_surface_flip_and_addr (graphics address
 * type), hubp401_set_flip_int, hubp401_in_blank, and dc/hubp/dcn20/dcn20_hubp.c hubp2_is_flip_pending, register for
 * register. See dcn41.h for the refusals that happen before any I/O.
 */
#include "dcn41_io.h"

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

#define DCN41_ADDR_LIMIT (1ull << 48)   /* PRIMARY_SURFACE_ADDRESS_HIGH_MASK 0x0000FFFF: 32 + 16 bits */

int dcn41_hubp_program_flip(struct dcn41_dev *dev, uint32_t hubp, uint64_t mc_addr, uint32_t vmid, bool tmz,
                            bool immediate)
{
    uint32_t fc, vm, sc, hi, lo;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    if (mc_addr == 0 || mc_addr >= DCN41_ADDR_LIMIT)
        return DCN41_E_ADDR;
    if (dev->scanout_hi != 0 && (mc_addr < dev->scanout_lo || mc_addr >= dev->scanout_hi))
        return DCN41_E_ADDR;
    /* build 0.0.518 (flip mode): an exact set, when one is set, admits only its members (A and B). */
    if (dev->flip_exact_n != 0u && mc_addr != dev->flip_exact[0] &&
        (dev->flip_exact_n < 2u || mc_addr != dev->flip_exact[1]))
        return DCN41_E_ADDR;
    if (vmid > DCN41_FG(DCN41_HUBPREQ_VMID_SETTINGS_0, VMID, 0xFFFFFFFFu))
        return DCN41_E_ARG;
    fc = DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, hubp);
    vm = DCN41_ADDR_I(dev, DCN41_HUBPREQ_VMID_SETTINGS_0, hubp);
    sc = DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_CONTROL, hubp);
    hi = DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, hubp);
    lo = DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS, hubp);
    if (fc == DCN41_BAD_OFFSET || vm == DCN41_BAD_OFFSET || sc == DCN41_BAD_OFFSET || hi == DCN41_BAD_OFFSET ||
        lo == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;

    /* REG_UPDATE(DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, flip_immediate); */
    rc = dcn41_update(dev, fc, DCN41_M(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE),
                      DCN41_FV(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, immediate ? 1 : 0));
    if (rc)
        return rc;
    /* if (flip_immediate == 0) REG_UPDATE(VMID_SETTINGS_0, VMID, address->vmid); */
    if (!immediate) {
        rc = dcn41_update(dev, vm, DCN41_M(DCN41_HUBPREQ_VMID_SETTINGS_0, VMID),
                          DCN41_FV(DCN41_HUBPREQ_VMID_SETTINGS_0, VMID, vmid));
        if (rc)
            return rc;
    }
    /* "turn off stereo if not in stereo": two separate REG_UPDATEs */
    rc = dcn41_update(dev, fc, DCN41_M(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_MODE_FOR_STEREOSYNC), 0);
    if (rc)
        return rc;
    rc = dcn41_update(dev, fc, DCN41_M(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_IN_STEREOSYNC), 0);
    if (rc)
        return rc;
    /* PLN_ADDR_TYPE_GRAPHICS: REG_UPDATE(DCSURF_SURFACE_CONTROL, PRIMARY_SURFACE_TMZ, address->tmz_surface); */
    rc = dcn41_update(dev, sc, DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_CONTROL, PRIMARY_SURFACE_TMZ),
                      DCN41_FV(DCN41_HUBPREQ_DCSURF_SURFACE_CONTROL, PRIMARY_SURFACE_TMZ, tmz ? 1 : 0));
    if (rc)
        return rc;
    /* "program high first and then the low addr, order matters!" REG_SET(..._HIGH, 0, ...); REG_SET(..., 0, ...); */
    rc = dcn41_set(dev, hi, 0, DCN41_M(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, PRIMARY_SURFACE_ADDRESS_HIGH),
                   DCN41_FV(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, PRIMARY_SURFACE_ADDRESS_HIGH,
                            (uint32_t)(mc_addr >> 32)));
    if (rc)
        return rc;
    rc = dcn41_set(dev, lo, 0, DCN41_M(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS, PRIMARY_SURFACE_ADDRESS),
                   DCN41_FV(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS, PRIMARY_SURFACE_ADDRESS,
                            (uint32_t)mc_addr));
    if (rc)
        return rc;
    /* hubp->request_address = *address; */
    dev->hubp_request_addr[hubp] = mc_addr;
    dev->hubp_request_valid[hubp] = 1;
    return DCN41_OK;
}

int dcn41_hubp_is_flip_pending(struct dcn41_dev *dev, uint32_t hubp, bool *pending, uint64_t *earliest_inuse)
{
    uint32_t flip_pending = 0, lo = 0, hi = 0;
    uint64_t earliest;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!pending)
        return DCN41_E_ARG;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* if (hubp && hubp->power_gated) return false; */
    if (dev->hubp_power_gated[hubp]) {
        *pending = false;
        return DCN41_OK;
    }
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, hubp),
                   DCN41_M(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL, SURFACE_FLIP_PENDING),
                   DCN41_HUBPREQ_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING__SHIFT, &flip_pending);
    if (rc)
        return rc;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE, hubp),
                   DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE, SURFACE_EARLIEST_INUSE_ADDRESS),
                   DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE__SURFACE_EARLIEST_INUSE_ADDRESS__SHIFT, &lo);
    if (rc)
        return rc;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, hubp),
                   DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, SURFACE_EARLIEST_INUSE_ADDRESS_HIGH),
                   DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH__SHIFT, &hi);
    if (rc)
        return rc;
    earliest = ((uint64_t)hi << 32) | lo;
    if (earliest_inuse)
        *earliest_inuse = earliest;
    *pending = flip_pending != 0 || earliest != dev->hubp_request_addr[hubp];
    return dev->hubp_request_valid[hubp] ? DCN41_OK : DCN41_E_NOREQ;
}

int dcn41_hubp_set_flip_int(struct dcn41_dev *dev, uint32_t hubp)
{
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_UPDATE(DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_MASK, 1); */
    return dcn41_update(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, hubp),
                        DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_MASK),
                        DCN41_FV(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_MASK, 1));
}

int dcn41_hubp_in_blank(struct dcn41_dev *dev, uint32_t hubp, bool *in_blank)
{
    uint32_t v = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!in_blank)
        return DCN41_E_ARG;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBP_DCHUBP_CNTL, hubp), DCN41_M(DCN41_HUBP_DCHUBP_CNTL, HUBP_IN_BLANK),
                   DCN41_HUBP_DCHUBP_CNTL__HUBP_IN_BLANK__SHIFT, &v);
    if (rc)
        return rc;
    *in_blank = v != 0;
    return DCN41_OK;
}

int dcn41_hubp_read_primary_addr(struct dcn41_dev *dev, uint32_t hubp, uint64_t *mc_addr)
{
    uint32_t hi = 0, lo = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!mc_addr)
        return DCN41_E_ARG;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, hubp),
                   DCN41_M(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, PRIMARY_SURFACE_ADDRESS_HIGH),
                   DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH__SHIFT, &hi);
    if (rc)
        return rc;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS, hubp),
                   DCN41_M(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS, PRIMARY_SURFACE_ADDRESS),
                   DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS__PRIMARY_SURFACE_ADDRESS__SHIFT, &lo);
    if (rc)
        return rc;
    *mc_addr = ((uint64_t)hi << 32) | lo;
    return DCN41_OK;
}
