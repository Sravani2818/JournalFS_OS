# JournalFS

A **crash-consistent journaling + log-structured filesystem simulator** in C11
for Linux.  It implements, in user space, the parts of a filesystem that make
crash consistency interesting — a write-ahead journal, an append-only
log-structured store, checkpoints, compaction, and recovery at mount time —
and keeps everything inside one disk image file that it manages itself.

It is a **simulator**: the image is a file on the host filesystem and a crash is
`_exit(99)` at a named point inside the engine.  The write/sync *ordering* is
real; the behaviour of a physical disk is not modelled.  See
[Limitations](#limitations).

```
$ make
$ build/journalfs -q -c "format /tmp/demo.img --size 1m"
formatted /tmp/demo.img (1024 KiB, 256 blocks of 4096 bytes)
$ build/journalfs -i /tmp/demo.img -c "mount" -c 'create /hello' -c 'write /hello HELLO' -c 'cat /hello' -c unmount
mounted /tmp/demo.img:
  inodes: 252 max
  log blocks: 102 per half
  checkpoint seq: 1
  applied txid: 0
recovery: journal window is empty, nothing to redo

created /hello (0644)
wrote /hello
HELLO
unmounted /tmp/demo.img (checkpoint written, image marked clean)

$ build/journalfs -i /tmp/demo.img --crash post_commit_sync -c "mount" -c 'append /hello _WORLD'
mounted /tmp/demo.img:
  inodes: 252 max
  log blocks: 102 per half
  checkpoint seq: 4
  applied txid: 2
recovery: journal window is empty, nothing to redo

[crash] injected crash at point 'post_commit_sync' (durable commit, changes not applied)
        -- terminating with _exit(99)
        -- no image write was flushed by this process

$ build/journalfs -i /tmp/demo.img -c "mount" -c 'cat /hello' -c unmount
mounted /tmp/demo.img:
  inodes: 252 max
  log blocks: 102 per half
  checkpoint seq: 5
  applied txid: 3
recovery ran: 1 transaction(s) replayed, 0 incomplete ignored

HELLO_WORLD
unmounted /tmp/demo.img (checkpoint written, image marked clean)
```

---

## Build and run

Requirements: `gcc` (C11), `make`, POSIX. Python 3 with `matplotlib` is
optional and only used for the benchmark plot; there are no other dependencies.

```bash
make                      # build build/journalfs and build/fsstress
make smoke                # quick functional subset
make test                 # functional, error, persistence, corruption, bcache
make test-crash           # crash injection + recovery suite
make test-compare         # without-journal failure mode vs with-journal recovery + group commit
make test-all             # every suite (306 checks)
make test-sanitize        # rebuild with ASan+UBSan and run every suite
make benchmark            # measured comparison -> results/benchmark.{csv,md,png}
make clean
```

`build/` is generated; if it is missing, `make` recreates it.

## Using the filesystem

```bash
build/journalfs                       # interactive shell on no image
build/journalfs -i disk.img           # interactive shell on an image
build/journalfs -i disk.img -c mount -c "write /f hello" -c unmount
build/journalfs -i disk.img -f script.txt     # run a script of commands
```

Commands: `format mount unmount create write append read cat ls stat rm
mkdir rmdir rename truncate chmod fill hash cd pwd df sync check journal
inspect stats set help exit` — `build/journalfs --help` lists them with their
arguments.

Options: `-i/--image`, `-c/--command` (repeatable), `-f/--script`,
`--crash POINT`, `--crash-after N`, `--quiet`, `--verbose`, `--no-recover`,
`--no-journal`, `--checkpoint-interval N`, `--group-commit N` (also via `set`),
`--color` / `--no-color` / `set color` (ANSI terminal color highlighting; auto-detected on TTY).

Exit codes: `0` success, `1` operation error, `2` usage error, `99` injected
crash.

## What it does, in one page

**Write path.**  An operation (say `append`) builds a set of redo operations in
memory.  The transaction manager plans how many journal blocks they need,
checks room, then:

1. writes the redo records (`OP`) into the journal and `fsync()`s — barrier #1;
2. writes a `COMMIT` record and `fsync()`s — barrier #2, the durability point;
3. applies the changes to the log-structured log (data blocks, then inodes,
   then a new inode-map record);
4. writes a checkpoint into the *other* checkpoint slot, `fsync()`s, and only
   then reclaims the journal records it made unnecessary.

**Group commit (Add-on 1).**  Batching the `fsync()` barriers of up to $N$
transactions (`--group-commit N` or `set group-commit N`) amortizes disk write
synchronization overhead, dramatically boosting throughput while preserving full crash consistency.

**Crash at any point.**  The next mount re-derives the journal window, replays
the committed-but-not-applied transactions, ignores incomplete ones, and leaves
a consistent image.  All 14 injection points are documented with the outcome
they must produce (`docs/journal_protocol.md` §8) and asserted by the tests.

**Log-structured storage.**  Every object lives in an append-only log with two
halves: new versions are appended, the old ones stay until a compaction copies
the live tree into the other half and a checkpoint switches over.  Journal and
log are different things on purpose: the journal remembers *what to redo* for a
short window, the log *is* the filesystem.

**Two barriers, not one.**  A single fsync after writing both the redo records
and the commit record would leave their ordering to the device's write cache,
and recovery could then find a commit whose redo description is missing.  The
protocol pays for the second barrier instead; `--no-journal` (benchmark mode)
shows what it costs.

## Test suites

| suite | checks | what it covers |
|---|---|---|
| `tests/test_functional.c` | 56 | every command's normal behaviour |
| `tests/test_errors.c` | 49 | bad input, missing files, wrong state, unmounted use |
| `tests/test_persistence.c` | 26 | content and metadata across restarts, hashed re-reads |
| `tests/test_corruption.c` | 35 | 13 deliberate damages + fuzzing, "refuse or report, never guess" |
| `tests/test_bcache.c` | 15 | bounded LRU buffer cache: hits, misses, dirty write-back, flushes |
| `tests/test_crash_recovery.c` | 57 | all crash points, replay, idempotency, batched checkpoints, compaction crashes |
| `tests/test_stress.c` | 41 | randomized workloads (`build/fsstress`): steady state, adopt, crashes, full image |
| `tests/test_comparison.c` | 27 | **Add-Ons**: without-journal failure modes vs with-journal recovery + group commit |
| **total** | **306** | plus the same 306 under AddressSanitizer + UBSan |

Logs (with the actual output of every check) are written to `results/`.

## Measured performance

`make benchmark` produces `results/benchmark.csv`, `.md` and `.png`.

### Group Commit Scaling (append ×3000)

| mode | batch | time (ms) | ops/s | speedup |
|---|---|---|---|---|
| per-transaction sync | 1 | 21 731 ms | 138 ops/s | 1.00× (baseline) |
| group commit | 4 | 15 734 ms | 190 ops/s | **1.38×** |
| group commit | 8 | 18 690 ms | 160 ops/s | **1.16×** |

| workload | journaled | baseline | ratio |
|---|---|---|---|
| create 240 files | 107 ms (8 972 ops/s) | 95 ms (10 105 ops/s) | 1.13× |
| append ×3000 | 617 ms (4 862 ops/s) | 579 ms (5 181 ops/s) | 1.07× |
| rewrite 8 KiB ×600 | 69 ms (8 710 ops/s) | 62 ms (9 694 ops/s) | 1.11× |
| bulk 62.5 KiB ×20 | 16 ms (76.3 MB/s) | 8 ms (152.6 MB/s) | 2.00× |
| churn ×600 | 251 ms (7 171 ops/s) | 212 ms (8 491 ops/s) | 1.18× |
| mkdir/rmdir ×1500 | 469 ms (6 397 ops/s) | 353 ms (8 499 ops/s) | 1.33× |
| read 32 KiB ×3000 | 324 ms (289 MB/s) | 323 ms (290 MB/s) | 1.00× |

Crash recovery (median of 5 mounts of a crashed image): clean mount 2 ms;
16 pending appends 3 ms; 5 pending 8 KiB rewrites 3 ms.  Reads do not touch the
journal, so the difference there is the noise floor of the machine; differences
below a few percent in this table are not meaningful.

## Documentation

| document | contents |
|---|---|
| `docs/architecture.md` | the six layers, one write end to end, design decisions |
| `docs/disk_format.md` | every structure, offset by offset, and the checksum rules |
| `docs/journal_protocol.md` | WAL protocol, journal record format, crash points, guarantees |
| `docs/recovery_design.md` | mount sequence, window re-derivation, replay, refusal rules |
| `docs/diagrams.md` | ASCII diagrams: layout, timeline, recovery flow, compaction, layers |
| `docs/testing_plan.md` | suites, requirement → test map, results, what is not tested |
| `docs/report.md` | final report: deliverables, results, self-found bugs, limitations |
| `docs/presentation.md` | 12-slide outline |
| `docs/demo_script.md` | 3–5 minute live demonstration (HELLO → JOURNALFS, two crashes) |
| `docs/checklist.md` | milestone-by-milestone state of the project |

## Repository layout

```
src/        14 modules: cli, main, filesystem, transaction, journal, recovery,
            storage, checkpoint, disk_image, inode, directory, permissions,
            checksum, util            (~6 000 lines of C)
include/    the headers, including the on-disk format contract
tests/      six suites, the stress driver (C) and the corruption tool (Python)
benchmarks/ the benchmark shell script and the plotting script (Python)
docs/       the documents listed above
results/    test logs, benchmark CSV/MD/PNG  (generated, safe to delete)
```

## Limitations

1. **Simulated durability.**  The image is a host file and a crash is
   `_exit(99)`.  Only the ordering of writes and syncs is real; no physical
   storage was measured, and no claim is made about how a real device behaves.
2. **Single process, single simulated user.**  One mount at a time (a
   superblock flag enforces it); uid/gid fixed at 1000; permission bits are
   stored and checked, but they are a model of a filesystem's checks, not a
   security boundary — the process can read the image file directly.
3. **Small geometry.**  1–64 MiB images, 252 inodes, 16 direct blocks per file
   (64 768 bytes maximum), 100 directory entries per block, no indirect blocks,
   no sparse files, no symlinks, 31 byte names.
4. **No fsck.**  Damage that redo cannot repair is reported, not repaired; a
   journal record with a broken payload checksum makes the image refuse to
   mount rather than guess.
5. **Journal capacity of 192 KiB**, and a transaction that does not fit is
   refused cleanly instead of the journal growing.
6. **Timing numbers are environment dependent** (shared machine, host page
   cache).  The benchmark reports medians and states its noise floor.
7. **`--no-journal` is not crash safe** — it exists to measure what the journal
   costs, and the CLI warns every time it is used.
