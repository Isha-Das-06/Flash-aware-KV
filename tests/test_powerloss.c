/*
 * Power-loss testing.
 *
 * For a fixed workload, first count how many program/erase operations it
 * performs. Then, for EVERY one of those operations, rerun the workload from
 * a freshly formatted device and cut power exactly there. After each cut:
 *
 *   - remount must succeed;
 *   - every operation that returned before the cut must be durable;
 *   - the interrupted operation must be atomic: the store equals the model
 *     either before or after it, never anything in between;
 *   - fkv_check() must pass, and the rest of the workload must run cleanly.
 *
 * The nested variant keeps cutting power again (during recovery, during the
 * compaction that follows, ...) up to several times per run.
 */
#include "test.h"

#include <string.h>

struct pl_stats {
    uint64_t cut_points;
    uint64_t cuts;
    uint64_t resolved_before;
    uint64_t resolved_after;
};

static struct pl_stats totals;

static void mount_after_cut(struct env *e, uint64_t *rng, int *cuts_left)
{
    for (;;) {
        fkv_sim_power_on(&e->sim);
        if (*cuts_left > 0 && rng_next(rng) % 3 == 0) {
            /* Maybe cut again while recovering. */
            (*cuts_left)--;
            fkv_sim_cut_after(&e->sim, rng_next(rng) % 4);
        }
        int rc = fkv_mount(&e->db, &e->cfg);
        if (rc == FKV_OK)
            break;
        REQUIRE_RC(rc, FKV_ERR_IO);
        totals.cuts++;
    }
    fkv_sim_power_on(&e->sim);
}

/* Run ops against a device with a power cut scheduled, resolving every cut. */
static void run_with_cuts(const struct geom *g, const struct op *ops, int nops, unsigned nkeys,
                          uint64_t first_cut, int extra_cuts, uint64_t seed)
{
    static struct model m, after;
    struct env e;
    uint64_t rng = seed;
    int cuts_left = extra_cuts;

    memset(&m, 0, sizeof m);
    env_init(&e, g, seed);
    fkv_sim_cut_after(&e.sim, first_cut);

    int i = 0;
    while (i < nops) {
        int rc = op_apply(&e.db, &ops[i]);
        if (rc != FKV_ERR_IO) {
            REQUIRE_RC(rc, op_expected(&m, &ops[i]));
            model_apply(&m, &ops[i]);
            i++;
            continue;
        }

        /* Power was cut during ops[i]. */
        totals.cuts++;
        mount_after_cut(&e, &rng, &cuts_left);
        REQUIRE_OK(fkv_check(&e.db));

        after = m;
        model_apply(&after, &ops[i]);
        if (model_matches(&e.db, &after, nkeys)) {
            m = after;
            totals.resolved_after++;
        } else {
            if (!model_matches(&e.db, &m, nkeys)) {
                fprintf(stderr, "\nstate after cut at op %d matches neither model\n", i);
                REQUIRE(false);
            }
            totals.resolved_before++;
        }
        i++;

        if (cuts_left > 0 && rng_next(&rng) % 2 == 0) {
            cuts_left--;
            fkv_sim_cut_after(&e.sim, rng_next(&rng) % 60);
        }
    }

    fkv_sim_power_on(&e.sim);
    require_matches(&e.db, &m, nkeys);
    env_remount(&e);
    require_matches(&e.db, &m, nkeys);
    env_free(&e);
}

static uint64_t count_ops(const struct geom *g, const struct op *ops, int nops)
{
    static struct model m;
    struct env e;
    memset(&m, 0, sizeof m);
    env_init(&e, g, 1);
    uint64_t start = e.sim.ops;
    for (int i = 0; i < nops; i++) {
        REQUIRE_RC(op_apply(&e.db, &ops[i]), op_expected(&m, &ops[i]));
        model_apply(&m, &ops[i]);
    }
    uint64_t n = e.sim.ops - start;
    env_free(&e);
    return n;
}

static void exhaustive(const struct geom *g, unsigned nkeys, int nops, int put_pct,
                       int extra_cuts, uint64_t seed)
{
    static struct op ops[1000];
    REQUIRE(nops <= 1000);
    gen_ops(ops, nops, nkeys, (uint16_t)g->max_val, put_pct, seed);

    uint64_t total = count_ops(g, ops, nops);
    for (uint64_t k = 0; k < total; k++) {
        run_with_cuts(g, ops, nops, nkeys, k, extra_cuts, seed * 1000003u + k);
        totals.cut_points++;
    }
}

static void report(const char *name)
{
    printf("      %s: %llu cut points, %llu power cuts (%llu rolled back, %llu completed)\n",
           name, (unsigned long long)totals.cut_points, (unsigned long long)totals.cuts,
           (unsigned long long)totals.resolved_before, (unsigned long long)totals.resolved_after);
    memset(&totals, 0, sizeof totals);
}

void test_powerloss_every_point(void)
{
    struct geom g = GEOM_SMALL;
    for (int policy = 0; policy < 2; policy++) {
        g.policy = (enum fkv_gc_policy)policy;
        exhaustive(&g, 24, 250, 75, 0, 11 + (unsigned)policy);
    }
    /* Byte-granular programming and a 3-block device: minimal slack. */
    g = GEOM_SMALL;
    g.prog_size = 1;
    g.block_count = 3;
    exhaustive(&g, 6, 250, 75, 0, 21);
    /* Delete-heavy: exercises tombstone dropping during compaction. */
    g = GEOM_SMALL;
    exhaustive(&g, 24, 250, 45, 0, 31);
    report("single cut");
}

void test_powerloss_nested(void)
{
    struct geom g = GEOM_SMALL;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        g.policy = (enum fkv_gc_policy)(seed % 2);
        exhaustive(&g, 24, 200, 70, 4, 100 + seed);
    }
    report("nested cuts");
}

void test_powerloss_random_long(void)
{
    /* Long random runs with many cuts each, on a bigger device. */
    static struct op ops[8000];
    struct geom g = {1024, 16, 8, 16, 120, 256, FKV_GC_COST_BENEFIT, 6};
    for (uint64_t seed = 1; seed <= 30; seed++) {
        gen_ops(ops, 8000, 60, 120, 70, seed);
        uint64_t rng = seed;
        run_with_cuts(&g, ops, 8000, 60, rng_next(&rng) % 500, 200, seed);
        totals.cut_points++;
    }
    report("long random");
}
