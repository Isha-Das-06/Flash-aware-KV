/*
 * fkv_sim - a NOR flash simulator for host-side testing.
 *
 * Models:
 *   - erase sets a whole block to 0xFF; program can only clear bits;
 *   - misuse (unaligned or out-of-range access, programming a byte that is
 *     not erased) is counted in `violations` so tests can assert on it;
 *   - power loss: the Nth program/erase from a chosen point is interrupted.
 *     An interrupted program writes a random prefix of its data and leaves
 *     one byte with a random subset of its bits programmed; an interrupted
 *     erase leaves the block untouched, partially erased, or with random
 *     bits set. After the cut every call fails until fkv_sim_power_on().
 */
#ifndef FKV_SIM_H
#define FKV_SIM_H

#include "fkv.h"

#include <stdint.h>

struct fkv_sim {
    struct fkv_flash flash; /* pass &sim.flash to fkv */
    uint8_t *mem;
    uint32_t *erase_count;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t prog_size;

    uint64_t ops;    /* program + erase calls so far */
    uint64_t cut_at; /* index of the op that gets interrupted; UINT64_MAX = never */
    int powered;
    uint64_t rng;

    uint64_t reads;
    uint64_t progs;
    uint64_t erases;
    uint64_t bytes_programmed;
    uint64_t violations;
};

int fkv_sim_init(struct fkv_sim *s, uint32_t block_size, uint32_t block_count,
                 uint32_t prog_size, uint64_t seed);
void fkv_sim_free(struct fkv_sim *s);

/* Interrupt the program/erase that happens `n` operations from now
 * (n = 0: the very next one). */
void fkv_sim_cut_after(struct fkv_sim *s, uint64_t n);
void fkv_sim_power_on(struct fkv_sim *s);

/* Flip one bit, e.g. to simulate bit rot. */
void fkv_sim_flip_bit(struct fkv_sim *s, uint32_t block, uint32_t off, unsigned bit);

uint64_t fkv_sim_rand(struct fkv_sim *s);

#endif /* FKV_SIM_H */
