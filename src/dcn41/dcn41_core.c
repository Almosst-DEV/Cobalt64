/*
 * dcn41_core.c - device setup, address resolution and refresh arithmetic for the dcn41 layer. See dcn41.h.
 */
#include "dcn41_io.h"

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

int dcn41_dev_init(struct dcn41_dev *dev, void *cookie, dcn41_rreg_fn rreg, dcn41_wreg_fn wreg,
                   dcn41_udelay_fn udelay, const uint32_t seg[DCN41_NUM_SEGS], uint32_t mmio_dwords, uint32_t flags)
{
    static const uint32_t expected[DCN41_NUM_SEGS] = {
        DCN41_SEG0_EXPECTED, DCN41_SEG1_EXPECTED, DCN41_SEG2_EXPECTED, DCN41_SEG3_EXPECTED, DCN41_SEG4_EXPECTED,
    };
    uint32_t i;

    if (!dev)
        return DCN41_E_ARG;
    for (i = 0; i < sizeof(*dev); i++)
        ((uint8_t *)dev)[i] = 0;
    if (!rreg || !wreg || !udelay || !seg || mmio_dwords == 0)
        return DCN41_E_ARG;
    if (!(flags & DCN41_F_ALLOW_OTHER_BASES)) {
        for (i = 0; i < DCN41_NUM_SEGS; i++)
            if (seg[i] != expected[i])
                return DCN41_E_BASES;
    }
    for (i = 0; i < DCN41_NUM_SEGS; i++) {
        uint32_t max_off = 0;
        if (!(DCN41_BASE_IDX_USED_MASK & (1u << i)))
            continue;
#ifdef DCN41_MAX_OFFSET_BASE0
        if (i == 0) max_off = DCN41_MAX_OFFSET_BASE0;
#endif
#ifdef DCN41_MAX_OFFSET_BASE1
        if (i == 1) max_off = DCN41_MAX_OFFSET_BASE1;
#endif
#ifdef DCN41_MAX_OFFSET_BASE2
        if (i == 2) max_off = DCN41_MAX_OFFSET_BASE2;
#endif
#ifdef DCN41_MAX_OFFSET_BASE3
        if (i == 3) max_off = DCN41_MAX_OFFSET_BASE3;
#endif
#ifdef DCN41_MAX_OFFSET_BASE4
        if (i == 4) max_off = DCN41_MAX_OFFSET_BASE4;
#endif
        if (seg[i] == 0 || (uint64_t)seg[i] + max_off >= mmio_dwords)
            return DCN41_E_WINDOW;
    }
    dev->cookie = cookie;
    dev->rreg = rreg;
    dev->wreg = wreg;
    dev->udelay = udelay;
    for (i = 0; i < DCN41_NUM_SEGS; i++)
        dev->seg[i] = seg[i];
    dev->mmio_dwords = mmio_dwords;
    dev->flags = flags;
    dev->magic = DCN41_DEV_MAGIC;
    return DCN41_OK;
}

int dcn41_dev_set_scanout_window(struct dcn41_dev *dev, uint64_t lo, uint64_t hi)
{
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (hi != 0 && (lo >= hi || hi > (1ull << 48)))
        return DCN41_E_ARG;
    dev->scanout_lo = lo;
    dev->scanout_hi = hi;
    return DCN41_OK;
}

int dcn41_dev_set_flip_exact(struct dcn41_dev *dev, const uint64_t *addrs, uint32_t n)
{
    uint32_t i;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (n > 2u || (n != 0u && !addrs))
        return DCN41_E_ARG;
    for (i = 0; i < n; i++) {
        if (addrs[i] == 0 || addrs[i] >= (1ull << 48))
            return DCN41_E_ARG;
        if (dev->scanout_hi != 0 && (addrs[i] < dev->scanout_lo || addrs[i] >= dev->scanout_hi))
            return DCN41_E_ARG;
    }
    dev->flip_exact[0] = n > 0u ? addrs[0] : 0u;
    dev->flip_exact[1] = n > 1u ? addrs[1] : 0u;
    dev->flip_exact_n = n;
    return DCN41_OK;
}

uint32_t dcn41_abs(const struct dcn41_dev *dev, uint32_t offset, uint32_t base_idx)
{
    uint64_t a;
    if (!dcn41_ready(dev) || offset == DCN41_BAD_OFFSET || base_idx >= DCN41_NUM_SEGS ||
        !(DCN41_BASE_IDX_USED_MASK & (1u << base_idx)))
        return DCN41_BAD_OFFSET;
    a = (uint64_t)dev->seg[base_idx] + offset;
    if (a >= dev->mmio_dwords)
        return DCN41_BAD_OFFSET;
    return (uint32_t)a;
}

uint32_t dcn41_frame_delta(uint32_t count0, uint32_t count1)
{
    return (count1 - count0) & DCN41_FRAME_COUNT_MASK;
}

uint64_t dcn41_refresh_mhz(uint32_t count0, uint32_t count1, uint64_t elapsed_ns)
{
    /* frames * 1e12 / ns = mHz. frames <= 0xFFFFFF, so frames * 1e12 <= 1.68e19 < 2^64 (1.84e19): no overflow. */
    uint64_t frames = dcn41_frame_delta(count0, count1);
    if (elapsed_ns == 0)
        return 0;
    return (frames * 1000000000000ull + elapsed_ns / 2) / elapsed_ns;
}

uint64_t dcn41_nominal_refresh_mhz(uint32_t pix_clk_100hz, uint32_t h_total, uint32_t v_total)
{
    /* pixel clock (Hz) = pix_clk_100hz * 100; mHz = Hz * 1000 / (h_total * v_total). 2^32 * 1e5 < 2^64. */
    uint64_t pixels = (uint64_t)h_total * v_total;
    if (pixels == 0)
        return 0;
    return ((uint64_t)pix_clk_100hz * 100000ull + pixels / 2) / pixels;
}
