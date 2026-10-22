# JournalFS — final report

**What it is.**  A crash-consistent, journaling, log-structured filesystem
simulator in C11 for Linux, with a custom on-disk image, a CLI, deterministic
crash injection, a recovery path that runs at mount time, six automated test
suites (264 checks) and a measured benchmark against the same filesystem
running without its journal.

**What it is not.**  It is not a kernel filesystem, not a FUSE mount, not a
performance-equivalent of ext4 or ZFS, and not a model of a physical device.
It is a simulator whose phase ordering is real and whose storage stack below
the image file is not modelled.  Section 7 lists the limitations in full.

---

## 1. Deliverables

| deliverable | where |
|---|---|
| engine (14 modules, ~6 000 lines of C) | `src/`, `include/` |
| build | `Makefile` (`make`, `clean`, `test`, `test-crash`, `test-all`, `smoke`, `benchmark`, `sanitize`, `test-sanitize`) |
| CLI with 29 commands | `src/cli.c`; run `build/journalfs --help` |
| interruptible crash injection (14 points) | `src/crash.c`, `--crash POINT`, `--crash-after N` |
| functional tests | `tests/test_functional.sh` — 56 checks |
| error handling tests | `tests/test_errors.sh` — 49 checks |
| persistence tests | `tests/test_persistence.sh` — 26 checks |
| corruption tests | `tests/test_corruption.sh` + `tests/corrupt.py` — 35 checks |
| crash recovery tests | `tests/test_crash_recovery.sh` — 57 checks |
| stress driver and suite | `tests/fsstress.c`, `tests/test_stress.sh` — 41 checks |
| benchmarks | `benchmarks/benchmark.sh`, `benchmarks/plot.py` → `results/benchmark.{csv,md,png}` |
| documentation | `README.md`, `docs/` (architecture, disk format, journal protocol, recovery design, diagrams, testing plan, this report, presentation, demo script, checklist) |

## 2. Milestones

| milestone | content | status |
|---|---|---|
| M1 | image + CLI + core file operations, functional tests | done, 56 checks |
| M2 | transaction manager, journal records, write-ahead logging | done, crash points `pre_write`…`post_commit_sync` |
| M3 | log-structured storage engine, compaction, checkpoints | done, `compact_mid`/`compact_post` covered |
| M4 | recovery manager: mount time scan, replay, idempotency | done, `recover_mid`, 57 checks |
| M5 | directories, permissions, metadata, persistence | done, 26 + 49 checks |
| M6 | corruption handling, stress testing, benchmarks, docs | done, 35 + 41 checks, `results/benchmark.*` |

Every milestone was implemented, built and tested before the next one started;
the test logs in `results/` are from the current build.

## 3. Verification results

```
tests/test_functional.sh       56 passed, 0 failed
tests/test_errors.sh           49 passed, 0 failed
tests/test_persistence.sh      26 passed, 0 failed
tests/test_corruption.sh       35 passed, 0 failed
tests/test_crash_recovery.sh   57 passed, 0 failed
tests/test_stress.sh           41 passed, 0 failed
                              ---
                              264 checks, 0 failed
```

* the same 264 checks pass under AddressSanitizer + UndefinedBehaviorSanitizer
  with leak detection and stack traces enabled (`make test-sanitize`);
* the build is warning free with `-Wall -Wextra -Wpedantic -Wshadow
  -Wstrict-prototypes -Wwrite-strings -Wpointer-arith -Wcast-align -Wformat=2`;
* a clean checkout builds with `make` and passes `make test-all` — the test
  scripts create their own images in a temporary directory, so nothing in
  `results/` needs to exist beforehand.

## 4. Crash-consistency evidence

The claim "a crash never leaves a torn state" is verified point by point
(`results/test_crash_recovery.log`):

| crash point | asserted outcome | result |
|---|---|---|
| `pre_write`, `post_write`, `post_sync` | the operation is not durable: the file keeps the old value | pass |
| `post_commit`, `post_commit_sync` | the commit is durable: recovery produces the new value (a process crash keeps the page cache, so `post_commit` is allowed to land either way, but never as a mixture) | pass |
| `post_apply`, `after_imap`, `checkpoint_mid`, `checkpoint_post` | the transaction is replayed again from the journal; content is the new value and identical on the second mount | pass |
| `compact_mid`, `compact_post` | the last committed version survives a crash in the middle of compaction; the image stays consistent | pass |
| `recover_mid` | a crash *during* recovery is survivable: the next mount finishes the replay | pass |
| `pre_format` | `format` cannot destroy an existing image before the new one is written | pass (byte-for-byte comparison) |
| two crashes in a row | both committed changes survive | pass |
| crash before a commit, then a crash after one | the older incomplete record is ignored and the newer commit is still replayed (no false damage report) | pass |
| batched checkpoints (3 commits pending) | all three are replayed; `3 committed and replayed`, window empty afterwards | pass |

Corruption evidence (`results/test_corruption.log`): each of 13 single-structure
damages is either refused at mount with a message naming the structure, or
mounted with the damage explicitly reported; eight images with 40 random
512-byte scribbles each never crashed or aborted the engine, and every mount
that succeeded still passed the consistency check.

## 5. Measured performance

`make benchmark` → `results/benchmark.md`, raw numbers in
`results/benchmark.csv`, figure in `results/benchmark.png`.  8 MiB image,
median of 5 runs per workload, journaled vs `--no-journal` (same engine, same
log, same checkpoints, no journal writes and no fsync barriers).

```
workload                  journaled            baseline            ratio (journaled/baseline)
create 240 files          107 ms   8 972 ops/s   95 ms  10 105 ops/s   1.13x
append x3000              617 ms   4 862 ops/s  579 ms   5 181 ops/s   1.07x
rewrite 8 KiB x600         69 ms   8 710 ops/s   62 ms   9 694 ops/s   1.11x
bulk 62.5 KiB x20          16 ms    76.3 MB/s    8 ms   152.6 MB/s    2.00x
churn (create/rename/rm)  251 ms   7 171 ops/s  212 ms   8 491 ops/s   1.18x
mkdir/rmdir x1500         469 ms   6 397 ops/s  353 ms   8 499 ops/s   1.33x
read 32 KiB x3000         324 ms     289 MB/s  323 ms     290 MB/s    1.00x  (no journal involvement)
```

Crash recovery (median of 5 mounts of the same crashed image):

```
clean mount, nothing to replay        0 txn,  0 blocks          2 ms
16 small appends pending             16 txn, 32 blocks   6 008 B  3 ms
5 x 8 KiB rewrites pending            5 txn, 12 blocks   9 732 B  3 ms
```

How to read this honestly:

* The cost of journaling shows up where work is *small and transaction-like*:
  metadata churn (+18 %), directory operations (+33 %), creating many files
  (+13 %).  Those are dominated by the two fsync barriers per transaction,
  which is the price of the guarantee.
* The 8 KiB rewrite workload moves least (+11 %) because the journal-record and
  fsync cost is small compared with writing 4.9 MB of payload.
* The 2.0× on bulk writes is the case where the journal's extra write of the
  same bytes is a real fraction of the work.
* Reads do not touch the journal at all, so the 1.00× there is the noise floor;
  on this shared 2-CPU machine, repeated runs move the numbers by a few
  percent.  Differences below ~5 % in this table are not meaningful, and
  repeated runs of the whole script have moved individual rows by up to ~10 %.
* Recovery is milliseconds regardless of the workload, because the journal
  bounds how much redo work can ever be pending: 48 blocks (192 KiB) is the
  maximum, and reclamation keeps the window much smaller in steady state.
* These numbers come from a file on a host filesystem with its own page cache.
  A device with a real write cache changes the shape (fsync gets more
  expensive, payload writes get relatively cheaper), not the direction.

## 6. Bugs the project found in itself

Testing was not decoration; the suites found real defects in the engine, and
each one is now covered by a check:

1. **Log address 0 collided with the "no block" sentinel.**  Fixed by
   permanently reserving offset 0 in each log half.
2. **Freeing journal records is logical, not physical.**  After the 48-block
   ring wrapped, mount's window scan re-read already-reclaimed records and
   either refused to journal at all or would have placed new records on top of
   live ones.  Fixed by deriving the window as "the checkpointed prefix,
   extended only by transactions newer than the checkpoint", with an explicit
   damage report when a valid record is found beyond a stop point.
3. **Growing or shrinking a file inside its existing last block.**  Two
   related defects: record reuse that ignored the stored length, and stale
   block references left behind by a shrink.  Both produced inodes that failed
   their own validation (and could have released live blocks).
4. **A durable transaction could fail to apply.**  Running out of log space
   during the apply left a committed transaction partly applied — the one
   state the engine refuses to continue from.  Fixed by checking journal *and*
   log room before the first write, with a retry after a forced
   checkpoint/compaction, and by a `failed` state that stops the filesystem
   until it is replayed.
5. **`chmod` looked like a block re-declaration.**  The transaction-only
   `provided_mask` was being persisted; cleared on load and always written as 0.
6. **Batch checkpointing never batched.**  Every transaction freed its own
   journal records, so `--checkpoint-interval N` did nothing (and the tests
   caught it); the deferral now honours N, bounded by a free-block guard, and
   `sync`/`unmount` still force a checkpoint.
7. **A stress run over a full filesystem reported false failures.**  Out-of-room
   refusals are now treated as legitimate results and verified as no-ops.
8. **The journal record sequence restarted at zero in every session.**  A
   session that began with records from an earlier crashed session still in the
   window wrote records with *lower* sequence numbers than the ones already
   there; the sequence rule that distinguishes live records from the residue of
   an earlier lap then classified the new records as damage and skipped a
   committed transaction.  Fixed by continuing the sequence from the highest
   record in the window.  Found while rehearsing the demo script (a crash
   before a commit, followed by a crash after one), and now covered by the
   check "an older incomplete record does not hide a newer commit".

## 7. Limitations (explicit)

1. **Simulated durability.**  The image is a file on the host filesystem, and a
   crash is `_exit(99)` inside the process.  The write/sync *order* is real; the
   behaviour of a physical device's write cache is not modelled, and no
   physical storage was measured.
2. **Single process, single user.**  One mount at a time (enforced by a
   superblock flag); uid/gid are fixed at 1000; permission bits are stored and
   checked but are not an isolation mechanism — the process owns the image
   file.
3. **Small geometry.**  1 – 64 MiB images, 252 inodes, 16 direct blocks per
   file (64 768 bytes maximum), 100 directory entries per block, 31 byte
   names, no indirect blocks, no sparse files, no symlinks, no attributes.
4. **No fsck.**  There is no offline repair tool: recovery is redo-based, and
   damage that redo cannot fix is reported, not repaired.
5. **Journal capacity.**  48 blocks (192 KiB) of pending redo; a transaction
   that cannot fit fails cleanly rather than growing the journal.
6. **Timing numbers are not reproducible to the percent** on a shared machine;
   the benchmark reports medians and states its noise floor.
7. **No concurrency control, no mmap, no async I/O** — the I/O path is
   synchronous `pread`/`pwrite`/`fsync`, which is what makes the ordering
   auditable.

## 8. What would come next

* Group commit: batch the fsyncs of several transactions to cut the dominant
  cost, with the durability contract unchanged.
* A second inode-map form (deltas instead of a 4 KiB rewrite) to reduce the
  per-transaction payload.
* Indirect blocks and a real free-space map, to lift the 64 KiB file limit.
* A reference implementation of the format in Python (extending
  `tests/corrupt.py`) so the on-disk structures have two independent readers by
  construction.
* An offline `fsck`-style consistency reporter for the cases that currently
  only refuse to mount.
