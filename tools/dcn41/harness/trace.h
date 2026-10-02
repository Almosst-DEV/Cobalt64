/* trace.h - the register model and trace recorder shared by the Linux side and our side of the dcn41 harness.
 *
 * Both sides run the same scenario against the same model state (a seeded pseudo-random value in every register, so
 * a read-modify-write that clobbers a neighbouring field shows up as a value difference), and every register read,
 * write and delay is appended to the current trace. main.c compares the two traces event for event. */
#ifndef DCN41_HARNESS_TRACE_H
#define DCN41_HARNESS_TRACE_H
#include <stdint.h>

#define TRACE_MAX 4096
#define MODEL_DWORDS 0x10000u

struct trace_ev { char op; uint32_t addr, val; };          /* op: 'R' read, 'W' write, 'D' delay (val = us) */
struct trace { struct trace_ev ev[TRACE_MAX]; int n; int overflow; };

struct model_behaviour {
    uint32_t lock_reg[4];        /* absolute OTGn_OTG_MASTER_UPDATE_LOCK: UPDATE_LOCK_STATUS (0x100) reads 1 once ... */
    int lock_after;              /* ... more than this many reads of it happened; -1 = never */
    uint32_t moving_reg;         /* absolute OTGn_OTG_STATUS_POSITION that advances by one line per read; 0 = none */
    uint32_t gpint_reg;          /* DMCUB_GPINT_DATAIN1: after gpint_after reads the "firmware" clears the status nibble */
    int gpint_after;             /* -1 = never acknowledges */
    uint32_t rptr_reg, wptr_reg; /* DMCUB_INBOX1_RPTR / _WPTR: each RPTR read advances it by rptr_step toward WPTR */
    uint32_t rptr_step, ring_capacity;
};

void model_reset(uint64_t seed, const struct model_behaviour *b, struct trace *t);   /* seed 0 = zeros, ~0 = ones */
void model_poke(uint32_t addr, uint32_t val);               /* set state without tracing */
uint32_t model_peek(uint32_t addr);
uint32_t model_read(uint32_t addr);
void model_write(uint32_t addr, uint32_t val);
void model_delay(uint32_t us);
#endif
