/* Randomized differential testing against a reference model, across flash
 * geometries and GC policies, with periodic remounts. */
#include "test.h"

#include <string.h>

static void run_model(const struct geom *g, unsigned nkeys, int nops, uint64_t seed)
{
    static struct model m;
    static struct op ops[20000];
    struct env e;

    memset(&m, 0, sizeof m);
    REQUIRE(nops <= (int)(sizeof ops / sizeof ops[0]));
    gen_ops(ops, nops, nkeys, (uint16_t)g->max_val, 75, seed);
    env_init(&e, g, seed);

    uint64_t rs = seed ^ 0xABCDu;
    for (int i = 0; i < nops; i++) {
        REQUIRE_RC(op_apply(&e.db, &ops[i]), op_expected(&m, &ops[i]));
        model_apply(&m, &ops[i]);
        if (rng_next(&rs) % 200 == 0) {
            env_remount(&e);
            require_matches(&e.db, &m, nkeys);
        }
    }
    REQUIRE_OK(fkv_check(&e.db));
    require_matches(&e.db, &m, nkeys);
    env_remount(&e);
    require_matches(&e.db, &m, nkeys);
    env_free(&e);
}

void test_model_geometries(void)
{
    static const struct {
        uint32_t bs, bc, ps, mv;
        unsigned nkeys;
    } G[] = {
        {512, 8, 4, 64, 24},     {512, 3, 1, 64, 6},      {256, 16, 16, 32, 30},
        {1024, 16, 8, 200, 40},  {4096, 8, 4, 500, 40},  {512, 2, 2, 64, 3},
        {2048, 32, 1, 128, 120},
    };
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++) {
        for (int policy = 0; policy < 2; policy++) {
            for (uint32_t wl = 0; wl <= 8; wl += 8) {
                struct geom g = {G[i].bs, G[i].bc, G[i].ps, 16, G[i].mv, 256,
                                 (enum fkv_gc_policy)policy, wl};
                run_model(&g, G[i].nkeys, 6000, 1000 + i * 10 + (unsigned)policy);
            }
        }
    }
}

void test_model_many_seeds(void)
{
    for (uint64_t seed = 1; seed <= 40; seed++)
        run_model(&GEOM_SMALL, 24, 3000, seed * 7919u);
}
