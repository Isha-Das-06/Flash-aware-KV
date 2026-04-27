/* API behaviour and edge cases. */
#include "test.h"

#include <string.h>

void test_sim_semantics(void)
{
    struct fkv_sim s;
    REQUIRE(fkv_sim_init(&s, 256, 4, 4, 1) == 0);
    uint8_t buf[8], a[4] = {0xF0, 0x0F, 0xFF, 0x00};

    REQUIRE(s.flash.read(s.flash.ctx, 0, 0, buf, 8) == 0);
    for (int i = 0; i < 8; i++)
        REQUIRE(buf[i] == 0xFF);

    REQUIRE(s.flash.prog(s.flash.ctx, 0, 0, a, 4) == 0);
    REQUIRE(s.flash.read(s.flash.ctx, 0, 0, buf, 4) == 0);
    REQUIRE(memcmp(buf, a, 4) == 0);
    REQUIRE(s.violations == 0);

    /* Programming over programmed bytes is a violation. */
    REQUIRE(s.flash.prog(s.flash.ctx, 0, 0, a, 4) == 0);
    REQUIRE(s.violations > 0);
    s.violations = 0;

    /* Misaligned program is rejected. */
    REQUIRE(s.flash.prog(s.flash.ctx, 0, 2, a, 4) != 0);
    REQUIRE(s.violations == 1);
    s.violations = 0;

    /* Power cut: the interrupted op and everything after fail. */
    fkv_sim_cut_after(&s, 1);
    REQUIRE(s.flash.erase(s.flash.ctx, 1) == 0);
    REQUIRE(s.flash.erase(s.flash.ctx, 0) != 0);
    REQUIRE(s.flash.read(s.flash.ctx, 0, 0, buf, 4) != 0);
    fkv_sim_power_on(&s);
    REQUIRE(s.flash.read(s.flash.ctx, 0, 0, buf, 4) == 0);
    fkv_sim_free(&s);
}

void test_basic_put_get(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 1);
    uint8_t buf[64];
    size_t n;

    REQUIRE_OK(fkv_put(&e.db, "alpha", 5, "one", 3));
    REQUIRE_OK(fkv_put(&e.db, "beta", 4, "two", 3));
    REQUIRE_OK(fkv_get(&e.db, "alpha", 5, buf, sizeof buf, &n));
    REQUIRE(n == 3 && memcmp(buf, "one", 3) == 0);

    REQUIRE_OK(fkv_put(&e.db, "alpha", 5, "uno!", 4));
    REQUIRE_OK(fkv_get(&e.db, "alpha", 5, buf, sizeof buf, &n));
    REQUIRE(n == 4 && memcmp(buf, "uno!", 4) == 0);

    REQUIRE_OK(fkv_del(&e.db, "beta", 4));
    REQUIRE_RC(fkv_get(&e.db, "beta", 4, buf, sizeof buf, &n), FKV_ERR_NOTFOUND);
    REQUIRE_RC(fkv_del(&e.db, "beta", 4), FKV_ERR_NOTFOUND);
    REQUIRE_RC(fkv_get(&e.db, "gamma", 5, buf, sizeof buf, &n), FKV_ERR_NOTFOUND);

    /* Re-create after delete. */
    REQUIRE_OK(fkv_put(&e.db, "beta", 4, "again", 5));
    REQUIRE_OK(fkv_check(&e.db));

    env_remount(&e);
    REQUIRE_OK(fkv_get(&e.db, "alpha", 5, buf, sizeof buf, &n));
    REQUIRE(n == 4 && memcmp(buf, "uno!", 4) == 0);
    REQUIRE_OK(fkv_get(&e.db, "beta", 4, buf, sizeof buf, &n));
    REQUIRE(n == 5 && memcmp(buf, "again", 5) == 0);
    env_free(&e);
}

void test_edge_values(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 2);
    uint8_t buf[64], big[64];
    size_t n = 99;

    /* Empty value is distinct from absent. */
    REQUIRE_OK(fkv_put(&e.db, "empty", 5, NULL, 0));
    REQUIRE_OK(fkv_get(&e.db, "empty", 5, buf, sizeof buf, &n));
    REQUIRE(n == 0);

    /* Binary keys, including NUL bytes. */
    const uint8_t k1[3] = {0, 1, 0}, k2[3] = {0, 1, 1};
    REQUIRE_OK(fkv_put(&e.db, k1, 3, "x", 1));
    REQUIRE_OK(fkv_put(&e.db, k2, 3, "y", 1));
    REQUIRE_OK(fkv_get(&e.db, k1, 3, buf, sizeof buf, &n));
    REQUIRE(n == 1 && buf[0] == 'x');

    /* Size limits. */
    memset(big, 0xAB, sizeof big);
    REQUIRE_OK(fkv_put(&e.db, "big", 3, big, 64));
    REQUIRE_RC(fkv_put(&e.db, "big", 3, big, 65), FKV_ERR_INVAL);
    REQUIRE_OK(fkv_put(&e.db, "0123456789abcdef", 16, "k", 1));
    REQUIRE_RC(fkv_put(&e.db, "0123456789abcdefg", 17, "k", 1), FKV_ERR_INVAL);
    REQUIRE_RC(fkv_put(&e.db, "", 0, "k", 1), FKV_ERR_INVAL);

    /* Small buffer, and length-only query. */
    REQUIRE_RC(fkv_get(&e.db, "big", 3, buf, 10, &n), FKV_ERR_TOOBIG);
    REQUIRE(n == 64);
    n = 0;
    REQUIRE_OK(fkv_get(&e.db, "big", 3, NULL, 0, &n));
    REQUIRE(n == 64);

    env_remount(&e);
    REQUIRE_OK(fkv_get(&e.db, "big", 3, buf, sizeof buf, &n));
    REQUIRE(n == 64 && memcmp(buf, big, 64) == 0);
    env_free(&e);
}

void test_identical_put_is_free(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 3);
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "value", 5));
    uint64_t before = e.sim.bytes_programmed;
    for (int i = 0; i < 100; i++)
        REQUIRE_OK(fkv_put(&e.db, "k", 1, "value", 5));
    REQUIRE(e.sim.bytes_programmed == before);
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "valuE", 5));
    REQUIRE(e.sim.bytes_programmed > before);
    env_free(&e);
}

struct walk {
    int n;
    int stop_after;
};

static int walk_cb(const void *k, size_t kl, const void *v, size_t vl, void *ctx)
{
    struct walk *w = ctx;
    (void)k, (void)v;
    REQUIRE(kl == 7 && vl == 2);
    return ++w->n == w->stop_after ? 42 : 0;
}

void test_foreach(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 4);
    char key[32];
    for (unsigned i = 0; i < 20; i++)
        REQUIRE_OK(fkv_put(&e.db, key, key_name(i, key), "vv", 2));
    for (unsigned i = 0; i < 20; i += 2)
        REQUIRE_OK(fkv_del(&e.db, key, key_name(i, key)));

    struct walk w = {0, 0};
    REQUIRE_OK(fkv_foreach(&e.db, walk_cb, &w));
    REQUIRE(w.n == 10);

    w = (struct walk){0, 3};
    REQUIRE_RC(fkv_foreach(&e.db, walk_cb, &w), 42);
    REQUIRE(w.n == 3);
    env_free(&e);
}

void test_config_validation(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 5);
    fkv_t db;
    struct fkv_config c = e.cfg;

    c.slot_count = 100; /* not a power of two */
    REQUIRE_RC(fkv_mount(&db, &c), FKV_ERR_INVAL);
    c = e.cfg;
    c.scratch_size = 10;
    REQUIRE_RC(fkv_mount(&db, &c), FKV_ERR_INVAL);
    c = e.cfg;
    c.max_value_len = 400; /* two records would not fit in a block */
    c.scratch_size = 1024;
    REQUIRE_RC(fkv_mount(&db, &c), FKV_ERR_INVAL);
    c = e.cfg;
    c.max_key_len = 0;
    REQUIRE_RC(fkv_mount(&db, &c), FKV_ERR_INVAL);
    REQUIRE_RC(fkv_mount(&db, NULL), FKV_ERR_INVAL);

    /* Unmounted handle. */
    REQUIRE_OK(fkv_unmount(&e.db));
    REQUIRE_RC(fkv_put(&e.db, "k", 1, "v", 1), FKV_ERR_INVAL);
    env_free(&e);
}

void test_unformatted(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 6);
    memset(e.sim.mem, 0xFF, (size_t)e.sim.block_size * e.sim.block_count);
    REQUIRE_RC(fkv_mount(&e.db, &e.cfg), FKV_ERR_NOFS);
    for (size_t i = 0; i < (size_t)e.sim.block_size * e.sim.block_count; i++)
        e.sim.mem[i] = (uint8_t)(i * 7u);
    REQUIRE_RC(fkv_mount(&e.db, &e.cfg), FKV_ERR_NOFS);
    REQUIRE_OK(fkv_format(&e.cfg));
    REQUIRE_OK(fkv_mount(&e.db, &e.cfg));
    REQUIRE_OK(fkv_put(&e.db, "k", 1, "v", 1));
    env_free(&e);
}

void test_format_keeps_wear(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 7);
    char key[32];
    uint8_t val[64] = {0};
    for (int i = 0; i < 2000; i++) {
        val[0] = (uint8_t)i;
        REQUIRE_OK(fkv_put(&e.db, key, key_name((unsigned)i % 5, key), val, 60));
    }
    struct fkv_stats a, b;
    REQUIRE_OK(fkv_stat(&e.db, &a));
    REQUIRE(a.erase_max > 10);

    REQUIRE_OK(fkv_format(&e.cfg));
    REQUIRE_OK(fkv_mount(&e.db, &e.cfg));
    REQUIRE_OK(fkv_stat(&e.db, &b));
    REQUIRE(b.keys == 0);
    REQUIRE(b.erase_min == a.erase_min + 1 && b.erase_max == a.erase_max + 1);
    /* The simulator's own counters agree with what fkv persisted. */
    for (uint32_t i = 0; i < e.sim.block_count; i++)
        REQUIRE(e.cfg.blocks[i].ec == e.sim.erase_count[i]);
    env_free(&e);
}

void test_tombstones_reclaimed(void)
{
    struct env e;
    env_init(&e, &GEOM_SMALL, 8);
    char key[32];
    /* Create and delete many distinct keys; tombstones must not pile up
     * and exhaust the index. */
    for (unsigned round = 0; round < 50; round++) {
        for (unsigned i = 0; i < 20; i++)
            REQUIRE_OK(fkv_put(&e.db, key, key_name(round * 20 + i, key), "v", 1));
        for (unsigned i = 0; i < 20; i++)
            REQUIRE_OK(fkv_del(&e.db, key, key_name(round * 20 + i, key)));
    }
    struct fkv_stats st;
    REQUIRE_OK(fkv_stat(&e.db, &st));
    REQUIRE(st.keys == 0);
    REQUIRE(st.tombstones < 96);
    REQUIRE_OK(fkv_check(&e.db));
    env_remount(&e);
    REQUIRE_OK(fkv_stat(&e.db, &st));
    REQUIRE(st.keys == 0);
    env_free(&e);
}

void test_crc_vector(void)
{
    /* The record header CRC is standard CRC-32: check against the "123456789"
     * test vector by building a record and reading it back raw. */
    struct env e;
    env_init(&e, &GEOM_SMALL, 9);
    REQUIRE_OK(fkv_put(&e.db, "1", 1, "23456789", 8));
    const uint8_t *rec = e.sim.mem + FKV_BLOCK_HEADER_SIZE;
    for (uint32_t b = 0; b < e.sim.block_count; b++) {
        const uint8_t *r = e.sim.mem + (size_t)b * e.sim.block_size + FKV_BLOCK_HEADER_SIZE;
        if (r[0] == 0x50)
            rec = r;
    }
    uint32_t dcrc = (uint32_t)rec[4] | (uint32_t)rec[5] << 8 | (uint32_t)rec[6] << 16 |
                    (uint32_t)rec[7] << 24;
    REQUIRE(dcrc == 0xCBF43926u);
    env_free(&e);
}
