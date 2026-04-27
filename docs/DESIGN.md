# fkv design

This document explains how fkv lays data out on flash, why every flash
operation can be interrupted without losing committed data, how space is
reclaimed without ever deadlocking, and how wear is spread across the device.

## 1. The flash model

fkv targets raw NOR-style flash, as found inside microcontrollers and on SPI
NOR chips:

| property | consequence |
|---|---|
| erase works on whole blocks (e.g. 4 KiB) and sets every byte to `0xFF` | data can't be updated in place; fkv is log-structured |
| programming can only clear bits (1 → 0) | a byte can be written once per erase; fkv never programs a byte that isn't `0xFF` |
| program granularity `prog_size` (1–16 bytes) | every write is aligned and padded to `prog_size` |
| each block survives a limited number of erases (~10k–100k) | erases have to be spread evenly (wear leveling) |
| power can fail at any moment | any single program or erase may be torn |

The simulator (`sim/fkv_sim.c`) enforces the first three rules. Any misuse
(programming a non-erased byte, unaligned access) is counted as a
*violation*, and every test requires zero violations. It models a power cut
like this:

- **Interrupted program:** a random prefix of the data is written, and the
  byte at the cut point gets a random subset of its bits programmed.
- **Interrupted erase:** the block is untouched, partially erased, or has
  random bits drifted toward `1`.
- **After the cut:** every call fails until the device is powered on again.

## 2. On-flash format

Every block starts with a 48-byte header made of three independently
programmable 16-byte fields, followed by records:

```
offset
  0  ┌───────────────────────────────────────────────────────────────┐
     │ A  magic "FLKV" │ version │ 0xFFFF │ erase_count │ crc32      │  written right after erase
 16  ├───────────────────────────────────────────────────────────────┤
     │ B  seq │ src_seq │ 0xFFFFFFFF │ crc32                         │  written to commit the block
 32  ├───────────────────────────────────────────────────────────────┤
     │ C  16 × 0xFF   (programmed to 0x00 to retire the block)        │
 48  ├───────────────────────────────────────────────────────────────┤
     │ record │ record │ record │ ...                   │ 0xFF 0xFF ...│
     └───────────────────────────────────────────────────────────────┘

record:  type u8 │ klen u8 │ vlen u16 │ data_crc u32 │ hdr_crc u32 │ key │ value │ pad
         type = PUT (0x50) or DEL (0x44, a tombstone); padded with 0xFF to prog_size
```

The three header fields give each block a small state machine. Each
transition is a single program or erase:

```
                erase + write A              write B                write C             erase + write A
   (garbage) ─────────────────▶  FREE  ──────────────▶  ACTIVE  ─────────────▶  RETIRED ────────────────▶ FREE
                                  │  A valid,              A, B valid             C != 0xFF
                                  │  B/C erased            records appended
```

When mounting, fkv classifies each block from its header alone:

| header state | classification |
|---|---|
| A invalid | *dirty*: erase before use; erase count estimated as the maximum known |
| A valid, C programmed (any bit) | *dirty* (retired) |
| A valid, B torn (not erased, bad CRC) | *dirty* |
| A valid, B erased | *free*: the data area is verified blank before first use |
| A valid, B valid | *active*: its records are replayed |

**Sequence numbers.** `seq` increases every time a block is committed and is
never reused; the next value is derived at mount from the largest `seq` still
on flash. Active blocks are replayed in `seq` order and later records win.
This is sufficient because of one invariant: *a record that is not the
latest for its key always sits either in an older block or earlier in the
same block.* User writes always go to the newest block, and compaction only
copies latest records, into a block with a new, higher `seq`.

## 3. Operations and their crash windows

### put / delete: one record append

An update is a single record programmed at the head block's append point. A
record is valid only if both CRCs match, so it is atomic:

| cut happens… | what mount sees | result |
|---|---|---|
| before any byte is written | erased space | old value |
| after a partial header | header CRC fails → end of log; tail not blank → head **sealed** | old value; next write opens a new block |
| after the header, inside key/value | header OK, data CRC fails → **skip** the record by its length | old value; appending continues after it |
| after the full record | valid record | new value |

Skipping a record whose header is intact but whose payload is bad has a
second use. A bit flipped by data retention (bit rot) in an old record loses
only that record, not everything after it in the block. The header CRC is
defense in depth: under the prefix-tearing model the data CRC and the length
checks already reject every torn header. Mutation testing confirms this;
disabling the header CRC is not observable.

### Compaction (garbage collection)

Compaction reclaims a *victim* block `v` by copying its live records
elsewhere. The naive approach (copy the records into the current head, then
erase `v`) breaks under power loss. A torn copy can seal the head and waste
the GC reserve, and repeated crashes can then leave no free block at all.
fkv copies into a **fresh block that is committed only after the copy is
complete**:

```
1. d = least-worn free block (erase it if it isn't verifiably blank)
2. copy every live record of v into d        (d's header B still erased)
3. write d's header B: seq = next, src_seq = v.seq          ◀ commit point
4. write v's header C (retire)
5. erase v, write v's header A with erase_count + 1
```

| cut happens during… | state after mount |
|---|---|
| 1 | `d` is dirty or free; `v` intact. Nothing lost. |
| 2 | `d` has records but no header B, so it counts as *free*. The blank check on allocation finds it isn't erased and erases it. `v` intact. |
| 3 (torn B) | `d` is *dirty* (B torn). `v` intact. |
| between 3 and 4 | `d` and `v` both active. Mount sees `d.src_seq == v.seq` and **finishes the job** by retiring `v` itself. |
| 4 (torn C) | any programmed bit in C means retired. `v` is ignored. |
| 5 | `v` is dirty (retired, or header destroyed by the torn erase). |

The `src_seq` link in step 3 is what makes the window between commit and
retire safe. Without it, `v` would be replayed alongside `d`. The values
would be identical, so that looks harmless, but any tombstone that
compaction legitimately dropped (below) would stay in `v`. Once `v` is later
retired, an older record of that key could come back to life.

### Tombstones

A delete writes a tombstone record. A tombstone can only be discarded once
no older record for the same key exists anywhere on flash; otherwise erasing
the tombstone resurrects the old value. During compaction of `v`, a
tombstone that is still the latest record for its key is:

- **dropped** if no active block older than `v` contains a record with that
  key (checked by scanning those blocks);
- **copied forward** otherwise.

If a write fails for lack of space or index slots while tombstones exist, fkv
compacts the log oldest-first. A tombstone in the oldest block can always be
dropped, so one pass over the log reclaims every tombstone whose key is
gone. This keeps a store that has been filled, cleared, and refilled from
reporting "full".

## 4. Space: why GC can never deadlock

Let `N` be the block count, `P` the payload bytes per block, and `M` the
largest possible record. fkv keeps two invariants:

1. **At least one block is free (or reclaimable by erase) at rest.** A new
   head block is opened only while two or more are free; otherwise a block is
   compacted. Compaction consumes one free block and releases one, and every
   crash window above leaves the free count unchanged.
2. **Live data never exceeds `capacity = (N − 1) · (P − M)`.** Live data is
   the on-flash size of every current record, including tombstones. `put`
   refuses with `FKV_ERR_NOSPC` if accepting the record would break this.

**Claim:** whenever the head cannot fit a record of size `r ≤ M`, some active
block has at least `M` reclaimable bytes (`P − live`).

**Proof:** GC is only needed when exactly one block is free, so `N − 1`
blocks are active. If every one of them had fewer than `M` reclaimable
bytes, each would hold more than `P − M` live bytes. Total live data would
then exceed `(N − 1)(P − M) = capacity`, contradicting invariant 2. ∎

Compacting that block into the free block yields a head with at least
`M ≥ r` bytes free, so a single compaction always makes room. The tests
check both directions:

- `NOSPC` is only returned when the capacity bound is really reached.
- A store at 100% capacity keeps accepting overwrites.

Mutation testing checks that the tests would notice a violation. Raising
the bound, or letting a head allocation spend the reserve block, makes them
fail.

**Cost of the bound:** one block plus one maximum-size record per block is
reserved. For 64 × 4 KiB blocks with 156-byte maximum records, capacity is
95% of the raw data area.

## 5. Wear leveling

**Dynamic wear leveling.** Whenever a block is needed, the free block with
the lowest erase count is taken. This alone evens out wear across blocks
that hold changing data.

**Static wear leveling.** Data that never changes pins its block, which then
never gets erased while the rest of the device wears out. Each time the head
fills, fkv compares the most-worn block's erase count with the least-worn
block that holds data. If the difference exceeds `wl_threshold`, that cold
block is compacted into the **most-worn** free block. The cold data then sits
on a worn block where it will stay put, and the barely-used block goes back
into circulation. Relocation reuses the compaction path, so it inherits all
of its crash safety.

Erase counts are stored in header A and survive reboots and reformatting. A
block whose header was destroyed (for example by a torn erase) is assumed to
be as worn as the most-worn known block. That is the conservative choice.

**GC victim policy** (`gc_policy`):

- `FKV_GC_GREEDY`: the block with the most reclaimable bytes.
- `FKV_GC_COST_BENEFIT`: the LFS formula `(1 − u) · age / (1 + u)`, with
  `u` = live fraction and `age` = blocks committed since.

## 6. Memory

fkv does no dynamic allocation. The caller provides:

| buffer | size |
|---|---|
| handle `fkv_t` | ~200 B |
| `struct fkv_block[block_count]` | 20 B per block |
| `struct fkv_slot[slot_count]` | 16 B per slot; up to ¾ of slots hold keys |
| scratch | one maximum-size record |

The index is an open-addressing hash table of `(hash, location)`. Keys stay
on flash and are compared there. Records are validated by streaming them
through a 32-byte stack buffer, so mount needs no RAM proportional to value
size. Mount is `O(blocks² + records)`: blocks are ordered without extra
memory.

## 7. Limitations and future work

- **`fkv_format` is not power-safe.** If interrupted, run it again.
- **One write stream.** Compaction output and new writes share the head
  block, so hot and cold data mix. That is the likely reason cost-benefit
  GC does not beat greedy in the benchmark: the age signal it relies on
  never forms.
  Separating hot and cold streams needs per-record sequence numbers instead
  of per-block ones.
- **No bad-block management.** A block that fails to erase or program is
  not retired. Flash that reports program/erase failures (most NAND, some
  NOR) would need it.
- **Bit rot found during compaction.** If a live record fails its CRC while
  being copied, fkv drops the key. After a remount, an older version of that
  key can reappear if one is still on flash. That matches what mount does
  with a damaged record, but it is visible to the application.
- **Read errors aren't injected.** The simulator cuts power on
  program/erase only. The error paths for failed reads make up most of the
  ~10% of lines the tests don't cover.
- **Wraparound limits.** `seq` is 32-bit, so about 4 × 10⁹ block commits.
  Keys are up to 255 bytes and values up to 64 KiB (bounded by the block
  size).
- **No multi-key transactions.** Each put and delete is atomic on its own.
