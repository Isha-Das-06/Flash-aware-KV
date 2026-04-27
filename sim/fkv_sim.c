#include "fkv_sim.h"

#include <stdlib.h>
#include <string.h>

uint64_t fkv_sim_rand(struct fkv_sim *s)
{
    /* xorshift64* */
    uint64_t x = s->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static int in_range(const struct fkv_sim *s, uint32_t block, uint32_t off, uint32_t len)
{
    return block < s->block_count && off <= s->block_size && len <= s->block_size - off;
}

/* Returns 1 if this program/erase is the one the power cut interrupts. */
static int tick(struct fkv_sim *s)
{
    int cut = s->ops == s->cut_at;
    s->ops++;
    if (cut)
        s->powered = 0;
    return cut;
}

static int sim_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t len)
{
    struct fkv_sim *s = ctx;
    if (!s->powered)
        return -1;
    if (!in_range(s, block, off, len)) {
        s->violations++;
        return -1;
    }
    s->reads++;
    memcpy(buf, s->mem + (size_t)block * s->block_size + off, len);
    return 0;
}

static int sim_prog(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t len)
{
    struct fkv_sim *s = ctx;
    const uint8_t *src = buf;
    if (!s->powered)
        return -1;
    if (!in_range(s, block, off, len) || off % s->prog_size || len % s->prog_size) {
        s->violations++;
        return -1;
    }
    uint8_t *dst = s->mem + (size_t)block * s->block_size + off;

    /* fkv promises to only program erased bytes. */
    for (uint32_t i = 0; i < len; i++)
        if (src[i] != 0xFF && dst[i] != 0xFF)
            s->violations++;

    if (tick(s)) {
        uint32_t n;
        switch (fkv_sim_rand(s) % 4) {
        case 0: n = 0; break;         /* nothing written */
        case 1: n = len; break;       /* written, but no ack */
        default: n = (uint32_t)(fkv_sim_rand(s) % len);
        }
        for (uint32_t i = 0; i < n; i++)
            dst[i] &= src[i];
        if (n < len)
            dst[n] &= (uint8_t)(src[n] | (uint8_t)fkv_sim_rand(s)); /* half-programmed byte */
        return -1;
    }

    for (uint32_t i = 0; i < len; i++)
        dst[i] &= src[i];
    s->progs++;
    s->bytes_programmed += len;
    return 0;
}

static int sim_erase(void *ctx, uint32_t block)
{
    struct fkv_sim *s = ctx;
    if (!s->powered)
        return -1;
    if (block >= s->block_count) {
        s->violations++;
        return -1;
    }
    uint8_t *dst = s->mem + (size_t)block * s->block_size;

    if (tick(s)) {
        switch (fkv_sim_rand(s) % 3) {
        case 0: /* not started */
            break;
        case 1: { /* partly erased */
            uint32_t n = (uint32_t)(fkv_sim_rand(s) % s->block_size);
            memset(dst, 0xFF, n);
            break;
        }
        default: /* cells drifted toward erased */
            for (uint32_t i = 0; i < s->block_size; i++)
                dst[i] |= (uint8_t)fkv_sim_rand(s);
        }
        return -1;
    }

    memset(dst, 0xFF, s->block_size);
    s->erase_count[block]++;
    s->erases++;
    return 0;
}

int fkv_sim_init(struct fkv_sim *s, uint32_t block_size, uint32_t block_count,
                 uint32_t prog_size, uint64_t seed)
{
    memset(s, 0, sizeof *s);
    s->mem = malloc((size_t)block_size * block_count);
    s->erase_count = calloc(block_count, sizeof *s->erase_count);
    if (!s->mem || !s->erase_count) {
        fkv_sim_free(s);
        return -1;
    }
    memset(s->mem, 0xFF, (size_t)block_size * block_count);
    s->block_size = block_size;
    s->block_count = block_count;
    s->prog_size = prog_size;
    s->cut_at = UINT64_MAX;
    s->powered = 1;
    s->rng = seed ? seed : 0x9E3779B97F4A7C15ull;

    s->flash.ctx = s;
    s->flash.block_size = block_size;
    s->flash.block_count = block_count;
    s->flash.prog_size = prog_size;
    s->flash.read = sim_read;
    s->flash.prog = sim_prog;
    s->flash.erase = sim_erase;
    return 0;
}

void fkv_sim_free(struct fkv_sim *s)
{
    free(s->mem);
    free(s->erase_count);
    s->mem = NULL;
    s->erase_count = NULL;
}

void fkv_sim_cut_after(struct fkv_sim *s, uint64_t n)
{
    s->cut_at = s->ops + n;
}

void fkv_sim_power_on(struct fkv_sim *s)
{
    s->powered = 1;
    s->cut_at = UINT64_MAX;
}

void fkv_sim_flip_bit(struct fkv_sim *s, uint32_t block, uint32_t off, unsigned bit)
{
    s->mem[(size_t)block * s->block_size + off] ^= (uint8_t)(1u << (bit & 7u));
}
