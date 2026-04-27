# fkv — a flash-aware, power-loss-safe key-value store

fkv is a small key-value store for raw NOR flash on microcontrollers, in the
spirit of [littlefs](https://github.com/littlefs-project/littlefs) but
narrower in scope. It is written in dependency-free C99 and does no dynamic
allocation. It survives having the power cut in the middle of any write, and
it spreads erases evenly so the flash doesn't wear out early.

The whole store runs on the host against a simulated flash chip, so the
test suite can cut the power **at every single flash operation** of a
workload and check that nothing committed is ever lost.

```c
fkv_put(&db, "wifi.ssid", 9, "workshop", 8);          // durable when it returns
fkv_get(&db, "wifi.ssid", 9, buf, sizeof buf, &len);
fkv_del(&db, "wifi.ssid", 9);
```

## Highlights

- **Crash safe by construction.** Every update is one self-validating record
  (header CRC + data CRC), so it is atomic. Garbage collection copies into a
  fresh block that is committed only after the copy completes. An
  interrupted compaction is finished at the next mount.
- **Exhaustively tested against power loss.** 18,000+ injected power cuts,
  including cuts during recovery, across GC policies, flash geometries
  (1–16 byte program units, 2–16 blocks), and delete-heavy and skewed
  workloads. After every cut the store equals the state just before or just
  after the interrupted operation, never a mix.
- **The tests are themselves tested.** A mutation-testing script plants 12
  classic crash-safety bugs, such as "erase before commit", "commit before
  copy", and "trust torn data". The suite catches all 12.
- **Wear leveling that measurably matters.** Least-worn-first allocation plus
  static wear leveling that evicts cold data from under-worn blocks. On a
  workload with static data, the most-worn block sees **866 → 213
  erases**: the device lasts **4× longer** for 8% extra write amplification.
- **Provably no GC deadlock.** A capacity bound with a short pigeonhole proof
  ([DESIGN.md §4](docs/DESIGN.md#4-space-why-gc-can-never-deadlock))
  guarantees one compaction always frees enough space. The tests check that
  a 100%-full store keeps accepting overwrites.
- **Embedded-friendly.** No malloc, and the caller owns all memory: about
  1.8 KB of RAM for a 64 KiB store with a 64-slot key index. Validation streams through
  a 32-byte buffer. Code is about 9 KB compiled for x86-64 with `-Os`, and
  there is a Makefile target for Cortex-M4 size.

## How it works (short version)

```
block:  [ A: magic, erase count ][ B: seq, src_seq ][ C: retired flag ][ record | record | ... | 0xFF ]
record: [ type | klen | vlen | data crc | hdr crc ][ key ][ value ][ pad ]
```

- **Log-structured.** Updates append a record to the newest block. On mount,
  blocks are replayed in commit order and the newest record for each key wins.
- **Torn writes.** A record whose data CRC fails but whose header is intact is
  skipped by its length. A torn header ends the block's log, and that block
  is never appended to again.
- **Compaction.** Copy the live records of a victim block into a fresh block,
  then commit it by writing header B, which records the victim's sequence
  number. After that, retire and erase the victim. If power is cut between
  commit and retire, mount sees the `src_seq` link and finishes the retire.
- **Tombstones.** A tombstone is dropped during compaction only when no older
  block could still hold the key; otherwise the deleted value would come
  back.
- **Wear.** Blocks are allocated least-worn first. When the erase-count
  spread exceeds a threshold, cold data is moved onto the most-worn free
  block.

[docs/DESIGN.md](docs/DESIGN.md) has the full format, a crash-window table
for each step, and the space proof.

## Testing

```bash
make test        # 23 tests, ~5 s
make mutants     # plant 12 crash-safety bugs; each must be caught
make sanitize    # ASan + UBSan (Linux/macOS)
make coverage    # gcov line/branch coverage of src/fkv.c
make bench       # wear / write-amplification benchmark
```

The simulator (`sim/`) models NOR flash precisely:

- Erase sets `0xFF`, and program can only clear bits.
- Programming a byte that isn't erased, or any unaligned access, counts as a
  **violation**. Every test requires zero violations.
- A power cut interrupts the chosen operation: a program writes a random
  prefix and leaves one byte half-programmed; an erase leaves the block
  untouched, partly erased, or with drifted bits.

| suite | what it checks |
|---|---|
| **power loss: every point** | Counts the flash operations of a workload, then reruns it once per operation with the power cut exactly there (9,058 cut points). After each cut, mount must succeed and `fkv_check()` must pass. Every earlier op must be durable, the interrupted op atomic, and the rest of the workload must run cleanly. |
| **power loss: nested** | The same, but power keeps failing again: during mount recovery, during the next compaction, and so on, up to five cuts per run. |
| **power loss: long random** | 30 × 8,000-op runs with up to 200 cuts each on a larger device. |
| **model-based** | Random put/get/delete against a reference model with random remounts, over 7 flash geometries × 2 GC policies × wear leveling on/off. |
| **space** | Fills to capacity, checks `NOSPC` fires only at the proven bound, overwrites a full store 5,000 times, and reclaims tombstones. |
| **wear** | Static WL keeps the erase spread within its threshold; without it, cold blocks stay at 1 erase while hot ones pass 500. |
| **corruption** | Bit rot in a value is detected (`FKV_ERR_CORRUPT`) and skipped on remount without losing neighbouring records; damaged headers are survived. |
| **unit** | API edge cases, config validation, CRC-32 test vector, simulator semantics. |

Mutation testing (`tests/mutants.sh`) plants these 12 bugs in a copy of
`fkv.c`. The suite must fail against each one, and it does:

```
killed    trust-torn-data               killed    erase-source-before-commit
killed    index-corrupt-records         killed    commit-before-copy
killed    append-after-torn-tail        killed    reuse-sequence-numbers
killed    skip-blank-check-on-alloc     killed    ignore-retired-flag
killed    always-drop-tombstones        killed    spend-gc-reserve
killed    no-supersede-at-mount         killed    capacity-overcommit
12/12 mutants killed
```

Coverage of `src/fkv.c` is 90% of lines and 79% of branches taken. The
uncovered lines are almost all `return err` paths for flash *read* failures,
which the simulator doesn't inject.

## Benchmark

`make bench` runs 256 KiB of flash (64 × 4 KiB blocks) filled to 50% or 80%,
then applies 400,000 updates of 100-byte values. Lifetime is the user data
that can be written before the most-worn block reaches 100k erase cycles.

| workload | fill | policy | WA | erase min / mean / max | evenness | lifetime |
|---|---|---|---|---|---|---|
| uniform | 80% | greedy | 2.44 | 452 / 457 / 460 | 99% | 10.5 GB |
| hot/cold 90/10 | 80% | greedy + static WL | 3.24 | 600 / 608 / 615 | 99% | 7.8 GB |
| hot/cold 99/1 | 80% | greedy, no static WL | 3.84 | 454 / 720 / 769 | 94% | 6.3 GB |
| hot/cold 99/1 | 80% | greedy + static WL | 3.95 | 730 / 739 / 746 | 99% | 6.5 GB |
| static + hot | 80% | greedy, no static WL | 1.01 | **1** / 190 / **866** | 22% | **5.6 GB** |
| static + hot | 80% | greedy + static WL | 1.09 | 197 / 204 / 213 | 96% | **22.6 GB** |

*WA* is write amplification: flash bytes programmed ÷ bytes the user wrote.
*Evenness* is mean ÷ max erase count. The full 24-row table is in
[docs/benchmark.md](docs/benchmark.md).

```
$ ./build/fkv_bench --heatmap
50% static data, all writes to 2% of keys; one cell per block, shaded 0..368 erases

without static wear leveling: erase counts 1..368
                                                              @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
with static wear leveling   : erase counts 185..201
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
```

Findings:

- **Static data is where wear leveling earns its keep.** Without static
  wear leveling, half the device is pinned under data that never changes
  and never gets erased; the other half wears out 4× faster.
- **Write amplification is driven by utilisation.** It roughly doubles from
  50% to 80% full. Wear leveling adds only a few percent on top.
- **Cost-benefit GC does not beat greedy here.** Compaction output and new
  writes share one head block, so hot and cold data mix and the "age" signal
  that cost-benefit depends on never forms. Separating hot and cold streams
  is the natural next step; see
  [DESIGN.md §7](docs/DESIGN.md#7-limitations-and-future-work).

## Using it

Provide a flash driver and the buffers:

```c
#include "fkv.h"

static int my_read (void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t len);
static int my_prog (void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t len);
static int my_erase(void *ctx, uint32_t block);

static const struct fkv_flash flash = {
    .block_size = 4096, .block_count = 16, .prog_size = 4,
    .read = my_read, .prog = my_prog, .erase = my_erase,
};

static struct fkv_block blocks[16];
static struct fkv_slot  slots[64];                        // power of two; up to 48 keys
static uint8_t scratch[FKV_SCRATCH_SIZE(32, 256, 4)];     // max key, max value, prog size

static const struct fkv_config cfg = {
    .flash = &flash, .max_key_len = 32, .max_value_len = 256,
    .gc_policy = FKV_GC_GREEDY, .wl_threshold = 16,
    .blocks = blocks, .slots = slots, .slot_count = 64,
    .scratch = scratch, .scratch_size = sizeof scratch,
};

fkv_t db;
if (fkv_mount(&db, &cfg) == FKV_ERR_NOFS) {   // first boot
    fkv_format(&cfg);
    fkv_mount(&db, &cfg);
}
```

| function | |
|---|---|
| `fkv_format(cfg)` | erase the device, keeping erase counts |
| `fkv_mount(db, cfg)` | rebuild the index; finish any interrupted compaction |
| `fkv_put(db, key, klen, val, vlen)` | insert or replace; durable on return; identical rewrites are free |
| `fkv_get(db, key, klen, buf, size, &vlen)` | read a value (`buf = NULL` → length only) |
| `fkv_del(db, key, klen)` | delete |
| `fkv_foreach(db, cb, ctx)` | iterate live keys |
| `fkv_stat(db, &st)` | usage, erase-count distribution, WA counters |
| `fkv_check(db)` | consistency check of index vs. flash |

After any `FKV_ERR_IO` (driver failure or power loss), the handle refuses
further calls until you remount. `examples/basic.c` shows a boot counter
surviving a power cut in the middle of a write:

```
$ make example
no store found, formatting
...
cutting power during the next flash operation...
put returned: flash I/O error (remount required)
boot #4
contents after recovery (old value intact, or the new one - never a mix):
  boot_count = 4
  wifi.ssid = workshop
```

## Layout

```
include/fkv.h        public API
src/fkv.c            the store (~1,300 lines incl. comments)
sim/fkv_sim.[ch]     NOR flash simulator with power-cut injection
tests/               test suite, reference model, mutation script
bench/bench.c        wear / write-amplification benchmark
examples/basic.c     minimal usage
docs/DESIGN.md       format, crash-safety argument, space proof
```

Build requirements: a C99 compiler and `make`. CI builds with GCC and Clang
on Linux and macOS, runs the sanitizers, runs mutation testing, and reports
Cortex-M4 code size.

## Limitations

- `fkv_format` is not power-safe.
- No bad-block management.
- No multi-key transactions.
- Keys up to 255 bytes; values up to 64 KiB (bounded by block size).

See [DESIGN.md §7](docs/DESIGN.md#7-limitations-and-future-work).

## License

MIT
