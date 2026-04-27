#include "test.h"

#include <string.h>

const char *t_current = "?";

/* 8 x 512-byte blocks: small enough that GC and wear leveling run
 * constantly, which is what the crash tests want. */
const struct geom GEOM_SMALL = {
    .block_size = 512,
    .block_count = 8,
    .prog_size = 4,
    .max_key = 16,
    .max_val = 64,
    .slots = 128,
    .policy = FKV_GC_GREEDY,
    .wl_threshold = 4,
};

uint64_t rng_next(uint64_t *s)
{
    /* splitmix64 */
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void env_init(struct env *e, const struct geom *g, uint64_t seed)
{
    memset(e, 0, sizeof *e);
    REQUIRE(fkv_sim_init(&e->sim, g->block_size, g->block_count, g->prog_size, seed) == 0);

    struct fkv_config *c = &e->cfg;
    c->flash = &e->sim.flash;
    c->max_key_len = g->max_key;
    c->max_value_len = g->max_val;
    c->gc_policy = g->policy;
    c->wl_threshold = g->wl_threshold;
    c->blocks = calloc(g->block_count, sizeof *c->blocks);
    c->slot_count = g->slots;
    c->slots = calloc(g->slots, sizeof *c->slots);
    c->scratch_size = FKV_SCRATCH_SIZE(g->max_key, g->max_val, g->prog_size);
    c->scratch = malloc(c->scratch_size);
    REQUIRE(c->blocks && c->slots && c->scratch);

    REQUIRE_OK(fkv_format(c));
    REQUIRE_OK(fkv_mount(&e->db, c));
}

void env_remount(struct env *e)
{
    REQUIRE_OK(fkv_unmount(&e->db));
    REQUIRE_OK(fkv_mount(&e->db, &e->cfg));
    REQUIRE_OK(fkv_check(&e->db));
}

void env_free(struct env *e)
{
    REQUIRE(e->sim.violations == 0);
    free(e->cfg.blocks);
    free(e->cfg.slots);
    free(e->cfg.scratch);
    fkv_sim_free(&e->sim);
}

size_t key_name(unsigned k, char *buf)
{
    return (size_t)sprintf(buf, "key-%03u", k);
}

void value_bytes(uint32_t seed, uint16_t vlen, uint8_t *out)
{
    uint64_t s = seed;
    for (uint16_t i = 0; i < vlen; i++)
        out[i] = (uint8_t)rng_next(&s);
}

void model_apply(struct model *m, const struct op *op)
{
    if (op->type == OP_PUT) {
        m->present[op->key] = true;
        m->vlen[op->key] = op->vlen;
        value_bytes(op->seed, op->vlen, m->val[op->key]);
    } else {
        m->present[op->key] = false;
    }
}

int op_apply(fkv_t *db, const struct op *op)
{
    char key[32];
    size_t klen = key_name(op->key, key);
    if (op->type == OP_DEL)
        return fkv_del(db, key, klen);
    uint8_t val[MODEL_VAL];
    value_bytes(op->seed, op->vlen, val);
    return fkv_put(db, key, klen, val, op->vlen);
}

int op_expected(const struct model *m, const struct op *op)
{
    if (op->type == OP_DEL && !m->present[op->key])
        return FKV_ERR_NOTFOUND;
    return FKV_OK;
}

static int count_cb(const void *k, size_t kl, const void *v, size_t vl, void *ctx)
{
    (void)k, (void)kl, (void)v, (void)vl;
    (*(unsigned *)ctx)++;
    return 0;
}

bool model_matches(fkv_t *db, const struct model *m, unsigned nkeys)
{
    unsigned want = 0, got = 0;
    for (unsigned k = 0; k < nkeys; k++) {
        char key[32];
        uint8_t buf[MODEL_VAL];
        size_t klen = key_name(k, key), vlen = 0;
        int rc = fkv_get(db, key, klen, buf, sizeof buf, &vlen);
        if (m->present[k]) {
            want++;
            if (rc != FKV_OK || vlen != m->vlen[k] || memcmp(buf, m->val[k], vlen) != 0)
                return false;
        } else if (rc != FKV_ERR_NOTFOUND) {
            return false;
        }
    }
    if (fkv_foreach(db, count_cb, &got) != FKV_OK)
        return false;
    return got == want;
}

void require_matches(fkv_t *db, const struct model *m, unsigned nkeys)
{
    REQUIRE(model_matches(db, m, nkeys));
}

void gen_ops(struct op *ops, int n, unsigned nkeys, uint16_t max_val, int put_pct,
             uint64_t seed)
{
    for (int i = 0; i < n; i++) {
        ops[i].type = (int)(rng_next(&seed) % 100) < put_pct ? OP_PUT : OP_DEL;
        ops[i].key = (uint8_t)(rng_next(&seed) % nkeys);
        ops[i].vlen = (uint16_t)(rng_next(&seed) % ((uint32_t)max_val + 1));
        ops[i].seed = (uint32_t)rng_next(&seed);
    }
}
