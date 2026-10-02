/* scen.h - one harness scenario, run once through Linux's compiled DC functions (linux_side.c) and once through
 * src/dcn41 (ours_side.c), against identical model state. */
#ifndef DCN41_HARNESS_SCEN_H
#define DCN41_HARNESS_SCEN_H
#include <stdint.h>

enum scen_kind {
    SC_IRQ_SET, SC_IRQ_ACK, SC_IH_MAP, SC_FLIP, SC_FLIP_PENDING, SC_SET_FLIP_INT, SC_IN_BLANK, SC_LOCK, SC_UNLOCK,
    SC_POSITION, SC_FRAME_COUNT, SC_SCANOUTPOS, SC_ACTIVE_SIZE, SC_COUNTER_MOVING, SC_KIND_COUNT
};

struct scen_args {
    int kind;
    int irq_kind, inst, enable;               /* irq_kind: enum dcn41_irq_kind */
    uint64_t addr;
    uint32_t vmid;
    int tmz, immediate;
    int force_latched, pending_bit;           /* SC_FLIP_PENDING: after the flip, make EARLIEST_INUSE == addr / set bit */
    uint32_t src, ext;                        /* SC_IH_MAP */
    int otg_enabled;                          /* SC_ACTIVE_SIZE: OTG_MASTER_EN forced to this */
};

struct scen_out { int n; int64_t v[8]; };

void L_init(void);
int L_run(const struct scen_args *a, struct scen_out *o);
uint32_t L_abs_lock_reg(int otg);             /* from Linux's own tg_regs, for the model's behaviours */
uint32_t L_abs_position_reg(int otg);
uint32_t L_abs_earliest_inuse(int hubp, int high);
uint32_t L_abs_flip_control(int hubp);
uint32_t L_abs_otg_control(int otg);

void O_init(void);
int O_run(const struct scen_args *a, struct scen_out *o);
#endif
