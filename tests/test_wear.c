/* Wear leveling, space accounting and corruption handling. */
#include "test.h"

#include <string.h>

/* ---- wear leveling ------------------------------------------------------ */

/* A mostly-static dataset plus a few very hot keys: the classic case where
 * dynamic wear leveling alone leaves the cold blocks unworn. */
static void hot_cold(uint32_t wl_threshold, enum fkv_gc_policy policy, struct fkv_stats *st)
{
    struct geom g = {1024, 32, 4, 16, 100, 512, policy, wl_threshold};
    struct env e;
    env_init(&e, &g, 42);
    char key[32];
    uint8_t val[100];
    uint64_t rng = 7;

    for (unsigned k = 0; k < 150; k++) {
        value_bytes(k, 100, val);
        REQUIRE_OK(fkv_put(&e.db, key, key_name(k, key), val, 100));
    }
    for (int i = 0; i < 60000; i++) {
        value_bytes((uint32_t)i, 100, val);
        unsigned k = 200 + (unsigned)(rng_next(&rng) % 4);
        REQUIRE_OK(fkv_put(&e.db, key, key_name(k, key), val, 100));
    }
    REQUIRE_OK(fkv_check(&e.db));
    REQUIRE_OK(fkv_stat(&e.db, st));
    env_remount(&e);
    for (unsigned k = 0; k < 150; k++) {
        uint8_t got[100];
        size_t n;
        value_bytes(k, 100, val);
        REQUIRE_OK(fkv_get(&e.db, key, key_name(k, key), got, sizeof got, &n));
        REQUIRE(n == 100 && memcmp(got, val, 100) == 0);
    }
    env_free(&e);
}

void test_static_wear_leveling(void)
{
    for (int policy = 0; policy < 2; policy++) {
        struct fkv_stats off, on;
        hot_cold(0, (enum fkv_gc_policy)policy, &off);
        hot_cold(8, (enum fkv_gc_policy)policy, &on);
        printf("      %s: erase spread without static WL %u..%u, with WL(8) %u..%u\n",
               policy ? "cost-benefit" : "greedy", off.erase_min, off.erase_max, on.erase_min,
               on.erase_max);
        /* Without static WL the cold blocks are never rewritten. */
        REQUIRE(off.erase_max - off.erase_min > 50);
        /* With it, every block stays within a small band. */
        REQUIRE(on.erase_max - on.erase_min <= 8 + 2);
    }
}

void test_dynamic_wear_leveling(void)
{
    /* With no static data at all, least-worn-first allocation alone must
     * keep erase counts tight. */
    struct geom g = {1024, 16, 4, 16, 100, 128, FKV_GC_GREEDY, 0};
    struct env e;
    env_init(&e, &g, 43);
    char key[32];
    uint8_t val[100] = {0};
    for (int i = 0; i < 30000; i++) {
        val[0] = (uint8_t)i;
        REQUIRE_OK(fkv_put(&e.db, key, key_name((unsigned)i % 3, key), val, 100));
    }
    struct fkv_stats st;
    REQUIRE_OK(fkv_stat(&e.db, &st));
    REQUIRE(st.erase_max - st.erase_min <= 2);
    env_free(&e);
}

/* ---- space accounting --------------------------------------------------- */

static void fill_to_capacity(const struct geom *g)
{
    struct env e;
    env_init(&e, g, 44);
    char key[32];
    uint8_t val[512];
    memset(val, 0x5A, sizeof val);

    unsigned n = 0;
    int rc;
    struct fkv_stats st;
    for (;;) {
        rc = fkv_put(&e.db, key, key_name(n, key), val, g->max_val);
        if (rc != FKV_OK)
            break;
        n++;
    }
    REQUIRE_RC(rc, FKV_ERR_NOSPC);
    REQUIRE_OK(fkv_stat(&e.db, &st));
    /* NOSPC is only reported when the capacity bound is actually reached. */
    uint32_t rec = FKV_RECORD_SIZE(key_name(n, key), g->max_val, g->prog_size);
    REQUIRE(st.live_bytes + rec > st.capacity_bytes);

    /* A full store must keep accepting overwrites indefinitely: GC can never
     * deadlock. */
    uint64_t rng = 1;
    for (int i = 0; i < 5000; i++) {
        val[0] = (uint8_t)i;
        REQUIRE_OK(fkv_put(&e.db, key, key_name((unsigned)(rng_next(&rng) % n), key), val,
                           g->max_val));
    }
    REQUIRE_OK(fkv_check(&e.db));
    env_remount(&e);

    /* Deleting everything frees the space again. */
    for (unsigned k = 0; k < n; k++)
        REQUIRE_OK(fkv_del(&e.db, key, key_name(k, key)));
    for (unsigned k = 0; k < n; k++)
        REQUIRE_OK(fkv_put(&e.db, key, key_name(k + 1000, key), val, g->max_val));
    REQUIRE_OK(fkv_check(&e.db));
    env_free(&e);
}

void test_fill_to_capacity(void)
{
    struct geom g = GEOM_SMALL;
    for (int policy = 0; policy < 2; policy++) {
        for (uint32_t wl = 0; wl <= 4; wl += 4) {
            g = GEOM_SMALL;
            g.policy = (enum fkv_gc_policy)policy;
            g.wl_threshold = wl;
            fill_to_capacity(&g);
            g.block_count = 2;
            fill_to_capacity(&g);
            g = (struct geom){4096, 16, 8, 16, 400, 512, (enum fkv_gc_policy)policy, wl};
            fill_to_capacity(&g);
        }
    }
}

void test_index_full(void)
{
    struct geom g = GEOM_SMALL;
    g.slots = 16; /* room for 12 keys */
    struct env e;
    env_init(&e, &g, 45);
    char key[32];
    for (unsigned k = 0; k < 12; k++)
        REQUIRE_OK(fkv_put(&e.db, key, key_name(k, key), "v", 1));
    REQUIRE_RC(fkv_put(&e.db, key, key_name(12, key), "v", 1), FKV_ERR_NOMEM);
    REQUIRE_OK(fkv_put(&e.db, key, key_name(3, key), "w", 1)); /* overwrite still fine */
    REQUIRE_OK(fkv_check(&e.db));
    env_free(&e);
}

/* ---- corruption --------------------------------------------------------- */

static size_t mem_find(const struct fkv_sim *s, const char *needle)
{
    size_t n = strlen(needle), total = (size_t)s->block_size * s->block_count;
    for (size_t i = 0; i + n <= total; i++)
        if (memcmp(s->mem + i, needle, n) == 0)
            return i;
    REQUIRE(false);
    return 0;
}

void test_bitrot_in_value(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 46);
    REQUIRE_OK(fkv_put(&e.db, "before", 6, "1111", 4));
    REQUIRE_OK(fkv_put(&e.db, "victim", 6, "2222", 4));
    REQUIRE_OK(fkv_put(&e.db, "after", 5, "3333", 4));

    size_t at = mem_find(&e.sim, "victim") + 6; /* first value byte */
    fkv_sim_flip_bit(&e.sim, (uint32_t)(at / e.sim.block_size),
                     (uint32_t)(at % e.sim.block_size), 3);

    uint8_t buf[16];
    size_t n;
    REQUIRE_RC(fkv_get(&e.db, "victim", 6, buf, sizeof buf, &n), FKV_ERR_CORRUPT);

    /* On remount the damaged record is skipped using its intact header, so
     * records after it in the same block survive. */
    env_remount(&e);
    struct fkv_stats st;
    REQUIRE_OK(fkv_stat(&e.db, &st));
    REQUIRE(st.corrupt_records == 1);
    REQUIRE_RC(fkv_get(&e.db, "victim", 6, buf, sizeof buf, &n), FKV_ERR_NOTFOUND);
    REQUIRE_OK(fkv_get(&e.db, "before", 6, buf, sizeof buf, &n));
    REQUIRE_OK(fkv_get(&e.db, "after", 5, buf, sizeof buf, &n));
    REQUIRE(n == 4 && memcmp(buf, "3333", 4) == 0);
    REQUIRE_OK(fkv_put(&e.db, "victim", 6, "fixed", 5));
    env_free(&e);
}

void test_bitrot_falls_back_to_older_version(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 47);
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "old-value", 9));
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "new-value", 9));
    size_t at = mem_find(&e.sim, "new-value");
    fkv_sim_flip_bit(&e.sim, (uint32_t)(at / e.sim.block_size),
                     (uint32_t)(at % e.sim.block_size), 0);
    env_remount(&e);
    uint8_t buf[16];
    size_t n;
    REQUIRE_OK(fkv_get(&e.db, "k", 1, buf, sizeof buf, &n));
    REQUIRE(n == 9 && memcmp(buf, "old-value", 9) == 0);
    env_free(&e);
}

void test_bitrot_in_header(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 48);
    REQUIRE_OK(fkv_put(&e.db, "first", 5, "1", 1));
    REQUIRE_OK(fkv_put(&e.db, "second", 6, "2", 1));
    /* Damage the length field of "second": nothing after it can be located
     * any more, but the store must still mount and stay writable. */
    size_t at = mem_find(&e.sim, "second") - FKV_RECORD_HEADER_SIZE + 2;
    fkv_sim_flip_bit(&e.sim, (uint32_t)(at / e.sim.block_size),
                     (uint32_t)(at % e.sim.block_size), 5);
    env_remount(&e);
    uint8_t buf[16];
    size_t n;
    REQUIRE_OK(fkv_get(&e.db, "first", 5, buf, sizeof buf, &n));
    REQUIRE_RC(fkv_get(&e.db, "second", 6, buf, sizeof buf, &n), FKV_ERR_NOTFOUND);
    for (unsigned i = 0; i < 200; i++) {
        char key[32];
        REQUIRE_OK(fkv_put(&e.db, key, key_name(i % 10, key), "v", 1));
    }
    env_remount(&e);
    env_free(&e);
}

void test_damaged_block_header(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 49);
    /* Corrupt header A of every block that is not in use: they become
     * "dirty" and must be erased before reuse, with no effect on data. */
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "v", 1));
    for (uint32_t b = 0; b < e.sim.block_count; b++)
        if (e.cfg.blocks[b].state != 2)
            fkv_sim_flip_bit(&e.sim, b, 9, 1);
    env_remount(&e);
    char key[32];
    for (unsigned i = 0; i < 500; i++)
        REQUIRE_OK(fkv_put(&e.db, key, key_name(i % 20, key), "value", 5));
    env_remount(&e);
    uint8_t buf[8];
    size_t n;
    REQUIRE_OK(fkv_get(&e.db, "k", 1, buf, sizeof buf, &n));
    env_free(&e);
}
