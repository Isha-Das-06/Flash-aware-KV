/*
 * Minimal fkv usage: a boot counter plus a settings key, on a simulated
 * 64 KiB NOR flash. On a microcontroller, replace the simulator with a
 * struct fkv_flash wired to your flash driver; everything else is the same.
 */
#include "fkv.h"
#include "fkv_sim.h"

#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 4096
#define BLOCK_COUNT 16
#define PROG_SIZE 4
#define MAX_KEY 32
#define MAX_VALUE 256

/* All memory is static: fkv never calls malloc. */
static struct fkv_block blocks[BLOCK_COUNT];
static struct fkv_slot slots[64];
static uint8_t scratch[FKV_SCRATCH_SIZE(MAX_KEY, MAX_VALUE, PROG_SIZE)];

static int print_entry(const void *key, size_t klen, const void *val, size_t vlen, void *ctx)
{
    (void)ctx;
    if (vlen == sizeof(uint32_t) && klen == 10 && memcmp(key, "boot_count", 10) == 0) {
        uint32_t v;
        memcpy(&v, val, sizeof v);
        printf("  boot_count = %u\n", (unsigned)v);
    } else {
        printf("  %.*s = %.*s\n", (int)klen, (const char *)key, (int)vlen, (const char *)val);
    }
    return 0;
}

static int boot(struct fkv_sim *sim, const struct fkv_config *cfg)
{
    fkv_t db;
    int err = fkv_mount(&db, cfg);
    if (err == FKV_ERR_NOFS) {
        printf("no store found, formatting\n");
        if ((err = fkv_format(cfg)) != FKV_OK || (err = fkv_mount(&db, cfg)) != FKV_OK)
            return err;
    } else if (err) {
        return err;
    }

    uint32_t boots = 0;
    size_t n;
    if (fkv_get(&db, "boot_count", 10, &boots, sizeof boots, &n) == FKV_OK)
        printf("boot #%u\n", (unsigned)(boots + 1));
    boots++;
    if ((err = fkv_put(&db, "boot_count", 10, &boots, sizeof boots)) != FKV_OK)
        return err;
    if ((err = fkv_put(&db, "wifi.ssid", 9, "workshop", 8)) != FKV_OK)
        return err;

    struct fkv_stats st;
    fkv_stat(&db, &st);
    printf("%u keys, %u/%u bytes used, %u/%u blocks free, erase counts %u..%u, "
           "%llu flash ops so far\n",
           (unsigned)st.keys, (unsigned)st.live_bytes, (unsigned)st.capacity_bytes,
           (unsigned)st.free_blocks, (unsigned)st.block_count, (unsigned)st.erase_min,
           (unsigned)st.erase_max, (unsigned long long)sim->ops);
    return fkv_unmount(&db);
}

int main(void)
{
    struct fkv_sim sim;
    if (fkv_sim_init(&sim, BLOCK_SIZE, BLOCK_COUNT, PROG_SIZE, 1) != 0)
        return 1;

    struct fkv_config cfg = {
        .flash = &sim.flash,
        .max_key_len = MAX_KEY,
        .max_value_len = MAX_VALUE,
        .gc_policy = FKV_GC_COST_BENEFIT,
        .wl_threshold = 16,
        .blocks = blocks,
        .slots = slots,
        .slot_count = 64,
        .scratch = scratch,
        .scratch_size = sizeof scratch,
    };

    for (int i = 0; i < 3; i++) {
        int err = boot(&sim, &cfg);
        if (err) {
            fprintf(stderr, "error: %s\n", fkv_strerror(err));
            return 1;
        }
    }

    /* Pull the plug in the middle of a write, then reboot. */
    printf("\ncutting power during the next flash operation...\n");
    fkv_sim_cut_after(&sim, 0);
    fkv_t db;
    fkv_mount(&db, &cfg);
    int err = fkv_put(&db, "wifi.ssid", 9, "a-much-longer-network-name", 26);
    printf("put returned: %s\n", fkv_strerror(err));
    fkv_sim_power_on(&sim);

    if (boot(&sim, &cfg) != FKV_OK)
        return 1;
    fkv_mount(&db, &cfg);
    printf("contents after recovery (old value intact, or the new one - never a mix):\n");
    fkv_foreach(&db, print_entry, NULL);
    fkv_sim_free(&sim);
    return 0;
}
