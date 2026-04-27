#!/usr/bin/env bash
# Mutation testing: plant a known crash-safety bug in a copy of src/fkv.c,
# rebuild the test suite against it, and require the suite to FAIL.
# A mutant that survives means the tests have a blind spot.
#
#   usage: tests/mutants.sh            (from the repository root)
set -u
CC=${CC:-cc}
OUT=build-mut
mkdir -p "$OUT"

# Each mutant: name, then a perl program applied to the whole source file.
names=()
progs=()
mutant() { names+=("$1"); progs+=("$2"); }

# Recovery must reject records whose payload was torn.
mutant trust-torn-data \
  's/if \(check_data\) \{/if (0) {/'
mutant index-corrupt-records \
  's/if \(st == REC_BAD_DATA\) \{/if (0) {/'
# A head block with a torn tail must not be appended to.
mutant append-after-torn-tail \
  's/h->sealed = blank \? 0 : 1;/h->sealed = 0;/'
# A "free" block may hold a half-finished compaction: verify before reuse.
mutant skip-blank-check-on-alloc \
  's/bl\[best\]\.verified = blank;/bl[best].verified = 1;/'
# Dropping a tombstone while an older record exists resurrects the key.
mutant always-drop-tombstones \
  's/(\*droppable = false;\n    for)/*droppable = true; return FKV_OK;\n    for/'
# After a crash between commit and retire, the source must be retired.
mutant no-supersede-at-mount \
  's/bl\[v\]\.seq == bl\[d\]\.live &&/0 &&/'
# The classic: erase the old copy before the new one is committed.
mutant erase-source-before-commit \
  's/(    err = block_commit\(db, d, bl\[v\]\.seq, out, dlive\);\n    if \(err\)\n        return err;\n    set_head\(db, d\);\n)(.*?err = block_reset\(db, v\);\n    if \(err\)\n        return err;\n)/    uint32_t vseq = bl[v].seq;\n$2$1/s;
   s/block_commit\(db, d, bl\[v\]\.seq, out, dlive\)/block_commit(db, d, vseq, out, dlive)/'
# Commit (write header B) first, then copy: a crash mid-copy loses data.
mutant commit-before-copy \
  's/int err = hdr_write_b\(db, b, db->next_seq, src\);\n    if \(err\)\n        return err;\n/int err = FKV_OK;\n    (void)src;\n/;
   s/(\n    err = block_commit\(db, b, NONE, OFF_DATA, 0\);)/\n    err = hdr_write_b(db, b, db->next_seq, NONE);\n    if (err)\n        return err;$1/;
   s/(int err = block_alloc\(db, wear_level, &d\);\n    if \(err\)\n        return err;\n)/$1    err = hdr_write_b(db, d, db->next_seq, bl[v].seq);\n    if (err)\n        return err;\n/'
# Sequence numbers must never repeat across mounts.
mutant reuse-sequence-numbers \
  's/db->next_seq = max_seq;/db->next_seq = max_seq ? max_seq - 1 : 0;/'
# A retired block must stay dead even if its erase never happened.
mutant ignore-retired-flag \
  's/if \(h\.retired \|\| h\.b == -1\)/if (h.b == -1)/'
# Opening a head block must not consume the block reserved for GC.
mutant spend-gc-reserve \
  's/if \(db->nfree >= 2\) \{/if (db->nfree >= 1) {/'
# Admitting more live data than the capacity bound can deadlock GC.
mutant capacity-overcommit \
  's/db->capacity = \(db->flash->block_count - 1\)/db->capacity = (db->flash->block_count + 1)/'

survived=0
for i in "${!names[@]}"; do
  name=${names[$i]}
  src="$OUT/fkv_$name.c"
  perl -0777 -pe "${progs[$i]}" src/fkv.c > "$src"
  if cmp -s src/fkv.c "$src"; then
    echo "  ??        $name: substitution did not apply"
    survived=$((survived + 1))
    continue
  fi
  bin="$OUT/test_$name"
  if ! $CC -O2 -std=c99 -w -Iinclude -Isim "$src" sim/fkv_sim.c tests/util.c tests/test_*.c \
       -o "$bin" 2> "$OUT/$name.log"; then
    echo "  ??        $name: does not compile (see $OUT/$name.log)"
    survived=$((survived + 1))
    continue
  fi
  if timeout 300 "$bin" > "$OUT/$name.out" 2>&1; then
    echo "  SURVIVED  $name"
    survived=$((survived + 1))
  else
    caught=$(grep -m1 -oE "in test_[a-z_]+" "$OUT/$name.out" | sed 's/^in //')
    [[ -z $caught ]] && caught=$(grep -m1 "RUN" "$OUT/$name.out" | tail -1)
    [[ -z $caught ]] && caught="crash"
    last=$(grep "RUN" "$OUT/$name.out" | tail -1 | sed 's/.*\] //')
    echo "  killed    $name  (by ${caught:-$last})"
  fi
done

total=${#names[@]}
echo
echo "$((total - survived))/$total mutants killed"
[[ $survived -eq 0 ]]
