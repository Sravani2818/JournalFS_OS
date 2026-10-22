# JournalFS testing plan and results

Every number in this document was produced by running the command shown, on
this machine, and the raw logs are kept in `results/`.  Nothing here is
estimated; nothing here claims a test that was not executed.

---

## 1. How the tests are organised

```
tests/lib.sh              shared shell helpers (jfs_run, assert_*, summary)
tests/test_functional.sh  56 checks: the normal behaviour of every command
tests/test_errors.sh      49 checks: malformed input, missing files, bad state
tests/test_persistence.sh 26 checks: content and metadata across restarts
tests/test_corruption.sh  35 checks: deliberately damaged images
tests/test_crash_recovery.sh 57 checks: every crash point, recovery, idempotency
tests/test_stress.sh      41 checks: randomized workloads (drives build/fsstress)
tests/fsstress.c          the randomized workload driver
tests/corrupt.py          the damage tool (reimplements the on-disk format)
```

Each script:

* writes a detailed log to `results/<script>.log` (pass/fail line per check,
  with expected and actual output on failure);
* exits non-zero when anything failed, so `make test` stops at the first
  broken suite;
* uses its own temporary directory for images, so it is safe to run repeatedly
  and in any order;
* is quiet on success and prints the failure details otherwise.

```bash
make            # build build/journalfs and build/fsstress
make smoke      # just test_functional.sh (fast check after a change)
make test       # functional, errors, persistence, corruption
make test-crash # crash injection + recovery
make test-all   # everything, including the stress driver
make test-sanitize  # rebuild with ASan+UBSan and run every suite
```

## 2. Requirement → test map

The table below maps each part of the specification to the checks that cover
it.  Check names are the ones printed by the suites and recorded in the logs.

| requirement | covered by |
|---|---|
| build a filesystem image, refuse to clobber one | `format creates an image`, `format refuses to overwrite without --force`, `format --force overwrites`, `pre_format: the image is byte for byte unchanged` |
| mount / unmount / clean shutdown | `mount succeeds on a fresh image`, `unmount succeeds`, `clean unmount: the image is marked clean`, `clean unmount: the journal window was reclaimed` |
| create / write / append / read / cat | `create reports success`, `write reports success`, `cat returns the content`, `read prints a size header`, `append extends the content`, `write with escapes produces two lines` |
| truncate (shrink and grow) | `truncate cuts the content`, `stat shows the truncated size`, `truncate can grow a file with zeros` |
| delete and rename | `rm reports success`, `removed file is gone from ls`, `rename reports success`, `renamed file appears in ls` |
| directories: mkdir / cd / pwd / ls / rmdir / rm -r | `mkdir reports success`, `nested mkdir reports success`, `cd changes the working directory`, `relative path reads through cd`, `cd .. moves back up`, `rmdir refuses a non-empty directory`, `rm -r removes a tree` |
| inode-like metadata | `stat shows the inode number`, `stat shows the mode`, `stat shows a log address`, `stat shows the size`, `stat marks directories`, `inode number survives a restart`, `size survives a restart` |
| permissions (simulated single user) | `chmod applies`, `read without permission is denied`, `write without permission is denied`, `create in a write-protected directory is denied`, `permission bits survive a restart` |
| size limits | `maximum file size is accepted`, `one byte over the limit is refused`, `file over the maximum size`, `absurd size` |
| space accounting and status | `df reports capacity`, `df reports inodes`, `stats` (engine counters), `check reports a consistent filesystem` |
| error handling for every command | all 49 checks in `test_errors.log`: nonexistent names, duplicate create, wrong types (`cat` on a directory, `rmdir` on a file), malformed sizes and modes, missing arguments, unknown commands, unknown crash points, paths too long, paths through files, unmounted use of ten different commands |
| journal record format (type, version, txid, seq, payload len, payload, checksum) | asserted structurally by `tests/corrupt.py` (it parses and rebuilds records and checksums), and behaviourally by `jrec-txid`, `jrec-type`, `jrec-oversize` cases in `test_corruption.sh` |
| write-ahead logging actually writes ahead | `pre_write: file still holds the old value`, `post_write`, `post_sync` (old value), `post_commit_sync: committed change replayed` |
| commit is the durability point | `post_commit: recorded commit replayed`, `the uncommitted file does not exist` (a `post_sync` crash leaves `/ghost` nonexistent) |
| recovery on mount | `recover_mid: the next mount finishes the replay`, `two crashes in a row keep both committed changes`, `batched crash session crashed at the third commit` + `all three committed appends were replayed` |
| recovery is idempotent | `… recovery is idempotent` after every crash point, `compact_mid/post: recovery is idempotent after compaction crash` |
| checkpointing | `sync writes a checkpoint`, `batched image is consistent`, `mount reports the replayed transactions`, `journal window is empty after the replay` |
| safe log reclamation | `clean unmount: the journal window was reclaimed`, `compact_mid`, `compact_post` cases, and `journal window is empty` after replay |
| log compaction under crash | `compact_mid: recovered content is the last committed version`, `compact_post: …`, both with a follow-up consistency check |
| corruption detection | `damaged redo payload: mount refuses with a clear reason`, `damaged record header: the truncation is reported`, `impossible record length`, `split transaction id`, `one destroyed checkpoint slot: mount falls back to the other`, `both checkpoint slots destroyed: mount refuses`, `window pointing outside the records: damage reported`, `broken superblock magic`, `broken superblock checksum`, `truncated image file`, `damaged inode map record`, `zeroed log record`, `random data: recognised as a foreign image` |
| no silent repair | `damaged window: the report survives a remount`, `the image is never repaired behind the user's back` section |
| robustness against arbitrary damage | `fuzz: no crash or abort in 8 damaged images` (40 random 512-byte scribbles per image, then mount/cat/check/ls) |
| persistence across restarts | all of `test_persistence.log`: 40 hashed files over 3 restart rounds, a 60 000 byte file hashed across mounts, deleted files staying deleted |
| randomized workloads | `test_stress.log`: 4 seeds × 600 operations, adopt-and-continue, 7 crash points with a follow-up workload, a crash during recovery, a full filesystem, determinism, clean-inspect at the end |
| no memory errors / undefined behaviour | `make test-sanitize` (AddressSanitizer + UndefinedBehaviorSanitizer, leak detection on) over all six suites |
| performance measurement | `benchmarks/benchmark.sh` → `results/benchmark.csv`, `.md`, `.png` |

## 3. Results (this machine, this build)

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

Under `make test-sanitize` (AddressSanitizer + UndefinedBehaviorSanitizer,
`detect_leaks=1`, `print_stacktrace=1`) the same six suites report the same 264
passing checks, with no sanitizer diagnostics.

The stress driver's own summary for the full-image run:
`OK -- 700 operation(s), 141 clean error(s), N file(s) verified`, where the
"clean errors" are the filesystem correctly refusing work on a 1 MiB image that
is out of room.

## 4. What the tests deliberately do not do

* **They do not prove anything about real hardware.**  Crashes are `_exit(99)`
  inside the engine; the image is a file on the host filesystem.  What is
  verified is the *order* in which the engine writes and syncs, not how a
  physical device reorders its cache.
* **They do not run concurrently.**  JournalFS is single-process by design
  (one mount at a time, enforced by the in-use flag); there is no locking test
  because there is no lock.
* **They do not fuzz the CLI's command parser with arbitrary byte streams.**
  The fuzzing is aimed at the disk image, which is where corruption actually
  matters.
* **They do not time much.**  Crash recovery timing appears in the benchmark
  output, not in the test pass/fail criteria, because a shared machine cannot
  support a timing assertion.

## 5. Adding a test

1. Pick the suite whose purpose matches (functional/error/persistence/
   corruption/crash/stress).
2. Add a `pass`/`fail` or `assert_*` call with a description that reads like a
   requirement, not like a code change.
3. Run the suite: `tests/test_<suite>.sh`, and check the log for the line.
4. If the change touches the disk format, update `docs/disk_format.md` and
   `tests/corrupt.py` in the same commit — they are the format's two
   independent readers, and disagreement between them is itself a bug.
