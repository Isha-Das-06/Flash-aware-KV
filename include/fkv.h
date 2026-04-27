/*
 * fkv - a flash-aware, power-loss-safe key-value store.
 *
 * fkv stores key/value records in a log on raw NOR-style flash:
 *
 *   - every update is a single self-validating record (header CRC + data
 *     CRC), so an update is atomic: after a power cut it either happened
 *     completely or not at all;
 *   - blocks are reclaimed by copy-compaction into a fresh block that is
 *     only "committed" after the copy is complete, so garbage collection
 *     never loses data even if power is cut halfway through;
 *   - writes are spread across the device with dynamic wear leveling
 *     (least-worn free block first) and static wear leveling (cold data is
 *     evicted from under-worn blocks once the erase-count spread exceeds a
 *     threshold);
 *   - no dynamic memory allocation: the caller provides every buffer.
 *
 * See docs/DESIGN.md for the on-flash format and the crash-safety argument.
 */
#ifndef FKV_H
#define FKV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FKV_VERSION "1.0.0"

/* Return codes. Every public function returns FKV_OK (0) or a negative code. */
enum {
    FKV_OK = 0,
    FKV_ERR_IO = -1,       /* flash driver error (incl. power loss); remount required */
    FKV_ERR_CORRUPT = -2,  /* stored data failed its CRC */
    FKV_ERR_NOSPC = -3,    /* store is full */
    FKV_ERR_NOTFOUND = -4, /* key does not exist */
    FKV_ERR_INVAL = -5,    /* invalid argument or configuration */
    FKV_ERR_NOMEM = -6,    /* key index is full (raise slot_count) */
    FKV_ERR_NOFS = -7,     /* no fkv store on this flash; run fkv_format() */
    FKV_ERR_TOOBIG = -8,   /* caller's value buffer is too small */
};

/* Fixed on-flash sizes. */
#define FKV_BLOCK_HEADER_SIZE 48u
#define FKV_RECORD_HEADER_SIZE 12u
#define FKV_MAX_KEY_LEN 255u

#define FKV_ALIGN_UP(x, a) ((((x) + (a) - 1u) / (a)) * (a))

/* On-flash size of one record, including header and padding. */
#define FKV_RECORD_SIZE(klen, vlen, prog_size) \
    FKV_ALIGN_UP(FKV_RECORD_HEADER_SIZE + (uint32_t)(klen) + (uint32_t)(vlen), (uint32_t)(prog_size))

/* Minimum size of the scratch buffer for a given configuration. */
#define FKV_SCRATCH_SIZE(max_key_len, max_value_len, prog_size) \
    FKV_RECORD_SIZE(max_key_len, max_value_len, prog_size)

/*
 * Flash driver. The device is `block_count` erase blocks of `block_size`
 * bytes. Erased bytes read as 0xFF; programming can only clear bits.
 * `prog` is always called with `off` and `len` aligned to `prog_size`
 * (1, 2, 4, 8 or 16). Callbacks return 0 on success, non-zero on failure.
 * fkv never programs a byte that is not in the erased state.
 */
struct fkv_flash {
    void *ctx;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t prog_size;
    int (*read)(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t len);
    int (*prog)(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t block);
};

/* How garbage collection chooses the block to reclaim. */
enum fkv_gc_policy {
    FKV_GC_GREEDY = 0,       /* most reclaimable bytes */
    FKV_GC_COST_BENEFIT = 1, /* LFS cost-benefit: (1-u) * age / (1+u) */
};

/* Per-block RAM state. Contents are private; the caller only allocates it. */
struct fkv_block {
    uint32_t seq;  /* commit sequence number (ACTIVE blocks) */
    uint32_t ec;   /* erase count */
    uint32_t end;  /* offset just past the last parsed record */
    uint32_t live; /* bytes of records that are still current */
    uint8_t state;
    uint8_t sealed;   /* ACTIVE block that must not be appended to */
    uint8_t verified; /* FREE block known to be fully erased */
    uint8_t ec_known;
};

/* One slot of the in-RAM key index (open addressing). Private. */
struct fkv_slot {
    uint32_t hash;
    uint32_t off;
    uint32_t len;
    uint16_t block;
    uint8_t klen;
    uint8_t flags;
};

struct fkv_config {
    const struct fkv_flash *flash;

    uint32_t max_key_len;   /* 1..255 */
    uint32_t max_value_len; /* largest value fkv_put() accepts */

    enum fkv_gc_policy gc_policy;
    /* Static wear leveling kicks in when (max erase count - erase count of
     * the least-worn block holding data) exceeds this. 0 disables it. */
    uint32_t wl_threshold;

    /* Caller-provided memory. */
    struct fkv_block *blocks; /* flash->block_count entries */
    struct fkv_slot *slots;   /* slot_count entries */
    uint32_t slot_count;      /* power of two; up to 3/4 of slots hold keys */
    uint8_t *scratch;         /* >= FKV_SCRATCH_SIZE(max_key_len, max_value_len, prog_size) */
    uint32_t scratch_size;
};

struct fkv_stats {
    uint32_t keys;           /* live keys */
    uint32_t tombstones;     /* deleted keys whose tombstone is still on flash */
    uint32_t live_bytes;     /* on-flash bytes of current records */
    uint32_t capacity_bytes; /* live_bytes can grow up to this */
    uint32_t block_count;
    uint32_t free_blocks;
    uint32_t erase_min; /* erase-count distribution across all blocks */
    uint32_t erase_max;
    uint64_t erase_total;
    /* Counters since mount. */
    uint64_t user_bytes;  /* record bytes requested by put/del */
    uint64_t flash_bytes; /* all bytes programmed (records, GC copies, headers) */
    uint32_t erases;
    uint32_t gc_runs;
    uint32_t wl_moves;
    uint32_t corrupt_records;  /* records dropped because they failed their CRC */
    uint32_t recovered_blocks; /* interrupted compactions completed at mount */
};

/* Store handle. All fields are private. */
typedef struct fkv {
    struct fkv_config cfg;
    const struct fkv_flash *flash;
    uint32_t payload;  /* data bytes per block */
    uint32_t max_rec;  /* largest record size */
    uint32_t capacity; /* live-byte limit that guarantees GC always progresses */
    uint32_t live_bytes;
    uint32_t slot_mask;
    uint32_t slot_used;
    uint32_t slot_limit;
    uint32_t head;
    uint32_t next_seq;
    uint32_t nfree;
    bool mounted;
    bool broken;
    struct fkv_stats st;
} fkv_t;

typedef int (*fkv_foreach_cb)(const void *key, size_t klen, const void *val, size_t vlen,
                              void *ctx);

/* Erase the device and write empty block headers. Existing erase counts are
 * preserved. Only cfg->flash is used. Not power-safe: rerun if interrupted. */
int fkv_format(const struct fkv_config *cfg);

/* Scan the flash and rebuild the in-RAM index. Completes any compaction that
 * was interrupted by a power cut. */
int fkv_mount(fkv_t *db, const struct fkv_config *cfg);
int fkv_unmount(fkv_t *db);

/* Insert or replace. Durable when it returns FKV_OK. Writing the value a key
 * already has is a no-op and costs no flash wear. */
int fkv_put(fkv_t *db, const void *key, size_t klen, const void *val, size_t vlen);

/* Copy the value into buf. *vlen (optional) receives the value length. With
 * buf == NULL only the length is reported. */
int fkv_get(fkv_t *db, const void *key, size_t klen, void *buf, size_t bufsize, size_t *vlen);

int fkv_del(fkv_t *db, const void *key, size_t klen);

/* Visit every live key in unspecified order. A non-zero return from cb stops
 * the walk and is returned. The store must not be modified from cb. */
int fkv_foreach(fkv_t *db, fkv_foreach_cb cb, void *ctx);

int fkv_stat(const fkv_t *db, struct fkv_stats *st);

/* Consistency check: re-verifies every indexed record and the space
 * accounting against flash. Returns FKV_OK or FKV_ERR_CORRUPT. */
int fkv_check(fkv_t *db);

const char *fkv_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* FKV_H */
