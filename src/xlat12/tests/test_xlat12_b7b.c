/* build 0.0.471 item 3 — THE B7b TEST: a class-10 kDTableAbi row whose ABI-pointer row does not declare the
 * entry-table slot must refuse XLAT12_TDESC_TOO_MANY at d_table_desc's own guard (`class10 && !d_tbl_abi_declares(a,
 * a->textbl1 - 1u)`, xlat12_ib.c), never place anything, and leave the translated output unchanged. 0.0.470 shipped
 * this guard with nothing driving a REAL draw through it alone (test_kdtableabi_rows.py and test_new_shapes() in
 * tests/test_xlat12_ib.c cover every SHIPPED row's own shape and ABI declaration, but none of them is malformed on
 * purpose) - the planted break this test catches (the guard line removed) went uncaught in 0.0.470.
 *
 * A SEPARATE, minimal binary, on purpose: it links xlat12_ib.c compiled WITH -DXLAT12_TEST_EXTRA_TBL_ROW, which
 * appends ONE test-only row to kDTableAbi (never present in the kext build, which never defines that macro, so the
 * kext's own 32-row table is byte-identical with and without this file existing). The extra row clones AZ's
 * (ws_AZ_TimgBdrkXhn_IsrcCrd) class-10 shape field for field but with an (ndw, fnv) pair no xlat12_abi_ptrs.h row
 * declares, so xlat12_abi_ptr_row returns NULL and d_tbl_abi_declares answers 0 for every slot including textbl1's.
 *
 * This file does NOT #include tests/test_xlat12_ib.c: that file's own test_new_shapes() (N4) asserts
 * xlat12_table_abi_count() == 32 and a fixed new-shape count - both would break under the extra row, for a reason
 * that has nothing to do with THIS test. The tiny harness below (TdE/td_slack/td_pgm/td_draw) is that file's own,
 * copied verbatim (byte-identical packet encodings) rather than shared, so a change to one never has to think about
 * the other's now-different row count.
 *
 * Build and run: make -C src/xlat12 test (wired into the `test` target's own recipe as this file's own binary). */
#include <stdio.h>
#include <string.h>
#include "xlat12.h"
#include "xlat12_ib.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* tests/test_xlat12_ib.c's own TdE/td_slack/td_pgm/td_draw, copied verbatim - see this file's own banner above for
 * why this is a copy and not a shared #include. */
typedef struct { uint32_t *in, k; } TdE;
static void td_slack(TdE *e, uint32_t q) { while (q--) { e->in[e->k++] = 0xC0016900u; e->in[e->k++] = (0x28c8cu - 0x28000u) >> 2; e->in[e->k++] = 0u; } }
static void td_pgm(TdE *e, uint32_t lo) { e->in[e->k++] = 0xC0047600u; e->in[e->k++] = (0xb020u >> 2) - 0x2c00u; e->in[e->k++] = lo;
                                          e->in[e->k++] = 0u; e->in[e->k++] = 0x020F0000u; e->in[e->k++] = 0x00000020u; }
static uint32_t td_draw(TdE *e) { const uint32_t at = e->k; e->in[e->k++] = 0xC0012D00u; e->in[e->k++] = 3u; e->in[e->k++] = 2u; return at; }

#define B7B_PGM_VA 0x400090000ull   /* free: tests/test_xlat12_ib.c's own fake program VAs never reach 0x400090000 */
#define B7B_PGM_LO 0x04000900u

static uint32_t gB7bAbi1;   /* the test-only row's 1-based index - always the LAST row, xlat12_table_abi_count() */

static int b7b_resolver(void *ctx, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    (void)ctx;
    if (stage == 0u && va == B7B_PGM_VA) out->ps_table_abi1 = gB7bAbi1;
    return 1;
}
/* never actually reached: the guard this test drives refuses before any class-19 record is read. A read that DID
 * happen would be this test's own bug, so it returns failure rather than manufacturing plausible data. */
static int b7b_read(void *ctx, uint64_t va, uint32_t ndw, uint32_t *out)
{
    (void)ctx; (void)va; (void)ndw; (void)out;
    return 0;
}
static int b7b_tiled(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes)
{
    (void)ctx; (void)va; (void)mode; (void)elemBytes;
    return 0;
}

int main(void)
{
#ifndef XLAT12_TEST_EXTRA_TBL_ROW
    printf("test_xlat12_b7b: built WITHOUT -DXLAT12_TEST_EXTRA_TBL_ROW (no test-only row) - nothing to drive; SKIP\n");
    return 0;
#else
    gB7bAbi1 = xlat12_table_abi_count();
    CHECK(gB7bAbi1 >= 33u, "B7b the test-only row is present (xlat12_table_abi_count %u)", gB7bAbi1);
    CHECK(xlat12_table_abi_row_ok(gB7bAbi1), "B7b the test-only row passes its OWN shape check (it clones AZ's, unchanged)");
    CHECK(xlat12_table_abi_new_shape(gB7bAbi1), "B7b the test-only row is a class-10 shape (xlat12_table_abi_new_shape)");
    CHECK(!xlat12_table_abi_is_gated(gB7bAbi1), "B7b control: the test-only row is NOT in the switch-43 gated list - "
          "it exists only to drive this one guard, never admitted by the real kext");

    const xlat12_draw_profile *pf = xlat12_ib_m2tri_profile();
    static uint32_t in[64], out[64];
    /* SENTINEL, not zero: d_region() translates the region BEFORE the draw unconditionally (context-register
     * packets always get rewritten, table row or not - that is not this guard's business and it happens whether
     * the row is valid or not). What THIS guard must never do is place any part of a table record: nothing from
     * the draw packet itself (index d0) onward - where a SUCCESSFUL table step would place T#/S#/entry records in
     * the trailing pad - may be written before the refusal. A live sentinel there is the precise, checkable form
     * of "the output unchanged" for a refusal that fires before any placement write. */
    for (uint32_t k = 0; k < 64u; k++) out[k] = 0xDEADBEEFu;
    xlat12_draw_stats ds;
    xlat12_draw_extra ex; memset(&ex, 0, sizeof ex);
    ex.pgm_profile = &b7b_resolver; ex.flags = XLAT12_EXTRA_TABLE_DESC;
    ex.ib_va = 0x4000d0000ull; ex.desc_read = &b7b_read; ex.desc_tiled_ok = &b7b_tiled;

    TdE e; e.in = in; e.k = 0;
    td_slack(&e, 4u); td_pgm(&e, B7B_PGM_LO); const uint32_t d0 = td_draw(&e); const uint32_t n = e.k;
    uint32_t len = 0xFFFFFFFFu;
    const uint32_t st = xlat12_ib_translate_draw_ex(pf, &ex, in, n, out, &len, &ds);
    CHECK(st == XLAT12_IB_ERR_DESC, "B7b the draw refuses IB_ERR_DESC (got %s)", xlat12_ib_status_name(st));
    CHECK(ds.err_op == XLAT12_TDESC_TOO_MANY, "B7b refused TOO_MANY - the row's own shape passes, only the ABI-pointer "
          "declaration is missing (err_op %#x)", ds.err_op);
    CHECK(ds.in_ptr_known == 0u, "B7b in_ptr_known stays 0 - the pointer set was never established");
    CHECK(len == 0u, "B7b *out_len stays 0 on refusal - the caller is told not to trust `out` (len %u)", len);
    { int touched = 0;
      for (uint32_t k = d0; k < 64u; k++) if (out[k] != 0xDEADBEEFu) touched++;
      CHECK(touched == 0, "B7b the output is unchanged from the draw packet onward (index %u..63): the guard fires "
            "before ANY placement write, not even the draw packet's own copy (%d dword(s) touched)", d0, touched); }

    printf("test_xlat12_b7b: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
#endif
}
