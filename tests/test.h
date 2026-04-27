#ifndef FKV_TEST_H
#define FKV_TEST_H

#include "fkv.h"
#include "fkv_sim.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- assertions: any failure prints context and exits non-zero ---------- */

extern const char *t_current;

#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "\n%s:%d: in %s: REQUIRE(%s) failed\n", __FILE__, __LINE__, \
                    t_current, #cond);                                                   \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

#define REQUIRE_RC(expr, want)                                                             \
    do {                                                                                   \
        int rc_ = (expr);                                                                  \
        if (rc_ != (want)) {                                                               \
            fprintf(stderr, "\n%s:%d: in %s: %s returned %d (%s), expected %d (%s)\n",   \
                    __FILE__, __LINE__, t_current, #expr, rc_, fkv_strerror(rc_), (want), \
                    fkv_strerror(want));                                                   \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

#define REQUIRE_OK(expr) REQUIRE_RC(expr, FKV_OK)

/* ---- test environment: simulated flash + caller-owned buffers ----------- */

struct geom {
    uint32_t block_size;
    uint32_t block_count;
    uint32_t prog_size;
    uint32_t max_key;
    uint32_t max_val;
    uint32_t slots;
    enum fkv_gc_policy policy;
    uint32_t wl_threshold;
};

struct env {
    struct fkv_sim sim;
    struct fkv_config cfg;
    fkv_t db;
};

/* Create flash, format and mount. */
void env_init(struct env *e, const struct geom *g, uint64_t seed);
void env_remount(struct env *e);
void env_free(struct env *e);
extern const struct geom GEOM_SMALL;

/* ---- reference model ---------------------------------------------------- */

#define MODEL_KEYS 128
#define MODEL_VAL 512

struct model {
    bool present[MODEL_KEYS];
    uint16_t vlen[MODEL_KEYS];
    uint8_t val[MODEL_KEYS][MODEL_VAL];
};

enum { OP_PUT, OP_DEL };

struct op {
    uint8_t type;
    uint8_t key;
    uint16_t vlen;
    uint32_t seed;
};

size_t key_name(unsigned k, char *buf);
void value_bytes(uint32_t seed, uint16_t vlen, uint8_t *out);

void model_apply(struct model *m, const struct op *op);
int op_apply(fkv_t *db, const struct op *op);
int op_expected(const struct model *m, const struct op *op);

/* True if the store holds exactly the model's contents. */
bool model_matches(fkv_t *db, const struct model *m, unsigned nkeys);
void require_matches(fkv_t *db, const struct model *m, unsigned nkeys);

/* Random workload. put_pct percent puts, the rest deletes. */
void gen_ops(struct op *ops, int n, unsigned nkeys, uint16_t max_val, int put_pct,
             uint64_t seed);

uint64_t rng_next(uint64_t *s);

#endif
