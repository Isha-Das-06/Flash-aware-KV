/*
 * Wear and write-amplification benchmark.
 *
 * Fills a 256 KiB simulated flash to a target utilisation, then performs a
 * long stream of updates under different access patterns and GC policies,
 * and reports:
 *
 *   WA        write amplification: flash bytes programmed / bytes the user
 *             asked to write (1.00 = no GC overhead)
 *   erase     min / mean / max erase count across all blocks
 *   evenness  mean / max erase count. 100% means every block wears at the
 *             same rate, so the device lives as long as physically possible.
 *
 *   usage: fkv_bench [--heatmap]
 */
#include "fkv.h"
#include "fkv_sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 4096u
#define BLOCK_COUNT 64u
#define PROG 4u
#define MAX_KEY 16u
#define MAX_VAL 128u
#define VAL_LEN 100u
#define SLOTS 4096u
#define UPDATES 400000
#define ENDURANCE 100000.0 /* erase cycles per block, typical NOR */

struct workload {
    const char *name;
    unsigned hot_pct;  /* share of writes that go to the hot set */
    unsigned hot_keys; /* share of keys (percent) in the hot set */
};

struct policy {
    const char *name;
    enum fkv_gc_policy gc;
    uint32_t wl;
};

struct result {
    double wa;
    uint32_t emin, emax;
    double emean, even;
    uint32_t gc_runs, wl_moves;
    double life_gb; /* user data writable before any block reaches ENDURANCE */
};

static uint64_t rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static struct fkv_block blocks[BLOCK_COUNT];
static struct fkv_slot slots[SLOTS];
static uint8_t scratch[FKV_SCRATCH_SIZE(MAX_KEY, MAX_VAL, PROG)];

static void die(const char *what, int err)
{
    fprintf(stderr, "%s: %s\n", what, fkv_strerror(err));
    exit(1);
}

static void run(const struct workload *w, const struct policy *p, unsigned fill_pct,
                struct result *r, uint32_t *heat)
{
    struct fkv_sim sim;
    if (fkv_sim_init(&sim, BLOCK_SIZE, BLOCK_COUNT, PROG, 1) != 0)
        die("sim", FKV_ERR_NOMEM);
    struct fkv_config cfg = {
        .flash = &sim.flash,
        .max_key_len = MAX_KEY,
        .max_value_len = MAX_VAL,
        .gc_policy = p->gc,
        .wl_threshold = p->wl,
        .blocks = blocks,
        .slots = slots,
        .slot_count = SLOTS,
        .scratch = scratch,
        .scratch_size = sizeof scratch,
    };
    fkv_t db;
    int err;
    if ((err = fkv_format(&cfg)) || (err = fkv_mount(&db, &cfg)))
        die("mount", err);

    struct fkv_stats st;
    fkv_stat(&db, &st);
    uint32_t rec = FKV_RECORD_SIZE(8, VAL_LEN, PROG);
    unsigned nkeys = (unsigned)((uint64_t)st.capacity_bytes * fill_pct / 100 / rec);
    unsigned nhot = nkeys * w->hot_keys / 100;
    if (nhot == 0)
        nhot = 1;

    char key[16];
    uint8_t val[VAL_LEN];
    uint64_t rng = 12345;
    for (unsigned k = 0; k < nkeys; k++) {
        memset(val, (int)k, sizeof val);
        if ((err = fkv_put(&db, key, (size_t)sprintf(key, "k%07u", k), val, VAL_LEN)))
            die("fill", err);
    }
    fkv_stat(&db, &st);
    uint64_t user0 = st.user_bytes, flash0 = st.flash_bytes;

    for (int i = 0; i < UPDATES; i++) {
        unsigned k;
        if (rng_next(&rng) % 100 < w->hot_pct)
            k = (unsigned)(rng_next(&rng) % nhot);
        else
            k = nhot + (unsigned)(rng_next(&rng) % (nkeys - nhot ? nkeys - nhot : 1));
        k %= nkeys;
        memcpy(val, &i, sizeof i);
        if ((err = fkv_put(&db, key, (size_t)sprintf(key, "k%07u", k), val, VAL_LEN)))
            die("update", err);
    }
    if ((err = fkv_check(&db)))
        die("check", err);
    fkv_stat(&db, &st);

    r->wa = (double)(st.flash_bytes - flash0) / (double)(st.user_bytes - user0);
    r->emin = st.erase_min;
    r->emax = st.erase_max;
    r->emean = (double)st.erase_total / BLOCK_COUNT;
    r->even = r->emean / r->emax;
    r->gc_runs = st.gc_runs;
    r->wl_moves = st.wl_moves;
    r->life_gb = (double)st.user_bytes / st.erase_max * ENDURANCE / 1e9;
    if (heat)
        for (uint32_t b = 0; b < BLOCK_COUNT; b++)
            heat[b] = blocks[b].ec;
    if (sim.violations)
        die("flash misuse", FKV_ERR_IO);
    fkv_sim_free(&sim);
}

/* One character pair per block, shaded on a 0..max scale. */
static void heatmap(const char *title, const uint32_t *ec, uint32_t max)
{
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t b = 0; b < BLOCK_COUNT; b++) {
        lo = ec[b] < lo ? ec[b] : lo;
        hi = ec[b] > hi ? ec[b] : hi;
    }
    printf("\n%s: erase counts %u..%u\n", title, (unsigned)lo, (unsigned)hi);
    static const char shade[] = " .:-=+*#%@";
    for (uint32_t b = 0; b < BLOCK_COUNT; b++) {
        int lvl = (int)lround(9.0 * ec[b] / max);
        putchar(shade[lvl]);
        putchar(shade[lvl]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    static const struct workload W[] = {
        {"uniform", 0, 0},
        {"hot/cold 90/10", 90, 10},
        {"hot/cold 99/1", 99, 1},
        {"static + hot", 100, 2},
    };
    static const struct policy P[] = {
        {"greedy, no static WL", FKV_GC_GREEDY, 0},
        {"greedy + static WL", FKV_GC_GREEDY, 16},
        {"cost-benefit + static WL", FKV_GC_COST_BENEFIT, 16},
    };
    static const unsigned fills[] = {50, 80};

    if (argc > 1 && strcmp(argv[1], "--heatmap") == 0) {
        uint32_t off[BLOCK_COUNT], on[BLOCK_COUNT];
        struct result r;
        run(&W[3], &P[0], 50, &r, off);
        run(&W[3], &P[1], 50, &r, on);
        uint32_t max = 1;
        for (uint32_t b = 0; b < BLOCK_COUNT; b++) {
            max = off[b] > max ? off[b] : max;
            max = on[b] > max ? on[b] : max;
        }
        printf("50%% static data, all writes to 2%% of keys; one cell per block, "
               "shaded 0..%u erases\n",
               (unsigned)max);
        heatmap("without static wear leveling", off, max);
        heatmap("with static wear leveling   ", on, max);
        return 0;
    }

    printf("%u blocks x %u B, %d updates of %u-byte values per run; "
           "lifetime assumes %.0fk erase cycles per block\n\n",
           BLOCK_COUNT, BLOCK_SIZE, UPDATES, VAL_LEN, ENDURANCE / 1000);
    printf("| workload | fill | policy | WA | erase min / mean / max | evenness | lifetime | "
           "GC runs | WL moves |\n");
    printf("|---|---|---|---|---|---|---|---|---|\n");
    for (size_t f = 0; f < sizeof fills / sizeof fills[0]; f++) {
        for (size_t w = 0; w < sizeof W / sizeof W[0]; w++) {
            for (size_t p = 0; p < sizeof P / sizeof P[0]; p++) {
                struct result r;
                run(&W[w], &P[p], fills[f], &r, NULL);
                printf("| %s | %u%% | %s | %.2f | %u / %.0f / %u | %.0f%% | %.1f GB | %u | %u |\n",
                       W[w].name, fills[f], P[p].name, r.wa, (unsigned)r.emin, r.emean,
                       (unsigned)r.emax, 100.0 * r.even, r.life_gb, (unsigned)r.gc_runs,
                       (unsigned)r.wl_moves);
                fflush(stdout);
            }
        }
    }
    return 0;
}
