/* ibtool - host census and translation check over a dumped Apple PM4 stream.
 *
 *   build/ibtool FILE.bin [...]      raw little-endian dwords (tri --dump *.bin, or any extracted IB)
 *
 * For each file: the xlat12_ib census the kext's renderib prints, then xlat12_ib_translate with
 * vs_mode UNKNOWN and with vs_mode NGG seeded, each with its status, failing opcode/register, the
 * translation statistics and whether the output fits in place (renderxlat mode 1 pads to the input
 * length). The same sources the kext links, so the host verdict is the kext's verdict. Host only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat12.h"
#include "xlat12_ib.h"

static void census_line(const char *f, const uint32_t *d, uint32_t n)
{
    const uint32_t w = xlat12_ib_walk(d, n);
    xlat12_ib_census c;
    xlat12_ib_census_run(d, w, &c);
    printf("%s: %u dwords, walk %u, packets %u, ctxctl-first %u, end-on-NOP-128 %u early %u\n", f, n, w, c.packets,
           c.first_is_ctxctl, c.ends_on_nop_128, c.early_nop_128);
    printf("  SET ctx %u sh-gfx %u sh-cs %u ucfg %u index %u; regs %u: identical %u moved %u repack %u absent %u "
           "legacy-vs %u UNKNOWN %u reused %u cs-proven %u\n", c.set_ctx, c.set_sh_gfx, c.set_sh_cs, c.set_ucfg, c.set_index,
           c.regs, c.reg_cls[0], c.reg_cls[1], c.reg_cls[2], c.reg_cls[3], c.reg_cls[4], c.reg_cls[5], c.reg_cls[6],
           c.cs_proven_regs);
    printf("  CLEAR_STATE %u LOAD_* %u CONTEXT_CONTROL proven %u other %u; COND_EXEC %u nested-IB %u DMA_DATA %u "
           "COPY_DATA %u bad-reg-operand %u; draws %u dispatches %u unlisted %u (first op 0x%x); first UNKNOWN reg 0x%x; "
           "first memory-loaded op 0x%x\n", c.clear_state, c.load_reg, c.ctxctl_proven, c.ctxctl_other, c.cond_exec,
           c.nested_ib, c.dma_data, c.copy_data, c.reg_operand_bad, c.draws, c.dispatches, c.unlisted,
           c.first_unlisted_op, c.first_unknown_reg, c.first_memloaded_op);
    for (int mode = 0; mode < 2; mode++) {
        xlat12_ctx ctx; xlat12_stats st; uint32_t olen = 0, eop = 0;
        ctx.vs_mode = mode ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        uint32_t cap = w * 2u + 64u;
        uint32_t *out = calloc(cap, sizeof *out);
        if (!out) return;
        const uint32_t s = xlat12_ib_translate(&ctx, d, w, out, cap, &olen, &st, &eop);
        printf("  translate (vs_mode %s): %s at op 0x%x reg 0x%x (%s), input dword %u; regs in %u id %u mv %u rp %u "
               "absent %u legacy-vs %u; SET in %u out %u split %u; out %u dw%s\n", mode ? "NGG seeded" : "unknown",
               xlat12_ib_status_name(s), eop, st.err_reg_addr, st.err_name ? st.err_name : "-", st.err_in_dword,
               st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked, st.regs_dropped_absent,
               st.regs_dropped_legacy_vs, st.set_packets_in, st.set_packets_out, st.set_runs_split, olen,
               s ? "" : (olen <= w ? " (fits in place)" : " (LONGER than the input: in place refused)"));
        free(out);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ibtool FILE.bin [...]\n"); return 2; }
    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) { perror(argv[i]); continue; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint32_t n = (uint32_t)(sz / 4);
        uint32_t *d = calloc(n ? n : 1, sizeof *d);
        if (!d || fread(d, 4, n, fp) != n) { fprintf(stderr, "%s: short read\n", argv[i]); fclose(fp); free(d); continue; }
        fclose(fp);
        census_line(argv[i], d, n);
        free(d);
    }
    return 0;
}
