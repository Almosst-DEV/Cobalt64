/* trace.c - see trace.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"

static uint32_t mem[MODEL_DWORDS];
static struct model_behaviour beh;
static struct trace *cur;
static int lock_reads[4];
static int gpint_reads;

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void model_reset(uint64_t seed, const struct model_behaviour *b, struct trace *t)
{
    uint64_t s = seed;
    uint32_t i;
    /* seed 0: every register 0; seed ~0: every register 0xFFFFFFFF; otherwise pseudo-random. A random state can
     * already hold the bits a scenario sets (the first run's planted VSTARTUP-vs-VUPDATE control went uncaught on
     * seed 7 for exactly that reason), so the fixed fills are always part of the seed set. */
    for (i = 0; i < MODEL_DWORDS; i++)
        mem[i] = seed == 0 ? 0u : seed == ~0ull ? 0xFFFFFFFFu : (uint32_t)splitmix(&s);
    memset(&beh, 0, sizeof(beh));
    if (b)
        beh = *b;
    memset(lock_reads, 0, sizeof(lock_reads));
    gpint_reads = 0;
    cur = t;
    if (t) { t->n = 0; t->overflow = 0; }
}

static void rec(char op, uint32_t a, uint32_t v)
{
    if (!cur) return;
    if (cur->n < TRACE_MAX) cur->ev[cur->n++] = (struct trace_ev){ op, a, v };
    else cur->overflow = 1;
}

void model_poke(uint32_t a, uint32_t v) { if (a < MODEL_DWORDS) mem[a] = v; }
uint32_t model_peek(uint32_t a) { return a < MODEL_DWORDS ? mem[a] : 0; }

uint32_t model_read(uint32_t a)
{
    uint32_t v = a < MODEL_DWORDS ? mem[a] : 0xdeadbeefu;
    int i;
    for (i = 0; i < 4; i++) {
        if (beh.lock_reg[i] && a == beh.lock_reg[i]) {
            lock_reads[i]++;
            v &= ~0x100u;
            if (beh.lock_after >= 0 && lock_reads[i] > beh.lock_after) v |= 0x100u;
        }
    }
    if (beh.gpint_reg && a == beh.gpint_reg && (v & 0xF0000000u)) {
        gpint_reads++;
        if (beh.gpint_after >= 0 && gpint_reads > beh.gpint_after) {
            mem[a] = v & 0x0FFFFFFFu;
            v = mem[a];
        }
    }
    if (beh.rptr_reg && a == beh.rptr_reg && beh.rptr_step && beh.ring_capacity && mem[a] != mem[beh.wptr_reg]) {
        mem[a] = (mem[a] + beh.rptr_step) % beh.ring_capacity;
        v = mem[a];
    }
    if (beh.moving_reg && a == beh.moving_reg) {
        mem[a] = (mem[a] & ~0x7fffu) | ((mem[a] + 1) & 0x7fffu);
        v = mem[a];
    }
    rec('R', a, v);
    return v;
}

void model_write(uint32_t a, uint32_t v)
{
    if (a < MODEL_DWORDS) mem[a] = v;
    else { fprintf(stderr, "model_write outside the model: 0x%x\n", a); abort(); }
    rec('W', a, v);
}

void model_delay(uint32_t us) { rec('D', 0, us); }
